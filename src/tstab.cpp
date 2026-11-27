// MGPU Bridge - TC1 temporal stabilisation (off by default).
// See tstab.hpp for where it runs and the contract it keeps. frame() and
// release() run on the bridge thread; the panel functions are atomics.

#include "d3dc.hpp"
#include "tstab.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::tstab
{
namespace
{
// ---- settings (any thread) ----
std::atomic<int>  g_mode{-1};       // -1: not read from mgpu.ini yet
std::atomic<int>  g_strength{30};
std::atomic<int>  g_debug{0};
std::atomic<bool> g_mode_changed{false};

// ---- statistics for the panel (written by the bridge thread) ----
std::atomic<bool>  a_active{false}, a_failed{false};
std::atomic<float> a_gpu_ms{0.0f}, a_shimmer{0.0f}, a_changed{0.0f}, a_weight{0.0f};
std::atomic<float> a_off{0.0f}, a_dep{0.0f}, a_col{0.0f};
std::atomic<unsigned> a_resets{0}, a_cuts{0};
std::atomic<unsigned long long> a_frames{0};

// ---- GPU objects (bridge thread) ----
ID3D12Device         *g_dev  = nullptr;   // borrowed
ID3D12RootSignature  *g_rs   = nullptr;
ID3D12PipelineState  *g_pso  = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
UINT                  g_inc  = 0;
ID3D12Resource *g_cur = nullptr;          // copy of the NR output (SRV)
ID3D12Resource *g_hist[2] = {};           // RGBA16F history
ID3D12Resource *g_hdep[2] = {};           // R32F depth history
D3D12_RESOURCE_STATES g_hist_st[2] = {}, g_hdep_st[2] = {};
ID3D12Resource *g_stat  = nullptr;        // 8 uints, DEFAULT, rests in COPY_DEST
ID3D12Resource *g_zero  = nullptr;        // 32 zero bytes, UPLOAD
ID3D12Resource *g_rb    = nullptr;        // READBACK, kSlots x 64 bytes
ID3D12QueryHeap *g_qh   = nullptr;        // 2 timestamps per slot
const unsigned char *g_rb_map = nullptr;

const unsigned kSlots = 4;
const unsigned kStride = 64;
const UINT     kDescPerSet = 9;           // t0..t4, u0..u3

unsigned g_w = 0, g_h = 0;
DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
unsigned g_read = 0;                      // which history holds the last frame
bool     g_valid = false;                 // the read history is usable
bool     g_off_this_arm = false;          // a failure turned the stage off until release
bool     g_said_on = false;
int      g_last_mode = 0;
bool     g_cut_next = false;              // a late readback saw a cut: reset once
double   g_ts_freq = 0.0;

struct pend { bool used; unsigned long long fv; };
pend     g_pend[kSlots] = {};
unsigned g_slot = 0;

// report window
unsigned long long w_frames = 0, w_px = 0, w_off = 0, w_dep = 0, w_col = 0, w_acc = 0,
                   w_flk = 0, w_chg = 0, w_wgt = 0;
double   w_gpu = 0.0;
unsigned w_gpu_n = 0;
ULONGLONG w_t0 = 0;
unsigned g_resets = 0, g_cuts = 0;
unsigned long long g_frames = 0;
bool     g_cost_said = false;

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

// Flags (constant 8).
enum : unsigned
{
    F_DEPTH = 1u, F_DINV = 2u, F_MV = 4u, F_RESET = 8u, F_APPLY = 16u, F_DEBUG = 32u, F_FLOAT = 64u
};

const char kHLSL[] =
    "Texture2D<float4> gCur  : register(t0);\n"
    "Texture2D<float4> gHist : register(t1);\n"
    "Texture2D<float2> gMV   : register(t2);\n"
    "Texture2D<float>  gDep  : register(t3);\n"
    "Texture2D<float>  gHDep : register(t4);\n"
    "RWTexture2D<float4> oOut  : register(u0);\n"
    "RWTexture2D<float4> oHist : register(u1);\n"
    "RWTexture2D<float>  oDep  : register(u2);\n"
    "RWByteAddressBuffer oStat : register(u3);\n"
    "SamplerState gLin : register(s0);\n"
    "cbuffer C : register(b0) { uint2 gSz; uint2 gMVsz; uint2 gDsz; float2 gScale;\n"
    "                           uint gFlags; float gStr; float gGamma; float gDTol; };\n"
    "groupshared uint gs[8];\n"
    "float3 toY(float3 c) { return float3(0.25f*c.r + 0.5f*c.g + 0.25f*c.b, 0.5f*c.r - 0.5f*c.b,\n"
    "                                     -0.25f*c.r + 0.5f*c.g - 0.25f*c.b); }\n"
    "float3 fromY(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }\n"
    // Catmull-Rom history fetch in 5 bilinear taps: bilinear alone would soften
    // the history a little more every frame.
    "float4 histCR(float2 uv)\n"
    "{\n"
    "  float2 sz = float2(gSz);\n"
    "  float2 pos = uv * sz; float2 t1 = floor(pos - 0.5f) + 0.5f; float2 f = pos - t1;\n"
    "  float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));\n"
    "  float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);\n"
    "  float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));\n"
    "  float2 w3 = f * f * (-0.5f + 0.5f * f);\n"
    "  float2 w12 = w1 + w2; float2 o12 = w2 / w12;\n"
    "  float2 c0 = (t1 - 1.0f) / sz, c3 = (t1 + 2.0f) / sz, c12 = (t1 + o12) / sz;\n"
    "  float4 r = gHist.SampleLevel(gLin, float2(c12.x, c0.y), 0) * (w12.x * w0.y)\n"
    "           + gHist.SampleLevel(gLin, float2(c0.x, c12.y), 0) * (w0.x * w12.y)\n"
    "           + gHist.SampleLevel(gLin, float2(c12.x, c12.y), 0) * (w12.x * w12.y)\n"
    "           + gHist.SampleLevel(gLin, float2(c3.x, c12.y), 0) * (w3.x * w12.y)\n"
    "           + gHist.SampleLevel(gLin, float2(c12.x, c3.y), 0) * (w12.x * w3.y);\n"
    "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
    "  return max(r / max(ws, 1e-4f), 0.0f);\n"
    "}\n"
    // Depth as a quantity that grows toward the camera in both conventions
    // (about 1/z), so one relative tolerance works for either.
    "float nearness(float d) { return ((gFlags & 2u) != 0u) ? d : 1.0f - d; }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)\n"
    "{\n"
    "  if (gi < 8u) gs[gi] = 0u;\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  bool inb = id.x < gSz.x && id.y < gSz.y;\n"
    "  if (inb) {\n"
    "    int2 p = int2(id.xy);\n"
    "    float2 uv = (float2(p) + 0.5f) / float2(gSz);\n"
    "    float4 c4 = gCur.Load(int3(p, 0));\n"
    "    float3 c = ((gFlags & 64u) != 0u) ? max(c4.rgb, 0.0f) : saturate(c4.rgb);\n"
    // the current 3x3 neighbourhood: mean and spread in YCoCg
    "    float3 m1 = 0.0f, m2 = 0.0f, mn = 1e9f, mx = -1e9f;\n"
    "    [unroll] for (int j = -1; j <= 1; ++j)\n"
    "    [unroll] for (int i = -1; i <= 1; ++i) {\n"
    "      int2 q = clamp(p + int2(i, j), int2(0, 0), int2(gSz) - 1);\n"
    "      float3 y = toY(gCur.Load(int3(q, 0)).rgb);\n"
    "      m1 += y; m2 += y * y; mn = min(mn, y); mx = max(mx, y);\n"
    "    }\n"
    "    m1 /= 9.0f; m2 /= 9.0f;\n"
    "    float3 sg = sqrt(max(m2 - m1 * m1, 0.0f));\n"
    "    float3 bmn = max(mn, m1 - gGamma * sg), bmx = min(mx, m1 + gGamma * sg);\n"
    "    float3 yc = toY(c);\n"
    // motion: the foreground's vector at edges (closest depth in 3x3), as FG1
    "    bool hasd = (gFlags & 1u) != 0u;\n"
    "    int2 mp = clamp(int2(uv * float2(gMVsz)), int2(0, 0), int2(gMVsz) - 1);\n"
    "    int2 best = mp;\n"
    "    float dscx = float(gDsz.x) / float(gMVsz.x), dscy = float(gDsz.y) / float(gMVsz.y);\n"
    "    if (hasd) {\n"
    "      float bd = -1.0f;\n"
    "      [unroll] for (int j2 = -1; j2 <= 1; ++j2)\n"
    "      [unroll] for (int i2 = -1; i2 <= 1; ++i2) {\n"
    "        int2 q = clamp(mp + int2(i2, j2), int2(0, 0), int2(gMVsz) - 1);\n"
    "        int2 dq = clamp(int2((float2(q) + 0.5f) * float2(dscx, dscy)), int2(0, 0), int2(gDsz) - 1);\n"
    "        float nd = nearness(gDep.Load(int3(dq, 0)));\n"
    "        if (nd > bd) { bd = nd; best = q; }\n"
    "      }\n"
    "    }\n"
    "    float dc = 0.0f;\n"
    "    if (hasd) dc = gDep.Load(int3(clamp(int2(uv * float2(gDsz)), int2(0, 0), int2(gDsz) - 1), 0));\n"
    "    oDep[p] = dc;\n"
    "    bool reset = (gFlags & 8u) != 0u || (gFlags & 4u) == 0u;\n"
    "    float2 v = reset ? float2(0.0f, 0.0f) : gMV.Load(int3(best, 0)) * gScale / float2(gMVsz);\n"
    "    float2 up = uv + v;\n"
    "    bool off = reset || any(up < 0.0f) || any(up > 1.0f);\n"
    "    bool drej = false, crej = false;\n"
    "    float w = 0.0f;\n"
    "    float3 yh = yc, yraw = yc;\n"
    "    if (!off) {\n"
    "      float4 h4 = histCR(up);\n"
    "      yraw = toY(h4.rgb);\n"
    "      if (hasd) {\n"
    "        int2 hp = clamp(int2(up * float2(gSz)), int2(0, 0), int2(gSz) - 1);\n"
    "        float a = nearness(dc), b = nearness(gHDep.Load(int3(hp, 0)));\n"
    "        drej = abs(a - b) > gDTol * max(max(a, b), 1e-6f) + 1e-6f;\n"
    "      }\n"
    // clip the history into the box, toward its centre
    "      float3 bc = 0.5f * (bmx + bmn), be = 0.5f * (bmx - bmn) + 1e-4f;\n"
    "      float3 dv = yraw - bc; float3 du = abs(dv / be);\n"
    "      float m = max(du.x, max(du.y, du.z));\n"
    "      yh = (m > 1.0f) ? bc + dv / m : yraw;\n"
    "      float dist = length(yraw - yh);\n"
    "      crej = dist > 0.12f;\n"
    "      if (!drej && !crej) {\n"
    "        float spx = length(v * float2(gSz));\n"
    "        w = gStr * saturate(1.0f - dist / 0.12f) * lerp(1.0f, 0.5f, saturate(spx / 32.0f));\n"
    "      }\n"
    "    }\n"
    // blend with luminance weights (bright single-frame sparks pull less)
    "    float wc = (1.0f - w) / (1.0f + max(yc.x, 0.0f)), wh = w / (1.0f + max(yh.x, 0.0f));\n"
    "    float3 yo = (yc * wc + yh * wh) / max(wc + wh, 1e-6f);\n"
    "    float3 o = fromY(yo);\n"
    "    if ((gFlags & 64u) == 0u) o = saturate(o);\n"
    "    bool apply = (gFlags & 16u) != 0u;\n"
    "    oHist[p] = float4(apply ? o : c, 1.0f);\n"
    "    if ((gFlags & 32u) != 0u) {\n"
    "      float g = w;\n"
    "      oOut[p] = off ? float4(0.8f, 0.1f, 0.1f, 1.0f) : drej ? float4(0.1f, 0.8f, 0.1f, 1.0f)\n"
    "              : crej ? float4(0.1f, 0.2f, 0.9f, 1.0f) : float4(g, g, g, 1.0f);\n"
    "    } else if (apply) oOut[p] = float4(o, c4.a);\n"
    // statistics: group totals first, one atomic per value per group
    "    uint acc = (!off && !drej && !crej) ? 1u : 0u;\n"
    "    InterlockedAdd(gs[0], 1u);\n"
    "    if (off && !reset) InterlockedAdd(gs[1], 1u);\n"
    "    if (drej) InterlockedAdd(gs[2], 1u);\n"
    "    if (crej) InterlockedAdd(gs[3], 1u);\n"
    "    if (acc != 0u) {\n"
    "      InterlockedAdd(gs[4], 1u);\n"
    "      float yNow = apply ? yo.x : yc.x;\n"
    "      float yPrev = yraw.x;\n"
    "      InterlockedAdd(gs[5], uint(saturate(abs(yNow - yPrev)) * 65536.0f));\n"
    "    }\n"
    "    if (apply) InterlockedAdd(gs[6], uint(saturate(abs(yo.x - yc.x)) * 65536.0f));\n"
    "    InterlockedAdd(gs[7], uint(saturate(w) * 65536.0f));\n"
    "  }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (gi == 0u) {\n"
    "    oStat.InterlockedAdd(0, gs[0]); oStat.InterlockedAdd(4, gs[1]);\n"
    "    oStat.InterlockedAdd(8, gs[2]); oStat.InterlockedAdd(12, gs[3]);\n"
    "    oStat.InterlockedAdd(16, gs[4]); oStat.InterlockedAdd(20, gs[5] >> 8);\n"
    "    oStat.InterlockedAdd(24, gs[6] >> 8); oStat.InterlockedAdd(28, gs[7] >> 8);\n"
    "  }\n"
    "}\n";

void rel(IUnknown *p) { if (p != nullptr) p->Release(); }

void release_all()
{
    if (g_rb != nullptr && g_rb_map != nullptr) g_rb->Unmap(0, nullptr);
    g_rb_map = nullptr;
    rel(g_pso); g_pso = nullptr;
    rel(g_rs); g_rs = nullptr;
    rel(g_heap); g_heap = nullptr;
    rel(g_cur); g_cur = nullptr;
    for (int i = 0; i < 2; ++i) { rel(g_hist[i]); g_hist[i] = nullptr; rel(g_hdep[i]); g_hdep[i] = nullptr; }
    rel(g_stat); g_stat = nullptr;
    rel(g_zero); g_zero = nullptr;
    rel(g_rb); g_rb = nullptr;
    rel(g_qh); g_qh = nullptr;
    g_dev = nullptr;
    g_w = g_h = 0; g_fmt = DXGI_FORMAT_UNKNOWN;
    g_valid = false;
    for (auto &p : g_pend) p = pend{};
    a_active.store(false, std::memory_order_relaxed);
}

// ---- mgpu.ini beside the module (the same reader as nr16) ----
bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    wchar_t mod[MAX_PATH];
    const DWORD len = GetModuleFileNameW(hm, mod, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(mod, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    _snwprintf_s(path, n, _TRUNCATE, L"%lsmgpu.ini", mod);
    return true;
}

void read_keys()
{
    int m = 0, st = 30, dbg = 0;
    wchar_t path[MAX_PATH];
    if (ini_path(path, MAX_PATH))
    {
        FILE *f = nullptr;
        if (_wfopen_s(&f, path, L"rb") == 0 && f != nullptr)
        {
            std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
            const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
            fclose(f);
            if (got <= mgpu::config::MAX_BYTES)
            {
                buf[got] = '\0';
                if (const char *v = mgpu::config::find(buf.data(), got, "TemporalStab")) m = atoi(v);
                if (const char *v = mgpu::config::find(buf.data(), got, "TemporalStabStrength")) st = atoi(v);
                if (const char *v = mgpu::config::find(buf.data(), got, "TemporalStabDebug")) dbg = atoi(v);
            }
        }
    }
    if (m < 0 || m > 2) m = 0;
    if (st < 0) st = 0;
    if (st > 100) st = 100;
    g_strength.store(st, std::memory_order_relaxed);
    g_debug.store(dbg != 0 ? 1 : 0, std::memory_order_relaxed);
    int expect = -1;
    g_mode.compare_exchange_strong(expect, m, std::memory_order_relaxed);
}

DXGI_FORMAT typed(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return f;
    }
}
bool is_float(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

HRESULT make_tex(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT f, bool uav,
                 D3D12_RESOURCE_STATES st, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = f; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(out));
}

HRESULT make_buf(ID3D12Device *dev, D3D12_HEAP_TYPE t, UINT64 n, bool uav, D3D12_RESOURCE_STATES st,
                 ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = n; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(out));
}

bool fail(const char *what, HRESULT hr)
{
    char l[300];
    snprintf(l, sizeof l, "[MGPU][TC1] temporal stabilisation is off for this run: %s (0x%08X). "
             "The picture is unchanged.", what, (unsigned)hr);
    mgpu::diag::error(l);
    release_all();
    g_off_this_arm = true;
    a_failed.store(true, std::memory_order_relaxed);
    return false;
}

bool build(ID3D12Device *dev, ID3D12Resource *out)
{
    const D3D12_RESOURCE_DESC od = out->GetDesc();
    if (od.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || od.Width == 0 || od.Height == 0)
        return fail("the neural output is not a 2D texture", E_INVALIDARG);
    pfn_d3dcompile compile = (pfn_d3dcompile)mgpu::d3dcompile();
    if (compile == nullptr) return fail("D3DCompile is not available", E_NOINTERFACE);
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kHLSL, strlen(kHLSL), "tstab", nullptr, nullptr, "main", "cs_5_1",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char l[900];
        snprintf(l, sizeof l, "[MGPU][TC1] kernel failed to compile: 0x%08X %s", (unsigned)hr,
                 err != nullptr ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(l);
        if (err != nullptr) err->Release();
        return fail("kernel compile", hr);
    }
    if (err != nullptr) err->Release();

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 5;
    rng[0].BaseShaderRegister = 0; rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 4;
    rng[1].BaseShaderRegister = 0; rng[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 13;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr))
    {
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs));
        sig->Release();
    }
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    cs->Release();
    if (FAILED(hr)) return fail("root signature / pipeline", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2u * kDescPerSet;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return fail("descriptor heap", hr);
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_w = (unsigned)od.Width; g_h = od.Height; g_fmt = od.Format;
    hr = make_tex(dev, g_w, g_h, od.Format, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &g_cur);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
    {
        hr = make_tex(dev, g_w, g_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_hist[i]);
        if (SUCCEEDED(hr))
            hr = make_tex(dev, g_w, g_h, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &g_hdep[i]);
        g_hist_st[i] = g_hdep_st[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (FAILED(hr)) return fail("history textures", hr);
    hr = make_buf(dev, D3D12_HEAP_TYPE_DEFAULT, 32, true, D3D12_RESOURCE_STATE_COPY_DEST, &g_stat);
    if (SUCCEEDED(hr)) hr = make_buf(dev, D3D12_HEAP_TYPE_UPLOAD, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ, &g_zero);
    if (SUCCEEDED(hr)) hr = make_buf(dev, D3D12_HEAP_TYPE_READBACK, kSlots * kStride, false,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &g_rb);
    if (FAILED(hr)) return fail("statistics buffers", hr);
    {
        void *z = nullptr;
        D3D12_RANGE none{ 0, 0 };
        hr = g_zero->Map(0, &none, &z);
        if (FAILED(hr) || z == nullptr) return fail("statistics buffer map", hr);
        memset(z, 0, 256);
        g_zero->Unmap(0, nullptr);
        void *r = nullptr;
        D3D12_RANGE all{ 0, kSlots * kStride };
        hr = g_rb->Map(0, &all, &r);
        if (FAILED(hr) || r == nullptr) return fail("readback map", hr);
        g_rb_map = (const unsigned char *)r;
    }
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = kSlots * 2u;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_qh)))) g_qh = nullptr;   // timing is optional

    g_dev = dev;
    g_read = 0;
    g_valid = false;
    a_failed.store(false, std::memory_order_relaxed);
    a_active.store(true, std::memory_order_relaxed);
    char l[300];
    snprintf(l, sizeof l, "[MGPU][TC1] temporal stabilisation ready: %ux%u (format %d), history RGBA16F + depth "
             "on the neural card (%.1f MB). Runs once per real frame, before frame generation.",
             g_w, g_h, (int)g_fmt, (double)g_w * g_h * (8.0 * 2 + 4.0 * 2 + 4.0) / 1048576.0);
    mgpu::diag::info(l);
    return true;
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (r == nullptr || a == b) return;
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &rb);
}

