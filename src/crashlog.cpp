// ---------------------------------------------------------------------------
// crashlog.cpp - CR1. See crashlog.hpp.
// ---------------------------------------------------------------------------
#include "crashlog.hpp"
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"

#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mgpu
{
namespace crashlog
{
namespace
{

PVOID g_veh = nullptr;
HMODULE g_self = nullptr;

// CR2: Windows' own list of recently UNLOADED modules (ntdll), so an address
// in a DLL that is no longer there can still be named.
struct unload_ev
{
    PVOID  BaseAddress;
    SIZE_T SizeOfImage;
    ULONG  Sequence;
    ULONG  TimeDateStamp;
    ULONG  CheckSum;
    WCHAR  ImageName[32];
    ULONG  Version[2];
};
typedef VOID (NTAPI *pfn_unload_trace)(PULONG *ElementSize, PULONG *ElementCount, PVOID *EventTrace);
pfn_unload_trace g_unload_trace = nullptr;

bool read_mem(const void *p, void *out, size_t n);

// ---- DW1: every DLL load and unload, as it happens ----
//
// Several crashes jumped into code that was no longer there, from DXGI's
// present (dxgi.dll+0x17f9d6): something that hooks Present went away while
// its hook stayed. Windows' own unload list named it once (sl.common.dll) and
// not at all the other times. The loader tells us about every load and
// unload; the callback runs under the loader lock, so it only copies into a
// ring here, and the bridge thread writes the lines to the log later.
struct dw_ev
{
    volatile LONG seq;           // index + 1 once the entry is complete
    unsigned char kind;          // 1 load, 2 unload
    ULONGLONG     ms;            // GetTickCount64
    const char   *base;
    SIZE_T        size;
    char          name[56];
};
const unsigned DW_N = 256;
dw_ev g_dw[DW_N] = {};
volatile LONG g_dw_w = 0;        // next index to write
LONG g_dw_r = 0;                 // next index to log (bridge thread)
ULONGLONG g_dw_t0 = 0;           // when watching started
PVOID g_dw_cookie = nullptr;

// ---- PN1: keep Streamline's code in memory ----
//
// The game starts NVIDIA Streamline (sl.*.dll) and shuts it down again about
// 9 s later; the crashes were calls from DXGI's present into sl.common.dll
// after it had been unloaded (a hook it left behind). Pinning a module makes
// FreeLibrary leave it mapped until the game exits, so a late call lands in
// real code instead of freed memory. Streamline's own shutdown still runs; only
// the unmapping is skipped. Cost: a few MB. mgpu.ini PinStreamline=0 turns it off.
//
// The loader-notification callback runs under the loader lock, where calling
// back into the loader is not allowed, so it only signals; this small thread
// does the pinning a moment later (Streamline unloads seconds after loading).
HANDLE g_pin_ev = nullptr;
volatile LONG g_pin_stop = 0;

bool is_sl(const char *n)
{
    if (_strnicmp(n, "sl.", 3) != 0) return false;
    const size_t k = strlen(n);
    return k > 7 && _stricmp(n + k - 4, ".dll") == 0;
}
bool is_ngx_family(const char *n) { return is_sl(n) || _strnicmp(n, "nvngx", 5) == 0; }

bool pin_enabled()
{
    if (g_self == nullptr) return true;
    wchar_t p[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(g_self, p, MAX_PATH * 2);
    wchar_t *slash = (n != 0 && n < MAX_PATH * 2) ? wcsrchr(p, L'\\') : nullptr;
    if (slash == nullptr || (size_t)(slash - p) + 12u >= (size_t)(MAX_PATH * 2)) return true;
    wcscpy(slash + 1, L"mgpu.ini");
    FILE *fp = _wfopen(p, L"rb");
    if (fp == nullptr) return true;
    static char buf[256 * 1024];
    const size_t got = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[got] = '\0';
    const char *v = mgpu::config::find(buf, got, "PinStreamline");
    return v == nullptr || *v != '0';
}

DWORD WINAPI pin_thread(LPVOID)
{
    const bool on = pin_enabled();
    if (!on)
    {
        mgpu::diag::info("[MGPU][PN1] PinStreamline=0: Streamline's DLLs are left to unload normally.");
        return 0;
    }
    mgpu::diag::info("[MGPU][PN1] Streamline's DLLs (sl.*) are kept in memory once loaded, so a hook it "
                     "leaves behind cannot jump into freed code (PinStreamline=0 turns this off).");
    char pinned[24][56] = {};
    unsigned n_pinned = 0;
    for (;;)
    {
        // every module now loaded; pin each sl.* not yet pinned
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me;
            me.dwSize = sizeof me;
            for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            {
                char nm[56];
                size_t k = 0;
                for (; k + 1 < sizeof nm && me.szModule[k] != 0; ++k)
                    nm[k] = (me.szModule[k] >= 0x20 && me.szModule[k] < 0x7F) ? (char)me.szModule[k] : '?';
                nm[k] = '\0';
                if (!is_sl(nm)) continue;
                bool seen = false;
                for (unsigned i = 0; i < n_pinned && !seen; ++i) seen = _stricmp(pinned[i], nm) == 0;
                if (seen || n_pinned >= 24) continue;
                HMODULE h = nullptr;
                const BOOL ok2 = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, me.szModule, &h);
                snprintf(pinned[n_pinned++], 56, "%s", nm);
                char l[160];
                snprintf(l, sizeof l, ok2 ? "[MGPU][PN1] kept in memory: %s" : "[MGPU][PN1] could not keep %s (error %lu)",
                         nm, ok2 ? 0ul : (unsigned long)GetLastError());
                mgpu::diag::info(l);
            }
            CloseHandle(snap);
        }
        WaitForSingleObject(g_pin_ev, INFINITE);
        if (g_pin_stop != 0) return 0;
    }
}

struct dw_ustr { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct dw_data { ULONG Flags; const dw_ustr *FullDllName; const dw_ustr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID (CALLBACK *pfn_dw_cb)(ULONG reason, const dw_data *data, PVOID ctx);
typedef LONG (NTAPI *pfn_dw_reg)(ULONG flags, pfn_dw_cb cb, PVOID ctx, PVOID *cookie);

VOID CALLBACK dw_cb(ULONG reason, const dw_data *d, PVOID)
{
    if (d == nullptr || (reason != 1 && reason != 2)) return;
    const LONG idx = InterlockedIncrement(&g_dw_w) - 1;
    dw_ev &e = g_dw[(unsigned)idx % DW_N];
    e.seq = 0;
    e.kind = (unsigned char)reason;
    e.ms = GetTickCount64();
    e.base = (const char *)d->DllBase;
    e.size = d->SizeOfImage;
    size_t k = 0;
    if (d->BaseDllName != nullptr && d->BaseDllName->Buffer != nullptr)
        for (; k + 1 < sizeof e.name && k < d->BaseDllName->Length / 2u; ++k)
        {
            const WCHAR c = d->BaseDllName->Buffer[k];
            e.name[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    e.name[k] = '\0';
    InterlockedExchange(&e.seq, idx + 1);
    if (reason == 1 && g_pin_ev != nullptr && is_sl(e.name)) SetEvent(g_pin_ev);   // PN1
}

// The newest unload (our own record) that covered `addr`.
bool dw_unloaded(const void *addr, char *name, size_t n, unsigned long long &off)
{
    const LONG w = g_dw_w;
    for (LONG i = w - 1; i >= 0 && i >= w - (LONG)DW_N; --i)
    {
        const dw_ev &e = g_dw[(unsigned)i % DW_N];
        if (e.seq != i + 1 || e.kind != 2 || e.base == nullptr) continue;
        if ((const char *)addr >= e.base && (const char *)addr < e.base + e.size)
        {
            snprintf(name, n, "%s", e.name);
            off = (unsigned long long)((const char *)addr - e.base);
            return true;
        }
    }
    return false;
}

bool unloaded_name(const void *addr, char *name, size_t n, unsigned long long &off)
{
    if (g_unload_trace == nullptr) return false;
    PULONG esz = nullptr, ecnt = nullptr; PVOID tr = nullptr;
    g_unload_trace(&esz, &ecnt, &tr);
    if (esz == nullptr || ecnt == nullptr || tr == nullptr || *esz < sizeof(unload_ev)) return false;
    // CR3: EventTrace is the ADDRESS of ntdll's pointer to the array, not the
    // array - one more dereference (read safely).
    PVOID arr = nullptr;
    if (!read_mem(tr, &arr, sizeof arr) || arr == nullptr) return false;
    const char *base = (const char *)arr;
    for (ULONG i = 0; i < *ecnt && i < 64u; ++i)
    {
        unload_ev ev{};
        if (!read_mem(base + (size_t)i * *esz, &ev, sizeof ev)) break;
        const unload_ev *e = &ev;
        if (e->BaseAddress == nullptr || e->SizeOfImage == 0 || e->SizeOfImage > 0x40000000) continue;
        const char *b = (const char *)e->BaseAddress;
        if ((const char *)addr >= b && (const char *)addr < b + e->SizeOfImage)
        {
            size_t k = 0;
            for (; k + 1 < n && k < 32 && e->ImageName[k] != 0; ++k)
            {
                const WCHAR c = e->ImageName[k];
                name[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            name[k] = '\0';
            off = (unsigned long long)((const char *)addr - b);
            return true;
        }
    }
    return false;
}

bool read_mem(const void *p, void *out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
volatile LONG g_count = 0;
wchar_t g_path[MAX_PATH * 2] = {};

bool fatal(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:      // 0xC0000005
    case EXCEPTION_STACK_OVERFLOW:        // 0xC00000FD
    case EXCEPTION_ILLEGAL_INSTRUCTION:   // 0xC000001D
    case EXCEPTION_PRIV_INSTRUCTION:      // 0xC0000096
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    // 0xC0000094
    case EXCEPTION_IN_PAGE_ERROR:         // 0xC0000006
    case 0xC0000374u:                     // heap corruption
    case 0xC0000409u:                     // stack buffer overrun / fail fast
        return true;
    default:
        return false;
    }
}

const char *code_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:    return "ACCESS VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:      return "STACK OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:    return "PRIVILEGED INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "DIVIDE BY ZERO";
    case EXCEPTION_IN_PAGE_ERROR:       return "IN-PAGE ERROR";
    case 0xC0000374u:                   return "HEAP CORRUPTION";
    case 0xC0000409u:                   return "STACK BUFFER OVERRUN";
    default:                            return "FATAL ERROR";
    }
}

// "module.dll+0x1234" for an address, or "?+0x..." when no module owns it.
void where(const void *addr, char *out, size_t n)
{
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h) && h != nullptr)
    {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(h, full, MAX_PATH);
        const char *base = strrchr(full, '\\');
        base = (base != nullptr) ? base + 1 : full;
        snprintf(out, n, "%s+0x%llx", base, (unsigned long long)((const char *)addr - (const char *)h));
    }
    else
    {
        char nm[40]; unsigned long long off = 0;
        if (unloaded_name(addr, nm, sizeof nm, off) || dw_unloaded(addr, nm, sizeof nm, off))
            snprintf(out, n, "UNLOADED %s+0x%llx", nm, off);   // CR2/DW1: a DLL that has been unloaded
        else
            snprintf(out, n, "?+0x%llx", (unsigned long long)(uintptr_t)addr);
    }
}

void write_file(const char *text, size_t len)
{
    if (g_path[0] == L'\0') return;
    HANDLE f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, text, (DWORD)len, &wr, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

// REV: the stack-overflow path. Almost no stack is left, so no snprintf, no
// call chain, no ReShade log: a fixed text, the module name and a hex offset,
// built by hand in small buffers.
void hex(char *&p, unsigned long long v)
{
    static const char d[] = "0123456789abcdef";
    char tmp[16]; int n = 0;
    do { tmp[n++] = d[v & 15u]; v >>= 4; } while (v != 0 && n < 16);
    *p++ = '0'; *p++ = 'x';
    while (n > 0) *p++ = tmp[--n];
}

void overflow_line(const void *addr)
{
    char line[400];
    char *p = line;
    const char *head = "[MGPU][CR1] STACK OVERFLOW (0xC00000FD) in ";
    while (*head) *p++ = *head++;
    HMODULE h = nullptr;
    char full[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h) && h != nullptr &&
        GetModuleFileNameA(h, full, MAX_PATH) != 0)
    {
        const char *b = full;
        for (const char *q = full; *q; ++q) if (*q == '\\') b = q + 1;
        while (*b && p < line + 300) *p++ = *b++;
        *p++ = '+';
        hex(p, (unsigned long long)((const char *)addr - (const char *)h));
    }
    else hex(p, (unsigned long long)(uintptr_t)addr);
    *p++ = '\r'; *p++ = '\n'; *p = '\0';
    write_file(line, (size_t)(p - line));
    OutputDebugStringA(line);
}

LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (!fatal(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_count) > 8) return EXCEPTION_CONTINUE_SEARCH;   // a storm says nothing new
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        overflow_line(er->ExceptionAddress);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char line[2400];
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char at[160];
    where(er->ExceptionAddress, at, sizeof at);
    int len = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u [MGPU][CR1] %s (0x%08lX, first-chance) in %s, thread %lu",
                       t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, code_name(er->ExceptionCode),
                       (unsigned long)er->ExceptionCode, at, GetCurrentThreadId());
    if (len < 0) len = 0;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && len < (int)sizeof line)
    {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        len += snprintf(line + len, sizeof line - (size_t)len, " | %s address 0x%llx",
                        kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                        (unsigned long long)er->ExceptionInformation[1]);
    }
    // CR2: who made the call. When the fault is a jump into nothing (no
    // module, or an unloaded one) the walk below cannot get past it, but the
    // caller's return address is the top of the stack, and the stack further
    // up holds the return addresses of the calls before it.
    if (ep->ContextRecord != nullptr && len < (int)sizeof line - 64)
    {
        const char *sp = (const char *)(uintptr_t)ep->ContextRecord->Rsp;
        unsigned long long ret = 0;
        if (read_mem(sp, &ret, sizeof ret))
        {
            char w[160];
            where((const void *)(uintptr_t)ret, w, sizeof w);
            len += snprintf(line + len, sizeof line - (size_t)len, " | return to %s", w);
        }
        len += snprintf(line + len, sizeof line - (size_t)len, " | code on stack:");
        unsigned shown = 0;
        for (unsigned i = 1; i < 96 && shown < 8 && len < (int)sizeof line - 80; ++i)
        {
            unsigned long long v = 0;
            if (!read_mem(sp + i * 8u, &v, sizeof v) || v < 0x10000ull) continue;
            HMODULE h = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)v, &h) || h == nullptr) continue;
            char w[160];
            where((const void *)(uintptr_t)v, w, sizeof w);
            len += snprintf(line + len, sizeof line - (size_t)len, " %s", w);
            ++shown;
        }
    }
    // The call chain.
    if (len < (int)sizeof line - 32)
    {
        void *fr[40] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 40, fr, nullptr);
        // Frames up to the system's exception dispatcher are this handler's own.
        USHORT start = 0;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE h = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)fr[i], &h) && h == ntdll)
                start = (USHORT)(i + 1);
        }
        if (start >= n) start = 0;
        len += snprintf(line + len, sizeof line - (size_t)len, " | calls:");
        for (USHORT i = start, shown = 0; i < n && shown < 10 && len < (int)sizeof line - 80; ++i, ++shown)
        {
            char w[160];
            where(fr[i], w, sizeof w);
            len += snprintf(line + len, sizeof line - (size_t)len, " %s", w);
        }
    }
    // DW1: the last DLL loads/unloads before this, newest first, with how long ago.
    if (len < (int)sizeof line - 80)
    {
        const LONG w = g_dw_w;
        const ULONGLONG now = GetTickCount64();
        len += snprintf(line + len, sizeof line - (size_t)len, " | recent DLL events:");
        unsigned shown = 0;
        for (LONG i = w - 1; i >= 0 && i >= w - (LONG)DW_N && shown < 8 && len < (int)sizeof line - 90; --i)
        {
            const dw_ev &e = g_dw[(unsigned)i % DW_N];
            if (e.seq != i + 1) continue;
            len += snprintf(line + len, sizeof line - (size_t)len, " %c%s(%.1fs ago)",
                            e.kind == 2 ? '-' : '+', e.name, (double)(now - e.ms) / 1000.0);
            ++shown;
        }
        if (shown == 0) len += snprintf(line + len, sizeof line - (size_t)len, " none recorded");
    }
    if (len > (int)sizeof line - 3) len = (int)sizeof line - 3;
    line[len++] = '\r'; line[len++] = '\n'; line[len] = '\0';

    write_file(line, (size_t)len);          // first: plain file I/O, survives a damaged heap
    OutputDebugStringA(line);
    // ReShade's log allocates: not with a damaged heap (REV).
    if (er->ExceptionCode != 0xC0000374u)
    {
        line[len - 2] = '\0';
        mgpu::diag::error(line + 13);       // then ReShade.log (without the time prefix)
    }
    return EXCEPTION_CONTINUE_SEARCH;       // never handled here: the error goes on as before
}

} // namespace

