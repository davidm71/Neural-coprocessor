// ---------------------------------------------------------------------------
// gputime.cpp - GT1. See gputime.hpp.
//
// GT2: intervals split at 1 s window boundaries; device removal handled;
// queues and fences held with references of our own; forget(queue).
//
// GT4: SLOT LIFETIME, PROVEN RATHER THAN TIMED. A slot (two timestamps in the
// query heap, 16 bytes in the readback buffer) is written by the GPU when its
// list runs. It is freed only when one of these is TRUE:
//   - a fence it holds has passed the value signalled after its list, or
//   - the caller cancelled it: the list was never executed (Close failed or
//     the list was dropped) - cancel() is the caller's statement of that, or
//   - the device was removed (it writes nothing any more).
// Nothing is freed because time has passed.
//
//   FREE --begin--> REC --cancel--> FREE                (never executed)
//                   REC --submitted(fence)--> SUB       (sample wanted)
//                   REC --submitted(none)---> ORPH      (our own fence, signalled
//                                                         on the same queue after it)
//                                         or  PIN       (no fence possible: kept
//                                                         until device removal)
//   SUB --fence passed--> sample, FREE    SUB --forget(queue)--> ORPH
//   ORPH --fence passed--> FREE           PIN --device removed--> FREE
//   REC --forget(queue)--> REC, detached (no sample; still needs cancel/submitted)
//
// A device context (heap + buffer) is released only when every slot is FREE;
// until then it is "retiring" (no new slots) and poll() releases it later. A
// slot id carries the context's generation, so an id from before a release
// can never touch a slot of a newer context.
// ---------------------------------------------------------------------------
#include "gputime.hpp"
#include "diag.hpp"

