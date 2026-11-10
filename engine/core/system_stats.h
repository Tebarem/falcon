#pragma once

// This process's and the system's CPU, memory and GPU use, sampled twice a second on a thread of
// its own (started by the first call, so it costs nothing until something asks).
//   CPU      process: its threads' time over all cores; system: busy time of all cores
//   RAM      process: its private memory in physical RAM (Task Manager's Memory column), and all
//            of it committed (part may be paged out, or never touched); system: in use / installed
//   GPU      Task Manager's figure, from Windows' "GPU Engine" counters: the busiest engine type
//            (3D, compute, copy, ...), as a share of its time; process: this process's work only
//   VRAM     dedicated memory this process uses, and the adapter's total
// Unknown values (no GPU counters on this system) are -1.

#include <cstdint>

namespace fx {

struct SystemUsage {
    float cpuProcess = -1.0f, cpuSystem = -1.0f;       // percent
    float gpuProcess = -1.0f, gpuSystem = -1.0f;       // percent
    float ramProcessMB = -1.0f, ramUsedMB = -1.0f, ramTotalMB = -1.0f;
    float ramCommittedMB = -1.0f;
    float vramProcessMB = -1.0f, vramTotalMB = -1.0f;
};

namespace systemstats {

// The latest sample (starts the sampler on the first call; until its first sample: unknowns).
SystemUsage latest();
void shutdown();

}  // namespace systemstats

}  // namespace fx
