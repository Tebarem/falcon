#pragma once

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace fx {

class GpuDevice;
class ScenePak;
struct ShaderFile;

// Mirrors SkinInstanceGpu in engine/shaders/skinning.slang.
struct SkinInstanceGpu {
    float bounds[4];  // animated bounding sphere, model space
    uint32_t firstBone, vertexBase, prevVertexBase, clusterBoundsBase;
    uint32_t firstVertex, vertexCount, firstSkinVertex, mesh;
    uint32_t clusterOffset, clusterCount;
    uint32_t hidden;  // 1: bones hidden (the collapse section has where their vertices go, for the eye)
    uint32_t pad;     // a deform record: its instance slot
};
static_assert(sizeof(SkinInstanceGpu) == 64);

// M8: GPU skinning. Every skinned instance owns a record, a range of skinning matrices, two
// generations of skinned vertices (this frame / last frame, for motion vectors) and one
// bounding sphere per cluster, all in one buffer that the culling, visibility, shadow and
// resolve passes read. Per frame the CPU writes matrices + bounds; the GPU skins every vertex,
// then re-bounds every cluster. Each skinned vertex also has a position for the eye: the same,
// but for vertices on hidden bones (a first-person head), pulled into the hidden bone's joint;
// the visibility and resolve passes draw that one, the shadows the whole character.
class SkinningSystem {
public:
    static constexpr uint32_t kHeaderBytes = 64;
    static constexpr uint32_t kVertexStride = 32;  // float3 position, oct normal, oct tangent, float3 position to the eye

    // Before pak.upload(): flags every skinned instance of the table (cooked and pooled) and
    // assigns its record. `boneCount` gives the bone count of a skeleton.
    void reserve(ScenePak& pak, const std::function<uint32_t(uint32_t skeleton)>& boneCount);
    // After reserve(), before init(): `count` floating records (the Animator's preview), each with
    // room for the largest skinned mesh of the pak and the largest skeleton. They have no instance
    // slot (instanceOfRecord: ~0u) and skin nothing until bound.
    void reserveFloating(uint32_t count, uint32_t maxBones);
    uint32_t floatingRecordCount() const { return floatingCount_; }
    uint32_t instanceRecordCount() const { return floatingCount_ ? firstFloating_ : deformCount_ ? firstDeform_ : recordCount(); }  // instance slots' records: [0, this)
    // Floating record `index` skins `mesh` from now on (its matrices: matrices(record)); the
    // record, or ~0u when the mesh is not skinned or doesn't fit.
    uint32_t bindFloating(uint32_t index, uint32_t mesh);
    void unbindFloating();  // every floating record skins nothing again
    // (V1) Deform records: instances whose material has a vertex function (vertex_deform), bound at
    // runtime from a budget of `records` records, `vertices` vertices (two generations each) and
    // `clusters` cluster bounds. Reserved after reserveFloating(), before init().
    void reserveDeform(uint32_t records, uint32_t vertices, uint32_t clusters);
    // Instance `slot` (of rigid `mesh`) deformed from now on: its record, or ~0u when the budget
    // is out (or the mesh is skinned). unbindDeform frees it.
    uint32_t bindDeform(uint32_t slot, uint32_t mesh);
    void unbindDeform(uint32_t record);
    bool isDeform(uint32_t record) const { return record >= firstDeform_ && record < firstDeform_ + deformCount_; }
    uint32_t deformBound() const { return deformBound_; }
    uint32_t deformVerticesUsed() const { return deformVerticesUsed_; }
    // This frame's: the materials (their flags and params, after the page table at paramsOffset)
    // and the time the vertex functions see.
    void setDeformInputs(SDL_GPUBuffer* materialData, uint32_t paramsOffset, float seconds)
    {
        materialData_ = materialData;
        paramsOffset_ = paramsOffset;
        seconds_ = seconds;
    }
    // The project's build of the vertex pass (its vertex functions: Cache/Shaders/deform.fshader);
    // nullptr: the engine's (vertices stay where they are).
    void setProjectDeform(const ShaderFile* file);

    bool init(const GpuDevice& gpu);
    void shutdown();

    uint32_t recordCount() const { return uint32_t(records_.size()); }
    uint32_t recordOfInstance(uint32_t slot) const;  // ~0 when not skinned
    uint32_t instanceOfRecord(uint32_t record) const { return instances_[record]; }
    uint32_t skeletonOfRecord(uint32_t record) const { return skeletons_[record]; }

    // This frame's skinning matrices of a record (3x4 rows, 12 floats per bone): model-space
    // bone transform x inverse bind. Write them, and the bounds, before record().
    float* matrices(uint32_t record) { return bones_.data() + size_t(records_[record].firstBone) * 12; }
    const float* matrices(uint32_t record) const { return bones_.data() + size_t(records_[record].firstBone) * 12; }
    // Hidden bones of a record (to the eye): per bone, where its vertices go (model space) and 1,
    // or 0 for a shown bone; read only while setHidden(record, true). Write them before record().
    float* collapse(uint32_t record) { return collapse_.data() + size_t(records_[record].firstBone) * 4; }
    const float* collapse(uint32_t record) const { return collapse_.data() + size_t(records_[record].firstBone) * 4; }
    void setHidden(uint32_t record, bool hidden) { records_[record].hidden = hidden ? 1 : 0; }
    bool hidden(uint32_t record) const { return records_[record].hidden != 0; }
    void setBounds(uint32_t record, float4 sphere);
    float4 bounds(uint32_t record) const;
    // The pose changed this frame (its shadow pages must be re-rendered).
    void markPosed(uint32_t record) { posed_[record] = 1; }
    // An inactive record (its instance hidden: an unused pool slot) is not skinned.
    void setActive(uint32_t record, bool active) { active_[record] = active ? 1 : 0; }
    // Model-space spheres around each posed record's last and current bounds; clears the flags.
    template <typename F>
    void consumePosed(F&& emit)
    {
        for (uint32_t r = 0; r < records_.size(); ++r) {
            if (!posed_[r])
                continue;
            posed_[r] = 0;
            emit(r, lastBounds_[r], bounds(r));
            lastBounds_[r] = bounds(r);
        }
    }

    // Uploads matrices + records, then runs the skinning and cluster-bounds passes.
    void record(SDL_GPUCommandBuffer* cmd);

    SDL_GPUBuffer* buffer() const { return buffer_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    SDL_GPUComputePipeline *skin_ = nullptr, *bounds_ = nullptr;
    SDL_GPUComputePipeline *deform_ = nullptr, *builtinDeform_ = nullptr, *projectDeform_ = nullptr;
    const GpuDevice* gpu_ = nullptr;
    // Deform records [firstDeform_, + deformCount_): their vertices and cluster bounds come from
    // free ranges (offset, size) of [deformVertexFirst_, ...) and [deformClusterFirst_, ...).
    uint32_t firstDeform_ = 0, deformCount_ = 0, deformBound_ = 0, deformVerticesUsed_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> freeVertices_, freeClusters_;
    SDL_GPUBuffer* materialData_ = nullptr;
    uint32_t paramsOffset_ = 0;
    float seconds_ = 0.0f;
    SDL_GPUBuffer* buffer_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    std::vector<SkinInstanceGpu> records_;
    std::vector<uint32_t> instances_, skeletons_;
    std::vector<uint32_t> recordOf_;  // per instance slot
    std::vector<float> bones_, collapse_;
    std::vector<uint8_t> posed_, active_;
    std::vector<float4> lastBounds_;
    std::vector<uint32_t> fresh_;  // deform records bound since the last record()
    uint32_t offsets_[16] = {};  // header: byte offsets of records, bones, vertices, cluster bounds, collapse
    uint32_t vertexCount_ = 0, clusterBoundsCount_ = 0, maxVertices_ = 0, maxClusters_ = 0;
    uint32_t firstFloating_ = 0, floatingCount_ = 0, floatingVertices_ = 0, floatingClusters_ = 0, floatingBones_ = 0;
    uint64_t frame_ = 0;
};

}  // namespace fx
