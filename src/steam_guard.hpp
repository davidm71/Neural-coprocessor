// ---------------------------------------------------------------------------
// steam_guard.hpp - SO2: the decisions behind the Steam overlay guard (SO1),
// kept free of Windows so they can be tested anywhere (tests/steam_guard_test).
//
// crashlog.cpp gathers the facts inside the exception handler (no locks, no
// heap) and asks these functions what to do; the bridge thread later asks
// health_verdict() whether rendering came back.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mgpu
{
namespace steam_guard
{

// The fault seen in every overlay crash in crash.txt, Oct-Nov 2026: an access
// violation READING 0xFFFFFFFFFFFFFFFF at gameoverlayrenderer64.dll+0x9B700.
// The offset belongs to one Steam client build; a Steam update moves it, so it
// is reported, not required. The bad address and the read are required.
const uint32_t kAccessViolation = 0xC0000005u;
const uint64_t kKnownBadAddress = 0xFFFFFFFFFFFFFFFFull;
const uint64_t kKnownOffset     = 0x9B700ull;

enum class mode : int { off = 0, known = 1, broad = 2, legacy = 3 };   // SteamOverlayGuard=0/1/2/3

enum class sig : int
{
    known_offset = 1,   // the known fault at the known offset
    known_moved  = 2,   // the known fault at another offset (a different Steam build)
    broad        = 3,   // any access violation in the overlay (SteamOverlayGuard=2 only)
    legacy       = 4,   // any access violation in the overlay, SO1's checks only (SteamOverlayGuard=3)
    not_av       = -1,  // not an access violation
    not_overlay  = -2,  // not inside the overlay's code
    other_av     = -3,  // an access violation in the overlay, but not the known one
    noncontinuable = -4,
    guard_off    = -5,
};

struct fault
{
    uint32_t code;          // ExceptionCode
    uint32_t flags;         // ExceptionFlags (1 = EXCEPTION_NONCONTINUABLE)
    uint32_t nparams;       // NumberParameters
    uint64_t av_kind;       // ExceptionInformation[0]: 0 read, 1 write, 8 execute
    uint64_t av_addr;       // ExceptionInformation[1]
    uint64_t rip;           // where it happened
    uint64_t ov_base, ov_size;   // the overlay module's range (0 = not loaded)
};

inline bool in_range(uint64_t a, uint64_t base, uint64_t size)
{
    return base != 0 && size != 0 && a >= base && a - base < size;
}

inline sig classify(const fault &f, mode m)
{
    if (m == mode::off) return sig::guard_off;
    if (f.code != kAccessViolation) return sig::not_av;
    if ((f.flags & 1u) != 0) return sig::noncontinuable;
    if (!in_range(f.rip, f.ov_base, f.ov_size)) return sig::not_overlay;
    const bool known = f.nparams >= 2 && f.av_kind == 0 && f.av_addr == kKnownBadAddress;
    if (known && m != mode::legacy) return (f.rip - f.ov_base == kKnownOffset) ? sig::known_offset : sig::known_moved;
    if (m == mode::legacy) return sig::legacy;
    return (m == mode::broad) ? sig::broad : sig::other_av;
}

inline bool accepted(sig s) { return static_cast<int>(s) > 0; }

inline const char *sig_text(sig s)
{
    switch (s)
    {
    case sig::known_offset:   return "the known overlay fault (read of 0xFFFFFFFFFFFFFFFF) at the known offset";
    case sig::known_moved:    return "the known overlay fault (read of 0xFFFFFFFFFFFFFFFF) at a new offset - Steam was probably updated";
    case sig::broad:          return "an access violation in the overlay (SteamOverlayGuard=2 accepts any)";
    case sig::legacy:         return "an access violation in the overlay (SteamOverlayGuard=3: SO1's original guard, no return checks)";
    case sig::not_av:         return "not an access violation";
    case sig::not_overlay:    return "not in Steam's overlay";
    case sig::other_av:       return "an access violation in the overlay that is NOT the known one (SteamOverlayGuard=2 or 3 would accept it)";
    case sig::noncontinuable: return "marked non-continuable by Windows";
    case sig::guard_off:      return "the guard is off (SteamOverlayGuard=0)";
    }
    return "?";
}

// ---- Is `dest` a return address, i.e. right after a CALL instruction? ----
//
// `b` holds the 8 bytes before dest (b[7] is the byte at dest-1). Accepts
// E8 rel32, and FF /2 (call r/m64) in every addressing form, with an optional
// REX prefix. A return into the middle of anything else is not a call site,
// and resuming there would execute arbitrary bytes.
inline int modrm_len(uint8_t modrm, uint8_t sib, bool have_sib)
{
    const int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return 1;
    int n = 1;
    if (rm == 4)
    {
        if (!have_sib) return -1;
        n += 1;
        if (mod == 0 && (sib & 7) == 5) n += 4;
    }
    else if (mod == 0 && rm == 5) n += 4;   // RIP-relative
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n;
}

inline bool call_before(const uint8_t b[8])
{
    if (b[3] == 0xE8) return true;          // call rel32: E8 xx xx xx xx
    for (int len = 2; len <= 8; ++len)      // call r/m64, optionally REX-prefixed
    {
        int i = 8 - len;
        if (b[i] >= 0x40 && b[i] <= 0x4F) ++i;          // REX
        if (i > 6 || b[i] != 0xFF) continue;
        const uint8_t modrm = b[i + 1];
        if (((modrm >> 3) & 7) != 2) continue;          // /2 = CALL
        const bool have_sib = i + 2 < 8;
        const int ml = modrm_len(modrm, have_sib ? b[i + 2] : 0, have_sib);
        if (ml > 0 && i + 1 + ml == 8) return true;     // ends exactly at dest
    }
    return false;
}

// ---- Can execution resume at the caller the unwind found? ----
enum class dest : int
{
    ok = 0,
    none = 1,            // the walk did not leave the overlay
    in_overlay = 2,
    not_executable = 3,  // not committed executable memory
    not_call_site = 4,   // not right after a CALL
    misaligned = 5,      // RSP not 16-byte aligned after the return
    stack = 6,           // RSP left the thread's stack or went backwards
    no_unwind_info = 7,  // a non-leaf overlay frame without unwind data
    too_deep = 8,        // more than 64 overlay frames
    unreadable = 9,
    reenters = 10,       // further up, the stack is in the overlay again: it was a callback, not the entry
    not_present = 11,    // SO4: the caller called another COM method and the thread is not in a present
};

struct dest_facts
{
    uint64_t rip, rsp;
    bool walked_out;         // the walk ended outside the overlay
    bool in_overlay;
    bool executable;         // committed, PAGE_EXECUTE*
    bool readable_before;    // the 8 bytes before rip could be read
    uint8_t before[8];
    bool reenters;           // a look further up the stack found the overlay again
};

inline dest check_dest(const dest_facts &d)
{
    if (!d.walked_out || d.rip == 0) return dest::none;
    if (d.in_overlay) return dest::in_overlay;
    if (!d.executable) return dest::not_executable;
    if (!d.readable_before) return dest::unreadable;
    if (!call_before(d.before)) return dest::not_call_site;
    if ((d.rsp & 15u) != 0) return dest::misaligned;
    if (d.reenters) return dest::reenters;
    return dest::ok;
}

inline const char *dest_text(dest r)
{
    switch (r)
    {
    case dest::ok:             return "ok";
    case dest::none:           return "the unwind did not leave the overlay";
    case dest::in_overlay:     return "the return lands inside the overlay";
    case dest::not_executable: return "the return address is not executable code";
    case dest::not_call_site:  return "the return address does not follow a CALL";
    case dest::misaligned:     return "the stack would be misaligned after the return";
    case dest::stack:          return "the unwind left the thread's stack";
    case dest::no_unwind_info: return "an overlay frame has no unwind data";
    case dest::too_deep:       return "more than 64 overlay frames";
    case dest::unreadable:     return "the code before the return address could not be read";
    case dest::reenters:       return "the overlay is further up the stack too (a callback, not its outermost entry)";
    case dest::not_present:    return "the caller called another method (not Present) and the thread was not presenting - S_OK would be the wrong answer";
    }
    return "?";
}

// One unwind step: the stack only grows back up, and stays on the thread's stack.
inline bool stack_step_ok(uint64_t old_rsp, uint64_t new_rsp, uint64_t stack_lo, uint64_t stack_hi)
{
    return new_rsp > old_rsp && new_rsp >= stack_lo && new_rsp < stack_hi;
}

// ---- At most `N` step-overs in any `window_ms`, lock-free ----
//
// Each step-over claims one of N time slots, replacing one older than the
// window with a compare-exchange; if all N are inside the window, it is
// refused. Safe from any number of threads at once, inside an exception
// handler (no locks, no heap).
template <unsigned N>
struct rate_window
{
    std::atomic<uint64_t> t[N];
    rate_window() { for (unsigned i = 0; i < N; ++i) t[i].store(0, std::memory_order_relaxed); }

    bool take(uint64_t now_ms, uint64_t window_ms)
    {
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            unsigned oldest = 0;
            uint64_t ov = t[0].load(std::memory_order_acquire);
            for (unsigned i = 1; i < N; ++i)
            {
                const uint64_t v = t[i].load(std::memory_order_acquire);
                if (v < ov) { ov = v; oldest = i; }
            }
            if (ov != 0 && now_ms - ov < window_ms) return false;   // all N within the window
            if (t[oldest].compare_exchange_strong(ov, now_ms == 0 ? 1 : now_ms, std::memory_order_acq_rel))
                return true;
        }
        return false;   // heavy contention: refuse rather than spin
    }
    unsigned used(uint64_t now_ms, uint64_t window_ms) const
    {
        unsigned n = 0;
        for (unsigned i = 0; i < N; ++i)
        {
            const uint64_t v = t[i].load(std::memory_order_acquire);
            if (v != 0 && now_ms - v < window_ms) ++n;
        }
        return n;
    }
};

// ---- Did rendering come back after a step-over? ----
enum class health : int
{
    pending = 0,
    resumed = 1,        // the faulting thread drew again, frames flowing, no failures
    resumed_other = 2,  // frames flowing, but the faulting thread was not seen again
    degraded = 3,       // frames flowing but well below the rate before, or a new fault
    failed = 4,         // no frames, a failed present, or the device was removed
};

struct health_facts
{
    uint64_t elapsed_ms;      // since the step-over
    bool same_thread_seen;    // the faulting thread reached a present again
    uint32_t presents_after;  // presents seen since (all sources)
    int32_t present_fail_hr;  // first failed HRESULT seen since (0 = none)
    int32_t device_hr;        // GetDeviceRemovedReason seen since (0 = S_OK)
    uint32_t new_faults;      // fatal first-chance exceptions since (not counting this one)
    double rate_before;       // presents/s in the second before (0 = unknown)
    double rate_after;        // presents/s in the second after the first second
};

inline health health_verdict(const health_facts &h, uint64_t settle_ms = 2000)
{
    if (h.present_fail_hr != 0 || h.device_hr != 0) return health::failed;
    if (h.elapsed_ms < settle_ms) return health::pending;
    if (h.presents_after == 0) return health::failed;
    if (h.new_faults > 0) return health::degraded;
    if (h.rate_before > 1.0 && h.rate_after < h.rate_before * 0.5) return health::degraded;
    return h.same_thread_seen ? health::resumed : health::resumed_other;
}

inline const char *health_text(health v)
{
    switch (v)
    {
    case health::pending:       return "pending";
    case health::resumed:       return "RESUMED - the faulting thread presented again and frames are flowing";
    case health::resumed_other: return "frames are flowing, but the faulting thread has not presented since";
    case health::degraded:      return "DEGRADED - frames are flowing, but slower than before or with new faults";
    case health::failed:        return "FAILED - no frames, a failed present or a removed device after the step-over";
    }
    return "?";
}

// ---- SO4: what the CALL before the return address says about the caller ----
//
// A CALL there and executable memory say only that the return is a call site.
// They do not say the call was IDXGISwapChain::Present, which is what makes
// "return S_OK" the right answer. Two pieces of evidence are gathered:
//  - the CALL's form: a COM call is call [reg+disp] with disp = vtable slot x 8;
//    Present is slot 8 (+0x40), Present1 slot 22 (+0xB0). Any other slot through
//    a plain register is another method. Direct, RIP-relative, register and
//    stack/frame-relative calls (hooks keep their "original" pointers there)
//    say nothing either way.
//  - the thread: the faulting thread passed ReShade's present event a moment
//    before (that event runs on the presenting thread, just before Present).
// Only CONTRARY evidence refuses (another method AND not presenting); missing
// evidence is reported, never treated as proof.
enum class call_kind : int { none = 0, direct = 1, rip_mem = 2, reg = 3, mem = 4 };
struct call_info { call_kind kind = call_kind::none; bool has_disp = false; bool frame_base = false; int64_t disp = 0; };

inline call_info decode_call(const uint8_t b[8])
{
    call_info ci{};
    if (b[3] == 0xE8) { ci.kind = call_kind::direct; return ci; }
    for (int len = 2; len <= 8; ++len)
    {
        int i = 8 - len;
        if (b[i] >= 0x40 && b[i] <= 0x4F) ++i;
        if (i > 6 || b[i] != 0xFF) continue;
        const uint8_t modrm = b[i + 1];
        if (((modrm >> 3) & 7) != 2) continue;
        const bool have_sib = i + 2 < 8;
        const int ml = modrm_len(modrm, have_sib ? b[i + 2] : 0, have_sib);
        if (ml <= 0 || i + 1 + ml != 8) continue;
        const int mod = modrm >> 6, rm = modrm & 7;
        if (mod == 3) { ci.kind = call_kind::reg; return ci; }
        if (mod == 0 && rm == 5) { ci.kind = call_kind::rip_mem; return ci; }
        ci.kind = call_kind::mem;
        ci.frame_base = (rm == 4) || (rm == 5);   // SIB (often rsp) or rbp: a pointer kept on the stack
        const int at = i + 2 + (rm == 4 ? 1 : 0);
        if (mod == 1) { ci.has_disp = true; ci.disp = (int8_t)b[at]; }
        else if (mod == 2)
        {
            ci.has_disp = true;
            ci.disp = (int32_t)((uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24));
        }
        return ci;
    }
    return ci;
}

enum class call_sem : int { present = 1, present1 = 2, unknown = 0, other_method = -1 };
inline call_sem call_semantic(const call_info &ci)
{
    if (ci.kind != call_kind::mem || ci.frame_base) return call_sem::unknown;
    const int64_t d = ci.has_disp ? ci.disp : 0;
    if (d == 0x40) return call_sem::present;
    if (d == 0xB0) return call_sem::present1;
    if (d >= 0 && d < 0x400 && (d % 8) == 0) return call_sem::other_method;   // a vtable slot that is not Present
    return call_sem::unknown;
}
inline const char *sem_text(call_sem s)
{
    switch (s)
    {
    case call_sem::present:      return "vtable +0x40 (IDXGISwapChain::Present)";
    case call_sem::present1:     return "vtable +0xB0 (IDXGISwapChain1::Present1)";
    case call_sem::other_method: return "a vtable slot that is NOT Present";
    case call_sem::unknown:      return "a call form that does not name the method";
    }
    return "?";
}
// 2 = both pieces agree, 1 = one of them, 0 = none (S_OK unverified), -1 = contrary.
inline int present_evidence(call_sem s, bool thread_presenting)
{
    if (s == call_sem::other_method && !thread_presenting) return -1;
    const int a = (s == call_sem::present || s == call_sem::present1) ? 1 : 0;
    return a + (thread_presenting ? 1 : 0);
}

// ---- SO4: the diagnostic ring between exception handlers and the bridge thread ----
//
// Any number of writers (exception handlers, any thread, at once), one reader.
// Each slot carries its sequence and a state - FREE, WRITING, READY, READING -
// in one atomic word, so a record is only written in a slot nobody is reading
// and only read from a slot nobody is writing. A writer never waits: if its slot
// is being written or read, the record is dropped (counted); if it holds an
// unread older record, that one is overwritten (counted). The reader skips what
// was overwritten or dropped and never reads a half-written record.
template <class T, unsigned N>
struct record_ring
{
    static_assert(N >= 2, "ring too small");
    enum : uint64_t { FREE = 0, WRITING = 1, READY = 2, READING = 3 };
    struct slot { std::atomic<uint64_t> st; T v; };
    slot s[N];
    std::atomic<uint64_t> w{0};
    std::atomic<uint64_t> dropped{0}, overwritten{0};
    uint64_t r = 0, lost = 0;   // reader only
    unsigned stall = 0;         // reader only
    record_ring() { for (unsigned i = 0; i < N; ++i) s[i].st.store(FREE, std::memory_order_relaxed); }
    static uint64_t pack(uint64_t seq, uint64_t state) { return (seq << 2) | state; }

    bool push(const T &v)   // any thread; lock-free, never blocks
    {
        const uint64_t idx = w.fetch_add(1, std::memory_order_acq_rel);
        slot &e = s[idx % N];
        uint64_t cur = e.st.load(std::memory_order_acquire);
        const uint64_t st = cur & 3u, seq = cur >> 2;
        const bool newer_there = (st != FREE || seq != 0) && seq > idx;   // a later lap got here first
        if (st == WRITING || st == READING || newer_there) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        if (!e.st.compare_exchange_strong(cur, pack(idx, WRITING), std::memory_order_acq_rel))
        { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        if (st == READY) overwritten.fetch_add(1, std::memory_order_relaxed);
        e.v = v;
        e.st.store(pack(idx, READY), std::memory_order_release);
        return true;
    }

    template <class F> unsigned drain(F f)   // the one reader
    {
        unsigned got = 0;
        const uint64_t wv = w.load(std::memory_order_acquire);
        if (wv > r + N) { lost += wv - N - r; r = wv - N; stall = 0; }   // lapped: those slots were reused
        for (int guard = 0; r < wv && guard < (int)(4 * N); ++guard)
        {
            slot &e = s[r % N];
            uint64_t cur = e.st.load(std::memory_order_acquire);
            const uint64_t st = cur & 3u, seq = cur >> 2;
            if (st == READY && seq == r)
            {
                if (!e.st.compare_exchange_strong(cur, pack(r, READING), std::memory_order_acq_rel)) continue;
                T copy = e.v;
                e.st.store(pack(r, FREE), std::memory_order_release);
                f(copy);
                ++got; ++r; stall = 0;
                continue;
            }
            if (seq > r) { ++lost; ++r; stall = 0; continue; }   // a later lap overwrote or dropped it
            // Record r not there yet: its writer is mid-write or gave up. Wait
            // two drains, then move on (a late arrival is overwritten next lap).
            if (++stall > 2) { ++lost; ++r; stall = 0; continue; }
            break;
        }
        return got;
    }
};

// ---- SO4: one health assessment per recovery ----
//
// Each step-over gets its own slot with its own counters, so a second fault a
// moment later cannot erase the first one's verdict. Claimed in the exception
// handler (lock-free; a full table drops the assessment, counted, never
// overwrites), fed from every present, released by the bridge thread once
// judged.
template <unsigned N>
struct assessments
{
    struct slot
    {
        std::atomic<long> n;          // recovery number; 0 free, -1 being set up
        std::atomic<uint32_t> tid;
        std::atomic<uint64_t> ms;
        std::atomic<long> same, presents, fail, dev, fatal0;
    };
    slot s[N];
    std::atomic<int> active{0};
    std::atomic<uint64_t> dropped{0};
    assessments()
    {
        for (unsigned i = 0; i < N; ++i)
        {
            s[i].n.store(0); s[i].tid.store(0); s[i].ms.store(0);
            s[i].same.store(0); s[i].presents.store(0); s[i].fail.store(0); s[i].dev.store(0); s[i].fatal0.store(0);
        }
    }
    int claim(long n, uint32_t tid, uint64_t ms, long fatal_now)
    {
        for (unsigned i = 0; i < N; ++i)
        {
            long z = 0;
            if (!s[i].n.compare_exchange_strong(z, -1, std::memory_order_acq_rel)) continue;
            s[i].tid.store(tid, std::memory_order_relaxed);
            s[i].ms.store(ms, std::memory_order_relaxed);
            s[i].same.store(0, std::memory_order_relaxed);
            s[i].presents.store(0, std::memory_order_relaxed);
            s[i].fail.store(0, std::memory_order_relaxed);
            s[i].dev.store(0, std::memory_order_relaxed);
            s[i].fatal0.store(fatal_now, std::memory_order_relaxed);
            s[i].n.store(n, std::memory_order_release);
            active.fetch_add(1, std::memory_order_acq_rel);
            return (int)i;
        }
        dropped.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    bool any() const { return active.load(std::memory_order_relaxed) > 0; }
    void note_present(uint32_t tid, long hr, uint64_t now, uint64_t window_ms)
    {
        for (unsigned i = 0; i < N; ++i)
        {
            if (s[i].n.load(std::memory_order_acquire) <= 0) continue;
            if (now - s[i].ms.load(std::memory_order_relaxed) > window_ms) continue;
            s[i].presents.fetch_add(1, std::memory_order_relaxed);
            if (hr < 0) { long z = 0; s[i].fail.compare_exchange_strong(z, hr); }
            if (tid == s[i].tid.load(std::memory_order_relaxed)) s[i].same.store(1, std::memory_order_relaxed);
        }
    }
    bool wants_device(uint64_t now, uint64_t window_ms) const
    {
        for (unsigned i = 0; i < N; ++i)
            if (s[i].n.load(std::memory_order_acquire) > 0 && s[i].dev.load(std::memory_order_relaxed) == 0 &&
                now - s[i].ms.load(std::memory_order_relaxed) <= window_ms)
                return true;
        return false;
    }
    void note_device(long hr, uint64_t now, uint64_t window_ms)
    {
        if (hr >= 0) return;
        for (unsigned i = 0; i < N; ++i)
            if (s[i].n.load(std::memory_order_acquire) > 0 && now - s[i].ms.load(std::memory_order_relaxed) <= window_ms)
            { long z = 0; s[i].dev.compare_exchange_strong(z, hr); }
    }
    void release(unsigned i)
    {
        if (s[i].n.exchange(0, std::memory_order_acq_rel) > 0) active.fetch_sub(1, std::memory_order_acq_rel);
    }
};

} // namespace steam_guard
} // namespace mgpu
