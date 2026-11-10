#include "assets/scene_pak.h"

#include "core/boot.h"
#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"

#include <SDL3/SDL.h>
#include <box3d/collision.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace fx {

namespace {

constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kSectionAlign = 256;

}  // namespace

bool ScenePak::beginRead(const char* path)
{
    path_ = path;
    queue_ = SDL_CreateAsyncIOQueue();
    if (!queue_ || !SDL_LoadFileAsync(path, queue_, nullptr)) {
        FX_ERROR("scene: cannot start reading %s: %s", path, SDL_GetError());
        return false;
    }
    return true;
}

bool ScenePak::readDone()
{
    if (queue_ && !readOutcome_)
        readOutcome_ = SDL_GetAsyncIOResult(queue_, &outcome_);
    return !queue_ || readOutcome_;
}

bool ScenePak::finishRead()
{
    FX_ZONE("ScenePak::finishRead");
    if (!queue_)
        return false;
    SDL_AsyncIOOutcome outcome = outcome_;
    const bool got = readOutcome_ || SDL_WaitAsyncIOResult(queue_, &outcome, -1);
    readOutcome_ = false;
    SDL_DestroyAsyncIOQueue(queue_);
    queue_ = nullptr;
    if (!got || outcome.result != SDL_ASYNCIO_COMPLETE) {
        FX_ERROR("scene: reading %s failed: %s", path_, SDL_GetError());
        SDL_free(outcome.buffer);
        return false;
    }
    data_ = outcome.buffer;
    size_ = size_t(outcome.bytes_transferred);
    if (!validate()) {
        SDL_free(data_);
        data_ = nullptr;
        return false;
    }
    // Bounds that are inverted, not finite or empty (a scene with nothing placed, from an older
    // cooker): a 100 x 20 x 100 m box at the origin, so everything sized by them stays sane.
    {
        fpak::Header& h = *static_cast<fpak::Header*>(data_);
        bool sane = true;
        for (int a = 0; a < 3; ++a)
            sane = sane && std::isfinite(h.boundsMin[a]) && std::isfinite(h.boundsMax[a]) && h.boundsMin[a] <= h.boundsMax[a];
        if (!sane) {
            const float lo[3] = {-50.0f, -1.0f, -50.0f}, hi[3] = {50.0f, 19.0f, 50.0f};
            std::memcpy(h.boundsMin, lo, sizeof(lo));
            std::memcpy(h.boundsMax, hi, sizeof(hi));
        }
    }
    uint32_t count = 0;
    const fpak::InstanceGpu* cooked = view<fpak::InstanceGpu>(fpak::Section::Instances, &count);
    instances_.assign(cooked, cooked + count);
    cookedInstances_ = count;
    const auto* meshes = view<fpak::MeshGpu>(fpak::Section::Meshes, &count);
    meshes_.assign(meshes, meshes + count);
    const auto* materials = view<fpak::MaterialGpu>(fpak::Section::Materials, &count);
    materials_.assign(materials, materials + count);
    // Meshes with a glowing (emissive) material: small-instance culling spares them (cluster_cull),
    // as a light strip a few pixels wide, far off, still shows (and blooms).
    {
        uint32_t clusterCount = 0;
        const auto* clusters = view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
        for (fpak::MeshGpu& m : meshes_)
            for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount && c < clusterCount; ++c) {
                const uint32_t material = clusters[c].counts >> 16;
                if (material < materials_.size()) {
                    const float* e = materials_[material].emissive;
                    if (std::max({e[0], e[1], e[2]}) * e[3] > 0.0f) {
                        m.posOffset[3] = 1.0f;
                        break;
                    }
                }
            }
    }
    readLibraries();
    readNodeAnimation();
    boot::mark("scene file read");
    return true;
}