void report(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (w_t0 == 0) w_t0 = now;
    if (!force && (now - w_t0 < 10000ull || w_frames == 0)) return;
    if (w_frames == 0) { w_t0 = now; return; }
    const double px = w_px ? (double)w_px : 1.0;
    const double acc = w_acc ? (double)w_acc : 1.0;
    const float shim = (float)((double)w_flk / 256.0 / acc * 1000.0);
    const float chg  = (float)((double)w_chg / 256.0 / px * 1000.0);
    const float wgt  = (float)((double)w_wgt / 256.0 / px * 100.0);
    const float off = (float)(100.0 * w_off / px), dep = (float)(100.0 * w_dep / px), col = (float)(100.0 * w_col / px);
    const float gms = w_gpu_n ? (float)(w_gpu / w_gpu_n) : 0.0f;
    a_shimmer.store(shim); a_changed.store(chg); a_weight.store(wgt);
    a_off.store(off); a_dep.store(dep); a_col.store(col); a_gpu_ms.store(gms);
    const int m = g_mode.load(std::memory_order_relaxed);
    char l[500];
    snprintf(l, sizeof l, "[MGPU][TC1] %s, strength %d%%: %llu frames, GPU %.3f ms, shimmer %.2f (x1000 of full "
             "scale, accepted pixels vs history), picture changed %.2f, history weight %.1f%%, rejected: "
             "off-screen %.1f%% depth %.1f%% colour %.1f%%, resets %u (cuts %u).",
             m == 2 ? "stabilising" : "observing (picture unchanged)", g_strength.load(), w_frames, (double)gms,
             (double)shim, (double)chg, (double)wgt, (double)off, (double)dep, (double)col, g_resets, g_cuts);
    mgpu::diag::info(l);
    if (!g_cost_said && w_gpu_n > 30 && gms > 0.5f && (unsigned long long)g_w * g_h <= 2560ull * 1440ull)
    {
        g_cost_said = true;
        char c[220];
        snprintf(c, sizeof c, "[MGPU][TC1] the stage costs %.2f ms at %ux%u - over its 0.5 ms target.", (double)gms, g_w, g_h);
        mgpu::diag::warn(c);
    }
    w_frames = w_px = w_off = w_dep = w_col = w_acc = w_flk = w_chg = w_wgt = 0;
    w_gpu = 0.0; w_gpu_n = 0; w_t0 = now;
}

