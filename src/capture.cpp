// ---------------------------------------------------------------------------
// capture.cpp - SS1 / VR1 / BM1. See capture.hpp.
//
// Threads:
//   present path  present_frame, serialised by its present lock. Records GPU
//                 copies into the bridge's own command list and nothing else -
//                 never waits, never touches a file.
//   worker        one thread, started on first use. Waits for the present
//                 fence, writes PNGs, drives the Media Foundation sink writer,
//                 writes benchmark files.
//   audio         WASAPI loopback capture while a recording runs.
// ---------------------------------------------------------------------------
#include "capture.hpp"
#include "framewarp.hpp"

#include <windows.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wincodec.h>
#include <propidl.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "diag.hpp"
#include "gpu1_context.hpp"
#include "replay.hpp"

#if defined(_MSC_VER)
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

// Windows 10 2004+ process loopback. Declared here when the SDK lacks the header.
#if defined(__has_include)
#  if __has_include(<audioclientactivationparams.h>)
#    include <audioclientactivationparams.h>
#    define MGPU_HAVE_ACAP 1
#  endif
#endif
#ifndef MGPU_HAVE_ACAP
typedef enum { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 } AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum { PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
               PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1 } PROCESS_LOOPBACK_MODE;
typedef struct { DWORD TargetProcessId; PROCESS_LOOPBACK_MODE ProcessLoopbackMode; } AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct { AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
                 union { AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; }; } AUDIOCLIENT_ACTIVATION_PARAMS;
#endif
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace mgpu
{
namespace capture
{
namespace
{

// ===========================================================================
// common
// ===========================================================================
std::mutex g_mx;   // queues, messages, benchmark job
// The present-path entry points. present_frame is already serialised, but the
// bridge's shutdown can run while the frame generation present thread is still
// presenting; this lock and g_dead keep shutdown from freeing what a present
// is using.
std::mutex g_pmx;
bool g_dead = false;

LONGLONG qpc_now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG qpc_freq()
{
    static LONGLONG f = 0;
    if (f == 0) { LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); f = q.QuadPart ? q.QuadPart : 1; }
    return f;
}
LONGLONG to_100ns(LONGLONG ticks) { return (LONGLONG)((double)ticks * 1.0e7 / (double)qpc_freq()); }

char      g_msg[400] = {};       // panel
char      g_hudmsg[64] = {};     // HUD, shown until g_hudmsg_until
ULONGLONG g_hudmsg_until = 0;

void set_msg(const char *panel, const char *hud, unsigned ms)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (panel != nullptr) snprintf(g_msg, sizeof g_msg, "%s", panel);
    if (hud != nullptr) { snprintf(g_hudmsg, sizeof g_hudmsg, "%s", hud); g_hudmsg_until = GetTickCount64() + ms; }
}

// ---- settings ----
struct setting { const char *key; int dflt, lo, hi; std::atomic<int> v; };
setting S_REC_SEC   { "RecordSeconds",  60, 0, 3600, {-1} };
setting S_REC_FPS   { "RecordFps",       0, 0, 240,  {-1} };
setting S_REC_AUDIO { "RecordAudio",     1, 0, 1,    {-1} };
setting S_BENCH_SEC { "BenchSeconds",   30, 5, 600,  {-1} };
setting S_BENCH_DLY { "BenchDelay",      3, 0, 30,   {-1} };
setting S_BENCH_RPL { "BenchReplay",     0, 0, 1,    {-1} };
setting S_SHOT_HUD  { "ScreenshotHud",   1, 0, 1,    {-1} };
setting S_REC_HUD   { "RecordHud",       0, 0, 1,    {-1} };

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

// ---- folders and file names ----
bool module_dir(wchar_t *out, unsigned n)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_dir), &m)) return false;
    const DWORD len = GetModuleFileNameW(m, out, n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

bool make_file(wchar_t *out, unsigned n, const wchar_t *sub, const wchar_t *stem, const wchar_t *ext)
{
    if (!base_folder(out, n)) return false;
    wcscat_s(out, n, sub);
    (void)CreateDirectoryW(out, nullptr);
    wcscat_s(out, n, L"\\");
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, 96, L"%ls_%04u%02u%02u_%02u%02u%02u_%03u%ls", stem, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, ext);
    wcscat_s(out, n, name);
    return true;
}

void narrow(const wchar_t *w, char *out, unsigned n)
{
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, nullptr, nullptr) <= 0 && n) out[0] = '\0';
}

const wchar_t *file_part(const wchar_t *p) { const wchar_t *s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }

// ---- pixel conversion (screenshots) ----
float half_to_float(unsigned short h)
{
    const unsigned s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = 65504.0f;
    else v = std::ldexp((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

unsigned char to8_lin(float v)
{
    if (!(v > 0.0f)) return 0;
    v = std::pow(v, 1.0f / 2.2f);
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

// One row of the bridge's format to 24-bit BGR. False if the format is unknown.
bool row_to_bgr(DXGI_FORMAT f, const unsigned char *src, unsigned char *dst, unsigned w)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        for (unsigned x = 0; x < w; ++x)
        {
            unsigned v; memcpy(&v, src + x * 4u, 4);
            dst[x * 3u + 2u] = (unsigned char)((v & 1023u) >> 2);
            dst[x * 3u + 1u] = (unsigned char)(((v >> 10) & 1023u) >> 2);
            dst[x * 3u + 0u] = (unsigned char)(((v >> 20) & 1023u) >> 2);
        }
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        for (unsigned x = 0; x < w; ++x)
        { dst[x*3u+2u] = src[x*4u]; dst[x*3u+1u] = src[x*4u+1u]; dst[x*3u] = src[x*4u+2u]; }
        return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        for (unsigned x = 0; x < w; ++x)
        { dst[x*3u] = src[x*4u]; dst[x*3u+1u] = src[x*4u+1u]; dst[x*3u+2u] = src[x*4u+2u]; }
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (unsigned x = 0; x < w; ++x)
        {
            unsigned short c[4]; memcpy(c, src + x * 8u, 8);
            dst[x*3u+2u] = to8_lin(half_to_float(c[0]));
            dst[x*3u+1u] = to8_lin(half_to_float(c[1]));
            dst[x*3u+0u] = to8_lin(half_to_float(c[2]));
        }
        return true;
    default:
        return false;
    }
}

bool write_png(const wchar_t *path, const unsigned char *bgr, unsigned w, unsigned h)
{
    IWICImagingFactory *f = nullptr;
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr) && pf != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = fr->WritePixels(h, w * 3u, w * 3u * h, const_cast<BYTE *>(bgr));
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    ok = SUCCEEDED(hr);
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

bool fence_done(ID3D12Fence *f, unsigned long long v)
{
    if (f == nullptr) return true;
    const UINT64 c = f->GetCompletedValue();
    return c != UINT64_MAX && c >= v;
}

// ===========================================================================
// SS1 screenshots
// ===========================================================================
struct shot
{
    ID3D12Resource *rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    unsigned w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12Fence *fence = nullptr;
    unsigned long long value = 0;
};
std::atomic<bool> g_ss_want{false};
std::vector<shot> g_ss_frame;   // present path: recorded, not yet submitted
std::vector<shot> g_ss_queue;   // worker (g_mx)

void shot_free(shot &s)
{
    if (s.rb) s.rb->Release();
    if (s.fence) s.fence->Release();
    s.rb = nullptr; s.fence = nullptr;
}

void shot_record(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb, unsigned w, unsigned h,
                 DXGI_FORMAT fmt)
{
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    shot s;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&bd, 0, 1, 0, &s.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&s.rb))))
    {
        set_msg("Screenshot failed: could not allocate the readback buffer.", "SCREENSHOT FAILED", 3000);
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = s.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = s.fp;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    s.w = (w < (unsigned)bd.Width) ? w : (unsigned)bd.Width;
    s.h = (h < bd.Height) ? h : bd.Height;
    s.fmt = fmt;
    g_ss_frame.push_back(s);
}

void shots_tick()
{
    std::vector<shot> ready;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (size_t i = 0; i < g_ss_queue.size();)
        {
            if (fence_done(g_ss_queue[i].fence, g_ss_queue[i].value))
            { ready.push_back(g_ss_queue[i]); g_ss_queue.erase(g_ss_queue.begin() + (long)i); }
            else ++i;
        }
    }
    for (shot &s : ready)
    {
        bool ok = false;
        wchar_t path[MAX_PATH * 2] = {};
        void *p = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)(s.fp.Footprint.RowPitch * s.h) };
        if (make_file(path, MAX_PATH * 2, L"captures", L"MGPU", L".png") && SUCCEEDED(s.rb->Map(0, &rr, &p)) && p)
        {
            std::vector<unsigned char> bgr((size_t)s.w * 3u * s.h);
            ok = true;
            for (unsigned y = 0; y < s.h && ok; ++y)
                ok = row_to_bgr(s.fmt, (const unsigned char *)p + s.fp.Offset + (size_t)y * s.fp.Footprint.RowPitch,
                                bgr.data() + (size_t)y * s.w * 3u, s.w);
            D3D12_RANGE wr{ 0, 0 };
            s.rb->Unmap(0, &wr);
            if (ok) ok = write_png(path, bgr.data(), s.w, s.h);
        }
        char l[600], fn[300];
        narrow(path, fn, sizeof fn);
        if (ok)
        {
            snprintf(l, sizeof l, "Screenshot saved: %s", fn);
            set_msg(l, "SCREENSHOT SAVED", 3000);
            char d[700]; snprintf(d, sizeof d, "[MGPU][SS1] %s (%ux%u)", l, s.w, s.h);
            mgpu::diag::info(d);
        }
        else
        {
            snprintf(l, sizeof l, "Screenshot failed (format %d, %ux%u).", (int)s.fmt, s.w, s.h);
            set_msg(l, "SCREENSHOT FAILED", 3000);
            char d[700]; snprintf(d, sizeof d, "[MGPU][SS1] %s Path: %s", l, fn);
            mgpu::diag::warn(d);
        }
        shot_free(s);
    }
}

