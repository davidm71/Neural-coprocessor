// tests/steam_guard_test.cpp - SO2: the Steam overlay guard's decisions.
//
// Portable (no Windows): build with any C++17 compiler, e.g.
//   g++ -std=c++17 -O1 -pthread -Isrc tests/steam_guard_test.cpp -o steam_guard_test && ./steam_guard_test
// or with CMake -DMGPU_BUILD_TESTS=ON (target mgpu_steam_guard_test).
#include "steam_guard.hpp"

#include <cstdio>
#include <thread>
#include <vector>

using namespace mgpu::steam_guard;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static fault base_fault()
{
    fault f{};
    f.code = kAccessViolation; f.flags = 0; f.nparams = 2;
    f.av_kind = 0; f.av_addr = kKnownBadAddress;
    f.ov_base = 0x7ff800000000ull; f.ov_size = 0x18A000ull;
    f.rip = f.ov_base + kKnownOffset;
    return f;
}

static void test_signature()
{
    fault f = base_fault();
    CHECK(classify(f, mode::known) == sig::known_offset);
    CHECK(accepted(classify(f, mode::known)));

    // ASLR: another base, same offset.
    f.ov_base = 0x7ffa12340000ull; f.rip = f.ov_base + kKnownOffset;
    CHECK(classify(f, mode::known) == sig::known_offset);

    // A Steam update moves the code: same fault, new offset - still the known fault.
    f.rip = f.ov_base + 0x9C120;
    CHECK(classify(f, mode::known) == sig::known_moved);
    CHECK(accepted(classify(f, mode::known)));

    // Unknown access violations inside the overlay are NOT stepped over by default.
    fault g = base_fault(); g.av_addr = 0x10;                   // null-ish read
    CHECK(classify(g, mode::known) == sig::other_av);
    CHECK(!accepted(classify(g, mode::known)));
    CHECK(classify(g, mode::broad) == sig::broad);              // ...only with SteamOverlayGuard=2
    CHECK(classify(g, mode::legacy) == sig::legacy);            // ...or 3 (SO1's original guard)
    CHECK(classify(base_fault(), mode::legacy) == sig::legacy); // 3 accepts the known fault too
    fault ol = base_fault(); ol.rip = 0x1000;
    CHECK(classify(ol, mode::legacy) == sig::not_overlay);      // ...but never outside the overlay
    fault nl = base_fault(); nl.flags = 1;
    CHECK(classify(nl, mode::legacy) == sig::noncontinuable);
    fault w = base_fault(); w.av_kind = 1;                      // a WRITE to -1
    CHECK(classify(w, mode::known) == sig::other_av);
    fault x = base_fault(); x.av_kind = 8;                      // DEP
    CHECK(!accepted(classify(x, mode::known)));
    fault np = base_fault(); np.nparams = 0;
    CHECK(classify(np, mode::known) == sig::other_av);

    // Outside the overlay: never.
    fault o = base_fault(); o.rip = o.ov_base + o.ov_size;      // first byte past the module
    CHECK(classify(o, mode::known) == sig::not_overlay);
    CHECK(classify(o, mode::broad) == sig::not_overlay);
    fault u = base_fault(); u.ov_base = 0; u.ov_size = 0;       // overlay not loaded
    CHECK(classify(u, mode::known) == sig::not_overlay);

    // Other exception codes, non-continuable, guard off.
    fault c = base_fault(); c.code = 0xC0000094u;
    CHECK(classify(c, mode::broad) == sig::not_av);
    fault n = base_fault(); n.flags = 1;
    CHECK(classify(n, mode::known) == sig::noncontinuable);
    CHECK(classify(base_fault(), mode::off) == sig::guard_off);
}

static void bytes(uint8_t out[8], std::initializer_list<int> tail)
{
    for (int i = 0; i < 8; ++i) out[i] = 0xCC;   // int3 padding
    int i = 8 - (int)tail.size();
    for (int v : tail) out[i++] = (uint8_t)v;
}

