// ---------------------------------------------------------------------------
// d3dc.hpp - RV1: one shared handle to d3dcompiler_47.dll's D3DCompile.
// The module is looked up (or loaded) ONCE per process and its reference is
// kept for the process's life, instead of each pipeline (re)build adding a
// LoadLibrary reference that is never released.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <d3dcompiler.h>
#include <mutex>

namespace mgpu
{
typedef HRESULT (WINAPI *pfn_d3dcompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                           LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
// SC1. REPORTED 2026-11-18 (Rise of the Tomb Raider): the game ships its own,
// older d3dcompiler_47.dll beside the exe and it is already loaded, so a
// lookup by name got THAT copy - which does not know shader model 5.1
// ("error X3506: unrecognized compiler target 'cs_5_1'") and frame
// generation never started. Always use Windows' own copy from System32, by
// full path, so a game's copy can neither shadow it nor be shadowed by it.
inline HMODULE sys_d3dcompiler_module()
{
    static std::once_flag once;
    static HMODULE m = nullptr;
    std::call_once(once, [] {
        wchar_t p[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 24)
        {
            lstrcatW(p, L"\\d3dcompiler_47.dll");
            m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // one reference, kept
        }
        if (m == nullptr) m = LoadLibraryW(L"d3dcompiler_47.dll");           // last resort
    });
    return m;
}

inline pfn_d3dcompile_t d3dcompile()
{
    static std::once_flag once;
    static pfn_d3dcompile_t fn = nullptr;
    std::call_once(once, [] {
        if (HMODULE m = sys_d3dcompiler_module()) fn = (pfn_d3dcompile_t)(void *)GetProcAddress(m, "D3DCompile");
    });
    return fn;
}
}
