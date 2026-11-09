// ---------------------------------------------------------------------------
// steam_hooks.cpp - SH1: how Steam's overlay hooks Present, reported once.
//
// History: OB1-OB3 tried to take Steam's Present hook out of the mod's own swap
// chain by giving it a private function table. The first real check
// (2026-11-08) found the table untouched - Steam patches DirectX's Present code
// itself - so a table swap could not help, and the option was removed (OB4).
// What stays is this report, made once when the bridge's swap chain is created
// and changing nothing: whether Steam redirected the function table, or patched
// dxgi.dll's Present / Present1 code in memory. If a Steam update ever moves to
// table hooks, this line is where it shows.
//
// Method: the swap chain's table must be System32\dxgi.dll's; dxgi.dll on disk
// must be the loaded build (time stamp, image size); slots 8 (Present) and 22
// (Present1) and the first 16 bytes of the functions they point at are compared
// with the file, relocated to where dxgi.dll is loaded.
// ---------------------------------------------------------------------------
#include <windows.h>

#include "steam_hooks.hpp"
#include "diag.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mgpu
{
namespace steam_hooks
{
namespace
{
bool in_range(const void *p, const void *base, size_t size)
{
    return p >= base && (const char *)p < (const char *)base + size;
}

bool file_read_rva(HANDLE f, const IMAGE_SECTION_HEADER *sec, unsigned nsec, DWORD rva, void *out, DWORD n)
{
    for (unsigned i = 0; i < nsec; ++i)
    {
        const DWORD va = sec[i].VirtualAddress, raw = sec[i].SizeOfRawData;
        if (rva >= va && rva + n <= va + raw)
        {
            LARGE_INTEGER at{}; at.QuadPart = (LONGLONG)sec[i].PointerToRawData + (rva - va);
            DWORD got = 0;
            return SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, out, n, &got, nullptr) && got == n;
        }
    }
    return false;
}
} // namespace

void report(void *real_swapchain)
{
    static bool said = false;
    if (said || real_swapchain == nullptr) return;
    said = true;
    void **vt = *(void ***)real_swapchain;

    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)vt, &m) || m == nullptr)
    { mgpu::diag::info("[MGPU][SH1] Steam hook report: the bridge swap chain's function table is not inside any module."); return; }
    wchar_t path[MAX_PATH] = {}, sys[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat_s(sys, L"\\dxgi.dll");
    if (_wcsicmp(path, sys) != 0)
    {
        char l[400];
        snprintf(l, sizeof l, "[MGPU][SH1] Steam hook report: the bridge swap chain's function table is not dxgi.dll's (%ls) "
                 "- something replaced it.", path);
        mgpu::diag::info(l);
        return;
    }
    const IMAGE_DOS_HEADER *dm = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS64 *nm = (const IMAGE_NT_HEADERS64 *)((const char *)m + dm->e_lfanew);
    const size_t msize = nm->OptionalHeader.SizeOfImage;
    if (!in_range(vt + 22, m, msize)) { mgpu::diag::info("[MGPU][SH1] Steam hook report: the table runs past dxgi.dll."); return; }

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { mgpu::diag::info("[MGPU][SH1] Steam hook report: dxgi.dll could not be opened."); return; }
    do
    {
        IMAGE_DOS_HEADER df{};
        DWORD got = 0;
        if (!ReadFile(f, &df, sizeof df, &got, nullptr) || got != sizeof df || df.e_magic != IMAGE_DOS_SIGNATURE) break;
        IMAGE_NT_HEADERS64 nf{};
        LARGE_INTEGER at{}; at.QuadPart = df.e_lfanew;
        if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, &nf, sizeof nf, &got, nullptr) || got != sizeof nf) break;
        if (nf.FileHeader.TimeDateStamp != nm->FileHeader.TimeDateStamp || nf.OptionalHeader.SizeOfImage != msize)
        { mgpu::diag::info("[MGPU][SH1] Steam hook report: dxgi.dll on disk is not the loaded build - not compared."); break; }
        static IMAGE_SECTION_HEADER sec[96];
        const unsigned nsec = nf.FileHeader.NumberOfSections < 96 ? nf.FileHeader.NumberOfSections : 96;
        at.QuadPart = df.e_lfanew + (LONGLONG)offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nf.FileHeader.SizeOfOptionalHeader;
        if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, sec, nsec * (DWORD)sizeof sec[0], &got, nullptr)) break;

        const unsigned long long delta = (unsigned long long)(uintptr_t)m - nf.OptionalHeader.ImageBase;
        HMODULE ov = GetModuleHandleW(L"gameoverlayrenderer64.dll");
        size_t ovs = 0;
        if (ov != nullptr)
        {
            const IMAGE_DOS_HEADER *od = (const IMAGE_DOS_HEADER *)ov;
            ovs = ((const IMAGE_NT_HEADERS64 *)((const char *)ov + od->e_lfanew))->OptionalHeader.SizeOfImage;
        }
        const unsigned slot[2] = { 8u, 22u };
        const char *verdict[2] = { "?", "?" };
        for (int k = 0; k < 2; ++k)
        {
            unsigned long long disk = 0;
            if (!file_read_rva(f, sec, nsec, (DWORD)((const char *)(vt + slot[k]) - (const char *)m), &disk, sizeof disk)) continue;
            void *orig = (void *)(uintptr_t)(disk + delta);
            if (vt[slot[k]] != orig)
            {
                verdict[k] = (ov != nullptr && in_range(vt[slot[k]], ov, ovs)) ? "TABLE redirected to Steam's overlay"
                                                                               : "table redirected elsewhere";
                continue;
            }
            unsigned char mem[16], dsk[16];
            memcpy(mem, orig, sizeof mem);
            if (!file_read_rva(f, sec, nsec, (DWORD)((const char *)orig - (const char *)m), dsk, sizeof dsk)) continue;
            verdict[k] = memcmp(mem, dsk, sizeof mem) != 0 ? "CODE patched in memory (an in-code hook)" : "untouched";
        }
        char l[400];
        snprintf(l, sizeof l, "[MGPU][SH1] Steam hook report (bridge swap chain, read-only): Present %s, Present1 %s. "
                 "Steam overlay %s.", verdict[0], verdict[1], ov != nullptr ? "loaded" : "not loaded");
        mgpu::diag::info(l);
    } while (false);
    CloseHandle(f);
}

} // namespace steam_hooks
} // namespace mgpu