static void test_call_before()
{
    uint8_t b[8];
    bytes(b, {0xE8, 0x10, 0x20, 0x30, 0x40});              CHECK(call_before(b));   // call rel32
    bytes(b, {0xFF, 0x15, 0x01, 0x02, 0x03, 0x04});        CHECK(call_before(b));   // call [rip+disp32]
    bytes(b, {0xFF, 0xD0});                                CHECK(call_before(b));   // call rax
    bytes(b, {0x41, 0xFF, 0xD3});                          CHECK(call_before(b));   // call r11
    bytes(b, {0xFF, 0x50, 0x18});                          CHECK(call_before(b));   // call [rax+0x18] (vtable)
    bytes(b, {0x48, 0xFF, 0x50, 0x40});                    CHECK(call_before(b));   // REX.W call [rax+0x40]
    bytes(b, {0xFF, 0x90, 0x00, 0x01, 0x00, 0x00});        CHECK(call_before(b));   // call [rax+disp32]
    bytes(b, {0xFF, 0x14, 0x24});                          CHECK(call_before(b));   // call [rsp]
    bytes(b, {0xFF, 0x54, 0x24, 0x08});                    CHECK(call_before(b));   // call [rsp+8]
    bytes(b, {0x41, 0xFF, 0x94, 0x24, 0x00, 0x01, 0x00, 0x00}); CHECK(call_before(b)); // call [r12+disp32]

    bytes(b, {0x48, 0x89, 0xC1});                          CHECK(!call_before(b));  // mov rcx, rax
    bytes(b, {0xFF, 0xE0});                                CHECK(!call_before(b));  // jmp rax (/4)
    bytes(b, {0xFF, 0x25, 0x01, 0x02, 0x03, 0x04});        CHECK(!call_before(b));  // jmp [rip+disp32]
    bytes(b, {0xC3});                                      CHECK(!call_before(b));  // ret
    bytes(b, {0xCC, 0xCC});                                CHECK(!call_before(b));  // padding
    bytes(b, {0xFF, 0x50});                                CHECK(!call_before(b));  // truncated: needs a disp8
}

static dest_facts good_dest()
{
    dest_facts d{};
    d.rip = 0x7ff700001234ull; d.rsp = 0x000000a0c0ffe0ull;   // 16-aligned
    d.walked_out = true; d.in_overlay = false; d.executable = true; d.readable_before = true;
    bytes(d.before, {0xFF, 0x50, 0x40});                      // call [rax+0x40]: a vtable call to Present
    return d;
}

static void test_dest()
{
    CHECK(check_dest(good_dest()) == dest::ok);
    dest_facts d = good_dest(); d.walked_out = false;          CHECK(check_dest(d) == dest::none);
    d = good_dest(); d.rip = 0;                                CHECK(check_dest(d) == dest::none);
    d = good_dest(); d.in_overlay = true;                      CHECK(check_dest(d) == dest::in_overlay);
    d = good_dest(); d.executable = false;                     CHECK(check_dest(d) == dest::not_executable);
    d = good_dest(); d.readable_before = false;                CHECK(check_dest(d) == dest::unreadable);
    d = good_dest(); bytes(d.before, {0x90, 0x90});            CHECK(check_dest(d) == dest::not_call_site);
    d = good_dest(); d.rsp += 8;                               CHECK(check_dest(d) == dest::misaligned);
    d = good_dest(); d.reenters = true;                        CHECK(check_dest(d) == dest::reenters);

    CHECK(stack_step_ok(0x1000, 0x1040, 0x0, 0x10000));
    CHECK(!stack_step_ok(0x1040, 0x1040, 0x0, 0x10000));       // did not move
    CHECK(!stack_step_ok(0x1040, 0x1000, 0x0, 0x10000));       // went backwards
    CHECK(!stack_step_ok(0x1040, 0x10000, 0x0, 0x10000));      // left the stack
}

static void test_rate()
{
    rate_window<5> r;
    uint64_t t = 1000;
    for (int i = 0; i < 5; ++i) CHECK(r.take(t + i * 100, 60000));
    CHECK(!r.take(t + 600, 60000));                   // 6th inside the minute: refused
    CHECK(r.used(t + 600, 60000) == 5);
    CHECK(!r.take(t + 59999, 60000));                 // the first is still 59.999 s old
    CHECK(r.take(t + 60000, 60000));                  // the first has left the window
    CHECK(!r.take(t + 60001, 60000));
    CHECK(r.take(t + 200000, 60000));                 // long after: free again

    // Many threads at once: never more than 5 granted inside one window.
    for (int round = 0; round < 50; ++round)
    {
        rate_window<5> rr;
        std::atomic<int> granted{0};
        std::vector<std::thread> th;
        for (int k = 0; k < 16; ++k)
            th.emplace_back([&] { for (int j = 0; j < 20; ++j) if (rr.take(5000, 60000)) ++granted; });
        for (auto &x : th) x.join();
        CHECK(granted.load() == 5);
    }
}

