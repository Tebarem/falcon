#include "render/preview_renderer.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/skinning.h"
#include "render/virtual_texture.h"

#include "animator_preview.shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

struct PreviewUniforms {
    float4x4 viewProj;
    float4x4 shadowViewProj;
    float cameraPos[4];     // w: exposure
    float sunDirection[4];
    float sunColor[4];      // w: ambient
    float ground[4];        // cell, radius, offset x, offset z
    uint32_t params[4];     // page table offset, shadow map size, flags (1 grid, 2 shadow)
};

// The sun's view of a sphere: orthographic, reverse-Z (1 = nearest the sun), deep enough behind
// it for the ground the shadow falls on.
float4x4 sunViewProj(float3 toSun, float4 focus)
{
    const float r = std::max(focus.w, 0.05f);
    const float3 f = normalize(toSun) * -1.0f;
    const float3 right = normalize(std::fabs(f.y) > 0.99f ? cross(f, float3{1, 0, 0}) : cross(f, float3{0, 1, 0}));
    const float3 up = cross(right, f);
    const float3 center = {focus.x, focus.y, focus.z};
    const float zNear = r, zFar = r * 40.0f;
    const float4x4 view = viewFromBasis(right, up, f, center - f * (2.0f * r));
    float4x4 proj = {};
    proj.m[0][0] = 1.0f / r;
    proj.m[1][1] = 1.0f / r;
    proj.m[2][2] = 1.0f / (zFar - zNear);  // z_view = -near -> 1, -far -> 0
    proj.m[3][2] = zFar / (zFar - zNear);
    proj.m[3][3] = 1.0f;
    return proj * view;
}

}  // namespace

SDL_GPUGraphicsPipeline* PreviewRenderer::pipeline(const GpuDevice& gpu, const char* name, bool depthOnly, bool depthTest,
                                                   bool depthWrite, SDL_GPUPrimitiveType primitive, uint32_t samples,
                                                   SDL_GPUCullMode cull)
{
    using namespace shaders;
    const std::string n = name;
    SDL_GPUShader* vs = gpu.createShader(n == "sky" ? animator_preview_vs_sky : n == "ground" ? animator_preview_vs_ground
                                         : n == "shadow"                    ? animator_preview_vs_shadow
                                         : n == "lines"                     ? animator_preview_vs_lines
                                                                            : animator_preview_vs_main);
    SDL_GPUShader* fs = gpu.createShader(n == "sky" ? animator_preview_fs_sky : n == "ground" ? animator_preview_fs_ground
                                         : n == "lines"                  ? animator_preview_fs_lines
                                         : n == "shadow"                 ? animator_preview_fs_shadow
                                                                         : animator_preview_fs_main);
    if (!vs || !fs) {
        if (vs)
            SDL_ReleaseGPUShader(device_, vs);
        if (fs)
            SDL_ReleaseGPUShader(device_, fs);
        return nullptr;
    }
    SDL_GPUColorTargetDescription color = {};
    color.format = kColorFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = primitive;
    info.rasterizer_state.cull_mode = cull;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = depthOnly ? SDL_GPU_SAMPLECOUNT_1 : SDL_GPUSampleCount(samples);
    info.depth_stencil_state.enable_depth_test = depthTest;
    info.depth_stencil_state.enable_depth_write = depthWrite;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
    if (!depthOnly) {
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
    }
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!p)
        FX_ERROR("preview %s pipeline: %s", name, SDL_GetError());
    return p;
}

