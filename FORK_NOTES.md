# davidm71 fork of MGPU Bridge (Neural-coprocessor)

Based on the **0.2.5 release** of [maohgad-web/Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
(tag `0.2.5`, commit 965a14f). All 0.2.5 features are kept; the changes below are
additions. This file covers the fork's first round (transport, pacing, overlay, HUD,
bug fixes) with its measurements. Later additions - the 0.3.1 backports, frame generation
(own / AMD FSR 3 / NVIDIA DLSS FG 2x-6x), Frame Warp (late camera reprojection), HUD-less mode, Color Strength, the present-time
hook and the status readout - are described in [README.md](README.md). Which features are
on is decided by the shipped `assets/mgpu.ini`.

## What this fork adds

### Multi-display composition overlay
`DcompOverlay=1` is accepted with **one or more** active displays (upstream accepts
exactly one). Lets you play on one panel with a second monitor for stats, with no
second window on top of the game and no input loss. `DcompOverlay=auto` keeps
upstream's single-display rule. Tested: game panel + stats monitor.

### Late-latch prefetch (`LateLatch=1`)
Upstream evaluates only the newest frame, so its f+1 copy-ahead never matches the
frame actually evaluated when the game outruns the neural card, and every evaluated
frame paid its unpack in series. Late-latch copies the newest arrived frame on the
copy queue shortly before the current evaluate finishes, so the unpack overlaps it.

| Key | Values | Default |
|---|---|---|
| `LateLatch` | 0 off, 1 on (needs `CopyQueue=1` or `2`) | 0 |
| `LateLatchAge` | 0-4: max game frames a latched frame may trail the newest | 2 |

Cost: up to `LateLatchAge` game frames of extra latency. Use 2; 1 discards ~28% of
latched copies in testing.

### GPU 0 async staging (`StageCopy=1`, on in the shipped mgpu.ini)
Upstream records the frame copies (colour, depth, motion vectors: ~44 MB per frame at
2560x1440) into the game's own command list, straight into the shared cross-adapter
heap in system memory. That PCIe transfer runs on the render card's graphics queue,
inside the game's frame. With `StageCopy=1` the game's list copies into a VRAM
staging buffer instead, and a separate copy queue on the render card moves each
slot to the shared heap on the DMA engine while the game renders its next frame. The
neural card still waits on the same fence, so nothing changes on its side. Costs
about 265 MB of VRAM on the render card at 2560x1440. If it fails to start, it turns
itself off and the run uses the upstream path.

The `[MGPU][OPT]` line now reports, in either mode, how long the render card's queue
spends on these copies ("GPU0 in-frame copies" and "MVec copy") and the staging DMA
time. Compare a `StageCopy=0` run with a `StageCopy=1` run at the same spot.

Measured (same spot, NR at 2560x1440, SR on GPU 1 off): the copies' cost on the render
card's queue fell from 3.40 ms to 0.68 ms per frame, and game fps rose from 70.1 to
74.9. The PCIe transfer itself (4.6 ms) now runs on the DMA engine. 0 staging failures
over 14,400 frames.

### Overlay panel controls (ReShade overlay, Home, MGPU panel)
All write mgpu.ini so the next launch starts the same way.

**Late-latch** (live): on/off, max age slider, and a live "used % of latched" readout.

**Latency and display:**

| Control | ini key | Applies |
|---|---|---|
| Reflex off / on / on + boost | `Reflex` | restart |
| Reflex sleep | `ReflexSleep` | restart |
| Bridge vsync | `PresentVsync` | live |
| Copy queue off / split / gated | `CopyQueue` | restart |
| GPU 0 async staging | `StageCopy` | restart |
| Ring window | `RingWindow` | restart |
| Overlay mode window / overlay / auto | `DcompOverlay` | restart |

The panel shows "Restart the game once to apply" when a restart-only setting differs
from what the session started with. Reflex was kept out of the upstream panel because
a crash could leave the driver's low-latency mode set; upstream's own V43 residue
clear (runs every launch when `ReflexApplied=1` is left behind) has since made that
safe. The panel's ini cache was enlarged from 12 to 32 keys so the extra controls do
not re-read mgpu.ini every frame.

### Camera-constants probe (`SLTags=1`, diagnostic, off by default)
Feasibility test for DLSS Frame Generation on the second GPU. Extends upstream's
read-only Streamline tag tap to also read the game's `slSetConstants` call (camera
matrices, jitter, motion-vector scale, near/far/FOV, flags). Read-only: the call is
passed through untouched, installs after 900 frames, Streamline is never called by
the mod. Logs `[MGPU][SLT2]` lines: a first full dump, then a summary every 1800 calls
(sane / zero / identity / invalid %, camera-moving %, inverse-matrix error).

