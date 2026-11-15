// tests/fg_clock_test.cpp - FC1: the even frame clock against simulated games.
//
//   g++ -std=c++17 -O2 -Isrc tests/fg_clock_test.cpp -o fg_clock_test && ./fg_clock_test
//
// A game hands over real frames with jitter (and the odd hitch); NVIDIA FG has
// made N in-between frames per pair. Both schedules are simulated:
//   per-pair (today's dynamic multiplier): K of N shown, evenly inside the pair;
//   clock (FGClock=1): one tick grid at the target, nearest frame per tick.
// Reported per scenario: shown fps, frame-time spread (how even the display
// is), and the spread of display time minus scene time (if that wobbles, motion
// judders even when frame times are even). Times are in milliseconds.
#include "fg_clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace mgpu::fg_clock;

struct shown { double t, c; };
struct result { double fps, dt_sd, dt_max, lat_mean, lat_sd; unsigned long long late, gaps, skipped, rebases, dropped_jobs; double err; };

static result measure(const std::vector<shown> &v, double secs)
{
    result r{};
    std::vector<double> dts, lats;
    for (size_t i = 1; i < v.size(); ++i) dts.push_back(v[i].t - v[i - 1].t);
    for (const auto &s : v) lats.push_back(s.t - s.c);
    auto sd = [](const std::vector<double> &x, double &mean) {
        mean = 0; for (double a : x) mean += a; mean /= (double)x.size();
        double q = 0; for (double a : x) q += (a - mean) * (a - mean); return std::sqrt(q / (double)x.size());
    };
    double m;
    r.dt_sd = sd(dts, m);
    r.dt_max = *std::max_element(dts.begin(), dts.end());
    r.lat_sd = sd(lats, r.lat_mean);
    r.fps = (double)v.size() / secs;
    return r;
}

struct scenario { const char *name; double real_fps, jitter_sd, hitch_p, hitch_ms, proc_ms, proc_sd, target; };

