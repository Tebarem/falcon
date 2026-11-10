#pragma once

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>

namespace fx {

class GpuDevice;

// AMD FSR through AMD's FSR API and its signed DLLs (bin/fsr/, fetched by scripts/fetch-fsr.ps1):
// FSR 4 on GPUs that support it, FSR 3.1 elsewhere. Direct3D 12 only: it records into SDL's
// command list through the native-access patch (SDL_falcon_native.h). Without FALCON_FSR, on
// Vulkan, or when the DLLs are missing it is unavailable and the renderer upscales with TAAU.
class FsrUpscaler {
public:
    bool init(const GpuDevice& gpu);  // false: unavailable (reason())
    void shutdown();
    bool available() const { return available_; }
    const std::string& reason() const { return reason_; }      // why unavailable
    const std::string& versionName() const { return version_; }  // "FSR 4.1.1" (after a dispatch)

    struct Inputs {
        SDL_GPUTexture *color, *depth, *velocity;  // render-sized: HDR, reverse-Z infinite depth,
                                                    // unjittered motion to last frame (render pixels)
        SDL_GPUTexture* composition = nullptr;     // optional: transparency-and-composition mask (r: 0..1)
        SDL_GPUTexture* reactive = nullptr;        // optional: reactive mask (r: 0..1; particles)
        SDL_GPUBuffer* exposureState;              // PostProcess::exposureState(): float 256
        float manualExposure;                      // > 0 replaces the measured one
        uint32_t renderWidth, renderHeight, displayWidth, displayHeight;
        float2 jitterPixels;  // render pixels (x right, y down), as the raster was jittered
        float frameTimeMs, zNear, fovY;  // fovY: vertical, radians
        float sharpness;      // RCAS, 0-1 (0: off)
        bool reset;           // camera cut: no history
    };
    // Outside passes. Returns the display-sized RGBA16F frame, or nullptr (the caller falls back).
    SDL_GPUTexture* dispatch(SDL_GPUCommandBuffer* cmd, const Inputs& in);

private:
    bool ensureContext(uint32_t displayWidth, uint32_t displayHeight);
    void destroyContext();

    SDL_GPUDevice* device_ = nullptr;
    void* d3d12Device_ = nullptr;
    void* loader_ = nullptr;  // HMODULEs
    void* upscalerModule_ = nullptr;
    struct Api;
    Api* api_ = nullptr;
    void* context_ = nullptr;  // ffxContext
    uint32_t contextWidth_ = 0, contextHeight_ = 0;
    SDL_GPUTexture* output_ = nullptr;
    SDL_GPUTexture* exposure_ = nullptr;  // 1x1 R32F
    SDL_GPUComputePipeline* exposurePass_ = nullptr;
    bool available_ = false, failed_ = false;
    std::string reason_, version_;
};

}  // namespace fx
