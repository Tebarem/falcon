#pragma once

#include <SDL3/SDL_gpu.h>

namespace fx {

class GpuDevice;

// M0 bring-up renderer: compute-written vertices + GPU-written indirect args,
// vertex pulling, and SV_ClipDistance. Replaced by the visibility-buffer renderer in M2.
class BringupRenderer {
public:
    static constexpr SDL_GPUTextureFormat kSceneFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    bool init(const GpuDevice& gpu);
    void shutdown();

    // Records the frame into `cmd` and leaves the result in sceneColor().
    void render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, float timeSeconds);
    SDL_GPUTexture* sceneColor() const { return sceneColor_; }

private:
    bool ensureSceneTarget(uint32_t width, uint32_t height);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* animate_ = nullptr;
    SDL_GPUGraphicsPipeline* draw_ = nullptr;
    SDL_GPUBuffer* vertices_ = nullptr;
    SDL_GPUBuffer* drawArgs_ = nullptr;
    SDL_GPUTexture* sceneColor_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
};

}  // namespace fx
