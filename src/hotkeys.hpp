// ---------------------------------------------------------------------------
// hotkeys.hpp - HK1. The add-on's hotkeys, read by polling instead of
// RegisterHotKey.
//
// Until HK1 every key was a Windows-wide RegisterHotKey. That took the
// combination away from every other program while the game ran, and when
// another program had registered it first, ours silently failed (measured
// 2026-10-15: CTRL+ALT+F8 and CTRL+ALT+F11 = FAILED). Now the bridge thread
// reads the keyboard state itself, and ONLY while a window of this process
// (the game or the bridge window) is in the foreground. Nothing is registered
// with Windows, so nothing can be refused and nothing is taken from anyone.
//
// Every key can be rebound in the panel with a capture field: click it,
// press the combination you want (ESC cancels). The panel writes the result to
// mgpu.ini as Hotkey<Name>=<code>; the file is not meant to be edited by hand.
// A binding that duplicates another one, or that another program has also
// registered (it would then react to the same keys), is flagged in the panel
// and in ReShade.log.
//
// No ReShade or D3D types here.
// ---------------------------------------------------------------------------
#pragma once

namespace mgpu
{
namespace hotkeys
{

enum action : unsigned
{
    A_TRANSIT = 0,   // arm / transit probe (was CTRL+ALT+F10)
    A_PEEK,          // hide/show the bridge visual (overlay mode)
    A_VIEW,          // output / input / split
    A_INT_TARGET,    // intensity: which pass
    A_INT_DOWN,
    A_INT_UP,
    A_SEAM_L,        // split seam left  (+SHIFT = coarse)
    A_SEAM_R,        // split seam right (+SHIFT = coarse)
    A_FPS_HUD,
    A_FRAMEGEN,
    A_HUDPROBE,
    A_SCREENSHOT,    // SS1
    A_RECORD,        // VR1
    A_BENCH,         // BM1
    A_PATH,          // BM2: record the input path start/stop
    A_CLOCK,         // CK1: show/hide the clock
    A_STOPWATCH,     // CK1: show/hide the stopwatch (starts from 0 when shown)
    A_SWEEP,         // BM3: benchmark sweep start/stop
    A_COUNT
};

// What one poll found: presses (and auto-repeats for the repeatable keys).
struct presses
{
    unsigned n[A_COUNT];
    int seam_coarse;   // +1 per coarse right, -1 per coarse left
};

// Bridge thread. Reads the saved bindings, checks them, logs the table.
void init();
// HK2: DllMain live unload - remove the keyboard hook, join nothing.
void emergency_unhook();

// PP3: the panel's mouse when there is no scaler cover window. A low-level
// mouse hook runs only while the panel is up (on = panel up). get: screen
// coordinates, buttons (1 L, 2 R, 4 M), wheel since the last call; false
// until the hook has seen an event.
void panel_mouse(bool on);
bool panel_mouse_get(int *x, int *y, unsigned *btn, int *wheel);
// PP3: a key's state as the keyboard hook saw it (ReShade freezes GetAsyncKeyState).
bool key_down(int vk);
// PO1: ReShade's overlay key itself already needs Shift (ReShade.ini KeyOverlay).
bool overlay_key_has_shift();
// PO2: ReShade's overlay key (virtual-key code), and whether the game or the
// bridge window is in front.
int overlay_vk();
// KS2: put the keyboard hook back (Windows drops a low-level hook silently when
// it answers slowly) - called whenever the MGPU panel opens. And its state:
// installed, and ms since it last saw a real key (~0 = never).
void rehook_keyboard();
// KS3: arrow presses (key-down events, Windows' key repeat included) seen by
// the hook - [0] LEFT, [1] RIGHT without CTRL/ALT, [2] any arrow at all.
void arrow_counts(unsigned out[3]);
// KS4: the key's physical state from the system, past ReShade's filter (which
// answers "up" for every key while the panel blocks the game's input).
bool raw_key(int vk);
// PO3: called every frame the MGPU panel is drawn on its own: for the next
// 250 ms Esc (which closes it) is kept from the game, its release too. Expires
// by itself, so Esc can never stay swallowed after the panel is gone.
void panel_escape_alive();
bool kb_hook_state(unsigned long long &last_key_ms);
bool in_front();
// SV3: how long ago a real (not injected) key press / mouse action was, in ms (~0 = never).
// False when the hooks are not running.
bool real_input_ages(unsigned long long &key_ms, unsigned long long &mouse_ms);

// Bridge thread, once per loop iteration. Fills `out`; returns true when any
// key fired. Does nothing (and reports nothing) unless a window of this
// process is in the foreground.
bool poll(presses &out);

// ---- panel (any thread) ----
unsigned    count();
const char *label(unsigned a);           // "Screenshot"
void        name(unsigned a, char *out, unsigned n);   // "CTRL+ALT+P"
// 0 = fine, 1 = the same keys as another of ours, 2 = another program has
// registered these keys too, 3 = no modifier (the game also sees the key).
int         conflict(unsigned a);
void        begin_capture(unsigned a);   // next combination pressed becomes the binding
void        cancel_capture();
int         capturing();                 // action being captured, -1 = none
// HK13: Arm (CTRL+ALT+F10) and Hide/show (CTRL+ALT+F6) are FIXED - not on the
// panel's list, never rebound, never taken by another action.
bool        fixed(unsigned a);
// HK15: the action's CURRENT combination as text ("CTRL+ALT+E"), for messages and
// tips that used to name the default. A small per-thread ring of buffers.
const char *name_now(unsigned a);
// HK10: why the last combination was refused (in use, not Ctrl/Alt + A-Z/F1-F12,
// ReShade's key) - for 6 s, while still capturing.
bool        capture_refusal(char *out, unsigned n);
// HK7: a key the PANEL saw (ImGui) while capturing - a second source beside the
// keyboard hook. mods: 1 CTRL, 2 ALT, 4 SHIFT. True if it became the binding.
bool        capture_from_panel(unsigned char mods, int vk);
void        reset_defaults();

} // namespace hotkeys
} // namespace mgpu
