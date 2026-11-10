#include "render/skinning.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/shader_file.h"

#include "vertex_deform.shader.h"
#include "skin_vertices.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace fx {

namespace {

struct SkinUniforms {
    uint32_t params[4];  // record count
};

struct DeformUniforms {
    uint32_t params[4];  // first deform record, deform record count, materials' params offset
    float time[4];
};

// First fit from a list of free ranges (offset, size); merged back on release.
uint32_t takeRange(std::vector<std::pair<uint32_t, uint32_t>>& free, uint32_t size)
{
    for (auto it = free.begin(); it != free.end(); ++it) {
        if (it->second < size)
            continue;
        const uint32_t at = it->first;
        it->first += size;
        it->second -= size;
        if (!it->second)
            free.erase(it);
        return at;
    }
    return ~0u;
}

void giveRange(std::vector<std::pair<uint32_t, uint32_t>>& free, uint32_t at, uint32_t size)
{
    if (!size)
        return;
    auto it = std::lower_bound(free.begin(), free.end(), std::pair{at, 0u});
    it = free.insert(it, {at, size});
    if (it + 1 != free.end() && it->first + it->second == (it + 1)->first) {  // with the next
        it->second += (it + 1)->second;
        free.erase(it + 1);
    }
    if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first) {  // with the previous
        (it - 1)->second += it->second;
        free.erase(it);
    }
}

}  // namespace

void SkinningSystem::reserveDeform(uint32_t records, uint32_t vertices, uint32_t clusters)
{
    firstDeform_ = uint32_t(records_.size());
    deformCount_ = records;
    freeVertices_ = {{vertexCount_, 2 * vertices}};
    freeClusters_ = {{clusterBoundsCount_, clusters}};
    vertexCount_ += 2 * vertices;
    clusterBoundsCount_ += clusters;
    for (uint32_t i = 0; i < records; ++i) {
        SkinInstanceGpu r = {};
        r.firstSkinVertex = fpak::kNotSkinned;  // (cs_skin leaves it to the vertex pass)
        r.firstBone = 0;
        instances_.push_back(~0u);
        skeletons_.push_back(~0u);
        records_.push_back(r);
    }
    posed_.resize(records_.size(), 0);
    active_.resize(records_.size(), 1);
    lastBounds_.resize(records_.size(), float4{0, 0, 0, 0});
}

uint32_t SkinningSystem::bindDeform(uint32_t slot, uint32_t mesh)
{
    uint32_t meshCount = 0, skinCount = 0;
    const auto* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* skins = pak_->view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    if (mesh >= pak_->meshes().size() || (mesh < skinCount && skins[mesh].firstSkinVertex != fpak::kNotSkinned))
        return ~0u;
    uint32_t record = ~0u;
    for (uint32_t i = 0; i < deformCount_ && record == ~0u; ++i)
        if (instances_[firstDeform_ + i] == ~0u)
            record = firstDeform_ + i;
    if (record == ~0u)
        return ~0u;
    const fpak::MeshGpu& m = mesh < meshCount ? meshes[mesh] : pak_->meshes()[mesh];
    // The mesh's vertices: the range its clusters use (the cooker keeps a mesh's vertices together).
    uint32_t clusterCount = 0;
    const fpak::ClusterGpu* clusters = pak_->view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
    uint32_t vertexCount = 0;
    const uint32_t* clusterVertices = pak_->view<uint32_t>(fpak::Section::ClusterVertices, &vertexCount);
    uint32_t lo = ~0u, hi = 0;
    for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount && c < clusterCount; ++c)
        for (uint32_t k = 0; k < (clusters[c].counts & 0xff) && clusters[c].vertexOffset + k < vertexCount; ++k) {
            lo = std::min(lo, clusterVertices[clusters[c].vertexOffset + k]);
            hi = std::max(hi, clusterVertices[clusters[c].vertexOffset + k]);
        }
    if (lo > hi)
        return ~0u;
    const uint32_t vertices = hi - lo + 1;
    const uint32_t at = takeRange(freeVertices_, 2 * vertices);
    if (at == ~0u)
        return ~0u;
    const uint32_t bounds = takeRange(freeClusters_, m.clusterCount);
    if (bounds == ~0u) {
        giveRange(freeVertices_, at, 2 * vertices);
        return ~0u;
    }
    SkinInstanceGpu& r = records_[record];
    r = {};
    const float3 half = float3{m.posScale[0], m.posScale[1], m.posScale[2]} * (65535.0f * 0.5f);
    const float3 c = float3{m.posOffset[0], m.posOffset[1], m.posOffset[2]} + half;
    r.bounds[0] = c.x, r.bounds[1] = c.y, r.bounds[2] = c.z, r.bounds[3] = length(half);
    r.vertexBase = at;
    r.prevVertexBase = at + vertices;
    r.clusterBoundsBase = bounds;
    r.firstVertex = lo;
    r.vertexCount = vertices;
    r.firstSkinVertex = fpak::kNotSkinned;
    r.mesh = mesh;
    r.clusterOffset = m.clusterOffset;
    r.clusterCount = m.clusterCount;
    r.pad = slot;
    instances_[record] = slot;
    if (slot < recordOf_.size())
        recordOf_[slot] = record;
    fresh_.push_back(record);
    maxClusters_ = std::max(maxClusters_, m.clusterCount);
    ++deformBound_;
    deformVerticesUsed_ += vertices;
    posed_[record] = 1;
    lastBounds_[record] = {r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]};
    return record;
}

