// mvec_census.cpp - R253. See mvec_census.hpp and DX11_CONTRACT_LEDGER.md
// section 13.
//
// Observe-only. Every handler here reads what ReShade hands it and writes to
// this file's own tables; nothing is created, bound or copied on the game's
// device, and the handlers return without changing the call they observe
// (update_buffer_region returns false: ReShade then forwards the call as
// it was).
//
// Thread notes. The bind event fires on the game's render thread; Map/Unmap
// and UpdateSubresource can fire on any thread the game uses for uploads.
// One mutex, held for table updates only, never across a log write of more
// than one line - the periodic report takes a copy under the lock and logs
// outside it.

#include "mvec_census.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"   // ui_ini_read

#include <reshade.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>

namespace mgpu::mvec_census
{
namespace
{
    std::atomic<bool> g_on{false};
    std::atomic<bool> g_report{false};   // MVecCensus=1: the periodic reports
    std::atomic<bool> g_tap{false};      // MVecTap=1 (R254)
    std::mutex g_cs;
    unsigned g_w = 0, g_h = 0;
    unsigned long long g_frame = 0;        // game frames (note_frame)
    unsigned g_bind_seq = 0;               // bind events so far in this frame

    // ---- RT census ----
    struct rt_entry
    {
        unsigned long long handle = 0;
        unsigned w = 0, h = 0;
        unsigned fmt = 0;
        unsigned long long created_frame = 0;
        unsigned long long binds = 0;               // bind events, total
        unsigned long long frames_bound = 0;        // distinct frames with >= 1 bind
        unsigned long long last_frame = ~0ull;      // last frame a bind was counted in
        unsigned first_seq_min = 0xFFFFFFFFu;       // earliest bind position seen in a frame
        unsigned first_seq_max = 0;                 // latest first-bind position seen
        bool alive = true;
    };
    std::vector<rt_entry> g_rt;
    const size_t RT_CAP = 256;

    bool two_channel(reshade::api::format f)
    {
        using reshade::api::format;
        switch (f)
        {
        case format::r16g16_typeless: case format::r16g16_float: case format::r16g16_unorm:
        case format::r16g16_snorm:    case format::r16g16_uint:
        case format::r32g32_typeless: case format::r32g32_float:
            return true;
        default: return false;
        }
    }

    const char *fmt_name(unsigned f)
    {
        using reshade::api::format;
        switch ((format)f)
        {
        case format::r16g16_typeless: return "R16G16_TYPELESS";
        case format::r16g16_float:    return "R16G16_FLOAT";
        case format::r16g16_unorm:    return "R16G16_UNORM";
        case format::r16g16_snorm:    return "R16G16_SNORM";
        case format::r16g16_uint:     return "R16G16_UINT";
        case format::r32g32_typeless: return "R32G32_TYPELESS";
        case format::r32g32_float:    return "R32G32_FLOAT";
        default: return "?";
        }
    }

    bool screen_sized(unsigned w, unsigned h)
    {
        // At least half the chain in each dimension: a velocity target is
        // either full size or the TAA's internal resolution.
        return g_w != 0 && g_h != 0 && w * 2 >= g_w && h * 2 >= g_h && w <= g_w * 2 && h <= g_h * 2;
    }

    // ---- CB census ----
    struct cb_entry
    {
        unsigned long long handle = 0;
        unsigned long long size = 0;                // buffer size from its desc
        unsigned long long updates = 0;             // Unmap / UpdateSubresource, total
        unsigned long long frames_updated = 0;      // distinct frames
        unsigned long long last_frame = ~0ull;
        unsigned long long first_frame = ~0ull;     // R259b: frame of the first write seen
        unsigned updates_this_frame = 0;
        unsigned max_per_frame = 0;
        unsigned long long changes = 0;             // content hash differed from the previous write
        unsigned long long last_hash = 0;
        unsigned char tail[512] = {};               // R259: first 512 bytes of the last write (was 256)
        unsigned tail_n = 0;
        void *mapped = nullptr;                     // between map and unmap
        unsigned long long mapped_size = 0;
    };
    std::vector<cb_entry> g_cb;
    const size_t CB_CAP = 512;

