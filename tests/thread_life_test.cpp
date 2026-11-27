// thread_life_test.cpp - LR100. The start / stop rules of src/thread_life.hpp,
// then two concurrent stress models built on those rules:
//
//   A. the FG present thread (gpu1_context.cpp fg_handoff / fg_present_stop):
//      producers hand jobs over while stoppers join the thread, from several
//      threads at once (today only the bridge thread does either; the model
//      checks the rules hold even if that changes).
//   B. the input-replay hook thread (replay.cpp hooks / toggle_record /
//      start_play / shutdown): two togglers (panel + hotkey), a player whose
//      thread removes the hooks when it ends, and repeated shutdown / re-arm.
//
// Portable (no Windows calls): a std::thread stands in for the pinned Win32
// thread, a flag + condition variable for its message queue. The locking,
// state and slot handling are written exactly as in the bridge. Build it with
// -fsanitize=thread to have the race detector watch every run.
//
//   g++ -std=c++17 -O1 -g -fsanitize=thread -Isrc tests/thread_life_test.cpp -o tl && ./tl
//
// Runs in well under a second; ctest stops it at 60 s, so a deadlock fails
// the test instead of stalling the run.
#include "thread_life.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace mgpu::thlife;

static int g_fail = 0, g_pass = 0;

// A short pause that is short on every OS. std::this_thread::sleep_for rounds
// up to the Windows timer tick (1 - 15.6 ms), which turned the 0.2 s run into
// minutes there; this yields until the time has passed instead.
static void pause_us(int us)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) std::this_thread::yield();
}
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------- rules ----
static void test_rules()
{
    CHECK(use(state::idle, false) == step::start);
    CHECK(use(state::idle, false, false) == step::skip);      // "remove hooks" never starts a thread
    CHECK(use(state::running, true) == step::use);
    CHECK(use(state::running, true, false) == step::use);
    CHECK(use(state::stopping, false) == step::refuse);       // never a second thread beside one being joined
    CHECK(use(state::stopping, false, false) == step::refuse);
    CHECK(use(state::stopping, true) == step::refuse);

    CHECK(started() == state::running);

    state s = state::running;
    CHECK(stop_take(s, true) && s == state::stopping);
    stop_done(s, true);
    CHECK(s == state::idle);

    s = state::idle;
    CHECK(!stop_take(s, false) && s == state::stopping);      // nothing to join - but the stopper owns the slot
    CHECK(use(s, false) == step::refuse);                     // no start during its cleanup (LR104)
    stop_done(s, true);
    CHECK(s == state::idle);

    s = state::running;
    CHECK(stop_take(s, true));
    stop_done(s, false);                                      // would not end: stays stopping
    CHECK(s == state::stopping);
    CHECK(use(s, false) == step::refuse);

    CHECK(consistent(state::idle, false));
    CHECK(!consistent(state::idle, true));
    CHECK(consistent(state::running, true));
    CHECK(!consistent(state::running, false));
    CHECK(consistent(state::stopping, false));
    CHECK(!consistent(state::stopping, true));
}

// ------------------------------------------------- A. FG present thread ----
namespace fg
{
std::mutex mx;
std::condition_variable cv;
bool has = false, stop = false;
int job = 0;                       // a "reference": 1 while a job is pending
std::thread *thr = nullptr;
state life = state::idle;

std::atomic<int> alive{0}, max_alive{0}, started_n{0}, refused{0};
std::atomic<long> made{0}, consumed{0}, dropped{0};

void thread_main()
{
    const int a = alive.fetch_add(1) + 1;
    int m = max_alive.load();
    while (a > m && !max_alive.compare_exchange_weak(m, a)) {}
    for (;;)
    {
        std::unique_lock<std::mutex> lk(mx);
        cv.wait(lk, [] { return has || stop; });
        if (stop) break;               // a pending job is dropped by stop()
        has = false;
        job = 0;
        consumed.fetch_add(1);
    }
    alive.fetch_sub(1);
}

// Mirrors fg_handoff from "std::unique_lock<std::mutex> lk(g_fgp_mx);" on.
bool handoff()
{
    made.fetch_add(1);
    std::unique_lock<std::mutex> lk(mx);
    const step st = use(life, thr != nullptr);
    if (st == step::refuse) { lk.unlock(); dropped.fetch_add(1); refused.fetch_add(1); return false; }
    if (st == step::start)
    {
        stop = false;
        try { thr = new std::thread(thread_main); }
        catch (...) { thr = nullptr; lk.unlock(); dropped.fetch_add(1); return false; }
        life = started();
        started_n.fetch_add(1);
    }
    if (has) dropped.fetch_add(1);     // the older pending job is dropped, newest kept
    job = 1;
    has = true;
    lk.unlock();
    cv.notify_one();
    return true;
}

// Mirrors fg_present_stop (LR104): one owner for the whole stop; the slot is
// reopened only after the tail (queue drain, retire list, pfence, set marks).
std::mutex stop_mx;
std::atomic<int> tail_overlaps{0};
void stop_thread()
{
    std::lock_guard<std::mutex> owner(stop_mx);
    std::thread *t = nullptr;
    {
        std::lock_guard<std::mutex> lk(mx);
        stop = true;
        t = thr;
        thr = nullptr;
        (void)stop_take(life, t != nullptr);
    }
    cv.notify_one();
    bool joined = false;
    if (t != nullptr)
    {
        if (t->joinable() && t->get_id() != std::this_thread::get_id()) { t->join(); joined = true; }
        else if (t->joinable()) t->detach();
        else joined = true;
        delete t;
    }
    {
        std::lock_guard<std::mutex> lk(mx);
        if (has) { has = false; job = 0; dropped.fetch_add(1); }
    }
    // the tail: shared FG state is reset here - no present thread may be alive
    if (alive.load() != 0) tail_overlaps.fetch_add(1);
    pause_us(5);
    if (alive.load() != 0) tail_overlaps.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(mx);
        stop_done(life, t == nullptr || joined);
    }
}

