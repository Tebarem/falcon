#include "assets/water_asset.h"

#include "core/json.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fx {

namespace {

const char* const kWaveModes[] = {"still", "gerstner", "ocean"};

using K = WaterField::Kind;
#define FX_WF(key, kind, lo, hi, step, group, label, tip) \
    WaterField{#key, kind, offsetof(WaterParams, key), lo, hi, step, group, label, tip}

const std::vector<WaterField> kFields = {
    FX_WF(deepColor, K::Color, 0, 1, 0, "Colour", "Deep colour", "What deep water looks like: the light it gives back"),
    FX_WF(shallowColor, K::Color, 0, 1, 0, "Colour", "Shallow tint", "The tint light takes travelling through the water"),
    FX_WF(clarity, K::Number, 0.05f, 100, 0.05f, "Colour", "Clarity (m)", "How deep you can see: metres until the bottom fades out"),
    FX_WF(scattering, K::Number, 0, 4, 0.01f, "Colour", "Scattering", "How much light the water itself gives back (murky, milky water: more)"),
    FX_WF(murk, K::Number, 0, 1, 0.01f, "Colour", "Murk", "Blur of what is seen through the water, growing with depth"),
    FX_WF(refraction, K::Number, 0, 2, 0.01f, "Colour", "Refraction", "How much the surface bends what is under it"),
    FX_WF(roughness, K::Number, 0, 1, 0.005f, "Surface", "Roughness", "Sharp mirror (0) to blurry, broken reflections"),
    FX_WF(reflection, K::Number, 0, 2, 0.01f, "Surface", "Reflection", "Reflection strength"),
    FX_WF(normalStrength, K::Number, 0, 3, 0.01f, "Surface", "Ripple strength", "The small ripples on the surface"),
    FX_WF(detailScale, K::Number, 0.1f, 50, 0.1f, "Surface", "Ripple size (m)", "Metres per small ripple pattern"),
    FX_WF(detailSpeed, K::Number, 0, 5, 0.01f, "Surface", "Ripple speed", "How fast the small ripples drift (m/s)"),
    FX_WF(filmColor, K::Color, 0, 1, 0, "Film", "Film colour", "Scum, algae or pollen floating on top"),
    FX_WF(film, K::Number, 0, 1, 0.01f, "Film", "Coverage", "How much of the surface the film covers (0: clean water)"),
    FX_WF(filmScale, K::Number, 0.2f, 100, 0.1f, "Film", "Patch size (m)", "Metres per film patch"),
    WaterField{"waves", K::Choice, offsetof(WaterParams, waves), 0, 2, 1, "Waves", "Waves",
               "Still water, a few rolling waves (Gerstner), or an ocean (FFT spectrum)", kWaveModes, 3},
    FX_WF(windSpeed, K::Number, 0, 30, 0.1f, "Waves", "Wind (m/s)", "Wave height and length follow the wind"),
    FX_WF(windDirection, K::Number, -180, 180, 1, "Waves", "Wind direction", "Degrees: 0 blows toward +X, 90 toward +Z"),
    FX_WF(waveHeight, K::Number, 0, 4, 0.01f, "Waves", "Height", "Multiplies the wind's wave height"),
    FX_WF(choppiness, K::Number, 0, 1.5f, 0.01f, "Waves", "Choppiness", "Sharp, pointed crests (0: round swell)"),
    FX_WF(waveSpread, K::Number, 0, 1, 0.01f, "Waves", "Spread", "How far wave directions stray from the wind"),
    FX_WF(shoreFade, K::Number, 0, 50, 0.1f, "Waves", "Shore calm (m)", "Waves calm over this much depth toward the shore"),
    FX_WF(flowSpeed, K::Number, -10, 10, 0.05f, "Flow", "Flow speed", "Rivers and currents: how fast the water moves (m/s)"),
    FX_WF(foamColor, K::Color, 0, 1, 0, "Foam", "Foam colour", ""),
    FX_WF(shoreFoam, K::Number, 0, 3, 0.01f, "Foam", "Shore foam (m)", "Foam where the water is shallower than this"),
    FX_WF(crestFoam, K::Number, 0, 1, 0.01f, "Foam", "Crest foam", "Foam where waves break"),
    FX_WF(foamScale, K::Number, 0.1f, 20, 0.05f, "Foam", "Foam size (m)", "Metres per foam pattern"),
    FX_WF(edgeFade, K::Number, 0, 5, 0.01f, "Foam", "Edge softness (m)", "Depth over which the edge fades into the shore"),
    FX_WF(caustics, K::Number, 0, 4, 0.01f, "Underwater", "Caustics", "Light patterns on what is under the water"),
    FX_WF(causticScale, K::Number, 0.1f, 20, 0.05f, "Underwater", "Caustic size (m)", ""),
    FX_WF(causticDepth, K::Number, 0.1f, 100, 0.1f, "Underwater", "Caustic depth (m)", "Depth over which the caustics fade"),
    FX_WF(fogColor, K::Color, 0, 1, 0, "Underwater", "Fog colour", "Looking through the water from inside it"),
    FX_WF(fogDensity, K::Number, 0, 4, 0.005f, "Underwater", "Fog density", "Per metre, from inside"),
    FX_WF(density, K::Number, 100, 3000, 10, "Physics", "Density (kg/m³)", "Buoyancy: fresh water 1000, sea water 1025"),
    FX_WF(drag, K::Number, 0, 10, 0.01f, "Physics", "Drag", "How strongly the water slows things moving through it"),
    FX_WF(current, K::Number, 0, 10, 0.01f, "Physics", "Current", "How strongly the flow carries floating things"),
    FX_WF(ripples, K::Bool, 0, 1, 1, "Physics", "Ripples", "Live ripples and wakes from things moving in the water"),
    FX_WF(rippleStrength, K::Number, 0, 4, 0.01f, "Physics", "Ripple height", ""),
    FX_WF(rippleDamping, K::Number, 0, 1, 0.01f, "Physics", "Ripple fade", "How fast ripples die out"),
    FX_WF(splashSound, K::Sound, 0, 0, 0, "Sound", "Splash", "Something falls in (a .fsound with variations is best; none: synthesized by size)"),
    FX_WF(splashVolume, K::Number, 0, 4, 0.01f, "Sound", "Splash volume", ""),
    FX_WF(sloshSound, K::Sound, 0, 0, 0, "Sound", "Slosh", "A loop while things move at the surface (none: synthesized)"),
    FX_WF(sloshVolume, K::Number, 0, 4, 0.01f, "Sound", "Slosh volume", ""),
    FX_WF(underwaterMuffle, K::Number, 0, 1, 0.01f, "Sound", "Muffle", "Heard from under it: how muffled the world is, and sounds above the surface more so"),
    FX_WF(underwaterSound, K::Sound, 0, 0, 0, "Sound", "Underwater", "A loop heard under it (none: a low synthesized rumble)"),
    FX_WF(underwaterVolume, K::Number, 0, 4, 0.01f, "Sound", "Under vol.", "The underwater loop's level"),
};
#undef FX_WF

float* number(WaterParams& m, const WaterField& f) { return reinterpret_cast<float*>(reinterpret_cast<char*>(&m) + f.offset); }
const float* number(const WaterParams& m, const WaterField& f)
{
    return reinterpret_cast<const float*>(reinterpret_cast<const char*>(&m) + f.offset);
}
bool* flag(WaterParams& m, const WaterField& f) { return reinterpret_cast<bool*>(reinterpret_cast<char*>(&m) + f.offset); }
bool flagOf(const WaterParams& m, const WaterField& f) { return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(&m) + f.offset); }
uint32_t* choice(WaterParams& m, const WaterField& f) { return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&m) + f.offset); }
uint32_t choiceOf(const WaterParams& m, const WaterField& f)
{
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(&m) + f.offset);
}
std::string* text(WaterParams& m, const WaterField& f) { return reinterpret_cast<std::string*>(reinterpret_cast<char*>(&m) + f.offset); }
const std::string& textOf(const WaterParams& m, const WaterField& f)
{
    return *reinterpret_cast<const std::string*>(reinterpret_cast<const char*>(&m) + f.offset);
}

