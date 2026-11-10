#include "render/transparent_renderer.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/gpu_driven_renderer.h"
#include "render/light_system.h"
#include "render/particle_renderer.h"
#include "render/post_process.h"
#include "render/shader_file.h"
#include "render/virtual_shadow_map.h"
#include "render/virtual_texture.h"
#include "render/probe_gi.h"

#include "transparent.shader.h"
#include "transparent_cull.shader.h"
#include "transparent_pick.shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

// culling.slang
constexpr uint32_t kArgCullTransparent = 192, kArgExpandTransparent = 208, kArgDrawTransparent = 224;

struct TransparentUniforms {
    float4 cameraPos, cameraRight, cameraUp, cameraForward;
    uint32_t screen[4];  // width, height, scene colour's mips, HiZ valid
    float4 sunDirection, sunColor;
    uint32_t vtParams[4];  // page-table offset, params offset, GI on
    float vtParamsF[4];    // mip bias, seconds
    uint32_t gridOrderOffset, gridTileOffset, gridTilesX, gridWords;
    float gridBinScale;
    uint32_t lightCount, gridPad[2];
    VsmParamsGpu vsm;
    ProbeVolumeGpu probes;
};

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

SDL_GPUBuffer* makeBuffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage, uint32_t size, const char* name)
{
    SDL_GPUBufferCreateInfo info = {usage, size, 0};
    SDL_GPUBuffer* b = gpumem::createBuffer("Transparent", device, &info);
    if (b)
        gpumem::setBufferName(device, b, name);
    else
        FX_ERROR("transparent: buffer %s: %s", name, SDL_GetError());
    return b;
}

}  // namespace

SDL_GPUGraphicsPipeline* TransparentRenderer::makePipeline(const ShaderBlob& vsBlob, const ShaderBlob& fsBlob) const
{
    SDL_GPUShader* vs = gpu_->createShader(vsBlob);
    SDL_GPUShader* fs = gpu_->createShader(fsBlob);
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vs && fs) {
        SDL_GPUColorTargetDescription targets[2] = {};
        SDL_GPUColorTargetDescription& color = targets[0];  // premultiplied
        color.format = PostProcess::kHdrFormat;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUColorTargetDescription& reactive = targets[1];  // the most coverage
        reactive.format = ParticleRenderer::kReactiveFormat;
        reactive.blend_state.enable_blend = true;
        reactive.blend_state.src_color_blendfactor = reactive.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        reactive.blend_state.src_alpha_blendfactor = reactive.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        reactive.blend_state.color_blend_op = reactive.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // (back faces: dropped or ordered by the expand)
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
        info.target_info.color_target_descriptions = targets;
        info.target_info.num_color_targets = 2;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = GpuDrivenRenderer::kDepthFormat;
        pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    if (vs)
        SDL_ReleaseGPUShader(device_, vs);
    if (fs)
        SDL_ReleaseGPUShader(device_, fs);
    return pipeline;
}

void TransparentRenderer::setProjectShader(const ShaderFile* file)
{
    const ShaderFile::Entry* vs = file ? file->entry("vs_transparent") : nullptr;
    const ShaderFile::Entry* fs = file ? file->entry("fs_transparent") : nullptr;
    SDL_GPUGraphicsPipeline* made = vs && fs && gpu_ ? makePipeline(vs->blob, fs->blob) : nullptr;
    if (file && !made)
        FX_WARN("transparent: the project's build of the pass made no pipeline (the engine's is used)");
    if (project_)
        SDL_ReleaseGPUGraphicsPipeline(device_, project_);  // (released once the frames using it are done)
    project_ = made;
    pipeline_ = made ? made : builtin_;
}

