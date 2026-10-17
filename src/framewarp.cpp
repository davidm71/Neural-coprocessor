// ---------------------------------------------------------------------------
// framewarp.cpp - FW1. See framewarp.hpp.
// ---------------------------------------------------------------------------
#include "framewarp.hpp"
#include "gpu1_context.hpp"
#include "diag.hpp"
#include "sl_tags.hpp"

#include <windows.h>
#include <d3dcompiler.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>

namespace mgpu
{
namespace framewarp
{
namespace
{

// ===========================================================================
// settings
// ===========================================================================
struct setting { const char *key; int dflt, lo, hi; std::atomic<int> v; };
setting S_ON     { "FrameWarp",         0, 0, 1,   {-1} };
setting S_SCALE  { "FrameWarpScale",   50, 1, 500, {-1} };
setting S_INVY   { "FrameWarpInvertY",  0, 0, 1,   {-1} };
setting S_LEAD   { "FrameWarpLead",     0, 0, 60,  {-1} };
setting S_FOV    { "FrameWarpFov",      0, 0, 1700, {-1} };   // FW2: tenths of a degree, 0 = not calibrated
setting S_AUTO   { "FrameWarpAuto",     1, 0, 1,   {-1} };    // FW3: keep calibrating during play
setting S_STR    { "FrameWarpStrength", 20, 0, 100, {-1} };   // FW5: % of the delay compensated
// FW5: does the camera actually follow the mouse right now? false in menus
// (the game may draw its own cursor, so the Windows cursor says nothing).
std::atomic<bool> g_camera_live{false};
// FW5: when the game's motion vectors last reached the menu check. A menu that
// stops the 3D scene (no vectors at all) must pause the warp too.
std::atomic<long long> g_last_mv_qpc{0};
std::atomic<unsigned long long> g_skip_menu{0};
std::atomic<bool> g_auto_kick{false};   // FW4: auto was switched on - start learning now
unsigned g_menu_ev = 0, g_cam_ev = 0;    // FW5: consecutive evidence (g_cmx)
LONGLONG g_follow_qpc = 0;               // FW7: last frame the picture clearly followed the mouse (g_cmx)
int g_dir = 0;                           // FW8: learned sign of (centre shift x mouse x), 0 = not yet (g_cmx)

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

// The most the camera is turned by one warp. A larger angle means the hook saw
// something that was not a camera turn (or the frame is very old); either way
// a picture slid that far would be mostly stretched edge.
const double kMaxDeg = 3.0;   // FW5: a bigger slide is mostly stretched edge

LONGLONG qpc_now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG qpc_freq() { static LONGLONG f = 0; if (f == 0) { LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); f = q.QuadPart; } return f; }

bool own_foreground()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return fg != nullptr && pid == GetCurrentProcessId();
}

// ===========================================================================
// mouse history - written by the hook thread, read at present
// ===========================================================================
// Cumulative mouse position (counts) after each move, with its QPC time. The
// warp for a frame is cum(now) - cum(content time).
struct mpt { LONGLONG t; long long x, y; };
const unsigned HIST = 8192;          // ~8 s at 1000 Hz
std::mutex g_hmx;
mpt g_hist[HIST];
unsigned g_hn = 0;                   // entries written (wraps the ring)
long long g_cx = 0, g_cy = 0;        // running totals (hook thread)
std::atomic<unsigned long long> g_moves{0}, g_moves_zero{0};

void hist_push(LONGLONG t, long long dx, long long dy)
{
    std::lock_guard<std::mutex> lk(g_hmx);
    g_cx += dx; g_cy += dy;
    g_hist[g_hn % HIST] = { t, g_cx, g_cy };
    ++g_hn;
}

// Cumulative position at time t: the last entry at or before t. false when the
// history does not reach back that far.
bool hist_at(LONGLONG t, long long &x, long long &y)
{
    std::lock_guard<std::mutex> lk(g_hmx);
    if (g_hn == 0) { x = y = 0; return true; }   // never moved: everything is 0
    const unsigned n = (g_hn < HIST) ? g_hn : HIST;
    const unsigned first = g_hn - n;             // oldest index kept
    const mpt &oldest = g_hist[first % HIST];
    if (t < oldest.t)
    {
        if (g_hn <= HIST) { x = y = 0; return true; }   // before the first move ever
        return false;                                    // fell off the ring
    }
    // binary search over [first, g_hn)
    unsigned lo = first, hi = g_hn - 1;
    while (lo < hi)
    {
        const unsigned mid = lo + (hi - lo + 1) / 2;
        if (g_hist[mid % HIST].t <= t) lo = mid; else hi = mid - 1;
    }
    x = g_hist[lo % HIST].x; y = g_hist[lo % HIST].y;
    return true;
}
void hist_now(long long &x, long long &y)
{
    std::lock_guard<std::mutex> lk(g_hmx);
    x = g_cx; y = g_cy;
}

// ===========================================================================
// the hook thread
// ===========================================================================
std::thread *g_thread = nullptr;     // a pointer: no joinable static at unload
std::atomic<DWORD> g_tid{0};
HHOOK g_hook = nullptr;

LRESULT CALLBACK ms_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_MOUSEMOVE)
    {
        const MSLLHOOKSTRUCT *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        // Called before the cursor moves, so pt - cursor is this move. Injected
        // moves count too: the benchmark's input replay turns the camera.
        POINT c{};
        if (GetCursorPos(&c) && own_foreground())
        {
            const long dx = m->pt.x - c.x, dy = m->pt.y - c.y;
            if (dx != 0 || dy != 0)
            {
                hist_push(qpc_now(), dx, dy);
                g_moves.fetch_add(1, std::memory_order_relaxed);
            }
            else g_moves_zero.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void hook_main()
{
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the queue
    // REV: the id first, so stop_hook can always reach this thread.
    g_tid.store(GetCurrentThreadId());
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ms_proc), &self);
    // The hook callback runs on this thread: keep it responsive.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, ms_proc, self, 0);
    if (g_hook == nullptr)
    {
        char l[200];
        snprintf(l, sizeof l, "[MGPU][FW1] the mouse hook could not be installed (error %lu) - Frame Warp has "
                 "no mouse input and does nothing.", GetLastError());
        mgpu::diag::warn(l);
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
}

std::mutex g_hook_mx;   // REV: ensure_hook (present thread, panel) vs stop_hook (panel, teardown)

void ensure_hook()
{
    std::lock_guard<std::mutex> hl(g_hook_mx);
    if (g_thread != nullptr) return;
    g_thread = new std::thread(hook_main);
    for (int i = 0; i < 500 && g_tid.load() == 0; ++i) Sleep(1);
    mgpu::diag::info("[MGPU][FW1] Frame Warp mouse hook running (reads mouse movement only while the game "
                     "is in front; input is never changed).");
}

void stop_hook()
{
    std::lock_guard<std::mutex> hl(g_hook_mx);
    if (g_thread == nullptr) return;
    // REV: the thread stores its id before anything else; give it time to.
    // Without an id WM_QUIT cannot be posted and join() would block forever.
    DWORD tid = g_tid.load();
    for (int i = 0; i < 2000 && tid == 0; ++i) { Sleep(1); tid = g_tid.load(); }
    if (tid != 0 && PostThreadMessageW(tid, WM_QUIT, 0, 0))
    {
        if (g_thread->joinable()) g_thread->join();
        delete g_thread;
    }
    else
    {
        // Cannot reach it: never block. Leave it parked (it only waits for messages).
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        if (g_thread->joinable()) g_thread->detach();
        delete g_thread;
        mgpu::diag::warn("[MGPU][FW1] the mouse hook thread could not be signalled - hook removed, thread left parked.");
    }
    g_thread = nullptr;
    g_tid.store(0);
}

// ===========================================================================
// the GPU pass
// ===========================================================================
typedef HRESULT (WINAPI *pfn_compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

// Rotation-only re-projection. For each output pixel: its view ray in the
// CURRENT camera, rotated back into the camera the frame was rendered with,
// projected into that frame, sampled there. Outside the frame the sampler
// clamps, which stretches the border into the uncovered edge.
const char *kWarpHLSL =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer C : register(b0) { float tx; float ty; float yaw; float pitch; uint w; uint h; uint p0; uint p1; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);\n"
    "    float3 d = float3((uv.x * 2.0 - 1.0) * tx, (uv.y * 2.0 - 1.0) * ty, 1.0);\n"
    "    float cp = cos(pitch), sp = sin(pitch);\n"
    "    float3 a = float3(d.x, d.y * cp + d.z * sp, -d.y * sp + d.z * cp);\n"
    "    float cy = cos(yaw), sy = sin(yaw);\n"
    "    float3 b = float3(a.x * cy + a.z * sy, a.y, -a.x * sy + a.z * cy);\n"
    "    float2 o = uv;\n"
    "    if (b.z > 1e-3) o = float2(b.x / (b.z * tx), b.y / (b.z * ty)) * 0.5 + 0.5;\n"
    "    dst[id.xy] = src.SampleLevel(smp, o, 0);\n"
    "}\n";

struct gpu_state
{
    ID3D12Device *dev = nullptr;     // AddRef'd, so its address cannot be reused by a new device
    unsigned w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *copy = nullptr;      // the frame as presented (SRV)
    ID3D12Resource *out = nullptr;       // the warped frame (UAV)
    bool failed = false;                 // this device/size/format cannot be warped
};
gpu_state G;

// Objects replaced by a rebuild (output resized or a new device) may still be
// in use by present lists in flight, so they are released a few presents later.
struct retired { IUnknown *o; unsigned left; };
std::vector<retired> g_retired;

void retire(IUnknown *o) { if (o != nullptr) g_retired.push_back({ o, 8u }); }
void age_retired()
{
    size_t w = 0;
    for (size_t i = 0; i < g_retired.size(); ++i)
    {
        if (--g_retired[i].left == 0u) g_retired[i].o->Release();
        else g_retired[w++] = g_retired[i];
    }
    g_retired.resize(w);
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
        snprintf(l, sizeof l, "[MGPU][FW1] Frame Warp cannot run on this output (%ux%u, format %u): %s (0x%08X). "
                 "Frames are shown unwarped.", w, h, (unsigned)fmt, why, (unsigned)hr);
        mgpu::diag::warn(l);
        G.failed = true;
        return false;
    };

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        return fail("the card cannot write this format from a shader", E_FAIL);

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pfn_compile cmp = dc ? (pfn_compile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (cmp == nullptr) return fail("d3dcompiler_47.dll not available", E_FAIL);
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = cmp(kWarpHLSL, strlen(kWarpHLSL), "framewarp", nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs, &err);
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
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
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

    // The views never change (the textures are fixed), so they are written
    // once here and never touched while a list that uses them may be in flight.
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

    snprintf(l, sizeof l, "[MGPU][FW1] Frame Warp ready on the bridge output (%ux%u, format %u).", w, h, (unsigned)fmt);
    mgpu::diag::info(l);
    return true;
}

// ---- status counters ----
std::atomic<unsigned long long> g_warped{0}, g_skip_cursor{0}, g_skip_unknown{0}, g_skip_still{0};
std::atomic<int> g_last_mdeg_x{0}, g_last_mdeg_y{0};   // last warp, thousandths of a degree
LONGLONG g_report_t = 0;

void periodic_report()
{
    const LONGLONG now = qpc_now();
    if (g_report_t == 0) { g_report_t = now; return; }
    if (now - g_report_t < qpc_freq() * 30) return;
    g_report_t = now;
    char l[400];
    snprintf(l, sizeof l, "[MGPU][FW1] last 30 s: mouse moves seen %llu (zero-length %llu) | frames warped %llu, "
             "skipped: menu %llu, cursor visible %llu, no content time %llu, mouse still %llu | last warp %.2f / %.2f deg",
             g_moves.exchange(0), g_moves_zero.exchange(0), g_warped.exchange(0), g_skip_menu.exchange(0),
             g_skip_cursor.exchange(0),
             g_skip_unknown.exchange(0), g_skip_still.exchange(0),
             g_last_mdeg_x.load() / 1000.0, g_last_mdeg_y.load() / 1000.0);
    mgpu::diag::info(l);
}

} // namespace

// ===========================================================================
// public
// ===========================================================================
bool enabled()  { return get(S_ON) != 0; }
void set_enabled(bool on)
{
    put(S_ON, on ? 1 : 0);
    if (!on) stop_hook();
}
int  scale()    { return get(S_SCALE); }
void set_scale(int v)
{
    put(S_SCALE, v);
    // A value set by hand is kept: automatic calibration would otherwise move it.
    if (get(S_AUTO) != 0)
    {
        put(S_AUTO, 0);
        mgpu::diag::info("[MGPU][FW3] degrees per count set by hand - automatic calibration switched off "
                         "so the value stays.");
    }
}
bool auto_calibrate() { return get(S_AUTO) != 0; }
void set_auto_calibrate(bool on) { put(S_AUTO, on ? 1 : 0); if (on) g_auto_kick.store(true); }
int  strength() { return get(S_STR); }
void set_strength(int pct) { put(S_STR, pct); }
bool invert_y() { return get(S_INVY) != 0; }
void set_invert_y(bool on) { put(S_INVY, on ? 1 : 0); }
int  lead_ms()  { return get(S_LEAD); }
void set_lead_ms(int ms) { put(S_LEAD, ms); }

bool apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
           unsigned w, unsigned h, DXGI_FORMAT fmt, unsigned long long content_qpc, float fov_h_deg)
{
    if (!enabled() || dev == nullptr || cl == nullptr || bb == nullptr || w < 16 || h < 16) return false;
    ensure_hook();
    periodic_report();
    age_retired();

    // A visible cursor means the mouse is moving a pointer (menu, inventory,
    // the ReShade overlay), not the camera.
    CURSORINFO ci{}; ci.cbSize = sizeof ci;
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0) { g_skip_cursor.fetch_add(1); return false; }
    if (!g_camera_live.load(std::memory_order_relaxed) ||
        (double)(qpc_now() - (LONGLONG)g_last_mv_qpc.load(std::memory_order_relaxed)) / (double)qpc_freq() > 0.25)
    { g_skip_menu.fetch_add(1); return false; }
    if (content_qpc == 0) { g_skip_unknown.fetch_add(1); return false; }

    LONGLONG t = (LONGLONG)content_qpc - (LONGLONG)((double)lead_ms() * (double)qpc_freq() / 1000.0);
    long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!hist_at(t, x0, y0)) { g_skip_unknown.fetch_add(1); return false; }
    hist_now(x1, y1);
    // degrees per count, times the share of the delay to compensate (FW5)
    const double k = (double)scale() / 1000.0 * (double)get(S_STR) / 100.0;
    double yaw_deg = (double)(x1 - x0) * k;
    double pitch_deg = (double)(y1 - y0) * k * (invert_y() ? -1.0 : 1.0);
    if (std::fabs(yaw_deg) < 0.005 && std::fabs(pitch_deg) < 0.005) { g_skip_still.fetch_add(1); return false; }
    if (yaw_deg > kMaxDeg) yaw_deg = kMaxDeg; else if (yaw_deg < -kMaxDeg) yaw_deg = -kMaxDeg;
    if (pitch_deg > kMaxDeg) pitch_deg = kMaxDeg; else if (pitch_deg < -kMaxDeg) pitch_deg = -kMaxDeg;

    if (!build(dev, w, h, fmt)) return false;

    // Field of view: the game's own (Streamline), the calibrated one, or
    // FSRFov - horizontal; vertical from the aspect ratio.
    double fov = fov_in_use(fov_h_deg, nullptr);
    const double tx = std::tan(fov * 0.5 * 3.14159265358979 / 180.0);
    const double ty = tx * (double)h / (double)w;

    // bb: COPY_DEST -> COPY_SOURCE, copy it, warp into G.out, copy back.
    D3D12_TEXTURE_COPY_LOCATION a{}, b{};
    a.Type = b.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{ 0, 0, 0, w, h, 1 };
    tbarrier(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    a.pResource = G.copy; b.pResource = bb;
    cl->CopyTextureRegion(&a, 0, 0, 0, &b, &box);
    tbarrier(cl, G.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap *hs[1] = { G.heap };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(G.rs);
    cl->SetPipelineState(G.pso);
    cl->SetComputeRootDescriptorTable(0, G.heap->GetGPUDescriptorHandleForHeapStart());
    struct { float tx, ty, yaw, pitch; UINT w, h, p0, p1; } c{
        (float)tx, (float)ty, (float)(yaw_deg * 3.14159265358979 / 180.0),
        (float)(pitch_deg * 3.14159265358979 / 180.0), w, h, 0u, 0u };
    cl->SetComputeRoot32BitConstants(1, 8, &c, 0);
    cl->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);

    tbarrier(cl, G.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tbarrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    a.pResource = bb; b.pResource = G.out;
    cl->CopyTextureRegion(&a, 0, 0, 0, &b, &box);
    tbarrier(cl, G.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    tbarrier(cl, G.copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    g_warped.fetch_add(1, std::memory_order_relaxed);
    g_last_mdeg_x.store((int)(yaw_deg * 1000.0));
    g_last_mdeg_y.store((int)(pitch_deg * 1000.0));
    return true;
}

void shutdown()
{
    // Only the hook stops. The GPU objects stay: a present list may still be
    // using them, and they go with the process.
    stop_hook();
}

// ===========================================================================
// FW2: calibration
// ===========================================================================
namespace
{
std::mutex g_cmx;
bool g_cal = false;
LONGLONG g_cal_t0 = 0;
std::vector<double> g_cal_tx2, g_cal_dpc;
unsigned g_cal_rej_slow = 0, g_cal_rej_shape = 0, g_cal_rej_mouse = 0, g_cal_rej_other = 0;
char g_cal_msg[300] = "";

const double kEdgeX = 0.88;            // the edge bands' centre in NDC (uv 0.06 / 0.94)
const unsigned kCalNeed = 40;          // good frames
const double kCalTimeout = 15.0;       // seconds

// FW3: automatic calibration during play
std::vector<double> g_auto_tx2, g_auto_dpc;   // rolling, newest last (g_cmx)
unsigned g_auto_new = 0;                      // samples since the last evaluation
unsigned long long g_auto_total = 0;
LONGLONG g_auto_saved_t = 0;
const size_t kAutoKeep = 300;                 // rolling window
const unsigned kAutoMin = 60;                 // before the first estimate
const unsigned kAutoEvery = 30;               // re-evaluate after this many new turns
// FW4: once the values have settled, sampling stops and only wakes for a
// re-check every few minutes, so steady play costs nothing.
bool g_auto_resting = false;
LONGLONG g_auto_rest_until = 0;
const double kAutoRestS = 300.0;              // 5 minutes between re-checks

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void cal_finish_locked(bool by_timeout)
{
    g_cal = false;
    char l[500];
    const unsigned seen = (unsigned)g_cal_tx2.size() + g_cal_rej_slow + g_cal_rej_shape + g_cal_rej_mouse + g_cal_rej_other;
    if (seen == 0u)
    {
        snprintf(g_cal_msg, sizeof g_cal_msg,
                 "Calibration failed: no motion vectors. Is the game's DLSS on?");
        snprintf(l, sizeof l, "[MGPU][FW2] %s", g_cal_msg);
        mgpu::diag::warn(l);
        return;
    }
    if (g_cal_tx2.size() < kCalNeed)
    {
        snprintf(g_cal_msg, sizeof g_cal_msg,
                 "%s: not enough clean turns (%u). Stand still and turn steadily.",
                 by_timeout ? "Calibration timed out" : "Calibration stopped", (unsigned)g_cal_tx2.size());
        snprintf(l, sizeof l, "[MGPU][FW2] %s Rejected frames: too slow %u, not a clean turn %u, no mouse %u, "
                 "other %u.", g_cal_msg, g_cal_rej_slow, g_cal_rej_shape, g_cal_rej_mouse, g_cal_rej_other);
        mgpu::diag::warn(l);
        return;
    }
    const double tx = std::sqrt(median(g_cal_tx2));
    const double fov = 2.0 * std::atan(tx) * 180.0 / 3.14159265358979;
    const double dpc = median(g_cal_dpc);
    if (fov < 40.0 || fov > 140.0 || dpc <= 0.0005 || dpc > 0.5)
    {
        snprintf(g_cal_msg, sizeof g_cal_msg,
                 "Calibration failed (FOV %.1f, %.4f deg/count looks wrong). Not saved - try again.", fov, dpc);
        snprintf(l, sizeof l, "[MGPU][FW2] %s", g_cal_msg);
        mgpu::diag::warn(l);
        return;
    }
    put(S_FOV, (int)(fov * 10.0 + 0.5));
    put(S_SCALE, (int)(dpc * 1000.0 + 0.5));
    snprintf(g_cal_msg, sizeof g_cal_msg,
             "Calibrated: FOV %.1f deg, %.3f deg/count.", fov, dpc);
    snprintf(l, sizeof l, "[MGPU][FW2] %s (FrameWarpFov=%d FrameWarpScale=%d). Rejected frames: too slow %u, "
             "not a clean turn %u, no mouse %u, other %u. Tip: FSRFov=%.0f would also suit frame generation.",
             g_cal_msg, get(S_FOV), get(S_SCALE), g_cal_rej_slow, g_cal_rej_shape, g_cal_rej_mouse,
             g_cal_rej_other, fov);
    mgpu::diag::info(l);
}
} // namespace

void calib_start()
{
    ensure_hook();
    std::lock_guard<std::mutex> lk(g_cmx);
    g_cal = true;
    g_cal_t0 = qpc_now();
    g_cal_tx2.clear(); g_cal_dpc.clear();
    g_cal_rej_slow = g_cal_rej_shape = g_cal_rej_mouse = g_cal_rej_other = 0;
    g_auto_tx2.clear(); g_auto_dpc.clear(); g_auto_new = 0;
    g_auto_resting = false;
    snprintf(g_cal_msg, sizeof g_cal_msg, "Calibrating - turn left and right...");
    mgpu::diag::info("[MGPU][FW2] calibration started.");
}

void calib_cancel()
{
    std::lock_guard<std::mutex> lk(g_cmx);
    if (g_cal) cal_finish_locked(false);
}

bool sampling()
{
    if (calibrating()) return true;
    // FW5: with Frame Warp on, every frame is sampled - the menu check needs it.
    if (get(S_ON) == 0) return false;
    std::lock_guard<std::mutex> lk(g_cmx);
    if (g_auto_kick.exchange(false)) g_auto_resting = false;
    if (g_auto_resting && qpc_now() >= g_auto_rest_until)
    {
        // Re-check: learn from fresh turns only, so a changed sensitivity shows.
        g_auto_resting = false;
        g_auto_tx2.clear(); g_auto_dpc.clear(); g_auto_new = 0;
    }
    return true;
}

bool calibrating()
{
    std::lock_guard<std::mutex> lk(g_cmx);
    if (g_cal && (double)(qpc_now() - g_cal_t0) / (double)qpc_freq() > kCalTimeout) cal_finish_locked(true);
    return g_cal;
}

// One frame's measurement: true with tx^2 and degrees per count when the frame
// is a clean turn. *why: 1 too slow, 2 not a clean turn, 3 no mouse, 4 other.
static bool measure(const float *mv, float to_uv_x, unsigned mv_el, unsigned long long cap_qpc,
                    unsigned long long prev_cap_qpc, double &tx2_out, double &dpc_out, int &why)
{
    why = 4;
    if (mv == nullptr || mv_el != 1u || cap_qpc == 0ull || prev_cap_qpc == 0ull || cap_qpc <= prev_cap_qpc ||
        to_uv_x == 0.0f || (double)(cap_qpc - prev_cap_qpc) / (double)qpc_freq() > 0.2)
        return false;
    CURSORINFO ci{}; ci.cbSize = sizeof ci;
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0) return false;
    if (!own_foreground()) return false;

    // Median horizontal movement per band, as a fraction of the screen width.
    const unsigned per = CALIB_COLS * CALIB_ROWS;
    double med[3];
    for (unsigned b = 0; b < 3u; ++b)
    {
        std::vector<double> v;
        v.reserve(per);
        for (unsigned i = 0; i < per; ++i)
        {
            const float x = mv[(b * per + i) * 2u];
            if (x == x && x > -1e6f && x < 1e6f) v.push_back((double)x * (double)to_uv_x);
        }
        if (v.size() < per / 2u) return false;
        med[b] = median(v);
    }
    const double c = med[1], edge = 0.5 * (med[0] + med[2]);
    why = 1;
    if (std::fabs(c) < 0.0015) return false;            // under ~4 px a frame at 2560
    why = 2;
    if ((med[0] > 0.0) != (c > 0.0) || (med[2] > 0.0) != (c > 0.0)) return false;
    // Left and right must agree: a turn moves both edges alike, walking sideways does not.
    if (std::fabs(med[0] - med[2]) > 0.25 * std::fabs(edge)) return false;
    const double ratio = edge / c;
    if (!(ratio > 1.02 && ratio < 5.0)) return false;
    const double tx2 = (ratio - 1.0) / (kEdgeX * kEdgeX);
    const double tx = std::sqrt(tx2);

    // The mouse during that game frame.
    why = 3;
    long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!hist_at((LONGLONG)prev_cap_qpc, x0, y0) || !hist_at((LONGLONG)cap_qpc, x1, y1)) return false;
    const double counts = std::fabs((double)(x1 - x0));
    if (counts < 3.0) return false;
    // A mostly vertical mouse move is not a turn to learn from.
    if (std::fabs((double)(y1 - y0)) > 0.5 * counts) { why = 2; return false; }
    // Centre shift in NDC is 2c; for a turn of theta it is tan(theta)/tx.
    const double theta = std::atan(2.0 * std::fabs(c) * tx) * 180.0 / 3.14159265358979;
    tx2_out = tx2;
    dpc_out = theta / counts;
    return true;
}

// ---- FW3: automatic calibration during play ----

static double rel_spread(const std::vector<double> &v, double med)
{
    std::vector<double> d;
    d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - med));
    return (med != 0.0) ? median(d) / std::fabs(med) : 1.0;
}

