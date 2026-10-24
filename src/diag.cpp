// MGPU Bridge - structured diagnostic logging (brief, section 08)
#include <reshade.hpp>

#include "diag.hpp"

#include <atomic>
#include <cstring>

namespace mgpu::diag
{
namespace
{
std::atomic<bool> g_detailed{true};   // until the ini is read at arm (startup lines stay)
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

void set_detailed(bool on) { g_detailed.store(on, std::memory_order_relaxed); }
bool detailed()            { return g_detailed.load(std::memory_order_relaxed); }

void info (const char *line)
{
    if (!g_detailed.load(std::memory_order_relaxed) && periodic(line)) return;
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
