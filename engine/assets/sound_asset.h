#pragma once

// Sound assets (.fsound): what plays when something asks for "the footstep" or "the door": one or
// more clips (.wav / .ogg, one picked each time), with level and pitch (and how much each varies),
// whether it is placed in the world and how it fades with distance, and its bus:
//
//   {"format": 1, "name": "Footstep", "preset": "footstep", "clips": ["Assets/Audio/step1.wav", ...],
//    "volume": -3, "volumeRandom": 2, "pitch": 0, "pitchRandom": 1.5, "spatial": true, ...}
//
// As for water materials (water_asset.h), every field is described once, in soundFields(): reading,
// writing and the editor's controls all go through that table. Missing keys keep the preset's
// value. Paths are relative to the project.
//
// Sound ids: an AudioSource names its sound by a number (components hold no strings); soundId()
// gives a path its number for this run, soundPath() the path back (the edits save paths).

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

struct SoundParams {
    float volume = 0.0f;        // dB
    float volumeRandom = 0.0f;  // ± dB, each time it plays
    float pitch = 0.0f;         // semitones
    float pitchRandom = 0.0f;   // ± semitones
    bool vary = true;           // varies as the project's Sound variation does (off: exactly as recorded, no randomness)
    bool loop = false;
    bool spatial = true;        // placed in the world (else 2D)
    float minDistance = 1.0f;   // full volume within (m)
    float maxDistance = 50.0f;  // silent beyond (m)
    uint32_t rolloff = 0;       // 0 inverse distance, 1 linear
    bool airAbsorption = true;  // far away, the highs fade first
    bool occlusion = true;      // quieter behind what stands between it and the listener
    bool transmission = true;   // heard through that, as its acoustic materials let it
    bool reflections = true;    // echoes and reverb from the scene
    uint32_t bus = 0;           // 0 sfx, 1 physics, 2 ambience, 3 music, 4 ui
    uint32_t selection = 0;     // which clip: 0 random, 1 shuffle (no repeats until all played), 2 in order
    float maxInstances = 0.0f;  // playing at once (0: any number; the oldest stops)
    float cooldown = 0.0f;      // ms before it plays again (0: none)
};

struct SoundAsset : SoundParams {
    std::string name = "Sound";
    std::string preset;
    std::vector<std::string> clips;
};

struct SoundField {
    enum class Kind : uint8_t { Number, Bool, Choice };
    const char* key;
    Kind kind;
    size_t offset;
    float min, max, step;
    const char* group;
    const char* label;
    const char* tip;
    const char* const* choices = nullptr;
    uint32_t choiceCount = 0;
};
const std::vector<SoundField>& soundFields();
extern const char* const kSoundBuses[5];

// Built-ins: "click" (UI), "footstep", "ambience", "impact", "music".
bool soundPreset(const std::string& name, SoundAsset& out);
const std::vector<std::pair<std::string, std::string>>& soundPresets();

void readSoundFields(const Json& j, SoundAsset& s);  // ("preset" first, then the fields and "clips")
void writeSoundAsset(JsonWriter& w, const SoundAsset& s);
bool readSoundAsset(const std::string& path, SoundAsset& out, std::string& error);
bool writeSoundAsset(const std::string& path, const SoundAsset& s, std::string& error);
void writeSoundSchema(JsonWriter& w, const char* key);  // the field table, as writeWaterSchema

bool isSoundAssetPath(const std::string& path);  // .fsound
bool isAudioClipPath(const std::string& path);   // .wav / .ogg

uint32_t soundId(const std::string& path);  // 0 for ""
const std::string& soundPath(uint32_t id);  // "" for 0 or an unknown id

}  // namespace fx
