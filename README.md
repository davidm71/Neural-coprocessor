# Atomic MGPU Bridge

**Version 3.0** - davidm71's fork of [maohgad-web/Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
(MGPU Bridge) by Marcelo Guibout, based on its 0.2.5 release.

A ReShade add-on for PCs with two NVIDIA graphics cards. The game renders on the first
card. Each finished frame is copied whole to the second card, which runs DLSS Neural
Rendering, makes the generated in-between frames and shows the result on its own monitor.
The rendering card is left free to render, and the second card does everything else.

All credit for the original idea and design goes to the upstream project; read its README
for how the bridge works and for its disclaimers. File names (`mgpu.ini`, the add-on file)
are the same as upstream's, so existing installs keep working.

This is a hobby project, not a product. Games with anti-cheat and online games are at your
own risk.

---

## What the fork adds

**Display**
- **Picture window on top** (`DcompOverlay=0`, the default): the bridge shows the
  picture in its own window over the game, without taking the focus. The game keeps the
  keyboard and mouse, and mouse clicks on the picture are passed on to the game. The
  window gets independent flip, so **G-SYNC works** (set G-SYNC to "full screen only").
- **Automatic monitor pick** (`Monitor=auto`): the picture goes to the monitor plugged
  into the second card, even when DirectX does not report that monitor.
- **ReShade panel on the picture** (`PanelOverPicture=1`): Home opens the panel over the
  full-screen picture, and the game ignores input while it is open.
- **HDR:** the picture follows the game's own HDR setting (HDR10 or scRGB), also when it
  is switched mid-game. For SDR games, `HDR=1` turns the picture into HDR10.
- **Tray icon:** shows the bridge's status while it starts (grey waiting, amber arming,
  green running, red error) and has a menu: a status summary, arm, hide the picture,
  open the log, About, and Exit (stops the bridge; the game keeps running).
- **Overlay mode** (`DcompOverlay=1`): draws the picture onto the game's own window
  instead, also with a second display attached.

**Frame generation on the second card**
- NVIDIA DLSS frame generation (2x up to 6x) or AMD FSR 3 frame generation (2x to 4x),
  made from the game's motion vectors and depth.
- HUD-less mode (`HudLess=1`, D3D12): generated frames keep the HUD sharp.
- Dynamic multiplier and target frame rate options in the panel.

**Performance and smoothness**
- Late-latch, background staging copies and a dedicated copy queue, so frames reach the
  second card without holding up the game.
- Neural rendering at a reduced resolution with detail rebuild, and a dynamic resolution
  that drops when frame pairs start arriving late and climbs back when they don't.
- Even frame cadence when the second card cannot show every game frame.

**Tools**
- FPS HUD (`CTRL+ALT+H`), screenshots, video recording, a benchmark and a benchmark sweep
  with results written to CSV files.
- FSR 1 / NIS scaling to the monitor, HDR output, Frame Warp, colour strength.
- A Steam overlay guard and a crash catcher, so a crash in a hooked overlay does not take
  the game down.
- Every setting is in the panel (ReShade overlay, Home) and saved to `mgpu.ini`.

## What you need

- Two NVIDIA graphics cards. The second card runs DLSS Neural Rendering, which NVIDIA
  documents as an RTX 50-series feature; an RTX 4090 has been reported working (Cyberpunk 2077).
  **Use an RTX 50-series (or RTX 40-series) card as the second card.** On a card that cannot run it, the driver
  refuses it ("FeatureNotSupported", 0xBAD00001) and you get the picture without neural
  rendering; the installer's Check and the log both warn when no RTX 50- or 40-series card is found.
- A monitor plugged into the **second** card. **Recommended: the card that renders the game
  runs headless** (no monitor), and your monitor hangs off the second card, which does the
  neural work, frame generation and the display.
