// ---------------------------------------------------------------------------
// scaler.cpp - SC1. See scaler.hpp.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <climits>
#include <string>
#include "d3dc.hpp"
#include <cmath>

#include "scaler.hpp"
#include "diag.hpp"
#include "cursormap.hpp"

// CPU-side constant setup from the vendors' own headers (MIT).
#define A_CPU 1
#include "third_party/fsr1/ffx_a.h"
#include "third_party/fsr1/ffx_fsr1.h"
#include "third_party/nis/NIS_Config.h"

#include "scaler_src.inl"

namespace mgpu
{
namespace scaler
{
namespace
{

std::atomic<int>  g_mode{0};
std::atomic<int>  g_sharp{50};
std::atomic<bool> g_failed{false};
// MAS1: motion-adaptive sharpening. On, the sharpening of each pixel is scaled
// between full (still) and g_mas_move % of it (moving 8+ px per frame), from a
// motion mask the neural stage builds out of the game's motion vectors.
std::atomic<int>  g_mas{0};
std::atomic<int>  g_mas_move{30};
const unsigned kMasTile = 16u;     // source pixels per mask cell

// ---- the shaders ----

const char *kFsrHLSL =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n"
    "#include \"ffx_a.h\"\n"
    "cbuffer C : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; };\n"
    "Texture2D InputTexture : register(t0);\n"
    "RWTexture2D<float4> OutputTexture : register(u0);\n"
    "SamplerState samLinearClamp : register(s0);\n"
    "RWStructuredBuffer<float> mas_mask : register(u1);\n"   // MAS1
    "#if SC_EASU\n"
    "#define FSR_EASU_F 1\n"
    "AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(samLinearClamp, p, int2(0,0)); }\n"
    "AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(samLinearClamp, p, int2(0,0)); }\n"
    "AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(samLinearClamp, p, int2(0,0)); }\n"
    "#else\n"
    "#define FSR_RCAS_F 1\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "#endif\n"
    "#include \"ffx_fsr1.h\"\n"
    "void Filter(AU2 pos)\n"
    "{\n"
    "#if SC_EASU\n"
    "  AF3 c; FsrEasuF(c, pos, Const0, Const1, Const2, Const3);\n"
    "  OutputTexture[pos] = AF4(c, 1.0);\n"
    "#else\n"
    "  AF3 c; FsrRcasF(c.r, c.g, c.b, pos, Const0);\n"
    // MAS1: Const1 = (scale x, on, cells x, cells y), Const2 = (scale y, motion share).
    "  if (Const1.y != 0u) {\n"
    "    float2 sp = (float2(pos) + 0.5f) * float2(asfloat(Const1.x), asfloat(Const2.x));\n"
    "    float2 t = sp / 16.0f - 0.5f; int2 i = int2(floor(t)); float2 f = t - float2(i);\n"
    "    int2 mx = int2(Const1.z, Const1.w) - 1;\n"
    "    int2 a = clamp(i, int2(0,0), mx), b = clamp(i + int2(1,1), int2(0,0), mx);\n"
    "    float m = lerp(lerp(mas_mask[a.y * Const1.z + a.x], mas_mask[a.y * Const1.z + b.x], f.x),\n"
    "                   lerp(mas_mask[b.y * Const1.z + a.x], mas_mask[b.y * Const1.z + b.x], f.x), f.y);\n"
    "    float k = lerp(1.0f, asfloat(Const2.y), saturate((m - 1.0f) / 7.0f));\n"
    "    AF3 soft = InputTexture.Load(int3(pos, 0)).rgb;\n"
    "    c = lerp(soft, c, k);\n"
    "  }\n"
    "  OutputTexture[pos] = AF4(c, 1.0);\n"
    "#endif\n"
    "}\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 L : SV_GroupThreadID, uint3 W : SV_GroupID)\n"
    "{\n"
    "  AU2 gxy = ARmp8x8(L.x) + AU2(W.x << 4u, W.y << 4u);\n"
    "  Filter(gxy); gxy.x += 8u; Filter(gxy); gxy.y += 8u; Filter(gxy); gxy.x -= 8u; Filter(gxy);\n"
    "}\n";

// NIS: the vendor's NIS_Main.hlsl entry, without its Vulkan binding macros.
const char *kNisHLSL =
    "#define NIS_HLSL 1\n"
    "cbuffer cb : register(b0)\n"
    "{\n"
    "  float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;\n"
    "  float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;\n"
    "  float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;\n"
    "  float kScaleX; float kScaleY;\n"
    "  float kDstNormX; float kDstNormY; float kSrcNormX; float kSrcNormY;\n"
    "  uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;\n"
    "  uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;\n"
    // MAS1: NIS's two reserved values carry the motion share and on/off; the
    // two DWORDs after NIS's 30 carry the mask size (the root constants are 32).
    "  float masMove; float masOn; uint masTW; uint masTH;\n"
    "};\n"
    "SamplerState samplerLinearClamp : register(s0);\n"
    "Texture2D in_texture : register(t0);\n"
    "RWTexture2D<float4> out_texture : register(u0);\n"
    "Texture2D coef_scaler : register(t1);\n"
    "Texture2D coef_usm : register(t2);\n"
    "RWStructuredBuffer<float> mas_mask : register(u1);\n"
    "static float g_mas = 1.0f;\n"
    "float mas_cell(int2 i) { i = clamp(i, int2(0,0), int2(masTW, masTH) - 1); return mas_mask[i.y * masTW + i.x]; }\n"
    "float mas_weight(float2 sp)\n"
    "{\n"
    "  if (masOn < 0.5f) return 1.0f;\n"
    "  float2 t = sp / 16.0f - 0.5f; int2 i = int2(floor(t)); float2 f = t - float2(i);\n"
    "  float m = lerp(lerp(mas_cell(i), mas_cell(i + int2(1,0)), f.x), lerp(mas_cell(i + int2(0,1)), mas_cell(i + int2(1,1)), f.x), f.y);\n"
    "  return lerp(1.0f, masMove, saturate((m - 1.0f) / 7.0f));\n"
    "}\n"
    "#define NIS_MAS_SCALE g_mas\n"
    "#define NIS_MAS_HOOK(dx, dy) g_mas = mas_weight((float2(dx, dy) + 0.5f) * float2(kScaleX, kScaleY))\n"
    "#include \"NIS_Scaler.h\"\n"
    "[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]\n"
    "void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)\n"
    "{\n"
    "  NVScaler(blockIdx.xy, threadIdx.x);\n"
    "}\n";

const char *kBilinearHLSL =
    "cbuffer C : register(b0) { uint2 gSrc; uint2 gDst; };\n"
    "Texture2D<float4> gIn : register(t0);\n"
    "RWTexture2D<float4> gOut : register(u0);\n"
    "SamplerState gLin : register(s0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gDst.x || id.y >= gDst.y) return;\n"
    "  float2 uv = (float2(id.xy) + 0.5f) / float2(gDst);\n"
    "  gOut[id.xy] = float4(gIn.SampleLevel(gLin, uv, 0).rgb, 1.0f);\n"
    "}\n";

const char *kCursorHLSL =
    "cbuffer C : register(b0) { float4 gRect; float2 gDst; float2 pad; };\n"
    "Texture2D<float4> gCur : register(t0);\n"
    "SamplerState gPt : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID)\n"
    "{\n"
    "  float2 c = float2(id & 1u, id >> 1u);\n"
    "  float2 px = gRect.xy + c * gRect.zw;\n"
    "  V o; o.p = float4(px / gDst * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = c; return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target { return gCur.Sample(gPt, i.uv); }\n";

const unsigned NIS_BW = 32, NIS_BH = 24, NIS_TG = 128;   // NIS's own choice for NVIDIA

// Serves the embedded vendor headers to D3DCompile.
// MAS1: NIS_Scaler.h as served to the compiler gets two small edits, made
// here so the vendor file stays as shipped: its sharpening term is scaled per
// pixel (NIS_MAS_SCALE), and a hook sets that scale for each output pixel just
// before the directional filters run (NIS_MAS_HOOK). If either spot is not
// found, the original is served and sharpening stays uniform.
std::string g_nis_patched;
bool g_nis_patch_ok = false;
const std::string &nis_source(const unsigned char *d, size_t n)
{
    if (!g_nis_patched.empty()) return g_nis_patched;
    std::string t((const char *)d, n);
    const char *a = "y_usm *= y_sharpness;";
    const char *b = "opY += AddDirFilters(p, fx, fy, fx_int, fy_int, w);";
    const size_t ia = t.find(a), ib = t.find(b);
    if (ia != std::string::npos && ib != std::string::npos && t.find(a, ia + 1) == std::string::npos)
    {
        t.replace(ib, std::strlen(b), std::string("NIS_MAS_HOOK(dstX, dstY); ") + b);
        const size_t ia2 = t.find(a);
        t.replace(ia2, std::strlen(a), "y_usm *= y_sharpness * NIS_MAS_SCALE;");
        g_nis_patch_ok = true;
    }
    g_nis_patched = t;
    return g_nis_patched;
}

struct embedded_include : public ID3DInclude
{
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
    {
        for (const auto &f : scaler_src::kFiles)
        {
            const char *base = std::strrchr(name, '/');
            const char *b2   = std::strrchr(name, '\\');
            if (b2 > base) base = b2;
            base = base ? base + 1 : name;
            if (_stricmp(base, f.name) == 0)
            {
                if (_stricmp(f.name, "NIS_Scaler.h") == 0)
                {
                    const std::string &p = nis_source(f.data, f.size);
                    *data = p.data(); *bytes = (UINT)p.size();
                    return S_OK;
                }
                *data = f.data; *bytes = f.size; return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};

typedef HRESULT (WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

struct state
{
    std::mutex mx;
    ID3D12Device *dev = nullptr;
    bool built = false;
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso_easu = nullptr, *pso_rcas = nullptr, *pso_nis = nullptr, *pso_bil = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    UINT inc = 0;
    ID3D12Resource *coef_s = nullptr, *coef_u = nullptr, *coef_up = nullptr;
    bool coef_uploaded = false;
    ID3D12Resource *tmp = nullptr, *out = nullptr;
    // MC1: the pointer sprite.
    ID3D12RootSignature *cur_rs = nullptr;
    ID3D12PipelineState *cur_pso = nullptr;
    DXGI_FORMAT cur_fmt = DXGI_FORMAT_UNKNOWN;
    bool cur_failed = false;
    ID3D12Resource *cur_tex = nullptr, *cur_up = nullptr;
    unsigned cur_w = 0, cur_h = 0, cur_serial = 0;
    ID3D12DescriptorHeap *rtv = nullptr;
    unsigned tw = 0, th = 0;
    DXGI_FORMAT tfmt = DXGI_FORMAT_UNKNOWN;
    bool said = false;
};
state g_s;

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b) return;
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &rb);
}

template <class T> void relt(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }

ID3D12PipelineState *make_pso(ID3D12Device *dev, ID3D12RootSignature *rs, pfn_compile cmp,
                              const char *src, const D3D_SHADER_MACRO *defs, const char *what)
{
    embedded_include inc;
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = cmp(src, std::strlen(src), what, defs, &inc, "main", "cs_5_1",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    if (FAILED(hr) || cs == nullptr)
    {
        char l[900];
        snprintf(l, sizeof l, "[MGPU][SC1] %s shader failed to compile: 0x%08X %.700s", what, (unsigned)hr,
                 err ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(l);
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    ID3D12PipelineState *pso = nullptr;
    hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    cs->Release();
    if (FAILED(hr))
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][SC1] %s pipeline state failed: 0x%08X", what, (unsigned)hr);
        mgpu::diag::error(l);
        return nullptr;
    }
    return pso;
}

// SC4: NOT sizeof(NISConfig). NIS pads that struct to 256 bytes (64 DWORDs), and
// 64 root constants plus the descriptor table is 65 - one over D3D12's 64-DWORD
// root signature limit, so the signature was refused and the scaler never built.
// The NIS cbuffer is 30 values, which HLSL packs into 8 registers = 32 DWORDs.
const UINT kRootDwords = 32u;
static_assert(offsetof(NISConfig, reserved1) + 4u <= kRootDwords * 4u, "NIS constants do not fit");

bool build(ID3D12Device *dev)
{
    pfn_compile cmp = (pfn_compile)mgpu::d3dcompile();   // RV1
    if (cmp == nullptr) { mgpu::diag::error("[MGPU][SC1] d3dcompiler_47.dll not available - scaler off."); return false; }

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 3; rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 2; rng[1].OffsetInDescriptorsFromTableStart = 3;   // MAS1: u1 = motion mask
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = kRootDwords;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        char l[600];
        snprintf(l, sizeof l, "[MGPU][SC1] root signature refused: 0x%08X %.450s", (unsigned)hr,
                 err ? (const char *)err->GetBufferPointer() : "");
        mgpu::diag::error(l);
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_s.rs));
    sig->Release();
    if (FAILED(hr))
    {
        char l[160];
        snprintf(l, sizeof l, "[MGPU][SC1] CreateRootSignature failed: 0x%08X", (unsigned)hr);
        mgpu::diag::error(l);
        return false;
    }

