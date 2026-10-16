// ---------------------------------------------------------------------------
// capture.hpp - SS1 screenshots, VR1 video recording, BM1 benchmark.
//
// All three take what the BRIDGE shows - the neural output, generated frames
// included, split view included, the FPS HUD excluded - from one tap in
// present_frame, right after the frame is copied into the bridge's back
// buffer. That matters: tools that hook the GAME (the NVIDIA overlay, Steam,
// OBS "Game Capture") record the render card's raw frames, with no neural
// rendering and no generated frames.
//
//   SS1  one PNG of the shown frame, written off the frame path.
//   VR1  an MP4 (H.264 on the neural card's hardware encoder via Media
//        Foundation, AAC audio). Audio is the GAME's own sound (Windows
//        process loopback, Windows 10 2004+), else everything the PC plays.
//        Stops on the hotkey again or after RecordSeconds.
//   BM1  a timed frame-time run: every shown frame's present time, the real
//        (not generated) frames' pacing, game and neural frame rates, and the
//        settings it ran with. Writes a per-run CSV and summary, and appends one
//        line to benchmarks\results.csv so runs with different settings line up.
//
// Files go to <add-on folder>\mgpu\captures\ (screenshots, videos) and
// <add-on folder>\mgpu\benchmarks\.
//
// Media Foundation and the audio activation are loaded at run time, so a
// Windows "N" edition without the Media Feature Pack still loads the add-on;
// recording then says it is unavailable.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d12.h>

namespace mgpu
{
namespace capture
{

// ---- triggers (any thread) ----
void request_screenshot();
void toggle_recording();
void toggle_benchmark();
void shutdown();                 // finishes a recording / benchmark in progress

// ---- the present path (present_frame, under its present lock) ----
// bb is the bridge back buffer in COPY_DEST holding the frame just copied in;
// it is left in COPY_DEST. `generated` = an in-between frame.
// cap_qpc: QPC time the game handed this frame to the mod (0 = unknown or a
// generated frame) - the benchmark's latency figure for real frames.
void on_present_copy(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                     unsigned w, unsigned h, DXGI_FORMAT fmt, bool generated,
                     unsigned long long cap_qpc);
// After the FPS HUD has been drawn: bb in RENDER_TARGET, left there. Captures
// set to include the HUD (ScreenshotHud=1, the default; RecordHud=1) are taken here.
void on_present_final(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
                      unsigned w, unsigned h, DXGI_FORMAT fmt);
// After the list carrying that copy was signalled with `value` on `fence`.
void on_present_submitted(ID3D12Fence *fence, unsigned long long value);
// After Present returned success (benchmark timing).
void on_present_shown();

// ---- settings (panel; saved to mgpu.ini) ----
int  record_seconds();   void set_record_seconds(int s);   // 0 = until stopped
int  record_fps();       void set_record_fps(int f);       // 0 = every frame shown
bool record_audio();     void set_record_audio(bool on);
int  bench_seconds();    void set_bench_seconds(int s);
int  bench_delay();      void set_bench_delay(int s);
bool bench_replay();     void set_bench_replay(bool on);   // BM2: play the recorded input path
bool screenshot_hud();   void set_screenshot_hud(bool on); // screenshots include the FPS HUD (default on)
bool record_hud();       void set_record_hud(bool on);     // video includes the FPS HUD (default off)

// ---- status ----
int  recording_state();  // 0 idle, 1 starting, 2 recording, 3 finishing
int  bench_state();      // 0 idle, 1 countdown, 2 measuring
// One HUD line (font-safe: A-Z 0-9 . - : / space), empty when there is nothing to say.
void hud_line(char *out, unsigned n);
// The last result or file, for the panel.
void last_message(char *out, unsigned n);
// <add-on folder>\mgpu\ (ends with a backslash).
bool base_folder(wchar_t *out, unsigned n);

} // namespace capture
} // namespace mgpu