bool TransparentRenderer::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    gpu_ = &gpu;
    cull_ = gpu.createComputePipeline(shaders::transparent_cull_cs_cull_transparent);
    sort_ = gpu.createComputePipeline(shaders::transparent_cull_cs_sort_transparent);
    expand_ = gpu.createComputePipeline(shaders::transparent_cull_cs_expand_transparent);
    pick_ = gpu.createComputePipeline(shaders::transparent_pick_cs_pick_transparent);
    builtin_ = pipeline_ = makePipeline(shaders::transparent_vs_transparent, shaders::transparent_fs_transparent);
    const SDL_GPUBufferUsageFlags rw = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    candidates_ = makeBuffer(device_, rw, kMaxCandidates * 8, "transparent candidates");
    list_ = makeBuffer(device_, rw, kMaxClusters * 16, "transparent list");
    draws_ = makeBuffer(device_, rw | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, kMaxClusters * 16, "transparent draws");
    indices_ = makeBuffer(device_, SDL_GPU_BUFFERUSAGE_INDEX | rw, kMaxClusters * 128 * 3 * 4, "transparent indices");
    if (!cull_ || !sort_ || !expand_ || !pick_ || !pipeline_ || !candidates_ || !list_ || !draws_ || !indices_) {
        FX_ERROR("transparent: pipelines failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void TransparentRenderer::shutdown()
{
    if (!device_)
        return;
    for (SDL_GPUComputePipeline* p : {cull_, sort_, expand_, pick_})
        if (p)
            SDL_ReleaseGPUComputePipeline(device_, p);
    for (SDL_GPUGraphicsPipeline* p : {builtin_, project_})
        if (p)
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
    builtin_ = project_ = nullptr;
    for (SDL_GPUBuffer* b : {candidates_, list_, draws_, indices_})
        gpumem::releaseBuffer(device_, b);
    cull_ = sort_ = expand_ = pick_ = nullptr;
    pipeline_ = nullptr;
    candidates_ = list_ = draws_ = indices_ = nullptr;
    device_ = nullptr;
}

void TransparentRenderer::prepare(SDL_GPUCommandBuffer* cmd, const CullInputs& in)
{
    auto pass = [&](const char* name, SDL_GPUComputePipeline* p, std::initializer_list<SDL_GPUBuffer*> rw,
                    std::initializer_list<SDL_GPUBuffer*> ro) {
        SDL_GPUStorageBufferReadWriteBinding b[3] = {};
        uint32_t n = 0;
        for (SDL_GPUBuffer* buf : rw)
            b[n++] = {buf, false};
        gpuprof::pass(cmd, name);
        SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, nullptr, 0, b, n);
        SDL_BindGPUComputePipeline(cp, p);
        SDL_BindGPUComputeStorageBuffers(cp, 0, ro.begin(), uint32_t(ro.size()));
        return cp;
    };
    SDL_PushGPUComputeUniformData(cmd, 0, in.cullUniforms, in.cullUniformsSize);
    SDL_GPUComputePass* cp = pass("transparent cull", cull_, {in.counters, list_}, {candidates_, in.scene, in.materials, in.hiz, in.skin});
    SDL_DispatchGPUComputeIndirect(cp, in.args, kArgCullTransparent);
    SDL_EndGPUComputePass(cp);
    cp = pass("transparent sort", sort_, {in.counters, draws_, in.args}, {list_, in.scene});
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);
    cp = pass("transparent expand", expand_, {in.counters, indices_}, {draws_, in.scene, in.materials, in.hiz, in.skin});
    SDL_DispatchGPUComputeIndirect(cp, in.args, kArgExpandTransparent);
    SDL_EndGPUComputePass(cp);
}