    const D3D_SHADER_MACRO easu[] = { { "SC_EASU", "1" }, { nullptr, nullptr } };
    const D3D_SHADER_MACRO rcas[] = { { "SC_EASU", "0" }, { nullptr, nullptr } };
    char bw[8], bh[8], tg[8];
    snprintf(bw, sizeof bw, "%u", NIS_BW); snprintf(bh, sizeof bh, "%u", NIS_BH); snprintf(tg, sizeof tg, "%u", NIS_TG);
    const D3D_SHADER_MACRO nis[] = { { "NIS_SCALER", "1" }, { "NIS_HDR_MODE", "0" }, { "NIS_BLOCK_WIDTH", bw },
                                     { "NIS_BLOCK_HEIGHT", bh }, { "NIS_THREAD_GROUP_SIZE", tg },
                                     { "NIS_USE_HALF_PRECISION", "0" }, { nullptr, nullptr } };
    g_s.pso_easu = make_pso(dev, g_s.rs, cmp, kFsrHLSL, easu, "FSR1 EASU");
    g_s.pso_rcas = make_pso(dev, g_s.rs, cmp, kFsrHLSL, rcas, "FSR1 RCAS");
    g_s.pso_nis  = make_pso(dev, g_s.rs, cmp, kNisHLSL, nis, "NIS");
    g_s.pso_bil  = make_pso(dev, g_s.rs, cmp, kBilinearHLSL, nullptr, "bilinear");
    if (g_s.pso_bil == nullptr) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kMaxBackBuffers * 2u * 5u + 1u;   // BB1: kMaxBackBuffers back buffers x 2 passes x 5 (MAS1: + mask), + the pointer (MC1)
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_s.heap))))
    { mgpu::diag::error("[MGPU][SC1] descriptor heap could not be created."); return false; }
    g_s.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // NIS coefficient tables: 2 x 64 texels of float4 each.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kFilterSize / 4; td.Height = kPhaseCount;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_s.coef_s))) ||
        FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_s.coef_u))))
    { relt(g_s.pso_nis); }
    else
    {
        const UINT row = 256u;   // >= 2 x 16 bytes, D3D12 row pitch alignment
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2ull * row * kPhaseCount; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&g_s.coef_up))))
        {
            unsigned char *p = nullptr;
            if (SUCCEEDED(g_s.coef_up->Map(0, nullptr, (void **)&p)))
            {
                for (size_t r = 0; r < kPhaseCount; ++r)
                {
                    std::memcpy(p + r * row, coef_scale[r], kFilterSize * sizeof(float));
                    std::memcpy(p + (size_t)row * kPhaseCount + r * row, coef_usm[r], kFilterSize * sizeof(float));
                }
                g_s.coef_up->Unmap(0, nullptr);
            }
        }
        if (g_s.coef_up == nullptr) relt(g_s.pso_nis);
    }
    char l[300];
    snprintf(l, sizeof l, "[MGPU][SC1] scaler built: FSR1 %s, NIS %s, bilinear ready.",
             (g_s.pso_easu && g_s.pso_rcas) ? "ready" : "UNAVAILABLE", g_s.pso_nis ? "ready" : "UNAVAILABLE");
    mgpu::diag::info(l);
    return true;
}

