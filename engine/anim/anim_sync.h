#pragma once

// Sync tracks and percentage time, ported from Esoterica (MIT): AnimationSyncTrack and
// Base/Types/Percentage. A sync track is a cyclic list of events covering [0, 1) of a pose
// node's cycle; synchronized blends advance every input by the same event-space range.

#include <cstdint>

namespace fx::anim {

// Percentage helpers (Esoterica Percentage). wrap: modf, exactly 1.0 -> 0.0 (and fixes the
// original's negative branch). normalizedTime: modf, but whole loops > 0 map to 1.0.
float clampPercentage(float p, bool allowLooping);
float normalizedTime(float p);

struct SyncTrackTime {
    int32_t eventIndex = 0;
    float percentageThrough = 0.0f;  // [0, 1]
    float toFloat() const { return float(eventIndex) + percentageThrough; }
};

struct SyncTrackTimeRange {
    SyncTrackTime start, end;
};

class SyncTrack {
public:
    static constexpr int32_t kMaxEvents = 32;  // also caps blended tracks (lowest common multiple)

    struct Event {
        uint32_t id = 0;
        float startTime = 0.0f;
        float duration = 1.0f;
    };

    SyncTrack() = default;  // one event over [0, 1)
    // From markers sorted by start time (percentages); empty -> the default track.
    SyncTrack(const Event* markers, int32_t count, int32_t startEventOffset = 0);
    // Blended track: LCM(n0, n1) events, offsets baked in, event 0 starts at 0.
    SyncTrack(const SyncTrack& track0, const SyncTrack& track1, float blendWeight);

    static float durationSynchronized(float d0, float d1, int32_t events0, int32_t events1, int32_t eventsLcm, float w);

    int32_t eventCount() const { return count_; }
    int32_t clampIndex(int32_t i) const;
    const Event& event(int32_t i) const { return events_[clampIndex(i + offset_)]; }
    uint32_t eventId(int32_t i) const { return event(i).id; }

    SyncTrackTime time(float percentage, bool withOffset = true) const;
    float percentageThrough(const SyncTrackTime& t, bool withOffset = true) const;
    SyncTrackTime endTime() const { return {count_ - 1, 1.0f}; }
    float percentageCovered(const SyncTrackTime& start, const SyncTrackTime& end) const;
    float percentageCovered(const SyncTrackTimeRange& r) const { return percentageCovered(r.start, r.end); }
    int32_t eventIndexForId(uint32_t id) const;
    int32_t closestEventIndexForId(const SyncTrackTime& t, uint32_t id) const;

private:
    Event events_[kMaxEvents] = {Event{}};
    int32_t count_ = 1;
    int32_t offset_ = 0;
};

}  // namespace fx::anim
