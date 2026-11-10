#include "render/post_process.h"

#include "render/shader_file.h"
#include <utility>
#include <filesystem>

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"

#include "ao.shader.h"
#include "ao_blur.shader.h"
#include "bloom.shader.h"
#include "exposure.shader.h"
#include "fxaa.shader.h"
#include "taa.shader.h"
#include "tonemap.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace fx {

namespace {

struct AoUniforms {
    ViewReconstructGpu view;
    float params[4];  // world radius, power, max radius (pixels)
    uint32_t mode[4];  // GTAO / SSAO, scale, sample offset
};

struct TaaUniforms {
    ViewReconstructGpu view;  // render size
    float4x4 viewToPrevClip;
    float params[4];  // current-frame blend, reset
    uint32_t display[4];  // display (output) size
};

struct ExposureUniforms {
    float range[4];  // min log2 luminance, 1 / range, adaptation blend, compensation
    uint32_t screen[4];
    float limit[4];  // the exposure past which it rises at half the rate (0: none)
};

struct BloomUniforms {
    float texel[4];  // source texel size, destination size
    uint32_t mode[4];
};

struct TonemapUniforms {
    float params[4];  // bloom intensity, manual exposure, look power, look saturation
    uint32_t screen[4];
};

struct FxaaUniforms {
    float texel[4];  // 1 / width, 1 / height
    uint32_t screen[4];
};

constexpr float kMinLogLuminance = -12.0f, kMaxLogLuminance = 8.0f;

SDL_GPUTexture* createTexture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                              SDL_GPUTextureUsageFlags usage, const char* name)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = std::max(w, 1u);
    info.height = std::max(h, 1u);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* t = gpumem::createTexture("Post", device, &info);
    if (t)
        gpumem::setTextureName(device, t, name);
    else
        FX_ERROR("post: texture %s: %s", name, SDL_GetError());
    return t;
}

void dispatch2d(SDL_GPUComputePass* pass, uint32_t w, uint32_t h)
{
    SDL_DispatchGPUCompute(pass, (w + 7) / 8, (h + 7) / 8, 1);
}

}  // namespace

bool PostProcess::init(const GpuDevice& gpu)
{
    FX_ZONE_CAT("PostProcess::init", Rendering);
    device_ = gpu.get();
    gpu_ = &gpu;
    aoPass_ = gpu.createComputePipeline(shaders::ao_cs_ao);
    aoBlurPass_ = gpu.createComputePipeline(shaders::ao_blur_cs_ao_blur);
    taaPass_ = gpu.createComputePipeline(shaders::taa_cs_taa);
    histogramPass_ = gpu.createComputePipeline(shaders::exposure_cs_histogram);
    exposurePass_ = gpu.createComputePipeline(shaders::exposure_cs_exposure);
    bloomDownPass_ = gpu.createComputePipeline(shaders::bloom_cs_bloom_down);
    bloomUpPass_ = gpu.createComputePipeline(shaders::bloom_cs_bloom_up);
    tonemapPass_ = gpu.createComputePipeline(shaders::tonemap_cs_tonemap);
    fxaaPass_ = gpu.createComputePipeline(shaders::fxaa_cs_fxaa);
    if (!aoPass_ || !aoBlurPass_ || !taaPass_ || !histogramPass_ || !exposurePass_ || !bloomDownPass_ || !bloomUpPass_ || !tonemapPass_ ||
        !fxaaPass_)
        return false;
    for (SDL_GPUTextureFormat f : {kHdrFormat, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, kOutputFormat})
        if (!SDL_GPUTextureSupportsFormat(device_, f, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            FX_ERROR("post: required storage format %d is not supported", int(f));
            return false;
        }

    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    point_ = SDL_CreateGPUSampler(device_, &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear_ = SDL_CreateGPUSampler(device_, &s);
    noReactive_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                                "no reactive mask");

    SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                 (256 + 4) * 4, 0};
    exposureState_ = gpumem::createBuffer("Post", device_, &b);
    gpumem::setBufferName(device_, exposureState_, "exposure state");
    if (!point_ || !linear_ || !exposureState_)
        return false;
    gpumem::setBufferName(device_, exposureState_, "exposure (histogram + state)");
    // Zeroed histogram, exposure 0 = "adopt the first measurement".
    SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, b.size, 0};
    SDL_GPUTransferBuffer* staging = gpumem::createTransferBuffer("Post", device_, &t, "exposure init upload");
    SDL_memset(SDL_MapGPUTransferBuffer(device_, staging, false), 0, b.size);
    SDL_UnmapGPUTransferBuffer(device_, staging);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {staging, 0};
    const SDL_GPUBufferRegion dst = {exposureState_, 0, b.size};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    gpumem::releaseTransferBuffer(device_, staging);
    return true;
}

