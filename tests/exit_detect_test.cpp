// exit_detect_test.cpp - EX1 (IN90): the game-closing rule and the
// device-reference ledger. g++ -std=c++17 -I src tests/exit_detect_test.cpp
#include <cstdio>
#include "exit_detect.hpp"

using namespace mgpu::exitdet;
static int g_n = 0, g_fail = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static int A, B, C, W1, W2;   // addresses as identities
static const void *sc1 = &A, *sc2 = &B, *sc3 = &C, *win1 = &W1, *win2 = &W2;

int main()
{
    // ---- RoboCop's exit: chain gone, window gone, then queues
    {
        detector d; d.session_begin();
        d.chain_created(sc1, win1);
        d.chain_destroyed(sc1, false);
        CHECK(d.evaluate(true) == verdict::none);          // window still up
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::exit);         // window gone + queue + no chain
        CHECK(d.evaluate(false) == verdict::none);         // once
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::none);
    }
    // ---- Black Myth's exit: chain + main queue with the window up, window goes later, more queues
    {
        detector d; d.session_begin();
        d.chain_created(sc1, win1);
        d.chain_destroyed(sc1, false);
        d.game_queue_released();
        CHECK(d.evaluate(true) == verdict::none);          // window still up
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::exit);         // next queue event, window gone
    }
    // ---- swap-chain REPLACEMENT mid-game, then a queue replacement: never an exit (the IN89 bug)
    {
        detector d; d.session_begin();
        d.chain_created(sc1, win1);
        d.chain_destroyed(sc1, false);
        d.chain_created(sc2, win1);                        // replacement
        d.game_queue_released();                           // queue replaced later
        CHECK(d.live() == 1);
        CHECK(!d.queue_evidence());
        CHECK(d.evaluate(true) == verdict::none);
        CHECK(d.evaluate(false) == verdict::none);         // even if the window check misfired
    }
    // ---- replacement in the other order (new chain first, then the old one destroyed)
    {
        detector d; d.session_begin();
        d.chain_created(sc1, win1);
        d.chain_created(sc2, win1);
        d.chain_destroyed(sc1, false);
        d.game_queue_released();
        CHECK(d.live() == 1);
        CHECK(d.evaluate(false) == verdict::none);
    }
    // ---- a resize is not a destroy
    {
        detector d; d.chain_created(sc1, win1);
        d.chain_destroyed(sc1, true);
        CHECK(d.live() == 1);
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::none);
    }
    // ---- false signals
    {
        detector d; d.session_begin();
        d.game_queue_released();                           // before any chain (start-up probes)
        CHECK(d.evaluate(false) == verdict::none);
        d.chain_created(sc1, win1);
        d.game_queue_released();                           // queue with a live chain
        CHECK(d.evaluate(false) == verdict::none);
        d.chain_destroyed(sc1, false);
        CHECK(d.evaluate(false) == verdict::none);         // chain + window gone, but no queue since
        d.chain_created(sc2, win2);                        // a new window/chain (splash -> main)
        CHECK(d.evaluate(false) == verdict::none);
        d.chain_destroyed(sc2, false);
        CHECK(d.evaluate(false) == verdict::none);         // still no queue since it went
    }
    // ---- queue evidence cleared by a new chain (device recreated with a new window)
    {
        detector d; d.chain_created(sc1, win1);
        d.chain_destroyed(sc1, false);
        d.game_queue_released();
        d.chain_created(sc2, win2);                        // rebuilt
        CHECK(!d.queue_evidence());
        d.chain_destroyed(sc2, false);
        CHECK(d.evaluate(false) == verdict::none);         // the old queue release does not count
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::exit);
        CHECK(d.last_window() == win2);
    }
    // ---- repeated sessions: the latch and the evidence reset per session
    {
        detector d; d.session_begin();
        d.chain_created(sc1, win1); d.chain_destroyed(sc1, false); d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::exit);
        CHECK(d.fired());
        d.session_begin();                                 // a new bridge session
        CHECK(!d.fired());
        CHECK(d.evaluate(false) == verdict::none);         // old evidence gone
        d.chain_created(sc2, win2); d.chain_destroyed(sc2, false); d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::exit);         // works again
        d.session_begin(); d.session_begin();
        CHECK(d.evaluate(false) == verdict::none);
    }
    // ---- chain table overflow: conservative (never a false exit)
    {
        detector d;
        static int ids[detector::MAX_CHAINS + 2];
        for (int &i : ids) d.chain_created(&i, win1);
        CHECK(d.live() == detector::MAX_CHAINS + 2);
        for (int &i : ids) d.chain_destroyed(&i, false);
        CHECK(d.live() == 2);                              // the untracked two stay "alive"
        d.game_queue_released();
        CHECK(d.evaluate(false) == verdict::none);
    }
    // ---- unknown chain destroyed / null ids
    {
        detector d; d.chain_created(sc1, win1);
        d.chain_destroyed(sc3, false);
        d.chain_destroyed(nullptr, false);
        d.chain_created(nullptr, win1);
        CHECK(d.live() == 1);
    }

    // ======================= ref_ledger =======================
    {
        ref_ledger<const void *> L;
        L.defer(sc1, true); L.defer(sc2, true);
        L.defer(sc3, false);                               // GPU work not proven: kept, never released
        L.defer(nullptr, true);
        CHECK(L.pending() == 2); CHECK(L.kept() == 1);
        auto t = L.take();
        CHECK(t.size() == 2); CHECK(L.pending() == 0);
        CHECK(L.take().empty());                           // released once only
    }
    {
        ref_ledger<const void *> L;                        // the same object twice = two references
        L.defer(sc1, true); L.defer(sc1, true);
        CHECK(L.take().size() == 2);
    }
    {
        ref_ledger<const void *> L;                        // many lifecycle cycles: no overflow, no loss
        static int many[1000];
        int total = 0;
        for (int cyc = 0; cyc < 10; ++cyc)
        {
            for (int i = 0; i < 100; ++i) L.defer(&many[cyc * 100 + i], true);
            total += (int)L.take().size();
        }
        CHECK(total == 1000);
        for (int i = 0; i < 37; ++i) L.defer(&many[i], true);   // more than IN89's 4 slots in one go
        CHECK(L.take().size() == 37);
    }
    {
        // timeout: the device reference of a session whose GPU 0 drain timed out is kept,
        // a later clean session's is released
        ref_ledger<const void *> L;
        L.defer(sc1, false);
        CHECK(L.take().empty()); CHECK(L.kept() == 1);
        L.defer(sc2, true);
        auto t = L.take();
        CHECK(t.size() == 1 && t[0] == sc2);
    }

    std::printf("exit_detect_test: %d/%d passed\n", g_n - g_fail, g_n);
    return g_fail == 0 ? 0 : 1;
}
