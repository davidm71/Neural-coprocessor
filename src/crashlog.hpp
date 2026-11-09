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
// ReShade.log. It handles one thing only: the known fault in Steam's overlay
// (SO1/SO2, crashlog.cpp), which it steps over after checking it is that fault
// and that the return is safe. Every other error goes on exactly as before.
//
// The handler sees errors first, before any code that might catch them, so a
// line can also appear for an error something recovered from. The one that
// matters is the last line before the game died.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

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
bool wants_device_check();                // true for ~5 s after a step-over
void note_device(long removed_reason);    // GetDeviceRemovedReason of the presenting device

} // namespace crashlog
} // namespace mgpu
