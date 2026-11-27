// replay_rules_test.cpp - LR106. When input replay stops (src/replay_rules.hpp).
#include "replay_rules.hpp"
#include <cstdio>

using namespace mgpu::replay_rules;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main()
{
    inputs q{};                                   // nothing happening: keep playing
    CHECK(should_stop(q) == stop_why::none);

    // the Cyberpunk report: overlay open while the replay runs -> stop
    { inputs i = q; i.overlay_open = true; CHECK(should_stop(i) == stop_why::overlay); }
    // Esc read from the key state, no hook needed (Cyberpunk calls no keyboard hook)...
    { inputs i = q; i.esc_down = true; CHECK(should_stop(i) == stop_why::escape); }
    // ...unless the replay itself is holding Esc (the path pressed it)
    { inputs i = q; i.esc_down = true; i.replay_holds_esc = true; CHECK(should_stop(i) == stop_why::none); }
    // a real key where the game allows hooks
    { inputs i = q; i.key_hook = true; CHECK(should_stop(i) == stop_why::key); }
    // the mouse: a button or the wheel at once, movement past the threshold
    { inputs i = q; i.mouse_button = true; CHECK(should_stop(i) == stop_why::mouse); }
    { inputs i = q; i.real_move_px = REAL_MOVE_PX; CHECK(should_stop(i) == stop_why::none); }      // a resting hand
    { inputs i = q; i.real_move_px = REAL_MOVE_PX + 1; CHECK(should_stop(i) == stop_why::mouse); }
    // another window in front
    { inputs i = q; i.foreground = false; CHECK(should_stop(i) == stop_why::foreground); }
    // the overlay is named first when several are true (it is what the user sees)
    { inputs i = q; i.overlay_open = true; i.mouse_button = true; i.foreground = false; CHECK(should_stop(i) == stop_why::overlay); }
    // every reason has words for the log and the panel
    for (int w = 0; w <= 5; ++w) CHECK(stop_text((stop_why)w)[0] != '?');

    // the benchmark countdown waits for the overlay only when replay is on
    CHECK(hold_countdown(true, true));
    CHECK(!hold_countdown(true, false));
    CHECK(!hold_countdown(false, true));

    std::printf("replay_rules_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
