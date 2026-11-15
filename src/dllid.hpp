// MGPU Bridge - ID1: what each DLL this mod loads actually is.
//
// One line per slot per launch: path, version, what kind of file it is (by its
// exported functions, never by name, size or version), and who signed it.
// LOG ONLY - nothing is blocked here. Newer versions of the same file always
// pass: no version, size or checksum is compared against a list. Official
// NVIDIA and AMD files are signed by their company; the rebuilt AMD DLL and
// third-party files are legitimately unsigned, so "not signed" is a fact, not an error.
// The signature check hashes the whole file, so it runs on its own thread.
#pragma once
#include <windows.h>

namespace mgpu::dllid
{
    // slot: 0 NR, 1 SR, 2 FG (NVIDIA), 3 AMD FSR, 4 rebuilt AMD.
    void check(int slot, HMODULE m);
    // What the file is, by its exports: "ANR FG", "NVIDIA NGX feature",
    // "AMD FidelityFX API" or "unknown".
    const char *kind(HMODULE m);
    // ANR FG's own export, which NVIDIA's nvngx_dlssg.dll does not have. AR1:
    // ANR support was removed; this is how its DLL is recognised and refused.
    bool is_anr(HMODULE m);
}
