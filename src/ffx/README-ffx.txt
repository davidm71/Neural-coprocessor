AMD FidelityFX API headers (FSR SDK 2.3.0, commit 60f4ea8), MIT licence - see the
licence text at the top of each header. Include paths flattened for this folder.
Used by the FG-test build's FrameGen=3 (FSR 3 frame generation) mode, which loads
AMD's signed amd_fidelityfx_framegeneration_dx12.dll at run time from the mgpu folder.
That DLL is AMD's binary, redistributable under AMD's licence (see LICENSE-AMD-FSR.txt
in the package); it is not built from this source.