bool ensure_targets(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    if (g_s.out != nullptr && g_s.tw == w && g_s.th == h && g_s.tfmt == fmt) return true;
    relt(g_s.tmp); relt(g_s.out);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // FSR 1's EASU result, read by RCAS
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&g_s.tmp)))) return false;
    td.Format = fmt;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&g_s.out)))) { relt(g_s.tmp); return false; }
    g_s.tw = w; g_s.th = h; g_s.tfmt = fmt;
    return true;
}

// ---- MAS1: the motion mask ----
// One float per 16x16 cell of the game's frame: the fastest motion in it, in
// display pixels per frame. Written on the neural stage's list from the motion
// vectors, read by the scaler on every shown frame. A BUFFER, created in
// COMMON and never transitioned: buffers promote implicitly on each queue, so
// the two queues can use it without a cross-queue barrier. A read that lands
// mid-write only mixes two frames' masks for a few cells - harmless.
struct mask_state
{
    std::mutex mx;
    ID3D12Device *dev = nullptr;
    bool failed = false;
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;   // 2 x (SRV mv, UAV mask)
    UINT inc = 0;
    ID3D12Resource *buf = nullptr;
    unsigned cw = 0, ch = 0, src_w = 0, src_h = 0;
    unsigned long long t_ms = 0;            // last write
    unsigned par = 0;
};
mask_state g_m;

