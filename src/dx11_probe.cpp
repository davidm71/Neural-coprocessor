// MGPU Bridge - DX11 transport-hop probe. See dx11_probe.hpp.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dx11_probe.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

namespace mgpu::dx11probe
{
namespace
{
constexpr unsigned SAMPLE_EVERY = 300u;     // presents between samples
constexpr unsigned MAX_SAMPLES  = 20u;      // then quiet

std::atomic<int> g_mode{0};                 // 0 off (also before init), 1 on
std::atomic<bool> g_inited{false};
bool g_ready = false;                       // the hop exists
bool g_failed = false;                      // said once, then quiet
unsigned g_w = 0, g_h = 0, g_fmt = 0;

// ---- D3D11 side (the game's device) ----
ID3D11Device *g_dev11 = nullptr;            // not owned (the game's)
void         *g_chain = nullptr;            // R228: the IDXGISwapChain* the probe was built on (not owned)
ID3D11Texture2D *g_shared11 = nullptr;      // ours, SHARED_NTHANDLE
ID3D11Texture2D *g_staging11 = nullptr;     // ours, CPU-readable reference
ID3D11Fence *g_fence11 = nullptr;           // ours, D3D11_FENCE_FLAG_SHARED
HANDLE g_tex_handle = nullptr, g_fence_handle = nullptr;

// ---- D3D12 side (our device on the SAME adapter) ----
ID3D12Device *g_dev12 = nullptr;
ID3D12CommandQueue *g_queue = nullptr;
ID3D12CommandAllocator *g_alloc = nullptr;
ID3D12GraphicsCommandList *g_list = nullptr;
ID3D12Resource *g_shared12 = nullptr;       // the D3D11 texture, opened here
ID3D12Resource *g_readback = nullptr;
ID3D12Fence *g_fence12 = nullptr;           // the D3D11 fence, opened here
ID3D12Fence *g_done = nullptr;              // ours: the readback copy landed
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
UINT g_rows = 0; UINT64 g_row_bytes = 0, g_total = 0;

unsigned long long g_presents = 0;
unsigned long long g_value = 0;             // shared fence value, D3D11 signals it
unsigned long long g_done_value = 0;
unsigned g_samples = 0, g_ok = 0, g_mismatch = 0, g_pending = 0;
bool g_in_flight = false;                   // a D3D12 copy is out; read it next time

template <class T> void rel(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }

bool addon_dir(wchar_t *out, size_t n)
{
    HMODULE hm = nullptr;
    static int anchor = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &hm))
        return false;
    const DWORD len = GetModuleFileNameW(hm, out, (DWORD)n);
    if (len == 0 || len >= n) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == nullptr) return false;
    slash[1] = L'\0';
    return true;
}

int read_key()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!addon_dir(dir, MAX_PATH)) return 0;
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsmgpu.ini", dir);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return 0;
    static char buf[mgpu::config::MAX_BYTES + 2u];
    const size_t got = fread(buf, 1, mgpu::config::MAX_BYTES + 1u, f);
    fclose(f);
    if (got > mgpu::config::MAX_BYTES) return 0;
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "Dx11Probe");
    if (v == nullptr || *v != '1') return 0;
    // R230: never beside the producer. 4 of 4 runs (TR-6..TR-8): with the
    // probe built, NGX Init on GPU 1 returned FAIL_OutOfDate and NR never ran;
    // without it, Success. The hop it was built to prove is proven (TR-5b) and
    // the producer is proven (TR-8), so with DX11=1 the probe refuses itself.
    const char *d = mgpu::config::find(buf, got, "DX11");
    if (d != nullptr && *d == '1') return 2;
    return 1;
}

void fail(const char *step, HRESULT hr)
{
    g_failed = true;
    char l[300];
    snprintf(l, sizeof l, "[MGPU][DX11] HOP FAILED at %s: hr=0x%08X. The probe stops here; the game is "
                          "untouched. This names the step the D3D11 producer would need to solve.",
             step, (unsigned)hr);
    mgpu::diag::error(l);
}

