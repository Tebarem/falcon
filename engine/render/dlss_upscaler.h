#pragma once

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>

namespace fx {

class GpuDevice;

// NVIDIA DLSS Super Resolution (DLAA at a render scale of 1) through the DLSS plugin,
// bin/dlss/falcon_dlss.dll (engine/render/dlss_plugin/, built when scripts/fetch-dlss.ps1 has
// fetched NVIDIA's SDK). falcon.exe has no NVIDIA code: the plugin is loaded at runtime.
// Direct3D 12 only: it records into SDL's command list through the native-access patch
// (SDL_falcon_native.h). On Vulkan, without the plugin, or without an NVIDIA RTX GPU it is
// unavailable and the renderer upscales with TAAU.
class DlssUpscaler {
public:
    void init(const GpuDevice& gpu);  // cheap: the plugin and NGX start at the first prepare()
    void shutdown();
    // Starts DLSS the first time it is wanted (once). false: unavailable (reason()).
    bool prepare();
    bool available() const { return available_; }
    const std::string& reason() const { return reason_; }      // why unavailable or failed
    const std::string& modeName() const { return mode_; }      // "DLAA", "Quality", ... (after a dispatch)

    struct Inputs {
        SDL_GPUTexture *color, *depth, *velocity;  // render-sized: HDR, reverse-Z depth,
                                                    // unjittered motion to last frame (render pixels)
        SDL_GPUTexture* reactive = nullptr;        // optional: favour the current frame (r: 0..1; particles)
        SDL_GPUBuffer* exposureState;              // PostProcess::exposureState(): float 256
        float manualExposure;                      // > 0 replaces the measured one
        uint32_t renderWidth, renderHeight, displayWidth, displayHeight;
        float2 jitterPixels;  // render pixels (x right, y down), as the raster was jittered
        float frameTimeMs;
        bool reset;           // camera cut: no history
    };
    // Outside passes. Returns the display-sized RGBA16F frame, or nullptr (the caller falls back).
    SDL_GPUTexture* dispatch(SDL_GPUCommandBuffer* cmd, const Inputs& in);

private:
    bool load();
    void destroyFeature();

    const GpuDevice* gpu_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    void* plugin_ = nullptr;  // HMODULE
    struct Api;
    Api* api_ = nullptr;
    void* feature_ = nullptr;
    uint32_t renderWidth_ = 0, renderHeight_ = 0, displayWidth_ = 0, displayHeight_ = 0;
    SDL_GPUTexture* output_ = nullptr;
    SDL_GPUTexture* exposure_ = nullptr;  // 1x1 R32F
    SDL_GPUComputePipeline* exposurePass_ = nullptr;
    bool available_ = false, failed_ = false, initialised_ = false, tried_ = false;
    std::string reason_, mode_;
};

}  // namespace fx
