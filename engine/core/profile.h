#pragma once

// FX_ZONE(name): a CPU zone for the in-engine profiler (core/profiler.h), and for Tracy when it
// is compiled in. FX_ZONE_CAT(name, Category) also sets the category the Profiler's CPU chart
// stacks by (a name keeps the category of its first use).

#include "core/profiler.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>

#include <atomic>

namespace fx::profiler {
// Tracy starts on a thread (runEngine: its timer calibration sleeps 200 ms); zones and frame
// marks before it is up are skipped (on demand, nothing is recorded before a viewer connects).
inline std::atomic<bool> tracyUp{false};
}  // namespace fx::profiler

#define FX_ZONE_CAT(name, category)                                       \
    FX_PROFILER_ZONE(name, ::fx::profiler::Category::category);           \
    SuppressVarShadowWarning(ZoneNamedN(___tracy_scoped_zone, name,       \
                                        ::fx::profiler::tracyUp.load(std::memory_order_acquire)))
#define FX_FRAME_MARK()                                                   \
    do {                                                                  \
        if (::fx::profiler::tracyUp.load(std::memory_order_acquire))      \
            FrameMark;                                                    \
    } while (0)
#else
#define FX_ZONE_CAT(name, category) FX_PROFILER_ZONE(name, ::fx::profiler::Category::category)
#define FX_FRAME_MARK()
#endif
#define FX_ZONE(name) FX_ZONE_CAT(name, Other)
