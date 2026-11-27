// MGPU Bridge - PanelOverPicture: the live panel (N01950 contribution).
// Taken into the davidm71 fork from upstream 0.4.0 (MIT); fork change PP1 in live_panel.cpp.
//
// WHAT IT IS FOR. With DcompOverlay=1 the neural picture is a composition
// visual on top of the game's window, so the game's own ReShade overlay is
// underneath it. The shipped answer (V65) steps the visual aside while the
// panel is open: the panel is usable and the picture is the native one. This
// file is the other answer: the panel is opened on the GPU 1 runtime whose
// swapchain IS the neural picture (the one with no window), so it is drawn
// over the live neural frame. That runtime gets no input from ReShade, so this
// file feeds its ImGui input, and the game's input is blocked while the panel
// is drawn.
//
// ON BY DEFAULT (PO1). PanelOverPicture=0 in mgpu.ini turns it off. The key is
// read ONCE, in DllMain (attach), because the reshade_overlay event this needs
// must be registered there with every other event. Key 0: the event is not
// registered, on() is false, every hook below returns at once, and the add-on
// behaves exactly as 0.3.1.
//
// OWNERSHIP. Contributed in the N01950 pull request; this file is that path's
// home. dllmain.cpp only calls the functions below; gpu1_context.cpp only
// provides dcomp_visual_rooted() and dcomp_game_hwnd().
//
// Log tags: [R292] the key, [R293] the input-release guard, [R294] the panel.
#pragma once

#include <reshade.hpp>

namespace mgpu::live_panel
{
    // DllMain, DLL_PROCESS_ATTACH, at the place of the other registrations.
    // Reads PanelOverPicture once; registers reshade_overlay unless it is 0
    // (and the build has ImGui). One log line only when it is on.
    void attach(void *module);

    // The key, as read at attach, AND an ImGui build. The single answer every
    // dllmain call site uses, so the panel is never half on.
    bool on();

    // R158's init_effect_runtime / destroy_effect_runtime. Records the game's
    // runtime and the windowless GPU 1 runtime; drops the host on destroy.
    void note_runtime(reshade::api::effect_runtime *runtime, bool is_game);
    void runtime_destroyed(reshade::api::effect_runtime *runtime);

    // The panel's state (1 = up on the neural layer).
    bool live();
// UI4: true once after the panel's host was rebuilt while the panel was open -
// the caller reopens it on the new host.
bool take_reopen();
    void set_live(bool up);

    // WH1: inside a window (the panel, or an open combo list), scroll it by the
    // mouse wheel turned since the last frame, if the pointer is over it.
    void wheel_here();

    // The GPU 1 runtime to open the overlay on: the windowless one, or
    // `fallback` (R158's bridge slot) before it has been seen.
    void *host_target(void *fallback);
    // True for the runtime the panel was opened on, or the windowless one.
    bool is_host(void *runtime);
    // R158's apply, the moment open_overlay is called on the GPU 1 side.
    void note_opened_on(void *runtime, bool open);
    // PW1: BT5 (picture on top, DcompOverlay off) opened ReShade's overlay on the
    // picture's runtime. The panel then gets its own window there too, as it
    // does in DcompOverlay mode, instead of only a tab behind Home. nullptr /
    // false: closed or the runtime is going away.
    void set_picture_panel(void *runtime, bool open);

    // PO1 "panel only" (PanelOnly=1, the default; needs PanelOverPicture=1):
    // Home opens only the MGPU panel - ReShade's own menu stays closed (its
    // open is vetoed) and the panel is drawn in its own window from the
    // reshade_overlay event, which ReShade raises every frame either way.
    // Shift+Home, or the panel's "ReShade menu" button, open ReShade's menu.
    bool panel_only();
    // Plain mode (no picture on top, no DcompOverlay): the runtime the panel is
    // shown on, nullptr = closed.
    void  po_set(void *runtime, bool open);
    void *po_runtime();
// HX1 (IN92b): true while the add-on itself is opening/closing an overlay
// (its Esc close), so that toggle is not taken for a mirrored key press.
bool self_toggle();
    // Any panel-only window is up (any mode) - set by dllmain; Esc then calls
    // the closer it registered (closes whichever mode's panel is open).
    void po_mark(bool up);
    bool po_up();
    void set_po_closer(void (*fn)());
    // PO2: Shift+overlay key, seen by the keyboard hook on the game's runtime
    // (ReShade's ForceShortcutModifiers=1, its default, never reports it).
    void set_po_shift(void (*fn)(reshade::api::effect_runtime *));
    // PO4: every frame on the game's runtime (panel-only mode): Esc closes
    // ReShade's menu, and the switch back to the MGPU panel runs here.
    void set_po_tick(void (*fn)(reshade::api::effect_runtime *));
    // The "ReShade menu" button: the next open on that runtime is not vetoed.
    void po_allow_reshade_once();
    bool po_take_allow();

    // The game chain's present, on its own thread. Blocks the game's input for
    // the next frame while the panel is live AND being drawn (R293).
    void block_game_input(reshade::api::effect_runtime *game_runtime);

    // UX1: the panel in its OWN window on the GPU 1 runtime - wide by default,
    // never narrower than its content needs, and brought to the front when the
    // overlay opens, instead of a tab behind ReShade's Home tab. `fn` draws the
    // panel's content (dllmain's overlay callback).
    void set_drawer(void (*fn)(reshade::api::effect_runtime *));
    // True while that window is drawing (the content is being drawn inside it).
    bool drawing_own();
    // True while the own window has been shown in the last half second - the
    // docked ReShade tab then shows a one-line pointer instead of a copy.
    bool own_window_shown();
}
