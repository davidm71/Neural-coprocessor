// ---------------------------------------------------------------------------
// pinthread.hpp - TL2: a background thread that cannot outlive this add-on.
//
// A detached std::thread keeps running our code after ReShade unloads the
// add-on (game exit, add-on reload) and crashes the game on the way out. A
// thread started here takes a reference on our module before it starts and
// drops it as its very last act (FreeLibraryAndExitThread), so Windows cannot
// unmap the code under it. A thread that never ends simply keeps the add-on
// loaded until the process exits - safe, and what a process-lifetime worker
// (the keyboard hook's message loop) wants anyway.
// Shared by the NVML monitor (TL1), DLL identification, the scaler's shader
// builds and the hotkey thread.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <functional>
#include <new>

namespace mgpu
{
namespace pin_detail
{
struct job { std::function<void()> fn; HMODULE self; };
inline DWORD WINAPI entry(LPVOID p)
{
    job *j = static_cast<job *>(p);
    HMODULE self = j->self;
    try { j->fn(); } catch (...) {}
    delete j;                              // our code: before the reference is dropped
    FreeLibraryAndExitThread(self, 0);
    return 0;
}
}   // namespace pin_detail

// Returns false (and runs nothing) if the thread could not be started.
inline bool start_pinned_thread(std::function<void()> fn, unsigned stack = 0)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&pin_detail::entry, &self))
        return false;
    pin_detail::job *j = new (std::nothrow) pin_detail::job{std::move(fn), self};
    if (j == nullptr) { FreeLibrary(self); return false; }
    HANDLE t = CreateThread(nullptr, stack, &pin_detail::entry, j, 0, nullptr);
    if (t == nullptr) { delete j; FreeLibrary(self); return false; }
    CloseHandle(t);
    return true;
}

// LR100: the same, but the caller keeps the thread handle so it can wait for
// the end (and close it). nullptr when the thread could not be started.
// Holding the handle also keeps the thread's id from being reused, so a
// PostThreadMessage to that id can never reach another thread.
inline HANDLE start_pinned_thread_h(std::function<void()> fn, unsigned stack = 0)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&pin_detail::entry, &self))
        return nullptr;
    pin_detail::job *j = new (std::nothrow) pin_detail::job{std::move(fn), self};
    if (j == nullptr) { FreeLibrary(self); return nullptr; }
    HANDLE t = CreateThread(nullptr, stack, &pin_detail::entry, j, 0, nullptr);
    if (t == nullptr) { delete j; FreeLibrary(self); return nullptr; }
    return t;
}
}   // namespace mgpu
