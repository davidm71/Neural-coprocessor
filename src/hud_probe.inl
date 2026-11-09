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
    std::atomic<unsigned long long> seen;   // HL5: frame of its last barrier (table replacement)
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
std::atomic<unsigned long long> g_hl_fire_ms{ 0 };   // HL3: GetTickCount64 of the last copy
std::atomic<unsigned long long> g_hl_unlocks{ 0 };
// HL4: during action RoboCop moves its finished scene to another texture of
// the same size and format every few seconds (2026-11-06: 13 LOCKED lines in
// 3 minutes, 1382 of 6001 frames without the copy). Each move cost 60 frames
// (~1.7 s at 36 fps) before the new one was locked, and HL3 read the gap as a
// menu: no NR and no frame generation for that time. Once a lock has been
// copying recently, a replacement that wins 2 frames running is taken at once.
std::atomic<bool> g_hl_ever{ false };
std::atomic<unsigned long long> g_hl_relocks{ 0 };
// HL5: after the first lock, the copy FOLLOWS the scene's role instead of a
// handle: whatever display-size texture of the back buffer's format went
// render target -> shader resource last before the back buffer went into
// render target is this frame's HUD-less scene, and when it is not the
// locked one it is copied right there (its state is the one its own last
// barrier set, not a guess). 2026-11-06 (HL4 build): after a save was loaded
// nothing was locked for 54 s while the camera followed the mouse - no frame
// generation in that scene - which a game cycling its scene through several
// textures, or the 128-entry table running full, both explain; this covers
// the first, the table replacement below the second. HudFollow=0 turns the
// following off (the vote alone, as before).
bool g_hl_follow = true;
// HLD1: a one-time trace for the save that still flickered on HL5b (the
// follow still ran every other frame). Armed after 50 follows; then for 40
// frames every barrier on a display-size texture of the back buffer's format
// that enters or leaves render target, and every back-buffer transition, is
// written as one line per frame, in recording order. Logging only.
std::mutex g_dbg_mx;
std::atomic<int> g_dbg{ 0 };          // 0 idle, 1 recording, 2 done
unsigned g_dbg_frames = 0;
char g_dbg_line[1800];
int g_dbg_len = 0;
void dbg_add(const char *piece)
{
    std::lock_guard<std::mutex> lk(g_dbg_mx);
    const int room = (int)sizeof g_dbg_line - g_dbg_len;
    if (room < 64) return;
    g_dbg_len += snprintf(g_dbg_line + g_dbg_len, (size_t)room, "%s", piece);
}
std::atomic<unsigned long long> g_hl_follows{ 0 };
// HL5c: the HLD1 trace (2026-11-07) showed the flickering save renders with
// TWO complete sets of post-process textures (4 each) and switches between
// them (A A A B ...); each set's last texture is the scene. The HL5 follow
// copied at the back-buffer barrier - that copy was the one HL2 judged 100%
// different. Now every texture that has been the last candidate before the
// back buffer goes into render target joins a small set, and the copy runs
// at the texture's OWN render target -> shader resource barrier, exactly like
// the locked texture's copy has always done. A set member that is not the
// last this frame may be copied first; the real last one is copied after it
// on the same queue and wins.
const unsigned kSet = 4u;
std::atomic<unsigned long long> g_set[kSet];
std::atomic<unsigned long long> g_set_t[kSet];   // frame it was last seen as the scene
bool in_set(unsigned long long h)
{
    for (unsigned i = 0; i < kSet; ++i)
        if (g_set[i].load(std::memory_order_relaxed) == h) return true;
    return false;
}
bool banned(unsigned long long h);   // HL5d, below
void set_note(unsigned long long h, unsigned long long frame)
{
    if (banned(h)) return;   // HL5d
    unsigned lru = 0; unsigned long long lt = ~0ull;
    for (unsigned i = 0; i < kSet; ++i)
    {
        if (g_set[i].load(std::memory_order_relaxed) == h) { g_set_t[i].store(frame); return; }
        const unsigned long long t = g_set[i].load() == 0 ? 0ull : g_set_t[i].load() + 1ull;
        if (t < lt) { lt = t; lru = i; }
    }
    g_set[lru].store(h); g_set_t[lru].store(frame);
    const unsigned long long nf = g_hl_follows.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (nf <= 4u)
    {
        char l[220];
        snprintf(l, sizeof l, "[MGPU][HL5] texture 0x%llx holds the game's scene - added to the scene set "
                 "(%llu added so far; copied at its own barrier from now on).", h, nf);
        mgpu::diag::info(l);
    }
}
// HL5d: UI2 (one locked texture, set-B frames without a copy) shows that
// save cleanly, so set B's picture may simply not be the scene. The menu
// check reports each copy's source: a texture whose copies differ by more
// than 25% twice, more often than they match, is banned - its frames run
// with the HUD in, as UI2 did. Bans are kept until the texture is destroyed.
const unsigned kBan = 8u;
std::atomic<unsigned long long> g_ban[kBan];
std::atomic<unsigned> g_ban_n{ 0 };
struct copy_score { std::atomic<unsigned long long> h; std::atomic<unsigned> bad, good; };
copy_score g_score[kSet * 2];
bool banned(unsigned long long h)
{
    if (h == 0) return false;
    for (unsigned i = 0; i < kBan; ++i) if (g_ban[i].load(std::memory_order_relaxed) == h) return true;
    return false;
}
void set_remove(unsigned long long h);
void ban(unsigned long long h, unsigned bad, unsigned good)
{
    if (banned(h)) return;
    const unsigned i = g_ban_n.fetch_add(1, std::memory_order_relaxed) % kBan;
    g_ban[i].store(h, std::memory_order_relaxed);
    set_remove(h);
    if (g_hl_lock.load() == h) g_hl_lock.store(0);
    char l[260];
    snprintf(l, sizeof l, "[MGPU][HL5] texture 0x%llx does not hold the scene (its copies differed from the "
             "frame %u times, matched %u) - not copied from now on; those frames run with the HUD in.", h, bad, good);
    mgpu::diag::info(l);
}
void set_remove(unsigned long long h)
{
    for (unsigned i = 0; i < kSet; ++i)
        if (g_set[i].load(std::memory_order_relaxed) == h) g_set[i].store(0);
}
unsigned set_size()
{
    unsigned k = 0;
    for (unsigned i = 0; i < kSet; ++i) if (g_set[i].load(std::memory_order_relaxed) != 0) ++k;
    return k;
}
std::atomic<unsigned long long> g_t_replaced{ 0 };
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
    if (slot < 0 && n >= HMAX)
    {
        // HL5: full. Reuse the entry whose last barrier is oldest (never the
        // locked, voted or newest candidate) instead of ignoring every new
        // texture from here on.
        const unsigned long long keep1 = g_hl_lock.load(), keep2 = g_hl_vote.load(), keep3 = g_hl_last.load();
        unsigned long long best = ~0ull;
        for (unsigned i = 0; i < n; ++i)
        {
            const unsigned long long hh = g_t[i].handle;
            if (hh == keep1 || hh == keep2 || hh == keep3) continue;
            const unsigned long long sn = g_t[i].seen.load(std::memory_order_relaxed);
            if (sn < best) { best = sn; slot = (int)i; }
        }
        if (slot < 0) return;
        if (g_t_replaced.fetch_add(1, std::memory_order_relaxed) == 0)
            mgpu::diag::info("[MGPU][HL5] the texture table is full (128) - the least recently used entries are "
                             "replaced from now on (counted in the [MGPU][HL1] probe line).");
    }
    if (slot < 0)
    {
        slot = (int)n;
    }
    hcand &c = g_t[slot];
    c.w = desc.texture.width; c.h = desc.texture.height; c.fmt = fmt;
    c.bars.store(0, std::memory_order_relaxed);
    c.last_old.store(0, std::memory_order_relaxed);
    c.last_new.store(0, std::memory_order_relaxed);
    c.seen.store(g_frame_a.load(std::memory_order_relaxed), std::memory_order_relaxed);   // HL5
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
    set_remove(resource.handle);   // HL5c: an address can be reused by a new texture
    for (unsigned i = 0; i < kBan; ++i)   // HL5d: and a banned address may come back as a new texture
        if (g_ban[i].load() == resource.handle) g_ban[i].store(0);
    for (auto &sc : g_score) if (sc.h.load() == resource.handle) sc.h.store(0);
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

