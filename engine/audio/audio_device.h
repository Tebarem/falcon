#pragma once

// Where the mixer's output goes: the default playback device (SDL3 audio, WASAPI on Windows; SDL's
// own thread pulls blocks from the mixer), or the null device, mixed on the main thread from the
// frame clock: --hidden and --audio-null (tests; 800 samples per frame at a fixed 60 Hz, so a run
// gives the same samples every time). Either can also write what it plays to a 32-bit float stereo
// WAV (--audio-capture).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace fx {

class AudioMixer;

class AudioDevice {
public:
    ~AudioDevice() { close(); }
    // null: no playback device (also the fallback when none opens). capturePath: optional.
    bool open(AudioMixer* mixer, bool null, const char* capturePath);
    void close();
    // Null device: mixes `seconds` worth of samples (carrying the fraction to the next call).
    void pump(double seconds);
    bool playing() const { return stream_ != nullptr; }  // a real device
    const std::string& name() const { return name_; }

private:
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void produce(uint32_t frames);  // mix, capture, and (real device) queue

    AudioMixer* mixer_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    bool subsystem_ = false;
    std::string name_;
    std::vector<float> scratch_;
    double owed_ = 0.0;  // null device: samples due but not yet mixed
    std::FILE* capture_ = nullptr;
    uint64_t capturedFrames_ = 0;
};

}  // namespace fx