// The Names section ends: ... [metadata JSON, padded][size]["FXMD"] [mesh name offsets][count]["MSHN"].
void ScenePak::readLibraries()
{
    libraries_.clear();
    materialSources_.clear();
    const fpak::SectionEntry& e = header().sections[uint32_t(fpak::Section::Names)];
    const char* base = view<char>(fpak::Section::Names);
    if (e.size < 16 || std::memcmp(base + e.size - 4, "MSHN", 4))
        return;
    uint32_t count = 0, metaSize = 0;
    std::memcpy(&count, base + e.size - 8, 4);
    const uint64_t offsets = e.size - 8 - uint64_t(count) * 4;
    if (offsets > e.size || offsets < 8 || std::memcmp(base + offsets - 4, "FXMD", 4))
        return;
    std::memcpy(&metaSize, base + offsets - 8, 4);
    const uint64_t padded = (uint64_t(metaSize) + 3) & ~uint64_t(3);
    if (padded + 8 > offsets)
        return;
    try {
        const Json meta = Json::parse(std::string_view(base + offsets - 8 - padded, metaSize));
        if (const Json* mats = meta.get("materials"))
            for (const Json& m : mats->array)
                materialSources_.push_back({m.getString("source"), uint32_t(m.getNumber("index"))});
        if (const Json* libs = meta.get("libraries"))
            for (const Json& l : libs->array) {
                Library lib;
                lib.source = l.getString("source");
                if (const Json* nodes = l.get("nodes"))
                    for (const Json& n : nodes->array) {
                        LibraryNode node;
                        node.mesh = uint32_t(n.getNumber("mesh"));
                        node.name = n.getString("name");
                        const Json* w = n.get("world");
                        if (!w || w->array.size() != 12)
                            continue;
                        for (int i = 0; i < 12; ++i)
                            node.world[i / 4][i % 4] = float(w->array[size_t(i)].number);
                        lib.nodes.push_back(std::move(node));
                    }
                if (const Json* cams = l.get("cameras"))
                    for (const Json& c : cams->array) {
                        LibraryCamera cam;
                        cam.name = c.getString("name", "Camera");
                        cam.yfov = float(c.getNumber("yfov", 0.8));
                        const Json* w = c.get("world");
                        if (!w || w->array.size() != 12)
                            continue;
                        for (int i = 0; i < 12; ++i)
                            cam.world[i / 4][i % 4] = float(w->array[size_t(i)].number);
                        lib.cameras.push_back(std::move(cam));
                    }
                libraries_.push_back(std::move(lib));
            }
    } catch (const std::exception& ex) {
        FX_WARN("scene: library metadata is malformed (%s)", ex.what());
        libraries_.clear();
    }
}

void ScenePak::readNodeAnimation()
{
    nodeTracks_.clear();
    const fpak::SectionEntry& e = header().sections[uint32_t(fpak::Section::NodeAnimation)];
    if (e.size < sizeof(fpak::NodeAnimHeader))
        return;
    const uint8_t* base = view<uint8_t>(fpak::Section::NodeAnimation);
    fpak::NodeAnimHeader h;
    std::memcpy(&h, base, sizeof(h));
    const uint64_t rowsAt = sizeof(h) + uint64_t(h.trackCount) * sizeof(fpak::NodeTrackEntry);
    if (rowsAt + uint64_t(h.floatCount) * 4 > e.size) {
        FX_WARN("scene: node animation is malformed");
        return;
    }
    const float* rows = reinterpret_cast<const float*>(base + rowsAt);
    nodeFps_ = h.fps > 0.0f ? h.fps : 30.0f;
    for (uint32_t i = 0; i < h.trackCount; ++i) {
        fpak::NodeTrackEntry t;
        std::memcpy(&t, base + sizeof(h) + i * sizeof(t), sizeof(t));
        if (t.count == 0 || uint64_t(t.offset) + uint64_t(t.count) * 12 > h.floatCount)
            continue;
        nodeTracks_.push_back({t.library, t.target, (t.flags & fpak::kNodeTrackCamera) != 0, t.first, t.count, t.loop, rows + t.offset});
    }
}

const ScenePak::NodeTrack* ScenePak::nodeTrack(uint32_t library, uint32_t target, bool camera) const
{
    for (const NodeTrack& t : nodeTracks_)
        if (t.library == library && t.target == target && t.camera == camera)
            return &t;
    return nullptr;
}

int32_t ScenePak::libraryIndex(const std::string& source) const
{
    for (size_t l = 0; l < libraries_.size(); ++l)
        if (libraries_[l].source == source)
            return int32_t(l);
    return -1;
}

