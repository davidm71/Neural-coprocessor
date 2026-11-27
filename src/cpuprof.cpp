// CP1 - see cpuprof.hpp.
#include "cpuprof.hpp"

#include <cstdio>
#include <mutex>

#include "diag.hpp"
#include "gpu1_context.hpp"

namespace mgpu
{
namespace cpuprof
{
std::atomic<int> g_on{-1};

namespace
{
// Log-spaced buckets in microseconds for the p99: 1, 2, 4 ... 2^19 (~0.5 s).
const unsigned kBuckets = 20;
struct acc
{
    std::atomic<unsigned long long> n{0}, sum_us{0}, max_us{0};
    std::atomic<unsigned> hist[kBuckets] = {};
};
acc g_acc[SITE_N];
double g_tick_us = 0.0;   // microseconds per QPC tick
std::atomic<long long> g_last{0};
std::mutex g_report_mx;

const char *const kName[SITE_N] = { "NR record (bridge)", "present CPU (per shown frame)", "Present() call",
                                    "game finish-effects (Limiter wait excluded)", "picture finish-effects",
                                    "game present hook", "panel", "Limiter wait (game thread, waiting)",
                                    "HDR display check (bridge, every 2 s)" };

unsigned bucket(unsigned long long us)
{
    unsigned b = 0;
    while (b + 1 < kBuckets && (1ull << b) < us) ++b;
    return b;
}
}

void init()
{
    if (g_on.load() >= 0) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_tick_us = f.QuadPart > 0 ? 1e6 / (double)f.QuadPart : 0.0;
    const int v = mgpu::gpu1::ui_ini_read("CpuProfile", 0) == 1 ? 1 : 0;
    g_last.store(qpc());
    g_on.store(v);
    if (v == 1)
        mgpu::diag::info("[MGPU][CP1] CPU profiling ON (CpuProfile=1): per-frame CPU cost of the mod's paths every 10 s. "
                         "Waits and the Present call are measured apart from the work.");
}

void add(site s, long long ticks)
{
    if (s >= SITE_N || ticks < 0) return;
    const unsigned long long us = (unsigned long long)((double)ticks * g_tick_us + 0.5);
    acc &a = g_acc[s];
    a.n.fetch_add(1ull, std::memory_order_relaxed);
    a.sum_us.fetch_add(us, std::memory_order_relaxed);
    unsigned long long m = a.max_us.load(std::memory_order_relaxed);
    while (us > m && !a.max_us.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    a.hist[bucket(us)].fetch_add(1u, std::memory_order_relaxed);
}

void tick()
{
    if (!on()) return;
    const long long now = qpc();
    const long long last = g_last.load(std::memory_order_relaxed);
    const double dt = (double)(now - last) * g_tick_us / 1e6;
    if (dt < 10.0) return;
    std::unique_lock<std::mutex> lk(g_report_mx, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (g_last.load(std::memory_order_relaxed) != last) return;   // another thread reported
    g_last.store(now, std::memory_order_relaxed);
    char l[1600];
    int k = snprintf(l, sizeof l, "[MGPU][CP1] CPU per call over %.0f s (calls/s, mean / p99 / max us, share of one core):", dt);
    for (unsigned s = 0; s < SITE_N && k > 0 && (size_t)k < sizeof l; ++s)
    {
        acc &a = g_acc[s];
        const unsigned long long n = a.n.exchange(0ull), sum = a.sum_us.exchange(0ull), mx = a.max_us.exchange(0ull);
        unsigned h[kBuckets];
        for (unsigned b = 0; b < kBuckets; ++b) h[b] = a.hist[b].exchange(0u);
        if (n == 0) continue;
        unsigned long long want = (n * 99ull + 99ull) / 100ull, seen = 0;
        unsigned p99 = 0;
        for (unsigned b = 0; b < kBuckets; ++b) { seen += h[b]; if (seen >= want) { p99 = 1u << b; break; } }
        k += snprintf(l + k, sizeof l - (size_t)k, " | %s %.0f/s, %.0f / <=%u / %llu us, %.1f%%", kName[s],
                      (double)n / dt, (double)sum / (double)n, p99, mx, (double)sum / (dt * 1e6) * 100.0);
    }
    mgpu::diag::info(l);
}
}
}
