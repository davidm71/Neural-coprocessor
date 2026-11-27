// ---------------------------------------------------------------------------
// crashlog.cpp - CR1. See crashlog.hpp.
// ---------------------------------------------------------------------------
#include "calibrator.hpp"   // UT3
#include "crashlog.hpp"
#include <dxgi.h>        // PG1: IDXGISwapChain::Present (the bridge's own, guarded)
#include "adapter.hpp"       // SO3: the render card's video memory
#include "diag.hpp"
#include "mgpu_ini_parser.hpp"
#include "steam_guard.hpp"   // SO2: the guard's decisions (tested in tests/steam_guard_test.cpp)
#include "hookprobe.hpp"     // SO5: who hooked Present, for the report

#include <tlhelp32.h>
#include <psapi.h>           // SO3: K32GetProcessMemoryInfo

#include <atomic>

#include <cstdint>
#include <cstdio>
#include <cctype>   // SW1c
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

void so_note_module(ULONG reason, const char *name, const void *base, SIZE_T size);   // SO2, below

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
    so_note_module(reason, e.name, d->DllBase, d->SizeOfImage);                      // SO2
    if (reason == 1) mgpu::calibrator::ut_on_load(d->DllBase, e.name);                // UT3
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

// SO2: crash.txt is opened once (install) and kept open, so writing a line in
// the exception handler is one WriteFile - no path conversion, no heap.
// SO4: the step-over's line is written WITHOUT FlushFileBuffers. A WriteFile
// lands in Windows' file cache, which outlives the process if the game dies a
// moment later; a flush only protects against Windows itself going down, and it
// is a disk wait inside the exception handler on the game's render thread. The
// CR1 line for a fault that is passed on (the process is likely ending) still
// flushes, as before. Without the open handle (crash.txt could not be created),
// the step-over writes no line here at all - the ring still carries the record.
HANDLE g_crash_h = INVALID_HANDLE_VALUE;
void crash_write(const char *text, size_t len, bool flush = true)
{
    if (g_crash_h != INVALID_HANDLE_VALUE)
    {
        DWORD wr = 0;
        WriteFile(g_crash_h, text, (DWORD)len, &wr, nullptr);   // FILE_APPEND_DATA: appends atomically
        if (flush) FlushFileBuffers(g_crash_h);
        return;
    }
    if (flush) write_file(text, len);   // the old open-per-line path: never from a step-over
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
    crash_write(line, (size_t)(p - line));
    OutputDebugStringA(line);
}

LONG log_exception(PEXCEPTION_POINTERS ep);

// ---- SO1 / SO2: STEP OVER THE KNOWN FAULT IN STEAM'S OVERLAY ----
//
// 9 of the 14 crashes in crash.txt (Oct-Nov 2026) are one fault: an access
// violation reading 0xffffffffffffffff at gameoverlayrenderer64.dll+0x9b700,
// inside the overlay's hook on DXGI Present - on the game's frames and on the
// mod's own alike. Steam injects that DLL into every game it starts and it
// cannot be kept out (Valve: the overlay checkbox only turns off its UI).
//
// When THAT fault happens, the handler unwinds the overlay's own frames with
// Windows' unwind tables (RtlVirtualUnwind restores every register the
// overlay saved, as a normal return would) until the code is back in its
// caller, sets the return value to S_OK and carries on: that one Present is
// skipped - one frame not shown - and the game goes on.
//
// SO2 (2026-11-07) made it stricter and observable:
//  - ONLY the known fault: an access violation READING 0xFFFFFFFFFFFFFFFF in
//    the overlay. Its offset is reported (it moves with Steam updates and with
//    nothing else - module base + offset, so ASLR does not matter) but not
//    required. Any other fault in the overlay is passed on as before.
//    SteamOverlayGuard=2 accepts any access violation in the overlay (SO1's
//    original rule); 0 turns the guard off.
//  - THE RETURN IS CHECKED before anything changes: every overlay frame must
//    have unwind data (only the faulting one may be a leaf), the stack only
//    moves up and stays on the thread's stack, and the caller it lands in must
//    be committed executable memory, right after a CALL instruction, with the
//    stack 16-byte aligned there. Otherwise the fault is passed on.
//  - NO LOCKS, NO HEAP, NO RESHADE LOG IN THE HANDLER. The overlay's range is
//    cached (no loader lock), crash.txt is kept open (a WriteFile, no path
//    work), the line is formatted by hand, and the full report - module names,
//    registers, the code at the fault - is written to ReShade.log by the
//    bridge thread from a preallocated ring.
//  - AT MOST 5 IN ANY 60 s, lock-free (steam_guard::rate_window). Past that,
//    the fault goes on as before, and the log says so.
//  - "STEPPED OVER" IS AN ATTEMPT, NOT A SUCCESS. Success is a later
//    milestone: the faulting thread reaching its next present, frames
//    flowing at a normal rate, no failed present, no removed device, no new
//    fault - judged 2 s later on the bridge thread ([MGPU][SO2]), with the
//    process's memory, handles and the render card's video memory compared
//    again 60 s later ([MGPU][SO3]).
//
// SO4 (2026-11-07, review):
//  - the records travel through a ring whose slots carry their own state, so a
//    writer never waits and the reader never sees a half-written record, also
//    when it wraps (steam_guard::record_ring);
//  - the handler's crash.txt line is a WriteFile without FlushFileBuffers;
//  - a CALL in executable code is not taken as proof of a Present: the CALL's
//    vtable slot and whether the thread was presenting are reported as the
//    evidence, and a return is refused only on contrary evidence;
//  - every step-over has its own health assessment, so back-to-back faults do
//    not erase each other's verdicts.
//
// What cannot be known from here: whether the overlay held a lock or had
// half-updated its own state when it faulted. Its own cleanup does not run
// (as with any fault that is stepped over). A freeze or a later overlay
// fault after a step-over would point there; SO2/SO3 are how it would show.
namespace sg = mgpu::steam_guard;

std::atomic<int>      g_so_mode{1};                 // SteamOverlayGuard 0 / 1 / 2
std::atomic<uint64_t> g_ov_base{0}, g_ov_size{0};   // gameoverlayrenderer64.dll, cached
sg::rate_window<5>    g_so_rate;
// SO5: PresentHookGuard - 1 (default) steps over a call into freed memory made
// from a Present (a hook whose code was just released); 0 off.
std::atomic<int>      g_fh_mode{1};
sg::rate_window<5>    g_fh_rate;
// PG1 (LR99): PresentGuard - 1 (default) guards the bridge's OWN Present; 0 off.
// PG2 (LR104): PresentGuardGame - 1 (default since LR105) also acts on the
// game's Present (strict checks); 0 only reports what it would have done.
std::atomic<int>      g_pg_mode{1};
std::atomic<int>      g_pg2_mode{1};                // PG2: PresentGuardGame - 1 act (default since LR105), 0 report only
std::atomic<bool>     g_pg_cet{false};              // the process runs with hardware shadow stacks: PG1 off
sg::rate_window<5>    g_pg_rate;
std::atomic<uintptr_t> g_pg_slot[2];                // dxgi's vtable slots for Present / Present1 (hookprobe)
thread_local CONTEXT *t_pg_ctx = nullptr;           // set only while guarded_present is inside Present
thread_local volatile LONG t_pg_hit = 0;            // the handler resumed guarded_present at its snapshot
std::atomic<long>     g_so_attempts{0}, g_so_declined{0}, g_so_resumed{0}, g_so_failed{0};
std::atomic<long>     g_fatal_seen{0};              // every fatal first-chance exception
thread_local int      t_in_handler = 0;             // a fault inside this handler is never handled

enum so_outcome : int { SO_STEPPED = 1, SO_DECLINED_SIG = 2, SO_DECLINED_DEST = 3, SO_DECLINED_RATE = 4,
                        SO_DECLINED_NESTED = 5 };
struct so_rec
{
    long     n;               // attempt number (stepped over only)
    int      outcome, sigv, destv;
    DWORD    tid, code;
    uint64_t kind, addr, rip, ov_base, dest_rip, dest_rsp;
    unsigned frames;
    unsigned handlers;        // overlay frames that have an exception handler of their own
    int      advice;          // SteamOverlayGuard=3: what the return checks would have said (sg::dest)
    ULONGLONG ms;
    uint64_t regs[16];        // rax rcx rdx rbx rsp rbp rsi rdi r8-r15 at the fault
    uint8_t  code_bytes[32];  // rip-16 .. rip+15
    bool     code_ok;
    DWORD    dest_type;       // MEM_IMAGE / MEM_PRIVATE
    int      call_kind;       // SO4: sg::call_kind of the CALL before the return
    long long call_disp;      //      its displacement (vtable slot x 8 for a COM call)
    int      sem;             //      sg::call_sem
    bool     presenting;      //      the faulting thread passed a present event < 250 ms before
    int      evidence;        //      sg::present_evidence: 2, 1, 0 (unverified), -1 (contrary)
    int      hp_slot;         //      its health assessment (-1: none - table full)
    int      variant;         // SO5: 0 = Steam's overlay faulted in its own code, 1 = a call jumped into freed memory,
                              // PG1: 2 = the bridge's own Present faulted in a hook being rewritten
                              // PG2: 3 = the GAME's Present faulted there (LR103)
    int      pg_where;        // PG1: 1 = Present's first bytes in dxgi.dll, 2 = code outside any module
    bool     pg2_on;          // PG2 (LR104): PresentGuardGame=1 (else: a report of what it would have done)
    int      pg2_why;         //      sg::pg2_why
    int      fp_reg;          //      the frame register in that Present's unwind data (0 none)
    DWORD    mem_state, mem_type;   // SO5: what the memory at the fault was (VirtualQuery)
    uint64_t mem_base;        //      its allocation base
};
// SO4: the records, from any number of exception handlers to the bridge
// thread, through a ring that never blocks a writer and never hands the
// reader a half-written record (steam_guard::record_ring, tested).
sg::record_ring<so_rec, 32> g_so_ring;

