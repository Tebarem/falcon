#pragma once

#include <cstdint>

namespace enki {
class TaskScheduler;
}

namespace fx::jobs {

// Process-wide enkiTS scheduler. `threads` 0 = one per performance core (approximated as
// half the hardware threads, since SMT siblings add little for physics), at least 1.
void init(uint32_t threads = 0);
void shutdown();
enki::TaskScheduler& scheduler();
uint32_t threadCount();

}  // namespace fx::jobs