#include <windows.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mgpu
{
namespace gputime
{
namespace
{

const int N_SLOTS = 256;      // lists in flight or waiting to be read, per device
const int N_DEV = 2;          // the present chain and the neural stage may use two device objects
const int N_Q = 4;

enum { FREE = 0, REC = 1, SUB = 2, ORPH = 3, PIN = 4 };

struct qinfo
{
    ID3D12CommandQueue *q = nullptr;     // AddRef'd while registered
    ID3D12Fence *own = nullptr;          // our fence on this queue (for lists whose own Signal failed)
    UINT64 own_v = 0;
    int di = -1;                         // its device context
    UINT64 freq = 0;
    UINT64 gpu0 = 0, cpu0 = 0;           // last calibration pair
    ULONGLONG cal_ms = 0;
};

struct slot
{
    int state = FREE;
    int qi = -1;                          // -1 once the queue was forgotten
    bool detached = false;                // REC whose queue was forgotten: no sample
    ID3D12Fence *fence = nullptr;         // AddRef'd in SUB / ORPH
    UINT64 value = 0;
};

struct devctx
{
    ID3D12Device *dev = nullptr;          // AddRef'd while the context exists
    ID3D12QueryHeap *heap = nullptr;
    ID3D12Resource *rb = nullptr;
    const UINT64 *map = nullptr;
    slot s[N_SLOTS];
    int next = 0;
    unsigned gen = 0;                     // bumped on every (re)build
    bool failed = false;                  // could not be built on this device: do not retry
    bool retiring = false;                // release once every slot is FREE
};

std::mutex g_mx;
devctx g_d[N_DEV];
qinfo g_q[N_Q];
unsigned g_gen = 0;
LONGLONG g_qpc_freq = 0;

struct ival { LONGLONG a, b; };
std::vector<ival> g_pending;
LONGLONG g_covered = 0;                   // QPC up to which busy time is accounted
LONGLONG g_win_start = 0;                 // current 1 s window (QPC)
LONGLONG g_win_busy = 0;
double   g_last_frac = -1.0;
ULONGLONG g_last_frac_ms = 0;

LONGLONG qpc_now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }

void reset_accounting()
{
    g_pending.clear();
    g_covered = 0; g_win_start = 0; g_win_busy = 0;
    g_last_frac = -1.0; g_last_frac_ms = 0;
}

void free_slot(slot &s)
{
    if (s.fence != nullptr) { s.fence->Release(); s.fence = nullptr; }
    s = slot{};
}

bool removed(const devctx &d) { return d.dev != nullptr && d.dev->GetDeviceRemovedReason() != S_OK; }

// Can the GPU still write this slot? REC: the caller has not said, so yes.
bool live(const devctx &d, const slot &s)
{
    switch (s.state)
    {
    case REC:  return true;
    case SUB:
    case ORPH:
    {
        const UINT64 v = s.fence->GetCompletedValue();
        return v != UINT64_MAX && v < s.value;
    }
    case PIN:  return !removed(d);
    default:   return false;
    }
}

void drop_queue(int qi)
{
    qinfo &q = g_q[qi];
    if (q.own != nullptr) q.own->Release();   // slots that use it hold their own reference
    if (q.q != nullptr) q.q->Release();
    q = qinfo{};
}

// id = gen << 16 | di * N_SLOTS + i
int make_id(int di, int i) { return (int)(((g_d[di].gen & 0x7FFFu) << 16) | (unsigned)(di * N_SLOTS + i)); }
slot *slot_of(int id, devctx **pd)
{
    if (id < 0) return nullptr;
    const int low = id & 0xFFFF, di = low / N_SLOTS, i = low % N_SLOTS;
    if (di >= N_DEV) return nullptr;
    devctx &d = g_d[di];
    if (d.map == nullptr || (unsigned)(id >> 16) != (d.gen & 0x7FFFu)) return nullptr;   // stale id
    if (pd) *pd = &d;
    return &d.s[i];
}

// Releases the context's heap and buffer - only if no slot can still be
// written. Otherwise it retires and poll() tries again. `keep_failed`: a
// device that could not be measured is remembered so it is not retried.
void drop_ctx(int di, bool keep_failed)
{
    devctx &d = g_d[di];
    for (int qi = 0; qi < N_Q; ++qi) if (g_q[qi].di == di) drop_queue(qi);
    if (!keep_failed)
    {
        for (int i = 0; i < N_SLOTS; ++i)
        {
            slot &s = d.s[i];
            if (s.state == FREE) continue;
            s.qi = -1;
            if (s.state == SUB) s.state = ORPH;          // no sample any more
            if (s.state == REC) s.detached = true;
            if (!live(d, s) && s.state != REC) free_slot(s);
        }
        for (int i = 0; i < N_SLOTS; ++i)
            if (d.s[i].state != FREE)
            {
                if (!d.retiring)
                    mgpu::diag::info("[MGPU][GT4] timing resources kept until every measured list is finished "
                                     "or cancelled.");
                d.retiring = true;
                return;
            }
    }
    for (int i = 0; i < N_SLOTS; ++i) free_slot(d.s[i]);
    if (d.rb != nullptr) { if (d.map != nullptr) d.rb->Unmap(0, nullptr); d.rb->Release(); }
    if (d.heap != nullptr) d.heap->Release();
    ID3D12Device *dev = d.dev;
    const bool was_retiring = d.retiring;
    d = devctx{};
    if (keep_failed) { d.dev = dev; d.failed = true; }   // reference kept: the address stays unique
    else if (dev != nullptr) dev->Release();
    if (was_retiring) mgpu::diag::info("[MGPU][GT4] timing resources released (nothing can write them any more).");
}

int qindex(ID3D12CommandQueue *q, int di)
{
    for (int i = 0; i < N_Q; ++i) if (g_q[i].q == q) return i;
    for (int i = 0; i < N_Q; ++i)
        if (g_q[i].q == nullptr)
        {
            UINT64 f = 0;
            if (FAILED(q->GetTimestampFrequency(&f)) || f == 0) return -1;
            ID3D12Fence *own = nullptr;
            if (FAILED(g_d[di].dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own)))) return -1;
            q->AddRef();
            g_q[i].q = q; g_q[i].own = own; g_q[i].own_v = 0; g_q[i].di = di; g_q[i].freq = f;
            return i;
        }
    return -1;
}

void calibrate(qinfo &qi)
{
    const ULONGLONG now = GetTickCount64();
    if (qi.cal_ms != 0 && now - qi.cal_ms < 1000ull) return;
    UINT64 g = 0, c = 0;
    if (SUCCEEDED(qi.q->GetClockCalibration(&g, &c))) { qi.gpu0 = g; qi.cpu0 = c; qi.cal_ms = now; }
}

