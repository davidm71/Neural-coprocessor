// hookprobe.cpp - SO5. See hookprobe.hpp.
#include "hookprobe.hpp"
#include "crashlog.hpp"   // PG1
#include "diag.hpp"
#include "gpu1_context.hpp"   // ui_ini_read

#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace mgpu
{
namespace hookprobe
{
namespace
{

std::mutex g_mx;
uintptr_t  g_slot[2] = {};          // the addresses of the Present and Present1 slots in dxgi's vtable
char       g_last[2][700] = {};     // the chains last logged
bool       g_on = false;
ULONGLONG  g_next = 0;

bool rd(uintptr_t a, void *out, size_t n)
{
    SIZE_T got = 0;
    return a != 0 && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, out, n, &got) && got == n;
}

// "dxgi.dll+0x1a2b0", or what the memory is when no module holds it.
void name_of(uintptr_t a, char *out, size_t n)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)a, &m) && m != nullptr)
    {
        wchar_t w[MAX_PATH] = {};
        GetModuleFileNameW(m, w, MAX_PATH);
        const wchar_t *f = wcsrchr(w, L'\\');
        char nm[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, f ? f + 1 : w, -1, nm, sizeof nm, nullptr, nullptr);
        snprintf(out, n, "%s+0x%llx", nm, (unsigned long long)(a - (uintptr_t)m));
        return;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) { snprintf(out, n, "0x%llx (unreadable)", (unsigned long long)a); return; }
    snprintf(out, n, "0x%llx (%s, no module, block at 0x%llx)", (unsigned long long)a,
             mbi.State == MEM_FREE ? "FREED memory" : mbi.State == MEM_RESERVE ? "reserved, not committed"
             : mbi.Type == MEM_PRIVATE ? "private code" : "mapped code",
             (unsigned long long)(uintptr_t)mbi.AllocationBase);
}

// The jump at the start of the code at a, if there is one.
bool jump_at(uintptr_t a, uintptr_t &to)
{
    uint8_t b[16] = {};
    if (!rd(a, b, sizeof b)) return false;
    int32_t r32 = 0; uint64_t q = 0;
    if (b[0] == 0xE9) { memcpy(&r32, b + 1, 4); to = a + 5 + (intptr_t)r32; return true; }              // jmp rel32
    if (b[0] == 0xEB) { to = a + 2 + (intptr_t)(int8_t)b[1]; return true; }                              // jmp rel8
    if (b[0] == 0xFF && b[1] == 0x25) { memcpy(&r32, b + 2, 4); return rd(a + 6 + (intptr_t)r32, &to, 8); }   // jmp [rip+x]
    if (b[0] == 0x48 && b[1] == 0xFF && b[2] == 0x25) { memcpy(&r32, b + 3, 4); return rd(a + 7 + (intptr_t)r32, &to, 8); }
    if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) { memcpy(&q, b + 2, 8); to = (uintptr_t)q; return true; }  // mov rax; jmp rax
    if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) { memcpy(&q, b + 2, 8); to = (uintptr_t)q; return true; }  // mov r11; jmp r11
    if (b[0] == 0x68 && b[5] == 0xC7 && b[6] == 0x44 && b[7] == 0x24 && b[8] == 0x04 && b[13] == 0xC3)   // push lo; mov [rsp+4], hi; ret
    {
        uint32_t lo = 0, hi = 0; memcpy(&lo, b + 1, 4); memcpy(&hi, b + 9, 4);
        to = (uintptr_t)lo | ((uintptr_t)hi << 32); return true;
    }
    return false;
}

