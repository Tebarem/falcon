#pragma once

// Water materials (.fwater): how a body of water looks and behaves, shared by every body that
// uses it (each body may override any field). One parameter set covers still pools, lakes,
// rivers and the ocean:
//
//   {"format": 1, "name": "Oasis", "preset": "oasis", "deepColor": [r, g, b], "clarity": 6, ...}
//
// Every field is described once, in waterFields(): its JSON key, kind, range, default, group and
// label. Reading, writing, per-body overrides and the editor's controls all go through that
// table, so a new parameter is one line there (plus its use in the shaders). Missing keys keep
// the preset's value ("preset": a built-in to start from, else the plain defaults). Colours are
// linear.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

enum class WaveMode : uint32_t { Still, Gerstner, Ocean };

// The parameters (plain data: the field table addresses them by offset).
struct WaterParams {
    // Colour and clarity.
    float deepColor[3] = {0.02f, 0.10f, 0.14f};      // what deep water looks like (in-scattered light)
    float shallowColor[3] = {0.80f, 0.96f, 0.92f};   // the tint light takes travelling through it
    float clarity = 8.0f;          // metres until the bottom fades out (light down to a tenth)
    float scattering = 1.0f;       // how much light the water itself gives back (murky water: more)
    float murk = 0.15f;            // blur of what is seen through it, with depth (0 clear .. 1 milky)
    float refraction = 0.5f;       // how much the surface bends what is under it
    // Surface.
    float roughness = 0.04f;       // of the surface's reflections
    float reflection = 1.0f;       // reflection strength
    float normalStrength = 1.0f;   // small ripples (detail normals)
    float detailScale = 2.0f;      // metres per detail ripple pattern
    float detailSpeed = 0.4f;      // m/s the detail drifts
    // Film: scum, algae, pollen floating on top.
    float filmColor[3] = {0.12f, 0.16f, 0.05f};
    float film = 0.0f;             // coverage 0 .. 1
    float filmScale = 6.0f;        // metres per patch
    // Waves.
    WaveMode waves = WaveMode::Gerstner;
    float windSpeed = 3.0f;        // m/s: wave height and length follow it
    float windDirection = 30.0f;   // degrees (0: toward +x, 90: toward +z)
    float waveHeight = 1.0f;       // multiplies the wind's wave height
    float choppiness = 0.6f;       // sharp crests (0: round swell)
    float waveSpread = 0.5f;       // how far wave directions stray from the wind (0 .. 1)
    float shoreFade = 4.0f;        // metres of depth over which waves calm toward the shore
    // Flow (rivers, currents).
    float flowSpeed = 0.0f;        // m/s
    // Foam.
    float foamColor[3] = {0.85f, 0.88f, 0.88f};
    float shoreFoam = 0.15f;       // foam where the water is shallower than this (m)
    float crestFoam = 0.5f;        // foam on breaking crests (0 .. 1)
    float foamScale = 1.5f;        // metres per foam pattern
    float edgeFade = 0.25f;        // metres of depth over which the edge fades into the shore
    // Caustics and underwater.
    float caustics = 1.0f;         // light patterns on what is under the water
    float causticScale = 2.0f;     // metres per caustic cell
    float causticDepth = 6.0f;     // metres below the surface they fade over
    float fogColor[3] = {0.02f, 0.09f, 0.12f};  // underwater, looking through the water
    float fogDensity = 0.12f;      // underwater: per metre
    // Physics.
    float density = 1000.0f;       // kg/m^3 (buoyancy)
    float drag = 1.0f;             // how strongly water slows things moving through it
    float current = 1.0f;          // how strongly the flow carries floating things
    // Ripples (the live simulation).
    bool ripples = true;
    float rippleStrength = 1.0f;
    float rippleDamping = 0.5f;    // how fast ripples die out (0 .. 1)
    // Sound (audio/water_audio.h): a sound or clip each (project paths; empty: synthesized).
    std::string splashSound;       // something falls in (scaled by how big and fast)
    float splashVolume = 1.0f;
    std::string sloshSound;        // a loop while things move at the surface
    float sloshVolume = 1.0f;
    float underwaterMuffle = 1.0f;   // under it: how muffled the world is (0 .. 1)
    std::string underwaterSound;     // a loop heard under it (empty: a low synthesized rumble)
    float underwaterVolume = 0.5f;
};

struct WaterMaterial : WaterParams {
    std::string name = "Water";
    std::string preset;  // the built-in it started from ("" none)
};

// One field of WaterMaterial, for every generic use (JSON, overrides, the editor's controls).
struct WaterField {
    enum class Kind : uint8_t { Number, Color, Bool, Choice, Sound };  // Sound: a project path (a std::string)
    const char* key;
    Kind kind;
    size_t offset;          // into WaterParams
    float min, max, step;   // Number
    const char* group;      // the editor's section
    const char* label;
    const char* tip;
    const char* const* choices = nullptr;  // Choice: names, by value
    uint32_t choiceCount = 0;
};
const std::vector<WaterField>& waterFields();
const char* waveModeName(WaveMode m);

// The built-in materials ("tropical", "ocean", "river", "swamp", "pool", "oasis"), and the
// names and display names of all of them.
bool waterPreset(const std::string& name, WaterMaterial& out);
const std::vector<std::pair<std::string, std::string>>& waterPresets();

// JSON fields onto a material (unknown keys ignored, numbers clamped to their range). The
// "preset" key, when present, starts from that preset first.
void readWaterFields(const Json& j, WaterMaterial& m);
// Every field (the format, name and preset first).
void writeWaterMaterial(JsonWriter& w, const WaterMaterial& m);
// Only the fields that differ from `base` (a body's overrides), as an object under `key`.
void writeWaterOverrides(JsonWriter& w, const char* key, const WaterMaterial& m, const WaterMaterial& base);

bool readWaterAsset(const std::string& path, WaterMaterial& out, std::string& error);
bool writeWaterAsset(const std::string& path, const WaterMaterial& m, std::string& error);
// The field table as JSON for the editor (key, kind, range, group, label, tip, choices).
void writeWaterSchema(JsonWriter& w, const char* key);

}  // namespace fx
