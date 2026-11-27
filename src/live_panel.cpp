// MGPU Bridge - PanelOverPicture: the live panel. See live_panel.hpp.
//
// davidm71 fork (PP1): taken from upstream 0.4.0 (MIT, Marcelo Guibout and
// NO1950). Fork change: with the scaler's 4K cover up (game window smaller
// than the monitor), the panel's pointer comes from the real pointer's place
// on the MONITOR, so it sits exactly under the Windows pointer, and clicks are
// read with GetAsyncKeyState (the cover takes the clicks while the panel is
// up, so they never reach the desktop). See sc_cover_live_rect in gpu1.
//
// Contributed in the N01950 pull request (Far Cry 6 and Kingdom Come:
// Deliverance II on the contributor's rig), moved into its own file with its
// key read at attach, its event registered only with the key, R-numbered
// lines, and the R293 input-release guard.

#include <windows.h>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Same ImGui preamble as dllmain.cpp (P7.1): ImTextureID must be ImU64 and the
// version must match reshade_overlay.hpp. Without ImGui, everything below
// compiles to "off".
#if defined(__has_include)
#  if __has_include(<imgui.h>)
#    define MGPU_HAVE_IMGUI 1
#    define ImTextureID ImU64
#    include <imgui.h>
#  endif
#endif

#include <reshade.hpp>
#include "panel_theme.hpp"   // TH1

