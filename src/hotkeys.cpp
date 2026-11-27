// ---------------------------------------------------------------------------
// hotkeys.cpp - HK1. See hotkeys.hpp.
// ---------------------------------------------------------------------------
#include "pinthread.hpp"   // TL2
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
    // HK11 (IN76): these five were END, PAGE DOWN, PAGE UP, comma and period -
    // outside HK10's Ctrl/Alt + A-Z/F1-F12 rule. Adjacent F-keys keep the pairs.
    // HK15 (IN79): F8 / F11 / F12 (IN76-78) were already registered by another
    // program on the user's PC (HK1: "ALSO USED BY ANOTHER PROGRAM"). F1/F2 sit
    // beside the seam keys F3/F4; F5 starts the next group.
    { "Intensity: select pass",    "HotkeyIntPass",    { M_CTRL | M_ALT, VK_F5 },         false },
    { "Intensity down",            "HotkeyIntDown",    { M_CTRL | M_ALT, VK_F1 },         true  },
    { "Intensity up",              "HotkeyIntUp",      { M_CTRL | M_ALT, VK_F2 },         true  },
    { "Split seam left (+SHIFT)",  "HotkeySeamLeft",   { M_CTRL | M_ALT, VK_F3 },         true  },
    { "Split seam right (+SHIFT)", "HotkeySeamRight",  { M_CTRL | M_ALT, VK_F4 },         true  },
    { "FPS HUD",                   "HotkeyHud",        { M_CTRL | M_ALT, 'H' },           false },
    { "Frame generation on/off",   "HotkeyFrameGen",   { M_CTRL | M_ALT, 'G' },           false },
    { "HUD-less capture (probe)",  "HotkeyHudProbe",   { M_CTRL | M_ALT, 'J' },           false },
    { "Screenshot",                "HotkeyScreenshot", { M_CTRL | M_ALT, 'P' },           false },
    { "Record video start/stop",   "HotkeyRecord",     { M_CTRL | M_ALT, 'R' },           false },
    { "Benchmark start/stop",      "HotkeyBenchmark",  { M_CTRL | M_ALT, 'B' },           false },
    { "Record input path (replay)", "HotkeyInputPath", { M_CTRL | M_ALT, 'I' },           false },
    { "Clock show/hide",           "HotkeyClock",      { M_CTRL | M_ALT, 'C' },           false },
    { "Stopwatch show/hide",       "HotkeyStopwatch",  { M_CTRL | M_ALT, 'S' },           false },
    { "Benchmark sweep start/stop", "HotkeySweep",     { M_CTRL | M_ALT, 'W' },           false },
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

// HK3: every key's up/down state as the low-level keyboard hook sees it.
// While ReShade blocks the game's input (the MGPU panel over the picture,
// PanelOverPicture=1) it answers GetAsyncKeyState with "not pressed" for the
// whole process, so the capture field never saw a key. The hook runs before
// ReShade and is not affected. Generic VK_CONTROL / VK_MENU / VK_SHIFT are
// derived from the left/right keys the hook reports.
std::atomic<bool> g_hk_down[256] = {};
std::atomic<unsigned long long> g_act_key{0}, g_act_mouse{0};   // SV3: real input times (see act_proc)
// HK6: when each key last went down (or auto-repeated) as the hook saw it.
std::atomic<unsigned long long> g_hk_t[256] = {};

// HK6: is a key held, for the CAPTURE field. The hook's table can keep a key
// "down" for ever when its release never reaches the hook - HOME, the panel
// key, after ReShade toggles its overlay (2026-11-08 08:54:43: "HOME (0x24)
// still reads as held"); the capture then waited for all keys to be released,
// and once armed it found HOME first and refused it (the panel key) - for
// ever. A held key auto-repeats every few dozen ms after the first ~0.25-1 s,
// so a key not seen going down for over 1.5 s is taken as released. Modifiers
// do not repeat while another key is pressed, so they are checked as before.
// KS4: the key's physical state WITHOUT ReShade's filter. ReShade hooks
// user32!GetAsyncKeyState and answers 0 for every key while it blocks the
// game's keyboard - which it does the whole time the MGPU panel is up. So
// when the hook below misses a key (IN72, RoboCop: slider picked for 43 s, the
// hook saw no arrow press at all), the old fallback could not see it either.
// win32u!NtUserGetAsyncKeyState is the system call user32's function wraps;
// ReShade does not hook it. Falls back to GetAsyncKeyState if absent.
static SHORT raw_async(int vk)
{
    using fn_t = SHORT (WINAPI *)(int);
    static fn_t fn = []() -> fn_t {
        HMODULE m = GetModuleHandleW(L"win32u.dll");
        if (m == nullptr) m = LoadLibraryW(L"win32u.dll");
        fn_t f = m ? reinterpret_cast<fn_t>(GetProcAddress(m, "NtUserGetAsyncKeyState")) : nullptr;
        return f != nullptr ? f : &GetAsyncKeyState;
    }();
    return fn(vk);
}

