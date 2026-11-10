#pragma once

// The audio mixer: voices (decoded clips, or a test tone) summed into stereo float at 48 kHz in
// blocks of 512 frames (Steam Audio's frame). The main thread sends it commands through a
// lock-free ring and hears back about voices that finished through another; render() runs on
// the device's thread (or the main thread with the null device) and never locks or allocates.
//
// A spatial voice is placed in the world (or relative to the listener): it is attenuated by
// distance (full volume within minDistance, then inverse distance (Steam Audio's model) or a
// straight line, silent at maxDistance), loses its highs with distance (air absorption, Steam
// Audio's) and is spatialized through a Spatializer slot (Steam Audio's HRTF) or, without one,
// panned (equal power; no air absorption).

#include "core/math.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace fx {

constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioFrameSize = 512;  // samples per channel per block (10.7 ms)

enum class AudioBus : uint8_t { Master, Sfx, Physics, Ambience, Music, Ui, Count };

// Decoded audio at kAudioSampleRate: interleaved float, 1 or 2 channels.
struct AudioClip {
    std::string path;
    std::vector<float> samples;
    uint32_t channels = 1, frames = 0;
};

enum AudioVoiceFlags : uint32_t {
    kVoiceLoop = 1u << 0,
    kVoiceSpatial = 1u << 1,           // placed at `position` (else 2D: straight to both ears)
    kVoiceListenerRelative = 1u << 2,  // position in the listener's space (x right, y up, -z ahead)
    kVoiceAirAbsorption = 1u << 3,     // spatial: far away, the highs fade first
    kVoiceLinearRolloff = 1u << 4,     // spatial: a straight line from minDistance to maxDistance
    kVoiceReflections = 1u << 5,       // spatial: echoes and reverb (its own, or the listener's reverb)
};

// (X4) Audio a script makes as it goes: a ring of interleaved float frames (1 or 2 channels) at
// kAudioSampleRate. The main thread writes, the mixer's voice reads (no locks; it must outlive the
// voice). A block it can't fill plays silence for what is missing and counts an underrun (once it
// has had something to play).
class AudioStream {
public:
    AudioStream(uint32_t channels, uint32_t capacityFrames);
    uint32_t channels() const { return channels_; }
    uint32_t capacity() const { return capacity_; }
    // Main thread: as many frames as there is room for; how many were taken.
    uint32_t write(const float* interleaved, uint32_t frames);
    uint32_t queued() const;  // frames written and not played yet
    uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    // The mixer's thread: up to `frames` into left / right (zero after what it had); how many.
    uint32_t read(float* left, float* right, uint32_t frames);

private:
    uint32_t channels_, capacity_;  // capacity: a power of two, in frames
    std::vector<float> samples_;
    std::atomic<uint32_t> head_{0}, tail_{0};  // frames
    std::atomic<uint32_t> underruns_{0};
    bool started_ = false;  // (mixer thread) something played: running dry counts from then
};

struct AudioVoiceParams {
    float gain = 1.0f, pitch = 1.0f;
    float3 position = {0.0f, 0.0f, 0.0f};
    float minDistance = 1.0f, maxDistance = 100.0f;
    AudioBus bus = AudioBus::Sfx;
    uint32_t flags = 0;
    float toneHz = 0.0f;  // no clip: a sine at this frequency, amplitude 1 (tests)
    AudioStream* stream = nullptr;  // (X4) no clip: what a script writes (pitch does not apply)
};

using AudioVoice = uint32_t;  // 0: none (generation << 16 | slot + 1)

struct ListenerPose {
    float3 position = {0.0f, 0.0f, 0.0f};
    float3 right = {1.0f, 0.0f, 0.0f}, up = {0.0f, 1.0f, 0.0f}, forward = {0.0f, 0.0f, -1.0f};
};

// Turns a mono block into a stereo one for a direction (listener space: x right, y up, -z ahead,
// unit length), with air absorption over `airDistance` metres (< 0: none). Slots are preallocated
// effects; everything here runs on the mixer's thread. direct: occlusion (1 clear .. 0 blocked)
// and transmission per band from the scene (null: nothing in the way).
class Spatializer {
public:
    virtual ~Spatializer() = default;
    virtual uint32_t slots() const = 0;
    virtual void reset(uint32_t slot) = 0;
    // Inverse-distance attenuation, 1 within minDistance.
    virtual float distanceAttenuation(float distance, float minDistance) const = 0;
    virtual void apply(uint32_t slot, float3 direction, float airDistance, const float* direct, const float* mono, float* left, float* right) = 0;

    // Reflections (optional): each block, begin, then per reflected voice either reflect() (its own
    // simulated reflections, through a reflection slot) or reverbSend() (into the listener's reverb),
    // then end() adds what they make (decoded for the listener) into left / right.
    virtual uint32_t reflectionSlots() const { return 0; }
    virtual void resetReflection(uint32_t) {}
    virtual void beginReflections() {}
    virtual void reflect(uint32_t, void*, const float*) {}  // slot, the simulated source (opaque), dry mono
    virtual void reverbSend(const float*) {}
    virtual void endReflections(const ListenerPose&, float, float*, float*) {}  // (gain)
    // Paths (optional, baked): the sound that finds its way around what blocks it, through a
    // spatial slot's path effect, its level ramped from gain0 to gain1, added into left / right.
    virtual bool paths() const { return false; }
    virtual void path(uint32_t, void*, const float*, float, float, const ListenerPose&, float*, float*) {}  // slot, source, dry
};

// Single-producer, single-consumer ring (N a power of two).
template <typename T, uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v)
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N)
            return false;
        items_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v)
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false;
        v = items_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    T items_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

