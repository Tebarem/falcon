#pragma once

#include "gpu/shader_blob.h"

#include <SDL3/SDL_gpu.h>

#include <atomic>
#include <thread>

namespace fx {

struct GpuConfig {
    const char* driver = nullptr;  // "direct3d12", "vulkan", "metal", or null for SDL's choice (D3D12 first)
#if defined(__APPLE__)
    // macOS: vsync. Metal's IMMEDIATE (displaySyncEnabled off) ran the launcher and editor
    // uncapped: hundreds of light frames a second, each with its CPU work (the frame loop,
    // Chromium's begin-frame, the UI's pixel copy), so the CPU was busy and the GPU nearly idle.
    SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
#else
    // Uncapped by default (tearing allowed); falls back to MAILBOX, then VSYNC.
    SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_IMMEDIATE;
#endif
    uint32_t framesInFlight = 2;
    bool debug = false;
};

// Capability probe results the renderer design depends on (M0 spikes).
struct GpuCaps {
    bool r32uintColorTarget = false;   // visibility buffer
    bool r32uintStorageRW = false;     // VSM physical pages / atomics
    bool rg32uintColorTarget = false;
    bool bc7 = false, bc5 = false;     // virtual texture physical cache
    bool d32Depth = false;
    bool mailbox = false;
};

class GpuDevice {
public:
    // Device creation is the longest boot step (driver init; SDL's D3D12 backend also
    // creates a throwaway probe device when DXIL is enabled), so it starts on a worker
    // thread at the top of main and overlaps window creation and other boot work.
    void beginCreate(const GpuConfig& config);
    // Before SDL starts (beginCreate needs its video subsystem): D3D12 hands out one device per
    // adapter, the same one again while it is alive, so a device made here on a thread, held
    // until SDL's is made, takes the driver's start (~250 ms) off SDL's two device creations.
    static void warmUp(const GpuConfig& config);
    ~GpuDevice() { shutdown(); }
    // The worker has finished (made the device or failed): attachWindow will not wait.
    bool created() const { return created_.load(std::memory_order_acquire); }
    // Joins device creation, then claims the window on the calling (main) thread.
    bool attachWindow(SDL_Window* window);
    void shutdown();

    SDL_GPUDevice* get() const { return device_; }
    SDL_Window* window() const { return window_; }
    const GpuCaps& caps() const { return caps_; }
    const char* driverName() const { return driverName_; }
    SDL_GPUTextureFormat swapchainFormat() const;
    // Switches the swapchain's present mode (falling back like at creation); returns the mode used.
    SDL_GPUPresentMode setPresentMode(SDL_GPUPresentMode mode);
    SDL_GPUPresentMode presentMode() const { return presentMode_; }

    // Picks DXIL, MSL or SPIR-V from the embedded blob for the active backend.
    SDL_GPUShader* createShader(const ShaderBlob& blob) const;
    SDL_GPUComputePipeline* createComputePipeline(const ShaderBlob& blob) const;

private:
    void probeCaps();

    GpuConfig config_;
    std::thread creator_;
    std::atomic<bool> created_{false};
    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_GPUShaderFormat format_ = SDL_GPU_SHADERFORMAT_INVALID;
    const char* driverName_ = "";
    GpuCaps caps_;
    SDL_GPUPresentMode presentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
};

}  // namespace fx
