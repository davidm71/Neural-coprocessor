// NB3: the NR / FG-stage timestamp decisions and the NR time budget
// (src/nb_logic.hpp). Portable: builds with MSVC, g++ and clang.
#include "nb_logic.hpp"

#include <cmath>
#include <cstdio>

namespace nb = mgpu::nb;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void resolve_tests()
{
    nb::range r[2];
    // A list that ran nothing measurable (NR off, no FG, or a failed list): nothing resolved.
    CHECK(nb::resolve_plan(0u, r) == 0u);
    // NR only: mark 5 alone, never 6-7.
    CHECK(nb::resolve_plan(nb::M_NR, r) == 1u);
    CHECK(r[0].first == 5u && r[0].count == 1u && r[0].offset == 40ull);
    // FG only: marks 6-7, never 5.
    CHECK(nb::resolve_plan(nb::M_FG, r) == 1u);
    CHECK(r[0].first == 6u && r[0].count == 2u && r[0].offset == 48ull);
    // Both: one range 5-7.
    CHECK(nb::resolve_plan(nb::M_NR | nb::M_FG, r) == 1u);
    CHECK(r[0].first == 5u && r[0].count == 3u && r[0].offset == 40ull);
    // An FG start without its end (the stage did not finish recording): not resolved.
    CHECK(nb::resolve_plan(nb::M_FG0, r) == 0u);
    CHECK(nb::resolve_plan(nb::M_FG1, r) == 0u);
    CHECK(nb::resolve_plan(nb::M_NR | nb::M_FG0, r) == 1u && r[0].first == 5u && r[0].count == 1u);
    // Every resolved query was written.
    for (unsigned m = 0; m < 8u; ++m)
    {
        const unsigned n = nb::resolve_plan(m, r);
        for (unsigned i = 0; i < n; ++i)
            for (unsigned q = r[i].first; q < r[i].first + r[i].count; ++q)
            {
                const unsigned bit = q == 5u ? nb::M_NR : (q == 6u ? nb::M_FG0 : nb::M_FG1);
                CHECK((m & bit) != 0u);
                CHECK(r[i].offset + 8ull * (q - r[i].first) == 40ull + 8ull * (q - 5u));
            }
    }
}

static void read_tests()
{
    const double k = 0.001;   // 1 tick = 1 us -> ms
    // tv2 = 1000 (NR start), tv3 = 9000, tv4 = 20000; mark 5 at 7000; FG 9500..15500.
    nb::sample s = nb::read(nb::M_NR | nb::M_FG, 1000, 9000, 20000, 7000, 9500, 15500, k);
    CHECK(near(s.nr_ms, 6.0) && near(s.fg_ms, 6.0) && !s.nr_bad && !s.fg_bad);
    // NR only: FG values in the buffer are leftovers and must not be read.
    s = nb::read(nb::M_NR, 1000, 9000, 20000, 7000, 1, 999999, k);
    CHECK(near(s.nr_ms, 6.0) && s.fg_ms < 0.0 && !s.fg_bad);
    // FG only: mark 5 is a leftover.
    s = nb::read(nb::M_FG, 1000, 9000, 20000, 5, 9500, 15500, k);
    CHECK(s.nr_ms < 0.0 && !s.nr_bad && near(s.fg_ms, 6.0));
    // Nothing written (failed / skipped list).
    s = nb::read(0u, 1000, 9000, 20000, 7000, 9500, 15500, k);
    CHECK(s.nr_ms < 0.0 && s.fg_ms < 0.0 && !s.nr_bad && !s.fg_bad);
    // Out of order: counted as unusable, not as a time.
    s = nb::read(nb::M_NR, 1000, 9000, 20000, 500, 0, 0, k);
    CHECK(s.nr_ms < 0.0 && s.nr_bad);
    s = nb::read(nb::M_FG, 1000, 9000, 20000, 0, 16000, 15000, k);
    CHECK(s.fg_ms < 0.0 && s.fg_bad);
    s = nb::read(nb::M_FG, 1000, 9000, 20000, 0, 9500, 25000, k);   // past the list's end
    CHECK(s.fg_ms < 0.0 && s.fg_bad);
    // Only the start of FG written: not read, not counted.
    s = nb::read(nb::M_FG0, 1000, 9000, 20000, 0, 9500, 15500, k);
    CHECK(s.fg_ms < 0.0 && !s.fg_bad);
}

