// ---------------------------------------------------------------------------
// hdr.cpp - HD1. See hdr.hpp.
// ---------------------------------------------------------------------------
#include "hdr.hpp"
#include "gpu1_context.hpp"
#include "diag.hpp"
#include "d3dc.hpp"

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>

namespace mgpu
{
namespace hdr
{
namespace
{

// ===========================================================================
// settings (same pattern as Frame Warp: read once, live from the panel)
// ===========================================================================
struct setting { const char *key; int dflt, lo, hi; std::atomic<int> v; };
setting S_ON    { "HDR",             0,  0, 1,    {-1} };
setting S_PEAK  { "HDRPeak",         0,  0, 4000, {-1} };   // 0 = the monitor's
setting S_PAPER { "HDRPaperWhite", 200, 80, 500,  {-1} };
setting S_HL    { "HDRHighlights",  60,  0, 100,  {-1} };
setting S_SAT   { "HDRSaturation", 100, 50, 150,  {-1} };

int get(setting &s)
{
    int v = s.v.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    v = mgpu::gpu1::ui_ini_read(s.key, s.dflt);
    if (v < s.lo || v > s.hi) v = s.dflt;
    s.v.store(v, std::memory_order_relaxed);
    return v;
}
void put(setting &s, int v)
{
    if (v < s.lo) v = s.lo;
    if (v > s.hi) v = s.hi;
    s.v.store(v, std::memory_order_relaxed);
    (void)mgpu::gpu1::ui_ini_write(s.key, v);
}

// ===========================================================================
// the monitor (bridge thread writes, present thread and panel read)
// ===========================================================================
std::atomic<int> g_disp_hdr{-1};     // -1 not checked yet, 0 SDR, 1 Windows HDR on
std::atomic<int> g_disp_peak{0};     // nits, 0 = unknown
std::atomic<int> g_fmt_ok{-1};       // -1 not seen, 0 the picture is not 10-bit, 1 ok
std::atomic<int> g_build_ok{-1};     // -1 not built, 0 failed, 1 ok

// ===========================================================================
// the GPU pass
// ===========================================================================
typedef HRESULT (WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

// SDR (gamma 2.2, Rec.709) -> HDR10 (PQ, Rec.2020).
//   1. decode gamma 2.2 to linear light (what an SDR monitor shows)
//   2. white = paper white nits
//   3. highlights: brightness is raised in proportion to luminance^3, so dark
//      and mid tones stay where they were and only bright areas grow toward
//      the peak; the gain is the same for R, G and B, so hue is kept
//   4. saturation around luminance
//   5. Rec.709 -> Rec.2020 primaries, clip at the peak, PQ encode
const char *kHdrHLSL =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "cbuffer C : register(b0) { float paper; float peak; float hl; float sat; uint w; uint h; uint p0; uint p1; };\n"
    "float3 pq(float3 nits)\n"
    "{\n"
    "    const float m1 = 0.1593017578125, m2 = 78.84375;\n"
    "    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "    float3 y = pow(saturate(nits / 10000.0), m1);\n"
    "    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float4 c = src.Load(int3(id.xy, 0));\n"
    "    float3 lin = pow(saturate(c.rgb), 2.2);\n"
    "    float y = dot(lin, float3(0.2126, 0.7152, 0.0722));\n"
    "    float ratio = max(peak / paper, 1.0);\n"
    "    float gain = 1.0 + hl * (ratio - 1.0) * y * y * y;\n"
    "    lin *= gain;\n"
    "    float yl = y * gain;\n"
    "    lin = max(yl + (lin - yl) * sat, 0.0);\n"
    "    float3 r = float3(dot(lin, float3(0.6274040, 0.3292820, 0.0433136)),\n"
    "                      dot(lin, float3(0.0690970, 0.9195400, 0.0113612)),\n"
    "                      dot(lin, float3(0.0163916, 0.0880132, 0.8955950)));\n"
    "    float3 nits = min(r * paper, peak);\n"
    "    dst[id.xy] = float4(pq(nits), c.a);\n"
    "}\n";

struct gpu_state
{
    ID3D12Device *dev = nullptr;     // AddRef'd, so its address cannot be reused by a new device
    unsigned w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *copy = nullptr;      // the frame as drawn (SRV)
    ID3D12Resource *out = nullptr;       // the HDR10 frame (UAV)
    bool failed = false;
};
gpu_state G;

// Objects replaced by a rebuild may still be in use by present lists in
// flight, so they are released a few presents later (as Frame Warp does).
// RV1: released when the present fence passes the value of the newest list
// that could use them (see framewarp.cpp), not after a count of presents.
struct retired { IUnknown *o; unsigned long long gen, value; };
std::vector<retired> g_retired;
void retire(IUnknown *o)
{
    if (o != nullptr)
        g_retired.push_back({ o, mgpu::gpu1::present_chain_gen(), mgpu::gpu1::present_recording_value() });
}
void age_retired()
{
    size_t k = 0;
    for (size_t i = 0; i < g_retired.size(); ++i)
    {
        if (mgpu::gpu1::present_value_done(g_retired[i].gen, g_retired[i].value)) g_retired[i].o->Release();
        else g_retired[k++] = g_retired[i];
    }
    g_retired.resize(k);
}
void release_gpu()
{
    retire(G.out);  G.out = nullptr;
    retire(G.copy); G.copy = nullptr;
    retire(G.heap); G.heap = nullptr;
    retire(G.pso);  G.pso = nullptr;
    retire(G.rs);   G.rs = nullptr;
    retire(G.dev);  G.dev = nullptr;
    G.w = G.h = 0; G.fmt = DXGI_FORMAT_UNKNOWN; G.failed = false;
}

void tbarrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

bool build(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    if (G.dev == dev && G.w == w && G.h == h && G.fmt == fmt) return !G.failed;
    release_gpu();
    dev->AddRef();
    G.dev = dev; G.w = w; G.h = h; G.fmt = fmt;
    char l[400];
    auto fail = [&](const char *why, HRESULT hr) {
        snprintf(l, sizeof l, "[MGPU][HD1] HDR cannot run on this output (%ux%u, format %u): %s (0x%08X). "
                 "The picture stays SDR.", w, h, (unsigned)fmt, why, (unsigned)hr);
        mgpu::diag::warn(l);
        G.failed = true;
        g_build_ok.store(0);
        return false;
    };

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        return fail("the card cannot write this format from a shader", E_FAIL);

    pfn_compile cmp = (pfn_compile)mgpu::d3dcompile();   // RV1
    if (cmp == nullptr) return fail("d3dcompiler_47.dll not available", E_FAIL);
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = cmp(kHdrHLSL, strlen(kHdrHLSL), "hdr", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr) || cs == nullptr) return fail("shader compile", hr);

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 1;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 1;
    rng[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 8;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) err->Release();
    if (SUCCEEDED(hr)) { hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&G.rs)); sig->Release(); }
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = G.rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&G.pso));
    }
    cs->Release();
    if (FAILED(hr)) return fail("pipeline", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.heap));
    if (FAILED(hr)) return fail("descriptor heap", hr);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&G.copy));
    if (SUCCEEDED(hr))
    {
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&G.out));
    }
    if (FAILED(hr)) return fail("textures", hr);

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE hc = G.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Format = fmt;
    dev->CreateShaderResourceView(G.copy, &sv, hc);
    hc.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = fmt;
    dev->CreateUnorderedAccessView(G.out, nullptr, &uv, hc);

    snprintf(l, sizeof l, "[MGPU][HD1] HDR output ready on the bridge output (%ux%u, format %u).", w, h, (unsigned)fmt);
    mgpu::diag::info(l);
    g_build_ok.store(1);
    return true;
}