void chain(uintptr_t slot, char *out, size_t n)
{
    out[0] = 0;
    uintptr_t a = 0;
    if (!rd(slot, &a, sizeof a) || a == 0) { snprintf(out, n, "slot unreadable"); return; }
    size_t w = 0;
    for (int hop = 0; hop < 6; ++hop)
    {
        char nm[300];
        name_of(a, nm, sizeof nm);
        const int u = snprintf(out + w, n - w, "%s%s", hop ? " -> jump to " : "", nm);
        if (u < 0 || (size_t)u >= n - w) return;
        w += (size_t)u;
        uintptr_t to = 0;
        if (!jump_at(a, to) || to == 0 || to == a) break;
        a = to;
    }
}

void check(bool force)
{
    char now[2][700];
    for (int i = 0; i < 2; ++i) chain(g_slot[i], now[i], sizeof now[i]);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (int i = 0; i < 2; ++i)
            if (strcmp(now[i], g_last[i]) != 0) { changed = true; memcpy(g_last[i], now[i], sizeof now[i]); }
    }
    if (!changed && !force) return;
    char l[1600];
    snprintf(l, sizeof l, "[MGPU][SO5] %s Present: %s | Present1: %s. A module other than dxgi.dll at the start of the "
             "chain is code that hooked it (gameoverlayrenderer64 = Steam's overlay, sl.* = Streamline, RTSSHooks = "
             "RivaTuner, discord_hook = Discord). A crash that jumps into freed memory is that code going away.",
             force ? "who handles" : "CHANGED -", now[0], now[1]);
    mgpu::diag::info(l);
}

} // namespace

void init(IUnknown *queue)
{
    if (g_on || queue == nullptr) return;
    if (mgpu::gpu1::ui_ini_read("PresentHookProbe", 1) == 0) return;
    wchar_t p[MAX_PATH] = {};
    const UINT k = GetSystemDirectoryW(p, MAX_PATH);
    if (k == 0 || k + 12 >= MAX_PATH) return;
    wcscat_s(p, MAX_PATH, L"\\dxgi.dll");
    HMODULE h = GetModuleHandleW(p);       // Windows' own, not ReShade's dxgi.dll beside the game
    if (h == nullptr) h = LoadLibraryW(p);
    if (h == nullptr) { mgpu::diag::warn("[MGPU][SO5] Windows' dxgi.dll not found - no Present hook probe."); return; }
    using cf2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
    cf2 create = (cf2)(void *)GetProcAddress(h, "CreateDXGIFactory2");
    IDXGIFactory2 *f = nullptr;
    if (create == nullptr || FAILED(create(0, __uuidof(IDXGIFactory2), (void **)&f)) || f == nullptr)
    { mgpu::diag::warn("[MGPU][SO5] no DXGI factory from Windows' dxgi.dll - no Present hook probe."); return; }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.Scaling = DXGI_SCALING_STRETCH; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    IDXGISwapChain1 *sc = nullptr;
    const HRESULT hr = f->CreateSwapChainForComposition(queue, &d, nullptr, &sc);
    if (FAILED(hr) || sc == nullptr)
    {
        char l[200]; snprintf(l, sizeof l, "[MGPU][SO5] probe swap chain refused (0x%08lX) - no Present hook probe.", (unsigned long)hr);
        mgpu::diag::warn(l);
        f->Release();
        return;
    }
    // The vtable lives in dxgi.dll's image, which stays loaded: its slots can
    // be re-read after the probe chain is gone.
    void **vt = *(void ***)sc;
    g_slot[0] = (uintptr_t)&vt[8];    // IDXGISwapChain::Present
    g_slot[1] = (uintptr_t)&vt[22];   // IDXGISwapChain1::Present1
    mgpu::crashlog::set_present_slots(g_slot[0], g_slot[1]);   // PG1: where a hook rewrites Present
    sc->Release();
    f->Release();
    g_on = true;
    g_next = GetTickCount64() + 10000ull;
    check(true);
}

void tick()
{
    if (!g_on) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next) return;
    g_next = now + 10000ull;
    check(false);
}

void last(char *out, unsigned n)
{
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    snprintf(out, n, "%s", g_last[0][0] ? g_last[0] : "not probed");
}

} // namespace hookprobe
} // namespace mgpu