bool PreviewRenderer::init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt, SkinningSystem& skinning)
{
    FX_ZONE_CAT("PreviewRenderer::init", Rendering);
    device_ = gpu.get();
    pak_ = &pak;
    vt_ = &vt;
    skinning_ = &skinning;
    samples_ = SDL_GPUTextureSupportsSampleCount(device_, kColorFormat, SDL_GPU_SAMPLECOUNT_4) &&
                       SDL_GPUTextureSupportsSampleCount(device_, kDepthFormat, SDL_GPU_SAMPLECOUNT_4)
                   ? SDL_GPU_SAMPLECOUNT_4
                   : SDL_GPU_SAMPLECOUNT_1;
    const uint32_t s = uint32_t(samples_);
    sky_ = pipeline(gpu, "sky", false, false, false, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, s, SDL_GPU_CULLMODE_NONE);
    ground_ = pipeline(gpu, "ground", false, true, true, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, s, SDL_GPU_CULLMODE_NONE);
    main_ = pipeline(gpu, "main", false, true, true, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, s, SDL_GPU_CULLMODE_NONE);
    shadow_ = pipeline(gpu, "shadow", true, true, true, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 1, SDL_GPU_CULLMODE_NONE);
    lines_ = pipeline(gpu, "lines", false, false, false, SDL_GPU_PRIMITIVETYPE_LINELIST, s, SDL_GPU_CULLMODE_NONE);
    if (!sky_ || !ground_ || !main_ || !shadow_ || !lines_)
        return false;

    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.enable_compare = true;
    si.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;  // lit: at least as near the sun as the nearest caster
    shadowSampler_ = SDL_CreateGPUSampler(device_, &si);

    SDL_GPUBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = kMaxLineVertices * 16;
    lineBuffer_ = gpumem::createBuffer("Animation preview", device_, &bi);
    const SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kMaxLineVertices * 16, 0};
    lineUpload_ = gpumem::createTransferBuffer("Animation preview", device_, &ti);
    return shadowSampler_ && lineBuffer_ && lineUpload_;
}

void PreviewRenderer::shutdown()
{
    if (!device_)
        return;
    releaseTargets();
    for (SDL_GPUGraphicsPipeline* p : {sky_, ground_, main_, shadow_, lines_})
        if (p)
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
    if (shadowSampler_)
        SDL_ReleaseGPUSampler(device_, shadowSampler_);
    gpumem::releaseBuffer(device_, items_);
    gpumem::releaseBuffer(device_, parts_);
    gpumem::releaseBuffer(device_, lineBuffer_);
    gpumem::releaseTransferBuffer(device_, lineUpload_);
    device_ = nullptr;
}

void PreviewRenderer::releaseTargets()
{
    if (!device_)
        return;
    for (SDL_GPUTexture** t : {&msaaColor_, &msaaDepth_, &resolved_, &shadowMap_}) {
        gpumem::releaseTexture(device_, *t);
        *t = nullptr;
    }
    width_ = height_ = 0;
}

bool PreviewRenderer::ensureTargets(uint32_t width, uint32_t height)
{
    if (!shadowMap_) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = info.height = kShadowSize;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.format = kDepthFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        shadowMap_ = gpumem::createTexture("Animation preview", device_, &info);
    }
    if (resolved_ && width == width_ && height == height_)
        return shadowMap_ != nullptr;
    for (SDL_GPUTexture** t : {&msaaColor_, &msaaDepth_, &resolved_}) {
        gpumem::releaseTexture(device_, *t);
        *t = nullptr;
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = kColorFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    resolved_ = gpumem::createTexture("Animation preview", device_, &info);
    info.sample_count = samples_;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (samples_ != SDL_GPU_SAMPLECOUNT_1)
        msaaColor_ = gpumem::createTexture("Animation preview", device_, &info);
    info.format = kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    msaaDepth_ = gpumem::createTexture("Animation preview", device_, &info);
    width_ = width;
    height_ = height;
    return resolved_ && msaaDepth_ && shadowMap_ && (samples_ == SDL_GPU_SAMPLECOUNT_1 || msaaColor_);
}