static void auto_evaluate_locked()
{
    g_auto_new = 0;
    if (g_auto_tx2.size() < kAutoMin) return;
    const double mtx2 = median(g_auto_tx2), mdpc = median(g_auto_dpc);
    // Only a consistent set is trusted: the middle half within about 10%.
    if (rel_spread(g_auto_tx2, mtx2) > 0.12 || rel_spread(g_auto_dpc, mdpc) > 0.12) return;
    const double fov = 2.0 * std::atan(std::sqrt(mtx2)) * 180.0 / 3.14159265358979;
    if (fov < 40.0 || fov > 140.0 || mdpc <= 0.0005 || mdpc > 0.5) return;
    const int new_fov = (int)(fov * 10.0 + 0.5), new_scale = (int)(mdpc * 1000.0 + 0.5);
    const int old_fov = get(S_FOV), old_scale = get(S_SCALE);
    const bool fov_moved = (old_fov == 0) || std::abs(new_fov - old_fov) >= 8;            // 0.8 deg
    const bool scale_moved = std::abs(new_scale - old_scale) * 100 >= 3 * (old_scale > 0 ? old_scale : 1);   // 3%
    const LONGLONG now = qpc_now();
    char l[300];
    if (fov_moved || scale_moved)
    {
        if (g_auto_saved_t != 0 && (double)(now - g_auto_saved_t) / (double)qpc_freq() < 10.0) return;   // at most every 10 s
        g_auto_saved_t = now;
        if (fov_moved) put(S_FOV, new_fov);
        if (scale_moved) put(S_SCALE, new_scale);
        snprintf(l, sizeof l, "[MGPU][FW3] auto-calibrated from the last %u turns: field of view %.1f deg, "
                 "%.3f deg per mouse count (saved).", (unsigned)g_auto_tx2.size(), fov, mdpc);
        mgpu::diag::info(l);
    }
    // FW4: settled - stop sampling until the next re-check.
    g_auto_resting = true;
    g_auto_rest_until = now + (LONGLONG)(kAutoRestS * (double)qpc_freq());
    snprintf(l, sizeof l, "[MGPU][FW4] calibration settled (field of view %.1f deg, %.3f deg per count) - "
             "sampling paused, next check in %.0f minutes.", fov, mdpc, kAutoRestS / 60.0);
    mgpu::diag::info(l);
}