// SO4: one health assessment per step-over (up to 8 waiting at once).
sg::assessments<8>    g_hp;
const uint64_t        kHpWindowMs = 5000;
std::atomic<long>     g_pres[2];   // presents seen: 0 ReShade's present event, 1 the bridge's own Present
thread_local uint64_t t_present_ms = 0;   // SO4: this thread's last present event (GetTickCount64)

// ---- a line built by hand: no CRT formatting, no locale, no heap ----
struct line_buf
{
    char b[640]; size_t n = 0;
    void s(const char *t) { while (*t && n + 1 < sizeof b) b[n++] = *t++; }
    void x(unsigned long long v) { char t[20]; char *p = t; hex(p, v); for (char *q = t; q < p && n + 1 < sizeof b; ++q) b[n++] = *q; }
    void d(unsigned long long v) { char t[24]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 24); while (k && n + 1 < sizeof b) b[n++] = t[--k]; }
    void d2(unsigned v) { if (v < 10) s("0"); d(v); }
};

bool so_executable(uint64_t a, DWORD &type)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof mbi) != sizeof mbi) return false;
    type = mbi.Type;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Handler only: put one record in the ring and one line in crash.txt.
void so_record(const so_rec &r)
{
    (void)g_so_ring.push(r);   // a busy slot drops the record (counted); never waits

    SYSTEMTIME t{};
    GetLocalTime(&t);
    line_buf l;
    l.d2(t.wHour); l.s(":"); l.d2(t.wMinute); l.s(":"); l.d2(t.wSecond);
    l.s(r.variant == 3 ? " [MGPU][PG2] " : r.variant == 2 ? " [MGPU][PG1] " : r.variant == 1 ? " [MGPU][SO5] " : " [MGPU][SO1] ");
    if (r.outcome == SO_STEPPED) { l.s("STEPPED OVER #"); l.d((unsigned long long)r.n); l.s(" (attempt; verdict follows as SO2)"); }
    else l.s("NOT stepped over");
    if (r.variant == 3)
    {
        l.s(": the game's Present faulted at "); l.x(r.rip);
        l.s(" (the first bytes of Windows' Present - a hook rewriting them), thread "); l.d(r.tid);
        if (r.outcome == SO_STEPPED) l.s(" - one frame not shown");
        else if (!r.pg2_on) l.s(r.pg2_why == 0 ? " - PG2 is off (PresentGuardGame=0); it WOULD have been taken"
                                               : " - PG2 is off (PresentGuardGame=0); it would NOT have been taken");
    }
    else if (r.variant == 2)
    {
        l.s(": the bridge's own Present faulted at "); l.x(r.rip);
        l.s(r.pg_where == 1 ? " (the first bytes of Windows' Present - a hook rewriting them)" : " (hook code outside any module)");
        l.s(", thread "); l.d(r.tid); l.s(" - one frame not shown");
    }
    else if (r.variant == 1)
    {
        l.s(": a call jumped into "); l.s(r.mem_state == MEM_FREE ? "FREED" : r.mem_state == MEM_RESERVE ? "uncommitted" : "non-code");
        l.s(" memory at "); l.x(r.rip); l.s(" (block "); l.x(r.mem_base); l.s("), thread "); l.d(r.tid);
    }
    else { l.s(": gameoverlayrenderer64.dll+"); l.x(r.rip - r.ov_base); l.s(" reading "); l.x(r.addr); l.s(", thread "); l.d(r.tid); }
    if (r.outcome == SO_STEPPED || r.outcome == SO_DECLINED_DEST || r.outcome == SO_DECLINED_RATE)
    { l.s(", return to "); l.x(r.dest_rip); if (r.variant == 0) { l.s(" after "); l.d(r.frames); l.s(" overlay frame(s)"); } }
    if (r.outcome == SO_STEPPED && r.variant == 2) l.s(", from the snapshot taken just before the call");
    else if (r.outcome == SO_STEPPED && r.variant == 3) { l.s(", return found "); l.d((unsigned long long)r.frames * 8u); l.s(" bytes above the fault's rsp"); }
    else if (r.outcome == SO_STEPPED)
        l.s(r.evidence >= 2 ? ", in a Present (call + thread)" : r.evidence == 1 ? ", in a Present (one sign)"
                                                                 : ", S_OK UNVERIFIED (no sign of a Present)");
    switch (r.outcome)
    {
    case SO_DECLINED_SIG:    l.s(" - "); l.s(sg::sig_text((sg::sig)r.sigv)); break;
    case SO_DECLINED_DEST:
        if (r.variant == 3) { if (r.pg2_why != 0) { l.s(" - "); l.s(sg::pg2_text((sg::pg2_why)r.pg2_why)); } }   // PG2
        else { l.s(" - unsafe return: "); l.s(sg::dest_text((sg::dest)r.destv)); }
        break;
    case SO_DECLINED_RATE:   l.s(" - LIMIT REACHED (5 in 60 s): the fault is passed on and the game will likely close"); break;
    case SO_DECLINED_NESTED: l.s(" - raised while another exception was being handled"); break;
    default: break;
    }
    l.s("\r\n");
    crash_write(l.b, l.n, false);   // SO4: no flush in the handler (see crash_write)
}

// Handler only. True = the context now returns from the overlay with S_OK.
bool so_try(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    sg::fault f{};
    f.code = er->ExceptionCode; f.flags = er->ExceptionFlags; f.nparams = er->NumberParameters;
    f.av_kind = er->NumberParameters >= 1 ? er->ExceptionInformation[0] : 0;
    f.av_addr = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    f.rip = (uint64_t)(uintptr_t)er->ExceptionAddress;
    f.ov_base = g_ov_base.load(std::memory_order_acquire);
    f.ov_size = g_ov_size.load(std::memory_order_acquire);
    const sg::sig s = sg::classify(f, (sg::mode)g_so_mode.load(std::memory_order_relaxed));
    if (s == sg::sig::not_av || s == sg::sig::not_overlay || s == sg::sig::guard_off) return false;   // not ours

    so_rec r{};
    r.hp_slot = -1;
    r.sigv = (int)s; r.tid = GetCurrentThreadId(); r.code = f.code; r.kind = f.av_kind; r.addr = f.av_addr;
    r.rip = f.rip; r.ov_base = f.ov_base; r.ms = GetTickCount64();
    const DWORD64 rg[16] = { cx->Rax, cx->Rcx, cx->Rdx, cx->Rbx, cx->Rsp, cx->Rbp, cx->Rsi, cx->Rdi,
                             cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15 };
    for (int i = 0; i < 16; ++i) r.regs[i] = rg[i];
    r.code_ok = read_mem((const void *)(uintptr_t)(f.rip - 16), r.code_bytes, sizeof r.code_bytes);

    if (!sg::accepted(s)) { r.outcome = SO_DECLINED_SIG; g_so_declined.fetch_add(1); so_record(r); return false; }
    if (er->ExceptionRecord != nullptr && s != sg::sig::legacy) { r.outcome = SO_DECLINED_NESTED; g_so_declined.fetch_add(1); so_record(r); return false; }

    // Walk out of the overlay on a copy; nothing real changes unless every check passes.
    // SteamOverlayGuard=3 (legacy) walks exactly as SO1 did - a missing unwind
    // entry pops [rsp] at any frame, the stack is not checked - and only notes
    // what the checks would have said (r.advice).
    const bool legacy = (s == sg::sig::legacy);
    sg::dest advice = sg::dest::ok;
    CONTEXT c = *cx;
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const uint64_t lo = (uint64_t)(uintptr_t)tib->StackLimit, hi = (uint64_t)(uintptr_t)tib->StackBase;
    sg::dest res = sg::dest::ok;
    unsigned frames = 0;
    for (; frames < 64 && sg::in_range(c.Rip, f.ov_base, f.ov_size); ++frames)
    {
        const uint64_t old_rsp = c.Rsp;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (fe == nullptr)
        {
            // Only the faulting function may be a leaf (no unwind data, return
            // address on top of the stack). A caller without unwind data is not
            // a leaf, and popping [rsp] there would be a guess.
            if (frames != 0)
            {
                if (!legacy) { res = sg::dest::no_unwind_info; break; }
                if (advice == sg::dest::ok) advice = sg::dest::no_unwind_info;
            }
            DWORD64 ret = 0;
            if (!read_mem((const void *)(uintptr_t)c.Rsp, &ret, sizeof ret)) { res = sg::dest::unreadable; break; }
            c.Rip = ret;
            c.Rsp += 8;
        }
        else
        {
            PVOID hd = nullptr;
            DWORD64 frame = 0;
            // EHANDLER only REPORTS whether this frame has a handler of its own
            // (the unwind itself is the same). VEH runs before such a handler, so
            // a frame with one is noted in the report.
            if (RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.Rip, fe, &c, &hd, &frame, nullptr) != nullptr) ++r.handlers;
        }
        if (!sg::stack_step_ok(old_rsp, c.Rsp, lo, hi))
        {
            if (!legacy) { res = sg::dest::stack; break; }
            if (advice == sg::dest::ok) advice = sg::dest::stack;
        }
    }
    if (res == sg::dest::ok && sg::in_range(c.Rip, f.ov_base, f.ov_size)) res = sg::dest::too_deep;
    if (res == sg::dest::ok)
    {
        sg::dest_facts d{};
        d.rip = c.Rip; d.rsp = c.Rsp; d.walked_out = true;
        d.in_overlay = sg::in_range(c.Rip, f.ov_base, f.ov_size);
        d.executable = so_executable(c.Rip, r.dest_type);
        d.readable_before = c.Rip > 8 && read_mem((const void *)(uintptr_t)(c.Rip - 8), d.before, 8);
        // Look further up (on another copy): if the overlay appears again, the
        // frames just unwound were a callback the overlay made through another
        // module, and that module does not expect an S_OK from a Present. Stops
        // quietly at the first frame without unwind data (nothing known).
        {
            CONTEXT u = c;
            for (int i = 0; i < 32 && !d.reenters; ++i)
            {
                DWORD64 ub = 0;
                PRUNTIME_FUNCTION ufe = RtlLookupFunctionEntry(u.Rip, &ub, nullptr);
                if (ufe == nullptr) break;
                const uint64_t ursp = u.Rsp;
                PVOID uhd = nullptr; DWORD64 ufr = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ub, u.Rip, ufe, &u, &uhd, &ufr, nullptr);
                if (!sg::stack_step_ok(ursp, u.Rsp, lo, hi) || u.Rip == 0) break;
                d.reenters = sg::in_range(u.Rip, f.ov_base, f.ov_size);
            }
        }
        res = sg::check_dest(d);
        // SO4: a call site in executable code is not proof the call was a
        // Present. Gather what can be known; refuse only on contrary evidence
        // (another vtable method, from a thread that was not presenting).
        if (d.readable_before)
        {
            const sg::call_info ci = sg::decode_call(d.before);
            const sg::call_sem sem = sg::call_semantic(ci);
            r.call_kind = (int)ci.kind; r.call_disp = ci.disp; r.sem = (int)sem;
        }
        r.presenting = t_present_ms != 0 && r.ms - t_present_ms < 250ull;
        r.evidence = sg::present_evidence((sg::call_sem)r.sem, r.presenting);
        if (res == sg::dest::ok && r.evidence < 0) res = sg::dest::not_present;
        if (legacy)
        {
            // SO1 required only a non-zero return outside the overlay.
            if (advice == sg::dest::ok) advice = res;
            res = (c.Rip != 0) ? sg::dest::ok : sg::dest::none;
        }
    }
    r.advice = (int)advice;
    r.dest_rip = c.Rip; r.dest_rsp = c.Rsp; r.frames = frames; r.destv = (int)res;
    if (res != sg::dest::ok) { r.outcome = SO_DECLINED_DEST; g_so_declined.fetch_add(1); so_record(r); return false; }

    if (!g_so_rate.take(r.ms, 60000ull)) { r.outcome = SO_DECLINED_RATE; g_so_declined.fetch_add(1); so_record(r); return false; }

    // Commit: the overlay's Present returns S_OK to its caller. Only the
    // registers the unwind restored and RAX change; ContextFlags stay.
    const DWORD keep = cx->ContextFlags;
    c.Rax = 0;
    *cx = c;
    cx->ContextFlags = keep;

    r.n = g_so_attempts.fetch_add(1) + 1;
    r.outcome = SO_STEPPED;
    // Its own health assessment (the bridge thread judges it); a full table
    // means no verdict for this one (counted), never a lost one for another.
    r.hp_slot = g_hp.claim(r.n, (uint32_t)r.tid, r.ms, g_fatal_seen.load(std::memory_order_relaxed));
    so_record(r);
    return true;
}