- For HDR, Windows HDR turned on for that monitor.
- [ReShade](https://reshade.me) 6.8.0 or newer, the version **with add-on support**.
- A DirectX 12 game. DirectX 11 games are experimental (`DX11=1`).
- Your own `nvngx_dlssnr.dll`. Nothing from NVIDIA is included in this project, and
  nothing from NVIDIA may be redistributed.

## Install

1. Unpack the release zip anywhere and run **`AtomicMGPU-Setup.exe`** from that folder.
2. Pick the game (your Steam games are listed; "Browse..." finds any other) and click
   **Install / Update**. It checks the game folder first and says what is missing.
3. Your own `nvngx_dlssnr.dll`: the installer looks for a copy already on this PC (another game
   set up for the bridge, Downloads, Desktop, Documents) and offers to copy it into the game's
   `mgpu` folder - or use "Add my nvngx_dlssnr.dll..." to pick it. It is never shipped.

The installer puts in the add-on and its files, the depth shader, and ReShade with add-on
support (as `dxgi.dll`, or `d3d12.dll` when another mod owns `dxgi.dll`) if the game has no
ReShade. It never overwrites your `mgpu.ini` (the new default goes beside it as
`mgpu.ini.new`), and **Uninstall** removes exactly what it put in. Doing it by hand is
described in `README.txt`.

## Recommended setup

1. Turn the game's own DLSS Super Resolution on (any quality mode), so the game produces
   motion vectors.
2. Use the shipped `mgpu.ini`: `DcompOverlay=0`, `Monitor=auto`, `PanelOverPicture=1`.
3. In the NVIDIA Control Panel, set G-SYNC to **full screen only**.
4. For 3x or 4x frame generation, cap the game's frame rate so that game fps times the
   multiplier fits your monitor (about 40 fps for 4x on a 240 Hz screen). Without a cap
   the second card skips game frames and motion judders.
5. Press `CTRL+ALT+H` for the HUD. NR SKIP near 0 means the second card is keeping up.
6. Set the game's resolution before the bridge starts; restart the game after changing it.
   Changing DLSS quality or the DLSS model while playing is fine - the bridge follows it.

## Measured

RTX 3080 Ti (render) + RTX 5060 Ti 8 GB (neural, frame generation and display), 4K 240 Hz
G-SYNC monitor, Black Myth: Wukong Benchmark Tool, game at 2560x1440, capped at 40 fps,
dynamic neural resolution settling at 66-67%. Your numbers will differ.

| Configuration | Frame-to-frame jitter | 1% low fps |
|---|---|---|
| NVIDIA frame generation 4x, picture window on top | 0.51 ms | 132 |
| NVIDIA frame generation 4x, overlay mode | 1.3-1.5 ms | 76-80 |
| AMD frame generation 4x, picture window on top | 0.77 ms | 85 |
| 3x (either), picture window on top | 0.78-0.81 ms | 103-104 |

PresentMon, picture window on top: 99.9% of frames on independent flip, 1.9 ms from
Present to screen, G-SYNC active.

## Hotkeys

All of them can be changed in the panel's hotkey section: Ctrl and/or Alt with A-Z or F1-F12, each combination used once.

| Keys | Action |
|---|---|
| Home | ReShade overlay with the Atomic MGPU Bridge panel |
| CTRL+ALT+F10 | start the bridge by hand (with `AutoArm=0`) |
| CTRL+ALT+F6 | hide / show the picture |
| CTRL+ALT+F7 | view: output / input / split |
| CTRL+ALT+F3 / F4 | move the split line left / right (add SHIFT for big steps) |
| CTRL+ALT+F5 | pick the pass the intensity keys change |
| CTRL+ALT+F1 / F2 | intensity down / up |
| CTRL+ALT+H | FPS HUD |
| CTRL+ALT+G | frame generation on / off |
| CTRL+ALT+P | screenshot |
| CTRL+ALT+R | video recording start / stop |
| CTRL+ALT+B | benchmark start / stop |
| CTRL+ALT+W | benchmark sweep start / stop |
| CTRL+ALT+C / S | clock / stopwatch |

## Reporting a problem

Set `DetailedLog=1` in `mgpu.ini` (or tick "Detailed log" in the panel), reproduce the
problem, and attach `ReShade.log` from the game folder. Without it the log only holds
warnings, errors and a few setup lines.

## Building

Visual Studio 2022 and CMake:

```
cmake -B build -A x64
cmake --build build --config Release
```

The ReShade and NGX headers are fetched at the commits pinned in `VENDOR_LOCK.md`;
`.github/workflows/build.yml` does the whole build and packages the release zip.

To make the installer package from a local build, run `installer\make_package.bat` (or
`installer\make_package.ps1`). It collects the installer, the add-on and its files into
`dist\<name>\` and zips it, and bundles ReShade's add-on build with its licence (downloaded
from reshade.me and unpacked with 7-Zip, or from `-ReShadeSetup <file>`; `-NoReShade` leaves it
out).

## License and credit

MIT, as upstream. The add-on, its design and its 0.2.5 code are Marcelo Guibout's
([maohgad-web](https://github.com/maohgad-web)). The panel over the picture
(`src/live_panel.cpp`) comes from upstream 0.4.0, where it was contributed by
[NO1950](https://github.com/NO1950). This fork's changes are by davidm71.

The release also carries [ReShade](https://reshade.me) by crosire (its add-on support build,
BSD 3-clause; the licence text ships beside it as `reshade/ReShade-LICENSE.txt`), for the
installer to put in. Third-party code built into the add-on: AMD FidelityFX FSR 1 (MIT, Copyright (c) 2021
Advanced Micro Devices, Inc.) and NVIDIA Image Scaling SDK (MIT, Copyright (c) 2022 NVIDIA
CORPORATION & AFFILIATES); their license texts are in `src/third_party/fsr1/LICENSE.txt` and
`src/third_party/nis/LICENSE.txt`. See also `ACKNOWLEDGEMENTS.md` and `THIRD_PARTY.md`.

Upstream's development records (`RESULTS.md`, `METHOD.md`, `ARCHITECTURE.md`, `history/`),
which some of those files refer to, are in the upstream repository.
