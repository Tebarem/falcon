#include "gpu/device.h"

#include "core/boot.h"
#include "core/log.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan_core.h>  // SDL's bundled Khronos headers; only for SDL_GPUVulkanOptions

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace fx {

namespace {

struct ShaderCode {
    const uint8_t* code;
    size_t size;
    const char* entry;
};

// The blob's code for the backend's format.
ShaderCode codeFor(const ShaderBlob& blob, SDL_GPUShaderFormat format)
{
    switch (format) {
    case SDL_GPU_SHADERFORMAT_DXIL: return {blob.dxil, blob.dxilSize, blob.dxilEntry};
    case SDL_GPU_SHADERFORMAT_MSL: return {blob.msl, blob.mslSize, blob.mslEntry};
    default: return {blob.spirv, blob.spirvSize, "main"};
    }
}

const char* presentModeName(SDL_GPUPresentMode m)
{
    switch (m) {
    case SDL_GPU_PRESENTMODE_VSYNC: return "vsync";
    case SDL_GPU_PRESENTMODE_IMMEDIATE: return "immediate";
    case SDL_GPU_PRESENTMODE_MAILBOX: return "mailbox";
    }
    return "?";
}

#if defined(_WIN32)
// warmUp's device (released once SDL has made its own; at exit when it never did).
struct WarmDevice {
    std::thread thread;
    ID3D12Device* device = nullptr;
    void finish()
    {
        if (thread.joinable())
            thread.join();
    }
    void release()
    {
        finish();
        if (device)
            device->Release();
        device = nullptr;
    }
    ~WarmDevice() { release(); }
} warm;

// The adapter SDL's D3D12 backend picks (high performance) and its feature level.
void makeWarmDevice()
{
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    const auto createDevice = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (createDevice && createFactory && SUCCEEDED(createFactory(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                      reinterpret_cast<void**>(&adapter))) &&
        SUCCEEDED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&warm.device))))
        boot::mark("gpu warm device (worker)");
    if (adapter)
        adapter->Release();
    if (factory)
        factory->Release();
    // (the libraries stay loaded: SDL loads them next)
}
#else
// D3D12 only: nothing to warm up elsewhere.
struct WarmDevice {
    void finish() {}
    void release() {}
} warm;
#endif

}  // namespace

void GpuDevice::warmUp([[maybe_unused]] const GpuConfig& config)
{
#if defined(_WIN32)
    // Not for Vulkan, nor with the debug layer (SDL turns it on first; a device made before
    // that would be removed).
    char hint[32] = {};
    GetEnvironmentVariableA("SDL_GPU_DRIVER", hint, DWORD(sizeof(hint)));
    const char* driver = hint[0] ? hint : config.driver;
    if (config.debug || (driver && SDL_strcasecmp(driver, "direct3d12") != 0) || warm.thread.joinable())
        return;
    warm.thread = std::thread(makeWarmDevice);
#endif
}

void GpuDevice::beginCreate(const GpuConfig& config)
{
    config_ = config;
    creator_ = std::thread([this] {
        // Vulkan: shaders are SPIR-V 1.5 (needs a 1.2 instance), and the visibility buffer
        // reads SV_PrimitiveID in the fragment stage, which Vulkan gates behind geometryShader.
        // Ignored by the D3D12 backend.
        VkPhysicalDeviceFeatures vkFeatures = {};
        vkFeatures.geometryShader = VK_TRUE;
        SDL_GPUVulkanOptions vkOptions = {};
        vkOptions.vulkan_api_version = VK_API_VERSION_1_2;
        vkOptions.vulkan_10_physical_device_features = &vkFeatures;

        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vkOptions);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, config_.debug);
        SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, false);
        if (config_.driver)
            SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, config_.driver);
        warm.finish();
        device_ = SDL_CreateGPUDeviceWithProperties(props);
        SDL_DestroyProperties(props);
        warm.release();
        if (!device_)
            FX_ERROR("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        else
            boot::mark("gpu device created (worker)");
        created_.store(true, std::memory_order_release);
    });
}