bool cap_key(int vk)
{
    if (vk <= 0 || vk >= 256) return false;
    if (g_hk_down[vk].load(std::memory_order_relaxed) &&
        GetTickCount64() - g_hk_t[vk].load(std::memory_order_relaxed) < 1500ull)
        return true;
    return (raw_async(vk) & 0x8000) != 0;   // KS4
}

bool key(int vk)
{
    if (vk > 0 && vk < 256 && g_swallowed[vk].load(std::memory_order_relaxed)) return true;
    if (vk > 0 && vk < 256 && g_hk_down[vk].load(std::memory_order_relaxed)) return true;
    return (raw_async(vk) & 0x8000) != 0;   // KS4: not ReShade's frozen answer
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

// HK12: every change of a binding - a new selection, a reset, a repair at
// start - is written into mgpu.ini at once (the line is replaced in place;
// later copies of it are dropped). It used to wait in the 400 ms write queue,
// which a game closed hard in that moment would lose.
void save(unsigned a)
{
    if (!mgpu::gpu1::ui_ini_write_now(DEFS[a].key, encode(g_bind[a])))
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][HK12] could not write %s to mgpu.ini - the change holds for this session only.",
                 DEFS[a].key);
        mgpu::diag::warn(l);
    }
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
std::atomic<unsigned> g_arrow[3];   // KS3: LEFT, RIGHT (no CTRL/ALT), any arrow
std::atomic<unsigned long long> g_esc_until{0};   // PO3: the panel was drawn - Esc is ours until then
std::atomic<unsigned long long> g_hk_any_t[256];  // HK14: last key-down the hook saw (injected too)
bool g_kb_started = false;   // TL2: the pinned hook thread was started
binding g_guard[A_COUNT] = {};        // copy of the bindings the hook reads
std::atomic<unsigned> g_guard_n{0};
std::mutex g_guard_mx;

unsigned char hook_mods()
{
    unsigned char m = 0;
    if (key(VK_CONTROL)) m |= M_CTRL;   // HK3: hook state first, then GetAsyncKeyState
    if (key(VK_MENU))    m |= M_ALT;
    if (key(VK_SHIFT))   m |= M_SHIFT;
    return m;
}

LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
        const DWORD vk = k->vkCode;
        const bool up = (k->flags & LLKHF_UP) != 0;
        const bool inj = (k->flags & LLKHF_INJECTED) != 0;
        // KS3: count arrow presses (injected ones too - some keyboard software
        // sends every key that way). A count cannot get stuck the way a held
        // state can when a release is missed. With CTRL/ALT they belong to the
        // split-view hotkeys and only count as "any".
        if (!up && vk < 256) g_hk_any_t[vk].store(GetTickCount64(), std::memory_order_relaxed);   // HK14
        if (!up && (vk == VK_LEFT || vk == VK_RIGHT))
        {
            g_arrow[2].fetch_add(1, std::memory_order_relaxed);
            if (!g_hk_down[VK_CONTROL].load(std::memory_order_relaxed) && !g_hk_down[VK_MENU].load(std::memory_order_relaxed))
                g_arrow[vk == VK_RIGHT ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        }
        // HK7: while a hotkey is being captured, keys marked "injected" count too.
        // Some keyboard software (remappers, macro tools, Steam Input) sends every
        // key that way, and the hook used to ignore them - the capture then saw
        // nothing at all. (Input replay is not running during a capture.)
        if (vk < 256 && inj && g_cap.load() >= 0)
        {
            g_hk_down[vk].store(!up, std::memory_order_relaxed);
            if (!up) g_hk_t[vk].store(GetTickCount64(), std::memory_order_relaxed);
        }
        if (vk < 256 && !inj)
        {
            if (!up) g_act_key.store(GetTickCount64(), std::memory_order_relaxed);   // SV3
            if (!up) g_hk_t[vk].store(GetTickCount64(), std::memory_order_relaxed);  // HK6
            // HK3: record the key's state for key() / the capture field.
            g_hk_down[vk].store(!up, std::memory_order_relaxed);
            auto pair = [](int l, int r, int g) {
                g_hk_down[g].store(g_hk_down[l].load(std::memory_order_relaxed) ||
                                   g_hk_down[r].load(std::memory_order_relaxed), std::memory_order_relaxed);
            };
            if (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL) pair(VK_LCONTROL, VK_RCONTROL, VK_CONTROL);
            if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU)          pair(VK_LMENU, VK_RMENU, VK_MENU);
            if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT)       pair(VK_LSHIFT, VK_RSHIFT, VK_SHIFT);

            // The release of a key we swallowed is swallowed too, whatever the
            // modifiers are by then: the game never saw it go down.
            if (up && g_swallowed[vk].load(std::memory_order_relaxed))
            {
                g_swallowed[vk].store(false, std::memory_order_relaxed);
                return 1;
            }
            // PO3: the Esc that closes the panel never reaches the game (key()
            // still reads it as down through g_swallowed until it is released).
            if (!up && vk == VK_ESCAPE && GetTickCount64() < g_esc_until.load(std::memory_order_relaxed) && own_foreground())
            {
                g_swallowed[vk].store(true, std::memory_order_relaxed);
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

// PP3: a low-level MOUSE hook, on this same thread, installed only while the
// MGPU panel is up over the picture. ReShade answers GetCursorPos and
// GetAsyncKeyState with frozen values while it blocks the game's input, and
// filters the game window's mouse messages, so without the scaler's cover
// window the panel had no working mouse. The hook sees the real events first
// and passes every one of them on.
HHOOK g_mshook = nullptr;
std::atomic<bool> g_ms_valid{false};
std::atomic<int>  g_ms_x{0}, g_ms_y{0}, g_ms_wheel{0};
std::atomic<unsigned> g_ms_btn{0};
const UINT WM_MS_ON  = WM_APP + 0x31;
const UINT WM_MS_OFF = WM_APP + 0x32;
const UINT WM_KB_REHOOK = WM_APP + 0x33;   // HK8
const UINT WM_KB_PING = WM_APP + 0x35;     // HK14: is the hook thread answering?
std::atomic<unsigned long long> g_kb_pong{0};   // HK14: tick of the last ping it answered

// SV3: real input only, for the screen saver. Windows' own idle timer
// (GetLastInputInfo) was reset every 45-65 s with nobody at the PC (2026-11-07):
// injected input or mouse jitter. These record a key press or mouse action
// that is not injected; a move counts only once the pointer is 4 px or more
// from where the last counted one left it, so sensor jitter does not.
std::atomic<bool> g_act_hooked{false};
HHOOK g_acthook = nullptr;
LONG g_act_x = 0, g_act_y = 0;   // hook thread only
LRESULT CALLBACK act_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if ((m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) == 0)
        {
            bool real = wp != WM_MOUSEMOVE;
            if (!real)
            {
                const LONG dx = m->pt.x - g_act_x, dy = m->pt.y - g_act_y;
                real = dx > 3 || dx < -3 || dy > 3 || dy < -3;
            }
            if (real)
            {
                g_act_x = m->pt.x; g_act_y = m->pt.y;
                g_act_mouse.store(GetTickCount64(), std::memory_order_relaxed);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK ms_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        g_ms_x.store(m->pt.x, std::memory_order_relaxed);
        g_ms_y.store(m->pt.y, std::memory_order_relaxed);
        unsigned b = g_ms_btn.load(std::memory_order_relaxed);
        switch (wp)
        {
        case WM_LBUTTONDOWN: b |= 1u;  break;
        case WM_LBUTTONUP:   b &= ~1u; break;
        case WM_RBUTTONDOWN: b |= 2u;  break;
        case WM_RBUTTONUP:   b &= ~2u; break;
        case WM_MBUTTONDOWN: b |= 4u;  break;
        case WM_MBUTTONUP:   b &= ~4u; break;
        case WM_MOUSEWHEEL:  g_ms_wheel.fetch_add((int)(short)HIWORD(m->mouseData), std::memory_order_relaxed); break;
        default: break;
        }
        g_ms_btn.store(b, std::memory_order_relaxed);
        g_ms_valid.store(true, std::memory_order_relaxed);
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // RV1: as Frame Warp's hook
    g_kbhook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, self, 0);
    g_acthook = SetWindowsHookExW(WH_MOUSE_LL, act_proc, self, 0);   // SV3
    g_act_hooked.store(g_kbhook != nullptr && g_acthook != nullptr);
    if (g_kbhook == nullptr)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][HK2] keyboard hook not installed (error %lu) - the game will also see "
                 "the hotkeys' keys.", GetLastError());
        mgpu::diag::warn(l);
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.hwnd == nullptr && msg.message == WM_KB_PING)   // HK14
        {
            g_kb_pong.store((unsigned long long)msg.wParam, std::memory_order_relaxed);
            continue;
        }
        // HK8: put the keyboard hook back. Windows removes a low-level hook
        // without telling anyone when it is slow to answer a few times.
        // Re-installed when the panel opens and when a capture starts.
        if (msg.hwnd == nullptr && (msg.message == WM_KB_REHOOK || msg.message == WM_MS_ON))
        {
            if (g_kbhook) UnhookWindowsHookEx(g_kbhook);
            g_kbhook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, self, 0);
            if (msg.message == WM_KB_REHOOK) continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_MS_ON)   // PP3
        {
            if (g_mshook == nullptr)
            {
                g_ms_btn.store(0); g_ms_wheel.store(0); g_ms_valid.store(false);
                g_mshook = SetWindowsHookExW(WH_MOUSE_LL, ms_proc, self, 0);
                if (g_mshook == nullptr)
                {
                    char l[160];
                    snprintf(l, sizeof l, "[MGPU][PP3] panel mouse hook not installed (error %lu).", GetLastError());
                    mgpu::diag::warn(l);
                }
            }
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_MS_OFF)
        {
            if (g_mshook) { UnhookWindowsHookEx(g_mshook); g_mshook = nullptr; }
            g_ms_valid.store(false); g_ms_btn.store(0);
            continue;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (g_mshook) { UnhookWindowsHookEx(g_mshook); g_mshook = nullptr; }
    if (g_acthook) { UnhookWindowsHookEx(g_acthook); g_acthook = nullptr; }   // SV3
    g_act_hooked.store(false);
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
    if (g_mshook) { UnhookWindowsHookEx(g_mshook); g_mshook = nullptr; }
    if (g_acthook) { UnhookWindowsHookEx(g_acthook); g_acthook = nullptr; }   // TL2: SV3's hook too
    // TL2: and let the (pinned) thread leave its message loop and drop its
    // reference on the add-on.
    if (const DWORD t = g_kb_tid.load()) PostThreadMessageW(t, WM_QUIT, 0, 0);
}

