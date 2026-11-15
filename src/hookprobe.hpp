// ---------------------------------------------------------------------------
// hookprobe.hpp - SO5. Who has hooked Present?
//
// Programs that draw over games (Steam's overlay, Streamline, recorders, FPS
// counters) detour Windows' Present through code of their own. When one of
// them frees that code while another thread is calling through it, the call
// jumps into empty memory and the game dies - the crash in crash.txt that says
// "executing address" with a return into dxgi.dll. This probe says whose code
// sits in front of Present, so such a crash can be pinned on someone.
//
// How: one small swap chain is made straight from Windows' own dxgi.dll (not
// through ReShade's), never shown, and released at once; the address its
// Present slot points to is read, and any jump at the start of that code is
// followed, up to six hops, each named by the module it lands in. Logged as
// [MGPU][SO5] when the bridge starts and again whenever the chain changes
// (checked every 10 s on the bridge thread). PresentHookProbe=0 turns it off.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <unknwn.h>   // IUnknown: the project builds with WIN32_LEAN_AND_MEAN

namespace mgpu
{
namespace hookprobe
{

void init(IUnknown *d3d12_queue);    // once, a D3D12 direct queue (the bridge's); makes and drops the probe chain
void tick();                         // bridge thread, often: re-reads every 10 s, logs a change
void last(char *out, unsigned n);    // the last chain seen ("" before init) - for the crash reports

} // namespace hookprobe
} // namespace mgpu
