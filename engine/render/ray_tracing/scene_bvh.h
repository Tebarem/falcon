#pragma once

// The scene's ray tracing structure, for the compute-shader tracer (ray_trace.slang): one bottom
// level BVH per mesh over its finest triangles (cooked meshes at load, on the job threads; live
// meshes and terrain patches as they are written), and a top level over the instances, rebuilt
// on the CPU when the set of instances changes and refit when only their transforms do (an
// editor drag, physics, scripts: they all arrive through the renderer's instance rows).
//
// Everything lives in one storage buffer:
//   header (256 B)  uint byte offsets: TLAS nodes, instance records, BLAS nodes, BLAS triangles;
//                   then the instance count and the TLAS node count
//   TLAS nodes      bvh_builder.h's two-wide nodes; a leaf is one instance record
//   instances       64 B each: world-to-mesh rows (3 x float4), the BLAS's node and triangle bases,
//                   the scene instance it is (bit 31: mirrored), and where in its BLAS to start
// A large instance gets several records, each a subtree of its BLAS (partial re-braiding: Benthin
// et al. 2017): Sponza's meshes each span the whole building, and with one box per instance every
// ray walked dozens of them. Opened largest world box first, to about twice the instances.
//   BLAS nodes      every mesh's nodes (child indices relative to its node base)
//   BLAS triangles  every mesh's triangles (leaf ranges relative to its triangle base)
// A BLAS that changes size moves to the end of its arena; when an arena (or the TLAS) runs out of
// room the buffer is made again, larger, and everything uploaded packed.
//
// Left out: skinned instances (they receive GI, they do not block it), alpha-blended clusters,
// hidden rows. Alpha-masked triangles are kept: the tracer tests their alpha at the hit.

#include "assets/fpak_format.h"
#include "render/ray_tracing/bvh_builder.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fx {

class ScenePak;

struct SceneBvhStats {
    uint32_t meshes = 0;          // with a BLAS
    uint32_t triangles = 0;       // across every BLAS
    uint32_t instances = 0;       // in the TLAS
    uint32_t records = 0;         // their records (a large instance's BLAS opened into several)
    uint64_t bytes = 0;           // GPU buffer
    float loadBuildMs = 0.0f;     // the cooked meshes' BLAS at load (wall clock)
    float lastTlasMs = 0.0f;      // the last TLAS build or refit (CPU)
    uint32_t tlasBuilds = 0, tlasRefits = 0;
};

class SceneBvh {
public:
    static constexpr uint32_t kHeaderBytes = 256;
    static constexpr uint32_t kInstanceBytes = 64;
    static constexpr uint32_t kInstanceMirrored = 0x80000000u;  // record instance bit: the world matrix flips handedness

    // Terrain patches, which bypass the instance rows: instance slot (identity transform).
    struct ExtraInstance {
        uint32_t instance, mesh;
    };

    bool init(SDL_GPUDevice* device, const ScenePak& pak);
    void shutdown();

    // The renderer's instance rows (the GPU table's CPU mirror), read on update().
    void setInstanceRows(const std::vector<fpak::InstanceGpu>* rows) { rows_ = rows; }
    // Slots the skinning pass deforms by a vertex function (slot -> record): traced as their still
    // mesh, as physics collides with it. Other skinned instances (characters) are left out.
    void setDeformed(const std::unordered_map<uint32_t, uint32_t>* deformed) { deformed_ = deformed; }
    // A row the renderer changed: shown or hidden, another mesh, or moved.
    void rowChanged(uint32_t slot, const fpak::InstanceGpu& before, const fpak::InstanceGpu& after);
    // The terrain's patches in use (replaces the list).
    void setExtraInstances(const std::vector<ExtraInstance>& extra);
    // A mesh's BLAS built ahead (buildMesh: any thread), for setBuilt.
    struct BuiltMesh {
        std::vector<rt::BvhNodeGpu> nodes;
        std::vector<rt::BvhTriangleGpu> triangles;
        rt::Aabb bounds;
    };
    static BuiltMesh buildMesh(std::vector<rt::BvhTriangleGpu> triangles);
    // Replaces a mesh's triangles (mesh space; empty: none), building its BLAS now.
    void setMesh(uint32_t mesh, std::vector<rt::BvhTriangleGpu> triangles) { setBuilt(mesh, buildMesh(std::move(triangles))); }
    void setBuilt(uint32_t mesh, BuiltMesh built);