void SkinningSystem::unbindDeform(uint32_t record)
{
    if (!isDeform(record) || instances_[record] == ~0u)
        return;
    SkinInstanceGpu& r = records_[record];
    giveRange(freeVertices_, std::min(r.vertexBase, r.prevVertexBase), 2 * r.vertexCount);
    giveRange(freeClusters_, r.clusterBoundsBase, r.clusterCount);
    deformVerticesUsed_ -= r.vertexCount;
    --deformBound_;
    if (instances_[record] < recordOf_.size())
        recordOf_[instances_[record]] = ~0u;
    instances_[record] = ~0u;
    r = {};
    r.firstSkinVertex = fpak::kNotSkinned;
    posed_[record] = 0;
}

void SkinningSystem::setProjectDeform(const ShaderFile* file)
{
    const ShaderFile::Entry* entry = file ? file->entry("cs_deform") : nullptr;
    SDL_GPUComputePipeline* made = entry && gpu_ ? gpu_->createComputePipeline(entry->blob) : nullptr;
    if (file && !made)
        FX_WARN("skinning: the project's vertex pass made no pipeline (the engine's is used)");
    if (projectDeform_)
        SDL_ReleaseGPUComputePipeline(device_, projectDeform_);  // (released once the frames using it are done)
    projectDeform_ = made;
    deform_ = made ? made : builtinDeform_;
}

