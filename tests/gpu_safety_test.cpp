// SF1 regression checks: failed queue waits, failed signals, shutdown
// timeouts and device removal, as decided by src/gpu_safety.hpp.
// Build: g++ -std=c++17 -I src tests/gpu_safety_test.cpp
#include "gpu_safety.hpp"
#include <cstdio>

using namespace mgpu::gpusafe;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    const int S_OK_ = 0, E_FAIL_ = (int)0x80004005u, DEV_REMOVED = (int)0x887A0005u, DEV_HUNG = (int)0x887A0006u;

    // ---- failed queue waits: the gated list is not submitted
    CHECK(gated_submit(S_OK_) == submit::go);
    CHECK(gated_submit(1) == submit::go);                 // S_FALSE is success
    CHECK(gated_submit(E_FAIL_) == submit::drop_frame);
    CHECK(gated_submit(DEV_REMOVED) == submit::drop_frame);
    CHECK(gated_submit(DEV_HUNG) == submit::drop_frame);

    // ---- failed signals: hold, never reset the allocator blind
    CHECK(on_signal(S_OK_) == after_signal::go);
    CHECK(on_signal(E_FAIL_) == after_signal::hold);
    CHECK(on_signal(DEV_REMOVED) == after_signal::hold);

    // ---- verdict table
    CHECK(verdict(true,  false) == teardown::free_all);
    CHECK(verdict(true,  true)  == teardown::free_all);
    CHECK(verdict(false, true)  == teardown::free_all);   // removed: nothing runs any more
    CHECK(verdict(false, false) == teardown::keep_all);   // the unsafe case: keep
    CHECK(may_close_event(true, false));
    CHECK(may_close_event(false, true));
    CHECK(!may_close_event(false, false));

    // ---- extension
    CHECK(extension_ms(true, false, 8000) == 0u);
    CHECK(extension_ms(false, true, 8000) == 0u);
    CHECK(extension_ms(false, false, 8000) == 8000u);

    // ---- shutdown: work already done -> free, no wait
    {
        sim_fence f; f.completed = 10; f.target = 10;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::free_all); CHECK(r.close_ok); CHECK(r.waited_ms == 0u);
    }
    // ---- shutdown: finishes inside the first wait
    {
        sim_fence f; f.completed = 9; f.target = 10; f.done_at_ms = 30;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::free_all); CHECK(r.close_ok);
    }
    // ---- shutdown: slow, finishes inside the extension (was: freed under the GPU at 5 s)
    {
        sim_fence f; f.completed = 9; f.target = 10; f.done_at_ms = 7000;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::free_all); CHECK(r.close_ok); CHECK(r.waited_ms == 5000u);
    }
    // ---- shutdown timeout: never finishes, device alive -> KEEP, event not closed
    {
        sim_fence f; f.completed = 9; f.target = 10; f.done_at_ms = 0xFFFFFFFFu;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::keep_all); CHECK(!r.close_ok); CHECK(r.waited_ms == 13000u);
    }
    // ---- work signals a LOWER value than asked (failed per-frame signal) -> keep
    {
        sim_fence f; f.completed = 8; f.target = 9; f.done_at_ms = 10;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::keep_all); CHECK(!r.close_ok);
    }
    // ---- device removal: no extension, free, event may close
    {
        sim_fence f; f.completed = 9; f.target = 10; f.done_at_ms = 0xFFFFFFFFu; f.removed = true;
        const drain_result r = sim_drain(f, 10, 5000, 8000);
        CHECK(r.what == teardown::free_all); CHECK(r.close_ok); CHECK(r.waited_ms == 0u);
        CHECK(f.completed == ~0ull);
    }

    // ======================= SF2 =======================
    const queue_state DONE{ false, true, false }, WAITED_DONE{ true, true, false },
                      TIMED_OUT{ true, false, false }, NOT_WAITED{ false, false, false },
                      REMOVED{ true, false, true };

    // ---- repeated shutdown timeouts: the drain stays on, every launch waits (bounded) and keeps
    {
        bool drain = true;
        for (int launch = 0; launch < 5; ++launch)
        {
            sim_fence f; f.completed = 9; f.target = 10; f.done_at_ms = 0xFFFFFFFFu;   // the game's queue never finishes
            const unsigned w = g0_wait_ms(drain, f.completed >= 10);
            CHECK(w == 2000u);                                             // bounded, every time
            const drain_result r = sim_drain(f, 10, w, 0);
            CHECK(r.what == teardown::keep_all); CHECK(!r.close_ok);
            drain = next_drain_enabled(drain, true);
            CHECK(drain);                                                  // never switched off by a timeout
        }
    }
    // ---- disabled drain (DrainGpu0=0): no wait - but never a blind free
    CHECK(g0_wait_ms(false, false) == 0u);
    CHECK(g0_wait_ms(false, true) == 0u);
    CHECK(g0_wait_ms(true, true) == 0u);
    CHECK(g0_wait_ms(true, false) == 2000u);
    CHECK(!proven(NOT_WAITED));                                            // off + not done -> keep
    CHECK(!may_free(group::g0_transfer, NOT_WAITED, DONE, DONE, DONE));
    CHECK(may_free(group::g0_transfer, DONE, DONE, DONE, DONE));           // off + already done -> free
    CHECK(!next_drain_enabled(false, true));                               // and a setting of 0 stays 0
    // ---- failed fence signalling: a trailing Signal that failed proves nothing
    {
        const queue_state sig_failed{ false, false, false };
        CHECK(!proven(sig_failed));
        CHECK(!may_free(group::g1_all, DONE, DONE, sig_failed, DONE));
        CHECK(!may_free(group::g1_all, DONE, DONE, DONE, sig_failed));
        const queue_state sig_failed_removed{ false, false, true };
        CHECK(proven(sig_failed_removed));                                 // unless the device is gone
        CHECK(on_signal(DEV_REMOVED) == after_signal::hold);
    }
    // ---- the shared fence: a value read AFTER the CPU-side advance is not evidence
    {
        CHECK(fence_is_evidence(false));
        CHECK(!fence_is_evidence(true));
        sim_fence f; f.completed = 7; f.target = 7; f.done_at_ms = 0xFFFFFFFFu;   // game's queue stuck at 7
        const bool before = sim_wait(f, 9, 0, 0);       // read first (SF2 order): not reached
        f.completed = 12;                               // then the CPU advances the shared fence (R28-B)
        const bool after = sim_wait(f, 9, 0, 0);        // a late read would say "reached"
        CHECK(!before); CHECK(after);
        CHECK(!(after && fence_is_evidence(true)));     // ... and must not be believed
    }
    // ---- shared-resource lifetime: each group waits for every queue that may touch it
    CHECK(!may_free(group::g0_transfer, TIMED_OUT, DONE, DONE, DONE));     // game queue still writing gxfer
    CHECK(!may_free(group::g0_transfer, DONE, TIMED_OUT, DONE, DONE));     // staging queue still writing gxfer
    CHECK(!may_free(group::g0_staging, TIMED_OUT, WAITED_DONE, DONE, DONE)); // game queue still writing gstage
    CHECK(may_free(group::g0_staging, WAITED_DONE, WAITED_DONE, TIMED_OUT, TIMED_OUT)); // GPU 1 is not its owner
    CHECK(may_free(group::g1_all, TIMED_OUT, TIMED_OUT, WAITED_DONE, DONE));             // nor GPU 0 of GPU 1's
    CHECK(!may_free(group::g1_all, DONE, DONE, WAITED_DONE, TIMED_OUT));
    CHECK(may_free(group::g0_transfer, REMOVED, REMOVED, DONE, DONE));     // removed GPU 0 runs nothing
    CHECK(may_free(group::g1_all, DONE, DONE, REMOVED, REMOVED));

    // ---- removal is read BEFORE completion (a removed device's fence reads UINT64_MAX)
    CHECK(fence_now(~0ull, 10, false) == fence_read::removed);   // sentinel, even with the device call saying OK
    CHECK(fence_now(~0ull, ~0ull, false) == fence_read::removed);
    CHECK(fence_now(12, 10, true) == fence_read::removed);       // device says removed: not "finished"
    CHECK(fence_now(10, 10, false) == fence_read::reached);
    CHECK(fence_now(9, 10, false) == fence_read::pending);
    CHECK(fence_now(0, 0, false) == fence_read::reached);
    // the verdict still frees in both cases - only the label differs
    CHECK(may_free(group::g0_transfer, queue_state{ false, false, true }, DONE, DONE, DONE));

    std::printf("gpu_safety_test: %d/%d passed\n", g_n - g_fail, g_n);
    return g_fail == 0 ? 0 : 1;
}