    unsigned long long fnv(const void *p, size_t n)
    {
        const unsigned char *b = (const unsigned char *)p;
        unsigned long long h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    }

    cb_entry *cb_find(unsigned long long handle, reshade::api::device *dev, bool create)
    {
        for (auto &e : g_cb) if (e.handle == handle) return &e;
        if (!create || g_cb.size() >= CB_CAP) return nullptr;
        cb_entry e; e.handle = handle; e.first_frame = g_frame;   // R259b
        if (dev != nullptr)
        {
            const reshade::api::resource_desc d = dev->get_resource_desc(reshade::api::resource{ handle });
            if (d.type == reshade::api::resource_type::buffer) e.size = d.buffer.size;
        }
        g_cb.push_back(e);
        return &g_cb.back();
    }

    void cb_note_write(cb_entry &e, const void *data, unsigned long long size)
    {
        ++e.updates;
        if (e.last_frame != g_frame) { e.last_frame = g_frame; ++e.frames_updated; e.updates_this_frame = 0; }
        ++e.updates_this_frame;
        if (e.updates_this_frame > e.max_per_frame) e.max_per_frame = e.updates_this_frame;
        if (data == nullptr || size == 0) return;
        // R253b: ReShade reports a whole-resource Map as size UINT64_MAX, and a
        // D3D11 Map always maps the whole buffer. Never read past the buffer's
        // own size, and never read a buffer whose size is unknown.
        unsigned long long bound = size;
        if (bound == ~0ull || (e.size != 0 && bound > e.size)) bound = e.size;
        if (bound == 0) return;
        const size_t n = (size_t)std::min<unsigned long long>(bound, 4096ull);
        const unsigned long long h = fnv(data, n);
        if (e.updates > 1 && h != e.last_hash) ++e.changes;
        e.last_hash = h;
        e.tail_n = (unsigned)std::min<size_t>(n, sizeof e.tail);
        memcpy(e.tail, data, e.tail_n);
    }

    // ---- handlers ----
    void on_init_resource(reshade::api::device *, const reshade::api::resource_desc &desc,
                          const reshade::api::subresource_data *, reshade::api::resource_usage,
                          reshade::api::resource res)
    {
        if (desc.type != reshade::api::resource_type::texture_2d) return;
        if (!two_channel(desc.texture.format)) return;
        if ((desc.usage & reshade::api::resource_usage::render_target) == 0) return;
        if (!screen_sized(desc.texture.width, desc.texture.height)) return;
        std::lock_guard<std::mutex> lk(g_cs);
        if (g_rt.size() >= RT_CAP) return;
        rt_entry e; e.handle = res.handle; e.w = desc.texture.width; e.h = desc.texture.height;
        e.fmt = (unsigned)desc.texture.format; e.created_frame = g_frame;
        g_rt.push_back(e);
        char l[300];
        snprintf(l, sizeof l, "[MGPU][R253][RT] created: 0x%016llX %ux%u %s (render target, two-channel, screen-sized) at game frame %llu",
                 e.handle, e.w, e.h, fmt_name(e.fmt), g_frame);
        mgpu::diag::info(l);
    }

    void on_destroy_resource(reshade::api::device *, reshade::api::resource res)
    {
        std::lock_guard<std::mutex> lk(g_cs);
        for (auto &e : g_rt) if (e.handle == res.handle && e.alive) { e.alive = false; }
        for (size_t i = 0; i < g_cb.size(); ++i)
            if (g_cb[i].handle == res.handle) { g_cb.erase(g_cb.begin() + (ptrdiff_t)i); break; }
    }

