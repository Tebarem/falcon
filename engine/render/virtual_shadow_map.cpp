#include "render/virtual_shadow_map.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"
#include "render/skinning.h"
#include "render/virtual_texture.h"

#include "vsm_pages.shader.h"
#include "vsm_copy.shader.h"
#include "vsm_mark.shader.h"
#include "vsm_raster.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace fx {

namespace {

// kArgCrossing: the crossing list's draws, static layer small / clipped, moving casters small / clipped (32 apart).
constexpr uint32_t kArgClear = 0, kArgCull = 16, kArgExpand = 32, kArgDraw = 48, kArgDrawMoving = 80, kArgCrossing = 112,
                   kArgBytes = 240;
constexpr uint32_t kMaxCrossing = 1u << 20;  // triangles (vsm_pages.slang)
constexpr uint32_t kIndicesPerRecord = 128 * 3;
constexpr uint32_t kCounterCount = VirtualShadowMap::kCounterWords;  // vsm.slang kVsmCounterCount

struct MarkUniforms {
    VsmParamsGpu vsm;
    float4 cameraPos, cameraRight, cameraUp, cameraForward;
    uint32_t screen[4];
};

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

}  // namespace

bool VirtualShadowMap::init(const GpuDevice& gpu, const ScenePak& pak, const LightSystem& lights,
                            const SkinningSystem& skinning, const VirtualTextureSystem& vt, uint32_t maxSlots)
{
    FX_ZONE_CAT("VirtualShadowMap::init", Rendering);
    device_ = gpu.get();
    pak_ = &pak;
    lights_ = &lights;
    skinning_ = &skinning;
    vt_ = &vt;
    const float3 sunDirection = lights.sunDirection();
    maxSlots_ = std::clamp(std::max(maxSlots, uint32_t(lights.shadowSlots().size())), 1u, LightSystem::kMaxShadowSlots);
    entries_ = kDirectionalEntries + maxSlots_ * 6 * kEntriesPerFace;

    clear_ = gpu.createComputePipeline(shaders::vsm_pages_cs_clear);
    mark_ = gpu.createComputePipeline(shaders::vsm_mark_cs_mark);
    touch_ = gpu.createComputePipeline(shaders::vsm_pages_cs_touch);
    free_ = gpu.createComputePipeline(shaders::vsm_pages_cs_free);
    alloc_ = gpu.createComputePipeline(shaders::vsm_pages_cs_alloc);
    args_ = gpu.createComputePipeline(shaders::vsm_pages_cs_args);
    cull_ = gpu.createComputePipeline(shaders::vsm_pages_cs_cull);
    args2_ = gpu.createComputePipeline(shaders::vsm_pages_cs_args2);
    expand_ = gpu.createComputePipeline(shaders::vsm_pages_cs_expand);
    invalidate_ = gpu.createComputePipeline(shaders::vsm_pages_cs_invalidate);
    invalidateSlots_ = gpu.createComputePipeline(shaders::vsm_pages_cs_invalidate_slots);
    args3_ = gpu.createComputePipeline(shaders::vsm_pages_cs_args3);
    if (!clear_ || !mark_ || !touch_ || !free_ || !alloc_ || !args_ || !cull_ || !args2_ || !expand_ || !invalidate_ ||
        !invalidateSlots_ || !args3_)
        return false;

    SDL_GPUShader* vsClear = gpu.createShader(shaders::vsm_raster_vs_clear);
    SDL_GPUShader* vsShadow = gpu.createShader(shaders::vsm_raster_vs_shadow);
    SDL_GPUShader* fsClear = gpu.createShader(shaders::vsm_raster_fs_clear);
    SDL_GPUShader* fs = gpu.createShader(shaders::vsm_raster_fs_depth);
    SDL_GPUShader* vsInside = gpu.createShader(shaders::vsm_raster_vs_inside);
    SDL_GPUShader* fsInside = gpu.createShader(shaders::vsm_raster_fs_inside);
    SDL_GPUShader* vsEdge = gpu.createShader(shaders::vsm_raster_vs_edge);
    SDL_GPUShader* fsEdge = gpu.createShader(shaders::vsm_raster_fs_edge);
    SDL_GPUShader* vsCopy = gpu.createShader(shaders::vsm_copy_vs_copy);
    SDL_GPUShader* fsCopy = gpu.createShader(shaders::vsm_copy_fs_copy);
    if (!vsClear || !vsShadow || !fsClear || !fs || !vsInside || !fsInside || !vsEdge || !fsEdge || !vsCopy || !fsCopy)
        return false;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.fragment_shader = fsClear;
    info.vertex_shader = vsClear;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    clearPages_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.vertex_shader = vsShadow;
    info.fragment_shader = fs;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // 1 = nearest the sun
    raster_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.vertex_shader = vsInside;
    info.fragment_shader = fsInside;
    rasterInside_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.vertex_shader = vsEdge;
    info.fragment_shader = fsEdge;
    rasterEdge_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.vertex_shader = vsCopy;
    info.fragment_shader = fsCopy;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    copyPages_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (SDL_GPUShader* s : {vsClear, vsShadow, fsClear, fs, vsInside, fsInside, vsEdge, fsEdge, vsCopy, fsCopy})
        SDL_ReleaseGPUShader(device_, s);
    if (!clearPages_ || !raster_ || !rasterInside_ || !rasterEdge_ || !copyPages_) {
        FX_ERROR("vsm pipelines: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    t.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    t.width = t.height = kAtlasTexels;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    atlas_ = gpumem::createTexture("Shadows", device_, &t);
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    s.enable_compare = true;
    s.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;  // lit when receiver depth >= occluder depth
    compare_ = SDL_CreateGPUSampler(device_, &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.enable_compare = false;
    point_ = SDL_CreateGPUSampler(device_, &s);
    if (!atlas_ || !compare_ || !point_)
        return false;
    gpumem::setTextureName(device_, atlas_, "vsm physical atlas");

    // State buffer layout (words), mirrored in vsm.slang.
    const uint32_t E = entries_, P = kPhysicalPages, maskWords = (E + 31) / 32;
    const uint32_t owner = 2 * E, lastUsed = owner + P, request = lastUsed + P, dirty = request + maskWords;
    counterOffset_ = dirty + maskWords;
    const uint32_t freeList = counterOffset_ + kCounterCount, dirtyList = freeList + P, staticDirty = dirtyList + P;
    const uint32_t total = staticDirty + maskWords;
    params_.config[0] = kLevels;
    params_.config[1] = E;
    params_.config[3] = kAllocBudget;
    params_.layout[0] = owner;
    params_.layout[1] = lastUsed;
    params_.layout[2] = request;
    params_.layout[3] = dirty;
    params_.layout2[0] = counterOffset_;
    params_.layout2[1] = freeList;
    params_.layout2[2] = dirtyList;
    params_.layout2[3] = staticDirty;

    for (int a = 0; a < 3; ++a) {
        boundsMin_[a] = pak.header().boundsMin[a];
        boundsMax_[a] = pak.header().boundsMax[a];
    }
    setSunDirection(sunDirection);
    rebuildAll_ = rebuildSun_ = false;
    params_.lod = {1.0f, 0.01f, 1.0f, 0};  // y: local near plane as a fraction of the light range; z: smallest scatter caster
    params_.local[1] = kDirectionalEntries;  // slots: set every frame from the light system

    // Static list of every (instance, 64-cluster chunk): shadow casters are not view-culled.
    uint32_t instanceCount = 0;
    const auto* instances = pak.instances().data();
    instanceCount = uint32_t(pak.instances().size());
    const auto* meshes = pak.meshes().data();
    std::vector<uint32_t> chunks;
    for (uint32_t i = 0; i < instanceCount; ++i) {
        const fpak::MeshGpu& m = meshes[instances[i].mesh];
        for (uint32_t c = 0; c < m.clusterCount; c += 64) {
            chunks.push_back(i);
            chunks.push_back(m.clusterOffset + c);
        }
    }
    chunkCount_ = staticChunks_ = uint32_t(chunks.size() / 2);
    params_.config2[0] = chunkCount_;
    // The live meshes' (level meshes, meshes games build): empty now, sent by LiveGeometry as they
    // are built. A mesh's run of N clusters is ceil(N / 64) chunks: at most blocks / 64 + one a mesh.
    const ScenePak::LiveSlots& live = pak.liveSlots();
    liveChunkCapacity_ = live.meshes ? live.blocks / 64u + live.meshes : 0u;
    params_.config2[1] = kMaxRecords;

    auto buffer = [&](SDL_GPUBufferUsageFlags usage, uint64_t size, const char* name) {
        SDL_GPUBufferCreateInfo b = {usage, uint32_t(std::max<uint64_t>(size, 16)), 0};
        SDL_GPUBuffer* buf = gpumem::createBuffer("Shadows", device_, &b);
        if (buf)
            gpumem::setBufferName(device_, buf, name);
        return buf;
    };
    const auto rw = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    state_ = buffer(rw | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uint64_t(total) * 4, "vsm state");
    chunks_ = buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, (chunks.size() + size_t(liveChunkCapacity_) * 2) * 4, "vsm caster chunks");
    if (liveChunkCapacity_) {
        const SDL_GPUTransferBufferCreateInfo linfo = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, liveChunkCapacity_ * 8u, 0};
        liveChunkUpload_ = gpumem::createTransferBuffer("Shadows", device_, &linfo);
    }
    records_ = buffer(rw | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uint64_t(kMaxRecords) * 16, "vsm records");
    indices_ = buffer(rw | SDL_GPU_BUFFERUSAGE_INDEX, uint64_t(kMaxRecords) * kIndicesPerRecord * 4, "vsm indices");
    argsBuffer_ = buffer(rw | SDL_GPU_BUFFERUSAGE_INDIRECT, kArgBytes, "vsm args");
    crossing_ = buffer(rw | SDL_GPU_BUFFERUSAGE_INDEX, uint64_t(kMaxCrossing) * 12, "vsm crossing triangles");
    invalidations_ = buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, uint64_t(kMaxInvalidations) * 16, "vsm invalidations");
    const SDL_GPUTransferBufferCreateInfo tinfo = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kMaxInvalidations * 16, 0};
    invalidationUpload_ = gpumem::createTransferBuffer("Shadows", device_, &tinfo);
    if (!state_ || !chunks_ || !records_ || !indices_ || !argsBuffer_ || !crossing_ || !invalidations_ || !invalidationUpload_ ||
        (liveChunkCapacity_ && !liveChunkUpload_))
        return false;

    // Initial state: nothing allocated, every physical page free.
    std::vector<uint32_t> init(total, 0);
    std::fill(init.begin() + owner, init.begin() + owner + P, 0xffffffffu);
    const uint32_t bytes = uint32_t(init.size() * 4 + chunks.size() * 4);
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* staging = gpumem::createTransferBuffer("Shadows", device_, &ti);
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, staging, false));
    SDL_memcpy(mapped, init.data(), init.size() * 4);
    SDL_memcpy(mapped + init.size() * 4, chunks.data(), chunks.size() * 4);
    SDL_UnmapGPUTransferBuffer(device_, staging);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {staging, 0};
    SDL_GPUBufferRegion dst = {state_, 0, uint32_t(init.size() * 4)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    src.offset = uint32_t(init.size() * 4);
    dst = {chunks_, 0, uint32_t(chunks.size() * 4)};
    if (!chunks.empty())
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    gpumem::releaseTransferBuffer(device_, staging);

    FX_INFO("vsm: %u levels (level 0 = %.3g, top = %.3g units), %zu local lights (room for %u), %u entries, %u-page atlas "
            "(%.0f MB), %u caster chunks",
            kLevels, double(params_.world.x), double(params_.world.x) * double(1u << (kLevels - 1)),
            lights_->shadowSlots().size(), maxSlots_, entries_,
            kPhysicalPages, double(kAtlasTexels) * kAtlasTexels * 4 / (1 << 20), chunkCount_);
    return true;
}