// ===========================================================================
// VR1 recording - the D3D12 side (present path)
// ===========================================================================
const unsigned RING = 8;
std::atomic<int> g_rec{0};          // 0 idle, 1 start wanted, 2 encoder opening, 3 recording, 4 stopping
std::atomic<int> g_slot_busy[RING];
ID3D12Resource *g_slot_tex[RING] = {};
HANDLE          g_slot_h[RING] = {};
ID3D12Resource *g_csrc = nullptr;   // a copy of the back buffer the shader can read
ID3D12Resource *g_scratch = nullptr;   // the shader's output; copied into a shared slot
unsigned g_last_w = 0, g_last_h = 0;   // the bridge's shown size (benchmark settings line)
ID3D12RootSignature *g_rs = nullptr;
ID3D12PipelineState *g_pso = nullptr;
ID3D12DescriptorHeap *g_heap = nullptr;
UINT g_inc = 0;
unsigned g_bw = 0, g_bh = 0, g_rw = 0, g_rh = 0;
DXGI_FORMAT g_srcfmt = DXGI_FORMAT_UNKNOWN, g_texfmt = DXGI_FORMAT_UNKNOWN;
LUID g_luid{};
unsigned g_ring_next = 0;
std::atomic<LONGLONG> g_rec_t0{0};
LONGLONG g_rec_last = 0;
unsigned g_nominal_fps = 60;
std::atomic<unsigned long long> g_rec_frames{0}, g_rec_dropped{0};
double g_present_rate = 0.0;        // present path
LONGLONG g_pr_t = 0;
unsigned g_pr_n = 0;
ID3D12Fence *g_last_fence = nullptr;   // AddRef'd: the newest value our copies are covered by
unsigned long long g_last_value = 0;

struct vframe { unsigned slot; LONGLONG qpc; ID3D12Fence *fence; unsigned long long value; };
std::vector<vframe> g_vf_frame;     // present path
std::deque<vframe>  g_vf_queue;     // worker (g_mx)

typedef HRESULT (WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

const char *kRecHLSL =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<unorm float4> dst : register(u0);\n"
    "cbuffer C : register(b0) { uint w; uint h; uint lin; uint pad; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float4 c = src[id.xy];\n"
    "    c.rgb = lin ? pow(saturate(c.rgb), 1.0 / 2.2) : saturate(c.rgb);\n"
    "    c.a = 1.0;\n"
    "    dst[id.xy] = c;\n"
    "}\n";

// REV7: buffers the GPU may still be using when the capture subsystem is torn
// down (a bridge rearm, or exit) and the last copy has not finished. They are
// parked here with the fence that covers them and released once that fence
// completes (or its device is lost), instead of being freed under the GPU or
// left referenced by the subsystem's globals - which the next arm would then
// reuse against a NEW device. Drained on every start, stop and worker tick.
struct grave { ID3D12Fence *fence; unsigned long long value; std::vector<IUnknown *> objs; };
std::vector<grave> g_grave;   // under g_pmx
unsigned long long g_grave_freed = 0;

void grave_drain(bool log)   // under g_pmx
{
    for (size_t i = 0; i < g_grave.size();)
    {
        grave &g = g_grave[i];
        const UINT64 c = g.fence->GetCompletedValue();
        if (c == UINT64_MAX || c >= g.value)
        {
            for (IUnknown *o : g.objs) o->Release();
            g.fence->Release();
            g_grave_freed += 1;
            g_grave.erase(g_grave.begin() + (long)i);
            if (log) mgpu::diag::info("[MGPU][REV7] parked capture buffers released (their GPU work finished).");
        }
        else ++i;
    }
}

void rec_release_d3d12()
{
    for (unsigned i = 0; i < RING; ++i)
    {
        if (g_slot_h[i]) { CloseHandle(g_slot_h[i]); g_slot_h[i] = nullptr; }
        if (g_slot_tex[i]) { g_slot_tex[i]->Release(); g_slot_tex[i] = nullptr; }
        g_slot_busy[i].store(0);
    }
    if (g_csrc) { g_csrc->Release(); g_csrc = nullptr; }
    if (g_scratch) { g_scratch->Release(); g_scratch = nullptr; }
    if (g_pso) { g_pso->Release(); g_pso = nullptr; }
    if (g_rs) { g_rs->Release(); g_rs = nullptr; }
    if (g_heap) { g_heap->Release(); g_heap = nullptr; }
    g_bw = g_bh = g_rw = g_rh = 0;
}

bool rec_build(ID3D12Device *dev, unsigned w, unsigned h, DXGI_FORMAT fmt, char *why, unsigned whyn)
{
    if (g_csrc != nullptr && g_bw == w && g_bh == h && g_srcfmt == fmt) return true;
    rec_release_d3d12();
    if (w < 64 || h < 64) { snprintf(why, whyn, "frame too small (%ux%u)", w, h); return false; }
    g_luid = dev->GetAdapterLuid();
    g_srcfmt = fmt;

    // BGRA8 is what the encoder takes directly; RGBA8 if the card cannot
    // store BGRA8 from a shader (then the encoder is told ABGR32).
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    const bool bgra = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
                      (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    g_texfmt = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_compile cmp = dc ? (pfn_compile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (cmp == nullptr) { snprintf(why, whyn, "d3dcompiler_47.dll not available"); return false; }
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = cmp(kRecHLSL, strlen(kRecHLSL), "rec", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
    if (err) { err->Release(); err = nullptr; }
    if (FAILED(hr) || cs == nullptr) { snprintf(why, whyn, "shader compile 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_RANGE rng[2] = {};
    rng[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rng[0].NumDescriptors = 1;
    rng[0].OffsetInDescriptorsFromTableStart = 0;
    rng[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rng[1].NumDescriptors = 1;
    rng[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = rng;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob *sig = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) err->Release();
    if (SUCCEEDED(hr)) { hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs)); sig->Release(); }
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso));
    }
    cs->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2u * RING;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
        g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (SUCCEEDED(hr))
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&g_csrc));
    }
    g_rw = w & ~1u; g_rh = h & ~1u;   // the encoder wants even sizes
    if (SUCCEEDED(hr))
    {
        // The shader writes here (UAV); each frame is then COPIED into a shared
        // slot. The shared slots carry no UAV flag: D3D11 refuses to open a
        // shared BGRA8 texture that has one (measured: E_INVALIDARG).
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = g_rw; d.Height = g_rh;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = g_texfmt; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                          IID_PPV_ARGS(&g_scratch));
    }
    for (unsigned i = 0; i < RING && SUCCEEDED(hr); ++i)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = g_rw; d.Height = g_rh;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = g_texfmt; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&g_slot_tex[i]));
        if (SUCCEEDED(hr)) hr = dev->CreateSharedHandle(g_slot_tex[i], nullptr, GENERIC_ALL, nullptr, &g_slot_h[i]);
        g_slot_busy[i].store(0);
    }
    if (FAILED(hr)) { snprintf(why, whyn, "GPU resources 0x%08X", (unsigned)hr); rec_release_d3d12(); return false; }
    g_bw = w; g_bh = h;
    return true;
}

