#pragma once

// Decoded sound clips by path: .wav (SDL) and .ogg (stb_vorbis), converted to float at the mixer's
// rate with at most 2 channels (more are mixed down to stereo). A clip lives until clear(), so voices
// playing it keep valid samples; a file that fails to load is reported once.

#include "audio/audio_mixer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace fx {

bool decodeAudioFile(const std::string& path, AudioClip& out, std::string& error);

class ClipCache {
public:
    const AudioClip* load(const std::string& path);  // nullptr if it does not decode
    void clear() { clips_.clear(); }
    size_t bytes() const;

private:
    std::unordered_map<std::string, std::unique_ptr<AudioClip>> clips_;  // null: failed
};

}  // namespace fx
