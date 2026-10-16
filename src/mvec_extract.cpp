// mvec_extract.cpp - R280. See mvec_extract.hpp and MVEC_D3D12_LEDGER.md.
//
// Thread notes. The barrier event fires on whatever thread the engine records
// on; the staging copy is taken with one compare-exchange (one thread wins
// per frame) and records only barriers and a copy on the engine's list.
// finish_effects runs on the game's render thread: our own list is recorded
// and executed there, once per frame, with our own objects. One mutex guards
// the candidate table; the report lines go through mgpu::diag (the queue).

#include "mvec_extract.hpp"
#include "diag.hpp"
#include "gpu1_context.hpp"   // ui_ini_read, ui_ini_write
#include "calibrator.hpp"     // game_ngx_init_seen: a contract title never enters
#include "probe.hpp"          // mvec_offer / mvec_defer (R280: two additive calls)

#include <reshade.hpp>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mgpu::mvec_extract
{
namespace
{
    // ---- the candidate table (the R279 signature, four-channel only) ----
    struct entry
    {
        unsigned long long handle = 0;
        unsigned w = 0, h = 0, fmt = 0;
        unsigned long long born = 0, died = 0, to_rt = 0, to_uav = 0, to_srv = 0, last_state = 0;
        unsigned long long win_w = 0, win_s = 0, streak = 0;   // writes/reads at the last check, consecutive good windows
    };
    const unsigned MAX = 64;
    entry g_t[MAX];
    std::mutex g_cs;

    std::atomic<bool> g_on{false};        // registered
    std::atomic<bool> g_live{true};       // not inert (no contract seen, not depth-only)
    void *g_dev = nullptr;                // the game's native device (identity)
    ID3D12Device *g_d3d = nullptr;        // the same, as D3D12
    unsigned g_w = 0, g_h = 0;
    std::atomic<unsigned long long> g_frame{0};
    int g_want_fmt = 0;                   // MVecExtract=<fmt> (learned) or 0 (decide by signature)

    // ---- the chosen source and the per-frame hand-off ----
    std::atomic<unsigned long long> g_src{0};     // the G-buffer target we extract from
    unsigned g_src_fmt = 0;
    bool g_src_rt = true;                 // the source is written as RT (copy at RT->SRV) or only as UAV (UAV->SRV)
    bool g_deferring = false;             // the probe holds its pick for us (released at build, give-up or inert)
    unsigned g_active_windows = 0;        // R280b: windows in which SOME candidate was written - the give-up clock (not wall frames)
    bool g_last_written = false;          // R280j: the last decision window saw a candidate written
    reshade::api::device *g_api_dev = nullptr;   // ReShade's device, for destroy_resource at shutdown
    // R280p (0.3.1): a target that leaves the transport (inert, resize) is WITHDRAWN at
    // once and DESTROYED later. Its last copy into the slot (the R277 tap, on ReShade's
    // list) can still be on the game's queue in the frame that tears it down, and
    // release_objects() waits only for our own list. GRAVE_FRAMES rendered frames is
    // past any frame latency the swap chain allows. Game render thread only.
    const unsigned long long GRAVE_FRAMES = 8ull;
    struct grave { reshade::api::resource r; unsigned long long due; };
    grave g_grave[4] = {};
    std::atomic<unsigned long long> g_staged_frame{~0ull};   // frame whose staging copy was recorded
    std::atomic<unsigned long long> g_copy_taken{0};        // frame of the last copy (R280d: every edge, last wins)
    std::atomic<unsigned> g_edges_frame{0};                  // R280d: copy edges seen this frame
    std::atomic<int> g_edge_first{0};                        // R280e: the judge's choice - 1 = hold the FIRST write edge of the frame, 0 = the last
    unsigned long long g_hist[3] = {};                       // R280d: frames with 1 / 2 / 3+ edges (finish_effects thread only)

    // ---- our objects on the game device ----
    ID3D12Resource *g_staging = nullptr;      // same format as the source, scene size
    ID3D12Resource *g_target = nullptr;       // R16G16_FLOAT, scene size: the virtual velocity target
    reshade::api::resource g_target_api = { 0 };
    ID3D12RootSignature *g_rs = nullptr;
    ID3D12PipelineState *g_pso = nullptr;
    ID3D12DescriptorHeap *g_heap = nullptr;
    ID3D12CommandAllocator *g_alloc[2] = {};
    ID3D12GraphicsCommandList *g_list = nullptr;
    ID3D12Fence *g_fence = nullptr;
    HANDLE g_fence_ev = nullptr;
    unsigned long long g_fence_v = 0;
    unsigned g_parity = 0;
    bool g_objects_ok = false, g_objects_tried = false;
    D3D12_RESOURCE_STATES g_target_state = D3D12_RESOURCE_STATE_COMMON;
    unsigned long long g_extracted = 0;
    bool g_zeroed = false;                // R280g: the target holds zeros (no source write this frame)
    // R280h: a size change is a PENDING reset that runs only once the new size has
    // been in force for RESET_SETTLE frames; a return to the old size cancels it.
    // RESET_MAX resets in a session = a title that recreates its chain at changing
    // sizes: extraction goes dormant for the session (no cost from there).
    // R280i: the bound is a 5-minute WINDOW, not the session. 4 resets inside 5 minutes
    // -> dormant for 5 minutes (no lock, no copy, no compute, no decision), then back.
    // 4 dormancies in a session -> off for the session. A player trying a few
    // resolutions over an evening never hits it; a title that thrashes pays at most
    // 4 rebuilds per 5 minutes, then nothing.
    const unsigned RESET_SETTLE = 120u, RESET_MAX = 4u, DORMANT_MAX = 4u;
    const unsigned long long WINDOW_MS = 5ull * 60ull * 1000ull;
    unsigned g_pend_w = 0, g_pend_h = 0;
    unsigned long long g_pend_since = 0;
    unsigned long long g_reset_ms[4] = {};      // tick of each reset in the window (ring)
    unsigned g_reset_n = 0, g_reset_i = 0;      // resets recorded, ring index
    unsigned g_cur_w = 0, g_cur_h = 0;          // the chain's size as last seen by init (also while dormant)
    unsigned long long g_dormant_until = 0;     // tick the dormancy lifts (0 = not dormant)
    unsigned g_dormancies = 0;
    bool g_dormant = false;                     // off for the session (DORMANT_MAX reached)

    const char *kExtractHLSL =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<float2> dst : register(u0);\n"
        "cbuffer C : register(b0) { uint W; uint H; uint pad0; uint pad1; };\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    if (id.x >= W || id.y >= H) return;\n"
        "    float4 v = src.Load(int3(id.xy, 0));\n"   // SNORM/UNORM/FLOAT decoded by the view
        "    dst[id.xy] = (pad0 != 0u) ? float2(0.0, 0.0) : v.xy;\n"
        "}\n";

    bool is_game(reshade::api::device *d)
    {
        return d != nullptr && reinterpret_cast<void *>(static_cast<uintptr_t>(d->get_native())) == g_dev;
    }
    bool four_ch16(unsigned f) { return f == 10 || f == 11 || f == 13; }   // RGBA16 FLOAT / UNORM / SNORM

    DXGI_FORMAT srv_format(unsigned f)
    {
        return f == 10 ? DXGI_FORMAT_R16G16B16A16_FLOAT : (f == 11 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_SNORM);
    }

    entry *find(unsigned long long h)
    {
        for (unsigned i = 0; i < MAX; ++i) if (g_t[i].handle == h && g_t[i].died == 0) return &g_t[i];
        return nullptr;
    }

    // ---- events ----
    void on_init_resource(reshade::api::device *device, const reshade::api::resource_desc &desc,
                          const reshade::api::subresource_data *, reshade::api::resource_usage,
                          reshade::api::resource res)
    {
        if (!g_live.load(std::memory_order_relaxed) || g_dormant_until != 0 || !is_game(device)) return;   // R280i
        if (desc.type != reshade::api::resource_type::texture_2d) return;
        const unsigned fmt = static_cast<unsigned>(desc.texture.format);
        // R280l: admitted within the scene band (half to full of the chain, any order of
        // creation vs. chain resize - RE4-X7); the DECISION requires the exact scene size.
        if (!four_ch16(fmt) || g_w == 0 || g_h == 0) return;
        if (desc.texture.width * 2u < g_w || desc.texture.width > g_w + g_w / 50u || desc.texture.height * 2u < g_h || desc.texture.height > g_h + g_h / 50u) return;
        std::lock_guard<std::mutex> lk(g_cs);
        entry *slot = nullptr;
        for (unsigned i = 0; i < MAX && slot == nullptr; ++i) if (g_t[i].handle == 0 || g_t[i].died != 0) slot = &g_t[i];
        if (slot == nullptr) return;
        *slot = entry{};
        slot->handle = res.handle; slot->w = desc.texture.width; slot->h = desc.texture.height; slot->fmt = fmt;
        slot->born = g_frame.load(std::memory_order_relaxed);
    }

    void on_destroy_resource(reshade::api::device *device, reshade::api::resource res)
    {
        if (!is_game(device)) return;
        std::lock_guard<std::mutex> lk(g_cs);
        if (entry *e = find(res.handle)) e->died = g_frame.load(std::memory_order_relaxed) + 1;
        if (g_src.load(std::memory_order_relaxed) == res.handle) g_src.store(0, std::memory_order_relaxed);
    }

    void on_barrier(reshade::api::command_list *cl, uint32_t count, const reshade::api::resource *res,
                    const reshade::api::resource_usage *olds, const reshade::api::resource_usage *news)
    {
        if (!g_live.load(std::memory_order_relaxed) || g_dormant_until != 0) return;   // R280i: dormant = first-load return
        if (cl == nullptr || res == nullptr || olds == nullptr || news == nullptr || !is_game(cl->get_device())) return;
        using ru = reshade::api::resource_usage;
        const unsigned uav = static_cast<unsigned>(ru::unordered_access);
        const unsigned srv = static_cast<unsigned>(ru::shader_resource);
        const unsigned rt  = static_cast<unsigned>(ru::render_target);
        const unsigned long long f = g_frame.load(std::memory_order_relaxed);
        const unsigned long long src = g_src.load(std::memory_order_relaxed);

        // ---- the staging copy: at EVERY RT -> SRV (or UAV -> SRV) edge of the source ----
        // R280d. RE4-X1/X2: in some scenes the engine writes this target twice per
        // frame (TC1 report 3: 1.4 RT writes per frame) and the first edge is not the
        // velocity pass - GPU 1 got a fixed pattern (R257: identical statistics in two
        // runs) and the picture strobed in exactly those scenes. The copy is taken at
        // every edge, last one wins: at finish_effects the staging holds the frame's
        // last write. One scene-size copy per edge, GPU 0 only.
        if (src != 0 && g_staging != nullptr)
            for (uint32_t k = 0; k < count; ++k)
            {
                if (res[k].handle != src) continue;
                const unsigned o = static_cast<unsigned>(olds[k]), n = static_cast<unsigned>(news[k]);
                if ((n & srv) == 0) continue;
                if (g_src_rt ? (o & rt) == 0 : (o & uav) == 0) continue;
                g_edges_frame.fetch_add(1u, std::memory_order_relaxed);
                // R280e: with the judge asking for the FIRST edge, later edges of the same
                // frame are counted but not copied.
                if (g_edge_first.load(std::memory_order_relaxed) != 0 && g_copy_taken.load(std::memory_order_relaxed) == f) continue;
                const reshade::api::resource s_api = { src };
                cl->barrier(s_api, news[k], ru::copy_source);
                cl->copy_resource(s_api, reshade::api::resource{ reinterpret_cast<uint64_t>(g_staging) });
                cl->barrier(s_api, ru::copy_source, news[k]);
                g_copy_taken.store(f, std::memory_order_relaxed);
                g_staged_frame.store(f, std::memory_order_release);
            }

        // ---- the census: usage counts for the signature ----
        // R280h: only while no source is decided. Decided, the counts serve nothing and
        // the mutex is the one lock this module took on the game's threads per barrier
        // call; it resumes by itself if the source is destroyed or a reset forgets it.
        if (src != 0) return;
        std::lock_guard<std::mutex> lk(g_cs);
        for (uint32_t k = 0; k < count; ++k)
        {
            entry *e = find(res[k].handle);
            if (e == nullptr) continue;
            const unsigned o = static_cast<unsigned>(olds[k]), n = static_cast<unsigned>(news[k]);
            if ((n & rt) != 0 && (o & rt) == 0) ++e->to_rt;
            if ((n & uav) != 0 && (o & uav) == 0) ++e->to_uav;
            if ((n & srv) != 0 && ((o & rt) != 0 || (o & uav) != 0)) ++e->to_srv;
            e->last_state = n;
        }
    }

    // The signature, checked every 30 frames: four-channel 16-bit, scene size,
    // written (RT or UAV) at least 24 times in the window (>= 0.8 per frame)
    // and read back as SRV at least 40% as often, for 10 consecutive windows
    // (300 frames of gameplay - a menu or a load does not count, a long menu
    // before gameplay does not dilute it). With MVecExtract=<fmt> learned,
    // only that format and 2 windows. Most written wins among equals.
    bool decide_source(unsigned long long f)
    {
        (void)f;
        std::lock_guard<std::mutex> lk(g_cs);
        const entry *best = nullptr;
        const unsigned long long need = (g_want_fmt != 0) ? 2ull : 10ull;
        bool any_written = false;
        for (unsigned i = 0; i < MAX; ++i)
        {
            entry &e = g_t[i];
            if (e.handle == 0 || e.died != 0) continue;
            if (e.w != g_w || e.h != g_h) continue;   // R280l: exact scene size decides
            const unsigned long long writes = e.to_rt + e.to_uav;
            const unsigned long long dw = writes - e.win_w, ds = e.to_srv - e.win_s;
            e.win_w = writes; e.win_s = e.to_srv;
            if (dw >= 24ull) any_written = true;
            if (dw >= 24ull && ds * 5ull >= dw * 2ull) ++e.streak; else e.streak = 0;
            if (g_want_fmt != 0 && (int)e.fmt != g_want_fmt) continue;
            if (e.streak < need) continue;
            if (best == nullptr || e.streak > best->streak || (e.streak == best->streak && writes > best->to_rt + best->to_uav)) best = &e;
        }
        if (any_written) ++g_active_windows;
        g_last_written = any_written;   // R280j
        if (best == nullptr) return false;
        g_src_fmt = best->fmt;
        g_src_rt = best->to_rt != 0ull;
        g_src.store(best->handle, std::memory_order_release);
        return true;
    }

    // ---- our objects, built once on the first decided source ----
    typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                             LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

    bool build_objects(reshade::api::device *api_dev)
    {
        if (g_objects_tried) return g_objects_ok;
        g_objects_tried = true;
        char l[400];
        HRESULT hr;

        // staging: same format as the source, scene size, plain texture
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = g_w; td.Height = g_h;
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Format = srv_format(g_src_fmt);
        hr = g_d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_staging));
        if (FAILED(hr)) { snprintf(l, sizeof l, "[MGPU][R280] staging texture failed 0x%08X - extraction off.", (unsigned)hr); mgpu::diag::error(l); return false; }

        // the virtual velocity target: created THROUGH ReShade's device, so it is
        // a ReShade resource handle (what the probe's tap copies) and is destroyed
        // through the same device at shutdown. The census is told about it below.
        reshade::api::resource_desc vd(g_w, g_h, 1, 1, reshade::api::format::r16g16_float, 1,
                                       reshade::api::memory_heap::gpu_only,
                                       reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_source);
        if (!api_dev->create_resource(vd, nullptr, reshade::api::resource_usage::shader_resource, &g_target_api) || g_target_api.handle == 0)
        {
            mgpu::diag::error("[MGPU][R280] the virtual velocity target could not be created - extraction off.");
            return false;
        }
        g_target = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(g_target_api.handle));
        g_target_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g_api_dev = api_dev;

        // shader
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        pfn_d3dcompile compile = dc ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (compile == nullptr) { mgpu::diag::error("[MGPU][R280] d3dcompiler_47 / D3DCompile not available - extraction off."); return false; }
        ID3DBlob *cs = nullptr, *err = nullptr;
        hr = compile(kExtractHLSL, strlen(kExtractHLSL), "mvec_extract", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
        if (FAILED(hr) || cs == nullptr)
        {
            snprintf(l, sizeof l, "[MGPU][R280] extract shader failed to compile 0x%08X %s", (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(l); if (err) err->Release(); return false;
        }
        if (err) err->Release();

        // root signature: table [SRV t0, UAV u0] + 4 constants
        D3D12_DESCRIPTOR_RANGE rng[2] = {};
        rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 1; rng[0].BaseShaderRegister = 0; rng[0].OffsetInDescriptorsFromTableStart = 0;
        rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 1; rng[1].BaseShaderRegister = 0; rng[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = rp;
        ID3DBlob *sig = nullptr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) { mgpu::diag::error("[MGPU][R280] root signature serialise failed - extraction off."); if (err) err->Release(); cs->Release(); return false; }
        hr = g_d3d->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs));
        sig->Release();
        if (FAILED(hr)) { cs->Release(); mgpu::diag::error("[MGPU][R280] root signature failed - extraction off."); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = g_rs; pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = g_d3d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
        cs->Release();
        if (FAILED(hr)) { mgpu::diag::error("[MGPU][R280] compute PSO failed - extraction off."); return false; }

        // descriptors: SRV of staging, UAV of target
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d3d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { mgpu::diag::error("[MGPU][R280] descriptor heap failed - extraction off."); return false; }
        const unsigned inc = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = td.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        g_d3d->CreateShaderResourceView(g_staging, &sv, cpu);
        cpu.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_R16G16_FLOAT; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_d3d->CreateUnorderedAccessView(g_target, nullptr, &uv, cpu);

        // our list, two allocators, a fence
        for (int i = 0; i < 2; ++i)
            if (FAILED(g_d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])))) { mgpu::diag::error("[MGPU][R280] command allocator failed - extraction off."); return false; }
        if (FAILED(g_d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list)))) { mgpu::diag::error("[MGPU][R280] command list failed - extraction off."); return false; }
        g_list->Close();
        if (FAILED(g_d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { mgpu::diag::error("[MGPU][R280] fence failed - extraction off."); return false; }
        g_fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        snprintf(l, sizeof l, "[MGPU][R280] VIRTUAL VELOCITY TARGET built: source 0x%llx fmt=%u %ux%u -> our RG16F target 0x%llx (x,y in UV, as the engine wrote them). "
                              "The census sees it as a two-channel scene-sized candidate; the transport, GPU 1 and the warp test see Skyrim's shape.",
                 g_src.load(), g_src_fmt, g_w, g_h, (unsigned long long)g_target_api.handle);
        mgpu::diag::info(l);
        g_objects_ok = true;
        // ReShade fires no init_resource for a resource an add-on creates
        // (device_impl::create_resource only registers it), so the probe's census
        // is told directly: one two-channel scene-sized candidate, the largest.
        // Then the probe's pick is released: its next dump names our target and
        // the arm's 240-frame stability hold does the rest.
        mgpu::probe::mvec_offer((unsigned long long)g_target_api.handle, g_w, g_h, static_cast<unsigned>(reshade::api::format::r16g16_float));
        mgpu::gpu1::set_mvec_extract_active(true);   // R280c: the arm derives uv, the judge writes nothing
        if (g_deferring) { g_deferring = false; mgpu::probe::mvec_defer(false); }
        return true;
    }

    void reap(bool all)   // R280p: destroy what is due (all = at shutdown)
    {
        const unsigned long long f = g_frame.load(std::memory_order_relaxed);
        for (grave &g : g_grave)
            if (g.r.handle != 0 && (all || f >= g.due))
            {
                if (g_api_dev != nullptr) g_api_dev->destroy_resource(g.r);
                g.r = { 0 }; g.due = 0;
            }
    }
    void bury(reshade::api::resource t)   // R280p: withdrawn already by the caller
    {
        if (t.handle == 0) return;
        const unsigned long long due = g_frame.load(std::memory_order_relaxed) + GRAVE_FRAMES;
        for (grave &g : g_grave) if (g.r.handle == 0) { g.r = t; g.due = due; return; }
        grave *old = &g_grave[0];   // full: the one that has waited longest goes now
        for (grave &g : g_grave) if (g.due < old->due) old = &g;
        if (g_api_dev != nullptr) g_api_dev->destroy_resource(old->r);
        old->r = t; old->due = due;
    }

    void release_objects()
    {
        if (g_fence != nullptr && g_fence_ev != nullptr && g_fence_v != 0 && g_fence->GetCompletedValue() < g_fence_v)
        { g_fence->SetEventOnCompletion(g_fence_v, g_fence_ev); WaitForSingleObject(g_fence_ev, 2000); }
        if (g_list) { g_list->Release(); g_list = nullptr; }
        for (int i = 0; i < 2; ++i) if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
        if (g_heap) { g_heap->Release(); g_heap = nullptr; }
        if (g_pso) { g_pso->Release(); g_pso = nullptr; }
        if (g_rs) { g_rs->Release(); g_rs = nullptr; }
        if (g_fence) { g_fence->Release(); g_fence = nullptr; }
        if (g_fence_ev) { CloseHandle(g_fence_ev); g_fence_ev = nullptr; }
        if (g_staging) { g_staging->Release(); g_staging = nullptr; }
        g_target = nullptr; g_target_api = { 0 };   // the caller destroys it: buried on inert / reset (R280p), at once at shutdown
        g_objects_ok = false;
    }
    // R280g/R280h: everything built at the old size goes; the decision runs again at the
    // new one. Called from on_finish_effects (the game's render thread, between frames).
    void reset_to(unsigned w, unsigned h)
    {
        const reshade::api::resource t = g_target_api;
        release_objects();
        if (t.handle != 0) { mgpu::probe::mvec_withdraw((unsigned long long)t.handle); bury(t); }   // R280p
        // R280i: x_active stays TRUE - the title is still on the R280 path; the judge must
        // not fall back to the R278 branch (which writes MVecLearned=99) while we rebuild.
        g_src.store(0); g_src_fmt = 0; g_objects_tried = false; g_objects_ok = false; g_zeroed = false;
        g_staged_frame.store(~0ull); g_copy_taken.store(0); g_edges_frame.store(0); g_active_windows = 0;
        // R280l: the table is KEPT - band entries are still live textures; the decision at
        // the new size finds the engine's new-size target among them (RE4-X7 lost it here).
        char l[260];
        snprintf(l, sizeof l, "[MGPU][R280] scene size %ux%u -> %ux%u (held %u frames): the virtual target is rebuilt - source forgotten, objects released, the census told; the decision runs again. Reset %u in the last 5 minutes (limit %u, then dormant 5 minutes).",
                 g_w, g_h, w, h, RESET_SETTLE, g_reset_n, RESET_MAX);
        mgpu::diag::info(l);
        g_w = w; g_h = h;
        if (g_live.load() && !g_deferring) { g_deferring = true; mgpu::probe::mvec_defer(true); }
    }
}   // namespace