const char *kMasHLSL =
    "Texture2D<float2> gMV : register(t0);\n"
    "RWStructuredBuffer<float> gMask : register(u0);\n"
    "cbuffer C : register(b0) { uint2 gMvSz; uint2 gCells; float2 gToPx; uint2 pad; };\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  uint n = gCells.x * gCells.y; if (id.x >= n) return;\n"
    "  uint cx = id.x % gCells.x, cy = id.x / gCells.x;\n"
    "  float m = 0.0f;\n"
    "  [unroll] for (int y = 0; y < 4; ++y) [unroll] for (int x = 0; x < 4; ++x) {\n"
    "    float2 uv = (float2(cx, cy) + (float2(x, y) + 0.5f) / 4.0f) / float2(gCells);\n"
    "    int2 p = clamp(int2(uv * float2(gMvSz)), int2(0,0), int2(gMvSz) - 1);\n"
    "    m = max(m, length(gMV.Load(int3(p, 0)) * gToPx));\n"
    "  }\n"
    "  gMask[id.x] = m;\n"
    "}\n";

// Writes one pass's 5 descriptors (3 SRV + 2 UAV) and returns its GPU handle.
// MAS1: u1 is the motion mask, or a null view when there is none.
D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Device *dev, unsigned slot, ID3D12Resource *s0, ID3D12Resource *s1,
                                  ID3D12Resource *s2, ID3D12Resource *u0, ID3D12Resource *mask = nullptr,
                                  unsigned mask_n = 0)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_s.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)slot * 5u * g_s.inc;
    ID3D12Resource *srv[3] = { s0, s1, s2 };
    for (unsigned i = 0; i < 3u; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        sv.Format = srv[i] ? srv[i]->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(srv[i], &sv, c);
        c.ptr += g_s.inc;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = u0->GetDesc().Format;
    dev->CreateUnorderedAccessView(u0, nullptr, &uv, c);
    c.ptr += g_s.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC mb{};
    mb.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; mb.Format = DXGI_FORMAT_UNKNOWN;
    mb.Buffer.StructureByteStride = 4u;
    mb.Buffer.NumElements = (mask != nullptr && mask_n != 0u) ? mask_n : 1u;
    dev->CreateUnorderedAccessView(mask, nullptr, &mb, c);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_s.heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)slot * 5u * g_s.inc;
    return g;
}

} // namespace

