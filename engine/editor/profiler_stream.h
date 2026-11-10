#pragma once

// The editor's Profiler tab <-> core/profiler.h.
//   page -> engine  profiler.subscribe {on}    the tab is open / recording (also turns GPU marks on)
//                   profiler.frame {index}     one frame's full detail
//                   profiler.memory            the GPU resource list
//   engine -> page  profiler.frames            every 100 ms while subscribed: new complete frames
//                   profiler.frameDetail       CPU zones of every thread, GPU marks, counters
//                   profiler.memoryDetail      every tracked GPU resource
// Names, counters and thread names go out incrementally with any of these (the page keeps
// tables), so a frame is a few hundred bytes of numbers.

#include <cstdint>
#include <string>

namespace fx {

class JsonWriter;
class UiLayer;
struct Json;

class ProfilerStream {
public:
    // A profiler.* message from the page; false if it was not one.
    bool handle(const std::string& type, const Json& m, UiLayer& ui);
    void update(UiLayer& ui);
    bool subscribed() const { return subscribed_; }

private:
    void writeTables(JsonWriter& w);  // names / counters / threads the page has not seen
    void sendDetail(uint64_t index, UiLayer& ui);
    void sendMemory(UiLayer& ui);

    bool subscribed_ = false;
    uint64_t lastSent_ = 0;    // frames up to here went out
    bool anySent_ = false;
    uint64_t sentNs_ = 0;
    uint32_t namesSent_ = 0, countersSent_ = 0, threadsSent_ = 0;
    bool categoriesSent_ = false;
};

}  // namespace fx