void init(void *game_device_native, unsigned screen_w, unsigned screen_h)
{
    // R280f: a second chain resizes only when it is on the same device (a resize of the
    // game's chain); another device's chain never moves the scene size.
    // R280g/R280h: a different size on the game's device is a RESET (the staging, the
    // target and the census candidate were built at the old size; copy_resource between
    // sizes is undefined) - PENDING until the new size has held RESET_SETTLE frames, so a
    // chain that flips size for a menu and returns costs nothing. Runs in on_finish_effects.
    if (g_on.load())
    {
        if (game_device_native != g_dev || screen_w == 0 || screen_h == 0) return;
        g_cur_w = screen_w; g_cur_h = screen_h;   // R280i: known even while dormant, for the lift
        if (g_dormant || g_dormant_until != 0) return;
        // R280l: nothing built yet (RE4-X7: the chain goes 1920x1080 -> 2560x1440 at every
        // launch, before gameplay) - nothing to tear down, so the new size is taken at once.
        if (!g_objects_ok && g_src.load(std::memory_order_relaxed) == 0)
        {
            if (screen_w != g_w || screen_h != g_h) { g_w = screen_w; g_h = screen_h; g_pend_w = g_pend_h = 0; }
            return;
        }
        if (screen_w == g_w && screen_h == g_h)
        {
            if (g_pend_w != 0)
            {
                char l[200];
                snprintf(l, sizeof l, "[MGPU][R280] scene size back to %ux%u before the %ux%u reset ran: cancelled, nothing rebuilt.", g_w, g_h, g_pend_w, g_pend_h);
                mgpu::diag::info(l);
                g_pend_w = g_pend_h = 0;
            }
            return;
        }
        if (screen_w != g_pend_w || screen_h != g_pend_h) { g_pend_w = screen_w; g_pend_h = screen_h; g_pend_since = g_frame.load(std::memory_order_relaxed); }
        return;
    }
    if (mgpu::gpu1::ui_ini_read("MVecExtractOff", 0) == 1) return;
    const int learned = mgpu::gpu1::ui_ini_read("MVecLearned", -1);
    int fmt = mgpu::gpu1::ui_ini_read("MVecExtract", 0);
    // Runs unless the title learned depth only (99). MVecExtract=<fmt>, written
    // at the first teardown after the source was decided, only shortens the
    // decision (2 windows of that format instead of 10 by signature).
    if (learned == 99) return;
    if (fmt != 0 && !four_ch16((unsigned)fmt)) fmt = 0;
    g_dev = game_device_native; g_d3d = reinterpret_cast<ID3D12Device *>(game_device_native);
    g_w = screen_w; g_h = screen_h; g_want_fmt = fmt;
    char l[360];
    snprintf(l, sizeof l, "[MGPU][R280] MVEC EXTRACT armed on a D3D12 chain (%ux%u): %s. Inert the moment this title initialises NGX "
                          "(a contract title keeps its own vectors). MVecExtractOff=1 turns it off.",
             screen_w, screen_h, fmt != 0 ? "learned source format from mgpu.ini" : "first launch - the source is decided by usage signature");
    mgpu::diag::info(l);
    reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::register_event<reshade::addon_event::barrier>(on_barrier);
    // The probe publishes no velocity source until our target exists (or 900
    // RENDERED frames pass without a fit, or a contract shows): otherwise its census would arm the
    // stream on a smaller two-channel buffer first (RE4-L1: 1280x720) and the
    // learning would spend this launch rejecting it.
    g_deferring = true; mgpu::probe::mvec_defer(true);
    g_on.store(true);
}