void PostProcess::setEffects(std::vector<EffectRequest> effects, float time, float delta, uint64_t frame)
{
    effects_ = std::move(effects);
    effectTime_[0] = time;
    effectTime_[1] = delta;
    effectTime_[2] = float(frame);
}

const PostProcess::EffectPipeline* PostProcess::effectPipeline(const std::string& path)
{
    EffectPipeline& e = effectPipelines_[path];
    const uint64_t now = SDL_GetTicksNS();
    if (now - e.checkedNs < 500'000'000ull && (e.pipeline || e.reported))
        return e.pipeline ? &e : nullptr;
    e.checkedNs = now;
    std::error_code ec;
    const int64_t stamp = int64_t(std::filesystem::last_write_time(path, ec).time_since_epoch().count());
    if (stamp == e.stamp && (e.pipeline || e.reported))
        return e.pipeline ? &e : nullptr;
    e.stamp = stamp;
    ShaderFile file;
    std::string error;
    const ShaderFile::Entry* entry = loadShaderFile(path, file, error) ? file.entry() : nullptr;
    SDL_GPUComputePipeline* made = entry && entry->blob.stage == ShaderStage::Compute && gpu_ ? gpu_->createComputePipeline(entry->blob) : nullptr;
    if (!made) {
        if (!e.reported)
            FX_WARN("post: %s: %s", path.c_str(), !error.empty() ? error.c_str() : "not a compute shader, or its pipeline failed");
        e.reported = true;
        return e.pipeline ? &e : nullptr;  // (the last good one, if any)
    }
    if (e.pipeline)
        SDL_ReleaseGPUComputePipeline(device_, e.pipeline);
    e.pipeline = made;
    e.samplers = std::min(entry->blob.numSamplers, 2u);
    e.reported = false;
    return &e;
}

void PostProcess::runEffects(SDL_GPUCommandBuffer* cmd, bool ldr, SDL_GPUTexture*& color, SDL_GPUTexture* const targets[2],
                             SDL_GPUTexture* depth, const ViewReconstructGpu& view)
{
    struct FalconFrame {
        float size[4], time[4], proj[4];
    };
    for (const EffectRequest& r : effects_) {
        if (r.ldr != ldr)
            continue;
        const EffectPipeline* p = effectPipeline(r.shader);
        if (!p)
            continue;
        SDL_GPUTexture* target = color == targets[0] ? targets[1] : targets[0];
        std::vector<uint8_t> params = r.params;
        params.resize(std::max<size_t>(params.size(), 16));
        SDL_PushGPUComputeUniformData(cmd, 0, params.data(), uint32_t(params.size()));
        const FalconFrame f = {{float(width_), float(height_), 1.0f / float(width_), 1.0f / float(height_)},
                               {effectTime_[0], effectTime_[1], effectTime_[2], 0.0f},
                               {view.proj[0], view.proj[1], view.proj[2], 0.0f}};
        SDL_PushGPUComputeUniformData(cmd, 1, &f, sizeof(f));
        SDL_GPUStorageTextureReadWriteBinding out = {target, 0, 0, false};
        gpuprof::pass(cmd, "post effect");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, p->pipeline);
        const SDL_GPUTextureSamplerBinding in[2] = {{color, linear_}, {depth, point_}};
        if (p->samplers)
            SDL_BindGPUComputeSamplers(pass, 0, in, p->samplers);
        dispatch2d(pass, width_, height_);
        SDL_EndGPUComputePass(pass);
        color = target;
    }
}

