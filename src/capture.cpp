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
#include "d3dc.hpp"   // SC1
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
#include <cctype>
#include <cwchar>
#include <string>

#include "diag.hpp"
#include "gpu1_context.hpp"
#include "gpu_safety_d3d12.hpp"   // LR100: the bounded drain with the event-retention rule
#include "replay_rules.hpp"   // LR106: a replay benchmark waits for the overlay to close
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
setting S_REC_CODEC { "RecordCodec",     0, 0, 1,    {-1} };   // VR2: 0 H.264, 1 H.265 (HEVC)

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
// VR2: what the encoder was actually opened with. g_rec_pace is the frame rate
// frames are taken at (0 = every frame shown); it follows a fallback.
unsigned g_codec = 0;               // 0 H.264, 1 HEVC
std::atomic<int> g_rec_pace{0};
bool g_enc_fmt_fail = false;        // the last enc_open failed on the video format (worth a retry)
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

    HMODULE dc = mgpu::sys_d3dcompiler_module() /* SC1 */;
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
// VR2: 'HEVC' FOURCC subtype, defined here so older SDK headers are enough.
const GUID MGPU_MFVideoFormat_HEVC = { 0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
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
    g_enc_fmt_fail = false;
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

    // Video: H.264 (or HEVC, VR2), bitrate from pixels x frame rate, at least
    // 15 and at most 80 Mbit/s (HEVC: 0.6x that, 10 to 60 Mbit/s).
    const unsigned fps = g_nominal_fps;
    const bool hevc = (g_codec == 1);
    double br = (double)g_rw * (double)g_rh * (double)fps * 0.08 * (hevc ? 0.6 : 1.0);
    if (br < (hevc ? 10.0e6 : 15.0e6)) br = hevc ? 10.0e6 : 15.0e6;
    if (br > (hevc ? 60.0e6 : 80.0e6)) br = hevc ? 60.0e6 : 80.0e6;
    const char *step = "describing the video";
    IMFMediaType *vo = nullptr, *vi = nullptr;
    hr = g_mf.CreateMediaType(&vo);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vo->SetGUID(MF_MT_SUBTYPE, hevc ? MGPU_MFVideoFormat_HEVC : MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)br);
    if (SUCCEEDED(hr)) hr = vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vo, MF_MT_FRAME_SIZE, g_rw, g_rh);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) { step = "the output format (AddStream)"; hr = g_enc.sw->AddStream(vo, &g_enc.vidx); }
    if (SUCCEEDED(hr)) hr = g_mf.CreateMediaType(&vi);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = vi->SetGUID(MF_MT_SUBTYPE, (g_texfmt == DXGI_FORMAT_B8G8R8A8_UNORM) ? MFVideoFormat_ARGB32
                                                                                             : MGPU_MFVideoFormat_ABGR32);
    if (SUCCEEDED(hr)) hr = vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(vi, MF_MT_FRAME_SIZE, g_rw, g_rh);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(vi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) { step = "the input frames (SetInputMediaType)"; hr = g_enc.sw->SetInputMediaType(g_enc.vidx, vi, nullptr); }
    if (vo) vo->Release();
    if (vi) vi->Release();
    if (FAILED(hr))
    {
        // VR2: say exactly what was refused, and mark it as a format refusal
        // so the caller can retry at a lower frame rate (or H.264).
        snprintf(why, whyn, "setting up the %s encoder 0x%08X at %s: %ux%u, %u fps, %.0f Mbit/s, input %s",
                 hevc ? "H.265 (HEVC)" : "H.264", (unsigned)hr, step, g_rw, g_rh, fps, br / 1.0e6,
                 (g_texfmt == DXGI_FORMAT_B8G8R8A8_UNORM) ? "BGRA8" : "RGBA8");
        g_enc_fmt_fail = true;
        enc_close();
        if (g_enc.path[0]) (void)DeleteFileW(g_enc.path);   // the empty file this attempt created
        return false;
    }

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
        // VR2: an encoder that refuses the size x frame rate (H.264 tops out
        // around 4K at 60) is retried at 120, 60 and 30 fps, and an H.265 that
        // is refused at every rate falls back to H.264. Each refusal is logged.
        const unsigned want = g_nominal_fps;
        const unsigned asked_codec = (unsigned)get(S_REC_CODEC);
        unsigned rates[4] = { want, 0, 0, 0 }, nr = 1;
        for (unsigned r : { 120u, 60u, 30u }) if (r < want) rates[nr++] = r;
        char why[400] = {};
        bool ok = false;
        for (unsigned c = asked_codec; !ok; --c)
        {
            for (unsigned i = 0; i < nr && !ok; ++i)
            {
                g_codec = c;
                g_nominal_fps = rates[i];
                ok = enc_open(why, sizeof why);
                if (!ok)
                {
                    char d[600]; snprintf(d, sizeof d, "[MGPU][VR2] refused: %s.", why);
                    mgpu::diag::warn(d);
                    if (!g_enc_fmt_fail) break;   // not a format refusal: retrying cannot help
                }
            }
            if (ok || !g_enc_fmt_fail || c == 0) break;
        }
        if (!ok)
        {
            char l[500]; snprintf(l, sizeof l, "Recording could not start: %s.", why);
            set_msg(l, "REC FAILED - SEE LOG", 4000);
            char d[600]; snprintf(d, sizeof d, "[MGPU][VR1] %s", l);
            mgpu::diag::warn(d);
            g_rec.store(0);
            return;
        }
        if (g_nominal_fps != want || g_codec != asked_codec)
        {
            g_rec_pace.store((int)g_nominal_fps);
            char l[300];
            snprintf(l, sizeof l, "Recording at %u fps in %s: the encoder refused %ux%u at %u fps%s.", g_nominal_fps,
                     g_codec == 1 ? "H.265" : "H.264", g_rw, g_rh, want,
                     g_codec != asked_codec ? " in H.265" : "");
            char h[40]; snprintf(h, sizeof h, "REC %u FPS", g_nominal_fps);
            set_msg(l, h, 4000);
        }
        const LONGLONG t0 = qpc_now();
        g_enc.t0_100 = to_100ns(t0);
        g_rec_t0.store(t0);
        g_rec_frames.store(0); g_rec_dropped.store(0);
        char fn[300]; narrow(file_part(g_enc.path), fn, sizeof fn);
        char d[600];
        snprintf(d, sizeof d, "[MGPU][VR1] recording %s: %ux%u %s at a nominal %u fps on the neural card, %s, "
                 "limit %d s (0 = until stopped).", fn, g_rw, g_rh, g_codec == 1 ? "H.265 (HEVC)" : "H.264", g_nominal_fps,
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
// BM1 benchmark, BM3 sweep and odd/even pacing
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
// BM3: the sweep's countdown and length for the next run (-1 = the panel's).
std::atomic<int> g_bm_dly_ovr{-1}, g_bm_len_ovr{-1};
int bm_delay_s() { const int o = g_bm_dly_ovr.load(); return o >= 0 ? o : get(S_BENCH_DLY); }
int bm_len_s()   { const int o = g_bm_len_ovr.load(); return o > 0 ? o : get(S_BENCH_SEC); }

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
    switch (m) { case 0: return "off"; case 1: return "built-in"; case 2: return "motion view";
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
        char mult[24] = "";   // BM3: AMD's multiplier too
        if (fg == 4) snprintf(mult, sizeof mult, " %dx", mgpu::gpu1::ui_fg_mult());
        if (fg == 3) snprintf(mult, sizeof mult, " %dx", mgpu::gpu1::ui_fsr_mult());
        char lim[64];         // BM3: the Limiter (the sweep names its own)
        if (mgpu::gpu1::ui_bs_active()) snprintf(lim, sizeof lim, "sweep");
        else if (mgpu::gpu1::ui_tf_cap_on()) snprintf(lim, sizeof lim, "Limiter %d fps", mgpu::gpu1::ui_tf_cap());
        else snprintf(lim, sizeof lim, "Limiter off%s", mgpu::gpu1::ui_tf_on() ? ", target frame rate on" : "");
        snprintf(settings, n,
                 "FrameGen=%d (%s)%s | %s | Neural=%s Passes=%u Intensity=%.2f ColorStrength=%.2f | %s%s | %s%s",
                 fg, fg_name(fg), mult, lim,
                 st.neural ? "on" : "off", st.passes, st.intensity[0], mgpu::gpu1::ui_color_strength(), res,
                 mgpu::gpu1::ui_dr_on() ? " (dynamic)" : "",
                 mgpu::gpu1::ui_present_vsync() ? "bridge vsync on" : "bridge vsync off", fw);
    }
    return s;
}

double pct(const std::vector<double> &v, double p)   // v sorted ascending
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

