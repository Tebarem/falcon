#pragma once

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

namespace fx {

class GpuDevice;
class ScenePak;

struct ViewParams {
    float4x4 viewProj;
    float3 cameraPos;
    uint32_t debugMode;  // 0 lit, 1 clusters, 2 normals, 3 instances
};

// M1 naive scene renderer: every leaf cluster of every instance in one instanced draw.
class SceneRenderer {
public:
    static constexpr SDL_GPUTextureFormat kSceneFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    static constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    bool init(const GpuDevice& gpu, const ScenePak& pak);
    void shutdown();
    void render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, const ViewParams& view);
    SDL_GPUTexture* sceneColor() const { return color_; }
    uint32_t drawItemCount() const { return drawItemCount_; }

private:
    bool ensureTargets(uint32_t width, uint32_t height);

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUBuffer* drawItems_ = nullptr;
    uint32_t drawItemCount_ = 0;
    SDL_GPUTexture* color_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
};

}  // namespace fx
