// ---------------------------------------------------------------------------
// replay_rules.hpp - LR106. When input replay must stop (portable, tested in
// tests/replay_rules_test.cpp; used by replay.cpp's playback thread).
//
// Cyberpunk 2077, 2026-11-26, LR105: an input-replay benchmark started while
// the overlay was open, and the replayed mouse moved the panel's cursor - the
// user could not use the panel until the replay ended. The only way out was a
// real KEY, seen by a low-level keyboard hook - and Cyberpunk never calls
// keyboard hooks (0 calls in the same log; its mouse hook works). So:
//   - the overlay (ReShade's menu or the MGPU panel) open = stop;
//   - Esc pressed for real = stop, read from the key's state (GetAsyncKeyState),
//     which needs no hook - unless the replay itself is holding Esc down;
//   - a real mouse button, wheel, or real movement past a small threshold =
//     stop (the mouse hook, which Cyberpunk does call);
//   - a real key (keyboard hook, where the game allows it) and another window
//     in front stop it as before.
// ---------------------------------------------------------------------------
#pragma once

namespace mgpu
{
namespace replay_rules
{
enum class stop_why : int { none = 0, key = 1, mouse = 2, escape = 3, overlay = 4, foreground = 5 };

// Real mouse movement, summed (|dx| + |dy|) since the replay started; a hand
// resting on the mouse does not move it this far.
const int REAL_MOVE_PX = 48;

struct inputs
{
    bool key_hook = false;        // the keyboard hook saw a real key go down
    bool mouse_button = false;    // the mouse hook saw a real button / wheel
    int real_move_px = 0;         // real movement summed by the mouse hook
    bool esc_down = false;        // GetAsyncKeyState(VK_ESCAPE) says down
    bool replay_holds_esc = false;// ...because the replay pressed it
    bool overlay_open = false;    // ReShade's menu or the MGPU panel
    bool foreground = true;       // the game's window is in front
};

inline stop_why should_stop(const inputs &in)
{
    if (in.overlay_open) return stop_why::overlay;
    if (in.esc_down && !in.replay_holds_esc) return stop_why::escape;
    if (in.key_hook) return stop_why::key;
    if (in.mouse_button || in.real_move_px > REAL_MOVE_PX) return stop_why::mouse;
    if (!in.foreground) return stop_why::foreground;
    return stop_why::none;
}

inline const char *stop_text(stop_why w)
{
    switch (w)
    {
    case stop_why::none:       return "it reached the end";
    case stop_why::key:        return "a real key was pressed";
    case stop_why::mouse:      return "the mouse was used";
    case stop_why::escape:     return "Esc was pressed";
    case stop_why::overlay:    return "the overlay (ReShade's menu or the MGPU panel) was open";
    case stop_why::foreground: return "another window came to the front";
    }
    return "?";
}

// A benchmark with input replay does not start its countdown's last second
// while the overlay is open: the countdown starts over when it closes.
inline bool hold_countdown(bool replay_on, bool overlay_open) { return replay_on && overlay_open; }
}   // namespace replay_rules
}   // namespace mgpu
