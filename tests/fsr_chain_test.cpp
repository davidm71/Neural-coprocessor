// tests/fsr_chain_test.cpp - AC3: the decisions of AMD chained 4x.
//
//   g++ -std=c++17 -O2 -Isrc tests/fsr_chain_test.cpp -o fsr_chain_test && ./fsr_chain_test
//
// fsr_chain.hpp holds every decision the chain makes - when context B must be
// primed, when a pair is 4x, what a failure leaves behind - and AMD's frame
// time. gpu1_context.cpp only carries those decisions out, so these tests
// cover the logic without a GPU. The `sim` driver below mirrors fsr_place:
// begin -> B steps -> finish -> compose n frames.
#include "fsr_chain.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace mgpu::fsr_chain;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// What B does on the next step: succeed, or fail the call.
struct script
{
    std::vector<bool> steps;   // consumed in order; missing entries succeed
    size_t i = 0;
    bool next() { return i < steps.size() ? steps[i++] : (++i, true); }
};

struct outcome { plan p; unsigned n; bool b_called_q, b_called_t, b_called_prime; };

// One pair, as fsr_place runs it. `a_fid` is the frame ID A just used.
static outcome pair(state &st, uint64_t a_fid, bool enabled, bool usable, script &b)
{
    outcome o{};
    o.p = begin(st, a_fid, enabled, usable);
    bool prime_ok = false, q_ok = false, t_ok = false;
    if (o.p == plan::prime) { o.b_called_prime = true; prime_ok = b.next(); }
    else if (o.p == plan::chain)
    {
        o.b_called_q = true; q_ok = b.next();
        if (q_ok) { o.b_called_t = true; t_ok = b.next(); }
    }
    o.n = finish(st, o.p, a_fid, prime_ok, q_ok, t_ok);
    return o;
}

static void test_first_pair_primes_then_chains()
{
    state st; script b;
    outcome o1 = pair(st, 1, true, true, b);
    CHECK(o1.p == plan::prime); CHECK(o1.n == 1); CHECK(st.b_ok_fid == 1);
    outcome o2 = pair(st, 2, true, true, b);
    CHECK(o2.p == plan::chain); CHECK(o2.n == 3); CHECK(st.b_ok_fid == 2);
    outcome o3 = pair(st, 3, true, true, b);
    CHECK(o3.p == plan::chain); CHECK(o3.n == 3);
    CHECK(st.pairs4 == 2); CHECK(st.pairs2 == 1); CHECK(st.primes == 1);
}

static void test_failed_prime_is_not_trusted()
{
    // The AC2 bug: a failed prime used to mark B's history as good.
    state st; script b{ { false } };   // the prime fails
    outcome o1 = pair(st, 1, true, true, b);
    CHECK(o1.p == plan::prime); CHECK(o1.n == 1);
    CHECK(st.b_ok_fid == 0); CHECK(st.prime_fails == 1); CHECK(st.primes == 0);
    outcome o2 = pair(st, 2, true, true, b);   // must prime again, never chain
    CHECK(o2.p == plan::prime); CHECK(!o2.b_called_q); CHECK(o2.n == 1);
    CHECK(st.b_ok_fid == 2);
    outcome o3 = pair(st, 3, true, true, b);
    CHECK(o3.p == plan::chain); CHECK(o3.n == 3);
}

static void test_partial_chain_failure_composes_2x_and_reprimes()
{
    // 1/4 succeeds, 3/4 fails: the pair is 2x, nothing of the 4x is composed,
    // and B's history (now ending at the 1/2 frame) is not trusted.
    state st; script b{ { true /*prime*/, true /*q*/, false /*t*/ } };
    pair(st, 1, true, true, b);
    outcome o2 = pair(st, 2, true, true, b);
    CHECK(o2.p == plan::chain); CHECK(o2.b_called_q); CHECK(o2.b_called_t);
    CHECK(o2.n == 1); CHECK(st.partials == 1); CHECK(st.b_ok_fid == 0);
    outcome o3 = pair(st, 3, true, true, b);
    CHECK(o3.p == plan::prime); CHECK(o3.n == 1);
    outcome o4 = pair(st, 4, true, true, b);
    CHECK(o4.p == plan::chain); CHECK(o4.n == 3);
}

static void test_quarter_failure_skips_three_quarter()
{
    state st; script b{ { true, false /*q fails*/ } };
    pair(st, 1, true, true, b);
    outcome o2 = pair(st, 2, true, true, b);
    CHECK(o2.b_called_q); CHECK(!o2.b_called_t); CHECK(o2.n == 1);
    CHECK(st.partials == 0); CHECK(st.b_ok_fid == 0);
}

static void test_skipped_frames_reprime()
{
    // A skipped a frame (its ID jumped): B's previous input is two real
    // frames back, so B must be primed, not chained.
    state st; script b;
    pair(st, 1, true, true, b);
    pair(st, 2, true, true, b);
    outcome o = pair(st, 4, true, true, b);    // 3 never reached B
    CHECK(o.p == plan::prime); CHECK(o.n == 1);
    outcome o2 = pair(st, 5, true, true, b);
    CHECK(o2.p == plan::chain); CHECK(o2.n == 3);
    // A large jump (a reset by +2 elsewhere) behaves the same.
    outcome o3 = pair(st, 9, true, true, b);
    CHECK(o3.p == plan::prime);
}

