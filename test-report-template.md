## Test Report Template

Copy this block into your issue or Reddit reply and fill in every field. Reports missing the PCIe link line or the Reflex pair can't be compared against other results and will be asked to re-run.

```
### Hardware
- Render GPU (game runs here):      e.g. RTX 3080 Ti, driver 5xx.xx
- Neural GPU (bridge runs here):    e.g. RTX 5060 Ti, driver 5xx.xx
- Motherboard / chipset:            e.g. ASUS ROG Maximus IX Apex (Z270)
- Second GPU physical slot:         e.g. lower x16 slot (PCIEX16_2)
- Second GPU NEGOTIATED link:       e.g. PCIe 4.0 x4   <-- see note below
- CPU / RAM:

### Display
- Output resolution and refresh:    e.g. 2560x1440 @ 144 Hz
- Game monitor connected to:        Render GPU / Neural GPU / iGPU
- Other monitors attached (and to which GPU):

### Software
- Fork version / commit:            e.g. v0.3.1-fork, commit abc1234
- Frame-gen path:                   Built-in generator / FSR 3 / Off
- Game and renderer:                e.g. RoboCop Rogue City, DX12
- Game graphics preset:
- DLSS Super Resolution mode:       Off / Quality / Balanced / Performance

### HUD numbers — Reflex ON
Same scene, same settings. Hold still for ~30 s before reading the HUD.
- Base FPS (render GPU):
- Output FPS (after frame gen):
- Bridge transfer time (ms):
- Frame time / latency (ms):

### HUD numbers — Reflex OFF
- Base FPS (render GPU):
- Output FPS (after frame gen):
- Bridge transfer time (ms):
- Frame time / latency (ms):

### Notes
Stutter, artifacts, crashes, anything unusual:
```

### How to find your negotiated PCIe link width

The slot label on the board is not the answer; many boards drop the lower x16 slot to x4 (or share lanes with M.2). Report what the card actually negotiated under load:

- **GPU-Z:** open the Graphics Card tab for the neural GPU, read the **Bus Interface** field (e.g. `PCIe x16 4.0 @ x4 4.0`). Click the `?` next to it to run the render test — the value after `@` is the live link.
- **nvidia-smi:** run `nvidia-smi -q -d PCI` in a terminal and read `Link Width > Current` and `Link Gen > Current` for each GPU.

Read it while the game is running; idle cards downclock the link and will report a lower Gen than they use under load.
