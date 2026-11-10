#pragma once

// Acoustic materials: how a surface treats sound, in Steam Audio's three bands (low < 800 Hz,
// mid 800 Hz - 8 kHz, high > 8 kHz):
//   absorption   - the share of sound energy the surface takes in on a bounce (0 .. 1)
//   scattering   - the share of what bounces off that scatters instead of mirroring (0 .. 1)
//   transmission - the share that passes through it (0 .. 1; per surface crossed)
// A material (.fmat) carries one ("acoustic": {"preset", "absorption", "scattering",
// "transmission"}), an entity may override its material's (AcousticSurface), and a terrain layer
// names a preset. The presets are Steam Audio's own (generic, brick, concrete, ceramic, gravel,
// carpet, glass, plaster, wood, metal, rock) plus outdoor and soft ones.

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

struct AcousticParams {
    float absorption[3] = {0.10f, 0.20f, 0.30f};
    float scattering = 0.05f;
    float transmission[3] = {0.100f, 0.050f, 0.030f};
};

struct AcousticPreset {
    const char* name;
    const char* label;
    AcousticParams params;
};
const std::vector<AcousticPreset>& acousticPresets();  // [0]: generic
// By name ("" or unknown: false, out unchanged). Index: 1-based (0: none), as AcousticSurface keeps it.
bool acousticPreset(const std::string& name, AcousticParams& out);
uint32_t acousticPresetIndex(const std::string& name);
const char* acousticPresetName(uint32_t index);  // "" for 0 or out of range
// A terrain layer's preset when it names none: from its generated look (grass, dirt, rock, sand, snow).
std::string acousticPresetForLook(const std::string& generated);

// {"preset", "absorption" [3], "scattering", "transmission" [3]}: the preset first, then the
// values (clamped to 0 .. 1). `preset` becomes "" when a value differs from the preset's.
void readAcousticJson(const Json& j, std::string& preset, AcousticParams& p);
void writeAcousticJson(JsonWriter& w, const char* key, const std::string& preset, const AcousticParams& p);
void writeAcousticFields(JsonWriter& w, const std::string& preset, const AcousticParams& p);  // (into an open object)
// The presets for the editor: [{"value", "label", "absorption", "scattering", "transmission"}].
void writeAcousticPresets(JsonWriter& w, const char* key);

bool sameAcoustic(const AcousticParams& a, const AcousticParams& b);

}  // namespace fx