// HL5: copy c (this frame's HUD-less scene, not the locked texture) on the
// list that is moving the back buffer into render target, and lock onto it.
// Its state is the one its own last barrier set; anything that is not a
// shader-read state is left alone.
void follow_copy(reshade::api::command_list *cl, unsigned long long c)
{
    if (g_hl_hook == nullptr) return;
    const int ci = find_hc(c);
    if (ci < 0) return;
    const unsigned cs = g_t[ci].last_new.load(std::memory_order_relaxed);
    if ((cs & 0xC0u) == 0u || cs == 0x80000000u || cs == 0x80000804u) return;
    ID3D12GraphicsCommandList *gl = reinterpret_cast<ID3D12GraphicsCommandList *>(
        static_cast<uintptr_t>(cl->get_native()));
    if (gl == nullptr || gl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    using ru = reshade::api::resource_usage;
    const reshade::api::resource r = { c };
    cl->barrier(r, static_cast<ru>(cs), ru::copy_source);
    g_hl_hook(reinterpret_cast<void *>(gl), c);
    cl->barrier(r, ru::copy_source, static_cast<ru>(cs));
    g_hl_fires.fetch_add(1, std::memory_order_relaxed);
    g_hl_fire_frame.store(g_frame_a.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_hl_fire_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_hl_lock.store(c, std::memory_order_relaxed);
    g_hl_vote.store(c, std::memory_order_relaxed);
    const unsigned long long nf = g_hl_follows.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (g_dbg.load(std::memory_order_relaxed) == 1)
    {
        char pc[48]; snprintf(pc, sizeof pc, " FOLLOW>%llx", c & 0xFFFFFull); dbg_add(pc);
    }
    if (nf == 50u)   // HLD1: arm the trace once
    {
        { std::lock_guard<std::mutex> lk(g_dbg_mx); g_dbg_len = 0; g_dbg_line[0] = 0; g_dbg_frames = 0; }
        int expect = 0;
        if (g_dbg.compare_exchange_strong(expect, 1))
            mgpu::diag::info("[MGPU][HLD1] scene texture trace: the next 40 frames, one line each "
                             "(handle:old>new state, * = candidate, /t = thread; BB = back buffer).");
    }
    if (nf <= 3u)
    {
        char l[240];
        snprintf(l, sizeof l, "[MGPU][HL5] the game's scene is in texture 0x%llx this frame - copied from there "
                 "and followed (%llu so far; later ones are counted in the probe line).", c, nf);
        mgpu::diag::info(l);
    }
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
            g_t[idx].seen.store(g_frame_a.load(std::memory_order_relaxed), std::memory_order_relaxed);   // HL5
        }
        if (hl)
        {
            // HL1. Candidate: display size, back-buffer format, render target
            // -> shader resource (just finished, about to be read).
            const bool cand = idx >= 0 && o == 0x4u && (n & 0xC0u) != 0u &&
                              n != 0x80000000u && n != 0x80000804u &&
                              g_t[idx].fmt == g_bb_fmt.load(std::memory_order_relaxed) &&
                              display_res(g_t[idx].w, g_t[idx].h);
            if (g_dbg.load(std::memory_order_relaxed) == 1 && idx >= 0 && (o == 0x4u || n == 0x4u) &&
                g_t[idx].fmt == g_bb_fmt.load(std::memory_order_relaxed) && display_res(g_t[idx].w, g_t[idx].h))
            {
                char pc[64];
                snprintf(pc, sizeof pc, " %llx:%x>%x%s/t%lu", h & 0xFFFFFull, o, n, cand ? "*" : "",
                         (unsigned long)(GetCurrentThreadId() % 100000u));
                dbg_add(pc);   // HLD1
            }
            if (cand)
            {
                g_hl_last.store(h, std::memory_order_relaxed);
                if ((h == g_hl_lock.load(std::memory_order_relaxed) ||
                     (g_hl_follow && in_set(h))) && !banned(h) && g_hl_hook != nullptr)   // HL5c/d
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
                        g_hl_fire_ms.store(GetTickCount64(), std::memory_order_relaxed);   // HL3
                    }
                }
            }
            // The back buffer going INTO render target: whatever finished last
            // is the scene the game is about to draw into it.
            if (bbi >= 0 && g_dbg.load(std::memory_order_relaxed) == 1)   // HLD1
            {
                char pc[96];
                snprintf(pc, sizeof pc, " | BB%d:%x>%x last=%llx lock=%llx fired=%d", bbi, o, n,
                         g_hl_last.load() & 0xFFFFFull, g_hl_lock.load() & 0xFFFFFull,
                         g_hl_fire_frame.load() == g_frame_a.load() ? 1 : 0);
                dbg_add(pc);
            }
            if (bbi >= 0 && o != 0x4u && n == 0x4u)
            {
                const unsigned long long c = g_hl_last.load(std::memory_order_relaxed);
                if (c != 0 && g_hl_follow && g_hl_ever.load(std::memory_order_relaxed))
                {
                    // HL5: follow the role. When c is the locked texture its
                    // copy already ran at its own barrier this frame.
                    // HL5b: and only when the locked texture was NOT copied
                    // this frame. One save (2026-11-06) drew a second
                    // display-size texture after the scene every other frame;
                    // following it copied a non-scene image half the time and
                    // HL2 flipped "menu" on and off every 2 frames (severe
                    // distortion). A lock that is still being drawn each frame
                    // is kept; only a lock that has stopped is replaced.
                    // HL5c: no copy here any more. c joins the scene set and is
                    // copied at its own barrier from the next time it is drawn.
                    set_note(c, g_frame_a.load(std::memory_order_relaxed));
                    if (!banned(c)) g_hl_lock.store(c, std::memory_order_relaxed);   // HL5d
                    (void)&follow_copy;
                }
                else if (c != 0)
                {
                    if (c == g_hl_vote.load(std::memory_order_relaxed))
                    {
                        const unsigned st = g_hl_streak.fetch_add(1, std::memory_order_relaxed) + 1u;
                        // HL4: 2 frames when a lock was copying within the last 3 s.
                        const bool fast = g_hl_ever.load(std::memory_order_relaxed) &&
                                          GetTickCount64() - g_hl_fire_ms.load(std::memory_order_relaxed) < 3000ull;
                        const unsigned need = fast ? 2u : 60u;
                        if (st >= need && g_hl_lock.load(std::memory_order_relaxed) != c)
                        {
                            // The 300-frame "not seen" timer starts NOW. Without
                            // this it measured from frame 0 and let go of the
                            // lock in the same frame it was taken (HL1.0 bug).
                            g_hl_fire_frame.store(g_frame_a.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
                            g_hl_lock.store(c, std::memory_order_relaxed);
                            const bool first = !g_hl_ever.exchange(true, std::memory_order_relaxed);
                            char l[300];
                            if (fast && !first)
                            {
                                // HL4: one line for the first few, then every 25th.
                                const unsigned long long r = g_hl_relocks.fetch_add(1, std::memory_order_relaxed) + 1u;
                                if (r <= 3u || r % 25u == 0u)
                                {
                                    snprintf(l, sizeof l,
                                             "[MGPU][HL4] the game moved its scene to texture 0x%llx - followed "
                                             "after 2 frames (%llu move(s) followed so far).", c, r);
                                    mgpu::diag::info(l);
                                }
                            }
                            else
                            {
                                snprintf(l, sizeof l,
                                         "[MGPU][HL1] LOCKED onto the HUD-less scene texture 0x%llx "
                                         "(won %u frames running). It is copied to the neural card "
                                         "every frame from now on.", c, need);
                                mgpu::diag::info(l);
                            }
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
    const char *fv = mgpu::config::find(buf, strlen(buf), "HudFollow");   // HL5
    g_hl_follow = !(fv != nullptr && atoi(fv) == 0);
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

void report_copy(unsigned long long h, double frac)   // HL5d: from the menu check
{
    if (h == 0 || !g_hl_follow) return;
    copy_score *sc = nullptr, *free_sc = nullptr;
    for (auto &x : g_score)
    {
        const unsigned long long xh = x.h.load();
        if (xh == h) { sc = &x; break; }
        if (xh == 0 && free_sc == nullptr) free_sc = &x;
    }
    if (sc == nullptr)
    {
        if (free_sc == nullptr) { free_sc = &g_score[0]; }   // full: reuse the first
        free_sc->h.store(h); free_sc->bad.store(0); free_sc->good.store(0);
        sc = free_sc;
    }
    // A real menu makes EVERY texture's copy differ, so a bad verdict only
    // counts against h when the verdict before it was for another texture and
    // matched - h is the odd one out, not the screen.
    static unsigned long long last_h = 0;
    static double last_frac = 1.0;
    if (frac > 0.25) { if (last_h != h && last_frac < 0.12) sc->bad.fetch_add(1); }
    else if (frac < 0.12) sc->good.fetch_add(1);
    last_h = h; last_frac = frac;
    const unsigned b = sc->bad.load(), g = sc->good.load();
    if (b >= 2u && b > g) ban(h, b, g);
}

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

unsigned scene_gap_ms()
{
    if (!g_hl_on.load(std::memory_order_relaxed)) return 0;
    const unsigned long long t = g_hl_fire_ms.load(std::memory_order_relaxed);
    if (t == 0) return 0;   // never copied yet: start-up runs as before
    const unsigned long long now = GetTickCount64();
    if (now <= t) return 0;
    const unsigned long long d = now - t;
    return d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)d;
}
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
    if (g_dbg.load(std::memory_order_relaxed) == 1)   // HLD1: one line per frame
    {
        char l[1900];
        {
            std::lock_guard<std::mutex> lk(g_dbg_mx);
            snprintf(l, sizeof l, "[MGPU][HLD1] f%llu:%s", g_frame, g_dbg_line);
            g_dbg_len = 0; g_dbg_line[0] = 0;
            ++g_dbg_frames;
        }
        mgpu::diag::info(l);
        if (g_dbg_frames >= 40u) g_dbg.store(2);
    }
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
            snprintf(l, sizeof l, "[MGPU][HL1] probe: locked=0x%llx, copies fired %llu, unlocks %llu, moves followed "
                     "%llu (2-frame), scene set %u (%llu added), table %u of %u, entries replaced %llu.",
                     g_hl_lock.load(), g_hl_fires.load(), g_hl_unlocks.load(), g_hl_relocks.load(),
                     set_size(), g_hl_follows.load(), g_n.load(), HMAX, g_t_replaced.load());
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