void PostProcess::shutdown()
{
    if (!device_)
        return;
    for (auto& [path, e] : effectPipelines_)
        if (e.pipeline)
            SDL_ReleaseGPUComputePipeline(device_, e.pipeline);
    effectPipelines_.clear();
    for (SDL_GPUTexture*& t : effectHdr_)
        gpumem::releaseTexture(device_, std::exchange(t, nullptr));
    for (SDL_GPUComputePipeline* p :
         {aoPass_, aoBlurPass_, taaPass_, histogramPass_, exposurePass_, bloomDownPass_, bloomUpPass_, tonemapPass_, fxaaPass_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    resizeRender(0, 0);
    resizeDisplay(0, 0);
    SDL_ReleaseGPUSampler(device_, point_);
    SDL_ReleaseGPUSampler(device_, linear_);
    gpumem::releaseTexture(device_, noReactive_);
    noReactive_ = nullptr;
    noReactiveCleared_ = false;
    gpumem::releaseBuffer(device_, exposureState_);
    device_ = nullptr;
}

bool PostProcess::resizeRender(uint32_t width, uint32_t height)
{
    if (width == renderW_ && height == renderH_ && velocity_)
        return true;
    gpumem::releaseTexture(device_, ao_);
    gpumem::releaseTexture(device_, aoRaw_);
    gpumem::releaseTexture(device_, velocity_);
    ao_ = aoRaw_ = velocity_ = nullptr;
    renderW_ = width;
    renderH_ = height;
    historyValid_ = false;
    if (!width || !height)
        return true;
    const auto rw = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    createAoTargets(aoScale_);
    velocity_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, width, height, rw, "velocity");
    return ao_ && aoRaw_ && velocity_;
}

bool PostProcess::resizeDisplay(uint32_t width, uint32_t height)
{
    if (width == width_ && height == height_ && output_)
        return true;
    gpumem::releaseTexture(device_, output_);
    gpumem::releaseTexture(device_, fxaaOutput_);
    for (auto* t : history_)
        gpumem::releaseTexture(device_, t);
    for (uint32_t i = 0; i < kBloomLevels; ++i) {
        gpumem::releaseTexture(device_, bloomDown_[i]);
        gpumem::releaseTexture(device_, bloomUp_[i]);
        bloomDown_[i] = bloomUp_[i] = nullptr;
    }
    for (SDL_GPUTexture*& t : effectHdr_)
        gpumem::releaseTexture(device_, std::exchange(t, nullptr));
    output_ = fxaaOutput_ = history_[0] = history_[1] = final_ = nullptr;
    fxaaApplied_ = false;
    width_ = width;
    height_ = height;
    historyValid_ = false;
    if (!width || !height)
        return true;

    const auto rw = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    output_ = createTexture(device_, kOutputFormat, width, height, rw, "post output");
    fxaaOutput_ = createTexture(device_, kOutputFormat, width, height, rw, "fxaa output");
    history_[0] = createTexture(device_, kHdrFormat, width, height, rw, "taa history 0");
    history_[1] = createTexture(device_, kHdrFormat, width, height, rw, "taa history 1");
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < kBloomLevels; ++i) {
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
        bloomW_[i] = w;
        bloomH_[i] = h;
        bloomDown_[i] = createTexture(device_, kHdrFormat, w, h, rw, "bloom down");
        bloomUp_[i] = createTexture(device_, kHdrFormat, w, h, rw, "bloom up");
    }
    return output_ && fxaaOutput_ && history_[0] && history_[1] && bloomDown_[kBloomLevels - 1] && bloomUp_[kBloomLevels - 1];
}

bool PostProcess::createAoTargets(uint32_t scale)
{
    gpumem::releaseTexture(device_, ao_);
    gpumem::releaseTexture(device_, aoRaw_);
    aoScale_ = scale;
    const uint32_t w = (renderW_ + scale - 1) / scale, h = (renderH_ + scale - 1) / scale;
    const auto rw = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    const bool half = scale > 1;
    ao_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, w, h, rw,
                        half ? "ambient occlusion (half res)" : "ambient occlusion");
    aoRaw_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, w, h, rw,
                           half ? "ambient occlusion (half res, raw)" : "ambient occlusion (raw)");
    return ao_ && aoRaw_;
}

