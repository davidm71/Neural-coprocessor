// HUD1 - the HUD-less frame search. See hud_probe.hpp.
//
// Compiled as part of probe.cpp (included at its end), so it shares that
// file's includes and its game-device test, and the build files do not change.
// Everything here is inert unless mgpu.ini has HudProbe=1: no event is even
// registered otherwise.

#include "hud_probe.hpp"

namespace mgpu { HMODULE module_handle(); }

namespace mgpu
{
namespace hudprobe
{
namespace
{

#define HUD_BUILD "HUD1.1 / HL1.1"

struct hcand
{
    unsigned long long handle;
    unsigned w, h, fmt;
    std::atomic<unsigned long long> bars;   // barriers seen on it, all time
    std::atomic<unsigned> last_old, last_new;
};

const unsigned HMAX = 128;
hcand g_t[HMAX];
std::atomic<unsigned> g_n{ 0 };
std::mutex g_lk;                             // table adds/evicts, and the capture frame

std::atomic<unsigned long long> g_bb[4];      // the swapchain's back buffers
std::atomic<bool> g_reg{ false };
std::atomic<bool> g_enabled{ false };
std::atomic<int> g_phase{ 0 };                // see hud_probe.hpp state()
std::atomic<int> g_req{ 0 };
unsigned long long g_frame = 0, g_wait_until = 0;
std::atomic<unsigned> g_cap_id{ 0 }, g_cap_images{ 0 };
unsigned g_scene_w_hp = 0, g_scene_h_hp = 0;
std::atomic<unsigned> g_bb_fmt{ 0 };

// ---- HL1: find the scene texture and copy it every frame ----
//
// Measured on RoboCop (capture 2026-10-10): the engine finishes the tone-
// mapped scene in a display-size texture of the back buffer's format, moves
// it render target -> shader resource, then draws it into the back buffer
// and the HUD on top. So: the last such texture to go RT -> SRV before the
// back buffer goes INTO render target is the HUD-less frame. Learned by
// vote, LOCKED once the same handle wins 60 frames running, then copied at
// its RT -> SRV barrier each frame through the hook dllmain installs.
typedef void (*hl_hook_fn)(void *cmd_list_native, unsigned long long resource);
hl_hook_fn g_hl_hook = nullptr;
std::atomic<bool> g_hl_on{ false };
std::atomic<unsigned long long> g_hl_last{ 0 };      // newest RT->SRV candidate (recording order)
std::atomic<unsigned long long> g_hl_vote{ 0 };      // candidate winning the vote
std::atomic<unsigned> g_hl_streak{ 0 };
std::atomic<unsigned long long> g_hl_lock{ 0 };      // the locked HUD-less texture
std::atomic<unsigned long long> g_hl_fires{ 0 };
std::atomic<unsigned long long> g_hl_fire_frame{ 0 };
std::atomic<unsigned long long> g_hl_unlocks{ 0 };
std::atomic<unsigned long long> g_frame_a{ 0 };

// One recorded event of the capture frame.
struct ev
{
    unsigned seq;
    unsigned tid;
    int idx;               // table index, or -1..-4 for back buffer 0..3
    unsigned w, h, fmt;
    unsigned old_s, new_s;
    int cap;               // capture slot, or -1
    int list_type;         // D3D12_COMMAND_LIST_TYPE, -1 unknown
    unsigned long long handle;
};
const unsigned EVMAX = 400;
ev g_ev[EVMAX];
unsigned g_evn = 0;
unsigned g_ev_dropped = 0;

struct capslot
{
    ID3D12Resource *rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 bytes;
    unsigned w, h, fmt;
    unsigned seq;
    char tag[96];
};
const unsigned CAPMAX = 24;
capslot g_cap[CAPMAX];
unsigned g_capn = 0;
UINT64 g_cap_bytes = 0;
const UINT64 CAP_BYTES_MAX = 512ull << 20;

// What we already captured this frame: (resource, old, new) - a pass that
// barriers the same texture the same way twice adds nothing.
struct seen3 { unsigned long long h; unsigned o, n; };
seen3 g_seen[CAPMAX];
unsigned g_seen_n = 0;

bool is_color_format(unsigned f)
{
    using reshade::api::format;
    switch (static_cast<format>(f))
    {
    case format::r8g8b8a8_typeless: case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb:
    case format::b8g8r8a8_typeless: case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
    case format::b8g8r8x8_typeless: case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb:
    case format::r10g10b10a2_typeless: case format::r10g10b10a2_unorm:
    case format::r11g11b10_float:
    case format::r16g16b16a16_typeless: case format::r16g16b16a16_float:
        return true;
    default:
        return false;
    }
}

const char *fmt_name(unsigned f)
{
    using reshade::api::format;
    switch (static_cast<format>(f))
    {
    case format::r8g8b8a8_typeless:     return "RGBA8_TYPELESS";
    case format::r8g8b8a8_unorm:        return "RGBA8_UNORM";
    case format::r8g8b8a8_unorm_srgb:   return "RGBA8_SRGB";
    case format::b8g8r8a8_typeless:     return "BGRA8_TYPELESS";
    case format::b8g8r8a8_unorm:        return "BGRA8_UNORM";
    case format::b8g8r8a8_unorm_srgb:   return "BGRA8_SRGB";
    case format::b8g8r8x8_typeless:     return "BGRX8_TYPELESS";
    case format::b8g8r8x8_unorm:        return "BGRX8_UNORM";
    case format::b8g8r8x8_unorm_srgb:   return "BGRX8_SRGB";
    case format::r10g10b10a2_typeless:  return "RGB10A2_TYPELESS";
    case format::r10g10b10a2_unorm:     return "RGB10A2_UNORM";
    case format::r11g11b10_float:       return "R11G11B10_FLOAT";
    case format::r16g16b16a16_typeless: return "RGBA16_TYPELESS";
    case format::r16g16b16a16_float:    return "RGBA16_FLOAT";
    default:                            return "other";
    }
}

// Short state name for the log. Values are reshade::api::resource_usage.
void usage_name(unsigned u, char *out, size_t n)
{
    if (u == 0x80000804u) { snprintf(out, n, "PRESENT"); return; }
    if (u == 0x80000000u) { snprintf(out, n, "GENERAL"); return; }
    if (u == 0u)          { snprintf(out, n, "UNDEF"); return; }
    out[0] = '\0';
    size_t w = 0;
    struct nm { unsigned bit; const char *s; };
    static const nm t[] = {
        { 0x4u, "RT" }, { 0x8u, "UAV" }, { 0xC0u, "SRV" }, { 0x80u, "SRVps" }, { 0x40u, "SRVcs" },
        { 0x800u, "COPYSRC" }, { 0x400u, "COPYDST" }, { 0x2000u, "RESOLVESRC" },
        { 0x1000u, "RESOLVEDST" }, { 0x30u, "DEPTH" }, { 0x20u, "DEPTHR" }, { 0x10u, "DEPTHW" },
    };
    unsigned left = u;
    for (const nm &e : t)
    {
        if ((left & e.bit) == e.bit && e.bit != 0)
        {
            w += (size_t)snprintf(out + w, n > w ? n - w : 0, "%s%s", w ? "|" : "", e.s);
            left &= ~e.bit;
        }
    }
    if (left != 0 || w == 0)
        snprintf(out + w, n > w ? n - w : 0, "%s0x%X", w ? "|" : "", left);
}

int find_hc(unsigned long long h)
{
    const unsigned n = g_n.load(std::memory_order_acquire);
    for (unsigned i = 0; i < n; ++i)
        if (g_t[i].handle == h) return (int)i;
    return -1;
}

int find_bb(unsigned long long h)
{
    for (int i = 0; i < 4; ++i)
        if (g_bb[i].load(std::memory_order_relaxed) == h) return i;
    return -1;
}

bool display_res(unsigned w, unsigned h)
{
    if (g_scene_w_hp == 0) return w >= 1280 && h >= 720;
    return w == g_scene_w_hp && h == g_scene_h_hp;
}

void on_init_resource_hp(reshade::api::device *device, const reshade::api::resource_desc &desc,
                         const reshade::api::subresource_data *, reshade::api::resource_usage,
                         reshade::api::resource resource)
{
    // 0.3.1 backport: D3D12 only. A D3D11 game streams through the DX11
    // producer's own D3D12 copies; its resources never reach this path.
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (desc.texture.samples > 1 || desc.texture.depth_or_layers > 1) return;
    const unsigned fmt = static_cast<unsigned>(desc.texture.format);
    if (!is_color_format(fmt)) return;
    if (!display_res(desc.texture.width, desc.texture.height)) return;
    if (desc.heap != reshade::api::memory_heap::default_) return;
    if (!mgpu::probe::is_game_device(device)) return;

    std::lock_guard<std::mutex> lk(g_lk);
    const unsigned n = g_n.load(std::memory_order_relaxed);
    int slot = -1;
    for (unsigned i = 0; i < n; ++i)
    {
        if (g_t[i].handle == resource.handle) return;
        if (slot < 0 && g_t[i].handle == 0) slot = (int)i;
    }
    if (slot < 0)
    {
        if (n >= HMAX) return;
        slot = (int)n;
    }
    hcand &c = g_t[slot];
    c.w = desc.texture.width; c.h = desc.texture.height; c.fmt = fmt;
    c.bars.store(0, std::memory_order_relaxed);
    c.last_old.store(0, std::memory_order_relaxed);
    c.last_new.store(0, std::memory_order_relaxed);
    c.handle = resource.handle;
    if ((unsigned)slot == n) g_n.store(n + 1, std::memory_order_release);
}

void on_destroy_resource_hp(reshade::api::device *, reshade::api::resource resource)
{
    if (resource.handle == 0) return;
    std::lock_guard<std::mutex> lk(g_lk);
    const unsigned n = g_n.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n; ++i)
        if (g_t[i].handle == resource.handle) g_t[i].handle = 0;
    // HL1: never copy from a freed texture.
    if (g_hl_lock.load() == resource.handle) { g_hl_lock.store(0); g_hl_unlocks.fetch_add(1); }
    if (g_hl_vote.load() == resource.handle) { g_hl_vote.store(0); g_hl_streak.store(0); }
    if (g_hl_last.load() == resource.handle) g_hl_last.store(0);
}

// Copies `res` (currently in reshade state `state`) into a new readback
// buffer. Game thread or one of the engine's recording threads; under g_lk.
int capture_one(reshade::api::command_list *cl, unsigned long long handle, unsigned state,
                unsigned w, unsigned h, unsigned fmt, unsigned seq, const char *tag)
{
    if (g_capn >= CAPMAX) return -1;
    ID3D12GraphicsCommandList *gl =
        reinterpret_cast<ID3D12GraphicsCommandList *>(static_cast<uintptr_t>(cl->get_native()));
    ID3D12Resource *src = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(handle));
    if (gl == nullptr || src == nullptr) return -1;
    if (gl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return -1;

    const D3D12_RESOURCE_DESC rd = src->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.SampleDesc.Count != 1) return -1;

