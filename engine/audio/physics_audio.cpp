#include "audio/physics_audio.h"

#include "audio/audio_system.h"
#include "core/profile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fx {

namespace {

constexpr uint32_t kImpactsPerFrame = 8;
constexpr uint32_t kLoops = 16;
constexpr double kCooldown = 0.06;  // s between hits of one pair
constexpr float kRange = 60.0f;      // m: nothing sounds beyond

// The mass that meets the hit: two moving bodies, their reduced mass; one, its own.
float hitMass(const ContactSide& a, const ContactSide& b)
{
    if (a.mass > 0.0f && b.mass > 0.0f)
        return a.mass * b.mass / (a.mass + b.mass);
    return std::max(a.mass, b.mass);
}
float massLoudness(float mass) { return 0.55f + 0.45f * std::clamp(std::log2(1.0f + mass) / 6.0f, 0.0f, 1.0f); }
float massPitch(float mass) { return std::clamp(1.3f - 0.08f * std::log2(1.0f + mass), 0.7f, 1.5f); }

uint32_t mix(uint64_t a, uint64_t b)
{
    uint64_t h = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull);
    h ^= h >> 31;
    return uint32_t(h * 0xbf58476d1ce4e5b9ull >> 32);
}

}  // namespace

void PhysicsAudio::update(const PhysicsWorld& physics, AudioSystem& audio, const Resolve& resolve, float3 listener, double time)
{
    FX_ZONE_CAT("PhysicsAudio::update", Other);
    ++frame_;
    // Impacts: the loudest few new ones.
    struct Hit {
        const PhysicsImpact* impact;
        float loudness;
    };
    std::vector<Hit> hits;
    for (const PhysicsImpact& h : physics.impacts()) {
        if (h.speed < 0.3f || length(h.point - listener) > kRange)
            continue;
        if (auto it = lastHit_.find(h.pair); it != lastHit_.end() && time - it->second < kCooldown)
            continue;
        const float loudness = std::clamp((h.speed - 0.3f) / 5.0f, 0.0f, 1.3f) * massLoudness(hitMass(h.a, h.b));
        if (loudness > 0.01f)
            hits.push_back({&h, loudness});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.loudness > y.loudness; });
    if (hits.size() > kImpactsPerFrame)
        hits.resize(kImpactsPerFrame);
    for (const Hit& hit : hits) {
        const PhysicsImpact& h = *hit.impact;
        lastHit_[h.pair] = time;
        const SurfaceSound sa = resolve(h.a, h.point), sb = resolve(h.b, h.point);
        const float mass = hitMass(h.a, h.b);
        bool sounded = false;
        for (int side = 0; side < 2; ++side) {
            const SurfaceSound& s = side ? sb : sa;
            const SurfaceSound& other = side ? sa : sb;
            const float gain = hit.loudness * s.gain * (0.25f + 0.75f * other.hardness);
            if (gain < 0.02f)
                continue;
            const float m = (side ? h.b : h.a).mass > 0.0f ? (side ? h.b : h.a).mass : mass;
            if (const AudioVoice v = audio.playSurface(s, SurfaceKind::Impact, h.point, gain, s.pitch * massPitch(m),
                                                       mix(h.pair, frame_ * 2 + uint64_t(side)))) {
                impacts_.push_back(v);
                sounded = true;
            }
        }
        stats_.impacts += sounded ? 1u : 0u;
    }
    std::erase_if(impacts_, [&](AudioVoice v) { return !audio.playing(v); });
    for (auto it = lastHit_.begin(); it != lastHit_.end();)  // (forget old pairs)
        it = time - it->second > 1.0 ? lastHit_.erase(it) : std::next(it);

    // Scrapes and rolls: the loudest touching contacts, each a loop that follows it.
    struct Motion {
        const PhysicsContactMotion* motion;
        bool roll;
        float gain, pitch;
        const ContactSide* surface;  // whose sounds play: the still (or heavier) side
        const ContactSide* other;
    };
    std::vector<Motion> motions;
    for (const PhysicsContactMotion& m : physics.motions()) {
        if (length(m.point - listener) > kRange)
            continue;
        const bool roll = m.roll > 0.15f;
        const float speed = roll ? m.roll : m.slip;
        if (!roll && (m.slip < 0.15f || m.force < 1.0f))
            continue;
        const float mass = std::max(hitMass(m.a, m.b), 0.1f);
        const float press = roll ? 1.0f : std::clamp(std::sqrt(m.force / (mass * 9.81f)), 0.3f, 1.5f);
        const float level = std::clamp(speed / 4.0f, 0.0f, 1.0f) * press * massLoudness(mass);
        const bool aStill = m.a.mass == 0.0f || (m.b.mass > 0.0f && m.a.mass > m.b.mass);
        motions.push_back({&m, roll, level, std::clamp(0.7f + 0.15f * speed, 0.7f, 1.6f), aStill ? &m.a : &m.b, aStill ? &m.b : &m.a});
    }
    std::stable_sort(motions.begin(), motions.end(), [](const Motion& x, const Motion& y) { return x.gain > y.gain; });
    if (motions.size() > kLoops)
        motions.resize(kLoops);
    for (const Motion& mo : motions) {
        const PhysicsContactMotion& m = *mo.motion;
        const SurfaceSound s = resolve(*mo.surface, m.point), other = resolve(*mo.other, m.point);
        const float gain = mo.gain * s.gain * (0.3f + 0.7f * other.hardness);
        const float pitch = mo.pitch * s.pitch;
        auto it = loops_.find(m.contact);
        if (it != loops_.end() && it->second.roll != mo.roll) {  // (from sliding to rolling, or back)
            audio.stop(it->second.voice, 0.08f);
            loops_.erase(it);
            it = loops_.end();
        }
        if (it == loops_.end()) {
            if (gain < 0.02f)
                continue;
            const AudioVoice v = audio.playSurface(s, mo.roll ? SurfaceKind::Roll : SurfaceKind::Scrape, m.point, gain, pitch,
                                                   mix(m.contact, mo.roll ? 2 : 1));
            if (!v)
                continue;
            loops_[m.contact] = {v, mo.roll, frame_};
            (mo.roll ? stats_.rolls : stats_.scrapes) += 1;
        } else {
            it->second.seen = frame_;
            audio.setVoice(it->second.voice, gain, pitch, m.point);
        }
    }
    // Those that stopped, parted or fell out of the loudest: faded out.
    for (auto it = loops_.begin(); it != loops_.end();)
        if (it->second.seen != frame_ || !audio.playing(it->second.voice)) {
            audio.stop(it->second.voice, 0.12f);
            it = loops_.erase(it);
        } else {
            ++it;
        }
    stats_.loops = uint32_t(loops_.size());
}

void PhysicsAudio::stopAll(AudioSystem& audio)
{
    for (auto& [contact, loop] : loops_)
        audio.stop(loop.voice, 0.05f);
    for (AudioVoice v : impacts_)
        audio.stop(v, 0.05f);
    loops_.clear();
    impacts_.clear();
    lastHit_.clear();
    stats_.loops = 0;
}

}  // namespace fx