// FW5: the menu check. When the mouse moves sideways, the camera turning
// makes the centre of the picture move; a menu does not. A few frames of the
// mouse moving with a still centre switch the warp off; a couple of frames of
// the centre following the mouse switch it back on.
static void camera_follow_locked(const float *mv, float to_uv_x, unsigned mv_el,
                                 unsigned long long cap_qpc, unsigned long long prev_cap_qpc)
{
    if (mv != nullptr) g_last_mv_qpc.store(qpc_now(), std::memory_order_relaxed);
    if (mv == nullptr || mv_el != 1u || cap_qpc == 0ull || prev_cap_qpc == 0ull || cap_qpc <= prev_cap_qpc ||
        to_uv_x == 0.0f || !own_foreground())
        return;
    long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!hist_at((LONGLONG)prev_cap_qpc, x0, y0) || !hist_at((LONGLONG)cap_qpc, x1, y1)) return;
    const double counts = std::fabs((double)(x1 - x0));
    if (counts < 6.0) return;                      // the mouse barely moved: no evidence either way
    const unsigned per = CALIB_COLS * CALIB_ROWS;
    std::vector<double> v;
    v.reserve(per);
    for (unsigned i = 0; i < per; ++i)
    {
        const float x = mv[(per + i) * 2u];        // the centre band
        if (x == x && x > -1e6f && x < 1e6f) v.push_back((double)x * (double)to_uv_x);
    }
    if (v.size() < per / 2u) return;
    const double cs = median(v);                   // signed: which way the centre moved
    const double c = std::fabs(cs);
    const double dxs = (double)(x1 - x0);
    // FW6: not just "did the picture move" but "did it move as much as this
    // mouse movement turns the camera". A menu over a live, drifting scene
    // moves the centre too, which passed the old test and let the menu wobble.
    const int fov10 = get(S_FOV);
    const double fov = (fov10 >= 300 && fov10 <= 1500) ? fov10 / 10.0 : 90.0;
    const double tx = std::tan(fov * 0.5 * 3.14159265358979 / 180.0);
    const double turn = counts * (double)get(S_SCALE) / 1000.0 * 3.14159265358979 / 180.0;
    const double expect = std::tan(turn < 1.2 ? turn : 1.2) / (2.0 * tx);   // centre shift, fraction of the width
    const double ratio = (expect > 1e-6) ? c / expect : 0.0;
    // FW7 guardrail 1, both ways (FW8): only a clear movement is evidence at
    // all. A turn this mouse movement would make must shift the centre by at
    // least 0.8% of the width (about 20 px at 2560). Small moves neither pause
    // nor resume - before FW8 they could still RESUME the warp in a menu whose
    // background drifted by about the right amount, and the menu wobbled.
    if (expect < 0.008) return;
    // FW8: the picture must also move the right WAY. The sign that goes with
    // a real turn is learned from clear turns; a drifting menu background
    // moves either way regardless of the mouse.
    const int dir = (cs * dxs > 0.0) ? 1 : -1;
    const bool dir_ok = (g_dir == 0) || (dir == g_dir);
    const bool follows = (c >= 0.0008) && ratio > 0.4 && ratio < 2.5 && dir_ok;
    if (follows && ratio > 0.6 && ratio < 1.6) g_dir = dir;
    const LONGLONG now = qpc_now();
    if (!follows)                                  // still, or moving but not with the mouse: a menu
    {
        // FW7 guardrail 2: right after the picture clearly followed the mouse,
        // one odd frame (a hit reaction, a short camera lock) is not a menu -
        // it takes three frames instead of two within 150 ms of that.
        const bool grace = g_follow_qpc != 0 && (double)(now - g_follow_qpc) / (double)qpc_freq() < 0.15;
        g_cam_ev = 0;
        if (++g_menu_ev >= (grace ? 3u : 2u) && g_camera_live.exchange(false))
            mgpu::diag::info("[MGPU][FW5] mouse moves but the camera does not follow it (menu) - Frame Warp paused.");
    }
    else                                           // the picture turned with the mouse
    {
        g_follow_qpc = now;
        g_menu_ev = 0;
        // FW8: resuming takes three clear frames, pausing two - a menu has to
        // fool the check three times in a row to wobble.
        if (++g_cam_ev >= 3u && !g_camera_live.exchange(true))
            mgpu::diag::info("[MGPU][FW5] the camera follows the mouse - Frame Warp active.");
    }
}

