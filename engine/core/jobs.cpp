#include "core/jobs.h"

#include "core/dmath.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profile.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <string>
#include <thread>

namespace fx::jobs {

namespace {

enki::TaskScheduler* g_scheduler = nullptr;
uint32_t g_threads = 1;

}  // namespace

void init(uint32_t threads)
{
    FX_ZONE("jobs::init");
    if (g_scheduler)
        return;
    if (!threads)
        threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    g_threads = std::min(threads, 32u);
    g_scheduler = new enki::TaskScheduler();
    enki::TaskSchedulerConfig config;
    config.numTaskThreadsToCreate = g_threads - 1;  // the calling thread works too
    // Worker names for the profiler's timeline (and debuggers).
    config.profilerCallbacks.threadStart = [](uint32_t threadnum) {
        dmath::setupFloatingPoint();  // (MXCSR is per thread: the simulation's steps run here too)
        const std::string name = "Worker " + std::to_string(threadnum);
        profiler::nameThread(name.c_str());
        platform::setThreadName(name);
    };
    g_scheduler->Initialize(config);
    FX_INFO("jobs: %u threads", g_threads);
}

void shutdown()
{
    if (!g_scheduler)
        return;
    g_scheduler->WaitforAllAndShutdown();
    delete g_scheduler;
    g_scheduler = nullptr;
}

enki::TaskScheduler& scheduler() { return *g_scheduler; }
uint32_t threadCount() { return g_threads; }

}  // namespace fx::jobs
