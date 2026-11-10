#include "audio/water_audio.h"

#include "audio/audio_system.h"
#include "audio/procedural_synth.h"
#include "core/profile.h"
#include "physics/physics_world.h"
#include "water/water_system.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace fx {

namespace {

constexpr uint32_t kSplashesPerFrame = 6;
constexpr uint32_t kSloshLoops = 8;
constexpr float kRange = 80.0f;  // m: splashes and slosh beyond are not heard

uint32_t mix(uint64_t a, uint64_t b)
{
    uint64_t h = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull);
    h ^= h >> 31;
    return uint32_t(h * 0xbf58476d1ce4e5b9ull >> 32);
}

// Synthesized splashes come in five sizes (4 cm to 1.2 m), four of each.
constexpr float kSplashSizes[] = {0.04f, 0.1f, 0.25f, 0.55f, 1.2f};

SoundParams placed(float maxDistance, uint32_t bus, bool loop)
{
    SoundParams p;
    p.loop = loop;
    p.spatial = true;
    p.minDistance = 1.0f;
    p.maxDistance = maxDistance;
    p.bus = bus;
    return p;
}

}  // namespace

bool WaterAudio::submerged(const WaterSystem& water, float3 p, float& muffle)
{
    const WaterSample s = water.sample(p);
    if (s.body < 0 || s.depth <= 0.0f)
        return false;
    const WaterSystem::Body& b = *water.body(uint32_t(s.body));
    if (b.desc.shape != WaterShape::Ocean && s.depth >= b.desc.depth)
        return false;
    muffle = b.material.underwaterMuffle;
    return true;
}