void rec_record(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb, unsigned slot)
{
    // bb is in COPY_SOURCE here.
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g_csrc; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{ 0, 0, 0, g_bw, g_bh, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(cl, g_csrc, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource *t = g_slot_tex[slot];

    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * 2u * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Format = g_srcfmt;
    dev->CreateShaderResourceView(g_csrc, &sv, h); h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = g_texfmt;
    dev->CreateUnorderedAccessView(g_scratch, nullptr, &uv, h);

    ID3D12DescriptorHeap *hs[1] = { g_heap };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g = g_heap->GetGPUDescriptorHandleForHeapStart();
    g.ptr += (UINT64)slot * 2u * g_inc;
    cl->SetComputeRootDescriptorTable(0, g);
    const UINT lin = (g_srcfmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1u : 0u;
    const UINT c[4] = { g_rw, g_rh, lin, 0u };
    cl->SetComputeRoot32BitConstants(1, 4, c, 0);
    cl->Dispatch((g_rw + 7u) / 8u, (g_rh + 7u) / 8u, 1u);

    barrier(cl, g_scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(cl, t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(t, g_scratch);
    barrier(cl, t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    barrier(cl, g_scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, g_csrc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}

// ===========================================================================
// VR1 recording - audio (WASAPI loopback)
// ===========================================================================
typedef HRESULT (WINAPI *pfn_activate)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *,
                                       IActivateAudioInterfaceAsyncOperation **);

MIDL_INTERFACE("94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90") mgpu_IAgileObject : public IUnknown {};

struct act_handler final : public IActivateAudioInterfaceCompletionHandler, public mgpu_IAgileObject
{
    LONG ref = 1;
    HANDLE ev = nullptr;
    HRESULT hr = E_FAIL;
    IAudioClient *client = nullptr;
    STDMETHODIMP QueryInterface(REFIID riid, void **pv) override
    {
        if (pv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *pv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        else if (riid == __uuidof(mgpu_IAgileObject))
            *pv = static_cast<mgpu_IAgileObject *>(this);
        else { *pv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
    {
        HRESULT ar = E_FAIL;
        IUnknown *u = nullptr;
        if (op != nullptr && SUCCEEDED(op->GetActivateResult(&ar, &u)) && SUCCEEDED(ar) && u != nullptr)
            (void)u->QueryInterface(__uuidof(IAudioClient), (void **)&client);
        if (u) u->Release();
        hr = ar;
        if (ev) SetEvent(ev);
        return S_OK;
    }
};

struct achunk { std::vector<short> pcm; LONGLONG t100; };   // stereo, interleaved
std::deque<achunk> g_aq;              // g_mx
std::thread g_audio;
std::atomic<bool> g_audio_stop{false};
std::atomic<int>  g_audio_mode{0};    // 0 none, 1 the game only, 2 everything the PC plays
const unsigned ARATE = 48000;

void audio_main()
{
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient *ac = nullptr;
    int mode = 0;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = ARATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = ARATE * 4u;

    // 1. The game's own sound only (Windows 10 2004+).
    HMODULE mm = LoadLibraryW(L"mmdevapi.dll");
    pfn_activate act = mm ? (pfn_activate)(void *)GetProcAddress(mm, "ActivateAudioInterfaceAsync") : nullptr;
    if (act != nullptr)
    {
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = reinterpret_cast<BYTE *>(&ap);
        act_handler *hd = new act_handler();
        hd->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        IActivateAudioInterfaceAsyncOperation *op = nullptr;
        if (hd->ev && SUCCEEDED(act(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, hd, &op)) &&
            WaitForSingleObject(hd->ev, 3000) == WAIT_OBJECT_0 && hd->client != nullptr)
        {
            ac = hd->client; hd->client = nullptr;
            if (SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                         2000000, 0, &wf, nullptr)))
                mode = 1;
            else { ac->Release(); ac = nullptr; }
        }
        if (op) op->Release();
        if (hd->client) { hd->client->Release(); hd->client = nullptr; }
        if (hd->ev) { CloseHandle(hd->ev); hd->ev = nullptr; }
        hd->Release();
    }
    // 2. Everything the PC plays (the default output device).
    if (mode == 0)
    {
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dv = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void **)&en)) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dv)) &&
            SUCCEEDED(dv->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac)))
        {
            if (SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                         2000000, 0, &wf, nullptr)))
                mode = 2;
            else { ac->Release(); ac = nullptr; }
        }
        if (dv) dv->Release();
        if (en) en->Release();
    }
    g_audio_mode.store(mode);
    if (mode == 0) { CoUninitialize(); return; }

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioCaptureClient *cc = nullptr;
    if (ev) (void)ac->SetEventHandle(ev);
    if (SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void **)&cc)) && SUCCEEDED(ac->Start()))
    {
        while (!g_audio_stop.load())
        {
            if (ev) WaitForSingleObject(ev, 10); else Sleep(10);
            UINT32 n = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n > 0)
            {
                BYTE *data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 dpos = 0, qpos = 0;
                if (FAILED(cc->GetBuffer(&data, &frames, &flags, &dpos, &qpos))) break;
                achunk c;
                c.pcm.resize((size_t)frames * 2u);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr) std::fill(c.pcm.begin(), c.pcm.end(), (short)0);
                else memcpy(c.pcm.data(), data, (size_t)frames * 4u);
                c.t100 = (LONGLONG)qpos;   // 100 ns units, QPC time base
                (void)cc->ReleaseBuffer(frames);
                std::lock_guard<std::mutex> lk(g_mx);
                if (g_aq.size() < 4000) g_aq.push_back(std::move(c));
            }
        }
        (void)ac->Stop();
    }
    if (cc) cc->Release();
    ac->Release();
    if (ev) CloseHandle(ev);
    CoUninitialize();
}

// ===========================================================================
// VR1 recording - the encoder (worker thread)
// ===========================================================================
typedef HRESULT (WINAPI *pfn_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *pfn_MFShutdown)();
typedef HRESULT (WINAPI *pfn_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *pfn_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *pfn_MFCreateDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **);
typedef HRESULT (WINAPI *pfn_MFCreateDXGISurfaceBuffer)(REFIID, IUnknown *, UINT, BOOL, IMFMediaBuffer **);
// Declared here: older SDK headers lack it (Windows 8+ API, IID from mfidl.h).
MIDL_INTERFACE("245BF8E9-0755-40f7-88A5-AE0F18D55E17") mgpu_IMFTrackedSample : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetAllocator(IMFAsyncCallback *pSampleAllocator, IUnknown *pUnkState) = 0;
};
// MFVideoFormat_ABGR32 (D3DFMT_A8B8G8R8 = 32 = DXGI R8G8B8A8): the input type when
// the card cannot store BGRA8 from a shader.
const GUID MGPU_MFVideoFormat_ABGR32 = { 0x00000020, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
typedef HRESULT (WINAPI *pfn_MFCreateTrackedSample)(mgpu_IMFTrackedSample **);
typedef HRESULT (WINAPI *pfn_MFCreateSample)(IMFSample **);
typedef HRESULT (WINAPI *pfn_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *pfn_MFCreateSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);

struct mf_api
{
    bool tried = false, ok = false;
    pfn_MFStartup Startup = nullptr; pfn_MFShutdown Shutdown = nullptr;
    pfn_MFCreateAttributes CreateAttributes = nullptr; pfn_MFCreateMediaType CreateMediaType = nullptr;
    pfn_MFCreateDXGIDeviceManager CreateDXGIDeviceManager = nullptr;
    pfn_MFCreateDXGISurfaceBuffer CreateDXGISurfaceBuffer = nullptr;
    pfn_MFCreateTrackedSample CreateTrackedSample = nullptr; pfn_MFCreateSample CreateSample = nullptr;
    pfn_MFCreateMemoryBuffer CreateMemoryBuffer = nullptr;
    pfn_MFCreateSinkWriterFromURL CreateSinkWriterFromURL = nullptr;
} g_mf;

bool mf_load()
{
    if (g_mf.tried) return g_mf.ok;
    g_mf.tried = true;
    HMODULE p = LoadLibraryW(L"mfplat.dll"), r = LoadLibraryW(L"mfreadwrite.dll");
    if (p == nullptr || r == nullptr) return false;
#define MGPU_MF(n, m) g_mf.n = (decltype(g_mf.n))(void *)GetProcAddress(m, "MF" #n)
    MGPU_MF(Startup, p); MGPU_MF(Shutdown, p); MGPU_MF(CreateAttributes, p); MGPU_MF(CreateMediaType, p);
    MGPU_MF(CreateDXGIDeviceManager, p); MGPU_MF(CreateDXGISurfaceBuffer, p); MGPU_MF(CreateTrackedSample, p);
    MGPU_MF(CreateSample, p); MGPU_MF(CreateMemoryBuffer, p); MGPU_MF(CreateSinkWriterFromURL, r);
#undef MGPU_MF
    g_mf.ok = g_mf.Startup && g_mf.Shutdown && g_mf.CreateAttributes && g_mf.CreateMediaType &&
              g_mf.CreateDXGIDeviceManager && g_mf.CreateDXGISurfaceBuffer && g_mf.CreateTrackedSample &&
              g_mf.CreateSample && g_mf.CreateMemoryBuffer && g_mf.CreateSinkWriterFromURL;
    return g_mf.ok;
}

// The encoder's "done with this frame" callback frees its ring slot.
struct slot_cb final : public IMFAsyncCallback
{
    unsigned slot = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void **pv) override
    {
        if (pv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) { *pv = this; return S_OK; }
        *pv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static lifetime
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult *) override { g_slot_busy[slot].store(0); return S_OK; }
};
slot_cb g_cb[RING];

struct encoder
{
    bool open = false;
    ID3D11Device *d11 = nullptr;
    ID3D11Texture2D *t11[RING] = {};
    IMFDXGIDeviceManager *mgr = nullptr;
    UINT token = 0;
    IMFSinkWriter *sw = nullptr;
    DWORD vidx = 0, aidx = 0;
    bool audio = false;
    LONGLONG t0_100 = 0;       // QPC time base, 100 ns
    LONGLONG last_vt = -1;
    LONGLONG a_next = -1;      // next audio sample time, 100 ns from t0
    unsigned long long frames = 0;
    wchar_t path[MAX_PATH * 2] = {};
    bool mf_started = false;
} g_enc;