void install(HMODULE self)
{
    if (g_veh != nullptr) return;
    g_self = self;
    const DWORD n = GetModuleFileNameW(self, g_path, MAX_PATH * 2);
    wchar_t *slash = (n != 0 && n < MAX_PATH * 2) ? wcsrchr(g_path, L'\\') : nullptr;
    if (slash != nullptr && (size_t)(slash - g_path) + 20u < (size_t)(MAX_PATH * 2))
        wcscpy_s(slash + 1, (size_t)(MAX_PATH * 2) - (size_t)(slash + 1 - g_path), L"mgpu\\crash.txt");
    else
        g_path[0] = L'\0';
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");   // CR2 (always loaded: no loader work)
    if (nt != nullptr) g_unload_trace = (pfn_unload_trace)(void *)GetProcAddress(nt, "RtlGetUnloadEventTraceEx");
    g_veh = AddVectoredExceptionHandler(1, on_exception);
}

void uninstall()
{
    // PN1: only at process exit in practice (threads are already gone then);
    // tell the pin thread to finish if it is still waiting.
    if (g_pin_ev != nullptr) { InterlockedExchange(&g_pin_stop, 1); SetEvent(g_pin_ev); }
    if (g_dw_cookie != nullptr)
    {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        typedef LONG (NTAPI *pfn_unreg)(PVOID);
        pfn_unreg un = nt ? (pfn_unreg)(void *)GetProcAddress(nt, "LdrUnregisterDllNotification") : nullptr;
        if (un != nullptr) un(g_dw_cookie);
        g_dw_cookie = nullptr;
    }
    if (g_veh != nullptr) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

void announce()
{
    static bool done = false;
    if (done) return;
    done = true;
    {   // DW1: watch DLL loads and unloads from here on
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        pfn_dw_reg reg = nt ? (pfn_dw_reg)(void *)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
        g_dw_t0 = GetTickCount64();
        // PN1: created here (the thread starts once the loader lock is free).
        g_pin_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_pin_ev != nullptr)
        {
            HANDLE th = CreateThread(nullptr, 0, pin_thread, nullptr, 0, nullptr);
            if (th != nullptr) CloseHandle(th);
            else { CloseHandle(g_pin_ev); g_pin_ev = nullptr; }
        }
        if (reg != nullptr && reg(0, dw_cb, nullptr, &g_dw_cookie) == 0)
            mgpu::diag::info("[MGPU][DW1] watching DLL loads and unloads - each is logged as [MGPU][DW1] "
                             "(startup loads are summed up), and a crash lists the last few.");
    }
    mgpu::diag::info(g_veh != nullptr
                         ? "[MGPU][CR1] crash catcher installed: a fatal error anywhere in the game is named "
                           "(module, offset, call chain) in mgpu\\crash.txt and here, as [MGPU][CR1]. Errors are "
                           "seen first-chance: if the game kept running after one, it was handled and is harmless."
                         : "[MGPU][CR1] crash catcher could not be installed.");
}