int ctx_for(ID3D12Device *dev)
{
    for (int i = 0; i < N_DEV; ++i)
        if (g_d[i].dev == dev) return (g_d[i].failed || g_d[i].retiring) ? -1 : i;
    for (int i = 0; i < N_DEV; ++i)
    {
        devctx &d = g_d[i];
        if (d.dev != nullptr) continue;
        d.dev = dev; dev->AddRef();
        d.gen = ++g_gen;
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = N_SLOTS * 2;
        HRESULT hr = dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&d.heap));
        if (SUCCEEDED(hr))
        {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (UINT64)N_SLOTS * 16ull;
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&d.rb));
        }
        void *p = nullptr;
        if (SUCCEEDED(hr)) hr = d.rb->Map(0, nullptr, &p);   // readback: mapped for good
        if (FAILED(hr) || p == nullptr)
        {
            char l[200];
            snprintf(l, sizeof l, "[MGPU][GT1] GPU busy time unavailable on this device (0x%08X).", (unsigned)hr);
            mgpu::diag::warn(l);
            drop_ctx(i, true);
            return -1;
        }
        d.map = (const UINT64 *)p;
        mgpu::diag::info("[MGPU][GT1] measuring the neural card's busy time (timestamps on the mod's lists).");
        return i;
    }
    return -1;
}

// Publish the window that just ended and move on to the next one.
void close_window()
{
    g_last_frac = (double)g_win_busy / (double)g_qpc_freq;
    if (g_last_frac > 1.0) g_last_frac = 1.0;   // rounding only: windows are split exactly now
    g_last_frac_ms = GetTickCount64();
    g_win_busy = 0;
    g_win_start += g_qpc_freq;
}

// Account merged intervals that started before `upto`. GT2: each interval is
// split at window boundaries, so every window gets exactly its own share.
void account(LONGLONG upto)
{
    if (g_pending.empty()) return;
    std::sort(g_pending.begin(), g_pending.end(), [](const ival &x, const ival &y) { return x.a < y.a; });
    size_t k = 0;
    for (; k < g_pending.size() && g_pending[k].a < upto; ++k)
    {
        LONGLONG a = g_pending[k].a, b = g_pending[k].b;
        if (g_win_start == 0) { g_win_start = a; g_covered = a; }
        if (b <= g_covered) continue;          // fully inside work already counted
        if (a < g_covered) a = g_covered;
        g_covered = b;
        // A long idle gap: restart the windows at this interval instead of
        // publishing a run of empty ones.
        if (a - (g_win_start + g_qpc_freq) > g_qpc_freq * 3) { g_win_start = a; g_win_busy = 0; }
        while (a < b)
        {
            const LONGLONG wend = g_win_start + g_qpc_freq;
            if (a >= wend) { close_window(); continue; }
            const LONGLONG part = ((b < wend) ? b : wend) - a;
            g_win_busy += part;
            a += part;
        }
    }
    g_pending.erase(g_pending.begin(), g_pending.begin() + (std::ptrdiff_t)k);
    // Windows that ended with no work after them still have to be published
    // once nothing can land in them any more.
    while (g_win_start != 0 && upto >= g_win_start + g_qpc_freq && g_pending.empty() && g_covered <= upto)
    {
        if (upto - g_win_start > g_qpc_freq * 3) break;   // idle: leave it, "busy" goes stale on its own
        close_window();
    }
}

} // namespace

int begin(ID3D12Device *dev, ID3D12CommandQueue *q, ID3D12GraphicsCommandList *cl)
{
    if (dev == nullptr || q == nullptr || cl == nullptr) return -1;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_qpc_freq == 0) { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); g_qpc_freq = f.QuadPart ? f.QuadPart : 1; }
    const int di = ctx_for(dev);
    if (di < 0) return -1;
    devctx &d = g_d[di];
    const int qi = qindex(q, di);
    if (qi < 0) return -1;
    for (int n = 0; n < N_SLOTS; ++n)
    {
        const int i = (d.next + n) % N_SLOTS;
        if (d.s[i].state != FREE) continue;
        d.next = (i + 1) % N_SLOTS;
        d.s[i] = slot{};
        d.s[i].state = REC; d.s[i].qi = qi;
        cl->EndQuery(d.heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(i * 2));
        return make_id(di, i);
    }
    return -1;   // all in use: this list goes unmeasured
}

void end(ID3D12GraphicsCommandList *cl, int id)
{
    if (cl == nullptr) return;
    std::lock_guard<std::mutex> lk(g_mx);
    devctx *d = nullptr;
    slot *s = slot_of(id, &d);
    if (s == nullptr || s->state != REC) return;
    const int i = (int)(s - d->s);
    cl->EndQuery(d->heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(i * 2 + 1));
    cl->ResolveQueryData(d->heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(i * 2), 2, d->rb, (UINT64)i * 16ull);
}

void cancel(int id)
{
    std::lock_guard<std::mutex> lk(g_mx);
    devctx *d = nullptr;
    slot *s = slot_of(id, &d);
    if (s == nullptr || s->state != REC) return;
    free_slot(*s);   // the caller guarantees the list was never executed
}