void PreviewRenderer::setParts(const std::vector<PreviewPart>& parts)
{
    const auto& meshes = pak_->meshes();
    const auto* clusters = pak_->view<fpak::ClusterGpu>(fpak::Section::Clusters);
    std::vector<uint32_t> items;
    std::vector<fpak::InstanceGpu> rows;
    materials_.clear();
    for (uint32_t p = 0; p < parts.size(); ++p) {
        const PreviewPart& part = parts[p];
        if (part.mesh >= meshes.size())
            continue;
        fpak::InstanceGpu row = {};
        std::memcpy(row.world, part.world, sizeof(row.world));
        row.mesh = part.mesh;
        if (part.skinRecord != ~0u) {
            row.flags |= fpak::kInstanceSkinned;
            row.skin = part.skinRecord;
        }
        if (part.material >= 0)
            row.flags |= (uint32_t(part.material) + 1) << fpak::kInstanceMaterialShift;
        const uint32_t index = uint32_t(rows.size());
        rows.push_back(row);
        const fpak::MeshGpu& m = meshes[part.mesh];
        for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount; ++c)
            if (clusters[c].selfError == 0.0f) {
                items.push_back(index);
                items.push_back(c);
                const uint32_t material = part.material >= 0 ? uint32_t(part.material) : clusters[c].counts >> 16;
                if (std::find(materials_.begin(), materials_.end(), material) == materials_.end())
                    materials_.push_back(material);
            }
    }
    itemCount_ = uint32_t(items.size() / 2);
    if (items.empty())
        return;
    // Grown as needed (one model at a time: small).
    const auto ensure = [&](SDL_GPUBuffer*& buffer, uint32_t& capacity, uint32_t bytes) {
        if (buffer && capacity >= bytes)
            return buffer != nullptr;
        gpumem::releaseBuffer(device_, buffer);
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        bi.size = capacity = std::max(bytes, 256u);
        buffer = gpumem::createBuffer("Animation preview", device_, &bi);
        return buffer != nullptr;
    };
    const uint32_t itemBytes = uint32_t(items.size() * 4), rowBytes = uint32_t(rows.size() * sizeof(fpak::InstanceGpu));
    if (!ensure(items_, itemCapacity_, itemBytes) || !ensure(parts_, partCapacity_, rowBytes)) {
        itemCount_ = 0;
        return;
    }
    const SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, itemBytes + rowBytes, 0};
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &ti);
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, staging, false));
    std::memcpy(mapped, items.data(), itemBytes);
    std::memcpy(mapped + itemBytes, rows.data(), rowBytes);
    SDL_UnmapGPUTransferBuffer(device_, staging);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation a = {staging, 0}, b = {staging, itemBytes};
    const SDL_GPUBufferRegion ra = {items_, 0, itemBytes}, rb = {parts_, 0, rowBytes};
    SDL_UploadToGPUBuffer(copy, &a, &ra, false);
    SDL_UploadToGPUBuffer(copy, &b, &rb, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, staging);
}

void PreviewRenderer::requestTextures(float pixels, uint64_t frame)
{
    const auto& materials = pak_->materials();
    std::vector<uint32_t> entries;
    for (const uint32_t index : materials_) {
        if (index >= materials.size())
            continue;
        const fpak::MaterialGpu& m = materials[index];
        if (m.vtPageTable == fpak::kVtNoTexture)
            continue;
        const uint32_t lw = m.vtSize & 0xff, lh = (m.vtSize >> 8) & 0xff, mips = (m.vtSize >> 16) & 0xff;
        // The mip whose texels are about the character's pixels (a texture spread over it once).
        const float texels = float(1u << std::max(lw, lh));
        const uint32_t mip = std::min(uint32_t(std::max(0.0f, std::floor(std::log2(texels / std::max(pixels, 1.0f))))), mips - 1);
        uint32_t offset = 0, total = 0;
        for (uint32_t k = 0; k < mips; ++k) {
            const uint32_t pages = std::max((1u << lw) >> k, 128u) / 128u * (std::max((1u << lh) >> k, 128u) / 128u);
            if (k < mip)
                offset += pages;
            total += pages;
        }
        for (uint32_t e = m.vtPageTable + offset; e < m.vtPageTable + total; ++e)
            entries.push_back(e);
    }
    vt_->request(entries, frame);
}

