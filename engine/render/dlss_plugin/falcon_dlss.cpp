// falcon_dlss.dll: NVIDIA DLSS Super Resolution (and DLAA) through NGX on Direct3D 12, behind the
// small C interface in dlss_plugin_api.h. This is the only place NVIDIA's SDK is linked (the static
// NGX library, cmake/FalconDlss.cmake); falcon.exe loads this DLL at runtime when it is there.
// NGX finds nvngx_dlss.dll in the plugin's directory (bin/dlss/).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>

#include <nvsdk_ngx_helpers_d3d.h>

#include "dlss_plugin_api.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#define FALCON_DLSS_EXPORT extern "C" __declspec(dllexport)

namespace {

// Falcon's NGX project id (a custom engine without an NVIDIA application id).
constexpr const char* kProjectId = "6f1d2b8e-5a43-4c9b-9e0d-3f7a1c52b8d4";
constexpr const char* kEngineVersion = "1.0";

ID3D12Device* g_device = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;  // NGX's capability parameters (also used to create)

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    const char* mode = "";
};

void setReason(char* reason, uint32_t size, const char* format, ...)
{
    if (!reason || size == 0)
        return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(reason, size, format, args);
    va_end(args);
}

// NGX's own description of a result (UTF-16) as UTF-8.
void resultText(NVSDK_NGX_Result result, char* out, int size)
{
    const wchar_t* text = GetNGXResultAsString(result);
    if (!text || !WideCharToMultiByte(CP_UTF8, 0, text, -1, out, size, nullptr, nullptr))
        std::snprintf(out, size_t(size), "0x%08x", unsigned(result));
}

// The quality modes and their nominal render scales (render / display per axis).
struct Mode {
    NVSDK_NGX_PerfQuality_Value value;
    const char* name;
    float scale;
};
constexpr Mode kModes[] = {
    {NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA", 1.0f},
    {NVSDK_NGX_PerfQuality_Value_UltraQuality, "Ultra Quality", 0.77f},
    {NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality", 0.667f},
    {NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced", 0.58f},
    {NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance", 0.5f},
    {NVSDK_NGX_PerfQuality_Value_UltraPerformance, "Ultra Performance", 0.333f},
};

// The render-size range DLSS takes for `mode` at this display size (dynamic resolution).
bool modeRange(NVSDK_NGX_PerfQuality_Value mode, uint32_t displayWidth, uint32_t displayHeight, uint32_t& minW,
               uint32_t& minH, uint32_t& maxW, uint32_t& maxH)
{
    void* callback = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_params, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
    if (!callback)
        return false;
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_Width, displayWidth);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_Height, displayHeight);
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_PerfQualityValue, mode);
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_RTXValue, 0);
    if (NVSDK_NGX_FAILED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(g_params)))
        return false;
    uint32_t w = 0, h = 0;
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_OutWidth, &w);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_OutHeight, &h);
    if (w == 0 || h == 0)
        return false;  // the mode is off on this GPU or driver
    minW = maxW = w;
    minH = maxH = h;
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &minW);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &minH);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &maxW);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &maxH);
    return true;
}

// The mode whose range holds the render size, nearest to its scale first; failing that, the
// nearest one.
const Mode& pickMode(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight)
{
    const float scale = float(renderWidth) / float(displayWidth);
    const Mode* order[sizeof(kModes) / sizeof(kModes[0])];
    const size_t count = sizeof(kModes) / sizeof(kModes[0]);
    for (size_t i = 0; i < count; ++i)
        order[i] = &kModes[i];
    for (size_t i = 1; i < count; ++i)  // insertion sort by distance to `scale`
        for (size_t j = i; j > 0 && std::fabs(order[j]->scale - scale) < std::fabs(order[j - 1]->scale - scale); --j) {
            const Mode* t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (size_t i = 0; i < count; ++i) {
        uint32_t minW, minH, maxW, maxH;
        if (modeRange(order[i]->value, displayWidth, displayHeight, minW, minH, maxW, maxH) && renderWidth >= minW &&
            renderWidth <= maxW && renderHeight >= minH && renderHeight <= maxH)
            return *order[i];
    }
    return *order[0];
}

}  // namespace

FALCON_DLSS_EXPORT int falconDlssVersion(void)
{
    return FALCON_DLSS_PLUGIN_VERSION;
}