// FNV-1a over the first `rows` rows, `row_bytes` each, from a mapped pointer
// with the given pitch. Same function both sides.
unsigned long long hash_rows(const unsigned char *p, size_t pitch, size_t row_bytes, unsigned rows)
{
    unsigned long long h = 1469598103934665603ull;
    for (unsigned r = 0; r < rows; ++r)
    {
        const unsigned char *row = p + (size_t)r * pitch;
        for (size_t i = 0; i < row_bytes; ++i) { h ^= row[i]; h *= 1099511628211ull; }
    }
    return h;
}

bool build(void *device, void *swapchain)
{
    g_dev11 = reinterpret_cast<ID3D11Device *>(device);
    g_chain = swapchain;   // R228: shutdown is for this chain only
    HRESULT hr;

    // 1. The shared texture on the game's device. SHARED_NTHANDLE needs an
    //    11.1 device; a 2016 title's device is 11.0 or 11.1 - the create
    //    says which.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_w; td.Height = g_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)g_fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    hr = g_dev11->CreateTexture2D(&td, nullptr, &g_shared11);
    if (FAILED(hr)) { fail("D3D11 CreateTexture2D(SHARED_NTHANDLE)", hr); return false; }

    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = g_dev11->CreateTexture2D(&sd, nullptr, &g_staging11);
    if (FAILED(hr)) { fail("D3D11 CreateTexture2D(staging)", hr); return false; }

    IDXGIResource1 *res1 = nullptr;
    hr = g_shared11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res1));
    if (FAILED(hr)) { fail("QueryInterface(IDXGIResource1)", hr); return false; }
    hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                  nullptr, &g_tex_handle);
    res1->Release();
    if (FAILED(hr)) { fail("IDXGIResource1::CreateSharedHandle(texture)", hr); return false; }

    // 2. The shared fence (D3D11.4). The one piece a pre-1703 Windows lacks.
    ID3D11Device5 *dev5 = nullptr;
    hr = g_dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
    if (FAILED(hr)) { fail("QueryInterface(ID3D11Device5) - no D3D11.4 fences on this device", hr); return false; }
    hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g_fence11));
    dev5->Release();
    if (FAILED(hr)) { fail("ID3D11Device5::CreateFence(SHARED)", hr); return false; }
    hr = g_fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_fence_handle);
    if (FAILED(hr)) { fail("ID3D11Fence::CreateSharedHandle", hr); return false; }

    // 3. Our D3D12 device on the SAME adapter as the game's D3D11 device.
    IDXGIDevice *dxgidev = nullptr;
    hr = g_dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgidev));
    if (FAILED(hr)) { fail("QueryInterface(IDXGIDevice)", hr); return false; }
    IDXGIAdapter *adapter = nullptr;
    hr = dxgidev->GetAdapter(&adapter);
    dxgidev->Release();
    if (FAILED(hr)) { fail("IDXGIDevice::GetAdapter", hr); return false; }
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g_dev12));
    adapter->Release();
    if (FAILED(hr)) { fail("D3D12CreateDevice(game adapter)", hr); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g_queue));
    if (FAILED(hr)) { fail("D3D12 CreateCommandQueue", hr); return false; }
    hr = g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_alloc));
    if (FAILED(hr)) { fail("D3D12 CreateCommandAllocator", hr); return false; }
    hr = g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_list));
    if (FAILED(hr)) { fail("D3D12 CreateCommandList", hr); return false; }
    g_list->Close();

    // 4. Open the D3D11 texture and fence in D3D12.
    hr = g_dev12->OpenSharedHandle(g_tex_handle, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_shared12));
    if (FAILED(hr)) { fail("ID3D12Device::OpenSharedHandle(texture)", hr); return false; }
    hr = g_dev12->OpenSharedHandle(g_fence_handle, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_fence12));
    if (FAILED(hr)) { fail("ID3D12Device::OpenSharedHandle(fence)", hr); return false; }
    hr = g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_done));
    if (FAILED(hr)) { fail("D3D12 CreateFence(done)", hr); return false; }

    // 5. The readback buffer for the opened texture's footprint.
    const D3D12_RESOURCE_DESC rd = g_shared12->GetDesc();
    g_dev12->GetCopyableFootprints(&rd, 0, 1, 0, &g_fp, &g_rows, &g_row_bytes, &g_total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = g_total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_readback));
    if (FAILED(hr)) { fail("D3D12 CreateCommittedResource(readback)", hr); return false; }

    char l[400];
    snprintf(l, sizeof l,
             "[MGPU][DX11] HOP BUILT: shared texture %ux%u fmt=%u on the game's D3D11 device, a D3D11.4 "
             "shared fence, our D3D12 device on the same adapter opened both (texture desc %llux%u, row "
             "%llu B, footprint %llu B). Sampling every %u presents, %u samples. Nothing on GPU 1.",
             g_w, g_h, g_fmt, (unsigned long long)rd.Width, (unsigned)rd.Height,
             (unsigned long long)g_row_bytes, (unsigned long long)g_total, SAMPLE_EVERY, MAX_SAMPLES);
    mgpu::diag::info(l);
    return true;
}

