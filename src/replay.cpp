// ---------------------------------------------------------------------------
// replay.cpp - BM2. See replay.hpp.
//
// Threads: the hook thread owns the low-level hooks and only pumps messages
// (a hook thread that stops pumping delays everyone's input); the playback
// thread sleeps between events and calls SendInput.
//
// LR100 - who may start and stop what:
//   g_ctl_mx  : toggle_record, start_play and shutdown, one at a time. The
//               panel (game thread) and the hotkey (bridge thread) can both
//               toggle; two at once double-started the hook thread
//               (std::terminate on assigning to a joinable std::thread).
//   g_life_mx : the hook thread's handle and life state (thread_life.hpp).
//               Taken by hooks() - also from the playback thread - and by
//               shutdown, never while waiting for the thread to end.
//   g_mx      : the path / recording data and the message (unchanged).
//   Order: g_ctl_mx -> g_life_mx -> g_mx. Nothing waits on a thread while
//   holding g_life_mx or g_mx.
//   The hook thread is PINNED (pinthread.hpp): it keeps the add-on loaded
//   until it leaves its message loop, so a FreeLibrary while it still sits in
//   GetMessage can no longer pull the code out from under it.
// ---------------------------------------------------------------------------
#include "replay.hpp"
#include "capture.hpp"
#include "diag.hpp"
#include "pinthread.hpp"
#include "thread_life.hpp"
#include "gpu1_context.hpp"   // LR104: ui_ini_read (InputHookPriority); LR106: ui_any_menu_open
#include "replay_rules.hpp"   // LR106: when playback stops

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mgpu
{
namespace replay
{
namespace
{

enum : unsigned char { E_KEY = 1, E_MOVE = 2, E_BTN = 3, E_WHEEL = 4 };

#pragma pack(push, 1)
struct ev
{
    double ms;          // from the start of the path
    unsigned char type;
    unsigned char up;   // key/button: 1 = release
    unsigned short code;// key: scan code | 0x100 if extended; button: 1 L, 2 R, 3 M, 4 X1, 5 X2
    int x, y;           // move: dx, dy; wheel: x = delta, y = 1 horizontal
};
struct file_head { char magic[8]; unsigned count; double seconds; };
#pragma pack(pop)

std::mutex g_mx;
std::vector<ev> g_path;          // the loaded / recorded path
double g_path_s = 0.0;
bool g_loaded = false;           // tried to load this session
std::vector<ev> g_rec;           // being recorded (hook thread appends)
std::atomic<LONGLONG> g_rec_t0{0};   // LR100: read by the hook callbacks and the HUD
// LR102: what the hooks saw while recording, and why an event was not kept -
// a "0 events" path (Cyberpunk, LR100) could not be told apart from a hook
// Windows never called. Reset at each start, reported at each stop.
std::atomic<unsigned> g_seen_kb{0}, g_seen_ms{0}, g_drop_fg{0}, g_drop_inj{0}, g_drop_still{0};
std::atomic<int> g_hk_kb_err{-1}, g_hk_ms_err{-1};   // -1 not tried, 0 installed, else GetLastError
// LR104: how late Windows delivered each event to the hook (the event's own
// timestamp vs now, ms) - the measure of whether the hook thread's priority
// helps - and the priority the thread was started with.
std::atomic<unsigned> g_lat_n{0}, g_lat_max{0};
std::atomic<unsigned long long> g_lat_sum{0};
std::atomic<int> g_hook_prio{0};   // InputHookPriority: 0 normal (default since LR106), 1 highest
void note_latency(DWORD ev_time)
{
    const DWORD d = GetTickCount() - ev_time;   // wraps correctly in DWORD arithmetic
    if (d > 60000u) return;                      // a bogus or synthetic timestamp
    g_lat_n.fetch_add(1u, std::memory_order_relaxed);
    g_lat_sum.fetch_add(d, std::memory_order_relaxed);
    unsigned m = g_lat_max.load(std::memory_order_relaxed);
    while (d > m && !g_lat_max.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
}
std::atomic<bool> g_recording{false}, g_playing{false}, g_abort{false}, g_aborted{false};
// LR106: the mouse during playback (the hook Cyberpunk does call) - a real
// button or wheel, and real movement summed in pixels.
std::atomic<bool> g_abort_mouse{false};
std::atomic<int>  g_real_move{0};
char g_msg[300] = {};

std::mutex g_ctl_mx;              // LR100: see the top of the file
std::mutex g_life_mx;             // LR100
HANDLE g_hook_h = nullptr;        // LR100: under g_life_mx; the pinned hook thread
mgpu::thlife::state g_hook_life = mgpu::thlife::state::idle;   // LR100: under g_life_mx
std::atomic<DWORD> g_hook_tid{0};
// LR100: atomics - the hook thread installs / removes them, emergency_unhook
// (DllMain) removes them too; exchange() makes each removal happen once.
std::atomic<HHOOK> g_kb{nullptr}, g_ms{nullptr};
std::thread g_play;
std::atomic<bool> g_play_stop{false};
const UINT WM_HK_INSTALL = WM_APP + 41, WM_HK_REMOVE = WM_APP + 42;

LONGLONG qpc() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
double qpc_ms(LONGLONG d) { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart ? (double)d * 1000.0 / (double)f.QuadPart : 0.0; }

bool own_foreground()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return fg != nullptr && pid == GetCurrentProcessId();
}

void set_msg(const char *m) { std::lock_guard<std::mutex> lk(g_mx); snprintf(g_msg, sizeof g_msg, "%s", m); }

bool path_file(wchar_t *out, unsigned n)
{
    if (!mgpu::capture::base_folder(out, n)) return false;
    wcscat_s(out, n, L"benchmarks\\");
    (void)CreateDirectoryW(out, nullptr);
    wcscat_s(out, n, L"paths\\");
    (void)CreateDirectoryW(out, nullptr);
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return false;
    const wchar_t *nm = wcsrchr(exe, L'\\');
    nm = nm ? nm + 1 : exe;
    wcscat_s(out, n, nm);
    wchar_t *dot = wcsrchr(out, L'.');
    if (dot && dot > wcsrchr(out, L'\\')) *dot = L'\0';
    wcscat_s(out, n, L".mgpupath");
    return true;
}

void load_once()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_loaded) return;
    g_loaded = true;
    wchar_t p[MAX_PATH * 2] = {};
    if (!path_file(p, MAX_PATH * 2)) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, p, L"rb") != 0 || f == nullptr) return;
    // REV5: a path file is ours, but it sits in the game folder and can be
    // truncated or damaged. Everything is checked before it is used: the size
    // on disk must be exactly header + count events, the count and length are
    // bounded (an hour of input is far more than any benchmark path), every
    // event must be a known type with sane fields, and time must not run
    // backwards. Anything else is refused and the path is simply absent.
    const unsigned kMaxEvents  = 1000000u;
    const double   kMaxSeconds = 3600.0;
    file_head h{};
    long long fsize = -1;
    if (_fseeki64(f, 0, SEEK_END) == 0) { fsize = _ftelli64(f); _fseeki64(f, 0, SEEK_SET); }
    const char *bad = nullptr;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "MGPUPTH1", 8) != 0) bad = "not a path file";
    else if (h.count > kMaxEvents) bad = "too many events";
    else if (!(h.seconds >= 0.0 && h.seconds <= kMaxSeconds)) bad = "bad length";
    else if (fsize != (long long)sizeof h + (long long)h.count * (long long)sizeof(ev)) bad = "size does not match";
    if (bad == nullptr)
    {
        std::vector<ev> v(h.count);
        if (h.count != 0 && fread(v.data(), sizeof(ev), h.count, f) != h.count) bad = "short read";
        double prev = 0.0;
        for (unsigned i = 0; bad == nullptr && i < h.count; ++i)
        {
            const ev &e = v[i];
            if (!(e.ms >= prev)) { bad = "timestamps out of order"; break; }
            // REV6: every event lies inside the recorded length (the stop
            // time is the header's seconds; 50 ms for rounding).
            if (!(e.ms <= h.seconds * 1000.0 + 50.0)) { bad = "event after the recorded end"; break; }
            prev = e.ms;
            if (e.up > 1) bad = "bad event";
            else if (e.type == E_KEY)   { if (e.x <= 0 || e.x > 255) bad = "bad key"; }
            else if (e.type == E_MOVE)  { if (e.x < -10000 || e.x > 10000 || e.y < -10000 || e.y > 10000) bad = "bad move"; }
            else if (e.type == E_BTN)   { if (e.code < 1 || e.code > 5) bad = "bad button"; }
            else if (e.type == E_WHEEL) { if (e.y < 0 || e.y > 1) bad = "bad wheel"; }
            else bad = "unknown event";
        }
        if (bad == nullptr) { g_path.swap(v); g_path_s = h.seconds; }
    }
    fclose(f);
    if (bad != nullptr)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][REV5] input path file refused (%s) - record a new path.", bad);
        mgpu::diag::warn(l);
    }
}