// ---- what the worker samples beside the frame times while a run measures ----
struct bm_side
{
    double dr_sum = 0.0; unsigned dr_n = 0;       // dynamic neural resolution, %
    double busy_sum = 0.0; unsigned busy_n = 0;   // neural card busy (GPU timestamps), %
    bool nv = false; float pw = -1.0f, util = -1.0f, clk = -1.0f; int temp = -1;   // NVML, neural card
};
bm_side g_side;          // worker thread
int g_side_prev = 0;
ULONGLONG g_side_t = 0;

void side_tick()
{
    const int st = g_bm.load();
    const ULONGLONG now = GetTickCount64();
    if (st == 1 || st == 2)
    {
        if (now - g_side_t >= 250ull)
        {
            g_side_t = now;
            mgpu::gpu1::ui_nvml_keepalive();   // warm during the countdown too
            if (st == 2)
            {
                const float d = mgpu::gpu1::ui_dr_now();
                if (d > 0.0f) { g_side.dr_sum += d; ++g_side.dr_n; }
                const float b = mgpu::gpu1::ui_dr_busy();
                if (b >= 0.0f) { g_side.busy_sum += b; ++g_side.busy_n; }
            }
        }
    }
    if (st == 2 && g_side_prev != 2) { g_side = bm_side{}; mgpu::gpu1::ui_bs_nvx_reset(); }
    if (st == 3 && g_side_prev == 2)
        g_side.nv = mgpu::gpu1::ui_bs_nvx_take(&g_side.pw, &g_side.util, &g_side.clk, &g_side.temp);
    g_side_prev = st;
}

// ---- one run (or several merged, for the sweep) as interval series ----
// Odd/even pacing: frame generation shows real and generated frames in turn.
// The average spacing can be perfect while every second gap is short and every
// other one long (6/10/6/10 ms instead of 8/8/8/8) - the eye sees that as judder.
// Three measures of it:
//   odd/even  - per 120-gap window, |average of the odd gaps - average of the even
//               gaps|, averaged over the windows (windows, so a dropped frame
//               that flips the parity cannot cancel the pattern out);
//   slots     - the average gap BEFORE each frame of a group, by its place: the
//               real frame (slot 0) and generated frame 1, 2, 3...; spread =
//               (longest - shortest slot) / average gap;
//   jitter    - the average change from one gap to the next.
struct bm_series
{
    std::vector<double> ft;           // shown-frame gaps, ms
    std::vector<unsigned char> ph;    // slot of the frame ending each gap (255 = before the first real frame)
    std::vector<double> ftr;          // real-to-real gaps, ms
    std::vector<double> lat;          // added latency, ms
    std::vector<double> oe;           // odd/even difference per window, ms
    double jit_sum = 0.0; size_t jit_n = 0;
    double dur = 0.0; size_t shown = 0, gen_n = 0;
    double game_frames = 0.0, nr_frames = 0.0;
    double dr_sum = 0.0; unsigned dr_n = 0;
    double busy_sum = 0.0; unsigned busy_n = 0;
    double pw_w = 0.0, util_w = 0.0, clk_w = 0.0, nv_t = 0.0; int temp = -1;   // NVML, weighted by duration
    unsigned runs = 0;
};

bool make_series(const bm_job &j, const bm_side &sd, bm_series &s)
{
    const double fq = (double)qpc_freq();
    const size_t n = j.t.size();
    if (n < 10) return false;
    s = bm_series{};
    s.runs = 1;
    s.dur = (double)(j.t[n - 1] - j.t[0]) / fq;
    s.shown = n;
    s.ft.reserve(n); s.ph.reserve(n);
    unsigned slot = 255;
    LONGLONG last_real = -1;
    for (size_t i = 0; i < n; ++i)
    {
        if (j.gen[i]) { ++s.gen_n; if (slot != 255 && slot < 254) ++slot; }
        else
        {
            slot = 0;
            if (last_real >= 0) s.ftr.push_back((double)(j.t[i] - last_real) * 1000.0 / fq);
            last_real = j.t[i];
        }
        if (i == 0) continue;
        s.ft.push_back((double)(j.t[i] - j.t[i - 1]) * 1000.0 / fq);
        s.ph.push_back((unsigned char)slot);
    }
    for (size_t i = 1; i < s.ft.size(); ++i) { s.jit_sum += std::fabs(s.ft[i] - s.ft[i - 1]); ++s.jit_n; }
    const size_t W = 120;
    for (size_t w = 0; w + W <= s.ft.size(); w += W)
    {
        double o = 0.0, e = 0.0;
        for (size_t k = 0; k < W; k += 2) { e += s.ft[w + k]; o += s.ft[w + k + 1]; }
        s.oe.push_back(std::fabs(o - e) / (double)(W / 2));
    }
    s.lat.assign(j.lat.begin(), j.lat.end());
    s.game_frames = (double)(j.b.produced - j.a.produced);
    s.nr_frames = (double)(j.b.consumed - j.a.consumed);
    s.dr_sum = sd.dr_sum; s.dr_n = sd.dr_n;
    s.busy_sum = sd.busy_sum; s.busy_n = sd.busy_n;
    if (sd.nv)
    {
        s.nv_t = s.dur;
        s.pw_w = sd.pw >= 0.0f ? sd.pw * s.dur : -1.0;
        s.util_w = sd.util * s.dur; s.clk_w = sd.clk * s.dur; s.temp = sd.temp;
    }
    return true;
}

void series_add(bm_series &d, const bm_series &s)
{
    d.ft.insert(d.ft.end(), s.ft.begin(), s.ft.end());
    d.ph.insert(d.ph.end(), s.ph.begin(), s.ph.end());
    d.ftr.insert(d.ftr.end(), s.ftr.begin(), s.ftr.end());
    d.lat.insert(d.lat.end(), s.lat.begin(), s.lat.end());
    d.oe.insert(d.oe.end(), s.oe.begin(), s.oe.end());
    d.jit_sum += s.jit_sum; d.jit_n += s.jit_n;
    d.dur += s.dur; d.shown += s.shown; d.gen_n += s.gen_n;
    d.game_frames += s.game_frames; d.nr_frames += s.nr_frames;
    d.dr_sum += s.dr_sum; d.dr_n += s.dr_n; d.busy_sum += s.busy_sum; d.busy_n += s.busy_n;
    if (s.nv_t > 0.0)
    {
        if (s.pw_w >= 0.0 && d.pw_w >= 0.0) d.pw_w += s.pw_w; else d.pw_w = -1.0;
        d.util_w += s.util_w; d.clk_w += s.clk_w; d.nv_t += s.nv_t;
        if (s.temp > d.temp) d.temp = s.temp;
    }
    d.runs += s.runs;
}

struct bm_stats
{
    double dur = 0, avg = 0, l1 = 0, l01 = 0, p50 = 0, p95 = 0, p99 = 0, mx = 0;
    size_t stut = 0, shown = 0, gen_n = 0;
    double real_fps = 0, rmean = 0, rsd = 0, rp99 = 0;
    bool has_lat = false; double lat_avg = 0, lat50 = 0, lat95 = 0, lat99 = 0;
    double game_fps = 0, nr_fps = 0, ran_as = 0;
    double ft_mean = 0, oe_ms = 0, oe_pct = 0, jit_ms = 0, jit_pct = 0, spread_pct = 0;
    int slots = 0; double slot_ms[8] = {}; size_t slot_n[8] = {};
    double dr = -1, busy = -1, pw = -1, util = -1, clk = -1; int temp = -1;
};

