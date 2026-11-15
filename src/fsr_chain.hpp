// ---------------------------------------------------------------------------
// fsr_chain.hpp - AC3: the decisions of AMD chained 4x, free of D3D12 so they
// can be tested anywhere (tests/fsr_chain_test.cpp).
//
// AMD FSR 3 frame generation makes one frame, at the halfway point, per pair
// of frames it is given. Context A is given the real frames and makes the 1/2
// frame. Context B is given a stream twice as fast - A's 1/2 frame, then the
// real frame - so its halfway frames are the 1/4 and the 3/4.
//
// B's history (its previous input) is only trustworthy when B's previous step
// succeeded and A's frame sequence has not broken since. `b_ok_fid` holds A's
// frame ID for which B's history is known good; 0 means "not known good". It
// is cleared before every attempt and set only after success, so a failure at
// any step leaves it 0 and the next pair primes B again.
//
// Composition is transactional: every AMD step runs first, and the frames are
// composed only once the outcome is known - all three (4x) or the 1/2 frame
// alone (2x). Nothing is written to the set's frame or HUD-mask state for a
// 4x pair that did not complete.
// ---------------------------------------------------------------------------
#pragma once
#include <cstdint>

namespace mgpu
{
namespace fsr_chain
{

enum class plan
{
    off,     // chain not used for this pair: compose the 1/2 frame (2x)
    prime,   // B's history is not known good: feed B the real frame only, compose 2x
    chain    // B's history is good: B makes 1/4 and 3/4, compose 4x if both land
};

struct state
{
    uint64_t b_ok_fid = 0;   // A's frame ID B's history is good for (0 = not good)
    uint64_t pairs4 = 0, pairs2 = 0, primes = 0, prime_fails = 0, partials = 0;
};

// Before any AMD step of B for the pair whose A frame ID is `a_fid` (the ID A
// just used). `enabled`: FSRChain on and FrameGen=3. `usable`: B has not been
// switched off for the run and the set has its two extra buffers.
inline plan begin(state &st, uint64_t a_fid, bool enabled, bool usable)
{
    const uint64_t good = st.b_ok_fid;
    st.b_ok_fid = 0;   // known good again only after a successful step below
    if (!enabled || !usable) return plan::off;
    if (good == 0 || a_fid != good + 1) return plan::prime;   // first pair, a break, or a failure
    return plan::chain;
}

// After the AMD steps of the plan. `prime_ok`: the priming step succeeded.
// `q_ok` / `t_ok`: the 1/4 and 3/4 steps succeeded (t is only attempted after
// q). Returns the in-between frames to compose: 3 (4x) or 1 (2x).
inline unsigned finish(state &st, plan p, uint64_t a_fid, bool prime_ok, bool q_ok, bool t_ok)
{
    switch (p)
    {
    case plan::off:
        return 1;
    case plan::prime:
        if (prime_ok) { st.b_ok_fid = a_fid; ++st.primes; }
        else ++st.prime_fails;
        ++st.pairs2;
        return 1;
    case plan::chain:
        if (q_ok && t_ok) { st.b_ok_fid = a_fid; ++st.pairs4; return 3; }
        if (q_ok) ++st.partials;   // 1/4 made, 3/4 failed: B's history is off
        ++st.pairs2;
        return 1;
    }
    return 1;
}

// AMD's frameTimeDelta from the hand-over times of two consecutive frames A
// was given (QPC ticks, `qpf` per second). Valid: both stamped, increasing,
// 2 to 200 ms apart. Otherwise the last good value is kept (16.7 ms if there
// is none), and *valid says which. This replaces the NR worker's own
// processing interval, which includes the bridge's scheduling jitter.
inline float frame_dt_ms(uint64_t prev_qpc, uint64_t cur_qpc, uint64_t qpf, float last_good, bool *valid)
{
    const float fallback = (last_good >= 2.0f && last_good <= 200.0f) ? last_good : 16.6667f;
    bool ok = prev_qpc != 0 && cur_qpc != 0 && qpf != 0 && cur_qpc > prev_qpc;
    double ms = 0.0;
    if (ok)
    {
        ms = (double)(cur_qpc - prev_qpc) * 1000.0 / (double)qpf;
        ok = ms >= 2.0 && ms <= 200.0;
    }
    if (valid != nullptr) *valid = ok;
    return ok ? (float)ms : fallback;
}

// AC4: why frame_dt_ms reused the last good value (or none if it did not).
// `contiguous`: A's frame IDs follow on from the last frame it was given.
enum class dt_reuse { none, sequence_break, missing_stamp, invalid_interval };
inline dt_reuse classify_dt(bool contiguous, uint64_t prev_qpc, uint64_t cur_qpc, bool valid)
{
    if (!contiguous) return dt_reuse::sequence_break;
    if (prev_qpc == 0 || cur_qpc == 0) return dt_reuse::missing_stamp;
    return valid ? dt_reuse::none : dt_reuse::invalid_interval;
}

}
}