bool save_path(const std::vector<ev> &v, double secs)
{
    wchar_t p[MAX_PATH * 2] = {};
    if (!path_file(p, MAX_PATH * 2)) return false;
    FILE *f = nullptr;
    if (_wfopen_s(&f, p, L"wb") != 0 || f == nullptr) return false;
    file_head h{};
    memcpy(h.magic, "MGPUPTH1", 8);
    h.count = (unsigned)v.size(); h.seconds = secs;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && (v.empty() || fwrite(v.data(), sizeof(ev), v.size(), f) == v.size());
    fclose(f);
    return ok;
}

// ---- the hooks ----
LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
        const bool injected = (k->flags & LLKHF_INJECTED) != 0;
        const bool up = (k->flags & LLKHF_UP) != 0;
        if (g_playing.load() && !injected && !up) g_abort.store(true);   // a real key stops playback
        if (g_recording.load())   // LR102: counted, so a stop can say what happened
        {
            g_seen_kb.fetch_add(1u, std::memory_order_relaxed);
            note_latency(k->time);   // LR104
            if (injected) g_drop_inj.fetch_add(1u, std::memory_order_relaxed);
            else if (!own_foreground()) g_drop_fg.fetch_add(1u, std::memory_order_relaxed);
        }
        if (g_recording.load() && !injected && own_foreground())
        {
            ev e{};
            e.ms = qpc_ms(qpc() - g_rec_t0.load(std::memory_order_relaxed));
            e.type = E_KEY; e.up = up ? 1 : 0;
            e.code = (unsigned short)((k->scanCode & 0xFF) | ((k->flags & LLKHF_EXTENDED) ? 0x100 : 0));
            e.x = (int)k->vkCode;
            std::lock_guard<std::mutex> lk(g_mx);
            g_rec.push_back(e);
        }
        (void)wp;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK ms_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_playing.load())   // LR106: the mouse can stop a replay
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if ((m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) == 0)
        {
            if (wp == WM_MOUSEMOVE)
            {
                POINT c{};
                GetCursorPos(&c);
                const int dx = m->pt.x - c.x, dy = m->pt.y - c.y;
                g_real_move.fetch_add((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy), std::memory_order_relaxed);
            }
            else g_abort_mouse.store(true);   // a button or the wheel
        }
    }
    if (code == HC_ACTION && g_recording.load())
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        g_seen_ms.fetch_add(1u, std::memory_order_relaxed);   // LR102
        note_latency(m->time);   // LR104
        if ((m->flags & LLMHF_INJECTED) != 0) g_drop_inj.fetch_add(1u, std::memory_order_relaxed);
        else if (!own_foreground()) g_drop_fg.fetch_add(1u, std::memory_order_relaxed);
        if ((m->flags & LLMHF_INJECTED) == 0 && own_foreground())
        {
            ev e{};
            e.ms = qpc_ms(qpc() - g_rec_t0.load(std::memory_order_relaxed));
            bool keep = true;
            switch (wp)
            {
            case WM_MOUSEMOVE:
            {
                // Called before the cursor moves, so this is the move's delta.
                POINT c{};
                GetCursorPos(&c);
                e.type = E_MOVE; e.x = m->pt.x - c.x; e.y = m->pt.y - c.y;
                keep = (e.x != 0 || e.y != 0);
                if (!keep) g_drop_still.fetch_add(1u, std::memory_order_relaxed);   // LR102: a move of 0 pixels
                break;
            }
            case WM_LBUTTONDOWN: e.type = E_BTN; e.code = 1; break;
            case WM_LBUTTONUP:   e.type = E_BTN; e.code = 1; e.up = 1; break;
            case WM_RBUTTONDOWN: e.type = E_BTN; e.code = 2; break;
            case WM_RBUTTONUP:   e.type = E_BTN; e.code = 2; e.up = 1; break;
            case WM_MBUTTONDOWN: e.type = E_BTN; e.code = 3; break;
            case WM_MBUTTONUP:   e.type = E_BTN; e.code = 3; e.up = 1; break;
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                e.type = E_BTN; e.code = (HIWORD(m->mouseData) == XBUTTON2) ? 5 : 4; e.up = (wp == WM_XBUTTONUP) ? 1 : 0; break;
            case WM_MOUSEWHEEL:  e.type = E_WHEEL; e.x = (short)HIWORD(m->mouseData); break;
            case WM_MOUSEHWHEEL: e.type = E_WHEEL; e.x = (short)HIWORD(m->mouseData); e.y = 1; break;
            default: keep = false; break;
            }
            if (keep) { std::lock_guard<std::mutex> lk(g_mx); g_rec.push_back(e); }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void unhook(std::atomic<HHOOK> &h)
{
    if (HHOOK x = h.exchange(nullptr)) UnhookWindowsHookEx(x);
}

void hook_main()
{
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the queue
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (m.message == WM_HK_INSTALL)
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&kb_proc), &self);
            // Only this thread installs, so check-then-store is enough here.
            // LR102: the result of each install is kept for the stop's report.
            if (g_kb.load() == nullptr)
            {
                HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, self, 0);
                g_hk_kb_err.store(h != nullptr ? 0 : (int)GetLastError());
                g_kb.store(h);
            }
            if (m.wParam != 0 && g_ms.load() == nullptr)
            {
                HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, ms_proc, self, 0);
                g_hk_ms_err.store(h != nullptr ? 0 : (int)GetLastError());
                g_ms.store(h);
            }
        }
        else if (m.message == WM_HK_REMOVE)
        {
            unhook(g_kb);
            unhook(g_ms);
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    unhook(g_kb);
    unhook(g_ms);
}

