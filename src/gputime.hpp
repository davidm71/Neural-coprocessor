// ---------------------------------------------------------------------------
// gputime.hpp - GT1: how busy the neural card really is.
//
// A timestamp is written at the start and the end of every command list the
// mod runs on the neural card (the neural stage's list and every present
// list, real and generated frames). Each finished list is turned into an
// interval on the CPU's clock (per-queue clock calibration), and the
// intervals are merged, so work on two queues that overlaps in time is
// counted once. The result is the fraction of each second the card spent
// executing the mod's work, at the clocks it was running at: the share of
// the time budget used, which is what decides whether a frame is late.
//
// Work the mod records into its own lists is included (NR, the DLSS SR / FG
// evaluates, NIS, HUD, HDR). Work a library submits on queues of its own is
// not visible here.
// ---------------------------------------------------------------------------
#pragma once
#include <d3d12.h>

namespace mgpu
{
namespace gputime
{
// Right after the list was Reset. Returns a slot, or -1 (unavailable / full).
int  begin(ID3D12Device *dev, ID3D12CommandQueue *q, ID3D12GraphicsCommandList *cl);
// Just before Close. Records the end mark and the resolve.
void end(ID3D12GraphicsCommandList *cl, int slot);
// GT4: every slot from begin() ends in exactly one of these two calls.
// The list WAS executed. `fence` and `value`: the fence signalled right after
// it on the same queue; nullptr when that Signal failed (the module then
// signals a fence of its own behind the list, or keeps the slot until the
// device is removed - it is never freed on a timer).
void submitted(int slot, ID3D12Fence *fence, unsigned long long value);
// The list was NOT executed and never will be (Close failed, or it was reset
// or dropped). The slot is freed at once - only safe because of that promise.
void cancel(int slot);
// Any thread, cheap: collects finished lists and updates the busy fraction.
void poll();
// The busy fraction of the last complete 1 s window (0..1). false = no
// measurement yet (or none in the last 3 s).
bool busy(double *frac);
// GT2: just before the bridge releases a queue: drops our references to it,
// the samples on it, and the device context when its last queue goes.
void forget(ID3D12CommandQueue *q);
}
}
