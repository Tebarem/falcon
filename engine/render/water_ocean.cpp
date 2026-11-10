#include "render/water_ocean.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "water/ocean_spectrum.h"

#include "water_fft.shader.h"
#include "water_mips.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace fx {

namespace {

struct FftUniforms {
    float cascade[4];  // size, N, time, choppiness
    float foam[4];     // decay per frame, threshold, gain, log2(N)
};

}  // namespace

bool WaterOcean::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    spectrum_ = gpu.createComputePipeline(shaders::water_fft_cs_spectrum);
    rows_ = gpu.createComputePipeline(shaders::water_fft_cs_ifft_rows);
    columns_ = gpu.createComputePipeline(shaders::water_fft_cs_ifft_columns);
    assemble_ = gpu.createComputePipeline(shaders::water_fft_cs_assemble);
    mips_pipe_ = gpu.createComputePipeline(shaders::water_mips_cs_mips);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    placeholder_ = gpumem::createTexture("Water", device_, &info);
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    scratch_ = gpumem::createTexture("Water", device_, &info);
    return scratch_ && spectrum_ && rows_ && columns_ && assemble_ && mips_pipe_ && placeholder_;
}

void WaterOcean::shutdown()
{
    if (!device_)
        return;
    for (SDL_GPUComputePipeline* p : {spectrum_, rows_, columns_, assemble_, mips_pipe_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    for (uint32_t c = 0; c < kCascades; ++c) {
        for (SDL_GPUTexture* t : {fieldA_[c], fieldB_[c], displacement_[c], slopes_[c]})
            gpumem::releaseTexture(device_, t);
        gpumem::releaseBuffer(device_, h0_[c]);
    }
    gpumem::releaseTexture(device_, placeholder_);
    gpumem::releaseTexture(device_, scratch_);
    device_ = nullptr;
}

bool WaterOcean::ensure(uint32_t n)
{
    if (n_ == n)
        return true;
    for (uint32_t c = 0; c < kCascades; ++c) {
        for (SDL_GPUTexture*& t : {std::ref(fieldA_[c]), std::ref(fieldB_[c]), std::ref(displacement_[c]), std::ref(slopes_[c])}) {
            gpumem::releaseTexture(device_, t);
            t = nullptr;
        }
        gpumem::releaseBuffer(device_, h0_[c]);
        h0_[c] = nullptr;
    }
    n_ = 0;
    mips_ = 0;
    for (uint32_t s = n; s; s >>= 1)
        ++mips_;
    for (uint32_t c = 0; c < kCascades; ++c) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.width = info.height = n;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        fieldA_[c] = gpumem::createTexture("Water", device_, &info);
        fieldB_[c] = gpumem::createTexture("Water", device_, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.num_levels = mips_;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        displacement_[c] = gpumem::createTexture("Water", device_, &info);
        slopes_[c] = gpumem::createTexture("Water", device_, &info);
        SDL_GPUBufferCreateInfo buf = {};
        buf.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        buf.size = n * n * 20;
        h0_[c] = gpumem::createBuffer("Water", device_, &buf);
        if (!fieldA_[c] || !fieldB_[c] || !displacement_[c] || !slopes_[c] || !h0_[c])
            return false;
        gpumem::setTextureName(device_, displacement_[c], "ocean displacement");
        gpumem::setTextureName(device_, slopes_[c], "ocean slopes");
    }
    n_ = n;
    uploaded_ = ~0ull;
    return true;
}

bool WaterOcean::update(SDL_GPUCommandBuffer* cmd, const OceanSpectrum& spectrum, float time, float whitecaps)
{
    ready_ = false;
    if (!spectrum.valid() || !spectrum_ || !ensure(spectrum.n))
        return false;
    FX_ZONE_CAT("WaterOcean::update", Rendering);
    const uint32_t n = n_;
    if (uploaded_ != spectrum.revision) {
        // h0 and omega of every cascade, one transfer.
        const uint32_t bytes = n * n * 20;
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes * kCascades, 0};
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &info);
        if (!tb)
            return false;
        uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
        for (uint32_t c = 0; c < kCascades; ++c) {
            std::memcpy(mapped + c * bytes, spectrum.h0[c].data(), n * n * 16);
            std::memcpy(mapped + c * bytes + n * n * 16, spectrum.omega[c].data(), n * n * 4);
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        FX_GPU_PASS(cmd, "ocean spectrum upload");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        for (uint32_t c = 0; c < kCascades; ++c) {
            const SDL_GPUTransferBufferLocation src = {tb, c * bytes};
            const SDL_GPUBufferRegion dst = {h0_[c], 0, bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        uploaded_ = spectrum.revision;
    }
    const uint32_t groups = (n + 7) / 8;
    // Foam fades over about a second of water time (none while the clock stands: captures).
    const float dt = lastTime_ < 0.0f ? 0.0f : std::clamp(time - lastTime_, 0.0f, 1.0f);
    lastTime_ = time;
    const float decay = std::exp(-dt * 2.5f);
    FX_GPU_SECTION(cmd, "Ocean");
    for (uint32_t c = 0; c < kCascades; ++c) {
        // Foam where the Jacobian drops below the threshold (more crest foam: sooner), fading
        // over about a second.
        FftUniforms u = {{spectrum.sizes[c], float(n), time, spectrum.choppiness},
                         {decay, 1.0f - 0.35f * whitecaps, 4.0f, float(mips_ - 1)}};
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
        const SDL_GPUStorageTextureReadWriteBinding fields[4] = {
            {fieldA_[c], 0, 0, false}, {fieldB_[c], 0, 0, false}, {displacement_[c], 0, 0, false}, {slopes_[c], 0, 0, false}};
        FX_GPU_PASS(cmd, "ocean fft");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, fields, 2, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, spectrum_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &h0_[c], 1);
        SDL_DispatchGPUCompute(pass, groups, groups, 1);
        SDL_EndGPUComputePass(pass);
        pass = SDL_BeginGPUComputePass(cmd, fields, 2, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, rows_);
        SDL_DispatchGPUCompute(pass, n, 1, 1);
        SDL_EndGPUComputePass(pass);
        pass = SDL_BeginGPUComputePass(cmd, fields, 2, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, columns_);
        SDL_DispatchGPUCompute(pass, n, 1, 1);
        SDL_EndGPUComputePass(pass);
        pass = SDL_BeginGPUComputePass(cmd, fields, 4, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, assemble_);
        SDL_DispatchGPUCompute(pass, groups, groups, 1);
        SDL_EndGPUComputePass(pass);
    }
    FX_GPU_PASS(cmd, "ocean mips");
    for (uint32_t c = 0; c < kCascades; ++c) {
        buildMips(cmd, displacement_[c]);
        buildMips(cmd, slopes_[c]);
    }
    ready_ = true;
    return true;
}

// Every mip below level 0, four levels per dispatch (water_mips.slang).
void WaterOcean::buildMips(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture)
{
    for (uint32_t s = 0; s + 1 < mips_; s += 4) {
        SDL_GPUStorageTextureReadWriteBinding rw[5];
        for (uint32_t k = 0; k < 5; ++k)
            rw[k] = s + k < mips_ ? SDL_GPUStorageTextureReadWriteBinding{texture, s + k, 0, false}
                                  : SDL_GPUStorageTextureReadWriteBinding{scratch_, 0, 0, false};  // (past the last level)
        const uint32_t size = n_ >> s;
        const uint32_t u[4] = {size, 0, 0, 0};
        SDL_PushGPUComputeUniformData(cmd, 0, u, sizeof(u));
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, rw, 5, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, mips_pipe_);
        const uint32_t groups = std::max((size / 2 + 15) / 16, 1u);
        SDL_DispatchGPUCompute(pass, groups, groups, 1);
        SDL_EndGPUComputePass(pass);
    }
}

}  // namespace fx