// LR100: one place starts the hook thread and one place stops it, both under
// g_life_mx. hooks(false, ...) with no thread no longer starts one just to
// tell it "remove"; while shutdown is stopping it, requests are dropped.
void hooks(bool on, bool mouse)
{
    // LR104: read before g_life_mx (the ini mirror has its own lock; never nest it).
    const int prio = on ? (mgpu::gpu1::ui_ini_read("InputHookPriority", 0) != 0 ? 1 : 0) : 0;   // LR106: default normal
    std::lock_guard<std::mutex> lk(g_life_mx);
    const mgpu::thlife::step st = mgpu::thlife::use(g_hook_life, g_hook_h != nullptr, on);
    if (st == mgpu::thlife::step::refuse || st == mgpu::thlife::step::skip) return;
    if (st == mgpu::thlife::step::start)
    {
        g_hook_prio.store(prio);   // LR104
        g_hook_tid.store(0);
        g_hook_h = mgpu::start_pinned_thread_h([] {
            MSG m;
            PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // the queue exists before the id is published
            // LR103: as the hotkey and Frame Warp hook threads (RV1). Windows
            // gives a low-level hook only a short time to answer and drops it
            // otherwise; at normal priority, under a game that keeps every
            // core busy, this thread got NO calls in 24.6 s (Cyberpunk, LR102:
            // "hooks installed, called 0 + 0 times"). It does almost nothing
            // per call, so the priority costs the game nothing.
            // LR104: switchable (InputHookPriority). LR106: normal by default -
            // the A/B on 2026-11-26 gave the same delivery delay either way
            // (RoboCop, HIGHEST: avg 0.1 ms, max 16; Cyberpunk, normal: avg 0.0,
            // max 16), so the higher priority bought nothing measurable.
            if (g_hook_prio.load() != 0) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            g_hook_tid.store(GetCurrentThreadId());
            hook_main();
        });
        if (g_hook_h == nullptr)
        {
            mgpu::diag::warn("[MGPU][BM2] input path: the hook thread could not be started - recording / replay "
                             "input is not watched this time.");
            return;
        }
        g_hook_life = mgpu::thlife::started();
        // Waits for the new thread only (it never takes g_life_mx).
        for (int i = 0; i < 500 && g_hook_tid.load() == 0; ++i) Sleep(1);
    }
    // Safe to post to this id: g_hook_h is open, so the id cannot be reused.
    const DWORD tid = g_hook_tid.load();
    if (tid) PostThreadMessageW(tid, on ? WM_HK_INSTALL : WM_HK_REMOVE, mouse ? 1 : 0, 0);
}

