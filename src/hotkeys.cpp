// ---------------------------------------------------------------------------
// hotkeys.cpp - HK1. See hotkeys.hpp.
// ---------------------------------------------------------------------------
#include "hotkeys.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>
#include <thread>

#include "diag.hpp"
#include "gpu1_context.hpp"

namespace mgpu
{
namespace hotkeys
{
namespace
{

enum : unsigned char { M_CTRL = 1, M_ALT = 2, M_SHIFT = 4 };

struct binding { unsigned char mods; unsigned char vk; };

struct def
{
    const char *label;
    const char *key;      // mgpu.ini key the panel writes
    binding     dflt;
    bool        repeat;   // auto-repeat while held
};

const def DEFS[A_COUNT] = {
    { "Arm / transit probe",       "HotkeyArm",        { M_CTRL | M_ALT, VK_F10 },        false },
    { "Hide/show bridge (overlay)", "HotkeyPeek",      { M_CTRL | M_ALT, VK_F6 },         false },
    { "View: output/input/split",  "HotkeyView",       { M_CTRL | M_ALT, VK_F7 },         false },
    { "Intensity: select pass",    "HotkeyIntPass",    { M_CTRL | M_ALT, VK_END },        false },
    { "Intensity down",            "HotkeyIntDown",    { M_CTRL | M_ALT, VK_NEXT },       true  },
    { "Intensity up",              "HotkeyIntUp",      { M_CTRL | M_ALT, VK_PRIOR },      true  },
    { "Split seam left (+SHIFT)",  "HotkeySeamLeft",   { M_CTRL | M_ALT, VK_OEM_COMMA },  true  },
    { "Split seam right (+SHIFT)", "HotkeySeamRight",  { M_CTRL | M_ALT, VK_OEM_PERIOD }, true  },
    { "FPS HUD",                   "HotkeyHud",        { M_CTRL | M_ALT, 'H' },           false },
    { "Frame generation on/off",   "HotkeyFrameGen",   { M_CTRL | M_ALT, 'G' },           false },
    { "HUD-less capture (probe)",  "HotkeyHudProbe",   { M_CTRL | M_ALT, 'J' },           false },
    { "Screenshot",                "HotkeyScreenshot", { M_CTRL | M_ALT, 'P' },           false },
    { "Record video start/stop",   "HotkeyRecord",     { M_CTRL | M_ALT, 'R' },           false },
    { "Benchmark start/stop",      "HotkeyBenchmark",  { M_CTRL | M_ALT, 'B' },           false },
    { "Record input path (replay)", "HotkeyInputPath", { M_CTRL | M_ALT, 'I' },           false },
};

std::mutex g_mx;                       // bindings + conflicts (panel reads them)
binding    g_bind[A_COUNT] = {};
int        g_conf[A_COUNT] = {};
bool       g_inited = false;
std::atomic<bool> g_recheck{false};
std::atomic<int>  g_cap{-1};           // action being captured
bool       g_cap_armed = false;        // bridge thread: all keys were up once

// Bridge thread state.
bool       g_down[A_COUNT] = {};
ULONGLONG  g_next_rep[A_COUNT] = {};
bool       g_down_coarse[2] = {};
ULONGLONG  g_next_rep_coarse[2] = {};

// HK2: keys of our combinations that the keyboard hook swallowed. A swallowed
// key never reaches the game - and never updates GetAsyncKeyState either, so
// the poll reads these alongside it.
std::atomic<bool> g_swallowed[256] = {};
std::atomic<unsigned> g_swallow_n{0};    // HK2: keys kept from the game (logged by the poll)
std::atomic<unsigned> g_swallow_vk{0};

bool key(int vk)
{
    if (vk > 0 && vk < 256 && g_swallowed[vk].load(std::memory_order_relaxed)) return true;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

unsigned char mods_now()
{
    unsigned char m = 0;
    if (key(VK_CONTROL)) m |= M_CTRL;
    if (key(VK_MENU))    m |= M_ALT;
    if (key(VK_SHIFT))   m |= M_SHIFT;
    return m;
}

bool is_modifier_or_mouse(int vk)
{
    switch (vk)
    {
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
    case VK_CANCEL:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

int encode(binding b) { return ((int)b.mods << 8) | (int)b.vk; }

bool decode(int v, binding &b)
{
    if (v <= 0 || v > 0x7FFF) return false;
    const int vk = v & 0xFF, m = (v >> 8) & 0xFF;
    if (vk == 0 || is_modifier_or_mouse(vk) || (m & ~7) != 0) return false;
    b.mods = (unsigned char)m; b.vk = (unsigned char)vk;
    return true;
}

void key_name(int vk, char *out, unsigned n)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, n, "%c", (char)vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "NUM %d", vk - VK_NUMPAD0); return; }
    const char *s = nullptr;
    switch (vk)
    {
    case VK_PRIOR: s = "PAGE UP"; break;     case VK_NEXT: s = "PAGE DOWN"; break;
    case VK_END: s = "END"; break;           case VK_HOME: s = "HOME"; break;
    case VK_INSERT: s = "INSERT"; break;     case VK_DELETE: s = "DELETE"; break;
    case VK_LEFT: s = "LEFT"; break;         case VK_RIGHT: s = "RIGHT"; break;
    case VK_UP: s = "UP"; break;             case VK_DOWN: s = "DOWN"; break;
    case VK_SPACE: s = "SPACE"; break;       case VK_RETURN: s = "ENTER"; break;
    case VK_TAB: s = "TAB"; break;           case VK_BACK: s = "BACKSPACE"; break;
    case VK_PAUSE: s = "PAUSE"; break;       case VK_SNAPSHOT: s = "PRINT SCREEN"; break;
    case VK_OEM_COMMA: s = ","; break;       case VK_OEM_PERIOD: s = "."; break;
    case VK_OEM_MINUS: s = "-"; break;       case VK_OEM_PLUS: s = "="; break;
    case VK_OEM_1: s = ";"; break;           case VK_OEM_2: s = "/"; break;
    case VK_OEM_3: s = "`"; break;           case VK_OEM_4: s = "["; break;
    case VK_OEM_5: s = "\\"; break;          case VK_OEM_6: s = "]"; break;
    case VK_OEM_7: s = "'"; break;
    case VK_MULTIPLY: s = "NUM *"; break;    case VK_ADD: s = "NUM +"; break;
    case VK_SUBTRACT: s = "NUM -"; break;    case VK_DIVIDE: s = "NUM /"; break;
    case VK_DECIMAL: s = "NUM ."; break;
    default: break;
    }
    if (s != nullptr) { snprintf(out, n, "%s", s); return; }
    snprintf(out, n, "KEY 0x%02X", vk);
}

void bind_name(binding b, char *out, unsigned n)
{
    char k[32];
    key_name(b.vk, k, sizeof k);
    snprintf(out, n, "%s%s%s%s",
             (b.mods & M_CTRL) ? "CTRL+" : "", (b.mods & M_ALT) ? "ALT+" : "",
             (b.mods & M_SHIFT) ? "SHIFT+" : "", k);
}

UINT win_mods(unsigned char m)
{
    UINT r = 0;
    if (m & M_CTRL)  r |= MOD_CONTROL;
    if (m & M_ALT)   r |= MOD_ALT;
    if (m & M_SHIFT) r |= MOD_SHIFT;
    return r;
}

// Bridge thread. Holds g_mx. A test registration, released at once: if
// Windows refuses it, another program has the same keys and will also react.
void recheck_locked(bool log)
{
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        g_conf[a] = 0;
        for (unsigned b = 0; b < A_COUNT; ++b)
            if (b != a && g_bind[b].vk == g_bind[a].vk && g_bind[b].mods == g_bind[a].mods)
                g_conf[a] = 1;
        if (g_conf[a] != 0) continue;
        if (g_bind[a].mods == 0 || g_bind[a].mods == M_SHIFT) { g_conf[a] = 3; continue; }
        const int id = 0xB100 + (int)a;
        if (RegisterHotKey(nullptr, id, win_mods(g_bind[a].mods), g_bind[a].vk) != FALSE)
            UnregisterHotKey(nullptr, id);
        else
            g_conf[a] = 2;
    }
    if (!log) return;
    char line[1800];
    int w = snprintf(line, sizeof line, "[MGPU][HK1] hotkeys (read only while the game or the bridge window is in front; "
                                        "rebind in the panel):");
    unsigned bad = 0;
    for (unsigned a = 0; a < A_COUNT && w > 0 && w < (int)sizeof line; ++a)
    {
        char nm[48];
        bind_name(g_bind[a], nm, sizeof nm);
        w += snprintf(line + w, sizeof line - (size_t)w, " %s=%s%s", DEFS[a].key + 6, nm,
                      g_conf[a] == 1 ? "(DUPLICATE)" : g_conf[a] == 2 ? "(ALSO USED BY ANOTHER PROGRAM)"
                      : g_conf[a] == 3 ? "(NO MODIFIER)" : "");
        if (g_conf[a] == 1 || g_conf[a] == 2) ++bad;
    }
    if (bad != 0) mgpu::diag::warn(line); else mgpu::diag::info(line);
}

void save(unsigned a)
{
    (void)mgpu::gpu1::ui_ini_write(DEFS[a].key, encode(g_bind[a]));
}

bool match(const binding &b, unsigned char m, bool allow_shift)
{
    if (b.vk == 0 || !key(b.vk)) return false;
    const unsigned char need = b.mods;
    const unsigned char have = allow_shift ? (unsigned char)(m & ~M_SHIFT) : m;
    const unsigned char want = allow_shift ? (unsigned char)(need & ~M_SHIFT) : need;
    return have == want;
}

// Edge + optional auto-repeat. Returns the number of presses to report.
unsigned edge(bool now, bool &was, ULONGLONG &next, bool repeat, ULONGLONG t)
{
    unsigned n = 0;
    if (now && !was) { n = 1; next = t + 400ull; }
    else if (now && repeat && t >= next) { n = 1; next = t + 60ull; }
    was = now;
    return n;
}

bool own_foreground()
{
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---- HK2: keep our combinations away from the game ----
// The poll only reads the keyboard, so the game saw every combination too:
// CTRL+ALT+H also triggered RoboCop's heal (bound to H). A low-level keyboard
// hook now discards the main key of a combination that is one of ours - the
// game sees CTRL and ALT, never the H. Only bindings with CTRL or ALT are
// guarded (a plain key belongs to the game), only while this process is in
// front, and never injected keys (input replay).
HHOOK g_kbhook = nullptr;
std::atomic<DWORD> g_kb_tid{0};
std::thread *g_kb_thread = nullptr;
binding g_guard[A_COUNT] = {};        // copy of the bindings the hook reads
std::atomic<unsigned> g_guard_n{0};
std::mutex g_guard_mx;

unsigned char hook_mods()
{
    unsigned char m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= M_CTRL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    m |= M_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   m |= M_SHIFT;
    return m;
}

LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
        const DWORD vk = k->vkCode;
        const bool up = (k->flags & LLKHF_UP) != 0;
        if (vk < 256 && (k->flags & LLKHF_INJECTED) == 0)
        {
            // The release of a key we swallowed is swallowed too, whatever the
            // modifiers are by then: the game never saw it go down.
            if (up && g_swallowed[vk].load(std::memory_order_relaxed))
            {
                g_swallowed[vk].store(false, std::memory_order_relaxed);
                return 1;
            }
            if (!up && g_cap.load() < 0 && own_foreground())
            {
                const unsigned char m = hook_mods();
                if ((m & (M_CTRL | M_ALT)) != 0)
                {
                    std::lock_guard<std::mutex> gl(g_guard_mx);
                    for (unsigned a = 0; a < A_COUNT; ++a)
                    {
                        const binding &b = g_guard[a];
                        if (b.vk != vk || (b.mods & (M_CTRL | M_ALT)) == 0) continue;
                        // The seam keys also fire with SHIFT added (coarse step).
                        const unsigned char have = (a == A_SEAM_L || a == A_SEAM_R) ? (unsigned char)(m & ~M_SHIFT) : m;
                        const unsigned char want = (a == A_SEAM_L || a == A_SEAM_R) ? (unsigned char)(b.mods & ~M_SHIFT) : b.mods;
                        if (have == want)
                        {
                            if (!g_swallowed[vk].load(std::memory_order_relaxed))
                            {
                                g_swallow_vk.store(vk | ((unsigned)m << 8), std::memory_order_relaxed);
                                g_swallow_n.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_swallowed[vk].store(true, std::memory_order_relaxed);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void kb_main()
{
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_kb_tid.store(GetCurrentThreadId());
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&kb_proc), &self);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_kbhook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, self, 0);
    if (g_kbhook == nullptr)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][HK2] keyboard hook not installed (error %lu) - the game will also see "
                 "the hotkeys' keys.", GetLastError());
        mgpu::diag::warn(l);
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_kbhook) { UnhookWindowsHookEx(g_kbhook); g_kbhook = nullptr; }
}

void guard_refresh_locked()   // under g_mx
{
    std::lock_guard<std::mutex> gl(g_guard_mx);
    memcpy(g_guard, g_bind, sizeof g_guard);
}

} // namespace

void emergency_unhook()
{
    if (g_kbhook) { UnhookWindowsHookEx(g_kbhook); g_kbhook = nullptr; }
}

void init()
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        binding b{};
        g_bind[a] = decode(mgpu::gpu1::ui_ini_read(DEFS[a].key, -1), b) ? b : DEFS[a].dflt;
    }
    g_inited = true;
    recheck_locked(true);
    guard_refresh_locked();   // HK2
    if (g_kb_thread == nullptr)
    {
        g_kb_thread = new std::thread(kb_main);   // lives for the process; DllMain unhooks on unload
        g_kb_thread->detach();
        mgpu::diag::info("[MGPU][HK2] hotkey keys are kept from the game (a CTRL/ALT combination of ours "
                         "never reaches it).");
    }
}