    void on_bind_rts(reshade::api::command_list *cmd, uint32_t count, const reshade::api::resource_view *rtvs,
                     reshade::api::resource_view)
    {
        if (count == 0 || rtvs == nullptr || cmd == nullptr) return;
        reshade::api::device *dev = cmd->get_device();
        if (dev == nullptr) return;
        std::lock_guard<std::mutex> lk(g_cs);
        const unsigned seq = g_bind_seq++;
        if (g_rt.empty()) return;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (rtvs[i].handle == 0) continue;
            const reshade::api::resource r = dev->get_resource_from_view(rtvs[i]);
            for (auto &e : g_rt)
            {
                if (e.handle != r.handle || !e.alive) continue;
                ++e.binds;
                if (e.last_frame != g_frame)
                {
                    e.last_frame = g_frame; ++e.frames_bound;
                    if (seq < e.first_seq_min) e.first_seq_min = seq;
                    if (seq > e.first_seq_max) e.first_seq_max = seq;
                }
            }
        }
    }

    void on_map_buffer(reshade::api::device *dev, reshade::api::resource res, uint64_t, uint64_t size,
                       reshade::api::map_access access, void **data)
    {
        if (access == reshade::api::map_access::read_only || data == nullptr) return;
        std::lock_guard<std::mutex> lk(g_cs);
        cb_entry *e = cb_find(res.handle, dev, true);
        if (e == nullptr) return;
        e->mapped = *data;
        e->mapped_size = (size != 0 && size != ~0ull) ? size : e->size;   // R253b
    }

    void on_unmap_buffer(reshade::api::device *dev, reshade::api::resource res)
    {
        std::lock_guard<std::mutex> lk(g_cs);
        cb_entry *e = cb_find(res.handle, dev, false);
        if (e == nullptr) return;
        // The event fires before the Unmap call: the mapping is still live.
        cb_note_write(*e, e->mapped, e->mapped_size);
        e->mapped = nullptr; e->mapped_size = 0;
    }

    bool on_update_buffer(reshade::api::device *dev, const void *data, reshade::api::resource dest,
                          uint64_t, uint64_t size)
    {
        std::lock_guard<std::mutex> lk(g_cs);
        cb_entry *e = cb_find(dest.handle, dev, true);
        if (e != nullptr) cb_note_write(*e, data, size);
        return false;   // observe only: ReShade forwards the call unchanged
    }

    void report(unsigned long long f)
    {
        std::vector<rt_entry> rt; std::vector<cb_entry> cb;
        {
            std::lock_guard<std::mutex> lk(g_cs);
            rt = g_rt; cb = g_cb;
        }
        char l[420];
        snprintf(l, sizeof l, "[MGPU][R253] CENSUS at game frame %llu | screen %ux%u | two-channel screen-sized render targets seen: %zu | CPU-written buffers seen: %zu",
                 f, g_w, g_h, rt.size(), cb.size());
        mgpu::diag::info(l);
        for (const auto &e : rt)
        {
            snprintf(l, sizeof l,
                     "[MGPU][R253][RT] 0x%016llX %ux%u %s %s | binds=%llu frames_bound=%llu of %llu since creation | "
                     "first bind position in frame: min=%u max=%u",
                     e.handle, e.w, e.h, fmt_name(e.fmt), e.alive ? "alive" : "DESTROYED",
                     e.binds, e.frames_bound, (f > e.created_frame) ? (f - e.created_frame) : 0ull,
                     e.first_seq_min == 0xFFFFFFFFu ? 0u : e.first_seq_min, e.first_seq_max);
            mgpu::diag::info(l);
        }
        // Buffers written in most frames, most active first, capped.
        std::sort(cb.begin(), cb.end(), [](const cb_entry &a, const cb_entry &b) { return a.frames_updated > b.frames_updated; });
        unsigned shown = 0;
        for (const auto &e : cb)
        {
            if (e.frames_updated < 30ull) break;
            if (shown++ >= 24u) break;
            snprintf(l, sizeof l,
                     "[MGPU][R253][CB] 0x%016llX size=%llu | updates=%llu frames_updated=%llu max_per_frame=%u | content changed on %llu writes",
                     e.handle, e.size, e.updates, e.frames_updated, e.max_per_frame, e.changes);
            mgpu::diag::info(l);
        }
        // R259: the camera dump. SK-28 showed the R253 dump printing the
        // first 32 floats of a 304-byte buffer whose content changed on
        // 1463 of 1464 writes - and those 32 floats were identical at 900
        // and 1800: the part that moves is past float 32. So: the whole
        // buffer (up to 128 floats), buffers written once or twice per
        // frame in nearly every frame whose content changes on nearly every
        // write (a camera does; a per-material block does not), and three
        // CONSECUTIVE frames so the per-frame delta is visible in one place.
    }