    ID3D12Device *dev = nullptr;
    if (FAILED(src->GetDevice(IID_PPV_ARGS(&dev))) || dev == nullptr) return -1;

    capslot &c = g_cap[g_capn];
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &c.fp, nullptr, nullptr, &total);
    if (total == 0 || g_cap_bytes + total > CAP_BYTES_MAX) { dev->Release(); return -1; }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&rb));
    dev->Release();
    if (FAILED(hr) || rb == nullptr) return -1;

    // Into COPY_SOURCE and back. A state that already allows copying from it
    // needs nothing; PRESENT (D3D12 COMMON) is transitioned explicitly.
    using ru = reshade::api::resource_usage;
    const ru st = static_cast<ru>(state);
    const bool need = !((state & 0x800u) != 0u && state != 0x80000804u && state != 0x80000000u);
    const reshade::api::resource r = { handle };
    if (need) cl->barrier(r, st, ru::copy_source);

    D3D12_TEXTURE_COPY_LOCATION s{}, d{};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    d.pResource = rb;  d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;  d.PlacedFootprint = c.fp;
    gl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

    if (need) cl->barrier(r, ru::copy_source, st);

    c.rb = rb; c.bytes = total; c.w = w; c.h = h; c.fmt = fmt; c.seq = seq;
    snprintf(c.tag, sizeof c.tag, "%s", tag);
    g_cap_bytes += total;
    return (int)g_capn++;
}

