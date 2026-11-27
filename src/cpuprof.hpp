#pragma once
// ---------------------------------------------------------------------------
// cpuprof.hpp - CP1: the CPU cost of the mod's per-frame paths, measured.
//
// OFF unless mgpu.ini says CpuProfile=1 (read once, at the stream's start).
// Off, every site costs one relaxed atomic load. On, a site costs two
// QueryPerformanceCounter reads and a few relaxed atomic adds.
//
// What is measured is the CPU time the mod's own code spends on the calling
// thread - blocking waits (fences, the present-queue wait) and the driver's
// Present call are paused out of PRESENT_CPU and reported on their own, so a
// number here is work, not waiting. Reported every 10 s as [MGPU][CP1]: per
// site, calls per second, mean / p99 / max microseconds, and the share of one
// CPU core.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <atomic>

namespace mgpu
{
namespace cpuprof
{
enum site : unsigned
{
    NR_RECORD = 0,   // bridge thread: one neural-stage list, Reset to ExecuteCommandLists (NGX record calls included)
    PRESENT_CPU,     // present thread: one shown frame, minus waits and the Present call
    PRESENT_CALL,    // present thread: IDXGISwapChain::Present itself (driver; may block with v-sync)
    GAME_FINISH,     // game render thread: the finish-effects hook on the GAME's queue, minus the Limiter's wait
    PICTURE_FINISH,  // the same hook called for the picture's own (bridge) runtime
    GAME_PRESENT,    // game thread: the present hook
    PANEL,           // the panel, per drawn frame while open
    LIMITER_WAIT,    // the game's thread waiting in the Limiter / queue guard (waiting, not work)
    HDR_POLL,        // bridge thread: the HDR display check (every 2 s with HDR on)
    SITE_N
};

extern std::atomic<int> g_on;   // -1 not read yet, 0 off, 1 on

// Read CpuProfile once (file read: call where that is allowed - at arm).
void init();
inline bool on() { return g_on.load(std::memory_order_relaxed) == 1; }

inline long long qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void add(site s, long long ticks);

// Scoped timer. pause()/resume() exclude a wait from the measured time.
struct scope
{
    site s;
    long long t0 = 0, paused = 0, p0 = 0;
    bool live;
    explicit scope(site st) : s(st), live(on()) { if (live) t0 = qpc(); }
    void pause()  { if (live && p0 == 0) p0 = qpc(); }
    void resume() { if (live && p0 != 0) { paused += qpc() - p0; p0 = 0; } }
    ~scope() { if (live) { resume(); add(s, qpc() - t0 - paused); } }
    scope(const scope &) = delete;
    scope &operator=(const scope &) = delete;
};

// Any thread, cheap when off: writes the report every 10 s.
void tick();
}
}
