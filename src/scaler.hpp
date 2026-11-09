// ---------------------------------------------------------------------------
// scaler.hpp - SC1. Spatial upscaling of the finished picture to the monitor.
//
// When the game runs at a resolution below the monitor's (e.g. windowed
// 1708x961 on a 1440p panel) and the bridge window fills the monitor
// (Window=FIT), the picture used to be stretched by DXGI with a plain bilinear
// filter. With Scaler on, the present chain is sized to the MONITOR instead and
// the mod enlarges each displayed frame (generated ones included) itself, on
// the neural card, with one of:
//
//   1 bilinear  - a plain compute resize (the old look, for comparison)
//   2 FSR 1     - AMD FidelityFX Super Resolution 1: EASU + RCAS sharpening
//   3 NIS       - NVIDIA Image Scaling: scaler + built-in sharpening
//
// Both FSR 1 and NIS are spatial and open source (MIT, src/third_party); they
// are compiled at run time from embedded source - no DLL. They need no motion
// vectors or jitter, so every frame (real or generated) is scaled the same way.
//
// Settings: Scaler (0 off / 1 bilinear / 2 FSR 1 / 3 NIS), ScalerSharpness
// (0-100). Whether the chain is sized to the monitor is decided when the
// present window is fitted (once per stream): 0 vs non-0 needs a restart; the
// filter and sharpness switch live.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d12.h>

namespace mgpu
{
namespace scaler
{

int  mode();            void set_mode(int m);          // 0..3
int  sharpness();       void set_sharpness(int pct);   // 0..100
// MAS1: motion-adaptive sharpening. mas_motion: % of the sharpening kept on
// fast-moving pixels (8+ px per frame); still pixels keep all of it.
int  mas();             void set_mas(int on);
int  mas_motion();      void set_mas_motion(int pct);   // 0..100
// NH1: NIS in half precision (NIS_USE_HALF_PRECISION=1, min16float). Live.
// The half pipeline is compiled the first time it is wanted, in the background
// (NH1b/c); full precision runs until it is ready.
// half_state: 0 not compiled yet, 1 half pipeline unavailable, 2 built but the
// card does not report 16-bit min precision (runs as full), 3 in effect,
// 4 being prepared.
int  half();            void set_half(int on);
int  half_state();

// MAS1. Neural stage's list, while the motion vectors are a shader resource:
// builds the motion mask (fastest motion per 16x16 cell, in display pixels per
// frame). to_px: multiplies a motion vector into display pixels. Does nothing
// when MAS is off.
void mas_record(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *mv, DXGI_FORMAT mv_fmt,
                unsigned mv_w, unsigned mv_h, float to_px_x, float to_px_y, unsigned src_w, unsigned src_h);

// Present thread. src is the finished frame (src_state on entry and exit),
// bb the back buffer in COPY_DEST (left in COPY_DEST). Records the enlargement
// of src (sw x sh) to bb (dw x dh). Returns false (nothing recorded) when the
// scaler is not ready - the caller then falls back to its plain copy.
bool run(ID3D12Device *dev, ID3D12GraphicsCommandList *cl,
         ID3D12Resource *src, D3D12_RESOURCE_STATES src_state, unsigned sw, unsigned sh,
         ID3D12Resource *bb, unsigned dw, unsigned dh, DXGI_FORMAT fmt, unsigned bb_index);

// MC1. Present thread, after run() and Frame Warp: draw the game's pointer onto
// the enlarged picture at the enlarged position (cursormap.hpp). bb is in
// COPY_DEST on entry and exit. sw/sh: the game's frame size; dw/dh: bb's.
void draw_cursor(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                 unsigned dw, unsigned dh, unsigned sw, unsigned sh, DXGI_FORMAT fmt);

void release();         // after the present queue has drained (teardown)

} // namespace scaler
} // namespace mgpu