void run(int producers, int stoppers, int iters)
{
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int p = 0; p < producers; ++p)
        ts.emplace_back([&] {
            while (!go.load()) {}
            for (int i = 0; i < iters; ++i)
            {
                handoff();
                if ((i & 7) == 0) pause_us(20);   // a frame's gap
                else std::this_thread::yield();
            }
        });
    for (int q = 0; q < stoppers; ++q)
        ts.emplace_back([&] { while (!go.load()) {} for (int i = 0; i < iters / 8; ++i) { stop_thread(); pause_us(100); } });
    go.store(true);
    for (auto &t : ts) t.join();
    stop_thread();                      // the final teardown
}
}   // namespace fg

static void test_fg_stress()
{
    fg::run(1, 1, 4000);                // today's shape: one bridge thread does both, here split in two
    fg::run(3, 2, 3000);                // more than today: the rule must still hold
    fg::run(2, 3, 2000);
    CHECK(fg::max_alive.load() <= 1);   // never two present threads
    CHECK(fg::tail_overlaps.load() == 0);   // LR104: the stop's tail never runs beside a present thread
    CHECK(fg::alive.load() == 0);
    CHECK(fg::thr == nullptr);
    CHECK(consistent(fg::life, fg::thr != nullptr));
    CHECK(fg::life == state::idle);     // the last stop reopened the slot
    CHECK(!fg::has && fg::job == 0);
    // every job handed over was presented or dropped exactly once
    CHECK(fg::made.load() == fg::consumed.load() + fg::dropped.load());
    CHECK(fg::started_n.load() >= 1);
    std::printf("  FG model: %ld jobs, %ld presented, %ld dropped, %d refused during a stop, %d thread starts\n",
                fg::made.load(), fg::consumed.load(), fg::dropped.load(), fg::refused.load(), fg::started_n.load());

    // re-arm after a full stop: a new hand-over starts a fresh thread
    const int before = fg::started_n.load();
    CHECK(fg::handoff());
    CHECK(fg::started_n.load() == before + 1);
    fg::stop_thread();
    CHECK(fg::life == state::idle && fg::thr == nullptr);
    fg::stop_thread();                  // double stop: harmless
    CHECK(fg::life == state::idle && fg::thr == nullptr);
}

