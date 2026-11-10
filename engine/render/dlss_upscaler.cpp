#include "render/dlss_upscaler.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/dlss_plugin_api.h"

#include "exposure_texture.shader.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <SDL3/SDL_falcon_native.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#endif

#include <cstring>

namespace fx {

#if defined(_WIN32)

// The plugin's entry points (dlss_plugin_api.h).
struct DlssUpscaler::Api {
    PfnFalconDlssInit init = nullptr;
    PfnFalconDlssCreate create = nullptr;
    PfnFalconDlssModeName modeName = nullptr;
    PfnFalconDlssEvaluateFn evaluate = nullptr;
    PfnFalconDlssRelease release = nullptr;
    PfnFalconDlssShutdown shutdown = nullptr;
};

namespace {

// bin/dlss/ next to falcon.exe, with a trailing backslash.
std::wstring dlssDirectory()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(MAX_PATH * 4));
    wchar_t* slash = length && length < MAX_PATH * 4 ? std::wcsrchr(path, L'\\') : nullptr;
    if (!slash)
        return {};
    slash[1] = 0;
    return std::wstring(path) + L"dlss\\";
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// DLSS reads its inputs from compute shaders: the state SDL keeps a texture in, or one that
// compute shaders can read.
D3D12_RESOURCE_STATES readable(UINT state)
{
    if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        return D3D12_RESOURCE_STATES(state);
    return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

SDL_GPUTexture* createTexture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, uint32_t w, uint32_t h, const char* name)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* t = gpumem::createTexture("Post", device, &info);
    if (t)
        gpumem::setTextureName(device, t, name);
    return t;
}

}  // namespace

void DlssUpscaler::init(const GpuDevice& gpu)
{
    gpu_ = &gpu;
    device_ = gpu.get();
    available_ = false;
    tried_ = false;
}

bool DlssUpscaler::prepare()
{
    if (!tried_ && device_) {
        tried_ = true;
        if (!load())
            FX_INFO("dlss: unavailable: %s", reason_.c_str());
    }
    return available_;
}

bool DlssUpscaler::load()
{
    const GpuDevice& gpu = *gpu_;
    if (std::strcmp(SDL_GetGPUDeviceDriver(device_), "direct3d12") != 0) {
        reason_ = "DLSS needs Direct3D 12";
        return false;
    }
    void* d3d12Device = SDL_FalconD3D12Device(device_);
    if (!d3d12Device) {
        reason_ = "no Direct3D 12 device";
        return false;
    }
    const std::wstring directory = dlssDirectory();
    const std::wstring pluginPath = directory + L"falcon_dlss.dll";
    if (GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        reason_ = "this build has no DLSS plugin (bin/dlss/; scripts/fetch-dlss.ps1, then reconfigure)";
        return false;
    }
    plugin_ = LoadLibraryExW(pluginPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!plugin_) {
        reason_ = "the DLSS plugin (bin/dlss/falcon_dlss.dll) did not load";
        return false;
    }
    const HMODULE plugin = HMODULE(plugin_);
    const auto version = reinterpret_cast<PfnFalconDlssVersion>(GetProcAddress(plugin, "falconDlssVersion"));
    if (!version || version() != FALCON_DLSS_PLUGIN_VERSION) {
        reason_ = "the DLSS plugin is from another build";
        return false;
    }
    api_ = new Api();
    api_->init = reinterpret_cast<PfnFalconDlssInit>(GetProcAddress(plugin, "falconDlssInit"));
    api_->create = reinterpret_cast<PfnFalconDlssCreate>(GetProcAddress(plugin, "falconDlssCreate"));
    api_->modeName = reinterpret_cast<PfnFalconDlssModeName>(GetProcAddress(plugin, "falconDlssModeName"));
    api_->evaluate = reinterpret_cast<PfnFalconDlssEvaluateFn>(GetProcAddress(plugin, "falconDlssEvaluate"));
    api_->release = reinterpret_cast<PfnFalconDlssRelease>(GetProcAddress(plugin, "falconDlssRelease"));
    api_->shutdown = reinterpret_cast<PfnFalconDlssShutdown>(GetProcAddress(plugin, "falconDlssShutdown"));
    if (!api_->init || !api_->create || !api_->modeName || !api_->evaluate || !api_->release || !api_->shutdown) {
        reason_ = "the DLSS plugin is incomplete";
        return false;
    }
    char reason[256] = {};
    if (!api_->init(d3d12Device, directory.c_str(), reason, sizeof(reason))) {
        reason_ = reason;
        return false;
    }
    initialised_ = true;
    exposurePass_ = gpu.createComputePipeline(shaders::exposure_texture_cs_exposure_texture);
    exposure_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, "dlss exposure");
    if (!exposurePass_ || !exposure_) {
        reason_ = "DLSS exposure texture";
        return false;
    }
    available_ = true;
    FX_INFO("dlss: available");
    return true;
}

void DlssUpscaler::destroyFeature()
{
    if (feature_) {
        SDL_WaitForGPUIdle(device_);  // DLSS's resources may be in flight
        api_->release(feature_);
        feature_ = nullptr;
    }
    gpumem::releaseTexture(device_, output_);
    output_ = nullptr;
    renderWidth_ = renderHeight_ = displayWidth_ = displayHeight_ = 0;
}

void DlssUpscaler::shutdown()
{
    if (!device_)
        return;
    if (api_) {
        destroyFeature();
        if (initialised_)
            api_->shutdown();
    }
    initialised_ = false;
    gpumem::releaseTexture(device_, exposure_);
    exposure_ = nullptr;
    if (exposurePass_)
        SDL_ReleaseGPUComputePipeline(device_, exposurePass_);
    exposurePass_ = nullptr;
    delete api_;
    api_ = nullptr;
    // The plugin stays loaded until exit (NGX may keep threads around).
    available_ = false;
    device_ = nullptr;
    gpu_ = nullptr;
}