void PreviewRenderer::render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, const PreviewFrame& f)
{
    FX_ZONE_CAT("PreviewRenderer::render", Rendering);
    if (!width || !height || !ensureTargets(width, height))
        return;
    FX_GPU_SECTION(cmd, "Animation preview");
    PreviewUniforms u = {};
    u.viewProj = f.viewProj;
    u.shadowViewProj = sunViewProj(f.sunDirection, f.focus);
    u.cameraPos[0] = f.cameraPos.x, u.cameraPos[1] = f.cameraPos.y, u.cameraPos[2] = f.cameraPos.z, u.cameraPos[3] = f.exposure;
    const float3 sun = normalize(f.sunDirection);
    u.sunDirection[0] = sun.x, u.sunDirection[1] = sun.y, u.sunDirection[2] = sun.z;
    u.sunColor[0] = f.sunColor.x, u.sunColor[1] = f.sunColor.y, u.sunColor[2] = f.sunColor.z, u.sunColor[3] = f.ambient;
    u.ground[0] = f.gridCell, u.ground[1] = f.groundRadius, u.ground[2] = f.gridOffset.x, u.ground[3] = f.gridOffset.y;
    const bool shadows = f.shadows && itemCount_ > 0;
    u.params[0] = vt_->pageTableOffset();
    u.params[1] = kShadowSize;
    u.params[2] = (f.grid ? 1u : 0u) | (shadows ? 2u : 0u);

    SDL_GPUBuffer* vsBuffers[4] = {items_, parts_, pak_->sceneBuffer(), skinning_->buffer()};
    // The character into the sun's map.
    {
        FX_GPU_PASS(cmd, "preview shadow");
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = shadowMap_;
        depth.clear_depth = 0.0f;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.cycle = true;
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, nullptr, 0, &depth);
        if (shadows) {
            SDL_BindGPUGraphicsPipeline(pass, shadow_);
            SDL_BindGPUVertexStorageBuffers(pass, 0, vsBuffers, 4);
            SDL_DrawGPUPrimitives(pass, fpak::kMaxClusterTriangles * 3, itemCount_, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }
    // Lines (bones): this frame's, staged before the render pass.
    uint32_t lineVertices = 0;
    if (f.lines && !f.lines->empty()) {
        lineVertices = std::min(uint32_t(f.lines->size()) & ~1u, kMaxLineVertices);
        std::memcpy(SDL_MapGPUTransferBuffer(device_, lineUpload_, true), f.lines->data(), lineVertices * 16);
        SDL_UnmapGPUTransferBuffer(device_, lineUpload_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src = {lineUpload_, 0};
        const SDL_GPUBufferRegion dst = {lineBuffer_, 0, lineVertices * 16};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
    }

    FX_GPU_PASS(cmd, "preview");
    const bool msaa = samples_ != SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUColorTargetInfo color = {};
    color.texture = msaa ? msaaColor_ : resolved_;
    color.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = msaa ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
    color.resolve_texture = msaa ? resolved_ : nullptr;
    color.cycle = true;
    color.cycle_resolve_texture = msaa;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = msaaDepth_;
    depth.clear_depth = 0.0f;  // reverse-Z: far = 0
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    SDL_GPUTextureSamplerBinding samplers[4] = {{vt_->physical(0), vt_->sampler()}, {vt_->physical(1), vt_->sampler()},
                                                {vt_->physical(2), vt_->sampler()}, {shadowMap_, shadowSampler_}};
    SDL_BindGPUGraphicsPipeline(pass, sky_);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_BindGPUGraphicsPipeline(pass, ground_);
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    if (itemCount_) {
        SDL_BindGPUGraphicsPipeline(pass, main_);
        SDL_BindGPUVertexStorageBuffers(pass, 0, vsBuffers, 4);
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
        SDL_GPUBuffer* materialData = vt_->materialData();
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &materialData, 1);
        SDL_DrawGPUPrimitives(pass, fpak::kMaxClusterTriangles * 3, itemCount_, 0, 0);
    }
    if (lineVertices) {
        SDL_BindGPUGraphicsPipeline(pass, lines_);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &lineBuffer_, 1);
        SDL_DrawGPUPrimitives(pass, lineVertices, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

}  // namespace fx