void SkinningSystem::reserve(ScenePak& pak, const std::function<uint32_t(uint32_t)>& boneCount)
{
    pak_ = &pak;
    uint32_t meshCount = 0, skinCount = 0;
    const auto* meshes = pak.view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    auto& table = pak.mutableInstances();
    recordOf_.assign(table.size(), ~0u);
    uint32_t bones = 0;
    for (uint32_t slot = 0; slot < table.size(); ++slot) {
        fpak::InstanceGpu& inst = table[slot];
        if (inst.mesh >= skinCount || skins[inst.mesh].firstSkinVertex == fpak::kNotSkinned)
            continue;
        const fpak::MeshSkinGpu& s = skins[inst.mesh];
        const fpak::MeshGpu& m = meshes[inst.mesh];
        SkinInstanceGpu r = {};
        const float3 half = float3{m.posScale[0], m.posScale[1], m.posScale[2]} * (65535.0f * 0.5f);
        const float3 c = float3{m.posOffset[0], m.posOffset[1], m.posOffset[2]} + half;
        r.bounds[0] = c.x, r.bounds[1] = c.y, r.bounds[2] = c.z, r.bounds[3] = length(half);
        r.firstBone = bones;
        r.vertexBase = vertexCount_;
        r.prevVertexBase = vertexCount_ + s.vertexCount;
        r.clusterBoundsBase = clusterBoundsCount_;
        r.firstVertex = s.firstVertex;
        r.vertexCount = s.vertexCount;
        r.firstSkinVertex = s.firstSkinVertex;
        r.mesh = inst.mesh;
        r.clusterOffset = m.clusterOffset;
        r.clusterCount = m.clusterCount;
        bones += boneCount(s.skeleton);
        vertexCount_ += 2 * s.vertexCount;
        clusterBoundsCount_ += m.clusterCount;
        maxVertices_ = std::max(maxVertices_, s.vertexCount);
        maxClusters_ = std::max(maxClusters_, m.clusterCount);

        inst.flags |= fpak::kInstanceSkinned;
        inst.skin = uint32_t(records_.size());
        recordOf_[slot] = uint32_t(records_.size());
        instances_.push_back(slot);
        skeletons_.push_back(s.skeleton);
        records_.push_back(r);
    }
    posed_.assign(records_.size(), 1);
    active_.assign(records_.size(), 1);
    lastBounds_.clear();
    for (const SkinInstanceGpu& r : records_)
        lastBounds_.push_back({r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]});
    // Identity matrices until the animation system writes poses.
    bones_.assign(size_t(bones) * 12, 0.0f);
    for (uint32_t b = 0; b < bones; ++b)
        bones_[b * 12 + 0] = bones_[b * 12 + 5] = bones_[b * 12 + 10] = 1.0f;
}

void SkinningSystem::reserveFloating(uint32_t count, uint32_t maxBones)
{
    firstFloating_ = uint32_t(records_.size());
    uint32_t meshCount = 0, skinCount = 0;
    const auto* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* skins = pak_->view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    for (uint32_t m = 0; m < std::min(meshCount, skinCount); ++m)
        if (skins[m].firstSkinVertex != fpak::kNotSkinned) {
            floatingVertices_ = std::max(floatingVertices_, skins[m].vertexCount);
            floatingClusters_ = std::max(floatingClusters_, meshes[m].clusterCount);
        }
    if (!floatingVertices_ || !count)
        return;
    floatingCount_ = count;
    floatingBones_ = std::max(maxBones, 1u);
    uint32_t bones = uint32_t(bones_.size() / 12);
    for (uint32_t i = 0; i < count; ++i) {
        SkinInstanceGpu r = {};
        r.firstBone = bones;
        r.vertexBase = r.prevVertexBase = vertexCount_;
        r.clusterBoundsBase = clusterBoundsCount_;
        bones += floatingBones_;
        vertexCount_ += 2 * floatingVertices_;
        clusterBoundsCount_ += floatingClusters_;
        instances_.push_back(~0u);
        skeletons_.push_back(~0u);
        records_.push_back(r);
    }
    maxVertices_ = std::max(maxVertices_, floatingVertices_);
    maxClusters_ = std::max(maxClusters_, floatingClusters_);
    posed_.resize(records_.size(), 0);  // (never invalidates shadow pages: not in the scene)
    active_.resize(records_.size(), 1);
    lastBounds_.resize(records_.size(), float4{0, 0, 0, 0});
    const size_t first = bones_.size();
    bones_.resize(size_t(bones) * 12, 0.0f);
    for (size_t b = first / 12; b < bones; ++b)
        bones_[b * 12 + 0] = bones_[b * 12 + 5] = bones_[b * 12 + 10] = 1.0f;
}

