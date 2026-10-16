// tex_census.cpp - R279. See tex_census.hpp. An instrument, off by default.
//
// Observe-only: every handler reads what ReShade hands it and writes to this
// file's own table. One mutex, held for table updates only; the report takes
// a copy under the lock and logs outside it.

#include "tex_census.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"   // ui_ini_read

#include <reshade.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mgpu::tex_census
{
namespace
{
    struct entry
    {
        unsigned long long handle = 0;   // 0 = free slot
        unsigned w = 0, h = 0, fmt = 0, usage = 0;
        unsigned long long born = 0, died = 0;      // game frame (died 0 = alive)
        unsigned long long barriers = 0;            // appearances in a barrier
        unsigned long long to_uav = 0;              // transitioned INTO unordered_access
        unsigned long long uav_to_srv = 0;          // unordered_access -> shader_resource
        unsigned long long to_rt = 0;               // transitioned INTO render_target
        unsigned long long rt_to_srv = 0;           // render_target -> shader_resource
        unsigned long long last_frame = 0;          // last frame seen in a barrier
        unsigned last_state = 0;                    // R279b: the state the engine LAST put it in (barrier new_state)
    };

    const unsigned MAX = 256;
    entry g_t[MAX];
    std::mutex g_cs;
    std::atomic<bool> g_on{false};
    void *g_dev = nullptr;               // the game's native device
    unsigned g_w = 0, g_h = 0;           // scene size (the game's back buffer)
    std::atomic<unsigned long long> g_frame{0};
    unsigned g_reported = 0;             // reports made (3 at most)
    // R279c: the peek state the barrier handler needs, defined before it.
    reshade::api::resource g_rb = { 0 };
    unsigned long long g_peek_due = 0;   // frame to map at (0 = none pending)
    unsigned g_pk_w = 0, g_pk_h = 0, g_pk_fmt = 0, g_pk_pitch = 0;
    unsigned long long g_pk_handle = 0, g_pk_at = 0;
    std::atomic<unsigned long long> g_arm{0};
    // Two-channel DXGI formats and their bytes per pixel; 0 = not two-channel.
    unsigned two_ch_bpp(unsigned f)
    {
        if (f >= 48 && f <= 52) return 2;    // R8G8_TYPELESS/UNORM/UINT/SNORM/SINT
        if (f >= 33 && f <= 38) return 4;    // R16G16_*
        if (f >= 15 && f <= 18) return 8;    // R32G32_*  (15 TYPELESS, 16 FLOAT, 17 UINT, 18 SINT)
        return 0;
    }
    // R279d: bytes per pixel and channels for the formats the peek can read
    // (the two-channel set, plus RGBA16 FLOAT/UNORM/SNORM: 10, 11, 13).
    unsigned peek_bpp(unsigned f)  { return (f == 10 || f == 11 || f == 13) ? 8u : two_ch_bpp(f); }
    unsigned peek_chans(unsigned f){ return (f == 10 || f == 11 || f == 13) ? 4u : (two_ch_bpp(f) ? 2u : 0u); }
    std::atomic<int> g_peek_fmt{0};      // R279d: TexPeekFormat (0 = the velocity usage signature)

    bool is_game(reshade::api::device *d)
    {
        return d != nullptr && reinterpret_cast<void *>(static_cast<uintptr_t>(d->get_native())) == g_dev;
    }

    // Between half and full scene size, on both axes - the band a per-pixel
    // or half-res screen buffer lives in. No uniformity test: a packed buffer
    // may be oddly shaped, and this is a census, not a pick.
    bool in_band(unsigned w, unsigned h)
    {
        if (g_w == 0 || g_h == 0 || w == 0 || h == 0) return false;
        const double fx = (double)w / (double)g_w, fy = (double)h / (double)g_h;
        return fx >= 0.49 && fx <= 1.02 && fy >= 0.49 && fy <= 1.02;
    }

    entry *find(unsigned long long h)
    {
        for (unsigned i = 0; i < MAX; ++i) if (g_t[i].handle == h && g_t[i].died == 0) return &g_t[i];
        return nullptr;
    }

    void on_init_resource(reshade::api::device *device, const reshade::api::resource_desc &desc,
                          const reshade::api::subresource_data *, reshade::api::resource_usage,
                          reshade::api::resource res)
    {
        if (!is_game(device) || desc.type != reshade::api::resource_type::texture_2d) return;
        if (!in_band(desc.texture.width, desc.texture.height)) return;
        std::lock_guard<std::mutex> lk(g_cs);
        entry *slot = nullptr;
        for (unsigned i = 0; i < MAX && slot == nullptr; ++i) if (g_t[i].handle == 0) slot = &g_t[i];
        if (slot == nullptr)   // full: reuse the oldest dead one
            for (unsigned i = 0; i < MAX && slot == nullptr; ++i) if (g_t[i].died != 0) slot = &g_t[i];
        if (slot == nullptr) return;
        *slot = entry{};
        slot->handle = res.handle;
        slot->w = desc.texture.width; slot->h = desc.texture.height;
        slot->fmt = static_cast<unsigned>(desc.texture.format);
        slot->usage = static_cast<unsigned>(desc.usage);
        slot->born = g_frame.load(std::memory_order_relaxed);
    }

    void on_destroy_resource(reshade::api::device *device, reshade::api::resource res)
    {
        if (!is_game(device)) return;
        std::lock_guard<std::mutex> lk(g_cs);
        if (entry *e = find(res.handle)) e->died = g_frame.load(std::memory_order_relaxed) + 1;
    }

    void on_barrier(reshade::api::command_list *cl, uint32_t count, const reshade::api::resource *res,
                    const reshade::api::resource_usage *olds, const reshade::api::resource_usage *news)
    {
        if (cl == nullptr || res == nullptr || !is_game(cl->get_device())) return;
        using ru = reshade::api::resource_usage;
        const unsigned uav = static_cast<unsigned>(ru::unordered_access);
        const unsigned srv = static_cast<unsigned>(ru::shader_resource);
        const unsigned rt  = static_cast<unsigned>(ru::render_target);
        const unsigned long long f = g_frame.load(std::memory_order_relaxed);
        // R279c: the armed peek - at the engine's RT -> SRV of the armed texture.
        // Taken with an exchange (one thread wins), recorded before our lock so our
        // own barriers re-entering this handler cannot deadlock or re-trigger.
        {
            const unsigned long long armed = g_arm.load(std::memory_order_relaxed);
            if (armed != 0 && olds != nullptr && news != nullptr)
                for (uint32_t k = 0; k < count; ++k)
                {
                    if (res[k].handle != armed) continue;
                    const unsigned o = static_cast<unsigned>(olds[k]), n = static_cast<unsigned>(news[k]);
                    if (((o & rt) == 0 && (o & uav) == 0) || (n & srv) == 0) continue;   // R279d: RT or UAV -> SRV
                    unsigned long long want = armed;
                    if (!g_arm.compare_exchange_strong(want, 0ull)) break;
                    const reshade::api::resource src = { armed };
                    cl->barrier(src, news[k], reshade::api::resource_usage::copy_source);
                    cl->copy_texture_to_buffer(src, 0, nullptr, g_rb, 0, g_pk_pitch / peek_bpp(g_pk_fmt), g_pk_h);
                    cl->barrier(src, reshade::api::resource_usage::copy_source, news[k]);
                    g_pk_at = f;
                    g_peek_due = f + 8ull;
                    break;
                }
        }
        std::lock_guard<std::mutex> lk(g_cs);
        for (uint32_t k = 0; k < count; ++k)
        {
            entry *e = find(res[k].handle);
            if (e == nullptr) continue;
            ++e->barriers; e->last_frame = f;
            if (news) e->last_state = static_cast<unsigned>(news[k]);   // R279b
            const unsigned o = olds ? static_cast<unsigned>(olds[k]) : 0u, n = news ? static_cast<unsigned>(news[k]) : 0u;
            if ((n & uav) != 0 && (o & uav) == 0) ++e->to_uav;
            if ((o & uav) != 0 && (n & srv) != 0) ++e->uav_to_srv;
            if ((n & rt) != 0 && (o & rt) == 0) ++e->to_rt;
            if ((o & rt) != 0 && (n & srv) != 0) ++e->rt_to_srv;
        }
    }

    void report(unsigned long long f)
    {
        entry copy[MAX]; unsigned n = 0, dead = 0;
        {
            std::lock_guard<std::mutex> lk(g_cs);
            for (unsigned i = 0; i < MAX; ++i)
            {
                if (g_t[i].handle == 0) continue;
                if (g_t[i].died != 0) { ++dead; continue; }
                copy[n++] = g_t[i];
            }
        }
        // Most active first (barrier appearances): the buffers a frame works on.
        for (unsigned a = 0; a < n; ++a)
            for (unsigned b = a + 1; b < n; ++b)
                if (copy[b].barriers > copy[a].barriers) { const entry t = copy[a]; copy[a] = copy[b]; copy[b] = t; }
        char l[400];
        snprintf(l, sizeof l,
                 "[MGPU][R279] TEX CENSUS at game frame %llu: %u live texture(s) between half and full scene size "
                 "(%ux%u), %u destroyed since start. Format numbers are DXGI_FORMAT values. Most active first; "
                 "usage is ReShade's resource_usage mask (0x8 UAV, 0xC0 SRV, 0x4 RT, 0x30 depth).",
                 f, n, g_w, g_h, dead);
        mgpu::diag::info(l);
        for (unsigned a = 0; a < n && a < 24u; ++a)
        {
            const entry &e = copy[a];
            snprintf(l, sizeof l,
                     "[MGPU][R279]   0x%llx %ux%u fmt=%u usage=0x%x | born f=%llu, last barrier f=%llu | barriers=%llu "
                     "toUAV=%llu UAV->SRV=%llu toRT=%llu RT->SRV=%llu",
                     e.handle, e.w, e.h, e.fmt, e.usage, e.born, e.last_frame, e.barriers,
                     e.to_uav, e.uav_to_srv, e.to_rt, e.rt_to_srv);
            mgpu::diag::info(l);
        }
    }

    // ---- R279b: THE PEEK (TexPeek=1, needs TexCensus=1) ----
    //
    // Picks the texture by HOW THE ENGINE USES IT, not by its format: two
    // channels, exactly the scene size, transitioned into RENDER_TARGET and
    // RENDER_TARGET -> SRV about once per frame since it was born - the usage
    // signature of a velocity target. Copies it three times (frames 1500,
    // 3300, 6600) from the state the engine's own last barrier left it in
    // (known, not assumed), maps the copy eight frames later and logs the
    // per-channel distribution. An encoded velocity sits at its centre when
    // the camera is still (0.5 for UNORM, 0 for SNORM/FLOAT) and spreads when
    // it moves. One readback buffer on the game device, three copies in all.
    std::atomic<bool> g_peek{false};
    reshade::api::device *g_api_dev = nullptr;
    unsigned long long g_rb_size = 0;
    unsigned g_peeks = 0;
    // R279c: the copy is ARMED at a peek frame and recorded by on_barrier at the
    // engine's own RT -> SRV transition of that texture, on the engine's command
    // list, inside the frame. Atomic: command lists are recorded on many threads.
    unsigned long long g_arm_frame = 0;
    const unsigned char SENT = 0xA5;   // sentinel byte written into the readback before each copy
    bool g_sent_ok = false;


    float half_f(unsigned short h)
    {
        const unsigned s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
        float v;
        if (e == 0) v = (float)m * (1.0f / 16777216.0f);
        else if (e == 31) v = 65504.0f;
        else v = (1.0f + (float)m / 1024.0f) * (float)(1u << e) / 32768.0f;
        return s ? -v : v;
    }

    // Decoded value of channel c at a pixel, and the format's "no motion" centre.
    float chan(const unsigned char *px, unsigned fmt, unsigned c)
    {
        switch (fmt)
        {
        case 49: case 48: case 50: return (float)px[c] / 255.0f;                         // RG8 UNORM (typeless/uint read as unorm)
        case 51: case 52: return (float)(signed char)px[c] / 127.0f;                     // RG8 SNORM
        case 34: case 33: return half_f(((const unsigned short *)px)[c]);                // RG16 FLOAT
        case 35: case 36: return (float)((const unsigned short *)px)[c] / 65535.0f;      // RG16 UNORM
        case 37: case 38: return (float)((const short *)px)[c] / 32767.0f;               // RG16 SNORM
        case 10: return half_f(((const unsigned short *)px)[c]);                         // R279d: RGBA16 FLOAT
        case 11: return (float)((const unsigned short *)px)[c] / 65535.0f;               // R279d: RGBA16 UNORM
        case 13: return (float)((const short *)px)[c] / 32767.0f;                        // R279d: RGBA16 SNORM
        default: return ((const float *)px)[c];                                          // RG32
        }
    }
    float centre_of(unsigned fmt) { return (fmt == 49 || fmt == 48 || fmt == 50 || fmt == 35 || fmt == 36 || fmt == 11) ? 0.5f : 0.0f; }

    // The census's choice: the usage signature, strongest first.
    bool pick_signature(unsigned long long f, unsigned long long &h, unsigned &w, unsigned &hh, unsigned &fmt, unsigned &state,
                        unsigned long long &to_rt, unsigned long long &rt_srv)
    {
        std::lock_guard<std::mutex> lk(g_cs);
        const entry *best = nullptr;
        for (unsigned i = 0; i < MAX; ++i)
        {
            const entry &e = g_t[i];
            if (e.handle == 0 || e.died != 0) continue;
            const unsigned long long life = (f > e.born) ? (f - e.born) : 0ull;
            // R279d: TexPeekFormat=N - the most active live texture of that format at
            // scene size, written (RT or UAV) at least once per 2 frames lived.
            const int pf = g_peek_fmt.load(std::memory_order_relaxed);
            if (pf != 0)
            {
                if ((int)e.fmt != pf || peek_bpp(e.fmt) == 0 || e.w != g_w || e.h != g_h) continue;
                if (life < 300ull || e.last_state == 0u) continue;
                if ((e.to_rt + e.to_uav) * 2ull < life) continue;
                if (best == nullptr || e.barriers > best->barriers) best = &e;
                continue;
            }
            if (two_ch_bpp(e.fmt) == 0 || e.w != g_w || e.h != g_h) continue;
            if (life < 300ull || e.last_state == 0u) continue;
            // about once per frame, both ways (0.8 .. 1.25 per frame lived)
            if (e.to_rt * 5ull < life * 4ull || e.to_rt * 4ull > life * 5ull) continue;
            if (e.rt_to_srv * 5ull < e.to_rt * 4ull) continue;
            if (best == nullptr || e.to_rt > best->to_rt) best = &e;
        }
        if (best == nullptr) return false;
        h = best->handle; w = best->w; hh = best->h; fmt = best->fmt; state = best->last_state;
        to_rt = best->to_rt; rt_srv = best->rt_to_srv;
        return true;
    }

    void peek_copy(reshade::api::command_queue *q, unsigned long long f)
    {
        unsigned long long h = 0, to_rt = 0, rt_srv = 0; unsigned w = 0, hh = 0, fmt = 0, st = 0;
        char l[400];
        if (!pick_signature(f, h, w, hh, fmt, st, to_rt, rt_srv))
        {
            snprintf(l, sizeof l, "[MGPU][R279b] PEEK at f=%llu: no live texture has the velocity usage signature (two channels, %ux%u, "
                                  "into RT and RT->SRV about once per frame). Nothing copied.", f, g_w, g_h);
            mgpu::diag::info(l);
            return;
        }
        const unsigned bpp = peek_bpp(fmt);
        const unsigned pitch = ((w * bpp) + 255u) & ~255u;          // D3D12 row pitch alignment
        const unsigned long long need = (unsigned long long)pitch * hh;
        if (g_rb.handle != 0 && g_rb_size < need) { g_api_dev->destroy_resource(g_rb); g_rb = { 0 }; g_rb_size = 0; }
        if (g_rb.handle == 0)
        {
            if (!g_api_dev->create_resource(reshade::api::resource_desc(need, reshade::api::memory_heap::gpu_to_cpu,
                                                                          reshade::api::resource_usage::copy_dest),
                                            nullptr, reshade::api::resource_usage::copy_dest, &g_rb))
            {
                mgpu::diag::warn("[MGPU][R279b] PEEK: the readback buffer could not be created - peek off for this run.");
                g_peek.store(false);
                return;
            }
            g_rb_size = need;
        }
        (void)q;
        // R279c: the sentinel. Every byte of the readback set to 0xA5 before the
        // copy; bytes still 0xA5 after it were never written (R85's lesson).
        {
            void *wp = nullptr;
            g_sent_ok = g_api_dev->map_buffer_region(g_rb, 0, UINT64_MAX, reshade::api::map_access::write_only, &wp) && wp != nullptr;
            if (g_sent_ok) { memset(wp, SENT, (size_t)need); g_api_dev->unmap_buffer_region(g_rb); }
        }
        g_pk_w = w; g_pk_h = hh; g_pk_fmt = fmt; g_pk_pitch = pitch; g_pk_handle = h;
        g_arm_frame = f;
        g_arm.store(h, std::memory_order_release);   // the barrier handler records the copy
        snprintf(l, sizeof l, "[MGPU][R279c] PEEK %u/3 ARMED at f=%llu: 0x%llx %ux%u fmt=%u (into RT %llu, RT->SRV %llu since birth). "
                              "Copied at the engine's next RT->SRV of it, inside the frame. Sentinel %s.",
                 g_peeks + 1, f, h, w, hh, fmt, to_rt, rt_srv, g_sent_ok ? "written (0xA5)" : "NOT written (map for write failed)");
        mgpu::diag::info(l);
        (void)st;
    }

    void peek_read()
    {
        void *data = nullptr;
        if (!g_api_dev->map_buffer_region(g_rb, 0, UINT64_MAX, reshade::api::map_access::read_only, &data) || data == nullptr)
        {
            mgpu::diag::warn("[MGPU][R279b] PEEK: map failed - this peek is lost.");
            return;
        }
        const unsigned bpp = peek_bpp(g_pk_fmt);
        const unsigned nch = peek_chans(g_pk_fmt);
        const float c0 = centre_of(g_pk_fmt);
        const float near_ = (bpp == 2) ? (1.5f / 255.0f) : 0.002f;
        double mn[4] = { 1e30, 1e30, 1e30, 1e30 }, mx[4] = { -1e30, -1e30, -1e30, -1e30 }, sum[4] = { 0, 0, 0, 0 }, sq[4] = { 0, 0, 0, 0 };
        unsigned long long n = 0, at_c[4] = { 0, 0, 0, 0 }, pos[4] = { 0, 0, 0, 0 }, sent_px = 0;   // R279c: pixels still all-sentinel
        const unsigned char *base = (const unsigned char *)data;
        for (unsigned y = 0; y < g_pk_h; y += 2)
        {
            const unsigned char *row = base + (size_t)y * g_pk_pitch;
            for (unsigned x = 0; x < g_pk_w; x += 4)
            {
                const unsigned char *px = row + (size_t)x * bpp;
                {   // R279c
                    bool all = true;
                    for (unsigned b = 0; b < bpp; ++b) if (px[b] != SENT) { all = false; break; }
                    if (all) ++sent_px;
                }
                for (unsigned c = 0; c < nch; ++c)
                {
                    const float v = chan(px, g_pk_fmt, c);
                    if (v < mn[c]) mn[c] = v;
                    if (v > mx[c]) mx[c] = v;
                    sum[c] += v; sq[c] += (double)v * v;
                    const float d = v - c0;
                    if (d < near_ && d > -near_) ++at_c[c];
                    if (d > 0.0f) ++pos[c];
                }
                ++n;
            }
        }
        g_api_dev->unmap_buffer_region(g_rb);
        if (n == 0) return;
        char l[520];
        snprintf(l, sizeof l, "[MGPU][R279c] PEEK %u/3 (copied f=%llu, armed f=%llu): sentinel %s - %.1f%% of sampled pixels still 0xA5 "
                              "(100%% = the copy never landed; 0%% = every sampled pixel was written by the copy).",
                 g_peeks + 1, g_pk_at, g_arm_frame, g_sent_ok ? "was written" : "was NOT written",
                 100.0 * (double)sent_px / (double)n);
        mgpu::diag::info(l);
        for (unsigned c = 0; c < nch; ++c)
        {
            const double mean = sum[c] / (double)n, var = sq[c] / (double)n - mean * mean;
            snprintf(l, sizeof l, "[MGPU][R279b] PEEK %u/3 (copied f=%llu) channel %c: min=%.4f max=%.4f mean=%.4f sd=%.4f | "
                                  "at the no-motion centre (%.2f +- %.4f) %.1f%% | above it %.1f%% | %llu samples (every 2nd row, 4th column)",
                     g_peeks + 1, g_pk_at, "xyzw"[c], mn[c], mx[c], mean, var > 0 ? std::sqrt(var) : 0.0,
                     c0, near_, 100.0 * (double)at_c[c] / (double)n, 100.0 * (double)pos[c] / (double)n, n);
            mgpu::diag::info(l);
        }
    }

    void on_present(reshade::api::command_queue *q, reshade::api::swapchain *sc,
                    const reshade::api::rect *, const reshade::api::rect *, uint32_t, const reshade::api::rect *)
    {
        if (sc == nullptr || !is_game(sc->get_device())) return;
        const unsigned long long f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        // R279b: the peek, three times; the read eight frames after each copy.
        if (g_peek.load(std::memory_order_relaxed) && q != nullptr)
        {
            if (g_api_dev == nullptr) g_api_dev = sc->get_device();
            if (g_peek_due != 0 && f >= g_peek_due) { g_peek_due = 0; peek_read(); ++g_peeks; }
            else if (g_peek_due == 0 && g_arm.load() == 0 && g_peeks < 3u && (f == 1500ull || f == 3300ull || f == 6600ull)) peek_copy(q, f);
            else if (g_arm.load() != 0 && f > g_arm_frame + 600ull)
            {   // R279c: armed, but the engine never transitioned it RT -> SRV in 600 frames
                g_arm.store(0);
                ++g_peeks;
                char l[220];
                snprintf(l, sizeof l, "[MGPU][R279c] PEEK %u/3: armed at f=%llu, no RT->SRV of it seen in 600 frames - disarmed, nothing copied.", g_peeks, g_arm_frame);
                mgpu::diag::info(l);
            }
        }
        if ((f == 1200ull && g_reported == 0) || (f == 3000ull && g_reported == 1) || (f == 6000ull && g_reported == 2))
        {
            ++g_reported;
            report(f);
        }
    }
}   // namespace

void init(void *game_device, unsigned screen_w, unsigned screen_h)
{
    if (g_on.load()) { g_w = screen_w; g_h = screen_h; return; }
    if (mgpu::gpu1::ui_ini_read("TexCensus", 0) != 1) return;
    g_dev = game_device; g_w = screen_w; g_h = screen_h;
    g_peek.store(mgpu::gpu1::ui_ini_read("TexPeek", 0) == 1);   // R279b
    g_peek_fmt.store(mgpu::gpu1::ui_ini_read("TexPeekFormat", 0)); // R279d: 0 = the velocity usage signature
    char l[300];
    snprintf(l, sizeof l, "[MGPU][R279] TEX CENSUS ON (TexCensus=1): observe-only. Every 2D texture between half and full "
                          "scene size (%ux%u) on the game's D3D12 device, any format, with its barrier transitions. "
                          "Reports at game frames 1200, 3000, 6000. An instrument: TexCensus=0 or absent turns it off.",
             screen_w, screen_h);
    mgpu::diag::info(l);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    reshade::register_event<reshade::addon_event::present>(on_present);
    g_on.store(true);
}

void shutdown()
{
    if (!g_on.load()) return;
    reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
    reshade::unregister_event<reshade::addon_event::present>(on_present);
    if (g_rb.handle != 0 && g_api_dev != nullptr) { g_api_dev->destroy_resource(g_rb); g_rb = { 0 }; }   // R279b
    g_on.store(false);
}
}   // namespace mgpu::tex_census
