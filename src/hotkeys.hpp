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
void        reset_defaults();

} // namespace hotkeys
} // namespace mgpu
