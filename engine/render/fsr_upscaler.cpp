#include "render/fsr_upscaler.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"

#include "exposure_texture.shader.h"

#include <SDL3/SDL.h>

#if FALCON_FSR
#include <SDL3/SDL_falcon_native.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <dx12/ffx_api_dx12.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#endif

#include <cmath>
#include <cstring>

namespace fx {

#if FALCON_FSR

// The FSR API's five entry points, from the loader DLL (no import library: falcon.exe starts
// without the DLLs, and FSR is then just unavailable).
struct FsrUpscaler::Api {
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

namespace {

// bin/fsr/<name> next to falcon.exe.
HMODULE loadFsrDll(const wchar_t* name)
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(MAX_PATH * 4));
    wchar_t* slash = length && length < MAX_PATH * 4 ? std::wcsrchr(path, L'\\') : nullptr;
    if (!slash)
        return nullptr;
    const size_t room = size_t(MAX_PATH * 4) - size_t(slash + 1 - path);
    if (swprintf_s(slash + 1, room, L"fsr\\%s", name) < 0)
        return nullptr;
    return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// SDL keeps every texture in one D3D12 state outside passes; FSR's descriptor of it.
uint32_t ffxState(UINT d3d12State)
{
    if (d3d12State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    if ((d3d12State & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (d3d12State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (d3d12State & D3D12_RESOURCE_STATE_RENDER_TARGET)
        return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    if (d3d12State & D3D12_RESOURCE_STATE_DEPTH_WRITE)
        return FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
    return FFX_API_RESOURCE_STATE_COMMON;
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

void fsrMessage(uint32_t type, const wchar_t* message)
{
    char text[512];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text, int(sizeof(text)), nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        FX_ERROR("fsr: %s", text);
    else
        FX_WARN("fsr: %s", text);
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

bool FsrUpscaler::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    available_ = false;
    if (std::strcmp(SDL_GetGPUDeviceDriver(device_), "direct3d12") != 0) {
        reason_ = "FSR needs Direct3D 12";
        return false;
    }
    d3d12Device_ = SDL_FalconD3D12Device(device_);
    // The upscaler DLL first: the loader then finds it already loaded (both live in bin/fsr/).
    upscalerModule_ = loadFsrDll(L"amd_fidelityfx_upscaler_dx12.dll");
    loader_ = loadFsrDll(L"amd_fidelityfx_loader_dx12.dll");
    if (!d3d12Device_ || !upscalerModule_ || !loader_) {
        reason_ = d3d12Device_ ? "the AMD FSR DLLs are missing (bin/fsr/; scripts/fetch-fsr.ps1)" : "no Direct3D 12 device";
        return false;
    }
    api_ = new Api();
    const HMODULE loader = HMODULE(loader_);
    api_->createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader, "ffxCreateContext"));
    api_->destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader, "ffxDestroyContext"));
    api_->query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader, "ffxQuery"));
    api_->dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader, "ffxDispatch"));
    if (!api_->createContext || !api_->destroyContext || !api_->query || !api_->dispatch) {
        reason_ = "the AMD FSR loader DLL has no FSR API";
        return false;
    }
    exposurePass_ = gpu.createComputePipeline(shaders::exposure_texture_cs_exposure_texture);
    exposure_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 1, 1, "fsr exposure");
    if (!exposurePass_ || !exposure_) {
        reason_ = "FSR exposure texture";
        return false;
    }
    available_ = true;
    return true;
}

void FsrUpscaler::destroyContext()
{
    if (context_) {
        SDL_WaitForGPUIdle(device_);  // FSR's resources may be in flight
        api_->destroyContext(reinterpret_cast<ffxContext*>(&context_), nullptr);
        context_ = nullptr;
    }
    gpumem::releaseTexture(device_, output_);
    output_ = nullptr;
    contextWidth_ = contextHeight_ = 0;
}

void FsrUpscaler::shutdown()
{
    if (!device_)
        return;
    if (api_)
        destroyContext();
    gpumem::releaseTexture(device_, exposure_);
    exposure_ = nullptr;
    if (exposurePass_)
        SDL_ReleaseGPUComputePipeline(device_, exposurePass_);
    exposurePass_ = nullptr;
    delete api_;
    api_ = nullptr;
    // The DLLs stay loaded until exit (FSR may keep threads or callbacks around).
    available_ = false;
    device_ = nullptr;
}

