#include "render/scene_renderer.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"

#include "naive_scene.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace fx {

namespace {

struct ViewUniforms {
    float4x4 viewProj;
    float cameraPos[4];
};

struct ShadeUniforms {
    float sunDirection[4];
    uint32_t debugMode, pad[3];
};

}  // namespace

bool SceneRenderer::init(const GpuDevice& gpu, const ScenePak& pak)
{
    FX_ZONE_CAT("SceneRenderer::init", Rendering);
    device_ = gpu.get();
    pak_ = &pak;

    SDL_GPUShader* vs = gpu.createShader(shaders::naive_scene_vs_main);
    SDL_GPUShader* fs = gpu.createShader(shaders::naive_scene_fs_main);
    if (!vs || !fs)
        return false;
    SDL_GPUColorTargetDescription color = {};
    color.format = kSceneFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // backface cone culling arrives with M2
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!pipeline_) {
        FX_ERROR("scene pipeline: %s", SDL_GetError());
        return false;
    }

    // (instance, leaf cluster) pairs. M2 replaces this with GPU culling + LOD selection.
    uint32_t instanceCount = 0;
    const auto* instances = pak.view<fpak::InstanceGpu>(fpak::Section::Instances, &instanceCount);
    const auto* meshes = pak.view<fpak::MeshGpu>(fpak::Section::Meshes);
    const auto* clusters = pak.view<fpak::ClusterGpu>(fpak::Section::Clusters);
    std::vector<uint32_t> items;
    for (uint32_t i = 0; i < instanceCount; ++i) {
        const fpak::MeshGpu& m = meshes[instances[i].mesh];
        for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount; ++c)
            if (clusters[c].selfError == 0.0f) {
                items.push_back(i);
                items.push_back(c);
            }
    }
    drawItemCount_ = uint32_t(items.size() / 2);
    const uint32_t bytes = uint32_t(std::max<size_t>(items.size() * 4, 16));

    SDL_GPUBufferCreateInfo binfo = {};
    binfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    binfo.size = bytes;
    drawItems_ = gpumem::createBuffer("Rendering", device_, &binfo);
    SDL_GPUTransferBufferCreateInfo tinfo = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* staging = gpumem::createTransferBuffer("Rendering", device_, &tinfo);
    if (!drawItems_ || !staging)
        return false;
    void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false);
    SDL_memcpy(mapped, items.data(), items.size() * 4);
    SDL_UnmapGPUTransferBuffer(device_, staging);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {staging, 0};
    SDL_GPUBufferRegion dst = {drawItems_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    gpumem::releaseTransferBuffer(device_, staging);
    return true;
}

void SceneRenderer::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseTexture(device_, color_);
    gpumem::releaseTexture(device_, depth_);
    gpumem::releaseBuffer(device_, drawItems_);
    SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    device_ = nullptr;
}

bool SceneRenderer::ensureTargets(uint32_t width, uint32_t height)
{
    if (color_ && width == width_ && height == height_)
        return true;
    gpumem::releaseTexture(device_, color_);
    gpumem::releaseTexture(device_, depth_);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = kSceneFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    color_ = gpumem::createTexture("Rendering", device_, &info);
    info.format = kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depth_ = gpumem::createTexture("Rendering", device_, &info);
    width_ = width;
    height_ = height;
    return color_ && depth_;
}

void SceneRenderer::render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, const ViewParams& view)
{
    FX_ZONE_CAT("SceneRenderer::render", Rendering);
    if (!ensureTargets(width, height))
        return;

    SDL_GPUColorTargetInfo color = {};
    color.texture = color_;
    color.clear_color = {0.30f, 0.38f, 0.48f, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    color.cycle = true;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depth_;
    depth.clear_depth = 0.0f;  // reverse-Z: far = 0
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.cycle = true;

    const ViewUniforms vu = {view.viewProj, {view.cameraPos.x, view.cameraPos.y, view.cameraPos.z, 1.0f}};
    const float3 sun = normalize(float3{0.35f, 0.9f, 0.25f});
    const ShadeUniforms su = {{sun.x, sun.y, sun.z, 0.0f}, view.debugMode, {0, 0, 0}};
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
    SDL_PushGPUFragmentUniformData(cmd, 0, &su, sizeof(su));

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBuffer* vsBuffers[2] = {drawItems_, pak_->sceneBuffer()};
    SDL_BindGPUVertexStorageBuffers(pass, 0, vsBuffers, 2);
    SDL_GPUBuffer* fsBuffers[1] = {pak_->sceneBuffer()};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fsBuffers, 1);
    if (drawItemCount_)
        SDL_DrawGPUPrimitives(pass, fpak::kMaxClusterTriangles * 3, drawItemCount_, 0, 0);
    SDL_EndGPURenderPass(pass);
}

}  // namespace fx
