#pragma once
// ---------------------------------------------------------------------------
// tstab.hpp - TC1: temporal stabilisation of the neural output (OFF by
// default).
//
// WHERE IT RUNS
//   In the neural stage's command list, on the neural card, right after the
//   last NR pass (and Color Strength) and BEFORE anything reads the result:
//   the liveness sample, frame generation (mod / AMD / NVIDIA), the HUD and the
//   present. That list runs once per REAL game frame, so generated in-between
//   frames can never feed the history - only real NR outputs advance it.
//
// WHAT IT DOES
//   Keeps a history of its own (RGBA16F + depth, on the neural card),
//   reprojects it with the game's motion vectors (the same convention and
//   scale as the built-in frame generator: a vector points from the current
//   pixel to where it was in the previous frame, times the game frames that
//   elapsed between the two NR outputs), and checks it before use:
//     - off-screen reprojection          -> rejected
//     - depth mismatch (disocclusion)     -> rejected (when depth is available)
//     - colour outside the current 3x3 neighbourhood (variance box, YCoCg)
//                                         -> clipped into it; far outside = rejected
//     - fast motion                        -> less history
//   History is reset on the first frame, after a gap (a real frame the stage
//   did not see), when motion vectors are missing, when NR itself resets, after
//   a long frame gap, and after a frame where most pixels were rejected (a
//   camera cut). DLSS-NR is already temporal, so the default strength is low
//   and the history is clipped hard - it trims frame-to-frame shimmer, it is
//   not a second TAA.
//
// MODES (mgpu.ini TemporalStab, live in the panel)
//   0  off      - nothing is created, no GPU work.
//   1  observe  - history, reprojection, validation and statistics run; the
//                 picture is NOT changed. Measures the shimmer that is there.
//   2  stabilise- the same, and the blended result replaces the NR output.
//   TemporalStabStrength  0-100 (default 30): the most history any pixel gets
//                         (100 = 90 %).
//   TemporalStabDebug     1: show why history was rejected (red off-screen,
//                         green depth, blue colour; grey = history weight).
//
// The log line [MGPU][TC1] every ~10 s: GPU time, reject rates, the shimmer
// measure (mean luminance change of accepted pixels against their reprojected
// history, in 1/1000 of full scale) and how much the picture was changed.
// Holds no gpu1_context type: it is handed D3D12 objects and plain values.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <d3d12.h>

namespace mgpu::tstab
{
// What the neural stage hands over for one real frame.
struct frame_in
{
    ID3D12Device              *dev = nullptr;
    ID3D12GraphicsCommandList *cl  = nullptr;
    ID3D12CommandQueue        *q   = nullptr;   // the neural queue (timestamp frequency)
    ID3D12Fence               *fence = nullptr; // signalled after this list ...
    unsigned long long         fence_value = 0; // ... with this value
    unsigned                   parity = 0;      // 0/1: descriptor set
    ID3D12Resource *out = nullptr;              // the NR output, UNORDERED_ACCESS on entry and exit
    ID3D12Resource *mv  = nullptr;              // NON_PIXEL_SHADER_RESOURCE, or null
    DXGI_FORMAT     mv_fmt = DXGI_FORMAT_UNKNOWN;
    unsigned        mv_w = 0, mv_h = 0;
    float           mv_sx = 0.0f, mv_sy = 0.0f; // MV -> MV-texture pixels, sign and elapsed frames applied
    float           mv_el = 1.0f;               // game frames between the two NR outputs
    ID3D12Resource *depth = nullptr;            // NON_PIXEL_SHADER_RESOURCE, or null
    unsigned        d_w = 0, d_h = 0;
    bool            d_inverted = false;
    bool            nr_reset = false;           // NR's own history was reset this frame
};

// The neural stage, once per real frame, at the point described above. `run`
// false: this real frame is not processed (NR did not write, profile, present
// mode...) - the history is then stale and the next frame starts fresh.
void frame(bool run, const frame_in &in);

// From stream_release, when the GPU no longer uses the stream's resources.
void release();

// Panel / ini. Live; a mode change resets the history.
int  mode();              void set_mode(int m);          // 0..2
int  strength();          void set_strength(int pct);    // 0..100
int  debug();             void set_debug(int on);        // 0/1

struct stats
{
    bool   active = false;      // running this arm
    bool   failed = false;      // turned itself off (see the log)
    float  gpu_ms = 0.0f;       // mean GPU time of the stage
    float  shimmer = 0.0f;      // mean |dY| of accepted pixels vs reprojected history, x1000
    float  changed = 0.0f;      // mean |dY| between the output and NR's own, x1000 (0 in observe)
    float  weight = 0.0f;       // mean history weight, %
    float  rej_off = 0.0f, rej_depth = 0.0f, rej_color = 0.0f;   // % of pixels
    unsigned resets = 0, cuts = 0;
    unsigned long long frames = 0;
};
stats snapshot();
}