### FPS HUD (CTRL+ALT+H: basic / detail / off)
MGPU IN, CONSUMED, NR EVAL, NR SKIP, PRESENT, NR GPU ms. NR SKIP counts every frame
that gets no neural evaluate (newest-only skips, ring-window skips, overruns and
failed submits). Over a steady interval EVAL + SKIP tracks MGPU IN closely; over a
single 0.25 s HUD sample they can differ by a few fps, because the producer and
consumer counters advance on different threads.

### Bug fixes (always on)
- **MVec race:** the evaluate re-read the live per-slot MVec/depth flags, which the
  game thread clears when the ring wraps, so a frame could be evaluated without the
  motion vectors that were copied for it (or with mismatched barriers). Validity is
  now recorded at copy time and used for the bind and both barriers.
- **MVec dropped when the consumer lags:** the per-slot MVec flag is cleared for the
  next frame one frame before the slot is reused, so when the neural card was 5 frames
  behind it skipped vectors that were actually in the slot (up to 4% of evaluates).
  Validity is now tagged with the frame number at seal time.
- **Late-latch cadence:** the two copy buffers on the neural card were chosen by frame
  parity, so late-latch could only pick a frame 1 or 3 game frames after the current
  one, never 2. With the game at ~77 fps and NR at ~48 that gave an uneven 1,3,1,3
  step pattern (measured 69% / 28% / 0% for steps of 1 / 3 / 2) that felt choppy.
  The buffer is now chosen explicitly (the one the current evaluate is not reading),
  so late-latch always takes the newest arrived frame.
- **Wasted copy-ahead:** with `CopyQueue=2`, the f+1 speculative copy is skipped when
  the producer is already ahead, since it would never be read.
- **Uncounted frames:** frames lost to a failed submit or fence wait are now counted.
- `[MGPU][OPT]` log line every 1200 frames (also in `Frames=0` runs).
- **Fence-wait hardening:** if registering the completion event fails, the consumer
  polls the fence to the real completion instead of timing out with GPU work still
  in flight (logged once as `[MGPU][HARDEN1]`).
- Late-latch's timer is released at shutdown, and all fork counters reset on each arm.
- **Super Resolution on GPU 1 motion fix:** SR's motion-vector scale is now multiplied
  by the game frames elapsed since its previous evaluate, as the NR evaluate already
  was (upstream R87). Without it SR reprojected its history by a fraction of the real
  motion whenever the consumer fell behind, which showed as flicker while moving. A gap
  of 8+ frames resets SR's history instead.

## Measured (RTX 3080 Ti render + RTX 5060 Ti neural on PCIe 4.0 x4, 2560x1440,
RoboCop Rogue City - Unfinished Business, same spot)

| Build | NR EVAL | NR GPU | Share of ceiling |
|---|---|---|---|
| Upstream 0.2.5-dev | ~32 fps | ~21 ms | ~68% |
| Fixes only (`LateLatch=0`) | ~36 fps | ~18 ms | ~64% |
| Fixes + `LateLatch=1`, age 2 | ~52 fps | ~18.7 ms | ~98% |

Final build, 2026-10-09 (StageCopy=1, LateLatch=1 age 2): with SR Native on, game ~79.5 fps,
NR ~65 fps, NR steps 77% one frame / 23% two frames / ~0 larger, latched copies 99.97%
used, 0 MVec disagreements, 0 staging failures, 0 errors over 8,700 frames. With SR off:
game ~74, NR ~48, steps 40% / 59% / 1%.

With SR Native on the neural card (1708x961 -> 2560x1440): NR GPU ~10.3 ms, NR EVAL
= game fps (~71.5) with 0 skips, so the render card is the limit.

Late-latch: 98.6% of latched copies used, 0 lost frames. With late-latch on, game fps
was ~70-75 vs ~88 off on this rig (render card at ~100%). `Reflex=1` +
`ReflexSleep=1` improved feel with no neural-rate cost.

Suggested settings (with the game's own DLSS on, and SR Native enabled in the MGPU panel):
```
CopyQueue=2
LateLatch=1
LateLatchAge=2
StageCopy=1
Reflex=1
ReflexSleep=1
```
If NR still skips the odd frame, capping the game a few fps below the NR EVAL rate
gives a perfectly even cadence.