// ---- SO5: A CALL INTO FREED MEMORY FROM A PRESENT ----
//
// Six crashes in crash.txt (RoboCop and Black Myth, Oct-Nov 2026) are one
// shape: "executing address" in memory no module holds, returning into
// dxgi.dll - Present went through a hook whose code had just been released
// (most likely while its owner was re-installing it), so the jump landed on
// nothing. Each happened once per session: a moment, not a broken hook.
//
// The jump arrives with the caller's return address on top of the stack and
// nothing else pushed, so returning is one pop. Stricter than SO1's checks:
//  - an execute fault on the first byte it tried to run, in memory that is not
//    committed code and not a loaded module;
//  - the return lands right after a CALL, in committed executable code, with
//    the stack aligned, on the thread's own stack (sg::check_dest);
//  - a POSITIVE sign it was a Present: the thread presented < 250 ms ago, and
//    the call was not contrary evidence (SO1 refuses only on contrary signs);
//  - not nested, at most 5 in any 60 s, its own health verdict (SO2).
// Then that one Present returns S_OK unshown and the game goes on. If the hook
// stays broken the limit runs out and the crash happens as before - with a
// line that says what it was.
bool fh_try(PEXCEPTION_POINTERS ep)
{
    if (g_fh_mode.load(std::memory_order_relaxed) == 0) return false;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return false;
    if (er->ExceptionInformation[0] != 8) return false;                    // DEP: an attempt to execute
    const uint64_t rip = (uint64_t)(uintptr_t)er->ExceptionAddress;
    if (er->ExceptionInformation[1] != rip || cx->Rip != rip) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((LPCVOID)(uintptr_t)rip, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State == MEM_COMMIT &&
        (mbi.Type == MEM_IMAGE ||
         (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0))
        return false;                                                      // real code: some other fault

    so_rec r{};
    r.variant = 1; r.hp_slot = -1;
    r.sigv = 0; r.tid = GetCurrentThreadId(); r.code = er->ExceptionCode; r.kind = 8; r.addr = rip;
    r.rip = rip; r.ov_base = 0; r.ms = GetTickCount64();
    r.mem_state = mbi.State; r.mem_type = mbi.Type; r.mem_base = (uint64_t)(uintptr_t)mbi.AllocationBase;
    const DWORD64 rg[16] = { cx->Rax, cx->Rcx, cx->Rdx, cx->Rbx, cx->Rsp, cx->Rbp, cx->Rsi, cx->Rdi,
                             cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15 };
    for (int i = 0; i < 16; ++i) r.regs[i] = rg[i];
    r.code_ok = false;
    if (er->ExceptionRecord != nullptr) { r.outcome = SO_DECLINED_NESTED; g_so_declined.fetch_add(1); so_record(r); return false; }

    CONTEXT c = *cx;
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const uint64_t lo = (uint64_t)(uintptr_t)tib->StackLimit, hi = (uint64_t)(uintptr_t)tib->StackBase;
    sg::dest res = sg::dest::ok;
    DWORD64 ret = 0;
    const uint64_t old_rsp = c.Rsp;
    if (!read_mem((const void *)(uintptr_t)c.Rsp, &ret, sizeof ret)) res = sg::dest::unreadable;
    else
    {
        c.Rip = ret; c.Rsp += 8;
        if (!sg::stack_step_ok(old_rsp, c.Rsp, lo, hi)) res = sg::dest::stack;
    }
    if (res == sg::dest::ok)
    {
        sg::dest_facts d{};
        d.rip = c.Rip; d.rsp = c.Rsp; d.walked_out = true; d.in_overlay = false; d.reenters = false;
        d.executable = so_executable(c.Rip, r.dest_type);
        d.readable_before = c.Rip > 8 && read_mem((const void *)(uintptr_t)(c.Rip - 8), d.before, 8);
        res = sg::check_dest(d);
        if (d.readable_before)
        {
            const sg::call_info ci = sg::decode_call(d.before);
            const sg::call_sem sem = sg::call_semantic(ci);
            r.call_kind = (int)ci.kind; r.call_disp = ci.disp; r.sem = (int)sem;
        }
        r.presenting = t_present_ms != 0 && r.ms - t_present_ms < 250ull;
        r.evidence = sg::present_evidence((sg::call_sem)r.sem, r.presenting);
        if (res == sg::dest::ok && (!r.presenting || r.sem == (int)sg::call_sem::other_method)) res = sg::dest::not_present;
    }
    r.dest_rip = c.Rip; r.dest_rsp = c.Rsp; r.frames = 0; r.destv = (int)res;
    if (res != sg::dest::ok) { r.outcome = SO_DECLINED_DEST; g_so_declined.fetch_add(1); so_record(r); return false; }
    if (!g_fh_rate.take(r.ms, 60000ull)) { r.outcome = SO_DECLINED_RATE; g_so_declined.fetch_add(1); so_record(r); return false; }

    const DWORD keep = cx->ContextFlags;
    c.Rax = 0;                       // S_OK from that Present
    *cx = c;
    cx->ContextFlags = keep;
    r.n = g_so_attempts.fetch_add(1) + 1;
    r.outcome = SO_STEPPED;
    r.hp_slot = g_hp.claim(r.n, (uint32_t)r.tid, r.ms, g_fatal_seen.load(std::memory_order_relaxed));
    so_record(r);
    return true;
}

// ---- PG1 (LR99): THE BRIDGE'S OWN PRESENT, GUARDED AGAINST A HOOK BEING REWRITTEN ----
//
// Cyberpunk 2077, 2026-11-26, halfway through the benchmark: RivaTuner (RTSS,
// with MSI Afterburner) put its hook on Windows' Present, took it off and put it
// back every 10 s ([MGPU][SO5] CHANGED at :04, :14, :24, :34, :44), rewriting the
// first bytes of dxgi.dll's Present each time. The bridge presents its own window
// from a thread of its own, and at 10:06:51 that thread ran Present while the
// bytes were half rewritten: an access violation at dxgi.dll+0x84c9, 9 bytes into
// Present (+0x84c0), writing 0x1a7 - an instruction made of half old, half new
// bytes. SO1 and SO5 could not take it: it is not in Steam's overlay, and it is
// not a jump into freed memory.
//
// Here the caller is the bridge itself, so the step-over does not have to work
// out a return by walking the stack. guarded_present() takes a snapshot of its
// own registers (RtlCaptureContext) just before it calls Present; if Present
// faults where a hook is being rewritten, the handler puts the thread back at
// that snapshot - the same point, the same registers, the same stack - and
// guarded_present returns S_OK without having shown the frame. Nothing is
// guessed: every register the code around it relies on is the one it had.
//
// Taken ONLY when all of these hold, otherwise the fault goes on as before (SO1,
// SO5, then CR1):
//  - the thread is inside guarded_present's Present (a thread-local snapshot);
//  - an access violation or an illegal/privileged instruction;
//  - in the first 32 bytes of dxgi.dll's Present or Present1 (the bytes a hook
//    rewrites, read from the vtable slots hookprobe found - before any lock is
//    taken), or in code outside every module (a hook's trampoline being
//    replaced or freed);
//  - not raised while another exception was being handled;
//  - at most 5 in any 60 s;
//  - the process does not run with hardware shadow stacks (CET), where moving a
//    thread back to a snapshot is refused by Windows - checked at install.
// It gets an SO2 health verdict like every other step-over. The GAME's own
// Present is not covered here (no snapshot of its caller); SO1 and SO5 stay its
// guards.
bool pg_try(PEXCEPTION_POINTERS ep)
{
    CONTEXT *saved = t_pg_ctx;
    if (saved == nullptr || g_pg_mode.load(std::memory_order_relaxed) == 0) return false;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    const DWORD code = er->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION) return false;
    const uint64_t rip = (uint64_t)(uintptr_t)er->ExceptionAddress;
    uint64_t fn[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i)
    {
        const uintptr_t slot = g_pg_slot[i].load(std::memory_order_acquire);
        uintptr_t v = 0;
        if (slot != 0 && read_mem((const void *)slot, &v, sizeof v)) fn[i] = (uint64_t)v;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const bool q = VirtualQuery((LPCVOID)(uintptr_t)rip, &mbi, sizeof mbi) == sizeof mbi;
    const bool in_module = q && mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE;
    const int where_at = sg::pg_where(rip, fn[0], fn[1], in_module);   // tested in steam_guard_test
    if (where_at == 0) return false;   // somewhere else in a module: not this guard's fault

    so_rec r{};
    r.variant = 2; r.pg_where = where_at; r.hp_slot = -1;
    r.tid = GetCurrentThreadId(); r.code = code;
    r.kind = er->NumberParameters >= 1 ? er->ExceptionInformation[0] : 0;
    r.addr = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    r.rip = rip; r.ms = GetTickCount64();
    if (q) { r.mem_state = mbi.State; r.mem_type = mbi.Type; r.mem_base = (uint64_t)(uintptr_t)mbi.AllocationBase; }
    const DWORD64 rg[16] = { cx->Rax, cx->Rcx, cx->Rdx, cx->Rbx, cx->Rsp, cx->Rbp, cx->Rsi, cx->Rdi,
                             cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15 };
    for (int i = 0; i < 16; ++i) r.regs[i] = rg[i];
    r.code_ok = read_mem((const void *)(uintptr_t)(rip - 16), r.code_bytes, sizeof r.code_bytes);
    r.dest_rip = saved->Rip; r.dest_rsp = saved->Rsp;
    r.presenting = true; r.evidence = 2;
    if (er->ExceptionRecord != nullptr) { r.outcome = SO_DECLINED_NESTED; g_so_declined.fetch_add(1); so_record(r); return false; }
    if (!g_pg_rate.take(r.ms, 60000ull)) { r.outcome = SO_DECLINED_RATE; g_so_declined.fetch_add(1); so_record(r); return false; }

    // Commit: back to the snapshot. The debug registers and the context's own
    // flags are the thread's as they are now (a snapshot does not hold them).
    CONTEXT c = *saved;
    c.Dr0 = cx->Dr0; c.Dr1 = cx->Dr1; c.Dr2 = cx->Dr2; c.Dr3 = cx->Dr3; c.Dr6 = cx->Dr6; c.Dr7 = cx->Dr7;
    c.ContextFlags = cx->ContextFlags;
    *cx = c;
    t_pg_ctx = nullptr;
    t_pg_hit = 1;

    r.n = g_so_attempts.fetch_add(1) + 1;
    r.outcome = SO_STEPPED;
    r.hp_slot = g_hp.claim(r.n, (uint32_t)r.tid, r.ms, g_fatal_seen.load(std::memory_order_relaxed));
    so_record(r);
    return true;
}

// ---- PG2 (LR103, made strict and opt-in in LR104): THE GAME'S PRESENT ----
//
// Cyberpunk 2077, 2026-11-26 11:30:09, LR102: RivaTuner flipped its Present
// hook again and the GAME's presenting thread (Streamline -> DLSS-G -> dxgi.dll)
// was inside the bytes: dxgi.dll+0x84c9, writing 0x1a7 - PG1's fault on a
// thread PG1 does not cover (no snapshot of its caller exists).
//
// LR104: a return is resumed ONLY when two independent derivations agree and
// nothing saved has changed - the decision is the portable rule
// sg::pg2_decide (tested; see steam_guard.hpp for each condition):
//  - the stack scan (sg::pg2_pick) finds a believable return just above rsp;
//  - Windows' own unwind data for that Present, applied from the faulting rip,
//    lands on the same return address and the same rsp;
//  - every callee-saved register (integer and xmm) the unwind reads back from
//    the stack equals the thread's current value;
//  - the CALL before the return provably targets that Present (direct call or
//    a [rip+x] pointer holding it) or goes through Present's vtable slot.
// What remains assumed - a register the prologue had not saved yet was not
// changed by the half-written bytes - cannot be checked from here, so:
//
// ON BY DEFAULT since LR105 (PresentGuardGame=1): it acts only at a moment the
// game would otherwise close, so for a user who never edits mgpu.ini it can
// only turn a certain crash into a skipped frame - or, if a check fails, into
// the same crash. PresentGuardGame=0 makes it report only: the same facts are
// gathered (reads only), the verdict is written ("it would have been taken" /
// "would not: <reason>") and the fault goes on exactly as before LR103.
//
// Also required: not the bridge's own guarded call (PG1); an access violation
// or illegal / privileged instruction in the first 32 bytes of dxgi.dll's
// Present or Present1; not nested; at most 5 in any 60 s (shared with PG1);
// not under CET (g_pg_mode is 0 there, and PG2 follows it).
bool pg2_try(PEXCEPTION_POINTERS ep)
{
    if (t_pg_ctx != nullptr) return false;                                  // the bridge's own call: PG1
    if (g_pg_mode.load(std::memory_order_relaxed) == 0) return false;      // PresentGuard=0 or CET: nothing
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    const DWORD code = er->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION) return false;
    const uint64_t rip = (uint64_t)(uintptr_t)er->ExceptionAddress;
    uint64_t fn[2] = { 0, 0 };
    for (int i = 0; i < 2; ++i)
    {
        const uintptr_t slot = g_pg_slot[i].load(std::memory_order_acquire);
        uintptr_t v = 0;
        if (slot != 0 && read_mem((const void *)slot, &v, sizeof v)) fn[i] = (uint64_t)v;
    }
    if (sg::pg_where(rip, fn[0], fn[1], true) != 1) return false;          // only Present's first bytes
    const uint64_t f_start = (fn[0] != 0 && rip >= fn[0] && rip - fn[0] < sg::PG_PROLOGUE) ? fn[0] : fn[1];
    MEMORY_BASIC_INFORMATION fm{};
    if (VirtualQuery((LPCVOID)(uintptr_t)f_start, &fm, sizeof fm) != sizeof fm || fm.Type != MEM_IMAGE) return false;
    const uint64_t f_mod = (uint64_t)(uintptr_t)fm.AllocationBase;
    const bool enabled = g_pg2_mode.load(std::memory_order_relaxed) == 1;

    so_rec r{};
    r.variant = 3; r.pg_where = 1; r.hp_slot = -1; r.pg2_on = enabled;
    r.tid = GetCurrentThreadId(); r.code = code;
    r.kind = er->NumberParameters >= 1 ? er->ExceptionInformation[0] : 0;
    r.addr = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    r.rip = rip; r.ms = GetTickCount64();
    r.mem_state = fm.State; r.mem_type = fm.Type; r.mem_base = f_mod;
    const DWORD64 rg[16] = { cx->Rax, cx->Rcx, cx->Rdx, cx->Rbx, cx->Rsp, cx->Rbp, cx->Rsi, cx->Rdi,
                             cx->R8, cx->R9, cx->R10, cx->R11, cx->R12, cx->R13, cx->R14, cx->R15 };
    for (int i = 0; i < 16; ++i) r.regs[i] = rg[i];
    r.code_ok = read_mem((const void *)(uintptr_t)(rip - 16), r.code_bytes, sizeof r.code_bytes);
    r.presenting = t_present_ms != 0 && r.ms - t_present_ms < 250ull;
    if (er->ExceptionRecord != nullptr) { r.outcome = SO_DECLINED_NESTED; g_so_declined.fetch_add(1); so_record(r); return false; }

    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    const uint64_t lo = (uint64_t)(uintptr_t)tib->StackLimit, hi = (uint64_t)(uintptr_t)tib->StackBase;
    sg::pg2_facts f{};

    // 1. the scan
    sg::pg2_cand cand[sg::PG2_SLOTS];
    int n = 0;
    for (; n < sg::PG2_SLOTS; ++n)
    {
        const uint64_t slot = (uint64_t)cx->Rsp + 8ull * (uint64_t)n;
        if (slot < lo || slot + 16 > hi) break;
        sg::pg2_cand &c = cand[n];
        c = sg::pg2_cand{};
        uint64_t v = 0;
        if (!read_mem((const void *)(uintptr_t)slot, &v, sizeof v)) break;
        c.d.rip = v; c.d.rsp = slot + 8; c.d.walked_out = true; c.d.in_overlay = false; c.d.reenters = false;
        DWORD type = 0;
        c.d.executable = v != 0 && so_executable(v, type);
        c.image = c.d.executable && type == MEM_IMAGE;
        if (c.image)
        {
            MEMORY_BASIC_INFORMATION vm{};
            c.same_module = VirtualQuery((LPCVOID)(uintptr_t)v, &vm, sizeof vm) == sizeof vm &&
                            (uint64_t)(uintptr_t)vm.AllocationBase == f_mod;
        }
        c.d.readable_before = v > 8 && read_mem((const void *)(uintptr_t)(v - 8), c.d.before, 8);
        if (c.d.readable_before)
        {
            const sg::call_info ci = sg::decode_call(c.d.before);
            c.sem = sg::call_semantic(ci);
            c.direct = ci.kind == sg::call_kind::direct;
            c.direct_to_fn = c.direct && sg::direct_target(c.d.before, v) == f_start;
        }
    }
    f.scan_index = sg::pg2_pick(cand, n);

    // 2. the unwind, on a copy (nothing real changes)
    CONTEXT u = *cx;
    if (f.scan_index >= 0 && cx->Rsp >= lo && cx->Rsp + 0x200 < hi)
    {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &base, nullptr);
        f.unwind_found = fe != nullptr && base + fe->BeginAddress == f_start;
        if (f.unwind_found)
        {
            // The frame register the prologue may set (UNWIND_INFO byte 3, low 4 bits).
            uint8_t ui[4] = {};
            int fp_reg = 0;
            if (read_mem((const void *)(uintptr_t)(base + fe->UnwindData), ui, sizeof ui)) fp_reg = ui[3] & 0x0F;
            KNONVOLATILE_CONTEXT_POINTERS nv{};
            PVOID hd = nullptr;
            DWORD64 fr = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, rip, fe, &u, &hd, &fr, &nv);
            const sg::pg2_cand &k = cand[f.scan_index];
            f.unwind_agrees = u.Rip == k.d.rip && u.Rsp == k.d.rsp;
            // integer: index = register number (0 rax .. 15 r15); xmm: 0..15
            bool rest[16]; uint64_t then[16], now[16];
            const DWORD64 *uc = &u.Rax, *cc = &cx->Rax;   // Rax..R15 are consecutive in CONTEXT
            for (int i = 0; i < 16; ++i) { rest[i] = nv.IntegerContext[i] != nullptr; then[i] = uc[i]; now[i] = cc[i]; }
            const bool int_same = sg::pg2_regs_same(rest, then, now, 16, fp_reg);
            bool x_same = true;
            for (int i = 0; i < 16; ++i)
                if (nv.FloatingContext[i] != nullptr && memcmp(&(&u.Xmm0)[i], &(&cx->Xmm0)[i], sizeof(M128A)) != 0)
                    x_same = false;
            f.regs_same = int_same && x_same;
            r.fp_reg = fp_reg;
        }
    }
    // 3. the call before the return
    if (f.scan_index >= 0)
    {
        const sg::pg2_cand &k = cand[f.scan_index];
        const sg::call_info ci = sg::decode_call(k.d.before);
        r.call_kind = (int)ci.kind; r.call_disp = ci.disp; r.sem = (int)k.sem;
        if (k.direct_to_fn) f.call_verified = true;
        const uint64_t ps = sg::rip_mem_slot(k.d.before, k.d.rip);
        uint64_t target = 0;
        if (ps != 0 && read_mem((const void *)(uintptr_t)ps, &target, sizeof target) && target == f_start)
            f.call_verified = true;
        f.call_names_present = k.sem == sg::call_sem::present || k.sem == sg::call_sem::present1;
        r.dest_rip = k.d.rip; r.dest_rsp = k.d.rsp; r.frames = (unsigned)f.scan_index;
        r.evidence = sg::present_evidence(k.sem, r.presenting);
    }
    else { r.dest_rip = 0; r.dest_rsp = cx->Rsp; }
    const sg::pg2_why why = sg::pg2_decide(f);
    r.pg2_why = (int)why;
    r.destv = (int)(why == sg::pg2_why::taken ? sg::dest::ok : sg::dest::none);

    if (why != sg::pg2_why::taken || !enabled)
    {
        // Off, or not every check passed: the fault goes on exactly as before.
        r.outcome = SO_DECLINED_DEST; g_so_declined.fetch_add(1); so_record(r); return false;
    }
    if (!g_pg_rate.take(r.ms, 60000ull)) { r.outcome = SO_DECLINED_RATE; g_so_declined.fetch_add(1); so_record(r); return false; }

    // Commit: that Present returns S_OK to its caller. rip, rsp and rax change;
    // the callee-saved registers are the unwind's, which were checked equal.
    const DWORD keep = cx->ContextFlags;
    CONTEXT c = *cx;
    c.Rip = cand[f.scan_index].d.rip;
    c.Rsp = cand[f.scan_index].d.rsp;
    c.Rax = 0;
    *cx = c;
    cx->ContextFlags = keep;

    r.n = g_so_attempts.fetch_add(1) + 1;
    r.outcome = SO_STEPPED;
    r.hp_slot = g_hp.claim(r.n, (uint32_t)r.tid, r.ms, g_fatal_seen.load(std::memory_order_relaxed));
    so_record(r);
    return true;
}

LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (!fatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    g_fatal_seen.fetch_add(1, std::memory_order_relaxed);
    if (t_in_handler > 0) return EXCEPTION_CONTINUE_SEARCH;   // a fault inside this handler: pass it on untouched
    ++t_in_handler;
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (ep->ContextRecord != nullptr && ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        (pg_try(ep) || pg2_try(ep) || so_try(ep) || fh_try(ep)))   // PG1 first: exact; PG2: the game's Present (LR103); SO5: freed memory
        r = EXCEPTION_CONTINUE_EXECUTION;
    else
        r = log_exception(ep);   // CR1, exactly as before
    --t_in_handler;
    return r;
}

// ---- bridge thread: the full report, and whether rendering came back ----
void so_refresh_overlay()
{
    if (g_ov_base.load(std::memory_order_acquire) != 0) return;
    HMODULE h = GetModuleHandleW(L"gameoverlayrenderer64.dll");
    if (h == nullptr) return;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)h;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)h + dos->e_lfanew);
    g_ov_size.store(nt->OptionalHeader.SizeOfImage, std::memory_order_release);
    g_ov_base.store((uint64_t)(uintptr_t)h, std::memory_order_release);
}

// Loader callback (under the loader lock): only stores the overlay's range.
void so_note_module(ULONG reason, const char *name, const void *base, SIZE_T size)
{
    if (_stricmp(name, "gameoverlayrenderer64.dll") != 0) return;
    if (reason == 1)
    {
        g_ov_size.store((uint64_t)size, std::memory_order_release);
        g_ov_base.store((uint64_t)(uintptr_t)base, std::memory_order_release);
    }
    else if (reason == 2)
    {
        g_ov_base.store(0, std::memory_order_release);
        g_ov_size.store(0, std::memory_order_release);
    }
}

