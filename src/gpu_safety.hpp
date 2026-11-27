// gpu_safety.hpp - SF1. The decisions behind the bridge's GPU error paths,
// kept free of D3D12 so tests/gpu_safety_test.cpp can drive them.
//
// Three rules, one principle ("never free or reuse what the GPU may still
// touch; when that cannot be established, keep it"):
//   1. A submission gated on a queue Wait is NOT made when the Wait failed:
//      the frame is dropped (counted) instead of running unordered.
//   2. A fence Signal that failed leaves the list's completion unknowable:
//      its allocator must not be reset - the stream holds.
//   3. Teardown frees GPU objects only when completion was observed or the
//      device is gone (a removed device runs nothing). Otherwise they are
//      kept (leaked) on purpose - recoverable, unlike freeing under the GPU.
//      An event handle registered with SetEventOnCompletion is closed only
//      under the same condition: the driver may still set it later, and a
//      closed handle value can be reused by something else.
#pragma once

namespace mgpu
{
namespace gpusafe
{
// HRESULT as a 32-bit int so the header needs no Windows headers (long is 64-bit on
// Linux, where the tests run; HRESULT is 32-bit everywhere).
inline bool hr_failed(int hr) { return hr < 0; }

// Rule 1.
enum class submit { go, drop_frame };
inline submit gated_submit(int wait_hr) { return hr_failed(wait_hr) ? submit::drop_frame : submit::go; }

// Rule 2. After a per-frame Signal: go on, or hold until the fence passes
// (it never will on a failed Signal unless something else signals past it).
enum class after_signal { go, hold };
inline after_signal on_signal(int signal_hr) { return hr_failed(signal_hr) ? after_signal::hold : after_signal::go; }

// Rule 3.
enum class teardown { free_all, keep_all };
inline teardown verdict(bool drained, bool device_removed)
{
    return (drained || device_removed) ? teardown::free_all : teardown::keep_all;
}
inline bool may_close_event(bool signalled, bool device_removed) { return signalled || device_removed; }

// Bounded drain with one extension: the first wait, then `ext_ms` more unless
// it already finished or the device is gone (then waiting is pointless).
inline unsigned extension_ms(bool first_ok, bool device_removed, unsigned ext_ms)
{
    return (first_ok || device_removed) ? 0u : ext_ms;
}

// ---- SF2: READING A FENCE AT TEARDOWN ----
// A removed device's fences read UINT64_MAX - which is >= any target, so a
// plain "completed >= target" calls removal "finished". Removal is checked
// FIRST: the free/keep answer is the same, but the log must say which.
enum class fence_read { reached, removed, pending };
constexpr unsigned long long FENCE_REMOVED_VALUE = ~0ull;   // UINT64_MAX
inline fence_read fence_now(unsigned long long completed, unsigned long long target, bool device_removed)
{
    if (device_removed || completed == FENCE_REMOVED_VALUE) return fence_read::removed;
    return completed >= target ? fence_read::reached : fence_read::pending;
}

// ---- SF2: GPU 0 TEARDOWN AND RESOURCE OWNERSHIP ----
// What teardown learnt about one queue.
struct queue_state
{
    bool waited;    // a bounded wait was made (false: no wait - already done, or the drain is off)
    bool drained;   // its completion was OBSERVED
    bool removed;   // its device is gone
};
inline bool proven(const queue_state &q) { return q.drained || q.removed; }

// GPU 0's game-queue wait: 2 s, or none when it already passed or the drain
// is off (DrainGpu0=0 skips the WAIT, never the check).
inline unsigned g0_wait_ms(bool drain_enabled, bool done_now)
{
    return (!drain_enabled || done_now) ? 0u : 2000u;
}

// A timeout never changes the setting (R275 used to write DrainGpu0=0, after
// which every launch freed with no check).
inline bool next_drain_enabled(bool current, bool /*timed_out*/) { return current; }

// The shared fence's value is evidence of GPU 0's completion only if it was
// read BEFORE the add-on advanced that same fence from the CPU (R28-B).
inline bool fence_is_evidence(bool cpu_advanced_before_read) { return !cpu_advanced_before_read; }

// Who may touch what, and so what each resource waits for:
//   GPU 0 transfer buffer (gxfer), its heap, the shared fence - the GAME's
//     queue (producer copies) and GPU 0's staging copy queue (writes gxfer).
//   GPU 0 staging buffer (gstage) and staging queue objects - the staging
//     copy queue (reads gstage), the game's queue (writes it).
//   GPU 1 transfer view (nxfer), heap view, textures, NGX features - GPU 1's
//     copy queue and NR queue.
enum class group { g0_transfer, g0_staging, g1_all };
inline bool may_free(group g, const queue_state &game_q, const queue_state &stage_q,
                     const queue_state &g1_copy, const queue_state &g1_nr)
{
    switch (g)
    {
    case group::g0_transfer: return proven(game_q) && proven(stage_q);
    case group::g0_staging:  return proven(game_q) && proven(stage_q);
    case group::g1_all:      return proven(g1_copy) && proven(g1_nr);
    }
    return false;
}

// A fake fence + queue for the tests and for reasoning: work completes at
// `done_at` (ms from the start of the wait), or never (UINT_MAX-ish).
struct sim_fence
{
    unsigned long long completed = 0;
    unsigned long long target = 0;     // the value the work signals when done
    unsigned done_at_ms = 0;           // when it gets there
    bool removed = false;              // device removed: completed reads ~0ull
};

// What a bounded wait of `ms` (starting at t0_ms) observes.
inline bool sim_wait(sim_fence &f, unsigned long long value, unsigned t0_ms, unsigned ms)
{
    if (f.removed) { f.completed = ~0ull; return true; }   // a removed device "completes" everything
    if (f.completed >= value) return true;
    if (f.done_at_ms <= t0_ms + ms && f.target >= value) { f.completed = f.target; return true; }
    return false;
}

// The full teardown drain as the bridge runs it: first wait, optional
// extension, removal check, verdict. `close_ok` reports whether the event used
// may be closed.
struct drain_result { teardown what; bool close_ok; unsigned waited_ms; };
inline drain_result sim_drain(sim_fence &f, unsigned long long value, unsigned first_ms, unsigned ext_ms)
{
    bool ok = sim_wait(f, value, 0u, first_ms);
    unsigned waited = ok ? 0u : first_ms;
    const unsigned ext = extension_ms(ok, f.removed, ext_ms);
    if (ext != 0u)
    {
        ok = sim_wait(f, value, first_ms, ext);
        waited += ok ? 0u : ext;
    }
    const bool removed = f.removed;
    return { verdict(ok, removed), may_close_event(ok, removed), waited };
}
}
}
