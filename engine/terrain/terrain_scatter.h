#pragma once

// Terrain scatter: layers of meshes placed over the terrain by rules, generated on the GPU
// (render/scatter_renderer.h) for the cells around the camera, the same every run.
//
// A layer has up to four assets (meshes of the scene, by name, with a weight and a scale range),
// a density, and rules for where they grow: the material layers under them, slope and height
// ranges, a clumping noise, and an optional painted mask (paint "erase" removes them where
// painted, "only" keeps them to painted ground; the first 7 layers can be painted, in the
// material map's spare bits). Placement: random yaw, a lean toward the ground's slope, random
// tilt, sinking into the ground, and a brightness variation. Single instances can be deleted
// (they are remembered as exclusions).
//
// Every asset of every layer has its own range of instance slots, reserved when the scene loads
// (`budget`), so a slot's mesh never changes; a layer or asset added in the editor shows after a
// restart. The layers are kept as JSON in terrain.json.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fx {

class ScenePak;
struct Json;

struct ScatterAsset {
    std::string mesh;          // the mesh's name in the scene (ScenePak::meshName)
    float weight = 1.0f;       // share of the layer's density
    float scaleMin = 0.8f, scaleMax = 1.2f;
    uint32_t budget = 20000;   // instance slots reserved for it
};

struct ScatterLayer {
    static constexpr uint32_t kMaxAssets = 16;
    static constexpr uint32_t kPaintable = 7;  // layers 0..6 can be painted (material map bits 25..31)
    std::string name = "Scatter";
    bool enabled = true;
    uint32_t seed = 1;
    std::vector<ScatterAsset> assets;
    float density = 0.05f;        // instances per square metre
    float drawDistance = 300.0f;  // metres
    // Where they grow.
    int32_t materialLayers[4] = {-1, -1, -1, -1};  // on any of these terrain layers (none listed: anywhere)
    float materialMin = 0.5f;                      // with at least this much of it
    float slope[2] = {0.0f, 35.0f};                // degrees
    float height[2] = {-1e5f, 1e5f};               // world
    float clumpScale = 30.0f;                      // metres per clump
    float clumpAmount = 0.0f;                      // 0: even .. 1: clumps only
    float clumpThreshold = 0.5f;                   // more: smaller clumps
    uint32_t paint = 0;                            // 0 ignore the mask, 1 erase where painted, 2 only where painted
    // The material map bit (0..6, kScatterMask << bit) its painted mask uses, kept when other
    // layers are removed; -1: none (all 7 taken), generated only. Older files: the layer's index.
    int32_t paintBit = -1;
    // How they stand.
    float yawRange = 360.0f;     // random turn around the up axis, degrees (0: all the same way)
    float jitter = 1.0f;         // 0: on an even grid .. 1: anywhere in its grid square
    bool castShadows = true;
    float align = 0.0f;          // 0 upright .. 1 along the ground's normal
    float tilt = 0.0f;           // random lean, degrees
    float sink = 0.0f;           // into the ground, metres per unit of scale
    float tintVariation = 0.15f;  // darkens up to this much at random
    bool collision = false;      // static bodies near the camera
    // Shaded like the terrain (generated rocks): with a terrain material layer, by number or by
    // its generated look ("rock"; -1 / "": the mesh's own material), and the ground's material
    // blended onto the parts up to `baseBlend` metres above the ground.
    int32_t terrainLayer = -1;
    std::string terrainLook;
    float baseBlend = 0.6f;
    std::vector<std::array<int32_t, 4>> exclusions;  // deleted: cell x, cell z, asset, candidate
};

std::vector<ScatterLayer> parseScatterLayers(const std::string& json);
// The fields `j` has (a layer object of the list, or the editor's changes to one) into `l`.
void readScatterLayer(const Json& j, ScatterLayer& l);
std::string scatterLayersJson(const std::vector<ScatterLayer>& layers);

// Built-in layers to start from (rocks of `falcon cook --rocks`): name, JSON of one layer.
struct ScatterPreset {
    const char* name;
    const char* json;
};
const std::vector<ScatterPreset>& scatterPresets();
const char* scatterPreset(const std::string& name);  // its JSON (nullptr: none)

class Terrain;
// The generated looks the terrain's scatter layers shade with (terrainLook), added to its
// material layers when missing.
void addScatterLooks(Terrain& terrain);

// Square cells of a layer (metres): about 512 candidates each at its density.
float scatterCellSize(const ScatterLayer& layer);

// Collision for scattered instances (render/scatter_renderer.h reads them back, physics adds
// them): one body per instance, per cell.
struct ScatterBody {
    uint32_t mesh;
    float world[3][4];
};
struct ScatterCellBodies {
    uint64_t cell;
    std::vector<ScatterBody> bodies;
};

// A range of instance slots for one asset of one layer.
struct ScatterPool {
    uint32_t layer, asset, mesh;
    uint32_t firstSlot, count;
};
// Before ScenePak::upload: a pool per (layer, asset) whose mesh the scene has.
std::vector<ScatterPool> reserveScatterPools(const std::vector<ScatterLayer>& layers, ScenePak& pak);

}  // namespace fx