void TransparentRenderer::draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, const DrawInputs& in)
{
    if (!pipeline_ || !in.target || !in.depth || !in.reactive || !in.sceneColor || !in.hiz)
        return;
    FX_GPU_PASS(cmd, "transparent draw");
    SDL_GPUColorTargetInfo targets[2] = {};
    targets[0].texture = in.target;
    targets[0].load_op = SDL_GPU_LOADOP_LOAD;
    targets[0].store_op = SDL_GPU_STOREOP_STORE;
    targets[1].texture = in.reactive;
    targets[1].load_op = in.clearReactive ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    targets[1].store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = in.depth;
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;  // (read only: kept as it was)
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, targets, 2, &depth);
    SDL_BindGPUGraphicsPipeline(rp, pipeline_);
    SDL_PushGPUVertexUniformData(cmd, 0, &in.rasterViewProj, sizeof(in.rasterViewProj));
    SDL_GPUBuffer* vsBuffers[3] = {draws_, in.scene, in.skin};
    SDL_BindGPUVertexStorageBuffers(rp, 0, vsBuffers, 3);

    TransparentUniforms u = {};
    u.cameraPos = f4(camera.position, camera.zNear);
    u.cameraRight = f4(camera.right, 0.0f);
    u.cameraUp = f4(camera.up, 0.0f);
    u.cameraForward = f4(camera.forward, 0.0f);
    u.screen[0] = in.width;
    u.screen[1] = in.height;
    u.screen[2] = in.sceneMips;
    u.screen[3] = in.hizValid ? 1u : 0u;
    u.sunDirection = f4(in.lights->sunDirection(), 0.0f);
    u.sunColor = f4(in.lights->sunColor(), 0.0f);
    u.vtParams[0] = in.vt->pageTableOffset();
    u.vtParams[1] = in.vt->paramsOffset();
    u.vtParams[2] = in.giOn ? 1u : 0u;
    u.vtParamsF[0] = in.mipBias;
    u.vtParamsF[1] = in.seconds;
    u.gridOrderOffset = in.lights->orderOffset();
    u.gridTileOffset = in.lights->tileOffset();
    u.gridTilesX = in.lights->tilesX();
    u.gridWords = in.lights->words();
    u.gridBinScale = in.lights->binScale();
    u.lightCount = in.lights->count();
    u.vsm = in.vsm->params();
    u.probes = in.probeVolume;
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
    const SDL_GPUTextureSamplerBinding samplers[8] = {{in.vt->physical(0), in.vt->sampler()},
                                                      {in.vt->physical(1), in.vt->sampler()},
                                                      {in.vt->physical(2), in.vt->sampler()},
                                                      {in.vsm->atlas(), in.vsm->compareSampler()},
                                                      in.probes[0],
                                                      in.probes[1],
                                                      in.probes[2],
                                                      {in.sceneColor, in.mipSampler}};
    SDL_BindGPUFragmentSamplers(rp, 0, samplers, 8);
    SDL_GPUBuffer* fsBuffers[5] = {in.vt->materialData(), in.lights->lights(), in.lights->grid(), in.vsm->state(), in.hiz};
    SDL_BindGPUFragmentStorageBuffers(rp, 0, fsBuffers, 5);
    const SDL_GPUBufferBinding ib = {indices_, 0};
    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitivesIndirect(rp, in.args, kArgDrawTransparent, 1);
    SDL_EndGPURenderPass(rp);
}

void TransparentRenderer::pick(SDL_GPUCommandBuffer* cmd, const CameraState& camera, const PickInputs& in)
{
    struct {
        float4 cameraPos, cameraRight, cameraUp, cameraForward;
        uint32_t pixel[4];
    } u = {f4(camera.position, camera.zNear), f4(camera.right, 1.0f / camera.p00), f4(camera.up, 1.0f / camera.p11),
           f4(camera.forward, 0.0f), {in.x, in.y, in.width, in.height}};
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    FX_GPU_PASS(cmd, "editor pick (transparent)");
    const SDL_GPUStorageBufferReadWriteBinding rw = {in.result, false};
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
    SDL_BindGPUComputePipeline(cp, pick_);
    const SDL_GPUTextureSamplerBinding depth = {in.depth, in.pointSampler};
    SDL_BindGPUComputeSamplers(cp, 0, &depth, 1);
    SDL_GPUBuffer* ro[4] = {draws_, in.scene, in.skin, in.counters};
    SDL_BindGPUComputeStorageBuffers(cp, 0, ro, 4);
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);
}