#include "live_panel.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"
#include "hotkeys.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::live_panel
{
namespace
{
// ---- R292: the key, read once at attach --------------------------------------
std::atomic<int> g_key{-1};   // -1 not read, 0 off, 1 on

// PO1: ON unless mgpu.ini says PanelOverPicture=0. It was OFF unless the file
// said 1, and the diagnostic mgpu.ini never did - so Home opened the panel in
// the game's own (smaller) window with the picture behind it (2026-11-15).
int read_key(void *module)
{
    wchar_t mod[MAX_PATH] = {};
    const DWORD len = (module != nullptr) ? GetModuleFileNameW((HMODULE)module, mod, MAX_PATH) : 0;
    if (len == 0 || len >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return 1;
    slash[1] = L'\0';
    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", mod);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 1;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 1;
    buf[got] = '\0';
    const char *k = mgpu::config::find(buf, got, "PanelOverPicture");
    return (k != nullptr && *k == '0') ? 0 : 1;
}

// ---- the panel's state ------------------------------------------------------
std::atomic<int>    g_live{0};            // 1 = the panel is up on the neural layer
// UX1: the panel's own window.
void (*g_drawer)(reshade::api::effect_runtime *) = nullptr;
std::atomic<int>    g_focus_req{0};       // frames left to bring the own window to the front
std::atomic<unsigned long long> g_own_ms{0};   // last time the own window was drawn
thread_local bool   t_own = false;
std::atomic<void *> g_game{nullptr};      // the game's effect runtime
std::atomic<void *> g_windowless{nullptr};// the GPU 1 runtime with no window = the neural picture
std::atomic<void *> g_host{nullptr};      // the runtime the panel was actually opened on
std::atomic<void *> g_pic_rt{nullptr};    // PW1: the picture runtime BT5 opened the overlay on
std::atomic<void *> g_po_rt{nullptr};     // PO1: plain mode - the runtime the panel is shown on
std::atomic<bool>   g_self_toggle{false}; // HX1 (IN92b): the add-on itself is toggling an overlay (Esc close)
std::atomic<int>    g_po_on{-1};          // PO1: PanelOnly, -1 not read
std::atomic<bool>   g_po_up{false};       // PO1: a panel-only window is up (any mode)
std::atomic<bool>   g_po_allow{false};    // PO1: the "ReShade menu" button's one open
void (*g_po_closer)() = nullptr;          // PO1: dllmain's close-everything
void (*g_po_shift)(reshade::api::effect_runtime *) = nullptr;   // PO2: Shift+Home
void (*g_po_tick)(reshade::api::effect_runtime *) = nullptr;    // PO4
std::atomic<int>    g_said_host{0};
std::atomic<int>    g_probe{0};
// WH1: the wheel for this frame, in notches. Written by feed_input and used by
// wheel_here, both on the host runtime's overlay pass. It cannot go into
// io.MouseWheel: that is read in NewFrame and cleared in EndFrame, and the
// feed runs between the two, so every notch written there was thrown away.
float               g_wheel_pend = 0.0f;
std::atomic<int>    g_wh_hook{0}, g_wh_cover{0};   // notches seen, for the R294 line

// R293: the last time the host's reshade_overlay event fired (GetTickCount64).
std::atomic<unsigned long long> g_host_drawn_ms{0};
std::atomic<int> g_guard_said{0};
constexpr unsigned long long GUARD_BLOCK_MS   = 250;    // drawn this recently: block the game's input
constexpr unsigned long long GUARD_RELEASE_MS = 2000;   // not drawn this long: the panel is gone

// The pointer as ReShade tracks it on the GAME runtime (it survives raw input
// with RIDEV_NOLEGACY; GetCursorPos does not). One atomic each, so the game
// thread (writer) and the bridge thread (reader) cannot tear a pair apart.
std::atomic<unsigned long long> g_pos{0};    // (float x << 32) | float y
std::atomic<unsigned long long> g_disp{0};   // the game runtime's DisplaySize, same packing
std::atomic<unsigned> g_btn{0};              // bit0 L, bit1 R, bit2 M, bit3 = position valid

inline unsigned long long pack2(float a, float b)
{
    unsigned ua = 0, ub = 0;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    return ((unsigned long long)ua << 32) | (unsigned long long)ub;
}
inline void unpack2(unsigned long long v, float &a, float &b)
{
    const unsigned ua = (unsigned)(v >> 32), ub = (unsigned)(v & 0xFFFFFFFFu);
    memcpy(&a, &ua, 4);
    memcpy(&b, &ub, 4);
}

#if defined(MGPU_HAVE_IMGUI)
// ---- the input feed ------------------------------------------------------------
// The GPU 1 runtime's ImGuiIO is written by nobody: ReShade feeds input only
// into a runtime that has a window, and a composition swapchain has none. So
// its io fields are free, and writing them is the panel's input path. The
// ImGui event API is not in the add-on function table; the plain fields are
// declared in imgui.h and need no import. ImGui derives clicks from them at
// the next NewFrame (one frame of latency).
//
// The pointer is normalised over the GAME runtime's display and scaled into
// this runtime's display, so the two may differ in size. GetCursorPos is the
// fallback only (frozen under raw input).
void feed_input(reshade::api::effect_runtime *runtime)
{
    // Every exit carries a reason; a change of reason is logged at once.
    static const char *last_why = "(nothing yet)";
    const char *why = "ok";

    do
    {
        if (runtime == nullptr) { why = "no runtime"; break; }
        if (!mgpu::gpu1::dcomp_overlay_mode()) { why = "DcompOverlay off"; break; }
        if (g_live.load(std::memory_order_relaxed) == 0) { why = "panel not live"; break; }

        void *host = g_host.load(std::memory_order_relaxed);
        if (host == nullptr) host = g_windowless.load(std::memory_order_relaxed);
        // R298: the game runtime calls in every frame while the panel is open.
        // Its reason never changes, but it shared last_why with the host, so
        // the two alternated and every call logged a CHANGED line (86% of an
        // FC6 log). It returns here without logging; nothing else differs.
        if ((void *)runtime != host) return;

        // R293: the host is drawing the panel now.
        g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);

        ImGuiIO &io = ImGui::GetIO();

        const unsigned btn = g_btn.load(std::memory_order_relaxed);
        float gx = 0.0f, gy = 0.0f, gdw = 0.0f, gdh = 0.0f;
        unpack2(g_pos.load(std::memory_order_relaxed), gx, gy);
        unpack2(g_disp.load(std::memory_order_relaxed), gdw, gdh);
        const bool from_game = (btn & 8u) != 0u && gdw > 0.0f && gdh > 0.0f;

        float u = 0.0f, v = 0.0f;
        bool ldown = false, rdown = false;
        RECT cov{};
        const bool scaled = mgpu::gpu1::sc_cover_live_rect(&cov) && cov.right > cov.left && cov.bottom > cov.top;
        int wheel = 0;
        bool mdown = false;
        bool hooked = false;   // PP3
        if (scaled)
        {
            // PP2: from the picture window's own mouse messages. GetCursorPos
            // and GetAsyncKeyState cannot be used here: ReShade answers them
            // with frozen values for the whole process while the game's input
            // is blocked (measured on the rig: the pointer never moved).
            unsigned b = 0;
            if (!mgpu::gpu1::sc_cover_mouse_get(&u, &v, &b, &wheel))
            {
                // No message yet: put the panel's pointer where Windows says.
                POINT p{};
                CURSORINFO ci{}; ci.cbSize = sizeof ci;
                if (GetCursorInfo(&ci)) p = ci.ptScreenPos;
                u = (float)(p.x - cov.left) / (float)(cov.right - cov.left);
                v = (float)(p.y - cov.top) / (float)(cov.bottom - cov.top);
            }
            ldown = (b & 1u) != 0u;
            rdown = (b & 2u) != 0u;
            mdown = (b & 4u) != 0u;
            // WH1: the picture window gets WM_MOUSEWHEEL only when Windows sends
            // it there (focus, "scroll inactive windows"); the mouse hook sees
            // every notch. Use the hook when it is in, never both.
            int hw = 0;
            const bool hk = mgpu::hotkeys::panel_mouse_get(nullptr, nullptr, nullptr, &hw);
            if (wheel != 0) g_wh_cover.fetch_add(wheel / WHEEL_DELTA, std::memory_order_relaxed);
            if (hw != 0) g_wh_hook.fetch_add(hw / WHEEL_DELTA, std::memory_order_relaxed);
            if (hk) wheel = hw;
        }
        else if (HWND gh = (HWND)mgpu::gpu1::dcomp_game_hwnd();
                 gh != nullptr && mgpu::hotkeys::panel_mouse_get(nullptr, nullptr, nullptr, nullptr))
        {
            // PP3: no cover window (scaler off). The picture is the game
            // window's client area; the mouse comes from the low-level hook,
            // which ReShade's input blocking does not reach.
            RECT rc{};
            POINT tl{0, 0};
            int mx = 0, my = 0;
            unsigned b = 0;
            if (GetClientRect(gh, &rc) && rc.right > 0 && rc.bottom > 0 && ClientToScreen(gh, &tl) &&
                mgpu::hotkeys::panel_mouse_get(&mx, &my, &b, &wheel))
            {
                u = (float)(mx - tl.x) / (float)rc.right;
                v = (float)(my - tl.y) / (float)rc.bottom;
                ldown = (b & 1u) != 0u;
                rdown = (b & 2u) != 0u;
                mdown = (b & 4u) != 0u;
                hooked = true;
            }
        }
        else if (from_game)
        {
            u = gx / gdw;
            v = gy / gdh;
            ldown = (btn & 1u) != 0u;
            rdown = (btn & 2u) != 0u;
        }
        else
        {
            HWND hwnd = (HWND)mgpu::gpu1::dcomp_game_hwnd();
            if (hwnd == nullptr) { why = "no game window recorded (desktop fallback)"; break; }
            RECT rc{};
            if (GetClientRect(hwnd, &rc) == FALSE) { why = "GetClientRect failed"; break; }
            const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            if (cw <= 0 || ch <= 0) { why = "game client is empty"; break; }
            POINT tl{rc.left, rc.top};
            POINT p{};
            if (ClientToScreen(hwnd, &tl) == FALSE) { why = "ClientToScreen failed"; break; }
            if (GetCursorPos(&p) == FALSE) { why = "GetCursorPos failed"; break; }
            u = (float)(p.x - tl.x) / (float)cw;
            v = (float)(p.y - tl.y) / (float)ch;
            ldown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            rdown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        }
        if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;

        const float want_x = u * io.DisplaySize.x;
        const float want_y = v * io.DisplaySize.y;

        // Feed probe every 120 fed frames: what ImGui had before this write,
        // and whether it thinks the pointer is over its UI.
        if ((g_probe.fetch_add(1, std::memory_order_relaxed) % 120) == 0)
        {
            char pr[560];
            snprintf(pr, sizeof pr,
                     "[MGPU][R294] feed probe: src=%s write pos=(%.0f,%.0f) L=%d R=%d | read back "
                     "before this write: pos=(%.0f,%.0f) valid=%d L=%d | cursor=%d wantCapture=%d | "
                     "game io pos=(%.0f,%.0f) disp=(%.0f,%.0f) | norm=(%.3f,%.3f)",
                     scaled ? "monitor pointer (scaled picture)" : hooked ? "mouse hook (game window)" : from_game ? "game runtime io" : "desktop GetCursorPos (fallback)",
                     (double)want_x, (double)want_y, (int)ldown, (int)rdown,
                     (double)io.MousePos.x, (double)io.MousePos.y,
                     (int)(io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX),
                     (int)io.MouseDown[0], (int)io.MouseDrawCursor, (int)io.WantCaptureMouse,
                     (double)gx, (double)gy, (double)gdw, (double)gdh, (double)u, (double)v);
            mgpu::diag::info(pr);
        }

        io.MousePos = ImVec2(want_x, want_y);
        io.MouseDrawCursor = true;   // ReShade sets this for runtimes it feeds; this one is ours
        if (scaled)
        {
            // PP1: the Windows pointer is already at this spot; draw ImGui's
            // only when Windows is not showing one (the game hid it).
            CURSORINFO ci{}; ci.cbSize = sizeof ci;
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor != nullptr)
                io.MouseDrawCursor = false;
        }
        io.MouseDown[0] = ldown;
        io.MouseDown[1] = rdown;
        io.MouseDown[2] = (scaled || hooked) ? mdown : ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        g_wheel_pend = (scaled || hooked) ? (float)wheel / (float)WHEEL_DELTA : 0.0f;   // PP2/PP3, WH1
        // PP3: modifier and ESC state from the keyboard hook - ReShade freezes
        // GetAsyncKeyState while it blocks the game's input.
        io.KeyCtrl  = mgpu::hotkeys::key_down(VK_CONTROL);
        io.KeyShift = mgpu::hotkeys::key_down(VK_SHIFT);
        io.KeyAlt   = mgpu::hotkeys::key_down(VK_MENU);
        io.KeySuper = false;

        // ESC closes the panel (edge-triggered). The live flag is NOT cleared
        // here: it follows the close event (dllmain's open-overlay handler),
        // so a close that does not take leaves a working panel, not a stranded
        // one. HOME is the reliable close.
        static int esc_prev = 0;
        const int esc = mgpu::hotkeys::key_down(VK_ESCAPE) ? 1 : 0;   // PP3
        if (esc != 0 && esc_prev == 0)
        {
            g_self_toggle.store(true, std::memory_order_relaxed);   // HX1: ours, not a mirrored key
            runtime->open_overlay(false, reshade::api::input_source::keyboard);
            g_self_toggle.store(false, std::memory_order_relaxed);
        }
        esc_prev = esc;

        why = scaled ? "fed (pointer on the scaled picture)" : hooked ? "fed (mouse hook, game window)" : from_game ? "fed (game runtime io)" : "fed (desktop fallback)";
    } while (0);

    static int beat = 0;
    if (why != last_why || (++beat % 600) == 0)
    {
        char pr[640];
        snprintf(pr, sizeof pr,
                 "[MGPU][R294] feed: %s%s | live=%d host=%p runtime=%p | io pos=(%.0f,%.0f) "
                 "cursor=%d wantCapture=%d down0=%d | wheel notches: hook %d, picture window %d",
                 why, (why != last_why) ? "  <- CHANGED" : "",
                 g_live.load(std::memory_order_relaxed),
                 g_host.load(std::memory_order_relaxed), (void *)runtime,
                 (double)ImGui::GetIO().MousePos.x, (double)ImGui::GetIO().MousePos.y,
                 (int)ImGui::GetIO().MouseDrawCursor, (int)ImGui::GetIO().WantCaptureMouse,
                 (int)ImGui::GetIO().MouseDown[0],
                 g_wh_hook.load(std::memory_order_relaxed), g_wh_cover.load(std::memory_order_relaxed));
        mgpu::diag::info(pr);
        last_why = why;
    }
}

// ---- the frame hook ------------------------------------------------------------
// reshade_overlay fires once per frame for every runtime whose overlay is open,
// after ReShade's overlay window loop - so it runs whatever tab is selected,
// with the ImGui context current. A registered overlay's own callback runs only
// while its tab is selected. Registered only with PanelOverPicture=1 (attach).
void on_reshade_overlay(reshade::api::effect_runtime *runtime)
{
    {   // TH2: with PanelTheme=2 every runtime's ReShade overlay gets the mod's colours
        static const int th = mgpu::gpu1::ui_ini_read("PanelTheme", 2);
        if (th == 2) mgpu::theme::apply_global();
    }
    feed_input(runtime);

    // UX1: the panel in its own window on the host runtime, while it is up.
    // ReShade docks a registered overlay as a tab next to Home, and on the
    // GPU 1 runtime that tab is neither selected nor sized when the overlay
    // opens - it opened on Home, narrow, every time. This window is opened
    // focused, wide on first use and never narrower than the panel needs.
    // PW1: also on the picture's runtime while BT5 has the overlay open there.
    const bool pic = runtime != nullptr && (void *)runtime == g_pic_rt.load(std::memory_order_relaxed);
    // PO1: plain mode - the panel shown on this runtime with ReShade's menu closed.
    const bool po = runtime != nullptr && (void *)runtime == g_po_rt.load(std::memory_order_relaxed);
    const bool live_host = g_live.load(std::memory_order_relaxed) != 0 && is_host((void *)runtime);
    if (g_drawer != nullptr && (live_host || pic || po))
    {
        if (g_po_up.load(std::memory_order_relaxed))
        {
            mgpu::hotkeys::panel_escape_alive();   // PO3: Esc is ours while this is drawn
            // PO1: ReShade's menu is closed, so ReShade neither draws its
            // pointer nor blocks the game: do both for as long as the panel is
            // drawn (the live host's feed and BT5's game-chain block already
            // do this for their modes; doing it again is harmless).
            if (!live_host)
            {
                ImGui::GetIO().MouseDrawCursor = true;
                if (po) runtime->block_input_next_frame();
                // ESC closes it (edge, from the keyboard hook - the picture
                // window never has the keyboard focus). The live host's feed
                // has its own ESC.
                static int esc_prev = 0;
                const int esc = mgpu::hotkeys::key_down(VK_ESCAPE) ? 1 : 0;
                // Not while a hotkey is being captured: there Esc cancels the capture.
                if (esc != 0 && esc_prev == 0 && g_po_closer != nullptr && mgpu::hotkeys::capturing() < 0) g_po_closer();
                esc_prev = esc;
            }
        }
        const float fs = ImGui::GetFontSize();
        const ImGuiIO &io = ImGui::GetIO();
        const float dw = (io.DisplaySize.x > 0.0f) ? io.DisplaySize.x : 1920.0f;
        const float dh = (io.DisplaySize.y > 0.0f) ? io.DisplaySize.y : 1080.0f;
        const float want_w = (fs * 58.0f < dw * 0.92f) ? fs * 58.0f : dw * 0.92f;
        const float min_w  = (fs * 44.0f < dw * 0.92f) ? fs * 44.0f : dw * 0.92f;
        // UX2: on the RIGHT of the screen. ReShade's own window opens on the
        // left, so the two no longer stack on top of each other.
        ImGui::SetNextWindowPos(ImVec2(dw - fs * 1.5f, fs * 3.0f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(want_w, dh * 0.82f), ImGuiCond_FirstUseEver);
        // PO1: shown on its own, the panel opens full height on the right, at
        // the width it was last left at.
        static float po_w = 0.0f;
        const bool po_full = g_po_up.load(std::memory_order_relaxed);
        if (po_full)
        {
            ImGui::SetNextWindowPos(ImVec2(dw - fs * 0.6f, fs * 0.6f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(po_w > min_w ? po_w : want_w, dh - fs * 1.2f), ImGuiCond_Appearing);
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_w, fs * 16.0f), ImVec2(FLT_MAX, FLT_MAX), nullptr, nullptr);
        if (g_focus_req.load(std::memory_order_relaxed) > 0)
        {
            ImGui::SetNextWindowFocus();
            g_focus_req.fetch_sub(1, std::memory_order_relaxed);
        }
        t_own = true;
        g_own_ms.store(GetTickCount64(), std::memory_order_relaxed);
        // TH1: the window frame in the panel's colours too (PanelTheme=0 keeps ReShade's).
        static const int theme = mgpu::gpu1::ui_ini_read("PanelTheme", 2);
        const int tn = (theme != 0) ? mgpu::theme::push_window() : 0;
        if (ImGui::Begin("Atomic MGPU Bridge###mgpu_panel_own", nullptr, ImGuiWindowFlags_NoDocking))
        {
            // WH1 (an open combo list takes it instead: it is the hovered window).
            // TH1: themed, the panel scrolls inside its child window, which calls it.
            if (tn == 0) wheel_here();
            if (po_full) po_w = ImGui::GetWindowSize().x;   // PO1
            g_drawer(runtime);
        }
        ImGui::End();
        if (tn != 0) ImGui::PopStyleColor(tn);
        g_wheel_pend = 0.0f;
        t_own = false;
    }

    // Harvest the GAME runtime's pointer. Registering this event keeps ReShade
    // feeding that runtime's ImGuiIO from its own raw-input tracking every
    // present, even though its overlay is refused.
    if ((void *)runtime == g_game.load(std::memory_order_relaxed))
    {
        // PO2: Shift+Home (Shift + ReShade's overlay key) opens ReShade's menu.
        // Edge of the key while Shift is held, from the keyboard hook, only
        // with the game (or the picture) in front. Not when ReShade's own key
        // is already a Shift combination - ReShade handles that one itself.
        if (g_po_shift != nullptr && panel_only() && !mgpu::hotkeys::overlay_key_has_shift())
        {
            static int prev = 0;
            const int vk = mgpu::hotkeys::overlay_vk();
            const int down = (vk > 0 && mgpu::hotkeys::key_down(vk)) ? 1 : 0;
            if (down != 0 && prev == 0 && mgpu::hotkeys::key_down(VK_SHIFT) &&
                !mgpu::hotkeys::key_down(VK_CONTROL) && !mgpu::hotkeys::key_down(VK_MENU) &&
                mgpu::hotkeys::in_front())
                g_po_shift(runtime);
            prev = down;
        }
        if (g_po_tick != nullptr && panel_only()) g_po_tick(runtime);   // PO4
        ImGuiIO &gio = ImGui::GetIO();
        const float x = gio.MousePos.x, y = gio.MousePos.y;
        const float dw = gio.DisplaySize.x, dh = gio.DisplaySize.y;
        const bool ok = (x != -FLT_MAX && y != -FLT_MAX && dw > 0.0f && dh > 0.0f);
        if (ok)
        {
            g_pos.store(pack2(x, y), std::memory_order_relaxed);
            g_disp.store(pack2(dw, dh), std::memory_order_relaxed);
        }
        unsigned b = ok ? 8u : 0u;
        if (gio.MouseDown[0]) b |= 1u;
        if (gio.MouseDown[1]) b |= 2u;
        if (gio.MouseDown[2]) b |= 4u;
        g_btn.store(b, std::memory_order_relaxed);
    }
}
#endif   // MGPU_HAVE_IMGUI
}   // namespace

// THE COST, which is ReShade's: registering reshade_overlay makes
// has_addon_event<reshade_overlay>() true for the whole process, which turns off
// draw_gui's early-out, so every runtime runs an empty ImGui frame per present,
// panel closed or not (the contributor measured tens of microseconds; not yet
// measured here - PO-1). It is also what keeps block_input_next_frame()
// applying. That is why it is registered only with PanelOverPicture=1.
void attach(void *module)
{
    int v = -1;
    if (!g_key.compare_exchange_strong(v, read_key(module), std::memory_order_acq_rel)) return;
    if (g_key.load(std::memory_order_acquire) != 1) return;   // off: nothing registered, no line
#if defined(MGPU_HAVE_IMGUI)
    reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    mgpu::diag::info("[MGPU][R292] PanelOverPicture=1: with DcompOverlay=1 an open panel is drawn by the "
                     "GPU 1 runtime over the live neural frame, and the game's input is blocked while it is "
                     "drawn (R293). Absent or 0: the shipped behaviour, and none of this code runs.");
#else
    mgpu::diag::warn("[MGPU][R292] PanelOverPicture=1, but this build has no ImGui: the key is ignored and "
                     "the shipped behaviour stays.");
#endif
}

void wheel_here()
{
#if defined(MGPU_HAVE_IMGUI)
    if (g_wheel_pend == 0.0f || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;
    // ImGui's own step: five lines a notch, at most two thirds of the window.
    float step = ImGui::GetFontSize() * 5.0f;
    const float cap = ImGui::GetWindowHeight() * 0.67f;
    if (cap > 0.0f && step > cap) step = cap;
    float y = ImGui::GetScrollY() - g_wheel_pend * step;
    const float ymax = ImGui::GetScrollMaxY();
    if (y > ymax) y = ymax;
    if (y < 0.0f) y = 0.0f;
    ImGui::SetScrollY(y);
    g_wheel_pend = 0.0f;
#endif
}

bool on()
{
#if defined(MGPU_HAVE_IMGUI)
    return g_key.load(std::memory_order_acquire) == 1;
#else
    return false;
#endif
}

// UI4: switching HDR output (or anything else that rebuilds the neural chain)
// destroys the panel's host runtime while the panel is open. It was dropped
// and the user had to press the overlay key twice to get it back - meanwhile
// the game runtime's half of the panel stayed up at the game window's place,
// without the pointer mapping. Now, if the host goes while the panel is live,
// the panel is reopened on the new host as soon as it is recorded (within
// 10 s), through the same pending-open path a keypress takes.
std::atomic<unsigned long long> g_reopen_ms{0};
std::atomic<int> g_reopen_go{0};

void note_runtime(reshade::api::effect_runtime *runtime, bool is_game)
{
    if (!on() || runtime == nullptr) return;
    if (is_game) { g_game.store(runtime, std::memory_order_relaxed); return; }
    if (runtime->get_hwnd() != nullptr) return;   // only the windowless chain is the neural picture
    g_windowless.store(runtime, std::memory_order_relaxed);
    {   // UI4
        const unsigned long long t = g_reopen_ms.exchange(0, std::memory_order_relaxed);
        if (t != 0ull && GetTickCount64() - t < 10000ull)
        {
            g_reopen_go.store(1, std::memory_order_relaxed);
            mgpu::diag::info("[MGPU][UI4] the neural chain was rebuilt with the panel open - reopening it on the new host.");
        }
    }
    if (g_said_host.exchange(1, std::memory_order_relaxed) == 0)
    {
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R294] live panel host = runtime %p: it has no window, so it is the composition "
                 "swapchain - the neural picture. The panel is opened and fed on this runtime.",
                 (void *)runtime);
        mgpu::diag::info(l);
    }
}