void writeField(JsonWriter& w, const WaterParams& m, const WaterField& f)
{
    switch (f.kind) {
    case K::Number: w.value(f.key, double(*number(m, f))); break;
    case K::Color: {
        const float* c = number(m, f);
        w.beginArray(f.key).value(nullptr, double(c[0])).value(nullptr, double(c[1])).value(nullptr, double(c[2])).endArray();
        break;
    }
    case K::Bool: w.value(f.key, flagOf(m, f)); break;
    case K::Choice: w.value(f.key, f.choices[std::min(choiceOf(m, f), f.choiceCount - 1)]); break;
    case K::Sound: w.value(f.key, textOf(m, f)); break;
    }
}

bool sameField(const WaterParams& a, const WaterParams& b, const WaterField& f)
{
    switch (f.kind) {
    case K::Number: return *number(a, f) == *number(b, f);
    case K::Color: return std::memcmp(number(a, f), number(b, f), 3 * sizeof(float)) == 0;
    case K::Bool: return flagOf(a, f) == flagOf(b, f);
    case K::Choice: return choiceOf(a, f) == choiceOf(b, f);
    case K::Sound: return textOf(a, f) == textOf(b, f);
    }
    return true;
}

struct Preset {
    const char* name;
    const char* display;
    const char* json;
};

