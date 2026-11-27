// gamepresent.cpp - GP1/GP2 (experiment). See gamepresent.hpp.
#include "gamepresent.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"   // ui_ini_read, dcomp_visual_rooted, gp_brake

#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace mgpu
{
namespace gamepresent
{
namespace
{

typedef HRESULT (STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *present1_fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

const unsigned SLOT_PRESENT = 8, SLOT_PRESENT1 = 22;   // IDXGISwapChain1 vtable

std::mutex g_mx;
std::atomic<void *> g_game{nullptr};       // the game's native chain while attached
void      **g_vt = nullptr;                 // its vtable (patched)
std::atomic<present_fn>  g_o_present{nullptr};
std::atomic<present1_fn> g_o_present1{nullptr};

std::atomic<int> g_on{-1};
std::atomic<unsigned long long> g_bridge_ms{0};     // the bridge's last S_OK Present
std::atomic<unsigned long long> g_bridge_since{0};  // start of its current unbroken run
std::atomic<bool> g_dead{false};                    // GP2: skipping off for the rest of the session

const unsigned long long WARMUP_MS = 10000ull;       // GP2: the picture runs this long before any skip
const unsigned long long IDLE_MS = 250ull;
std::atomic<unsigned> g_latency{2};                  // GamePresentLatency (1..3); GP3: atomic

thread_local int t_depth = 0;                        // GP2: re-entry guard

// Counters for the log. GP3 (IN51 review): atomics. The game presents from one
// thread, but the hooks sit in a vtable the bridge's own chain can share, so
// another thread can pass through them (g_reenter), and a game that presents
// from two threads would race plain counters. Relaxed: they are statistics.
using acnt = std::atomic<unsigned long long>;
acnt g_skip{0}, g_pass_hidden{0}, g_pass_idle{0}, g_pass_warm{0}, g_pass_other{0};
acnt g_timeouts{0}, g_timeouts_all{0}, g_reenter{0};
acnt g_wait_us{0}, g_wait_max_us{0};        // brake waits, microseconds
acnt g_log_ms{0};
std::atomic<bool> g_said_first{false};
// GP3: calls inside hk_present / hk_present1 right now. unpatch waits for them
// to leave before reporting the hooks gone, so teardown never races a call
// still running our code. t_in: this thread's own share (it may unpatch from
// inside a call and must not wait for itself).
std::atomic<int> g_inflight{0};
thread_local int t_in = 0;
struct inflight { inflight() { ++t_in; g_inflight.fetch_add(1, std::memory_order_acq_rel); }
                  ~inflight() { g_inflight.fetch_sub(1, std::memory_order_acq_rel); --t_in; } };
std::atomic<bool> g_streak{false};   // TL1: atomic, detach() writes it too                               // the last game Present was skipped
acnt g_ovr_at_start{0};                              // the hand-over's overrun count when skipping began

void name_of(void *a, char *out, size_t n)
{
    HMODULE m = nullptr;
    if (a != nullptr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCWSTR)a, &m) && m != nullptr)
    {
        wchar_t p[MAX_PATH] = L"";
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t *b = wcsrchr(p, L'\\');
        snprintf(out, n, "%ls+0x%llx", b ? b + 1 : p, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
    }
    else snprintf(out, n, "0x%p", a);
}

bool patch(void **slot, void *fn)
{
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &prot)) return false;
    InterlockedExchangePointer(slot, fn);
    DWORD dummy = 0;
    VirtualProtect(slot, sizeof(void *), prot, &dummy);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}

double now_ms()
{
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return f.QuadPart ? (double)q.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

void stop(const char *why)
{
    if (g_dead.exchange(true)) return;
    char l[400];
    snprintf(l, sizeof l, "[MGPU][GP2] skipping STOPPED for this session: %s. The game's own frames go to the screen "
             "again; nothing else changes.", why);
    mgpu::diag::warn(l);
}

void log_window(unsigned long long now)
{
    unsigned long long last = g_log_ms.load(std::memory_order_relaxed);
    if (last == 0) { g_log_ms.compare_exchange_strong(last, now); return; }
    if (now - last < 30000ull) return;
    if (!g_log_ms.compare_exchange_strong(last, now)) return;   // another thread took this window
    const unsigned long long sk = g_skip.exchange(0), ph = g_pass_hidden.exchange(0), pi = g_pass_idle.exchange(0),
                             pw = g_pass_warm.exchange(0), po = g_pass_other.exchange(0), to = g_timeouts.exchange(0),
                             re = g_reenter.exchange(0), wu = g_wait_us.exchange(0), wm = g_wait_max_us.exchange(0);
    char l[520];
    snprintf(l, sizeof l, "[MGPU][GP2] last 30 s: game presents skipped %llu, let through %llu (picture not on screen "
             "%llu, bridge not presenting %llu, warming up %llu, other %llu) | brake waited %.2f ms on average, %.1f ms "
             "longest, timed out %llu | re-entered %llu%s.",
             sk, ph + pi + pw + po, ph, pi, pw, po, sk ? (double)wu / 1000.0 / (double)sk : 0.0, (double)wm / 1000.0,
             to, re, g_dead.load() ? " | STOPPED for this session" : "");
    mgpu::diag::info(l);
}

// One game Present: true = it was skipped (the caller returns S_OK).
bool try_skip(UINT flags)
{
    const unsigned long long now = GetTickCount64();
    log_window(now);
    if (g_dead.load()) { ++g_pass_other; g_streak = false; return false; }
    if (flags & DXGI_PRESENT_TEST) { ++g_pass_other; return false; }
    if (!mgpu::gpu1::dcomp_visual_rooted()) { ++g_pass_hidden; g_streak = false; return false; }
    const unsigned long long b = g_bridge_ms.load(std::memory_order_relaxed);
    if (b == 0 || now - b > IDLE_MS) { ++g_pass_idle; g_streak = false; return false; }
    const unsigned long long since = g_bridge_since.load(std::memory_order_relaxed);
    if (since == 0 || now - since < WARMUP_MS) { ++g_pass_warm; g_streak = false; return false; }

    // The brake: what the real Present would have waited for.
    unsigned long long ovr = 0;
    const double t0 = now_ms();
    const int r = mgpu::gpu1::gp_brake(g_latency.load(std::memory_order_relaxed), 100u, &ovr);
    const double w = now_ms() - t0;
    if (r == 2) { stop("a graphics device reported lost (a fence read 2^64-1)"); ++g_pass_other; return false; }
    if (r == 3) { ++g_pass_idle; g_streak = false; return false; }
    if (r == 1)
    {
        // The render card is far behind: let the real Present throttle this frame.
        ++g_timeouts; ++g_pass_other; g_streak = false;
        if (++g_timeouts_all >= 5) stop("the brake timed out 5 times (the render card fell more than 100 ms behind)");
        return false;
    }
    if (!g_streak.load()) { g_streak = true; g_ovr_at_start.store(ovr); }
    else if (ovr > g_ovr_at_start.load()) { stop("the frame hand-over overran while game presents were skipped"); ++g_pass_other; return false; }

    ++g_skip;
    const unsigned long long wus = (unsigned long long)(w * 1000.0);
    g_wait_us.fetch_add(wus, std::memory_order_relaxed);
    unsigned long long mx = g_wait_max_us.load(std::memory_order_relaxed);
    while (wus > mx && !g_wait_max_us.compare_exchange_weak(mx, wus, std::memory_order_relaxed)) {}
    if (!g_said_first.exchange(true))
    {
        mgpu::diag::info("[MGPU][GP2] first game Present skipped (after the 10 s warm-up): the game's own frames no "
                         "longer reach the screen while the neural picture is up. Check PresentMon: the game's swap "
                         "chain should show almost no presents, the bridge's its full rate.");
    }
    return true;
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    inflight in;   // GP3
    present_fn o = g_o_present.load(std::memory_order_acquire);
    if (t_depth > 0) { ++g_reenter; return o(sc, sync, flags); }   // called from inside ourselves: straight through
    struct guard { guard() { ++t_depth; } ~guard() { --t_depth; } } gd;
    if ((void *)sc == g_game.load(std::memory_order_acquire) && try_skip(flags)) return S_OK;
    return o(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp)
{
    inflight in;   // GP3
    present1_fn o = g_o_present1.load(std::memory_order_acquire);
    if (t_depth > 0) { ++g_reenter; return o(sc, sync, flags, pp); }
    struct guard { guard() { ++t_depth; } ~guard() { --t_depth; } } gd;
    if ((void *)sc == g_game.load(std::memory_order_acquire) && try_skip(flags)) return S_OK;
    return o(sc, sync, flags, pp);
}

// AU3: once a vtable slot has pointed at hk_present / hk_present1, this module
// stays loaded until the process exits. The in-flight count (GP3) covers a call
// from its first line to its last, but not the instructions before the counter
// goes up (a thread that read the slot just before it was put back) or after it
// goes down (the epilogue and return), and the wait is bounded at 500 ms. A
// pinned module makes all three harmless: the code those threads run can never
// be unmapped under them. Only taken when the hook was actually installed
// (GamePresentSkip=1, off by default). The add-on cannot then be unloaded
// before the game exits - ReShade does not unload add-ons earlier anyway.
void pin_self()
{
    static std::atomic<bool> pinned{false};
    if (pinned.exchange(true)) return;
    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&hk_present), &h))
        mgpu::diag::info("[MGPU][GP2] add-on pinned in memory while its Present hooks may still be called (AU3).");
    else
        mgpu::diag::warn("[MGPU][GP2] could not pin the add-on in memory (AU3) - the bounded wait at unhook still applies.");
}

