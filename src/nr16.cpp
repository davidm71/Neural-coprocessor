// MGPU Bridge - R208 FP16 input path (experimental, off by default). See
// nr16.hpp for what it is for and the contract it keeps. Everything here runs
// on the bridge thread.

#include "nr16.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::nr16
{
namespace
{
std::atomic<bool> g_active{false};
std::atomic<bool> g_reset{false};

float    g_power = 2.2f;
bool     g_ini_on = false;   // R219-2: NRInput16 was 4 at the last arm
unsigned g_w = 0, g_h = 0;
unsigned g_fails = 0;
bool     g_said_first = false;

ID3D12Device         *g_dev  = nullptr;   // borrowed, not AddRef'd
ID3D12RootSignature  *g_rs   = nullptr;
ID3D12PipelineState  *g_enc  = nullptr;
ID3D12PipelineState  *g_dec  = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
UINT                  g_inc  = 0;
ID3D12Resource       *g_in16  = nullptr;
ID3D12Resource       *g_out16 = nullptr;

const unsigned FAILS_MAX = 3;
const UINT     SLOTS_PER_PARITY = 4;   // SRV src, UAV in16 | SRV out16, UAV dst

// Code values in, code values out. The encode raises them to the power; the
// decode is its exact inverse, clamped to 0..1 because the texture it writes
// holds no more.
const char kHLSL[] =
    "Texture2D<float4>   gIn  : register(t0);\n"
    "RWTexture2D<float4> gOut : register(u0);\n"
    "cbuffer C : register(b0) { uint gW; uint gH; float gP; };\n"
    "[numthreads(8,8,1)]\n"
    "void enc(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gW || id.y >= gH) return;\n"
    "  gOut[id.xy] = float4(pow(saturate(gIn.Load(int3(id.xy, 0)).rgb), gP), 1.0);\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void dec(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "  if (id.x >= gW || id.y >= gH) return;\n"
    "  gOut[id.xy] = float4(pow(saturate(gIn.Load(int3(id.xy, 0)).rgb), 1.0 / gP), 1.0);\n"
    "}\n";

typedef HRESULT (WINAPI *pfn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

void rel(IUnknown *&p)
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

void release_all()
{
    g_active.store(false, std::memory_order_relaxed);
    IUnknown *u;
    u = g_enc;   rel(u); g_enc   = nullptr;
    u = g_dec;   rel(u); g_dec   = nullptr;
    u = g_rs;    rel(u); g_rs    = nullptr;
    u = g_heap;  rel(u); g_heap  = nullptr;
    u = g_in16;  rel(u); g_in16  = nullptr;
    u = g_out16; rel(u); g_out16 = nullptr;
    g_dev = nullptr;
}

// ---- keys, from mgpu.ini beside the module ----
bool ini_path(wchar_t *path, size_t n)
{
    static const int anchor = 0;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
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

// raw_mode: -1 key absent, else its value. power_state: -1 default, 0 out of
// range (default used), 1 read.
void read_keys(bool &on, float &power, int &raw_mode, int &power_state)
{
    on = false; power = 2.2f; raw_mode = -1; power_state = -1;
    wchar_t path[MAX_PATH];
    if (!ini_path(path, MAX_PATH)) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return;
    std::vector<char> buf(mgpu::config::MAX_BYTES + 2u, '\0');
    const size_t got = fread(buf.data(), 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return;   // whole-document rule
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf.data(), got, "NRInput16");
    if (v == nullptr) return;
    raw_mode = atoi(v);
    if (raw_mode != 4) return;
    on = true;
    const char *pw = mgpu::config::find(buf.data(), got, "NRInput16Power");
    if (pw != nullptr)
    {
        char *end = nullptr;
        const double d = strtod(pw, &end);
        if (end != pw && d >= 1.0 && d <= 3.0) { power = (float)d; power_state = 1; }
        else power_state = 0;
    }
}

// ---- GPU objects ----
HRESULT make_tex16(ID3D12Device *dev, unsigned w, unsigned h, ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                        IID_PPV_ARGS(out));
}

bool build_kernels(ID3D12Device *dev)
{
    char line[900];
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_d3dcompile compile = (dc != nullptr)
                               ? (pfn_d3dcompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (compile == nullptr)
    {
        mgpu::diag::error("[MGPU][R208] d3dcompiler_47.dll / D3DCompile not available - the "
                          "FP16 input path stays off for this arm.");
        return false;
    }
    ID3DBlob *cs[2] = { nullptr, nullptr };
    const char *entry[2] = { "enc", "dec" };
    for (int i = 0; i < 2; ++i)
    {
        ID3DBlob *err = nullptr;
        const HRESULT hr = compile(kHLSL, strlen(kHLSL), "nr16", nullptr, nullptr, entry[i],
                                   "cs_5_1", 0, 0, &cs[i], &err);
        if (FAILED(hr) || cs[i] == nullptr)
        {
            snprintf(line, sizeof line, "[MGPU][R208] kernel '%s' failed to compile: 0x%08X %s",
                     entry[i], (unsigned)hr,
                     (err != nullptr) ? (const char *)err->GetBufferPointer() : "");
            mgpu::diag::error(line);
            if (err != nullptr) err->Release();
            if (cs[0] != nullptr) cs[0]->Release();
            if (cs[1] != nullptr) cs[1]->Release();
            return false;
        }
        if (err != nullptr) err->Release();
    }

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rng[0].NumDescriptors = 1;
    rng[0].BaseShaderRegister = 0;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rng[1].NumDescriptors = 1;
    rng[1].BaseShaderRegister = 0;
    rng[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2;
    rp[0].DescriptorTable.pDescriptorRanges = rng;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0;
    rp[1].Constants.Num32BitValues = 3;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err != nullptr) err->Release();
    if (SUCCEEDED(hr))
    {
        hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                      IID_PPV_ARGS(&g_rs));
        sig->Release();
    }
    ID3D12PipelineState **pso[2] = { &g_enc, &g_dec };
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs[i]->GetBufferPointer();
        pd.CS.BytecodeLength = cs[i]->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso[i]));
    }
    cs[0]->Release();
    cs[1]->Release();
    if (FAILED(hr))
    {
        snprintf(line, sizeof line, "[MGPU][R208] root signature / PSO failed: 0x%08X",
                 (unsigned)hr);
        mgpu::diag::error(line);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2u * SLOTS_PER_PARITY;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
    if (FAILED(hr)) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// An _SRGB view would linearise on read; the kernels work on code values.
DXGI_FORMAT code_value_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:                              return f;
    }
}