void enc_close()
{
    if (g_enc.sw) { g_enc.sw->Release(); g_enc.sw = nullptr; }
    for (unsigned i = 0; i < RING; ++i) if (g_enc.t11[i]) { g_enc.t11[i]->Release(); g_enc.t11[i] = nullptr; }
    if (g_enc.mgr) { g_enc.mgr->Release(); g_enc.mgr = nullptr; }
    if (g_enc.d11) { g_enc.d11->Release(); g_enc.d11 = nullptr; }
    if (g_enc.mf_started) { (void)g_mf.Shutdown(); g_enc.mf_started = false; }
    for (unsigned i = 0; i < RING; ++i) g_slot_busy[i].store(0);
    g_enc.open = false;
}

void audio_stop()
{
    g_audio_stop.store(true);
    if (g_audio.joinable()) g_audio.join();
    g_audio_stop.store(false);
}

bool enc_open(char *why, unsigned whyn)
{
    g_enc = encoder{};
    if (!mf_load()) { snprintf(why, whyn, "Media Foundation is not available (Windows N edition without the Media Feature Pack?)"); return false; }

    IDXGIFactory4 *fac = nullptr;
    IDXGIAdapter *ad = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) hr = fac->EnumAdapterByLuid(g_luid, IID_PPV_ARGS(&ad));
    if (SUCCEEDED(hr))
    {
        const D3D_FEATURE_LEVEL fl[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               fl, 2, D3D11_SDK_VERSION, &g_enc.d11, nullptr, nullptr);
    }
    if (ad) ad->Release();
    if (fac) fac->Release();
    if (FAILED(hr)) { snprintf(why, whyn, "D3D11 device on the neural card 0x%08X", (unsigned)hr); enc_close(); return false; }
    {
        ID3D11Multithread *mt = nullptr;
        if (SUCCEEDED(g_enc.d11->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    }
    ID3D11Device1 *d1 = nullptr;
    hr = g_enc.d11->QueryInterface(IID_PPV_ARGS(&d1));
    for (unsigned i = 0; i < RING && SUCCEEDED(hr); ++i)
        hr = d1->OpenSharedResource1(g_slot_h[i], IID_PPV_ARGS(&g_enc.t11[i]));
    if (d1) d1->Release();
    if (FAILED(hr))
    {
        snprintf(why, whyn, "sharing the frames with the encoder 0x%08X (adapter LUID %08lX:%08lX, format %d, %ux%u)",
                 (unsigned)hr, (unsigned long)g_luid.HighPart, (unsigned long)g_luid.LowPart, (int)g_texfmt, g_rw, g_rh);
        enc_close(); return false;
    }

    hr = g_mf.Startup(MF_VERSION, 0);
    if (FAILED(hr)) { snprintf(why, whyn, "MFStartup 0x%08X", (unsigned)hr); enc_close(); return false; }
    g_enc.mf_started = true;
    hr = g_mf.CreateDXGIDeviceManager(&g_enc.token, &g_enc.mgr);
    if (SUCCEEDED(hr)) hr = g_enc.mgr->ResetDevice(g_enc.d11, g_enc.token);
    IMFAttributes *at = nullptr;
    if (SUCCEEDED(hr)) hr = g_mf.CreateAttributes(&at, 4);
    if (SUCCEEDED(hr)) hr = at->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, g_enc.mgr);
    if (SUCCEEDED(hr)) hr = at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr) && !make_file(g_enc.path, MAX_PATH * 2, L"captures", L"MGPU", L".mp4")) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = g_mf.CreateSinkWriterFromURL(g_enc.path, nullptr, at, &g_enc.sw);
    if (at) at->Release();
    if (FAILED(hr)) { snprintf(why, whyn, "opening the MP4 writer 0x%08X", (unsigned)hr); enc_close(); return false; }

    // Video: H.264, bitrate from pixels x frame rate, at least 15 and at most 80 Mbit/s.
    const unsigned fps = g_nominal_fps;
    double br = (double)g_rw * (double)g_rh * (double)fps * 0.08;
    if (br < 15.0e6) br = 15.0e6;
    if (br > 80.0e6) br = 80.0e6;
    IMFMediaType *vo = nullptr, *vi = nullptr;
    hr = g_mf.CreateMediaType(&vo);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)br);
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vo, MF_MT_FRAME_SIZE, g_rw, g_rh);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = g_enc.sw->AddStream(vo, &g_enc.vidx);
    if (SUCCEEDED(hr)) hr = g_mf.CreateMediaType(&vi);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_SUBTYPE, (g_texfmt == DXGI_FORMAT_B8G8R8A8_UNORM) ? MFVideoFormat_ARGB32
                                                                                             : MGPU_MFVideoFormat_ABGR32);
    if (SUCCEEDED(hr)) hr = vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vi, MF_MT_FRAME_SIZE, g_rw, g_rh);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = g_enc.sw->SetInputMediaType(g_enc.vidx, vi, nullptr);
    if (vo) vo->Release();
    if (vi) vi->Release();
    if (FAILED(hr)) { snprintf(why, whyn, "setting up the H.264 encoder 0x%08X", (unsigned)hr); enc_close(); return false; }

    // Audio: AAC 192 kbit/s from 16-bit stereo PCM. A failure here records video only.
    if (get(S_REC_AUDIO) != 0)
    {
        IMFMediaType *ao = nullptr, *ai = nullptr;
        HRESULT ah = g_mf.CreateMediaType(&ao);
        if (SUCCEEDED(ah)) ah = ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(ah)) ah = ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(ah)) ah = ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(ah)) ah = ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, ARATE);
        if (SUCCEEDED(ah)) ah = ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(ah)) ah = ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        if (SUCCEEDED(ah)) ah = g_enc.sw->AddStream(ao, &g_enc.aidx);
        if (SUCCEEDED(ah)) ah = g_mf.CreateMediaType(&ai);
        if (SUCCEEDED(ah)) ah = ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(ah)) ah = ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(ah)) ah = ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(ah)) ah = ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, ARATE);
        if (SUCCEEDED(ah)) ah = ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(ah)) ah = ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(ah)) ah = ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, ARATE * 4u);
        if (SUCCEEDED(ah)) ah = g_enc.sw->SetInputMediaType(g_enc.aidx, ai, nullptr);
        if (ao) ao->Release();
        if (ai) ai->Release();
        g_enc.audio = SUCCEEDED(ah);
        if (!g_enc.audio)
        {
            char l[200]; snprintf(l, sizeof l, "[MGPU][VR1] AAC audio could not be set up (0x%08X) - recording video only.", (unsigned)ah);
            mgpu::diag::warn(l);
        }
    }
    hr = g_enc.sw->BeginWriting();
    if (FAILED(hr)) { snprintf(why, whyn, "BeginWriting 0x%08X", (unsigned)hr); enc_close(); return false; }
    if (g_enc.audio)
    {
        { std::lock_guard<std::mutex> lk(g_mx); g_aq.clear(); }
        g_audio_mode.store(0);
        g_audio = std::thread(audio_main);
    }
    g_enc.open = true;
    return true;
}

void enc_write_audio(const short *pcm, size_t frames, LONGLONG t)
{
    while (frames > 0)
    {
        const size_t n = (frames > ARATE / 10u) ? ARATE / 10u : frames;   // at most 100 ms per sample
        const DWORD bytes = (DWORD)(n * 4u);
        IMFMediaBuffer *b = nullptr;
        IMFSample *s = nullptr;
        BYTE *p = nullptr;
        if (SUCCEEDED(g_mf.CreateMemoryBuffer(bytes, &b)) && SUCCEEDED(b->Lock(&p, nullptr, nullptr)))
        {
            if (pcm) memcpy(p, pcm, bytes); else memset(p, 0, bytes);
            b->Unlock();
            b->SetCurrentLength(bytes);
            const LONGLONG dur = (LONGLONG)((double)n * 1.0e7 / (double)ARATE);
            if (SUCCEEDED(g_mf.CreateSample(&s)) && SUCCEEDED(s->AddBuffer(b)))
            {
                s->SetSampleTime(t); s->SetSampleDuration(dur);
                (void)g_enc.sw->WriteSample(g_enc.aidx, s);
            }
            t += dur;
            g_enc.a_next = t;
        }
        if (s) s->Release();
        if (b) b->Release();
        if (pcm) pcm += n * 2u;
        frames -= n;
    }
}

