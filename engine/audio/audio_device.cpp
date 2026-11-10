#include "audio/audio_device.h"

#include "audio/audio_mixer.h"
#include "core/log.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cstring>

namespace fx {

namespace {

void writeU32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void writeU16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

// RIFF header for IEEE float stereo at the mixer's rate; sizes patched on close.
void writeWavHeader(std::FILE* f, uint64_t frames)
{
    const uint32_t data = uint32_t(std::min<uint64_t>(frames * 8, 0xffffffffull - 64));
    std::fwrite("RIFF", 1, 4, f);
    writeU32(f, 36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    writeU32(f, 16);
    writeU16(f, 3);  // WAVE_FORMAT_IEEE_FLOAT
    writeU16(f, 2);
    writeU32(f, kAudioSampleRate);
    writeU32(f, kAudioSampleRate * 8);
    writeU16(f, 8);
    writeU16(f, 32);
    std::fwrite("data", 1, 4, f);
    writeU32(f, data);
}

}  // namespace

bool AudioDevice::open(AudioMixer* mixer, bool null, const char* capturePath)
{
    close();
    mixer_ = mixer;
    scratch_.assign(size_t(kAudioFrameSize) * 2 * 8, 0.0f);
    if (capturePath && *capturePath) {
        capture_ = std::fopen(capturePath, "wb");
        if (!capture_)
            FX_WARN("audio: cannot write %s", capturePath);
        else
            writeWavHeader(capture_, 0);
    }
    if (!null) {
        // Ask for blocks of the mixer's size (10.7 ms) instead of the driver's default period.
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            FX_WARN("audio: SDL audio did not start (%s): no sound", SDL_GetError());
        } else {
            subsystem_ = true;
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kAudioSampleRate)};
            stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioDevice::callback, this);
            if (!stream_) {
                FX_WARN("audio: no playback device (%s): no sound", SDL_GetError());
            } else {
                const char* device = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
                name_ = std::string(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?") + ": " + (device ? device : "default");
                SDL_ResumeAudioStreamDevice(stream_);
            }
        }
    }
    if (!stream_)
        name_ = "null";
    return true;
}

void AudioDevice::close()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);  // stops the device thread first
        stream_ = nullptr;
    }
    if (subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        subsystem_ = false;
    }
    if (capture_) {
        std::fseek(capture_, 0, SEEK_SET);
        writeWavHeader(capture_, capturedFrames_);
        std::fclose(capture_);
        capture_ = nullptr;
    }
    capturedFrames_ = 0;
    owed_ = 0.0;
    mixer_ = nullptr;
}

void AudioDevice::produce(uint32_t frames)
{
    const uint32_t chunk = uint32_t(scratch_.size() / 2);
    while (frames > 0) {
        const uint32_t n = std::min(frames, chunk);
        mixer_->render(scratch_.data(), n);
        if (capture_) {
            std::fwrite(scratch_.data(), sizeof(float), size_t(n) * 2, capture_);
            capturedFrames_ += n;
        }
        if (stream_)
            SDL_PutAudioStreamData(stream_, scratch_.data(), int(n * 2 * sizeof(float)));
        frames -= n;
    }
}

void AudioDevice::callback(void* user, SDL_AudioStream*, int additional, int)
{
    AudioDevice& d = *static_cast<AudioDevice*>(user);
    if (additional > 0 && d.mixer_)
        d.produce(uint32_t((additional + 7) / 8));
}

void AudioDevice::pump(double seconds)
{
    if (stream_ || !mixer_)
        return;
    owed_ += seconds * double(kAudioSampleRate);
    // (1e-6: a 60 Hz frame is 800 samples, not 799.999...); at most 1 s after a stall.
    const uint32_t frames = uint32_t(std::min(owed_ + 1e-6, double(kAudioSampleRate)));
    owed_ = std::max(0.0, owed_ - double(frames));
    if (frames)
        produce(frames);
}

}  // namespace fx