// The content is FRESH at this barrier if the resource was just written
// (leaving a write state) or is about to be read (entering a read state).
bool fresh(unsigned o, unsigned n)
{
    const bool written = o == 0x4u || o == 0x8u || o == 0x400u || o == 0x1000u;
    const bool read    = (n & (0xC0u | 0x800u)) != 0u && n != 0x80000000u;
    return written || read;
}

void on_barrier_hp(reshade::api::command_list *cl, uint32_t count,
                   const reshade::api::resource *res,
                   const reshade::api::resource_usage *old_states,
                   const reshade::api::resource_usage *new_states)
{
    if (res == nullptr || count == 0 || cl == nullptr) return;
    if (cl->get_device() == nullptr || cl->get_device()->get_api() != reshade::api::device_api::d3d12) return;   // D3D12 only
    // Our own copy barriers must never come back through here holding g_lk.
    static thread_local bool t_in = false;
    if (t_in) return;
    struct guard { guard() { t_in = true; } ~guard() { t_in = false; } } gd;
    const bool rec = g_phase.load(std::memory_order_acquire) == 2;
    if (g_n.load(std::memory_order_relaxed) == 0 && !rec) return;

    for (uint32_t k = 0; k < count; ++k)
    {
        const unsigned long long h = res[k].handle;
        if (h == 0) continue;
        int idx = find_hc(h);
        int bbi = -1;
        const bool hl = g_hl_on.load(std::memory_order_relaxed);
        if (idx < 0)
        {
            if (!rec && !hl) continue;
            bbi = find_bb(h);
            if (bbi < 0) continue;
        }
        const unsigned o = old_states ? static_cast<unsigned>(old_states[k]) : 0u;
        const unsigned n = new_states ? static_cast<unsigned>(new_states[k]) : 0u;
        if (idx >= 0)
        {
            g_t[idx].bars.fetch_add(1, std::memory_order_relaxed);
            g_t[idx].last_old.store(o, std::memory_order_relaxed);
            g_t[idx].last_new.store(n, std::memory_order_relaxed);
        }
        if (hl)
        {
            // HL1. Candidate: display size, back-buffer format, render target
            // -> shader resource (just finished, about to be read).
            const bool cand = idx >= 0 && o == 0x4u && (n & 0xC0u) != 0u &&
                              n != 0x80000000u && n != 0x80000804u &&
                              g_t[idx].fmt == g_bb_fmt.load(std::memory_order_relaxed) &&
                              display_res(g_t[idx].w, g_t[idx].h);
            if (cand)
            {
                g_hl_last.store(h, std::memory_order_relaxed);
                if (h == g_hl_lock.load(std::memory_order_relaxed) && g_hl_hook != nullptr)
                {
                    ID3D12GraphicsCommandList *gl = reinterpret_cast<ID3D12GraphicsCommandList *>(
                        static_cast<uintptr_t>(cl->get_native()));
                    if (gl != nullptr && gl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                    {
                        using ru = reshade::api::resource_usage;
                        const reshade::api::resource r = { h };
                        cl->barrier(r, static_cast<ru>(n), ru::copy_source);
                        g_hl_hook(reinterpret_cast<void *>(gl), h);
                        cl->barrier(r, ru::copy_source, static_cast<ru>(n));
                        g_hl_fires.fetch_add(1, std::memory_order_relaxed);
                        g_hl_fire_frame.store(g_frame_a.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
                    }
                }
            }
            // The back buffer going INTO render target: whatever finished last
            // is the scene the game is about to draw into it.
            if (bbi >= 0 && o != 0x4u && n == 0x4u)
            {
                const unsigned long long c = g_hl_last.load(std::memory_order_relaxed);
                if (c != 0)
                {
                    if (c == g_hl_vote.load(std::memory_order_relaxed))
                    {
                        const unsigned st = g_hl_streak.fetch_add(1, std::memory_order_relaxed) + 1u;
                        if (st == 60u && g_hl_lock.load(std::memory_order_relaxed) != c)
                        {
                            // The 300-frame "not seen" timer starts NOW. Without
                            // this it measured from frame 0 and let go of the
                            // lock in the same frame it was taken (HL1.0 bug).
                            g_hl_fire_frame.store(g_frame_a.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
                            g_hl_lock.store(c, std::memory_order_relaxed);
                            char l[300];
                            snprintf(l, sizeof l,
                                     "[MGPU][HL1] LOCKED onto the HUD-less scene texture 0x%llx "
                                     "(won 60 frames running). It is copied to the neural card "
                                     "every frame from now on.", c);
                            mgpu::diag::info(l);
                        }
                    }
                    else
                    {
                        g_hl_vote.store(c, std::memory_order_relaxed);
                        g_hl_streak.store(1u, std::memory_order_relaxed);
                    }
                }
            }
        }
        if (!rec) continue;

        std::lock_guard<std::mutex> lk(g_lk);
        if (g_phase.load(std::memory_order_relaxed) != 2) return;
        if (idx >= 0 && g_t[idx].handle != h) continue;   // evicted meanwhile
        if (g_evn >= EVMAX) { ++g_ev_dropped; continue; }

        ev &e = g_ev[g_evn];
        e.seq = g_evn;
        e.tid = GetCurrentThreadId();
        e.idx = (idx >= 0) ? idx : -1 - bbi;
        e.handle = h;
        e.w = (idx >= 0) ? g_t[idx].w : g_scene_w_hp;
        e.h = (idx >= 0) ? g_t[idx].h : g_scene_h_hp;
        e.fmt = (idx >= 0) ? g_t[idx].fmt : g_bb_fmt.load(std::memory_order_relaxed);
        e.old_s = o; e.new_s = n;
        e.cap = -1;
        {
            ID3D12GraphicsCommandList *gl = reinterpret_cast<ID3D12GraphicsCommandList *>(
                static_cast<uintptr_t>(cl->get_native()));
            e.list_type = (gl != nullptr) ? (int)gl->GetType() : -1;
        }
        ++g_evn;

        if (!fresh(o, n) || n == 0u || e.list_type != (int)D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
        bool dup = false;
        for (unsigned i = 0; i < g_seen_n; ++i)
            if (g_seen[i].h == h && g_seen[i].o == o && g_seen[i].n == n) { dup = true; break; }
        if (dup) continue;
        if (g_seen_n < CAPMAX) g_seen[g_seen_n++] = seen3{ h, o, n };

        char so[48], sn[48], tag[96];
        usage_name(o, so, sizeof so); usage_name(n, sn, sizeof sn);
        if (idx >= 0) snprintf(tag, sizeof tag, "e%03u_T%d_%s_%s-%s", e.seq, idx, fmt_name(e.fmt), so, sn);
        else          snprintf(tag, sizeof tag, "e%03u_BB%d_%s-%s", e.seq, bbi, so, sn);
        for (char *p = tag; *p; ++p) if (*p == '|') *p = '+';
        e.cap = capture_one(cl, h, n, e.w, e.h, e.fmt, e.seq, tag);
    }
}

// ---- writing ----

float half_to_float(unsigned short v)
{
    const unsigned s = (v >> 15) & 1u, e = (v >> 10) & 31u, m = v & 1023u;
    float f;
    if (e == 0)       f = ldexpf((float)m, -24);
    else if (e == 31) f = 65504.0f;
    else              f = ldexpf((float)(m | 1024u), (int)e - 25);
    return s ? -f : f;
}

float uf_to_float(unsigned v, unsigned mbits)   // unsigned 5-bit-exponent mini float
{
    const unsigned e = (v >> mbits) & 31u, m = v & ((1u << mbits) - 1u);
    if (e == 0)  return ldexpf((float)m, -14 - (int)mbits);
    if (e == 31) return 65504.0f;
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - (int)mbits);
}

unsigned char tm8(float x)   // linear HDR -> display, for looking at only
{
    if (!(x > 0.0f)) return 0;
    x = x / (1.0f + x);
    x = powf(x, 1.0f / 2.2f);
    const int v = (int)(x * 255.0f + 0.5f);
    return (unsigned char)(v > 255 ? 255 : v);
}

// One pixel to 8-bit RGB + 8-bit alpha.
void px(const unsigned char *p, unsigned fmt, unsigned char *rgb, unsigned char &a)
{
    using reshade::api::format;
    switch (static_cast<format>(fmt))
    {
    case format::b8g8r8a8_typeless: case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
    case format::b8g8r8x8_typeless: case format::b8g8r8x8_unorm: case format::b8g8r8x8_unorm_srgb:
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; a = p[3]; return;
    case format::r10g10b10a2_typeless: case format::r10g10b10a2_unorm:
    {
        unsigned v; memcpy(&v, p, 4);
        rgb[0] = (unsigned char)((v & 1023u) >> 2);
        rgb[1] = (unsigned char)(((v >> 10) & 1023u) >> 2);
        rgb[2] = (unsigned char)(((v >> 20) & 1023u) >> 2);
        a = (unsigned char)(((v >> 30) & 3u) * 85u);
        return;
    }
    case format::r11g11b10_float:
    {
        unsigned v; memcpy(&v, p, 4);
        rgb[0] = tm8(uf_to_float(v & 2047u, 6));
        rgb[1] = tm8(uf_to_float((v >> 11) & 2047u, 6));
        rgb[2] = tm8(uf_to_float((v >> 22) & 1023u, 5));
        a = 255;
        return;
    }
    case format::r16g16b16a16_typeless: case format::r16g16b16a16_float:
    {
        unsigned short q[4]; memcpy(q, p, 8);
        rgb[0] = tm8(half_to_float(q[0])); rgb[1] = tm8(half_to_float(q[1]));
        rgb[2] = tm8(half_to_float(q[2]));
        float af = half_to_float(q[3]); af = af < 0 ? 0 : (af > 1 ? 1 : af);
        a = (unsigned char)(af * 255.0f + 0.5f);
        return;
    }
    default:   // RGBA8 family, and the back buffer when its format is unknown
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; a = p[3]; return;
    }
}

unsigned bpp(unsigned fmt)
{
    using reshade::api::format;
    switch (static_cast<format>(fmt))
    {
    case format::r16g16b16a16_typeless: case format::r16g16b16a16_float: return 8;
    default: return 4;
    }
}

bool write_bmp(const wchar_t *path, const unsigned char *rgb, unsigned w, unsigned h)
{
    FILE *fp = _wfopen(path, L"wb");
    if (fp == nullptr) return false;
    const unsigned row = (w * 3u + 3u) & ~3u;
    const unsigned size = 54u + row * h;
    unsigned char hd[54] = { 'B', 'M' };
    auto put32 = [&](int o, unsigned v) { hd[o] = (unsigned char)v; hd[o + 1] = (unsigned char)(v >> 8);
                                          hd[o + 2] = (unsigned char)(v >> 16); hd[o + 3] = (unsigned char)(v >> 24); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    hd[26] = 1; hd[28] = 24; put32(34, row * h);
    fwrite(hd, 1, 54, fp);
    unsigned char pad[4] = {};
    for (unsigned y = 0; y < h; ++y)
    {
        const unsigned char *s = rgb + (size_t)(h - 1u - y) * w * 3u;
        for (unsigned x = 0; x < w; ++x)
        {
            const unsigned char bgr[3] = { s[x * 3 + 2], s[x * 3 + 1], s[x * 3 + 0] };
            fwrite(bgr, 1, 3, fp);
        }
        fwrite(pad, 1, row - w * 3u, fp);
    }
    fclose(fp);
    return true;
}

struct job
{
    unsigned n;
    unsigned id;
    capslot cap[CAPMAX];
    wchar_t dir[MAX_PATH * 2];
};

DWORD WINAPI writer(void *p)
{
    job *j = static_cast<job *>(p);
    unsigned wrote = 0;
    for (unsigned i = 0; i < j->n; ++i)
    {
        capslot &c = j->cap[i];
        unsigned char *m = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)c.bytes };
        if (c.rb != nullptr && SUCCEEDED(c.rb->Map(0, &rr, (void **)&m)) && m != nullptr)
        {
            const unsigned hw = c.w / 2u, hh = c.h / 2u, b = bpp(c.fmt);
            unsigned char *rgb = (unsigned char *)malloc((size_t)hw * hh * 3u);
            unsigned char *al  = (unsigned char *)malloc((size_t)hw * hh * 3u);
            if (rgb != nullptr && al != nullptr)
            {
                unsigned amin = 255, amax = 0;
                double sum = 0.0;
                for (unsigned y = 0; y < hh; ++y)
                    for (unsigned x = 0; x < hw; ++x)
                    {
                        unsigned acc[3] = {}, aa = 0;
                        for (unsigned dy = 0; dy < 2; ++dy)
                            for (unsigned dx = 0; dx < 2; ++dx)
                            {
                                const unsigned char *s = m + c.fp.Offset +
                                    (size_t)(y * 2u + dy) * c.fp.Footprint.RowPitch +
                                    (size_t)(x * 2u + dx) * b;
                                unsigned char q[3], a;
                                px(s, c.fmt, q, a);
                                acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; aa += a;
                            }
                        unsigned char *o = rgb + ((size_t)y * hw + x) * 3u;
                        o[0] = (unsigned char)(acc[0] / 4u); o[1] = (unsigned char)(acc[1] / 4u);
                        o[2] = (unsigned char)(acc[2] / 4u);
                        const unsigned char av = (unsigned char)(aa / 4u);
                        unsigned char *oa = al + ((size_t)y * hw + x) * 3u;
                        oa[0] = oa[1] = oa[2] = av;
                        if (av < amin) amin = av;
                        if (av > amax) amax = av;
                        sum += (o[0] + o[1] + o[2]) / 3.0;
                    }
                wchar_t path[MAX_PATH * 3];
                _snwprintf_s(path, MAX_PATH * 3, _TRUNCATE, L"%scap%u_%02u_%S.bmp", j->dir, j->id, i, c.tag);
                const bool ok = write_bmp(path, rgb, hw, hh);
                bool aok = false;
                const bool alpha_matters = amax > amin + 8u &&
                    c.fmt != static_cast<unsigned>(reshade::api::format::r11g11b10_float);
                if (alpha_matters)
                {
                    _snwprintf_s(path, MAX_PATH * 3, _TRUNCATE, L"%scap%u_%02u_%S_ALPHA.bmp", j->dir, j->id, i, c.tag);
                    aok = write_bmp(path, al, hw, hh);
                }
                if (ok) ++wrote;
                char l[400];
                snprintf(l, sizeof l,
                         "[MGPU][HUD1] image %02u %s: %ux%u %s mean %.1f, alpha %u..%u -> %s%s",
                         i, c.tag, c.w, c.h, fmt_name(c.fmt), sum / ((double)hw * hh), amin, amax,
                         ok ? "written" : "WRITE FAILED", aok ? " (+ALPHA image)" : "");
                mgpu::diag::info(l);
            }
            free(rgb); free(al);
            D3D12_RANGE none{ 0, 0 };
            c.rb->Unmap(0, &none);
        }
        if (c.rb != nullptr) c.rb->Release();
    }
    char l[300];
    snprintf(l, sizeof l,
             "[MGPU][HUD1] capture %u done: %u image(s) in the mgpu\\hudprobe folder next to the add-on. "
             "Zip that folder and send it with ReShade.log.", j->id, wrote);
    mgpu::diag::info(l);
    g_cap_images.store(wrote, std::memory_order_relaxed);
    g_cap_id.store(j->id, std::memory_order_relaxed);
    delete j;
    g_phase.store(0, std::memory_order_release);
    return 0;
}

bool out_dir(wchar_t *dir, size_t n)
{
    const DWORD k = GetModuleFileNameW(mgpu::module_handle(), dir, (DWORD)n);
    if (k == 0 || k >= n) return false;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    wcsncat_s(dir, n, L"mgpu", _TRUNCATE);
    CreateDirectoryW(dir, nullptr);
    wcsncat_s(dir, n, L"\\hudprobe\\", _TRUNCATE);
    CreateDirectoryW(dir, nullptr);
    return true;
}

void log_events(unsigned id)
{
    char l[600];
    snprintf(l, sizeof l,
             "[MGPU][HUD1] capture %u, frame %llu: %u barrier(s) on display-res colour textures and the back "
             "buffer (%u not logged, table full), %u image(s) taken. Recording order, NOT GPU order: the engine "
             "records on several threads (tid). T<n> = texture n, BB<n> = swapchain buffer n.",
             id, g_frame, g_evn, g_ev_dropped, g_capn);
    mgpu::diag::info(l);
    for (unsigned i = 0; i < g_evn && i < 300u; ++i)
    {
        const ev &e = g_ev[i];
        char so[48], sn[48], who[16];
        usage_name(e.old_s, so, sizeof so); usage_name(e.new_s, sn, sizeof sn);
        if (e.idx >= 0) snprintf(who, sizeof who, "T%d", e.idx);
        else            snprintf(who, sizeof who, "BB%d", -1 - e.idx);
        snprintf(l, sizeof l, "[MGPU][HUD1]   e%03u tid=%u list=%d %s 0x%llx %ux%u %s %s -> %s%s",
                 e.seq, e.tid, e.list_type, who, e.handle, e.w, e.h,
                 e.idx >= 0 ? fmt_name(e.fmt) : "(swapchain)", so, sn,
                 e.cap >= 0 ? "  [IMAGE]" : "");
        mgpu::diag::info(l);
    }
}

void summary()
{
    char l[1400];
    int w = snprintf(l, sizeof l, "[MGPU][HUD1] %s display-res colour textures (%ux%u): ", HUD_BUILD,
                     g_scene_w_hp, g_scene_h_hp);
    const unsigned n = g_n.load(std::memory_order_acquire);
    unsigned live = 0;
    for (unsigned i = 0; i < n && w < (int)sizeof l - 120; ++i)
    {
        if (g_t[i].handle == 0) continue;
        ++live;
        if (!display_res(g_t[i].w, g_t[i].h)) continue;
        char so[48], sn[48];
        usage_name(g_t[i].last_old.load(std::memory_order_relaxed), so, sizeof so);
        usage_name(g_t[i].last_new.load(std::memory_order_relaxed), sn, sizeof sn);
        w += snprintf(l + w, sizeof l - (size_t)w, "T%u %s bars/frame=%.2f last %s->%s | ", i,
                      fmt_name(g_t[i].fmt),
                      g_frame ? (double)g_t[i].bars.load(std::memory_order_relaxed) / (double)g_frame : 0.0,
                      so, sn);
    }
    snprintf(l + w, sizeof l - (size_t)w, "(%u live). CTRL+ALT+J captures one frame.", live);
    mgpu::diag::info(l);
}

} // namespace

void start()
{
    if (g_reg.load()) return;
    static char buf[mgpu::config::MAX_BYTES + 1];
    if (!mgpu::probe::slurp(buf, sizeof buf)) return;
    const char *v = mgpu::config::find(buf, strlen(buf), "HudProbe");
    const char *hv = mgpu::config::find(buf, strlen(buf), "HudLess");
    const bool probe_on = v != nullptr && atoi(v) == 1;
    const bool hl_on = hv != nullptr && atoi(hv) == 1;
    if (!probe_on && !hl_on) return;
    g_hl_on.store(hl_on);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource_hp);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource_hp);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier_hp);
    g_reg.store(true);
    g_enabled.store(probe_on);
    if (hl_on)
        mgpu::diag::info("[MGPU][HL1] HudLess=1: looking for the game's HUD-less scene texture. "
                         "Expect a LOCKED line within a few seconds of gameplay.");
    if (probe_on) mgpu::diag::info("[MGPU][HUD1] " HUD_BUILD " HUD-less frame search ON (HudProbe=1). Catalogues display-resolution "
                     "colour textures; CTRL+ALT+J (or the panel button) saves one frame's worth of them as images to "
                     "mgpu\\hudprobe. Costs a table lookup per barrier while on. Set HudProbe=0 when done.");
}

