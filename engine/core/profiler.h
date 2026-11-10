#pragma once

// In-engine profiler: CPU zones on every thread, GPU marks (gpu/gpu_profiler.h), counters, and a
// history of recent frames for the editor's Profiler tab (and --profile).
//
// Zones are recorded when they close: {name, depth, start, end} goes to the closing thread's
// buffer, so a frame boundary never has to split an open zone. endFrame() (main thread, between
// frames) moves every thread's closed zones into the frame's record. Worker zones are complete
// by then because the frame waits for its tasks.
//
// Names are interned once per call site (FX_ZONE keeps a static id) and carry a category, which
// the CPU chart stacks by. Recording is off unless enabled (editor, --profile): a disabled zone
// costs one relaxed atomic load.

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fx::profiler {

enum class Category : uint8_t { Other, Rendering, Scripts, Physics, Animation, UI, Editor, Wait, Count };
const char* categoryName(Category c);

struct Zone {
    uint32_t name;   // interned id
    uint32_t depth;  // nesting on its thread, 0 = outermost
    uint64_t start, end;  // performance-counter ticks
};

struct ThreadZones {
    uint32_t thread;  // index into threadNames()
    std::vector<Zone> zones;  // in closing order (children before their parents)
};

// One GPU mark: the time from this mark to the next mark of the same or a higher level.
struct GpuSpan {
    uint32_t name;
    uint32_t level;  // 0 = section, 1 = pass
    double startMs, endMs;  // from the frame's first mark
};

struct FrameRecord {
    uint64_t index = 0;
    uint64_t start = 0, end = 0;  // ticks: the previous endFrame to this one
    std::vector<ThreadZones> threads;
    std::array<double, size_t(Category::Count)> categoryMs{};  // main thread self time; Other = uncovered
    std::vector<double> counters;  // by counter id; NaN = not sampled yet
    std::vector<GpuSpan> gpu;
    double gpuMs = 0.0;
    bool gpuDone = false;  // GPU marks arrived (or none were written)
    double cpuMs() const;
};

extern std::atomic<bool> g_enabled;
inline bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
void setEnabled(bool on);

// Names: interned by content; the id is stable for the process.
uint32_t intern(const char* name, Category category = Category::Other);
std::string nameOf(uint32_t id);
Category categoryOf(uint32_t id);
uint32_t nameCount();

uint64_t now();  // performance-counter ticks
double ticksToMs(uint64_t ticks);

// Threads: the calling thread's display name ("Main", "Worker 3"). Unnamed threads get
// "Thread N" on their first zone.
void nameThread(const char* name);
std::vector<std::string> threadNames();

class ScopedZone {
public:
    explicit ScopedZone(uint32_t name) : name_(name), active_(enabled()) { if (active_) begin(); }
    ~ScopedZone() { if (active_) end(); }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    void begin();
    void end();
    uint32_t name_;
    uint32_t depth_ = 0;
    uint64_t start_ = 0;
    bool active_;
};

// Counters: registered once (name, unit, module), sampled per frame; the last sample sticks.
uint32_t counter(const char* name, const char* unit, const char* module);
void setCounter(uint32_t id, double value);
struct CounterInfo {
    std::string name, unit, module;
};
std::vector<CounterInfo> counters();

// Frame boundary (main thread). Closes the current frame's record and starts the next one.
void endFrame();
uint64_t currentFrame();  // index of the frame being recorded

// GPU marks for a recorded frame (from GpuProfiler, a few frames late). expectGpu(): marks are
// being written for the current frame, so its record waits for them.
void expectGpu();
void attachGpu(uint64_t frame, std::vector<GpuSpan> spans, double gpuMs);
void markGpuDone(uint64_t frame);  // no marks were written for it

// History: the last kHistory frames, oldest first. Readers run on the main thread, like the
// writers, so no locking.
constexpr size_t kHistory = 600;
const std::deque<FrameRecord>& history();
const FrameRecord* find(uint64_t frame);

}  // namespace fx::profiler

#define FX_PROFILER_CONCAT2(a, b) a##b
#define FX_PROFILER_CONCAT(a, b) FX_PROFILER_CONCAT2(a, b)
#define FX_PROFILER_ZONE(name, category)                                                               \
    static const uint32_t FX_PROFILER_CONCAT(fxZoneName, __LINE__) = ::fx::profiler::intern(name, category); \
    const ::fx::profiler::ScopedZone FX_PROFILER_CONCAT(fxZone, __LINE__)(FX_PROFILER_CONCAT(fxZoneName, __LINE__))
