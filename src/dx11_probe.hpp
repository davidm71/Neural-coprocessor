// MGPU Bridge - DX11 transport-hop probe (0.3.0 R&D, DX11_CONTRACT_LEDGER.md).
//
// WHAT IT ANSWERS. The stream's producer records a copy on the GAME's D3D12
// queue straight into the cross-adapter heap. A D3D11 title has no D3D12
// queue and no cross-adapter heap, so a D3D11 producer needs one extra hop:
//   game's D3D11 context -> a shared NT-handle texture we own on the game's
//   D3D11 device -> opened on a D3D12 device of ours on the SAME adapter ->
//   from there the existing path (cross-adapter heap, seal, unpack, NR).
// This probe builds that hop and nothing else: no GPU 1, no stream. It
// copies the back buffer across the hop every SAMPLE_EVERY presents, reads
// the pixels back on BOTH sides (D3D11 staging, D3D12 readback), hashes
// them and logs whether they match. Pass = the hop is proven and the
// producer gets its D3D11 branch. Fail = the HRESULT names the step.
//
// NEVER WAITS ON THE GAME THREAD. The D3D11 side is two CopyResource calls
// and a fence Signal on the immediate context. The D3D12 side is GPU work
// only (queue Wait on the shared fence, copy, Signal); the CPU looks at the
// readback at the NEXT sample, and only if our fence says it is done. The
// staging Map uses DO_NOT_WAIT.
//
// Holds no ReShade type: every interface arrives as a void* native.
// KEY. Dx11Probe=1 in mgpu.ini, read once at the first swap chain. Off (or
// absent): every call site is one load; nothing is created.
#pragma once

namespace mgpu::dx11probe
{
    // on_init_swapchain, D3D11 device only. Reads the key on the first call.
    // device: ID3D11Device*; swapchain: IDXGISwapChain*.
    void init(void *device, void *swapchain, unsigned width, unsigned height, unsigned dxgi_format);

    // on_present, D3D11 only. context: ID3D11DeviceContext* (the immediate
    // context); back_buffer: ID3D11Texture2D* (the current one).
    void on_present(void *context, void *back_buffer);

    // destroy_swapchain of the chain it was built on (is_chain) / destroy_device:
    // releases everything. R228: never for another chain - the bridge's own
    // present chain is resized at arm, and that resize must not tear this down.
    void shutdown();
    bool is_chain(void *swapchain);

    bool on();
}