static void test_health()
{
    health_facts h{};
    h.elapsed_ms = 500; h.presents_after = 30;
    CHECK(health_verdict(h) == health::pending);

    h.elapsed_ms = 2100; h.same_thread_seen = true; h.rate_before = 60; h.rate_after = 58;
    CHECK(health_verdict(h) == health::resumed);
    h.same_thread_seen = false;
    CHECK(health_verdict(h) == health::resumed_other);
    h.same_thread_seen = true; h.rate_after = 20;
    CHECK(health_verdict(h) == health::degraded);
    h.rate_after = 58; h.new_faults = 1;
    CHECK(health_verdict(h) == health::degraded);
    h.new_faults = 0; h.presents_after = 0;
    CHECK(health_verdict(h) == health::failed);

    health_facts f{}; f.elapsed_ms = 100; f.present_fail_hr = (int32_t)0x887A0005;   // DXGI_ERROR_DEVICE_REMOVED
    CHECK(health_verdict(f) == health::failed);                                     // at once, not after 2 s
    health_facts d{}; d.elapsed_ms = 100; d.device_hr = (int32_t)0x887A0006;        // DEVICE_HUNG
    CHECK(health_verdict(d) == health::failed);

    health_facts unknown_before{}; unknown_before.elapsed_ms = 2500; unknown_before.presents_after = 3;
    unknown_before.same_thread_seen = true;                                        // no baseline: not degraded
    CHECK(health_verdict(unknown_before) == health::resumed);
}

// ---- SO4 ----
static void test_call_semantics()
{
    uint8_t b[8];
    bytes(b, {0xFF, 0x50, 0x40});                      CHECK(call_semantic(decode_call(b)) == call_sem::present);
    bytes(b, {0x48, 0xFF, 0x50, 0x40});                CHECK(call_semantic(decode_call(b)) == call_sem::present);
    bytes(b, {0xFF, 0x90, 0xB0, 0x00, 0x00, 0x00});    CHECK(call_semantic(decode_call(b)) == call_sem::present1);
    bytes(b, {0x41, 0xFF, 0x51, 0x40});                CHECK(call_semantic(decode_call(b)) == call_sem::present);   // call [r9+0x40]
    bytes(b, {0xFF, 0x50, 0x68});                      CHECK(call_semantic(decode_call(b)) == call_sem::other_method); // ResizeBuffers
    bytes(b, {0xFF, 0x10});                            CHECK(call_semantic(decode_call(b)) == call_sem::other_method); // [rax]: slot 0
    bytes(b, {0xFF, 0x54, 0x24, 0x08});                CHECK(call_semantic(decode_call(b)) == call_sem::unknown);      // [rsp+8]
    bytes(b, {0xFF, 0x55, 0x40});                      CHECK(call_semantic(decode_call(b)) == call_sem::unknown);      // [rbp+0x40]
    bytes(b, {0xFF, 0x15, 0x01, 0x02, 0x03, 0x04});    CHECK(call_semantic(decode_call(b)) == call_sem::unknown);      // [rip+x]
    bytes(b, {0xE8, 0x10, 0x20, 0x30, 0x40});          CHECK(call_semantic(decode_call(b)) == call_sem::unknown);
    bytes(b, {0x41, 0xFF, 0xD3});                      CHECK(call_semantic(decode_call(b)) == call_sem::unknown);
    bytes(b, {0xFF, 0x50, 0xF0});                      CHECK(call_semantic(decode_call(b)) == call_sem::unknown);      // negative disp
    bytes(b, {0x90, 0x90});                            CHECK(decode_call(b).kind == call_kind::none);

    CHECK(present_evidence(call_sem::present, true) == 2);
    CHECK(present_evidence(call_sem::present, false) == 1);
    CHECK(present_evidence(call_sem::unknown, true) == 1);
    CHECK(present_evidence(call_sem::unknown, false) == 0);          // unverified: reported, not refused
    CHECK(present_evidence(call_sem::other_method, true) == 1);      // presenting thread outweighs the slot
    CHECK(present_evidence(call_sem::other_method, false) == -1);    // contrary: refused
}