SDL_GPUTexture* DlssUpscaler::dispatch(SDL_GPUCommandBuffer* cmd, const Inputs& in)
{
    FX_ZONE_CAT("DlssUpscaler::dispatch", Rendering);
    if (!available_)
        return nullptr;
    const bool sizeChanged = !feature_ || in.renderWidth != renderWidth_ || in.renderHeight != renderHeight_ ||
                             in.displayWidth != displayWidth_ || in.displayHeight != displayHeight_;
    if (failed_ && !sizeChanged)
        return nullptr;  // failed at this size: no retry every frame
    if (sizeChanged) {
        destroyFeature();
        failed_ = false;
        output_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, in.displayWidth, in.displayHeight,
                                "dlss output");
        renderWidth_ = in.renderWidth;
        renderHeight_ = in.renderHeight;
        displayWidth_ = in.displayWidth;
        displayHeight_ = in.displayHeight;
        if (!output_) {
            failed_ = true;
            return nullptr;
        }
    }

    // The exposure the tonemapper will use, as DLSS's 1x1 input.
    {
        const float params[4] = {in.manualExposure, 0, 0, 0};
        SDL_PushGPUComputeUniformData(cmd, 0, params, sizeof(params));
        const SDL_GPUStorageTextureReadWriteBinding out = {exposure_, 0, 0, false};
        gpuprof::pass(cmd, "dlss exposure");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, exposurePass_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &in.exposureState, 1);
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }

    gpuprof::pass(cmd, "dlss");
    auto* list = static_cast<ID3D12GraphicsCommandList*>(SDL_FalconBeginD3D12Commands(cmd));
    if (!list) {
        FX_ERROR("dlss: no command list: %s", SDL_GetError());
        failed_ = true;
        return nullptr;
    }
    struct Native {
        ID3D12Resource* resource;
        Uint32 state;
    };
    auto native = [&](SDL_GPUTexture* t) {
        Native n = {};
        n.resource = static_cast<ID3D12Resource*>(SDL_FalconD3D12Texture(cmd, t, &n.state));
        return n;
    };
    const Native color = native(in.color), depth = native(in.depth), velocity = native(in.velocity),
                 exposure = native(exposure_), output = native(output_);
    const Native reactive = in.reactive ? native(in.reactive) : Native{};
    if (!color.resource || !depth.resource || !velocity.resource || !exposure.resource || !output.resource) {
        SDL_FalconEndD3D12Commands(cmd);
        FX_ERROR("dlss: no native textures: %s", SDL_GetError());
        failed_ = true;
        return nullptr;
    }

    char reason[256] = {};
    if (!feature_) {
        feature_ = api_->create(list, in.renderWidth, in.renderHeight, in.displayWidth, in.displayHeight, 0, reason,
                                sizeof(reason));
        if (!feature_) {
            SDL_FalconEndD3D12Commands(cmd);
            FX_ERROR("dlss: %s", reason);
            reason_ = reason;
            failed_ = true;
            return nullptr;
        }
        mode_ = api_->modeName(feature_);
        FX_INFO("dlss: %s, %ux%u to %ux%u", mode_.c_str(), in.renderWidth, in.renderHeight, in.displayWidth,
                in.displayHeight);
    }

    // Inputs readable by compute shaders, the output a UAV; all back where SDL keeps them after.
    const Native inputs[] = {color, depth, velocity, exposure, reactive};
    for (const Native& n : inputs)
        if (n.resource)
            barrier(list, n.resource, D3D12_RESOURCE_STATES(n.state), readable(n.state));
    barrier(list, output.resource, D3D12_RESOURCE_STATES(output.state), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    FalconDlssEvaluate e = {};
    e.color = color.resource;
    e.depth = depth.resource;
    e.motion = velocity.resource;
    e.exposure = exposure.resource;
    e.bias = reactive.resource;
    e.output = output.resource;
    e.jitterX = in.jitterPixels.x;
    e.jitterY = in.jitterPixels.y;
    e.renderWidth = in.renderWidth;
    e.renderHeight = in.renderHeight;
    e.frameTimeMs = in.frameTimeMs;
    e.reset = in.reset ? 1 : 0;
    const int ok = api_->evaluate(feature_, list, &e, reason, sizeof(reason));

    barrier(list, output.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATES(output.state));
    for (const Native& n : inputs)
        if (n.resource)
            barrier(list, n.resource, readable(n.state), D3D12_RESOURCE_STATES(n.state));
    SDL_FalconEndD3D12Commands(cmd);
    if (!ok) {
        FX_ERROR("dlss: %s", reason);
        reason_ = reason;
        failed_ = true;
        return nullptr;
    }
    return output_;
}

#else  // DLSS runs on Direct3D 12 only

struct DlssUpscaler::Api {};

void DlssUpscaler::init(const GpuDevice& gpu)
{
    gpu_ = &gpu;
    device_ = gpu.get();
    reason_ = "NVIDIA DLSS runs on Direct3D 12 (Windows) only";
}
bool DlssUpscaler::prepare() { return false; }
bool DlssUpscaler::load() { return false; }
void DlssUpscaler::destroyFeature() {}
void DlssUpscaler::shutdown()
{
    device_ = nullptr;
    gpu_ = nullptr;
}
SDL_GPUTexture* DlssUpscaler::dispatch(SDL_GPUCommandBuffer*, const Inputs&) { return nullptr; }

#endif

}  // namespace fx