// PP3
void panel_mouse(bool on)
{
    const DWORD t = g_kb_tid.load();
    if (t != 0) PostThreadMessageW(t, on ? WM_MS_ON : WM_MS_OFF, 0, 0);
}
bool panel_mouse_get(int *x, int *y, unsigned *btn, int *wheel)
{
    if (g_mshook == nullptr || !g_ms_valid.load(std::memory_order_relaxed)) return false;
    if (x) *x = g_ms_x.load(std::memory_order_relaxed);
    if (y) *y = g_ms_y.load(std::memory_order_relaxed);
    if (btn) *btn = g_ms_btn.load(std::memory_order_relaxed);
    if (wheel) *wheel = g_ms_wheel.exchange(0, std::memory_order_relaxed);
    return true;
}
bool key_down(int vk) { return key(vk); }
// KS4: public (outside the anonymous namespace - IN73 left it inside: LNK2019).
bool raw_key(int vk) { return vk > 0 && vk < 256 && (raw_async(vk) & 0x8000) != 0; }
bool real_input_ages(unsigned long long &key_ms, unsigned long long &mouse_ms)   // SV3
{
    if (!g_act_hooked.load(std::memory_order_relaxed)) return false;
    const unsigned long long now = GetTickCount64();
    const unsigned long long k = g_act_key.load(std::memory_order_relaxed), m = g_act_mouse.load(std::memory_order_relaxed);
    key_ms = k ? now - k : ~0ull;
    mouse_ms = m ? now - m : ~0ull;
    return true;
}

// HK5: ReShade's overlay key (the one that opens this panel) can never be one
// of ours. Read from ReShade.ini next to the game (KeyOverlay=vk,ctrl,shift,alt);
// HOME when it is not there.
binding g_overlay{ 0, VK_HOME };
void read_overlay_key()
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    wchar_t *slash = (n != 0 && n < MAX_PATH * 2) ? wcsrchr(path, L'\\') : nullptr;
    if (slash == nullptr) return;
    wcscpy_s(slash + 1, (MAX_PATH * 2) - (size_t)(slash + 1 - path), L"ReShade.ini");
    wchar_t v[64] = {};
    if (GetPrivateProfileStringW(L"INPUT", L"KeyOverlay", L"", v, 64, path) == 0) return;
    int vk = 0, c = 0, s = 0, al = 0;
    if (swscanf_s(v, L"%d,%d,%d,%d", &vk, &c, &s, &al) < 1 || vk <= 0 || vk > 0xFE) return;
    g_overlay.vk = (unsigned char)vk;
    g_overlay.mods = (unsigned char)((c ? M_CTRL : 0) | (al ? M_ALT : 0) | (s ? M_SHIFT : 0));
}
bool is_overlay(const binding &b) { return b.vk == g_overlay.vk && b.mods == g_overlay.mods; }
bool overlay_key_has_shift() { return (g_overlay.mods & M_SHIFT) != 0; }
int overlay_vk() { return g_overlay.vk; }
void panel_escape_alive() { g_esc_until.store(GetTickCount64() + 250ull, std::memory_order_relaxed); }
void arrow_counts(unsigned out[3]) { for (int i = 0; i < 3; ++i) out[i] = g_arrow[i].load(std::memory_order_relaxed); }
void rehook_keyboard() { if (const DWORD t = g_kb_tid.load()) PostThreadMessageW(t, WM_KB_REHOOK, 0, 0); }
bool kb_hook_state(unsigned long long &last_key_ms)
{
    const unsigned long long k = g_act_key.load(std::memory_order_relaxed);
    last_key_ms = k ? GetTickCount64() - k : ~0ull;
    return g_kbhook != nullptr;
}
bool in_front() { return own_foreground(); }

