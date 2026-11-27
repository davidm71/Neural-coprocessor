// ---------------------------------------------------------------------------
// archspoof.cpp - AS1: let the DLSS-NR runtime create on RTX 20/30/40 cards.
//
// REPORTED 2026-11-19 (an RTX 4090 neural card, IN46): every setup step of
// DLSS-NR succeeded and CreateFeature(Reserved18) failed, with the same
// modified 310.9.16 nvngx_dlssnr.dll that works on an RTX 5060 Ti. Public
// write-ups of another project that runs this runtime (its TECHNICAL.md)
// describe why: the DLL asks NVIDIA's NvAPI for the GPU's architecture
// (NvAPI_GPU_GetArchInfo, reached through nvapi_QueryInterface) and refuses
// feature 18 below Blackwell ("Unsupported GPU architecture 0x%x, minimum
// required 0x%x"), even in the builds that carry kernels for older cards.
//
// This file is our own implementation of that idea (no code was taken):
// only inside the NR DLL's own process view, the answer to that one question
// is rewritten to "Blackwell" when the real card is older. Nothing on disk is
// changed, every other NvAPI call goes to NVIDIA untouched, and on an RTX 50
// card the real answer is already Blackwell, so nothing is rewritten.
//
// How the NR DLL is reached: its import of GetProcAddress (the route it uses
// to find nvapi_QueryInterface) is pointed at us, and - in case it looked
// those up already, e.g. in its DllMain - its writable data is searched for
// the real nvapi_QueryInterface and GetArchInfo addresses, which are
// replaced with ours. ArchSpoof=0 in mgpu.ini turns all of this off.
// ---------------------------------------------------------------------------
#include "archspoof.hpp"
#include "diag.hpp"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace mgpu::archspoof
{
namespace
{
constexpr unsigned kGetArchInfoId = 0xD8265D24u;   // NvAPI_GPU_GetArchInfo
constexpr unsigned kBlackwell2    = 0x1B0u;        // RTX 50 (GB20x): the minimum the NR runtime accepts

typedef void *(__cdecl *pf_qi)(unsigned int);
typedef int (__cdecl *pf_arch)(void *gpu, void *info);
typedef FARPROC (WINAPI *pf_gpa)(HMODULE, LPCSTR);

std::atomic<pf_qi>   g_real_qi{nullptr};
std::atomic<pf_arch> g_real_arch{nullptr};
pf_gpa               g_real_gpa = nullptr;
std::atomic<unsigned> g_rewrites{0}, g_calls{0};
std::atomic<bool>    g_said{false};

int __cdecl as_arch(void *gpu, void *info)
{
    pf_arch real = g_real_arch.load(std::memory_order_relaxed);
    const int st = real ? real(gpu, info) : -1;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (st == 0 && info != nullptr)
    {
        // NV_GPU_ARCH_INFO: { NvU32 version; architecture; implementation; revision; }
        unsigned *arch = reinterpret_cast<unsigned *>(static_cast<char *>(info) + 4);
        const unsigned was = *arch;
        if (was < kBlackwell2)
        {
            *arch = kBlackwell2;
            g_rewrites.fetch_add(1, std::memory_order_relaxed);
            if (!g_said.exchange(true))
            {
                char l[260];
                snprintf(l, sizeof l, "[MGPU][AS1] the NR runtime asked for the GPU architecture: real 0x%X, "
                         "answered 0x%X (Blackwell) so it will create on this card. ArchSpoof=0 turns this off.",
                         was, kBlackwell2);
                mgpu::diag::info(l);
            }
        }
    }
    return st;
}

void *__cdecl as_qi(unsigned int id)
{
    pf_qi real = g_real_qi.load(std::memory_order_relaxed);
    void *p = real ? real(id) : nullptr;
    if (p != nullptr && id == kGetArchInfoId)
    {
        g_real_arch.store((pf_arch)p, std::memory_order_relaxed);
        return (void *)&as_arch;
    }
    return p;
}

FARPROC WINAPI as_gpa(HMODULE m, LPCSTR name)
{
    FARPROC real = g_real_gpa ? g_real_gpa(m, name) : nullptr;
    if (real != nullptr && name != nullptr && ((ULONG_PTR)name >> 16) != 0 &&
        std::strcmp(name, "nvapi_QueryInterface") == 0)
    {
        g_real_qi.store((pf_qi)real, std::memory_order_relaxed);
        return (FARPROC)&as_qi;
    }
    return real;
}

// Point the module's import of `fn` (from any DLL) at `repl`. Returns the slot count.
unsigned patch_iat(HMODULE mod, const char *fn, void *repl, void **orig)
{
    auto *base = reinterpret_cast<unsigned char *>(mod);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;
    unsigned hits = 0;
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (imp->OriginalFirstThunk == 0) continue;
        auto *oft = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto *ft  = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft)
        {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto *ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + oft->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(ibn->Name), fn) != 0) continue;
            void **slot = reinterpret_cast<void **>(&ft->u1.Function);
            if (*slot == repl) continue;
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (orig != nullptr && *orig == nullptr) *orig = *slot;
            *slot = repl;
            VirtualProtect(slot, sizeof(void *), old, &old);
            ++hits;
        }
    }
    return hits;
}