    // R259b: the camera dump on its own frames. SK-32: the dump ran inside
    // report(), which runs every 300 frames, so 901/902/1801 never printed;
    // and "written in >= 90% of frames" counted from frame 0, which on a
    // run that opens with 170 frames of loading screen filtered the one
    // camera-shaped buffer (732 of 900). Now: counted from the buffer's own
    // first write, and the dump frames are checked directly in note_frame.
    void camera_dump(unsigned long long f)
    {
        std::vector<cb_entry> cb;
        {
            std::lock_guard<std::mutex> lk(g_cs);
            cb = g_cb;
        }
        char l[420];
        {
            unsigned dumped = 0;
            for (const auto &e : cb)
            {
                // R259d: widened from <=2 to <=8 writes per frame (SK-34: nothing
                // at <=2 moves with the camera; the 368-byte buffer at 8/frame is
                // the next by shape). The tail is the LAST write of the frame.
                if (dumped >= 6u) break;
                if (e.size < 64ull || e.size > 2048ull || e.max_per_frame > 8u || e.tail_n < 64u) continue;
                const unsigned long long since = (e.first_frame != ~0ull && f > e.first_frame) ? (f - e.first_frame) : f;
                if (e.frames_updated * 10ull < since * 9ull) continue;        // in >= 90% of frames since its first write (R259b)
                if (e.updates == 0 || e.changes * 10ull < e.updates * 9ull) continue;  // changed on >= 90% of writes
                ++dumped;
                const unsigned nf = std::min(e.tail_n / 4u, 128u);
                const float *fp = (const float *)e.tail;
                char d[700];
                snprintf(d, sizeof d, "[MGPU][R259][CB-DUMP] f=%llu 0x%016llX size=%llu updates=%llu changes=%llu max_per_frame=%u | %u floats in rows of 16 (last write of the frame):",
                         f, e.handle, e.size, e.updates, e.changes, e.max_per_frame, nf);
                mgpu::diag::info(d);
                for (unsigned r0 = 0; r0 < nf; r0 += 16u)
                {
                    size_t n = (size_t)snprintf(d, sizeof d, "[MGPU][R259][CB-DUMP]   %3u:", r0);
                    for (unsigned i = r0; i < std::min(r0 + 16u, nf) && n + 16 < sizeof d; ++i)
                        n += (size_t)snprintf(d + n, sizeof d - n, " %.6g", fp[i]);
                    mgpu::diag::info(d);
                }
            }
            if (dumped == 0)
            {
                snprintf(l, sizeof l, "[MGPU][R259][CB-DUMP] f=%llu: no buffer met the camera shape (64..2048 bytes, <=8 writes/frame, in >=90%% of frames since its first write, changed on >=90%% of writes).", f);
                mgpu::diag::info(l);
            }
        }
    }
}

