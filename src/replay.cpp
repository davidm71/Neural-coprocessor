// ---------------------------------------------------------------------------
// replay.cpp - BM2. See replay.hpp.
//
// Threads: the hook thread owns the low-level hooks and only pumps messages
// (a hook thread that stops pumping delays everyone's input); the playback
// thread sleeps between events and calls SendInput.
// ---------------------------------------------------------------------------
#include "replay.hpp"
#include "capture.hpp"
#include "diag.hpp"

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
LONGLONG g_rec_t0 = 0;
std::atomic<bool> g_recording{false}, g_playing{false}, g_abort{false}, g_aborted{false};
char g_msg[300] = {};

std::atomic<DWORD> g_hook_tid{0};
std::thread g_hook_thread;
HHOOK g_kb = nullptr, g_ms = nullptr;
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
        if (g_recording.load() && !injected && own_foreground())
        {
            ev e{};
            e.ms = qpc_ms(qpc() - g_rec_t0);
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
    if (code == HC_ACTION && g_recording.load())
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if ((m->flags & LLMHF_INJECTED) == 0 && own_foreground())
        {
            ev e{};
            e.ms = qpc_ms(qpc() - g_rec_t0);
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
            if (g_kb == nullptr) g_kb = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, self, 0);
            if (m.wParam != 0 && g_ms == nullptr) g_ms = SetWindowsHookExW(WH_MOUSE_LL, ms_proc, self, 0);
        }
        else if (m.message == WM_HK_REMOVE)
        {
            if (g_kb) { UnhookWindowsHookEx(g_kb); g_kb = nullptr; }
            if (g_ms) { UnhookWindowsHookEx(g_ms); g_ms = nullptr; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_kb) { UnhookWindowsHookEx(g_kb); g_kb = nullptr; }
    if (g_ms) { UnhookWindowsHookEx(g_ms); g_ms = nullptr; }
}

void ensure_hook_thread()
{
    if (g_hook_thread.joinable()) return;
    g_hook_thread = std::thread([] { MSG m; PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                                     g_hook_tid.store(GetCurrentThreadId()); hook_main(); });
    for (int i = 0; i < 500 && g_hook_tid.load() == 0; ++i) Sleep(1);
}

void hooks(bool on, bool mouse)
{
    ensure_hook_thread();
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
    for (const ev &e : path)
    {
        for (;;)
        {
            if (g_play_stop.load()) goto done;
            if (g_abort.load() || !own_foreground()) { stopped_by_user = true; goto done; }
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
        set_msg("Input replay stopped: a key was pressed or another window came to the front.");
        mgpu::diag::warn("[MGPU][BM2] input replay stopped early (a real key, or the game lost the foreground). "
                         "All keys and buttons released.");
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
    if (g_playing.load()) return;
    if (!g_recording.load())
    {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_rec.clear();
            g_rec.reserve(200000);
        }
        g_rec_t0 = qpc();
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
    const double end_ms = qpc_ms(qpc() - g_rec_t0);
    std::vector<ev> v;
    { std::lock_guard<std::mutex> lk(g_mx); v.swap(g_rec); }
    trim(v, end_ms);
    const double secs = end_ms / 1000.0;
    char l[300];
    if (secs < 2.0)
    {
        snprintf(l, sizeof l, "Input path too short (%.1f s) - not saved.", secs);
        set_msg(l);
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
    if (g_recording.load() || g_playing.load()) return false;
    if (!has_path()) return false;
    join_play();
    std::vector<ev> v;
    { std::lock_guard<std::mutex> lk(g_mx); v = g_path; }
    g_abort.store(false);
    g_aborted.store(false);
    g_playing.store(true);
    hooks(true, false);    // keyboard only: a real key stops playback
    g_play = std::thread(play_main, std::move(v));
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
    // sure Windows stops calling into this image.
    if (g_kb) { UnhookWindowsHookEx(g_kb); g_kb = nullptr; }
    if (g_ms) { UnhookWindowsHookEx(g_ms); g_ms = nullptr; }
}

void shutdown()
{
    g_recording.store(false);
    join_play();
    if (g_hook_thread.joinable())
    {
        if (g_hook_tid.load()) PostThreadMessageW(g_hook_tid.load(), WM_QUIT, 0, 0);
        g_hook_thread.join();
        g_hook_tid.store(0);   // REV4: the next ensure_hook_thread must wait for the new thread's id
    }
}

void hud_line(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    out[0] = '\0';
    if (g_recording.load()) snprintf(out, n, "PATH REC %.0f S", qpc_ms(qpc() - g_rec_t0) / 1000.0);
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
