// mvec_census.hpp - R253: what a D3D11 title exposes for motion vectors.
//
// DX11_CONTRACT_LEDGER.md section 13. The no-contract vector ladder is
//   rung 1 TAP       the game's own velocity target (needs: one exists)
//   rung 2 CAMERA    vectors from the game's camera, per-title profile
//                    (needs: the constant buffer that carries it)
//   rung 3 ESTIMATE  motion estimation from the transported colour
// This module answers the two "needs" by watching, not by touching:
//   RT census  every two-channel render-target texture of screen size
//              (created, bound as a render target, destroyed), with how
//              many frames it is bound in and where in the frame.
//   CB census  every buffer the game writes from the CPU (Map/Unmap and
//              UpdateSubresource), with how often per frame and whether
//              the bytes changed; at a fixed frame a short dump of the
//              float content of the most active small buffers, which is
//              where a view/projection matrix shows itself.
// OFF unless MVecCensus=1 (one ini read at init). On, it is observe-only:
// it never binds, copies or creates anything on the game's device.
#pragma once

namespace mgpu::mvec_census
{
    // D3D11 game chain at init. Reads MVecCensus; off = one read, nothing
    // registered. Registers its own ReShade events when on.
    void init(unsigned screen_w, unsigned screen_h);
    bool on();
    // Once per game frame from dllmain's D3D11 finish_effects branch.
    void note_frame();
    void shutdown();

    // R254: THE TAP (rung 1). MVecTap=1 turns the census machinery on even
    // with MVecCensus=0 (no reports) and makes tap_candidate() answer: the
    // live two-channel target bound in the most frames so far, but only if
    // it was bound in the CURRENT game frame (a menu frame that did not write
    // it is "no vectors", not stale ones). 0 = none this frame. The handle is
    // the native ID3D11Resource*, which is what dx11producer::on_mvec takes.
    bool tap_on();
void tap_enable_live();   // R278d: the first-launch learning switches the tap on in-session
    unsigned long long tap_candidate(unsigned *w, unsigned *h, unsigned *fmt,
                                     unsigned long long *frames_bound, unsigned long long *frames_total);
}