FALCON_DLSS_EXPORT int falconDlssInit(void* d3d12Device, const wchar_t* dllDirectory, char* reason, uint32_t reasonSize)
{
    if (g_device) {
        setReason(reason, reasonSize, "DLSS is already initialised");
        return 0;
    }
    auto* device = static_cast<ID3D12Device*>(d3d12Device);
    if (!device) {
        setReason(reason, reasonSize, "no Direct3D 12 device");
        return 0;
    }
    // NGX writes its logs under the application data path.
    wchar_t dataPath[MAX_PATH + 32] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, dataPath);
    if (tempLength == 0 || tempLength >= MAX_PATH)
        wcscpy_s(dataPath, L".\\");
    wcscat_s(dataPath, L"falcon_ngx\\");
    CreateDirectoryW(dataPath, nullptr);

    const wchar_t* paths[1] = {dllDirectory};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = dllDirectory ? 1 : 0;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    char text[256];
    NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                                  dataPath, device, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result)) {
        resultText(result, text, int(sizeof(text)));
        setReason(reason, reasonSize, "NGX did not start (%s): an NVIDIA RTX GPU and driver are needed", text);
        return 0;
    }
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(result) || !g_params) {
        resultText(result, text, int(sizeof(text)));
        setReason(reason, reasonSize, "NGX has no capability parameters (%s)", text);
        NVSDK_NGX_D3D12_Shutdown1(device);
        g_params = nullptr;
        return 0;
    }
    int needsDriver = 0;
    unsigned int minMajor = 0, minMinor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver)) &&
        needsDriver) {
        NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        setReason(reason, reasonSize, "DLSS needs NVIDIA driver %u.%u or newer", minMajor, minMinor);
        NVSDK_NGX_D3D12_DestroyParameters(g_params);
        NVSDK_NGX_D3D12_Shutdown1(device);
        g_params = nullptr;
        return 0;
    }
    int available = 0;
    result = NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_FAILED(result) || !available) {
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        resultText(NVSDK_NGX_Result(initResult), text, int(sizeof(text)));
        setReason(reason, reasonSize, "DLSS is not available on this GPU (%s)", text);
        NVSDK_NGX_D3D12_DestroyParameters(g_params);
        NVSDK_NGX_D3D12_Shutdown1(device);
        g_params = nullptr;
        return 0;
    }
    g_device = device;
    return 1;
}

FALCON_DLSS_EXPORT void* falconDlssCreate(void* commandList, uint32_t renderWidth, uint32_t renderHeight,
                                          uint32_t displayWidth, uint32_t displayHeight, int autoExposure, char* reason,
                                          uint32_t reasonSize)
{
    if (!g_device || !g_params) {
        setReason(reason, reasonSize, "DLSS is not initialised");
        return nullptr;
    }
    const Mode& mode = pickMode(renderWidth, renderHeight, displayWidth, displayHeight);
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = renderWidth;
    create.Feature.InHeight = renderHeight;
    create.Feature.InTargetWidth = displayWidth;
    create.Feature.InTargetHeight = displayHeight;
    create.Feature.InPerfQualityValue = mode.value;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  (autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    auto* feature = new Feature();
    feature->mode = mode.name;
    const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(commandList), 1, 1,
                                                              &feature->handle, g_params, &create);
    if (NVSDK_NGX_FAILED(result) || !feature->handle) {
        char text[256];
        resultText(result, text, int(sizeof(text)));
        setReason(reason, reasonSize, "DLSS %s for %ux%u to %ux%u was not created (%s)", mode.name, renderWidth, renderHeight,
                  displayWidth, displayHeight, text);
        delete feature;
        return nullptr;
    }
    return feature;
}

FALCON_DLSS_EXPORT const char* falconDlssModeName(void* feature)
{
    return feature ? static_cast<Feature*>(feature)->mode : "";
}

FALCON_DLSS_EXPORT int falconDlssEvaluate(void* feature, void* commandList, const FalconDlssEvaluate* in, char* reason,
                                          uint32_t reasonSize)
{
    auto* f = static_cast<Feature*>(feature);
    if (!f || !f->handle || !in || !g_params) {
        setReason(reason, reasonSize, "no DLSS feature");
        return 0;
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = static_cast<ID3D12Resource*>(in->color);
    eval.Feature.pInOutput = static_cast<ID3D12Resource*>(in->output);
    eval.pInDepth = static_cast<ID3D12Resource*>(in->depth);
    eval.pInMotionVectors = static_cast<ID3D12Resource*>(in->motion);
    eval.pInExposureTexture = static_cast<ID3D12Resource*>(in->exposure);
    eval.pInBiasCurrentColorMask = static_cast<ID3D12Resource*>(in->bias);
    eval.InJitterOffsetX = in->jitterX;
    eval.InJitterOffsetY = in->jitterY;
    eval.InRenderSubrectDimensions.Width = in->renderWidth;
    eval.InRenderSubrectDimensions.Height = in->renderHeight;
    eval.InReset = in->reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;  // already render pixels, current -> previous
    eval.InMVScaleY = 1.0f;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    eval.InFrameTimeDeltaInMsec = in->frameTimeMs;
    const NVSDK_NGX_Result result =
        NGX_D3D12_EVALUATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(commandList), f->handle, g_params, &eval);
    if (NVSDK_NGX_FAILED(result)) {
        char text[256];
        resultText(result, text, int(sizeof(text)));
        setReason(reason, reasonSize, "DLSS evaluation failed (%s)", text);
        return 0;
    }
    return 1;
}

FALCON_DLSS_EXPORT void falconDlssRelease(void* feature)
{
    auto* f = static_cast<Feature*>(feature);
    if (!f)
        return;
    if (f->handle && g_device)
        NVSDK_NGX_D3D12_ReleaseFeature(f->handle);
    delete f;
}

FALCON_DLSS_EXPORT void falconDlssShutdown(void)
{
    if (!g_device)
        return;
    if (g_params)
        NVSDK_NGX_D3D12_DestroyParameters(g_params);
    g_params = nullptr;
    NVSDK_NGX_D3D12_Shutdown1(g_device);
    g_device = nullptr;
}
