#pragma once

// Surface sounds: what a surface sounds like when something hits it, scrapes along it or rolls on
// it (physics sounds, engine/audio/physics_audio.h). A material (.fmat) carries them ("surface":
// {...}), an entity may override its material's preset and level (AcousticSurface), and a terrain
// layer names a preset.
//
// Each of the three is a sound (.fsound, or a clip) when one is set, else synthesized from the
// surface's character (audio/procedural_synth.h): its hardness (soft surfaces thud, hard ones
// ring and click), roughness (a scrape's grain), the pitch of its ringing (frequency, Hz), how long
// it rings (decay, s) and how much of a hit is noise rather than tone. Presets match the acoustic
// ones (concrete, wood, metal, glass, sand, ...).

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

struct SurfaceSound {
    std::string impact, scrape, roll;  // .fsound or clip paths ("": synthesized)
    float hardness = 0.5f;   // 0 soft (sand, cloth) .. 1 hard (metal, glass)
    float roughness = 0.5f;  // scrape grain: 0 smooth (glass) .. 1 coarse (gravel)
    float gain = 1.0f, pitch = 1.0f;
    bool synth = true;       // without a sound set: synthesized (else silent)
    float frequency = 600.0f;  // the ringing's base pitch (Hz)
    float decay = 0.12f;       // how long it rings (s)
    float noise = 0.3f;        // the share of a hit that is noise (dirt, gravel) rather than tone (metal, glass)
};

struct SurfacePreset {
    const char* name;
    const char* label;
    SurfaceSound sound;
};
const std::vector<SurfacePreset>& surfacePresets();  // [0]: generic
bool surfacePreset(const std::string& name, SurfaceSound& out);  // (keeps out's sound paths)
uint32_t surfacePresetIndex(const std::string& name);  // 1-based, 0: none
const char* surfacePresetName(uint32_t index);

// {"preset", "impact", "scrape", "roll", "hardness", "roughness", "gain", "pitch", "synth",
// "frequency", "decay", "noise"}: the preset first, then the rest. `preset` becomes "" when a
// character value differs from the preset's.
void readSurfaceJson(const Json& j, std::string& preset, SurfaceSound& s);
void writeSurfaceFields(JsonWriter& w, const std::string& preset, const SurfaceSound& s);  // (into an open object)
void writeSurfacePresets(JsonWriter& w, const char* key);

}  // namespace fx
