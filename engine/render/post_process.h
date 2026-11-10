#pragma once

#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;

enum class AoMode : uint32_t { Gtao, Ssao, Off };

// Upscaling: the scene renders at renderScale x the display size and an upscaler brings it to
// the display. Off: native resolution (TAA, FXAA and MSAA as set). Taau: our temporal
// upscaler (the TAA pass), both backends. Fsr: AMD FSR (FSR 4 where supported, else 3.1),
// D3D12 with the AMD DLLs. Dlss: NVIDIA DLSS (DLAA at a render scale of 1), D3D12 with the
// DLSS plugin on an NVIDIA RTX GPU. Either falls back to Taau when it can't run.
enum class Upscaler : uint32_t { Off, Taau, Fsr, Dlss };

struct PostSettings {
    bool taa = false;
    bool fxaa = false;    // FXAA on the tonemapped frame
    uint32_t msaa = 1;    // samples per pixel for edge coverage (GpuDrivenRenderer; 1 = off)
    Upscaler upscaler = Upscaler::Off;
    float renderScale = 1.0f;  // render / display size while upscaling (presets: kUpscalePresets)
    float sharpness = 0.5f;    // FSR's RCAS, 0-1
    AoMode ao = AoMode::Gtao;
    float aoRadius = 0.5f;          // world units
    float aoPower = 1.5f;
    bool aoHalfRes = true;          // one AO sample per 2x2 pixels, upsampled by the resolve
    float bloomIntensity = 0.12f;  // share of the (level-averaged) bloom added to the frame
    float exposureCompensation = 0.0f;  // EV
    float manualExposure = 0.0f;        // > 0 overrides auto exposure
    float exposureBlend = 0.05f;        // per-frame adaptation (1 = instant)
    // Auto exposure brightens fully up to this many EV above daylight's, past it at half the rate
    // (< 0: no limit): a room lit only through its door was lifted to the outside's mid grey, the
    // opening far over and its bloom a veil across the walls. A hard limit at 2.5 EV left Sponza's
    // arcades near black. `daylight` is set by the renderer from the sun: the luminance of a grey
    // (0.18) ground in it, sun and sky (0: no sun, no limit).
    float exposureLiftKnee = 2.0f;
    float daylight = 0.0f;
    // AgX look (ASC CDL on the display-encoded image, as in the reference AgX): power > 1
    // deepens midtones and shadows, saturation restores the colour AgX's curve washes out.
    float lookPower = 1.35f;            // 1 = AgX base (flat)
    float lookSaturation = 1.4f;
    bool passthrough = false;           // debug views: no TAA / exposure / tonemap
};

// Upscaling quality presets (AMD's ratios): render scale per preset, Native AA first.
inline constexpr float kUpscalePresets[5] = {1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 1.0f / 2.0f, 1.0f / 3.0f};
inline constexpr const char* kUpscalePresetNames[5] = {"native", "quality", "balanced", "performance", "ultra"};

// Mirrors ViewReconstruct in engine/shaders/post_common.slang.
struct ViewReconstructGpu {
    float proj[4];    // P00, P11, zNear
    float jitter[4];  // NDC jitter of this frame
    uint32_t screen[4];
};