bool is_mod_vk(int vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
}

// Drop what belongs to our own hotkey: releases at the start of keys that were
// already down, and the presses at the end that are still held when it stops.
void trim(std::vector<ev> &v, double end_ms)
{
    std::vector<ev> out;
    out.reserve(v.size());
    bool down[512] = {};
    for (const ev &e : v)
    {
        if (e.type == E_KEY)
        {
            const unsigned c = e.code & 0x1FF;
            if (e.up && !down[c]) continue;   // went down before the recording started
            down[c] = !e.up;
        }
        out.push_back(e);
    }
    // Presses still held at the stop, made in the last 1.5 s: the stop hotkey.
    for (size_t i = out.size(); i-- > 0;)
    {
        const ev &e = out[i];
        if (end_ms - e.ms > 1500.0) break;
        if (e.type == E_KEY && !e.up && down[e.code & 0x1FF] && (is_mod_vk(e.x) || end_ms - e.ms < 600.0))
        {
            down[e.code & 0x1FF] = false;
            out.erase(out.begin() + (long)i);
        }
    }
    v.swap(out);
}

// ---- playback ----
void send_key(unsigned short code, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)(code & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0) | ((code & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof in);
}

void send_btn(unsigned short b, bool up)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (b)
    {
    case 1: in.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN; break;
    case 2: in.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN; break;
    case 3: in.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN; break;
    case 4: case 5: in.mi.dwFlags = up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN; in.mi.mouseData = (b == 5) ? XBUTTON2 : XBUTTON1; break;
    default: return;
    }
    SendInput(1, &in, sizeof in);
}

