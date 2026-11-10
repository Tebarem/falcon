#pragma once

// Water sounds, from each body's material (.fwater "Sound": its sounds, else synthesized;
// procedural_synth.h), while the game runs:
//   - Splashes: a body falling in (physics' water entries) splashes where it broke the surface,
//     louder for faster and bigger things, lower for bigger ones: a pebble's plip to a crate's
//     sploosh. At most 6 a frame (the loudest), none beyond 80 m.
//   - Slosh: things moving at the surface play a loop that follows them, with their speed through
//     the water and their size. The 8 loudest play.
//   - Under water: the listener inside a body (as the renderer decides) muffles the world (the
//     mixer's low-pass, by the body's Underwater muffle), sounds across the surface more so (above
//     it when under, under it when above), and plays the body's underwater loop.

#include "audio/audio_mixer.h"
#include "core/math.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fx {

class AudioSystem;
class PhysicsWorld;
class WaterSystem;

struct WaterAudioStats {
    uint64_t splashes = 0;     // played since the start
    uint32_t sloshing = 0;     // slosh loops playing now
    float underwater = 0.0f;   // the listener under water: how much (0 .. 1, fading)
};

class WaterAudio {
public:
    void update(const WaterSystem& water, const PhysicsWorld* physics, AudioSystem& audio, const ListenerPose& listener, float dt);
    void stopAll(AudioSystem& audio);
    const WaterAudioStats& stats() const { return stats_; }

    // Whether a point is under a body's surface (within its depth; an ocean: any), and that body's
    // muffle. listenerUnder(): the listener, as of the last update.
    static bool submerged(const WaterSystem& water, float3 p, float& muffle);
    bool listenerUnder() const { return listenerUnder_; }
    float listenerMuffle() const { return listenerMuffle_; }

private:
    struct Loop {
        AudioVoice voice = 0;
        uint64_t seen = 0;
    };
    std::unordered_map<uint64_t, Loop> slosh_;     // by physics body
    std::vector<AudioVoice> splashes_;             // ringing (stopped with the game)
    AudioVoice bed_ = 0;                           // the underwater loop
    bool listenerUnder_ = false;
    float listenerMuffle_ = 0.0f;
    uint64_t frame_ = 0;
    WaterAudioStats stats_;
};

}  // namespace fx
