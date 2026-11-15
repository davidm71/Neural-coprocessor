# AMD chained 4x frame generation (FSRChain, experimental)

AMD FSR 3 frame generation makes one frame, at the halfway point, per pair of
frames it is given. With `FSRChain=1` (panel: Generator AMD FSR 3, "4x
(Chained, Experimental)") a second AMD context is used:

| Context | Fed | Makes |
|---|---|---|
| A | real 0, real 1, ... (as in 2x) | the 1/2 frame |
| B | real 0, A's 1/2, real 1, A's 1/2, ... | the 1/4 (real 0 -> 1/2) and the 3/4 (1/2 -> real 1) |

B is fed twice per real frame with the game's motion vectors and the frame time
halved. B keeps its own history, which is why it is a separate context. Off by
default; AMD 2x is unchanged.

## AC3: how a pair is made

All decisions live in `src/fsr_chain.hpp` (no D3D12) and are tested in
`tests/fsr_chain_test.cpp`. `fsr_place` in `src/gpu1_context.cpp` carries them out.

1. Context A writes the 1/2 frame (`fsr_out`).
2. `begin()` decides: **off** (chain disabled or unusable), **prime** (B's
   history is not known good: first pair, a break in A's frame IDs, a toggle,
   or any earlier B failure) or **chain**.
3. B's steps run into B's own textures: `fsrb_q` (1/4) and `fsrb_t` (3/4). A
   prime feeds B the real frame only; its output is discarded.
4. `finish()` returns 3 only if both chained steps succeeded; otherwise 1.
   B's history is marked good only after a successful prime or a complete 4x
   pair, and is cleared before every attempt.
5. **Only then** is anything composed into the set: three composition passes
   (1/4 -> `fg_out[k]`, 1/2 -> `fg_outx[k][0]`, 3/4 -> `fg_outx[k][1]`) or one
   (1/2 -> `fg_out[k]`). A failed 3/4 step therefore leaves no partial frame
   or HUD-mask state behind, and no output texture is written twice.

Frame time (`frameTimeDelta`) comes from the game's own hand-over stamps
(`qpc_submit`) of consecutive frames A was given. An interval outside 2-200 ms,
a missing stamp, or a break in A's frame IDs keeps the last good value. AC4
counts every reuse by cause (`classify_dt` in `fsr_chain.hpp`): missing
stamps, invalid intervals and sequence breaks (the first frame counts as one).

GPU timestamps on the NR list measure five stages separately: A, B's 1/4
step, B's 3/4 step, B's priming step and the composition (AC4 split priming
out of the 3/4 figure). The marks of each list are resolved into one slot of a
four-slot ring in the readback buffer. The slot is tied to the NR fence value
signalled after that list, and is read only once `GetCompletedValue()` has
passed it. `UINT64_MAX`, or a device-removed reason, stops the timing without
mapping anything. A slot overwritten before its fence passed is counted as
lost. Each stage's first 8 uses carry first-use setup costs, so they are
reported once on their own and kept out of the averages. `[MGPU][AC4]`
reports averages and maxima every ~30 s (1200 pairs).

## Inputs to the synthetic midpoint: what is approximate

| B step | Present colour | Depth given | Vectors given | Accuracy |
|---|---|---|---|---|
| 1/4 (real 0 -> 1/2) | A's 1/2 frame (synthetic) | real 1's depth | real 1's vectors x 1/2 | **approximate** |
| 3/4 (1/2 -> real 1) | real 1 | real 1's depth | real 1's vectors x 1/2 | exact under linear motion |

* The **3/4 step** gets true inputs: real 1's depth belongs to real 1, and
  half of real 1's vectors is the motion from the 1/2 frame to real 1 when
  motion is linear over the interval. AMD reconstructs the previous frame's
  depth itself by reprojection. Its errors come mainly from defects already in
  the 1/2 image.
* The **1/4 step** is the weak one. Its present colour is the 1/2 frame, but
  the depth and vectors describe real 1's pixel positions, which sit half a
  motion vector away. At moving edges, AMD's disocclusion mask, depth
  reconstruction and vector scatter use the wrong pixels' values. Expect
  ghosting or haloes around moving characters, and smear at areas uncovered
  between 1/2 and 1.

Nothing here reconstructs geometry. Areas hidden at real 1 but visible at
1/2 have no true depth or motion in any input.

### Proposed low-cost improvement (not in AC3)

A **half-step gather** before the 1/4 step: one compute pass at vector
resolution that resamples real 1's depth and vectors to the 1/2 frame's pixel
positions. For each 1/2-frame pixel p, find the real-1 pixel c that moves
there, with c = p - m(c)/2 solved by two fixed-point iterations starting at
c = p (m = the game's current-to-previous vector). Write depth(c) and m(c)/2;
B then gets these at vector scale 1.

* Cost: one full-screen pass reading two textures and writing two. The
  0.05-0.1 ms figure is an estimate only; it must be benchmarked on the RTX
  5060 Ti before any decision. Evaluate it only after the basic chain is
  shown to give stable 4x.
* Gains: edge alignment of depth and vectors with the 1/2 image, the main
  source of the 1/4 frame's haloes.
* Limits: where the iteration lands on a foreground pixel in an area
  uncovered at 1/2, the value is still wrong. A max-depth (farthest) pick over
  a 3x3 window reduces foreground bleed but does not create the missing
  background.
* Validation: capture 1/4 frames with and without the pass on the same
  scene; compare edge haloes and the AC3 GPU times.

## Does the installed AMD provider support `numGeneratedFrames > 1`?

`ffxDispatchDescFrameGeneration` has `outputs[4]` and `numGeneratedFrames`.
From the AMD FSR SDK 2.4.0 source (2026-10-28):

* The analytical FSR 3 provider
  (`Kits/FidelityFX/framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp`)
  writes only `outputs[0]` (`fiDispatchDesc.output = desc->outputs[0]`).
  `numGeneratedFrames` is copied only when translating the older callback ABI.
  AMD's frame generation swapchain and the FSR sample always pass 1.
* The ML frame generation provider (FSR Frame Generation 4.0.2) requires
  Windows 11 and a Radeon RX 9000 GPU, so it is not selected on an RTX 5060 Ti.

Conclusion: on this rig the analytical provider is expected to ignore any
frame beyond the first, so AC3 keeps `numGeneratedFrames = 1` and does not
use multiple outputs. The provider name and version are in the
`[MGPU][FSR] AMD FSR FRAME GENERATION UP` log line. If a future DLL is to be
checked, only a one-shot probe on a throwaway third context would be safe.
It would clear `outputs[0..1]` to sentinel colours, dispatch
`numGeneratedFrames = 2`, read both back, and require `outputs[1]` to differ
from both the sentinel and `outputs[0]`. Never use the live A or B context.
Not implemented.

## SF1: the rebuilt AMD DLL - one optical flow for all three frames

**SF2 (current):** the two DLLs sit side by side. AMD's makes context A (2x,
and the 1/2 frame of the chained fallback); the rebuilt one gets its own
context M, made the first time 4x runs, after a version query (before any
context) confirms its provider is `-mgpu1`. Mode per frame: 4x ticked + rebuilt
usable -> M; else `FSRChainFallback=1` -> the A + B chain (panel: "not
optimal"); else 2x with the 4x box greyed out. A and M share one frame counter
and each is passed `reset` when it sat out the previous frame. Three failed M
calls turn 4x off for the run. If AMD's own DLL is missing, the rebuilt one
makes 2x too (its 2x is AMD's maths unchanged). The text below describes SF1,
where the rebuilt DLL replaced AMD's for everything.

`amd_fidelityfx_framegeneration_mgpu1_dx12.dll` is AMD's FSR 3.1.6 frame
generation (FidelityFX SDK 2.4.0 source, MIT) rebuilt with one change, MGPU1:
`numGeneratedFrames` 2-3 makes that many frames between the same two real
frames, `outputs[i]` at t = (i + 1) / (n + 1). The bridge loads it first when
it is in the `mgpu` folder; its provider names itself `3.1.6-mgpu1`, which is
what switches the bridge to one call per pair (`fsr_record` passes `fsrb_q`,
`fsr_out`, `fsrb_t`; `fsr_place` only composes). Context B is never made.

What runs once per real frame: prepare (reconstruct and dilate), the optical
flow (pyramid, search, filter, scene-change detection). What runs per frame
made: setup, depth of the in-between frame, the game- and optical-flow vector
fields, disocclusion, interpolation, inpainting. The previous source frame is
replaced only after the last one.

The shaders keep AMD's half vector h = m/2 (m: current -> previous) and add two
factors, 2t towards the previous frame and 2(1 - t) towards the current one.
Both are exactly 1 at t = 0.5, so the 1/2 frame - and 2x - is AMD's own maths.
The blend weight is t; a frame disoccluded on one side still takes only the
other side, as AMD's. The frames-since-reset counter moves once per real frame.

Against the AC chain: every frame comes from the two real frames with exact
vectors (no synthetic present colour, no halved vectors, no reprojected depth
from the wrong frame), and the optical flow is paid once instead of three
times. `[MGPU][SF1]` reports the GPU time of the whole call (all three frames)
every ~30 s; compare with AC4's A + B 1/4 + B 3/4.

Built on Linux: DXC (Microsoft, from source, signed DXIL) compiles every
permutation; `ffx_sc.py` writes the headers AMD's tool would (bindings from
D3D12 reflection, checked against the host's name tables); the DLL is
cross-built with clang/MinGW with `WIDL_EXPLICIT_AGGREGATE_RETURNS` so COM
calls that return structs use Microsoft's convention. The source package also
has a CMakeLists.txt for Visual Studio using the same generated headers.

## Windows test plan

Build with `MGPU_BUILD_TESTS=ON` and run `ctest` (`mgpu_fsr_chain_test`
should report 0 failed). Then, in RoboCop with the AMD DLL in `mgpu\`:

1. **2x unchanged:** Generator AMD FSR 3, chain box off. Expect the usual
   `[MGPU][FSR] ... UP` line, shown fps about 2x real, and no `[MGPU][AC1]`
   or `[MGPU][AC4]` lines.
2. **Chain starts:** tick "4x (Chained, Experimental)". Expect
   `[MGPU][AC1] second AMD frame generation context up` once, then `[MGPU][AC4]`
   lines with nearly all pairs at 4x, one prime at the start, and 0 errors.
   Shown fps should be about 4x real.
3. **Timings:** in `[MGPU][AC4]`, A and each B step should cost about the
   same; composition should be small; priming should appear only after
   breaks. "timing records lost" should stay 0. The one-off "first 8 uses"
   line shows first-use setup costs. Compare the neural card's busy share
   (GT1 / HUD) with the chain off and on.
4. **Toggle:** untick and re-tick a few times in play. Each re-tick should
   cost one prime (the prime count rises by one); no flashes.
5. **Breaks:** open and close a full-screen menu, load a save, alt-tab.
   Each break should cost one prime, then 4x resumes.
6. **Frame time:** the `[MGPU][AC4]` frame time should match 1000 / real fps
   (about 25 ms at 40 fps). Missing stamps should be 0; invalid intervals and
   sequence breaks should rise only around loads, menus and skipped frames.
7. **Image:** compare fast mouse turns and moving characters against 2x. Look
   for haloes on the 1/4 frames in particular (see above).
8. **Failure path (optional):** a run that switches the chain off (three B
   errors) should log "chain OFF for this run" and continue at 2x without
   flashes.
9. **Regressions:** NVIDIA FG (FrameGen=4), NR, transport and the Steam
   overlay guard should behave as with AC2; their log lines are unchanged.
