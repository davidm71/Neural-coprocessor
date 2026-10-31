// ---------------------------------------------------------------------------
// hdr.hpp - HD1: HDR output. The game's SDR picture is turned into HDR10
// (PQ, Rec.2020) on the neural card, as the very last step before the
// picture is presented, so the scaler, Frame Warp, the HUD and the clock are
// all converted together and screenshots/video stay SDR.
//
// Needs Windows HDR on for the monitor the picture is on, and a 10-bit
// picture (R10G10B10A2, which is what Unreal games use). Otherwise it stays
// off and says why in the panel; nothing is changed.
//
// Settings (mgpu.ini, live from the panel):
//   HDR=0/1               off / on
//   HDRPeak=0             brightest highlight in nits; 0 = what the monitor reports
//   HDRPaperWhite=200     brightness of white paper / UI white, nits
//   HDRHighlights=60      how far bright areas are pushed toward the peak, %
//   HDRSaturation=100     colour strength, %
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace mgpu
{
namespace hdr
{
bool enabled();
void set_enabled(bool on);
int  peak();            // the setting: 0 = monitor's
void set_peak(int nits);
int  paper_white();
void set_paper_white(int nits);
int  highlights();
void set_highlights(int pct);
int  saturation();
void set_saturation(int pct);

// Bridge thread, every ~2 s: is Windows HDR on for the monitor showing `wnd`,
// and what peak does it report.
void poll_display(HWND wnd);
bool display_hdr();     // Windows HDR is on for that monitor
int  display_peak();    // nits the monitor reports (0 = unknown)
int  peak_in_use();     // the peak actually used

// Present thread. `want` = this frame is converted. Sets the swap chain's
// colour space to HDR10 or back to SDR when it changes (and after the chain
// is recreated). Returns true when the chain is in HDR10.
bool sync_colour_space(IDXGISwapChain3 *sc, bool want);

// Present thread. bb is in RENDER_TARGET state and is returned in it.
// Converts the whole backbuffer from SDR to HDR10 in place. false = not run
// (off, wrong format, or the pass could not be built).
bool apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
           unsigned w, unsigned h, DXGI_FORMAT fmt);

// Why HDR is not running right now, for the panel ("" when it is).
const char *status();
}
}