bool on() { return g_on.load() && g_live.load(); }

void on_finish_effects(void *reshade_command_list, void *reshade_command_queue)
{
    if (!g_on.load()) return;
    reshade::api::command_list *cl = static_cast<reshade::api::command_list *>(reshade_command_list);
    reshade::api::command_queue *q = static_cast<reshade::api::command_queue *>(reshade_command_queue);
    if (cl == nullptr || q == nullptr) return;
    const unsigned long long f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    reap(false);   // R280p: targets torn down GRAVE_FRAMES ago

    // A contract title: go inert, once, say it.
    if (g_live.load(std::memory_order_relaxed) && (f % 30ull) == 0ull && mgpu::gpu1::game_contract_seen())   // R280k: Init seen OR the game's NGX table
    {
        g_live.store(false);
        // R280k: anything already built goes too, and the census forgets our target -
        // otherwise the probe could keep naming it (largest candidate) over the game's own.
        if (g_objects_ok || g_src.load(std::memory_order_relaxed) != 0)
        {
            const reshade::api::resource t = g_target_api;
            release_objects();
            if (t.handle != 0) { mgpu::probe::mvec_withdraw((unsigned long long)t.handle); bury(t); }   // R280p
            g_src.store(0); g_src_fmt = 0; g_objects_tried = false;
        }
        if (g_deferring) { g_deferring = false; mgpu::probe::mvec_defer(false); }
        mgpu::gpu1::set_mvec_extract_active(false);
        mgpu::gpu1::set_mvec_extract_deciding(false);   // R280j
        mgpu::diag::info("[MGPU][R280] this title initialised NGX (a contract): extraction inert from here - its own vectors are the route.");
        return;
    }
    if (!g_live.load(std::memory_order_relaxed) || g_dormant) return;

    // R280i: dormant for a window - lift it when the window has passed. The pick stays
    // deferred throughout (no other buffer gets published: RE4's 1280x720 would strobe).
    if (g_dormant_until != 0)
    {
        const unsigned long long now = GetTickCount64();
        if (now < g_dormant_until) return;
        g_dormant_until = 0; g_reset_n = 0; g_reset_i = 0;
        g_pend_w = g_pend_h = 0;
        mgpu::diag::info("[MGPU][R280] dormancy over (5 minutes): extraction back on; the decision runs again at the current scene size.");
        if (g_cur_w != 0 && (g_cur_w != g_w || g_cur_h != g_h)) { g_w = g_cur_w; g_h = g_cur_h; }
        return;
    }

    // R280h/R280i: the pending reset, once the new size has held long enough. Bounded
    // per 5-minute window: the 4th reset inside the window is the last - dormant for
    // 5 minutes (handlers return at their first load: no lock, no copy, no compute).
    if (g_pend_w != 0 && f - g_pend_since >= (unsigned long long)RESET_SETTLE)
    {
        const unsigned w = g_pend_w, h = g_pend_h; g_pend_w = g_pend_h = 0;
        const unsigned long long now = GetTickCount64();
        // drop resets older than the window
        unsigned in_window = 0;
        for (unsigned i = 0; i < g_reset_n && i < RESET_MAX; ++i) if (now - g_reset_ms[i] <= WINDOW_MS) ++in_window;
        if (in_window < RESET_MAX) { if (g_reset_n < RESET_MAX) ++g_reset_n; g_reset_ms[g_reset_i] = now; g_reset_i = (g_reset_i + 1u) % RESET_MAX; reset_to(w, h); return; }
        // the bound: release everything, dormant for one window
        reset_to(w, h);
        ++g_dormancies;
        if (g_dormancies >= DORMANT_MAX)
        {
            g_dormant = true; g_live.store(false); mgpu::gpu1::set_mvec_extract_deciding(false);   // R280j
            mgpu::diag::warn("[MGPU][R280] scene size changed 4 times in 5 minutes, for the 4th time this session: extraction OFF for the rest of the session. The next launch starts fresh.");
            return;
        }
        g_dormant_until = now + WINDOW_MS; mgpu::gpu1::set_mvec_extract_deciding(false);   // R280j
        char l[260];
        snprintf(l, sizeof l, "[MGPU][R280] scene size changed %u times in 5 minutes: extraction DORMANT for 5 minutes (no copies, no compute, no decision; vectors absent meanwhile). Dormancy %u of %u this session.",
                 RESET_MAX, g_dormancies, DORMANT_MAX);
        mgpu::diag::warn(l);
        return;
    }

    // R280j: tell the arm's hold whether a decision is coming with evidence.
    mgpu::gpu1::set_mvec_extract_deciding(g_src.load(std::memory_order_relaxed) == 0 && g_deferring && g_last_written);

    // Decide the source (once, or again if it was destroyed).
    if (g_src.load(std::memory_order_relaxed) == 0)
    {
        if ((f % 30ull) != 0ull) return;
        if (!decide_source(f))
        {
            // R280b: the give-up clock is 30 windows in which SOME candidate was written
            // (the engine is rendering and nothing fits), never wall frames: a menu that
            // renders no G-buffer runs no clock, so a long menu cannot release the
            // probe's pick onto a smaller buffer before gameplay has started.
            if (g_deferring && g_active_windows >= 30u)
            {
                g_deferring = false; mgpu::probe::mvec_defer(false);
                mgpu::diag::info("[MGPU][R280] no four-channel scene-sized target with a per-frame write/read signature in 900 rendered frames: "
                                 "the probe's census proceeds on its own. The signature keeps being checked; a late source is offered when it settles.");
            }
            return;
        }
        char l[300];
        snprintf(l, sizeof l, "[MGPU][R280] source decided at f=%llu: 0x%llx fmt=%u (four-channel, scene size, written and read every frame). Extracting x,y from here.",
                 f, g_src.load(), g_src_fmt);
        mgpu::diag::info(l);
        if (g_staging == nullptr && !build_objects(cl->get_device()) && g_deferring)
        { g_deferring = false; mgpu::probe::mvec_defer(false); }   // failed: the census proceeds as before
        return;
    }
    if (!g_objects_ok) return;
    // R280d: how many copy edges the last frame had (1 = one write, as TP3; 2+ = the
    // target is written more than once per frame and the last write is what we hold).
    {
        const unsigned e = g_edges_frame.exchange(0u, std::memory_order_relaxed);
        if (e != 0u) ++g_hist[e >= 3u ? 2 : e - 1u];
        mgpu::gpu1::set_mvec_extract_edges(e);                                       // R280e: the judge reads it
        g_edge_first.store(mgpu::gpu1::mvec_extract_edge_request(), std::memory_order_relaxed);   // R280e: the judge's choice for the next frame
    }
    // Only when this frame's staging copy was recorded by the barrier handler.
    // R280g: no copy this frame (the engine did not write the source: a pause, a menu
    // over a frozen scene, the window in the background - RE4-X6) -> the target is
    // ZEROED once, so GPU 1 does not keep applying the last real motion to a picture
    // that changes without it. The first real write after that extracts as usual.
    const bool staged = (g_staged_frame.load(std::memory_order_acquire) == f - 1 || g_staged_frame.load(std::memory_order_acquire) == f);
    if (!staged && g_zeroed) return;
    const bool zero_pass = !staged;

    // ---- our list: staging -> target (x,y), on the game's queue ----
    ID3D12CommandQueue *gq = reinterpret_cast<ID3D12CommandQueue *>(static_cast<uintptr_t>(q->get_native()));
    if (gq == nullptr) return;
    // the allocator of this parity must be free: wait for the fence of two frames ago
    if (g_fence_v >= 2 && g_fence->GetCompletedValue() < g_fence_v - 1)
    { g_fence->SetEventOnCompletion(g_fence_v - 1, g_fence_ev); WaitForSingleObject(g_fence_ev, 20); }
    ID3D12CommandAllocator *al = g_alloc[g_parity]; g_parity ^= 1u;
    if (FAILED(al->Reset()) || FAILED(g_list->Reset(al, g_pso))) return;

    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition.pResource = g_staging;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource = g_target;
    b[1].Transition.StateBefore = g_target_state; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(2, b);
    g_list->SetComputeRootSignature(g_rs);
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    const unsigned c[4] = { g_w, g_h, zero_pass ? 1u : 0u, 0 };
    g_list->SetComputeRoot32BitConstants(1, 4, c, 0);
    g_list->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_list->ResourceBarrier(1, &b[0]);   // staging back to COPY_DEST for the next mid-frame copy
    g_list->Close();
    ID3D12CommandList *lists[1] = { g_list };
    gq->ExecuteCommandLists(1, lists);
    gq->Signal(g_fence, ++g_fence_v);
    g_target_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ---- on ReShade's list (executed after ours on the same queue): the target's
    // UAV -> SRV barrier, so it sits in the state the R277 tap assumes
    // (shader_resource) when the tap copies it into the slot, before the seal. ----
    cl->barrier(g_target_api, reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::shader_resource);
    g_target_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (zero_pass) { g_zeroed = true; return; }
    g_zeroed = false;
    ++g_extracted;
    if (g_extracted == 1 || g_extracted == 600 || (g_extracted % 6000ull) == 0ull)
    {
        char l[400];
        snprintf(l, sizeof l, "[MGPU][R280] extracted %llu frame(s) into the virtual velocity target 0x%llx (f=%llu). Source write edges per frame so far: "
                              "1 edge %llu frame(s), 2 edges %llu, 3+ edges %llu (edge extracted: %s).",
                 g_extracted, (unsigned long long)g_target_api.handle, f, g_hist[0], g_hist[1], g_hist[2], g_edge_first.load(std::memory_order_relaxed) ? "first of the frame" : "last of the frame");
        mgpu::diag::info(l);
    }
}

void shutdown()
{
    if (!g_on.load()) return;
    reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
    reshade::unregister_event<reshade::addon_event::barrier>(on_barrier);
    // R280c: no MVecExtract write. ReShade raises no destroy_device on a clean exit
    // (V21, dllmain), so this ran on no clean exit; and the next launch decides in
    // 300 gameplay frames anyway. The key stays honoured when a user sets it.
    mgpu::gpu1::set_mvec_extract_active(false);
    mgpu::gpu1::set_mvec_extract_deciding(false);   // R280j
    g_src.store(0);
    if (g_deferring) { g_deferring = false; mgpu::probe::mvec_defer(false); }
    const reshade::api::resource t = g_target_api;
    release_objects();   // waits for our last list, then drops our objects
    if (t.handle != 0 && g_api_dev != nullptr) g_api_dev->destroy_resource(t);   // the device is alive during destroy_device
    reap(true);   // R280p: and anything still waiting in the grave
    g_api_dev = nullptr;
    g_on.store(false);
}
}   // namespace mgpu::mvec_extract