// Readbacks whose list has finished.
void collect(ID3D12Fence *fence)
{
    if (fence == nullptr || g_rb_map == nullptr) return;
    const unsigned long long done = fence->GetCompletedValue();
    if (done == ~0ull) return;   // device removed
    for (unsigned i = 0; i < kSlots; ++i)
    {
        if (!g_pend[i].used || g_pend[i].fv > done) continue;
        g_pend[i].used = false;
        unsigned v[8];
        memcpy(v, g_rb_map + i * kStride, sizeof v);
        if (v[0] == 0) continue;
        w_px += v[0]; w_off += v[1]; w_dep += v[2]; w_col += v[3]; w_acc += v[4];
        w_flk += v[5]; w_chg += v[6]; w_wgt += v[7];
        // A camera cut: most of the screen had no usable history. The pixels
        // already fell back to the current frame; reset once so nothing of the
        // old shot survives in the corners.
        if (v[0] > 0 && (unsigned long long)(v[1] + v[2] + v[3]) * 100ull > (unsigned long long)v[0] * 60ull)
        { g_cut_next = true; ++g_cuts; }
        if (g_qh != nullptr && g_ts_freq > 0.0)
        {
            unsigned long long t[2];
            memcpy(t, g_rb_map + i * kStride + 32, sizeof t);
            if (t[1] > t[0]) { w_gpu += (double)(t[1] - t[0]) * 1000.0 / g_ts_freq; ++w_gpu_n; }
        }
    }
}
} // namespace