const ScenePak::Library* ScenePak::library(const std::string& source) const
{
    for (const Library& l : libraries_)
        if (l.source == source)
            return &l;
    return nullptr;
}

int32_t ScenePak::findMaterial(const std::string& source) const
{
    for (const auto& [s, index] : materialSources_)
        if (s == source)
            return int32_t(index);
    return -1;
}

uint32_t ScenePak::addMaterials(uint32_t count)
{
    const uint32_t first = uint32_t(materials_.size());
    fpak::MaterialGpu m = {};
    m.baseColor[0] = m.baseColor[1] = m.baseColor[2] = m.baseColor[3] = 1.0f;
    m.roughness = 0.5f;
    m.alphaCutoff = 0.5f;
    m.vtPageTable = fpak::kVtNoTexture;
    m.normalScale = m.occlusionStrength = 1.0f;
    materials_.insert(materials_.end(), count, m);
    return first;
}

bool ScenePak::setMaterial(uint32_t index, const fpak::MaterialGpu& m)
{
    if (index >= materials_.size())
        return false;
    materials_[index] = m;
    return true;
}

std::string ScenePak::meshSource(uint32_t mesh) const
{
    for (const Library& l : libraries_)
        for (const LibraryNode& n : l.nodes)
            if (n.mesh == mesh)
                return l.source;
    return {};
}

const ScenePak::TerrainSlots& ScenePak::reserveTerrain(uint32_t patches)
{
    if (terrain_.patches || !patches)
        return terrain_;
    const fpak::Header& h = header();
    terrain_.patches = patches;
    terrain_.firstInstance = uint32_t(instances_.size());
    terrain_.firstMesh = uint32_t(meshes_.size());
    terrain_.firstCluster = h.sections[uint32_t(fpak::Section::Clusters)].count;
    terrain_.firstVertex = h.sections[uint32_t(fpak::Section::Vertices)].count;
    terrain_.firstClusterVertex = h.sections[uint32_t(fpak::Section::ClusterVertices)].count;
    terrain_.firstTriangle = h.sections[uint32_t(fpak::Section::ClusterTriangles)].count;
    terrain_.material = uint32_t(materials_.size());

    fpak::MaterialGpu m = {};
    m.baseColor[0] = m.baseColor[1] = m.baseColor[2] = m.baseColor[3] = 1.0f;
    m.roughness = 0.9f;
    m.alphaCutoff = 0.5f;
    m.flags = fpak::kMaterialTerrain;
    m.vtPageTable = fpak::kVtNoTexture;
    m.normalScale = 1.0f;
    m.occlusionStrength = 1.0f;
    materials_.push_back(m);
    fpak::InstanceGpu hidden = {};
    hidden.world[0][0] = hidden.world[1][1] = hidden.world[2][2] = 1.0f;
    hidden.flags = fpak::kInstanceHidden;
    for (uint32_t p = 0; p < patches; ++p) {
        hidden.mesh = terrain_.firstMesh + p;
        instances_.push_back(hidden);
        fpak::MeshGpu mesh = {};
        mesh.posScale[0] = mesh.posScale[1] = mesh.posScale[2] = 1.0f / 65535.0f;
        mesh.clusterOffset = terrain_.firstCluster + p;
        mesh.clusterCount = 1;
        meshes_.push_back(mesh);
    }
    return terrain_;
}

const ScenePak::LiveSlots& ScenePak::reserveLive(uint32_t meshes, uint32_t blocks)
{
    if (live_.meshes || !meshes || !blocks)
        return live_;
    // After the cooked rows and the terrain's patches (reserveTerrain first, if at all).
    const fpak::Header& h = header();
    const uint32_t p = terrain_.patches;
    live_.meshes = meshes;
    live_.blocks = blocks;
    live_.firstInstance = uint32_t(instances_.size());
    live_.firstMesh = uint32_t(meshes_.size());
    live_.firstCluster = h.sections[uint32_t(fpak::Section::Clusters)].count + p;
    live_.firstVertex = h.sections[uint32_t(fpak::Section::Vertices)].count + p * kPatchVertices;
    live_.firstClusterVertex = h.sections[uint32_t(fpak::Section::ClusterVertices)].count + p * kPatchVertices;
    live_.firstTriangle = h.sections[uint32_t(fpak::Section::ClusterTriangles)].count + p * kPatchTriangles;
    fpak::InstanceGpu hidden = {};
    hidden.world[0][0] = hidden.world[1][1] = hidden.world[2][2] = 1.0f;
    hidden.flags = fpak::kInstanceHidden;
    for (uint32_t m = 0; m < meshes; ++m) {
        hidden.mesh = live_.firstMesh + m;
        instances_.push_back(hidden);
        fpak::MeshGpu mesh = {};  // no clusters until LiveGeometry writes it
        mesh.posScale[0] = mesh.posScale[1] = mesh.posScale[2] = 1.0f / 65535.0f;
        meshes_.push_back(mesh);
    }
    liveColliders_.assign(meshes, nullptr);
    liveRevisions_.assign(meshes, 0);
    return live_;
}