struct AudioMixerStats {
    uint64_t blocks = 0;  // blocks mixed since start
    uint64_t mixNs = 0;   // time spent mixing them
    uint32_t voices = 0;  // voices playing (main thread's view)
    uint32_t spatialized = 0;  // of the last block's voices, through the Spatializer
    uint32_t paths = 0;        // of those, heard along their baked paths too
    float peak = 0.0f;         // (X4) the last block's loudest sample (0 .. 1)
};

class AudioMixer {
public:
    // spatializer: optional (null: panning), owned by the caller, outliving the mixer.
    void init(uint32_t maxVoices, Spatializer* spatializer);

    // Main thread. Clips must stay alive while a voice plays them.
    AudioVoice play(const AudioClip* clip, const AudioVoiceParams& params);
    void stop(AudioVoice voice, float fadeSeconds = 0.02f);
    void set(AudioVoice voice, float gain, float pitch, float3 position);
    void setListener(const ListenerPose& listener);
    void setBusGain(AudioBus bus, float gain);
    void setSpatializerEnabled(bool on);  // off: every spatial voice is panned (no HRTF)
    // What the scene does to a voice: occlusion (1 clear .. 0 blocked) and transmission per band.
    void setDirect(AudioVoice voice, float occlusion, const float transmission[3]);
    // A voice's own reflections: the Spatializer's reflection slot and its simulated source (an
    // opaque handle that stays valid while the voice plays); slot < 0: the listener's reverb instead.
    void setReflections(AudioVoice voice, int slot, void* source);
    // A voice's paths around obstacles (baked pathing): its simulated source (null: none). Heard as
    // much as the direct path is blocked.
    void setPathing(AudioVoice voice, void* source);
    void setReflectionGain(float gain);  // 0: no reflections at all
    // Heard from under water: the world's buses (effects, physics, ambience; not music or UI)
    // low-passed, from 20 kHz (0) down to 350 Hz (1), and a little quieter.
    void setUnderwater(float amount);
    void collect();  // voices the mixer finished: their slots become free
    bool playing(AudioVoice voice) const;
    AudioMixerStats stats() const;

    // The device's thread: interleaved stereo.
    void render(float* out, uint32_t frames);

private:
    struct Command {
        enum Type : uint8_t { Play, Stop, Set, Listener, BusGain, Spatializer, Direct, Reflections, ReflectionGain, Underwater, Pathing } type = Play;
        AudioVoice voice = 0;
        const AudioClip* clip = nullptr;
        AudioVoiceParams params;
        float a = 0.0f, b = 0.0f;
        float direct[4] = {};
        void* source = nullptr;
        ListenerPose listener;
    };
    struct Voice {
        AudioVoice id = 0;  // 0: free
        const AudioClip* clip = nullptr;
        AudioVoiceParams p;
        double position = 0.0;  // source frames (clips) or phase in turns (tones)
        float gain = 0.0f;      // applied at the end of the last block (ramped toward the target)
        float fade = 1.0f, fadeStep = 0.0f;  // stopping: fade drops by fadeStep per block
        float panL = 0.0f, panR = 0.0f;      // panning at the end of the last block
        int spatialSlot = -1;
        float direct[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // occlusion, transmission per band
        bool hasDirect = false;
        int reflectionSlot = -1;  // its own reflections (with reflectionSource), else the listener's reverb
        void* reflectionSource = nullptr;
        void* pathSource = nullptr;  // its baked paths (with an HRTF slot)
        float pathGain = 0.0f;       // as of the end of the last block
        float dryGain = 0.0f;     // the gain at the end of the last block without distance (reflections' input)
        bool fresh = true;  // no block yet: start at the target gain and pan (no ramp in)
    };

    void push(const Command& c);
    void mixBlock();
    void finish(Voice& v);
    uint32_t source(Voice& v, float* left, float* right, bool& ended);  // frames written (rest zero)

    // Main thread.
    std::vector<uint16_t> generation_, free_;
    std::vector<bool> live_;
    uint32_t liveCount_ = 0;
    std::vector<Command> overflow_;  // commands the ring had no room for (sent next time)

    // Mixer thread.
    std::vector<Voice> voices_;
    std::vector<int> freeSpatial_;
    Spatializer* spatializer_ = nullptr;
    bool spatializerOn_ = true;
    ListenerPose listener_;
    float busGain_[size_t(AudioBus::Count)] = {1, 1, 1, 1, 1, 1};
    float block_[kAudioFrameSize * 2] = {};  // the last mixed block, interleaved
    uint32_t blockRead_ = kAudioFrameSize;   // frames of it already handed out
    float mixL_[kAudioFrameSize] = {}, mixR_[kAudioFrameSize] = {};
    float srcL_[kAudioFrameSize] = {}, srcR_[kAudioFrameSize] = {};
    float outL_[kAudioFrameSize] = {}, outR_[kAudioFrameSize] = {};
    float mono_[kAudioFrameSize] = {}, dry_[kAudioFrameSize] = {};
    float flatL_[kAudioFrameSize] = {}, flatR_[kAudioFrameSize] = {};  // music and UI (never muffled)
    float reflectionGain_ = 0.0f;
    float underwater_ = 0.0f, underwaterLast_ = 0.0f;  // the target, and as of the last block's end
    float muffle_[4] = {};  // the low-pass's state: two poles per channel

    SpscRing<Command, 1024> commands_;
    SpscRing<AudioVoice, 1024> finished_;
    std::atomic<uint64_t> blocks_{0}, mixNs_{0};
    std::atomic<uint32_t> spatialized_{0}, paths_{0};
    std::atomic<float> peak_{0.0f};
};

}  // namespace fx
