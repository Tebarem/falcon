#include "render/bringup_renderer.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"

#include "bringup_triangle.shader.h"

#include <SDL3/SDL.h>

namespace fx {

namespace {

struct AnimateParams {
    float time, aspect, pad[2];
};
struct DrawParams {
    float clipPlane[4];
};

}  // namespace

bool BringupRenderer::init(const GpuDevice& gpu)
{
    FX_ZONE_CAT("BringupRenderer::init", Rendering);
    device_ = gpu.get();

    animate_ = gpu.createComputePipeline(shaders::bringup_triangle_cs_animate);
    SDL_GPUShader* vs = gpu.createShader(shaders::bringup_triangle_vs_main);
    SDL_GPUShader* fs = gpu.createShader(shaders::bringup_triangle_fs_main);
    if (!animate_ || !vs || !fs)
        return false;

    SDL_GPUColorTargetDescription color = {};
    color.format = kSceneFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    draw_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    if (!draw_) {
        FX_ERROR("bring-up pipeline: %s", SDL_GetError());
        return false;
    }

    SDL_GPUBufferCreateInfo vb = {};
    vb.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    vb.size = 3 * 32;
    vertices_ = gpumem::createBuffer("Rendering", device_, &vb);
    SDL_GPUBufferCreateInfo ab = {};
    ab.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_INDIRECT;
    ab.size = 16;
    drawArgs_ = gpumem::createBuffer("Rendering", device_, &ab);
    return vertices_ && drawArgs_;
}

void BringupRenderer::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseTexture(device_, sceneColor_);
    gpumem::releaseBuffer(device_, drawArgs_);
    gpumem::releaseBuffer(device_, vertices_);
    SDL_ReleaseGPUGraphicsPipeline(device_, draw_);
    SDL_ReleaseGPUComputePipeline(device_, animate_);
    device_ = nullptr;
}

bool BringupRenderer::ensureSceneTarget(uint32_t width, uint32_t height)
{
    if (sceneColor_ && width == width_ && height == height_)
        return true;
    gpumem::releaseTexture(device_, sceneColor_);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kSceneFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    sceneColor_ = gpumem::createTexture("Rendering", device_, &info);
    width_ = width;
    height_ = height;
    return sceneColor_ != nullptr;
}

void BringupRenderer::render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, float time)
{
    FX_ZONE_CAT("BringupRenderer::render", Rendering);
    if (!ensureSceneTarget(width, height))
        return;

    // Compute writes this frame's vertices and draw args. Cycling hands us a fresh
    // backing buffer if the previous frame is still reading it, so the GPU never waits.
    const AnimateParams animate = {time, float(width) / float(height), {0, 0}};
    SDL_PushGPUComputeUniformData(cmd, 0, &animate, sizeof(animate));
    SDL_GPUStorageBufferReadWriteBinding rw[2] = {{vertices_, true}, {drawArgs_, true}};
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, 2);
    SDL_BindGPUComputePipeline(cp, animate_);
    SDL_DispatchGPUCompute(cp, 1, 1, 1);
    SDL_EndGPUComputePass(cp);

    SDL_GPUColorTargetInfo target = {};
    target.texture = sceneColor_;
    target.clear_color = {0.02f, 0.025f, 0.035f, 1.0f};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(rp, draw_);
    SDL_BindGPUVertexStorageBuffers(rp, 0, &vertices_, 1);
    const float a = time * 0.35f;
    const DrawParams draw = {{SDL_cosf(a), SDL_sinf(a), 0.0f, 0.2f}};
    SDL_PushGPUVertexUniformData(cmd, 0, &draw, sizeof(draw));
    SDL_DrawGPUPrimitivesIndirect(rp, drawArgs_, 0, 1);
    SDL_EndGPURenderPass(rp);
}

}  // namespace fx
