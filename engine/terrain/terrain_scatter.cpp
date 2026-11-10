#include "terrain/terrain_scatter.h"

#include "assets/scene_pak.h"
#include "core/json.h"
#include "core/log.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace fx {

void readScatterLayer(const Json& j, ScatterLayer& l)
{
    auto num = [&](const char* key, float& v, float lo, float hi) {
        if (const Json* n = j.get(key); n && n->type == Json::Type::Number)
            v = float(std::clamp(n->number, double(lo), double(hi)));
    };
    l.name = j.getString("name", l.name.c_str());
    l.enabled = j.getBool("enabled", l.enabled);
    l.seed = uint32_t(j.getNumber("seed", l.seed));
    if (const Json* a = j.get("assets")) {
        l.assets.clear();
        for (const Json& aj : a->array) {
            if (l.assets.size() == ScatterLayer::kMaxAssets)
                break;
            ScatterAsset s;
            s.mesh = aj.getString("mesh");
            s.weight = float(std::clamp(aj.getNumber("weight", 1.0), 0.0, 1000.0));
            s.scaleMin = float(std::clamp(aj.getNumber("scaleMin", 0.8), 0.01, 100.0));
            s.scaleMax = float(std::clamp(aj.getNumber("scaleMax", 1.2), 0.01, 100.0));
            s.budget = uint32_t(std::clamp(aj.getNumber("budget", 20000), 256.0, 4000000.0));
            l.assets.push_back(s);
        }
    }
    num("density", l.density, 0.0001f, 100.0f);
    num("drawDistance", l.drawDistance, 10.0f, 10000.0f);
    if (const Json* m = j.get("materialLayers"))
        for (size_t i = 0; i < 4; ++i)
            l.materialLayers[i] = i < m->array.size() ? int32_t(m->array[i].number) : -1;
    num("materialMin", l.materialMin, 0.0f, 1.0f);
    num("slopeMin", l.slope[0], 0.0f, 90.0f);
    num("slopeMax", l.slope[1], 0.0f, 90.0f);
    num("heightMin", l.height[0], -1e5f, 1e5f);
    num("heightMax", l.height[1], -1e5f, 1e5f);
    num("clumpScale", l.clumpScale, 0.5f, 10000.0f);
    num("clumpAmount", l.clumpAmount, 0.0f, 1.0f);
    num("clumpThreshold", l.clumpThreshold, 0.0f, 1.0f);
    l.paint = uint32_t(std::clamp(j.getNumber("paint", l.paint), 0.0, 2.0));
    if (const Json* r = j.get("randomYaw"); r && !j.get("yawRange"))  // (older files: on or off)
        l.yawRange = r->b ? 360.0f : 0.0f;
    num("yawRange", l.yawRange, 0.0f, 360.0f);
    num("jitter", l.jitter, 0.0f, 1.0f);
    l.castShadows = j.getBool("castShadows", l.castShadows);
    if (const Json* b = j.get("paintBit"); b && b->type == Json::Type::Number)
        l.paintBit = int32_t(std::clamp(b->number, -1.0, double(ScatterLayer::kPaintable - 1)));
    num("align", l.align, 0.0f, 1.0f);
    num("tilt", l.tilt, 0.0f, 90.0f);
    num("sink", l.sink, -10.0f, 10.0f);
    num("tintVariation", l.tintVariation, 0.0f, 1.0f);
    l.collision = j.getBool("collision", l.collision);
    if (const Json* t = j.get("terrainLayer"); t && t->type == Json::Type::Number)
        l.terrainLayer = int32_t(std::clamp(t->number, -1.0, 255.0));
    l.terrainLook = j.getString("terrainLook", l.terrainLook.c_str());
    num("baseBlend", l.baseBlend, 0.0f, 25.0f);
    if (const Json* e = j.get("exclusions")) {
        l.exclusions.clear();
        for (const Json& x : e->array)
            if (x.array.size() == 4)
                l.exclusions.push_back({int32_t(x.array[0].number), int32_t(x.array[1].number), int32_t(x.array[2].number),
                                        int32_t(x.array[3].number)});
    }
}

