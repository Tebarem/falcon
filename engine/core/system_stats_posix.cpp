// core/system_stats.h on Linux and macOS: CPU and RAM (GPU and VRAM stay unknown: -1). Sampled
// when asked, at most twice a second.
#include "core/system_stats.h"

#include "core/platform.h"

#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_page_size.h>
#include <sys/sysctl.h>
#endif

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

namespace fx::systemstats {

namespace {

std::mutex g_mutex;
SystemUsage g_latest;
std::chrono::steady_clock::time_point g_sampled;
bool g_have = false;
double g_lastProcessCpu = 0.0;            // seconds of CPU time
uint64_t g_lastBusy = 0, g_lastTotal = 0;  // system ticks

double processCpuSeconds()
{
    rusage usage = {};
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
}

// All cores' busy and total ticks.
bool systemTicks(uint64_t& busy, uint64_t& total)
{
#if defined(__APPLE__)
    host_cpu_load_info_data_t load = {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, host_info_t(&load), &count) != KERN_SUCCESS)
        return false;
    const uint64_t idle = load.cpu_ticks[CPU_STATE_IDLE];
    total = 0;
    for (int i = 0; i < CPU_STATE_MAX; ++i)
        total += load.cpu_ticks[i];
    busy = total - idle;
    return true;
#else
    std::ifstream stat("/proc/stat");
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!(stat >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || cpu != "cpu")
        return false;
    busy = user + nice + system + irq + softirq + steal;
    total = busy + idle + iowait;
    return true;
#endif
}

void ram(float& usedMB, float& totalMB)
{
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0)
        totalMB = float(double(bytes) / (1024.0 * 1024.0));
    vm_statistics64_data_t vm = {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, host_info64_t(&vm), &count) == KERN_SUCCESS) {
        const double page = double(vm_kernel_page_size);
        usedMB = float(double(vm.active_count + vm.wire_count + vm.compressor_page_count) * page / (1024.0 * 1024.0));
    }
#else
    std::ifstream meminfo("/proc/meminfo");
    double totalKB = -1.0, availableKB = -1.0;
    for (std::string key; meminfo >> key;) {
        double value = 0.0;
        meminfo >> value;
        meminfo.ignore(64, '\n');
        if (key == "MemTotal:")
            totalKB = value;
        else if (key == "MemAvailable:")
            availableKB = value;
    }
    if (totalKB > 0.0) {
        totalMB = float(totalKB / 1024.0);
        if (availableKB >= 0.0)
            usedMB = float((totalKB - availableKB) / 1024.0);
    }
#endif
}

}  // namespace

SystemUsage latest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_have && now - g_sampled < std::chrono::milliseconds(500))
        return g_latest;
    const double seconds = g_have ? std::chrono::duration<double>(now - g_sampled).count() : 0.0;
    const double cpu = processCpuSeconds();
    uint64_t busy = 0, total = 0;
    const bool ticks = systemTicks(busy, total);
    SystemUsage u;
    if (g_have && seconds > 0.0) {
        const long cores = sysconf(_SC_NPROCESSORS_ONLN);
        u.cpuProcess = float((cpu - g_lastProcessCpu) / (seconds * double(cores > 0 ? cores : 1)) * 100.0);
        if (ticks && total > g_lastTotal)
            u.cpuSystem = float(double(busy - g_lastBusy) / double(total - g_lastTotal) * 100.0);
    }
    const platform::ProcessMemory memory = platform::processMemory();
    u.ramProcessMB = float(double(memory.residentBytes) / (1024.0 * 1024.0));
    u.ramCommittedMB = float(double(memory.privateBytes) / (1024.0 * 1024.0));
    ram(u.ramUsedMB, u.ramTotalMB);
    g_lastProcessCpu = cpu;
    g_lastBusy = busy;
    g_lastTotal = total;
    g_sampled = now;
    g_have = true;
    g_latest = u;
    return u;
}

void shutdown() {}

}  // namespace fx::systemstats