struct rec { uint64_t a, b; uint64_t pad[14]; };
static rec make_rec(uint64_t v) { rec r{}; r.a = v; r.b = ~v; for (auto &p : r.pad) p = v * 2654435761ull; return r; }
static bool sound(const rec &r) { if (r.b != ~r.a) return false; for (auto p : r.pad) if (p != r.a * 2654435761ull) return false; return true; }

static void test_ring_wrap()
{
    record_ring<rec, 8> ring;
    for (uint64_t i = 0; i < 8; ++i) CHECK(ring.push(make_rec(i)));
    std::vector<uint64_t> got;
    ring.drain([&](const rec &r) { CHECK(sound(r)); got.push_back(r.a); });
    CHECK(got.size() == 8 && got.front() == 0 && got.back() == 7);
    for (uint64_t i = 8; i < 16; ++i) CHECK(ring.push(make_rec(i)));   // second lap, after reading
    got.clear();
    ring.drain([&](const rec &r) { got.push_back(r.a); });
    CHECK(got.size() == 8 && got.front() == 8 && got.back() == 15);
    CHECK(ring.lost == 0 && ring.overwritten.load() == 0 && ring.dropped.load() == 0);

    // The reader falls 92 behind: the newest 8 survive, in order, and the rest are counted.
    for (uint64_t i = 16; i < 116; ++i) ring.push(make_rec(i));
    got.clear();
    ring.drain([&](const rec &r) { CHECK(sound(r)); got.push_back(r.a); });
    CHECK(got.size() == 8 && got.front() == 108 && got.back() == 115);
    CHECK(ring.lost == 92);
    CHECK(ring.overwritten.load() == 92);
    for (size_t i = 1; i < got.size(); ++i) CHECK(got[i] == got[i - 1] + 1);
}

static void test_ring_concurrent()
{
    for (int round = 0; round < 6; ++round)
    {
        record_ring<rec, 32> ring;
        std::atomic<bool> done{false};
        std::atomic<uint64_t> pushed{0}, accepted{0};
        uint64_t delivered = 0, last = 0; bool order_ok = true, torn = false;
        std::thread reader([&] {
            for (;;)
            {
                const bool fin = done.load();
                ring.drain([&](const rec &r) {
                    if (!sound(r)) torn = true;
                    if (delivered && r.a <= last) order_ok = false;   // sequence numbers only rise
                    last = r.a; ++delivered;
                });
                if (fin)
                {
                    for (int k = 0; k < 64 && ring.r < ring.w.load(); ++k)
                        ring.drain([&](const rec &r) { if (!sound(r)) torn = true; ++delivered; });
                    break;
                }
                std::this_thread::yield();
            }
        });
        std::vector<std::thread> wr;
        for (int t = 0; t < 12; ++t)
            wr.emplace_back([&, t] {
                for (int i = 0; i < 4000; ++i)
                {
                    // the record carries its own ring index, so order can be checked
                    const uint64_t idx = ring.w.load();   // a hint only; the ring assigns the real index
                    (void)idx; (void)t;
                    const bool ok = ring.push(make_rec(ring.w.load() + 1000000));
                    pushed.fetch_add(1);
                    if (ok) accepted.fetch_add(1);
                }
            });
        for (auto &x : wr) x.join();
        done.store(true);
        reader.join();
        CHECK(!torn);
        // Every push is accounted for: dropped, or stored and then read, overwritten or still parked.
        uint64_t parked = 0;
        for (auto &sl : ring.s) if ((sl.st.load() & 3u) == 2u) ++parked;
        CHECK(pushed.load() == 12u * 4000u);
        CHECK(accepted.load() + ring.dropped.load() == pushed.load());
        CHECK(delivered + ring.overwritten.load() + parked == accepted.load());
        (void)order_ok;   // payload values are hints under contention; order is checked in test_ring_order
    }
}