void enc_audio_tick()
{
    if (!g_enc.audio) { std::lock_guard<std::mutex> lk(g_mx); g_aq.clear(); return; }
    std::deque<achunk> q;
    { std::lock_guard<std::mutex> lk(g_mx); q.swap(g_aq); }
    const LONGLONG GAP = 300000;   // 30 ms
    for (achunk &c : q)
    {
        const size_t frames = c.pcm.size() / 2u;
        LONGLONG t = c.t100 - g_enc.t0_100;
        if (t + (LONGLONG)((double)frames * 1.0e7 / ARATE) <= 0) continue;   // from before the recording
        if (g_enc.a_next < 0)
        {
            g_enc.a_next = 0;
            if (t > 0) enc_write_audio(nullptr, (size_t)((double)t * ARATE / 1.0e7), 0);   // silence up to the first sound
        }
        else if (t > g_enc.a_next + GAP)
            enc_write_audio(nullptr, (size_t)((double)(t - g_enc.a_next) * ARATE / 1.0e7), g_enc.a_next);   // nothing played
        enc_write_audio(c.pcm.data(), frames, g_enc.a_next);
    }
    // Nothing arrived for a while (the game is silent, or the capture only
    // delivers while sound plays): keep the audio track moving with silence so
    // the writer never holds video back waiting for it.
    const LONGLONG now_t = to_100ns(qpc_now()) - g_enc.t0_100;
    const LONGLONG LAG = 3000000;   // 300 ms
    if (g_enc.a_next < 0) { if (now_t > LAG) { g_enc.a_next = 0; } else return; }
    if (now_t - LAG > g_enc.a_next)
        enc_write_audio(nullptr, (size_t)((double)(now_t - LAG - g_enc.a_next) * ARATE / 1.0e7), g_enc.a_next);
}

void enc_video_tick()
{
    std::vector<vframe> ready;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        while (!g_vf_queue.empty() && fence_done(g_vf_queue.front().fence, g_vf_queue.front().value))
        { ready.push_back(g_vf_queue.front()); g_vf_queue.pop_front(); }
    }
    const LONGLONG dur = (LONGLONG)(1.0e7 / (double)(g_nominal_fps ? g_nominal_fps : 60));
    for (vframe &v : ready)
    {
        bool sent = false;
        LONGLONG t = to_100ns(v.qpc) - g_enc.t0_100;
        if (t <= g_enc.last_vt) t = g_enc.last_vt + 1;
        IMFMediaBuffer *b = nullptr;
        mgpu_IMFTrackedSample *ts = nullptr;
        IMFSample *s = nullptr;
        if (g_enc.open && t >= 0 &&
            SUCCEEDED(g_mf.CreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), g_enc.t11[v.slot], 0, FALSE, &b)))
        {
            IMF2DBuffer *b2 = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2)))) { if (SUCCEEDED(b2->GetContiguousLength(&len))) b->SetCurrentLength(len); b2->Release(); }
            if (SUCCEEDED(g_mf.CreateTrackedSample(&ts)) && SUCCEEDED(ts->QueryInterface(IID_PPV_ARGS(&s))) &&
                SUCCEEDED(s->AddBuffer(b)))
            {
                s->SetSampleTime(t);
                s->SetSampleDuration(dur);
                g_cb[v.slot].slot = v.slot;
                if (SUCCEEDED(ts->SetAllocator(&g_cb[v.slot], nullptr)))
                {
                    sent = SUCCEEDED(g_enc.sw->WriteSample(g_enc.vidx, s));
                    if (sent) { g_enc.last_vt = t; ++g_enc.frames; }
                }
            }
        }
        if (s) s->Release();
        if (ts) ts->Release();   // the allocator callback frees the slot once the encoder lets go
        if (b) b->Release();
        if (!sent) g_slot_busy[v.slot].store(0);
        if (v.fence) v.fence->Release();
    }
}

void enc_finish()
{
    // Whatever the present path has queued: wait up to 2 s for its fences.
    const ULONGLONG until = GetTickCount64() + 2000;
    for (;;)
    {
        enc_video_tick();
        bool empty;
        { std::lock_guard<std::mutex> lk(g_mx); empty = g_vf_queue.empty(); }
        if (empty || GetTickCount64() > until) break;
        Sleep(2);
    }
    audio_stop();
    enc_audio_tick();
    const HRESULT hr = g_enc.sw ? g_enc.sw->Finalize() : E_FAIL;
    char fn[400], l[700];
    narrow(file_part(g_enc.path), fn, sizeof fn);
    const LONGLONG secs = g_enc.last_vt > 0 ? g_enc.last_vt / 10000000 : 0;
    const int am = g_audio_mode.load();
    snprintf(l, sizeof l, "%s %s: %llu frames, %lld s, audio %s, %llu frames skipped (encoder busy).",
             SUCCEEDED(hr) ? "Video saved" : "Video FAILED to finish", fn, g_enc.frames, (long long)secs,
             !g_enc.audio ? "off" : am == 1 ? "game only" : am == 2 ? "all PC sound" : "none captured",
             (unsigned long long)g_rec_dropped.load());
    set_msg(l, SUCCEEDED(hr) ? "VIDEO SAVED" : "VIDEO FAILED", 4000);
    char d[800]; snprintf(d, sizeof d, "[MGPU][VR1] %s Finalize=0x%08X", l, (unsigned)hr);
    if (SUCCEEDED(hr)) mgpu::diag::info(d); else mgpu::diag::warn(d);
    enc_close();
    std::lock_guard<std::mutex> lk(g_mx);
    for (vframe &v : g_vf_queue) { if (v.fence) v.fence->Release(); g_slot_busy[v.slot].store(0); }
    g_vf_queue.clear();
}

void rec_tick()
{
    const int st = g_rec.load();
    if (st == 2 && !g_enc.open)
    {
        char why[300] = {};
        if (!enc_open(why, sizeof why))
        {
            char l[400]; snprintf(l, sizeof l, "Recording could not start: %s.", why);
            set_msg(l, "REC FAILED - SEE LOG", 4000);
            char d[500]; snprintf(d, sizeof d, "[MGPU][VR1] %s", l);
            mgpu::diag::warn(d);
            g_rec.store(0);
            return;
        }
        const LONGLONG t0 = qpc_now();
        g_enc.t0_100 = to_100ns(t0);
        g_rec_t0.store(t0);
        g_rec_frames.store(0); g_rec_dropped.store(0);
        char fn[300]; narrow(file_part(g_enc.path), fn, sizeof fn);
        char d[600];
        snprintf(d, sizeof d, "[MGPU][VR1] recording %s: %ux%u H.264 at a nominal %u fps on the neural card, %s, "
                 "limit %d s (0 = until stopped).", fn, g_rw, g_rh, g_nominal_fps,
                 g_enc.audio ? "with audio" : "no audio", get(S_REC_SEC));
        mgpu::diag::info(d);
        g_rec.store(3);
        return;
    }
    if (g_enc.open)
    {
        enc_video_tick();
        enc_audio_tick();
        if (st == 4) { enc_finish(); g_rec.store(0); }
        return;
    }
    if (st == 4) g_rec.store(0);
    // Idle: anything queued late frees its slot.
    std::lock_guard<std::mutex> lk(g_mx);
    for (vframe &v : g_vf_queue) { if (v.fence) v.fence->Release(); g_slot_busy[v.slot].store(0); }
    g_vf_queue.clear();
}

// ===========================================================================
// BM1 benchmark
// ===========================================================================
std::atomic<int> g_bm{0};             // 0 idle, 1 countdown, 2 measuring, 3 writing
std::atomic<bool> g_bm_stop{false};
LONGLONG g_bm_cd0 = 0, g_bm_t0 = 0;   // present path
std::vector<LONGLONG> g_bm_t;
std::vector<unsigned char> g_bm_gen;
std::vector<float> g_bm_lat;          // ms, real frames: game hand-over -> shown
double g_bm_len = 0.0;                // this run's length in seconds
bool g_bm_replay = false;             // this run plays the input path
bool g_frame_copy = false, g_frame_gen = false;   // present path: this present carried a frame
unsigned long long g_frame_cap = 0;

struct bm_snap { unsigned long long produced = 0, consumed = 0; };
struct bm_job
{
    std::vector<LONGLONG> t;
    std::vector<unsigned char> gen;
    std::vector<float> lat;
    bm_snap a, b;
    char settings[600] = {};
    bool replay = false, replay_aborted = false;
    bool ready = false;
} g_bm_job;                           // g_mx

const char *fg_name(int m)
{
    switch (m) { case 0: return "off"; case 1: return "mod's own"; case 2: return "motion view";
                 case 3: return "AMD FSR 3"; case 4: return "NVIDIA DLSS FG"; default: return "?"; }
}

bm_snap bm_snapshot(char *settings, unsigned n)
{
    mgpu::gpu1::ui_state st{};
    mgpu::gpu1::ui_read(st);
    bm_snap s; s.produced = st.produced; s.consumed = st.consumed;
    if (settings != nullptr)
    {
        const int fg = mgpu::gpu1::ui_frame_gen();
        char res[160];
        if (st.sr_on)
            snprintf(res, sizeof res, "neural rendering %ux%u, upscaled to %ux%u", st.sr_w, st.sr_h, g_last_w, g_last_h);
        else
            snprintf(res, sizeof res, "neural rendering %ux%u (no upscaling)", g_last_w, g_last_h);
        char fw[64] = "";
        if (mgpu::framewarp::enabled())
            snprintf(fw, sizeof fw, " | FrameWarp on (%.3f deg/count)", (double)mgpu::framewarp::scale() / 1000.0);
        snprintf(settings, n,
                 "FrameGen=%d (%s)%s | Neural=%s Passes=%u Intensity=%.2f ColorStrength=%.2f | %s | %s%s",
                 fg, fg_name(fg),
                 (fg == 4) ? (std::string(" ") + std::to_string(mgpu::gpu1::ui_fg_mult()) + "x").c_str() : "",
                 st.neural ? "on" : "off", st.passes, st.intensity[0], mgpu::gpu1::ui_color_strength(), res,
                 mgpu::gpu1::ui_present_vsync() ? "bridge vsync on" : "bridge vsync off", fw);
    }
    return s;
}

