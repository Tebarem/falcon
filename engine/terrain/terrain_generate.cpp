#include "terrain/terrain_generate.h"

#include "core/json.h"
#include "core/log.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"

#include <algorithm>
#include <cctype>

namespace fx {

const char* const kGenOpNames[uint32_t(GenOp::Count)] = {"noise", "terrace", "plateau", "smooth", "erode", "paint"};
const char* const kGenNoiseNames[uint32_t(GenNoise::Count)] = {"fbm", "ridged", "billow", "eroded", "cells", "crags", "mesas"};
const char* const kGenBlendNames[uint32_t(GenBlend::Count)] = {"add", "max", "min", "replace", "multiply"};

namespace {

template <typename E, size_t N>
void readEnum(const Json& j, const char* key, E& v, const char* const (&names)[N])
{
    if (const Json* s = j.get(key); s && s->type == Json::Type::String)
        for (size_t i = 0; i < N; ++i)
            if (s->str == names[i])
                v = E(i);
}

}  // namespace

void readGenStep(const Json& j, GenStep& s)
{
    auto num = [&](const char* key, float& v, float lo, float hi) {
        if (const Json* n = j.get(key); n && n->type == Json::Type::Number)
            v = float(std::clamp(n->number, double(lo), double(hi)));
    };
    auto count = [&](const char* key, uint32_t& v, uint32_t lo, uint32_t hi) {
        if (const Json* n = j.get(key); n && n->type == Json::Type::Number)
            v = uint32_t(std::clamp(n->number, double(lo), double(hi)));
    };
    readEnum(j, "op", s.op, kGenOpNames);
    s.enabled = j.getBool("enabled", s.enabled);
    s.name = j.getString("name", s.name.c_str());
    readEnum(j, "blend", s.blend, kGenBlendNames);
    num("amount", s.amount, 0.0f, 1.0f);
    count("seed", s.seed, 0, 1u << 30);
    readEnum(j, "noise", s.noise, kGenNoiseNames);
    num("scale", s.scale, 1.0f, 100000.0f);
    num("height", s.height, -10000.0f, 10000.0f);
    num("offset", s.offset, -1.0f, 1.0f);
    count("octaves", s.octaves, 1, 12);
    num("lacunarity", s.lacunarity, 1.1f, 4.0f);
    num("gain", s.gain, 0.05f, 0.95f);
    num("warp", s.warp, 0.0f, 10000.0f);
    num("warpScale", s.warpScale, 1.0f, 100000.0f);
    num("sharpness", s.sharpness, 0.1f, 8.0f);
    num("step", s.step, 0.1f, 1000.0f);
    num("stepSharpness", s.stepSharpness, 0.0f, 1.0f);
    num("strata", s.strata, 0.0f, 1.0f);
    num("level", s.level, -1e5f, 1e5f);
    num("knee", s.knee, 0.1f, 10000.0f);
    num("radius", s.radius, 0.1f, 500.0f);
    count("iterations", s.iterations, 1, 5000);
    num("cellSize", s.cellSize, 0.25f, 64.0f);
    num("rain", s.rain, 0.0f, 1.0f);
    num("capacity", s.capacity, 0.0f, 20.0f);
    num("solubility", s.solubility, 0.0f, 1.0f);
    num("deposition", s.deposition, 0.0f, 1.0f);
    num("evaporation", s.evaporation, 0.0f, 1.0f);
    num("talus", s.talus, 5.0f, 89.0f);
    num("thermal", s.thermal, 0.0f, 1.0f);
    count("layer", s.layer, 0, 255);
    s.look = j.getString("look", s.look.c_str());
    num("heightMin", s.heightMin, -1e5f, 1e5f);
    num("heightMax", s.heightMax, -1e5f, 1e5f);
    num("heightSoft", s.heightSoft, 0.01f, 1000.0f);
    num("slopeMin", s.slopeMin, 0.0f, 90.0f);
    num("slopeMax", s.slopeMax, 0.0f, 90.0f);
    num("slopeSoft", s.slopeSoft, 0.01f, 45.0f);
    num("curvatureMin", s.curvatureMin, -1.0f, 1.0f);
    num("curvatureMax", s.curvatureMax, -1.0f, 1.0f);
    num("curvatureScale", s.curvatureScale, 0.5f, 500.0f);
    num("maskNoise", s.maskNoise, 0.0f, 1.0f);
    num("maskNoiseScale", s.maskNoiseScale, 0.5f, 10000.0f);
    num("flowMin", s.flowMin, 0.0f, 1.0f);
    num("flowMax", s.flowMax, 0.0f, 1.0f);
    num("depositMin", s.depositMin, 0.0f, 1.0f);
    num("depositMax", s.depositMax, 0.0f, 1.0f);
}

void readGenRegion(const Json& j, GenRegion& r)
{
    auto num = [&](const char* key, float& v, float lo, float hi) {
        if (const Json* n = j.get(key); n && n->type == Json::Type::Number)
            v = float(std::clamp(n->number, double(lo), double(hi)));
    };
    r.all = j.getBool("all", r.all);
    num("x", r.x, -1e6f, 1e6f);
    num("z", r.z, -1e6f, 1e6f);
    num("size", r.size, 1.0f, 100000.0f);
    num("falloff", r.falloff, 0.0f, 10000.0f);
    r.flat = j.getBool("flat", r.flat);
    num("base", r.base, -1e5f, 1e5f);
}

TerrainGenStack parseGenStack(const std::string& json)
{
    TerrainGenStack g;
    try {
        const Json root = Json::parse(json);
        if (const Json* r = root.get("region"))
            readGenRegion(*r, g.region);
        if (const Json* steps = root.get("steps"))
            for (const Json& j : steps->array) {
                GenStep s;
                readGenStep(j, s);
                g.steps.push_back(std::move(s));
            }
    } catch (const std::exception& e) {
        FX_WARN("terrain: bad generator (%s)", e.what());
    }
    return g;
}

std::string genStackJson(const TerrainGenStack& g)
{
    JsonWriter w;
    w.beginObject();
    const GenRegion& r = g.region;
    w.beginObject("region")
        .value("all", r.all)
        .value("x", double(r.x))
        .value("z", double(r.z))
        .value("size", double(r.size))
        .value("falloff", double(r.falloff))
        .value("flat", r.flat)
        .value("base", double(r.base))
        .endObject();
    w.beginArray("steps");
    for (const GenStep& s : g.steps) {
        w.beginObject()
            .value("op", kGenOpNames[uint32_t(s.op)])
            .value("enabled", s.enabled)
            .value("name", s.name)
            .value("blend", kGenBlendNames[uint32_t(s.blend)])
            .value("amount", double(s.amount))
            .value("seed", s.seed)
            .value("noise", kGenNoiseNames[uint32_t(s.noise)])
            .value("scale", double(s.scale))
            .value("height", double(s.height))
            .value("offset", double(s.offset))
            .value("octaves", s.octaves)
            .value("lacunarity", double(s.lacunarity))
            .value("gain", double(s.gain))
            .value("warp", double(s.warp))
            .value("warpScale", double(s.warpScale))
            .value("sharpness", double(s.sharpness))
            .value("step", double(s.step))
            .value("stepSharpness", double(s.stepSharpness))
            .value("strata", double(s.strata))
            .value("level", double(s.level))
            .value("knee", double(s.knee))
            .value("radius", double(s.radius))
            .value("iterations", s.iterations)
            .value("cellSize", double(s.cellSize))
            .value("rain", double(s.rain))
            .value("capacity", double(s.capacity))
            .value("solubility", double(s.solubility))
            .value("deposition", double(s.deposition))
            .value("evaporation", double(s.evaporation))
            .value("talus", double(s.talus))
            .value("thermal", double(s.thermal))
            .value("layer", s.layer)
            .value("look", s.look)
            .value("heightMin", double(s.heightMin))
            .value("heightMax", double(s.heightMax))
            .value("heightSoft", double(s.heightSoft))
            .value("slopeMin", double(s.slopeMin))
            .value("slopeMax", double(s.slopeMax))
            .value("slopeSoft", double(s.slopeSoft))
            .value("curvatureMin", double(s.curvatureMin))
            .value("curvatureMax", double(s.curvatureMax))
            .value("curvatureScale", double(s.curvatureScale))
            .value("maskNoise", double(s.maskNoise))
            .value("maskNoiseScale", double(s.maskNoiseScale))
            .value("flowMin", double(s.flowMin))
            .value("flowMax", double(s.flowMax))
            .value("depositMin", double(s.depositMin))
            .value("depositMax", double(s.depositMax))
            .endObject();
    }
    w.endArray().endObject();
    return w.str();
}

// Presets. Paint steps name their looks (added to the terrain's layers when missing); the
// first step of each replaces the ground, so they work from any start.
const std::vector<GenPreset>& terrainGeneratorPresets()
{
    static const std::vector<GenPreset> presets = {
        {"Rolling hills", R"({"steps": [
            {"op": "noise", "name": "Hills", "blend": "replace", "noise": "fbm", "scale": 700, "height": 90, "octaves": 6, "warp": 120, "warpScale": 900, "seed": 3},
            {"op": "noise", "name": "Detail", "noise": "eroded", "scale": 140, "height": 10, "octaves": 6, "seed": 7},
            {"op": "erode", "name": "Erosion", "iterations": 250, "cellSize": 2, "rain": 0.01, "talus": 40, "thermal": 0.2},
            {"op": "paint", "name": "Grass", "look": "grass", "slopeMax": 90},
            {"op": "paint", "name": "Dirt in gullies", "look": "dirt", "flowMin": 0.35, "maskNoise": 0.4, "maskNoiseScale": 20},
            {"op": "paint", "name": "Rock on slopes", "look": "rock", "slopeMin": 28, "slopeSoft": 4}
        ]})"},
        {"Mountains", R"({"steps": [
            {"op": "noise", "name": "Ranges", "blend": "replace", "noise": "ridged", "scale": 1400, "height": 520, "offset": 0.25, "octaves": 7, "gain": 0.48, "warp": 250, "warpScale": 1600, "sharpness": 1.4, "seed": 11},
            {"op": "noise", "name": "Foothills", "noise": "fbm", "scale": 500, "height": 70, "octaves": 5, "seed": 12},
            {"op": "noise", "name": "Detail", "noise": "eroded", "scale": 220, "height": 40, "octaves": 7, "seed": 13},
            {"op": "erode", "name": "Erosion", "iterations": 400, "cellSize": 2, "talus": 36, "thermal": 0.35},
            {"op": "paint", "name": "Grass", "look": "grass"},
            {"op": "paint", "name": "Dirt in gullies", "look": "dirt", "flowMin": 0.5, "maskNoise": 0.3},
            {"op": "paint", "name": "Rock on slopes", "look": "rock", "slopeMin": 38, "slopeSoft": 6, "maskNoise": 0.3, "maskNoiseScale": 30},
            {"op": "paint", "name": "Snow up high", "look": "snow", "heightMin": 260, "heightSoft": 40, "slopeMax": 42, "maskNoise": 0.35, "maskNoiseScale": 60}
        ]})"},
        {"Canyons", R"({"steps": [
            {"op": "noise", "name": "Plateau", "blend": "replace", "noise": "fbm", "scale": 900, "height": 140, "octaves": 5, "warp": 200, "warpScale": 700, "seed": 21},
            {"op": "noise", "name": "Gorges", "blend": "min", "noise": "ridged", "scale": 600, "height": -120, "offset": 0.6, "octaves": 4, "sharpness": 2.5, "warp": 150, "warpScale": 500, "seed": 22},
            {"op": "terrace", "name": "Strata", "step": 14, "stepSharpness": 0.85, "strata": 0.45, "seed": 23},
            {"op": "erode", "name": "Erosion", "iterations": 200, "cellSize": 2, "talus": 50, "thermal": 0.25},
            {"op": "paint", "name": "Sand", "look": "sand"},
            {"op": "paint", "name": "Rock walls", "look": "rock", "slopeMin": 26, "slopeSoft": 4},
            {"op": "paint", "name": "Dirt below", "look": "dirt", "depositMin": 0.3, "slopeMax": 20}
        ]})"},
        {"Mesas", R"({"steps": [
            {"op": "noise", "name": "Base", "blend": "replace", "noise": "fbm", "scale": 800, "height": 30, "octaves": 5, "seed": 31},
            {"op": "noise", "name": "Mesas", "blend": "max", "noise": "mesas", "scale": 320, "height": 110, "sharpness": 5, "warp": 60, "warpScale": 250, "seed": 32},
            {"op": "erode", "name": "Erosion", "iterations": 220, "cellSize": 2, "talus": 45, "thermal": 0.35},
            {"op": "paint", "name": "Sand", "look": "sand"},
            {"op": "paint", "name": "Rock", "look": "rock", "slopeMin": 30, "slopeSoft": 5}
        ]})"},
        {"Dunes", R"({"steps": [
            {"op": "noise", "name": "Swell", "blend": "replace", "noise": "fbm", "scale": 900, "height": 40, "octaves": 4, "seed": 41},
            {"op": "noise", "name": "Dunes", "noise": "ridged", "scale": 200, "height": 30, "octaves": 3, "sharpness": 1.2, "warp": 110, "warpScale": 450, "seed": 42},
            {"op": "noise", "name": "Ripples", "noise": "ridged", "scale": 9, "height": 0.4, "octaves": 2, "warp": 6, "warpScale": 40, "seed": 43},
            {"op": "smooth", "name": "Soften", "radius": 1},
            {"op": "paint", "name": "Sand", "look": "sand"}
        ]})"},
        {"Crags", R"({"steps": [
            {"op": "noise", "name": "Hills", "blend": "replace", "noise": "fbm", "scale": 600, "height": 80, "octaves": 5, "seed": 51},
            {"op": "noise", "name": "Crags", "blend": "add", "noise": "crags", "scale": 180, "height": 45, "sharpness": 2, "warp": 90, "warpScale": 160, "seed": 52, "maskNoise": 0.6, "maskNoiseScale": 400},
            {"op": "noise", "name": "Detail", "noise": "eroded", "scale": 70, "height": 14, "octaves": 6, "seed": 53},
            {"op": "erode", "name": "Erosion", "iterations": 200, "cellSize": 2, "talus": 42, "thermal": 0.3},
            {"op": "paint", "name": "Grass", "look": "grass"},
            {"op": "paint", "name": "Rock", "look": "rock", "slopeMin": 32, "slopeSoft": 5, "curvatureMax": 0.05}
        ]})"},
    };
    return presets;
}

