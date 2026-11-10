#pragma once

// Valve's Steam Audio (Apache 2.0; scripts/fetch-steamaudio.ps1). phonon.dll is delay-loaded: init()
// first loads it from audio/ next to the executable by full path, so the delay-load helper finds it
// already loaded, and returns false (the mixer then pans) when it is missing or does not start. No
// ipl* call is made before that.
//
// The context, the default HRTF, and per spatial voice a direct effect (air absorption, occlusion,
// transmission) and a binaural effect (HRTF). Reflections: effects for the voices with reflections
// of their own, one for the listener's reverb (every other voice), a mixer (convolution), all into
// an ambisonic field decoded once a block for the listener through the HRTF. Distance attenuation is Steam Audio's inverse-distance model, scaled to
// 1 at the minimum distance (its own gives 1 / max(distance, minDistance)).

#include "audio/audio_mixer.h"

#include <atomic>
#include <memory>

namespace fx {

class SteamAudio final : public Spatializer {
public:
    SteamAudio();
    ~SteamAudio() override;
    struct Reflections {
        uint32_t type = 2;       // 0 convolution, 1 parametric, 2 hybrid
        uint32_t order = 1;      // ambisonic order
        float seconds = 2.0f;    // the longest impulse response
        uint32_t slots = 16;     // voices with reflections of their own
    };
    bool init(uint32_t slots, const Reflections& reflections);
    // The listener's reverb: its simulated source (opaque; null: none), from the scene.
    void setListenerSource(void* source);
    const Reflections& reflections() const;
    void shutdown();
    bool active() const;
    const char* version() const;  // "4.8.1" (the DLL's), or "" before init

    uint32_t slots() const override;
    void reset(uint32_t slot) override;
    float distanceAttenuation(float distance, float minDistance) const override;
    void apply(uint32_t slot, float3 direction, float airDistance, const float* direct, const float* mono, float* left, float* right) override;
    void* context() const;  // the IPLContext (null before init)

    uint32_t reflectionSlots() const override;
    void resetReflection(uint32_t slot) override;
    void beginReflections() override;
    void reflect(uint32_t slot, void* source, const float* dry) override;
    void reverbSend(const float* in) override;
    void endReflections(const ListenerPose& listener, float gain, float* left, float* right) override;
    bool paths() const override;
    void path(uint32_t slot, void* source, const float* dry, float gain0, float gain1, const ListenerPose& listener, float* left,
              float* right) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fx
