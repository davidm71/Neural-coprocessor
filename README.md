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
  `FrameGen=1` BUILT-IN (the mod's own warp), `3` AMD FSR 3 frame generation (needs
  `amd_fidelityfx_framegeneration_dx12.dll` in the `mgpu` folder), `4` NVIDIA DLSS Frame
  Generation (RTX 40/50 neural card; uses the driver's own model, nothing to download).
- NVIDIA mode does 2x to 6x (`FGMultiplier`, also in the panel), as far as the neural card's
  driver allows (RTX 40: 2x; RTX 50: 4x, 6x on newer drivers - the panel greys out the rest). If a generator cannot
  start, or fails three times, BUILT-IN takes over and the panel says so in red.
  Games with an older Streamline (RoboCop: Rogue City) do not report how many in-between
  frames the driver allows; the mod then tries the multiplier you set and drops to 2x if it
  is refused.
- **Dynamic multiplier** (`FGDynamic=1`, `FGDynTarget` in fps, default 180; "dynamic
  multiplier" in the panel, NVIDIA mode only): instead of a fixed multiplier against a capped
  game, each real frame gets as many in-between frames as it takes to reach the target - at
  67 fps real and 180 shown, about seven frames in ten get 3x and three get 2x (2.69x on
  average), like DLSS 4.5's Dynamic Multi Frame Generation. The game needs no cap. Spacing
  is even within each real frame but changes slightly between them, so a fixed cap ×
  multiplier stays the steadier choice; the log's `[MGPU][DMF1]` line gives the average and
  the mix every 30 s. Target frame rate and auto-tune are off while it is on.
  NVIDIA's generator is always asked for the same number of in-between frames (the most a
  pair needs, changed only after about half a second), and the fraction comes from showing
  fewer of them on some pairs - so its history never sees a count change, which flashed.
- **Even frame clock** (`FGClock=1`, off by default; "Frame spacing: Even clock" under the dynamic
  multiplier - "Per real frame", the default, is the original method; switch either way at any time,
  live; NVIDIA mode with the dynamic multiplier only): instead of spacing the frames of each real
  frame evenly inside it (5.0 ms apart in a pair of 3, 7.5 ms in a pair of 2 at 67 → 180 fps), every
  frame is shown on one steady clock at the shown-fps target (5.56 ms). For each tick the in-between
  frame nearest in scene time is shown, so motion stays at the right speed; the clock follows the
  real frames (it does not run on its own, which is why the earlier global pacing failed), and a
  late tick carries on from where it is, skipping frames that are now stale instead of rushing them.
  In simulation (`tests/fg_clock_test.cpp`) the frame-time spread drops from 1.1-2.7 ms to
  0.02-1.8 ms and motion is steadier, for about one tick (4-7 ms) more delay. Every 30 s
  `[MGPU][FC1]` gives the frame-time spread with the clock on or off, so the two can be compared,
  and what the clock did (late or empty ticks, grid restarts). Best with G-SYNC and bridge vsync off.
- **HUD-less mode** (`HudLess=1`, D3D12): frame generation runs on the scene without the
  HUD and the HUD is laid back on top, so text and markers never warp.
  Full-screen menus are recognised (most of the screen differs from the scene) and shown as
  the game draws them, so they are never see-through. When the game stops drawing its scene
  (loading, exit to the main menu) its frames are also shown as drawn until the scene is back,
  so the menu does not shake. Once the scene texture has been found, the mod follows the
  game's finished scene from frame to frame even when the game keeps moving it to other textures
  (RoboCop does this in action and after loading some saves): each frame it copies whichever
  texture holds the scene (`[MGPU][HL5]`), so neural rendering and frame generation keep running.
  Games that alternate between whole sets of post-process textures are handled by keeping a
  small set of known scene textures and copying each at its own barrier.
  `HudFollow=0` turns that off and keeps only the original 60-frame lock.
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
- **NR network choice** (`NRModel`, panel Model section): NR DLL 310.9.16 carries two
  networks, A (original) and B (smaller, about 2x faster, its default). Pick one, or
  leave it to the DLL. Applies on the next launch.
- **FG Lite** (`FGLite`, panel frame generation section): asks NVIDIA frame generation for
  its lighter blending mode, the one behind NVIDIA's faster frame generation preset. Needs a
  310.9.16+ frame generation DLL; `FGSource=1` loads `mgpu\nvngx_dlssg.dll` ahead of the
  driver's copy so a newer DLL can be used. Applies on the next launch.
- **NR resolution scale with residual rebuild** (`SRScale` 33-90, `SRRecon=1`): NR runs on a
  smaller copy of the frame and only its change is enlarged back onto the game's full-res
  frame. The game's native detail (e.g. DLAA) is kept and NR costs a fraction of full
  resolution. Defaults: residual, area down, bilinear up; the filters and the older DLSS SR
  rebuild are under Super resolution > Advanced in the panel.
- **Dynamic neural resolution** (`SRDynamic=1`, the "dynamic" box under the Custom scale
  slider): the scale follows the load, between `SRDynMin` (default 50%) and `SRDynMax`
  (default 80%), starting at `SRScale`. The reduced textures are made at the highest value
  (a new highest takes effect at the next start); when the scale moves nothing is rebuilt - NR
  works on part of the reduced texture and its history restarts for that one frame.
  It works with NVIDIA frame generation only (the box is greyed out otherwise); with FSR 3,
  BUILT-IN frame generation or frame generation off, the fixed `SRScale` is used.
  - **What it steers by:** every clean 2 s, the frames shown against the game frames counted ×
    the multiplier (primary), and the neural card's **busy time** - the share of each second it
    spent executing the mod's work, from GPU timestamps on every list, overlapping work counted
    once (early warning; shown in the panel). Short by more than 4% → 2% down (8% → 4%); still
    delivering but busy 98%+ for two windows → 2% down before frames are lost; clean and busy
    under 90% → 2% up, after a wait that doubles each time a try fails (8 s up to 4 min; kept at
    8 s while busy is under 80%). Missing frames only lower NR while the neural card is at least
    75% busy; below that the shortfall is in frame generation or presentation, the resolution is
    kept, and the log says so (a lower Limiter or multiplier is what helps then).
  - **Smoothness** (`SRDynSmooth`, the "smoothness" choice under it: off / relaxed / normal /
    strict, default normal): a steady 180 feels smooth and a 140-200 bounce does not, even at a
    similar average. Each window also measures how evenly frames arrive - the spacing's spread as
    a share of the gap, and hitches (gaps over twice the average). Uneven spacing lowers NR a
    step even when the average looks fine, and the scale only goes up when spacing is steady.
    Normal (spread over 25% of the gap, or 3+ hitches in 2 s) sits between a smooth 40 × 4 run
    (12-20%, 0-1 hitch) and uneven runs (24-42%, 4-14 hitches); strict (20%, 2) reacts sooner,
    relaxed (35%, 6) later. The panel shows the current spacing, so the number can be matched
    to what you see. Spacing only counts while the neural card is at least 75% busy: below that
    the unevenness comes from the game or the present path, and lowering NR would only cost detail.
  - **What it ignores:** a window is thrown away (and a fresh 2 s starts) on alt-tab, the panel,
    a game menu, the picture stepping aside, a game hitch (loading, shader compiling), a
    presentation stall over 60 ms, frame generation pausing, or a Limiter / multiplier change.
    It only runs with NVIDIA frame generation.
  - **When NR is not the problem:** if two reductions in a row do not raise delivery by at least
    2 points, the limit is frame generation / presenting, not NR. The scale goes back to where it
    was, reductions stop for 60 s (or until the multiplier or Limiter changes), the log says
    `FG/PRESENT BOTTLENECK SUSPECTED - NR SCALING INEFFECTIVE`, the panel says so in amber, and
    with target frame rate on the overload brake lowers the multiplier instead. Logged as
    `[MGPU][DR3]` / `[MGPU][GT1]`.
- **Foveated NR** (`FovNR=1`, off by default): on top of the resolution scale above, a centred
  rectangle (`FovSize`, 30-90% of each side, default 60) is also neural rendered at full
  resolution and blended over the reduced picture with a soft edge (`FovFeather`, 0-50% of the
  centre's half-size, default 15). The centre, where the eye usually is, gets full-resolution
  NR; the edges get the cheaper reduced pass. It costs a second NR evaluate on the centre's
  area, so the saving over full-resolution NR depends on both sizes: e.g. 50% scale with a 60%
  centre evaluates about 25% + 36% = 61% of the full frame's pixels. Needs Custom scale, the
  residual rebuild and one pass; otherwise it stays off. Live, under Super resolution > Advanced.
  Two shapes: `FovNR=1` the centred box and `FovNR=2` a horizontal band (`FovBand` 30-90% of
  the height, default 50; `FovBandPos` 0 top / 50 middle / 100 bottom, default middle). A middle
  band covers faces and the horizon edge to edge; a 50% middle band over a 67% scale evaluates
  about 50% + 45% = 72% of the full frame's pixels. A bottom band at full width skips the reduced
  pass beneath it. The full-resolution area is never wider than 16:9 of the picture's height, so
  on a 21:9 or 32:9 monitor the extra width (periphery) stays at the reduced scale and costs the
  same as on 16:9; `FovFullWidth=1` lets a band run the full width (racing games).
  `FovNR=3` (centre 2x) is the two-pass variant: the whole picture gets one pass, then the
  centre box gets a second pass over that result and is blended in - the stronger two-pass look
  where you are looking, at the cost of one extra evaluate on the box's area. Its strength is
  pass 2's intensity. Note that a second pass changes tone and contrast, not only detail, so a
  larger soft edge helps hide the boundary.
- **Target frame rate** (`FGAuto=1`, NVIDIA frame generation section of the panel): pick the
  output rate you want (`FGTarget`) and the frame generation multiplier is chosen
  automatically from the game's measured frame rate (target ÷ game fps, 2x up to the card's
  limit, re-picked when the game speeds up or slows down, after 1.5 s). With the mod's
  **Limiter** (`FGCapOn`, `FGCap`) a 240 fps target runs at 6x with a 40 fps cap, 5x at 48
  and 4x at 60. The Limiter also works on its own, with a multiplier picked by hand.
  **Overload brake** (`FGBrake`, on by default): if the frames actually shown fall more than
  8% short of game fps × multiplier for 2 s, the neural card can't keep up - the multiplier
  steps down one notch, holds 30 s, then tries again (longer holds if it keeps failing). The
  panel shows shown-vs-expected fps and warns in amber, also for a hand-picked multiplier.
  **Auto-tune** (button in the same section, NVIDIA frame generation): click it, close the
  panel and play for a minute or two. It tries Limiter × multiplier pairs on the running game
  (2 s to settle, 4 s measured each; progress shows top-right) and keeps the first pair, from
  2x up, where the game holds the Limiter and the frames shown reach Limiter × multiplier - the
  highest smooth output with the fewest generated frames. It starts at the target frame rate
  (or the monitor's refresh rate) and lowers it 10% at a time if nothing passes. The result is
  applied as Limiter + multiplier (target frame rate switched off) and every test is logged as
  `[MGPU][AT1]`. Cancel, alt-tab or the panel pausing it changes nothing.
  Panel settings are written to `mgpu.ini` once a slider has stood still for 0.4 s. Without G-SYNC on the picture, use a target that divides
  the refresh rate evenly; the panel warns when it does not.
- **NVIDIA DLSS indicator** (`tools/DLSS-indicator-on.reg` / `-off.reg`): double-click to show or
  hide NVIDIA's own on-screen DLSS text (version, preset, resolution) for the game's DLSS and
  the mod's neural rendering and frame generation; restart the game afterwards. It is a
  Windows-wide driver setting under HKEY_LOCAL_MACHINE, so Windows asks for permission.
- **NR UI correction** (`NRUICorrection`): the NR DLL's own HUD protection, auto/off/on.
- **Performance summary in the log**: every 10 s a `[MGPU][PF1]` line gives NR GPU time
  (avg/p95/max, frames over 11 ms), present spacing (avg/sd/p99/max, hitches) and real,
  generated and presented frame rates. Two panel checkboxes, both off by default: **detailed
  log** (`DetailedLog`, the repeating diagnostic lines) and **performance log** (`PerfLog`,
  this summary). Turn them on before a run you want to report.
- **FSR 1 / NIS scaling** (`Scaler`, panel View section): when a game is set below the
  monitor's resolution, the picture is enlarged to the monitor with AMD FSR 1 or NVIDIA Image
  Scaling (with a sharpness slider) instead of Windows' plain stretch. Spatial, so every frame
  including generated ones is scaled; built in, no DLL. FSR 1 and NIS are compiled from source
  in the background when scaling starts (about 3 s for NIS); plain bilinear is shown until they
  are ready, so the game does not freeze (`[MGPU][SC1] compiled in the background` in the log).
  If another program (for example the Explorer folder the game was started from) takes the
  foreground in the first 3 s after the scaled picture appears, the mod gives it back to the game
  (up to 4 tries, `[MGPU][SC10]`) and keeps the picture up meanwhile; after that, switching away
  makes the picture step aside as before.
  Typical use: a 4K monitor with the game windowed at 2560x1440 (set before launch). Works
  with `DcompOverlay=1`: the picture is shown on a click-through cover over the monitor, so
  the game keeps focus and input. As in Lossless Scaling, the mouse pointer is drawn on the
  enlarged picture at its real position in the game and kept inside the game's window, so menus
  line up (it moves 1.5x as far on screen at 1440p -> 4K). If the game is not the window in front,
  the picture steps aside so you can click back in.
  The MGPU panel (Home) opens on top of the 4K picture, drawn at full resolution with the pointer
  lined up (`PanelOverPicture`, below). If that is turned off, Windows' full-screen magnifier
  enlarges the game's window while the overlay is open instead (`ScalerConsole`).
- **Panel over the picture** (`PanelOverPicture=1`, on by default here; from upstream 0.4.0,
  contributed there by NO1950): with `DcompOverlay=1` the panel opens on the neural picture
  itself instead of the picture stepping aside. The game's input is blocked while it is open;
  ESC or Home closes it.
  The panel opens in its own window, in front and wide (it can be resized, never narrower than
  its content), instead of as a tab behind ReShade's Home tab.
- **Panel sections** (UI6): the panel is one column of sections that fold - Scaling to the
  monitor, Neural rendering (with Intensity), Frame generation, Frame Warp, Latency and display,
  HDR, View and HUD, Screenshots video and benchmark, DLSS on the second GPU, Hotkeys, and Logs and
  diagnostics. Each header shows what the section is set to on its right, so the panel reads as a
  summary when closed. Expand all / Collapse all are at the top. Less-used options sit one fold
  deeper: "More frame generation options" (FG Lite, `mgpu\nvngx_dlssg.dll`, motion view) and
  "Copy, queue and late-latch". The live figures, counters, install check, driver and DLL
  versions and the build line stay at the foot, always shown. Which sections are open is kept in
  mgpu.ini (`UiOpen`); Scaling, Neural rendering and Frame generation are open the first time.
  Headers are in Title Case and every option starts with a capital; every control explains itself
  in a tooltip when the mouse rests on it, greyed-out controls included (UI7).
- **Startup checks**: if there is no second NVIDIA card (integrated graphics do not count), or the
  NVIDIA driver is older than 610.47 (the first with DLSS 5 neural rendering) or NVIDIA's NGX
  reports that it needs updating, the mod stays off and says why in red at the top of the panel
  and in ReShade.log (`[MGPU][T2]`, `[MGPU][DRV1]`). The panel also shows the driver version.
- **Less sharpening in motion** (`ScalerMAS=1`, checkbox under Scaling to the monitor; live):
  motion-adaptive sharpening. The neural stage turns the game's motion vectors into a small mask
  (the fastest motion in each 16×16 cell), and the scaler sharpens each pixel between fully (still)
  and `ScalerMASMotion` % of it (default 30%) at 8+ px of motion per frame - calmer edges in
  motion, the same detail when still. Works with NIS (the sharpening term is scaled per pixel;
  the vendor file is unchanged, the edit is made as the shader is compiled) and FSR 1 (RCAS is
  blended back toward the unsharpened picture). Generated frames use the latest real frame's
  mask. Costs a few reads per pixel. Logged as `[MGPU][MAS1]`.
- **NIS half precision** (`ScalerHalf=1`, checkbox under Scaling to the monitor; live; default
  off): a second NIS, built with NVIDIA's own `NIS_USE_HALF_PRECISION=1` (16-bit `min16float` for
  the filter tables and the pixel tile in shared memory), is compiled the first time the option is
  on while NIS is scaling. That compile takes about 3 s, so it runs in the background and full
  precision is shown until it is ready (the panel says "preparing..."); nothing at all happens at
  start-up while the option stays off. After that the checkbox switches between the two live, so
  they can be compared on the same scene. The saving is expected to be small (NIS is already a
  light pass on the neural card) and the 16-bit storage can, rarely, show faint banding in smooth
  gradients (sky, fog, dark vignettes) on a 10-bit output, which is why full precision stays the
  default. `min16float` is a hint: a card that does not report 16-bit minimum precision runs it at
  full precision, and the panel says so. Logged as `[MGPU][NH1]` (with the compile time).
- **Low-latency present** (`PresentLatency1=1`, "low-latency present" under Latency and display;
  restart): the bridge's swap chain is made waitable with a maximum frame latency of 1, and each
  present first waits until the previous frame has left Windows' queue (at most 100 ms). A
  finished frame then reaches the screen at the next opportunity instead of waiting behind up to
  three others - a few ms less delay. Best with G-SYNC. Logged as `[MGPU][LP1]`.
- **Bus traffic** (panel, Live section): `bus  sent … GB/s  received … GB/s` - what the render
  card copied out to system memory and what the neural card read in over the last second
  (picture, depth, motion vectors and HUD-less copy), counted from the mod's own copies.
  The FPS HUD (CTRL+ALT+H) has three levels: BASIC (fps), DETAIL (the pipeline's frame rates) and
  ADVANCED (DETAIL plus BUS OUT / BUS IN, bridge latency, ready time and GPU 0 queue depth).
  "log bus traffic" (`BusLog=1`) writes the bus figures every 10 s (`[MGPU][BW4]`, ~35 KB an hour).
  Beside the mod's own (exact) figures: GAME - the game's own frames, which Windows copies from the
  render card to the monitor's card (an estimate: frame size x frame rate) - and TOTAL OUT / TOTAL IN,
  everything on the render card's link out and the neural card's link in, from the NVIDIA driver's
  counter (nvml.dll, read 4 times a second while a reader is showing; includes packet overhead).
- **Screen saver** (`ScreenSaver=10` minutes, 0 = off; `ScreenSaverStyle=1`; panel, next to the
  clock): for OLED monitors. After that long with no mouse, keyboard or controller input, the
  picture is covered - black, or the words MGPU BRIDGE drifting slowly in dim grey. Any input
  brings the game back at once; the game keeps running underneath, so pause it first. Never
  while the panel is open, and the wait starts over when the panel closes (SV4). Logged as
  `[MGPU][SV1]`.
- **Queue guard** (`QueueGuard=1`, live checkbox in the same section): when the neural card falls
  2 or more frames behind, the game's render thread waits in 0.5 ms steps (at most 25 ms) until
  it catches up, so the game never makes frames that would be skipped - saving the 3080 Ti's
  work, the link, and delay. Only fresh readings count (a stopped bridge never slows the game).
  With a Limiter that fits it rarely acts; `[MGPU][QD1]` reports holds every 30 s.
- **HDR output** (`HDR=1`, panel View section): turns the game's SDR picture into HDR10 on
  the neural card for an HDR monitor, as the last step before it is shown (screenshots and
  video stay SDR). Needs Windows HDR on for that monitor and a 10-bit game picture (Unreal
  games are); the panel says in amber when either is missing. **Peak** is the brightest
  highlight (far left = what the monitor reports), **paper white** the brightness of white
  walls and menus (200 nits is a good start), **highlights** how far bright areas (sky,
  lamps, fire) reach toward the peak, **saturation** colour strength. Dark and mid tones keep
  the game's look; only bright areas are lifted. The cost is one small pass and two copies per
  shown frame. Logged as `[MGPU][HD1]`.
- **Clock and stopwatch** (top-right of the picture): `CTRL+ALT+C` shows/hides the time
  (12-hour, remembered), `CTRL+ALT+S` shows/hides a stopwatch that counts up from 0 each
  time it is shown. Also checkboxes in the panel's View section; keys can be rebound.

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
- **`SLTags=3` on RoboCop: Rogue City - Unfinished Business.** Up to SLD1 it hid the game's DLSS
  start from the mod: SLTags and the mod's NGX catch both need the game exe's one GetProcAddress
  import slot, SLTags took it first, and the NGX catch never saw the game create DLSS. The mod
  then treated RoboCop as an unknown game, and its motion-vector test mode crashed the game at
  the menu. Since SLD2 the NGX catch is chained behind the SLTags hook (`[MGPU][SLD2]` in the
  log). Even so, this game only sends its camera data to Streamline while its OWN frame
  generation is on, so with the mod's frame generation `SLTags=3` receives nothing there - leave
  it at 0. `SLTags=4` installs the same hook but hands back NVIDIA's own functions untouched;
  both 3 and 4 log every Streamline/NGX function the game looks up (`[MGPU][SLD1] GetProcAddress
  lookups`) and a summary at frames 900, 3600 and 10800 (`[MGPU][SLD1] SUMMARY`).

**Steam overlay guard** (`SteamOverlayGuard=1`, default on): Steam injects its overlay DLL into
every game it starts, even with the overlay switched off, and its DXGI hook crashes now and then
in this setup (`gameoverlayrenderer64.dll+0x9b700`, reading `0xffffffffffffffff`). When that fault
happens, the crash catcher steps the program back out of the overlay's code, as if the overlay had
returned, and the game carries on with one frame not shown. Since SO2 it only does so for that
fault (a read of `0xffffffffffffffff` inside the overlay, at whatever offset a Steam update moves it
to) and only after checking that the return is safe (unwind data, stack, a CALL site, alignment);
`SteamOverlayGuard=2` accepts any access violation in the overlay, `3` is SO1's original guard
exactly (any overlay fault, no return checks, with the new logging), `0` turns it off. At most 5 times
a minute; past that the fault goes on as before. Every attempt is logged (`[MGPU][SO1]`, also in
crash.txt), and about 2 s later `[MGPU][SO2]` says whether rendering came back (the faulting thread
presenting again, frame rate, failed presents, device removed); `[MGPU][SO3]` compares memory a
minute later. Since SO4 each line also says how sure it is the call was a Present (the CALL's
vtable slot, and whether the thread was presenting); only contrary evidence refuses a step-over.
Details, a test checklist and rollback: `docs/steam-overlay-guard.md`.

**How Steam hooks Present** (`[MGPU][SH1]`, once per session, read-only): when the mod's own swap
chain is created, the log says whether Steam's overlay redirected its function table or patched
DirectX's Present code itself. With the current Steam client it is the code ("CODE patched in
memory"), which is why a table-swap bypass (OB1-OB3) could not work and was removed in OB4. If a
Steam update ever changes this, that line will show it.


**Crash catcher**: if the game ever crashes to the desktop, the add-on writes one line naming
what crashed (the module, the place inside it and the call chain) to `mgpu\crash.txt` and to
ReShade.log as `[MGPU][CR1]`, even when Windows and the game record nothing. Send that line with a
bug report. It never changes how an error is handled.

**Streamline kept in memory**: some games start NVIDIA Streamline (`sl.*.dll`) and shut it down
again a few seconds later, leaving a hook behind; a rare later call through that hook crashed
the game. The add-on keeps Streamline's DLLs loaded until the game exits, so such a call lands
in real code. It costs a few MB. `PinStreamline=0` in mgpu.ini turns it off.

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

**Steam games:** start them from Steam as usual. Steam always puts its overlay into the game, and
that overlay has crashed in the present path; the Steam overlay guard (below) steps over such a
crash so the game keeps running. `SteamAppIdFile=1` makes the mod write `steam_appid.txt` beside
the game's exe, which only matters when the exe is started directly (off by default, not in the
panel: it never helped a game started from Steam).

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
   FrameGen=3          ; AMD FSR 3 frame generation (4 = NVIDIA DLSS FG, 1 = BUILT-IN)
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

Third-party code built into the add-on: AMD FidelityFX FSR 1 (MIT, Copyright (c) 2021
Advanced Micro Devices, Inc.) and NVIDIA Image Scaling SDK (MIT, Copyright (c) 2022 NVIDIA
CORPORATION & AFFILIATES); their license texts are in `src/third_party/fsr1/LICENSE.txt` and
`src/third_party/nis/LICENSE.txt`.


`src/live_panel.cpp` (the panel over the picture) comes from upstream 0.4.0, where it was
contributed by [NO1950](https://github.com/NO1950); the fork adds the scaled-picture pointer.

MIT, as upstream. The add-on, its design and its 0.2.5 code are Marcelo Guibout's
([maohgad-web](https://github.com/maohgad-web)). This fork's changes are by davidm71.
Nothing from NVIDIA is included, and nothing may be.