bool FsrUpscaler::ensureContext(uint32_t displayWidth, uint32_t displayHeight)
{
    if (context_ && displayWidth == contextWidth_ && displayHeight == contextHeight_)
        return true;
    destroyContext();
    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    desc.maxRenderSize = {displayWidth, displayHeight};  // any render scale up to native
    desc.maxUpscaleSize = {displayWidth, displayHeight};
    desc.fpMessage = fsrMessage;
    ffxCreateContextDescUpscaleVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = static_cast<ID3D12Device*>(d3d12Device_);
    desc.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    const ffxReturnCode_t rc = api_->createContext(reinterpret_cast<ffxContext*>(&context_), &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        FX_ERROR("fsr: context creation failed (%u)", rc);
        context_ = nullptr;
        return false;
    }
    output_ = createTexture(device_, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, displayWidth, displayHeight, "fsr output");
    if (!output_) {
        destroyContext();
        return false;
    }
    contextWidth_ = displayWidth;
    contextHeight_ = displayHeight;
    // Which provider the API picked (FSR 4 on supported GPUs).
    ffxQueryGetProviderVersion provider = {};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api_->query(reinterpret_cast<ffxContext*>(&context_), &provider.header) == FFX_API_RETURN_OK && provider.versionName)
        version_ = std::string("FSR ") + provider.versionName;
    else
        version_ = "FSR";
    FX_INFO("fsr: %s, up to %ux%u", version_.c_str(), displayWidth, displayHeight);
    return true;
}

SDL_GPUTexture* FsrUpscaler::dispatch(SDL_GPUCommandBuffer* cmd, const Inputs& in)
{
    FX_ZONE_CAT("FsrUpscaler::dispatch", Rendering);
    if (!available_ || failed_ || !ensureContext(in.displayWidth, in.displayHeight)) {
        failed_ = available_;  // a context that failed once stays failed (no retry every frame)
        return nullptr;
    }
    // The exposure the tonemapper will use, as FSR's 1x1 input.
    {
        const float params[4] = {in.manualExposure, 0, 0, 0};
        SDL_PushGPUComputeUniformData(cmd, 0, params, sizeof(params));
        const SDL_GPUStorageTextureReadWriteBinding out = {exposure_, 0, 0, false};
        gpuprof::pass(cmd, "fsr exposure");
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, exposurePass_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &in.exposureState, 1);
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }

    gpuprof::pass(cmd, "fsr");
    auto* list = static_cast<ID3D12GraphicsCommandList*>(SDL_FalconBeginD3D12Commands(cmd));
    if (!list) {
        FX_ERROR("fsr: no command list: %s", SDL_GetError());
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
    const Native composition = in.composition ? native(in.composition) : Native{};
    const Native reactive = in.reactive ? native(in.reactive) : Native{};
    if (!color.resource || !depth.resource || !velocity.resource || !exposure.resource || !output.resource) {
        SDL_FalconEndD3D12Commands(cmd);
        FX_ERROR("fsr: no native textures: %s", SDL_GetError());
        failed_ = true;
        return nullptr;
    }
    // Inputs stay where SDL keeps them (shader resources); the output becomes a UAV for FSR.
    barrier(list, output.resource, D3D12_RESOURCE_STATES(output.state), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = ffxApiGetResourceDX12(color.resource, ffxState(color.state));
    d.depth = ffxApiGetResourceDX12(depth.resource, ffxState(depth.state));
    d.motionVectors = ffxApiGetResourceDX12(velocity.resource, ffxState(velocity.state));
    d.exposure = ffxApiGetResourceDX12(exposure.resource, ffxState(exposure.state));
    // Screen-space reflections: their content moves unlike the surface (less history there).
    if (composition.resource)
        d.transparencyAndComposition = ffxApiGetResourceDX12(composition.resource, ffxState(composition.state));
    // Particles: drawn over surfaces they don't move with (less history where they are).
    if (reactive.resource)
        d.reactive = ffxApiGetResourceDX12(reactive.resource, ffxState(reactive.state));
    d.output = ffxApiGetResourceDX12(output.resource, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {in.jitterPixels.x, in.jitterPixels.y};
    d.motionVectorScale = {1.0f, 1.0f};  // already render pixels, current -> previous
    d.renderSize = {in.renderWidth, in.renderHeight};
    d.upscaleSize = {in.displayWidth, in.displayHeight};
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = in.sharpness;
    d.frameTimeDelta = in.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.zNear;
    d.cameraFar = 1e6f;  // infinite far plane (DEPTH_INFINITE)
    d.cameraFovAngleVertical = in.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    const ffxReturnCode_t rc = api_->dispatch(reinterpret_cast<ffxContext*>(&context_), &d.header);

    barrier(list, output.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATES(output.state));
    SDL_FalconEndD3D12Commands(cmd);
    if (rc != FFX_API_RETURN_OK) {
        FX_ERROR("fsr: dispatch failed (%u)", rc);
        failed_ = true;
        return nullptr;
    }
    return output_;
}

#else  // !FALCON_FSR

struct FsrUpscaler::Api {};

bool FsrUpscaler::init(const GpuDevice&)
{
    reason_ = "this build has no AMD FSR (scripts/fetch-fsr.ps1, then reconfigure)";
    return false;
}
void FsrUpscaler::shutdown() {}
bool FsrUpscaler::ensureContext(uint32_t, uint32_t) { return false; }
void FsrUpscaler::destroyContext() {}
SDL_GPUTexture* FsrUpscaler::dispatch(SDL_GPUCommandBuffer*, const Inputs&) { return nullptr; }

#endif

}  // namespace fx