// --------------------------------------------- B. replay hook thread -------
namespace rp
{
std::mutex ctl_mx, life_mx;
std::thread *hook = nullptr;            // stands in for g_hook_h
state life = state::idle;
std::atomic<int> tid{0};                // stands in for g_hook_tid

// the hook thread's "message queue"
std::mutex q_mx;
std::condition_variable q_cv;
bool q_quit = false;
int q_install = 0, q_remove = 0;

std::atomic<int> alive{0}, max_alive{0}, starts{0}, posts_to_dead{0};
std::atomic<int> installed{0};          // stands in for g_kb (0/1)

std::atomic<bool> recording{false}, playing{false}, play_stop{false};
std::thread play;
std::atomic<long> toggles{0}, toggles_busy{0}, plays{0};

void hook_main()
{
    const int a = alive.fetch_add(1) + 1;
    int m = max_alive.load();
    while (a > m && !max_alive.compare_exchange_weak(m, a)) {}
    tid.store(1);
    std::unique_lock<std::mutex> lk(q_mx);
    for (;;)
    {
        q_cv.wait(lk, [] { return q_quit || q_install > 0 || q_remove > 0; });
        while (q_install > 0) { --q_install; installed.store(1); }
        while (q_remove > 0) { --q_remove; installed.store(0); }
        if (q_quit) break;
    }
    installed.store(0);                 // hook_main's final unhook
    alive.fetch_sub(1);
}

void post(bool on)
{
    if (alive.load() == 0) posts_to_dead.fetch_add(1);
    { std::lock_guard<std::mutex> lk(q_mx); if (on) ++q_install; else ++q_remove; }
    q_cv.notify_one();
}

// Mirrors replay.cpp hooks().
void hooks(bool on)
{
    std::lock_guard<std::mutex> lk(life_mx);
    const step st = use(life, hook != nullptr, on);
    if (st == step::refuse || st == step::skip) return;
    if (st == step::start)
    {
        tid.store(0);
        { std::lock_guard<std::mutex> q(q_mx); q_quit = false; q_install = q_remove = 0; }
        hook = new std::thread(hook_main);
        life = started();
        starts.fetch_add(1);
        for (int i = 0; i < 500 && tid.load() == 0; ++i) pause_us(50);
    }
    if (tid.load()) post(on);
}

void join_play()
{
    if (play.joinable()) { play_stop.store(true); play.join(); }
    play_stop.store(false);
}

void play_main()
{
    for (int i = 0; i < 50 && !play_stop.load(); ++i) std::this_thread::yield();
    hooks(false);                       // as play_main does when it ends
    playing.store(false);
}

void toggle_record()
{
    std::unique_lock<std::mutex> cl(ctl_mx, std::try_to_lock);
    if (!cl.owns_lock()) { toggles_busy.fetch_add(1); return; }
    toggles.fetch_add(1);
    if (playing.load()) return;
    if (!recording.load()) { recording.store(true); hooks(true); return; }
    recording.store(false);
    hooks(false);
}

bool start_play()
{
    std::lock_guard<std::mutex> cl(ctl_mx);
    if (recording.load() || playing.load()) return false;
    join_play();
    playing.store(true);
    hooks(true);
    play = std::thread(play_main);
    plays.fetch_add(1);
    return true;
}

void shutdown()
{
    std::lock_guard<std::mutex> cl(ctl_mx);
    recording.store(false);
    join_play();
    std::thread *h = nullptr;
    {
        std::lock_guard<std::mutex> lk(life_mx);
        h = hook;
        hook = nullptr;
        if (!stop_take(life, h != nullptr)) { stop_done(life, true); return; }
    }
    { std::lock_guard<std::mutex> q(q_mx); q_quit = true; }
    q_cv.notify_one();
    h->join();                          // the bridge waits on the handle, bounded; here it always ends
    delete h;
    std::lock_guard<std::mutex> lk(life_mx);
    tid.store(0);
    stop_done(life, true);
}

void run(int iters)
{
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    auto toggler = [&] {
        while (!go.load()) {}
        for (int i = 0; i < iters; ++i) { toggle_record(); pause_us(30); }
    };
    ts.emplace_back(toggler);   // panel (game thread)
    ts.emplace_back(toggler);   // hotkey (bridge thread)
    ts.emplace_back([&] { while (!go.load()) {} for (int i = 0; i < iters / 4; ++i) { start_play(); pause_us(120); } });
    ts.emplace_back([&] { while (!go.load()) {} for (int i = 0; i < iters / 16; ++i) { shutdown(); pause_us(400); } });
    go.store(true);
    for (auto &t : ts) t.join();
    shutdown();
}
}   // namespace rp

static void test_replay_stress()
{
    rp::run(3000);
    rp::run(3000);
    CHECK(rp::max_alive.load() <= 1);   // never two hook threads
    CHECK(rp::alive.load() == 0);
    CHECK(rp::hook == nullptr && rp::life == state::idle);
    CHECK(!rp::play.joinable());
    CHECK(rp::installed.load() == 0);   // no hook left behind
    CHECK(rp::posts_to_dead.load() == 0);
    CHECK(rp::starts.load() >= 1);
    std::printf("  replay model: %ld toggles (%ld dropped as busy), %ld plays, %d hook-thread starts\n",
                rp::toggles.load(), rp::toggles_busy.load(), rp::plays.load(), rp::starts.load());

    // "remove" with no thread never starts one
    const int s0 = rp::starts.load();
    rp::hooks(false);
    CHECK(rp::starts.load() == s0 && rp::hook == nullptr);
    // shutdown with nothing running, twice
    rp::shutdown();
    rp::shutdown();
    CHECK(rp::hook == nullptr && rp::life == state::idle);
}

int main()
{
    test_rules();
    test_fg_stress();
    test_replay_stress();
    std::printf("thread_life: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