double pct(std::vector<double> v, double p)   // v sorted ascending
{
    if (v.empty()) return 0.0;
    size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

double low_fps(const std::vector<double> &desc, double frac)   // desc = frame times sorted descending
{
    if (desc.empty()) return 0.0;
    size_t n = (size_t)((double)desc.size() * frac);
    if (n < 1) n = 1;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += desc[i];
    const double m = s / (double)n;
    return m > 0.0 ? 1000.0 / m : 0.0;
}

void bench_write(bm_job &j)
{
    const double fq = (double)qpc_freq();
    const size_t n = j.t.size();
    if (n < 10)
    {
        set_msg("Benchmark too short - no result.", "BENCH TOO SHORT", 4000);
        return;
    }
    const double dur = (double)(j.t[n - 1] - j.t[0]) / fq;
    std::vector<double> ft, ftr;
    ft.reserve(n);
    LONGLONG last_real = -1;
    size_t gen_n = 0;
    for (size_t i = 1; i < n; ++i) ft.push_back((double)(j.t[i] - j.t[i - 1]) * 1000.0 / fq);
    for (size_t i = 0; i < n; ++i)
    {
        if (j.gen[i]) { ++gen_n; continue; }
        if (last_real >= 0) ftr.push_back((double)(j.t[i] - last_real) * 1000.0 / fq);
        last_real = j.t[i];
    }
    std::vector<double> asc = ft, desc = ft;
    std::sort(asc.begin(), asc.end());
    std::sort(desc.begin(), desc.end(), [](double a, double b) { return a > b; });
    const double avg = (double)(n - 1) / dur;
    const double p50 = pct(asc, 0.50), p95 = pct(asc, 0.95), p99 = pct(asc, 0.99), mx = asc.back();
    const double l1 = low_fps(desc, 0.01), l01 = low_fps(desc, 0.001);
    size_t stut = 0;
    for (double v : ft) if (v > 2.5 * p50) ++stut;
    std::vector<double> rasc = ftr;
    std::sort(rasc.begin(), rasc.end());
    double rmean = 0.0, rsd = 0.0;
    for (double v : ftr) rmean += v;
    if (!ftr.empty()) rmean /= (double)ftr.size();
    for (double v : ftr) rsd += (v - rmean) * (v - rmean);
    if (ftr.size() > 1) rsd = std::sqrt(rsd / (double)(ftr.size() - 1));
    const double real_fps = rmean > 0.0 ? 1000.0 / rmean : 0.0;
    std::vector<double> lat(j.lat.begin(), j.lat.end());
    std::sort(lat.begin(), lat.end());
    double lat_avg = 0.0;
    for (double v : lat) lat_avg += v;
    if (!lat.empty()) lat_avg /= (double)lat.size();
    const double lat50 = pct(lat, 0.50), lat95 = pct(lat, 0.95), lat99 = pct(lat, 0.99);
    const char *mode = !j.replay ? "manual" : j.replay_aborted ? "input replay (STOPPED EARLY)" : "input replay";
    const double game_fps = (double)(j.b.produced - j.a.produced) / dur;
    const double nr_fps   = (double)(j.b.consumed - j.a.consumed) / dur;

    wchar_t csv[MAX_PATH * 2] = {}, txt[MAX_PATH * 2] = {}, res[MAX_PATH * 2] = {};
    if (!make_file(csv, MAX_PATH * 2, L"benchmarks", L"MGPU_bench", L".csv")) return;
    wcscpy_s(txt, MAX_PATH * 2, csv);
    wchar_t *dot = wcsrchr(txt, L'.');
    if (dot) wcscpy_s(dot, (size_t)(MAX_PATH * 2 - (dot - txt)), L".txt");
    base_folder(res, MAX_PATH * 2);
    wcscat_s(res, MAX_PATH * 2, L"benchmarks\\results.csv");

    FILE *f = nullptr;
    if (_wfopen_s(&f, csv, L"w") == 0 && f)
    {
        fprintf(f, "frame,time_ms,frametime_ms,generated\n");
        for (size_t i = 0; i < n; ++i)
            fprintf(f, "%zu,%.3f,%.3f,%u\n", i, (double)(j.t[i] - j.t[0]) * 1000.0 / fq,
                    i ? ft[i - 1] : 0.0, (unsigned)j.gen[i]);
        fclose(f);
    }
    SYSTEMTIME tm{}; GetLocalTime(&tm);
    char when[40];
    snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u:%02u", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
    if (_wfopen_s(&f, txt, L"w") == 0 && f)
    {
        fprintf(f, "MGPU Bridge benchmark - %s\n\nSettings: %s\n\n", when, j.settings);
        fprintf(f, "Duration            %.1f s\nFrames shown        %zu (%zu generated, %zu real)\n\n", dur, n, gen_n, n - gen_n);
        fprintf(f, "SHOWN FRAMES (what reached the screen)\n");
        fprintf(f, "  Average           %.1f fps\n  1%% low            %.1f fps\n  0.1%% low          %.1f fps\n", avg, l1, l01);
        fprintf(f, "  Frame time        median %.2f ms, 95th %.2f ms, 99th %.2f ms, max %.2f ms\n", p50, p95, p99, mx);
        fprintf(f, "  Stutters          %zu frames over 2.5x the median\n\n", stut);
        fprintf(f, "REAL FRAMES (neural output, generated frames excluded)\n");
        fprintf(f, "  Average           %.1f fps\n  Pacing            mean %.2f ms, std dev %.2f ms, 99th %.2f ms\n\n",
                real_fps, rmean, rsd, pct(rasc, 0.99));
        fprintf(f, "LATENCY ADDED BY THE MOD (game hands over its frame -> neural frame on screen)\n");
        if (lat.empty()) fprintf(f, "  not measured (no real frames carried a hand-over time)\n\n");
        else fprintf(f, "  Average           %.1f ms\n  Median            %.1f ms\n  95th / 99th       %.1f / %.1f ms\n"
                        "  Includes transport, neural rendering, upscaling and frame generation's hold-back.\n"
                        "  Not included: the game's own input-to-render time and the monitor.\n\n",
                     lat_avg, lat50, lat95, lat99);
        fprintf(f, "GAME / NEURAL CARD\n  Game frames       %.1f fps\n  Neural card took  %.1f fps\n", game_fps, nr_fps);
        fprintf(f, "\nRun: %s\n", mode);
        fprintf(f, "\nFor repeatable results: same save, same spot, same camera move, same length.\n");
        fclose(f);
    }
    bool header = (GetFileAttributesW(res) == INVALID_FILE_ATTRIBUTES);
    if (_wfopen_s(&f, res, L"a") == 0 && f)
    {
        if (header)
            fprintf(f, "date,run,duration_s,avg_fps,1pct_low_fps,0p1pct_low_fps,ft_median_ms,ft_p95_ms,ft_p99_ms,ft_max_ms,"
                       "stutters,real_fps,real_pacing_sd_ms,latency_avg_ms,latency_p95_ms,latency_p99_ms,game_fps,neural_fps,"
                       "generated_share,settings\n");
        fprintf(f, "%s,%s,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%zu,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,\"%s\"\n", when,
                !j.replay ? "manual" : j.replay_aborted ? "replay-stopped" : "replay", dur, avg, l1, l01,
                p50, p95, p99, mx, stut, real_fps, rsd, lat_avg, lat95, lat99, game_fps, nr_fps, (double)gen_n / (double)n,
                j.settings);
        fclose(f);
    }
    char l[700], hud[64], fn[300];
    narrow(file_part(txt), fn, sizeof fn);
    snprintf(l, sizeof l, "Benchmark %.0f s (%s): %.1f fps avg, 1%% low %.1f, 0.1%% low %.1f, real %.1f fps (pacing sd %.2f ms), "
             "added latency %.1f ms avg / %.1f ms 99th, game %.1f fps. Saved %s and added to results.csv.",
             dur, mode, avg, l1, l01, real_fps, rsd, lat_avg, lat99, game_fps, fn);
    snprintf(hud, sizeof hud, "BENCH %.1f FPS 1PCT %.1f LAT %.1f MS", avg, l1, lat_avg);
    set_msg(l, hud, 20000);
    char d[900]; snprintf(d, sizeof d, "[MGPU][BM1] %s Settings: %s", l, j.settings);
    mgpu::diag::info(d);
}

void bench_tick()
{
    if (g_bm.load() != 3) return;
    bm_job j;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_bm_job.ready) return;
        j.t.swap(g_bm_job.t); j.gen.swap(g_bm_job.gen); j.lat.swap(g_bm_job.lat);
        j.a = g_bm_job.a; j.b = g_bm_job.b;
        j.replay = g_bm_job.replay; j.replay_aborted = g_bm_job.replay_aborted;
        memcpy(j.settings, g_bm_job.settings, sizeof j.settings);
        g_bm_job.ready = false;
    }
    bench_write(j);
    g_bm.store(0);
}