int  mode()     { int m = g_mode.load(std::memory_order_relaxed); if (m < 0) { read_keys(); m = g_mode.load(); } return m; }
void set_mode(int m)
{
    if (m < 0 || m > 2) m = 0;
    if (g_mode.exchange(m, std::memory_order_relaxed) != m) g_mode_changed.store(true, std::memory_order_relaxed);
}
int  strength() { if (g_mode.load() < 0) read_keys(); return g_strength.load(std::memory_order_relaxed); }
void set_strength(int p) { if (p < 0) p = 0; if (p > 100) p = 100; g_strength.store(p, std::memory_order_relaxed); }
int  debug()    { if (g_mode.load() < 0) read_keys(); return g_debug.load(std::memory_order_relaxed); }
void set_debug(int on) { g_debug.store(on != 0 ? 1 : 0, std::memory_order_relaxed); }

stats snapshot()
{
    stats s;
    s.active = a_active.load(); s.failed = a_failed.load();
    s.gpu_ms = a_gpu_ms.load(); s.shimmer = a_shimmer.load(); s.changed = a_changed.load();
    s.weight = a_weight.load(); s.rej_off = a_off.load(); s.rej_depth = a_dep.load(); s.rej_color = a_col.load();
    s.resets = a_resets.load(); s.cuts = a_cuts.load(); s.frames = a_frames.load();
    return s;
}