uint32_t SkinningSystem::bindFloating(uint32_t index, uint32_t mesh)
{
    uint32_t meshCount = 0, skinCount = 0;
    const auto* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* skins = pak_->view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    if (index >= floatingCount_ || mesh >= std::min(meshCount, skinCount) || skins[mesh].firstSkinVertex == fpak::kNotSkinned)
        return ~0u;
    const fpak::MeshSkinGpu& s = skins[mesh];
    const fpak::MeshGpu& m = meshes[mesh];
    const uint32_t record = firstFloating_ + index;
    SkinInstanceGpu& r = records_[record];
    // Its own ranges stay; the two vertex generations swap each frame, so take the lower as base.
    const uint32_t base = std::min(r.vertexBase, r.prevVertexBase);
    r.vertexBase = base;
    r.prevVertexBase = base + s.vertexCount;
    const float3 half = float3{m.posScale[0], m.posScale[1], m.posScale[2]} * (65535.0f * 0.5f);
    const float3 c = float3{m.posOffset[0], m.posOffset[1], m.posOffset[2]} + half;
    r.bounds[0] = c.x, r.bounds[1] = c.y, r.bounds[2] = c.z, r.bounds[3] = length(half);
    r.firstVertex = s.firstVertex;
    r.vertexCount = s.vertexCount;
    r.firstSkinVertex = s.firstSkinVertex;
    r.mesh = mesh;
    r.clusterOffset = m.clusterOffset;
    r.clusterCount = m.clusterCount;
    skeletons_[record] = s.skeleton;
    return record;
}

void SkinningSystem::unbindFloating()
{
    for (uint32_t i = 0; i < floatingCount_; ++i) {
        SkinInstanceGpu& r = records_[firstFloating_ + i];
        r.vertexCount = r.clusterCount = 0;
        skeletons_[firstFloating_ + i] = ~0u;
    }
}

uint32_t SkinningSystem::recordOfInstance(uint32_t slot) const
{
    return slot < recordOf_.size() ? recordOf_[slot] : ~0u;
}

void SkinningSystem::setBounds(uint32_t record, float4 sphere)
{
    SkinInstanceGpu& r = records_[record];
    r.bounds[0] = sphere.x, r.bounds[1] = sphere.y, r.bounds[2] = sphere.z, r.bounds[3] = sphere.w;
}

float4 SkinningSystem::bounds(uint32_t record) const
{
    const SkinInstanceGpu& r = records_[record];
    return {r.bounds[0], r.bounds[1], r.bounds[2], r.bounds[3]};
}

bool SkinningSystem::init(const GpuDevice& gpu)
{
    FX_ZONE_CAT("SkinningSystem::init", Rendering);
    device_ = gpu.get();
    gpu_ = &gpu;
    skin_ = gpu.createComputePipeline(shaders::skin_vertices_cs_skin);
    bounds_ = gpu.createComputePipeline(shaders::skin_vertices_cs_skin_bounds);
    deform_ = builtinDeform_ = gpu.createComputePipeline(shaders::vertex_deform_cs_deform);
    if (!skin_ || !bounds_ || !deform_)
        return false;

    // Layout: header | records | matrices | collapse | vertices (2 generations) | cluster bounds.
    // The CPU writes [0, vertices) each frame.
    collapse_.assign(bones_.size() / 12 * 4, 0.0f);
    auto align = [](uint64_t v) { return uint32_t((v + 255) & ~uint64_t(255)); };
    offsets_[0] = kHeaderBytes;
    offsets_[1] = align(offsets_[0] + records_.size() * sizeof(SkinInstanceGpu));
    offsets_[4] = align(offsets_[1] + bones_.size() * sizeof(float));
    offsets_[2] = align(offsets_[4] + collapse_.size() * sizeof(float));
    offsets_[3] = align(offsets_[2] + uint64_t(vertexCount_) * kVertexStride);
    const uint32_t total = align(offsets_[3] + uint64_t(clusterBoundsCount_) * 16) + 256;

    SDL_GPUBufferCreateInfo b = {};
    b.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
              SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    b.size = total;
    buffer_ = gpumem::createBuffer("Skinning", device_, &b);
    // Per frame: header + records + matrices.
    const SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, std::max(offsets_[2], 256u), 0};
    upload_ = gpumem::createTransferBuffer("Skinning", device_, &t);
    if (!buffer_ || !upload_)
        return false;
    gpumem::setBufferName(device_, buffer_, "skinning (records, matrices, vertices, cluster bounds)");
    if (instanceRecordCount())
        FX_INFO("skinning: %u skinned instances, %zu bones, %u skinned vertices (x2, with the vertex functions' budget), %.1f MB",
                instanceRecordCount(), bones_.size() / 12, vertexCount_ / 2, double(total) / (1 << 20));
    return true;
}

