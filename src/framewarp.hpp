// ---------------------------------------------------------------------------
// framewarp.hpp - FW1. Frame Warp: late camera reprojection at present time.
//
// The idea is NVIDIA Reflex 2's Frame Warp, done with our own code. Every frame
// the bridge shows is old by the time it reaches the screen: the game rendered
// it, handed it over, and the neural card spent tens of milliseconds on it
// (transport, neural rendering, upscaling, frame generation's hold-back). The
// mouse kept moving during all of that.
//
// Right before a frame is presented, FW1 adds up the mouse movement since the
// moment that frame's content was made, turns it into a camera rotation (yaw
// and pitch, through the game's field of view) and re-projects the picture by
// that rotation on the neural card. Turning the camera then shows up within the
// next displayed frame instead of after the whole pipeline. The edge the turn
// uncovers is filled by stretching the border, which is the "swimming edge"
// Reflex 2 shows too.
//
// What it cannot do: movement, shooting, animation and anything else that
// needs a newly rendered frame still arrive with the full delay. The game's own
// HUD is part of the picture and moves with it during a turn.
//
// Mouse input comes from a low-level mouse hook on its own thread, counted only
// while a window of this process is in front. The hook never consumes or
// changes input. Warping is skipped whenever the mouse cursor is visible
// (menus, inventory, the ReShade overlay), because then the mouse is moving a
// pointer, not the camera.
//
// Settings (panel; mgpu.ini):
//   FrameWarp=0/1          off by default
//   FrameWarpScale=50      thousandths of a degree the camera turns per mouse
//                          count - matches the game's sensitivity; calibrate
//                          in the panel
//   FrameWarpInvertY=0     1 if the game's vertical look is inverted
//   FrameWarpLead=0        extra ms of mouse history to include (the game's own
//                          input-to-handover time), 0-60
//   FSRFov                 the game's horizontal FOV (shared with frame gen)
// ---------------------------------------------------------------------------
#pragma once

#include <d3d12.h>

namespace mgpu
{
namespace framewarp
{

// ---- the present path (present_frame, under its present lock) ----
// bb is the bridge back buffer in COPY_DEST holding the frame just copied in;
// it is left in COPY_DEST. content_qpc = the QPC time the shown picture's
// content belongs to (0 = unknown: not warped). fov_h_deg = horizontal FOV.
// Returns true when the frame was warped.
bool apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
           unsigned w, unsigned h, DXGI_FORMAT fmt, unsigned long long content_qpc, float fov_h_deg);

void shutdown();          // stops the mouse hook
void release_gpu_now();   // after the present queue has been drained (teardown)
void emergency_unhook();  // REV: DllMain live unload - remove the mouse hook, join nothing

// ---- settings (panel; saved to mgpu.ini) ----
bool  enabled();      void set_enabled(bool on);
int   scale();        void set_scale(int milli_deg_per_count);   // 1..500
bool  invert_y();     void set_invert_y(bool on);
int   lead_ms();      void set_lead_ms(int ms);                  // 0..60
int   strength();     void set_strength(int pct);                // FW5: 0..100, default 20

// ---- FW2: calibration (panel button) ----
// Click Calibrate, then turn the camera left and right steadily for a few
// seconds while standing still. The mod reads the game's motion vectors in a
// band near each edge and in the centre: during a pure turn the edges move
// faster than the centre by exactly 1 + (x * tan(FOV/2))^2, which gives the
// field of view, and the centre's movement against the mouse's gives the
// degrees per mouse count. Both are saved (FrameWarpFov, FrameWarpScale).
const unsigned CALIB_COLS = 6, CALIB_ROWS = 24;
const unsigned CALIB_SAMPLES = 3u * CALIB_COLS * CALIB_ROWS;   // left, centre, right bands
void calib_start();
void calib_cancel();
bool calibrating();
// FW3: whether the stream should sample motion vectors this frame (a manual
// calibration, or Frame Warp on with automatic calibration).
bool sampling();
bool auto_calibrate();   void set_auto_calibrate(bool on);   // FrameWarpAuto, default on;
                                                              // moving the degrees slider by hand turns it off
// Bridge thread, once the frame's list has run: CALIB_SAMPLES float2 motion
// vectors (band-major), the factor that turns a vector's x into a fraction of
// the screen width, the game frames the vectors cover, and the hand-over times
// of this frame and the previous one.
void calib_frame(const float *mv, float to_uv_x, unsigned mv_el,
                 unsigned long long cap_qpc, unsigned long long prev_cap_qpc);
void calib_status(char *out, unsigned n);

// The field of view Frame Warp uses and where it came from:
// "game" (Streamline), "calibrated", or "FSRFov".
float fov_in_use(float fsr_fov, const char **source);

// ---- status (panel) ----
// One line: whether mouse movement is being seen, frames warped, the last
// warp angle, and why frames were skipped.
void status(char *out, unsigned n);

} // namespace framewarp
} // namespace mgpu
