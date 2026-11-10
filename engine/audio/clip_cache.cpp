#include "audio/clip_cache.h"

#include "core/log.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_vorbis.c>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
// stb_vorbis leaves these single-letter macros defined.
#undef L
#undef C
#undef R

namespace fx {

namespace {

bool endsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
            return false;
    return true;
}

// Any SDL format, rate and channel count to float at the mixer's rate, mono or stereo.
bool convert(const SDL_AudioSpec& spec, const uint8_t* data, int bytes, AudioClip& out, std::string& error)
{
    const SDL_AudioSpec dst{SDL_AUDIO_F32, std::min(spec.channels, 2), int(kAudioSampleRate)};
    uint8_t* converted = nullptr;
    int convertedBytes = 0;
    if (!SDL_ConvertAudioSamples(&spec, data, bytes, &dst, &converted, &convertedBytes)) {
        error = SDL_GetError();
        return false;
    }
    out.channels = uint32_t(dst.channels);
    out.frames = uint32_t(convertedBytes / int(sizeof(float) * out.channels));
    out.samples.assign(reinterpret_cast<const float*>(converted), reinterpret_cast<const float*>(converted) + size_t(out.frames) * out.channels);
    SDL_free(converted);
    return true;
}

}  // namespace

bool decodeAudioFile(const std::string& path, AudioClip& out, std::string& error)
{
    out = AudioClip{};
    out.path = path;
    if (endsWith(path, ".ogg")) {
        int channels = 0, rate = 0;
        short* pcm = nullptr;
        const int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &pcm);
        if (frames < 0 || !pcm || channels <= 0) {
            error = "not a readable Ogg Vorbis file";
            return false;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, channels, rate};
        const bool ok = convert(spec, reinterpret_cast<const uint8_t*>(pcm), frames * channels * int(sizeof(short)), out, error);
        free(pcm);
        return ok;
    }
    if (endsWith(path, ".wav")) {
        SDL_AudioSpec spec{};
        uint8_t* data = nullptr;
        uint32_t bytes = 0;
        if (!SDL_LoadWAV(path.c_str(), &spec, &data, &bytes)) {
            error = SDL_GetError();
            return false;
        }
        const bool ok = convert(spec, data, int(bytes), out, error);
        SDL_free(data);
        return ok;
    }
    error = "not a .wav or .ogg file";
    return false;
}

const AudioClip* ClipCache::load(const std::string& path)
{
    auto it = clips_.find(path);
    if (it != clips_.end())
        return it->second.get();
    auto clip = std::make_unique<AudioClip>();
    std::string error;
    if (!decodeAudioFile(path, *clip, error)) {
        FX_WARN("audio: %s: %s", path.c_str(), error.c_str());
        clip.reset();
    }
    return (clips_[path] = std::move(clip)).get();
}

size_t ClipCache::bytes() const
{
    size_t n = 0;
    for (const auto& [path, clip] : clips_)
        if (clip)
            n += clip->samples.size() * sizeof(float);
    return n;
}

}  // namespace fx
