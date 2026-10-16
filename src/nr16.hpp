#pragma once

// MGPU Bridge - R208 FP16 input path (experimental, off by default)
//
// WHAT THIS IS FOR
//
// An experimental colour option. DLSS-NR handles colour differently when it is
// fed values closer to linear light, in a 16-bit float texture. The live stream
// is converted on the second card, right before NR and right after it:
//
//   tex_in (the game's frame) --encode: code ^ power--> in16 (R16G16B16A16_FLOAT)
//   NR: Color = in16, Output = out16 (R16G16B16A16_FLOAT)
//   out16 --decode: code ^ (1/power)--> tex_out (what everything downstream reads)
//
// THE CONTRACT
//
//   - OPTIONAL MODULE, keys in mgpu.ini, read at arm:
//       NRInput16       absent or anything but 4: off. The stream is the 0.2.5
//                       stream: no texture, no kernel, no extra pass.
//                       4: on.
//       NRInput16Power  1.0-3.0, default 2.2 (about linear light). 1.0 is what
//                       NR gets without this module.
//   - OFF COSTS: one file read at arm, then per frame a few integer compares and
//     two relaxed atomic loads. No GPU work, no memory.
//   - Used only when Passes=1 and the whole frame NR sees is evaluated - the
//     full frame, or with SRUpscale on the whole reduced frame (fork: the
//     textures follow the SR size via resize()); otherwise the normal path.
//   - Three failed evaluates in a row turn it off for the arm and force a
//     Reset, so the stream falls back to the normal path instead of freezing.
//   - Holds no gpu1_context type: gpu1_context hands it D3D12 objects and plain
//     values.

#include <windows.h>
#include <d3d12.h>

namespace mgpu::nr16
{
// At arm, after the stream's textures exist. Reads the keys; when on, builds
// the two kernels and the two FP16 textures. False (and off) otherwise.
bool arm(ID3D12Device *dev, unsigned width, unsigned height);

// One relaxed load.
bool active();

// Fork: re-size the two FP16 textures (SRUpscale evaluates NR at the reduced
// size). Only call when the GPU no longer uses them. True when the path is on
// at the new size; a size change forces the next evaluate's Reset.
bool resize(unsigned width, unsigned height);
// Fork: the textures are exactly width x height.
bool fits(unsigned width, unsigned height);

// Before the evaluate. `src` is the frame NR would have read (in
// NON_PIXEL_SHADER_RESOURCE). Records the encode on `cl` and returns the FP16
// textures to hand NGX as Color and Output. `parity` picks the descriptor set.
void encode(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *src,
            ID3D12Resource **nr_color, ID3D12Resource **nr_output);

// After the evaluate. Decodes NR's FP16 output into `dst` (in
// UNORDERED_ACCESS), or leaves `dst` alone when the evaluate failed.
void decode(ID3D12GraphicsCommandList *cl, unsigned parity, ID3D12Resource *dst,
            bool evaluate_ok);

// True once after arm and once after the path turns itself off: the caller
// sets DLSSNR.Reset=1 on that evaluate. Off: one relaxed load.
bool take_reset();

// From stream_release, when the GPU no longer uses the stream's resources.
void release();

// R219-2. What the last arm read from mgpu.ini: on = NRInput16 was 4, power as
// used (2.2 when off, absent or out of range). For the panel's restart line.
void ini_values(bool &on, float &power);

// R219-2. The same keys read from the file now, with the same reader arm uses,
// so the panel and the arm never parse them differently. Reads the file.
void ini_file_values(bool &on, float &power);
}