void ScenePak::setLiveCollider(uint32_t mesh, b3MeshData* collider)
{
    if (!live_.ownsMesh(mesh)) {
        if (collider)
            b3DestroyMesh(collider);
        return;
    }
    b3MeshData*& slot = liveColliders_[mesh - live_.firstMesh];
    if (slot)
        b3DestroyMesh(slot);
    slot = collider;
    bumpLive(mesh);
}

uint64_t ScenePak::gpuSectionSize(uint32_t s) const
{
    using S = fpak::Section;
    if (!fpak::sectionOnGpu(S(s)))
        return 0;
    const uint64_t cooked = header().sections[s].size, p = terrain_.patches, b = live_.blocks;
    switch (S(s)) {
    case S::Instances: return instances_.size() * sizeof(fpak::InstanceGpu);
    case S::Meshes: return meshes_.size() * sizeof(fpak::MeshGpu);
    case S::Materials: return materials_.size() * sizeof(fpak::MaterialGpu);
    // (then the live blocks' extra stream: 8 bytes a vertex, render/live_geometry.h extraOffset)
    case S::Vertices: return cooked + (p * kPatchVertices + b * kLiveBlockVertices) * sizeof(fpak::VertexGpu) + b * kLiveBlockVertices * 8;
    case S::ClusterVertices: return cooked + (p * kPatchVertices + b * kLiveBlockVertices) * 4;
    case S::ClusterTriangles: return cooked + (p * kPatchTriangles + b * kLiveBlockTriangles) * 4;
    case S::Clusters: return cooked + (p + b) * sizeof(fpak::ClusterGpu);
    default: return cooked;
    }
}

uint32_t ScenePak::addInstancePool(uint32_t mesh, uint32_t count)
{
    const uint32_t first = uint32_t(instances_.size());
    fpak::InstanceGpu hidden = {};
    hidden.world[0][0] = hidden.world[1][1] = hidden.world[2][2] = 1.0f;
    hidden.mesh = mesh;
    hidden.flags = fpak::kInstanceHidden;
    instances_.resize(instances_.size() + count, hidden);
    return first;
}

std::string ScenePak::meshName(uint32_t mesh) const
{
    const fpak::SectionEntry& e = header().sections[uint32_t(fpak::Section::Names)];
    const char* base = view<char>(fpak::Section::Names);
    const uint32_t meshes = cookedMeshCount();
    // The cooker's trailer: offsets, count, "MSHN".
    if (e.size >= 8 && !std::memcmp(base + e.size - 4, "MSHN", 4)) {
        uint32_t count, at;
        std::memcpy(&count, base + e.size - 8, 4);
        if (count == meshes && mesh < count && e.size >= 8 + uint64_t(count) * 4) {
            std::memcpy(&at, base + e.size - 8 - uint64_t(count) * 4 + mesh * 4, 4);
            if (at < e.size)
                return base + at;
        }
    }
    for (uint32_t i = 0; i < cookedInstances_; ++i)
        if (instances_[i].mesh == mesh)
            if (const char* n = instanceName(i))
                return n;
    for (uint32_t k = 0; k < uint32_t(fpak::Builtin::Count); ++k)
        if (header().builtinMeshes[k] == mesh)
            return fpak::kBuiltinNames[k];
    return "Mesh " + std::to_string(mesh);
}