    // Once a frame before anything traces: builds or refits the TLAS and uploads what changed.
    void update(SDL_GPUCommandBuffer* cmd);

    SDL_GPUBuffer* buffer() const { return buffer_; }
    bool ready() const { return buffer_ != nullptr; }
    uint32_t triangles() const { return stats_.triangles; }
    const SceneBvhStats& stats() const { return stats_; }

private:
    struct Blas {
        std::vector<rt::BvhNodeGpu> nodes;
        std::vector<rt::BvhTriangleGpu> triangles;
        rt::Aabb bounds;
        uint32_t nodeBase = 0, nodeRoom = 0;  // where it sits in the arenas, and how much room it has
        uint32_t triBase = 0, triRoom = 0;
        bool uploaded = false;
    };
    struct Record {  // mirrors the GPU instance record
        float inverse[3][4];
        uint32_t nodeBase, triBase, instance, start;  // start: a BLAS node, or a leaf (bvh_builder.h's child)
    };
    static_assert(sizeof(Record) == kInstanceBytes);

    void buildBlas(Blas& b, std::vector<rt::BvhTriangleGpu>&& triangles);
    bool place(Blas& b);  // room in the arenas (false: they are full)
    bool include(uint32_t slot, const fpak::InstanceGpu& row) const;
    // The world-to-mesh rows of an instance (false: it is scaled to nothing).
    static bool inverse(const float world[3][4], Record& r);
    void buildTlas();
    void refitTlas();
    bool ensureBuffer(uint32_t tlasNodes, uint32_t instances, uint32_t blasNodes, uint32_t blasTriangles);
    void stage(uint32_t offset, const void* data, uint32_t bytes);

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    const std::vector<fpak::InstanceGpu>* rows_ = nullptr;
    const std::unordered_map<uint32_t, uint32_t>* deformed_ = nullptr;
    std::vector<ExtraInstance> extra_;
    std::vector<Blas> blas_;  // per mesh

    // TLAS: per record (in leaf order) its instance (slot, or an extra one: kExtraBit | index), the
    // record, its subtree's box in mesh space and in the world.
    static constexpr uint32_t kExtraBit = 0x80000000u;
    rt::Bvh tlas_;
    std::vector<uint32_t> members_;
    std::vector<Record> records_;
    std::vector<rt::Aabb> local_, boxes_;
    std::vector<uint8_t> inTlas_;  // per slot: has records
    std::vector<uint32_t> moved_;    // slots moved since the last update
    bool rebuild_ = true, tlasUploaded_ = false;
    uint32_t refitsSinceBuild_ = 0;

    // The buffer and its regions (in elements).
    SDL_GPUBuffer* buffer_ = nullptr;
    uint64_t bufferBytes_ = 0;
    uint32_t tlasNodeCap_ = 0, instanceCap_ = 0, blasNodeCap_ = 0, blasTriCap_ = 0;
    uint32_t tlasNodesAt_ = 0, instancesAt_ = 0, blasNodesAt_ = 0, blasTrisAt_ = 0;  // byte offsets
    uint32_t nodeTop_ = 0, triTop_ = 0;  // arena fill
    bool repack_ = false;                // everything placed and uploaded again

    struct Write {
        uint32_t offset, bytes, src;
    };
    std::vector<Write> writes_;
    std::vector<uint8_t> staging_;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    SceneBvhStats stats_;
};

}  // namespace fx
