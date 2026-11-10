// Ported from Esoterica (MIT): Engine/Animation/AnimationSyncTrack, Base/Types/Percentage.

#include "anim/anim_sync.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace fx::anim {

float clampPercentage(float p, bool allowLooping)
{
    if (!allowLooping)
        return std::clamp(p, 0.0f, 1.0f);
    float whole;
    float frac = std::modf(p, &whole);
    if (frac < 0.0f)
        frac += 1.0f;  // Esoterica computes 1 - frac here (> 1)
    return frac;
}

float normalizedTime(float p)
{
    float loops;
    float frac = std::modf(p, &loops);
    if (loops > 0.0f && frac == 0.0f)
        frac = 1.0f;
    return frac;
}

SyncTrack::SyncTrack(const Event* markers, int32_t count, int32_t startEventOffset)
{
    if (count <= 0)
        return;  // default: one event over [0, 1)
    count_ = std::min(count, kMaxEvents);
    for (int32_t i = 0; i < count_; ++i)
        events_[i] = {markers[i].id, markers[i].startTime, 0.0f};
    for (int32_t i = 0; i + 1 < count_; ++i)
        events_[i].duration = events_[i + 1].startTime - events_[i].startTime;
    // The last event wraps around to the first.
    events_[count_ - 1].duration = 1.0f - (events_[count_ - 1].startTime - events_[0].startTime);
    offset_ = clampIndex(startEventOffset);
}

SyncTrack::SyncTrack(const SyncTrack& track0, const SyncTrack& track1, float w)
{
    const int32_t n0 = track0.count_, n1 = track1.count_;
    const int32_t lcm = std::min(std::lcm(n0, n1), kMaxEvents);
    const float scale0 = float(n0) / float(lcm), scale1 = float(n1) / float(lcm);
    float start = 0.0f;
    count_ = lcm;
    for (int32_t i = 0; i < lcm; ++i) {
        const Event& e0 = track0.event(i);
        const Event& e1 = track1.event(i);
        const float d = (e0.duration * scale0) + ((e1.duration * scale1) - (e0.duration * scale0)) * w;
        events_[i] = {w > 0.5f ? e1.id : e0.id, start, d};
        start += d;
    }
    const float normalize = start > 0.0f ? 1.0f / start : 1.0f;
    for (int32_t i = 0; i < lcm; ++i) {
        events_[i].startTime *= normalize;
        events_[i].duration *= normalize;
    }
    events_[lcm - 1].duration = 1.0f - events_[lcm - 1].startTime;
    offset_ = 0;
}

float SyncTrack::durationSynchronized(float d0, float d1, int32_t events0, int32_t events1, int32_t eventsLcm, float w)
{
    const float s0 = d0 * (float(eventsLcm) / float(events0));
    const float s1 = d1 * (float(eventsLcm) / float(events1));
    return s0 + (s1 - s0) * w;
}

int32_t SyncTrack::clampIndex(int32_t i) const
{
    int32_t r = i % count_;
    return r < 0 ? r + count_ : r;
}

SyncTrackTime SyncTrack::time(float percentage, bool withOffset) const
{
    SyncTrackTime t;
    if (percentage == 1.0f)
        return {count_ - 1, 1.0f};
    float loops;
    float p = std::modf(percentage, &loops);
    if (p < 0.0f) {
        p += 1.0f;
        loops -= 1.0f;
    }
    if (p < events_[0].startTime) {
        // In the wrapped tail of the last event.
        t.eventIndex = count_ - 1;
        const float delta = events_[0].startTime - p;
        t.percentageThrough = (events_[count_ - 1].duration - delta) / events_[count_ - 1].duration;
    } else {
        for (int32_t i = 0; i < count_; ++i) {
            if (events_[i].startTime + events_[i].duration >= p) {
                t.eventIndex = i;
                t.percentageThrough = (p - events_[i].startTime) / events_[i].duration;
                break;
            }
        }
    }
    t.eventIndex += int32_t(loops) * count_;
    t.eventIndex = clampIndex(t.eventIndex - (withOffset ? offset_ : 0));
    return t;
}

float SyncTrack::percentageThrough(const SyncTrackTime& t, bool withOffset) const
{
    const int32_t i = clampIndex(t.eventIndex + (withOffset ? offset_ : 0));
    float p = events_[i].startTime + events_[i].duration * t.percentageThrough;
    if (p > 1.0f)
        p = normalizedTime(p);
    return p;
}

float SyncTrack::percentageCovered(const SyncTrackTime& start, const SyncTrackTime& end) const
{
    float d = 0.0f;
    const int32_t s = clampIndex(start.eventIndex), e = clampIndex(end.eventIndex);
    if (s == e && start.percentageThrough < end.percentageThrough) {
        d = end.percentageThrough - start.percentageThrough;
    } else {
        d = 1.0f - start.percentageThrough;
        for (int32_t i = clampIndex(s + 1), guard = 0; i != e && guard < count_; i = clampIndex(i + 1), ++guard)
            d += 1.0f;
        d += end.percentageThrough;
    }
    return d / float(count_);
}

int32_t SyncTrack::eventIndexForId(uint32_t id) const
{
    for (int32_t i = 0; i < count_; ++i)
        if (events_[i].id == id)
            return clampIndex(i - offset_);
    return offset_;
}

int32_t SyncTrack::closestEventIndexForId(const SyncTrackTime& t, uint32_t id) const
{
    const int32_t start = clampIndex(t.eventIndex);
    if (events_[start].id == id)
        return clampIndex(start - offset_);
    int32_t down = -1, up = -1;
    for (int32_t i = start - 1; i >= 0; --i)  // (the original never checks index 0)
        if (events_[i].id == id) {
            down = i;
            break;
        }
    for (int32_t i = start + 1; i < count_; ++i)
        if (events_[i].id == id) {
            up = i;
            break;
        }
    int32_t best = -1;
    if (down >= 0 && up >= 0)
        best = (start - down) < (up - start) ? down : up;
    else
        best = down >= 0 ? down : up;
    return best >= 0 ? clampIndex(best - offset_) : offset_;
}

}  // namespace fx::anim
