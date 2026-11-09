// ---------------------------------------------------------------------------
// fg_clock.hpp - FC1: the even frame clock (FGClock=1). Portable, no Windows:
// tests/fg_clock_test.cpp runs it against simulated games.
//
// Dynamic multiplier spaces the frames of each real frame (a "pair") evenly
// inside that pair: at 67 real fps and 180 shown, pairs of 3 frames are 5.0 ms
// apart and pairs of 2 are 7.5 ms apart. The clock instead shows every frame on
// one tick grid at the target rate (5.56 ms), and for each tick shows the
// generated frame whose scene time is nearest to what that tick should show.
//
// WHY THE FIRST ATTEMPT (PERF1, FGGlobalPace) FAILED, AND WHAT IS DIFFERENT:
// its clock ran on its own and was thrown away whenever a frame missed its slot
// by half a tick - 208-303 times per 30 s at 180-200 fps, each a hitch. Here:
//  - the clock FOLLOWS the real frames: the delay between a frame's scene time
//    and its display time (L) is a slow average of what the hand-off actually
//    shows, so the grid drifts with the game instead of fighting it;
//  - a late tick does not restart anything: the grid continues from now
//    (one longer gap), and the frames that are now stale are skipped instead of
//    being shown in a burst;
//  - the frame for a tick is CHOSEN by scene time, so the picture moves at the
//    right speed even though the count per real frame varies;
//  - a small cushion (half a tick, more when the hand-off time wobbles) keeps
//    most ticks from being late at all.
// The cost is that cushion: about half a tick of extra delay (2.8 ms at 180).
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>

namespace mgpu
{
namespace fg_clock
{

struct state
{
    double next_tick = 0.0;   // when the next frame is due (time units of the caller, e.g. QPC)
    double L = 0.0;           // scene time -> display time, followed slowly
    double jit = 0.0;         // how much the hand-off wobbles around L
    bool   have = false;
    // counts since the last report
    unsigned long long shown = 0, late = 0, gaps = 0, skipped = 0, rebases = 0;
    double err_sum = 0.0;     // |scene time shown - scene time wanted|
    double cushion = 0.0;     // last cushion used
    void reset() { have = false; next_tick = 0.0; L = 0.0; jit = 0.0; }
    void clear_counts() { shown = late = gaps = skipped = rebases = 0; err_sum = 0.0; }
};

// One pair. c[0..M-1] are the scene times of the frames in order, the last one
// the real frame (c[M-1] = its capture time); span is the time since the
// previous real frame; t0 is when the pair was handed over; P is the tick.
// now() reads the clock, wait_until(t) sleeps until t, present(j, wanted)
// shows frame j and returns false on failure. Returns false if a present failed.
template <class Now, class Wait, class Present>
bool run_pair(state &st, double P, double t0, double span, const double *c, unsigned M,
              Now now, Wait wait_until, Present present)
{
    if (M == 0 || P <= 0.0) return true;
    const unsigned N = M - 1;   // in-between frames
    // The delay the per-pair schedule has: the first in-between frame shown at
    // hand-over. Followed slowly (1/8 per pair); the wobble decides the cushion.
    const double base = (t0 - c[M - 1]) + span * (double)N / (double)(N + 1);
    if (!st.have) { st.L = base; st.jit = P * 0.25; }
    else
    {
        const double d = base - st.L;
        st.L += d / 8.0;
        st.jit += (std::fabs(d) - st.jit) / 16.0;
    }
    double cushion = 2.0 * st.jit;
    if (cushion < P * 0.5) cushion = P * 0.5;
    if (cushion > P * 2.0) cushion = P * 2.0;
    st.cushion = cushion;
    const double Lc = st.L + cushion;

    double t = now();
    const double first_ideal = c[0] + Lc;
    if (!st.have || st.next_tick < t - 4.0 * P || st.next_tick > (t > first_ideal ? t : first_ideal) + 4.0 * P)
    {
        if (st.have) ++st.rebases;   // after a stall (pause, loading): start the grid again
        st.next_tick = (t > first_ideal) ? t : first_ideal;
        st.have = true;
    }

    unsigned idx = 0;
    int empty = 0;
    while (idx < M)
    {
        t = now();
        if (t > st.next_tick + P * 0.5) { ++st.late; st.next_tick = t; }   // late: carry on from now
        const double want = st.next_tick - Lc;
        if (c[idx] - want > P * 0.5)
        {
            // Nothing to show yet for this tick (the frames are further ahead):
            // leave it empty. Bounded, then the grid moves to the frame.
            ++st.gaps;
            st.next_tick += P;
            if (++empty > 8) { st.next_tick = c[idx] + Lc; empty = 0; ++st.rebases; }
            continue;
        }
        empty = 0;
        unsigned j = idx;   // the frame nearest to what this tick should show (never back in time)
        while (j + 1 < M && std::fabs(c[j + 1] - want) <= std::fabs(c[j] - want)) ++j;
        wait_until(st.next_tick);
        if (!present(j, want)) return false;
        st.err_sum += std::fabs(c[j] - want);
        st.skipped += j - idx;
        ++st.shown;
        idx = j + 1;
        st.next_tick += P;
    }
    return true;
}

} // namespace fg_clock
} // namespace mgpu