// The presets as the JSON a .fwater would hold: fields left out keep the defaults.
const Preset kPresets[] = {
    {"lake", "Lake", R"({"name": "Lake"})"},
    {"tropical", "Tropical lagoon", R"({"name": "Tropical lagoon",
        "deepColor": [0.0, 0.16, 0.20], "shallowColor": [0.72, 1.0, 0.94], "clarity": 16, "scattering": 1.2, "murk": 0.05,
        "roughness": 0.03, "windSpeed": 4, "waveHeight": 0.6, "choppiness": 0.5, "shoreFoam": 0.25, "caustics": 1.3,
        "fogColor": [0.02, 0.14, 0.16], "fogDensity": 0.05})"},
    {"ocean", "Ocean", R"({"name": "Ocean", "waves": "ocean",
        "deepColor": [0.004, 0.028, 0.055], "shallowColor": [0.62, 0.92, 0.86], "clarity": 12, "scattering": 0.7, "murk": 0.1,
        "roughness": 0.05, "windSpeed": 9, "choppiness": 0.9, "waveSpread": 0.6, "shoreFade": 12, "crestFoam": 0.7,
        "shoreFoam": 0.6, "density": 1025, "fogColor": [0.01, 0.05, 0.08], "fogDensity": 0.08})"},
    {"river", "Muddy river", R"({"name": "Muddy river",
        "deepColor": [0.09, 0.065, 0.03], "shallowColor": [0.55, 0.45, 0.25], "clarity": 0.8, "scattering": 1.6, "murk": 0.6,
        "roughness": 0.06, "windSpeed": 2, "waveHeight": 0.2, "choppiness": 0.2, "flowSpeed": 1.5, "shoreFoam": 0.2,
        "caustics": 0.1, "fogColor": [0.08, 0.06, 0.03], "fogDensity": 1.2})"},
    {"swamp", "Swamp", R"({"name": "Swamp", "waves": "still",
        "deepColor": [0.03, 0.04, 0.01], "shallowColor": [0.35, 0.40, 0.15], "clarity": 0.6, "scattering": 0.8, "murk": 0.8,
        "roughness": 0.12, "normalStrength": 0.4, "detailSpeed": 0.05, "film": 0.55, "filmColor": [0.10, 0.14, 0.03],
        "filmScale": 4, "shoreFoam": 0, "caustics": 0, "fogColor": [0.03, 0.04, 0.01], "fogDensity": 1.5, "drag": 2.5,
        "splashVolume": 0.7})"},
    {"pool", "Pool", R"({"name": "Pool", "waves": "still",
        "deepColor": [0.0, 0.20, 0.35], "shallowColor": [0.86, 1.0, 1.0], "clarity": 30, "scattering": 0.6, "murk": 0.0,
        "roughness": 0.02, "normalStrength": 0.5, "detailScale": 1.2, "detailSpeed": 0.2, "shoreFoam": 0, "edgeFade": 0.02,
        "caustics": 1.5, "causticScale": 0.8, "fogColor": [0.05, 0.25, 0.35], "fogDensity": 0.03})"},
    {"oasis", "Oasis", R"({"name": "Oasis",
        "deepColor": [0.0, 0.12, 0.13], "shallowColor": [0.70, 0.95, 0.80], "clarity": 6, "scattering": 1.0, "murk": 0.2,
        "windSpeed": 2, "waveHeight": 0.3, "choppiness": 0.3, "film": 0.08, "filmColor": [0.10, 0.16, 0.04],
        "shoreFoam": 0.1, "caustics": 1.0, "fogColor": [0.02, 0.10, 0.09], "fogDensity": 0.15})"},
};

}  // namespace

const std::vector<WaterField>& waterFields() { return kFields; }

const char* waveModeName(WaveMode m) { return kWaveModes[std::min(uint32_t(m), 2u)]; }

