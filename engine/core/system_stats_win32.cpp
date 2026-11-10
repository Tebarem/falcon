#include "core/system_stats.h"

#include <windows.h>

#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fx::systemstats {

namespace {

uint64_t fileTime(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 21H1 on; the SDK shows it only to builds targeting
// that): PROCESS_MEMORY_COUNTERS_EX, then the private working set.
struct MemoryCounters {
    PROCESS_MEMORY_COUNTERS_EX ex;
    SIZE_T privateWorkingSetSize;
    ULONG64 sharedCommitUsage;
};

// Windows' GPU counters, per instance: "pid_1234_luid_0x00000000_0x0000D1A5_phys_0_eng_3_engtype_3D".
struct GpuCounters {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engines = nullptr, memory = nullptr;
    std::wstring pidTag;  // "pid_1234_"
    std::vector<uint8_t> buffer;

    bool open()
    {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
            return false;
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return false;
        }
        pidTag = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        PdhCollectQueryData(query);  // rates need two samples
        return true;
    }
    void close()
    {
        if (query)
            PdhCloseQuery(query);
        query = nullptr;
    }

    // Every instance of a counter, formatted as doubles.
    template <typename F>
    void each(PDH_HCOUNTER counter, F&& f)
    {
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA)
            return;
        buffer.resize(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
            return;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                f(items[i].szName, items[i].FmtValue.doubleValue);
    }

    // GPU use (the busiest engine type: the sum over its engines, as Task Manager shows) for this
    // process and for all; this process's dedicated memory, and the LUID of the adapter it is on.
    bool sample(float& process, float& system, float& vramMB, LUID& adapter)
    {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS)
            return false;
        std::map<std::wstring, double> mine, all;  // per adapter + engine type
        each(engines, [&](const wchar_t* name, double v) {
            const wchar_t* luid = std::wcsstr(name, L"luid_");
            const wchar_t* phys = luid ? std::wcsstr(luid, L"_phys") : nullptr;
            const wchar_t* type = std::wcsstr(name, L"engtype_");
            if (!phys || !type)
                return;
            const std::wstring key = std::wstring(luid, size_t(phys - luid)) + type;  // the adapter and the engine type
            all[key] += v;
            if (std::wcsncmp(name, pidTag.c_str(), pidTag.size()) == 0)
                mine[key] += v;
        });
        double p = 0.0, s = 0.0;
        for (const auto& [k, v] : mine)
            p = std::max(p, v);
        for (const auto& [k, v] : all)
            s = std::max(s, v);
        process = float(std::min(p, 100.0));
        system = float(std::min(s, 100.0));
        double bytes = 0.0, most = -1.0;
        each(memory, [&](const wchar_t* name, double v) {
            if (std::wcsncmp(name, pidTag.c_str(), pidTag.size()) != 0)
                return;
            bytes += v;
            unsigned long high = 0, low = 0;
            if (const wchar_t* l = std::wcsstr(name, L"luid_"); l && v > most && std::swscanf(l, L"luid_0x%lx_0x%lx", &high, &low) == 2) {
                most = v;
                adapter.HighPart = LONG(high);
                adapter.LowPart = DWORD(low);
            }
        });
        vramMB = float(bytes / (1024.0 * 1024.0));
        return true;
    }
};

// Dedicated memory of the adapter with this LUID (the largest adapter's when unknown).
float adapterMemoryMB(LUID luid)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return -1.0f;
    float match = -1.0f, largest = -1.0f;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(a->GetDesc1(&d))) {
            const float mb = float(double(d.DedicatedVideoMemory) / (1024.0 * 1024.0));
            largest = std::max(largest, mb);
            if (d.AdapterLuid.HighPart == luid.HighPart && d.AdapterLuid.LowPart == luid.LowPart)
                match = mb;
        }
        a->Release();
    }
    factory->Release();
    return match > 0.0f ? match : largest;
}

struct Sampler {
    std::mutex lock;
    std::condition_variable wake;
    std::thread thread;
    bool stop = false;
    SystemUsage usage;

    void run()
    {
        GpuCounters gpu;
        const bool hasGpu = gpu.open();
        float vramTotal = -1.0f;
        LUID adapterSeen = {};
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        const double cores = double(std::max<DWORD>(si.dwNumberOfProcessors, 1));
        uint64_t lastWall = 0, lastProc = 0, lastIdle = 0, lastBusy = 0;
        for (bool first = true;; first = false) {
            SystemUsage u;
            FILETIME now, created, exited, kernel, user, idle, sysKernel, sysUser;
            GetSystemTimeAsFileTime(&now);
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) &&
                GetSystemTimes(&idle, &sysKernel, &sysUser)) {
                const uint64_t wall = fileTime(now), proc = fileTime(kernel) + fileTime(user);
                const uint64_t idleT = fileTime(idle), busy = fileTime(sysKernel) + fileTime(sysUser);  // kernel includes idle
                if (!first && wall > lastWall) {
                    u.cpuProcess = float(100.0 * double(proc - lastProc) / (double(wall - lastWall) * cores));
                    const double total = double(busy - lastBusy);
                    u.cpuSystem = total > 0.0 ? float(100.0 * (total - double(idleT - lastIdle)) / total) : 0.0f;
                }
                lastWall = wall, lastProc = proc, lastIdle = idleT, lastBusy = busy;
            }
            MemoryCounters pm = {};
            constexpr double kMB = 1024.0 * 1024.0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) {
                u.ramProcessMB = float(double(pm.privateWorkingSetSize) / kMB);
                u.ramCommittedMB = float(double(pm.ex.PrivateUsage) / kMB);
            } else if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm.ex), sizeof(pm.ex))) {
                u.ramCommittedMB = float(double(pm.ex.PrivateUsage) / kMB);  // (older Windows: no private working set)
            }
            MEMORYSTATUSEX ms = {sizeof(ms)};
            if (GlobalMemoryStatusEx(&ms)) {
                u.ramTotalMB = float(double(ms.ullTotalPhys) / (1024.0 * 1024.0));
                u.ramUsedMB = float(double(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0));
            }
            LUID adapter = {};
            if (hasGpu && !first && gpu.sample(u.gpuProcess, u.gpuSystem, u.vramProcessMB, adapter)) {
                if (vramTotal < 0.0f || adapter.HighPart != adapterSeen.HighPart || adapter.LowPart != adapterSeen.LowPart) {
                    adapterSeen = adapter;
                    vramTotal = adapterMemoryMB(adapter);
                }
                u.vramTotalMB = vramTotal;
            }
            std::unique_lock l(lock);
            usage = u;  // (the first: memory only; CPU and GPU are rates, from the next)
            if (wake.wait_for(l, std::chrono::milliseconds(first ? 250 : 500), [&] { return stop; }))
                break;
        }
        gpu.close();
    }
};

std::mutex g_startLock;
Sampler* g_sampler = nullptr;

}  // namespace

SystemUsage latest()
{
    {
        std::lock_guard g(g_startLock);
        if (!g_sampler) {
            g_sampler = new Sampler();
            g_sampler->thread = std::thread([] { g_sampler->run(); });
        }
    }
    std::lock_guard l(g_sampler->lock);
    return g_sampler->usage;
}

void shutdown()
{
    std::lock_guard g(g_startLock);
    if (!g_sampler)
        return;
    {
        std::lock_guard l(g_sampler->lock);
        g_sampler->stop = true;
    }
    g_sampler->wake.notify_all();
    g_sampler->thread.join();
    delete g_sampler;
    g_sampler = nullptr;
}

}  // namespace fx::systemstats
