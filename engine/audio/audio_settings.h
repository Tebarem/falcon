#pragma once

// Volumes (linear), the HRTF switch and the reflections: saved with the project ("audio" in
// project.falcon), for the editor and the game alike.

#include <cstdint>

namespace fx {

struct AudioSettings {
    float master = 1.0f;
    float buses[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // sfx, physics, ambience, music, ui
    bool hrtf = true;  // off: spatial sounds are panned
    // Reflections and reverb from the scene (Steam Audio, simulated on a thread of their own):
    // 0 off, 1 low, 2 medium, 3 high (more rays, bounces, a longer tail, more sounds with their own).
    uint32_t reflections = 2;
    uint32_t reverbType = 2;  // 0 convolution, 1 parametric, 2 hybrid (convolution, then a parametric tail); from the next start
    float reverb = 1.0f;      // their level
    // Sound variation: every one-shot sound without variation of its own plays a little different
    // each time (about +-1 dB, +-0.5 semitone; several clips: never the same one twice running).
    // Loops and the music bus are left as they are; a sound's or an Audio Source's Vary turns it off.
    bool variation = true;
    bool physicsSounds = true;
    bool waterSounds = true;    // splashes, slosh, under water  // impacts, scrapes and rolls from the physics (on the physics bus)
};

// What a reflections quality means.
struct ReflectionQuality {
    uint32_t rays, bounces;
    float seconds;     // impulse response length
    uint32_t sources;  // the nearest sounds with reflections of their own (the rest share the listener's reverb)
    float rate;        // simulations a second
};
inline ReflectionQuality reflectionQuality(uint32_t level)
{
    static const ReflectionQuality kLevels[4] = {
        {0, 0, 0.0f, 0, 0.0f}, {1024, 16, 1.0f, 4, 10.0f}, {2048, 32, 1.5f, 8, 15.0f}, {4096, 64, 2.0f, 16, 20.0f}};
    return kLevels[level < 4 ? level : 3];
}

}  // namespace fx