void runtime_destroyed(reshade::api::effect_runtime *runtime)
{
    if (!on() || runtime == nullptr) return;
    void *r = runtime;
    if (g_windowless.load(std::memory_order_relaxed) == r)
        g_windowless.store(nullptr, std::memory_order_relaxed);   // re-recorded by note_runtime
    if (g_po_rt.load(std::memory_order_relaxed) == r)   // PO1: never drawn on a runtime that is going away
    {
        g_po_rt.store(nullptr, std::memory_order_relaxed);
        g_po_up.store(false, std::memory_order_relaxed);
    }
    if (g_host.load(std::memory_order_relaxed) == r)
    {
        g_host.store(nullptr, std::memory_order_relaxed);
        if (g_live.exchange(0, std::memory_order_relaxed) != 0 && (mgpu::gpu1::sc_set_panel_live(false), true) &&
            (g_reopen_ms.store(GetTickCount64(), std::memory_order_relaxed), true))   // UI4
            mgpu::diag::info("[MGPU][R294] the panel's host runtime is being destroyed (the neural chain was "
                             "re-armed or resized): the panel is dropped and the game's input is released. "
                             "The overlay key opens it again on the new runtime.");
    }
}

bool live() { return g_live.load(std::memory_order_relaxed) != 0; }
bool take_reopen() { return g_reopen_go.exchange(0, std::memory_order_relaxed) != 0; }   // UI4