bool poll(presses &out)
{
    memset(&out, 0, sizeof out);
    if (!g_inited) return false;
    {   // HK2: say (the first few times) that a key was kept from the game.
        static unsigned said = 0, seen = 0;
        const unsigned n = g_swallow_n.load(std::memory_order_relaxed);
        if (n != seen)
        {
            seen = n;
            if (said < 5u)
            {
                ++said;
                const unsigned v = g_swallow_vk.load(std::memory_order_relaxed);
                char nm[48], l[160];
                bind_name(binding{ (unsigned char)(v >> 8), (unsigned char)(v & 0xFF) }, nm, sizeof nm);
                snprintf(l, sizeof l, "[MGPU][HK2] kept %s from the game (only CTRL/ALT/SHIFT reached it).", nm);
                mgpu::diag::info(l);
            }
        }
    }
    if (g_recheck.exchange(false))
    {
        std::lock_guard<std::mutex> lk(g_mx);
        recheck_locked(true);
    }
    if (!own_foreground())
    {
        // Nothing is held while another program is in front.
        memset(g_down, 0, sizeof g_down);
        g_down_coarse[0] = g_down_coarse[1] = false;
        return false;
    }

    // ---- capture: the next combination pressed becomes the binding ----
    const int cap = g_cap.load();
    if (cap >= 0 && cap < (int)A_COUNT)
    {
        int hit = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (!is_modifier_or_mouse(vk) && key(vk)) { hit = vk; break; }
        if (!g_cap_armed) { if (hit == 0) g_cap_armed = true; return false; }
        if (hit == 0) return false;
        if (hit == VK_ESCAPE) { g_cap.store(-1); return false; }
        char nm[48];
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_bind[cap].mods = mods_now();
            g_bind[cap].vk = (unsigned char)hit;
            save((unsigned)cap);
            guard_refresh_locked();   // HK2
            bind_name(g_bind[cap], nm, sizeof nm);
            recheck_locked(false);
        }
        g_cap.store(-1);
        // The key that was just bound must not also fire.
        g_down[cap] = true;
        char l[200];
        snprintf(l, sizeof l, "[MGPU][HK1] %s is now %s (saved to mgpu.ini).", DEFS[cap].label, nm);
        mgpu::diag::info(l);
        g_recheck.store(true);
        return false;
    }

    binding b[A_COUNT];
    {
        std::lock_guard<std::mutex> lk(g_mx);
        memcpy(b, g_bind, sizeof b);
    }
    const ULONGLONG t = GetTickCount64();
    const unsigned char m = mods_now();
    bool any = false;
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        const bool seam = (a == A_SEAM_L || a == A_SEAM_R);
        if (seam)
        {
            // Plain = fine step, with SHIFT added = coarse step.
            const bool plain  = match(b[a], m, false);
            const bool coarse = !plain && (b[a].mods & M_SHIFT) == 0 &&
                                match(binding{ (unsigned char)(b[a].mods | M_SHIFT), b[a].vk }, m, false);
            out.n[a] += edge(plain, g_down[a], g_next_rep[a], true, t);
            const unsigned ci = (a == A_SEAM_L) ? 0u : 1u;
            const unsigned c = edge(coarse, g_down_coarse[ci], g_next_rep_coarse[ci], true, t);
            out.seam_coarse += (a == A_SEAM_L) ? -(int)c : (int)c;
            if (c) any = true;
        }
        else
        {
            out.n[a] += edge(match(b[a], m, false), g_down[a], g_next_rep[a], DEFS[a].repeat, t);
        }
        if (out.n[a]) any = true;
    }
    return any;
}

unsigned count() { return A_COUNT; }

const char *label(unsigned a) { return (a < A_COUNT) ? DEFS[a].label : ""; }

void name(unsigned a, char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    if (a >= A_COUNT) { out[0] = '\0'; return; }
    std::lock_guard<std::mutex> lk(g_mx);
    bind_name(g_inited ? g_bind[a] : DEFS[a].dflt, out, n);
}

int conflict(unsigned a)
{
    if (a >= A_COUNT) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    return g_conf[a];
}

void begin_capture(unsigned a)
{
    if (a >= A_COUNT) return;
    g_cap_armed = false;
    g_cap.store((int)a);
}

void cancel_capture() { g_cap.store(-1); }

int capturing() { return g_cap.load(); }

void reset_defaults()
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (unsigned a = 0; a < A_COUNT; ++a) { g_bind[a] = DEFS[a].dflt; save(a); }
        guard_refresh_locked();   // HK2
    }
    g_recheck.store(true);
    mgpu::diag::info("[MGPU][HK1] hotkeys reset to the defaults.");
}

} // namespace hotkeys
} // namespace mgpu
