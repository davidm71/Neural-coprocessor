# Neural Coprocessor - davidm71 fork

**A fork of [maohgad-web/Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
(MGPU Bridge) by Marcelo Guibout, based on the 0.2.5 release.**

The add-on runs a game's DLSS Neural Rendering on a second GPU while the first one renders
the game. All credit for that belongs to the upstream project; read its README for how it
works, what it needs, and its disclaimers. This fork keeps every 0.2.5 feature and adds
performance work, smoothness fixes, an FPS HUD, multi-display overlay support and more
panel controls. New features are off unless the shipped `mgpu.ini` turns them on.

Selected 0.3.1 features are backported on top, one commit each: core fixes, startup,
DX11 games, and motion vectors for games without DLSS. See "Backported from 0.3.1" below.

Research code, not a product. Run games with anti-cheat and online games at your own risk.

---

## What the fork adds

**Performance and smoothness**
- **Late-latch** (`LateLatch=1`): while the second card evaluates one frame, the newest
  arrived frame is already being copied to it, so the next evaluate starts without
  waiting for its copy.
- **GPU 0 async staging** (`StageCopy=1`): upstream copies every frame across PCIe
  inside the game's own frame on the render card. The fork copies it to a VRAM buffer
  instead and sends it across PCIe on a separate copy engine in the background.
  Measured: 3.4 ms to 0.7 ms per frame on the render card, +7% game fps.
- **Even frame cadence:** when the second card can't show every game frame, late-latch
  now always takes the newest frame. Before, it could only step 1 or 3 frames, never 2,
  which looked choppy.

**Bug fixes**
- Motion vectors that had arrived were sometimes not used: the per-frame flag was read
  after the game thread had already cleared it, and was reset one frame too early when
  the second card fell behind.
- DLSS Super Resolution on the second card now scales motion vectors by the number of
  frames skipped, like the neural stage already did, and resets its history after long
  gaps.
- Frames lost to a failed submit or wait are counted; fence-wait and teardown hardening.

**Usability**
- **Multi-display overlay:** `DcompOverlay=1` works with more than one active display,
  so you can game on one monitor with a second monitor (for example on the integrated
  GPU) showing stats, with no bridge window over the game and no input loss.
- **FPS HUD** (`CTRL+ALT+H`: basic / detail / off): MGPU IN, CONSUMED, NR EVAL, NR SKIP,
  PRESENT, NR GPU ms.
- **Panel controls** (ReShade overlay, `Home`, MGPU panel): late-latch on/off and age,
  Reflex off/on/boost, Reflex sleep, bridge vsync, copy queue mode, GPU 0 staging, ring
  window and overlay mode. All write `mgpu.ini`.

**Frame generation on the second card** (`FrameGen`, panel, `CTRL+ALT+G`)
- In-between frames are made on the neural card from the game's motion vectors and depth,
  so the render card does no extra work. Three generators, switchable live:
  `FrameGen=1` the mod's own warp, `3` AMD FSR 3 frame generation (needs
  `amd_fidelityfx_framegeneration_dx12.dll` in the `mgpu` folder), `4` NVIDIA DLSS Frame
  Generation (RTX 40/50 neural card; uses the driver's own model, nothing to download).
- NVIDIA mode does 2x to 6x (`FGMultiplier`, also in the panel), as far as the neural card's
  driver allows (RTX 40: 2x; RTX 50: 4x, 6x on newer drivers - the panel greys out the rest). If a generator cannot
  start, or fails three times, the mod's own takes over and the panel says so in red.
- **HUD-less mode** (`HudLess=1`, D3D12): frame generation runs on the scene without the
  HUD and the HUD is laid back on top, so text and markers never warp.
  Full-screen menus are recognised (most of the screen differs from the scene) and shown as
  the game draws them, so they are never see-through.
- NVIDIA mode's camera data is estimated: set `FSRFov` to the game's horizontal FOV if
  nearby objects wobble in generated frames.
- **Cap the game's frame rate for 3x/4x** - see Recommended setup.

**Screenshots, video and benchmark** (panel section of the same name)
- They capture what the bridge shows: neural output and generated frames, no HUD. Tools
  that hook the game (NVIDIA overlay, Steam, OBS Game Capture) only see the render card's
  raw frames.
- **Screenshot** (`CTRL+ALT+P`): a PNG in `mgpu\captures\`.
- **Video** (`CTRL+ALT+R` start/stop): MP4, H.264 on the neural card's hardware encoder,
  with the game's own audio. Length limit (`RecordSeconds`) and frame rate (`RecordFps`,
  default every frame shown) are in the panel.
- **Benchmark** (`CTRL+ALT+B`): a timed run (countdown, then `BenchSeconds`). It reports:
  - average fps, 1% and 0.1% lows, frame-time percentiles and stutters;
  - real-frame pacing, i.e. how evenly the non-generated frames arrive;
  - the latency the mod adds, from the game handing over its frame to the neural frame
    on screen;
  - game and neural-card fps.
  Each run is saved with its settings and appended to `mgpu\benchmarks\results.csv`,
  so settings can be compared side by side.
- **Input replay for repeatable runs** (`BenchReplay=1`): record a path once (`CTRL+ALT+I`
  start/stop) and every benchmark replays the same keyboard and mouse input. Turn off
  Windows "Enhance pointer precision" for exact mouse turns; single-player games only.

**Frame Warp** (`FrameWarp=1`, panel section of the same name)
- **What it does:** the idea of NVIDIA Reflex 2's Frame Warp, with the mod's own code. Right
  before each frame is shown, the second card shifts the picture by the mouse movement made
  since that frame was rendered, so camera turns show up in the next displayed frame instead
  of after the bridge's delay.
- **Strength** (slider, default 20%): how much of the delay to make up. The bridge's delay is
  about 100 ms, and making all of it up means sliding the picture far, with a wide stretched
  edge; a part of it gives quicker-feeling turns with little distortion. Each warp is also
  limited to 3 degrees.
- **Limits:** only camera turning gets faster; walking, shooting and animation still take the
  full delay. The screen edges stretch while turning, and the game's HUD moves with the picture
  during a turn. It pauses in menus by itself: when the mouse moves but the picture does not
  turn, it is a menu (this works with games that draw their own cursor).
- **Auto-calibrate** (checkbox, on by default): while Frame Warp is on, clean camera turns are
  measured and the field of view and degrees per mouse count update and save themselves. Once
  they settle, sampling pauses and re-checks every 5 minutes. The last values are kept for the
  next launch.
- **Calibrate** (button): measures both now. Click it, close the overlay so the mouse turns the
  camera, then stand still and turn left and right steadily for about 5 seconds. The values
  are measured from the game's own motion vectors and saved.
- **Fine-tuning by hand:** the "degrees per mouse count" slider. Turn and stop: if the view
  springs back after you stop, lower it; if it still lags behind the mouse, raise it. Moving
  the slider switches auto-calibrate off so your value stays.
- The game's own field of view is used instead when it reaches the mod through Streamline
  (`SLTags=3`). Turn off Windows "Enhance pointer precision" for best results.

**Crash catcher**: if the game ever crashes to the desktop, the add-on writes one line naming
what crashed (the module, the place inside it and the call chain) to `mgpu\crash.txt` and to
ReShade.log as `[MGPU][CR1]`, even when Windows and the game record nothing. Send that line with a
bug report. It never changes how an error is handled.

**Hotkeys**: all of them can be changed in the panel by clicking one and pressing the new
combination. They only work while the game or the bridge window is in front, so they no
longer clash with other programs. The game never sees the key of a CTRL/ALT combination of
ours (CTRL+ALT+H no longer also presses H in the game). The panel flags a combination that another program also
uses.

**Image, compatibility and status**
- **Color Strength** (`ColorStrength=0.00-1.00`, panel slider under Intensity): keeps the
  neural pass's brightness and relighting but restores the game's own hue and saturation,
  so skin, cloth and foliage keep their palette. 1.00 = unchanged and costs nothing; the
  pass is one small compute shader on the second card.
- **Present-time fallback hook** (`HookPoint`): if the game's ReShade runtime never runs an
  effect pass (the bridge never arms, log `[R138]`), the bridge takes the frame at present
  instead after 1800 frames. `HookPoint=1` turns the fallback off, `HookPoint=2` always
  uses present.
- **Status readout** (`StatusHud=1`, or the panel checkbox): adds neural state, neural frame
  count, last NGX result code, last NGX error, frame generator in use and the hook to the
  FPS HUD, for screenshots in bug reports. Off by default.

Full details and measurements: [FORK_NOTES.md](FORK_NOTES.md).

---

## Backported from 0.3.1

Added feature by feature to the 0.2.5 fork (the fork was not rebased onto 0.3.1). The git
history has one commit per stage:

1. **Core fixes.** The GPU 0 teardown drain now actually runs (R275, `DrainGpu0`), NGX init
   through Init_Ext (R274/R255/R256), NGX core locator (R240), wait for the game's own NGX
   init (R239), and the old-msvcp140 mutex crash switch in CMake.
2. **Startup.** DLSS 5 starts after the game's own DLSS is running (`NgxProbeAfterScene`),
   retry after an OutOfDate init (R247-R251), bounded wait for depth (`DepthHoldMax`), and
   the vector hold at arm.
3. **DX11 games** (`DX11=1`). The D3D11 producer feeds the same stream through a D3D12
   device of its own on the game's card. `DcompForceWindowed` turns exclusive fullscreen
   into borderless when the overlay is on. HUD-less mode is D3D12-only and is switched
   off by itself on DX11.
4. **Motion vectors for games without DLSS.** A D3D11 vector census and tap, a D3D12 tap
   with a per-title learning rule (`MVecTap`, off by default), and the R280 extractor for
   D3D12 engines that keep vectors in a G-buffer (inert on any game that uses DLSS).

5. **FP16 input to NR** (`NRInput16=4`, off by default). NR runs on a 16-bit float copy of
   the frame (about linear light) and the result is converted back. Needs `Passes=1`;
   works with `SRUpscale` on (fork addition: at the reduced SR size) or off.

Not taken from 0.3.1: the display detection and launcher record, OwnReflex, the journal,
stall watch and log queue, and the other experiments (foveated, crop, chained vectors,
game-FG filter).

## Install

Install exactly as upstream 0.2.5 describes (add-on-enabled ReShade 6.8.0+, the
`.addon64`, `mgpu_depth_tap.fx`, your own `nvngx_dlssnr.dll` in a `mgpu` folder next to
the add-on, never beside the game's `.exe`). Then use **this fork's `mgpu.ini`** from
`assets/`, which has the recommended settings below. Every line it changes from
upstream is marked `; FORK`.

## Recommended setup

1. **Turn the game's own DLSS Super Resolution on** in its graphics menu (any quality
   mode). Without it the game produces no motion vectors and Native Upscaling has nothing
   to inherit.
2. Use the shipped `mgpu.ini`. The key lines:
   ```
   CopyQueue=2
   LateLatch=1
   LateLatchAge=2
   StageCopy=1
   SRUpscale=1
   SRScale=0
   SRMvLowRes=2
   Reflex=1
   ReflexSleep=1
   FrameGen=3          ; AMD FSR 3 frame generation (4 = NVIDIA DLSS FG, 1 = the mod's own)
   HudLess=1           ; generated frames keep the HUD sharp (D3D12)
   HookPoint=0         ; auto fallback if the game's ReShade never runs an effect
   ```
   Optional, off in the shipped file: `ColorStrength` below 1.00 (restore the game's
   colours), `StatusHud=1` (status lines for bug reports), `FGMultiplier=3` to `6` (NVIDIA FG
   3x-6x), `NRInput16=4` (FP16 input). `DcompOverlay=1` plays on the neural card's monitor
   with no bridge window (the author's setup, with a second display on the iGPU); the
   shipped file has 0, a normal bridge window.
   `SRUpscale=1` with `SRScale=0` is Native Upscaling: the second card runs neural
   rendering at the game's render resolution and DLSS scales it back up. That is what
   lets a mid-range second card keep up.
3. **With NVIDIA 3x/4x, cap the game's frame rate** (in-game limiter, RTSS or the NVIDIA
   control panel) so the second card processes every game frame and game fps x multiplier
   fits the monitor. On an RTX 5060 Ti at 1440p: about 40 fps for 4x (160 fps shown),
   about 50 for 3x. Uncapped, the second card skips game frames and motion judders even
   at a high fps count. The log's `[R87]` line should read above 90% "at 1".
4. Press `CTRL+ALT+H` for the HUD. **NR SKIP near 0** means the second card is keeping
   up. If it isn't, cap the game a few fps below the NR EVAL figure for an even cadence.
5. Set the game's resolution to match the Windows desktop before launching. The add-on
   captures the game's frame size when it arms; restart the game after any resolution
   change.

## Measured

RTX 3080 Ti (render, PCIe x8) + RTX 5060 Ti (neural, PCIe 4.0 x4), 2560x1440,
RoboCop: Rogue City - Unfinished Business, same spot. Your numbers will differ.

| Configuration | Game fps | NR EVAL | Notes |
|---|---|---|---|
| Upstream 0.2.5-dev, NR at full res | - | ~32 | NR GPU ~21 ms |
| Fork, late-latch, NR at full res | ~70-75 | ~52 | |
| Fork, + SR Native | 71.5 | 71.5 | 0 skips, NR GPU 10.3 ms |
| Fork final, NR at full res + StageCopy | ~74 | ~48 | steps of 1-2 frames only, MVec bound 99.95% |
| Fork final, SR Native + StageCopy | ~79.5 | ~65 | steps of 1-2 frames only, 0 MVec disagreements, 0 errors |

## Building

Visual Studio 2022. This package includes its own `CMakeLists.txt` (upstream 0.2.5's plus
the new source files and the mutex switch); copy it over the upstream one, then

```
cmake -B build -A x64
cmake --build build --config Release
```

`git log` shows the fork baseline, each 0.3.1 backport stage and each fork feature as its
own commit.

## License and credit

MIT, as upstream. The add-on, its design and its 0.2.5 code are Marcelo Guibout's
([maohgad-web](https://github.com/maohgad-web)). This fork's changes are by davidm71.
Nothing from NVIDIA is included, and nothing may be.