void set_drawer(void (*fn)(reshade::api::effect_runtime *)) { g_drawer = fn; }
bool drawing_own() { return t_own; }
bool own_window_shown()
{
    const unsigned long long t = g_own_ms.load(std::memory_order_relaxed);
    return t != 0ull && GetTickCount64() - t < 500ull;
}

void set_live(bool up)
{
    if (up && g_live.load(std::memory_order_relaxed) == 0) g_focus_req.store(3, std::memory_order_relaxed);   // UX1
    g_live.store(up ? 1 : 0, std::memory_order_relaxed);
    mgpu::gpu1::sc_set_panel_live(up);   // PP1: the cover takes the clicks while the panel is up
    if (up) g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);   // R293: grace until the first draw
}

void *host_target(void *fallback)
{
    void *w = g_windowless.load(std::memory_order_relaxed);
    return (w != nullptr) ? w : fallback;
}

bool is_host(void *runtime)
{
    if (runtime == nullptr) return false;
    return runtime == g_host.load(std::memory_order_relaxed) ||
           runtime == g_windowless.load(std::memory_order_relaxed);
}

void set_picture_panel(void *runtime, bool open)
{
    if (!on()) return;
    if (open && runtime != nullptr)
    {
        if (g_pic_rt.exchange(runtime, std::memory_order_relaxed) != runtime)
            g_focus_req.store(3, std::memory_order_relaxed);   // opened focused, like UX1
    }
    else if (runtime == nullptr || g_pic_rt.load(std::memory_order_relaxed) == runtime)
        g_pic_rt.store(nullptr, std::memory_order_relaxed);
}