void SkinningSystem::shutdown()
{
    if (!device_)
        return;
    SDL_ReleaseGPUComputePipeline(device_, skin_);
    SDL_ReleaseGPUComputePipeline(device_, bounds_);
    for (SDL_GPUComputePipeline* p : {builtinDeform_, projectDeform_})
        if (p)
            SDL_ReleaseGPUComputePipeline(device_, p);
    deform_ = builtinDeform_ = projectDeform_ = nullptr;
    gpumem::releaseBuffer(device_, buffer_);
    gpumem::releaseTransferBuffer(device_, upload_);
    device_ = nullptr;
}

void SkinningSystem::record(SDL_GPUCommandBuffer* cmd)
{
    FX_ZONE_CAT("SkinningSystem::record", Rendering);
    // Alternate the two vertex generations: this frame writes where last frame's previous was.
    // The first frame has no history: both point at the same vertices.
    for (SkinInstanceGpu& r : records_) {
        if (frame_ > 0)
            std::swap(r.vertexBase, r.prevVertexBase);
    }
    std::vector<SkinInstanceGpu> upload = records_;
    if (frame_ == 0)
        for (SkinInstanceGpu& r : upload)
            r.prevVertexBase = r.vertexBase;
    for (const uint32_t r : fresh_)  // (just bound: no last frame yet)
        upload[r].prevVertexBase = upload[r].vertexBase;
    fresh_.clear();
    for (size_t i = 0; i < upload.size(); ++i)
        if (!active_[i])
            upload[i].vertexCount = upload[i].clusterCount = 0;  // both passes skip it

    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    std::memcpy(mapped, offsets_, sizeof(offsets_));
    std::memcpy(mapped + offsets_[0], upload.data(), upload.size() * sizeof(SkinInstanceGpu));
    std::memcpy(mapped + offsets_[1], bones_.data(), bones_.size() * sizeof(float));
    std::memcpy(mapped + offsets_[4], collapse_.data(), collapse_.size() * sizeof(float));
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_SECTION(cmd, "Skinning");
    FX_GPU_PASS(cmd, "skin upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {upload_, 0};
    const SDL_GPUBufferRegion dst = {buffer_, 0, records_.empty() ? uint32_t(sizeof(offsets_)) : offsets_[2]};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    ++frame_;
    if (records_.empty())
        return;

    const SkinUniforms u = {{uint32_t(records_.size()), 0, 0, 0}};
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    SDL_GPUBuffer* scene = pak_->sceneBuffer();
    const SDL_GPUStorageBufferReadWriteBinding rw = {buffer_, false};
    // The vertex pass between the two (V1): bound deform records' vertices, before their bounds.
    auto deform = [&] {
        if (!deformBound_ || !materialData_)
            return;
        const DeformUniforms du = {{firstDeform_, deformCount_, paramsOffset_, 0}, {seconds_, 0.0f, 0.0f, 0.0f}};
        SDL_PushGPUComputeUniformData(cmd, 0, &du, sizeof(du));
        gpuprof::pass(cmd, "vertex functions");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, deform_);
        SDL_GPUBuffer* ro[2] = {scene, materialData_};
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);
        SDL_DispatchGPUCompute(pass, maxClusters_, deformCount_, 1);
        SDL_EndGPUComputePass(pass);
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    };
    for (SDL_GPUComputePipeline* p : {skin_, bounds_}) {
        if (p == bounds_)
            deform();
        gpuprof::pass(cmd, p == skin_ ? "skin vertices" : "skinned cluster bounds");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, p);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &scene, 1);
        const uint32_t items = p == skin_ ? maxVertices_ : maxClusters_;
        SDL_DispatchGPUCompute(pass, (items + 63) / 64, uint32_t(records_.size()), 1);
        SDL_EndGPUComputePass(pass);
    }
}

}  // namespace fx