void calib_frame(const float *mv, float to_uv_x, unsigned mv_el,
                 unsigned long long cap_qpc, unsigned long long prev_cap_qpc)
{
    std::lock_guard<std::mutex> lk(g_cmx);
    const bool manual = g_cal;
    if (get(S_ON) != 0) camera_follow_locked(mv, to_uv_x, mv_el, cap_qpc, prev_cap_qpc);
    if (!manual && (!(get(S_AUTO) != 0 && get(S_ON) != 0) || g_auto_resting)) return;
    double tx2 = 0.0, dpc = 0.0;
    int why = 0;
    const bool ok = measure(mv, to_uv_x, mv_el, cap_qpc, prev_cap_qpc, tx2, dpc, why);
    if (manual)
    {
        if (!ok)
        {
            if (why == 1) ++g_cal_rej_slow; else if (why == 2) ++g_cal_rej_shape;
            else if (why == 3) ++g_cal_rej_mouse; else ++g_cal_rej_other;
            return;
        }
        g_cal_tx2.push_back(tx2);
        g_cal_dpc.push_back(dpc);
        if (g_cal_tx2.size() >= kCalNeed * 2u ||
            (g_cal_tx2.size() >= kCalNeed && (double)(qpc_now() - g_cal_t0) / (double)qpc_freq() > 4.0))
            cal_finish_locked(false);
        else
            snprintf(g_cal_msg, sizeof g_cal_msg, "Calibrating - %u/%u", (unsigned)g_cal_tx2.size(), kCalNeed);
        return;
    }
    if (!ok) return;
    g_auto_tx2.push_back(tx2);
    g_auto_dpc.push_back(dpc);
    if (g_auto_tx2.size() > kAutoKeep) { g_auto_tx2.erase(g_auto_tx2.begin()); g_auto_dpc.erase(g_auto_dpc.begin()); }
    ++g_auto_total;
    if (++g_auto_new >= kAutoEvery) auto_evaluate_locked();
}