void PostProcess::renderAo(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, const ViewReconstructGpu& view,
                           const PostSettings& settings, const uint32_t offset[2])
{
    FX_ZONE_CAT("PostProcess::renderAo", Rendering);
    const uint32_t scale = settings.aoHalfRes ? 2u : 1u;
    if (scale != aoScale_ && !createAoTargets(scale))
        return;
    const uint32_t w = (renderW_ + scale - 1) / scale, h = (renderH_ + scale - 1) / scale;
    AoUniforms u = {};
    u.view = view;
    u.params[0] = settings.aoRadius;
    u.params[1] = settings.ao == AoMode::Off ? 0.0f : settings.aoPower;  // power 0 => AO 1
    u.params[2] = 96.0f * float(renderH_) / float(std::max(height_, 1u));  // 96 display pixels
    u.mode[0] = settings.ao == AoMode::Ssao ? 1u : 0u;
    u.mode[1] = scale;
    u.mode[2] = offset[0];
    u.mode[3] = offset[1];
    // With TAA the noise rotates every frame and the history integrates it. Without, a static
    // pattern (no flicker) is denoised spatially into ao_.
    const bool denoise = !settings.taa && settings.ao != AoMode::Off;
    if (!settings.taa)
        u.view.screen[2] = 0;
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    SDL_GPUStorageTextureReadWriteBinding out = {denoise ? aoRaw_ : ao_, 0, 0, false};
    FX_GPU_SECTION(cmd, "Ambient Occlusion");
    gpuprof::pass(cmd, settings.ao == AoMode::Ssao ? "ssao" : "gtao");
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, aoPass_);
    const SDL_GPUTextureSamplerBinding d = {depth, point_};
    SDL_BindGPUComputeSamplers(pass, 0, &d, 1);
    dispatch2d(pass, w, h);
    SDL_EndGPUComputePass(pass);
    if (!denoise)
        return;
    const uint32_t blurSize[4] = {w, h, 0, 0};
    SDL_PushGPUComputeUniformData(cmd, 0, blurSize, sizeof(blurSize));
    SDL_GPUStorageTextureReadWriteBinding blurOut = {ao_, 0, 0, false};
    FX_GPU_PASS(cmd, "ao denoise");
    pass = SDL_BeginGPUComputePass(cmd, &blurOut, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, aoBlurPass_);
    const SDL_GPUTextureSamplerBinding input = {aoRaw_, point_};
    SDL_BindGPUComputeSamplers(pass, 0, &input, 1);
    dispatch2d(pass, w, h);
    SDL_EndGPUComputePass(pass);
}

void PostProcess::measureExposure(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, uint32_t width, uint32_t height,
                                  const PostSettings& settings)
{
    ExposureUniforms eu = {};
    eu.range[0] = kMinLogLuminance;
    eu.range[1] = 1.0f / (kMaxLogLuminance - kMinLogLuminance);
    eu.range[2] = std::clamp(settings.exposureBlend, 0.0f, 1.0f);
    eu.range[3] = settings.exposureCompensation;
    eu.screen[0] = width;
    eu.screen[1] = height;
    // Daylight's exposure maps that grey ground to 0.18 (as auto exposure maps the average).
    if (settings.daylight > 0.0f && settings.exposureLiftKnee >= 0.0f)
        eu.limit[0] = std::exp2(settings.exposureLiftKnee + settings.exposureCompensation) * 0.18f / settings.daylight;
    SDL_PushGPUComputeUniformData(cmd, 0, &eu, sizeof(eu));
    SDL_GPUStorageBufferReadWriteBinding state = {exposureState_, false};
    FX_GPU_PASS(cmd, "luminance histogram");
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &state, 1);
    SDL_BindGPUComputePipeline(pass, histogramPass_);
    const SDL_GPUTextureSamplerBinding in = {hdr, point_};
    SDL_BindGPUComputeSamplers(pass, 0, &in, 1);
    SDL_DispatchGPUCompute(pass, (width + 15) / 16, (height + 15) / 16, 1);
    SDL_EndGPUComputePass(pass);
    FX_GPU_PASS(cmd, "exposure");
    pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &state, 1);
    SDL_BindGPUComputePipeline(pass, exposurePass_);
    SDL_DispatchGPUCompute(pass, 1, 1, 1);
    SDL_EndGPUComputePass(pass);
    exposureMeasured_ = true;
}