// ===========================================================================
// worker thread
// ===========================================================================
std::thread g_worker;
std::atomic<bool> g_wstop{false};
// REV4: start/stop of the worker. It was a once_flag, so after the first
// shutdown (every bridge rearm) nothing could ever start it again and the
// present-side taps stayed dead (g_dead) for the rest of the session.
std::mutex g_wmx;

void worker_main()
{
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!g_wstop.load())
    {
        shots_tick();
        rec_tick();
        bench_tick();
        {   // REV7: cheap - normally empty
            std::lock_guard<std::mutex> plk(g_pmx);
            if (!g_grave.empty()) grave_drain(true);
        }
        Sleep(2);
    }
    // Shutting down: finish a recording that is still open.
    if (g_enc.open) { g_rec.store(4); rec_tick(); }
    shots_tick();
    CoUninitialize();
}

void ensure_worker()
{
    std::lock_guard<std::mutex> wl(g_wmx);
    if (g_worker.joinable()) return;
    g_wstop.store(false);
    {
        std::lock_guard<std::mutex> plk(g_pmx);
        g_dead = false;
        grave_drain(true);   // REV7
    }
    g_worker = std::thread(worker_main);
}

} // namespace

// ===========================================================================
// public
// ===========================================================================
bool base_folder(wchar_t *out, unsigned n)
{
    if (!module_dir(out, n)) return false;
    wcscat_s(out, n, L"mgpu\\");
    (void)CreateDirectoryW(out, nullptr);
    return true;
}

void request_screenshot()
{
    ensure_worker();
    g_ss_want.store(true);
}

void toggle_recording()
{
    ensure_worker();
    int st = g_rec.load();
    if (st == 0)
    {
        g_rec_dropped.store(0);
        g_rec.store(1);
        set_msg("Recording starting...", "REC STARTING", 2000);
    }
    else if (st == 2 || st == 3) g_rec.store(4);
}

void toggle_benchmark()
{
    ensure_worker();
    const int st = g_bm.load();
    if (st == 0) { g_bm_stop.store(false); g_bm_cd0 = 0; g_bm.store(1); }
    else if (st == 1) { g_bm.store(0); set_msg("Benchmark cancelled.", "BENCH CANCELLED", 3000); }
    else if (st == 2) g_bm_stop.store(true);
}

void shutdown()
{
    mgpu::replay::shutdown();
    std::lock_guard<std::mutex> wl(g_wmx);   // REV4: not while a hotkey is starting it
    if (!g_worker.joinable()) return;
    if (g_rec.load() != 0)
    {
        g_rec.store(4);
        const ULONGLONG until = GetTickCount64() + 5000;
        while (g_rec.load() != 0 && GetTickCount64() < until) Sleep(5);
    }
    g_wstop.store(true);
    g_worker.join();
    std::lock_guard<std::mutex> plk(g_pmx);
    g_dead = true;
    grave_drain(true);   // REV7: anything parked by an earlier stop
    // Our GPU work must be done before its resources go.
    // REV5: and if it cannot be confirmed done (a stalled GPU, a wait that
    // times out - a LOST device is the exception, see REV6 below), the
    // resources are LEFT ALLOCATED rather than
    // released under the GPU. A leak at shutdown costs nothing; freeing memory
    // the GPU is still writing can take the driver down with the game.
    bool gpu_idle = true;
    if (g_last_fence != nullptr)
    {
        if (!fence_done(g_last_fence, g_last_value))
        {
            gpu_idle = false;
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev && SUCCEEDED(g_last_fence->SetEventOnCompletion(g_last_value, ev)))
                (void)WaitForSingleObject(ev, 5000);
            if (ev) CloseHandle(ev);
            // REV6: decided here, not by fence_done(). fence_done() treats
            // UINT64_MAX as NOT done on purpose - for a screenshot it means
            // the copied pixels are invalid. For shutdown the question is
            // different: is the GPU still using these buffers? A removed
            // device reports UINT64_MAX on every fence and executes nothing
            // more, so then the answer is no and they are released normally.
            const UINT64 c = g_last_fence->GetCompletedValue();
            const bool removed = (c == UINT64_MAX);
            gpu_idle = removed || c >= g_last_value;
            if (removed)
                mgpu::diag::warn("[MGPU][REV6] capture shutdown: the GPU device was lost - nothing is in "
                                 "flight, buffers released.");
        }
        if (!gpu_idle)
        {
            // REV7: park everything the GPU may still touch with the fence that
            // covers it, then reset the globals as a normal shutdown would, so
            // a rearm starts clean and builds against its own device.
            grave g{ g_last_fence, g_last_value, {} };   // takes over g_last_fence's reference
            g_last_fence = nullptr;
            for (unsigned i = 0; i < RING; ++i)
            {
                if (g_slot_h[i]) { CloseHandle(g_slot_h[i]); g_slot_h[i] = nullptr; }
                if (g_slot_tex[i]) { g.objs.push_back(g_slot_tex[i]); g_slot_tex[i] = nullptr; }
                g_slot_busy[i].store(0);
            }
            if (g_csrc)    { g.objs.push_back(g_csrc);    g_csrc = nullptr; }
            if (g_scratch) { g.objs.push_back(g_scratch); g_scratch = nullptr; }
            if (g_pso)     { g.objs.push_back(g_pso);     g_pso = nullptr; }
            if (g_rs)      { g.objs.push_back(g_rs);      g_rs = nullptr; }
            if (g_heap)    { g.objs.push_back(g_heap);    g_heap = nullptr; }
            g_bw = g_bh = g_rw = g_rh = 0;
            auto park = [&](shot &sh) {
                if (sh.rb) g.objs.push_back(sh.rb);
                if (sh.fence) g.objs.push_back(sh.fence);
                sh.rb = nullptr; sh.fence = nullptr;
            };
            for (shot &sh : g_ss_frame) park(sh);
            g_ss_frame.clear();
            {
                std::lock_guard<std::mutex> lk(g_mx);
                for (shot &sh : g_ss_queue) park(sh);
                g_ss_queue.clear();
            }
            g_grave.push_back(std::move(g));
            char l[220];
            snprintf(l, sizeof l, "[MGPU][REV7] capture shutdown: the last GPU copy did not finish within 5 s - "
                     "its buffers are parked and released once it does (%u parked set(s)).",
                     (unsigned)g_grave.size());
            mgpu::diag::warn(l);
            return;
        }
        g_last_fence->Release();
        g_last_fence = nullptr;
    }
    for (shot &s : g_ss_frame) shot_free(s);
    g_ss_frame.clear();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (shot &s : g_ss_queue) shot_free(s);
        g_ss_queue.clear();
    }
    rec_release_d3d12();
}

