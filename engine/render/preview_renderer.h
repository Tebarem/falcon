#pragma once

// M15: the Animator's preview pane. One character (a few skinned or rigid parts of a model) on a
// ground disc with a grid, lit by a sun with its own shadow map and by the sky, drawn forward into
// a target of its own (4x MSAA, tonemapped there; engine/shaders/animator_preview.slang). It shares the
// scene's geometry, skinning and virtual textures but none of the main renderer's per-view state,
// so the scene view keeps its history while the preview draws. Graphics shaders can't write the
// virtual texture's feedback list, so the pages the parts need are asked for from the CPU
// (requestTextures).

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;
class ScenePak;
class SkinningSystem;
class VirtualTextureSystem;

struct PreviewPart {
    uint32_t mesh = 0;
    uint32_t skinRecord = ~0u;  // its skinning record (SkinningSystem floating records), ~0u: rigid
    float world[3][4] = {};     // model -> preview space
    int32_t material = -1;      // a material drawn with instead of the mesh's own (-1: its own)
};

struct PreviewFrame {
    float4x4 viewProj;
    float3 cameraPos;
    float exposure = 1.0f;
    float3 sunDirection = {0.4f, 0.8f, 0.45f};  // toward the sun
    float3 sunColor = {3.0f, 2.9f, 2.7f};
    float ambient = 1.0f;
    float4 focus = {0, 1, 0, 1};  // the character's bounds (xyz, radius): the shadow map covers it
    float groundRadius = 30.0f, gridCell = 1.0f;
    float2 gridOffset = {0, 0};  // the grid's shift (root motion moving the world under a character kept still)
    bool grid = true, shadows = true;
    const std::vector<float4>* lines = nullptr;  // line pairs: xyz, w = packed RGBA8 (bit-cast)
};

class PreviewRenderer {
public:
    static constexpr uint32_t kShadowSize = 2048;
    static constexpr uint32_t kMaxLineVertices = 4096;

    bool init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt, SkinningSystem& skinning);
    void shutdown();

    // What it draws from now on (leaf clusters of each part's mesh).
    void setParts(const std::vector<PreviewPart>& parts);
    bool empty() const { return itemCount_ == 0; }
    // Asks the virtual texture for the parts' pages from the mip that `pixels` (the character's
    // size on screen) needs down, refreshing those already there. Call once a frame while shown.
    void requestTextures(float pixels, uint64_t frame);

    void render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, const PreviewFrame& frame);
    SDL_GPUTexture* color() const { return resolved_; }  // display-ready RGBA8, width x height
    // Its targets go (the pane closed); the next render() makes them again.
    void releaseTargets();

private:
    bool ensureTargets(uint32_t width, uint32_t height);
    SDL_GPUGraphicsPipeline* pipeline(const GpuDevice& gpu, const char* name, bool depthOnly, bool depthTest, bool depthWrite,
                                      SDL_GPUPrimitiveType primitive, uint32_t samples, SDL_GPUCullMode cull);

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    VirtualTextureSystem* vt_ = nullptr;
    SkinningSystem* skinning_ = nullptr;
    SDL_GPUGraphicsPipeline *sky_ = nullptr, *ground_ = nullptr, *main_ = nullptr, *shadow_ = nullptr, *lines_ = nullptr;
    SDL_GPUSampler* shadowSampler_ = nullptr;
    SDL_GPUBuffer *items_ = nullptr, *parts_ = nullptr, *lineBuffer_ = nullptr;
    SDL_GPUTransferBuffer* lineUpload_ = nullptr;
    uint32_t itemCount_ = 0, itemCapacity_ = 0, partCapacity_ = 0;
    std::vector<uint32_t> materials_;  // the parts' materials (texture requests)
    SDL_GPUTexture *msaaColor_ = nullptr, *msaaDepth_ = nullptr, *resolved_ = nullptr, *shadowMap_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
};

}  // namespace fx