void TransparentRenderer::requestPages(const ScenePak& pak, const std::vector<fpak::InstanceGpu>& instances,
                                       VirtualTextureSystem& vt, const CameraState& camera, uint32_t height, uint64_t frame)
{
    FX_ZONE_CAT("TransparentRenderer::requestPages", Rendering);
    const auto& materials = pak.materials();
    const auto& meshes = pak.meshes();
    // The blend materials each mesh's clusters use: found again when a material's blend flag changes.
    uint64_t signature = materials.size();
    for (uint32_t m = 0; m < materials.size(); ++m)
        if (materials[m].flags & fpak::kMaterialAlphaBlend)
            signature = signature * 1099511628211ull + m + 1;
    if (signature != signature_ || meshBlend_.size() != meshes.size()) {
        signature_ = signature;
        meshBlend_.assign(meshes.size(), {});
        uint32_t clusterCount = 0;
        const fpak::ClusterGpu* clusters = pak.view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
        for (uint32_t i = 0; i < meshes.size(); ++i) {
            const fpak::MeshGpu& mesh = meshes[i];
            for (uint32_t c = mesh.clusterOffset; c < mesh.clusterOffset + mesh.clusterCount && c < clusterCount; ++c) {
                const uint32_t m = clusters[c].counts >> 16;
                if (m < materials.size() && (materials[m].flags & fpak::kMaterialAlphaBlend) &&
                    std::find(meshBlend_[i].begin(), meshBlend_[i].end(), m) == meshBlend_[i].end())
                    meshBlend_[i].push_back(m);
            }
        }
    }
    // Per blend material: the most pixels across of any instance in view drawn with it.
    need_.assign(materials.size(), 0.0f);
    bool any = false;
    for (const fpak::InstanceGpu& inst : instances) {
        if ((inst.flags & fpak::kInstanceHidden) || inst.mesh >= meshes.size())
            continue;
        const uint32_t own = inst.flags >> 16;
        const bool overridden = own != 0 && own - 1 < materials.size();
        if (overridden ? !(materials[own - 1].flags & fpak::kMaterialAlphaBlend) : meshBlend_[inst.mesh].empty())
            continue;
        const fpak::MeshGpu& mesh = meshes[inst.mesh];
        float scale = 0.0f;
        for (int c = 0; c < 3; ++c)
            scale = std::max(scale, length(float3{inst.world[0][c], inst.world[1][c], inst.world[2][c]}));
        const float3 half = {mesh.posScale[0] * 32767.5f, mesh.posScale[1] * 32767.5f, mesh.posScale[2] * 32767.5f};
        const float3 local = {mesh.posOffset[0] + half.x, mesh.posOffset[1] + half.y, mesh.posOffset[2] + half.z};
        float3 center;
        for (int r = 0; r < 3; ++r)
            (&center.x)[r] = inst.world[r][0] * local.x + inst.world[r][1] * local.y + inst.world[r][2] * local.z + inst.world[r][3];
        const float radius = length(half) * scale;
        const float3 d = center - camera.position;
        const float along = d.x * camera.forward.x + d.y * camera.forward.y + d.z * camera.forward.z;
        if (along < -radius)
            continue;  // behind the camera
        const float distance = std::max(length(d) - radius, camera.zNear);
        const float pixels = std::min(2.0f * radius * camera.p11 * 0.5f * float(height) / distance, 4.0f * float(height));
        auto note = [&](uint32_t m) {
            need_[m] = std::max(need_[m], pixels);
            any = true;
        };
        if (overridden)
            note(own - 1);
        else
            for (uint32_t m : meshBlend_[inst.mesh])
                note(m);
    }
    if (!any)
        return;
    // Its pages at the mip that size asks for (texture across the instance) and the next coarser
    // (the trilinear blend); never more than kMaxPagesPerLevel pages of one level (coarser then).
    constexpr uint32_t kMaxPagesPerLevel = 64;
    pageEntries_.clear();
    for (uint32_t index = 0; index < materials.size(); ++index) {
        if (need_[index] <= 0.0f)
            continue;
        const fpak::MaterialGpu& m = materials[index];
        if (m.vtPageTable == fpak::kVtNoTexture)
            continue;
        const uint32_t lw = m.vtSize & 0xff, lh = (m.vtSize >> 8) & 0xff, mips = (m.vtSize >> 16) & 0xff;
        auto pagesAt = [&](uint32_t k) {
            return std::max((1u << lw) >> k, 128u) / 128u * (std::max((1u << lh) >> k, 128u) / 128u);
        };
        const float texels = float(1u << std::max(lw, lh));
        uint32_t mip = std::min(uint32_t(std::max(0.0f, std::floor(std::log2(texels / std::max(need_[index], 1.0f))))), mips - 1);
        while (mip + 1 < mips && pagesAt(mip) > kMaxPagesPerLevel)
            ++mip;
        uint32_t offset = 0;
        for (uint32_t k = 0; k < mips; ++k) {
            const uint32_t pages = pagesAt(k);
            if (k == mip || k == mip + 1)
                for (uint32_t e = 0; e < pages; ++e)
                    pageEntries_.push_back(m.vtPageTable + offset + e);
            offset += pages;
        }
    }
    if (!pageEntries_.empty())
        vt.request(pageEntries_, frame);
}

}  // namespace fx
