// d3d12_drain_itest.cpp - SF2. Windows-only integration test: runs the
// bridge's real teardown drain (gpu_safety_d3d12.hpp) against a real D3D12
// device (WARP, the software adapter - no game, no second GPU needed).
//
//   1. a queue stuck on a wait          -> bounded wait, "not finished", keep
//   2. the same queue released          -> "finished"
//   3. the shared-fence trap (R28-B)    -> a value read after a CPU-side
//                                          advance would lie; read first
//   4. repeated timeouts                -> every one bounded, none hangs
//   5. device removed                   -> "removed", nothing waited for
//
// Exit code 0 = all passed. Prints "SKIP" and exits 0 if no D3D12 device.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include "gpu_safety_d3d12.hpp"

using namespace mgpu::gpusafe;
static int g_n = 0, g_fail = 0;
#define CHECK(x) do { ++g_n; if (!(x)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

int main()
{
    IDXGIFactory4 *fac = nullptr; IDXGIAdapter *warp = nullptr; ID3D12Device *dev = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&fac)) ||
        FAILED(fac->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void **)&warp)) ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev)))
    {
        std::printf("d3d12_drain_itest: SKIP (no D3D12 WARP device)\n");
        rel(warp); rel(fac); return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q = nullptr; ID3D12Fence *work = nullptr, *gate = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&q)));
    CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&work)));
    CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&gate)));
    if (!q || !work || !gate) { std::printf("d3d12_drain_itest: setup failed\n"); return 1; }

    // 1. the queue is stuck: it waits on `gate`, which nobody signals yet
    CHECK(SUCCEEDED(q->Wait(gate, 1)));
    CHECK(SUCCEEDED(q->Signal(work, 1)));
    {
        const DWORD t0 = GetTickCount();
        const drain_r r = drain_fence(work, 1, 100, 100, dev);
        const DWORD ms = GetTickCount() - t0;
        CHECK(!r.drained); CHECK(!r.removed);
        CHECK(ms >= 150 && ms < 2000);                        // bounded: first + extension, no hang
        CHECK(!proven(queue_state{ true, r.drained, r.removed }));   // -> resources kept
        // LR100: the variant that asks the fence for its device (capture, MV)
        const drain_r rs = drain_fence_self(work, 1, 50, 0);
        CHECK(!rs.drained); CHECK(!rs.removed);
    }
    // 3. the shared-fence trap: read GPU 0's answer FIRST, then the CPU advance
    {
        const bool before = work->GetCompletedValue() >= 1;  // SF2 order
        CHECK(!before);
        // (A CPU Signal(work, 1) here - what R28-B does to the shared fence -
        //  would make a later read say "done" while the queue is still stuck.
        //  Not done here: it would hide test 2.)
        CHECK(!fence_is_evidence(true));
    }
    // 4. repeated timeouts: each bounded, the setting never flips
    {
        bool drain = true;
        for (int i = 0; i < 3; ++i)
        {
            const DWORD t0 = GetTickCount();
            const drain_r r = drain_fence(work, 1, g0_wait_ms(drain, false) / 40, 0, dev);   // 50 ms each
            CHECK(!r.drained);
            CHECK(GetTickCount() - t0 < 1000);
            drain = next_drain_enabled(drain, true);
            CHECK(drain);
        }
    }
    // 2. release the queue: it now executes the Signal
    CHECK(SUCCEEDED(gate->Signal(1)));
    {
        const drain_r r = drain_fence(work, 1, 2000, 0, dev);
        CHECK(r.drained); CHECK(!r.removed);
        CHECK(proven(queue_state{ true, r.drained, r.removed }));
        const drain_r rs = drain_fence_self(work, 1, 2000, 0);   // LR100
        CHECK(rs.drained); CHECK(!rs.removed);
    }
    // nothing pending: returns at once
    {
        const drain_r r = drain_fence(work, 1, 2000, 0, dev);
        CHECK(r.drained);
    }
    // 5. device removed (ID3D12Device5::RemoveDevice, if the runtime has it)
    ID3D12Device5 *d5 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device5), (void **)&d5)) && d5)
    {
        CHECK(SUCCEEDED(q->Wait(gate, 2)));
        CHECK(SUCCEEDED(q->Signal(work, 2)));
        d5->RemoveDevice();
        const DWORD t0 = GetTickCount();
        const drain_r r = drain_fence(work, 2, 2000, 2000, dev);
        CHECK(r.removed);
        CHECK(!r.drained);                                    // removal is not reported as "finished"
        const drain_r r2 = drain_fence(work, 1, 0, 0, dev);   // even for a value the fence had passed
        CHECK(r2.removed); CHECK(!r2.drained);
        const drain_r r3 = drain_fence_self(work, 2, 2000, 0);   // LR100: the fence's own device says "removed"
        CHECK(r3.removed); CHECK(!r3.drained);
        CHECK(GetTickCount() - t0 < 1500);                    // removal is noticed, not waited out
        CHECK(proven(queue_state{ true, r.drained, r.removed }));
        rel(d5);
    }
    else std::printf("d3d12_drain_itest: (device-removal case skipped: no ID3D12Device5)\n");

    rel(gate); rel(work); rel(q); rel(dev); rel(warp); rel(fac);
    std::printf("d3d12_drain_itest: %d/%d passed\n", g_n - g_fail, g_n);
    return g_fail == 0 ? 0 : 1;
}