int32_t ScenePak::findMesh(const std::string& name) const
{
    for (uint32_t m = 0; m < cookedMeshCount(); ++m)
        if (meshName(m) == name)
            return int32_t(m);
    return -1;
}

const b3MeshData* ScenePak::staticCollider(uint32_t mesh) const
{
    if (live_.ownsMesh(mesh))
        return liveColliders_[mesh - live_.firstMesh];
    uint32_t count = 0;
    const auto* colliders = view<fpak::MeshCollider>(fpak::Section::MeshColliders, &count);
    if (mesh >= count || !colliders[mesh].meshBytes)
        return nullptr;
    return reinterpret_cast<const b3MeshData*>(view<uint8_t>(fpak::Section::Colliders) + colliders[mesh].meshOffset);
}

const b3HullData* ScenePak::hullCollider(uint32_t mesh) const
{
    uint32_t count = 0;
    const auto* colliders = view<fpak::MeshCollider>(fpak::Section::MeshColliders, &count);
    if (mesh >= count || !colliders[mesh].hullBytes)
        return nullptr;
    return reinterpret_cast<const b3HullData*>(view<uint8_t>(fpak::Section::Colliders) + colliders[mesh].hullOffset);
}

bool ScenePak::validate() const
{
    if (size_ < sizeof(fpak::Header) || std::memcmp(header().magic, fpak::kMagic, 4) != 0) {
        FX_ERROR("scene: %s is not an .fpak", path_);
        return false;
    }
    if (header().version != fpak::kVersion) {
        FX_ERROR("scene: %s has version %u, expected %u (re-cook it)", path_, header().version, fpak::kVersion);
        return false;
    }
    for (const fpak::SectionEntry& e : header().sections)
        if (e.offset + e.size > size_ || (e.count && e.size != uint64_t(e.count) * e.stride)) {
            FX_ERROR("scene: %s has a corrupt section table", path_);
            return false;
        }
    return true;
}