// Read the result of the previous sample, if the GPU is done with it.
void read_previous(ID3D11DeviceContext *ctx)
{
    if (!g_in_flight) return;
    if (g_done->GetCompletedValue() < g_done_value) { ++g_pending; return; }
    g_in_flight = false;

    // D3D12 side.
    unsigned char *p12 = nullptr;
    D3D12_RANGE rr{0, (SIZE_T)g_total};
    HRESULT hr = g_readback->Map(0, &rr, reinterpret_cast<void **>(&p12));
    if (FAILED(hr)) { fail("D3D12 readback Map", hr); return; }
    const unsigned rows = (g_h < 64u) ? g_h : 64u;   // enough to catch a wrong copy, cheap
    const unsigned long long h12 = hash_rows(p12 + g_fp.Offset, g_fp.Footprint.RowPitch, (size_t)g_row_bytes, rows);
    const unsigned char *c12 = p12 + g_fp.Offset + (size_t)(g_h / 2u) * g_fp.Footprint.RowPitch + (size_t)(g_w / 2u) * 4u;
    const unsigned mid12 = (unsigned)c12[0] | ((unsigned)c12[1] << 8) | ((unsigned)c12[2] << 16) | ((unsigned)c12[3] << 24);
    D3D12_RANGE wr{0, 0};
    g_readback->Unmap(0, &wr);

    // D3D11 side, DO_NOT_WAIT: if the staging copy is still in flight the
    // sample is reported as D3D12-only, never waited for.
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(g_staging11, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    char l[400];
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
    {
        snprintf(l, sizeof l, "[MGPU][DX11] sample %u: D3D12 readback hash=%016llx centre=0x%08X | D3D11 staging "
                              "still drawing (not waited for)", g_samples, h12, mid12);
        mgpu::diag::info(l);
        return;
    }
    if (FAILED(hr)) { fail("D3D11 staging Map", hr); return; }
    const unsigned long long h11 = hash_rows(reinterpret_cast<const unsigned char *>(m.pData), m.RowPitch, (size_t)g_row_bytes, rows);
    ctx->Unmap(g_staging11, 0);
    const bool same = (h11 == h12);
    if (same) ++g_ok; else ++g_mismatch;
    snprintf(l, sizeof l,
             "[MGPU][DX11] sample %u: %s | D3D11 staging hash=%016llx | D3D12 readback hash=%016llx | centre pixel "
             "0x%08X | first %u rows x %llu B | ok=%u mismatch=%u pending=%u",
             g_samples, same ? "HOP OK - the pixels that left the game's D3D11 context arrived in our D3D12 device"
                             : "HOP MISMATCH - the two sides read different bytes",
             h11, h12, mid12, rows, (unsigned long long)g_row_bytes, g_ok, g_mismatch, g_pending);
    if (same) mgpu::diag::info(l); else mgpu::diag::warn(l);
}

void sample(ID3D11DeviceContext *ctx, ID3D11Texture2D *bb)
{
    // D3D11: two copies, one signal. No wait.
    ctx->CopyResource(g_shared11, bb);
    ctx->CopyResource(g_staging11, bb);
    ID3D11DeviceContext4 *ctx4 = nullptr;
    HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4));
    if (FAILED(hr)) { fail("QueryInterface(ID3D11DeviceContext4)", hr); return; }
    ++g_value;
    hr = ctx4->Signal(g_fence11, g_value);
    ctx4->Release();
    if (FAILED(hr)) { fail("ID3D11DeviceContext4::Signal", hr); return; }

    // D3D12: wait for that signal ON THE GPU, copy to the readback, signal
    // ours. The CPU does not wait; read_previous looks at it next time.
    hr = g_queue->Wait(g_fence12, g_value);
    if (FAILED(hr)) { fail("ID3D12CommandQueue::Wait(shared fence)", hr); return; }
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_shared12;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = g_shared12; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = g_readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = g_fp;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_list->ResourceBarrier(1, &b);
    hr = g_list->Close();
    if (FAILED(hr)) { fail("D3D12 command list Close", hr); return; }
    ID3D12CommandList *ls[1] = { g_list };
    g_queue->ExecuteCommandLists(1, ls);
    ++g_done_value;
    hr = g_queue->Signal(g_done, g_done_value);
    if (FAILED(hr)) { fail("ID3D12CommandQueue::Signal(done)", hr); return; }
    g_in_flight = true;
    ++g_samples;
}
}   // namespace