static bool hk_key_ok(int vk);   // HK10, below
bool fixed(unsigned a) { return a == A_TRANSIT || a == A_PEEK; }   // HK13
void init()
{
    std::lock_guard<std::mutex> lk(g_mx);
    read_overlay_key();   // HK5
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        binding b{};
        g_bind[a] = decode(mgpu::gpu1::ui_ini_read(DEFS[a].key, -1), b) ? b : DEFS[a].dflt;
        if (is_overlay(g_bind[a]))   // HK5: an earlier capture took the panel key - put the default back
        {
            g_bind[a] = DEFS[a].dflt;
            save(a);
            char nm[48], l[200];
            bind_name(g_bind[a], nm, sizeof nm);
            snprintf(l, sizeof l, "[MGPU][HK5] %s was bound to ReShade's overlay key - back to %s.", DEFS[a].label, nm);
            mgpu::diag::warn(l);
        }
    }
    // HK13: the fixed two are always their defaults (a value an earlier build
    // saved for them is put back, in mgpu.ini too).
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        if (!fixed(a)) continue;
        if (g_bind[a].vk == DEFS[a].dflt.vk && g_bind[a].mods == DEFS[a].dflt.mods) continue;
        char was[48], nm[48], l[220];
        bind_name(g_bind[a], was, sizeof was);
        g_bind[a] = DEFS[a].dflt;
        save(a);
        bind_name(g_bind[a], nm, sizeof nm);
        snprintf(l, sizeof l, "[MGPU][HK13] %s is fixed at %s (was %s).", DEFS[a].label, nm, was);
        mgpu::diag::warn(l);
    }
    // HK15: a binding still on one of this action's EARLIER defaults moves to
    // the current default (if free) - nobody picks an old default on purpose.
    {
        struct old_def { unsigned a; binding b; };
        static const old_def OLD[] = {
            { A_INT_TARGET, { M_CTRL | M_ALT, VK_END } },  { A_INT_TARGET, { M_CTRL | M_ALT, VK_F8 } },
            { A_INT_DOWN,   { M_CTRL | M_ALT, VK_NEXT } }, { A_INT_DOWN,   { M_CTRL | M_ALT, VK_F11 } },
            { A_INT_UP,     { M_CTRL | M_ALT, VK_PRIOR } },{ A_INT_UP,     { M_CTRL | M_ALT, VK_F12 } },
            { A_SEAM_L,     { M_CTRL | M_ALT, VK_OEM_COMMA } }, { A_SEAM_R, { M_CTRL | M_ALT, VK_OEM_PERIOD } },
        };
        for (const old_def &o : OLD)
        {
            binding &cb = g_bind[o.a];
            if (cb.vk != o.b.vk || cb.mods != o.b.mods) continue;
            bool free = true;
            for (unsigned k = 0; k < A_COUNT; ++k)
                if (k != o.a && g_bind[k].vk == DEFS[o.a].dflt.vk && g_bind[k].mods == DEFS[o.a].dflt.mods) free = false;
            if (!free) continue;
            char was[48], nm[48], l[220];
            bind_name(cb, was, sizeof was);
            cb = DEFS[o.a].dflt;
            save(o.a);
            bind_name(cb, nm, sizeof nm);
            snprintf(l, sizeof l, "[MGPU][HK15] %s was on an earlier default, %s - now %s.", DEFS[o.a].label, was, nm);
            mgpu::diag::warn(l);
        }
    }
    // HK11: a binding saved by an earlier build outside the HK10 rule (Ctrl
    // and/or Alt, no Shift, A-Z or F1-F12 - e.g. the old END / PAGE UP / comma
    // defaults) goes to its default, if that is free; otherwise it stays.
    for (unsigned a = 0; a < A_COUNT; ++a)
    {
        const binding &cb = g_bind[a];
        const bool ok = hk_key_ok(cb.vk) && (cb.mods & (M_CTRL | M_ALT)) != 0 && (cb.mods & M_SHIFT) == 0;
        if (ok) continue;
        bool free = true;
        for (unsigned k = 0; k < A_COUNT; ++k)
            if (k != a && g_bind[k].vk == DEFS[a].dflt.vk && g_bind[k].mods == DEFS[a].dflt.mods) free = false;
        if (!free) continue;
        char was[48], nm[48], l[260];
        bind_name(g_bind[a], was, sizeof was);
        g_bind[a] = DEFS[a].dflt;
        save(a);
        bind_name(g_bind[a], nm, sizeof nm);
        snprintf(l, sizeof l, "[MGPU][HK10] %s was %s, outside Ctrl/Alt + A-Z/F1-F12 - now %s.", DEFS[a].label, was, nm);
        mgpu::diag::warn(l);
    }
    // HK10: duplicates saved by earlier builds (which took them): of each pair,
    // the action that is NOT on its default goes back to its default, if that
    // is free; otherwise it stays and is flagged as before.
    for (unsigned a = 0; a < A_COUNT; ++a)
        for (unsigned b = 0; b < A_COUNT; ++b)
        {
            if (b == a || g_bind[b].vk != g_bind[a].vk || g_bind[b].mods != g_bind[a].mods) continue;
            const bool a_def = g_bind[a].vk == DEFS[a].dflt.vk && g_bind[a].mods == DEFS[a].dflt.mods;
            if (a_def) continue;   // a keeps it; b is handled on its own turn
            bool free = true;
            for (unsigned k = 0; k < A_COUNT; ++k)
                if (k != a && g_bind[k].vk == DEFS[a].dflt.vk && g_bind[k].mods == DEFS[a].dflt.mods) free = false;
            if (!free) continue;
            char was[48], nm[48], l[260];
            bind_name(g_bind[a], was, sizeof was);
            g_bind[a] = DEFS[a].dflt;
            save(a);
            bind_name(g_bind[a], nm, sizeof nm);
            snprintf(l, sizeof l, "[MGPU][HK10] %s had %s, which \"%s\" also uses - back to its default %s.",
                     DEFS[a].label, was, DEFS[b].label, nm);
            mgpu::diag::warn(l);
            break;
        }
    g_inited = true;
    recheck_locked(true);
    guard_refresh_locked();   // HK2
    if (!g_kb_started)
    {
        // TL2: pinned. It lives for the process and sits in GetMessage inside
        // our code; a detached std::thread let ReShade unload the add-on
        // under it. Pinned, the add-on stays loaded until the thread ends
        // (process exit), and emergency_unhook's WM_QUIT lets it end cleanly.
        g_kb_started = mgpu::start_pinned_thread([] { kb_main(); });
        mgpu::diag::info("[MGPU][HK2] hotkey keys are kept from the game (a CTRL/ALT combination of ours "
                         "never reaches it).");
    }
}

