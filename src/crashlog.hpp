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
// ReShade.log. It never handles anything: the error then goes on exactly as
// before.
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

void install(HMODULE self);   // DllMain, process attach (no file I/O there)
void uninstall();             // DllMain, process detach
void announce();              // once, from a normal thread: says it is installed

} // namespace crashlog
} // namespace mgpu
