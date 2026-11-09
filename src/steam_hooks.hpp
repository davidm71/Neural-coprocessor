// steam_hooks.hpp - SH1: a one-time, read-only report of how Steam's overlay
// hooks Present on the bridge's swap chain (see steam_hooks.cpp).
#pragma once

namespace mgpu
{
namespace steam_hooks
{
void report(void *real_swapchain);   // once; the swap chain under ReShade's wrapper
}
}