bm_stats compute(const bm_series &s)
{
    bm_stats r;
    r.dur = s.dur; r.shown = s.shown; r.gen_n = s.gen_n;
    if (s.ft.empty() || s.dur <= 0.0) return r;
    std::vector<double> asc = s.ft, desc = s.ft;
    std::sort(asc.begin(), asc.end());
    std::sort(desc.begin(), desc.end(), [](double a, double b) { return a > b; });
    double sum = 0.0;
    for (double v : s.ft) sum += v;
    r.ft_mean = sum / (double)s.ft.size();
    r.avg = r.ft_mean > 0.0 ? 1000.0 / r.ft_mean : 0.0;
    r.p50 = pct(asc, 0.50); r.p95 = pct(asc, 0.95); r.p99 = pct(asc, 0.99); r.mx = asc.back();
    r.l1 = low_fps(desc, 0.01); r.l01 = low_fps(desc, 0.001);
    for (double v : s.ft) if (v > 2.5 * r.p50) ++r.stut;
    if (!s.ftr.empty())
    {
        std::vector<double> ra = s.ftr;
        std::sort(ra.begin(), ra.end());
        for (double v : s.ftr) r.rmean += v;
        r.rmean /= (double)s.ftr.size();
        for (double v : s.ftr) r.rsd += (v - r.rmean) * (v - r.rmean);
        if (s.ftr.size() > 1) r.rsd = std::sqrt(r.rsd / (double)(s.ftr.size() - 1));
        r.rp99 = pct(ra, 0.99);
        r.real_fps = r.rmean > 0.0 ? 1000.0 / r.rmean : 0.0;
    }
    if (!s.lat.empty())
    {
        std::vector<double> l = s.lat;
        std::sort(l.begin(), l.end());
        for (double v : l) r.lat_avg += v;
        r.lat_avg /= (double)l.size();
        r.lat50 = pct(l, 0.50); r.lat95 = pct(l, 0.95); r.lat99 = pct(l, 0.99);
        r.has_lat = true;
    }
    r.game_fps = s.game_frames / s.dur;
    r.nr_fps = s.nr_frames / s.dur;
    r.ran_as = (r.real_fps > 0.0) ? r.avg / r.real_fps : 0.0;
    // odd/even, slots, jitter
    if (!s.oe.empty()) { for (double v : s.oe) r.oe_ms += v; r.oe_ms /= (double)s.oe.size(); }
    r.oe_pct = r.ft_mean > 0.0 ? 100.0 * r.oe_ms / r.ft_mean : 0.0;
    r.jit_ms = s.jit_n ? s.jit_sum / (double)s.jit_n : 0.0;
    r.jit_pct = r.ft_mean > 0.0 ? 100.0 * r.jit_ms / r.ft_mean : 0.0;
    double ssum[8] = {};
    for (size_t i = 0; i < s.ft.size(); ++i)
    {
        const unsigned k = s.ph[i];
        if (k >= 8u) continue;
        ssum[k] += s.ft[i]; ++r.slot_n[k];
    }
    size_t most = 0;
    for (int k = 0; k < 8; ++k) if (r.slot_n[k] > most) most = r.slot_n[k];
    double lo = 1e9, hi = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        if (r.slot_n[k] == 0) continue;
        r.slot_ms[k] = ssum[k] / (double)r.slot_n[k];
        if (r.slot_n[k] * 4 < most) continue;   // a slot that is rarely reached (a dynamic multiplier)
        r.slots = k + 1;
        if (r.slot_ms[k] < lo) lo = r.slot_ms[k];
        if (r.slot_ms[k] > hi) hi = r.slot_ms[k];
    }
    if (hi > 0.0 && r.ft_mean > 0.0) r.spread_pct = 100.0 * (hi - lo) / r.ft_mean;
    if (s.dr_n) r.dr = s.dr_sum / s.dr_n;
    if (s.busy_n) r.busy = s.busy_sum / s.busy_n;
    if (s.nv_t > 0.0)
    {
        if (s.pw_w >= 0.0) r.pw = s.pw_w / s.nv_t;
        r.util = s.util_w / s.nv_t; r.clk = s.clk_w / s.nv_t; r.temp = s.temp;
    }
    return r;
}

void slots_text(const bm_stats &r, char *out, size_t n)
{
    out[0] = 0;
    size_t w = 0;
    for (int k = 0; k < r.slots && k < 8; ++k)
    {
        if (r.slot_n[k] == 0) continue;
        char one[32];
        if (k == 0) snprintf(one, sizeof one, "real %.2f", r.slot_ms[k]);
        else        snprintf(one, sizeof one, "gen%d %.2f", k, r.slot_ms[k]);
        const int u = snprintf(out + w, n - w, "%s%s", w ? ", " : "", one);
        if (u < 0 || (size_t)u >= n - w) break;
        w += (size_t)u;
    }
}

void fig(char *out, size_t n, double v, const char *fmt) { if (v < 0.0) snprintf(out, n, "-"); else snprintf(out, n, fmt, v); }

// results.csv: one line per run. BM3 added the pacing and card columns; an
// older file is kept as results-before-BM3.csv and a new one started.
void results_append(const char *when, const char *run, const bm_stats &r, const char *settings)
{
    wchar_t res[MAX_PATH * 2] = {};
    if (!base_folder(res, MAX_PATH * 2)) return;
    wcscat_s(res, MAX_PATH * 2, L"benchmarks");
    (void)CreateDirectoryW(res, nullptr);
    wcscat_s(res, MAX_PATH * 2, L"\\results.csv");
    FILE *f = nullptr;
    bool header = (GetFileAttributesW(res) == INVALID_FILE_ATTRIBUTES);
    if (!header && _wfopen_s(&f, res, L"r") == 0 && f)
    {
        char first[1200] = {};
        if (fgets(first, sizeof first, f) == nullptr) first[0] = 0;
        fclose(f); f = nullptr;
        if (strstr(first, "oddeven_ms") == nullptr)
        {
            wchar_t old[MAX_PATH * 2];
            wcscpy_s(old, MAX_PATH * 2, res);
            wchar_t *dot = wcsrchr(old, L'.');
            if (dot) wcscpy_s(dot, (size_t)(MAX_PATH * 2 - (dot - old)), L"-before-BM3.csv");
            if (MoveFileExW(res, old, MOVEFILE_REPLACE_EXISTING)) header = true;
        }
    }
    if (_wfopen_s(&f, res, L"a") != 0 || !f) return;
    if (header)
        fprintf(f, "date,run,duration_s,avg_fps,1pct_low_fps,0p1pct_low_fps,ft_median_ms,ft_p95_ms,ft_p99_ms,ft_max_ms,"
                   "stutters,real_fps,real_pacing_sd_ms,ran_as_x,oddeven_ms,oddeven_pct,slot_spread_pct,jitter_ms,"
                   "latency_avg_ms,latency_p95_ms,latency_p99_ms,game_fps,neural_fps,generated_share,"
                   "nr_scale_pct,neural_power_pct,neural_busy_pct,settings\n");
    char dr[16], pw[16], bz[16];
    fig(dr, sizeof dr, r.dr, "%.0f"); fig(pw, sizeof pw, r.pw, "%.0f");
    fig(bz, sizeof bz, r.busy >= 0.0 ? r.busy : r.util, "%.0f");
    fprintf(f, "%s,%s,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%zu,%.1f,%.2f,%.2f,%.2f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,"
               "%s,%s,%s,\"%s\"\n",
            when, run, r.dur, r.avg, r.l1, r.l01, r.p50, r.p95, r.p99, r.mx, r.stut, r.real_fps, r.rsd, r.ran_as,
            r.oe_ms, r.oe_pct, r.spread_pct, r.jit_ms, r.lat_avg, r.lat95, r.lat99, r.game_fps, r.nr_fps,
            r.shown ? (double)r.gen_n / (double)r.shown : 0.0, dr, pw, bz, settings);
    fclose(f);
}

