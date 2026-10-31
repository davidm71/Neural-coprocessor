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
inline pfn_d3dcompile_t d3dcompile()
{
    static std::once_flag once;
    static pfn_d3dcompile_t fn = nullptr;
    std::call_once(once, [] {
        HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll");   // already loaded: no new reference
        if (m == nullptr) m = LoadLibraryW(L"d3dcompiler_47.dll");   // one reference, kept
        if (m != nullptr) fn = (pfn_d3dcompile_t)(void *)GetProcAddress(m, "D3DCompile");
    });
    return fn;
}
}