void stop()
{
    if (!g_reg.load()) return;
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier_hp);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource_hp);
    reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource_hp);
    g_reg.store(false);
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_copy_hook(void (*fn)(void *cmd_list_native, unsigned long long resource))
{
    g_hl_hook = fn;
}

void request_capture()
{
    if (!enabled()) return;
    int z = 0;
    if (g_phase.compare_exchange_strong(z, 1)) g_req.store(1);
}

int state() { return g_phase.load(std::memory_order_relaxed); }
unsigned last_capture_id() { return g_cap_id.load(std::memory_order_relaxed); }
unsigned last_capture_images() { return g_cap_images.load(std::memory_order_relaxed); }

void note_frame(void *effect_runtime_ptr, void *command_list_ptr)
{
    if (!g_reg.load(std::memory_order_relaxed)) return;
    reshade::api::effect_runtime *rt = static_cast<reshade::api::effect_runtime *>(effect_runtime_ptr);
    reshade::api::command_list *cl = static_cast<reshade::api::command_list *>(command_list_ptr);
    if (rt == nullptr) return;
    // D3D12 game only: the HUD-less copy is recorded on the game's own D3D12
    // list. On a D3D11 game (DX11 producer) nothing here runs, so HudLess=1
    // simply never locks and frame generation uses the frame with the HUD.
    if (rt->get_device() == nullptr || rt->get_device()->get_api() != reshade::api::device_api::d3d12) return;
    ++g_frame;

    // Back buffers and the display size, from the game's own swapchain.
    const uint32_t nbb = rt->get_back_buffer_count();
    for (uint32_t i = 0; i < 4u; ++i)
        g_bb[i].store(i < nbb ? rt->get_back_buffer(i).handle : 0ull, std::memory_order_relaxed);
    reshade::api::resource cur_bb = rt->get_current_back_buffer();
    unsigned bb_fmt = 0;
    if (reshade::api::device *dv = rt->get_device())
    {
        const reshade::api::resource_desc d = dv->get_resource_desc(cur_bb);
        g_scene_w_hp = d.texture.width; g_scene_h_hp = d.texture.height;
        bb_fmt = static_cast<unsigned>(d.texture.format);
    }
    g_bb_fmt.store(bb_fmt, std::memory_order_relaxed);
    g_frame_a.store(g_frame, std::memory_order_relaxed);
    if (g_hl_on.load(std::memory_order_relaxed))
    {
        // A locked texture that has not been seen for 5 s (a menu, a level
        // change) is let go, and the vote starts again.
        const unsigned long long lk = g_hl_lock.load(std::memory_order_relaxed);
        if (lk != 0 && g_frame > g_hl_fire_frame.load(std::memory_order_relaxed) + 300ull)
        {
            g_hl_lock.store(0); g_hl_vote.store(0); g_hl_streak.store(0);
            g_hl_unlocks.fetch_add(1);
            mgpu::diag::info("[MGPU][HL1] scene texture not seen for 300 frames - unlocked, "
                             "looking again (frames run with the HUD in the meantime).");
        }
        if ((g_frame % 3600ull) == 1800ull)
        {
            char l[300];
            snprintf(l, sizeof l, "[MGPU][HL1] probe: locked=0x%llx, copies fired %llu, unlocks %llu.",
                     g_hl_lock.load(), g_hl_fires.load(), g_hl_unlocks.load());
            mgpu::diag::info(l);
        }
    }

    if ((g_frame % 3600ull) == 600ull) summary();

    const int ph = g_phase.load(std::memory_order_acquire);
    if (ph == 1)
    {
        std::lock_guard<std::mutex> lk(g_lk);
        g_evn = 0; g_ev_dropped = 0; g_capn = 0; g_cap_bytes = 0; g_seen_n = 0;
        g_phase.store(2, std::memory_order_release);
        mgpu::diag::info("[MGPU][HUD1] recording one frame ...");
    }
    else if (ph == 2)
    {
        std::lock_guard<std::mutex> lk(g_lk);
        // The finished frame with HUD (and ReShade effects), for comparison.
        // ReShade hands finish_effects the back buffer as a render target.
        if (cl != nullptr && cur_bb.handle != 0)
        {
            char tag[96];
            snprintf(tag, sizeof tag, "FINAL_with_HUD_%s", fmt_name(bb_fmt));
            (void)capture_one(cl, cur_bb.handle, 0x4u, g_scene_w_hp, g_scene_h_hp, bb_fmt, 999u, tag);
        }
        g_phase.store(3, std::memory_order_release);
        g_wait_until = g_frame + 6ull;
        log_events(g_cap_id.load() + 1u);
    }
    else if (ph == 3 && g_frame >= g_wait_until)
    {
        // The copies are in command lists the game has submitted by now; make
        // sure they have executed before anything is read. One stall, once.
        if (reshade::api::command_queue *q = rt->get_command_queue()) q->wait_idle();
        job *j = new job{};
        {
            std::lock_guard<std::mutex> lk(g_lk);
            j->n = g_capn;
            for (unsigned i = 0; i < g_capn; ++i) j->cap[i] = g_cap[i];
            g_capn = 0;
        }
        j->id = g_cap_id.load() + 1u;
        if (!out_dir(j->dir, MAX_PATH * 2))
        {
            mgpu::diag::warn("[MGPU][HUD1] cannot resolve the add-on folder; capture discarded.");
            for (unsigned i = 0; i < j->n; ++i) if (j->cap[i].rb) j->cap[i].rb->Release();
            delete j;
            g_phase.store(0);
            return;
        }
        g_phase.store(4, std::memory_order_release);
        HANDLE th = CreateThread(nullptr, 0, writer, j, 0, nullptr);
        if (th != nullptr) CloseHandle(th);
        else writer(j);
    }
}

} // namespace hudprobe
} // namespace mgpu
