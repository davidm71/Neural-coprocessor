// gpu_safety_d3d12.hpp - SF1/SF2. The bounded teardown fence drain, on real
// D3D12 objects, following the rules in gpu_safety.hpp. Header-only so that
// tests/d3d12_drain_itest.cpp runs the very code the bridge runs.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include "gpu_safety.hpp"

namespace mgpu
{
namespace gpusafe
{
struct drain_r { bool drained; bool removed; };

// Waits for `v` on `f`: `first_ms`, then `ext_ms` more unless finished or the
// device is gone. The event is closed only when the fence passed or the device
// is gone (rule 3) - otherwise the driver may still set it, so it is kept.
// SF2: every reading goes through fence_now(), which checks removal (the
// UINT64_MAX sentinel, or GetDeviceRemovedReason) BEFORE "reached" - a removed
// device's fence passes every target, and must not be reported as finished.
inline drain_r drain_fence(ID3D12Fence *f, UINT64 v, DWORD first_ms, DWORD ext_ms, ID3D12Device *dev)
{
    if (f == nullptr) return drain_r{ true, false };
    auto now = [&]() {
        const UINT64 c = f->GetCompletedValue();
        return fence_now(c, v, dev != nullptr && FAILED(dev->GetDeviceRemovedReason()));
    };
    auto as_r = [](fence_read st) { return drain_r{ st == fence_read::reached, st == fence_read::removed }; };
    fence_read st = now();
    if (st != fence_read::pending) return as_r(st);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr || FAILED(f->SetEventOnCompletion(v, ev)))
    {
        if (ev != nullptr) CloseHandle(ev);   // never registered: safe to close
        const unsigned long long t0 = GetTickCount64();
        while (GetTickCount64() - t0 < (unsigned long long)first_ms + ext_ms)
        {
            st = now();
            if (st != fence_read::pending) return as_r(st);
            Sleep(1);
        }
        return drain_r{ false, false };
    }
    (void)WaitForSingleObject(ev, first_ms);
    st = now();
    const unsigned ext = extension_ms(st == fence_read::reached, st == fence_read::removed, ext_ms);
    if (ext != 0u)
    {
        (void)WaitForSingleObject(ev, ext);
        st = now();
    }
    const drain_r r = as_r(st);
    if (may_close_event(r.drained, r.removed)) CloseHandle(ev);
    // else: kept on purpose (rule 3).
    return r;
}

// LR100: the same, asking the FENCE for its device. For callers that hold no
// counted reference of their own to a live device (a raw pointer to the
// game's device may be dangling by teardown); a D3D12 object keeps its device
// alive, so the fence's device is always safe to ask.
inline drain_r drain_fence_self(ID3D12Fence *f, UINT64 v, DWORD first_ms, DWORD ext_ms)
{
    if (f == nullptr) return drain_r{ true, false };
    ID3D12Device *dev = nullptr;
    if (FAILED(f->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dev)))) dev = nullptr;
    const drain_r r = drain_fence(f, v, first_ms, ext_ms, dev);
    if (dev != nullptr) dev->Release();
    return r;
}
}
}