void WaterAudio::update(const WaterSystem& water, const PhysicsWorld* physics, AudioSystem& audio, const ListenerPose& l, float dt)
{
    FX_ZONE_CAT("WaterAudio::update", Other);
    ++frame_;
    const float3 at = l.position;

    // Splashes: the loudest few of the bodies that fell in.
    if (physics) {
        struct Splash {
            const WaterEntry* entry;
            float loudness;
        };
        std::vector<Splash> splashes;
        for (const WaterEntry& e : physics->waterEntries()) {
            if (!water.body(uint32_t(e.water)) || length(e.point - at) > kRange)
                continue;
            const float big = std::clamp(std::log2(1.0f + e.size * 10.0f) / 4.0f, 0.0f, 1.0f);
            const float loudness = std::clamp(e.speed / 6.0f, 0.05f, 1.3f) * (0.35f + 0.65f * big);
            splashes.push_back({&e, loudness});
        }
        std::stable_sort(splashes.begin(), splashes.end(), [](const Splash& a, const Splash& b) { return a.loudness > b.loudness; });
        if (splashes.size() > kSplashesPerFrame)
            splashes.resize(kSplashesPerFrame);
        for (const Splash& s : splashes) {
            const WaterEntry& e = *s.entry;
            const WaterMaterial& m = water.body(uint32_t(e.water))->material;
            const float gain = s.loudness * m.splashVolume;
            if (gain < 0.01f)
                continue;
            const uint32_t seed = mix(uint64_t(e.point.x * 1000.0f) ^ (uint64_t(e.point.z * 1000.0f) << 21), frame_);
            // Bigger: lower (a synthesized one is already made for its size class).
            const float pitch = std::clamp(1.25f - 0.2f * std::log2(1.0f + e.size * 4.0f), 0.6f, 1.4f) * (0.95f + 0.1f * float(seed % 101u) / 100.0f);
            uint32_t size = 0;
            for (uint32_t k = 1; k < 5; ++k)
                if (std::fabs(std::log(e.size / kSplashSizes[k])) < std::fabs(std::log(e.size / kSplashSizes[size])))
                    size = k;
            const uint32_t variant = (seed >> 8) % 4u;
            const AudioClip* clip = audio.synthesized("splash|" + std::to_string(size) + "|" + std::to_string(variant),
                                                      [&] { return synthSplash(kSplashSizes[size], variant * 7u + size + 1u); });
            SoundPlay how;
            how.position = e.point;
            how.volume = gain;
            how.pitch = m.splashSound.empty() ? 0.97f + 0.06f * float(seed % 37u) / 36.0f : pitch;
            const SoundParams p = placed(kRange, 1, false);
            if (const AudioVoice v = audio.playOr(m.splashSound, clip, p, how)) {
                splashes_.push_back(v);
                ++stats_.splashes;
            }
        }
        std::erase_if(splashes_, [&](AudioVoice v) { return !audio.playing(v); });

        // Slosh: the loudest things moving at the surface.
        struct Stir {
            const WaterStir* stir;
            float level;
        };
        std::vector<Stir> stirs;
        for (const WaterStir& s : physics->waterStirs()) {
            if (!water.body(uint32_t(s.water)) || length(s.point - at) > kRange)
                continue;
            const float level = std::clamp(s.speed / 2.5f, 0.0f, 1.0f) * std::clamp(s.size / 0.5f, 0.2f, 1.5f);
            if (level > 0.02f)
                stirs.push_back({&s, level});
        }
        std::stable_sort(stirs.begin(), stirs.end(), [](const Stir& a, const Stir& b) { return a.level > b.level; });
        if (stirs.size() > kSloshLoops)
            stirs.resize(kSloshLoops);
        for (const Stir& st : stirs) {
            const WaterStir& s = *st.stir;
            const WaterMaterial& m = water.body(uint32_t(s.water))->material;
            const float gain = 0.8f * st.level * m.sloshVolume;
            const float pitch = (0.8f + 0.3f * std::clamp(s.speed / 3.0f, 0.0f, 1.0f)) * std::clamp(1.2f - 0.3f * s.size, 0.7f, 1.2f);
            auto it = slosh_.find(s.body);
            if (it == slosh_.end()) {
                if (gain < 0.02f)
                    continue;
                const uint32_t variant = uint32_t(s.body % 2u);
                const AudioClip* clip = audio.synthesized("slosh|" + std::to_string(variant), [&] { return synthSlosh(21u + variant); });
                SoundPlay how;
                how.position = s.point;
                how.volume = gain;
                how.pitch = pitch;
                const AudioVoice v = audio.playOr(m.sloshSound, clip, placed(kRange, 1, true), how);
                if (v)
                    slosh_[s.body] = {v, frame_};
            } else {
                it->second.seen = frame_;
                audio.setVoice(it->second.voice, gain, pitch, s.point);
            }
        }
    }
    for (auto it = slosh_.begin(); it != slosh_.end();)
        if (it->second.seen != frame_ || !audio.playing(it->second.voice)) {
            audio.stop(it->second.voice, 0.25f);
            it = slosh_.erase(it);
        } else {
            ++it;
        }
    stats_.sloshing = uint32_t(slosh_.size());

    // Under water: the world muffled, fading over 0.15 s (a dive is quick), and its loop.
    float muffle = 0.0f;
    listenerUnder_ = submerged(water, at, muffle);
    listenerMuffle_ = listenerUnder_ ? muffle : 0.0f;
    const float target = listenerUnder_ ? std::max(muffle, 0.05f) : 0.0f;
    const float step = std::clamp(dt / 0.15f, 0.0f, 1.0f);
    const float before = stats_.underwater;
    stats_.underwater += std::clamp(target - stats_.underwater, -step, step);
    if (stats_.underwater != before)
        audio.setUnderwater(stats_.underwater);
    const WaterSample here = water.sample(at);
    const WaterSystem::Body* in = here.body >= 0 ? water.body(uint32_t(here.body)) : nullptr;
    const float bedGain = in ? in->material.underwaterVolume * std::min(stats_.underwater / std::max(target, 0.05f), 1.0f) : 0.0f;
    if (listenerUnder_ && bedGain > 0.0f) {
        if (!bed_ || !audio.playing(bed_)) {
            const AudioClip* clip = audio.synthesized("underwater", [] { return synthUnderwater(41u); });
            SoundParams p;
            p.loop = true;
            p.spatial = false;
            p.bus = 2;
            SoundPlay how;
            how.volume = 1e-4f;
            bed_ = audio.playOr(in->material.underwaterSound, clip, p, how);
        }
        audio.setVoice(bed_, bedGain, 1.0f, at);
    } else if (bed_) {
        audio.stop(bed_, 0.3f);
        bed_ = 0;
    }
}

void WaterAudio::stopAll(AudioSystem& audio)
{
    for (AudioVoice v : splashes_)
        audio.stop(v, 0.05f);
    splashes_.clear();
    for (auto& [body, loop] : slosh_)
        audio.stop(loop.voice, 0.05f);
    if (bed_)
        audio.stop(bed_, 0.05f);
    slosh_.clear();
    bed_ = 0;
    listenerUnder_ = false;
    listenerMuffle_ = 0.0f;
    if (stats_.underwater > 0.0f)
        audio.setUnderwater(0.0f);
    stats_.underwater = 0.0f;
    stats_.sloshing = 0;
}

}  // namespace fx
