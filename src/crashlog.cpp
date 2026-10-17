// ---------------------------------------------------------------------------
// crashlog.cpp - CR1. See crashlog.hpp.
// ---------------------------------------------------------------------------
#include "crashlog.hpp"
#include "diag.hpp"

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

bool unloaded_name(const void *addr, char *name, size_t n, unsigned long long &off)
{
    if (g_unload_trace == nullptr) return false;
    PULONG esz = nullptr, ecnt = nullptr; PVOID tr = nullptr;
    g_unload_trace(&esz, &ecnt, &tr);
    if (esz == nullptr || ecnt == nullptr || tr == nullptr || *esz < sizeof(unload_ev)) return false;
    const char *base = (const char *)tr;
    for (ULONG i = 0; i < *ecnt && i < 64u; ++i)
    {
        const unload_ev *e = (const unload_ev *)(base + (size_t)i * *esz);
        if (e->BaseAddress == nullptr) continue;
        const char *b = (const char *)e->BaseAddress;
        if ((const char *)addr >= b && (const char *)addr < b + e->SizeOfImage)
        {
            size_t k = 0;
            for (; k + 1 < n && k < 32 && e->ImageName[k] != 0; ++k) name[k] = (char)e->ImageName[k];
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
        if (unloaded_name(addr, nm, sizeof nm, off))
            snprintf(out, n, "UNLOADED %s+0x%llx", nm, off);   // CR2: a DLL that has been unloaded
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
    if (g_veh != nullptr) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

void announce()
{
    static bool done = false;
    if (done) return;
    done = true;
    mgpu::diag::info(g_veh != nullptr
                         ? "[MGPU][CR1] crash catcher installed: a fatal error anywhere in the game is named "
                           "(module, offset, call chain) in mgpu\\crash.txt and here, as [MGPU][CR1]. Errors are "
                           "seen first-chance: if the game kept running after one, it was handled and is harmless."
                         : "[MGPU][CR1] crash catcher could not be installed.");
}

} // namespace crashlog
} // namespace mgpu