void calib_status(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_cmx);
    if (g_cal || get(S_AUTO) == 0 || get(S_ON) == 0) { snprintf(out, n, "%s", g_cal_msg); return; }
    if (g_auto_resting)
    {
        const double left = (double)(g_auto_rest_until - qpc_now()) / (double)qpc_freq();
        snprintf(out, n, "Auto: settled, next check in %.0f min", left > 60.0 ? left / 60.0 : 1.0);
    }
    else if (g_auto_tx2.size() < kAutoMin)
        snprintf(out, n, "Auto: learning %u/%u", (unsigned)g_auto_tx2.size(), kAutoMin);
    else
        snprintf(out, n, "Auto: refining (%u turns)", (unsigned)g_auto_tx2.size());
}

float fov_in_use(float fsr_fov, const char **source)
{
    float g = 0.0f;
    if (mgpu::sltags::camera_hfov_deg(g)) { if (source) *source = "from the game"; return g; }
    const int c = get(S_FOV);
    if (c >= 300 && c <= 1500) { if (source) *source = "calibrated"; return (float)c / 10.0f; }
    if (source) *source = "FSRFov";
    return (fsr_fov >= 30.0f && fsr_fov <= 150.0f) ? fsr_fov : 90.0f;
}

void emergency_unhook()
{
    // DllMain (live unload): no joins under the loader lock. Removing the hook
    // stops Windows calling ms_proc once this image is gone; the thread stays
    // parked in GetMessage, which no message will ever wake.
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
}

void release_gpu_now()
{
    release_gpu();
    for (const retired &r : g_retired) r.o->Release();
    g_retired.clear();
}

void status(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    if (!enabled()) { snprintf(out, n, "off"); return; }
    if (G.failed) { snprintf(out, n, "cannot run here (see log [FW1])"); return; }
    snprintf(out, n, "%s | last warp %.2f / %.2f deg",
             (g_thread == nullptr) ? "waiting"
                                   : (g_hook == nullptr ? "NO MOUSE HOOK (see log)"
                                      : (g_camera_live.load() ? "running" : "paused (menu or not turning yet)")),
             g_last_mdeg_x.load() / 1000.0, g_last_mdeg_y.load() / 1000.0);
}

} // namespace framewarp
} // namespace mgpu