void so_overlay_line()
{
    static bool said = false;
    const uint64_t b = g_ov_base.load(std::memory_order_acquire);
    if (said || b == 0) return;
    said = true;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)(uintptr_t)b;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)dos + dos->e_lfanew);
    static const char *const mn[] = { "OFF (SteamOverlayGuard=0)", "on, the known fault only",
                                      "on, any access violation (SteamOverlayGuard=2)",
                                      "on, SO1's original guard - any access violation, no return checks (SteamOverlayGuard=3)" };
    const int m = g_so_mode.load(std::memory_order_relaxed);
    char l[400];
    snprintf(l, sizeof l, "[MGPU][SO2] Steam overlay guard %s | gameoverlayrenderer64.dll at 0x%llx, %llu KB, build stamp "
             "0x%08lX (the known fault was at +0x%llx in the build seen Oct-Nov 2026).",
             mn[(m >= 0 && m <= 3) ? m : 1], (unsigned long long)b,
             (unsigned long long)g_ov_size.load() / 1024ull, (unsigned long)nt->FileHeader.TimeDateStamp,
             (unsigned long long)sg::kKnownOffset);
    mgpu::diag::info(l);
}

struct so_res { unsigned long long commit, handles, vram; bool vram_ok; };
so_res so_resources()
{
    so_res r{};
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pm, sizeof pm)) r.commit = pm.PrivateUsage;
    DWORD hc = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &hc)) r.handles = hc;
    unsigned long long budget = 0, usage = 0;
    r.vram_ok = mgpu::adapter::game_adapter_vmem(budget, usage);
    r.vram = usage;
    return r;
}

