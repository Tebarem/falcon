#include "editor/profiler_stream.h"

#include "core/json.h"
#include "core/profiler.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL_timer.h>

#include <cmath>

namespace fx {

namespace {

constexpr uint64_t kSendIntervalNs = 100'000'000ull;
constexpr size_t kMaxFramesPerMessage = 600;
constexpr uint64_t kGpuPatienceFrames = 16;  // frames newer than this may still get GPU marks

}  // namespace

bool ProfilerStream::handle(const std::string& type, const Json& m, UiLayer& ui)
{
    if (type == "profiler.subscribe") {
        subscribed_ = m.getBool("on");
        gpuprof::setActive(subscribed_);
        if (subscribed_) {  // a (re)opened page: tables and recent history again
            namesSent_ = countersSent_ = threadsSent_ = 0;
            categoriesSent_ = anySent_ = false;
            sentNs_ = 0;
        }
        return true;
    }
    if (type == "profiler.frame") {
        sendDetail(uint64_t(m.getNumber("index", -1)), ui);
        return true;
    }
    if (type == "profiler.memory") {
        sendMemory(ui);
        return true;
    }
    return false;
}

void ProfilerStream::writeTables(JsonWriter& w)
{
    if (!categoriesSent_) {
        w.beginArray("categories");
        for (uint32_t c = 0; c < uint32_t(profiler::Category::Count); ++c)
            w.value(nullptr, profiler::categoryName(profiler::Category(c)));
        w.endArray();
        categoriesSent_ = true;
    }
    const uint32_t names = profiler::nameCount();
    if (names > namesSent_) {
        w.beginArray("names");
        for (uint32_t i = namesSent_; i < names; ++i) {
            w.beginArray().value(nullptr, i).value(nullptr, std::string_view(profiler::nameOf(i)))
                .value(nullptr, uint32_t(profiler::categoryOf(i))).endArray();
        }
        w.endArray();
        namesSent_ = names;
    }
    const auto counters = profiler::counters();
    if (counters.size() > countersSent_) {
        w.value("counterBase", countersSent_).beginArray("counters");
        for (size_t i = countersSent_; i < counters.size(); ++i)
            w.beginArray().value(nullptr, std::string_view(counters[i].name)).value(nullptr, std::string_view(counters[i].unit))
                .value(nullptr, std::string_view(counters[i].module)).endArray();
        w.endArray();
        countersSent_ = uint32_t(counters.size());
    }
    const auto threads = profiler::threadNames();
    if (threads.size() != threadsSent_) {
        w.beginArray("threadNames");  // not "threads": frameDetail uses that for the zones
        for (const std::string& t : threads)
            w.value(nullptr, std::string_view(t));
        w.endArray();
        threadsSent_ = uint32_t(threads.size());
    }
}

void ProfilerStream::update(UiLayer& ui)
{
    if (!subscribed_)
        return;
    const uint64_t now = SDL_GetTicksNS();
    if (now - sentNs_ < kSendIntervalNs)
        return;
    sentNs_ = now;
    const auto& history = profiler::history();
    if (history.empty())
        return;
    const uint64_t newest = history.back().index;
    JsonWriter w;
    w.beginObject().value("type", "profiler.frames");
    writeTables(w);
    w.value("gpuTiming", gpuprof::available()).beginArray("frames");
    size_t count = 0;
    // Recent history first time round; afterwards frames since the last message. A frame waits
    // for its GPU marks, unless it is old enough that they are not coming.
    size_t first = 0;
    if (!anySent_ && history.size() > kMaxFramesPerMessage)
        first = history.size() - kMaxFramesPerMessage;
    for (size_t i = first; i < history.size() && count < kMaxFramesPerMessage; ++i) {
        const profiler::FrameRecord& f = history[i];
        if (anySent_ && f.index <= lastSent_)
            continue;
        if (!f.gpuDone && newest - f.index < kGpuPatienceFrames)
            break;
        w.beginArray().value(nullptr, f.index).value(nullptr, f.cpuMs()).value(nullptr, f.gpuDone ? f.gpuMs : 0.0);
        w.beginArray();
        for (double ms : f.categoryMs)
            w.value(nullptr, ms);
        w.endArray().beginArray();
        for (const profiler::GpuSpan& s : f.gpu)
            if (s.level == 0)
                w.value(nullptr, s.name).value(nullptr, s.endMs - s.startMs);
        w.endArray().beginArray();
        for (double v : f.counters)
            w.value(nullptr, v);
        w.endArray().endArray();
        lastSent_ = f.index;
        anySent_ = true;
        ++count;
    }
    w.endArray().endObject();
    if (count)
        ui.send(w.str());
}

void ProfilerStream::sendDetail(uint64_t index, UiLayer& ui)
{
    const profiler::FrameRecord* f = profiler::find(index);
    JsonWriter w;
    w.beginObject().value("type", "profiler.frameDetail").value("index", index);
    writeTables(w);
    if (!f) {
        w.value("missing", true).endObject();
        ui.send(w.str());
        return;
    }
    const auto us = [&](uint64_t t) { return (double(int64_t(t - f->start))) * profiler::ticksToMs(1) * 1000.0; };
    w.value("cpuMs", f->cpuMs()).value("gpuMs", f->gpuMs).value("gpuDone", f->gpuDone);
    w.beginArray("threads");
    for (const profiler::ThreadZones& tz : f->threads) {
        w.beginArray().value(nullptr, tz.thread).beginArray();
        for (const profiler::Zone& z : tz.zones)
            w.value(nullptr, z.name).value(nullptr, z.depth).value(nullptr, us(z.start)).value(nullptr, us(z.end));
        w.endArray().endArray();
    }
    w.endArray().beginArray("gpu");
    for (const profiler::GpuSpan& s : f->gpu)
        w.value(nullptr, s.name).value(nullptr, s.level).value(nullptr, s.startMs).value(nullptr, s.endMs);
    w.endArray().beginArray("categoryMs");
    for (double ms : f->categoryMs)
        w.value(nullptr, ms);
    w.endArray().beginArray("counters");
    for (double v : f->counters)
        w.value(nullptr, v);
    w.endArray().endObject();
    ui.send(w.str());
}

void ProfilerStream::sendMemory(UiLayer& ui)
{
    JsonWriter w;
    w.beginObject().value("type", "profiler.memoryDetail").beginArray("resources");
    for (const gpumem::Resource& r : gpumem::resources())
        w.beginArray().value(nullptr, std::string_view(r.module)).value(nullptr, std::string_view(r.name))
            .value(nullptr, std::string_view(r.kind)).value(nullptr, r.bytes).endArray();
    w.endArray().endObject();
    ui.send(w.str());
}

}  // namespace fx
