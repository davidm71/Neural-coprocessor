// archspoof.hpp - AS1: report RTX 20/30/40 cards as Blackwell to the DLSS-NR
// runtime's architecture check only (see archspoof.cpp). ArchSpoof=0 disables.
#pragma once
#include <windows.h>

namespace mgpu::archspoof
{
// Route the NR DLL's NvAPI architecture question through the bridge. Call
// after the DLL is loaded and before NGX Init / CreateFeature. Once per DLL.
void apply(HMODULE nr_dll, const char *site);
// One log line: how often the question was asked and rewritten.
void report();
}