void play_main(std::vector<ev> path)
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    const LONGLONG t0 = qpc();
    bool kdown[512] = {}, bdown[6] = {};
    bool stopped_by_user = false;
    mgpu::replay_rules::stop_why why = mgpu::replay_rules::stop_why::none;
    const unsigned esc_sc = MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) & 0x1FF;   // 0x01
    for (const ev &e : path)
    {
        for (;;)
        {
            if (g_play_stop.load()) goto done;
            {
                // LR106: replay_rules - the overlay, Esc read without a hook,
                // the mouse, a real key, another window in front.
                mgpu::replay_rules::inputs in;
                in.key_hook = g_abort.load();
                in.mouse_button = g_abort_mouse.load();
                in.real_move_px = g_real_move.load(std::memory_order_relaxed);
                in.esc_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                in.replay_holds_esc = kdown[esc_sc];
                in.overlay_open = mgpu::gpu1::ui_any_menu_open();
                in.foreground = own_foreground();
                why = mgpu::replay_rules::should_stop(in);
                if (why != mgpu::replay_rules::stop_why::none) { stopped_by_user = true; goto done; }
            }
            const double left = e.ms - qpc_ms(qpc() - t0);
            if (left <= 0.0) break;
            if (left > 1.5 && timer)
            {
                LARGE_INTEGER due{}; due.QuadPart = -(LONGLONG)((left > 20.0 ? 15.0 : left - 1.0) * 10000.0);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 30);
            }
            else YieldProcessor();
        }
        switch (e.type)
        {
        case E_KEY:   send_key(e.code, e.up != 0); kdown[e.code & 0x1FF] = !e.up; break;
        case E_MOVE:
        {
            INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = e.x; in.mi.dy = e.y;
            SendInput(1, &in, sizeof in);
            break;
        }
        case E_BTN:   send_btn(e.code, e.up != 0); if (e.code < 6) bdown[e.code] = !e.up; break;
        case E_WHEEL:
        {
            INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = e.y ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            in.mi.mouseData = (DWORD)e.x;
            SendInput(1, &in, sizeof in);
            break;
        }
        default: break;
        }
    }
done:
    // Never leave a key or button held down.
    for (unsigned c = 0; c < 512; ++c) if (kdown[c]) send_key((unsigned short)c, true);
    for (unsigned short b = 1; b < 6; ++b) if (bdown[b]) send_btn(b, true);
    if (timer) CloseHandle(timer);
    if (stopped_by_user)
    {
        g_aborted.store(true);
        char m1[200], m2[300];
        snprintf(m1, sizeof m1, "Input replay stopped: %s.", mgpu::replay_rules::stop_text(why));
        set_msg(m1);
        snprintf(m2, sizeof m2, "[MGPU][BM2] input replay stopped early: %s. All keys and buttons released.",
                 mgpu::replay_rules::stop_text(why));
        mgpu::diag::warn(m2);
    }
    hooks(false, false);
    g_playing.store(false);
}

