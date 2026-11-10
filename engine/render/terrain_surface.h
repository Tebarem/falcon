#pragma once

// The terrain's materials on the GPU, and the pass that shades them (terrain_surface.slang):
//   layer textures   three BC texture arrays (albedo + height, normal, occlusion / roughness /
//                    metallic), one array layer per material layer, built and cached by
//                    TerrainLayerBuilder, uploaded as they finish
//   tile atlas       heights (R16) and material maps (RGBA8) of the resident tiles nearest the
//                    camera (up to kAtlasTiles), refreshed where the terrain changes
//   coarse maps      every 8th sample's height and material over the whole terrain, always there
//   data buffer      per-layer shading parameters and the tile -> atlas layer page table
// shade() runs after the visibility buffer: for the terrain's pixels, the material and normal the
// resolve then lights.

#include "core/math.h"
#include "scene/fly_camera.h"
#include "terrain/terrain_layers.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

class GpuDevice;
class Terrain;

class TerrainSurface {
public:
    static constexpr uint32_t kAtlasTiles = 96;        // tiles of full detail on the GPU (~38 MB)
    static constexpr uint32_t kLayerResolution = 1024;  // layer textures, texels per side

    bool init(const GpuDevice& gpu);
    void shutdown();
    // The terrain to shade (nullptr: none) and its scene (layer images and their cache).
    void setTerrain(const Terrain* terrain, const std::string& scenePath);

    // Before rendering: layer textures that finished building, the atlas around `focus`, what the
    // terrain changed. One copy pass.
    void update(SDL_GPUCommandBuffer* cmd, float3 focus);
    // After the visibility buffer: the terrain's material and normal per pixel into output(), and
    // terrain-shaded instances' (fpak::kInstanceTerrainShaded; their triangles from `scene`).
    void shade(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* visBuffer, SDL_GPUBuffer* visible,
               SDL_GPUSampler* pointSampler, uint32_t width, uint32_t height, const CameraState& camera, float2 jitter,
               uint32_t firstInstance, uint32_t instanceCount, SDL_GPUBuffer* scene);
    // RGBA32_UINT, render-sized while a terrain is shaded (1 x 1 otherwise; the resolve binds it).
    SDL_GPUTexture* output() const { return output_; }
    bool active() const { return active_; }

    // Layers whose textures are on the GPU, and their average colour (linear): patches carry it
    // as their tint for GI. Bumped whenever a mean changes.
    uint32_t layerCount() const { return uint32_t(layers_.size()); }
    bool layerMean(uint32_t layer, float3& mean) const;
    uint64_t meanRevision() const { return meanRevision_; }
    bool building() { return builder_.busy(); }

    // For other passes that read the terrain (scatter): its height and material maps.
    SDL_GPUTexture* coarseHeights() const { return coarseHeights_; }
    SDL_GPUTexture* coarseMaterials() const { return coarseMaterials_; }
    SDL_GPUTexture* tileHeights() const { return tileHeights_; }
    SDL_GPUTexture* tileMaterials() const { return tileMaterials_; }
    SDL_GPUBuffer* data() const { return data_; }  // the page table at pageTableOffset()
    // Where the coarse maps are: world x, z of their first sample, their spacing, 1 / it; height
    // min, range, the maps' side, layers with textures (0: the default look). False: no terrain.
    bool coarseLayout(float4& grid, float4& heights) const;
    static uint32_t pageTableOffset();
    SDL_GPUSampler* linearClamp() const { return linearClamp_; }
    SDL_GPUSampler* pointClamp() const { return pointClamp_; }
    // Bumped when a tile's full detail reaches or leaves the GPU (what reads it may want to redo).
    uint64_t tileRevision(uint32_t tile) const { return tile < tileRevision_.size() ? tileRevision_[tile] : 0; }

private:
    bool ensureLayerArrays(uint32_t capacity, uint32_t mips);
    void releaseTerrainTextures();
    void writeData();

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* pass_ = nullptr;
    SDL_GPUSampler *linearClamp_ = nullptr, *pointClamp_ = nullptr, *layerSampler_ = nullptr;
    SDL_GPUTexture* output_ = nullptr;
    uint32_t outputW_ = 0, outputH_ = 0;
    bool active_ = false;

    const Terrain* terrain_ = nullptr;
    std::string scenePath_, sceneFolder_;
    uint64_t terrainRevision_ = 0;
    bool fullCoarse_ = true;

    // Layers.
    std::string layersJson_;
    std::vector<TerrainLayer> layers_;
    std::vector<uint64_t> requested_, loaded_;  // texture keys per layer
    std::vector<float3> means_;
    uint64_t meanRevision_ = 1;
    TerrainLayerBuilder builder_;
    SDL_GPUTexture* layerTex_[3] = {};
    uint32_t layerCapacity_ = 0, layerMips_ = 0;

    // Tile atlas and coarse maps.
    SDL_GPUTexture *tileHeights_ = nullptr, *tileMaterials_ = nullptr;
    SDL_GPUTexture *coarseHeights_ = nullptr, *coarseMaterials_ = nullptr;
    uint32_t coarseSide_ = 0;
    std::vector<int32_t> atlasOfTile_;  // per tile: atlas layer, -1
    std::vector<int32_t> tileOfAtlas_;  // per atlas layer: tile, -1
    std::vector<uint8_t> atlasStale_;   // per atlas layer: to upload again
    std::vector<uint32_t> pageTable_;   // per tile: atlas layer + 1, 0 (what the GPU has)
    std::vector<uint64_t> tileRevision_;

    // Shading parameters + page table.
    SDL_GPUBuffer* data_ = nullptr;
    uint32_t dataBytes_ = 0;
    bool dataDirty_ = true;
    std::vector<uint8_t> dataCpu_;

    // Staging, filled per update.
    struct TextureWrite {
        SDL_GPUTexture* texture;
        uint32_t mip, layer, x, y, w, h, src;
    };
    std::vector<uint8_t> staging_;
    std::vector<TextureWrite> writes_;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
};

}  // namespace fx