static void run(const scenario &sc, result &pp, result &ck)
{
    std::mt19937 rng(12345);
    std::normal_distribution<double> nj(0.0, sc.jitter_sd), np(0.0, sc.proc_sd);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double secs = 60.0, P = 1000.0 / sc.target, present_cost = 0.25;
    // real frames: capture times and hand-over times
    std::vector<double> cap, t0;
    double t = 0.0;
    while (t < secs * 1000.0)
    {
        double dt = 1000.0 / sc.real_fps + nj(rng);
        if (u(rng) < sc.hitch_p) dt += sc.hitch_ms;
        if (dt < 4.0) dt = 4.0;
        t += dt;
        cap.push_back(t);
        double p = sc.proc_ms + np(rng); if (p < 1.0) p = 1.0;
        t0.push_back(t + p);
    }
    // N per pair (DMF5: ceil(target x interval) - 1, held 20 pairs) and the
    // per-pair K (the accumulator), from the hand-over interval average.
    std::vector<unsigned> Nv(cap.size(), 1), Kv(cap.size(), 1);
    {
        double ema = 0, acc = 0; unsigned held = 0, want_n = 0, run_n = 0;
        for (size_t k = 1; k < cap.size(); ++k)
        {
            const double dt = t0[k] - t0[k - 1];
            if (dt > 3.0 && dt < 60.0) ema = ema <= 0 ? dt : ema * 0.85 + dt * 0.15;
            const double want = sc.target * ema / 1000.0;
            unsigned need = (unsigned)std::max(1.0, std::ceil(want - 1e-6) - 1.0); if (need > 5) need = 5;
            if (held == 0) held = need;
            else if (need != held) { if (need == want_n) ++run_n; else { want_n = need; run_n = 1; } if (run_n >= 20) { held = need; run_n = 0; } }
            Nv[k] = held;
            double f = std::min(std::max(want, 2.0), (double)held + 1.0);
            acc += f; unsigned n = (unsigned)acc; n = std::max(2u, std::min(n, held + 1)); acc -= n;
            acc = std::max(-1.0, std::min(1.0, acc));
            Kv[k] = n - 1;
        }
    }
    // ---- per-pair schedule (today) ----
    {
        std::vector<shown> out;
        double free_at = 0.0, ema = 0.0;
        size_t pending = 0; bool have_pending = false; unsigned long long dropped = 0;
        // event loop over hand-overs: the present thread takes the newest job when free
        for (size_t k = 1; k < cap.size(); ++k)
        {
            const double dt = t0[k] - t0[k - 1];
            if (dt > 3.0 && dt < 60.0) ema = ema <= 0 ? dt : ema * 0.85 + dt * 0.15;
            if (have_pending && free_at > t0[k]) ++dropped;   // busy: the pending one is replaced
            pending = k; have_pending = true;
            if (free_at > (k + 1 < cap.size() ? t0[k + 1] : 1e18)) continue;   // still busy at the next hand-over
            const size_t j = pending; have_pending = false;
            const unsigned N = Nv[j], K = std::min(Kv[j], N);
            const double span = cap[j] - cap[j - 1];
            const double step = ema / (double)(K + 1);
            double now = std::max(free_at, t0[j]);
            for (unsigned q = 1; q <= K + 1; ++q)
            {
                const double due = t0[j] + (double)(q - 1) * step;
                now = std::max(now, due);
                double frac;
                if (q == K + 1) frac = 1.0;
                else { unsigned jj = (unsigned)std::floor((double)q * (N + 1) / (K + 1) + 0.5); jj = std::max(1u, std::min(jj, N)); frac = (double)jj / (N + 1); }
                out.push_back({ now, cap[j] - span + span * frac });
                now += present_cost;
            }
            free_at = now;
        }
        pp = measure(out, secs);
        pp.dropped_jobs = dropped;
    }
    // ---- clock ----
    {
        std::vector<shown> out;
        state st;
        double sim = 0.0;
        unsigned long long dropped = 0;
        double free_at = 0.0;
        bool have_pending = false;
        for (size_t k = 1; k < cap.size(); ++k)
        {
            if (have_pending && free_at > t0[k]) ++dropped;
            have_pending = true;
            if (free_at > (k + 1 < cap.size() ? t0[k + 1] : 1e18)) continue;
            have_pending = false;
            const size_t j = k;
            const unsigned N = Nv[j];
            const double span = cap[j] - cap[j - 1];
            double c[8]; unsigned M = 0;
            for (unsigned i = 1; i <= N; ++i) c[M++] = cap[j] - span + span * (double)i / (N + 1);
            c[M++] = cap[j];
            sim = std::max(free_at, t0[j]);
            run_pair(st, P, t0[j], span, c, M,
                     [&] { return sim; },
                     [&](double until) { if (until > sim) sim = until; },
                     [&](unsigned f, double) { out.push_back({ sim, c[f] }); sim += present_cost; return true; });
            free_at = sim;
        }
        ck = measure(out, secs);
        ck.late = st.late; ck.gaps = st.gaps; ck.skipped = st.skipped; ck.rebases = st.rebases;
        ck.err = st.shown ? st.err_sum / (double)st.shown : 0.0;
        ck.dropped_jobs = dropped;
    }
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("  FAIL: %s\n", #c); } } while (0)

int main()
{
    const scenario scs[] = {
        { "67 fps -> 180, steady game",            67, 0.5, 0.000, 0,  6, 0.3, 180 },
        { "67 fps -> 180, jittery game",           67, 2.0, 0.005, 8,  6, 1.0, 180 },
        { "45 fps -> 200, jittery game",           45, 2.5, 0.005, 12, 7, 1.5, 200 },
        { "55 fps -> 240, jittery + slow NR",      55, 2.0, 0.010, 10, 9, 2.5, 240 },
        { "40 fps -> 160, hitchy game",            40, 3.0, 0.020, 25, 8, 1.5, 160 },
    };
    std::printf("%-36s %-9s %7s %8s %8s %9s %8s %6s %6s %6s %6s %6s\n", "scenario", "schedule", "fps", "dt sd", "dt max",
                "lat mean", "lat sd", "late", "gaps", "skip", "rebase", "drop");
    for (const auto &sc : scs)
    {
        result a{}, b{};
        run(sc, a, b);
        std::printf("%-36s %-9s %7.1f %8.2f %8.2f %9.2f %8.2f %6s %6s %6s %6s %6llu\n", sc.name, "per-pair", a.fps, a.dt_sd,
                    a.dt_max, a.lat_mean, a.lat_sd, "-", "-", "-", "-", a.dropped_jobs);
        std::printf("%-36s %-9s %7.1f %8.2f %8.2f %9.2f %8.2f %6llu %6llu %6llu %6llu %6llu  (scene error %.2f ms)\n", "", "clock",
                    b.fps, b.dt_sd, b.dt_max, b.lat_mean, b.lat_sd, b.late, b.gaps, b.skipped, b.rebases, b.dropped_jobs, b.err);
        const double P = 1000.0 / sc.target;
        CHECK(b.dt_sd < a.dt_sd);                       // more even on screen
        CHECK(b.rebases <= 30);                         // per minute: not the PERF1 failure (400-600/min)
        CHECK(b.fps > sc.target * 0.85);                // still near the target
        CHECK(b.lat_mean - a.lat_mean < P * 1.5);       // the cost: about half a tick, never more than 1.5
        CHECK(b.lat_sd <= a.lat_sd * 1.25 + 0.3);       // motion no less steady
    }
    std::printf("fg_clock_test: %s\n", fails == 0 ? "all checks passed" : "FAILED");
    return fails == 0 ? 0 : 1;
}
