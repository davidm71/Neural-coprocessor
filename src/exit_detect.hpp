// exit_detect.hpp - EX1 (IN90). Is the game closing? And which references on
// the game's device may the add-on give back? Pure C++ (no Windows), so
// tests/exit_detect_test.cpp drives exactly the rules the bridge uses.
//
// THE QUESTION. The add-on holds references on the game's D3D12 device (IN88/
// IN89); ReShade raises destroy_device only when they are gone, and the add-on
// used to give them back only on destroy_device. EX1 starts the add-on's
// ordered shutdown on its own evidence instead. IN89 took "the game released
// the queue the add-on works on, after one non-resize swap-chain destroy" -
// and never forgot that destroy, so a game that REPLACES its swap chain and
// later its queue would have been read as closing in the middle of play.
//
// THE RULE NOW - all three, at one moment:
//   1. no game swap chain is alive. Chains are tracked BY IDENTITY: created ->
//      alive, destroyed (not a resize) -> gone; a replacement brings the count
//      back up, so "one was destroyed once" means nothing by itself;
//   2. the game's window - the one its last swap chain was on - is gone;
//   3. a queue on the game's device was released while no game chain was
//      alive (queues go with the device's teardown, not with a window or a
//      chain change; a splash window or a display-mode swap does not do this).
// A new game chain clears 2-3. Fires once per bridge session (session_begin()).
//
// Seen in the field (IN85-IN89): RoboCop destroys its chain, its window goes,
// then its queues; Black Myth destroys its chain and main queue with the
// window still up, the window goes 0.3-0.8 s later, then five more queues.
// Both satisfy all three at a later queue event. A game that satisfies them
// only after its last destroy event never gets EX1 - exactly the behaviour
// before IN89 (Windows reclaims everything), never a false stop.
#pragma once
#include <cstddef>
#include <vector>

namespace mgpu
{
namespace exitdet
{
enum class verdict { none, exit };

class detector
{
public:
    static constexpr int MAX_CHAINS = 8;

    // A new bridge session (bridge thread started, or the stream armed): the
    // "fired" latch and the queue evidence start over. Chains stay tracked -
    // they are the game's, not the session's.
    void session_begin() { fired_ = false; queue_since_gone_ = false; }

    // A GAME swap chain (not the bridge's own) was created / destroyed.
    void chain_created(const void *id, const void *hwnd)
    {
        if (id == nullptr) return;
        seen_chain_ = true;
        queue_since_gone_ = false;   // the game is building, not leaving
        for (slot &s : chains_) if (s.id == id) { s.hwnd = hwnd; return; }
        for (slot &s : chains_) if (s.id == nullptr) { s.id = id; s.hwnd = hwnd; return; }
        ++untracked_;   // no room: counted alive forever (conservative - never a false exit)
    }
    void chain_destroyed(const void *id, bool resize)
    {
        if (id == nullptr || resize) return;   // a resize keeps the chain
        for (slot &s : chains_)
            if (s.id == id) { last_hwnd_ = s.hwnd; s.id = nullptr; s.hwnd = nullptr; return; }
    }
    // A queue on the GAME's device was released (the caller filters the device).
    void game_queue_released()
    {
        if (seen_chain_ && live() == 0) queue_since_gone_ = true;
    }

    // Evaluate now. `last_window_alive`: is window last_window() still there?
    verdict evaluate(bool last_window_alive)
    {
        if (fired_ || !seen_chain_ || live() != 0 || last_window_alive || !queue_since_gone_ || last_hwnd_ == nullptr)
            return verdict::none;
        fired_ = true;
        return verdict::exit;
    }

    int live() const
    {
        int n = untracked_;
        for (const slot &s : chains_) if (s.id != nullptr) ++n;
        return n;
    }
    const void *last_window() const { return last_hwnd_; }
    bool fired() const { return fired_; }
    bool queue_evidence() const { return queue_since_gone_; }

private:
    struct slot { const void *id = nullptr; const void *hwnd = nullptr; };
    slot chains_[MAX_CHAINS];
    int  untracked_ = 0;
    const void *last_hwnd_ = nullptr;
    bool seen_chain_ = false;
    bool queue_since_gone_ = false;
    bool fired_ = false;
};

// References the add-on holds on the game's device (its GetDevice references,
// ReShade queue wrappers built on it), given back by the bridge thread at the
// end of its teardown with no lock held. Unbounded (IN89 had 4 fixed slots and
// would have dropped the fifth). A reference whose GPU work was NOT proven
// finished is KEPT - counted, never handed back - as GS2 keeps resources.
template <class T>
class ref_ledger
{
public:
    void defer(T p, bool gpu_work_proven_done)
    {
        if (p == nullptr) return;
        if (gpu_work_proven_done) pending_.push_back(p);
        else ++kept_;
    }
    // Everything to release now; the ledger is empty afterwards.
    std::vector<T> take()
    {
        std::vector<T> out;
        out.swap(pending_);
        return out;
    }
    std::size_t pending() const { return pending_.size(); }
    int kept() const { return kept_; }

private:
    std::vector<T> pending_;
    int kept_ = 0;
};
}
}