void join_play()
{
    if (g_play.joinable()) { g_play_stop.store(true); g_play.join(); }
    g_play_stop.store(false);
}

} // namespace

void toggle_record()
{
    // LR100: the panel (game thread) and the hotkey (bridge thread) both land
    // here. try_lock: a second press while one is being handled is dropped,
    // and the game thread never waits on the bridge's file save.
    std::unique_lock<std::mutex> cl(g_ctl_mx, std::try_to_lock);
    if (!cl.owns_lock()) return;
    if (g_playing.load()) return;
    if (!g_recording.load())
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_rec.clear();
            g_rec.reserve(200000);
        }
        g_seen_kb.store(0u); g_seen_ms.store(0u); g_drop_fg.store(0u); g_drop_inj.store(0u); g_drop_still.store(0u);   // LR102
        g_hk_kb_err.store(-1); g_hk_ms_err.store(-1);
        g_lat_n.store(0u); g_lat_max.store(0u); g_lat_sum.store(0ull);   // LR104
        g_rec_t0.store(qpc(), std::memory_order_relaxed);   // published by the g_recording store below
        g_recording.store(true);
        hooks(true, true);
        int acc[3] = {};
        const bool accel = SystemParametersInfoW(SPI_GETMOUSE, 0, acc, 0) && acc[2] != 0;
        set_msg(accel ? "Recording input path... (Windows \"Enhance pointer precision\" is ON: mouse turns may not repeat exactly)"
                      : "Recording input path...");
        mgpu::diag::info(accel ? "[MGPU][BM2] input path recording started. NOTE: Windows \"Enhance pointer precision\" is on - "
                                 "mouse turns will not replay exactly; switch it off for repeatable camera moves."
                               : "[MGPU][BM2] input path recording started.");
        return;
    }
    g_recording.store(false);
    hooks(false, false);
    const double end_ms = qpc_ms(qpc() - g_rec_t0.load(std::memory_order_relaxed));
    std::vector<ev> v;
    { std::lock_guard<std::mutex> lk(g_mx); v.swap(g_rec); }
    const size_t raw = v.size();
    trim(v, end_ms);
    const double secs = end_ms / 1000.0;
    char l[300];
    // LR102: what the hooks saw. "installed" / "FAILED (error n)" / "not tried"
    // (the hook thread never got the request).
    auto hk = [](int e, char *b, size_t n) {
        if (e == 0) snprintf(b, n, "installed");
        else if (e < 0) snprintf(b, n, "not tried");
        else snprintf(b, n, "FAILED (error %d)", e);
    };
    char kbs[40], mss[40], rep[600];
    hk(g_hk_kb_err.load(), kbs, sizeof kbs);
    hk(g_hk_ms_err.load(), mss, sizeof mss);
    const unsigned ln = g_lat_n.load();
    snprintf(rep, sizeof rep,
             "[MGPU][BM2] recording report: keyboard hook %s, mouse hook %s | Windows called them %u (keys) + %u (mouse) "
             "times | not kept: %u another window in front, %u injected, %u mouse moves of 0 pixels | %zu kept, %zu after trim "
             "| delivery delay avg %.1f ms, max %u ms, hook thread priority %s (InputHookPriority).",
             kbs, mss, g_seen_kb.load(), g_seen_ms.load(), g_drop_fg.load(), g_drop_inj.load(), g_drop_still.load(),
             raw, v.size(), ln ? (double)g_lat_sum.load() / (double)ln : 0.0, g_lat_max.load(),
             g_hook_prio.load() != 0 ? "HIGHEST" : "normal");
    mgpu::diag::info(rep);
    if (secs < 2.0)
    {
        snprintf(l, sizeof l, "Input path too short (%.1f s) - not saved.", secs);
        set_msg(l);
        return;
    }
    if (v.empty())
    {
        // LR102: an empty path was saved and replaced the previous one (two
        // in Cyberpunk, LR100). Nothing to replay - keep what was there.
        snprintf(l, sizeof l, "Nothing was recorded (%.1f s, 0 events) - NOT saved; the previous path is kept. "
                              "See the [BM2] recording report line in the log.", secs);
        set_msg(l);
        char d[400]; snprintf(d, sizeof d, "[MGPU][BM2] %s", l);
        mgpu::diag::warn(d);
        return;
    }
    const bool ok = save_path(v, secs);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_path = v; g_path_s = secs; g_loaded = true;
    }
    snprintf(l, sizeof l, "Input path %s: %.1f s, %zu events. Benchmarks with input replay now play it.",
             ok ? "saved" : "recorded (NOT saved to disk)", secs, v.size());
    set_msg(l);
    char d[400]; snprintf(d, sizeof d, "[MGPU][BM2] %s", l);
    mgpu::diag::info(d);
}

