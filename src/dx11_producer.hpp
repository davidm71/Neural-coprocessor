// MGPU Bridge - the DX11 producer (0.3.0 R&D, R227; DX11_CONTRACT_LEDGER.md
// section 7).
//
// WHAT IT IS. The stream's ring, seals, cross-adapter heap, unpack, NR, SR
// and presenter are D3D12 objects on the GAME'S ADAPTER, not on the game's
// device. A D3D11 title has no D3D12 device, so this module owns one, on
// that adapter, and feeds the EXISTING producer (gpu1::stream_on_finish_
// effects) exactly what the D3D12 path feeds it: a command list, a queue and
// resources on a device whose adapter LUID is the game's. The producer
// derives its device from the colour resource and filters by LUID; both hold.
//
// THE HOP (proved by the probe, R225, TR-5b: zero wait on the game thread):
//   game's D3D11 immediate context  --CopyResource-->  shared NT-handle
//   textures (colour, depth) on the game's D3D11 device  --Signal-->  a
//   D3D11.4 shared fence  ==  its D3D12 twin  --queue Wait-->  our D3D12
//   device on the same adapter, where the producer records its copies into
//   the ring slot from the opened textures, and our queue executes them.
//
// NEVER WAITS ON THE GAME THREAD. The D3D11 side is copies and a Signal. The
// D3D12 side is GPU work only: Wait(fence), execute, Signal(gfence) by the
// producer at the next event, as on D3D12.
//
// REAL VECTORS (MVec=3). When the producer is built, the module tells the
// stream that no real vectors are known yet (R227): an arm that comes before
// any vectors uses the synthetic field (MVec=1) with a warning, for that run.
// The game's vectors reach on_mvec from the calibrator's D3D11 evaluate tap
// (R232) and from the census tap (R254, MVecTap); the first one clears R227
// (stream_note_real_vectors) and MVec=3 stays as requested. Own Reflex: not
// wired to the D3D11 device yet; it does not block NR on screen.
//
// GATES. (1) dllmain calls in only from a D3D11 game device/swap chain
// (get_api() == d3d11); the D3D12 path never reaches this file, and init
// refuses a non-D3D11 native. (2) DX11=1 in mgpu.ini; absent = nothing is
// created and every call site is one load (R226 keeps a DX11 title inert).
// Holds no ReShade type: every interface arrives as a native void*.
#pragma once

namespace mgpu::dx11producer
{
    // on_init_swapchain, D3D11 game chain only. Reads DX11 once. device:
    // ID3D11Device*; swapchain: IDXGISwapChain*. Establishes the game's
    // adapter LUID with the selection (adapter::note_game_luid_d3d11).
    void init(void *device, void *swapchain, unsigned width, unsigned height, unsigned dxgi_format);

    // reshade_finish_effects on the D3D11 game runtime. context: the
    // immediate ID3D11DeviceContext*; back_buffer: ID3D11Texture2D* (the
    // resource behind this frame's rtv); depth: ID3D11Texture2D* from the
    // tap (probe::depth_source()), or 0.
    void on_finish_effects(void *context, void *back_buffer, void *depth);

    // R232 (A1). From the calibrator's D3D11 evaluate tap, mid-frame on the
    // game's thread: context: the ID3D11DeviceContext* the game handed DLSS;
    // resource: ID3D11Resource* (DLSS MotionVectors). Copied on that context
    // into a shared texture made from its description; hopped and copied into
    // the ring slot's MVec region at the next on_finish_effects, on our list,
    // through the stream's own stream_mvec_copy - the D3D12 path's copy.
    void on_mvec(void *context, void *resource);

    // destroy_device of the D3D11 game device: the stream has been shut down
    // first (dllmain mirrors the D3D12 teardown), then this.
    void shutdown();

    // init_swapchain again on a D3D11 chain after init: a resize. The armed
    // stream was sized from the first chain; this build stops the producer
    // for the session and says so, rather than copy a resized surface into
    // a slot sized for the old one.
    void on_chain_rebuilt(unsigned width, unsigned height, unsigned dxgi_format);

    // destroy_device: is this native the D3D11 device the producer holds?
    bool is_game_device(void *device);

    bool on();
}