bool waterPreset(const std::string& name, WaterMaterial& out)
{
    for (const Preset& p : kPresets)
        if (name == p.name) {
            out = {};
            readWaterFields(Json::parse(p.json), out);
            out.preset = p.name;
            return true;
        }
    return false;
}

const std::vector<std::pair<std::string, std::string>>& waterPresets()
{
    static const std::vector<std::pair<std::string, std::string>> names = [] {
        std::vector<std::pair<std::string, std::string>> v;
        for (const Preset& p : kPresets)
            v.emplace_back(p.name, p.display);
        return v;
    }();
    return names;
}

void readWaterFields(const Json& j, WaterMaterial& m)
{
    if (!j.isObject())
        return;
    if (const Json* p = j.get("preset"); p && p->type == Json::Type::String && p->str != m.preset) {
        const std::string name = m.name;
        if (waterPreset(p->str, m) && !j.get("name"))
            m.name = name.empty() ? m.name : name;
    }
    if (const Json* n = j.get("name"); n && n->type == Json::Type::String)
        m.name = n->str;
    for (const WaterField& f : kFields) {
        const Json* v = j.get(f.key);
        if (!v)
            continue;
        switch (f.kind) {
        case K::Number:
            if (v->type == Json::Type::Number && std::isfinite(v->number))
                *number(m, f) = std::clamp(float(v->number), f.min, f.max);
            break;
        case K::Color:
            for (size_t i = 0; i < 3 && i < v->array.size(); ++i)
                if (v->array[i].type == Json::Type::Number && std::isfinite(v->array[i].number))
                    number(m, f)[i] = std::clamp(float(v->array[i].number), 0.0f, 1.0f);
            break;
        case K::Bool:
            if (v->type == Json::Type::Bool)
                *flag(m, f) = v->b;
            break;
        case K::Choice:
            for (uint32_t c = 0; c < f.choiceCount; ++c)
                if ((v->type == Json::Type::String && v->str == f.choices[c]) ||
                    (v->type == Json::Type::Number && uint32_t(v->number) == c))
                    *choice(m, f) = c;
            break;
        case K::Sound:
            if (v->type == Json::Type::String)
                *text(m, f) = v->str;
            break;
        }
    }
}

void writeWaterMaterial(JsonWriter& w, const WaterMaterial& m)
{
    w.value("format", 1u).value("name", m.name);
    if (!m.preset.empty())
        w.value("preset", m.preset);
    for (const WaterField& f : kFields)
        writeField(w, m, f);
}

void writeWaterOverrides(JsonWriter& w, const char* key, const WaterMaterial& m, const WaterMaterial& base)
{
    w.beginObject(key);
    for (const WaterField& f : kFields)
        if (!sameField(m, base, f))
            writeField(w, m, f);
    w.endObject();
}

bool readWaterAsset(const std::string& path, WaterMaterial& out, std::string& error)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text) {
        error = "Cannot read " + path;
        return false;
    }
    try {
        const Json j = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        out = {};
        out.name = std::filesystem::path(path).stem().string();
        readWaterFields(j, out);
    } catch (const std::exception& e) {
        error = path + " is malformed (" + e.what() + ")";
        return false;
    }
    return true;
}

bool writeWaterAsset(const std::string& path, const WaterMaterial& m, std::string& error)
{
    JsonWriter w;
    w.beginObject();
    writeWaterMaterial(w, m);
    w.endObject();
    const std::string text = Json::parse(w.str()).dump(2) + "\n";
    const std::string tmp = path + ".tmp";
    if (!SDL_SaveFile(tmp.c_str(), text.data(), text.size())) {
        error = "Cannot write " + path;
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = "Cannot write " + path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

void writeWaterSchema(JsonWriter& w, const char* key)
{
    w.beginArray(key);
    for (const WaterField& f : kFields) {
        static const char* const kKinds[] = {"number", "color", "bool", "choice", "sound"};
        w.beginObject().value("key", f.key).value("kind", kKinds[uint32_t(f.kind)]).value("group", f.group).value("label", f.label);
        if (f.tip && *f.tip)
            w.value("tip", f.tip);
        if (f.kind == K::Number)
            w.value("min", double(f.min)).value("max", double(f.max)).value("step", double(f.step));
        if (f.kind == K::Choice) {
            w.beginArray("choices");
            for (uint32_t c = 0; c < f.choiceCount; ++c)
                w.value(nullptr, f.choices[c]);
            w.endArray();
        }
        w.endObject();
    }
    w.endArray();
}

}  // namespace fx