// Sample offset inside each 2x2 pixel block for the half-resolution ambient passes (AO, GI).
// With TAA it walks the block every 4 frames (inside the 8-frame jitter period, so converged
// frames stay deterministic); without TAA it stays put.
inline void ambientSampleOffset(uint64_t frame, bool taa, uint32_t out[2])
{
    static const uint32_t kWalk[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const uint32_t i = taa ? uint32_t(frame & 3) : 0u;
    out[0] = kWalk[i][0];
    out[1] = kWalk[i][1];
}

// M5 post chain: GTAO/SSAO (before shading) and TAA / TAAU -> auto exposure -> bloom -> AgX ->
// FXAA (after shading). All compute; history and exposure live on the GPU. AO and velocity are
// render-sized; the history and everything after it are display-sized (they differ while
// upscaling).

class PostProcess {
public:
    static constexpr SDL_GPUTextureFormat kHdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    static constexpr SDL_GPUTextureFormat kOutputFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    static constexpr uint32_t kBloomLevels = 6;

    bool init(const GpuDevice& gpu);
    void shutdown();
    bool resizeRender(uint32_t width, uint32_t height);   // AO, velocity
    bool resizeDisplay(uint32_t width, uint32_t height);  // history, bloom, output

    // AO at full or half resolution (settings.aoHalfRes; `offset` from ambientSampleOffset).
    void renderAo(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, const ViewReconstructGpu& view,
                  const PostSettings& settings, const uint32_t offset[2]);
    // `viewToPrevClip`: current view space -> previous frame's unjittered clip space. `hdr`,
    // `depth` and `view` are render-sized; with settings.taa (forced on while upscaling) the
    // TAA pass upscales to the display size. `upscaled`: the frame an external upscaler (FSR, DLSS)
    // already brought to the display size, which then replaces the TAA pass.
    // `reactive`: a render-sized R8 mask of what TAA should trust the current frame for (particles);
    // nullptr: none.
    void renderPost(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, SDL_GPUTexture* depth, const ViewReconstructGpu& view,
                    const float4x4& viewToPrevClip, const PostSettings& settings, SDL_GPUTexture* upscaled = nullptr,
                    SDL_GPUTexture* reactive = nullptr);
    // While upscaling, exposure is measured on the render-sized HDR frame before the upscaler
    // (FSR takes it as an input). renderPost then skips its own measurement.
    void measureExposure(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* hdr, uint32_t width, uint32_t height,
                         const PostSettings& settings);
    SDL_GPUBuffer* exposureState() const { return exposureState_; }  // float 256: the exposure multiplier

    SDL_GPUTexture* ao() const { return ao_; }  // RG16F: AO, view depth; aoScale() pixels per texel
    uint32_t aoScale() const { return aoScale_; }
    SDL_GPUTexture* velocity() const { return velocity_; }  // written by the resolve (RG16F, pixels)
    SDL_GPUSampler* pointSampler() const { return point_; }
    SDL_GPUTexture* output() const { return final_ ? final_ : output_; }  // display-ready RGBA8

    // (R3) A project's post effects this frame, in order (scripts' PostEffect): a compute shader each
    // (a .fshader: render/shader_file.h, on engine/shaders/include/falcon_post.slang) on the HDR frame
    // before exposure, bloom and the tonemap, or (`ldr`) on the display frame before FXAA, with its
    // own uniform block (slot 0) and the frame's (slot 1). Pipelines are made when first used, and
    // again when their file changes.
    struct EffectRequest {
        std::string shader;  // the .fshader's path
        bool ldr = false;
        std::vector<uint8_t> params;
    };
    void setEffects(std::vector<EffectRequest> effects, float time, float delta, uint64_t frame);
    void invalidateHistory() { historyValid_ = false; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline *aoPass_ = nullptr, *taaPass_ = nullptr, *histogramPass_ = nullptr, *exposurePass_ = nullptr,
                           *bloomDownPass_ = nullptr, *bloomUpPass_ = nullptr, *tonemapPass_ = nullptr,
                           *aoBlurPass_ = nullptr, *fxaaPass_ = nullptr;
    SDL_GPUSampler *point_ = nullptr, *linear_ = nullptr;
    SDL_GPUTexture* ao_ = nullptr;
    SDL_GPUTexture* aoRaw_ = nullptr;  // without TAA: the noisy AO, denoised into ao_
    uint32_t aoScale_ = 2;
    SDL_GPUTexture* velocity_ = nullptr;
    SDL_GPUTexture* history_[2] = {};
    SDL_GPUTexture* noReactive_ = nullptr;  // 1x1, 0: TAA's reactive mask when there is none
    bool noReactiveCleared_ = false;
    SDL_GPUTexture* bloomDown_[kBloomLevels] = {};
    SDL_GPUTexture* bloomUp_[kBloomLevels] = {};
    uint32_t bloomW_[kBloomLevels] = {}, bloomH_[kBloomLevels] = {};
    SDL_GPUTexture* output_ = nullptr;
    SDL_GPUTexture* fxaaOutput_ = nullptr;
    bool fxaaApplied_ = false;  // this frame's output is fxaaOutput_
    SDL_GPUBuffer* exposureState_ = nullptr;
    uint32_t width_ = 0, height_ = 0, historyIndex_ = 0;  // display size
    uint32_t renderW_ = 0, renderH_ = 0;
    bool historyValid_ = false;
    bool exposureMeasured_ = false;  // this frame, by measureExposure

    bool createAoTargets(uint32_t scale);

    // (R3) Project post effects.
    const GpuDevice* gpu_ = nullptr;
    struct EffectPipeline {
        SDL_GPUComputePipeline* pipeline = nullptr;
        uint32_t samplers = 0;
        int64_t stamp = 0;       // the file's time when made
        uint64_t checkedNs = 0;  // the file last looked at
        bool reported = false;   // a failure logged (once per change)
    };
    std::unordered_map<std::string, EffectPipeline> effectPipelines_;
    std::vector<EffectRequest> effects_;
    float effectTime_[4] = {};
    SDL_GPUTexture* effectHdr_[2] = {};  // the HDR effects' ping-pong (display-sized)
    SDL_GPUTexture* final_ = nullptr;    // this frame's display-ready frame
    const EffectPipeline* effectPipeline(const std::string& path);
    // The effects of one stage, `color` to the next texture each (from `targets`, never `color`).
    void runEffects(SDL_GPUCommandBuffer* cmd, bool ldr, SDL_GPUTexture*& color, SDL_GPUTexture* const targets[2],
                    SDL_GPUTexture* depth, const ViewReconstructGpu& view);
};

}  // namespace fx