bool recording() { return g_recording.load(); }
bool playing()   { return g_playing.load(); }

bool has_path(double *seconds, unsigned *events)
{
    load_once();
    std::lock_guard<std::mutex> lk(g_mx);
    if (seconds) *seconds = g_path_s;
    if (events) *events = (unsigned)g_path.size();
    return !g_path.empty() && g_path_s >= 2.0;
}

bool start_play()
{
    std::lock_guard<std::mutex> cl(g_ctl_mx);   // LR100: g_play is assigned below
    if (g_recording.load() || g_playing.load()) return false;
    if (!has_path()) return false;
    join_play();
    std::vector<ev> v;
    { std::lock_guard<std::mutex> lk(g_mx); v = g_path; }
    g_abort.store(false);
    g_aborted.store(false);
    g_abort_mouse.store(false);   // LR106
    g_real_move.store(0);
    g_playing.store(true);
    hooks(true, true);     // LR106: keyboard AND mouse - either, used for real, stops playback
    try
    {
        g_play = std::thread(play_main, std::move(v));
    }
    catch (...)
    {
        // LR100: no playback thread - undo what was set up for it.
        g_playing.store(false);
        hooks(false, false);
        return false;
    }
    return true;
}

void stop_play()
{
    // Non-blocking: the playback thread sees this within ~15 ms, releases
    // everything it holds and ends. It is joined on the next start or at shutdown.
    if (g_playing.load()) g_play_stop.store(true);
}

bool aborted() { return g_aborted.load(); }

void emergency_unhook()
{
    // REV: DllMain live unload - no joins under the loader lock; just make
    // sure Windows stops calling into this image. LR100: exchange, so a hook
    // is removed once even if the hook thread is removing it too. (The hook
    // thread is pinned, so it is already gone when this runs on unload.)
    unhook(g_kb);
    unhook(g_ms);
}

void shutdown()
{
    std::lock_guard<std::mutex> cl(g_ctl_mx);   // LR100: no toggle / start while stopping
    g_recording.store(false);
    join_play();
    // LR100: take the thread out under the lock (state: stopping), wait for it
    // with the lock released. Bounded: WM_QUIT is re-posted every 100 ms in
    // case the first was lost (a full queue), for up to 5 s.
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_life_mx);
        h = g_hook_h;
        g_hook_h = nullptr;
        if (!mgpu::thlife::stop_take(g_hook_life, h != nullptr)) { mgpu::thlife::stop_done(g_hook_life, true); return; }
    }
    bool ended = false;
    for (int i = 0; i < 50 && !ended; ++i)
    {
        if (const DWORD t = g_hook_tid.load()) PostThreadMessageW(t, WM_QUIT, 0, 0);
        ended = WaitForSingleObject(h, 100) == WAIT_OBJECT_0;
    }
    std::lock_guard<std::mutex> lk(g_life_mx);
    if (ended)
    {
        CloseHandle(h);
        g_hook_tid.store(0);   // REV4: the next start must wait for the new thread's id
        mgpu::thlife::stop_done(g_hook_life, true);
        return;
    }
    // Never seen; kept safe: the thread is pinned (the add-on stays loaded
    // under it), its handle stays open (its id is never reused), and no second
    // hook thread is started this session (state stays stopping).
    unhook(g_kb);
    unhook(g_ms);
    mgpu::diag::warn("[MGPU][BM2] input path: the hook thread did not stop within 5 s - left running (it keeps "
                     "the add-on loaded); recording / replay input is off until the game restarts.");
}

void hud_line(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    out[0] = '\0';
    if (g_recording.load()) snprintf(out, n, "PATH REC %.0f S", qpc_ms(qpc() - g_rec_t0.load(std::memory_order_relaxed)) / 1000.0);
    else if (g_playing.load()) snprintf(out, n, "INPUT REPLAY");
}

void last_message(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    snprintf(out, n, "%s", g_msg);
}

} // namespace replay
} // namespace mgpu