void unpatch_locked()
{
    if (g_vt == nullptr) return;
    bool other = false;
    if (g_vt[SLOT_PRESENT] == (void *)&hk_present) patch(&g_vt[SLOT_PRESENT], (void *)g_o_present.load());
    else other = true;
    if (g_vt[SLOT_PRESENT1] == (void *)&hk_present1) patch(&g_vt[SLOT_PRESENT1], (void *)g_o_present1.load());
    else other = true;
    if (other) mgpu::diag::warn("[MGPU][GP2] something else hooked the swap chain on top of GP2 - its slots stay as they are.");
    g_vt = nullptr;
    // GP3: new calls now go to the originals; wait (bounded - the brake caps a
    // call at ~100 ms) for calls already inside our hooks to return.
    for (int i = 0; i < 500 && g_inflight.load(std::memory_order_acquire) > t_in; ++i) Sleep(1);
    if (g_inflight.load(std::memory_order_acquire) > t_in)
        mgpu::diag::warn("[MGPU][GP2] a Present call was still inside the hook 500 ms after it was removed.");
}

} // namespace

bool on()
{
    int v = g_on.load(std::memory_order_relaxed);
    if (v < 0)
    {
        v = mgpu::gpu1::ui_ini_read("GamePresentSkip", 0) != 0 ? 1 : 0;
        int lat = mgpu::gpu1::ui_ini_read("GamePresentLatency", 2);
        g_latency.store((unsigned)(lat < 1 ? 1 : (lat > 3 ? 3 : lat)), std::memory_order_relaxed);
        g_on.store(v);
    }
    return v != 0;
}