void VirtualShadowMap::shutdown()
{
    if (!device_)
        return;
    for (SDL_GPUComputePipeline* p : {clear_, mark_, touch_, free_, alloc_, args_, cull_, args2_, expand_, invalidate_, invalidateSlots_,
                                      args3_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    SDL_ReleaseGPUGraphicsPipeline(device_, clearPages_);
    SDL_ReleaseGPUGraphicsPipeline(device_, raster_);
    SDL_ReleaseGPUGraphicsPipeline(device_, rasterInside_);
    SDL_ReleaseGPUGraphicsPipeline(device_, rasterEdge_);
    SDL_ReleaseGPUGraphicsPipeline(device_, copyPages_);
    gpumem::releaseTexture(device_, atlas_);
    if (staticAtlas_)
        gpumem::releaseTexture(device_, staticAtlas_);
    staticAtlas_ = nullptr;
    SDL_ReleaseGPUSampler(device_, compare_);
    SDL_ReleaseGPUSampler(device_, point_);
    for (SDL_GPUBuffer* b : {state_, chunks_, records_, indices_, argsBuffer_, crossing_, invalidations_})
        gpumem::releaseBuffer(device_, b);
    gpumem::releaseTransferBuffer(device_, invalidationUpload_);
    if (liveChunkUpload_)
        gpumem::releaseTransferBuffer(device_, liveChunkUpload_);
    liveChunkUpload_ = nullptr;
    staticChunks_ = liveChunkCapacity_ = 0;
    device_ = nullptr;
}

void VirtualShadowMap::setLiveChunks(SDL_GPUCopyPass* copy, const std::vector<uint32_t>& chunks)
{
    if (!device_ || !liveChunkUpload_)
        return;
    const uint32_t n = std::min(uint32_t(chunks.size() / 2), liveChunkCapacity_);
    if (n) {
        void* mapped = SDL_MapGPUTransferBuffer(device_, liveChunkUpload_, true);
        SDL_memcpy(mapped, chunks.data(), size_t(n) * 8);
        SDL_UnmapGPUTransferBuffer(device_, liveChunkUpload_);
        const SDL_GPUTransferBufferLocation src = {liveChunkUpload_, 0};
        const SDL_GPUBufferRegion dst = {chunks_, staticChunks_ * 8u, n * 8u};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    }
    chunkCount_ = staticChunks_ + n;
    params_.config2[0] = chunkCount_;
}

void VirtualShadowMap::setSunDirection(float3 sunDirection)
{
    sunDirection_ = sunDirection;
    // Light basis and depth range from the scene bounds.
    lightZ_ = normalize(sunDirection * -1.0f);
    const float3 up = std::fabs(lightZ_.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
    lightX_ = normalize(cross(up, lightZ_));
    lightY_ = cross(lightZ_, lightX_);
    struct {
        const float *boundsMin, *boundsMax;
    } h = {boundsMin_, boundsMax_};
    float zMin = 1e30f, zMax = -1e30f;
    sceneLightMin_ = {1e30f, 1e30f};
    sceneLightMax_ = {-1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float3 p = {c & 1 ? h.boundsMax[0] : h.boundsMin[0], c & 2 ? h.boundsMax[1] : h.boundsMin[1],
                          c & 4 ? h.boundsMax[2] : h.boundsMin[2]};
        zMin = std::min(zMin, dot(p, lightZ_));
        zMax = std::max(zMax, dot(p, lightZ_));
        sceneLightMin_ = {std::min(sceneLightMin_.x, dot(p, lightX_)), std::min(sceneLightMin_.y, dot(p, lightY_))};
        sceneLightMax_ = {std::max(sceneLightMax_.x, dot(p, lightX_)), std::max(sceneLightMax_.y, dot(p, lightY_))};
    }
    const float pad = (zMax - zMin) * 0.01f + 0.01f;
    zMin -= pad;
    zMax += pad;
    // (At least 64 m: a scene of a few small things still gets usable pages.)
    const float sceneSize = std::max({h.boundsMax[0] - h.boundsMin[0], h.boundsMax[1] - h.boundsMin[1],
                                      h.boundsMax[2] - h.boundsMin[2], 64.0f});
    params_.lightX = f4(lightX_, 0);
    params_.lightY = f4(lightY_, 0);
    params_.lightZ = f4(lightZ_, zMin);
    // The coarsest level spans twice the scene, so it covers all of it from any point of the
    // scene's light-space footprint (the windows follow the camera clamped to it: render).
    params_.world = {2.0f * sceneSize / float(1u << (kLevels - 1)), 1.0f / (zMax - zMin), 0.0f, 0.0f};
    rebuildSun_ = true;
}

void VirtualShadowMap::setSceneBounds(const float lo[3], const float hi[3])
{
    for (int a = 0; a < 3; ++a) {
        boundsMin_[a] = lo[a];
        boundsMax_[a] = hi[a];
    }
    setSunDirection(sunDirection_);
}

void VirtualShadowMap::noteRecordOverflow(bool overflow)
{
    // On overflow every page that lost a record is dropped and requested again, so the same set
    // of new pages overflows again every frame and none becomes resident (dense foliage under a
    // coarse level: thousands of clusters per page). Halve the pages started per frame until
    // their records fit; grow back slowly once they do.
    allocBudget_ = overflow ? std::max(allocBudget_ / 2, 1u)
                            : std::min(allocBudget_ + std::max(allocBudget_ / 8, 1u), kAllocBudget);
    // Scatter casters (grass, flowers) smaller than this many texels of a page are left out:
    // raised while records overflow, lowered slowly once they fit.
    if (overflow) {
        params_.lod.z = std::min(params_.lod.z * 1.5f, 64.0f);
        scatterCalm_ = 0;
    } else if (params_.lod.z > 1.0f && ++scatterCalm_ >= 60) {
        params_.lod.z = std::max(params_.lod.z * 0.9f, 1.0f);
        scatterCalm_ = 0;
    }
}

uint32_t VirtualShadowMap::sunPages() const
{
    uint32_t local = 0;
    for (const uint32_t p : slotPages_)
        local += p;
    return requested_ > local ? requested_ - local : 0u;
}

float VirtualShadowMap::pageBudget() const
{
    if (budgetMs_ <= 0.0f || !cache_)
        return 3.0e38f;
    return std::max((budgetMs_ - baseMs_) / msPerPage_, 16.0f);
}

bool VirtualShadowMap::grant(float cost, uint32_t& waited)
{
    const float budget = pageBudget();
    cost = std::max(cost, 1.0f);
    // Fits what is left; or nothing else this frame and it has waited long enough that, every so
    // many frames, it averages within the budget.
    if (spent_ + cost <= budget || (spent_ == 0.0f && float(waited + 1) * budget >= cost)) {
        spent_ += cost;
        waited = 0;
        return true;
    }
    ++waited;
    return false;
}

void VirtualShadowMap::chooseSun()
{
    chosen_ = true;
    sunGranted_ = false;
    if (length(lights_->sunDirection() - sunDirection_) > 1e-6f)
        sunGranted_ = grant(float(sunPages()), sunWaited_);  // (a new basis: its pages again)
    else
        sunWaited_ = 0;
}

uint32_t VirtualShadowMap::chooseSlots(uint32_t waiting, uint32_t forced)
{
    spent_ = 0.0f;
    for (uint32_t s = 0; s < 32; ++s)
        if (forced & (1u << s)) {
            spent_ += float(std::max(slotPages_[s], 1u));
            slotWaited_[s] = 0;
        }
    // The sun and the waiting slots, longest waiting first (the sun before a slot as long).
    uint32_t order[32], n = 0;
    for (uint32_t s = 0; s < 32; ++s)
        if (waiting & (1u << s))
            order[n++] = s;
    std::stable_sort(order, order + n, [&](uint32_t a, uint32_t b) { return slotWaited_[a] > slotWaited_[b]; });
    const bool sunWaits = length(lights_->sunDirection() - sunDirection_) > 1e-6f;
    bool sunDone = !sunWaits;
    if (!sunWaits) {
        chosen_ = true;
        sunGranted_ = false;
        sunWaited_ = 0;
    }
    uint32_t chosen = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t s = order[k];
        if (!sunDone && sunWaited_ >= slotWaited_[s]) {
            chooseSun();
            sunDone = true;
        }
        if (grant(float(slotPages_[s]), slotWaited_[s]))
            chosen |= 1u << s;
    }
    if (!sunDone)
        chooseSun();
    for (uint32_t s = 0; s < 32; ++s)
        if (!(waiting & (1u << s)))
            slotWaited_[s] = 0;
    return chosen;
}

void VirtualShadowMap::noteFrame(const uint32_t* c, float shadowMs)
{
    requested_ = c[6];  // kVsmRequested
    for (uint32_t s = 0; s < 32; ++s)
        slotPages_[s] = c[32 + s];  // kVsmSlotRequested
    if (shadowMs < 0.0f)
        return;
    timedMs_ += shadowMs;
    worstMs_ = std::max(worstMs_, shadowMs);
    ++timed_;
    const uint32_t dirty = c[2];  // kVsmDirty
    if (dirty == 0)
        baseMs_ += (shadowMs - baseMs_) * 0.1f;
    else if (dirty >= 16)
        msPerPage_ += (std::clamp(std::max(shadowMs - baseMs_, 0.0f) / float(dirty), 1e-4f, 0.5f) - msPerPage_) * 0.25f;
}

bool VirtualShadowMap::takeTiming(float& meanMs, float& worstMs)
{
    if (!timed_)
        return false;
    meanMs = float(timedMs_ / timed_);
    worstMs = worstMs_;
    timedMs_ = 0.0;
    worstMs_ = 0.0f;
    timed_ = 0;
    return true;
}

void VirtualShadowMap::enableMovingCasters()
{
    if (staticAtlas_ || !device_)
        return;
    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    t.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    t.width = t.height = kAtlasTexels;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    staticAtlas_ = gpumem::createTexture("Shadows", device_, &t);
    if (!staticAtlas_) {
        FX_WARN("vsm: no static layer atlas (%s): moving casters re-render whole pages", SDL_GetError());
        return;
    }
    gpumem::setTextureName(device_, staticAtlas_, "vsm static layer");
    params_.config2[2] = 1;
    rebuildAll_ = true;  // the pages so far have no static layer
    FX_INFO("vsm: moving casters drawn over a cached static layer (%.0f MB)",
            double(kAtlasTexels) * kAtlasTexels * 4 / (1 << 20));
}

void VirtualShadowMap::uploadInvalidations(SDL_GPUCopyPass* copy, const std::vector<float4>& spheres, bool append)
{
    const uint32_t first = append ? invalidationCount_ : 0;
    if (!append)
        invalidateAll_ = false;
    if (invalidateAll_ || first + spheres.size() > kMaxInvalidations) {
        invalidateAll_ = true;
        invalidationCount_ = 0;
        return;
    }
    invalidationCount_ = first + uint32_t(spheres.size());
    if (spheres.empty())
        return;
    const uint32_t bytes = uint32_t(spheres.size()) * 16;
    // Its own region of the (cycled) transfer buffer, so an append does not overwrite the first batch.
    void* mapped = SDL_MapGPUTransferBuffer(device_, invalidationUpload_, first == 0);
    std::memcpy(static_cast<uint8_t*>(mapped) + first * 16, spheres.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, invalidationUpload_);
    const SDL_GPUTransferBufferLocation src = {invalidationUpload_, first * 16};
    const SDL_GPUBufferRegion dst = {invalidations_, first * 16, bytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, first == 0);
}

void VirtualShadowMap::render(SDL_GPUCommandBuffer* cmd, const CameraState& camera, SDL_GPUTexture* depth,
                              SDL_GPUSampler* pointSampler, uint32_t width, uint32_t height, uint32_t displayHeight,
                              uint64_t frame, bool cache, SDL_GPUTexture* extraDepth)
{
    FX_ZONE_CAT("VirtualShadowMap::render", Rendering);
    cache_ = cache;
    if (!chosen_) {  // (no slot chooser this frame)
        spent_ = 0.0f;
        chooseSun();
    }
    if (sunGranted_)
        setSunDirection(lights_->sunDirection());  // the sun moved: new basis, its pages re-rendered
    chosen_ = sunGranted_ = false;
    if (rebuildAll_) {
        invalidateAll_ = true;
        rebuildAll_ = false;
    }
    const bool sunStale = rebuildSun_;
    rebuildSun_ = false;
    // Clipmap windows follow the camera in whole pages, so cached pages keep their world position.
    // A camera outside the scene's light-space footprint (an editor view of the whole scene) is
    // clamped onto it: the windows stay over the scene, and the coarsest one covers all of it
    // (centred on the camera itself, the scene's far side fell outside every window: no shadows).
    const float camX = std::clamp(dot(camera.position, lightX_), sceneLightMin_.x, sceneLightMax_.x);
    const float camY = std::clamp(dot(camera.position, lightY_), sceneLightMin_.y, sceneLightMax_.y);
    for (uint32_t k = 0; k < kLevels; ++k) {
        const float pageSize = params_.world.x * float(1u << k) / float(kPagesPerSide);
        params_.centers[k][0] = int32_t(std::floor(camX / pageSize));
        params_.centers[k][1] = int32_t(std::floor(camY / pageSize));
    }
    // Pixel footprint per unit distance, of a display pixel: shadow detail follows the output
    // resolution while upscaling.
    params_.world.z = 2.0f / (camera.p11 * float(displayHeight));
    params_.config[2] = uint32_t(frame) + 2;                 // initial lastUsed (0) counts as old
    params_.config2[3] = !cache || invalidateAll_ ? 2u : sunStale ? 1u : 0u;
    params_.local[2] = invalidationCount_;
    // The uncached reference (and a full rebuild) renders every page at once, unless records
    // overflowed recently (then pages come in over several frames).
    // New pages within what moved lights left of the time budget (at least 16: a camera turning
    // still gets them, a coarser level shows meanwhile).
    const uint32_t all = allocBudget_ < kAllocBudget ? allocBudget_ : kPhysicalPages;
    const float left = std::max(pageBudget() - spent_, 16.0f);
    params_.config[3] = cache && !invalidateAll_ && !sunStale ? std::min(allocBudget_, uint32_t(std::min(left, float(kAllocBudget)))) : all;
    // Shadow slots follow the light system (lights come, go and move in the editor).
    const std::vector<uint32_t>& slots = lights_->shadowSlots();
    params_.local[0] = std::min(uint32_t(slots.size()), maxSlots_);
    for (uint32_t s = 0; s < params_.local[0]; ++s)
        params_.slotLights[s / 4][s % 4] = slots[s];
    params_.local[3] = lights_->changedSlots() & (maxSlots_ >= 32 ? ~0u : (1u << maxSlots_) - 1u);

    auto pass = [&](const char* name, SDL_GPUComputePipeline* p, std::initializer_list<SDL_GPUBuffer*> rw,
                    std::initializer_list<SDL_GPUBuffer*> ro) {
        SDL_GPUStorageBufferReadWriteBinding b[3] = {};
        uint32_t n = 0;
        for (SDL_GPUBuffer* buf : rw)
            b[n++] = {buf, false};
        gpuprof::pass(cmd, name);
        SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, nullptr, 0, b, n);
        SDL_BindGPUComputePipeline(cp, p);
        if (ro.size())
            SDL_BindGPUComputeStorageBuffers(cp, 0, ro.begin(), uint32_t(ro.size()));
        return cp;
    };
    const uint32_t E = entries_;
    SDL_GPUBuffer* lightBuffer = lights_->lights();

    SDL_PushGPUComputeUniformData(cmd, 0, &params_, sizeof(params_));
    FX_GPU_SECTION(cmd, "Shadows");
    SDL_GPUComputePass* cp = pass("vsm clear requests", clear_, {state_}, {});
    SDL_DispatchGPUCompute(cp, ((E + 31) / 32 + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(cp);

    MarkUniforms mu = {};
    mu.vsm = params_;
    mu.cameraPos = f4(camera.position, 1.0f);
    mu.cameraRight = f4(camera.right, 1.0f / camera.p00);
    mu.cameraUp = f4(camera.up, 1.0f / camera.p11);
    mu.cameraForward = f4(camera.forward, camera.zNear);
    mu.screen[0] = width;
    mu.screen[1] = height;
    SDL_PushGPUComputeUniformData(cmd, 0, &mu, sizeof(mu));
    cp = pass("vsm mark pages", mark_, {state_}, {lightBuffer});
    const SDL_GPUTextureSamplerBinding depthBinding = {depth, pointSampler};
    SDL_BindGPUComputeSamplers(cp, 0, &depthBinding, 1);
    SDL_DispatchGPUCompute(cp, ((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8, 1);
    SDL_EndGPUComputePass(cp);
    if (extraDepth) {
        cp = pass("vsm mark pages (water)", mark_, {state_}, {lightBuffer});
        const SDL_GPUTextureSamplerBinding extraBinding = {extraDepth, pointSampler};
        SDL_BindGPUComputeSamplers(cp, 0, &extraBinding, 1);
        SDL_DispatchGPUCompute(cp, ((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8, 1);
        SDL_EndGPUComputePass(cp);
    }

    SDL_PushGPUComputeUniformData(cmd, 0, &params_, sizeof(params_));
    cp = pass("vsm touch", touch_, {state_}, {});
    SDL_DispatchGPUCompute(cp, (E + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(cp);
    if (params_.local[3]) {  // a shadowed light moved, changed shape or owner: its slot's pages re-render
        cp = pass("vsm invalidate slots", invalidateSlots_, {state_}, {});
        SDL_DispatchGPUCompute(cp, (maxSlots_ * 6 * kEntriesPerFace + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(cp);
    }
    if (invalidationCount_) {
        cp = pass("vsm invalidate", invalidate_, {state_}, {invalidations_, pak_->sceneBuffer(), lightBuffer});
        const uint32_t views = kLevels + params_.local[0] * 6;
        SDL_DispatchGPUCompute(cp, (invalidationCount_ * views + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(cp);
    }
    for (SDL_GPUComputePipeline* p : {free_, alloc_}) {
        cp = pass(p == free_ ? "vsm free list" : "vsm allocate", p, {state_}, {});
        SDL_DispatchGPUCompute(cp, p == free_ ? 1 : (E + 63) / 64, 1, 1);  // the free list is one group (a prefix sum)
        SDL_EndGPUComputePass(cp);
    }
    invalidationCount_ = 0;
    invalidateAll_ = false;
    cp = pass("vsm cull args", args_, {state_, argsBuffer_}, {});
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);
    cp = pass("vsm caster cull", cull_, {state_, records_}, {chunks_, pak_->sceneBuffer(), lightBuffer, skinning_->buffer()});
    SDL_DispatchGPUComputeIndirect(cp, argsBuffer_, kArgCull);
    SDL_EndGPUComputePass(cp);
    cp = pass("vsm expand args", args2_, {state_, argsBuffer_}, {});
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);
    cp = pass("vsm expand", expand_, {state_, indices_, crossing_}, {records_, pak_->sceneBuffer(), lightBuffer, skinning_->buffer()});
    SDL_DispatchGPUComputeIndirect(cp, argsBuffer_, kArgExpand);
    SDL_EndGPUComputePass(cp);
    cp = pass("vsm crossing args", args3_, {state_, argsBuffer_}, {});
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);

    // Clear the new pages, then rasterize their casters (two draws, both GPU-sized). With moving
    // casters, that is the static layer; each page being rendered then starts from its static
    // layer in the atlas, and the moving casters are drawn over it.
    SDL_GPUDepthStencilTargetInfo target = {};
    target.texture = staticAtlas_ ? staticAtlas_ : atlas_;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_PushGPUVertexUniformData(cmd, 0, &params_, sizeof(params_));
    struct {
        uint32_t pageTableOffset;
        float mipBias;
        uint32_t pad[2];
    } mask = {vt_->pageTableOffset(), vt_->mipBias(), {0, 0}};
    SDL_PushGPUFragmentUniformData(cmd, 0, &mask, sizeof(mask));
    FX_GPU_PASS(cmd, staticAtlas_ ? "vsm clear + raster static layer" : "vsm clear + raster pages");
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &target);
    SDL_GPUBuffer* vs[5] = {records_, pak_->sceneBuffer(), state_, lightBuffer, skinning_->buffer()};
    SDL_BindGPUGraphicsPipeline(rp, clearPages_);
    SDL_BindGPUVertexStorageBuffers(rp, 0, vs, 4);
    SDL_DrawGPUPrimitivesIndirect(rp, argsBuffer_, kArgClear, 1);
    // Alpha-masked casters: the albedo pages and material data for the alpha test.
    const SDL_GPUTextureSamplerBinding albedo = {vt_->physical(0), vt_->sampler()};
    SDL_GPUBuffer* materialData = vt_->materialData();
    const SDL_GPUBufferBinding ib = {indices_, 0}, crossingIb = {crossing_, 0};
    // A layer's casters: the triangles inside their pages, then the small ones crossing an edge
    // (discarded past it), then the large ones (clipped).
    auto drawCasters = [&](uint32_t insideArgs, uint32_t crossingArgs) {
        const struct {
            SDL_GPUGraphicsPipeline* pipeline;
            const SDL_GPUBufferBinding* indices;
            uint32_t args;
        } draws[3] = {{rasterInside_, &ib, insideArgs}, {rasterEdge_, &crossingIb, crossingArgs},
                      {raster_, &crossingIb, crossingArgs + 32}};
        for (const auto& d : draws) {
            SDL_BindGPUGraphicsPipeline(rp, d.pipeline);
            SDL_BindGPUVertexStorageBuffers(rp, 0, vs, 5);
            SDL_BindGPUFragmentSamplers(rp, 0, &albedo, 1);
            SDL_BindGPUFragmentStorageBuffers(rp, 0, &materialData, 1);
            SDL_BindGPUIndexBuffer(rp, d.indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitivesIndirect(rp, argsBuffer_, d.args, 1);
        }
    };
    drawCasters(kArgDraw, kArgCrossing);
    SDL_EndGPURenderPass(rp);
    if (!staticAtlas_)
        return;

    FX_GPU_PASS(cmd, "vsm copy static + raster moving");
    target.texture = atlas_;
    rp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &target);
    SDL_BindGPUGraphicsPipeline(rp, copyPages_);
    SDL_BindGPUVertexStorageBuffers(rp, 0, &state_, 1);
    const SDL_GPUTextureSamplerBinding staticLayer = {staticAtlas_, point_};
    SDL_BindGPUFragmentSamplers(rp, 0, &staticLayer, 1);
    SDL_DrawGPUPrimitivesIndirect(rp, argsBuffer_, kArgClear, 1);
    drawCasters(kArgDrawMoving, kArgCrossing + 64);
    SDL_EndGPURenderPass(rp);
}

}  // namespace fx
