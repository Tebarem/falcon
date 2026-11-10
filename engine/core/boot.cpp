#include "core/boot.h"

#include "core/log.h"
#include "core/platform.h"

namespace fx::boot {

double msSinceProcessStart() { return platform::msSinceProcessStart(); }

void mark(const char* label)
{
    FX_INFO("[boot] %-28s %8.2f ms", label, msSinceProcessStart());
}

}  // namespace fx::boot
