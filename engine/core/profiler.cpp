#include "core/profiler.h"

#include <SDL3/SDL_timer.h>

#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fx::profiler {

std::atomic<bool> g_enabled{false};

namespace {

struct ThreadBuffer {
    uint32_t index = 0;
    uint32_t depth = 0;  // owning thread only
    std::mutex mutex;    // zones: owning thread appends, endFrame takes
    std::vector<Zone> zones;
};

struct NameEntry {
    std::string name;
    Category category;
};

std::mutex g_namesMutex;
std::unordered_map<std::string, uint32_t> g_nameIds;
std::vector<NameEntry> g_names;

std::mutex g_threadsMutex;
std::vector<std::unique_ptr<ThreadBuffer>> g_threads;
std::vector<std::string> g_threadNames;
thread_local ThreadBuffer* t_buffer = nullptr;

std::mutex g_countersMutex;
std::vector<CounterInfo> g_counterInfo;
std::vector<double> g_counterValues;

std::deque<FrameRecord> g_history;
uint64_t g_frame = 0;
uint64_t g_frameStart = 0;
bool g_expectGpu = false;

const double g_tickMs = 1000.0 / double(SDL_GetPerformanceFrequency());

ThreadBuffer& registerThread(const char* name)
{
    std::lock_guard lock(g_threadsMutex);
    auto b = std::make_unique<ThreadBuffer>();
    b->index = uint32_t(g_threads.size());
    g_threadNames.push_back(name ? name : "Thread " + std::to_string(b->index));
    t_buffer = b.get();
    g_threads.push_back(std::move(b));
    return *t_buffer;
}

ThreadBuffer& buffer() { return t_buffer ? *t_buffer : registerThread(nullptr); }

}  // namespace

const char* categoryName(Category c)
{
    static const char* const kNames[] = {"Other", "Rendering", "Scripts", "Physics", "Animation", "UI", "Editor", "Wait"};
    return kNames[size_t(c) < size_t(Category::Count) ? size_t(c) : 0];
}

void setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

uint32_t intern(const char* name, Category category)
{
    std::lock_guard lock(g_namesMutex);
    const auto [it, inserted] = g_nameIds.try_emplace(name ? name : "?", uint32_t(g_names.size()));
    if (inserted)
        g_names.push_back({it->first, category});
    return it->second;
}

std::string nameOf(uint32_t id)
{
    std::lock_guard lock(g_namesMutex);
    return id < g_names.size() ? g_names[id].name : std::string("?");
}

Category categoryOf(uint32_t id)
{
    std::lock_guard lock(g_namesMutex);
    return id < g_names.size() ? g_names[id].category : Category::Other;
}

uint32_t nameCount()
{
    std::lock_guard lock(g_namesMutex);
    return uint32_t(g_names.size());
}

uint64_t now() { return SDL_GetPerformanceCounter(); }
double ticksToMs(uint64_t ticks) { return double(ticks) * g_tickMs; }

void nameThread(const char* name)
{
    if (t_buffer) {
        std::lock_guard lock(g_threadsMutex);
        g_threadNames[t_buffer->index] = name;
        return;
    }
    registerThread(name);
}

std::vector<std::string> threadNames()
{
    std::lock_guard lock(g_threadsMutex);
    return g_threadNames;
}

void ScopedZone::begin()
{
    ThreadBuffer& b = buffer();
    depth_ = b.depth++;
    start_ = now();
}

void ScopedZone::end()
{
    const uint64_t t = now();
    ThreadBuffer& b = buffer();
    --b.depth;
    std::lock_guard lock(b.mutex);
    b.zones.push_back({name_, depth_, start_, t});
}

uint32_t counter(const char* name, const char* unit, const char* module)
{
    std::lock_guard lock(g_countersMutex);
    for (uint32_t i = 0; i < g_counterInfo.size(); ++i)
        if (g_counterInfo[i].name == name)
            return i;
    g_counterInfo.push_back({name, unit, module});
    g_counterValues.push_back(std::numeric_limits<double>::quiet_NaN());
    return uint32_t(g_counterInfo.size() - 1);
}

void setCounter(uint32_t id, double value)
{
    std::lock_guard lock(g_countersMutex);
    if (id < g_counterValues.size())
        g_counterValues[id] = value;
}

std::vector<CounterInfo> counters()
{
    std::lock_guard lock(g_countersMutex);
    return g_counterInfo;
}

double FrameRecord::cpuMs() const { return ticksToMs(end - start); }

void endFrame()
{
    const uint64_t t = now();
    FrameRecord r;
    r.index = g_frame;
    r.start = g_frameStart ? g_frameStart : t;
    r.end = t;
    const uint32_t mainThread = t_buffer ? t_buffer->index : ~0u;
    {
        std::lock_guard lock(g_threadsMutex);
        for (const auto& b : g_threads) {
            std::lock_guard zl(b->mutex);
            if (b->zones.empty())
                continue;
            r.threads.push_back({b->index, std::move(b->zones)});
            b->zones = {};
        }
    }
    // Main thread self time per category. Zones arrive children first, so the children's total
    // at depth d + 1 is complete when their parent at depth d closes.
    double covered = 0.0;
    for (const ThreadZones& tz : r.threads) {
        if (tz.thread != mainThread)
            continue;
        std::vector<double> childMs(64, 0.0);
        for (const Zone& z : tz.zones) {
            const double ms = ticksToMs(z.end - z.start);
            const uint32_t d = std::min<uint32_t>(z.depth, 62);
            const double self = std::max(ms - childMs[d + 1], 0.0);
            childMs[d + 1] = 0.0;
            childMs[d] += ms;
            r.categoryMs[size_t(categoryOf(z.name))] += self;
            if (z.depth == 0)
                covered += ms;
        }
    }
    r.categoryMs[size_t(Category::Other)] += std::max(r.cpuMs() - covered, 0.0);
    {
        std::lock_guard lock(g_countersMutex);
        r.counters = g_counterValues;
    }
    r.gpuDone = !g_expectGpu;
    g_expectGpu = false;
    g_history.push_back(std::move(r));
    while (g_history.size() > kHistory)
        g_history.pop_front();
    ++g_frame;
    g_frameStart = t;
}

uint64_t currentFrame() { return g_frame; }

void expectGpu() { g_expectGpu = true; }

static FrameRecord* findMutable(uint64_t frame)
{
    for (auto it = g_history.rbegin(); it != g_history.rend(); ++it)
        if (it->index == frame)
            return &*it;
    return nullptr;
}

const FrameRecord* find(uint64_t frame) { return findMutable(frame); }

void attachGpu(uint64_t frame, std::vector<GpuSpan> spans, double gpuMs)
{
    if (FrameRecord* r = findMutable(frame)) {
        r->gpu = std::move(spans);
        r->gpuMs = gpuMs;
        r->gpuDone = true;
    }
}

void markGpuDone(uint64_t frame)
{
    if (FrameRecord* r = findMutable(frame))
        r->gpuDone = true;
}

const std::deque<FrameRecord>& history() { return g_history; }

}  // namespace fx::profiler
