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
    "[MGPU][P1.0c]",                       // DL1: the neural rendering check and its result code
    "[MGPU][DL1]",                         // DL1: which nvngx_dlssnr.dll: path, version, size
    "[MGPU][RS2]",                         // RS2: ReShade Effect Runtime Sync on/off (it loops with the bridge)
    "[MGPU][RC1]",                         // RC1: resolution changed after the stream started - restart
    "[MGPU][PG1]",                         // PG1 (LR99): the bridge's own Present guard - on, off, or a step-over
    "[MGPU][PG2]",                         // PG2 (LR103): the game's Present guard - a step-over or why not
    "[MGPU][LR1]",                         // LR1 (LR97): a vector / depth change followed live, or why not
    "[MGPU][BM2]",                         // LR102: input path record / save and its recording report (a few lines per use)
    "[MGPU][CP1]",                         // CP1: CPU profile (only when CpuProfile=1)
    "[MGPU][TC1]",                         // TC1: temporal stabilisation (only when TemporalStab is on)
    "[MGPU][PO1]",                         // PO1: panel shown by itself, opened/closed
    "[MGPU][PO3]",                         // PO3 (IN87): pointer put back when the panel closes
    "[MGPU][GS1]",                         // GPU safety (SF1, IN70): a GPU error path took the safe branch
    "[MGPU][HX1]",                         // HX1 (IN92): overlay key mirrored to the picture's runtime, ignored
    "[MGPU][RC3]",                         // RC3 (IN91): picture hidden while the game runs at another size
    "[MGPU][EX1]",                         // EX1 (IN89): the game is closing - ordered shutdown started
    "[MGPU][XT1]",                         // exit trace (IN85): what is alive at each destroy event
    "[MGPU][GS2]",                         // GPU safety (SF2, IN82): teardown summary / GPU 0 resources kept
    "[MGPU][RC2]",                         // RC2: colour copy refused / resumed after a resolution change
    "[MGPU][KS1]",                         // KS2: slider picked / first arrow nudge (hook state)
    "[MGPU][PO4]",                         // PO4: Esc closed ReShade's menu / back to the panel
    "[MGPU][HK10]",                        // HK10: a hotkey combination refused / duplicate repaired
    "[MGPU][HK12]",                        // HK12: a hotkey could not be written to mgpu.ini
    "[MGPU][HK13]",                        // HK13: a fixed hotkey put back
    "[MGPU][HK14]",                        // HK14: the keyboard hook missed a key / its thread is stuck
    "[MGPU][HK15]",                        // HK15: a hotkey moved off an earlier default
    "[MGPU][AS1]",                         // AS1: NR architecture answer (RTX 20/30/40)
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
