// ---------------------------------------------------------------------------
// gamepresent.hpp - GP1/GP2 (experiment). Keep the game's own frames off the screen.
//
// The game keeps presenting its own frames (about 40 fps, from the render card)
// to its window while the bridge shows the neural picture over it. PresentMon
// (2026-11-14, gsync.csv / 120.csv) shows both swap chains reaching the screen:
// the bridge's at 120 fps and the game's at 38 fps, 8594 of 9174 shown. Two
// live pictures keep Windows composing every refresh at a fixed 240 Hz, and
// with windowed G-SYNC the driver follows the game's 38 fps chain, so the
// compositor ran near 60 Hz and the bridge's Present blocked ~16 ms (58 fps).
//
// GP1 (13:26) skipped the game's Present and faked the back-buffer index; the
// game raced the six-slot hand-over and a device was lost within a second
// (Unreal: "GPU Crash dump Triggered"). GP2, GamePresentSkip=1 (off by default):
// - the back-buffer index is left alone, so the game, ReShade and Windows agree;
// - skipping starts only after the picture has run 10 s without a break;
// - each skipped Present waits, like the real one, until the render card has
//   finished the game's frame from GamePresentLatency (default 2) frames ago;
// - a re-entry guard sends any nested call straight to Windows;
// - skipping stops for the session on a lost device, a hand-over overrun or
//   5 brake time-outs (100 ms each).
// While the picture steps aside (panel open, CTRL+ALT+F6) or the bridge stops,
// the game's presents go through. Present and Present1 are hooked through the
// game's own swap chain's vtable; other chains sharing it pass straight through.
// Logged as [MGPU][GP2]: at attach, at the first skip, then every 30 s.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <unknwn.h>   // IUnknown: the project builds with WIN32_LEAN_AND_MEAN

namespace mgpu
{
namespace gamepresent
{

void attach(IUnknown *game_swapchain);   // the game's native D3D12 swap chain, at init (and after a resize)
void detach(IUnknown *game_swapchain);   // the same chain, at destroy (and before a resize)
void bridge_presented();                 // the bridge thread, after each S_OK Present
bool on();                               // GamePresentSkip=1 (read once)

} // namespace gamepresent
} // namespace mgpu