// Replace every pointer-aligned word equal to `find` in the module's writable sections.
unsigned scan_data(HMODULE mod, const void *find, const void *repl)
{
    if (find == nullptr) return 0;
    auto *base = reinterpret_cast<unsigned char *>(mod);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned hits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if ((sec->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
        auto *p = reinterpret_cast<const void **>(base + sec->VirtualAddress);
        const size_t n = sec->Misc.VirtualSize / sizeof(void *);
        for (size_t k = 0; k < n; ++k)
        {
            __try
            {
                if (p[k] == find) { p[k] = repl; ++hits; }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
    }
    return hits;
}

bool ini_off()
{
    wchar_t path[MAX_PATH * 2] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ini_off, &self) || GetModuleFileNameW(self, path, MAX_PATH * 2) == 0)
        return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    wcscpy_s(slash + 1, (size_t)(path + MAX_PATH * 2 - (slash + 1)), L"mgpu.ini");
    // First "ArchSpoof=" line wins, like the rest of mgpu.ini.
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[32768] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(f);
    for (const char *c = buf; (c = strstr(c, "ArchSpoof=")) != nullptr; c += 10)
        if (c == buf || c[-1] == '\n' || c[-1] == '\r')
            return c[10] == '0';
    return false;
}
}   // namespace

void apply(HMODULE nr_dll, const char *site)
{
    if (nr_dll == nullptr) return;
    static std::atomic<HMODULE> s_done{nullptr};
    if (s_done.load() == nr_dll) return;
    if (ini_off())
    {
        mgpu::diag::info("[MGPU][AS1] ArchSpoof=0: the NR runtime sees the card's real architecture.");
        s_done.store(nr_dll);
        return;
    }
    HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
    if (nvapi == nullptr) nvapi = LoadLibraryW(L"nvapi64.dll");
    pf_qi qi = nvapi ? (pf_qi)GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
    if (qi == nullptr)
    {
        mgpu::diag::warn("[MGPU][AS1] nvapi64.dll / nvapi_QueryInterface not found - architecture answer left alone.");
        return;
    }
    g_real_qi.store(qi);
    void *arch = qi(kGetArchInfoId);
    if (arch != nullptr) g_real_arch.store((pf_arch)arch);

    void *gpa_orig = nullptr;
    const unsigned slots = patch_iat(nr_dll, "GetProcAddress", (void *)&as_gpa, &gpa_orig);
    if (gpa_orig != nullptr && g_real_gpa == nullptr) g_real_gpa = (pf_gpa)gpa_orig;
    if (g_real_gpa == nullptr) g_real_gpa = &GetProcAddress;
    const unsigned d_qi   = scan_data(nr_dll, (void *)qi, (void *)&as_qi);
    const unsigned d_arch = scan_data(nr_dll, arch, (void *)&as_arch);
    s_done.store(nr_dll);

    char l[300];
    snprintf(l, sizeof l, "[MGPU][AS1] %s: NR runtime's architecture question routed through the bridge "
             "(GetProcAddress import slots %u, cached nvapi_QueryInterface %u, cached GetArchInfo %u). "
             "Older cards are reported as Blackwell to it; RTX 50 is untouched.",
             site, slots, d_qi, d_arch);
    mgpu::diag::info(l);
}

void report()
{
    char l[200];
    snprintf(l, sizeof l, "[MGPU][AS1] GetArchInfo seen %u time(s), rewritten %u time(s).",
             g_calls.load(), g_rewrites.load());
    mgpu::diag::info(l);
}
}   // namespace mgpu::archspoof