void submitted(int id, ID3D12Fence *fence, unsigned long long value)
{
    std::lock_guard<std::mutex> lk(g_mx);
    devctx *d = nullptr;
    slot *s = slot_of(id, &d);
    if (s == nullptr || s->state != REC) return;
    const bool want = !s->detached && s->qi >= 0;
    if (fence != nullptr)
    {
        fence->AddRef();
        s->fence = fence; s->value = value;
        s->state = want ? SUB : ORPH;
        return;
    }
    // Executed, but its own Signal failed: signal OUR fence on the same queue
    // right behind it - queue order makes it pass only after the list.
    if (s->qi >= 0 && g_q[s->qi].q != nullptr && g_q[s->qi].own != nullptr)
    {
        qinfo &q = g_q[s->qi];
        const UINT64 v = q.own_v + 1;
        if (SUCCEEDED(q.q->Signal(q.own, v)))
        {
            q.own_v = v;
            q.own->AddRef();
            s->fence = q.own; s->value = v; s->state = ORPH;
            return;
        }
    }
    s->state = PIN;   // nothing proves completion: kept until the device is removed
}

void poll()
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_qpc_freq == 0) return;
    for (int di = 0; di < N_DEV; ++di)
    {
        devctx &d = g_d[di];
        if (d.map == nullptr) continue;
        bool lost = false;
        for (int i = 0; i < N_SLOTS; ++i)
        {
            slot &s = d.s[i];
            if (s.state == ORPH || s.state == PIN)
            {
                if (!live(d, s)) free_slot(s);
                continue;
            }
            if (s.state != SUB) continue;
            const UINT64 done = s.fence->GetCompletedValue();
            // A removed device signals every fence to UINT64_MAX: nothing it
            // wrote can be trusted, and nothing will be written any more.
            if (done == UINT64_MAX) { lost = true; free_slot(s); continue; }
            if (done < s.value) continue;
            const UINT64 t0 = d.map[i * 2], t1 = d.map[i * 2 + 1];
            const int qi = s.qi;
            free_slot(s);
            if (qi < 0 || g_q[qi].q == nullptr || g_q[qi].freq == 0 || t1 < t0) continue;
            qinfo &q = g_q[qi];
            calibrate(q);
            if (q.cal_ms == 0) continue;
            const double k = (double)g_qpc_freq / (double)q.freq;
            const LONGLONG a = (LONGLONG)q.cpu0 + (LONGLONG)(((double)(LONGLONG)(t0 - q.gpu0)) * k);
            const LONGLONG b = (LONGLONG)q.cpu0 + (LONGLONG)(((double)(LONGLONG)(t1 - q.gpu0)) * k);
            if (b > a) g_pending.push_back({ a, b });
        }
        if (lost && !d.retiring)
        {
            mgpu::diag::warn("[MGPU][GT2] the neural card's device was removed - GPU busy time discarded; it "
                             "starts again on the new device.");
            drop_ctx(di, false);
            reset_accounting();
        }
        else if (d.retiring)
        {
            bool any = false;
            for (int i = 0; i < N_SLOTS; ++i) if (d.s[i].state != FREE) { any = true; break; }
            if (!any) drop_ctx(di, false);
        }
    }
    // Lists on another queue that overlap these may finish a little later:
    // only intervals that started more than 200 ms ago are final.
    account(qpc_now() - g_qpc_freq / 5);
}

bool busy(double *frac)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_last_frac < 0.0 || GetTickCount64() - g_last_frac_ms > 3000ull) return false;
    if (frac) *frac = g_last_frac;
    return true;
}

void forget(ID3D12CommandQueue *q)
{
    if (q == nullptr) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (int qi = 0; qi < N_Q; ++qi)
    {
        if (g_q[qi].q != q) continue;
        const int di = g_q[qi].di;
        if (di >= 0)
            for (int i = 0; i < N_SLOTS; ++i)
            {
                slot &s = g_d[di].s[i];
                if (s.qi != qi) continue;
                s.qi = -1;
                if (s.state == SUB) s.state = ORPH;     // still waits for its fence, no sample
                if (s.state == REC) s.detached = true;  // still waits for cancel / submitted
            }
        drop_queue(qi);
        // Last queue of that device gone: release (or retire) its context.
        bool any = false;
        for (int j = 0; j < N_Q; ++j) if (g_q[j].q != nullptr && g_q[j].di == di) any = true;
        if (di >= 0 && !any && !g_d[di].failed && g_d[di].map != nullptr) drop_ctx(di, false);
    }
    reset_accounting();
}

} // namespace gputime
} // namespace mgpu