bool ScenePak::upload(SDL_GPUDevice* device)
{
    FX_ZONE("ScenePak::upload");
    const fpak::Header& h = header();
    uint32_t offsets[kHeaderBytes / 4] = {};
    uint64_t total = kHeaderBytes;
    static_assert(uint32_t(fpak::Section::Count) <= kHeaderBytes / 4);
    // GPU sections only; instances, meshes, materials and the geometry sections include what was
    // added at runtime (pools, terrain slots) after the cooked data.
    auto gpuSize = [&](uint32_t s) { return gpuSectionSize(s); };
    for (uint32_t s = 0; s < uint32_t(fpak::Section::Count); ++s) {
        total = (total + kSectionAlign - 1) / kSectionAlign * kSectionAlign;
        offsets[s] = uint32_t(total);
        total += gpuSize(s);
    }
    total = std::max<uint64_t>(total, 16);

    SDL_GPUBufferCreateInfo binfo = {};
    binfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                  SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;  // (scatter writes instance rows)
    binfo.size = uint32_t(total);
    scene_ = gpumem::createBuffer("Scene", device, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(total), 0};
    SDL_GPUTransferBuffer* staging = gpumem::createTransferBuffer("Scene", device, &tinfo);
    uint8_t* mapped = staging ? static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, staging, false)) : nullptr;
    if (!scene_ || !mapped) {
        FX_ERROR("scene: buffer (%llu bytes): %s", (unsigned long long)total, SDL_GetError());
        if (staging)
            gpumem::releaseTransferBuffer(device, staging);
        return false;
    }
    gpumem::setBufferName(device, scene_, "scene (all .fpak sections)");
    std::memset(mapped, 0, size_t(total));
    std::memcpy(mapped, offsets, sizeof(offsets));
    std::memcpy(gpuOffsets_, offsets, sizeof(offsets));
    using S = fpak::Section;
    for (uint32_t s = 0; s < uint32_t(fpak::Section::Count); ++s) {
        if (s == uint32_t(S::Instances))
            std::memcpy(mapped + offsets[s], instances_.data(), size_t(gpuSize(s)));
        else if (s == uint32_t(S::Meshes))
            std::memcpy(mapped + offsets[s], meshes_.data(), size_t(gpuSize(s)));
        else if (s == uint32_t(S::Materials))
            std::memcpy(mapped + offsets[s], materials_.data(), size_t(gpuSize(s)));
        else if (gpuSize(s))
            std::memcpy(mapped + offsets[s], static_cast<const uint8_t*>(data_) + h.sections[s].offset, size_t(h.sections[s].size));
    }
    // Terrain slots: each cluster's vertices and triangles are its own runs of the Vertices and
    // ClusterTriangles sections (written per patch by the terrain renderer; holes are degenerate
    // triangles). They start as the full 8 x 8-quad grid, its diagonals alternating so the grid
    // has no preferred direction.
    if (const uint32_t p = terrain_.patches) {
        auto* clusterVertices = reinterpret_cast<uint32_t*>(mapped + offsets[uint32_t(S::ClusterVertices)] +
                                                            h.sections[uint32_t(S::ClusterVertices)].size);
        const uint32_t firstClusterVertex = h.sections[uint32_t(S::ClusterVertices)].count;
        for (uint32_t i = 0; i < p * kPatchVertices; ++i)
            clusterVertices[i] = terrain_.firstVertex + i;
        auto* triangles = reinterpret_cast<uint32_t*>(mapped + offsets[uint32_t(S::ClusterTriangles)] +
                                                      h.sections[uint32_t(S::ClusterTriangles)].size);
        const uint32_t firstTriangle = h.sections[uint32_t(S::ClusterTriangles)].count;
        uint32_t t = 0;
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { triangles[t++] = a | b << 8 | c << 16; };
        for (uint32_t j = 0; j < 8; ++j)
            for (uint32_t i = 0; i < 8; ++i) {
                const uint32_t v00 = j * 9 + i, v10 = v00 + 1, v01 = v00 + 9, v11 = v00 + 10;
                if (((i + j) & 1) == 0) {  // counter-clockwise seen from above (+y)
                    tri(v00, v01, v11);
                    tri(v00, v11, v10);
                } else {
                    tri(v00, v01, v10);
                    tri(v10, v01, v11);
                }
            }
        for (uint32_t i = 1; i < p; ++i)
            std::memcpy(triangles + i * kPatchTriangles, triangles, kPatchTriangles * 4);
        auto* clusters = reinterpret_cast<fpak::ClusterGpu*>(mapped + offsets[uint32_t(S::Clusters)] +
                                                             h.sections[uint32_t(S::Clusters)].size);
        for (uint32_t i = 0; i < p; ++i) {
            fpak::ClusterGpu& c = clusters[i];
            c = {};
            c.parentError = 3.0e38f;  // always its own LOD: the terrain picks patch levels itself
            c.vertexOffset = firstClusterVertex + i * kPatchVertices;
            c.triangleOffset = firstTriangle + i * kPatchTriangles;
            c.counts = kPatchVertices | kPatchTriangles << 8 | terrain_.material << 16;
            c.cone = 127u << 24;  // no backface cone
        }
    }
    SDL_UnmapGPUTransferBuffer(device, staging);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {staging, 0};
    const SDL_GPUBufferRegion dst = {scene_, 0, uint32_t(total)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    // Submitted ahead of the first frame on the same queue, so no fence wait is needed.
    const bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    gpumem::releaseTransferBuffer(device, staging);  // SDL defers the free until the copy retires
    FX_INFO("scene: %s, %.1f MB uploaded, %u instances (+%u pooled), %u clusters, %u leaf triangles", path_,
            double(total) / (1 << 20), cookedInstances_, uint32_t(instances_.size()) - cookedInstances_,
            h.sections[uint32_t(fpak::Section::Clusters)].count, h.leafTriangles);
    return ok;
}

void ScenePak::release(SDL_GPUDevice* device)
{
    for (b3MeshData* c : liveColliders_)
        if (c)
            b3DestroyMesh(c);
    liveColliders_.clear();
    gpumem::releaseBuffer(device, scene_);
    scene_ = nullptr;
    if (queue_) {
        SDL_AsyncIOOutcome outcome;
        if (SDL_WaitAsyncIOResult(queue_, &outcome, -1))
            SDL_free(outcome.buffer);
        SDL_DestroyAsyncIOQueue(queue_);
        queue_ = nullptr;
    }
    SDL_free(data_);
    data_ = nullptr;
}

}  // namespace fx
