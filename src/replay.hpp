// ---------------------------------------------------------------------------
// replay.hpp - BM2. Input replay for repeatable benchmarks.
//
// Record once: the path hotkey starts recording keyboard and mouse input
// (keys, relative mouse movement, buttons, wheel) while the game is in front,
// and stops it again. One path per game, saved to
// <add-on folder>\mgpu\benchmarks\paths\<game exe>.mgpupath.
//
// Play: with BenchReplay=1 every benchmark plays the path back from the end of
// its countdown and lasts exactly as long as the path. Any key pressed on the
// real keyboard, or another window coming to the front, stops the playback at
// once and releases every key and button it was holding.
//
// Read by low-level hooks (WH_KEYBOARD_LL / WH_MOUSE_LL) that exist only while
// a path is being recorded or played - never registered raw input, which would
// take the game's own registration away. Played with SendInput (scan codes and
// relative mouse moves), which raw-input games read like a real device.
//
// Mouse turns repeat exactly only with Windows "Enhance pointer precision"
// off: the hook sees accelerated movement. The log says when it is on.
// Games with anti-cheat may object to injected input - single-player only.
// ---------------------------------------------------------------------------
#pragma once

namespace mgpu
{
namespace replay
{

void toggle_record();             // the path hotkey
bool recording();
bool playing();
bool has_path(double *seconds = nullptr, unsigned *events = nullptr);

// Benchmark. Starts playing now; false if there is no path.
bool start_play();
void stop_play();
bool aborted();                   // the last playback was stopped by the user

void shutdown();
void emergency_unhook();   // REV: DllMain live unload - remove the hooks, join nothing
void hud_line(char *out, unsigned n);       // "PATH REC 12" / "" (font-safe)
void last_message(char *out, unsigned n);   // for the panel

} // namespace replay
} // namespace mgpu
