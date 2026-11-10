#pragma once

// The ocean's FFT on the GPU (water_fft.slang): per cascade its initial amplitudes (from
// OceanSpectrum, uploaded when the spectrum changes), and every frame the spectrum at that time,
// two inverse FFT passes and the assembly into a displacement texture (x, y, z, foam) and a slope
// texture (dh/dx, dh/dz, Jacobian), both with mips, which the water surface samples.

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace fx {

class GpuDevice;
struct OceanSpectrum;

class WaterOcean {
public:
    static constexpr uint32_t kCascades = 3;

    bool init(const GpuDevice& gpu);
    void shutdown();
    // Outside any pass, before the frame's compute passes that keep their uniforms (they push
    // their own). False: no spectrum (nothing to sample).
    // `whitecaps` 0..1: how readily folding crests make foam (the material's crest foam).
    bool update(SDL_GPUCommandBuffer* cmd, const OceanSpectrum& spectrum, float time, float whitecaps);
    bool ready() const { return ready_; }
    SDL_GPUTexture* displacement(uint32_t c) const { return displacement_[c]; }
    SDL_GPUTexture* slopes(uint32_t c) const { return slopes_[c]; }
    // A 1 x 1 texture for the bindings while there is no ocean.
    SDL_GPUTexture* placeholder() const { return placeholder_; }

private:
    bool ensure(uint32_t n);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline *spectrum_ = nullptr, *rows_ = nullptr, *columns_ = nullptr, *assemble_ = nullptr, *mips_pipe_ = nullptr;
    void buildMips(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture);
    SDL_GPUTexture *fieldA_[kCascades] = {}, *fieldB_[kCascades] = {};
    SDL_GPUTexture *displacement_[kCascades] = {}, *slopes_[kCascades] = {};
    SDL_GPUTexture* placeholder_ = nullptr;
    SDL_GPUTexture* scratch_ = nullptr;  // where mip passes write the levels a texture doesn't have
    SDL_GPUBuffer* h0_[kCascades] = {};
    uint32_t n_ = 0, mips_ = 0;
    uint64_t uploaded_ = ~0ull;  // the spectrum revision on the GPU
    float lastTime_ = -1.0f;     // the water time of the last update (foam fades with time, not frames)
    bool ready_ = false;
};

}  // namespace fx