std::vector<ScatterLayer> parseScatterLayers(const std::string& json)
{
    std::vector<ScatterLayer> layers;
    try {
        const Json root = Json::parse(json);
        uint32_t used = 0;  // paint bits taken (each layer's own)
        for (const Json& j : root.array) {
            ScatterLayer l;
            readScatterLayer(j, l);
            if (!j.get("paintBit"))  // (older files: by index)
                l.paintBit = layers.size() < ScatterLayer::kPaintable ? int32_t(layers.size()) : -1;
            if (l.paintBit >= 0 && (used & (1u << l.paintBit)))
                l.paintBit = -1;
            if (l.paintBit >= 0)
                used |= 1u << l.paintBit;
            layers.push_back(std::move(l));
        }
    } catch (const std::exception& e) {
        FX_WARN("terrain: bad scatter list (%s)", e.what());
    }
    return layers;
}

std::string scatterLayersJson(const std::vector<ScatterLayer>& layers)
{
    JsonWriter w;
    w.beginArray();
    for (const ScatterLayer& l : layers) {
        w.beginObject().value("name", l.name).value("enabled", l.enabled).value("seed", l.seed);
        w.beginArray("assets");
        for (const ScatterAsset& a : l.assets)
            w.beginObject()
                .value("mesh", a.mesh)
                .value("weight", double(a.weight))
                .value("scaleMin", double(a.scaleMin))
                .value("scaleMax", double(a.scaleMax))
                .value("budget", a.budget)
                .endObject();
        w.endArray().value("density", double(l.density)).value("drawDistance", double(l.drawDistance));
        w.beginArray("materialLayers");
        for (int32_t m : l.materialLayers)
            w.value(nullptr, m);
        w.endArray()
            .value("materialMin", double(l.materialMin))
            .value("slopeMin", double(l.slope[0]))
            .value("slopeMax", double(l.slope[1]))
            .value("heightMin", double(l.height[0]))
            .value("heightMax", double(l.height[1]))
            .value("clumpScale", double(l.clumpScale))
            .value("clumpAmount", double(l.clumpAmount))
            .value("clumpThreshold", double(l.clumpThreshold))
            .value("paint", l.paint)
            .value("yawRange", double(l.yawRange))
            .value("jitter", double(l.jitter))
            .value("castShadows", l.castShadows)
            .value("align", double(l.align))
            .value("tilt", double(l.tilt))
            .value("sink", double(l.sink))
            .value("tintVariation", double(l.tintVariation))
            .value("collision", l.collision)
            .value("terrainLayer", l.terrainLayer)
            .value("terrainLook", l.terrainLook)
            .value("baseBlend", double(l.baseBlend));
        if (l.paintBit >= 0)  // (none: left out, so a layer made in code gets one by its index when read)
            w.value("paintBit", l.paintBit);
        w.beginArray("exclusions");
        for (const auto& x : l.exclusions) {
            w.beginArray();
            for (int32_t v : x)
                w.value(nullptr, v);
            w.endArray();
        }
        w.endArray().endObject();
    }
    w.endArray();
    return w.str();
}

