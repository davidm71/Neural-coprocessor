// ---------------------------------------------------------------------------
// crashlog.hpp - CR1. Names what crashed.
//
// A crash that kills the game instantly leaves nothing behind: no Windows
// Error Reporting entry, no Unreal crash report, and a ReShade.log that just
// stops. CR1 installs a vectored exception handler that, the moment a fatal
// error is raised anywhere in the process, writes one line naming the module
// it happened in (this add-on, an NVIDIA DLL, the driver, the game) with the
// offset inside it and the call chain, to <add-on folder>\mgpu\crash.txt
// (plain WriteFile, flushed, safe even when the heap is damaged) and to
// ReShade.log. It steps over only faults it can recognise and return from
// safely: the known fault in Steam's overlay (SO1/SO2), a Present hook that
// jumped into freed memory (SO5), and a fault in the bridge's own Present while
// a hook rewrites it (PG1). Every other error goes on exactly as before.
//
// The handler sees errors first, before any code that might catch them, so a
// line can also appear for an error something recovered from. The one that
// matters is the last line before the game died.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <cstdint>

struct IDXGISwapChain;   // PG1 (dxgi.h)

namespace mgpu
{
namespace crashlog
{

void install(HMODULE self);   // DllMain, process attach (opens crash.txt, reads mgpu.ini; loads nothing)
void uninstall();             // DllMain, process detach
void announce();              // once, from a normal thread: says it is installed
void flush_dll_events();      // DW1: bridge thread, a few times a second (also SO2's report and verdicts)

// SO2: milestones for judging a Steam overlay step-over. Cheap enough for every
// frame (one relaxed atomic add and one load unless a step-over is being judged).
void note_present(int source, long hr);   // 0 = ReShade's present event (hr 0), 1 = the bridge's own Present
void present_begin();                     // SO5: the calling thread is about to Present (one TLS write)
bool wants_device_check();                // true for ~5 s after a step-over
void note_device(long removed_reason);    // GetDeviceRemovedReason of the presenting device

// PG1 (LR99): the bridge's OWN Present, guarded against a hook (RivaTuner /
// Afterburner, an overlay) rewriting the first bytes of Windows' Present - or
// replacing its own trampoline - while this thread is running through them.
// Such a fault puts the thread back where it was just before the call and
// returns S_OK: one frame not shown instead of a crash. [MGPU][PG1].
long guarded_present(IDXGISwapChain *sc, unsigned interval, unsigned flags);
// hookprobe: the addresses of dxgi.dll's vtable slots for Present and Present1.
void set_present_slots(uintptr_t present_slot, uintptr_t present1_slot);

} // namespace crashlog
} // namespace mgpu
