#pragma once

// Draws a Terrain through the GPU-driven renderer's own passes: every patch the terrain shows is
// one of the scene's reserved terrain slots (ScenePak::reserveTerrain), an ordinary instance +
// mesh + cluster whose 81 vertices and 128 triangles (holes: degenerate) this class writes. Culling, the visibility buffer, the resolve,
// shadows, GI and picking all take it as they take a cooked cluster.
//
// Patch levels (CPU, when the camera moves or the terrain changes): a quadtree descent splits a
// node while its error projects to more than `pixels` on screen, measured from the camera to
// the node's box. Off-screen nodes stay coarser (8x the error) but are kept: shadows and GI need
// them; the GPU culls to the view. Then neighbours are balanced to at most one level apart (a
// split node's neighbours' parents split too), and a patch next to a coarser one moves its odd
// edge vertices onto the coarser edge, so the surface has no cracks. Only patches that changed
// are rewritten, and the cached shadow pages under them redrawn.

#include "assets/scene_pak.h"
#include "core/math.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fx {

class Terrain;
class TerrainSurface;
class SceneBvh;
class VirtualShadowMap;

struct TerrainRenderStats {
    uint32_t patches = 0;     // drawn (selected) this frame
    uint32_t rewritten = 0;   // patches written in the last change
    uint32_t capacity = 0;    // reserved slots
    float selectMs = 0.0f;    // CPU time of the last selection
    float pixels = 0.0f;      // error threshold actually used (raised when the slots run out)
};

class TerrainRenderer {
public:
    bool init(SDL_GPUDevice* device, const ScenePak& pak, VirtualShadowMap& vsm);
    void shutdown();
    // The terrain to draw (nullptr: none). Every patch is written again.
    void setTerrain(const Terrain* terrain);
    const Terrain* terrain() const { return terrain_; }
    // The materials' GPU side: patches carry their layers' average colour for GI.
    void setSurface(const TerrainSurface* surface) { surface_ = surface; }
    // Each patch's triangles also go to the ray tracing BVH (nullptr: not).
    void setBvh(SceneBvh* bvh) { bvh_ = bvh; }

    // Before culling: picks patch levels and uploads what changed into the scene buffer and
    // `prevInstances` (motion vectors). `pixels`: largest projected error; nearer than
    // `detailDistance`, the finest patches. Returns true when patches changed (GI may want to know).
    bool update(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* prevInstances, const CameraState& camera,
                uint32_t displayHeight, float pixels, float detailDistance = 0.0f);

    // The patches in use (their instance, cluster and world bounding sphere), for the BVH.
    struct PatchItem {
        uint32_t instance, cluster;
        float4 sphere;
    };
    const std::vector<PatchItem>& patchItems() const { return patchItems_; }
    // What GI must relight since the last call: everything (a new terrain), or the world box of
    // the edits (min xyz, max xyz). False: nothing.
    bool takeGiChange(bool& all, float region[6]);
    bool ownsSlot(uint32_t instance) const
    {
        return slots_.patches && instance >= slots_.firstInstance && instance < slots_.firstInstance + slots_.patches;
    }
    const TerrainRenderStats& stats() const { return stats_; }

private:
    struct Patch {
        uint32_t slot;
        uint8_t stitch;  // edges next to a coarser neighbour: 1 -x, 2 +x, 4 -z, 8 +z
        float4 sphere;   // world bounds, for shadow invalidation
    };
    static uint64_t key(uint32_t level, uint32_t x, uint32_t z) { return uint64_t(level) << 48 | uint64_t(z) << 24 | x; }
    // The quadtree cut: the nodes split into their children (a bit per node of every level, and
    // a list per level to walk them); the leaves are the patches.
    struct Selection {
        std::vector<std::vector<uint64_t>> bits;    // [level][node / 64]
        std::vector<std::vector<uint64_t>> splits;  // [level]: keys of the split nodes
        std::vector<uint32_t> side;                 // nodes per side, per level
        std::vector<uint64_t> leaves;
        bool isSplit(uint32_t l, uint32_t x, uint32_t z) const
        {
            const uint64_t i = uint64_t(z) * side[l] + x;
            return (bits[l][i >> 6] >> (i & 63)) & 1;
        }
        void setSplit(uint32_t l, uint32_t x, uint32_t z);
        bool isLeaf(uint64_t k) const;
    };

    void select(const CameraState& camera, uint32_t displayHeight, float pixels, float detailDistance, Selection& s) const;
    void balance(Selection& s) const;
    void collectLeaves(Selection& s) const;
    uint8_t stitchOf(uint64_t k, const Selection& s) const;
    // One patch's mesh, cluster and vertices, appended to the upload.
    float4 writePatch(uint64_t k, uint8_t stitch, uint32_t slot);
    void hideAll();

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    VirtualShadowMap* vsm_ = nullptr;
    ScenePak::TerrainSlots slots_;
    const Terrain* terrain_ = nullptr;
    const TerrainSurface* surface_ = nullptr;
    SceneBvh* bvh_ = nullptr;
    uint64_t meanRevision_ = 0;

    std::unordered_map<uint64_t, Patch> patches_;
    Selection selection_;
    std::vector<uint32_t> freeSlots_;
    std::vector<PatchItem> patchItems_;

    // The camera and terrain revisions the patches were picked for.
    float3 selectedFrom_ = {1e30f, 1e30f, 1e30f};
    float selectedPixels_ = 0.0f, selectedDetail_ = 0.0f;
    float lastPixels_ = 0.0f;  // the threshold the slots allowed last time (a starting guess)
    uint32_t selectedHeight_ = 0;
    uint64_t terrainRevision_ = 0, residencyRevision_ = 0;
    bool rewriteAll_ = true;
    bool giAll_ = false, giRegion_ = false;
    float giBox_[6] = {};

    // Upload staging, filled per change.
    struct Write {
        uint32_t section, offset, bytes, src;  // section: fpak::Section, or ~0u: previous instances
    };
    std::vector<uint8_t> staging_;
    std::vector<Write> writes_;
    std::vector<float4> invalidations_;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    TerrainRenderStats stats_;
};

}  // namespace fx