int  mode()                 { return g_mode.load(std::memory_order_relaxed); }
void set_mode(int m)        { if (m < 0 || m > 3) m = 0; g_mode.store(m, std::memory_order_relaxed); }
int  sharpness()            { return g_sharp.load(std::memory_order_relaxed); }
int  mas()                  { return g_mas.load(std::memory_order_relaxed); }
void set_mas(int on)        { g_mas.store(on ? 1 : 0, std::memory_order_relaxed); }
int  mas_motion()           { return g_mas_move.load(std::memory_order_relaxed); }
void set_mas_motion(int pct){ if (pct < 0) pct = 0; if (pct > 100) pct = 100; g_mas_move.store(pct, std::memory_order_relaxed); }

void mas_record(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *mv, DXGI_FORMAT mv_fmt,
                unsigned mv_w, unsigned mv_h, float to_px_x, float to_px_y, unsigned src_w, unsigned src_h)
{
    if (g_mas.load(std::memory_order_relaxed) == 0 || dev == nullptr || cl == nullptr || mv == nullptr ||
        mv_w == 0 || mv_h == 0 || src_w == 0 || src_h == 0)
        return;
    std::lock_guard<std::mutex> lk(g_m.mx);
    if (g_m.failed || (g_m.dev != nullptr && g_m.dev != dev)) return;
    if (g_m.dev == nullptr)
    {
        g_m.dev = dev;
        auto fail = [](const char *why) { g_m.failed = true; char l[200];
            snprintf(l, sizeof l, "[MGPU][MAS1] motion-adaptive sharpening unavailable: %s. Sharpening stays uniform.", why);
            mgpu::diag::warn(l); };
        pfn_compile cmp = (pfn_compile)mgpu::d3dcompile();   // RV1
        if (cmp == nullptr) { fail("no d3dcompiler"); return; }
        D3D12_DESCRIPTOR_RANGE rng[2] = {};
        rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 1; rng[0].OffsetInDescriptorsFromTableStart = 0;
        rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 1; rng[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.Num32BitValues = 8;
        D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = rp;
        ID3DBlob *sig = nullptr, *err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err) err->Release();
        if (SUCCEEDED(hr)) { hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_m.rs)); sig->Release(); }
        if (FAILED(hr)) { fail("root signature"); return; }
        ID3DBlob *cs = nullptr;
        hr = cmp(kMasHLSL, std::strlen(kMasHLSL), "mas", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
        if (err) err->Release();
        if (FAILED(hr) || cs == nullptr) { fail("shader compile"); return; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = g_m.rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_m.pso));
        cs->Release();
        if (FAILED(hr)) { fail("pipeline"); return; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_m.heap)))) { fail("descriptor heap"); return; }
        g_m.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        mgpu::diag::info(g_nis_patch_ok || g_nis_patched.empty()
                             ? "[MGPU][MAS1] motion-adaptive sharpening ready (mask from the game's motion vectors)."
                             : "[MGPU][MAS1] NIS source not as expected - motion-adaptive sharpening works with FSR 1 only.");
    }
    const unsigned cw = (src_w + kMasTile - 1u) / kMasTile, ch = (src_h + kMasTile - 1u) / kMasTile;
    if (g_m.buf == nullptr || g_m.cw != cw || g_m.ch != ch)
    {
        // A size change leaves the old buffer to a reader that may still use it: it
        // is released only at teardown (it is a few KB).
        static ID3D12Resource *s_old[8] = {}; static unsigned s_on = 0;
        if (g_m.buf != nullptr && s_on < 8u) s_old[s_on++] = g_m.buf;
        g_m.buf = nullptr;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)cw * ch * 4ull; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&g_m.buf))))
        { g_m.buf = nullptr; return; }
        g_m.cw = cw; g_m.ch = ch;
    }
    g_m.src_w = src_w; g_m.src_h = src_h;
    g_m.par ^= 1u;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_m.heap->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)g_m.par * 2u * g_m.inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Format = mv_fmt;
    dev->CreateShaderResourceView(mv, &sv, c);
    c.ptr += g_m.inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Format = DXGI_FORMAT_UNKNOWN;
    uv.Buffer.NumElements = cw * ch; uv.Buffer.StructureByteStride = 4u;
    dev->CreateUnorderedAccessView(g_m.buf, nullptr, &uv, c);
    ID3D12DescriptorHeap *hs[1] = { g_m.heap };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_m.rs);
    cl->SetPipelineState(g_m.pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_m.heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)g_m.par * 2u * g_m.inc;
    cl->SetComputeRootDescriptorTable(0, g);
    UINT k[8] = { mv_w, mv_h, cw, ch, 0, 0, 0, 0 };
    std::memcpy(&k[4], &to_px_x, 4); std::memcpy(&k[5], &to_px_y, 4);
    cl->SetComputeRoot32BitConstants(1, 8, k, 0);
    cl->Dispatch((cw * ch + 63u) / 64u, 1u, 1u);
    g_m.t_ms = GetTickCount64();
}
void set_sharpness(int pct) { if (pct < 0) pct = 0; if (pct > 100) pct = 100; g_sharp.store(pct, std::memory_order_relaxed); }