// Present thread only.
IDXGISwapChain3 *g_cs_chain = nullptr;   // the chain the colour space below was set on (not owned)
int g_cs_set = -1;                       // -1 unknown, 0 SDR, 1 HDR10
std::atomic<bool> g_cs_refused{false};   // this chain will not take HDR10: stop converting

} // namespace

// ===========================================================================
// public
// ===========================================================================
bool enabled()               { return get(S_ON) != 0; }
void set_enabled(bool on)    { put(S_ON, on ? 1 : 0); }
int  peak()                  { return get(S_PEAK); }
void set_peak(int nits)      { put(S_PEAK, nits); }
int  paper_white()           { return get(S_PAPER); }
void set_paper_white(int n)  { put(S_PAPER, n); }
int  highlights()            { return get(S_HL); }
void set_highlights(int pct) { put(S_HL, pct); }
int  saturation()            { return get(S_SAT); }
void set_saturation(int pct) { put(S_SAT, pct); }

bool display_hdr()  { return g_disp_hdr.load() == 1; }
int  display_peak() { return g_disp_peak.load(); }
int  peak_in_use()
{
    const int p = peak();
    if (p > 0) return p;
    const int d = display_peak();
    return (d >= 200) ? d : 1000;   // nothing sensible reported: a typical HDR monitor
}