// ---- HK10: WHAT A HOTKEY MAY BE ----
// REPORTED 2026-11-22 (IN74): a combination already used by another action was
// taken anyway (flagged DUPLICATE, both then fired). Asked for: refuse it, and
// allow only Ctrl and/or Alt with A-Z or F1-F12. Shift is not allowed (the
// split-view seam keys add it themselves for the coarse step). Every capture
// path - typed (hook or panel) and the mouse picker - goes through here.
static bool hk_key_ok(int vk) { return (vk >= 'A' && vk <= 'Z') || (vk >= VK_F1 && vk <= VK_F12); }
static char g_refuse[200] = "";                        // the last refusal, for the panel
static std::atomic<unsigned long long> g_refuse_t{0};
// nullptr = may be bound to `cap`; otherwise why not. Caller holds g_mx.
static const char *refuse_locked(int cap, unsigned char mods, int vk, char *buf, size_t n)
{
    if (!hk_key_ok(vk)) return "only A-Z or F1-F12 can be a hotkey's key";
    if ((mods & (M_CTRL | M_ALT)) == 0) return "a hotkey needs Ctrl and/or Alt";
    if ((mods & M_SHIFT) != 0) return "Shift is not allowed in a hotkey";
    if (is_overlay(binding{ mods, (unsigned char)vk })) return "that is ReShade's panel key";
    for (unsigned a = 0; a < A_COUNT; ++a)
        if ((int)a != cap && g_bind[a].vk == (unsigned char)vk && g_bind[a].mods == mods)
        {
            snprintf(buf, n, fixed(a) ? "reserved for \"%s\" (fixed)" : "already used by \"%s\"", DEFS[a].label);
            return buf;
        }
    return nullptr;
}
// Says why (log at most every 3 s, panel text always) and returns true when refused.
static bool refuse_capture(int cap, unsigned char mods, int vk)
{
    char buf[120];
    const char *why;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        why = refuse_locked(cap, mods, vk, buf, sizeof buf);
        if (why == nullptr) return false;
        char nm[48];
        bind_name(binding{ mods, (unsigned char)vk }, nm, sizeof nm);
        snprintf(g_refuse, sizeof g_refuse, "%s not taken: %s.", nm, why);
    }
    const unsigned long long now = GetTickCount64();
    static std::atomic<unsigned long long> said{0};   // IN81: the hook path and the panel path both get here
    unsigned long long was = said.load(std::memory_order_relaxed);
    if ((was == 0 || now - was > 3000) && said.compare_exchange_strong(was, now))
    {
        char l[260];
        snprintf(l, sizeof l, "[MGPU][HK10] %s The capture keeps waiting for another combination.", g_refuse);
        mgpu::diag::warn(l);
    }
    g_refuse_t.store(now, std::memory_order_relaxed);
    return true;
}

