// ---------------------------------------------------------------------------
// thread_life.hpp - LR100. The life of a worker thread that is started on
// first use and stopped (joined) at teardown, possibly to be started again.
//
// Used by the frame generation present thread (gpu1_context.cpp: fg_handoff /
// fg_present_stop) and the input-replay hook thread (replay.cpp: hooks /
// shutdown). Portable and header-only, so tests/thread_life_test.cpp checks
// the very rules the bridge runs, on any compiler.
//
// The state is read and written ONLY under the owner's mutex. The join itself
// is done with that mutex RELEASED (the thread usually needs it to see the
// stop request), which is why "stopping" exists at all:
//
//   idle ----use(need)----> running ----stop_take----> stopping
//     ^                                                  |
//     +--------------- stop_done (joined) ---------------+
//
//   * use() while stopping is refused: no second thread is made while the
//     first is being joined, and no work is handed to a thread that is going.
//   * stop_take() takes the thread out of the shared slot (the caller nulls
//     its pointer / handle under the same lock), so a second stop finds
//     nothing to join - never a double join, never a join of a moved-from or
//     replaced thread object. It marks the slot stopping even when there was
//     no thread (LR104): the stopper's cleanup must not overlap a new start.
//   * stop_done() is called at the END of the stop, under the lock again;
//     stops are serialised by their caller (one owner at a time).
//   * A thread that would not end (stop_stuck) leaves the state at stopping:
//     a new thread is never started beside one that is still running.
// ---------------------------------------------------------------------------
#pragma once

namespace mgpu
{
namespace thlife
{
enum class state : unsigned char { idle = 0, running = 1, stopping = 2 };

enum class step : unsigned char
{
    start = 0,    // no thread: make one, then hand over
    use = 1,      // a thread is there: hand over
    skip = 2,     // no thread, and the request does not need one (e.g. "remove hooks")
    refuse = 3,   // a stop is in progress: do nothing, the caller takes its fallback
};

// `has_thread`: the shared slot holds a thread. `need`: the request needs a
// thread to exist (a job, "install hooks"); false for requests that only
// make sense to a thread that is already there.
inline step use(state s, bool has_thread, bool need = true)
{
    if (s == state::stopping) return step::refuse;
    if (has_thread) return step::use;
    return need ? step::start : step::skip;
}

// After a successful start, under the lock.
inline state started() { return state::running; }

// Under the lock. The stopper OWNS the slot from here until stop_done, whether
// or not there was a thread to take (LR104: a stop with nothing to join still
// runs its cleanup, and a hand-over must not start a thread in the middle of
// it). Returns true when there is a thread to join (the caller took it out of
// the slot). Callers serialise their stops (one owner at a time).
inline bool stop_take(state &s, bool has_thread)
{
    s = state::stopping;
    return has_thread;
}

// Under the lock, at the END of the stop. `ok`: the thread was joined, or there
// was none. Not ok (a thread that would not end): stays stopping - never a
// second thread beside one still running.
inline void stop_done(state &s, bool ok)
{
    if (ok) s = state::idle;
}

// The pairing check the tests use: in a quiet moment (no use / stop in
// flight) the state and the slot agree.
inline bool consistent(state s, bool has_thread)
{
    if (s == state::running) return has_thread;
    if (s == state::idle) return !has_thread;
    return !has_thread;   // stopping: the stopper holds the thread, not the slot
}
}   // namespace thlife
}   // namespace mgpu
