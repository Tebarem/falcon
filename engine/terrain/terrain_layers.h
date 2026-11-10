#pragma once

// Terrain material layers: what a painted material looks like and how it is laid over the
// ground so that it never visibly repeats.
//
// A layer's textures are albedo (+ height in alpha), a tangent-space normal map and
// occlusion / roughness / metallic, either from the user's images (paths relative to the scene
// folder) or generated (tileable procedural grass, dirt, rock, sand, snow). They are resized to
// one resolution, mipped and BC-encoded (BC7 / BC5 / BC7) on a background thread, and cached in
// <scene>.terrain/layers/ by a key of their sources, so a layer is built once.
//
// The shading parameters (tile size, hex tiling, height blending, projection, macro variation,
// tint, ...) are uniforms of the terrain surface pass (render/terrain_renderer.h), so changing
// them costs nothing to rebuild. The list is kept as JSON in terrain.json.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fx {

struct TerrainLayer {
    std::string name = "Layer";
    // Textures: a generated look ("grass", "dirt", "rock", "sand", "snow"), or images.
    std::string generated = "grass";
    uint32_t seed = 1;
    std::string albedo, normal, roughnessMap, ao, height;  // image files, relative to the scene folder
    // Shading.
    float tileSize = 4.0f;        // metres per texture repeat
    bool hex = true;              // hex tiling: every cell of a hex grid its own rotation and offset
    float rotation = 1.0f;        // its random rotation, as a share of a full turn
    float contrast = 0.5f;        // how sharply hex cells meet (0 soft blend .. 1 crisp)
    float heightBlend = 0.25f;    // transition against other layers, metres of texture height (small = sharp)
    float macro = 0.25f;          // large-scale brightness / hue variation
    float macroScale = 60.0f;     // its feature size, metres
    uint32_t projection = 1;      // 0: from above, 1: biplanar on steep ground, 2: always biplanar
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float roughness = 1.0f;       // multiplier
    float normalStrength = 1.0f;
    float farScale = 4.0f;        // in the distance, a second sample this much larger is blended in
    // How it sounds (an acoustic preset, assets/acoustic_material.h); "": from its generated look.
    std::string acoustic;
    // What it sounds like hit or walked on (a surface sound preset); "": as its acoustics.
    std::string surface;
};

std::vector<TerrainLayer> parseTerrainLayers(const std::string& json);
std::string terrainLayersJson(const std::vector<TerrainLayer>& layers);
// Names of the generated looks.
extern const char* const kGeneratedLayers[5];

struct TerrainLayerTextures {
    uint32_t resolution = 0, mips = 0;
    std::vector<uint8_t> channel[3];  // BC blocks, mips concatenated: albedo+height BC7 sRGB, normal BC5, ORM BC7
    float meanAlbedo[3] = {0.5f, 0.5f, 0.5f};  // linear, of the whole texture (GI)
};

// What a layer's textures depend on (sources, file sizes and times, resolution): equal keys
// mean equal textures.
uint64_t terrainLayerKey(const TerrainLayer& layer, const std::string& sceneFolder, uint32_t resolution);

// Builds layers' textures on a thread of its own, newest request per layer first.
class TerrainLayerBuilder {
public:
    ~TerrainLayerBuilder() { stop(); }
    void request(uint32_t index, const TerrainLayer& layer, const std::string& sceneFolder, const std::string& cacheDir,
                 uint32_t resolution);
    // A finished layer (its index and key), once.
    bool poll(uint32_t& index, uint64_t& key, TerrainLayerTextures& out);
    bool busy();
    void stop();

private:
    struct Job {
        uint32_t index;
        uint64_t key;
        TerrainLayer layer;
        std::string sceneFolder, cacheDir;
        uint32_t resolution;
    };
    struct Done {
        uint32_t index;
        uint64_t key;
        TerrainLayerTextures textures;
    };
    void main();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool quit_ = false, working_ = false;
};

}  // namespace fx