bool run(ID3D12Device *dev, ID3D12GraphicsCommandList *cl,
         ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, unsigned sw, unsigned sh,
         ID3D12Resource *bb, unsigned dw, unsigned dh, DXGI_FORMAT fmt, unsigned bb_index)
{
    if (dev == nullptr || cl == nullptr || src == nullptr || bb == nullptr || sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return false;
    if (g_failed.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(g_s.mx);
    if (!g_s.built)
    {
        g_s.built = true;
        g_s.dev = dev;
        if (!build(dev)) { g_failed.store(true); mgpu::diag::error("[MGPU][SC1] scaler could not be built - plain copy used."); return false; }
    }
    if (dev != g_s.dev) return false;
    if (!ensure_targets(dev, dw, dh, fmt)) return false;

    int m = g_mode.load(std::memory_order_relaxed);
    if (m == 2 && (g_s.pso_easu == nullptr || g_s.pso_rcas == nullptr)) m = 1;
    if (m == 3 && g_s.pso_nis == nullptr) m = 1;
    if (m == 0) m = 1;   // the chain is at monitor size: enlarge plainly
    const float sharp = (float)g_sharp.load(std::memory_order_relaxed) / 100.0f;
    // MAS1: the mask, if it is on, fresh (written in the last 250 ms) and of this frame's size.
    ID3D12Resource *mbuf = nullptr; unsigned mcw = 0, mch = 0;
    if (g_mas.load(std::memory_order_relaxed) != 0)
    {
        std::lock_guard<std::mutex> ml(g_m.mx);
        if (g_m.buf != nullptr && g_m.dev == dev && g_m.src_w == sw && g_m.src_h == sh &&
            GetTickCount64() - g_m.t_ms < 250ull)
        { mbuf = g_m.buf; mcw = g_m.cw; mch = g_m.ch; }
    }
    const float mmove = (float)g_mas_move.load(std::memory_order_relaxed) / 100.0f;
    const float msx = (float)sw / (float)dw, msy = (float)sh / (float)dh;

    if (m == 3 && !g_s.coef_uploaded)
    {
        const UINT row = 256u;
        ID3D12Resource *dst[2] = { g_s.coef_s, g_s.coef_u };
        for (unsigned k = 0; k < 2u; ++k)
        {
            D3D12_TEXTURE_COPY_LOCATION d{}, s{};
            d.pResource = dst[k]; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            s.pResource = g_s.coef_up; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint.Offset = (UINT64)k * row * kPhaseCount;
            s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            s.PlacedFootprint.Footprint.Width = kFilterSize / 4; s.PlacedFootprint.Footprint.Height = kPhaseCount;
            s.PlacedFootprint.Footprint.Depth = 1; s.PlacedFootprint.Footprint.RowPitch = row;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            barrier(cl, dst[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        g_s.coef_uploaded = true;
    }

    barrier(cl, src, src_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heaps[1] = { g_s.heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_s.rs);
    const unsigned set = (bb_index % kMaxBackBuffers) * 2u;   // BB1: a set per buffer, or a frame still on the GPU loses its descriptors
    UINT k[64] = {};

    if (m == 2)
    {
        AU1 c0[4], c1[4], c2[4], c3[4];
        FsrEasuCon(c0, c1, c2, c3, (AF1)sw, (AF1)sh, (AF1)sw, (AF1)sh, (AF1)dw, (AF1)dh);
        std::memcpy(k, c0, 16); std::memcpy(k + 4, c1, 16); std::memcpy(k + 8, c2, 16); std::memcpy(k + 12, c3, 16);
        cl->SetPipelineState(g_s.pso_easu);
        cl->SetComputeRootDescriptorTable(0, table(dev, set, src, nullptr, nullptr, g_s.tmp));
        cl->SetComputeRoot32BitConstants(1, kRootDwords, k, 0);
        cl->Dispatch((dw + 15u) / 16u, (dh + 15u) / 16u, 1u);
        barrier(cl, g_s.tmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        AU1 rc[4];
        FsrRcasCon(rc, (1.0f - sharp) * 2.0f);   // 0 stops = sharpest
        std::memset(k, 0, sizeof k); std::memcpy(k, rc, 16);
        if (mbuf != nullptr)   // MAS1: Const1 / Const2
        {
            std::memcpy(&k[4], &msx, 4); k[5] = 1u; k[6] = mcw; k[7] = mch;
            std::memcpy(&k[8], &msy, 4); std::memcpy(&k[9], &mmove, 4);
        }
        cl->SetPipelineState(g_s.pso_rcas);
        cl->SetComputeRootDescriptorTable(0, table(dev, set + 1u, g_s.tmp, nullptr, nullptr, g_s.out, mbuf, mcw * mch));
        cl->SetComputeRoot32BitConstants(1, kRootDwords, k, 0);
        cl->Dispatch((dw + 15u) / 16u, (dh + 15u) / 16u, 1u);
        barrier(cl, g_s.tmp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else if (m == 3)
    {
        NISConfig cfg{};
        NVScalerUpdateConfig(cfg, sharp, 0, 0, sw, sh, sw, sh, 0, 0, dw, dh, dw, dh, NISHDRMode::None);
        std::memcpy(k, &cfg, sizeof cfg);
        {   // MAS1: masMove, masOn, cell counts (DWORDs 28-31)
            const float on = (mbuf != nullptr && g_nis_patch_ok) ? 1.0f : 0.0f;
            std::memcpy(&k[28], &mmove, 4); std::memcpy(&k[29], &on, 4); k[30] = mcw; k[31] = mch;
        }
        cl->SetPipelineState(g_s.pso_nis);
        cl->SetComputeRootDescriptorTable(0, table(dev, set, src, g_s.coef_s, g_s.coef_u, g_s.out, mbuf, mcw * mch));
        cl->SetComputeRoot32BitConstants(1, kRootDwords, k, 0);
        cl->Dispatch((dw + NIS_BW - 1u) / NIS_BW, (dh + NIS_BH - 1u) / NIS_BH, 1u);
    }
    else
    {
        k[0] = sw; k[1] = sh; k[2] = dw; k[3] = dh;
        cl->SetPipelineState(g_s.pso_bil);
        cl->SetComputeRootDescriptorTable(0, table(dev, set, src, nullptr, nullptr, g_s.out));
        cl->SetComputeRoot32BitConstants(1, kRootDwords, k, 0);
        cl->Dispatch((dw + 7u) / 8u, (dh + 7u) / 8u, 1u);
    }

    barrier(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, src_state);
    barrier(cl, g_s.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(bb, g_s.out);
    barrier(cl, g_s.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!g_s.said)
    {
        g_s.said = true;
        static const char *const names[4] = { "off", "bilinear", "FSR 1", "NIS" };
        char l[200];
        snprintf(l, sizeof l, "[MGPU][SC1] scaling %ux%u -> %ux%u with %s (sharpness %d%%).",
                 sw, sh, dw, dh, names[m], g_sharp.load());
        mgpu::diag::info(l);
    }
    return true;
}

namespace
{
bool build_cursor(ID3D12Device *dev, DXGI_FORMAT fmt)
{
    if (g_s.cur_pso != nullptr && g_s.cur_fmt == fmt) return true;
    if (g_s.cur_failed) return false;
    relt(g_s.cur_pso);
    pfn_compile cmp = (pfn_compile)mgpu::d3dcompile();   // RV1
    if (cmp == nullptr) { g_s.cur_failed = true; return false; }
    if (g_s.cur_rs == nullptr)
    {
        D3D12_DESCRIPTOR_RANGE rng{};
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &rng;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.Num32BitValues = 8;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2; rsd.pParameters = rp; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
        ID3DBlob *sig = nullptr, *err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err) err->Release();
        if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                         IID_PPV_ARGS(&g_s.cur_rs));
        if (sig) sig->Release();
        if (FAILED(hr))
        {
            char l[160]; snprintf(l, sizeof l, "[MGPU][MC1] pointer root signature failed: 0x%08X", (unsigned)hr);
            mgpu::diag::error(l); g_s.cur_failed = true; return false;
        }
    }
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    HRESULT hr = cmp(kCursorHLSL, std::strlen(kCursorHLSL), "pointer", nullptr, nullptr, "vs", "vs_5_1",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err);
    if (err) { err->Release(); err = nullptr; }
    if (SUCCEEDED(hr)) hr = cmp(kCursorHLSL, std::strlen(kCursorHLSL), "pointer", nullptr, nullptr, "ps", "ps_5_1",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err);
    if (err) err->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_s.cur_rs;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_s.cur_pso));
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (FAILED(hr) || g_s.cur_pso == nullptr)
    {
        char l[160]; snprintf(l, sizeof l, "[MGPU][MC1] pointer pipeline failed: 0x%08X - pointer not drawn.", (unsigned)hr);
        mgpu::diag::error(l); g_s.cur_failed = true; return false;
    }
    if (g_s.rtv == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_s.rtv)))) { g_s.cur_failed = true; return false; }
    }
    g_s.cur_fmt = fmt;
    return true;
}
} // namespace

void draw_cursor(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                 unsigned dw, unsigned dh, unsigned sw, unsigned sh, DXGI_FORMAT fmt)
{
    if (dev == nullptr || cl == nullptr || bb == nullptr || sw == 0 || sh == 0) return;
    // MC2: under the scaler's lock - real and generated frames present from
    // different threads, and the snapshot converts the pointer image in place.
    std::lock_guard<std::mutex> lk(g_s.mx);
    mgpu::cursormap::sprite sp;
    if (!mgpu::cursormap::snapshot(sp) || sp.pixels == nullptr || sp.w == 0 || sp.h == 0) return;
    if (!g_s.built || g_s.heap == nullptr || dev != g_s.dev) return;
    if (!build_cursor(dev, fmt)) return;

    // The sprite texture, (re)made when the pointer's size changes.
    const UINT pitch = (sp.w * 4u + 255u) & ~255u;
    // MC2: three upload regions in turn, so a pointer change (arrow -> hand)
    // never overwrites bytes a frame still in flight is copying from.
    const UINT64 region = ((UINT64)pitch * sp.h + 511u) & ~(UINT64)511u;
    if (g_s.cur_tex == nullptr || g_s.cur_w != sp.w || g_s.cur_h != sp.h)
    {
        relt(g_s.cur_tex); relt(g_s.cur_up);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = sp.w; td.Height = sp.h;
        td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = region * 3u; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&g_s.cur_tex))) ||
            FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&g_s.cur_up))))
        { relt(g_s.cur_tex); relt(g_s.cur_up); return; }
        g_s.cur_w = sp.w; g_s.cur_h = sp.h; g_s.cur_serial = 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D12_CPU_DESCRIPTOR_HANDLE c = g_s.heap->GetCPUDescriptorHandleForHeapStart();
        c.ptr += (SIZE_T)(kMaxBackBuffers * 2u * 5u) * g_s.inc;   // MAS1, BB1: after every table of 5
        dev->CreateShaderResourceView(g_s.cur_tex, &sv, c);
    }
    if (g_s.cur_serial != sp.serial)
    {
        unsigned char *p = nullptr;
        if (FAILED(g_s.cur_up->Map(0, nullptr, (void **)&p)) || p == nullptr) return;
        const UINT64 off = region * (sp.serial % 3u);
        for (unsigned y = 0; y < sp.h; ++y)
            std::memcpy(p + off + (size_t)y * pitch, sp.pixels + (size_t)y * sp.w, sp.w * 4u);
        g_s.cur_up->Unmap(0, nullptr);
        barrier(cl, g_s.cur_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = g_s.cur_tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = g_s.cur_up; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = off;
        s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        s.PlacedFootprint.Footprint.Width = sp.w; s.PlacedFootprint.Footprint.Height = sp.h;
        s.PlacedFootprint.Footprint.Depth = 1; s.PlacedFootprint.Footprint.RowPitch = pitch;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        barrier(cl, g_s.cur_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_s.cur_serial = sp.serial;
    }

    // Where the game's pointer is, on the enlarged picture.
    const float px = (float)sp.x * (float)dw / (float)sw - (float)sp.hot_x;
    const float py = (float)sp.y * (float)dh / (float)sh - (float)sp.hot_y;

    barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rt = g_s.rtv->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(bb, nullptr, rt);
    cl->OMSetRenderTargets(1, &rt, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, (LONG)dw, (LONG)dh };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap *heaps[] = { g_s.heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_s.cur_rs);
    cl->SetPipelineState(g_s.cur_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_s.heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)(kMaxBackBuffers * 2u * 5u) * g_s.inc;
    cl->SetGraphicsRootDescriptorTable(0, g);
    const float k[8] = { px, py, (float)sp.w, (float)sp.h, (float)dw, (float)dh, 0.0f, 0.0f };
    cl->SetGraphicsRoot32BitConstants(1, 8, k, 0);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, 1, 0, 0);
    barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
}

