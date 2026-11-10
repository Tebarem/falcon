#include "render/gpu_driven_renderer.h"

#include "render/shader_file.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"
#include "render/skinning.h"
#include "render/virtual_shadow_map.h"
#include "render/virtual_texture.h"
#include "terrain/terrain.h"

#include "cluster_cull.shader.h"
#include "hiz_debug.shader.h"
#include "cluster_expand.shader.h"
#include "hiz.shader.h"
#include "msaa_edges.shader.h"
#include "msaa_edges_masked.shader.h"
#include "reflections.shader.h"
#include "reflection_source.shader.h"
#include "reflection_far.shader.h"
#include "visbuffer_resolve.shader.h"
#include "visbuffer.shader.h"
#include "vt_feedback.shader.h"
#include "editor_pick.shader.h"
#include "ray_traced_view.shader.h"
#include "probe_view.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_set>

namespace fx {

namespace {

// Mirrors engine/shaders/culling.slang.
constexpr uint32_t kArgCull1 = 0, kArgCull2 = 16, kArgExpand1 = 32, kArgExpand2 = 48;
constexpr uint32_t kArgDraw1 = 64, kArgDraw1Masked = 96, kArgDraw2 = 128, kArgDraw2Masked = 160;
constexpr uint32_t kArgsBytes = 256;  // (192..256: the transparent pass's, culling.slang)
constexpr uint32_t kCounterCount = 16;
constexpr uint32_t kClustersPerChunk = 64;
constexpr uint32_t kIndicesPerCluster = 128 * 3;
constexpr uint32_t kFlagOcclusion = 1u << 0, kFlagCone = 1u << 1, kFlagForceRetest = 1u << 2, kFlagNoAlphaMask = 1u << 3;
constexpr uint32_t kMaxVisibleClusters = 1u << 17;

struct CullUniforms {
    float4x4 view, prevView;
    float4 planes[5];
    float4 cameraPos;  // w: LOD threshold (fraction of screen height)
    float4 proj;       // P00, P11, zNear
    uint32_t hizLevels[16][4];  // [0][3], [1][3]: terrain patch instances (first, count)
    uint32_t limits[4];  // instances, maxChunks, maxVisible, maxCandidates
    uint32_t params[4];  // flags, hiz level count, width, height
    float4 scatter;      // x: smallest scatter instance drawn (as proj.w), y: full-detail distance
};
static_assert(sizeof(CullUniforms) == 544);

struct ExpandUniforms {
    uint32_t pass, maxVisible, backfaces, pad;
    float4 eye;
};

struct HizUniforms {
    uint32_t src[4], dst[4];
};

struct MsaaUniforms {
    uint32_t screen[2];
    uint32_t pageTableOffset;
    float mipBias;
};

SDL_GPUSampleCount sampleCountOf(uint32_t samples)
{
    return samples >= 8   ? SDL_GPU_SAMPLECOUNT_8
           : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4
           : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2
                          : SDL_GPU_SAMPLECOUNT_1;
}

struct ResolveUniforms {
    float4x4 prevViewProj;  // unjittered, for motion vectors
    float4 cameraPos, cameraRight, cameraUp, cameraForward, sunDirection;
    uint32_t params[4];    // width, height, debug mode, frame
    uint32_t vtParams[4];  // page-table byte offset, feedback capacity, page count
    float vtParamsF[4];    // mip bias
    float4 sunColor;
    uint32_t gridOrderOffset, gridTileOffset, gridTilesX, gridWords;
    float gridBinScale;
    uint32_t lightCount, gridPad[2];
    VsmParamsGpu vsm;
    float4 jitter;
    ProbeVolumeGpu probes;
    uint32_t ambient[4];  // AO scale; probe GI on; the AO's sample offset
    uint32_t terrain[4];  // x: the terrain surface texture holds this frame
    float4 reflect;       // x: the reflection pass adds them
    uint32_t decals[4];   // count, words per tile, tiles across, tile size
};

struct ReflectUniforms {
    float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
    uint32_t params[4];  // width, height, debug view, frame
    ProbeVolumeGpu probes;
    float4 sun;       // toward the sun, bounce intensity
    float4 sunColor;  // its irradiance, sky intensity
    float4 reflect;   // GI on, screen space below this roughness, zNear
    float4 ssr;       // thickness (times the default), intensity, steps per ray, trace scale (1, 2: half resolution)
    uint32_t trace[4];  // VT page-table byte offset, the BVH is there, alpha-masked holes
};

struct HizViewUniforms {
    uint32_t level[4], screen[4];
};

// Normalized clip plane row3 + sign * row(r) of a column-major view-projection (Gribb-Hartmann).
float4 plane(const float4x4& m, float sign, int r)
{
    const float4 p = {m.m[0][3] + sign * m.m[0][r], m.m[1][3] + sign * m.m[1][r], m.m[2][3] + sign * m.m[2][r],
                      m.m[3][3] + sign * m.m[3][r]};
    const float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return {p.x / l, p.y / l, p.z / l, p.w / l};
}

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

float halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index; index /= base) {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// Inverse of viewFromBasis (a rigid transform): view space -> world.
float4x4 viewToWorld(const CameraState& c)
{
    float4x4 r = {};
    const float3 back = c.forward * -1.0f;
    const float3 cols[4] = {c.right, c.up, back, c.position};
    for (int i = 0; i < 4; ++i) {
        r.m[i][0] = cols[i].x;
        r.m[i][1] = cols[i].y;
        r.m[i][2] = cols[i].z;
    }
    r.m[3][3] = 1.0f;
    return r;
}

SDL_GPUBuffer* createBuffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage, uint64_t size, const char* name)
{
    SDL_GPUBufferCreateInfo info = {};
    info.usage = usage;
    info.size = uint32_t(std::max<uint64_t>(size, 16));
    SDL_GPUBuffer* b = gpumem::createBuffer("Renderer", device, &info);
    if (!b)
        FX_ERROR("buffer %s (%llu bytes): %s", name, (unsigned long long)size, SDL_GetError());
    else
        gpumem::setBufferName(device, b, name);
    return b;
}

void dispatchGroups(SDL_GPUComputePass* pass, uint32_t threads, uint32_t groupSize)
{
    if (threads)
        SDL_DispatchGPUCompute(pass, (threads + groupSize - 1) / groupSize, 1, 1);
}

}  // namespace

bool GpuDrivenRenderer::init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt, LightSystem& lights,
                             VirtualShadowMap& vsm, SkinningSystem& skinning)
{
    FX_ZONE_CAT("GpuDrivenRenderer::init", Rendering);
    device_ = gpu.get();
    gpu_ = &gpu;
    pak_ = &pak;
    vt_ = &vt;
    lights_ = &lights;
    vsm_ = &vsm;
    skinning_ = &skinning;
    lights.setSlotChooser([&vsm](uint32_t waiting, uint32_t forced) { return vsm.chooseSlots(waiting, forced); });

    if (!SDL_GPUTextureSupportsFormat(device_, kVisFormat, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ)) {
        FX_ERROR("visibility buffer format R32_UINT (color target + compute read) is not supported");
        return false;
    }

    reset_ = gpu.createComputePipeline(shaders::cluster_cull_cs_reset);
    instances_ = gpu.createComputePipeline(shaders::cluster_cull_cs_instances);
    argsCull1_ = gpu.createComputePipeline(shaders::cluster_cull_cs_args_cull1);
    cullFirst_ = gpu.createComputePipeline(shaders::cluster_cull_cs_cull_first);
    argsDraw1_ = gpu.createComputePipeline(shaders::cluster_cull_cs_args_draw1);
    cullSecond_ = gpu.createComputePipeline(shaders::cluster_cull_cs_cull_second);
    argsDraw2_ = gpu.createComputePipeline(shaders::cluster_cull_cs_args_draw2);
    expand_ = gpu.createComputePipeline(shaders::cluster_expand_cs_expand);
    argsRaster1_ = gpu.createComputePipeline(shaders::cluster_cull_cs_args_raster1);
    argsRaster2_ = gpu.createComputePipeline(shaders::cluster_cull_cs_args_raster2);
    hizDepth_ = gpu.createComputePipeline(shaders::hiz_cs_hiz_depth);
    hizDown_ = gpu.createComputePipeline(shaders::hiz_cs_hiz_down);
    resolve_ = builtinResolve_ = gpu.createComputePipeline(shaders::visbuffer_resolve_cs_resolve);
    reflect_ = gpu.createComputePipeline(shaders::reflections_cs_reflect);
    reflectTrace_ = gpu.createComputePipeline(shaders::reflections_cs_trace);
    reflectFarPipe_ = gpu.createComputePipeline(shaders::reflection_far_cs_reflect_far);
    reflectSourcePipe_ = gpu.createComputePipeline(shaders::reflection_source_cs_source);
    pick_ = gpu.createComputePipeline(shaders::editor_pick_cs_pick);
    viewHiz_ = gpu.createComputePipeline(shaders::hiz_debug_cs_view_hiz);
    rayView_ = gpu.createComputePipeline(shaders::ray_traced_view_cs_ray_view);
    probeView_ = gpu.createComputePipeline(shaders::probe_view_cs_probe_view);
    vtClear_ = gpu.createComputePipeline(shaders::vt_feedback_cs_vt_clear);
    if (!post_.init(gpu))
        return false;
    if (!fsr_.init(gpu))
        FX_INFO("fsr: unavailable: %s", fsr_.reason().c_str());
    dlss_.init(gpu);  // DLSS starts when first picked
    if (!vtClear_ || !reset_ || !instances_ || !argsCull1_ || !cullFirst_ || !argsDraw1_ || !cullSecond_ || !argsDraw2_ || !expand_ || !argsRaster1_ || !argsRaster2_ ||
        !hizDepth_ || !hizDown_ || !resolve_ || !reflect_ || !reflectTrace_ || !reflectFarPipe_ || !reflectSourcePipe_ || !viewHiz_ || !rayView_ || !probeView_)
        return false;

    SDL_GPUShader* vs = gpu.createShader(shaders::visbuffer_vs_main);
    SDL_GPUShader* fs = gpu.createShader(shaders::visbuffer_fs_main);
    SDL_GPUShader* vsMasked = gpu.createShader(shaders::visbuffer_vs_masked);
    SDL_GPUShader* fsMasked = gpu.createShader(shaders::visbuffer_fs_masked);
    if (!vs || !fs || !vsMasked || !fsMasked)
        return false;
    SDL_GPUColorTargetDescription visTarget = {};
    visTarget.format = kVisFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // double-sided materials; cone culling removes most backfaces
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
    info.target_info.color_target_descriptions = &visTarget;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;
    visbuffer_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    info.vertex_shader = vsMasked;
    info.fragment_shader = fsMasked;
    visbufferMasked_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (SDL_GPUShader* s : {vs, fs, vsMasked, fsMasked})
        SDL_ReleaseGPUShader(device_, s);
    if (!visbuffer_ || !visbufferMasked_) {
        FX_ERROR("visibility buffer pipeline: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler = {};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    pointSampler_ = SDL_CreateGPUSampler(device_, &sampler);
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.max_lod = 16.0f;
    mipSampler_ = SDL_CreateGPUSampler(device_, &sampler);

    // Capacities from the scene: every (instance, cluster) pair could be a candidate.
    const auto* instances = pak.instances().data();
    instanceCount_ = uint32_t(pak.instances().size());
    const auto* meshes = pak.meshes().data();
    uint64_t clusterInstances = 0;
    for (uint32_t i = 0; i < instanceCount_; ++i) {
        const uint32_t n = meshes[instances[i].mesh].clusterCount;
        clusterInstances += n;
        maxChunks_ += (n + kClustersPerChunk - 1) / kClustersPerChunk;
    }
    maxVisible_ = uint32_t(std::min<uint64_t>(std::max<uint64_t>(clusterInstances, 1), kMaxVisibleClusters));

    const auto rw = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    chunks_ = createBuffer(device_, rw, uint64_t(maxChunks_) * 8, "chunks");
    visible_ = createBuffer(device_, rw | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uint64_t(maxVisible_) * 8, "visible clusters");
    candidates_ = createBuffer(device_, rw, uint64_t(maxVisible_) * 8, "occlusion candidates");
    counters_ = createBuffer(device_, rw, kCounterCount * 4, "cull counters");
    pickBuffer_ = createBuffer(device_, rw, 16, "pick result");
    args_ = createBuffer(device_, SDL_GPU_BUFFERUSAGE_INDIRECT | rw, kArgsBytes, "indirect args");
    indices_ = createBuffer(device_, SDL_GPU_BUFFERUSAGE_INDEX | rw, uint64_t(maxVisible_) * kIndicesPerCluster * 4,
                            "cluster indices");
    triangleBase_ = createBuffer(device_, rw | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, uint64_t(maxVisible_) * 4,
                                 "cluster triangle bases");
    if (!pointSampler_ || !chunks_ || !visible_ || !candidates_ || !counters_ || !args_ || !indices_ || !triangleBase_)
        return false;

    // Dynamic instances: last frame's rows (motion vectors) start as the initial table, and a
    // transfer buffer large enough to rewrite every row twice (current + previous) per frame.
    mirror_ = pak.instances();
    const uint32_t tableBytes = instanceCount_ * uint32_t(sizeof(fpak::InstanceGpu));
    prevInstances_ = createBuffer(device_, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, tableBytes,
                                  "previous instances");
    SDL_GPUTransferBufferCreateInfo up = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, std::max(tableBytes * 2, 64u), 0};
    instanceUpload_ = gpumem::createTransferBuffer("Renderer", device_, &up, "instance upload");
    if (!prevInstances_ || !instanceUpload_)
        return false;
    {
        void* mapped = SDL_MapGPUTransferBuffer(device_, instanceUpload_, false);
        std::memcpy(mapped, mirror_.data(), tableBytes);
        SDL_UnmapGPUTransferBuffer(device_, instanceUpload_);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src = {instanceUpload_, 0};
        const SDL_GPUBufferRegion dst = {prevInstances_, 0, tableBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    terrain_.init(device_, pak, vsm);
    live_.init(device_, pak, &vsm);
    // Ray tracing: a BLAS per cooked mesh now; live meshes and terrain patches as they are written.
    if (bvh_.init(device_, pak)) {
        bvh_.setInstanceRows(&mirror_);
        bvh_.setDeformed(&deformOf_);
        live_.setBvh(&bvh_);
        terrain_.setBvh(&bvh_);
        if (!probes_.init(gpu, pak, vt))
            FX_WARN("probe gi: unavailable");
    } else {
        FX_WARN("ray tracing: unavailable");
    }
    if (!terrainSurface_.init(gpu))
        return false;
    terrain_.setSurface(&terrainSurface_);
    if (!scatter_.init(gpu, pak, vsm) || !generator_.init(gpu))
        return false;
    if (!water_.init(gpu))
        FX_WARN("water: unavailable");
    if (!particles_.init(gpu, pak, vt))
        FX_WARN("particles: unavailable");
    if (!transparent_.init(gpu))
        return false;
    if (!decals_.init(gpu))
        return false;
    if (!layers_.init(device_, 64))
        return false;

    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
        SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, kCounterCount * 4, 0};
        readback_[i] = gpumem::createTransferBuffer("Renderer", device_, &t, "cull stats readback");
        t.size = (1 + VirtualTextureSystem::kFeedbackCapacity) * 4;
        feedbackReadback_[i] = gpumem::createTransferBuffer("Renderer", device_, &t, "vt feedback readback");
        t.size = VirtualShadowMap::kCounterWords * 4;
        vsmReadback_[i] = gpumem::createTransferBuffer("Renderer", device_, &t, "shadow stats readback");
        t.size = 16;
        pickReadback_[i] = gpumem::createTransferBuffer("Renderer", device_, &t, "pick readback");
    }
    FX_INFO("gpu-driven: %u instances, %llu cluster instances, %u chunks, %u max visible clusters (%.0f MB index buffer)",
            instanceCount_, (unsigned long long)clusterInstances, maxChunks_, maxVisible_,
            double(maxVisible_) * kIndicesPerCluster * 4 / (1 << 20));
    return true;
}

void GpuDrivenRenderer::setTerrain(const Terrain* terrain, const std::string& scenePath)
{
    if (!pak_->terrainSlots().patches) {
        if (terrain)
            FX_WARN("terrain: the scene was loaded without terrain slots; it shows after a restart");
        return;
    }
    terrain_.setTerrain(terrain);
    terrainSurface_.setTerrain(terrain, scenePath);
    scatter_.setTerrain(terrain);
    float lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        lo[a] = pak_->header().boundsMin[a];
        hi[a] = pak_->header().boundsMax[a];
    }
    if (terrain && terrain->valid()) {
        float tlo[3], thi[3];
        terrain->bounds(tlo, thi);
        // Heights: the whole range they can take (sculpting stays inside the shadow volume).
        tlo[1] = terrain->settings().heightMin;
        thi[1] = terrain->settings().heightMax;
        // An empty scene's bounds are a point at the origin: the terrain alone then.
        const bool emptyScene = pak_->cookedInstanceCount() == 0;
        for (int a = 0; a < 3; ++a) {
            lo[a] = emptyScene ? tlo[a] : std::min(lo[a], tlo[a]);
            hi[a] = emptyScene ? thi[a] : std::max(hi[a], thi[a]);
        }
    }
    vsm_->setSceneBounds(lo, hi);
}