// HK7: the one place a captured combination becomes a binding.
void commit_capture(int cap, unsigned char mods, int vk, const char *src)
{
    char nm[48];
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_bind[cap].mods = mods;
        g_bind[cap].vk = (unsigned char)vk;
        save((unsigned)cap);
        guard_refresh_locked();   // HK2
        bind_name(g_bind[cap], nm, sizeof nm);
        recheck_locked(false);
    }
    g_cap.store(-1);
    // The key that was just bound must not also fire.
    g_down[cap] = true;
    char l[220];
    snprintf(l, sizeof l, "[MGPU][HK1] %s is now %s (saved to mgpu.ini; seen by the %s).", DEFS[cap].label, nm, src);
    mgpu::diag::info(l);
    g_recheck.store(true);
}

// ---- HK14: IS THE KEYBOARD HOOK GETTING THE KEYS? ----
// REPORTED 2026-11-22: CTRL+ALT+H reached the game, although HK2 keeps our
// combinations from it - and that works only through the hook. The same day
// (IN72) the hook saw no key at all for 70 s while the panel was up. So: for
// the keys the mod uses, compare the key's real state (raw_async, past
// ReShade's filter) with what the hook saw. A key that went down without the
// hook seeing it since the last look is a MISS: said (first 8, with whether the
// panel was up and when the hook thread last answered), and the hook is put
// back (at most once a second). Every 2 s the hook thread is pinged, so a
// thread that stopped answering is told apart from a hook Windows dropped.
static void hook_watch()
{
    static const unsigned char keys[] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_ESCAPE, VK_HOME };
    static bool prev[256] = {};
    static unsigned said = 0, misses = 0;
    static unsigned long long last_rehook = 0, ping_t = 0, said_thread = 0, last_watch = 0;
    const unsigned long long now = GetTickCount64();
    // The key went down since the last look; the hook, called before Windows
    // updates the key state, must have seen it after that look (50 ms slack).
    // A stalled loop therefore cannot make a press look like a miss.
    const unsigned long long since = last_watch > 50 ? last_watch - 50 : 0;
    const bool first = (last_watch == 0);
    last_watch = now;
    const DWORD tid = g_kb_tid.load();
    if (tid != 0 && now - ping_t >= 2000)
    {
        ping_t = now;
        PostThreadMessageW(tid, WM_KB_PING, (WPARAM)now, 0);
    }
    const unsigned long long pong = g_kb_pong.load(std::memory_order_relaxed);
    const bool thread_late = ping_t != 0 && pong + 3000 < ping_t && now - ping_t > 1000;
    if (thread_late && (said_thread == 0 || now - said_thread > 30000))
    {
        said_thread = now;
        char l[200];
        snprintf(l, sizeof l, "[MGPU][HK14] the keyboard hook's thread has not answered for %llu ms - it is stuck, "
                 "so the hook gets no keys.", pong ? now - pong : now - ping_t);
        mgpu::diag::warn(l);
    }
    for (unsigned char vk : keys)
    {
        const bool d = (raw_async(vk) & 0x8000) != 0;
        if (!first && d && !prev[vk] && !g_swallowed[vk].load(std::memory_order_relaxed) &&
            g_hk_any_t[vk].load(std::memory_order_relaxed) < since)
        {
            ++misses;
            if (said < 8)
            {
                ++said;
                char nm[32], l[300];
                key_name(vk, nm, sizeof nm);
                snprintf(l, sizeof l, "[MGPU][HK14] the keyboard hook MISSED %s (Windows saw it go down, the hook did not; "
                         "%u so far) - MGPU panel up: %s, hook thread last answered %llu ms ago. Re-installing the hook.",
                         nm, misses, now < g_esc_until.load(std::memory_order_relaxed) + 500 ? "yes" : "no",
                         pong ? now - pong : 0ull);
                mgpu::diag::warn(l);
            }
            if (tid != 0 && now - last_rehook >= 1000)
            {
                last_rehook = now;
                PostThreadMessageW(tid, WM_KB_REHOOK, 0, 0);
            }
        }
        prev[vk] = d;
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
        if (g_cap.load() >= 0)   // HK4: say why a capture sees no keys
        {
            static ULONGLONG said = 0;
            if (said == 0 || GetTickCount64() - said > 10000)
            {
                said = GetTickCount64();
                char cls[96] = "?";
                if (HWND fg = GetForegroundWindow()) GetClassNameA(fg, cls, sizeof cls);
                char l[220];
                snprintf(l, sizeof l, "[MGPU][HK4] capturing a hotkey, but another program's window is in front "
                         "(class \"%s\") - keys are not read until the game is.", cls);
                mgpu::diag::warn(l);
            }
        }
        // Nothing is held while another program is in front.
        memset(g_down, 0, sizeof g_down);
        g_down_coarse[0] = g_down_coarse[1] = false;
        return false;
    }
    hook_watch();   // HK14: only with the game / bridge in front

    // ---- capture: the next combination pressed becomes the binding ----
    const int cap = g_cap.load();
    if (cap >= 0 && cap < (int)A_COUNT)
    {
        int hit = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (!is_modifier_or_mouse(vk) && cap_key(vk)) { hit = vk; break; }   // HK6: stale "held" keys skipped
        static ULONGLONG cap_t0 = 0; static int cap_for = -1; static bool said_wait = false;
        if (cap_for != cap) { cap_for = cap; cap_t0 = GetTickCount64(); said_wait = false; }
        if (!g_cap_armed)
        {
            if (hit == 0) g_cap_armed = true;
            else if (!said_wait && GetTickCount64() - cap_t0 > 3000)
            {   // HK4: a key that never reads as released blocks the capture
                said_wait = true;
                char nm[32], l[200];
                key_name(hit, nm, sizeof nm);
                snprintf(l, sizeof l, "[MGPU][HK4] waiting for every key to be released before capturing - %s "
                         "(0x%02X) still reads as held.", nm, hit);
                mgpu::diag::warn(l);
            }
            return false;
        }
        if (hit == 0) return false;
        if (hit == VK_ESCAPE) { g_cap.store(-1); return false; }
        // HK4/HK5/HK10: ReShade's panel key, a combination in use, or one
        // outside Ctrl/Alt + A-Z/F1-F12 is never a binding. Refused: wait for
        // every key to be released, then capture again.
        if (refuse_capture(cap, mods_now(), hit))
        {
            g_cap_armed = false;
            return false;
        }
        commit_capture(cap, mods_now(), hit, "keyboard");   // IN81: the hook or the key state
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
const char *name_now(unsigned a)
{
    static thread_local char ring[8][48];
    static thread_local unsigned i = 0;
    char *b = ring[i++ & 7u];
    name(a, b, 48);
    return b;
}

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
    if (a >= A_COUNT || fixed(a)) return;   // HK13
    // HK6: start from a clean slate. Rebind is clicked with the mouse, so no
    // key should be held; a key the table still shows as down missed its
    // release (see cap_key) and would block the capture.
    for (int vk = 0; vk < 256; ++vk) g_hk_down[vk].store(false, std::memory_order_relaxed);
    g_cap_armed = false;
    g_refuse_t.store(0, std::memory_order_relaxed);   // HK10
    if (const DWORD t = g_kb_tid.load()) PostThreadMessageW(t, WM_KB_REHOOK, 0, 0);   // HK8
    g_cap.store((int)a);
}

void cancel_capture() { g_cap.store(-1); }

bool capture_from_panel(unsigned char mods, int vk)
{
    const int cap = g_cap.load();
    if (cap < 0 || cap >= (int)A_COUNT || vk <= 0 || vk >= 256 || is_modifier_or_mouse(vk)) return false;
    if (vk == VK_ESCAPE) { g_cap.store(-1); return false; }
    if (refuse_capture(cap, (unsigned char)(mods & 7u), vk)) return false;   // HK10
    commit_capture(cap, (unsigned char)(mods & 7u), vk, "panel");
    return true;
}

int capturing() { return g_cap.load(); }
bool capture_refusal(char *out, unsigned n)
{
    const unsigned long long t = g_refuse_t.load(std::memory_order_relaxed);
    if (out == nullptr || n == 0 || t == 0 || GetTickCount64() - t > 6000 || g_cap.load() < 0) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    snprintf(out, n, "%s", g_refuse);
    return true;
}

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