bool panel_only()
{
    int v = g_po_on.load(std::memory_order_relaxed);
    if (v < 0) { v = mgpu::gpu1::ui_ini_read("PanelOnly", 1) != 0 ? 1 : 0; g_po_on.store(v); }
    return v == 1 && on();
}
void po_set(void *runtime, bool open)
{
    if (open && runtime != nullptr)
    {
        if (g_po_rt.exchange(runtime, std::memory_order_relaxed) != runtime)
            g_focus_req.store(3, std::memory_order_relaxed);
    }
    else if (runtime == nullptr || g_po_rt.load(std::memory_order_relaxed) == runtime)
        g_po_rt.store(nullptr, std::memory_order_relaxed);
}
void *po_runtime() { return g_po_rt.load(std::memory_order_relaxed); }
bool self_toggle() { return g_self_toggle.load(std::memory_order_relaxed); }
void po_mark(bool up)
{
    g_po_up.store(up, std::memory_order_relaxed);
    if (up) mgpu::hotkeys::rehook_keyboard();   // KS2: arrows, Esc and Shift+Home come from it
}
bool po_up() { return g_po_up.load(std::memory_order_relaxed); }
void set_po_closer(void (*fn)()) { g_po_closer = fn; }
void set_po_shift(void (*fn)(reshade::api::effect_runtime *)) { g_po_shift = fn; }
void set_po_tick(void (*fn)(reshade::api::effect_runtime *)) { g_po_tick = fn; }
void po_allow_reshade_once() { g_po_allow.store(true, std::memory_order_relaxed); }
bool po_take_allow() { return g_po_allow.exchange(false, std::memory_order_relaxed); }

