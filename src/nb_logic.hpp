#pragma once
// ---------------------------------------------------------------------------
// nb_logic.hpp - NB1/NB3: the decisions behind the NR / FG-stage timestamps and
// the NR time budget, with no D3D12 or Windows types, so they can be tested on
// any compiler (tests/nb_logic_test.cpp).
//
// Marks (GB1's query heap): 5 = NR passes done, 6 = FG stage start, 7 = FG
// stage end. A list records only the marks for the work it did; only those are
// resolved and only those are read.
// ---------------------------------------------------------------------------

namespace mgpu
{
namespace nb
{
enum : unsigned { M_NR = 1u, M_FG0 = 2u, M_FG1 = 4u, M_FG = M_FG0 | M_FG1 };

const unsigned kFirstQuery = 5u;          // heap index of mark 5
const unsigned long long kBaseOffset = 40ull;   // readback offset of mark 5 (8 bytes per mark)

struct range { unsigned first = 0, count = 0; unsigned long long offset = 0; };

// The ResolveQueryData ranges for the marks this list wrote: never an unwritten
// query. Returns how many (0-2). Both NR and FG: one range 5-7.
inline unsigned resolve_plan(unsigned mask, range out[2])
{
    const bool nr = (mask & M_NR) != 0u;
    const bool fg = (mask & M_FG) == M_FG;   // a start without an end is not a measurement
    unsigned n = 0;
    if (nr && fg) { out[n++] = { kFirstQuery, 3u, kBaseOffset }; return n; }
    if (nr) out[n++] = { kFirstQuery, 1u, kBaseOffset };
    if (fg) out[n++] = { kFirstQuery + 1u, 2u, kBaseOffset + 8ull };
    return n;
}

// Which marks resolve_plan resolved, i.e. which may be read back.
inline unsigned readable(unsigned mask)
{
    unsigned r = 0;
    if ((mask & M_NR) != 0u) r |= M_NR;
    if ((mask & M_FG) == M_FG) r |= M_FG;
    return r;
}

struct sample
{
    double nr_ms = -1.0, fg_ms = -1.0;   // -1: not measured this list
    bool nr_bad = false, fg_bad = false; // written and resolved, but out of order (counted as missing)
};

// tv2: NR stage start, tv3: end of NR + post-NR work, tv4: end of the list
// (P2.2 marks); g5-g7 the NB marks; k = ms per tick.
inline sample read(unsigned mask, unsigned long long tv2, unsigned long long tv3, unsigned long long tv4,
                   unsigned long long g5, unsigned long long g6, unsigned long long g7, double k)
{
    sample s;
    const unsigned r = readable(mask);
    if ((r & M_NR) != 0u)
    {
        if (g5 >= tv2 && g5 <= tv3) s.nr_ms = (double)(g5 - tv2) * k;
        else s.nr_bad = true;
    }
    if ((r & M_FG) == M_FG)
    {
        if (g6 >= tv3 && g7 >= g6 && g7 <= tv4) s.fg_ms = (double)(g7 - g6) * k;
        else s.fg_bad = true;
    }
    return s;
}

// The budget controller's window. Every NR sample carries the scale its list
// was RECORDED at; only samples at the scale the controller is waiting on
// count. A sample from before a change (still in flight when the scale moved)
// is dropped as stale and never resets the window.
struct window
{
    int scale = -1;            // the scale being measured (x10), -1 = none yet
    double sum = 0.0;
    unsigned n = 0;
    unsigned long long stale = 0;

    void note(double nr_ms, int recorded_scale)
    {
        if (nr_ms < 0.0 || recorded_scale < 0 || scale < 0) return;   // no scale, or no window open yet
        if (recorded_scale != scale) { ++stale; return; }
        sum += nr_ms; ++n;
    }
    // The mean at `cur` since the last take (-1 / 0 when there is none), then
    // start a fresh window at `cur`.
    void take(int cur, double *avg, unsigned *count)
    {
        *avg = -1.0; *count = 0;
        if (scale == cur && n > 0) { *avg = sum / (double)n; *count = n; }
        scale = cur; sum = 0.0; n = 0;
    }
};

const unsigned kMinSamples = 20u;   // real frames at one scale before a budget decision
const int kStepMin = 20, kStepMax = 60;   // x10: 2-6 points

struct step
{
    int next = 0;         // the scale to go to (== cur: stay)
    bool over = false;    // the reading is over the budget
    bool at_floor = false;// over, and already at the lowest scale
};

// Over budget: a step down sized from NR's cost following the pixel count (the
// square of the scale), bounded 2-6 points, not below lo. dwell_ok: enough
// time since the last budget move. bud: whole ms, 0 = off.
inline step budget_step(int cur, int lo, double avg, unsigned n, int bud, bool dwell_ok)
{
    step r; r.next = cur;
    if (bud <= 0 || avg <= 0.0 || n < kMinSamples || avg <= (double)bud) return r;
    r.over = true;
    if (cur <= lo) { r.at_floor = true; return r; }
    if (!dwell_ok) return r;
    // sqrt without <cmath>: a few Newton steps on a value in (0, 1].
    const double q = (double)bud * 0.95 / avg;
    double x = q < 1.0 ? q : 1.0;
    if (x <= 0.0) x = 1e-6;
    double y = 0.5 * (x + 1.0);
    for (int i = 0; i < 8; ++i) y = 0.5 * (y + x / y);
    int st = cur - (int)((double)cur * y);
    if (st < kStepMin) st = kStepMin;
    if (st > kStepMax) st = kStepMax;
    r.next = cur - st;
    if (r.next < lo) r.next = lo;
    return r;
}

// May the scale step up by `up` (x10)? Off: yes. On: only with a reading, and
// only when the predicted NR time at the next scale leaves 10% of the budget.
inline bool room_up(int cur, int up, double avg, unsigned n, int bud)
{
    if (bud <= 0) return true;
    if (avg <= 0.0 || n < kMinSamples || cur <= 0) return false;
    const double g = (double)(cur + up) / (double)cur;
    return avg * g * g <= (double)bud * 0.9;
}
}
}
