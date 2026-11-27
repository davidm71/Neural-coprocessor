## Atomic MGPU Bridge - Problem Report (3.0)

**First, collect your versions:** copy `mgpu-report.bat` (in the release, next to
AtomicMGPU-Setup.exe) into the game's exe folder and double-click it. It writes `mgpu-report.txt`
with your graphics cards, the exact NVIDIA driver version, and the version and SHA-256 hash of
ReShade, the bridge add-on, `nvngx_dlssnr.dll` and the other DLLs. It only reads; nothing is
changed or sent. **Attach mgpu-report.txt and ReShade.log.**

Then copy this block into a GitHub issue (or use **New issue → Problem report** on the repo) and fill
in every field. **Attach ReShade.log** from the game's folder - most problems can only be found
there. Wrong or old DLLs and drivers are the most common cause, so the "Files" section matters.

```
### Bridge
- Bridge version and build tag:     e.g. 3.0 IN17   (first [MGPU][R141] line in ReShade.log,
                                    or the tray icon's About)
- Installed with:                   AtomicMGPU-Setup.exe / by hand
- What happens:                     won't start / no picture / crash / stutter / artifacts / other
- When:                             at game start / when arming / in game / when opening the panel

### Graphics cards and drivers
- Card the GAME runs on:            e.g. RTX 3080 Ti
- Card the BRIDGE runs on:          e.g. RTX 5060 Ti
- NVIDIA driver version:            e.g. 5xx.xx   (mgpu-report.txt, NVIDIA App, or nvidia-smi)
- Other GPUs (iGPU etc.):
- Second card's slot and PCIe link: e.g. lower x16 slot, PCIe 4.0 x4  (GPU-Z → Bus Interface)
- Windows version:                  e.g. Windows 11 24H2 (winver)

### Files in the game's exe folder  (all in mgpu-report.txt - attach it)
- mgpu-report.txt attached:         yes / no
- ReShade version:                  e.g. 6.8.0 with add-on support  (ReShade64.dll / dxgi.dll → Properties → Details)
- nvngx_dlssnr.dll version:         in the mgpu folder → Properties → Details → File version
- nvngx_dlssnr.dll SHA-256:         from mgpu-report.txt (or: certutil -hashfile nvngx_dlssnr.dll SHA256)
- Where nvngx_dlssnr.dll came from: e.g. NVIDIA DLSS SDK / another game (which?)
- AMD FG DLL (only for AMD frame generation): present? version?
- Installer "Check" output:         paste the text from AtomicMGPU-Setup.exe → Check

### Display
- Monitor(s), resolution, refresh:  e.g. 3840x2160 @ 240 Hz
- Monitor plugged into:             game card / bridge card / iGPU
- HDR in Windows / in the game:     on / off

### Game
- Game and version:
- DirectX version:                  DX12 / DX11
- Game's upscaler:                  DLSS (which mode) / DLAA / FSR / TAA / off
- Game's own frame generation:      on / off

### Bridge settings (mgpu.ini)
- FrameGen:                         0 off / 1 built-in / 3 AMD / 4 NVIDIA
- FGMultiplier / FSRMult:
- SRUpscale (NIS / none):
- Any other settings you changed:

### Numbers (if it runs) - same scene, hold still ~30 s
- Real fps (game):
- Shown fps (after frame generation):
- NR ms (bridge HUD):
- Latency ms (bridge HUD):

### Notes
Anything else: what you tried, screenshots, when it last worked.
```