static void test_toggle_off_and_on_reprimes()
{
    state st; script b;
    pair(st, 1, true, true, b);
    pair(st, 2, true, true, b);
    outcome off = pair(st, 3, false, true, b);   // the panel box unticked
    CHECK(off.p == plan::off); CHECK(off.n == 1); CHECK(!off.b_called_q && !off.b_called_prime);
    CHECK(st.b_ok_fid == 0);
    outcome on = pair(st, 4, true, true, b);     // ticked again: B missed frame 3
    CHECK(on.p == plan::prime);
    outcome on2 = pair(st, 5, true, true, b);
    CHECK(on2.p == plan::chain); CHECK(on2.n == 3);
}

static void test_unusable_falls_back_to_2x()
{
    // B switched off for the run (three errors) or the extra buffers missing.
    state st; script b;
    pair(st, 1, true, true, b);
    pair(st, 2, true, true, b);
    outcome o = pair(st, 3, true, false, b);
    CHECK(o.p == plan::off); CHECK(o.n == 1); CHECK(!o.b_called_q && !o.b_called_prime);
    CHECK(st.b_ok_fid == 0);
    for (uint64_t f = 4; f < 10; ++f) { outcome x = pair(st, f, true, false, b); CHECK(x.n == 1); }
}

static void test_counts_add_up()
{
    state st; script b{ { true, true, true, true, false, true, true, true } };
    unsigned frames = 0;
    for (uint64_t f = 1; f <= 6; ++f) frames += pair(st, f, true, true, b).n;
    CHECK(st.pairs4 + st.pairs2 == 6);
    CHECK(frames == st.pairs4 * 3u + st.pairs2);
}

static void test_frame_dt()
{
    const uint64_t qpf = 10000000ull;   // 10 MHz, as Windows reports
    bool v = false;
    float d = frame_dt_ms(1000000ull, 1250000ull, qpf, 16.6667f, &v);   // 25 ms
    CHECK(v); CHECK(std::fabs(d - 25.0f) < 0.01f);
    d = frame_dt_ms(0, 1250000ull, qpf, 25.0f, &v);                       // no previous stamp
    CHECK(!v); CHECK(d == 25.0f);
    d = frame_dt_ms(1250000ull, 1250000ull, qpf, 25.0f, &v);              // not increasing
    CHECK(!v); CHECK(d == 25.0f);
    d = frame_dt_ms(1000000ull, 1010000ull, qpf, 25.0f, &v);              // 1 ms: too short
    CHECK(!v); CHECK(d == 25.0f);
    d = frame_dt_ms(1000000ull, 4000000ull, qpf, 25.0f, &v);              // 300 ms: a load hitch
    CHECK(!v); CHECK(d == 25.0f);
    d = frame_dt_ms(0, 0, qpf, 0.0f, &v);                                 // no history at all
    CHECK(!v); CHECK(std::fabs(d - 16.6667f) < 0.01f);
    d = frame_dt_ms(5, 4, qpf, 500.0f, &v);                               // a bad last value is not kept
    CHECK(!v); CHECK(std::fabs(d - 16.6667f) < 0.01f);
    d = frame_dt_ms(1000000ull, 1020000ull, 0, 25.0f, &v);                // no frequency
    CHECK(!v); CHECK(d == 25.0f);
}

static void test_dt_reuse_causes()
{
    // AC4: every reuse of the last value has exactly one cause.
    CHECK(classify_dt(false, 100, 200, true) == dt_reuse::sequence_break);   // even with good stamps
    CHECK(classify_dt(false, 0, 0, false) == dt_reuse::sequence_break);
    CHECK(classify_dt(true, 0, 200, false) == dt_reuse::missing_stamp);
    CHECK(classify_dt(true, 100, 0, false) == dt_reuse::missing_stamp);
    CHECK(classify_dt(true, 100, 200, false) == dt_reuse::invalid_interval);
    CHECK(classify_dt(true, 100, 200, true) == dt_reuse::none);
    // Driven through frame_dt_ms as gpu1_context does: a skipped frame passes
    // 0 as the previous stamp and is counted as a break, not as missing.
    bool v = true;
    (void)frame_dt_ms(0, 250000, 10000000, 25.0f, &v);
    CHECK(!v); CHECK(classify_dt(false, 0, 250000, v) == dt_reuse::sequence_break);
}

int main()
{
    test_dt_reuse_causes();
    test_first_pair_primes_then_chains();
    test_failed_prime_is_not_trusted();
    test_partial_chain_failure_composes_2x_and_reprimes();
    test_quarter_failure_skips_three_quarter();
    test_skipped_frames_reprime();
    test_toggle_off_and_on_reprimes();
    test_unusable_falls_back_to_2x();
    test_counts_add_up();
    test_frame_dt();
    std::printf("fsr_chain_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