// Writes an SRV of `srv_res` and a UAV of `uav_res` into two adjacent slots.
D3D12_GPU_DESCRIPTOR_HANDLE write_pair(unsigned parity, unsigned first,
                                       ID3D12Resource *srv_res, ID3D12Resource *uav_res)
{
    const SIZE_T slot = (SIZE_T)(parity & 1u) * SLOTS_PER_PARITY + first;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += slot * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    sv.Format = code_value_format(srv_res->GetDesc().Format);
    g_dev->CreateShaderResourceView(srv_res, &sv, h);
    h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = code_value_format(uav_res->GetDesc().Format);
    g_dev->CreateUnorderedAccessView(uav_res, nullptr, &uv, h);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)slot * g_inc;
    return g;
}

void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

void uav_barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

void dispatch(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso,
              D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    ID3D12DescriptorHeap *heaps[1] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(pso);
    cl->SetComputeRootDescriptorTable(0, table);
    UINT c[3] = { g_w, g_h, 0u };
    std::memcpy(&c[2], &g_power, sizeof(float));
    cl->SetComputeRoot32BitConstants(1, 3, c, 0);
    cl->Dispatch((g_w + 7u) / 8u, (g_h + 7u) / 8u, 1u);
}
} // namespace

bool arm(ID3D12Device *dev, unsigned width, unsigned height)
{
    release_all();
    g_fails = 0;
    g_said_first = false;
    bool on = false;
    int raw_mode = -1, power_state = -1;
    read_keys(on, g_power, raw_mode, power_state);
    g_ini_on = on;   // R219-2
    if (!on)
    {
        if (raw_mode != -1)
        {
            char l[200];
            snprintf(l, sizeof l, "[MGPU][R208] FP16 input path OFF (NRInput16=%d; 4 turns it "
                     "on). The stream runs its normal path.", raw_mode);
            mgpu::diag::info(l);
        }
        return false;
    }
    if (dev == nullptr || width == 0 || height == 0) return false;
    g_dev = dev;
    g_w = width;
    g_h = height;
    HRESULT h = make_tex16(dev, width, height, &g_in16);
    if (SUCCEEDED(h)) h = make_tex16(dev, width, height, &g_out16);
    if (FAILED(h) || !build_kernels(dev))
    {
        char l[240];
        snprintf(l, sizeof l, "[MGPU][R208] FP16 input path could not be built (0x%08X) - "
                 "OFF for this arm; the stream runs its normal path.", (unsigned)h);
        mgpu::diag::error(l);
        release_all();
        return false;
    }
    char l[500];
    snprintf(l, sizeof l,
             "[MGPU][R208] FP16 INPUT PATH ON (experimental): NRInput16=4, power %.2f%s, %ux%u. "
             "NR is fed and writes R16G16B16A16_FLOAT; the output is decoded back into the "
             "stream's texture. Used at Passes=1 on the full frame, or on the whole reduced "
             "frame when SRUpscale is on (the textures follow the SR size). The first "
             "evaluate's result follows; three failures in a row turn it off.",
             g_power,
             (power_state == 0) ? " (NRInput16Power out of range 1.0-3.0, default used)"
                                : ((power_state == -1) ? " (default)" : ""),
             width, height);
    mgpu::diag::info(l);
    g_reset.store(true, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
    return true;
}

bool resize(unsigned width, unsigned height)
{
    // Fork: the SR path evaluates NR at the reduced size. Only the two FP16
    // textures depend on the size; the kernels and the heap stay. The caller
    // guarantees the GPU no longer uses the old pair (SR create at arm, or the
    // C2-SR rebuild after its drain).
    if (g_dev == nullptr || g_enc == nullptr || g_dec == nullptr) return false;
    if (width == 0 || height == 0) return false;
    if (width == g_w && height == g_h) return true;
    const bool was_active = g_active.load(std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    IUnknown *u;
    u = g_in16;  rel(u); g_in16  = nullptr;
    u = g_out16; rel(u); g_out16 = nullptr;
    HRESULT h = make_tex16(g_dev, width, height, &g_in16);
    if (SUCCEEDED(h)) h = make_tex16(g_dev, width, height, &g_out16);
    char l[300];
    if (FAILED(h))
    {
        u = g_in16;  rel(u); g_in16  = nullptr;
        u = g_out16; rel(u); g_out16 = nullptr;
        g_w = g_h = 0;
        snprintf(l, sizeof l, "[MGPU][R208] FP16 input path: textures for %ux%u could not be made (0x%08X) - "
                 "OFF for this arm; the stream runs its normal path.", width, height, (unsigned)h);
        mgpu::diag::error(l);
        return false;
    }
    snprintf(l, sizeof l, "[MGPU][R208] FP16 input path resized %ux%u -> %ux%u (%s).",
             g_w, g_h, width, height, was_active ? "the next evaluate resets NR history" : "path stays off");
    mgpu::diag::info(l);
    g_w = width; g_h = height;
    g_said_first = false;
    if (was_active)
    {
        g_reset.store(true, std::memory_order_relaxed);
        g_active.store(true, std::memory_order_release);
    }
    return was_active;
}

bool fits(unsigned width, unsigned height)
{
    return width == g_w && height == g_h;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void encode(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *src,
            ID3D12Resource **nr_color, ID3D12Resource **nr_output)
{
    if (!active() || cl == nullptr || src == nullptr) return;
    const D3D12_GPU_DESCRIPTOR_HANDLE t = write_pair(parity, 0u, src, g_in16);
    dispatch(cl, g_enc, t);
    // in16 as NR's input, in the state this project always hands NGX an input.
    transition(cl, g_in16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (nr_color != nullptr) *nr_color = g_in16;
    if (nr_output != nullptr) *nr_output = g_out16;
}

void decode(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *dst,
            bool evaluate_ok)
{
    if (g_in16 == nullptr || cl == nullptr) return;
    transition(cl, g_in16, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!g_said_first)
    {
        g_said_first = true;
        mgpu::diag::info(evaluate_ok
            ? "[MGPU][R208] first FP16 evaluate: SUCCESS - NGX accepted R16G16B16A16_FLOAT "
              "for Color and Output."
            : "[MGPU][R208] first FP16 evaluate: FAILED - see the P4.1 EvaluateFeature line "
              "for the result code.");
    }
    if (!evaluate_ok)
    {
        if (++g_fails >= FAILS_MAX)
        {
            g_active.store(false, std::memory_order_relaxed);
            g_reset.store(true, std::memory_order_relaxed);
            mgpu::diag::error("[MGPU][R208] FP16 input path: three failed evaluates in a row - "
                              "OFF for the rest of this arm, the stream is back on its normal "
                              "8-bit path with a Reset.");
        }
        return;
    }
    g_fails = 0;
    if (dst == nullptr) return;
    uav_barrier(cl, g_out16);   // NR's writes done before the decode reads
    transition(cl, g_out16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const D3D12_GPU_DESCRIPTOR_HANDLE t = write_pair(parity, 2u, g_out16, dst);
    dispatch(cl, g_dec, t);
    transition(cl, g_out16, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(cl, dst);       // the decode's writes done before anything reads dst
}

bool take_reset()
{
    // Off: one relaxed load, no read-modify-write on the frame path.
    if (!g_reset.load(std::memory_order_relaxed)) return false;
    return g_reset.exchange(false, std::memory_order_relaxed);
}

void release()
{
    release_all();
}

void ini_values(bool &on, float &power)
{
    on = g_ini_on;
    power = g_power;
}

void ini_file_values(bool &on, float &power)
{
    int raw_mode = -1, power_state = -1;
    read_keys(on, power, raw_mode, power_state);
}
}