void PostProcess::renderPost(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, SDL_GPUTexture* depth,
                             const ViewReconstructGpu& view, const float4x4& viewToPrevClip, const PostSettings& settings,
                             SDL_GPUTexture* upscaled, SDL_GPUTexture* reactive)
{
    if (!noReactiveCleared_ && noReactive_) {  // (once: a texture's contents start undefined)
        SDL_GPUColorTargetInfo t = {};
        t.texture = noReactive_;
        t.load_op = SDL_GPU_LOADOP_CLEAR;
        t.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &t, 1, nullptr));
        noReactiveCleared_ = true;
    }
    FX_ZONE_CAT("PostProcess::renderPost", Rendering);
    static const char* const kBloomDown[] = {"bloom down 0", "bloom down 1", "bloom down 2", "bloom down 3",
                                             "bloom down 4", "bloom down 5", "bloom down 6", "bloom down 7"};
    static const char* const kBloomUp[] = {"bloom up 0", "bloom up 1", "bloom up 2", "bloom up 3",
                                           "bloom up 4", "bloom up 5", "bloom up 6", "bloom up 7"};
    FX_GPU_SECTION(cmd, "Post");
    auto computePass = [&](const char* name, SDL_GPUComputePipeline* p, SDL_GPUTexture* target) {
        SDL_GPUStorageTextureReadWriteBinding out = {target, 0, 0, false};
        gpuprof::pass(cmd, name);
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, p);
        return pass;
    };

    const bool measured = exposureMeasured_;
    exposureMeasured_ = false;

    // 1. TAA (TAAU while upscaling) into the next history texture, which is also the
    //    anti-aliased display-sized frame; or the frame an external upscaler made.
    SDL_GPUTexture* color = hdr;
    if (upscaled) {
        color = upscaled;
        historyValid_ = false;  // our history pauses; it restarts when TAA takes over again
    } else if (settings.taa && !settings.passthrough) {
        SDL_GPUTexture* prev = history_[historyIndex_];
        SDL_GPUTexture* next = history_[historyIndex_ ^ 1];
        TaaUniforms u = {};
        u.view = view;
        u.viewToPrevClip = viewToPrevClip;
        u.params[0] = 0.1f;
        u.params[1] = historyValid_ ? 0.0f : 1.0f;
        u.params[2] = reactive ? 1.0f : 0.0f;
        u.display[0] = width_;
        u.display[1] = height_;
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPUComputePass* pass = computePass(width_ != renderW_ || height_ != renderH_ ? "taau" : "taa", taaPass_, next);
        const SDL_GPUTextureSamplerBinding inputs[5] = {{hdr, point_}, {prev, linear_}, {depth, point_}, {velocity_, point_},
                                                        {reactive ? reactive : noReactive_, linear_}};
        SDL_BindGPUComputeSamplers(pass, 0, inputs, 5);
        dispatch2d(pass, width_, height_);
        SDL_EndGPUComputePass(pass);
        historyIndex_ ^= 1;
        historyValid_ = true;
        color = next;
    } else {
        historyValid_ = false;
    }

    // 1b. The project's post effects on the HDR frame (R3), into two textures of their own.
    if (!settings.passthrough && std::any_of(effects_.begin(), effects_.end(), [](const EffectRequest& r) { return !r.ldr; })) {
        if (!effectHdr_[0]) {
            const auto rw = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            effectHdr_[0] = createTexture(device_, kHdrFormat, width_, height_, rw, "post effects 0");
            effectHdr_[1] = createTexture(device_, kHdrFormat, width_, height_, rw, "post effects 1");
        }
        runEffects(cmd, false, color, effectHdr_, depth, view);
    }

    if (!settings.passthrough) {
        // 2. Auto exposure from the anti-aliased HDR frame (no readback: the tonemapper reads it),
        //    unless measureExposure already ran on the render-sized frame.
        if (!measured)
            measureExposure(cmd, color, width_, height_, settings);
        SDL_GPUComputePass* pass = nullptr;

        // 3. Bloom: downsample chain, then upsample-accumulate back to the first level.
        for (uint32_t i = 0; i < kBloomLevels; ++i) {
            SDL_GPUTexture* src = i == 0 ? color : bloomDown_[i - 1];
            const float sw = float(i == 0 ? width_ : bloomW_[i - 1]), sh = float(i == 0 ? height_ : bloomH_[i - 1]);
            const BloomUniforms bu = {{1.0f / sw, 1.0f / sh, float(bloomW_[i]), float(bloomH_[i])}, {i == 0 ? 1u : 0u, 0, 0, 0}};
            SDL_PushGPUComputeUniformData(cmd, 0, &bu, sizeof(bu));
            pass = computePass(kBloomDown[std::min(i, 7u)], bloomDownPass_, bloomDown_[i]);
            const SDL_GPUTextureSamplerBinding b = {src, linear_};  // one binding per texture per pass
            SDL_BindGPUComputeSamplers(pass, 0, &b, 1);
            dispatch2d(pass, bloomW_[i], bloomH_[i]);
            SDL_EndGPUComputePass(pass);
        }
        for (int32_t i = int32_t(kBloomLevels) - 2; i >= 0; --i) {
            SDL_GPUTexture* coarse = i == int32_t(kBloomLevels) - 2 ? bloomDown_[i + 1] : bloomUp_[i + 1];
            const BloomUniforms bu = {{1.0f / float(bloomW_[i + 1]), 1.0f / float(bloomH_[i + 1]), float(bloomW_[i]),
                                       float(bloomH_[i])},
                                      {0, 0, 0, 0}};
            SDL_PushGPUComputeUniformData(cmd, 0, &bu, sizeof(bu));
            pass = computePass(kBloomUp[std::min(i, 7)], bloomUpPass_, bloomUp_[i]);
            const SDL_GPUTextureSamplerBinding b[2] = {{coarse, linear_}, {bloomDown_[i], linear_}};
            SDL_BindGPUComputeSamplers(pass, 0, b, 2);
            dispatch2d(pass, bloomW_[i], bloomH_[i]);
            SDL_EndGPUComputePass(pass);
        }
    }

    // 4. Exposure + bloom + AgX into the display target (debug views pass straight through).
    TonemapUniforms tu = {};
    tu.params[0] = settings.bloomIntensity / float(kBloomLevels);  // the up chain sums every level
    tu.params[1] = settings.manualExposure;
    tu.params[2] = settings.lookPower;
    tu.params[3] = settings.lookSaturation;
    tu.screen[0] = width_;
    tu.screen[1] = height_;
    tu.screen[2] = settings.passthrough ? 1u : 0u;
    SDL_PushGPUComputeUniformData(cmd, 0, &tu, sizeof(tu));
    SDL_GPUComputePass* pass = computePass("tonemap (AgX)", tonemapPass_, output_);
    const SDL_GPUTextureSamplerBinding in[2] = {{color, point_}, {bloomUp_[0], linear_}};
    SDL_BindGPUComputeSamplers(pass, 0, in, 2);
    SDL_BindGPUComputeStorageBuffers(pass, 0, &exposureState_, 1);
    dispatch2d(pass, width_, height_);
    SDL_EndGPUComputePass(pass);

    // 4b. The project's post effects on the display frame (R3), between the two display textures.
    SDL_GPUTexture* const ldr[2] = {output_, fxaaOutput_};
    SDL_GPUTexture* frame = output_;
    if (!settings.passthrough)
        runEffects(cmd, true, frame, ldr, depth, view);

    // 5. FXAA on the display-encoded frame (before the editor draws its grid and gizmos on it).
    fxaaApplied_ = settings.fxaa && !settings.passthrough;
    if (fxaaApplied_) {
        SDL_GPUTexture* target = frame == output_ ? fxaaOutput_ : output_;
        const FxaaUniforms fu = {{1.0f / float(width_), 1.0f / float(height_), 0, 0}, {width_, height_, 0, 0}};
        SDL_PushGPUComputeUniformData(cmd, 0, &fu, sizeof(fu));
        pass = computePass("fxaa", fxaaPass_, target);
        const SDL_GPUTextureSamplerBinding in = {frame, linear_};
        SDL_BindGPUComputeSamplers(pass, 0, &in, 1);
        dispatch2d(pass, width_, height_);
        SDL_EndGPUComputePass(pass);
        frame = target;
    }
    final_ = frame;
}

}  // namespace fx