void poll_display(HWND wnd)
{
    if (wnd == nullptr) return;
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    if (mon == nullptr) return;
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || f == nullptr) return;
    int hdr_on = 0, nits = 0;
    bool found = false;
    IDXGIAdapter1 *a = nullptr;
    for (UINT ai = 0; !found && f->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai)
    {
        IDXGIOutput *o = nullptr;
        for (UINT oi = 0; !found && a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi)
        {
            IDXGIOutput6 *o6 = nullptr;
            if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6))) && o6 != nullptr)
            {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == mon)
                {
                    found = true;
                    hdr_on = (d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ? 1 : 0;
                    nits = (int)(d.MaxLuminance + 0.5f);
                }
                o6->Release();
            }
            o->Release();
        }
        a->Release();
    }
    f->Release();
    if (!found) return;
    const int was = g_disp_hdr.exchange(hdr_on);
    g_disp_peak.store(nits);
    if (was != hdr_on)
    {
        char l[240];
        snprintf(l, sizeof l, "[MGPU][HD1] the picture's monitor: Windows HDR %s, reported peak %d nits.",
                 hdr_on ? "ON" : "off", nits);
        mgpu::diag::info(l);
    }
}

bool sync_colour_space(IDXGISwapChain3 *sc, bool want)
{
    if (sc == nullptr) return false;
    if (sc != g_cs_chain) { g_cs_chain = sc; g_cs_set = -1; g_cs_refused.store(false); }   // new chain
    const int target = want ? 1 : 0;
    if (g_cs_set == target) return target == 1;
    const DXGI_COLOR_SPACE_TYPE cs = want ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                          : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT sup = 0;
    HRESULT hr = sc->CheckColorSpaceSupport(cs, &sup);
    if (SUCCEEDED(hr) && (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0)
        hr = sc->SetColorSpace1(cs);
    else if (SUCCEEDED(hr))
        hr = E_NOTIMPL;
    char l[240];
    if (SUCCEEDED(hr))
    {
        g_cs_set = target;
        snprintf(l, sizeof l, "[MGPU][HD1] picture colour space: %s.",
                 want ? "HDR10 (PQ, Rec.2020) - HDR output on" : "SDR");
        mgpu::diag::info(l);
        return target == 1;
    }
    // Could not switch. HDR10 refused: stop converting on this chain (apply
    // checks the flag), so a PQ picture is never shown as SDR.
    snprintf(l, sizeof l, "[MGPU][HD1] the swap chain refused colour space %s (0x%08X).%s",
             want ? "HDR10" : "SDR", (unsigned)hr, want ? " HDR stays off on this picture." : "");
    mgpu::diag::warn(l);
    if (want) g_cs_refused.store(true);
    else g_cs_set = 0;
    return false;
}

bool apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
           unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    age_retired();
    if (!enabled() || !display_hdr() || dev == nullptr || cl == nullptr || bb == nullptr || w < 16 || h < 16)
        return false;
    if (g_cs_refused.load()) return false;
    if (fmt != DXGI_FORMAT_R10G10B10A2_UNORM) { g_fmt_ok.store(0); return false; }
    g_fmt_ok.store(1);
    if (!build(dev, w, h, fmt)) return false;

    // bb: RENDER_TARGET -> COPY_SOURCE, copy it, convert into G.out, copy back.
    D3D12_TEXTURE_COPY_LOCATION a{}, b{};
    a.Type = b.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{ 0, 0, 0, w, h, 1 };
    tbarrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    a.pResource = G.copy; b.pResource = bb;
    cl->CopyTextureRegion(&a, 0, 0, 0, &b, &box);
    tbarrier(cl, G.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap *hs[1] = { G.heap };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(G.rs);
    cl->SetPipelineState(G.pso);
    cl->SetComputeRootDescriptorTable(0, G.heap->GetGPUDescriptorHandleForHeapStart());
    struct { float paper, peak, hl, sat; UINT w, h, p0, p1; } c{
        (float)paper_white(), (float)peak_in_use(), (float)highlights() / 100.0f,
        (float)saturation() / 100.0f, w, h, 0u, 0u };
    cl->SetComputeRoot32BitConstants(1, 8, &c, 0);
    cl->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);

    tbarrier(cl, G.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tbarrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    a.pResource = bb; b.pResource = G.out;
    cl->CopyTextureRegion(&a, 0, 0, 0, &b, &box);
    tbarrier(cl, G.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tbarrier(cl, G.copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    tbarrier(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

const char *status()
{
    if (!enabled()) return "";
    if (g_disp_hdr.load() == 0) return "Windows HDR is off for this monitor - turn on 'Use HDR' in Windows display settings.";
    if (g_disp_hdr.load() < 0) return "Checking the monitor...";
    if (g_fmt_ok.load() == 0) return "This game's picture is not 10-bit, so HDR cannot be made from it.";
    if (g_build_ok.load() == 0 || g_cs_refused.load()) return "HDR could not start on this output - see the log ([MGPU][HD1]).";
    return "";
}

} // namespace hdr
} // namespace mgpu