void init(void *device, void *swapchain, unsigned width, unsigned height, unsigned dxgi_format)
{
    bool expect = false;
    if (!g_inited.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) return;
    const int key = read_key();
    if (key == 2)
    {
        mgpu::diag::warn("[MGPU][DX11] Dx11Probe=1 REFUSED because DX11=1: the probe and the producer do not run "
                         "together (with the probe built, NGX Init on GPU 1 returns FAIL_OutOfDate and NR never "
                         "runs - 4 of 4 runs). The probe's job is done; set Dx11Probe=0. Nothing was built.");
        return;
    }
    if (key != 1) return;
    if (device == nullptr || width == 0 || height == 0) return;
    g_w = width; g_h = height; g_fmt = dxgi_format;
    mgpu::diag::info("[MGPU][DX11] Dx11Probe=1: building the D3D11 -> D3D12 transport hop on the game's adapter. "
                     "Read-only for the game: it copies the back buffer every 300 presents and compares both sides.");
    if (build(device, swapchain)) { g_ready = true; g_mode.store(1, std::memory_order_release); }
    else shutdown();
}

bool on() { return g_mode.load(std::memory_order_acquire) == 1; }

// R228. destroy_swapchain fires for EVERY chain in the process, and the
// bridge's own present chain is resized once at arm (P7.3) - which destroyed
// this probe from the bridge thread while the game thread was inside the
// same ReShade resize path (TR-6: froze there; TR-6b without the probe: ran).
// Only the chain the probe was built on is its teardown point.
bool is_chain(void *swapchain) { return swapchain != nullptr && swapchain == g_chain; }

void on_present(void *context, void *back_buffer)
{
    if (!on() || !g_ready || g_failed) return;
    if (context == nullptr || back_buffer == nullptr) return;
    ID3D11DeviceContext *ctx = reinterpret_cast<ID3D11DeviceContext *>(context);
    const unsigned long long n = ++g_presents;
    if (n % SAMPLE_EVERY != 0ull) return;
    read_previous(ctx);
    if (g_failed) return;
    if (g_samples >= MAX_SAMPLES)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            char l[200];
            snprintf(l, sizeof l, "[MGPU][DX11] probe done: %u samples, ok=%u mismatch=%u pending=%u. Quiet from here.",
                     g_samples, g_ok, g_mismatch, g_pending);
            mgpu::diag::info(l);
        }
        return;
    }
    sample(ctx, reinterpret_cast<ID3D11Texture2D *>(back_buffer));
}

void shutdown()
{
    g_mode.store(0, std::memory_order_release);
    g_ready = false;
    if (g_queue != nullptr && g_done != nullptr && g_in_flight)
    {
        // One bounded wait at teardown only, off every game frame.
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev != nullptr)
        {
            if (g_done->GetCompletedValue() < g_done_value)
            {
                g_done->SetEventOnCompletion(g_done_value, ev);
                WaitForSingleObject(ev, 2000);
            }
            CloseHandle(ev);
        }
        g_in_flight = false;
    }
    rel(g_readback); rel(g_done); rel(g_fence12); rel(g_shared12);
    rel(g_list); rel(g_alloc); rel(g_queue); rel(g_dev12);
    if (g_fence_handle != nullptr) { CloseHandle(g_fence_handle); g_fence_handle = nullptr; }
    if (g_tex_handle != nullptr) { CloseHandle(g_tex_handle); g_tex_handle = nullptr; }
    rel(g_fence11); rel(g_staging11); rel(g_shared11);
    g_dev11 = nullptr;
}
}   // namespace mgpu::dx11probe