// DW1: bridge thread, a few times a second. Loads in the first 30 s after
// watching started are only counted (the game's startup); everything after
// that, and every unload, gets its own line.
void flush_dll_events()
{
    static unsigned startup_loads = 0;
    static bool startup_said = false;
    const LONG w = g_dw_w;
    if (w - g_dw_r > (LONG)DW_N) g_dw_r = w - (LONG)DW_N;   // fell behind: skip the overwritten ones
    for (; g_dw_r < w; ++g_dw_r)
    {
        const dw_ev &e = g_dw[(unsigned)g_dw_r % DW_N];
        if (e.seq != g_dw_r + 1) break;   // still being written
        // PN1: Streamline and NGX loads are named even during startup.
        if (e.kind == 1 && e.ms - g_dw_t0 < 30000ull && !is_ngx_family(e.name)) { ++startup_loads; continue; }
        if (!startup_said && e.ms - g_dw_t0 >= 30000ull)
        {
            startup_said = true;
            char s0[120];
            snprintf(s0, sizeof s0, "[MGPU][DW1] %u other DLL(s) loaded during the first 30 s (startup, not listed).", startup_loads);
            mgpu::diag::info(s0);
        }
        char l[200];
        snprintf(l, sizeof l, "[MGPU][DW1] %s %s at 0x%p (%llu KB) t+%.1fs", e.kind == 2 ? "UNLOADED" : "loaded", e.name,
                 (const void *)e.base, (unsigned long long)e.size / 1024ull, (double)(e.ms - g_dw_t0) / 1000.0);
        mgpu::diag::info(l);
    }
}

} // namespace crashlog
} // namespace mgpu