const std::vector<ScatterPreset>& scatterPresets()
{
    static const std::vector<ScatterPreset> presets = {
        {"Boulders", R"({"name": "Boulders", "assets": [{"mesh": "rock_boulder_a", "weight": 1, "scaleMin": 0.4, "scaleMax": 1.6, "budget": 20000},
            {"mesh": "rock_boulder_b", "weight": 1, "scaleMin": 0.4, "scaleMax": 1.4, "budget": 20000},
            {"mesh": "rock_boulder_c", "weight": 0.7, "scaleMin": 0.3, "scaleMax": 1.2, "budget": 20000}],
            "density": 0.004, "drawDistance": 400, "slopeMax": 32, "clumpAmount": 0.6, "clumpScale": 60, "clumpThreshold": 0.55,
            "align": 0.6, "tilt": 12, "sink": 0.25, "tintVariation": 0.2, "collision": true, "terrainLook": "rock", "baseBlend": 0.8})"},
        {"Scree", R"({"name": "Scree", "assets": [{"mesh": "rock_boulder_a", "weight": 1, "scaleMin": 0.08, "scaleMax": 0.35, "budget": 40000},
            {"mesh": "rock_boulder_c", "weight": 1, "scaleMin": 0.06, "scaleMax": 0.3, "budget": 40000}],
            "density": 0.08, "drawDistance": 100, "slopeMin": 26, "slopeMax": 44, "clumpAmount": 0.8, "clumpScale": 30, "clumpThreshold": 0.6,
            "align": 1, "tilt": 20, "sink": 0.15, "tintVariation": 0.25, "terrainLook": "rock", "baseBlend": 0.15})"},
        {"Cliff rocks", R"({"name": "Cliff rocks", "assets": [{"mesh": "rock_slab_a", "weight": 1, "scaleMin": 0.8, "scaleMax": 2.4, "budget": 20000},
            {"mesh": "rock_slab_b", "weight": 1, "scaleMin": 0.8, "scaleMax": 2.2, "budget": 20000},
            {"mesh": "rock_boulder_b", "weight": 0.5, "scaleMin": 1.0, "scaleMax": 2.5, "budget": 10000}],
            "density": 0.012, "drawDistance": 600, "slopeMin": 34, "slopeMax": 90, "align": 1, "tilt": 15, "sink": 0.5,
            "tintVariation": 0.15, "collision": true, "terrainLook": "rock", "baseBlend": 0})"},
        {"Spires and arches", R"({"name": "Spires and arches", "assets": [{"mesh": "rock_spire_a", "weight": 1, "scaleMin": 0.8, "scaleMax": 1.6, "budget": 4000},
            {"mesh": "rock_spire_b", "weight": 1, "scaleMin": 0.7, "scaleMax": 1.4, "budget": 4000},
            {"mesh": "rock_arch", "weight": 0.25, "scaleMin": 0.8, "scaleMax": 1.3, "budget": 2000}],
            "density": 0.0006, "drawDistance": 1500, "slopeMax": 25, "clumpAmount": 0.7, "clumpScale": 300, "clumpThreshold": 0.5,
            "align": 0.2, "tilt": 6, "sink": 0.4, "tintVariation": 0.1, "collision": true, "terrainLook": "rock", "baseBlend": 1.5})"},
    };
    return presets;
}

const char* scatterPreset(const std::string& name)
{
    for (const ScatterPreset& p : scatterPresets())
        if (name == p.name)
            return p.json;
    return nullptr;
}

void addScatterLooks(Terrain& terrain)
{
    std::vector<TerrainLayer> layers = parseTerrainLayers(terrain.layersJson());
    const size_t before = layers.size();
    for (const ScatterLayer& l : parseScatterLayers(terrain.scatterJson())) {
        if (l.terrainLook.empty() || layers.size() >= terrain::kMaxLayers ||
            std::find(std::begin(kGeneratedLayers), std::end(kGeneratedLayers), l.terrainLook) == std::end(kGeneratedLayers))
            continue;
        if (std::any_of(layers.begin(), layers.end(), [&](const TerrainLayer& t) { return t.generated == l.terrainLook; }))
            continue;
        TerrainLayer t;
        t.generated = l.terrainLook;
        t.name = std::string(1, char(std::toupper(uint8_t(l.terrainLook[0])))) + l.terrainLook.substr(1);
        t.seed = uint32_t(layers.size() + 1);
        layers.push_back(t);
    }
    if (layers.size() != before)
        terrain.setLayersJson(terrainLayersJson(layers));
}

float scatterCellSize(const ScatterLayer& layer)
{
    return std::clamp(std::sqrt(512.0f / std::max(layer.density, 1e-4f)), 4.0f, 256.0f);
}

std::vector<ScatterPool> reserveScatterPools(const std::vector<ScatterLayer>& layers, ScenePak& pak)
{
    std::vector<ScatterPool> pools;
    uint32_t total = 0;
    for (uint32_t l = 0; l < layers.size(); ++l)
        for (uint32_t a = 0; a < layers[l].assets.size(); ++a) {
            const ScatterAsset& asset = layers[l].assets[a];
            const int32_t mesh = pak.findMesh(asset.mesh);
            if (mesh < 0) {
                FX_WARN("terrain: scatter layer '%s': the scene has no mesh '%s'", layers[l].name.c_str(), asset.mesh.c_str());
                continue;
            }
            pools.push_back({l, a, uint32_t(mesh), pak.addInstancePool(uint32_t(mesh), asset.budget), asset.budget});
            total += asset.budget;
        }
    if (!pools.empty())
        FX_INFO("terrain: %zu scatter pools, %u instance slots", pools.size(), total);
    return pools;
}

}  // namespace fx
