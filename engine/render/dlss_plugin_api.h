#pragma once

// The C interface of the DLSS plugin, bin/dlss/falcon_dlss.dll (engine/render/dlss_plugin/).
// The plugin holds every piece of NVIDIA's DLSS SDK (the static NGX library); falcon.exe only
// loads it at runtime through these entry points (engine/render/dlss_upscaler.cpp), so a
// build or a machine without it still runs. Direct3D 12 only: every pointer below is a
// D3D12 object, and the caller records into its own command list.

#include <stdint.h>

#define FALCON_DLSS_PLUGIN_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FalconDlssEvaluate {
    // ID3D12Resource *s. color, depth, motion, exposure, bias: readable by compute shaders
    // (NON_PIXEL_SHADER_RESOURCE); output: UNORDERED_ACCESS.
    void* color;     // render-sized HDR
    void* depth;     // render-sized, reverse Z
    void* motion;    // render-sized, unjittered motion to the previous frame in render pixels
    void* exposure;  // 1x1 R32F: the exposure the tonemapper applies (NULL: DLSS measures it)
    void* bias;      // optional: favour the current frame (r 0..1; particles), or NULL
    void* output;    // display-sized
    float jitterX, jitterY;  // render pixels, as the raster was jittered (x right, y down)
    uint32_t renderWidth, renderHeight;
    float frameTimeMs;
    int reset;  // camera cut: no history
} FalconDlssEvaluate;

// FALCON_DLSS_PLUGIN_VERSION of the plugin (a mismatched one is not used).
typedef int (*PfnFalconDlssVersion)(void);
// NGX on `d3d12Device` (ID3D12Device*), finding nvngx_dlss.dll in `dllDirectory`. 1: DLSS Super
// Resolution runs on this device; 0: `reason` (UTF-8) says why not.
typedef int (*PfnFalconDlssInit)(void* d3d12Device, const wchar_t* dllDirectory, char* reason, uint32_t reasonSize);
// A DLSS feature for render -> display upscaling (DLAA when they match), recorded into
// `commandList` (ID3D12GraphicsCommandList*). NULL on failure, with `reason`.
// `autoExposure`: no exposure texture is coming.
typedef void* (*PfnFalconDlssCreate)(void* commandList, uint32_t renderWidth, uint32_t renderHeight,
                                     uint32_t displayWidth, uint32_t displayHeight, int autoExposure,
                                     char* reason, uint32_t reasonSize);
// The quality mode a feature was created with ("DLAA", "Quality", ...).
typedef const char* (*PfnFalconDlssModeName)(void* feature);
// Records the upscale. 1: done; 0: failed (`reason`).
typedef int (*PfnFalconDlssEvaluateFn)(void* feature, void* commandList, const FalconDlssEvaluate* in, char* reason,
                                       uint32_t reasonSize);
// After the GPU is done with it.
typedef void (*PfnFalconDlssRelease)(void* feature);
typedef void (*PfnFalconDlssShutdown)(void);

#ifdef __cplusplus
}
#endif
