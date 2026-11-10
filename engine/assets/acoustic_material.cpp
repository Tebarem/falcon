#include "assets/acoustic_material.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

// Steam Audio's presets (its Unity / Unreal plugins' material list), then ours for terrain, water,
// leaves and soft furnishings.
const std::vector<AcousticPreset> kPresets = {
    {"generic", "Generic", {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}}},
    {"brick", "Brick", {{0.03f, 0.04f, 0.07f}, 0.05f, {0.015f, 0.015f, 0.015f}}},
    {"concrete", "Concrete", {{0.05f, 0.07f, 0.08f}, 0.05f, {0.015f, 0.002f, 0.001f}}},
    {"ceramic", "Ceramic", {{0.01f, 0.02f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}},
    {"gravel", "Gravel", {{0.60f, 0.70f, 0.80f}, 0.05f, {0.031f, 0.012f, 0.008f}}},
    {"carpet", "Carpet", {{0.24f, 0.69f, 0.73f}, 0.05f, {0.020f, 0.005f, 0.003f}}},
    {"glass", "Glass", {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}},
    {"plaster", "Plaster", {{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}}},
    {"wood", "Wood", {{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}}},
    {"metal", "Metal", {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}}},
    {"rock", "Rock", {{0.13f, 0.20f, 0.24f}, 0.05f, {0.015f, 0.002f, 0.001f}}},
    {"sand", "Sand", {{0.40f, 0.55f, 0.65f}, 0.60f, {0.020f, 0.005f, 0.002f}}},
    {"grass", "Grass", {{0.20f, 0.45f, 0.60f}, 0.60f, {0.050f, 0.020f, 0.010f}}},
    {"dirt", "Dirt", {{0.15f, 0.35f, 0.50f}, 0.40f, {0.020f, 0.005f, 0.002f}}},
    {"snow", "Snow", {{0.45f, 0.75f, 0.90f}, 0.30f, {0.020f, 0.010f, 0.005f}}},
    {"water", "Water", {{0.01f, 0.02f, 0.03f}, 0.05f, {0.100f, 0.050f, 0.030f}}},
    {"foliage", "Foliage", {{0.10f, 0.25f, 0.40f}, 0.80f, {0.800f, 0.600f, 0.400f}}},
    {"cloth", "Cloth", {{0.15f, 0.45f, 0.65f}, 0.30f, {0.300f, 0.150f, 0.100f}}},
    {"plastic", "Plastic", {{0.05f, 0.08f, 0.10f}, 0.10f, {0.100f, 0.050f, 0.030f}}},
};

void readBands(const Json& j, const char* key, float out[3])
{
    if (const Json* a = j.get(key); a && a->array.size() == 3)
        for (int k = 0; k < 3; ++k)
            if (std::isfinite(a->array[size_t(k)].number))
                out[k] = std::clamp(float(a->array[size_t(k)].number), 0.0f, 1.0f);
}

}  // namespace

const std::vector<AcousticPreset>& acousticPresets() { return kPresets; }

bool acousticPreset(const std::string& name, AcousticParams& out)
{
    for (const AcousticPreset& p : kPresets)
        if (name == p.name) {
            out = p.params;
            return true;
        }
    return false;
}

uint32_t acousticPresetIndex(const std::string& name)
{
    for (size_t i = 0; i < kPresets.size(); ++i)
        if (name == kPresets[i].name)
            return uint32_t(i + 1);
    return 0;
}

const char* acousticPresetName(uint32_t index) { return index >= 1 && index <= kPresets.size() ? kPresets[index - 1].name : ""; }

std::string acousticPresetForLook(const std::string& generated)
{
    for (const char* look : {"grass", "dirt", "rock", "sand", "snow"})
        if (generated == look)
            return look;
    return "generic";
}

bool sameAcoustic(const AcousticParams& a, const AcousticParams& b) { return std::memcmp(&a, &b, sizeof(AcousticParams)) == 0; }

void readAcousticJson(const Json& j, std::string& preset, AcousticParams& p)
{
    if (!j.isObject())
        return;
    if (const Json* v = j.get("preset"); v && v->type == Json::Type::String && acousticPreset(v->str, p))
        preset = v->str;
    const AcousticParams before = p;
    readBands(j, "absorption", p.absorption);
    readBands(j, "transmission", p.transmission);
    if (const Json* v = j.get("scattering"); v && std::isfinite(v->number))
        p.scattering = std::clamp(float(v->number), 0.0f, 1.0f);
    if (!sameAcoustic(before, p))
        preset.clear();  // (values of its own: no longer the preset)
}

void writeAcousticJson(JsonWriter& w, const char* key, const std::string& preset, const AcousticParams& p)
{
    w.beginObject(key);
    writeAcousticFields(w, preset, p);
    w.endObject();
}

void writeAcousticFields(JsonWriter& w, const std::string& preset, const AcousticParams& p)
{
    if (!preset.empty())
        w.value("preset", preset);
    w.beginArray("absorption");
    for (float v : p.absorption)
        w.value(nullptr, double(v));
    w.endArray().value("scattering", double(p.scattering)).beginArray("transmission");
    for (float v : p.transmission)
        w.value(nullptr, double(v));
    w.endArray();
}

void writeAcousticPresets(JsonWriter& w, const char* key)
{
    w.beginArray(key);
    for (const AcousticPreset& p : kPresets) {
        w.beginObject().value("value", p.name).value("label", p.label).beginArray("absorption");
        for (float v : p.params.absorption)
            w.value(nullptr, double(v));
        w.endArray().value("scattering", double(p.params.scattering)).beginArray("transmission");
        for (float v : p.params.transmission)
            w.value(nullptr, double(v));
        w.endArray().endObject();
    }
    w.endArray();
}

}  // namespace fx