const char* terrainGeneratorPreset(const std::string& name)
{
    for (const GenPreset& p : terrainGeneratorPresets())
        if (name == p.name)
            return p.json;
    return nullptr;
}

void resolveGenLayers(TerrainGenStack& g, Terrain& terrain)
{
    std::vector<TerrainLayer> layers = parseTerrainLayers(terrain.layersJson());
    const size_t before = layers.size();
    for (GenStep& s : g.steps) {
        if (s.op != GenOp::Paint || s.look.empty())
            continue;
        auto it = std::find_if(layers.begin(), layers.end(), [&](const TerrainLayer& l) { return l.generated == s.look; });
        if (it == layers.end() && layers.size() < terrain::kMaxLayers &&
            std::find(std::begin(kGeneratedLayers), std::end(kGeneratedLayers), s.look) != std::end(kGeneratedLayers)) {
            TerrainLayer l;
            l.generated = s.look;
            l.name = std::string(1, char(std::toupper(uint8_t(s.look[0])))) + s.look.substr(1);
            l.seed = uint32_t(layers.size() + 1);
            layers.push_back(l);
            it = layers.end() - 1;
        }
        if (it != layers.end())
            s.layer = uint32_t(it - layers.begin());
    }
    if (layers.size() != before)
        terrain.setLayersJson(terrainLayersJson(layers));
}

}  // namespace fx