void frame(bool run, const frame_in &in)
{
    const int m = mode();
    if (m == 0 || g_off_this_arm)
    {
        if (g_valid) g_valid = false;   // switched off: the next start is fresh
        g_last_mode = m;
        if (m == 0 && w_frames != 0) report(true);
        return;
    }
    if (g_dev != nullptr) collect(in.fence);
    if (!run || in.dev == nullptr || in.cl == nullptr || in.out == nullptr)
    {
        g_valid = false;   // a real frame the stage did not see
        return;
    }
    if (g_dev == nullptr)
    {
        if (!build(in.dev, in.out)) return;
        if (in.q != nullptr)
        {
            UINT64 fq = 0;
            if (SUCCEEDED(in.q->GetTimestampFrequency(&fq)) && fq != 0) g_ts_freq = (double)fq;
        }
    }
    else if (g_dev != in.dev)
    {
        // Not freed here: the old device's lists may still use them. release()
        // (stream teardown, GPU idle) frees them.
        mgpu::diag::error("[MGPU][TC1] the neural device changed without a release - temporal stabilisation "
                          "is off until the stream restarts.");
        g_off_this_arm = true;
        a_failed.store(true, std::memory_order_relaxed);
        return;
    }
    {
        const D3D12_RESOURCE_DESC od = in.out->GetDesc();
        if ((unsigned)od.Width != g_w || od.Height != g_h || od.Format != g_fmt)
        {
            // The old textures may still be in flight: do not free them here.
            char l[200];
            snprintf(l, sizeof l, "[MGPU][TC1] the neural output changed size or format (%ux%u -> %llux%u) - "
                     "temporal stabilisation is off until the stream restarts.", g_w, g_h,
                     (unsigned long long)od.Width, od.Height);
            mgpu::diag::warn(l);
            g_off_this_arm = true;
            a_failed.store(true, std::memory_order_relaxed);
            return;
        }
    }
    if (!g_said_on)
    {
        g_said_on = true;
        char l[260];
        snprintf(l, sizeof l, "[MGPU][TC1] temporal stabilisation %s (strength %d%%%s). TemporalStab=0 turns it off.",
                 m == 2 ? "ON - the NR output is blended with its own reprojected history"
                        : "OBSERVING - history and statistics only, the picture is unchanged",
                 g_strength.load(), g_debug.load() ? ", rejection view" : "");
        mgpu::diag::info(l);
    }

    // When the history cannot be trusted this frame.
    bool reset = !g_valid || in.nr_reset || g_cut_next || m != g_last_mode ||
                 g_mode_changed.exchange(false, std::memory_order_relaxed) ||
                 in.mv == nullptr || in.mv_w == 0 || in.mv_h == 0 || in.mv_el > 8.0f;
    g_cut_next = false;
    g_last_mode = m;
    if (reset) { ++g_resets; a_resets.store(g_resets, std::memory_order_relaxed); }
    const bool has_d = in.depth != nullptr && in.d_w != 0 && in.d_h != 0;

    ID3D12GraphicsCommandList *cl = in.cl;
    const unsigned slot = g_slot;
    g_slot = (g_slot + 1u) % kSlots;
    const bool slot_free = !g_pend[slot].used;   // a list still in flight owns it: skip its statistics
    if (g_qh != nullptr && slot_free) cl->EndQuery(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u);

    // 1. the current NR output into a texture the kernel can read
    barrier(cl, in.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(cl, g_cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(g_cur, in.out);
    barrier(cl, g_cur, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, in.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // 2. clear the statistics
    cl->CopyBufferRegion(g_stat, 0, g_zero, 0, 32);
    barrier(cl, g_stat, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // 3. history: read side to shader resource, write side stays UAV
    const unsigned rd = g_read, wr = g_read ^ 1u;
    barrier(cl, g_hist[rd], g_hist_st[rd], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cl, g_hdep[rd], g_hdep_st[rd], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_hist_st[rd] = g_hdep_st[rd] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier(cl, g_hist[wr], g_hist_st[wr], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, g_hdep[wr], g_hdep_st[wr], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_hist_st[wr] = g_hdep_st[wr] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // 4. descriptors (per parity, so a list still being read is not rewritten)
    ID3D12Device *dev = in.dev;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)(in.parity & 1u) * kDescPerSet * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    const DXGI_FORMAT vf = typed(g_fmt);
    sv.Format = vf;                                dev->CreateShaderResourceView(g_cur, &sv, h); h.ptr += g_inc;
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;    dev->CreateShaderResourceView(g_hist[rd], &sv, h); h.ptr += g_inc;
    sv.Format = in.mv != nullptr ? in.mv_fmt : DXGI_FORMAT_R16G16_FLOAT;
    dev->CreateShaderResourceView(in.mv, &sv, h); h.ptr += g_inc;
    sv.Format = has_d ? in.depth->GetDesc().Format : DXGI_FORMAT_R32_FLOAT;
    dev->CreateShaderResourceView(has_d ? in.depth : nullptr, &sv, h); h.ptr += g_inc;
    sv.Format = DXGI_FORMAT_R32_FLOAT;             dev->CreateShaderResourceView(g_hdep[rd], &sv, h); h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = vf;                                dev->CreateUnorderedAccessView(in.out, nullptr, &uv, h); h.ptr += g_inc;
    uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;    dev->CreateUnorderedAccessView(g_hist[wr], nullptr, &uv, h); h.ptr += g_inc;
    uv.Format = DXGI_FORMAT_R32_FLOAT;             dev->CreateUnorderedAccessView(g_hdep[wr], nullptr, &uv, h); h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bv{};
    bv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bv.Format = DXGI_FORMAT_R32_TYPELESS;
    bv.Buffer.NumElements = 8;
    bv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(g_stat, nullptr, &bv, h);

    // 5. the kernel
    ID3D12DescriptorHeap *hs[1] = { g_heap };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)(in.parity & 1u) * kDescPerSet * g_inc;
    cl->SetComputeRootDescriptorTable(0, gh);
    unsigned flags = 0;
    if (has_d) flags |= F_DEPTH;
    if (has_d && in.d_inverted) flags |= F_DINV;
    if (in.mv != nullptr && in.mv_w != 0 && in.mv_h != 0) flags |= F_MV;
    if (reset) flags |= F_RESET;
    if (m == 2) flags |= F_APPLY;
    if (g_debug.load(std::memory_order_relaxed) != 0) flags |= F_DEBUG;
    if (is_float(vf)) flags |= F_FLOAT;
    const float str = 0.9f * (float)g_strength.load(std::memory_order_relaxed) / 100.0f;
    const float gamma = 1.0f, dtol = 0.10f;
    UINT c[13];
    c[0] = g_w; c[1] = g_h;
    c[2] = in.mv_w ? in.mv_w : 1u; c[3] = in.mv_h ? in.mv_h : 1u;
    c[4] = has_d ? in.d_w : 1u; c[5] = has_d ? in.d_h : 1u;
    memcpy(&c[6], &in.mv_sx, 4); memcpy(&c[7], &in.mv_sy, 4);
    c[8] = flags;
    memcpy(&c[9], &str, 4); memcpy(&c[10], &gamma, 4); memcpy(&c[11], &dtol, 4);
    c[12] = 0;
    cl->SetComputeRoot32BitConstants(1, 13, c, 0);
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1);

    // 6. after: the output and the new history are ready for their readers
    D3D12_RESOURCE_BARRIER ub[3] = {};
    ub[0].Type = ub[1].Type = ub[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub[0].UAV.pResource = in.out; ub[1].UAV.pResource = g_hist[wr]; ub[2].UAV.pResource = g_hdep[wr];
    cl->ResourceBarrier(3, ub);
    barrier(cl, g_stat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (slot_free)
    {
        cl->CopyBufferRegion(g_rb, (UINT64)slot * kStride, g_stat, 0, 32);
        if (g_qh != nullptr)
        {
            cl->EndQuery(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u + 1u);
            cl->ResolveQueryData(g_qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u, 2, g_rb, (UINT64)slot * kStride + 32u);
        }
        if (in.fence != nullptr) { g_pend[slot].used = true; g_pend[slot].fv = in.fence_value; }
    }
    barrier(cl, g_stat, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    g_read = wr;
    g_valid = true;
    ++g_frames; ++w_frames;
    a_frames.store(g_frames, std::memory_order_relaxed);
    report(false);
}

void release()
{
    if (w_frames != 0) report(true);
    release_all();
    g_off_this_arm = false;
    g_said_on = false;
    g_cut_next = false;
    g_cost_said = false;
    a_failed.store(false, std::memory_order_relaxed);
}
}