void bridge_presented()
{
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = g_bridge_ms.exchange(now, std::memory_order_relaxed);
    if (last == 0 || now - last > IDLE_MS) g_bridge_since.store(now, std::memory_order_relaxed);
}

void attach(IUnknown *game_swapchain)
{
    if (game_swapchain == nullptr || !on()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    IDXGISwapChain1 *sc1 = nullptr;
    if (FAILED(game_swapchain->QueryInterface(__uuidof(IDXGISwapChain1), (void **)&sc1)) || sc1 == nullptr)
    {
        mgpu::diag::warn("[MGPU][GP2] the game's swap chain has no IDXGISwapChain1 - GamePresentSkip does nothing here.");
        return;
    }
    const bool same = (void *)sc1 == (void *)game_swapchain;
    DXGI_SWAP_CHAIN_DESC1 d{};
    sc1->GetDesc1(&d);
    sc1->Release();
    if (!same)
    {
        mgpu::diag::warn("[MGPU][GP2] the game's swap chain answered IDXGISwapChain1 with a different pointer (a wrapper "
                         "in the way) - GamePresentSkip does nothing here.");
        return;
    }

    void **vt = *reinterpret_cast<void ***>(game_swapchain);
    if (g_vt != vt)
    {
        unpatch_locked();
        // The originals first: another thread (the bridge's chain may share this
        // vtable) can call through a slot the moment it is patched.
        g_o_present.store((present_fn)vt[SLOT_PRESENT], std::memory_order_release);
        g_o_present1.store((present1_fn)vt[SLOT_PRESENT1], std::memory_order_release);
        if (!patch(&vt[SLOT_PRESENT1], (void *)&hk_present1) || !patch(&vt[SLOT_PRESENT], (void *)&hk_present))
        {
            if (vt[SLOT_PRESENT1] == (void *)&hk_present1) patch(&vt[SLOT_PRESENT1], (void *)g_o_present1.load());
            mgpu::diag::warn("[MGPU][GP2] could not hook the game's swap chain (VirtualProtect failed) - "
                             "GamePresentSkip does nothing here.");
            return;
        }
        g_vt = vt;
        pin_self();   // AU3
    }
    g_streak = false;
    g_game.store(game_swapchain, std::memory_order_release);

    char a[160], b[160], c[160];
    name_of((void *)g_o_present.load(), a, sizeof a);
    name_of((void *)g_o_present1.load(), b, sizeof b);
    name_of((void *)vt, c, sizeof c);
    char l[900];
    snprintf(l, sizeof l, "[MGPU][GP2] EXPERIMENT ON (GamePresentSkip=1): after the picture has run 10 s, the game's own "
             "Present is skipped while the picture is on screen, with a brake that waits for the render card to "
             "finish the frame from %u frame(s) ago (GamePresentLatency). The back-buffer index is left alone. Stops "
             "for the session on a lost device, a hand-over overrun or 5 brake time-outs. Game chain 0x%p, %u "
             "buffers, flags 0x%X | vtable %s | Present was %s, Present1 was %s.",
             g_latency.load(), (void *)game_swapchain, d.BufferCount, d.Flags, c, a, b);
    mgpu::diag::warn(l);
}

void detach(IUnknown *game_swapchain)
{
    if (game_swapchain == nullptr) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_game.load() != (void *)game_swapchain) return;
    g_game.store(nullptr, std::memory_order_release);
    g_streak = false;
    unpatch_locked();
    mgpu::diag::info("[MGPU][GP2] the game's swap chain went away - hooks removed.");
}

} // namespace gamepresent
} // namespace mgpu
