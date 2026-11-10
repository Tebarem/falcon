#include "gpu/gpu_profiler.h"

#include "core/log.h"
#include "core/profiler.h"

#include <SDL3/SDL_falcon_timestamps.h>

#include <algorithm>
#include <vector>

namespace fx::gpuprof {

namespace {

constexpr uint32_t kSets = 6;          // frames in flight plus readback latency
constexpr uint32_t kCapacity = 1024;   // marks per frame

struct Mark {
    uint32_t name, level;
};

struct Set {
    SDL_FalconTimestamps* timestamps = nullptr;
    std::vector<Mark> marks;
    uint64_t frame = 0;   // profiler frame
    uint64_t ticket = 0;  // 0 = free
    bool full = false;    // every mark (active), else sections only
};

// The sections of the last frames read (ticket, name, ms).
struct SectionTime {
    uint64_t ticket;
    uint32_t name;
    float ms;
};
constexpr size_t kKeptSections = 256;

SDL_GPUDevice* g_device = nullptr;
double g_periodNs = 0.0;
bool g_active = false;
Set g_sets[kSets];
Set* g_current = nullptr;
uint64_t g_nextTicket = 1;
uint64_t g_lastTicket = 0;
Stats g_stats;
uint32_t g_endName = 0;
std::vector<SectionTime> g_sections;

}  // namespace

bool init(SDL_GPUDevice* device)
{
    g_device = device;
    g_periodNs = SDL_FalconTimestampPeriodNs(device);
    if (g_periodNs <= 0.0) {
        FX_WARN("gpu profiler: this device cannot write timestamps (%s)", SDL_GetError());
        return false;
    }
    for (Set& s : g_sets) {
        s.timestamps = SDL_FalconCreateTimestamps(device, kCapacity);
        if (!s.timestamps) {
            FX_WARN("gpu profiler: %s", SDL_GetError());
            shutdown();
            return false;
        }
        s.marks.reserve(kCapacity);
    }
    g_endName = profiler::intern("(end)", profiler::Category::Rendering);
    FX_INFO("gpu profiler: timestamps at %.3g ns, %u sets of %u", g_periodNs, kSets, kCapacity);
    return true;
}

void shutdown()
{
    for (Set& s : g_sets) {
        if (s.timestamps)
            SDL_FalconReleaseTimestamps(g_device, s.timestamps);
        s = {};
    }
    g_current = nullptr;
    g_periodNs = 0.0;
}

bool available() { return g_periodNs > 0.0 && g_sets[0].timestamps; }
void setActive(bool on) { g_active = on && available(); }
bool active() { return g_active; }

void beginFrame(SDL_GPUCommandBuffer* cmd)
{
    g_current = nullptr;
    if (!available())
        return;
    for (Set& s : g_sets)
        if (s.ticket == 0) {
            g_current = &s;
            break;
        }
    if (!g_current) {
        ++g_stats.dropped;
        return;
    }
    g_current->marks.clear();
    g_current->full = g_active;
    SDL_FalconResetTimestamps(cmd, g_current->timestamps);
}

void mark(SDL_GPUCommandBuffer* cmd, uint32_t name, uint32_t level)
{
    if (!g_current || g_current->marks.size() + 1 >= kCapacity || (level > 0 && !g_current->full))  // (the last slot: the end mark)
        return;
    SDL_FalconWriteTimestamp(cmd, g_current->timestamps, uint32_t(g_current->marks.size()));
    g_current->marks.push_back({name, level});
}

void pass(SDL_GPUCommandBuffer* cmd, const char* name)
{
    if (g_current && g_current->full)
        mark(cmd, profiler::intern(name, profiler::Category::Rendering), 1);
}

uint64_t endFrame(SDL_GPUCommandBuffer* cmd)
{
    Set* s = g_current;
    g_current = nullptr;
    if (!s || s->marks.empty())
        return 0;
    SDL_FalconWriteTimestamp(cmd, s->timestamps, uint32_t(s->marks.size()));
    s->marks.push_back({g_endName, 0});
    SDL_FalconResolveTimestamps(cmd, s->timestamps, uint32_t(s->marks.size()));
    s->frame = profiler::currentFrame();
    s->ticket = g_nextTicket++;
    g_lastTicket = s->ticket;
    g_stats.marks = uint32_t(s->marks.size());
    if (s->full)
        profiler::expectGpu();
    return s->ticket;
}

uint64_t lastTicket() { return g_lastTicket; }

void completed(uint64_t ticket)
{
    std::vector<uint64_t> ticks;
    for (Set& s : g_sets) {
        if (s.ticket == 0 || s.ticket > ticket)
            continue;
        const uint32_t n = uint32_t(s.marks.size());
        ticks.assign(n, 0);
        if (SDL_FalconReadTimestamps(g_device, s.timestamps, n, ticks.data())) {
            const double toMs = g_periodNs * 1e-6;
            const uint64_t t0 = ticks[0];
            const auto at = [&](uint32_t i) { return double(std::max(ticks[i], t0) - t0) * toMs; };
            std::vector<profiler::GpuSpan> spans;
            spans.reserve(n);
            for (uint32_t i = 0; i + 1 < n; ++i) {
                uint32_t j = i + 1;
                while (j + 1 < n && s.marks[j].level > s.marks[i].level)
                    ++j;
                spans.push_back({s.marks[i].name, s.marks[i].level, at(i), std::max(at(j), at(i))});
                if (s.marks[i].level == 0) {
                    if (g_sections.size() >= kKeptSections)
                        g_sections.erase(g_sections.begin(), g_sections.begin() + kKeptSections / 2);
                    g_sections.push_back({s.ticket, s.marks[i].name, float(std::max(at(j), at(i)) - at(i))});
                }
            }
            if (s.full)
                profiler::attachGpu(s.frame, std::move(spans), at(n - 1));
        } else if (s.full) {
            profiler::markGpuDone(s.frame);
        }
        s.ticket = 0;
    }
}

Stats stats() { return g_stats; }

bool sectionMs(uint64_t ticket, uint32_t name, float& ms)
{
    for (auto it = g_sections.rbegin(); it != g_sections.rend() && it->ticket >= ticket; ++it)
        if (it->ticket == ticket && it->name == name) {
            ms = it->ms;
            return true;
        }
    return false;
}

}  // namespace fx::gpuprof
