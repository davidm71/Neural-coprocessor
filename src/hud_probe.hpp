#pragma once

// HUD1 - the HUD-less frame search (FG-test build only).
//
// Frame generation interpolates the whole presented image, HUD included, so
// on-screen text warps between real frames. The fix is to give the generator
// the scene WITHOUT the HUD. UE draws the tone-mapped scene first and the UI
// (Slate/UMG) on top of it, so a HUD-less image exists for a moment every
// frame - but which resource holds it, and at what barrier, has to be found
// on the rig, not guessed.
//
// This file finds it. OFF unless mgpu.ini has HudProbe=1. When on:
//   - catalogues every colour texture at exactly the display resolution
//     (and the swapchain back buffers),
//   - CTRL+ALT+J (or the panel button) records ONE frame: every barrier on
//     those resources goes to the log, and the texture's contents at each of
//     those moments is saved as a half-resolution BMP under
//     <addon dir>\mgpu\hudprobe\, next to the final frame (with HUD).
// Looking at the pictures says which one is the scene without the HUD; the
// log line with the same number says how to catch it every frame.
//
// Holds no ReShade types; implemented in hud_probe.inl, compiled as part of
// probe.cpp so no build file changes.

namespace mgpu
{
namespace hudprobe
{
// Reads HudProbe= from mgpu.ini and registers the events. Call once, on the
// thread ReShade raises events on (the probe's start block in dllmain).
void start();
void stop();
bool enabled();

// From on_reshade_finish_effects, GAME runtime only. Both pointers are the
// ReShade types (effect_runtime *, command_list *) passed as void *.
void note_frame(void *effect_runtime_ptr, void *command_list_ptr);

// HL1. dllmain installs the transport's copy (gpu1 stream_hud_copy). Called
// on the game's recording thread with the scene texture in COPY_SOURCE.
void set_copy_hook(void (*fn)(void *cmd_list_native, unsigned long long resource));

// Hotkey / panel. Records the next whole frame.
void request_capture();

// 0 idle, 1 waiting for the frame, 2 recording, 3 waiting for the GPU,
// 4 writing files. For the panel.
int state();
// Number of the last finished capture (0 = none yet), and how many images it wrote.
unsigned last_capture_id();
unsigned last_capture_images();
}
}