void GpuDrivenRenderer::setProjectShaders(const std::string& resolvePath, const std::string& transparentPath,
                                          const std::string& deformPath)
{
    setProjectResolve(resolvePath);
    {
        ShaderFile deform;
        std::string why;
        std::error_code e;
        const bool there = !deformPath.empty() && std::filesystem::exists(deformPath, e);
        if (there && !loadShaderFile(deformPath, deform, why))
            FX_WARN("renderer: the project's vertex pass %s: %s (the engine's is used)", deformPath.c_str(), why.c_str());
        skinning_->setProjectDeform(there && why.empty() ? &deform : nullptr);
    }
    ShaderFile file;
    std::string error;
    std::error_code ec;
    const bool there = !transparentPath.empty() && std::filesystem::exists(transparentPath, ec);
    if (there && !loadShaderFile(transparentPath, file, error))
        FX_WARN("renderer: the project's transparent pass %s: %s (the engine's is used)", transparentPath.c_str(), error.c_str());
    transparent_.setProjectShader(there && error.empty() ? &file : nullptr);
}

void GpuDrivenRenderer::setProjectResolve(const std::string& path)
{
    ShaderFile file;
    std::string error;
    std::error_code ec;
    const ShaderFile::Entry* entry = !path.empty() && std::filesystem::exists(path, ec) && loadShaderFile(path, file, error)
                                         ? file.entry("cs_resolve")
                                         : nullptr;
    SDL_GPUComputePipeline* made = entry && gpu_ ? gpu_->createComputePipeline(entry->blob) : nullptr;
    if (!path.empty() && std::filesystem::exists(path, ec) && !made)
        FX_WARN("renderer: the project's resolve %s: %s (the engine's is used)", path.c_str(), error.empty() ? "no pipeline" : error.c_str());
    if (projectResolve_)
        SDL_ReleaseGPUComputePipeline(device_, projectResolve_);  // (released once the frames using it are done)
    projectResolve_ = made;
    resolve_ = made ? made : builtinResolve_;
    if (made)
        FX_INFO("renderer: the project's resolve (its surface shaders) from %s", path.c_str());
}

SDL_GPUTextureSamplerBinding GpuDrivenRenderer::probeBinding(int k) const
{
    if (!probes_.irradiance())
        return {post_.ao(), post_.pointSampler()};
    return k == 0 ? SDL_GPUTextureSamplerBinding{probes_.irradiance(), probes_.linear()}
           : k == 1 ? SDL_GPUTextureSamplerBinding{probes_.distance(), probes_.linear()}
                    : SDL_GPUTextureSamplerBinding{probes_.data(), probes_.point()};
}

void GpuDrivenRenderer::shutdown()
{
    probes_.shutdown();
    bvh_.shutdown();
    terrain_.shutdown();
    live_.shutdown();
    terrainSurface_.shutdown();
    water_.shutdown();
    particles_.shutdown();
    transparent_.shutdown();
    decals_.shutdown();
    layers_.shutdown();
    layeredOf_.clear();
    layeredChanged_.clear();
    hiddenSlots_.clear();
    ownHidden_.clear();
    scatter_.shutdown();
    generator_.shutdown();
    if (!device_)
        return;
    fsr_.shutdown();
    dlss_.shutdown();
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
        if (readbackFence_[i])
            SDL_ReleaseGPUFence(device_, readbackFence_[i]);
        gpumem::releaseTransferBuffer(device_, readback_[i]);
        gpumem::releaseTransferBuffer(device_, pickReadback_[i]);
        gpumem::releaseTransferBuffer(device_, feedbackReadback_[i]);
        gpumem::releaseTransferBuffer(device_, vsmReadback_[i]);
    }
    for (SDL_GPUTexture* t : {visTexture_, depth_, color_, reactive_, reflectSource_, reflectWeight_, reflectNormal_, reflectMask_, reflectHits_, reflectFar_})
        gpumem::releaseTexture(device_, t);
    releaseMsaa();
    if (projectResolve_)
        SDL_ReleaseGPUComputePipeline(device_, std::exchange(projectResolve_, nullptr));
    resolve_ = builtinResolve_;
    for (SDL_GPUBuffer* b : {chunks_, visible_, candidates_, counters_, args_, indices_, triangleBase_, hiz_, pickBuffer_})
        gpumem::releaseBuffer(device_, b);
    for (SDL_GPUComputePipeline* p : {reset_, instances_, argsCull1_, cullFirst_, argsDraw1_, cullSecond_, argsDraw2_,
                                      expand_, argsRaster1_, argsRaster2_, hizDepth_, hizDown_, resolve_, reflect_, reflectTrace_, reflectFarPipe_, reflectSourcePipe_, viewHiz_, rayView_, probeView_, vtClear_, pick_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    SDL_ReleaseGPUGraphicsPipeline(device_, visbuffer_);
    SDL_ReleaseGPUGraphicsPipeline(device_, visbufferMasked_);
    SDL_ReleaseGPUSampler(device_, pointSampler_);
    SDL_ReleaseGPUSampler(device_, mipSampler_);
    gpumem::releaseBuffer(device_, prevInstances_);
    gpumem::releaseTransferBuffer(device_, instanceUpload_);
    post_.shutdown();
    device_ = nullptr;
}

uint32_t GpuDrivenRenderer::msaaSamples(uint32_t requested) const
{
    for (uint32_t samples = std::min(std::bit_floor(std::max(requested, 1u)), 8u); samples > 1; samples /= 2)
        if (SDL_GPUTextureSupportsSampleCount(device_, kColorFormat, sampleCountOf(samples)) &&
            SDL_GPUTextureSupportsSampleCount(device_, kDepthFormat, sampleCountOf(samples)))
            return samples;
    return 1;
}

void GpuDrivenRenderer::releaseMsaa()
{
    for (SDL_GPUTexture* t : {msaaColor_, msaaDepth_, colorAA_})
        gpumem::releaseTexture(device_, t);
    msaaColor_ = msaaDepth_ = colorAA_ = nullptr;
    for (SDL_GPUGraphicsPipeline* p : {msaaFill_, msaaOpaque_, msaaMasked_})
        SDL_ReleaseGPUGraphicsPipeline(device_, p);
    msaaFill_ = msaaOpaque_ = msaaMasked_ = nullptr;
    msaaSamples_ = 1;
    msaaWidth_ = msaaHeight_ = 0;
}

bool GpuDrivenRenderer::ensureMsaa(uint32_t samples)
{
    if (samples == msaaSamples_ && msaaWidth_ == width_ && msaaHeight_ == height_ && colorAA_)
        return true;
    if (samples != msaaSamples_ || !msaaFill_) {
        releaseMsaa();
        SDL_GPUShader* stages[6] = {gpu_->createShader(shaders::msaa_edges_vs_fill),   gpu_->createShader(shaders::msaa_edges_fs_fill),
                                    gpu_->createShader(shaders::msaa_edges_vs_main),   gpu_->createShader(shaders::msaa_edges_fs_main),
                                    gpu_->createShader(shaders::msaa_edges_masked_vs_masked),
                                    gpu_->createShader(shaders::msaa_edges_masked_fs_masked)};
        if (std::find(std::begin(stages), std::end(stages), nullptr) == std::end(stages)) {
            SDL_GPUColorTargetDescription target = {};
            target.format = kColorFormat;
            SDL_GPUGraphicsPipelineCreateInfo info = {};
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // as the visibility raster
            info.multisample_state.sample_count = sampleCountOf(samples);
            info.target_info.color_target_descriptions = &target;
            info.target_info.num_color_targets = 1;
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = kDepthFormat;
            info.vertex_shader = stages[0];
            info.fragment_shader = stages[1];
            msaaFill_ = SDL_CreateGPUGraphicsPipeline(device_, &info);  // no depth test: every sample
            info.depth_stencil_state.enable_depth_test = true;
            info.depth_stencil_state.enable_depth_write = true;
            info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
            info.vertex_shader = stages[2];
            info.fragment_shader = stages[3];
            msaaOpaque_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
            info.vertex_shader = stages[4];
            info.fragment_shader = stages[5];
            msaaMasked_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        }
        for (SDL_GPUShader* s : stages)
            if (s)
                SDL_ReleaseGPUShader(device_, s);
        if (!msaaFill_ || !msaaOpaque_ || !msaaMasked_) {
            FX_ERROR("msaa %ux pipelines: %s", samples, SDL_GetError());
            releaseMsaa();
            msaaBroken_ = true;  // don't retry every frame
            return false;
        }
        msaaSamples_ = samples;
    }
    for (SDL_GPUTexture* t : {msaaColor_, msaaDepth_, colorAA_})
        gpumem::releaseTexture(device_, t);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width_;
    info.height = height_;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = sampleCountOf(samples);
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    msaaColor_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, msaaColor_, "msaa color");
    info.format = kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    msaaDepth_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, msaaDepth_, "msaa depth");
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colorAA_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, colorAA_, "hdr color (msaa resolved)");
    msaaWidth_ = width_;
    msaaHeight_ = height_;
    if (!msaaColor_ || !msaaDepth_ || !colorAA_) {
        FX_ERROR("msaa %ux targets: %s", samples, SDL_GetError());
        releaseMsaa();
        return false;
    }
    return true;
}