static void window_tests()
{
    nb::window w;
    double a; unsigned n;
    // Before the controller opens a window nothing counts (and nothing is "stale").
    w.note(5.0, 670);
    CHECK(w.n == 0u && w.stale == 0ull);
    w.take(670, &a, &n);
    CHECK(a < 0.0 && n == 0u);
    for (int i = 0; i < 30; ++i) w.note(8.0, 670);
    // No scale on the sample (the list ran without SR): ignored.
    w.note(99.0, -1);
    w.take(670, &a, &n);
    CHECK(near(a, 8.0) && n == 30u);
    // The controller moves 67% -> 63%: it opens the next window at 630 ...
    w.take(630, &a, &n);
    // ... while lists recorded at 67% are still being read back: stale, and
    // they must not reset or pollute the new window.
    w.note(7.0, 630);
    w.note(8.0, 670);
    w.note(8.0, 670);
    w.note(7.0, 630);
    CHECK(w.stale == 2ull && w.n == 2u && w.scale == 630);
    w.take(630, &a, &n);
    CHECK(near(a, 7.0) && n == 2u);
    // Taking at a scale the window was not measuring gives nothing.
    for (int i = 0; i < 25; ++i) w.note(6.0, 630);
    w.take(600, &a, &n);
    CHECK(a < 0.0 && n == 0u);
}

static void budget_tests()
{
    nb::step s;
    // Off, no reading, too few frames, or under budget: stay.
    s = nb::budget_step(670, 500, 15.0, 40, 0, true);   CHECK(s.next == 670 && !s.over);
    s = nb::budget_step(670, 500, -1.0, 0, 12, true);   CHECK(s.next == 670 && !s.over);
    s = nb::budget_step(670, 500, 15.0, 19, 12, true);  CHECK(s.next == 670 && !s.over);
    s = nb::budget_step(670, 500, 11.9, 40, 12, true);  CHECK(s.next == 670 && !s.over);
    // Slightly over: a small step. sqrt(0.95*12/12.2) = 0.9666 -> 670 - 647 = 2.3 points.
    s = nb::budget_step(670, 500, 12.2, 40, 12, true);  CHECK(s.over && s.next == 647);
    // Just over: never less than 2 points.
    s = nb::budget_step(670, 500, 12.01, 40, 12, true); CHECK(s.over && s.next == 650);
    // Far over: capped at 6 points.
    s = nb::budget_step(670, 500, 30.0, 40, 12, true);  CHECK(s.over && s.next == 610);
    // In between: sized from the pixel count. 67% at 14 ms for 12: sqrt(0.95*12/14) = 0.902 -> ~6.5 pts -> 6 (cap).
    s = nb::budget_step(670, 500, 14.0, 40, 12, true);  CHECK(s.over && s.next == 610);
    // 13 ms: sqrt(0.95*12/13) = 0.936 -> 4.3 points.
    s = nb::budget_step(670, 500, 13.0, 40, 12, true);  CHECK(s.over && s.next >= 620 && s.next <= 630);
    // Dwell not over: over, but no move.
    s = nb::budget_step(670, 500, 13.0, 40, 12, false); CHECK(s.over && s.next == 670 && !s.at_floor);
    // Never below the floor; at the floor it says so.
    s = nb::budget_step(510, 500, 30.0, 40, 12, true);  CHECK(s.next == 500);
    s = nb::budget_step(500, 500, 30.0, 40, 12, true);  CHECK(s.over && s.at_floor && s.next == 500);
    // Room up: off = always; on = needs a reading and 10% spare at the next scale.
    CHECK(nb::room_up(670, 20, -1.0, 0, 0));
    CHECK(!nb::room_up(670, 20, -1.0, 0, 12));
    CHECK(!nb::room_up(670, 20, 10.0, 5, 12));
    // 10 ms at 67% -> 69%: 10 * (69/67)^2 = 10.6 <= 10.8: yes.
    CHECK(nb::room_up(670, 20, 10.0, 40, 12));
    // 10.3 ms -> 10.93 > 10.8: no (hysteresis: under budget, but not with room).
    CHECK(!nb::room_up(670, 20, 10.3, 40, 12));
    // A down step then an up step cannot ping-pong: after stepping down because
    // 12.2 > 12, the new reading 12.2*(64.7/67)^2 = 11.38 is not room to go back up.
    CHECK(!nb::room_up(647, 20, 12.2 * (647.0 / 670.0) * (647.0 / 670.0), 40, 12));
}

int main()
{
    resolve_tests();
    read_tests();
    window_tests();
    budget_tests();
    std::printf("nb_logic_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