static void test_ring_order()
{
    // One writer, one reader racing: what is delivered arrives in push order.
    record_ring<rec, 32> ring;
    std::atomic<bool> done{false};
    std::vector<uint64_t> got;
    std::thread reader([&] {
        for (;;) { const bool fin = done.load(); ring.drain([&](const rec &r) { got.push_back(r.a); }); if (fin) break; }
        for (int k = 0; k < 20 && ring.r < ring.w.load(); ++k)   // a dropped record costs the reader two passes
            ring.drain([&](const rec &r) { got.push_back(r.a); });
    });
    for (uint64_t i = 0; i < 200000; ++i) ring.push(make_rec(i));
    done.store(true);
    reader.join();
    bool ordered = true;
    for (size_t i = 1; i < got.size(); ++i) if (got[i] <= got[i - 1]) ordered = false;
    CHECK(ordered);
    // The newest record arrives unless it was itself dropped (its slot being read
    // at that instant) - then it is within the last lap and the drop is counted.
    CHECK(!got.empty() && (got.back() == 199999 || (ring.dropped.load() > 0 && got.back() >= 199999 - 32)));
}

static void test_assessments()
{
    assessments<8> a;
    // Back-to-back recoveries each keep their own verdict inputs.
    const int s1 = a.claim(1, 100, 1000, 5);
    const int s2 = a.claim(2, 200, 1050, 6);
    CHECK(s1 >= 0 && s2 >= 0 && s1 != s2);
    a.note_present(200, 0, 1100, 5000);                       // thread 200 presents
    CHECK(a.s[s1].presents.load() == 1 && a.s[s2].presents.load() == 1);
    CHECK(a.s[s1].same.load() == 0 && a.s[s2].same.load() == 1);
    a.note_present(100, (long)(int32_t)0x887A0005, 1200, 5000);        // thread 100 fails a present
    CHECK(a.s[s1].same.load() == 1 && a.s[s1].fail.load() == (long)(int32_t)0x887A0005);
    CHECK(a.s[s2].fail.load() == (long)(int32_t)0x887A0005);           // it happened after both
    a.release((unsigned)s1);                                  // #1 judged
    CHECK(a.s[s2].n.load() == 2 && a.s[s2].presents.load() == 2);   // #2 untouched
    const int s3 = a.claim(3, 300, 1300, 7);
    CHECK(s3 == s1);                                          // the freed slot is reused, fresh
    CHECK(a.s[s3].presents.load() == 0 && a.s[s3].fail.load() == 0 && a.s[s3].same.load() == 0);
    // The window: old assessments stop counting.
    a.note_present(999, 0, 1050 + 6000, 5000);
    CHECK(a.s[s2].presents.load() == 2);
    // Device check per assessment.
    CHECK(a.wants_device(1400, 5000));
    a.note_device((long)(int32_t)0x887A0006, 1400, 5000);
    CHECK(a.s[s3].dev.load() == (long)(int32_t)0x887A0006);
    // A full table drops the newest assessment (counted); it never overwrites one.
    assessments<4> f;
    for (int i = 1; i <= 4; ++i) CHECK(f.claim(i, 1, 1, 0) >= 0);
    CHECK(f.claim(5, 1, 1, 0) == -1 && f.dropped.load() == 1);
    for (unsigned i = 0; i < 4; ++i) CHECK(f.s[i].n.load() == (long)(i + 1));
    // Concurrent claims: exactly N succeed.
    for (int round = 0; round < 50; ++round)
    {
        assessments<8> c;
        std::atomic<int> ok{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 16; ++t) th.emplace_back([&, t] { if (c.claim(t + 1, (uint32_t)t, 10, 0) >= 0) ++ok; });
        for (auto &x : th) x.join();
        CHECK(ok.load() == 8 && c.dropped.load() == 8 && c.active.load() == 8);
    }
    // Claims, presents and releases racing.
    {
        assessments<8> c;
        std::atomic<bool> stop{false};
        std::thread pres([&] { while (!stop.load()) c.note_present(7, 0, 20, 5000); });
        for (int i = 1; i <= 20000; ++i) { const int k = c.claim(i, 7, 10, 0); if (k >= 0) c.release((unsigned)k); }
        stop.store(true);
        pres.join();
        CHECK(c.active.load() == 0);
    }
}

int main()
{
    test_signature();
    test_call_before();
    test_dest();
    test_rate();
    test_health();
    test_call_semantics();
    test_ring_wrap();
    test_ring_concurrent();
    test_ring_order();
    test_assessments();
    std::printf("steam_guard_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