void GpuDrivenRenderer::renderMsaa(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* scene, const float4x4& rasterViewProj,
                                   uint32_t width, uint32_t height)
{
    FX_GPU_SECTION(cmd, "MSAA");
    SDL_GPUColorTargetInfo target = {};
    target.texture = msaaColor_;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;  // the fill writes every sample
    target.store_op = SDL_GPU_STOREOP_RESOLVE;
    target.resolve_texture = colorAA_;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = msaaDepth_;
    depth.clear_depth = 0.0f;  // reverse-Z far
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    const MsaaUniforms mu = {{width, height}, vt_->pageTableOffset(), vt_->mipBias()};
    SDL_PushGPUFragmentUniformData(cmd, 0, &mu, sizeof(mu));
    SDL_PushGPUVertexUniformData(cmd, 0, &rasterViewProj, sizeof(rasterViewProj));
    gpuprof::pass(cmd, "msaa raster");
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &target, 1, &depth);
    const SDL_GPUTextureSamplerBinding frame[3] = {{color_, pointSampler_}, {depth_, pointSampler_},
                                                   {vt_->physical(0), vt_->sampler()}};  // (albedo: masked)
    // 1. Every sample: the sky next to the pixel (or the pixel's own colour).
    SDL_BindGPUGraphicsPipeline(rp, msaaFill_);
    SDL_BindGPUFragmentSamplers(rp, 0, frame, 2);
    SDL_BindGPUFragmentStorageTextures(rp, 0, &visTexture_, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    // 2. The visibility draws of both occlusion passes (disjoint index ranges), opaque then
    //    alpha-masked; samples a triangle covers take its shaded colour.
    SDL_GPUBuffer* vsBuffers[4] = {visible_, scene, skinning_->buffer(), triangleBase_};
    const SDL_GPUBufferBinding ib = {indices_, 0};
    SDL_BindGPUGraphicsPipeline(rp, msaaOpaque_);
    SDL_BindGPUVertexStorageBuffers(rp, 0, vsBuffers, 4);
    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_BindGPUFragmentSamplers(rp, 0, frame, 2);
    SDL_BindGPUFragmentStorageTextures(rp, 0, &visTexture_, 1);
    SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, kArgDraw1, 1);
    SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, kArgDraw2, 1);
    SDL_BindGPUGraphicsPipeline(rp, msaaMasked_);
    SDL_BindGPUVertexStorageBuffers(rp, 0, vsBuffers, 4);
    SDL_BindGPUFragmentSamplers(rp, 0, frame, 3);
    SDL_BindGPUFragmentStorageTextures(rp, 0, &visTexture_, 1);
    SDL_GPUBuffer* materialData = vt_->materialData();
    SDL_BindGPUFragmentStorageBuffers(rp, 0, &materialData, 1);
    SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, kArgDraw1Masked, 1);
    SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, kArgDraw2Masked, 1);
    SDL_EndGPURenderPass(rp);
}

bool GpuDrivenRenderer::ensureTargets(uint32_t width, uint32_t height)
{
    if (color_ && width == width_ && height == height_)
        return true;
    for (SDL_GPUTexture* t : {visTexture_, depth_, color_, reactive_, reflectSource_, reflectWeight_, reflectNormal_, reflectMask_, reflectHits_, reflectFar_})
        gpumem::releaseTexture(device_, t);
    gpumem::releaseBuffer(device_, hiz_);

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = kVisFormat;
    // Graphics read: the editor's selection outline, MSAA.
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
                 SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    visTexture_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, visTexture_, "visibility buffer");
    info.format = kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depth_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, depth_, "depth");
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;  // (particles draw into it)
    color_ = gpumem::createTexture("Renderer", device_, &info);
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    gpumem::setTextureName(device_, color_, "hdr color");
    {
        // TAA's and FSR's reactive mask: the coverage of what has no motion vectors (transparent
        // surfaces, particles), written by the first of them each frame.
        SDL_GPUTextureCreateInfo r = info;
        r.format = ParticleRenderer::kReactiveFormat;
        r.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        reactive_ = gpumem::createTexture("Renderer", device_, &r);
        gpumem::setTextureName(device_, reactive_, "reactive mask");
    }
    // Reflections: the resolve's per-pixel specular weight and normal, and a copy of its image
    // with mips (what screen-space reflections see, blurrier when glossy).
    reflectWeight_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, reflectWeight_, "reflection weight");
    reflectNormal_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, reflectNormal_, "reflection normal");
    reflectMask_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, reflectMask_, "reflection mask");
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    info.num_levels = std::min(uint32_t(std::bit_width(std::max(width, height))), 7u);  // (reflections read up to level 6)
    reflectLevels_ = info.num_levels;
    reflectSource_ = gpumem::createTexture("Renderer", device_, &info);
    gpumem::setTextureName(device_, reflectSource_, "reflection source");
    info.num_levels = 1;
    // The screen-space hits, traced at half resolution (uv, mip, trust).
    {
        SDL_GPUTextureCreateInfo h = info;
        h.width = width;  // (half resolution uses its top left quarter)
        h.height = height;
        h.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        h.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        reflectHits_ = gpumem::createTexture("Renderer", device_, &h);
        gpumem::setTextureName(device_, reflectHits_, "reflection hits");
        h.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        reflectFar_ = gpumem::createTexture("Renderer", device_, &h);
        gpumem::setTextureName(device_, reflectFar_, "reflection far");
    }

    // HiZ levels: level 0 is half resolution, halving down to 1x1.
    uint32_t w = (width + 1) / 2, h = (height + 1) / 2, offset = 0;
    hizLevelCount_ = 0;
    while (hizLevelCount_ < kMaxHizLevels) {
        hizLevels_[hizLevelCount_++] = {offset, w, h};
        offset += w * h;
        if (w == 1 && h == 1)
            break;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    hiz_ = createBuffer(device_, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                     SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,  // (the transparent pass's refraction)
                        uint64_t(offset) * 4, "hiz");
    hizValid_ = false;
    width_ = width;
    height_ = height;
    return visTexture_ && depth_ && color_ && reactive_ && reflectWeight_ && reflectNormal_ && reflectMask_ && reflectSource_ && reflectHits_ && reflectFar_ && hiz_ &&
           post_.resizeRender(width, height);
}

void GpuDrivenRenderer::buildHiz(SDL_GPUCommandBuffer* cmd)
{
    FX_ZONE_CAT("buildHiz", Rendering);
    static const char* const kLevelNames[] = {"hiz 0", "hiz 1", "hiz 2", "hiz 3", "hiz 4", "hiz 5", "hiz 6", "hiz 7",
                                              "hiz 8", "hiz 9", "hiz 10", "hiz 11", "hiz 12", "hiz 13", "hiz 14", "hiz 15"};
    SDL_GPUStorageBufferReadWriteBinding rw = {hiz_, false};
    for (uint32_t level = 0; level < hizLevelCount_; ++level) {
        HizUniforms u = {};
        const HizLevel& dst = hizLevels_[level];
        if (level == 0) {
            u.src[1] = width_;
            u.src[2] = height_;
        } else {
            const HizLevel& src = hizLevels_[level - 1];
            u.src[0] = src.offset;
            u.src[1] = src.width;
            u.src[2] = src.height;
        }
        u.dst[0] = dst.offset;
        u.dst[1] = dst.width;
        u.dst[2] = dst.height;
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
        // One pass per level: SDL only synchronizes between passes.
        gpuprof::pass(cmd, kLevelNames[std::min(level, 15u)]);
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        if (level == 0) {
            SDL_BindGPUComputePipeline(pass, hizDepth_);
            SDL_GPUTextureSamplerBinding depth = {depth_, pointSampler_};
            SDL_BindGPUComputeSamplers(pass, 0, &depth, 1);
        } else {
            SDL_BindGPUComputePipeline(pass, hizDown_);
        }
        SDL_DispatchGPUCompute(pass, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
    }
}

void GpuDrivenRenderer::requestPick(uint32_t x, uint32_t y)
{
    pickX_ = x;
    pickY_ = y;
    ++pickRequest_;
}

bool GpuDrivenRenderer::pickResult(uint32_t& slot, uint32_t* cluster, uint32_t* triangle)
{
    if (pickAnswered_ == 0)
        return false;
    slot = pickSlot_;
    if (cluster)
        *cluster = pickCluster_;
    if (triangle)
        *triangle = pickTriangle_;
    pickAnswered_ = 0;
    return true;
}

float GpuDrivenRenderer::scatterShadowTexels() const { return vsm_->scatterTexels(); }

void GpuDrivenRenderer::pollStats()
{
    FX_ZONE_CAT("Renderer stats readback", Rendering);
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
        if (!readbackFence_[i] || !SDL_QueryGPUFence(device_, readbackFence_[i]))
            continue;
        gpuprof::completed(readbackTicket_[i]);
        const uint32_t* c = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device_, readback_[i], false));
        if (c && readbackFrame_[i] >= stats_.frame) {
            const auto* v = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device_, vsmReadback_[i], false));
            stats_ = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9],
                      v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 0, v ? v[4] : 0, v ? v[5] : 0, v ? v[6] : 0,
                      v ? v[9] : 0, c[14], c[15],
                      readbackFrame_[i], true};
            if (v) {
                static const uint32_t shadowsName = profiler::intern("Shadows", profiler::Category::Rendering);
                float shadowMs = -1.0f;
                if (!gpuprof::sectionMs(readbackTicket_[i], shadowsName, shadowMs))
                    shadowMs = -1.0f;
                vsm_->noteFrame(v, shadowMs);
                SDL_UnmapGPUTransferBuffer(device_, vsmReadback_[i]);
            }
            vsm_->noteRecordOverflow(stats_.vsmOverflow != 0);
            // Scatter's smallest size on screen: raised at once by about what the visible clusters
            // wanted over the budget (their counts go on past it), lowered slowly (and only well
            // under it) so the edge where far grass ends doesn't come and go.
            const uint32_t used = stats_.visible1 + stats_.visible2 + stats_.masked1 + stats_.masked2;
            if ((stats_.overflow & 3) || used > maxVisible_ / 10 * 9) {
                const float over = std::sqrt(float(used) / (float(maxVisible_) * 0.8f));
                scatterPixels_ = std::min(std::max(scatterPixels_, 2.0f) * std::clamp(over, 1.1f, 2.0f), 512.0f);
                scatterCalm_ = 0;
            } else if (scatterPixels_ > 0.0f && used < maxVisible_ / 10 * 6 && ++scatterCalm_ >= 30) {
                scatterPixels_ = scatterPixels_ * 0.9f < 2.0f ? 0.0f : scatterPixels_ * 0.9f;
                scatterCalm_ = 0;
            }
        }
        if (c)
            SDL_UnmapGPUTransferBuffer(device_, readback_[i]);
        if (pickSlotRequest_[i] != 0) {
            if (const auto* p = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device_, pickReadback_[i], false))) {
                if (pickSlotRequest_[i] == pickRequest_) {  // a newer click supersedes older answers
                    pickSlot_ = p[0];
                    pickCluster_ = p[1];
                    pickTriangle_ = p[2];
                    pickAnswered_ = pickRequest_;
                }
                SDL_UnmapGPUTransferBuffer(device_, pickReadback_[i]);
            }
            pickSlotRequest_[i] = 0;
        }
        if (const auto* fb = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device_, feedbackReadback_[i], false))) {
            vt_->consumeFeedback(fb, readbackFrame_[i]);
            SDL_UnmapGPUTransferBuffer(device_, feedbackReadback_[i]);
        }
        scatter_.finishDownloads(i);
        SDL_ReleaseGPUFence(device_, readbackFence_[i]);
        readbackFence_[i] = nullptr;
    }
}

void GpuDrivenRenderer::frameSubmitted(SDL_GPUFence* fence)
{
    if (pendingSlot_ >= 0 && fence) {
        readbackFence_[pendingSlot_] = fence;
        readbackTicket_[pendingSlot_] = gpuprof::lastTicket();  // ended just before this submit
    } else if (fence) {
        SDL_ReleaseGPUFence(device_, fence);
    }
    pendingSlot_ = -1;
}