void so_log_record(const so_rec &e)
{
    char rip[160], dst[160];
    where((const void *)(uintptr_t)e.rip, rip, sizeof rip);
    if (e.dest_rip != 0) where((const void *)(uintptr_t)e.dest_rip, dst, sizeof dst); else snprintf(dst, sizeof dst, "-");
    char l[1400];
    int n = 0;
    if (e.variant == 3)   // PG2 (LR103; strict and opt-in since LR104)
    {
        char who[700];
        mgpu::hookprobe::last(who, sizeof who);
        if (e.outcome == SO_STEPPED)
            n = snprintf(l, sizeof l, "[MGPU][PG2] STEPPED OVER #%ld (an attempt - the SO2 line says whether rendering came "
                         "back): the GAME's Present faulted in its first bytes (%s at %s, %s 0x%llx, thread %lu) - a hook "
                         "(RivaTuner, an overlay) rewriting them. ONE FRAME WAS NOT SHOWN: that Present returned S_OK to %s "
                         "(return found %u bytes above the fault's rsp; call form %d, %s; thread presenting: %s). Present was "
                         "last seen going through: %s | this session: %ld stepped over, %ld passed on",
                         e.n, code_name(e.code), rip, e.kind == 1 ? "writing" : e.kind == 8 ? "executing" : "reading",
                         (unsigned long long)e.addr, (unsigned long)e.tid, dst, e.frames * 8u, e.call_kind,
                         sg::sem_text((sg::call_sem)e.sem), e.presenting ? "yes" : "no", who,
                         g_so_attempts.load(), g_so_declined.load());
        else
            n = snprintf(l, sizeof l, "[MGPU][PG2] NOT STEPPED OVER%s: the game's Present faulted in its first bytes (%s at %s, "
                         "thread %lu) | %s%s | scan: return %s %u bytes above rsp, call form %d (%s); unwind frame register %d | "
                         "the fault was passed on as before (see the CR1 line) | Present was last seen going through: %s",
                         e.pg2_on ? "" : " (PG2 is off: PresentGuardGame=0 - report only)",
                         code_name(e.code), rip, (unsigned long)e.tid,
                         e.outcome == SO_DECLINED_RATE ? "LIMIT REACHED: 5 in the last 60 s"
                         : e.outcome == SO_DECLINED_NESTED ? "raised while another exception was being handled"
                         : e.pg2_why == 0 ? "every independent check passed - it WOULD have been taken"
                                          : sg::pg2_text((sg::pg2_why)e.pg2_why),
                         e.outcome == SO_DECLINED_DEST && e.pg2_why != 0 ? " - it would NOT have been taken" : "",
                         dst, e.frames * 8u, e.call_kind, sg::sem_text((sg::call_sem)e.sem), e.fp_reg, who);
    }
    else if (e.variant == 2)   // PG1
    {
        char who[700];
        mgpu::hookprobe::last(who, sizeof who);
        const char *at = e.pg_where == 1 ? "in the first bytes of Windows' Present - the bytes a hook (RivaTuner, an overlay) "
                                           "rewrites when it puts its hook on or takes it off"
                                         : "in code that belongs to no module - a hook's trampoline being replaced or freed";
        if (e.outcome == SO_STEPPED)
            n = snprintf(l, sizeof l, "[MGPU][PG1] STEPPED OVER #%ld (an attempt - the SO2 line says whether rendering came "
                         "back): the bridge's own Present faulted (%s at %s, %s 0x%llx), %s. ONE FRAME WAS NOT SHOWN; the "
                         "present thread went on from its snapshot just before the call (rsp 0x%llx) with Present's S_OK. "
                         "Present was last seen going through: %s | this session: %ld stepped over, %ld passed on",
                         e.n, code_name(e.code), rip, e.kind == 1 ? "writing" : e.kind == 8 ? "executing" : "reading",
                         (unsigned long long)e.addr, at, (unsigned long long)e.dest_rsp, who,
                         g_so_attempts.load(), g_so_declined.load());
        else
            n = snprintf(l, sizeof l, "[MGPU][PG1] NOT STEPPED OVER: the bridge's own Present faulted (%s at %s), %s | %s | "
                         "the fault was passed on as before (see the CR1 line) | Present was last seen going through: %s",
                         code_name(e.code), rip, at,
                         e.outcome == SO_DECLINED_RATE ? "LIMIT REACHED: 5 in the last 60 s"
                                                       : "raised while another exception was being handled", who);
    }
    else if (e.variant == 1)   // SO5
    {
        char who[700];
        mgpu::hookprobe::last(who, sizeof who);
        const char *mem = e.mem_state == MEM_FREE ? "FREED memory" : e.mem_state == MEM_RESERVE ? "reserved, uncommitted memory"
                                                                   : "memory that is not code";
        if (e.outcome == SO_STEPPED)
            n = snprintf(l, sizeof l, "[MGPU][SO5] STEPPED OVER #%ld (an attempt - the SO2 line says whether rendering came "
                         "back): a call jumped into %s at 0x%llx (block 0x%llx), thread %lu - a hook on Present whose code was "
                         "released | returned S_OK to %s, rsp 0x%llx | Present was last seen going through: %s | this "
                         "session: %ld stepped over, %ld passed on",
                         e.n, mem, (unsigned long long)e.rip, (unsigned long long)e.mem_base, (unsigned long)e.tid, dst,
                         (unsigned long long)e.dest_rsp, who, g_so_attempts.load(), g_so_declined.load());
        else
            n = snprintf(l, sizeof l, "[MGPU][SO5] NOT STEPPED OVER: a call jumped into %s at 0x%llx (block 0x%llx), thread "
                         "%lu | %s%s%s | Present was last seen going through: %s | the fault was passed on as before (see "
                         "the CR1 line)",
                         mem, (unsigned long long)e.rip, (unsigned long long)e.mem_base, (unsigned long)e.tid,
                         e.outcome == SO_DECLINED_DEST ? sg::dest_text((sg::dest)e.destv)
                         : e.outcome == SO_DECLINED_RATE ? "LIMIT REACHED: 5 in the last 60 s"
                                                         : "raised while another exception was being handled",
                         e.outcome == SO_DECLINED_DEST ? " - would have returned to " : "",
                         e.outcome == SO_DECLINED_DEST ? dst : "", who);
    }
    else if (e.outcome == SO_STEPPED)
        n = snprintf(l, sizeof l, "[MGPU][SO1] STEPPED OVER #%ld (an attempt - the SO2 line says whether rendering came "
                     "back): %s in %s, reading 0x%llx, thread %lu | %s | returned S_OK to %s (%s memory) after %u overlay "
                     "frame(s) (%u with a handler of their own), rsp 0x%llx | this session: %ld stepped over, %ld passed on",
                     e.n, code_name(e.code), rip, (unsigned long long)e.addr, (unsigned long)e.tid,
                     sg::sig_text((sg::sig)e.sigv), dst, e.dest_type == MEM_IMAGE ? "module" : "private",
                     e.frames, e.handlers, (unsigned long long)e.dest_rsp, g_so_attempts.load(), g_so_declined.load());
    else
        n = snprintf(l, sizeof l, "[MGPU][SO1] NOT STEPPED OVER: %s in %s, reading 0x%llx, thread %lu | %s | %s%s%s | the "
                     "fault was passed on as before (see the CR1 line)",
                     code_name(e.code), rip, (unsigned long long)e.addr, (unsigned long)e.tid,
                     e.outcome == SO_DECLINED_SIG ? sg::sig_text((sg::sig)e.sigv)
                     : e.outcome == SO_DECLINED_DEST ? "unsafe return"
                     : e.outcome == SO_DECLINED_RATE ? "LIMIT REACHED: 5 step-overs in the last 60 s"
                                                     : "raised while another exception was being handled",
                     e.outcome == SO_DECLINED_DEST ? sg::dest_text((sg::dest)e.destv) : "",
                     e.outcome == SO_DECLINED_DEST ? " - would have returned to " : "",
                     e.outcome == SO_DECLINED_DEST ? dst : "");
    if (n < 0) n = 0;
    // SO4: why S_OK is believed to be the right answer - or that it is not known.
    if ((e.outcome == SO_STEPPED || (e.outcome == SO_DECLINED_DEST && e.destv == (int)sg::dest::not_present)) &&
        n < (int)sizeof l - 300)
    {
        static const char *const ck[] = { "no call", "direct call", "call through a pointer variable", "call through a register",
                                          "call through memory" };
        char disp[40] = "";
        if (e.call_kind == (int)sg::call_kind::mem) snprintf(disp, sizeof disp, " +0x%llx", (unsigned long long)e.call_disp);
        n += snprintf(l + n, sizeof l - (size_t)n, " | Present evidence %s: caller made a %s%s = %s; thread %s",
                      e.evidence >= 2 ? "STRONG" : e.evidence == 1 ? "partial" : e.evidence == 0 ? "NONE (S_OK unverified)"
                                                                                 : "CONTRARY",
                      ck[(e.call_kind >= 0 && e.call_kind <= 4) ? e.call_kind : 0], disp,
                      sg::sem_text((sg::call_sem)e.sem),
                      e.presenting ? "was presenting (present event < 250 ms before)" : "had no recent present event");
        if (e.outcome == SO_STEPPED && e.hp_slot < 0 && n < (int)sizeof l - 80)
            n += snprintf(l + n, sizeof l - (size_t)n, " | no health verdict for this one (8 already waiting)");
    }
    if (e.outcome == SO_STEPPED && e.sigv == (int)sg::sig::legacy && n < (int)sizeof l - 200)
        n += snprintf(l + n, sizeof l - (size_t)n, " | SteamOverlayGuard=3: the return checks would have %s%s",
                      e.advice == 0 ? "passed" : "REFUSED - ", e.advice == 0 ? "" : sg::dest_text((sg::dest)e.advice));
    // Registers and the code at the fault, so the overlay's bad read can be
    // worked out (which register held -1) - the first time per offset only.
    static uint64_t seen[8] = {};
    const uint64_t off = e.rip - e.ov_base;
    bool first = true;
    for (uint64_t s : seen) if (s == off + 1) first = false;
    if (first && n < (int)sizeof l - 600)
    {
        for (uint64_t &s : seen) if (s == 0) { s = off + 1; break; }
        static const char *const rn[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
        n += snprintf(l + n, sizeof l - (size_t)n, " | registers:");
        for (int i = 0; i < 16 && n < (int)sizeof l - 40; ++i)
            n += snprintf(l + n, sizeof l - (size_t)n, " %s=%llx", rn[i], (unsigned long long)e.regs[i]);
        if (e.code_ok)
        {
            n += snprintf(l + n, sizeof l - (size_t)n, " | code at rip-16:");
            for (int i = 0; i < 32 && n < (int)sizeof l - 8; ++i)
                n += snprintf(l + n, sizeof l - (size_t)n, i == 16 ? " [%02x" : (i == 17 ? "] %02x" : " %02x"), e.code_bytes[i]);
        }
    }
    mgpu::diag::warn(l);
}

void so_poll()
{
    so_refresh_overlay();
    so_overlay_line();
    const ULONGLONG now = GetTickCount64();

    // Present counts every ~100 ms, the last 4 s, for the rates before and after.
    struct sample { ULONGLONG ms; long n; };
    static sample hist[48] = {};
    static unsigned hw = 0;
    const long total = g_pres[0].load(std::memory_order_relaxed) + g_pres[1].load(std::memory_order_relaxed);
    hist[hw++ % 48] = { now, total };
    auto count_at = [&](ULONGLONG t, long &out) -> bool {   // the newest sample at or before t
        bool got = false; ULONGLONG best = 0;
        for (const sample &s : hist) if (s.ms != 0 && s.ms <= t && s.ms >= best) { best = s.ms; out = s.n; got = true; }
        return got && t - best < 300ull;
    };
    auto rate = [&](ULONGLONG a, ULONGLONG b) -> double {
        long na = 0, nb = 0;
        if (b <= a || !count_at(a, na) || !count_at(b, nb)) return 0.0;
        return (double)(nb - na) * 1000.0 / (double)(b - a);
    };

    // The records the handler left (SO4: through the ring; a record that was
    // dropped or overwritten is counted, never read half-written).
    g_so_ring.drain([](const so_rec &e) { so_log_record(e); });
    {
        static uint64_t said_d = 0, said_o = 0, said_l = 0, said_h = 0;
        const uint64_t d = g_so_ring.dropped.load(), o = g_so_ring.overwritten.load(), lo = g_so_ring.lost,
                       hd = g_hp.dropped.load();
        if (d != said_d || o != said_o || lo != said_l || hd != said_h)
        {
            said_d = d; said_o = o; said_l = lo; said_h = hd;
            char l[300];
            snprintf(l, sizeof l, "[MGPU][SO4] guard records not logged this session: %llu dropped (slot busy), %llu "
                     "overwritten unread, %llu skipped by the reader | step-overs without a health verdict (table "
                     "full): %llu. crash.txt has a line for every step-over regardless.",
                     (unsigned long long)d, (unsigned long long)o, (unsigned long long)lo, (unsigned long long)hd);
            mgpu::diag::warn(l);
        }
    }

    // SO4: a verdict for EACH step-over, from its own assessment slot.
    const unsigned HN = sizeof g_hp.s / sizeof g_hp.s[0];
    static long   seen_n[sizeof g_hp.s / sizeof g_hp.s[0]] = {};   // which recovery each slot was set up for
    static double before[sizeof g_hp.s / sizeof g_hp.s[0]] = {};
    static so_res res0[sizeof g_hp.s / sizeof g_hp.s[0]] = {};
    struct so3_due { long n; ULONGLONG at; so_res r0; };
    static so3_due so3[8] = {};
    for (unsigned i = 0; i < HN; ++i)
    {
        const long n = g_hp.s[i].n.load(std::memory_order_acquire);
        if (n <= 0) { seen_n[i] = 0; continue; }
        const ULONGLONG t0 = g_hp.s[i].ms.load(std::memory_order_relaxed);
        if (seen_n[i] != n)
        {
            seen_n[i] = n;
            before[i] = rate(t0 > 1000 ? t0 - 1000 : 0, t0);
            res0[i] = so_resources();
        }
        sg::health_facts h{};
        h.elapsed_ms = now - t0;
        h.same_thread_seen = g_hp.s[i].same.load() != 0;
        h.presents_after = (uint32_t)g_hp.s[i].presents.load();
        h.present_fail_hr = (int32_t)g_hp.s[i].fail.load();
        h.device_hr = (int32_t)g_hp.s[i].dev.load();
        const long fat = g_fatal_seen.load() - g_hp.s[i].fatal0.load();
        h.new_faults = fat > 0 ? (uint32_t)fat : 0u;
        h.rate_before = before[i];
        h.rate_after = (h.elapsed_ms >= 2000) ? rate(t0 + 1000, t0 + 2000) : 0.0;
        const sg::health v = sg::health_verdict(h);
        if (v == sg::health::pending) continue;
        if (v == sg::health::resumed || v == sg::health::resumed_other) g_so_resumed.fetch_add(1);
        else g_so_failed.fetch_add(1);
        char fl[24] = "none", dv[32] = "ok";
        if (h.present_fail_hr != 0) snprintf(fl, sizeof fl, "0x%08lX", (unsigned long)(uint32_t)h.present_fail_hr);
        if (h.device_hr != 0) snprintf(dv, sizeof dv, "REMOVED 0x%08lX", (unsigned long)(uint32_t)h.device_hr);
        char l[700];
        snprintf(l, sizeof l, "[MGPU][SO2] step-over #%ld: %s | %u present(s) in %.1f s, %.0f/s before -> %.0f/s after, "
                 "faulting thread %s, failed present %s, device %s, new faults %u%s | session: %ld stepped over, "
                 "%ld resumed, %ld not, %ld passed on",
                 n, sg::health_text(v), h.presents_after, (double)h.elapsed_ms / 1000.0, before[i], h.rate_after,
                 h.same_thread_seen ? "presented again" : "not seen", fl, dv, h.new_faults,
                 h.new_faults ? " (a later step-over counts here too)" : "",
                 g_so_attempts.load(), g_so_resumed.load(), g_so_failed.load(), g_so_declined.load());
        if (v == sg::health::resumed || v == sg::health::resumed_other) mgpu::diag::info(l);
        else mgpu::diag::warn(l);
        for (so3_due &d : so3) if (d.n == 0) { d.n = n; d.at = t0 + 60000ull; d.r0 = res0[i]; break; }
        seen_n[i] = 0;
        g_hp.release(i);   // this slot only; the others keep their own verdicts
    }
    for (so3_due &d : so3)
    {
        if (d.n == 0 || now < d.at) continue;
        const so_res r = so_resources();
        char l[400];
        snprintf(l, sizeof l, "[MGPU][SO3] 60 s after step-over #%ld: process memory %+.1f MB, handles %+lld, render "
                 "card video memory %s%+.1f MB, %.0f presents/s, %ld fatal fault(s) seen this session. Steady numbers here "
                 "mean nothing piled up after the skipped frame (other work in the game moves them too).",
                 d.n, ((double)r.commit - (double)d.r0.commit) / 1048576.0,
                 (long long)r.handles - (long long)d.r0.handles, (r.vram_ok && d.r0.vram_ok) ? "" : "(not measured) ",
                 (r.vram_ok && d.r0.vram_ok) ? ((double)r.vram - (double)d.r0.vram) / 1048576.0 : 0.0,
                 rate(now > 1000 ? now - 1000 : 0, now), g_fatal_seen.load());
        mgpu::diag::info(l);
        d.n = 0;
    }
}

// SW1. STACK OVERFLOW, WITH THE CALL CHAIN. REPORTED 2026-11-18 (Rise of the
// Tomb Raider, IN31, and Josefo's earlier Rise run): crash.txt said only
// "STACK OVERFLOW in KERNELBASE.dll+0x...", which names where the stack ran
// out, not what filled it. The handler has almost no stack left, so it copies
// the faulting thread's registers into a static buffer and wakes a thread
// made at start-up for this; that thread walks the overflowed stack with
// Windows' unwind tables (on its own, healthy stack) and writes the first
// frames plus the return addresses that repeat most - a runaway recursion
// shows up as one or two addresses repeated thousands of times.
CONTEXT g_sw_ctx;
volatile LONG g_sw_busy = 0;
DWORD g_sw_tid = 0;
HANDLE g_sw_go = nullptr, g_sw_done = nullptr;

// SW1c: like where(), but marks WHICH copy of a module: two DLLs can share a
// name - ReShade installed as dxgi.dll next to the game and Windows' own
// dxgi.dll in System32 - and IN33's SW1 line could not tell them apart.
void where_tagged(const void *addr, char *out, size_t n)
{
    HMODULE h = nullptr;
    char full[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h) && h != nullptr && GetModuleFileNameA(h, full, MAX_PATH) != 0)
    {
        const char *base = strrchr(full, '\\');
        base = (base != nullptr) ? base + 1 : full;
        char low[MAX_PATH] = {};
        for (size_t i = 0; full[i] && i < MAX_PATH - 1; ++i) low[i] = (char)tolower((unsigned char)full[i]);
        const char *tag = strstr(low, "\\windows\\") ? "Windows:" : "local:";
        snprintf(out, n, "%s%s+0x%llx", tag, base, (unsigned long long)((const char *)addr - (const char *)h));
    }
    else where(addr, out, n);
}

