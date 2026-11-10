#pragma once

// Terrain scatter on the GPU (terrain_scatter.slang). Every asset of every scatter layer
// (terrain/terrain_scatter.h) is a "sub-layer" with its own pool of instance slots (fixed mesh),
// split into equal blocks, one per cell: a square of the layer's cell size holding a k x k grid of
// candidates, one slot each.
//
// Per frame: the cells within each layer's draw distance get a block (nearest first, while
// blocks last) and a generation job; cells beyond 1.15 x that distance are cleared (their slots
// hidden) and give their block back; cells the terrain changed under (heights, paint, holes), or
// whose tiles' full detail arrived on the GPU, are generated again. A changed layer list redoes
// every cell. The cached shadow pages over every generated or cleared cell are redrawn.
// Cells of layers with collision near the camera are read back (a few frames late) for physics.

#include "assets/scene_pak.h"
#include "core/math.h"
#include "terrain/terrain_scatter.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;
class Terrain;
class TerrainSurface;
class VirtualShadowMap;

struct ScatterStats {
    uint32_t cells = 0, slots = 0;  // generated cells and their candidate slots
    uint32_t jobs = 0;              // generated or cleared in the last update
    uint32_t starved = 0;           // cells wanted but out of blocks (raise the asset's budget)
    uint32_t subs = 0;              // layer meshes drawn (with slots, enabled, a share)
    uint32_t unplaced = 0;          // layer meshes without slots this session (save and restart)
};

class ScatterRenderer {
public:
    static constexpr uint32_t kMaxJobsPerFrame = 256;  // generations (clears are not limited)

    bool init(const GpuDevice& gpu, const ScenePak& pak, VirtualShadowMap& vsm);
    void shutdown();
    // The slot pools reserved for the scene's scatter layers (before setTerrain), and spare slots
    // (the editor's) that layers and meshes added later are given slots from.
    void setPools(std::vector<ScatterPool> pools, uint32_t spareFirst = 0, uint32_t spareCount = 0);
    const std::vector<ScatterPool>& pools() const { return pools_; }
    void setTerrain(const Terrain* terrain);

    // After the terrain surface's update, before culling.
    void update(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* prevInstances, const TerrainSurface& surface, float3 camera);

    // The scatter instance in a slot (editor picking): layer, asset, cell and candidate.
    struct Hit {
        uint32_t layer, asset;
        int32_t cellX, cellZ;
        uint32_t candidate;
    };
    bool find(uint32_t slot, Hit& hit) const;
    bool ownsSlot(uint32_t slot) const;

    // Collision: cells of layers with collision within `radius` of `focus` are read back.
    void setCollisionFocus(float3 focus, float radius)
    {
        collisionFocus_ = focus;
        collisionRadius_ = radius;
    }
    // Inside the renderer's readback copy pass (ring slot `ring`), and once its fence passed.
    void recordDownloads(SDL_GPUCopyPass* copy, uint32_t ring);
    void finishDownloads(uint32_t ring);
    // Bodies to create (whole cells, replacing what the cell had) and cells whose bodies go.
    void takeCollision(std::vector<ScatterCellBodies>& added, std::vector<uint64_t>& removed);

    const ScatterStats& stats() const { return stats_; }

private:
    struct Sub {
        uint32_t layer, asset, pool;
        uint32_t first;  // its pool's first slot (kept: the pools change with the layers)
        uint32_t mesh, k;
        float cellSize, drawDistance;
        uint32_t blocks;
        std::vector<uint32_t> freeBlocks;
        bool collision;
        int32_t paintOnly;  // grows only where painted: its paint bit (cells without paint get no slots); -1 not
    };
    // Which scatter paint bits a terrain tile has anywhere (its full detail when resident, the
    // coarse map's otherwise), cached until the terrain's materials change there.
    struct TilePaint {
        bool valid = false, resident = false;
        uint8_t bits = 0;
    };
    struct Cell {
        uint32_t sub;
        int32_t x, z;
        uint32_t block;
        uint64_t revision;    // the terrain tiles' revisions it was generated from
        uint32_t generation;  // of its rows (a readback of older rows is dropped)
        bool generated;       // at least once
        bool stale;
        bool downloaded;      // collision rows read back for this generation
    };
    struct Download {
        uint64_t cell;
        uint32_t sub, slots, generation;
        SDL_GPUTransferBuffer* buffer;
        uint32_t ring;
        bool recorded;
    };
    static uint64_t cellKey(uint32_t sub, int32_t x, int32_t z)
    {
        return uint64_t(sub) << 48 | uint64_t(uint32_t(z) & 0xffffff) << 24 | (uint32_t(x) & 0xffffff);
    }
    void buildSubs(const std::vector<ScatterLayer>& layers, std::vector<Sub>& subs, std::vector<uint8_t>& params) const;
    void assignPools(const std::vector<ScatterLayer>& layers);  // pools_ from arena_, in layer order
    uint64_t tileRevisions(const Cell& c, const TerrainSurface& surface) const;
    uint8_t tilePaintBits(uint32_t tx, uint32_t tz);
    bool cellPainted(const Sub& sub, int32_t x, int32_t z);
    float4 cellSphere(const Cell& c) const;

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    VirtualShadowMap* vsm_ = nullptr;
    SDL_GPUComputePipeline* pass_ = nullptr;
    SDL_GPUBuffer *jobs_ = nullptr, *params_ = nullptr, *exclusions_ = nullptr;
    uint32_t jobCapacity_ = 0, exclusionCapacity_ = 0;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;

    std::vector<ScatterPool> pools_;
    std::vector<std::pair<uint32_t, uint32_t>> arena_;  // instance slot ranges: first, count
    const Terrain* terrain_ = nullptr;
    std::string scatterJson_, layersJson_;
    std::vector<ScatterLayer> layers_;
    std::vector<Sub> subs_;
    std::unordered_map<uint64_t, Cell> cells_;
    uint64_t terrainRevision_ = 0;
    uint32_t generations_ = 0;
    bool paramsDirty_ = true, reset_ = true;
    std::vector<uint8_t> paramsCpu_;

    float3 collisionFocus_ = {0, 0, 0};
    float collisionRadius_ = 0.0f;
    std::vector<Download> downloads_;
    std::vector<ScatterCellBodies> collisionAdded_;
    std::vector<uint64_t> collisionRemoved_;
    ScatterStats stats_;
    std::vector<TilePaint> tilePaint_;
};

}  // namespace fx