// (V1) The instances whose material has a vertex function (its override's, or one of its mesh's
// clusters'), within the material's distance of the camera, nearest first: a deform record each
// (render/skinning.h; the budget decides how many), their rows flagged skinned with it. The rest
// lose theirs. Rows that change for it go into `rows` (this frame's stream).
void GpuDrivenRenderer::updateDeformers(std::vector<InstanceUpdate>& rows)
{
    const auto& materials = pak_->materials();
    const auto& meshes = pak_->meshes();
    uint64_t signature = materials.size();
    for (uint32_t m = 0; m < materials.size(); ++m)
        if (materials[m].flags & fpak::kMaterialVertex)
            signature = signature * 1099511628211ull + m + 1;
    if (signature == materials.size() && deformOf_.empty())
        return;  // (no vertex functions)
    FX_ZONE_CAT("GpuDrivenRenderer::updateDeformers", Rendering);
    // An instance's row as it is this frame (this frame's stream, else the last streamed), and a
    // changed one put into this frame's stream.
    std::unordered_map<uint32_t, size_t> streamed;
    for (size_t i = 0; i < rows.size(); ++i)
        streamed[rows[i].slot] = i;
    auto rowOf = [&](uint32_t s) -> const fpak::InstanceGpu& {
        const auto it = streamed.find(s);
        return it != streamed.end() ? rows[it->second].row : mirror_[s];
    };
    auto put = [&](uint32_t s, const fpak::InstanceGpu& row) {
        if (const auto it = streamed.find(s); it != streamed.end()) {
            rows[it->second].row = row;
        } else {
            streamed[s] = rows.size();
            rows.push_back({s, row});
        }
    };
    if (signature != deformSignature_ || meshVertex_.size() != meshes.size()) {
        deformSignature_ = signature;
        meshVertex_.assign(meshes.size(), {});
        uint32_t clusterCount = 0;
        const fpak::ClusterGpu* clusters = pak_->view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
        for (uint32_t i = 0; i < meshes.size(); ++i)
            for (uint32_t c = meshes[i].clusterOffset; c < meshes[i].clusterOffset + meshes[i].clusterCount && c < clusterCount; ++c) {
                const uint32_t m = clusters[c].counts >> 16;
                if (m < materials.size() && (materials[m].flags & fpak::kMaterialVertex) &&
                    std::find(meshVertex_[i].begin(), meshVertex_[i].end(), m) == meshVertex_[i].end())
                    meshVertex_[i].push_back(m);
            }
        deformScanFrame_ = 0;
    }
    // What vertex-function materials an instance draws with (an override replaces the mesh's).
    auto vertexMaterials = [&](const fpak::InstanceGpu& r, uint32_t (&out)[8]) {
        const uint32_t own = r.flags >> 16;
        if (own != 0)
            return own - 1 < materials.size() && (materials[own - 1].flags & fpak::kMaterialVertex) ? (out[0] = own - 1, 1u) : 0u;
        uint32_t n = 0;
        if (r.mesh < meshVertex_.size())
            for (const uint32_t m : meshVertex_[r.mesh])
                if (n < 8)
                    out[n++] = m;
        return n;
    };
    // The candidates: found again every half second (overrides and pools change slowly).
    if (deformScanFrame_ == 0 || frame_ - deformScanFrame_ >= 30) {
        deformScanFrame_ = std::max<uint64_t>(frame_, 1);
        deformCandidates_.clear();
        uint32_t m[8];
        for (uint32_t s = 0; s < mirror_.size(); ++s) {
            const fpak::InstanceGpu& r = rowOf(s);
            const bool character = (r.flags & fpak::kInstanceSkinned) && !deformOf_.count(s);
            if (!character && vertexMaterials(r, m))
                deformCandidates_.push_back(s);
        }
    }
    // Wanted: candidates shown and within their distance, nearest first.
    struct Want {
        uint32_t slot;
        float distance, bound;
    };
    std::vector<Want> wanted;
    for (const uint32_t s : deformCandidates_) {
        const fpak::InstanceGpu& r = rowOf(s);
        uint32_t m[8];
        const uint32_t n = vertexMaterials(r, m);
        if ((r.flags & fpak::kInstanceHidden) || !n || r.mesh >= meshes.size())
            continue;
        float limit = 0.0f, bound = 0.0f;
        bool always = false;
        for (uint32_t k = 0; k < n; ++k) {
            const float* p = engineParams_ ? engineParams_(m[k]) : nullptr;
            const float d = p ? p[3] : 0.0f;
            always |= d <= 0.0f;
            limit = std::max(limit, d);
            bound = std::max(bound, p ? p[2] : 1.0f);
        }
        const fpak::MeshGpu& mesh = meshes[r.mesh];
        const float3 half = float3{mesh.posScale[0], mesh.posScale[1], mesh.posScale[2]} * (65535.0f * 0.5f);
        const float3 c = float3{mesh.posOffset[0], mesh.posOffset[1], mesh.posOffset[2]} + half;
        float scale = 0.0f;
        for (int col = 0; col < 3; ++col)
            scale = std::max(scale, length(float3{r.world[0][col], r.world[1][col], r.world[2][col]}));
        float3 center;
        for (int row = 0; row < 3; ++row)
            (&center.x)[row] = r.world[row][0] * c.x + r.world[row][1] * c.y + r.world[row][2] * c.z + r.world[row][3];
        const float distance = std::max(length(center - lastCamera_) - length(half) * scale - bound, 0.0f);
        if (always || distance <= limit)
            wanted.push_back({s, distance, bound / std::max(scale, 1e-6f)});
    }
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.distance < b.distance; });
    // Unbind those no longer wanted (their rows lose the flag in this frame's stream, so the
    // record is free before anything reads it again).
    std::vector<uint32_t> keep;
    keep.reserve(wanted.size());
    for (const Want& w : wanted)
        keep.push_back(w.slot);
    std::sort(keep.begin(), keep.end());
    for (auto it = deformOf_.begin(); it != deformOf_.end();) {
        if (std::binary_search(keep.begin(), keep.end(), it->first)) {
            ++it;
            continue;
        }
        skinning_->unbindDeform(it->second);
        fpak::InstanceGpu row = rowOf(it->first);
        row.flags &= ~uint32_t(fpak::kInstanceSkinned);
        row.skin = 0;
        put(it->first, row);
        it = deformOf_.erase(it);
    }
    // Bind the nearest first, while the budget lasts; every bound one is re-bounded and posed (its
    // shadow pages follow it) each frame.
    for (const Want& w : wanted) {
        auto it = deformOf_.find(w.slot);
        if (it == deformOf_.end()) {
            const uint32_t record = skinning_->bindDeform(w.slot, rowOf(w.slot).mesh);
            if (record == ~0u)
                continue;
            it = deformOf_.emplace(w.slot, record).first;
            fpak::InstanceGpu row = rowOf(w.slot);
            row.flags |= fpak::kInstanceSkinned;
            row.skin = record;
            put(w.slot, row);
        }
        const fpak::MeshGpu& mesh = meshes[rowOf(w.slot).mesh];
        const float3 half = float3{mesh.posScale[0], mesh.posScale[1], mesh.posScale[2]} * (65535.0f * 0.5f);
        const float3 c = float3{mesh.posOffset[0], mesh.posOffset[1], mesh.posOffset[2]} + half;
        skinning_->setBounds(it->second, {c.x, c.y, c.z, length(half) + w.bound});
        skinning_->markPosed(it->second);
    }
}

uint32_t GpuDrivenRenderer::deformedVertices() const { return skinning_->deformVerticesUsed(); }

void GpuDrivenRenderer::setInstanceHidden(uint32_t slot, bool hidden)
{
    if (hidden ? !hiddenSlots_.insert(slot).second : hiddenSlots_.erase(slot) == 0)
        return;
    if (!hidden && slot < mirror_.size()) {  // (its own row back: hidden only if it hid itself)
        const auto own = ownHidden_.find(slot);
        if (own == ownHidden_.end() || !own->second)
            mirror_[slot].flags &= ~uint32_t(fpak::kInstanceHidden);
        if (own != ownHidden_.end())
            ownHidden_.erase(own);
    }
    layeredChanged_.push_back(slot);
}

void GpuDrivenRenderer::setInstanceLayer(uint32_t slot, int32_t layer)
{
    const auto it = layeredOf_.find(slot);
    if (layer < 0 ? it == layeredOf_.end() : it != layeredOf_.end() && it->second == uint32_t(layer))
        return;
    if (layer < 0)
        layeredOf_.erase(it);
    else
        layeredOf_[slot] = uint32_t(layer);
    layeredChanged_.push_back(slot);
}

void GpuDrivenRenderer::streamInstances(SDL_GPUCommandBuffer* cmd, const std::vector<InstanceUpdate>& incoming)
{
    FX_ZONE_CAT("GpuDrivenRenderer::streamInstances", Rendering);
    // (V1) Deformed instances: their rows keep the skinned flag and record (the scene's rows don't
    // have them); rows whose deforming starts or stops are added.
    std::vector<InstanceUpdate> patched;
    const bool deforming = !deformOf_.empty() || deformSignature_ != ~0ull || [&] {
        for (const fpak::MaterialGpu& m : pak_->materials())
            if (m.flags & fpak::kMaterialVertex)
                return true;
        return false;
    }();
    // Layered level meshes: their rows carry the layer (the scene's rows don't have it); rows
    // whose layer changed are streamed again.
    const bool layering = !layeredOf_.empty() || !layeredChanged_.empty() || !hiddenSlots_.empty();
    if (deforming || layering) {
        patched = incoming;
        if (layering) {
            std::unordered_set<uint32_t> present;
            for (const InstanceUpdate& u : patched)
                present.insert(u.slot);
            for (const uint32_t s : layeredChanged_)
                if (s < mirror_.size() && present.insert(s).second)
                    patched.push_back({s, mirror_[s]});
            layeredChanged_.clear();
        }
        const size_t fromScene = incoming.size();  // (rows after these: last streamed ones, already patched)
        for (size_t i = 0; i < patched.size(); ++i) {
            InstanceUpdate& u = patched[i];
            if (const auto it = deformOf_.find(u.slot); it != deformOf_.end()) {
                u.row.flags |= fpak::kInstanceSkinned;
                u.row.skin = it->second;
            }
            if (hiddenSlots_.count(u.slot)) {  // (whether its own row hides it: remembered for when it shows again)
                if (i < fromScene || !ownHidden_.count(u.slot))  // (the scene's row, or the last one before it was patched)
                    ownHidden_[u.slot] = (u.row.flags & fpak::kInstanceHidden) != 0;
                u.row.flags |= fpak::kInstanceHidden;
            }
            if (const auto it = layeredOf_.find(u.slot); it != layeredOf_.end()) {
                u.row.flags |= fpak::kInstanceLayered;
                u.row.skin = it->second;
            } else if (u.row.flags & fpak::kInstanceLayered) {
                u.row.flags &= ~uint32_t(fpak::kInstanceLayered);
                u.row.skin = 0;
            }
        }
        if (deforming)
            updateDeformers(patched);
    }
    const std::vector<InstanceUpdate>& rows = deforming || layering ? patched : incoming;
    if (rows.empty() && lastStreamed_.empty() && movingSlots_.empty())
        return;
    // World bounding sphere of an instance row (none when hidden).
    const auto* meshes = pak_->meshes().data();
    auto sphere = [&](const fpak::InstanceGpu& r, float3& center, float& radius) {
        if (r.flags & fpak::kInstanceHidden)
            return false;
        const fpak::MeshGpu& m = meshes[r.mesh];
        const float3 half = float3{m.posScale[0], m.posScale[1], m.posScale[2]} * (65535.0f * 0.5f);
        const float3 c = float3{m.posOffset[0], m.posOffset[1], m.posOffset[2]} + half;
        float scale = 0.0f;
        for (int col = 0; col < 3; ++col)
            scale = std::max(scale, length(float3{r.world[0][col], r.world[1][col], r.world[2][col]}));
        for (int row = 0; row < 3; ++row)
            (&center.x)[row] = r.world[row][0] * c.x + r.world[row][1] * c.y + r.world[row][2] * c.z + r.world[row][3];
        radius = length(half) * scale;
        return true;
    };
    // Shadow pages under the old and the new pose: one sphere enclosing both while they overlap
    // (a body moving), two for a jump (a recycled pool slot). `movingOnly`: only the moving
    // casters' layer of those pages goes (a negative radius), their static layer stays.
    movedBounds_.clear();
    auto invalidate = [&](const fpak::InstanceGpu& old, const fpak::InstanceGpu& row, bool movingOnly) {
        const float sign = movingOnly ? -1.0f : 1.0f;
        float3 c0, c1;
        float r0, r1;
        const bool a = sphere(old, c0, r0), b = sphere(row, c1, r1);
        if (a && b && length(c1 - c0) > r0 + r1) {
            movedBounds_.push_back({c0.x, c0.y, c0.z, r0 * sign});
            movedBounds_.push_back({c1.x, c1.y, c1.z, r1 * sign});
        } else if (a && b) {
            const float d = length(c1 - c0);
            const float r = std::max({r0, r1, (d + r0 + r1) * 0.5f});
            const float3 c = d > 1e-6f && r > r0 ? c0 + (c1 - c0) * ((r - r0) / d) : (r0 >= r1 ? c0 : c1);
            movedBounds_.push_back({c.x, c.y, c.z, r * sign});
        } else if (a || b) {
            const float3 c = a ? c0 : c1;
            movedBounds_.push_back({c.x, c.y, c.z, (a ? r0 : r1) * sign});
        }
    };

    // Moving casters: a row that changes is flagged kInstanceDynamic until it has been still for
    // kSettleFrames, and the shadow map draws it over its pages' cached static layer instead of
    // into it. (A played model's moving parts re-rendered the whole of every page they touched,
    // the room around them included: 2-3 ms of shadows.) Its first move and its settling change
    // the static layer, so they invalidate whole pages; moving on, only the moving layer.
    constexpr uint32_t kSettleFrames = 60;
    ++streamFrame_;
    if (movedFrame_.size() < mirror_.size())
        movedFrame_.resize(mirror_.size(), 0);
    float giBox[6] = {};  // (rows shown, hidden or re-meshed: GI's region to relight)
    bool giShown = false;
    float moveBox[6] = {};  // (rows changed: the probes round them wake)
    bool moved = false;
    std::vector<InstanceUpdate> changed;  // rows as streamed (with the flag)
    changed.reserve(rows.size());
    std::vector<uint32_t> current;  // their slots
    current.reserve(rows.size());
    for (const InstanceUpdate& u : rows) {
        const fpak::InstanceGpu& old = mirror_[u.slot];
        fpak::InstanceGpu row = u.row;
        row.flags |= old.flags & fpak::kInstanceDynamic;
        if (std::memcmp(&old, &row, sizeof(old)) == 0)
            continue;
        bvh_.rowChanged(u.slot, old, row);
        for (const fpak::InstanceGpu* r : {&old, static_cast<const fpak::InstanceGpu*>(&row)}) {
            float3 c;
            float radius;
            if ((r->flags & (fpak::kInstanceSkinned | fpak::kInstanceHidden)) || !sphere(*r, c, radius))
                continue;
            for (int a = 0; a < 3; ++a) {
                moveBox[a] = moved ? std::min(moveBox[a], (&c.x)[a] - radius) : (&c.x)[a] - radius;
                moveBox[3 + a] = moved ? std::max(moveBox[3 + a], (&c.x)[a] + radius) : (&c.x)[a] + radius;
            }
            moved = true;
        }
        const bool wasMoving = (old.flags & fpak::kInstanceDynamic) != 0;
        if (!(old.flags & fpak::kInstanceHidden) && !(row.flags & fpak::kInstanceHidden))
            vsm_->enableMovingCasters();  // (once: something really moves)
        if (vsm_->movingCasters()) {
            row.flags |= fpak::kInstanceDynamic;
            if (!wasMoving && movedFrame_[u.slot] != streamFrame_)
                movingSlots_.push_back(u.slot);
            movedFrame_[u.slot] = streamFrame_;
        }
        invalidate(old, row, wasMoving);
        // GI: a row shown, hidden or given another mesh (a level mesh's live slot once it is
        // built, an entity hidden in the editor) relights the probes where it is and was first.
        // (Moves are the editor's to report: a moving body would do it every frame; it wakes them.)
        if (((old.flags ^ row.flags) & fpak::kInstanceHidden) || old.mesh != row.mesh)
            for (const fpak::InstanceGpu* r : {&old, static_cast<const fpak::InstanceGpu*>(&row)}) {
                float3 c;
                float radius;
                if (!(r->flags & fpak::kInstanceSkinned) && sphere(*r, c, radius))
                    for (int a = 0; a < 3; ++a) {
                        giBox[a] = giShown ? std::min(giBox[a], (&c.x)[a] - radius) : (&c.x)[a] - radius;
                        giBox[3 + a] = giShown ? std::max(giBox[3 + a], (&c.x)[a] + radius) : (&c.x)[a] + radius;
                    }
                giShown = giShown || (!(r->flags & fpak::kInstanceSkinned) && !(r->flags & fpak::kInstanceHidden));
            }
        changed.push_back({u.slot, row});
        current.push_back(u.slot);
    }
    if (giShown)
        probes_.invalidateRegion(giBox);
    if (moved)
        probes_.wakeRegion(moveBox);
    for (size_t i = 0; i < movingSlots_.size();) {  // settled: back into the static layer
        const uint32_t s = movingSlots_[i];
        if (streamFrame_ - movedFrame_[s] < kSettleFrames) {
            ++i;
            continue;
        }
        fpak::InstanceGpu row = mirror_[s];
        row.flags &= ~uint32_t(fpak::kInstanceDynamic);
        invalidate(row, row, false);
        changed.push_back({s, row});
        current.push_back(s);
        movingSlots_[i] = movingSlots_.back();
        movingSlots_.pop_back();
    }
    if (current.empty() && lastStreamed_.empty())
        return;
    std::sort(current.begin(), current.end());
    current.erase(std::unique(current.begin(), current.end()), current.end());
    std::vector<uint32_t> previous;
    previous.reserve(current.size() + lastStreamed_.size());
    std::set_union(current.begin(), current.end(), lastStreamed_.begin(), lastStreamed_.end(), std::back_inserter(previous));

    // Runs of slots, merged across small gaps: the rows between are unchanged (their previous row
    // is their current one), so rewriting them is harmless, and a played model's thousands of
    // scattered moving parts would otherwise cost a copy each.
    constexpr uint32_t kRow = sizeof(fpak::InstanceGpu), kGap = 32;
    const auto runs = [](const std::vector<uint32_t>& slots) {
        std::vector<std::pair<uint32_t, uint32_t>> r;  // first, last
        for (const uint32_t s : slots)
            if (!r.empty() && s <= r.back().second + kGap)
                r.back().second = s;
            else
                r.push_back({s, s});
        return r;
    };
    const auto previousRuns = runs(previous), currentRuns = runs(current);
    auto* mapped = static_cast<fpak::InstanceGpu*>(SDL_MapGPUTransferBuffer(device_, instanceUpload_, true));
    uint32_t staged = 0;
    for (const auto& [a, b] : previousRuns)  // last frame's rows, before this frame's land
        for (uint32_t s = a; s <= b; ++s)
            mapped[staged++] = mirror_[s];
    const uint32_t currentAt = staged;
    for (const InstanceUpdate& u : changed)
        mirror_[u.slot] = u.row;
    for (const auto& [a, b] : currentRuns)
        for (uint32_t s = a; s <= b; ++s)
            mapped[staged++] = mirror_[s];
    SDL_UnmapGPUTransferBuffer(device_, instanceUpload_);

    FX_GPU_SECTION(cmd, "Uploads");
    FX_GPU_PASS(cmd, "instance upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    auto uploadRuns = [&](const std::vector<std::pair<uint32_t, uint32_t>>& list, uint32_t row, SDL_GPUBuffer* dst, uint32_t dstBase) {
        for (const auto& [a, b] : list) {
            const SDL_GPUTransferBufferLocation src = {instanceUpload_, row * kRow};
            const SDL_GPUBufferRegion region = {dst, dstBase + a * kRow, (b - a + 1) * kRow};
            SDL_UploadToGPUBuffer(copy, &src, &region, false);
            row += b - a + 1;
        }
    };
    uploadRuns(previousRuns, 0, prevInstances_, 0);
    uploadRuns(currentRuns, currentAt, pak_->sceneBuffer(), pak_->gpuSectionOffset(fpak::Section::Instances));
    vsm_->uploadInvalidations(copy, movedBounds_);
    SDL_EndGPUCopyPass(copy);
    lastStreamed_ = std::move(current);
}