void sw_walk()
{
    CONTEXT c = g_sw_ctx;
    struct hit { DWORD64 pc; unsigned n; };
    static hit top[64];
    unsigned ntop = 0;
    static DWORD64 first[24];
    unsigned nfirst = 0, frames = 0;
    for (; frames < 20000; ++frames)
    {
        const DWORD64 pc = c.Rip;
        if (pc == 0) break;
        if (nfirst < 24) first[nfirst++] = pc;
        unsigned i = 0;
        for (; i < ntop; ++i) if (top[i].pc == pc) { ++top[i].n; break; }
        if (i == ntop && ntop < 64) { top[ntop].pc = pc; top[ntop].n = 1; ++ntop; }

        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &base, nullptr);
        if (fe != nullptr)
        {
            PVOID hd = nullptr; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fe, &c, &hd, &ef, nullptr);
        }
        else
        {
            DWORD64 ret = 0;
            if (!read_mem((const void *)(uintptr_t)c.Rsp, &ret, sizeof ret)) break;
            c.Rip = ret; c.Rsp += 8;
        }
    }
    // most repeated first
    for (unsigned a = 0; a < ntop; ++a)
        for (unsigned b = a + 1; b < ntop; ++b)
            if (top[b].n > top[a].n) { hit t = top[a]; top[a] = top[b]; top[b] = t; }

    static char line[6000];
    SYSTEMTIME t{}; GetLocalTime(&t);
    int len = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u [MGPU][SW1] stack overflow on thread %lu: %u frame(s) "
                       "walked | most repeated:", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                       (unsigned long)g_sw_tid, frames);
    char at[160];
    for (unsigned i = 0; i < ntop && i < 6 && len < 5000; ++i)
    {
        where_tagged((const void *)(uintptr_t)top[i].pc, at, sizeof at);
        len += snprintf(line + len, sizeof line - (size_t)len, " %s x%u", at, top[i].n);
    }
    len += snprintf(line + len, sizeof line - (size_t)len, " | first frames:");
    for (unsigned i = 0; i < nfirst && len < 5800; ++i)
    {
        where_tagged((const void *)(uintptr_t)first[i], at, sizeof at);
        len += snprintf(line + len, sizeof line - (size_t)len, " %s", at);
    }
    if (len > (int)sizeof line - 3) len = (int)sizeof line - 3;
    line[len++] = '\r'; line[len++] = '\n'; line[len] = '\0';
    crash_write(line, (size_t)len);
}

