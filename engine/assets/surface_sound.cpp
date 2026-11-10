#include "assets/surface_sound.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>

namespace fx {

namespace {

SurfaceSound make(float hardness, float roughness, float frequency, float decay, float noise, float gain = 1.0f)
{
    SurfaceSound s;
    s.hardness = hardness;
    s.roughness = roughness;
    s.frequency = frequency;
    s.decay = decay;
    s.noise = noise;
    s.gain = gain;
    return s;
}

// Characters by ear: hardness, roughness, ring pitch (Hz), ring time (s), noise share, level.
const std::vector<SurfacePreset> kPresets = {
    {"generic", "Generic (hard object)", make(0.55f, 0.45f, 520.0f, 0.10f, 0.35f)},
    {"wood", "Wood", make(0.55f, 0.50f, 380.0f, 0.09f, 0.35f)},
    {"concrete", "Concrete", make(0.80f, 0.60f, 900.0f, 0.04f, 0.70f)},
    {"brick", "Brick", make(0.75f, 0.70f, 800.0f, 0.04f, 0.70f)},
    {"rock", "Rock", make(0.85f, 0.70f, 1100.0f, 0.05f, 0.65f)},
    {"metal", "Metal", make(0.95f, 0.30f, 1250.0f, 0.60f, 0.12f)},
    {"glass", "Glass", make(0.98f, 0.10f, 2400.0f, 0.35f, 0.10f)},
    {"ceramic", "Ceramic", make(0.92f, 0.15f, 1800.0f, 0.18f, 0.15f)},
    {"plastic", "Plastic", make(0.55f, 0.25f, 700.0f, 0.06f, 0.30f)},
    {"gravel", "Gravel", make(0.60f, 1.00f, 1500.0f, 0.02f, 0.95f)},
    {"sand", "Sand", make(0.15f, 0.80f, 300.0f, 0.02f, 0.95f, 0.8f)},
    {"dirt", "Dirt", make(0.25f, 0.70f, 220.0f, 0.03f, 0.90f, 0.9f)},
    {"grass", "Grass", make(0.15f, 0.60f, 260.0f, 0.02f, 0.95f, 0.7f)},
    {"snow", "Snow", make(0.10f, 0.40f, 200.0f, 0.02f, 0.95f, 0.6f)},
    {"carpet", "Carpet", make(0.10f, 0.30f, 180.0f, 0.02f, 0.90f, 0.6f)},
    {"cloth", "Cloth", make(0.08f, 0.20f, 160.0f, 0.02f, 0.90f, 0.5f)},
    {"water", "Water", make(0.20f, 0.20f, 400.0f, 0.05f, 0.85f, 0.8f)},
    {"foliage", "Foliage", make(0.10f, 0.50f, 900.0f, 0.03f, 0.95f, 0.6f)},
};

}  // namespace

const std::vector<SurfacePreset>& surfacePresets() { return kPresets; }

bool surfacePreset(const std::string& name, SurfaceSound& out)
{
    for (const SurfacePreset& p : kPresets)
        if (name == p.name) {
            SurfaceSound s = p.sound;
            s.impact = out.impact;
            s.scrape = out.scrape;
            s.roll = out.roll;
            s.synth = out.synth;
            out = s;
            return true;
        }
    return false;
}

uint32_t surfacePresetIndex(const std::string& name)
{
    for (size_t i = 0; i < kPresets.size(); ++i)
        if (name == kPresets[i].name)
            return uint32_t(i + 1);
    return 0;
}

const char* surfacePresetName(uint32_t index) { return index >= 1 && index <= kPresets.size() ? kPresets[index - 1].name : ""; }

void readSurfaceJson(const Json& j, std::string& preset, SurfaceSound& s)
{
    if (!j.isObject())
        return;
    if (const Json* v = j.get("preset"); v && v->type == Json::Type::String && surfacePreset(v->str, s))
        preset = v->str;
    for (auto [key, out] : {std::pair{"impact", &s.impact}, std::pair{"scrape", &s.scrape}, std::pair{"roll", &s.roll}})
        if (const Json* v = j.get(key); v && v->type == Json::Type::String)
            *out = v->str;
    if (const Json* v = j.get("synth"); v && v->type == Json::Type::Bool)
        s.synth = v->b;
    const auto number = [&](const char* key, float& out, float lo, float hi) {
        if (const Json* v = j.get(key); v && v->type == Json::Type::Number && std::isfinite(v->number))
            out = std::clamp(float(v->number), lo, hi);
    };
    number("gain", s.gain, 0.0f, 4.0f);
    number("pitch", s.pitch, 0.25f, 4.0f);
    const SurfaceSound before = s;
    number("hardness", s.hardness, 0.0f, 1.0f);
    number("roughness", s.roughness, 0.0f, 1.0f);
    number("frequency", s.frequency, 40.0f, 8000.0f);
    number("decay", s.decay, 0.005f, 3.0f);
    number("noise", s.noise, 0.0f, 1.0f);
    if (before.hardness != s.hardness || before.roughness != s.roughness || before.frequency != s.frequency || before.decay != s.decay ||
        before.noise != s.noise)
        preset.clear();  // (a character of its own)
}

void writeSurfaceFields(JsonWriter& w, const std::string& preset, const SurfaceSound& s)
{
    if (!preset.empty())
        w.value("preset", preset);
    w.value("impact", s.impact).value("scrape", s.scrape).value("roll", s.roll).value("synth", s.synth)
        .value("hardness", double(s.hardness)).value("roughness", double(s.roughness)).value("gain", double(s.gain))
        .value("pitch", double(s.pitch)).value("frequency", double(s.frequency)).value("decay", double(s.decay))
        .value("noise", double(s.noise));
}

void writeSurfacePresets(JsonWriter& w, const char* key)
{
    w.beginArray(key);
    for (const SurfacePreset& p : kPresets) {
        w.beginObject().value("value", p.name).value("label", p.label);
        writeSurfaceFields(w, "", p.sound);
        w.endObject();
    }
    w.endArray();
}

}  // namespace fx