void note_opened_on(void *runtime, bool open)
{
    if (!on() || runtime == nullptr) return;
    if (open)
    {
        g_host.store(runtime, std::memory_order_relaxed);
        g_host_drawn_ms.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

// R293. THE INPUT-RELEASE GUARD. The game's input is blocked only while the
// host runtime is actually drawing the panel. An overlay the player cannot see
// made games look frozen while they ran fine; blocking on a flag alone could
// bring that back (host not presenting, window covered, a close event that was
// missed). Drawn within GUARD_BLOCK_MS: block. Not drawn for GUARD_RELEASE_MS:
// the panel is treated as gone - the flag is cleared and the input released.
void block_game_input(reshade::api::effect_runtime *game_runtime)
{
    if (!on() || game_runtime == nullptr) return;
    if (g_live.load(std::memory_order_relaxed) == 0) return;

    const unsigned long long now  = GetTickCount64();
    const unsigned long long last = g_host_drawn_ms.load(std::memory_order_relaxed);
    const unsigned long long age  = (now >= last) ? now - last : 0ull;

    if (age <= GUARD_BLOCK_MS)
    {
        game_runtime->block_input_next_frame();
        return;
    }
    if (age >= GUARD_RELEASE_MS && g_live.exchange(0, std::memory_order_relaxed) != 0)
    {
        mgpu::gpu1::sc_set_panel_live(false);   // PP1
        char l[300];
        snprintf(l, sizeof l,
                 "[MGPU][R293] the panel has not been drawn for %llu ms: the game's input is released "
                 "and the panel state is cleared. The overlay key opens it again.%s",
                 age, (g_guard_said.exchange(1, std::memory_order_relaxed) == 0) ? " (first time this run)" : "");
        mgpu::diag::warn(l);
    }
}
}   // namespace mgpu::live_panel