DWORD WINAPI sw_thread(LPVOID)
{
    for (;;)
    {
        if (WaitForSingleObject(g_sw_go, INFINITE) != WAIT_OBJECT_0) return 0;
        __try { sw_walk(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        SetEvent(g_sw_done);
    }
}

void sw_start()
{
    g_sw_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_sw_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_sw_go == nullptr || g_sw_done == nullptr) return;
    HANDLE h = CreateThread(nullptr, 256 * 1024, &sw_thread, nullptr, 0, nullptr);
    if (h != nullptr) CloseHandle(h);
    else { CloseHandle(g_sw_go); g_sw_go = nullptr; }
}

// From the handler: tiny stack use - a struct copy and two kernel calls.
void sw_report(const CONTEXT *ctx)
{
    if (g_sw_go == nullptr || ctx == nullptr) return;
    if (InterlockedExchange(&g_sw_busy, 1) != 0) return;   // once
    g_sw_ctx = *ctx;
    g_sw_tid = GetCurrentThreadId();
    SetEvent(g_sw_go);
    WaitForSingleObject(g_sw_done, 3000);
}

LONG log_exception(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (InterlockedIncrement(&g_count) > 8) return EXCEPTION_CONTINUE_SEARCH;   // a storm says nothing new
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        // SW1b: hand the chain to the helper FIRST - after overflow_line's
        // module lookups there may be no stack left to wake it (IN32 wrote
        // the KERNELBASE line and nothing else).
        sw_report(ep->ContextRecord);
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

    crash_write(line, (size_t)len);         // first: plain file I/O, survives a damaged heap
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
    // SO2: crash.txt kept open for the exception handler (created empty if absent).
    if (g_path[0] != L'\0')
        g_crash_h = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    {   // SO1/SO2: SteamOverlayGuard in mgpu.ini (beside the add-on): 0 off, 1 the known fault (default), 2 any
        // access violation in the overlay, 3 SO1's original guard (any, without the return checks)
        wchar_t ini[MAX_PATH * 2] = {};
        const DWORD m = GetModuleFileNameW(self, ini, MAX_PATH * 2);
        wchar_t *sl = (m != 0 && m < MAX_PATH * 2) ? wcsrchr(ini, L'\\') : nullptr;
        if (sl != nullptr)
        {
            wcscpy_s(sl + 1, (size_t)(MAX_PATH * 2) - (size_t)(sl + 1 - ini), L"mgpu.ini");
            HANDLE f = CreateFileW(ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                static char buf[65536];
                DWORD got = 0;
                if (ReadFile(f, buf, sizeof buf - 1, &got, nullptr))
                {
                    buf[got] = 0;
                    const char *v = mgpu::config::find(buf, got, "SteamOverlayGuard");
                    if (v != nullptr && *v >= '0' && *v <= '3') g_so_mode.store(*v - '0');
                    const char *ph = mgpu::config::find(buf, got, "PresentHookGuard");   // SO5
                    if (ph != nullptr && (*ph == '0' || *ph == '1')) g_fh_mode.store(*ph - '0');
                    const char *pgv = mgpu::config::find(buf, got, "PresentGuard");      // PG1 (LR99)
                    if (pgv != nullptr && *pgv >= '0' && *pgv <= '2') g_pg_mode.store(*pgv == '0' ? 0 : 1);   // LR103's "2" = 1
                    const char *pgg = mgpu::config::find(buf, got, "PresentGuardGame");  // PG2 (LR104)
                    if (pgg != nullptr && (*pgg == '0' || *pgg == '1')) g_pg2_mode.store(*pgg - '0');
                }
                CloseHandle(f);
            }
        }
    }
    {   // PG1: with hardware shadow stacks Windows refuses to move a thread back to a snapshot
        PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY ss{};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &ss, sizeof ss) &&
            ss.EnableUserShadowStack)
            g_pg_cet.store(true);
        if (g_pg_cet.load()) g_pg_mode.store(0);
    }
    so_refresh_overlay();   // Steam injects the overlay before ReShade loads add-ons
    sw_start();   // SW1
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
    // crash.txt (SO2) is left open: the process is ending and a late fault can still use it.
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
    mgpu::diag::info(g_pg_mode.load() != 0
                         ? (g_pg2_mode.load() == 1
                                ? "[MGPU][PG1] the bridge's own Present is guarded, and PG2 (PresentGuardGame=1, default) "
                                  "also steps over the game's Present when a hook rewriting its first bytes (RivaTuner/"
                                  "Afterburner, an overlay) faults it and every independent check passes - one frame not "
                                  "shown instead of the game closing. PresentGuardGame=0 makes PG2 report only."
                                : "[MGPU][PG1] the bridge's own Present is guarded: if it faults in the first bytes of "
                                  "Windows' Present while a hook (RivaTuner/Afterburner, an overlay) is rewriting them, that "
                                  "one frame is not shown and the bridge goes on (PresentGuard=0 turns this off). The game's "
                                  "own Present is NOT stepped over (PresentGuardGame=0: PG2 reports only).")
                         : g_pg_cet.load()
                         ? "[MGPU][PG1] Present guard OFF: this game runs with hardware shadow stacks (CET), where "
                           "Windows will not let a thread be moved back to a snapshot."
                         : "[MGPU][PG1] Present guard OFF (PresentGuard=0).");
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
    so_poll();   // SO2: the overlay guard's report and verdicts
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

// PG1 (LR99): see pg_try. NOT inlined: the snapshot belongs to this frame,
// which stays alive for the whole Present call.
__declspec(noinline) long guarded_present(IDXGISwapChain *sc, unsigned interval, unsigned flags)
{
    if (g_pg_mode.load(std::memory_order_relaxed) == 0) return (long)sc->Present(interval, flags);
    CONTEXT snap;
    t_pg_hit = 0;
    RtlCaptureContext(&snap);
    // The handler comes back HERE (RtlCaptureContext "returns" a second time)
    // with t_pg_hit set, the snapshot's registers and stack. Nothing set after
    // the snapshot is read on that path - only the thread-local flag.
    if (t_pg_hit != 0)
    {
        t_pg_hit = 0;
        return (long)S_OK;
    }
    t_pg_ctx = &snap;
    const HRESULT hr = sc->Present(interval, flags);
    t_pg_ctx = nullptr;
    return (long)hr;
}

void set_present_slots(uintptr_t present_slot, uintptr_t present1_slot)
{
    g_pg_slot[0].store(present_slot, std::memory_order_release);
    g_pg_slot[1].store(present1_slot, std::memory_order_release);
}

// SO2: the milestones a step-over is judged by. Per frame this is one relaxed
// atomic add and one load; the rest runs only in the seconds after a step-over.
void present_begin()
{
    t_present_ms = GetTickCount64();   // SO5: the bridge's own present thread is presenting
}

void note_present(int source, long hr)
{
    g_pres[source & 1].fetch_add(1, std::memory_order_relaxed);
    if (source == 0) t_present_ms = GetTickCount64();   // SO4: this thread is presenting (ReShade's event, before Present)
    if (!g_hp.any()) return;                            // nothing being judged: two loads and a TLS write per frame
    g_hp.note_present((uint32_t)GetCurrentThreadId(), hr, GetTickCount64(), kHpWindowMs);
}

bool wants_device_check()
{
    return g_hp.any() && g_hp.wants_device(GetTickCount64(), kHpWindowMs);
}

void note_device(long hr)
{
    g_hp.note_device(hr, GetTickCount64(), kHpWindowMs);
}

} // namespace crashlog
} // namespace mgpu