void now_text(char *when, size_t n)
{
    SYSTEMTIME tm{}; GetLocalTime(&tm);
    snprintf(when, n, "%04u-%02u-%02u %02u:%02u:%02u", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
}

void pacing_text(FILE *f, const bm_stats &r)
{
    char sl[200];
    slots_text(r, sl, sizeof sl);
    fprintf(f, "ODD/EVEN PACING (are the gaps between shown frames even?)\n");
    fprintf(f, "  Odd/even          %.2f ms (%.1f%% of the average gap) - 0 = every other gap is the same\n", r.oe_ms, r.oe_pct);
    fprintf(f, "  Gap by slot       %s ms (gap BEFORE the real frame and before each generated one)\n", sl[0] ? sl : "-");
    fprintf(f, "  Slot spread       %.1f%% (longest slot - shortest slot, of the average gap)\n", r.spread_pct);
    fprintf(f, "  Jitter            %.2f ms (%.1f%%) average change from one gap to the next\n", r.jit_ms, r.jit_pct);
    fprintf(f, "  Even pacing: odd/even and spread under ~10%%. Over ~25%% is a short-long judder you can see.\n\n");
}

bool g_sw_step_run = false;   // worker: the run being written is a sweep step
bool g_sw_discard = false;    // worker: the sweep was stopped part-way through this step
void sweep_result(const bm_series *s, const bm_stats *r, bool replay_aborted);   // below

void bench_write(bm_job &j)
{
    bm_series s;
    if (!make_series(j, g_side, s))
    {
        set_msg("Benchmark too short - no result.", "BENCH TOO SHORT", 4000);
        if (g_sw_step_run) sweep_result(nullptr, nullptr, j.replay && j.replay_aborted);
        return;
    }
    const bm_stats r = compute(s);
    const char *mode = !j.replay ? "manual" : j.replay_aborted ? "input replay (STOPPED EARLY)" : "input replay";
    char when[40];
    now_text(when, sizeof when);

    if (g_sw_step_run)   // BM3: a sweep step - one results.csv line, the sweep writes the rest
    {
        if (!g_sw_discard)
            results_append(when, j.replay ? (j.replay_aborted ? "sweep-replay-stopped" : "sweep-replay") : "sweep", r, j.settings);
        sweep_result(&s, &r, j.replay && j.replay_aborted);
        return;
    }

    const double fq = (double)qpc_freq();
    const size_t n = j.t.size();
    wchar_t csv[MAX_PATH * 2] = {}, txt[MAX_PATH * 2] = {};
    if (!make_file(csv, MAX_PATH * 2, L"benchmarks", L"MGPU_bench", L".csv")) return;
    wcscpy_s(txt, MAX_PATH * 2, csv);
    wchar_t *dot = wcsrchr(txt, L'.');
    if (dot) wcscpy_s(dot, (size_t)(MAX_PATH * 2 - (dot - txt)), L".txt");

    FILE *f = nullptr;
    if (_wfopen_s(&f, csv, L"w") == 0 && f)
    {
        fprintf(f, "frame,time_ms,frametime_ms,generated,slot\n");
        for (size_t i = 0; i < n; ++i)
            fprintf(f, "%zu,%.3f,%.3f,%u,%d\n", i, (double)(j.t[i] - j.t[0]) * 1000.0 / fq,
                    i ? s.ft[i - 1] : 0.0, (unsigned)j.gen[i], i && s.ph[i - 1] != 255 ? (int)s.ph[i - 1] : -1);
        fclose(f);
    }
    if (_wfopen_s(&f, txt, L"w") == 0 && f)
    {
        fprintf(f, "Atomic MGPU Bridge benchmark - %s\n\nSettings: %s\n\n", when, j.settings);
        fprintf(f, "Duration            %.1f s\nFrames shown        %zu (%zu generated, %zu real)\n\n", r.dur, r.shown, r.gen_n, r.shown - r.gen_n);
        fprintf(f, "SHOWN FRAMES (what reached the screen)\n");
        fprintf(f, "  Average           %.1f fps\n  1%% low            %.1f fps\n  0.1%% low          %.1f fps\n", r.avg, r.l1, r.l01);
        fprintf(f, "  Frame time        median %.2f ms, 95th %.2f ms, 99th %.2f ms, max %.2f ms\n", r.p50, r.p95, r.p99, r.mx);
        fprintf(f, "  Stutters          %zu frames over 2.5x the median\n\n", r.stut);
        pacing_text(f, r);
        fprintf(f, "REAL FRAMES (neural output, generated frames excluded)\n");
        fprintf(f, "  Average           %.1f fps (shown = %.2fx real)\n  Pacing            mean %.2f ms, std dev %.2f ms, 99th %.2f ms\n\n",
                r.real_fps, r.ran_as, r.rmean, r.rsd, r.rp99);
        fprintf(f, "LATENCY ADDED BY THE MOD (game hands over its frame -> neural frame on screen)\n");
        if (!r.has_lat) fprintf(f, "  not measured (no real frames carried a hand-over time)\n\n");
        else fprintf(f, "  Average           %.1f ms\n  Median            %.1f ms\n  95th / 99th       %.1f / %.1f ms\n"
                        "  Includes transport, neural rendering, upscaling and frame generation's hold-back.\n"
                        "  Not included: the game's own input-to-render time and the monitor.\n\n",
                     r.lat_avg, r.lat50, r.lat95, r.lat99);
        fprintf(f, "GAME / NEURAL CARD\n  Game frames       %.1f fps\n  Neural card took  %.1f fps\n", r.game_fps, r.nr_fps);
        if (r.dr >= 0.0)   fprintf(f, "  Neural resolution %.0f%% average (dynamic)\n", r.dr);
        if (r.busy >= 0.0) fprintf(f, "  Neural card busy  %.0f%% (GPU timestamps)\n", r.busy);
        if (r.util >= 0.0) fprintf(f, "  Neural card       driver-busy %.0f%%, power %s%% of its limit, %.0f MHz, %d C max\n",
                                   r.util, r.pw >= 0.0 ? std::to_string((int)(r.pw + 0.5)).c_str() : "-", r.clk, r.temp);
        fprintf(f, "\nRun: %s\n", mode);
        fprintf(f, "\nFor repeatable results: same save, same spot, same camera move, same length.\n");
        fclose(f);
    }
    results_append(when, !j.replay ? "manual" : j.replay_aborted ? "replay-stopped" : "replay", r, j.settings);
    char l[800], hud[64], fn[300];
    narrow(file_part(txt), fn, sizeof fn);
    snprintf(l, sizeof l, "Benchmark %.0f s (%s): %.1f fps avg, 1%% low %.1f, 0.1%% low %.1f, real %.1f fps (pacing sd %.2f ms), "
             "odd/even %.2f ms (%.0f%%), slot spread %.0f%%, jitter %.2f ms, "
             "added latency %.1f ms avg / %.1f ms 99th, game %.1f fps. Saved %s and added to results.csv.",
             r.dur, mode, r.avg, r.l1, r.l01, r.real_fps, r.rsd, r.oe_ms, r.oe_pct, r.spread_pct, r.jit_ms,
             r.lat_avg, r.lat99, r.game_fps, fn);
    snprintf(hud, sizeof hud, "BENCH %.1f FPS 1PCT %.1f LAT %.1f MS", r.avg, r.l1, r.lat_avg);
    set_msg(l, hud, 20000);
    char d[1500]; snprintf(d, sizeof d, "[MGPU][BM1] %s Settings: %s", l, j.settings);
    mgpu::diag::info(d);
}

// ===========================================================================
// BM3 sweep: the benchmark once per configuration, round after round
// ===========================================================================
// Interleaved on purpose: round 1 runs every configuration once, then round 2
// again... so in a fly-through (a built-in benchmark) or while playing, every
// configuration samples the same stretch of the scene, and a scene change
// cannot favour whichever one happened to run then. With an input path
// (BenchReplay=1) each step replays the path - record one that ends where it
// starts (pan left, then back right).
// BM5: every step is kept on its own; a configuration's score is the MEDIAN of
// its clean steps, so one scene cut or a step that ran past the end of a
// fly-through cannot drag a whole row. A step is flagged (and left out of the
// median) when it hitched: over 30 stutters a minute, a frame gap over 100 ms,
// or the multiplier not running (shown / real under 95% of it).
struct sw_step { int round = 0; bm_stats r; bool flagged = false; char why[64] = {}; };
// NB2: nr - 0 = the sweep-wide rule (BenchSweepNR / BenchSweepNRStart), 1 = hold
// at nr_pct, 2 = dynamic with NR time budget `budget` ms (0 = no budget).
struct sw_cfg { int gen = 4, cap = 40, mult = 2; int nr = 0, nr_pct = 0, budget = 0; char name[32] = {}; bm_series acc;
                std::vector<sw_step> steps; };
std::vector<sw_cfg> g_sw_cfg;              // worker thread
std::vector<std::string> g_sw_lines;       // worker thread: one line per step
std::atomic<int> g_sw_req{0};              // any thread -> worker: 1 start, 2 stop
std::atomic<int> g_sw{0};                  // 0 idle, 1 running
std::atomic<int> g_sw_step{0}, g_sw_steps{0};
char g_sw_label[40] = {};                  // g_mx: "AMD 40X4" for the HUD
int  g_sw_rounds = 1, g_sw_settle = 5, g_sw_dwell = 20, g_sw_nr = 0;
bool g_sw_in_step = false, g_sw_stop = false, g_sw_replay = false;
// NB3: NR / FG-stage GPU totals when the step's measurement began (exact step means).
bool g_sw_nb_snap = false;
unsigned long long g_sw_nb0[4] = {};
char g_sw_why[160] = {};
ULONGLONG g_sw_step_t = 0, g_sw_t0 = 0;
char g_sw_start[40] = {}, g_sw_base[600] = {};
wchar_t g_sw_file[MAX_PATH * 2] = {};      // BM5: the summary, rewritten after every step

setting S_SW_SEC    { "BenchSweepSeconds", 20, 5, 300, {-1} };
setting S_SW_SETTLE { "BenchSweepSettle",   5, 2, 30,  {-1} };
setting S_SW_ROUNDS { "BenchSweepRounds",   2, 1, 10,  {-1} };
setting S_SW_NR     { "BenchSweepNR",       0, 0, 90,  {-1} };   // BM5: hold neural resolution at this % (0 = dynamic as set)
// BM6: with it dynamic, where each step STARTS: -1 = the scale the sweep began at
// (default), 33-90 = that %, 0 = whatever the step before left (BM3-BM5).
setting S_SW_NRSTART{ "BenchSweepNRStart", -1, -1, 90, {-1} };
int g_sw_nr0 = 0;   // the scale when the sweep began (%), for BenchSweepNRStart=-1
int g_sw_nrstart = 0;   // the common start in force for this sweep (%), 0 = none
const char *SW_DEFAULT = "amd:40x4,amd:50x3,nv:40x4,nv:50x3";

// BM5: "BenchSweep=" pasted into the value (BenchSweep=BenchSweep=amd:...) is dropped.
const char *sw_skip_key(const char *p)
{
    while (*p == ' ' || *p == '\t') ++p;
    while (_strnicmp(p, "BenchSweep=", 11) == 0) p += 11;
    return p;
}

bool sw_parse(const char *list, std::vector<sw_cfg> &out, char *bad, size_t bn)
{
    out.clear();
    bad[0] = 0;
    char tok[48];
    const char *p = sw_skip_key(list);
    while (*p != 0 && out.size() < 12)
    {
        size_t k = 0;
        while (*p != 0 && *p != ',' && k + 1 < sizeof tok) tok[k++] = *p++;
        tok[k] = 0;
        while (*p != 0 && *p != ',') ++p;
        if (*p == ',') ++p;
        if (k == 0) continue;
        for (size_t i = 0; i < k; ++i) tok[i] = (char)tolower((unsigned char)tok[i]);
        sw_cfg c;
        char g[16] = {};
        int cap = 0, mult = 0;
        const char *colon = strchr(tok, ':');
        bool ok = colon != nullptr && (size_t)(colon - tok) < sizeof g;
        if (ok)
        {
            memcpy(g, tok, (size_t)(colon - tok));
            ok = sscanf(colon + 1, "%dx%d", &cap, &mult) == 2 || sscanf(colon + 1, "%d*%d", &cap, &mult) == 2;
        }
        if (ok)
        {
            if (!strcmp(g, "amd") || !strcmp(g, "fsr") || !strcmp(g, "fsr3")) c.gen = 3;
            else if (!strcmp(g, "nv") || !strcmp(g, "nvidia") || !strcmp(g, "dlss") || !strcmp(g, "dlssg")) c.gen = 4;
            else ok = false;
        }
        if (ok) ok = cap >= 20 && cap <= 240 && mult >= 2 && mult <= (c.gen == 3 ? 4 : 6);
        // NB2: optional neural resolution per configuration: @60 holds 60%,
        // @dyn runs dynamic resolution, @b8 dynamic with an 8 ms NR budget.
        if (ok)
        {
            const char *at = strchr(tok, '@');
            if (at != nullptr)
            {
                int v = 0;
                if (!strcmp(at + 1, "dyn")) { c.nr = 2; c.budget = 0; }
                else if (at[1] == 'b' && sscanf(at + 2, "%d", &v) == 1 && v >= 1 && v <= 50) { c.nr = 2; c.budget = v; }
                else if (sscanf(at + 1, "%d", &v) == 1 && v >= 33 && v <= 90) { c.nr = 1; c.nr_pct = v; }
                else ok = false;
            }
        }
        if (!ok)
        {
            const size_t u = strlen(bad);
            if (u + k + 3 < bn) snprintf(bad + u, bn - u, "%s%s", u ? ", " : "", tok);
            continue;
        }
        c.cap = cap; c.mult = mult;
        char nrs[16] = "";
        if (c.nr == 1) snprintf(nrs, sizeof nrs, " @%d%%", c.nr_pct);
        else if (c.nr == 2 && c.budget > 0) snprintf(nrs, sizeof nrs, " b%dms", c.budget);
        else if (c.nr == 2) snprintf(nrs, sizeof nrs, " dyn");
        snprintf(c.name, sizeof c.name, "%s %dx%d%s", c.gen == 3 ? "AMD" : "NVIDIA", cap, mult, nrs);
        out.push_back(c);
    }
    return !out.empty();
}

void sw_label(const sw_cfg &c)
{
    std::lock_guard<std::mutex> lk(g_mx);
    char nrs[16] = "";   // NB2
    if (c.nr == 1) snprintf(nrs, sizeof nrs, " @%d%%", c.nr_pct);
    else if (c.nr == 2 && c.budget > 0) snprintf(nrs, sizeof nrs, " B%dMS", c.budget);
    else if (c.nr == 2) snprintf(nrs, sizeof nrs, " DYN");
    snprintf(g_sw_label, sizeof g_sw_label, "%s %dX%d%s", c.gen == 3 ? "AMD" : "NV", c.cap, c.mult, nrs);
}

void sweep_start()
{
    if (g_bm.load() != 0) { set_msg("Sweep not started: a benchmark is running.", "SWEEP NOT STARTED", 3000); return; }
    char list[400] = {};
    mgpu::gpu1::ui_ini_read_word("BenchSweep", list, sizeof list);
    if (list[0] == 0) snprintf(list, sizeof list, "%s", SW_DEFAULT);
    char bad[200];
    std::vector<sw_cfg> cfg;
    const bool any = sw_parse(list, cfg, bad, sizeof bad);
    if (bad[0])
    {
        char l[400]; snprintf(l, sizeof l, "[MGPU][BM3] BenchSweep: skipped what it could not read: %s "
                              "(write gen:CAPxMULT, e.g. amd:40x4,nv:50x3; AMD 2-4x, NVIDIA 2-6x, cap 20-240; "
                              "optional @60 / @dyn / @b8 for neural resolution)", bad);
        mgpu::diag::warn(l);
    }
    if (!any) { set_msg("Sweep not started: BenchSweep in mgpu.ini lists no configuration it can read.", "SWEEP NOT STARTED", 4000); return; }
    for (const sw_cfg &c : cfg)
        if (c.gen == 3 && c.mult == 3 && !mgpu::gpu1::ui_fsr_rebuilt())
            mgpu::diag::warn("[MGPU][BM3] AMD 3x needs the rebuilt AMD DLL - that step will show what actually ran (see \"ran as\").");
    g_sw_cfg.swap(cfg);
    g_sw_lines.clear();
    g_sw_rounds = get(S_SW_ROUNDS);
    g_sw_settle = get(S_SW_SETTLE);
    g_sw_dwell  = get(S_SW_SEC);
    double ps = 0.0;
    g_sw_replay = get(S_BENCH_RPL) != 0 && mgpu::replay::has_path(&ps);
    g_sw_steps.store((int)g_sw_cfg.size() * g_sw_rounds);
    g_sw_step.store(0);
    g_sw_in_step = g_sw_stop = false;
    g_sw_why[0] = 0;
    g_sw_t0 = GetTickCount64();
    now_text(g_sw_start, sizeof g_sw_start);
    (void)bm_snapshot(g_sw_base, sizeof g_sw_base);   // before the first step changes anything
    if (!make_file(g_sw_file, MAX_PATH * 2, L"benchmarks", L"MGPU_sweep", L".txt")) g_sw_file[0] = 0;
    // BM5: neural resolution held for the whole sweep, so every step is at the
    // same picture quality and none inherits the scale the one before left.
    g_sw_nr = 0;
    g_sw_nr0 = (int)(mgpu::gpu1::ui_dr_now() + 0.5f);
    g_sw_nrstart = 0;
    char nrl[200] = "dynamic resolution left as set";
    const int want_nr = get(S_SW_NR);
    if (want_nr > 0)
    {
        g_sw_nr = mgpu::gpu1::ui_bs_hold_nr(want_nr);
        if (g_sw_nr > 0) snprintf(nrl, sizeof nrl, "neural resolution held at %d%% (BenchSweepNR=%d)", g_sw_nr, want_nr);
        else snprintf(nrl, sizeof nrl, "BenchSweepNR=%d not applied: dynamic resolution is off, so the fixed resolution already applies", want_nr);
    }
    else if (mgpu::gpu1::ui_dr_on() && g_sw_nr0 > 0)
    {
        // BM6: dynamic, but every step starts from the same scale.
        const int st = get(S_SW_NRSTART);
        g_sw_nrstart = st == -1 ? g_sw_nr0 : st;
        if (g_sw_nrstart > 0) snprintf(nrl, sizeof nrl, "dynamic resolution, every step starting at %d%% (BenchSweepNRStart)", g_sw_nrstart);
        else snprintf(nrl, sizeof nrl, "dynamic resolution, each step starting where the one before left it (BenchSweepNRStart=0)");
    }
    g_sw.store(1);
    const double per = (double)g_sw_settle + (g_sw_replay ? ps : (double)g_sw_dwell);
    char l[800];
    snprintf(l, sizeof l, "Sweep started: %zu configurations x %d round(s), %s, %s, about %.0f min. Press the sweep hotkey again to stop.",
             g_sw_cfg.size(), g_sw_rounds, g_sw_replay ? "input replay each step" : "timed steps", nrl,
             per * (double)g_sw_steps.load() / 60.0 + 0.5);
    set_msg(l, "SWEEP STARTED", 3000);
    char d[1400];
    snprintf(d, sizeof d, "[MGPU][BM3] %s List: %s. Settle %d s, measure %s per step. Settings before: %s",
             l, list, g_sw_settle, g_sw_replay ? "the input path" : (std::to_string(g_sw_dwell) + " s").c_str(), g_sw_base);
    mgpu::diag::info(d);
}

double med(std::vector<double> v)
{
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// BM5: one configuration's score - the median of its clean steps (all of its
// steps when every one was flagged, said so in the table).
struct sw_score
{
    const sw_cfg *c = nullptr;
    unsigned used = 0, total = 0; bool all_flagged = false;
    double shown = -1, real = -1, ran = -1, l1 = -1, p99 = -1, stutm = -1, rsd = -1, oe = -1, spread = -1, jit = -1,
           lat = -1, lat99 = -1, dr = -1, busy = -1, pw = -1;
};

sw_score score(const sw_cfg &c)
{
    sw_score s; s.c = &c; s.total = (unsigned)c.steps.size();
    std::vector<const bm_stats *> use;
    for (const sw_step &st : c.steps) if (!st.flagged) use.push_back(&st.r);
    if (use.empty()) { for (const sw_step &st : c.steps) use.push_back(&st.r); s.all_flagged = !use.empty(); }
    s.used = s.all_flagged ? 0u : (unsigned)use.size();
    if (use.empty()) return s;
    auto m = [&](auto f) { std::vector<double> v; for (const bm_stats *r : use) { const double x = f(*r); if (x >= 0.0) v.push_back(x); } return med(v); };
    s.shown = m([](const bm_stats &r) { return r.avg; });
    s.real  = m([](const bm_stats &r) { return r.real_fps; });
    s.ran   = m([](const bm_stats &r) { return r.ran_as; });
    s.l1    = m([](const bm_stats &r) { return r.l1; });
    s.p99   = m([](const bm_stats &r) { return r.p99; });
    s.stutm = m([](const bm_stats &r) { return r.dur > 0.0 ? (double)r.stut * 60.0 / r.dur : -1.0; });
    s.rsd   = m([](const bm_stats &r) { return r.rsd; });
    s.oe    = m([](const bm_stats &r) { return r.oe_pct; });
    s.spread= m([](const bm_stats &r) { return r.spread_pct; });
    s.jit   = m([](const bm_stats &r) { return r.jit_ms; });
    s.lat   = m([](const bm_stats &r) { return r.has_lat ? r.lat_avg : -1.0; });
    s.lat99 = m([](const bm_stats &r) { return r.has_lat ? r.lat99 : -1.0; });
    s.dr    = m([](const bm_stats &r) { return r.dr; });
    s.busy  = m([](const bm_stats &r) { return r.busy >= 0.0 ? r.busy : r.util; });
    s.pw    = m([](const bm_stats &r) { return r.pw; });
    return s;
}

// BM5: the summary file, rewritten after every step (final = the sweep is over).
void sweep_write(const char *state, bool final, std::vector<sw_score> &rows,
                 const sw_score **b_jit, const sw_score **b_even, const sw_score **b_l1, const sw_score **b_lat)
{
    rows.clear();
    for (const sw_cfg &c : g_sw_cfg) if (!c.steps.empty()) rows.push_back(score(c));
    auto best = [&](auto key, bool low) -> const sw_score * {
        const sw_score *b = nullptr;
        for (const sw_score &x : rows) { const double v = key(x); if (v < 0.0) continue;
            if (b == nullptr || (low ? v < key(*b) : v > key(*b))) b = &x; }
        return b;
    };
    *b_jit  = best([](const sw_score &s) { return s.jit; }, true);
    *b_even = best([](const sw_score &s) { return s.oe >= 0.0 && s.spread >= 0.0 ? s.oe + s.spread : -1.0; }, true);
    *b_l1   = best([](const sw_score &s) { return s.l1; }, false);
    *b_lat  = best([](const sw_score &s) { return s.lat; }, true);
    if (g_sw_file[0] == 0 || rows.empty()) return;

    FILE *f = nullptr;
    if (_wfopen_s(&f, g_sw_file, L"w") != 0 || !f) return;
    const double mins = (double)(GetTickCount64() - g_sw_t0) / 60000.0;
    fprintf(f, "Atomic MGPU Bridge benchmark sweep - %s\n\n", g_sw_start);
    fprintf(f, "%s: %s after %.1f min. %d of %d steps measured.\n", final ? "Ended" : "IN PROGRESS", state, mins,
            g_sw_step.load(), g_sw_steps.load());
    fprintf(f, "Each step: settle %d s (not measured), then %s. Steps are interleaved: every configuration\n"
               "once per round, so all of them sample the same parts of the scene.\n",
            g_sw_settle, g_sw_replay ? "the input path replayed" : (std::to_string(g_sw_dwell) + " s measured").c_str());
    if (g_sw_nr > 0) fprintf(f, "Neural resolution held at %d%% for the whole sweep (BenchSweepNR): every step at the same picture quality.\n", g_sw_nr);
    else if (g_sw_nrstart > 0) fprintf(f, "Neural resolution dynamic, every step starting at %d%% (BenchSweepNRStart): NR %% shows where each settled.\n", g_sw_nrstart);
    else fputs("Neural resolution: dynamic, each step starting where the one before left it (BenchSweepNR holds it, BenchSweepNRStart gives a common start).\n", f);
    fprintf(f, "Settings before the sweep (restored after it): %s\n\n", g_sw_base);
    fputs("Each figure is the MEDIAN of the configuration's clean steps. A step is flagged and left out when it\n"
          "hitched (over 30 stutters a minute, a gap over 100 ms, or the multiplier not running) - a scene cut,\n"
          "a loading screen or the end of a fly-through. \"used\" says how many steps count.\n\n", f);
    fprintf(f, "%-22s %5s %7s %7s %6s %7s %7s %7s %7s %8s %7s %7s %10s %5s %6s %5s\n",
            "configuration", "used", "shown", "real", "ran", "1% low", "p99 ft", "stut/m", "real sd",
            "odd/even", "spread", "jitter", "latency", "NR %", "power", "busy");
    fprintf(f, "%-22s %5s %7s %7s %6s %7s %7s %7s %7s %8s %7s %7s %10s %5s %6s %5s\n",
            "", "", "fps", "fps", "as", "fps", "ms", "", "ms", "%", "%", "ms", "avg/p99 ms", "", "%", "%");
    for (const sw_score &x : rows)
    {
        char used[12], lat[24], dr[12], pw[12], bz[12];
        if (x.all_flagged) snprintf(used, sizeof used, "0/%u!", x.total); else snprintf(used, sizeof used, "%u/%u", x.used, x.total);
        if (x.lat >= 0.0) snprintf(lat, sizeof lat, "%.1f/%.1f", x.lat, x.lat99); else snprintf(lat, sizeof lat, "-");
        fig(dr, sizeof dr, x.dr, "%.0f"); fig(pw, sizeof pw, x.pw, "%.0f"); fig(bz, sizeof bz, x.busy, "%.0f");
        fprintf(f, "%-22s %5s %7.1f %7.1f %5.2fx %7.1f %7.2f %7.1f %7.2f %8.1f %7.1f %7.2f %10s %5s %6s %5s%s%s\n",
                x.c->name, used, x.shown, x.real, x.ran, x.l1, x.p99, x.stutm, x.rsd, x.oe, x.spread, x.jit,
                lat, dr, pw, bz,
                x.real >= 0.0 && x.real < (double)x.c->cap * 0.99 ? "  < short of the Limiter" : "  = held the Limiter",
                x.all_flagged ? " (every step flagged - shown anyway)" : "");
    }
    fputs("\nGap by slot (ms, all steps pooled; the gap before the real frame, then before each generated frame):\n", f);
    for (const sw_score &x : rows)
    {
        char sl[200]; slots_text(compute(x.c->acc), sl, sizeof sl);
        fprintf(f, "  %-22s %s\n", x.c->name, sl[0] ? sl : "-");
    }
    fputs("\n", f);
    if (*b_jit)  fprintf(f, "Smoothest (lowest jitter):        %s\n", (*b_jit)->c->name);
    if (*b_even) fprintf(f, "Most even (odd/even + spread):    %s\n", (*b_even)->c->name);
    if (*b_l1)   fprintf(f, "Best 1%% low:                      %s\n", (*b_l1)->c->name);
    if (*b_lat)  fprintf(f, "Lowest added latency:             %s\n", (*b_lat)->c->name);
    fputs("\nHow to read it:\n"
          "  held / short   whether the real frame rate reached the Limiter. Pacing is even when it is held:\n"
          "                 a card that falls even slightly short spaces its frames unevenly.\n"
          "  ran as    shown fps / real fps - should match the multiplier (4x -> about 4.00x).\n"
          "  odd/even  how different every other gap is, % of the average gap. Under ~10% is even.\n"
          "  spread    longest minus shortest slot, % of the average gap. Under ~10% is even;\n"
          "            over ~25% is a short-long pattern you can see as judder.\n"
          "  jitter    average change from one gap to the next. Lower = smoother.\n"
          "  NR %      neural resolution (- = not dynamic). Lower = the card had to cut resolution to keep up.\n"
          "  power     neural card power, % of its limit. busy = neural card busy %.\n"
          "  stut/m    frames over 2.5x the median gap, per minute.\n", f);   // fputs: the text has plain % signs
    fputs("\nSteps (* = flagged, left out of the medians):\n", f);
    for (const std::string &s : g_sw_lines) fprintf(f, "  %s\n", s.c_str());
    fclose(f);
}

void sweep_finish(const char *why)
{
    mgpu::gpu1::ui_bs_end();   // also lets go of the held resolution (BM5)
    g_bm_dly_ovr.store(-1); g_bm_len_ovr.store(-1);
    g_sw_in_step = false;
    g_sw.store(0);
    { std::lock_guard<std::mutex> lk(g_mx); g_sw_label[0] = 0; }

    std::vector<sw_score> rows;
    const sw_score *b_jit = nullptr, *b_even = nullptr, *b_l1 = nullptr, *b_lat = nullptr;
    sweep_write(why, true, rows, &b_jit, &b_even, &b_l1, &b_lat);
    if (rows.empty())
    {
        char l[300]; snprintf(l, sizeof l, "Sweep %s - no step finished, nothing to report.", why);
        set_msg(l, "SWEEP - NO RESULT", 4000);
        mgpu::diag::info((std::string("[MGPU][BM3] ") + l).c_str());
        return;
    }
    for (const sw_score &x : rows)
    {
        char l[800];
        snprintf(l, sizeof l, "[MGPU][BM5] %s (median of %u of %u step(s)%s): shown %.1f fps, real %.1f fps (%s the Limiter, ran as "
                 "%.2fx), 1%% low %.1f, p99 %.2f ms, real sd %.2f ms, odd/even %.1f%%, slot spread %.1f%%, jitter %.2f ms, "
                 "latency %.1f ms, NR %.0f%%, neural power %.0f%%, busy %.0f%%",
                 x.c->name, x.all_flagged ? x.total : x.used, x.total, x.all_flagged ? ", all flagged" : "", x.shown, x.real,
                 x.real < (double)x.c->cap * 0.99 ? "short of" : "held", x.ran, x.l1, x.p99, x.rsd, x.oe, x.spread, x.jit,
                 x.lat, x.dr, x.pw, x.busy);
        mgpu::diag::info(l);
    }
    char fn[300] = "";
    if (g_sw_file[0]) narrow(file_part(g_sw_file), fn, sizeof fn);
    char l[600], hud[64];
    snprintf(l, sizeof l, "Sweep %s: %zu configuration(s). Smoothest %s, most even %s, best 1%% low %s. Saved %s.",
             why, rows.size(), b_jit ? b_jit->c->name : "-", b_even ? b_even->c->name : "-", b_l1 ? b_l1->c->name : "-", fn);
    snprintf(hud, sizeof hud, "SWEEP DONE - SEE PANEL");
    set_msg(l, hud, 15000);
    mgpu::diag::info((std::string("[MGPU][BM3] ") + l).c_str());
}

void sweep_result(const bm_series *s, const bm_stats *r, bool replay_aborted)
{
    if (g_sw.load() != 1 || !g_sw_in_step) return;
    g_sw_in_step = false;
    const int idx = g_sw_step.load();
    if (g_sw_discard) { g_sw_discard = false; return; }   // stopped part-way: not counted
    if (replay_aborted)
    {
        g_sw_stop = true;
        snprintf(g_sw_why, sizeof g_sw_why, "stopped (the input replay was interrupted by a key or the game losing focus)");
        return;
    }
    sw_cfg &c = g_sw_cfg[(size_t)idx % g_sw_cfg.size()];
    const int round = idx / (int)g_sw_cfg.size() + 1;
    char line[500];
    if (s != nullptr && r != nullptr)
    {
        series_add(c.acc, *s);
        sw_step st; st.round = round; st.r = *r;
        const double stutm = r->dur > 0.0 ? (double)r->stut * 60.0 / r->dur : 0.0;
        if (stutm > 30.0) { st.flagged = true; snprintf(st.why, sizeof st.why, "%.0f stutters/min", stutm); }
        else if (r->mx > 100.0) { st.flagged = true; snprintf(st.why, sizeof st.why, "a %.0f ms gap", r->mx); }
        else if (r->ran_as > 0.0 && r->ran_as < (double)c.mult * 0.95) { st.flagged = true; snprintf(st.why, sizeof st.why, "ran as %.2fx", r->ran_as); }
        c.steps.push_back(st);
        // NB2: the neural card's NR and frame generation times (GPU timestamps,
        // smoothed over the last ~10 real frames of the step).
        // NB3: exact means over the measured part of the step (from the
        // running totals), falling back to the smoothed value without a start mark.
        char nb[120] = "";
        {
            unsigned long long t[4] = {};
            mgpu::gpu1::ui_nb_totals(&t[0], &t[1], &t[2], &t[3]);
            double nrm = -1.0, fgm = -1.0;
            if (g_sw_nb_snap)
            {
                if (t[1] > g_sw_nb0[1]) nrm = (double)(t[0] - g_sw_nb0[0]) / 1000.0 / (double)(t[1] - g_sw_nb0[1]);
                if (t[3] > g_sw_nb0[3]) fgm = (double)(t[2] - g_sw_nb0[2]) / 1000.0 / (double)(t[3] - g_sw_nb0[3]);
            }
            else { nrm = mgpu::gpu1::ui_nb_nr_ms(); fgm = mgpu::gpu1::ui_nb_fg_ms(); }
            if (nrm >= 0.0 || fgm >= 0.0)
                snprintf(nb, sizeof nb, ", NR GPU %.2f ms, FG stage GPU %.2f ms per real frame%s",
                         nrm >= 0.0 ? nrm : 0.0, fgm >= 0.0 ? fgm : 0.0, g_sw_nb_snap ? "" : " (smoothed)");
        }
        snprintf(line, sizeof line, "%sround %d %-22s shown %.1f, real %.1f fps, ran as %.2fx, 1%% low %.1f, odd/even %.1f%%, "
                 "spread %.1f%%, jitter %.2f ms, latency %.1f ms, NR %.0f%%%s%s%s",
                 st.flagged ? "* " : "  ", round, c.name, r->avg, r->real_fps, r->ran_as, r->l1, r->oe_pct, r->spread_pct,
                 r->jit_ms, r->lat_avg, r->dr, nb, st.flagged ? " - flagged: " : "", st.flagged ? st.why : "");
    }
    else snprintf(line, sizeof line, "  round %d %-22s too short - no result", round, c.name);
    g_sw_lines.push_back(line);
    g_sw_step.fetch_add(1);
    // BM5: the step in ReShade.log, and the summary rewritten now - a crash or
    // an early quit keeps everything measured so far.
    mgpu::diag::info((std::string("[MGPU][BM5] step ") + std::to_string(idx + 1) + "/" + std::to_string(g_sw_steps.load()) +
                      ": " + (line + 2)).c_str());
    std::vector<sw_score> rows;
    const sw_score *a = nullptr, *b = nullptr, *cc = nullptr, *d = nullptr;
    char st[80]; snprintf(st, sizeof st, "step %d of %d done", idx + 1, g_sw_steps.load());
    sweep_write(st, false, rows, &a, &b, &cc, &d);
}

void sweep_tick()
{
    const int rq = g_sw_req.exchange(0);
    if (rq == 1 && g_sw.load() == 0) sweep_start();
    else if (rq == 2 && g_sw.load() == 1 && !g_sw_stop) { g_sw_stop = true; snprintf(g_sw_why, sizeof g_sw_why, "stopped by you"); }
    if (g_sw.load() != 1) return;
    const int b = g_bm.load();
    if (g_sw_stop)
    {
        if (b == 1) { std::lock_guard<std::mutex> plk(g_pmx); g_bm.store(0); g_sw_in_step = false; }
        else if (b == 2) { g_sw_discard = true; g_bm_stop.store(true); return; }   // bench_tick reports it, discarded
        else if (b == 3) return;
        sweep_finish(g_sw_why[0] ? g_sw_why : "stopped");
        return;
    }
    if (g_sw_in_step)
    {
        // NB3: the measurement has begun (settle over) - note the GPU timing totals.
        if (b == 2 && !g_sw_nb_snap)
        {
            mgpu::gpu1::ui_nb_totals(&g_sw_nb0[0], &g_sw_nb0[1], &g_sw_nb0[2], &g_sw_nb0[3]);
            g_sw_nb_snap = true;
        }
        // A countdown that never sees a frame (the bridge is not running).
        if (b == 1 && g_bm_cd0 == 0 && GetTickCount64() - g_sw_step_t > 15000ull)
        {
            g_sw_stop = true;
            snprintf(g_sw_why, sizeof g_sw_why, "stopped (no frames - is the bridge running?)");
        }
        if (b == 0 && GetTickCount64() - g_sw_step_t > 1000ull) g_sw_in_step = false;   // cancelled from outside
        return;
    }
    if (b != 0) return;
    const int idx = g_sw_step.load();
    if (idx >= g_sw_steps.load()) { sweep_finish("done"); return; }
    const sw_cfg &c = g_sw_cfg[(size_t)idx % g_sw_cfg.size()];
    mgpu::gpu1::ui_bs_apply(c.gen, c.cap, c.mult);
    // NB2: this configuration's own neural resolution, else the sweep-wide rule.
    if (c.nr == 1)
    {
        mgpu::gpu1::ui_bs_budget(-1);
        if (mgpu::gpu1::ui_bs_hold_nr(c.nr_pct) == 0)
            mgpu::diag::warn("[MGPU][NB2] sweep step asks for a held neural resolution, but dynamic resolution is "
                             "off - the fixed SRScale applies to this step.");
    }
    else if (c.nr == 2)
    {
        (void)mgpu::gpu1::ui_bs_hold_nr(0);
        mgpu::gpu1::ui_bs_budget(c.budget);
        const int st = g_sw_nrstart > 0 ? g_sw_nrstart : g_sw_nr0;
        if (st > 0) (void)mgpu::gpu1::ui_bs_dr_start(st);
        if (!mgpu::gpu1::ui_dr_on())
            mgpu::diag::warn("[MGPU][NB2] sweep step asks for dynamic resolution, but it is off (SRDynamic=0) - "
                             "the fixed SRScale applies to this step.");
    }
    else
    {
        mgpu::gpu1::ui_bs_budget(-1);
        if (g_sw_nr > 0) (void)mgpu::gpu1::ui_bs_hold_nr(g_sw_nr);
        else (void)mgpu::gpu1::ui_bs_hold_nr(0);
        if (g_sw_nr == 0 && g_sw_nrstart > 0) (void)mgpu::gpu1::ui_bs_dr_start(g_sw_nrstart);   // BM6: a common start
    }
    sw_label(c);
    g_bm_dly_ovr.store(g_sw_settle);
    g_bm_len_ovr.store(g_sw_dwell);
    g_sw_in_step = true;
    g_sw_nb_snap = false;   // NB3
    g_sw_step_t = GetTickCount64();
    std::lock_guard<std::mutex> plk(g_pmx);
    g_bm_stop.store(false); g_bm_cd0 = 0; g_bm.store(1);
}

void bench_tick()
{
    side_tick();
    sweep_tick();
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
    g_sw_step_run = (g_sw.load() == 1 && g_sw_in_step);
    if (g_sw_step_run)   // the sweep's name for the step leads the settings
    {
        char tmp[600];
        const sw_cfg &c = g_sw_cfg[(size_t)g_sw_step.load() % g_sw_cfg.size()];
        snprintf(tmp, sizeof tmp, "SWEEP round %d %s | %s", g_sw_step.load() / (int)g_sw_cfg.size() + 1, c.name, j.settings);
        memcpy(j.settings, tmp, sizeof j.settings);
    }
    bench_write(j);
    g_sw_step_run = false;
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
    if (g_sw.load() == 1) { g_sw_req.store(2); return; }   // BM3: during a sweep it stops the sweep
    const int st = g_bm.load();
    if (st == 0) { g_bm_stop.store(false); g_bm_cd0 = 0; g_bm.store(1); }
    else if (st == 1) { g_bm.store(0); set_msg("Benchmark cancelled.", "BENCH CANCELLED", 3000); }
    else if (st == 2) g_bm_stop.store(true);
}

// BM3
void toggle_sweep()
{
    ensure_worker();
    g_sw_req.store(g_sw.load() == 1 ? 2 : 1);
}
int  sweep_state()            { return g_sw.load(); }
void sweep_progress(int *step, int *steps) { if (step) *step = g_sw_step.load(); if (steps) *steps = g_sw_steps.load(); }
int  sweep_seconds()          { return get(S_SW_SEC); }    void set_sweep_seconds(int s) { put(S_SW_SEC, s); }
int  sweep_settle()           { return get(S_SW_SETTLE); } void set_sweep_settle(int s)  { put(S_SW_SETTLE, s); }
int  sweep_rounds()           { return get(S_SW_ROUNDS); } void set_sweep_rounds(int r)  { put(S_SW_ROUNDS, r); }
void sweep_list(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    mgpu::gpu1::ui_ini_read_word("BenchSweep", out, n);
    const char *k = sw_skip_key(out);   // BM5: show it without a pasted "BenchSweep="
    if (k != out) memmove(out, k, strlen(k) + 1);
    if (out[0] == 0) snprintf(out, n, "%s", SW_DEFAULT);
}
int  sweep_nr()               { return get(S_SW_NR); }     void set_sweep_nr(int p)      { put(S_SW_NR, p); }
bool set_sweep_list(const char *list)
{
    if (list == nullptr) return false;
    std::vector<sw_cfg> tmp; char bad[200];
    char clean[400]; size_t k = 0;
    for (const char *p = sw_skip_key(list); *p && k + 1 < sizeof clean; ++p) if (*p != ' ' && *p != '\t') clean[k++] = *p;
    clean[k] = 0;
    if (!sw_parse(clean, tmp, bad, sizeof bad) || bad[0]) return false;
    return mgpu::gpu1::ui_ini_write_text("BenchSweep", clean);
}

void shutdown()
{
    if (g_sw.load() == 1) { mgpu::gpu1::ui_bs_end(); g_sw.store(0); }   // BM3: the saved settings back
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
            // LR100: the shared bounded drain (gpu_safety_d3d12.hpp). It closes
            // its event ONLY when the fence passed or the device is gone - the
            // old code closed it after a 5 s timeout too, while the driver
            // still held it registered and could set it later (a handle that
            // may by then name another object). Kept on purpose when pending.
            // It also asks the device itself (GetDeviceRemovedReason), so a
            // removal is seen before the fence reads UINT64_MAX.
            const mgpu::gpusafe::drain_r dr = mgpu::gpusafe::drain_fence_self(g_last_fence, g_last_value, 5000, 0);
            // REV6: decided here, not by fence_done(). fence_done() treats
            // UINT64_MAX as NOT done on purpose - for a screenshot it means
            // the copied pixels are invalid. For shutdown the question is
            // different: is the GPU still using these buffers? A removed
            // device reports UINT64_MAX on every fence and executes nothing
            // more, so then the answer is no and they are released normally.
            const bool removed = dr.removed;
            gpu_idle = removed || dr.drained;
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
            g_rec_pace.store(f);   // VR2: may be lowered by an encoder fallback
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
            const int f = g_rec_pace.load();   // VR2: the rate the encoder accepted
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
        // LR106: with input replay on, the countdown starts over while the
        // overlay is open - a replay that began under an open panel moved its
        // cursor (Cyberpunk), and now stops at once if the overlay is up.
        if (mgpu::replay_rules::hold_countdown(get(S_BENCH_RPL) != 0, mgpu::gpu1::ui_any_menu_open())) g_bm_cd0 = now;
        if (now - g_bm_cd0 >= (LONGLONG)bm_delay_s() * qpc_freq())
        {
            g_bm_t.clear(); g_bm_gen.clear(); g_bm_lat.clear();
            g_bm_t.reserve(200000); g_bm_gen.reserve(200000); g_bm_lat.reserve(100000);
            g_bm_len = (double)bm_len_s();
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
int  record_codec()            { return get(S_REC_CODEC); }
void set_record_codec(int c)   { put(S_REC_CODEC, c); }
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
    char sw[40] = {};   // BM3
    if (g_sw.load() == 1) { std::lock_guard<std::mutex> lk(g_mx); snprintf(sw, sizeof sw, "%s", g_sw_label); }
    if (bs == 1)
    {
        const LONGLONG cd = (g_bm_cd0 == 0) ? 0 : (now - g_bm_cd0) / qpc_freq();
        const long long left = (long long)bm_delay_s() - (long long)cd;
        if (sw[0]) snprintf(out, n, "SWEEP %d/%d %s SETTLE %lld", g_sw_step.load() + 1, g_sw_steps.load(), sw, left > 0 ? left : 0);
        else snprintf(out, n, "BENCH STARTS IN %lld", left > 0 ? left : 0);
        return;
    }
    if (bs == 2)
    {
        const LONGLONG s = (now - g_bm_t0) / qpc_freq();
        if (sw[0]) snprintf(out, n, "SWEEP %d/%d %s %lld/%.0f S", g_sw_step.load() + 1, g_sw_steps.load(), sw, (long long)s, g_bm_len);
        else snprintf(out, n, "BENCH %lld / %.0f S%s", (long long)s, g_bm_len, g_bm_replay ? " REPLAY" : "");
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
