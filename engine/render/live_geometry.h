#pragma once
// Live meshes: geometry made or changed while the engine runs (level meshes in the editor, meshes
// games build), drawn by the visibility-buffer passes like cooked meshes: shadows, GI, VT
// materials, picking. Their rows live in the scene buffer's live arena (ScenePak::reserveLive):
// each mesh is clustered here (meshoptimizer meshlets, one LOD level), quantized to its bounds and
// written into a run of cluster blocks, then uploaded with the frame (flush). Each mesh owns one
// instance slot (instanceOf); the caller spawns its entity on it, so transforms, visibility and
// selection go the usual way. A mesh also gets a Box3D triangle collider (ScenePak::setLiveCollider),
// which physics and audio pick up through ScenePak::staticCollider.

#include "assets/scene_pak.h"
#include "core/math.h"
#include "render/live_mesh_data.h"
#include "render/ray_tracing/scene_bvh.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <vector>

struct b3MeshData;

namespace fx {

class VirtualShadowMap;

// A live mesh made ready for LiveGeometry::commit: all that set() works out from the data alone
// (tangents, quantizing, clusters, the BVH, the collider). LiveGeometry::prepare makes it on any
// thread, so a level cut does it in its job and the main thread only finds it room and uploads it.
struct PreparedLiveMesh {
    struct ColliderDeleter {
        void operator()(b3MeshData* mesh) const;
    };
    bool valid = false;
    uint32_t triangles = 0;
    float lo[3] = {}, hi[3] = {}, scale[3] = {};
    // Per cluster block, as written; the rows' vertex and triangle offsets are block-relative
    // (commit adds where the blocks went).
    std::vector<fpak::VertexGpu> blockVerts;
    std::vector<uint64_t> blockExtra;
    std::vector<uint32_t> blockTris;
    std::vector<fpak::ClusterGpu> rows;
    std::vector<uint32_t> sourceTriangles;
    // The BLAS (traced: made at all); its triangle ids count clusters from the mesh's first.
    bool traced = false;
    SceneBvh::BuiltMesh bvh;
    std::unique_ptr<b3MeshData, ColliderDeleter> collider;
    float ms = 0.0f;  // prepare's time
};

struct LiveGeometryStats {
    uint32_t meshes = 0, meshCapacity = 0;   // live meshes in use
    uint32_t blocks = 0, blockCapacity = 0;  // cluster blocks in use
    uint32_t triangles = 0;                  // across all live meshes
    uint32_t bvhTriangles = 0;               // theirs in the ray-tracing BVH (what the probe GI's rays hit)
    uint32_t uploads = 0;                    // meshes written since the start
    float lastBuildMs = 0.0f;                // the last set(): clustering, quantizing, collider
};

class LiveGeometry {
public:
    void init(SDL_GPUDevice* device, const ScenePak& pak, VirtualShadowMap* vsm);
    void shutdown();
    // Colliders go to this pak (the one init got, writable). Without it, no colliders are made.
    void setColliderPak(ScenePak* pak) { colliderPak_ = pak; }
    // Each mesh's triangles also go to the ray tracing BVH (nullptr: not).
    void setBvh(SceneBvh* bvh) { bvh_ = bvh; }
    bool valid() const { return slots_.meshes > 0; }

    // A free live mesh, or -1 when all are taken. Empty until set().
    int32_t create();
    // Replaces a mesh's triangles. `world` is where its instance is (its old and new bounds there
    // get their cached shadow pages and GI re-made). False: no room (cluster blocks) or bad data;
    // the mesh keeps what it had.
    bool set(uint32_t live, const LiveMeshData& data, const float world[3][4], bool collider = true);
    // set() in two halves. prepare reads only its arguments (any thread): `blended` says per
    // material whether it is alpha-blended (blendedMaterials(), left out of the BVH), `traced`
    // whether to build the BVH (traced()). False: bad data. commit does the rest on the main
    // thread; false: no room, or a mesh not in use.
    static bool prepare(const LiveMeshData& data, const std::vector<uint8_t>& blended, bool traced, bool collider,
                        PreparedLiveMesh& out);
    bool commit(uint32_t live, PreparedLiveMesh&& prepared, const float world[3][4]);
    std::vector<uint8_t> blendedMaterials() const;
    bool traced() const { return bvh_ != nullptr; }
    bool makesColliders() const { return colliderPak_ != nullptr; }
    // Empties it (no clusters, no collider) and frees it for create().
    void destroy(uint32_t live, const float world[3][4]);

