# Atomic MGPU — 20 FPS Community Edition

This archive is an experimental community variant of the uploaded Atomic MGPU 3.0 project.

## What changed

- `CMakeLists.txt`: added a `ATOMIC_COMMUNITY_20FPS` CMake option, **ON by default**.
- `src/gpu1_context.cpp`: one shared QPC-paced ceiling at `present_frame()` limits normal and FG presentation submissions to no more than 20 per second.
- `README.md`: prominent 20 FPS, Patreon, and DLSS compatibility notices.
- Dynamic FG (`FGDynamic=1`): the `Shown fps` slider is limited to **120 FPS**; loading `FGDynTarget` from INI and calling its UI setter are also clamped to 120 FPS (only when the Community build flag is enabled).

The 120 FPS dynamic setting is a *target for FG frame selection*, **not** an additional allowance to output at 120 FPS. The 20 FPS presentation cap remains in force.

This controls the **final bridge output**, not base game FPS. NVIDIA DLSS-G and AMD FSR multipliers are not disabled: generated frames still may be computed and discarded or delayed, so this is a demonstration cap, not a performance-friendly 2x license tier. It cannot be used as strong access control with freely available MIT-licensed source.

## Build the community edition on Windows

```cmd
cmake -S . -B build-community -A x64 -DATOMIC_COMMUNITY_20FPS=ON
cmake --build build-community --config Release --parallel 8
```

`build-community\mgpu_bridge.addon64` is the build output expected by the project's add-on packaging. Do not replace your known-good unrestricted addon until after testing.

If you have an existing CMake cache in a build directory, changing the default does not overwrite its cached option. Use the explicit `-DATOMIC_COMMUNITY_20FPS=ON` argument as shown.

To build the original unrestricted code for private comparison from a separately retained original source ZIP, use its normal build instructions. This archive also supports `-DATOMIC_COMMUNITY_20FPS=OFF` in a separate build directory; because this is MIT-licensed open source it is not a tamper-resistant paid-only license mechanism.

## Limitations

- 20 FPS is an approximate *upper ceiling* on `Present()` submission rate. V-Sync, display refresh, OS scheduling, frame starvation, compositor and device stalls can reduce actual displayed FPS further.
- Holding the present thread to 20 FPS can create FG queue overwrites and higher latency; this is expected.
- This build was statically checked in a Linux container. It has **not** been compiled under Visual Studio or tested in D3D12 on your GPUs.
- You must maintain the original project and third-party license notices. Verify that any separate premium-only code is owned or appropriately licensed.
