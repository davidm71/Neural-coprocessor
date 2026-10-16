// mvec_extract.hpp - R280: the VIRTUAL VELOCITY TARGET for D3D12 titles
// with no NGX contract. One module, its own keys; nothing downstream knows it.
//
// MVEC_D3D12_LEDGER.md. RE4 (RE Engine) keeps its motion vectors in channels
// x,y of a four-channel R16G16B16A16_SNORM G-buffer target, in UV units,
// written once per frame (RE4-TP3). The rest of the add-on expects what
// Skyrim gives it: a two-channel RG16F scene-sized target in UV units that
// the engine writes every frame. This module MANUFACTURES that target on
// GPU 0 and lets every existing piece see it:
//   1. mid-frame, at the engine's own RT/UAV -> SRV of the G-buffer target
//      (the instant the data is complete - RE4-TP2/TP3), on the engine's
//      command list: barrier, copy into a staging copy of ours, barrier back.
//      Copies and barriers only, never pipeline state (the R83 pattern).
//   2. at finish_effects, on our own command list executed on the game's
//      queue: one compute pass staging(x,y) -> our RG16F target, then its
//      UAV -> SRV barrier on ReShade's list. The target is OFFERED to the
//      probe's census once (ReShade fires no init_resource for what an add-on
//      creates): two-channel, scene-sized, the largest by area, so the rule
//      names it and the R277 tap copies it into the slot before the seal,
//      exactly as on Skyrim. Until it exists the probe's pick is DEFERRED
//      (bounded: 900 frames in which the engine wrote a candidate - menu
//      frames do not count - a build failure, or a contract release it).
// Downstream untouched: probe's census and rule, the transport (4 bytes per
// pixel as today), GPU 1, the warp test (UV candidates), R278's keys, D3D11,
// contract titles.
//
// WHEN IT RUNS. D3D12 game chain, and at init: MVecLearned absent (learning
// possible) or MVecExtract=<format> present (learned). Inert, one atomic per
// handler, the moment the game's NGX init is seen (a contract title) or
// MVecLearned=99 (depth only). Keys: MVecExtract (format number, written by
// this module when its signature settles; absent = decide by signature),
// MVecExtractOff=1 (off, always). Source signature: four-channel 16-bit
// (DXGI 10/11/13), exactly scene size, transitioned into RT or UAV about
// once per frame, alive >= 300 frames, most active.
//
// COST (GPU 0 only, Marcelo 20:55): one local copy of the G-buffer target
// and one full-screen compute per frame. Nothing reaches the bus or GPU 1.
//
// TO REMOVE: these two files, the CMakeLists line, the dllmain lines marked R280.
#pragma once

namespace mgpu::mvec_extract
{
    // D3D12 game chain at init (dllmain). Reads the keys; off = nothing registered.
    void init(void *game_device_native, unsigned screen_w, unsigned screen_h);
    // dllmain's D3D12 finish_effects, BEFORE stream_on_finish_effects. Takes the
    // runtime's command list and queue as void* (this header holds no ReShade
    // type). Runs the extraction for this frame when a staging copy landed.
    void on_finish_effects(void *reshade_command_list, void *reshade_command_queue);
    // Game device destroyed (dllmain). Releases everything, unregisters.
    void shutdown();
    bool on();
}