bool GpuDevice::attachWindow(SDL_Window* window)
{
    if (creator_.joinable())
        creator_.join();
    if (!device_)
        return false;
    driverName_ = SDL_GetGPUDeviceDriver(device_);
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    format_ = (formats & SDL_GPU_SHADERFORMAT_DXIL)  ? SDL_GPU_SHADERFORMAT_DXIL
              : (formats & SDL_GPU_SHADERFORMAT_MSL) ? SDL_GPU_SHADERFORMAT_MSL
                                                     : SDL_GPU_SHADERFORMAT_SPIRV;
    const GpuConfig& config = config_;

    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        FX_ERROR("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    window_ = window;
    probeCaps();

    const SDL_GPUPresentMode mode = setPresentMode(config.presentMode);
    SDL_SetGPUAllowedFramesInFlight(device_, SDL_clamp(config.framesInFlight, 1u, 3u));

    FX_INFO("gpu: driver=%s shaders=%s present=%s frames-in-flight=%u", driverName_,
            format_ == SDL_GPU_SHADERFORMAT_DXIL ? "dxil" : format_ == SDL_GPU_SHADERFORMAT_MSL ? "msl" : "spirv", presentModeName(mode),
            SDL_clamp(config.framesInFlight, 1u, 3u));
    FX_INFO("gpu caps: r32uint-rt=%d r32uint-rw=%d rg32uint-rt=%d bc7=%d bc5=%d d32=%d mailbox=%d",
            caps_.r32uintColorTarget, caps_.r32uintStorageRW, caps_.rg32uintColorTarget, caps_.bc7, caps_.bc5,
            caps_.d32Depth, caps_.mailbox);
    return true;
}

SDL_GPUPresentMode GpuDevice::setPresentMode(SDL_GPUPresentMode mode)
{
    // Immediate -> mailbox -> vsync (always supported).
    if (mode == SDL_GPU_PRESENTMODE_IMMEDIATE && !SDL_WindowSupportsGPUPresentMode(device_, window_, mode))
        mode = SDL_GPU_PRESENTMODE_MAILBOX;
    if (mode == SDL_GPU_PRESENTMODE_MAILBOX && !SDL_WindowSupportsGPUPresentMode(device_, window_, mode))
        mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
        presentMode_ = mode;
    return presentMode_;
}

void GpuDevice::probeCaps()
{
    auto supports = [&](SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage) {
        return SDL_GPUTextureSupportsFormat(device_, f, SDL_GPU_TEXTURETYPE_2D, usage);
    };
    caps_.r32uintColorTarget = supports(SDL_GPU_TEXTUREFORMAT_R32_UINT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    caps_.r32uintStorageRW = supports(SDL_GPU_TEXTUREFORMAT_R32_UINT, SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
    caps_.rg32uintColorTarget = supports(SDL_GPU_TEXTUREFORMAT_R32G32_UINT, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    caps_.bc7 = supports(SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    caps_.bc5 = supports(SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    caps_.d32Depth = supports(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    caps_.mailbox = SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX);
}

void GpuDevice::shutdown()
{
    if (creator_.joinable())
        creator_.join();
    if (!device_)
        return;
    SDL_WaitForGPUIdle(device_);
    if (window_)
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    window_ = nullptr;
}

SDL_GPUTextureFormat GpuDevice::swapchainFormat() const
{
    return SDL_GetGPUSwapchainTextureFormat(device_, window_);
}

SDL_GPUShader* GpuDevice::createShader(const ShaderBlob& blob) const
{
    SDL_GPUShaderCreateInfo info = {};
    const ShaderCode code = codeFor(blob, format_);
    if (!code.size) {
        FX_ERROR("shader %s: no code for this GPU backend (compile it again)", blob.name);
        return nullptr;
    }
    info.code = code.code;
    info.code_size = code.size;
    info.entrypoint = code.entry;
    info.format = format_;
    info.stage = blob.stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = blob.numSamplers;
    info.num_storage_textures = blob.numStorageTextures;
    info.num_storage_buffers = blob.numStorageBuffers;
    info.num_uniform_buffers = blob.numUniformBuffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader)
        FX_ERROR("shader %s: %s", blob.name, SDL_GetError());
    return shader;
}

SDL_GPUComputePipeline* GpuDevice::createComputePipeline(const ShaderBlob& blob) const
{
    SDL_GPUComputePipelineCreateInfo info = {};
    const ShaderCode code = codeFor(blob, format_);
    if (!code.size) {
        FX_ERROR("compute pipeline %s: no code for this GPU backend (compile it again)", blob.name);
        return nullptr;
    }
    info.code = code.code;
    info.code_size = code.size;
    info.entrypoint = code.entry;
    info.format = format_;
    info.num_samplers = blob.numSamplers;
    info.num_readonly_storage_textures = blob.numStorageTextures;
    info.num_readonly_storage_buffers = blob.numStorageBuffers;
    info.num_readwrite_storage_textures = blob.numRwStorageTextures;
    info.num_readwrite_storage_buffers = blob.numRwStorageBuffers;
    info.num_uniform_buffers = blob.numUniformBuffers;
    info.threadcount_x = blob.threadsX;
    info.threadcount_y = blob.threadsY;
    info.threadcount_z = blob.threadsZ;
    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
    if (!pipeline)
        FX_ERROR("compute pipeline %s: %s", blob.name, SDL_GetError());
    return pipeline;
}

}  // namespace fx
