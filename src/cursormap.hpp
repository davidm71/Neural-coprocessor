// ---------------------------------------------------------------------------
// cursormap.hpp - MC1. The mouse pointer on the enlarged picture.
//
// With the scaler on, the game runs in a 2560x1440 window and the mod shows it
// enlarged over the whole 4K monitor. The game still reads the mouse in its
// own window's pixels, so the pointer Windows draws and the buttons on the
// enlarged picture do not line up. This does what Lossless Scaling does:
//
//   - while the game shows a pointer, the real pointer is confined to the
//     game's window (a click can never land on the desktop and steal focus);
//   - the game's own pointer is hidden on screen (its SetCursor calls are
//     answered with a blank cursor), and the SAME pointer image is drawn into
//     the enlarged picture at the enlarged position.
//
// The game is never lied to about where the mouse is - it gets real input in
// its own window. Only what you see moves. The pointer travels 1.5x as far on
// screen as the hand moves it at 1440p -> 4K, the same as Lossless Scaling.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <cstdint>

namespace mgpu
{
namespace cursormap
{

// Patch the game's imports of SetCursor / ShowCursor (by name). True when
// SetCursor was found - without it the real pointer cannot be hidden and the
// caller keeps the old behaviour (the picture steps aside in menus).
bool install(HWND game);
void uninstall();
bool installed();

// Bridge thread, ~10x a second. `shown`: the enlarged picture is on screen
// and the game has the foreground. Confines the pointer to the game window
// while the game shows one, releases it otherwise.
void tick(bool shown);

// Present thread. Whether a pointer should be drawn into the picture, where
// (game-window pixels, hot spot already applied by the caller), and its image
// (BGRA, top-down). `serial` changes when the image changes.
struct sprite
{
    bool          visible = false;
    int           x = 0, y = 0;          // pointer position in game-window pixels
    int           hot_x = 0, hot_y = 0;  // hot spot inside the image
    unsigned      w = 0, h = 0;
    unsigned      serial = 0;
    const uint32_t *pixels = nullptr;    // valid until the next call
};
bool snapshot(sprite &out);

} // namespace cursormap
} // namespace mgpu