    uint32_t meshOf(uint32_t live) const { return slots_.firstMesh + live; }
    uint32_t instanceOf(uint32_t live) const { return slots_.firstInstance + live; }
    // The extra stream (mask UVs, colours: layered level meshes) after the live vertices in the
    // Vertices section, 8 bytes a vertex: its offset there, and the address the resolve reads a
    // live vertex g's at (+ g * 8; ScenePak::gpuSectionOffset included, the first live vertex taken off).
    uint32_t extraOffset() const { return (slots_.firstVertex + slots_.blocks * 128u) * 24u; }
    uint32_t extraAddress() const { return verticesOffset_ + extraOffset() - slots_.firstVertex * 8u; }
    // The live mesh of an instance slot or a mesh (-1: not live).
    int32_t liveOfInstance(uint32_t slot) const
    {
        return slot - slots_.firstInstance < slots_.meshes ? int32_t(slot - slots_.firstInstance) : -1;
    }
    int32_t liveOfMesh(uint32_t mesh) const { return slots_.ownsMesh(mesh) ? int32_t(mesh - slots_.firstMesh) : -1; }
    // Its triangles as last set (0: empty).
    uint32_t triangles(uint32_t live) const { return live < meshes_.size() ? meshes_[live].triangles : 0u; }
    // Mesh-space bounds (min, max) of a live mesh (zero when empty).
    void bounds(uint32_t live, float lo[3], float hi[3]) const;
    // A triangle of the mesh as set() got it (its index in data.indices / 3) from a cluster-local
    // one: what a pick under the cursor reports (cluster, triangle). -1: not this mesh's.
    int32_t sourceTriangle(uint32_t live, uint32_t cluster, uint32_t triangle) const;

    // Once a frame, before culling: uploads what set()/destroy() wrote. False: nothing to do.
    bool flush(SDL_GPUCommandBuffer* cmd);
    // A world region (min xyz, max xyz) whose GI must be relit, merged since the last call.
    bool takeGiChange(float region[6]);
    const LiveGeometryStats& stats() const { return stats_; }

private:
    struct Mesh {
        bool used = false;
        uint32_t firstBlock = 0, blockCount = 0;  // allocation (a run of blocks)
        uint32_t clusters = 0;                    // blocks written
        float lo[3] = {}, hi[3] = {};
        float sphere[4] = {};                     // world sphere when last set (shadow invalidation)
        uint32_t triangles = 0;
        uint32_t bvhTriangles = 0;
        std::vector<uint32_t> sourceTriangles;    // per cluster block: source triangle of each local one
    };
    struct Write {
        uint32_t section, offset, bytes, src;
    };
    bool allocate(uint32_t count, uint32_t& first);
    void release(uint32_t first, uint32_t count);
    void add(uint32_t section, uint32_t offset, const void* data, uint32_t bytes);
    void invalidate(const float sphere[4]);

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    ScenePak* colliderPak_ = nullptr;
    SceneBvh* bvh_ = nullptr;
    VirtualShadowMap* vsm_ = nullptr;
    ScenePak::LiveSlots slots_;
    uint32_t verticesOffset_ = 0;  // the Vertices section in the scene buffer (extraAddress)
    std::vector<Mesh> meshes_;
    std::vector<std::pair<uint32_t, uint32_t>> free_;  // runs of free blocks (first, count), sorted
    std::vector<Write> writes_;
    std::vector<uint8_t> staging_;
    std::vector<float4> invalidations_;
    bool chunksChanged_ = false;  // a mesh's clusters moved: the shadow map's caster chunks sent again
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    bool giChanged_ = false;
    float giBox_[6] = {};
    LiveGeometryStats stats_;
};

}  // namespace fx
