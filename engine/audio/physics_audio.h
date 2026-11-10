#pragma once

// Physics sounds: what Box3D's contacts sound like, from each surface's sounds (or synthesized;
// assets/surface_sound.h), placed where they happen (so they are occluded, echo and reverberate as
// any sound in the world).
//   - Impacts: each hit of a step (its approach speed) sounds on both surfaces, louder for faster
//     and heavier hits, lower for heavier things, each damped by how soft the other one is (metal
//     on sand: mostly a thud). At most 8 new ones a frame (the loudest), none closer than 60 ms on
//     the same pair, none beyond 60 m.
//   - Scrapes and rolls: a contact sliding along a surface, or a ball rolling on it, plays a loop
//     that follows it (its level with the speed and how hard they press, its pitch with the speed),
//     fading out when it stops or parts. The 16 loudest play.

#include "assets/surface_sound.h"
#include "audio/audio_mixer.h"
#include "physics/physics_world.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace fx {

class AudioSystem;

struct PhysicsAudioStats {
    uint64_t impacts = 0, scrapes = 0, rolls = 0;  // started since the start
    uint32_t loops = 0;                            // scrapes and rolls playing now
};

class PhysicsAudio {
public:
    // A contact side's surface sounds, where it was touched.
    using Resolve = std::function<SurfaceSound(const ContactSide& side, float3 point)>;

    void update(const PhysicsWorld& physics, AudioSystem& audio, const Resolve& resolve, float3 listener, double time);
    void stopAll(AudioSystem& audio);
    const PhysicsAudioStats& stats() const { return stats_; }

private:
    struct Loop {
        AudioVoice voice = 0;
        bool roll = false;
        uint64_t seen = 0;
    };
    std::unordered_map<uint64_t, Loop> loops_;       // by contact
    std::vector<AudioVoice> impacts_;                // ringing (stopped with the game)
    std::unordered_map<uint64_t, double> lastHit_;   // by shape pair
    uint64_t frame_ = 0;
    PhysicsAudioStats stats_;
};

}  // namespace fx