void release()
{
    std::lock_guard<std::mutex> lk(g_s.mx);
    relt(g_s.tmp); relt(g_s.out); relt(g_s.coef_s); relt(g_s.coef_u); relt(g_s.coef_up);
    relt(g_s.pso_easu); relt(g_s.pso_rcas); relt(g_s.pso_nis); relt(g_s.pso_bil);
    {   // MAS1
        std::lock_guard<std::mutex> ml(g_m.mx);
        relt(g_m.buf); relt(g_m.pso); relt(g_m.rs); relt(g_m.heap);
        g_m.dev = nullptr; g_m.failed = false; g_m.cw = g_m.ch = 0;
    }
    relt(g_s.heap); relt(g_s.rs);
    relt(g_s.cur_pso); relt(g_s.cur_rs); relt(g_s.cur_tex); relt(g_s.cur_up); relt(g_s.rtv);
    g_s.cur_fmt = DXGI_FORMAT_UNKNOWN; g_s.cur_failed = false; g_s.cur_w = g_s.cur_h = g_s.cur_serial = 0;
    g_s.built = false; g_s.coef_uploaded = false; g_s.tw = g_s.th = 0; g_s.dev = nullptr; g_s.said = false;
    g_failed.store(false);
}

} // namespace scaler
} // namespace mgpu