static void tap(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                unsigned w, unsigned h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st, bool hud_stage)
{
    if (dev == nullptr || cl == nullptr || bb == nullptr) return;
    // Each capture is taken at one of the two points: before the FPS HUD is
    // drawn (clean) or after it (exactly what is on screen).
    const bool ss_here  = (get(S_SHOT_HUD) != 0) == hud_stage;
    const bool rec_here = (get(S_REC_HUD) != 0) == hud_stage;
    const bool want_ss = ss_here && g_ss_want.exchange(false);
    int rs = rec_here ? g_rec.load() : 0;
    if (rs == 1)
    {
        char why[200] = {};
        if (!rec_build(dev, w, h, fmt, why, sizeof why))
        {
            char l[300]; snprintf(l, sizeof l, "Recording could not start: %s.", why);
            set_msg(l, "REC FAILED - SEE LOG", 4000);
            char d[400]; snprintf(d, sizeof d, "[MGPU][VR1] %s", l);
            mgpu::diag::warn(d);
            g_rec.store(0);
        }
        else
        {
            const int f = get(S_REC_FPS);
            unsigned nom = (f > 0) ? (unsigned)f : (unsigned)(g_present_rate + 0.5);
            if (nom < 30u) nom = 30u;
            if (nom > 240u) nom = 240u;
            g_nominal_fps = nom;
            g_rec_last = 0;
            g_rec.store(2);   // the worker opens the encoder, then sets 3
        }
        rs = g_rec.load();
    }

    bool rec_frame = false;
    unsigned slot = 0;
    if (rs == 3)
    {
        const LONGLONG now = qpc_now();
        const int lim = get(S_REC_SEC);
        if (lim > 0 && now - g_rec_t0.load() >= (LONGLONG)lim * qpc_freq()) g_rec.store(4);
        else if (w != g_bw || h != g_bh || fmt != g_srcfmt)
        {
            set_msg("Recording stopped: the bridge's resolution or format changed.", "REC STOPPED", 4000);
            g_rec.store(4);
        }
        else
        {
            const int f = get(S_REC_FPS);
            const bool due = (f <= 0) || g_rec_last == 0 ||
                             (double)(now - g_rec_last) >= 0.9 * (double)qpc_freq() / (double)f;
            if (due)
            {
                for (unsigned i = 0; i < RING && !rec_frame; ++i)
                {
                    const unsigned k = (g_ring_next + i) % RING;
                    int expect = 0;
                    if (g_slot_busy[k].compare_exchange_strong(expect, 1)) { slot = k; rec_frame = true; }
                }
                if (rec_frame) { g_ring_next = (slot + 1u) % RING; g_rec_last = now; }
                else g_rec_dropped.fetch_add(1);
            }
        }
    }

    if (!want_ss && !rec_frame) return;
    barrier(cl, bb, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (want_ss) shot_record(dev, cl, bb, w, h, fmt);
    if (rec_frame)
    {
        rec_record(dev, cl, bb, slot);
        g_vf_frame.push_back(vframe{ slot, qpc_now(), nullptr, 0 });
        g_rec_frames.fetch_add(1);
    }
    barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
}


void on_present_copy(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                     unsigned w, unsigned h, DXGI_FORMAT fmt, bool generated, unsigned long long cap_qpc)
{
    std::lock_guard<std::mutex> plk(g_pmx);
    if (g_dead) return;
    // A previous frame whose list never got signalled: drop what it recorded.
    for (shot &s : g_ss_frame) shot_free(s);
    g_ss_frame.clear();
    for (vframe &v : g_vf_frame) g_slot_busy[v.slot].store(0);
    g_vf_frame.clear();

    g_frame_copy = true;
    g_frame_gen = generated;
    g_frame_cap = generated ? 0ull : cap_qpc;
    g_last_w = w; g_last_h = h;
    tap(dev, cl, bb, w, h, fmt, D3D12_RESOURCE_STATE_COPY_DEST, false);
}

void on_present_final(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                      unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    std::lock_guard<std::mutex> plk(g_pmx);
    if (g_dead) return;
    tap(dev, cl, bb, w, h, fmt, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
}


void on_present_submitted(ID3D12Fence *fence, unsigned long long value)
{
    if (fence == nullptr) return;
    std::lock_guard<std::mutex> plk(g_pmx);
    if (g_dead) return;
    if (g_ss_frame.empty() && g_vf_frame.empty()) return;
    if (g_last_fence != fence)
    {
        if (g_last_fence) g_last_fence->Release();
        g_last_fence = fence; fence->AddRef();
    }
    g_last_value = value;
    std::lock_guard<std::mutex> lk(g_mx);
    for (shot &s : g_ss_frame) { s.fence = fence; fence->AddRef(); s.value = value; g_ss_queue.push_back(s); }
    g_ss_frame.clear();
    for (vframe &v : g_vf_frame) { v.fence = fence; fence->AddRef(); v.value = value; g_vf_queue.push_back(v); }
    g_vf_frame.clear();
}

void on_present_shown()
{
    std::lock_guard<std::mutex> plk(g_pmx);
    if (g_dead) return;
    const LONGLONG now = qpc_now();
    // Shown-frame rate, refreshed every second: the encoder's nominal rate.
    ++g_pr_n;
    if (g_pr_t == 0) g_pr_t = now;
    else if (now - g_pr_t >= qpc_freq())
    {
        g_present_rate = (double)g_pr_n * (double)qpc_freq() / (double)(now - g_pr_t);
        g_pr_n = 0; g_pr_t = now;
    }
    if (!g_frame_copy) return;   // an idle-screen present carries no frame
    g_frame_copy = false;

    const int bs = g_bm.load();
    if (bs == 1)
    {
        if (g_bm_cd0 == 0) g_bm_cd0 = now;
        if (now - g_bm_cd0 >= (LONGLONG)get(S_BENCH_DLY) * qpc_freq())
        {
            g_bm_t.clear(); g_bm_gen.clear(); g_bm_lat.clear();
            g_bm_t.reserve(200000); g_bm_gen.reserve(200000); g_bm_lat.reserve(100000);
            g_bm_len = (double)get(S_BENCH_SEC);
            g_bm_replay = false;
            if (get(S_BENCH_RPL) != 0)
            {
                double ps = 0.0;
                if (mgpu::replay::has_path(&ps) && mgpu::replay::start_play()) { g_bm_replay = true; g_bm_len = ps; }
                else set_msg("Input replay is on but no input path is recorded for this game - running a normal benchmark.",
                             nullptr, 0);
            }
            char settings[600] = {};
            const bm_snap a = bm_snapshot(settings, sizeof settings);
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_bm_job.a = a;
                memcpy(g_bm_job.settings, settings, sizeof settings);
                g_bm_job.ready = false;
            }
            g_bm_t0 = now;
            g_bm.store(2);
            char d[700]; snprintf(d, sizeof d, "[MGPU][BM1] benchmark started (%.1f s%s). Settings: %s", g_bm_len,
                                  g_bm_replay ? ", input replay" : "", settings);
            mgpu::diag::info(d);
        }
    }
    else if (bs == 2)
    {
        if (g_bm_t.size() < 2000000) { g_bm_t.push_back(now); g_bm_gen.push_back(g_frame_gen ? 1 : 0); }
        if (g_frame_cap != 0 && g_bm_lat.size() < 2000000 && now > (LONGLONG)g_frame_cap)
        {
            const double ms = (double)(now - (LONGLONG)g_frame_cap) * 1000.0 / (double)qpc_freq();
            if (ms < 1000.0) g_bm_lat.push_back((float)ms);
        }
        const bool replay_over = g_bm_replay && !mgpu::replay::playing();
        if (g_bm_stop.load() || replay_over || (double)(now - g_bm_t0) >= g_bm_len * (double)qpc_freq())
        {
            if (g_bm_replay) mgpu::replay::stop_play();
            const bm_snap b = bm_snapshot(nullptr, 0);
            std::lock_guard<std::mutex> lk(g_mx);
            g_bm_job.t.swap(g_bm_t); g_bm_job.gen.swap(g_bm_gen); g_bm_job.lat.swap(g_bm_lat);
            g_bm_job.replay = g_bm_replay;
            g_bm_job.replay_aborted = g_bm_replay && mgpu::replay::aborted();
            g_bm_job.b = b;
            g_bm_job.ready = true;
            g_bm_t.clear(); g_bm_gen.clear(); g_bm_lat.clear();
            g_bm.store(3);
        }
    }
}

int  record_seconds()          { return get(S_REC_SEC); }
void set_record_seconds(int s) { put(S_REC_SEC, s); }
int  record_fps()              { return get(S_REC_FPS); }
void set_record_fps(int f)     { put(S_REC_FPS, f); }
bool record_audio()            { return get(S_REC_AUDIO) != 0; }
void set_record_audio(bool on) { put(S_REC_AUDIO, on ? 1 : 0); }
int  bench_seconds()           { return get(S_BENCH_SEC); }
void set_bench_seconds(int s)  { put(S_BENCH_SEC, s); }
int  bench_delay()             { return get(S_BENCH_DLY); }
bool bench_replay()            { return get(S_BENCH_RPL) != 0; }
bool screenshot_hud()          { return get(S_SHOT_HUD) != 0; }
void set_screenshot_hud(bool on) { put(S_SHOT_HUD, on ? 1 : 0); }
bool record_hud()              { return get(S_REC_HUD) != 0; }
void set_record_hud(bool on)   { put(S_REC_HUD, on ? 1 : 0); }
void set_bench_replay(bool on) { put(S_BENCH_RPL, on ? 1 : 0); }
void set_bench_delay(int s)    { put(S_BENCH_DLY, s); }

int recording_state()
{
    const int s = g_rec.load();
    return (s == 1 || s == 2) ? 1 : (s == 3) ? 2 : (s == 4) ? 3 : 0;
}

int bench_state()
{
    const int s = g_bm.load();
    return (s == 1) ? 1 : (s == 2 || s == 3) ? 2 : 0;
}

void hud_line(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    out[0] = '\0';
    const LONGLONG now = qpc_now();
    const int rs = g_rec.load();
    if (rs == 1 || rs == 2) { snprintf(out, n, "REC STARTING"); return; }
    if (rs == 3)
    {
        const LONGLONG s = (now - g_rec_t0.load()) / qpc_freq();
        const int lim = get(S_REC_SEC);
        if (lim > 0) snprintf(out, n, "REC %02lld:%02lld / %02d:%02d", (long long)(s / 60), (long long)(s % 60), lim / 60, lim % 60);
        else         snprintf(out, n, "REC %02lld:%02lld", (long long)(s / 60), (long long)(s % 60));
        return;
    }
    if (rs == 4) { snprintf(out, n, "REC SAVING"); return; }
    const int bs = g_bm.load();
    if (bs == 1)
    {
        const LONGLONG cd = (g_bm_cd0 == 0) ? 0 : (now - g_bm_cd0) / qpc_freq();
        const long long left = (long long)get(S_BENCH_DLY) - (long long)cd;
        snprintf(out, n, "BENCH STARTS IN %lld", left > 0 ? left : 0);
        return;
    }
    if (bs == 2)
    {
        const LONGLONG s = (now - g_bm_t0) / qpc_freq();
        snprintf(out, n, "BENCH %lld / %.0f S%s", (long long)s, g_bm_len, g_bm_replay ? " REPLAY" : "");
        return;
    }
    if (bs == 3) { snprintf(out, n, "BENCH SAVING"); return; }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_hudmsg[0] != '\0' && GetTickCount64() < g_hudmsg_until) snprintf(out, n, "%s", g_hudmsg);
}

void last_message(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    snprintf(out, n, "%s", g_msg);
}

} // namespace capture
} // namespace mgpu
