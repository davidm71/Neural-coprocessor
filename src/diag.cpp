// MGPU Bridge - structured diagnostic logging (brief, section 08)
#include <reshade.hpp>

#include "diag.hpp"

#include <atomic>
#include <cstring>

#ifdef MGPU_PRODUCTION
#include <windows.h>
#endif

namespace mgpu::diag
{
namespace
{
#ifndef MGPU_PRODUCTION
std::atomic<bool> g_detailed{true};   // until the ini is read at arm (startup lines stay)
#else
// PR1: the release build. Information lines are written only when they are
// on the short list below, or when DetailedLog=1 - read here, straight from
// the mgpu.ini beside the add-on, so a bug report run gets its startup lines
// too. Warnings and errors are always written.
std::atomic<bool> g_detailed{false};
std::atomic<int>  g_detailed_ini{-1};   // -1 not read yet
const char *const kRelease[] = {
    "[MGPU][R141]",                        // which build this is
    "[MGPU][P7.2]",                        // which mgpu.ini is in force
    "[MGPU][P7.10] window placement",      // which monitor the picture went to
    "[MGPU][P1.6]", "[MGPU][R53]",         // is the depth tap loaded (install check)
    "[MGPU][PF1]",                         // the performance summary, when PerfLog=1
    "[MGPU][HD2]",                         // the game's own HDR, followed
};
bool release_line(const char *line)
{
    if (line == nullptr) return false;
    for (const char *t : kRelease) if (std::strstr(line, t) != nullptr) return true;
    return false;
}
bool detailed_from_ini()
{
    int v = g_detailed_ini.load(std::memory_order_relaxed);
    if (v >= 0) return v != 0;
    v = 0;
    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&detailed_from_ini), &h) != FALSE && h != nullptr)
    {
        wchar_t p[1024];
        const DWORD got = GetModuleFileNameW(h, p, 1000);
        if (got > 0 && got < 1000)
        {
            wchar_t *cut = wcsrchr(p, L'\\');
            if (cut != nullptr)
            {
                wcscpy_s(cut + 1, 1000 - (size_t)(cut + 1 - p), L"mgpu.ini");
                v = (GetPrivateProfileIntW(L"MGPU", L"DetailedLog", 0, p) == 1) ? 1 : 0;
            }
        }
    }
    g_detailed_ini.store(v, std::memory_order_relaxed);
    return v != 0;
}
#endif
// The lines that repeat every few seconds for the whole run.
const char *const kPeriodic[] = {
    "[MGPU][SEAL]", "[MGPU][P9.1]", "[MGPU][R82]", "[MGPU][R71]", "[MGPU][R167]",
    "[MGPU][R101]", "[MGPU][R78]", "[MGPU][R87]", "[MGPU][R63]", "[MGPU][HL1]",
    "[MGPU][T5]", "[MGPU][OPT]", "[MGPU][FW1]", "[MGPU][R258]",
};
bool periodic(const char *line)
{
    if (line == nullptr) return false;
    for (const char *t : kPeriodic) if (std::strstr(line, t) != nullptr) return true;
    return false;
}
}

void set_detailed(bool on)
{
    g_detailed.store(on, std::memory_order_relaxed);
#ifdef MGPU_PRODUCTION
    g_detailed_ini.store(on ? 1 : 0, std::memory_order_relaxed);   // the panel/arm value wins from here on
#endif
}
bool detailed()            { return g_detailed.load(std::memory_order_relaxed); }

void info (const char *line)
{
#ifndef MGPU_PRODUCTION
    if (!g_detailed.load(std::memory_order_relaxed) && periodic(line)) return;
#else
    if (!release_line(line) && !g_detailed.load(std::memory_order_relaxed) &&
        !detailed_from_ini())
        return;
#endif
    reshade::log::message(reshade::log::level::info, line);
}

void warn (const char *line)
{
    reshade::log::message(reshade::log::level::warning, line);
}

void error(const char *line)
{
    reshade::log::message(reshade::log::level::error, line);
}
}