void GpuDrivenRenderer::skipFrame(SDL_GPUCommandBuffer* cmd)
{
    FX_ZONE_CAT("GpuDrivenRenderer::skipFrame", Rendering);
    pollStats();
    pendingSlot_ = -1;
    // (posed records keep their flag: consumePosed() on the next render() covers their old and new bounds)
    skinning_->record(cmd);
    vt_->update(cmd, frame_);
    ++frame_;  // (the texture cache's clock: what the preview uses stays recent)
}

void GpuDrivenRenderer::render(SDL_GPUCommandBuffer* cmd, uint32_t displayWidth, uint32_t displayHeight,
                               const CameraState& camera, const FrameSettings& settings)
{
    FX_ZONE_CAT("GpuDrivenRenderer::render", Rendering);
    pollStats();
    if (meshes_)
        meshes_->upload(cmd);  // (R5: runtime meshes made or changed since the last frame)
    pendingSlot_ = -1;  // a frame without frameSubmitted() (e.g. a capture) leaves no fence
    // Upscaling (Lit view only; the debug views stay pixel-exact at the display size): the
    // scene renders at renderScale x the display size. Everything up to the upscaler is
    // render-sized (`width` x `height` below); post-processing after it is display-sized.
    const bool lit = settings.debugView == DebugView::Lit;
    const bool upscale = lit && settings.post.upscaler != Upscaler::Off;
    const float renderScale = upscale ? std::clamp(settings.post.renderScale, 0.25f, 1.0f) : 1.0f;
    const uint32_t width = std::clamp(uint32_t(std::lround(float(displayWidth) * renderScale)), std::min(64u, displayWidth), displayWidth);
    const uint32_t height = std::clamp(uint32_t(std::lround(float(displayHeight) * renderScale)), std::min(64u, displayHeight), displayHeight);
    if (!ensureTargets(width, height) || !post_.resizeDisplay(displayWidth, displayHeight))
        return;
    displayWidth_ = displayWidth;
    displayHeight_ = displayHeight;
    if (!upscale)
        upscalerStatus_.clear();
    else {
        char status[96];
        SDL_snprintf(status, sizeof(status), "TAAU, %ux%u to %ux%u", width, height, displayWidth, displayHeight);
        upscalerStatus_ = status;
    }
    shadowCacheEnabled_ = settings.shadowCache;
    vsm_->setBudget(settings.shadowBudgetMs);
    // Terrain patches for this camera (before the BVH, which traces them, and before culling).
    {
        const CameraState& lodCamera = settings.freezeCulling && wasFrozen_ ? frozen_ : camera;
        terrainSurface_.update(cmd, lodCamera.position);  // (first: patches take its layer colours)
        if (terrain_.update(cmd, prevInstances_, lodCamera, displayHeight, settings.terrainPixels, settings.detailDistance)) {
            std::vector<SceneBvh::ExtraInstance> extra;
            const ScenePak::TerrainSlots& ts = pak_->terrainSlots();
            for (const TerrainRenderer::PatchItem& item : terrain_.patchItems())
                extra.push_back({item.instance, item.instance - ts.firstInstance + ts.firstMesh});
            bvh_.setExtraInstances(extra);
        }
        scatter_.update(cmd, prevInstances_, terrainSurface_, lodCamera.position);
        bool all = false;
        float region[6];
        if (terrain_.takeGiChange(all, region)) {
            if (all)
                probes_.invalidate();
            else
                probes_.invalidateRegion(region);
        }
        // Live meshes written since the last frame (after streamInstances' shadow invalidations).
        live_.flush(cmd);
        if (live_.takeGiChange(region))
            probes_.invalidateRegion(region);
    }
    bvh_.update(cmd);  // (the TLAS after this frame's rows, terrain patches and live meshes)
    // Probe GI: this frame's probe updates through the BVH (first, like skinning: its passes push
    // their own compute uniforms).
    probes_.setCamera(camera.position);
    probes_.update(cmd, bvh_, *lights_, settings.gi);
    // Skinning first: it pushes its own compute uniforms, and every pass below reads its output.
    // (V1: with the vertex functions' pass, on this frame's materials and time.)
    skinning_->setDeformInputs(vt_->materialData(), vt_->paramsOffset(), settings.seconds);
    skinning_->record(cmd);
    lastCamera_ = camera.position;
    // Characters whose pose changed: re-render the cached shadow pages under them.
    skinnedBounds_.clear();
    skinning_->consumePosed([&](uint32_t record, float4 last, float4 now) {
        const fpak::InstanceGpu& inst = mirror_[skinning_->instanceOfRecord(record)];
        auto toWorld = [&](float4 s) {
            float scale = 0.0f;
            for (int c = 0; c < 3; ++c)
                scale = std::max(scale, length(float3{inst.world[0][c], inst.world[1][c], inst.world[2][c]}));
            float3 p;
            for (int row = 0; row < 3; ++row)
                (&p.x)[row] = inst.world[row][0] * s.x + inst.world[row][1] * s.y + inst.world[row][2] * s.z + inst.world[row][3];
            return float4{p.x, p.y, p.z, s.w * scale};
        };
        const float4 a = toWorld(last), b = toWorld(now);
        const float d = length(float3{b.x - a.x, b.y - a.y, b.z - a.z});
        if (d + b.w <= a.w)
            skinnedBounds_.push_back(a);  // the new sphere is inside the old one
        else if (d + a.w <= b.w)
            skinnedBounds_.push_back(b);
        else {
            const float r = (d + a.w + b.w) * 0.5f;
            const float t = d > 0.0f ? (r - a.w) / d : 0.0f;
            skinnedBounds_.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, r});
        }
    });
    if (!skinnedBounds_.empty()) {
        FX_GPU_PASS(cmd, "skinned shadow invalidations");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        vsm_->uploadInvalidations(copy, skinnedBounds_, true);
        SDL_EndGPUCopyPass(copy);
    }
    vt_->update(cmd, frame_);
    lights_->update(cmd, camera, width, height);
    // Water: this frame's patches and bodies (nothing visible: none of its passes run).
    water_.setRipples(settings.water.ripples, settings.water.rippleExtent, settings.water.rippleResolution);
    const bool waterOn = lit && settings.water.enabled && settings.debugView != DebugView::Hiz && water_.prepare(cmd, camera, width, height);
    // Decals: the visible ones and their screen tiles, for the resolve.
    decals_.prepare(cmd, camera, width, height);
    layers_.upload(cmd);
    // Transparent (alpha-blend) materials: whether the scene has any (else their passes don't run),
    // and the pages of those in view.
    bool refraction = false;  // (T2: the reflection source is then made, what refraction sees)
    {
        bool blend = false;
        for (const fpak::MaterialGpu& m : pak_->materials()) {
            blend |= (m.flags & fpak::kMaterialAlphaBlend) != 0;
            refraction |= (m.flags & (fpak::kMaterialAlphaBlend | fpak::kMaterialRefraction)) ==
                          (fpak::kMaterialAlphaBlend | fpak::kMaterialRefraction);
        }
        transparent_.setActive(blend);
        if (blend)
            transparent_.requestPages(*pak_, mirror_, *vt_, camera, height, frame_);
    }
    // Particles: this frame's sprites sorted and uploaded, their texture pages asked for.
    const bool particlesOn = lit && settings.debugView != DebugView::Hiz && particles_.prepare(cmd, camera, width, height, frame_);

    // TAA / upscaling: Halton(2,3) subpixel jitter of the visibility raster (Lit view only; the
    // debug views stay pixel-exact). Culling keeps the unjittered camera. The cycle is 8 frames,
    // and AMD's 8 x (display / render)^2 while upscaling, so every display pixel gets samples.
    // jitterPixels: the offset in render pixels (y down, as FSR takes it); jx, jy: in NDC.
    const bool taa = lit && (settings.post.taa || upscale);
    const float ratio = float(displayWidth) / float(width);
    jitterPhases_ = upscale ? std::max(8u, uint32_t(std::ceil(8.0f * ratio * ratio))) : 8u;
    jitterPhase_ = uint32_t(frame_ % jitterPhases_);
    float jx = 0.0f, jy = 0.0f;
    float2 jitterPixels = {0.0f, 0.0f};
    if (taa) {
        jitterPixels = {halton(jitterPhase_ + 1, 2) - 0.5f, -(halton(jitterPhase_ + 1, 3) - 0.5f)};
        jx = 2.0f * jitterPixels.x / float(width);
        jy = -2.0f * jitterPixels.y / float(height);
    }
    // Texture LOD while upscaling: the display's (AMD: log2(render / display); FSR adds -1).
    // TAAU and DLSS take log2(render / display).
    const float upscaleBias = upscale ? std::log2(float(width) / float(displayWidth)) -
                                            (settings.post.upscaler == Upscaler::Fsr && fsr_.available() ? 1.0f : 0.0f)
                                      : 0.0f;
    const float mipBias = vt_->mipBias() + upscaleBias;
    float4x4 rasterViewProj = camera.viewProj;  // clip.xy += jitter * clip.w
    for (int c = 0; c < 4; ++c) {
        rasterViewProj.m[c][0] += jx * rasterViewProj.m[c][3];
        rasterViewProj.m[c][1] += jy * rasterViewProj.m[c][3];
    }
    ViewReconstructGpu view = {{camera.p00, camera.p11, camera.zNear, 0.0f},
                               {jx, jy, 0.0f, 0.0f},
                               {width, height, uint32_t(frame_), 0}};
    PostSettings post = settings.post;
    post.taa = taa;
    {
        // Daylight for auto exposure's limit: a grey ground in the sun and the sky (giSky over an
        // open hemisphere gives about a fifth of the sun's irradiance).
        const float3 sun = lights_->sunColor();
        const float sunLum = 0.2126f * sun.x + 0.7152f * sun.y + 0.0722f * sun.z;
        const float height = std::max(lights_->sunDirection().y, 0.25f);
        post.daylight = 0.18f * sunLum * (height + 0.2f) / 3.14159265f;
    }
    post.passthrough = !lit;

    if (settings.freezeCulling && !wasFrozen_)
        frozen_ = camera;
    wasFrozen_ = settings.freezeCulling;
    const CameraState& cullCam = settings.freezeCulling ? frozen_ : camera;
    const bool occlusion = settings.occlusion && !settings.freezeCulling;

    CullUniforms cu = {};
    cu.view = cullCam.view;
    cu.prevView = hizValid_ ? prevView_ : cullCam.view;
    const float4x4& vp = cullCam.viewProj;
    cu.planes[0] = plane(vp, 1.0f, 0);   // left
    cu.planes[1] = plane(vp, -1.0f, 0);  // right
    cu.planes[2] = plane(vp, 1.0f, 1);   // bottom
    cu.planes[3] = plane(vp, -1.0f, 1);  // top
    cu.planes[4] = plane(vp, -1.0f, 2);  // near (reverse-Z: z <= w); the far plane is at infinity
    cu.cameraPos = f4(cullCam.position, settings.lodPixels / float(displayHeight));  // LOD for the display
    cu.proj = {cullCam.p00, cullCam.p11, cullCam.zNear, settings.cullPixels / float(displayHeight)};
    cu.scatter = {scatterPixels_ / float(displayHeight), settings.detailDistance, 0.0f, 0.0f};
    for (uint32_t i = 0; i < hizLevelCount_; ++i) {
        cu.hizLevels[i][0] = hizLevels_[i].offset;
        cu.hizLevels[i][1] = hizLevels_[i].width;
        cu.hizLevels[i][2] = hizLevels_[i].height;
    }
    // (The levels' spare words: the terrain's patch instances, never contribution-culled.)
    cu.hizLevels[0][3] = pak_->terrainSlots().firstInstance;
    cu.hizLevels[1][3] = pak_->terrainSlots().patches;
    cu.limits[0] = instanceCount_;
    cu.limits[1] = maxChunks_;
    cu.limits[2] = maxVisible_;
    cu.limits[3] = maxVisible_;
    const uint32_t coneFlag = (settings.coneCulling ? kFlagCone : 0) | (settings.alphaMask ? 0 : kFlagNoAlphaMask);
    cu.params[0] = coneFlag | (occlusion && hizValid_ ? kFlagOcclusion : 0) |
                   (occlusion && settings.forceRetest ? kFlagForceRetest : 0);
    cu.params[1] = hizLevelCount_;
    cu.params[2] = width;
    cu.params[3] = height;
    SDL_PushGPUComputeUniformData(cmd, 0, &cu, sizeof(cu));

    SDL_GPUBuffer* scene = pak_->sceneBuffer();
    SDL_GPUBuffer* materials = vt_->materialData();  // materials + VT page table

    SDL_GPUBuffer* skin = skinning_->buffer();

    auto computePass = [&](const char* name, SDL_GPUComputePipeline* pipeline, std::initializer_list<SDL_GPUBuffer*> rwBuffers,
                           std::initializer_list<SDL_GPUBuffer*> roBuffers) {
        SDL_GPUStorageBufferReadWriteBinding rw[4] = {};
        uint32_t n = 0;
        for (SDL_GPUBuffer* b : rwBuffers)
            rw[n++] = {b, false};
        gpuprof::pass(cmd, name);
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, n);
        SDL_BindGPUComputePipeline(pass, pipeline);
        if (roBuffers.size())
            SDL_BindGPUComputeStorageBuffers(pass, 0, roBuffers.begin(), uint32_t(roBuffers.size()));
        return pass;
    };

    // 1. Reset counters and args.
    FX_GPU_SECTION(cmd, "Culling");
    {
        SDL_GPUComputePass* pass = computePass("cull reset", reset_, {counters_, args_}, {});
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    // 2. Instance culling -> 64-cluster chunks. Slot 0 is unused by this kernel but must hold a
    //    buffer; never bind one buffer to two slots of a pass (SDL transitions it per binding).
    {
        // Slots 0, 2 and 3 are unused by this kernel but must hold distinct buffers.
        SDL_GPUComputePass* pass = computePass("instance cull", instances_, {counters_, chunks_}, {materials, scene, hiz_, candidates_, skin});
        dispatchGroups(pass, instanceCount_, 64);
        SDL_EndGPUComputePass(pass);
    }
    {
        SDL_GPUComputePass* pass = computePass("cull args 1", argsCull1_, {counters_, args_}, {});
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    // 3. Cluster culling, pass 1: DAG cut, frustum, cone, HiZ of the previous frame.
    {
        SDL_GPUComputePass* pass = computePass("cluster cull 1", cullFirst_, {counters_, visible_, candidates_, transparent_.candidates()},
                                               {chunks_, scene, materials, hiz_, skin});
        SDL_DispatchGPUComputeIndirect(pass, args_, kArgCull1);
        SDL_EndGPUComputePass(pass);
    }
    {
        SDL_GPUComputePass* pass = computePass("draw args 1", argsDraw1_, {counters_, args_}, {});
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }

    auto expandAndDraw = [&](uint32_t passIndex, bool clear) {
        const ExpandUniforms eu = {passIndex, maxVisible_, settings.coneCulling ? 1u : 0u, 0, f4(camera.position, 1.0f)};
        SDL_PushGPUComputeUniformData(cmd, 0, &eu, sizeof(eu));
        SDL_GPUComputePass* cp = computePass(passIndex == 0 ? "expand 1" : "expand 2", expand_, {counters_, indices_, triangleBase_},
                                             {visible_, scene, materials, skin});
        SDL_DispatchGPUComputeIndirect(cp, args_, passIndex == 0 ? kArgExpand1 : kArgExpand2);
        SDL_EndGPUComputePass(cp);
        cp = computePass(passIndex == 0 ? "raster args 1" : "raster args 2", passIndex == 0 ? argsRaster1_ : argsRaster2_,
                         {counters_, args_}, {});
        SDL_DispatchGPUCompute(cp, 1, 1, 1);
        SDL_EndGPUComputePass(cp);

        SDL_GPUColorTargetInfo vis = {};
        vis.texture = visTexture_;
        vis.clear_color = {0, 0, 0, 0};  // 0 = empty
        vis.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        vis.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = depth_;
        depth.clear_depth = 0.0f;  // reverse-Z far
        depth.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_PushGPUVertexUniformData(cmd, 0, &rasterViewProj, sizeof(rasterViewProj));
        gpuprof::pass(cmd, passIndex == 0 ? "visibility raster 1" : "visibility raster 2");
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &vis, 1, &depth);
        SDL_BindGPUGraphicsPipeline(rp, visbuffer_);
        SDL_GPUBuffer* vsBuffers[4] = {visible_, scene, skin, triangleBase_};
        SDL_BindGPUVertexStorageBuffers(rp, 0, vsBuffers, 4);
        const SDL_GPUBufferBinding ib = {indices_, 0};
        SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, passIndex == 0 ? kArgDraw1 : kArgDraw2, 1);

        // Alpha-masked clusters: same buffers, plus the albedo pages for the alpha test.
        const uint32_t mu[4] = {vt_->pageTableOffset(), 0, 0, 0};
        float bias = mipBias;
        SDL_memcpy((void*)&mu[1], &bias, 4);
        SDL_PushGPUFragmentUniformData(cmd, 0, mu, sizeof(mu));
        SDL_BindGPUGraphicsPipeline(rp, visbufferMasked_);
        SDL_BindGPUVertexStorageBuffers(rp, 0, vsBuffers, 4);
        const SDL_GPUTextureSamplerBinding albedo = {vt_->physical(0), vt_->sampler()};
        SDL_BindGPUFragmentSamplers(rp, 0, &albedo, 1);
        SDL_GPUBuffer* materialData = vt_->materialData();
        SDL_BindGPUFragmentStorageBuffers(rp, 0, &materialData, 1);
        SDL_DrawGPUIndexedPrimitivesIndirect(rp, args_, passIndex == 0 ? kArgDraw1Masked : kArgDraw2Masked, 1);
        SDL_EndGPURenderPass(rp);
    };

    // 4. Draw pass 1 into the visibility buffer.
    FX_GPU_SECTION(cmd, "Visibility 1");
    expandAndDraw(0, true);

    // 5. Occlusion re-test: HiZ of pass-1 depth, then candidates against it, then draw pass 2.
    if (occlusion) {
        FX_GPU_SECTION(cmd, "HiZ (pass 1 depth)");
        buildHiz(cmd);
        FX_GPU_SECTION(cmd, "Occlusion re-test");
        cu.params[0] = coneFlag | kFlagOcclusion;
        SDL_PushGPUComputeUniformData(cmd, 0, &cu, sizeof(cu));
        {
            SDL_GPUComputePass* pass = computePass("cluster cull 2", cullSecond_, {counters_, visible_},
                                                   {candidates_, scene, materials, hiz_, skin});
            SDL_DispatchGPUComputeIndirect(pass, args_, kArgCull2);
            SDL_EndGPUComputePass(pass);
        }
        {
            SDL_GPUComputePass* pass = computePass("draw args 2", argsDraw2_, {counters_, args_}, {});
            SDL_DispatchGPUCompute(pass, 1, 1, 1);
            SDL_EndGPUComputePass(pass);
        }
        FX_GPU_SECTION(cmd, "Visibility 2");
        expandAndDraw(1, false);
        // 6. HiZ of the final depth, consumed by next frame's pass 1.
        FX_GPU_SECTION(cmd, "HiZ (final)");
        buildHiz(cmd);
        hizValid_ = true;
        prevView_ = cullCam.view;
    } else {
        hizValid_ = false;
    }

    // 6b. Transparent clusters: against the final HiZ (none without occlusion culling), sorted,
    //     expanded into their own index buffer (render/transparent_renderer.h).
    if (transparent_.active() && lit) {
        FX_GPU_SECTION(cmd, "Transparent cull");
        cu.params[0] = coneFlag | (occlusion ? kFlagOcclusion : 0);
        TransparentRenderer::CullInputs ti;
        ti.cullUniforms = &cu;
        ti.cullUniformsSize = sizeof(cu);
        ti.counters = counters_;
        ti.args = args_;
        ti.scene = scene;
        ti.materials = materials;
        ti.hiz = hiz_;
        ti.skin = skin;
        transparent_.prepare(cmd, ti);
    }

    // 6a. Water: the scene's depth with the water's surface in it, and the surface's normals.
    if (waterOn)
        water_.raster(cmd, depth_, pointSampler_, rasterViewProj, width, height);
    SDL_GPUTexture* surfaceDepth = waterOn ? water_.depth() : depth_;  // what the camera sees first

    // 6b. The terrain's materials and per-pixel normals, for the resolve.
    if (pak_->terrainSlots().patches)
        terrainSurface_.shade(cmd, depth_, visTexture_, visible_, pointSampler_, width, height, camera, {jx, jy},
                              pak_->terrainSlots().firstInstance, pak_->terrainSlots().patches, pak_->sceneBuffer());

    // 7. Sun shadows: virtual shadow map pages for this frame's final depth.
    vsm_->render(cmd, camera, depth_, pointSampler_, width, height, displayHeight, frame_, settings.shadowCache,
                 waterOn ? water_.depth() : nullptr);

    // 7b. Ambient terms from the final depth, at half resolution by default (the resolve
    // upsamples them): screen-space AO.
    uint32_t ambientOffset[2];
    ambientSampleOffset(frame_, post.taa, ambientOffset);
    if (lit)
        post_.renderAo(cmd, depth_, view, post, ambientOffset);
    // Probe GI: sampled per pixel by the resolve, with normal maps.
    const bool probesOn = settings.gi.enabled && probes_.ready();

    // 8. Resolve (or HiZ debug view) into scene color, writing virtual texture feedback.
    const bool reflections = settings.reflections && settings.debugView != DebugView::Hiz;
    FX_GPU_SECTION(cmd, "Resolve");
    vt_->recordFeedbackClear(cmd, vtClear_);
    FX_GPU_PASS(cmd, "resolve");
    SDL_GPUStorageTextureReadWriteBinding out[4] = {{color_, 0, 0, false}, {post_.velocity(), 0, 0, false},
                                                    {reflectWeight_, 0, 0, false}, {reflectNormal_, 0, 0, false}};
    SDL_GPUStorageBufferReadWriteBinding feedback[2] = {{vt_->feedbackList(), false}, {vt_->feedbackMask(), false}};
    const bool hizView = settings.debugView == DebugView::Hiz && occlusion;
    SDL_GPUComputePass* pass =
        SDL_BeginGPUComputePass(cmd, out, hizView ? 1 : 4, hizView ? nullptr : feedback, hizView ? 0 : 2);
    if (hizView) {
        const uint32_t level = std::min(settings.hizViewLevel, hizLevelCount_ - 1);
        const HizViewUniforms hu = {{hizLevels_[level].offset, hizLevels_[level].width, hizLevels_[level].height, level},
                                    {width, height, 0, 0}};
        SDL_PushGPUComputeUniformData(cmd, 0, &hu, sizeof(hu));
        SDL_BindGPUComputePipeline(pass, viewHiz_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &hiz_, 1);
    } else {
        ResolveUniforms ru = {};
        ru.prevViewProj = havePrevViewProj_ ? prevViewProj_ : camera.viewProj;
        ru.cameraPos = f4(camera.position, 1.0f);
        ru.cameraRight = f4(camera.right, 1.0f / camera.p00);
        ru.cameraUp = f4(camera.up, 1.0f / camera.p11);
        ru.cameraForward = f4(camera.forward, 0.0f);
        ru.sunDirection = f4(lights_->sunDirection(), 0.0f);
        ru.sunColor = f4(lights_->sunColor(), 0.0f);
        ru.gridOrderOffset = lights_->orderOffset();
        ru.gridTileOffset = lights_->tileOffset();
        ru.gridTilesX = lights_->tilesX();
        ru.gridWords = lights_->words();
        ru.gridBinScale = lights_->binScale();
        ru.lightCount = lights_->count();
        ru.vsm = vsm_->params();
        ru.params[0] = width;
        ru.params[1] = height;
        ru.params[2] = settings.debugView == DebugView::Hiz         ? 0u
                       : settings.debugView == DebugView::RayTraced || settings.debugView == DebugView::Probes
                           ? uint32_t(DebugView::Triangles)  // (the sky and motion; the rays overwrite the rest)
                           : uint32_t(settings.debugView);
        ru.params[3] = uint32_t(frame_);
        ru.vtParams[0] = vt_->pageTableOffset();
        ru.vtParams[1] = VirtualTextureSystem::kFeedbackCapacity;
        ru.vtParams[2] = vt_->pageCount();
        ru.vtParams[3] = vt_->paramsOffset();
        ru.vtParamsF[0] = mipBias;
        ru.vtParamsF[1] = settings.seconds;
        ru.jitter = {jx, jy, 0.0f, 0.0f};
        ru.probes = probes_.volume();
        ru.ambient[0] = post_.aoScale();
        ru.ambient[1] = probesOn ? 1u : 0u;
        ru.ambient[2] = ambientOffset[0];
        ru.ambient[3] = ambientOffset[1];
        ru.terrain[0] = terrainSurface_.active() ? 1u : 0u;
        ru.reflect = {reflections ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        ru.decals[0] = decals_.count();
        ru.decals[1] = decals_.words();
        ru.decals[2] = decals_.tilesX();
        ru.decals[3] = DecalRenderer::kTile;
        ru.terrain[1] = live_.extraAddress();
        ru.terrain[2] = layers_.used() ? 1u : 0u;
        SDL_PushGPUComputeUniformData(cmd, 0, &ru, sizeof(ru));
        SDL_BindGPUComputePipeline(pass, resolve_);
        // Without probe GI (no BVH, or the GPU can't) the AO fills its slots (not read then).
        SDL_GPUTextureSamplerBinding samplers[9] = {{vt_->physical(0), vt_->sampler()},
                                                    {vt_->physical(1), vt_->sampler()},
                                                    {vt_->physical(2), vt_->sampler()},
                                                    {vsm_->atlas(), vsm_->compareSampler()},
                                                    {post_.ao(), post_.pointSampler()},
                                                    probeBinding(0),
                                                    probeBinding(1),
                                                    probeBinding(2),
                                                    {layers_.masks(), layers_.sampler()}};
        SDL_BindGPUComputeSamplers(pass, 0, samplers, 9);
        SDL_GPUTexture* storage[5] = {visTexture_, terrainSurface_.output(), decals_.data(), decals_.bins(), layers_.records()};
        SDL_BindGPUComputeStorageTextures(pass, 0, storage, 5);
        SDL_GPUBuffer* ro[8] = {visible_, scene, materials, lights_->lights(), lights_->grid(), vsm_->state(), prevInstances_,
                                skinning_->buffer()};
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 8);
    }
    SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);

    // 8'. Debug view Ray Traced: camera rays through the BVH over what the resolve drew.
    if (settings.debugView == DebugView::RayTraced && bvh_.ready()) {
        struct RayViewUniforms {
            float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
            uint32_t params[4];
        } rv = {};
        rv.cameraPos = f4(camera.position, 1.0f);
        rv.cameraRight = f4(camera.right, 1.0f / camera.p00);
        rv.cameraUp = f4(camera.up, 1.0f / camera.p11);
        rv.cameraForward = f4(camera.forward, 0.0f);
        rv.jitter = {jx, jy, 0.0f, 0.0f};
        rv.params[0] = width;
        rv.params[1] = height;
        rv.params[2] = settings.alphaMask ? 1u : 0u;
        rv.params[3] = vt_->pageTableOffset();
        FX_GPU_PASS(cmd, "ray traced view");
        SDL_GPUStorageTextureReadWriteBinding target = {color_, 0, 0, false};
        SDL_GPUComputePass* rp = SDL_BeginGPUComputePass(cmd, &target, 1, nullptr, 0);
        SDL_PushGPUComputeUniformData(cmd, 0, &rv, sizeof(rv));
        SDL_BindGPUComputePipeline(rp, rayView_);
        const SDL_GPUTextureSamplerBinding albedo = {vt_->physical(0), vt_->sampler()};
        SDL_BindGPUComputeSamplers(rp, 0, &albedo, 1);
        SDL_GPUBuffer* const rb[3] = {bvh_.buffer(), scene, materials};
        SDL_BindGPUComputeStorageBuffers(rp, 0, rb, 3);
        SDL_DispatchGPUCompute(rp, (width + 7) / 8, (height + 7) / 8, 1);
        SDL_EndGPUComputePass(rp);
    }
    // 8''. Debug view Probes: the probe GI's finest grid as spheres, over the probes' light.
    if (settings.debugView == DebugView::Probes && bvh_.ready() && probes_.ready()) {
        struct ProbeViewUniforms {
            float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
            uint32_t params[4];
            ProbeVolumeGpu probes;
        } pv = {};
        pv.cameraPos = f4(camera.position, 1.0f);
        pv.cameraRight = f4(camera.right, 1.0f / camera.p00);
        pv.cameraUp = f4(camera.up, 1.0f / camera.p11);
        pv.cameraForward = f4(camera.forward, 0.0f);
        pv.jitter = {jx, jy, 0.0f, 0.0f};
        pv.params[0] = width;
        pv.params[1] = height;
        pv.params[2] = settings.alphaMask ? 1u : 0u;
        pv.params[3] = vt_->pageTableOffset();
        pv.probes = probes_.volume();
        FX_GPU_PASS(cmd, "probe view");
        SDL_GPUStorageTextureReadWriteBinding target = {color_, 0, 0, false};
        SDL_GPUComputePass* pp = SDL_BeginGPUComputePass(cmd, &target, 1, nullptr, 0);
        SDL_PushGPUComputeUniformData(cmd, 0, &pv, sizeof(pv));
        SDL_BindGPUComputePipeline(pp, probeView_);
        const SDL_GPUTextureSamplerBinding samplers[4] = {{vt_->physical(0), vt_->sampler()}, probeBinding(0), probeBinding(1),
                                                          probeBinding(2)};
        SDL_BindGPUComputeSamplers(pp, 0, samplers, 4);
        SDL_GPUBuffer* const pb[3] = {bvh_.buffer(), scene, materials};
        SDL_BindGPUComputeStorageBuffers(pp, 0, pb, 3);
        SDL_DispatchGPUCompute(pp, (width + 7) / 8, (height + 7) / 8, 1);
        SDL_EndGPUComputePass(pp);
    }

    // 8a. Reflections: this frame's image copied with mips for the screen-space trace
    //     (reflection_source.slang: a level per dispatch, NaN and overflow cleaned), then each
    //     pixel's reflection added to it (reflections.slang).
    const bool refractionOn = refraction && transparent_.active() && lit;
    if ((reflections || waterOn || refractionOn) && !hizView) {
        FX_GPU_PASS(cmd, "reflection source");
        {
            // All its levels in one dispatch, a 64x64 tile per group (levels past the texture's
            // own count, at tiny sizes, go to its last one: written, never read).
            SDL_GPUStorageTextureReadWriteBinding out[7];
            for (uint32_t l = 0; l < 7; ++l)
                out[l] = {reflectSource_, std::min(l, reflectLevels_ - 1), 0, false};
            const uint32_t su[4] = {width, height, 0, 0};
            SDL_GPUComputePass* sp = SDL_BeginGPUComputePass(cmd, out, 7, nullptr, 0);
            SDL_PushGPUComputeUniformData(cmd, 0, su, sizeof(su));
            SDL_BindGPUComputePipeline(sp, reflectSourcePipe_);
            const SDL_GPUTextureSamplerBinding image = {color_, mipSampler_};
            SDL_BindGPUComputeSamplers(sp, 0, &image, 1);
            SDL_DispatchGPUCompute(sp, (width + 63) / 64, (height + 63) / 64, 1);
            SDL_EndGPUComputePass(sp);
        }
    }
    // 8b. Water over the resolved frame (that frame's mips above: what is seen through it).
    if (waterOn && !hizView) {
        WaterRenderer::CompositeInputs wi;
        wi.sceneDepth = depth_;
        wi.refractSource = reflectSource_;
        wi.refractMips = reflectLevels_;
        wi.pointSampler = pointSampler_;
        wi.mipSampler = mipSampler_;
        wi.color = color_;
        wi.velocity = post_.velocity();
        wi.reflectWeight = reflectWeight_;
        wi.reflectNormal = reflectNormal_;
        wi.vsmAtlas = vsm_->atlas();
        wi.vsmSampler = vsm_->compareSampler();
        wi.vsmState = vsm_->state();
        wi.vsm = &vsm_->params();
        for (int k = 0; k < 3; ++k)
            wi.probes[k] = probeBinding(k);
        wi.probeVolume = probes_.volume();
        wi.giOn = probesOn;
        wi.bounce = settings.gi.intensity;
        wi.sky = settings.gi.skyIntensity;
        wi.bvh = probesOn && bvh_.ready() ? bvh_.buffer() : nullptr;
        wi.scene = scene;
        wi.materials = materials;
        wi.vtAlbedo = {vt_->physical(0), vt_->sampler()};
        wi.pageTable = vt_->pageTableOffset();
        wi.alphaMask = settings.alphaMask;
        wi.reflectionsFollow = reflections;
        wi.sunDirection = lights_->sunDirection();
        wi.sunColor = lights_->sunColor();
        wi.prevViewProj = havePrevViewProj_ ? prevViewProj_ : camera.viewProj;
        wi.jitter = {jx, jy};
        wi.rasterViewProj = rasterViewProj;
        if (terrainSurface_.coarseLayout(wi.terrainGrid, wi.terrainRange)) {
            wi.terrainHeights = terrainSurface_.coarseHeights();
            wi.terrainMaterials = terrainSurface_.coarseMaterials();
            wi.terrainData = terrainSurface_.data();
        }
        water_.composite(cmd, camera, width, height, wi);
    }
    if (reflections && !hizView) {
        ReflectUniforms rf = {};
        rf.cameraPos = f4(camera.position, 1.0f);
        rf.cameraRight = f4(camera.right, 1.0f / camera.p00);
        rf.cameraUp = f4(camera.up, 1.0f / camera.p11);
        rf.cameraForward = f4(camera.forward, 0.0f);
        rf.jitter = {jx, jy, 0.0f, 0.0f};
        rf.params[0] = width;
        rf.params[1] = height;
        rf.params[2] = uint32_t(settings.debugView);
        rf.params[3] = uint32_t(frame_);
        rf.probes = probes_.volume();
        const float3 sunDir = lights_->sunDirection(), sunColor = lights_->sunColor();
        rf.sun = f4(sunDir, settings.gi.intensity);
        rf.sunColor = f4(sunColor, settings.gi.skyIntensity);
        const ReflectionSettings& rs = settings.reflect;
        const uint32_t traceScale = rs.halfRes ? 2u : 1u;
        rf.reflect = {probesOn ? 1.0f : 0.0f, rs.screenSpace ? std::clamp(rs.maxRoughness, 0.0f, 1.0f) : 0.0f, camera.zNear, 0.0f};
        const bool traced = probesOn && bvh_.ready();
        rf.trace[0] = vt_->pageTableOffset();
        rf.trace[1] = traced ? 1u : 0u;
        rf.trace[2] = settings.alphaMask ? 1u : 0u;
        rf.ssr = {std::clamp(rs.thickness, 0.1f, 8.0f), std::max(rs.intensity, 0.0f), float(std::clamp(rs.steps, 8u, 128u)),
                  float(traceScale)};
        const SDL_GPUTextureSamplerBinding samplers[9] = {{surfaceDepth, pointSampler_},
                                                          {reflectSource_, mipSampler_},
                                                          {reflectWeight_, pointSampler_},
                                                          {reflectNormal_, pointSampler_},
                                                          probeBinding(0),
                                                          probeBinding(1),
                                                          probeBinding(2),
                                                          {reflectHits_, pointSampler_},
                                                          {reflectFar_, pointSampler_}};
        const SDL_GPUTextureSamplerBinding farSamplers[8] = {{surfaceDepth, pointSampler_},
                                                             {reflectWeight_, pointSampler_},
                                                             {reflectNormal_, pointSampler_},
                                                             {reflectHits_, pointSampler_},
                                                             {vt_->physical(0), vt_->sampler()},
                                                             probeBinding(0),
                                                             probeBinding(1),
                                                             probeBinding(2)};
        // (Without the BVH the scene buffer stands in: not read.)
        SDL_GPUBuffer* const traceBuffers[3] = {traced ? bvh_.buffer() : scene, scene, materials};
        const uint32_t tracedW = (width + traceScale - 1) / traceScale, tracedH = (height + traceScale - 1) / traceScale;
        // The screen-space trace, at half resolution by default (into reflectHits_; the first two
        // bindings are the reflection pass's, unused here)...
        FX_GPU_PASS(cmd, "reflection trace");
        SDL_GPUStorageTextureReadWriteBinding traceRw[3] = {{color_, 0, 0, false}, {reflectMask_, 0, 0, false}, {reflectHits_, 0, 0, false}};
        SDL_GPUComputePass* tp = SDL_BeginGPUComputePass(cmd, traceRw, 3, nullptr, 0);
        SDL_PushGPUComputeUniformData(cmd, 0, &rf, sizeof(rf));
        SDL_BindGPUComputePipeline(tp, reflectTrace_);
        SDL_BindGPUComputeSamplers(tp, 0, samplers, 4);
        SDL_DispatchGPUCompute(tp, (tracedW + 7) / 8, (tracedH + 7) / 8, 1);
        SDL_EndGPUComputePass(tp);
        // ...the far reflections for the same pixels, through the BVH (into reflectFar_)...
        FX_GPU_PASS(cmd, "reflection far");
        SDL_GPUStorageTextureReadWriteBinding farRw = {reflectFar_, 0, 0, false};
        SDL_GPUComputePass* fp = SDL_BeginGPUComputePass(cmd, &farRw, 1, nullptr, 0);
        SDL_PushGPUComputeUniformData(cmd, 0, &rf, sizeof(rf));
        SDL_BindGPUComputePipeline(fp, reflectFarPipe_);
        SDL_BindGPUComputeSamplers(fp, 0, farSamplers, 8);
        SDL_BindGPUComputeStorageBuffers(fp, 0, traceBuffers, 3);
        SDL_DispatchGPUCompute(fp, (tracedW + 7) / 8, (tracedH + 7) / 8, 1);
        SDL_EndGPUComputePass(fp);
        // ...then each pixel's reflection, put together from both.
        FX_GPU_PASS(cmd, "reflections");
        SDL_GPUStorageTextureReadWriteBinding rw[2] = {{color_, 0, 0, false}, {reflectMask_, 0, 0, false}};
        SDL_GPUComputePass* rp = SDL_BeginGPUComputePass(cmd, rw, 2, nullptr, 0);
        SDL_PushGPUComputeUniformData(cmd, 0, &rf, sizeof(rf));
        SDL_BindGPUComputePipeline(rp, reflect_);
        SDL_BindGPUComputeSamplers(rp, 0, samplers, 9);
        SDL_DispatchGPUCompute(rp, (width + 7) / 8, (height + 7) / 8, 1);
        SDL_EndGPUComputePass(rp);
    }
    reflectMaskValid_ = reflections && !hizView;

    // 8b. MSAA: the visibility draws again at N samples per pixel, for edge coverage.
    SDL_GPUTexture* hdr = color_;
    const uint32_t samples = lit && !upscale && !msaaBroken_ ? msaaSamples(settings.post.msaa) : 1u;
    if (samples > 1 && ensureMsaa(samples)) {
        renderMsaa(cmd, scene, rasterViewProj, width, height);
        hdr = colorAA_;
    }

    // 8c. Particles: sprites blended over the lit frame (render/particle_renderer.h), before TAA /
    //     FSR take it. Not in reflections or water's refraction (those read the frame before).
    // 8b. Transparent materials: blended over the lit frame, farthest first, tested against the
    //     opaque depth (not water's: they draw over it). The first writer of the reactive mask.
    bool reactiveWritten = false;
    if (transparent_.active() && lit) {
        FX_GPU_SECTION(cmd, "Transparent");
        TransparentRenderer::DrawInputs ti;
        ti.target = hdr;
        ti.depth = depth_;
        ti.reactive = reactive_;
        ti.clearReactive = true;
        ti.args = args_;
        ti.scene = scene;
        ti.skin = skin;
        ti.rasterViewProj = rasterViewProj;
        ti.lights = lights_;
        ti.vsm = vsm_;
        ti.vt = vt_;
        for (int k = 0; k < 3; ++k)
            ti.probes[k] = probeBinding(k);
        ti.probeVolume = probes_.volume();
        ti.giOn = probesOn;
        ti.mipBias = mipBias;
        ti.seconds = settings.seconds;
        ti.sceneColor = reflectSource_;
        ti.mipSampler = mipSampler_;
        ti.sceneMips = reflectLevels_;
        ti.hiz = hiz_;
        ti.hizValid = occlusion;
        ti.width = width;
        ti.height = height;
        transparent_.draw(cmd, camera, ti);
        reactiveWritten = true;
    }
    // 8b'. Runtime meshes (R5): lit like the scene, tested against its depth, before particles.
    if (meshes_) {
        MeshRenderer::Inputs mi;
        mi.target = lit ? hdr : nullptr;
        mi.depth = surfaceDepth;
        mi.rasterViewProj = rasterViewProj;
        mi.lights = lights_;
        mi.vsm = vsm_;
        meshes_->draw(cmd, camera, width, height, mi);  // (no target: this frame's draws dropped)
    }
    if (particlesOn) {
        ParticleRenderer::DrawInputs pi;
        pi.target = hdr;
        pi.depth = surfaceDepth;
        pi.rasterViewProj = rasterViewProj;
        pi.lights = lights_;
        pi.vsm = vsm_;
        pi.mipBias = mipBias;
        pi.reactive = reactive_;
        pi.clearReactive = !reactiveWritten;
        particles_.draw(cmd, camera, width, height, frame_, pi);
        reactiveWritten = true;
    }

    // 8c. AMD FSR (D3D12, with the AMD DLLs): from the render-sized frame to the display size,
    //     with the exposure measured on the render-sized frame first (an FSR input). Unavailable
    //     or failed: the TAA pass upscales instead (TAAU).
    SDL_GPUTexture* upscaled = nullptr;
    const bool fsrWanted = upscale && settings.post.upscaler == Upscaler::Fsr;
    if (fsrWanted && fsr_.available()) {
        FX_GPU_SECTION(cmd, "Upscale (FSR)");
        post_.measureExposure(cmd, color_, width, height, post);
        FsrUpscaler::Inputs in = {};
        in.color = color_;
        in.depth = surfaceDepth;
        in.velocity = post_.velocity();
        in.composition = reflectMaskValid_ ? reflectMask_ : nullptr;
        in.reactive = reactiveWritten ? reactive_ : nullptr;
        in.exposureState = post_.exposureState();
        in.manualExposure = post.manualExposure;
        in.renderWidth = width;
        in.renderHeight = height;
        in.displayWidth = displayWidth;
        in.displayHeight = displayHeight;
        in.jitterPixels = jitterPixels;
        in.frameTimeMs = settings.frameTimeMs;
        in.zNear = camera.zNear;
        in.fovY = 2.0f * std::atan(1.0f / camera.p11);
        in.sharpness = post.sharpness;
        in.reset = !havePrevViewProj_ || !fsrLastFrame_ || width != fsrRenderWidth_ || height != fsrRenderHeight_;
        upscaled = fsr_.dispatch(cmd, in);
    }
    // 8d. NVIDIA DLSS (D3D12, with the DLSS plugin and an RTX GPU): the same, through the
    //     plugin; DLAA at a render scale of 1. Unavailable or failed: TAAU.
    const bool dlssWanted = upscale && settings.post.upscaler == Upscaler::Dlss;
    if (dlssWanted && dlss_.prepare()) {
        FX_GPU_SECTION(cmd, "Upscale (DLSS)");
        post_.measureExposure(cmd, color_, width, height, post);
        DlssUpscaler::Inputs in = {};
        in.color = color_;
        in.depth = surfaceDepth;
        in.velocity = post_.velocity();
        in.reactive = reactiveWritten ? reactive_ : nullptr;
        in.exposureState = post_.exposureState();
        in.manualExposure = post.manualExposure;
        in.renderWidth = width;
        in.renderHeight = height;
        in.displayWidth = displayWidth;
        in.displayHeight = displayHeight;
        in.jitterPixels = jitterPixels;
        in.frameTimeMs = settings.frameTimeMs;
        in.reset = !havePrevViewProj_ || !fsrLastFrame_ || width != fsrRenderWidth_ || height != fsrRenderHeight_;
        upscaled = dlss_.dispatch(cmd, in);
    }
    if (dlssWanted) {
        char status[256];
        if (upscaled)
            SDL_snprintf(status, sizeof(status), "NVIDIA DLSS %s, %ux%u to %ux%u", dlss_.modeName().c_str(), width, height,
                         displayWidth, displayHeight);
        else
            SDL_snprintf(status, sizeof(status), "DLSS unavailable (%s): TAAU, %ux%u to %ux%u",
                         dlss_.reason().empty() ? "it failed; see the log" : dlss_.reason().c_str(), width, height,
                         displayWidth, displayHeight);
        upscalerStatus_ = status;
    }
    fsrLastFrame_ = upscaled != nullptr;
    fsrRenderWidth_ = width;
    fsrRenderHeight_ = height;
    if (fsrWanted) {
        char status[192];
        if (upscaled)
            SDL_snprintf(status, sizeof(status), "%s, %ux%u to %ux%u", fsr_.versionName().c_str(), width, height,
                         displayWidth, displayHeight);
        else
            SDL_snprintf(status, sizeof(status), "FSR unavailable (%s): TAAU, %ux%u to %ux%u",
                         fsr_.available() ? "it failed; see the log" : fsr_.reason().c_str(), width, height, displayWidth,
                         displayHeight);
        upscalerStatus_ = status;
    }

    // 9. Post: TAA / TAAU (or FSR's or DLSS's frame) -> auto exposure -> bloom -> AgX (-> FXAA) into the
    //    display target.
    if (!havePrevViewProj_)
        post_.invalidateHistory();
    const float4x4 viewToPrevClip = (havePrevViewProj_ ? prevViewProj_ : camera.viewProj) * viewToWorld(camera);
    post_.renderPost(cmd, hdr, surfaceDepth, view, viewToPrevClip, post, upscaled, reactiveWritten ? reactive_ : nullptr);
    prevViewProj_ = camera.viewProj;
    havePrevViewProj_ = true;

    // 10. Editor pick: one thread reads the visibility buffer (last: its uniforms replace the
    // post passes' push constants).
    uint32_t freeSlot = kReadbackSlots;
    for (uint32_t i = 0; i < kReadbackSlots && freeSlot == kReadbackSlots; ++i)
        if (!readbackFence_[i])
            freeSlot = i;
    const bool pickNow = pickRequest_ > pickDispatched_ && freeSlot < kReadbackSlots;
    if (pickNow) {
        const uint32_t pu[4] = {std::min(uint32_t(uint64_t(pickX_) * width / std::max(displayWidth, 1u)), width - 1),
                                std::min(uint32_t(uint64_t(pickY_) * height / std::max(displayHeight, 1u)), height - 1), width,
                                height};
        SDL_PushGPUComputeUniformData(cmd, 0, pu, sizeof(pu));
        FX_GPU_PASS(cmd, "editor pick");
        const SDL_GPUStorageBufferReadWriteBinding rwb = {pickBuffer_, false};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rwb, 1);
        SDL_BindGPUComputePipeline(pass, pick_);
        SDL_BindGPUComputeStorageTextures(pass, 0, &visTexture_, 1);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &visible_, 1);
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
        if (transparent_.active() && lit) {
            TransparentRenderer::PickInputs ti;
            ti.result = pickBuffer_;
            ti.counters = counters_;
            ti.scene = scene;
            ti.skin = skin;
            ti.depth = depth_;
            ti.pointSampler = pointSampler_;
            ti.x = pu[0];
            ti.y = pu[1];
            ti.width = width;
            ti.height = height;
            transparent_.pick(cmd, camera, ti);
        }
        pickDispatched_ = pickRequest_;
    }

    // 11. Async stats readback into a free slot (read a few frames later, never waited on).
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
        if (readbackFence_[i])
            continue;
        FX_GPU_PASS(cmd, "stats readback");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUBufferRegion src = {counters_, 0, kCounterCount * 4};
        const SDL_GPUTransferBufferLocation dst = {readback_[i], 0};
        SDL_DownloadFromGPUBuffer(copy, &src, &dst);
        const SDL_GPUBufferRegion fbSrc = {vt_->feedbackList(), 0, (1 + VirtualTextureSystem::kFeedbackCapacity) * 4};
        const SDL_GPUTransferBufferLocation fbDst = {feedbackReadback_[i], 0};
        SDL_DownloadFromGPUBuffer(copy, &fbSrc, &fbDst);
        const SDL_GPUBufferRegion vsmSrc = {vsm_->state(), vsm_->counterOffset() * 4, VirtualShadowMap::kCounterWords * 4};
        const SDL_GPUTransferBufferLocation vsmDst = {vsmReadback_[i], 0};
        SDL_DownloadFromGPUBuffer(copy, &vsmSrc, &vsmDst);
        if (pickNow && i == freeSlot) {
            const SDL_GPUBufferRegion pickSrc = {pickBuffer_, 0, 16};
            const SDL_GPUTransferBufferLocation pickDst = {pickReadback_[i], 0};
            SDL_DownloadFromGPUBuffer(copy, &pickSrc, &pickDst);
            pickSlotRequest_[i] = pickDispatched_;
        }
        scatter_.recordDownloads(copy, i);
        SDL_EndGPUCopyPass(copy);
        pendingSlot_ = int32_t(i);
        readbackFrame_[i] = frame_;
        break;
    }
    ++frame_;
}

}  // namespace fx