void init(unsigned screen_w, unsigned screen_h)
{
    if (g_on.load()) { g_w = screen_w; g_h = screen_h; return; }
    const bool census = (mgpu::gpu1::ui_ini_read("MVecCensus", 0) == 1);
    const bool tap    = (mgpu::gpu1::ui_ini_read("MVecTap", 0) == 1);   // R254
    // R278d: the lean tracking (resource create/destroy, render-target binds)
    // is on for EVERY D3D11 chain, so the first-launch learning can switch the
    // tap on live at arm (tap_enable_live) without registering an event from
    // a render thread (the D2.0 rule). Cost when nothing is on: one handler per
    // RT bind with an atomic, and nothing else - SK-30 measured the tap path
    // at the baseline. The reports and camera dumps stay behind MVecCensus=1.
    g_report.store(census);
    g_tap.store(tap);
    g_w = screen_w; g_h = screen_h;
    if (tap)
        mgpu::diag::info("[MGPU][R254] MVEC TAP ON (MVecTap=1): the game's own two-channel velocity target, found by the "
                         "census, is handed to the producer every frame it is bound. Rung 1 of the no-contract vector "
                         "ladder (DX11_CONTRACT_LEDGER.md section 13). MVec=3 in mgpu.ini is what makes the stream use it.");
    if (!census) { g_on.store(true); goto reg; }
    {
        char l[300];
        snprintf(l, sizeof l, "[MGPU][R253] MVEC CENSUS ON (MVecCensus=1): observe-only. Watching two-channel screen-sized render "
                              "targets (tap candidates) and CPU-written buffers (camera candidates) on the %ux%u D3D11 chain. "
                              "Report every 300 game frames; R259c camera dumps: three consecutive frames every 300 from 900.", screen_w, screen_h);
        mgpu::diag::info(l);
    }
reg:
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
    if (g_report.load())
    {
        // The buffer census is for the camera rung only; the tap does not need it.
        reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
        reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
        reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
    }
    g_on.store(true);
}

bool tap_on() { return g_tap.load(); }

// R278d: the first-launch learning turns the tap on for this session.
void tap_enable_live()
{
    if (!g_on.load() || g_tap.load()) return;
    g_tap.store(true);
    mgpu::diag::info("[MGPU][R278d] MVEC TAP ON (first-launch learning, no key): the game's two-channel velocity target, "
                     "found by the lean tracking, is handed to the producer every frame it is bound. The warp test judges it.");
}

unsigned long long tap_candidate(unsigned *w, unsigned *h, unsigned *fmt,
                                 unsigned long long *frames_bound, unsigned long long *frames_total)
{
    if (!g_tap.load()) return 0;
    std::lock_guard<std::mutex> lk(g_cs);
    const rt_entry *best = nullptr;
    for (const auto &e : g_rt)
    {
        // R254b: dllmain advances the frame (note_frame) before asking, so the
        // binds of the frame being finished carry g_frame - 1. Accept that
        // frame; anything older is a target the game stopped writing.
        if (!e.alive || e.last_frame == ~0ull || e.last_frame + 1ull < g_frame) continue;
        if (best == nullptr || e.frames_bound > best->frames_bound) best = &e;
    }
    if (best == nullptr) return 0;
    if (w) *w = best->w; if (h) *h = best->h; if (fmt) *fmt = best->fmt;
    if (frames_bound) *frames_bound = best->frames_bound;
    if (frames_total) *frames_total = (g_frame > best->created_frame) ? (g_frame - best->created_frame) : 0ull;
    return best->handle;
}

bool on() { return g_on.load(); }

void note_frame()
{
    if (!g_on.load()) return;
    unsigned long long f;
    {
        std::lock_guard<std::mutex> lk(g_cs);
        f = ++g_frame;
        g_bind_seq = 0;
    }
    if (g_report.load() && (f % 300ull) == 0ull) report(f);
    // R259c: a triple (f, f+1, f+2) every 300 frames from 900 on, so that
    // whichever 300-frame window holds the camera pan, one triple has it.
    // SK-33: the 900-902 triple fell on a still camera and the run ended
    // before 1800.
    if (g_report.load() && f >= 900ull && (f % 300ull) <= 2ull) camera_dump(f);
}

void shutdown()
{
    if (!g_on.load()) return;
    reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
    if (g_report.load())
    {
        reshade::unregister_event<reshade::addon_event::map_buffer_region>(on_map_buffer);
        reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer);
        reshade::unregister_event<reshade::addon_event::update_buffer_region>(on_update_buffer);
    }
    g_on.store(false);
}
}
