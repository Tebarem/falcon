#pragma once

#include "render/dlss_upscaler.h"
#include "render/fsr_upscaler.h"
#include "render/mesh_renderer.h"
#include "render/post_process.h"
#include "render/scatter_renderer.h"
#include "render/terrain_generator.h"
#include "render/live_geometry.h"
#include "render/terrain_renderer.h"
#include "render/terrain_surface.h"
#include "render/decal_renderer.h"
#include "render/surface_layers.h"
#include "render/particle_renderer.h"
#include "render/transparent_renderer.h"
#include "render/water_renderer.h"
#include "render/gi_settings.h"
#include "render/probe_gi.h"
#include "render/ray_tracing/scene_bvh.h"
#include "scene/fly_camera.h"
#include "scene/scene.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;
class ScenePak;
class VirtualTextureSystem;
class LightSystem;
class VirtualShadowMap;
class SkinningSystem;

enum class DebugView : uint32_t {
    Lit, Clusters, Normals, Instances, Triangles, LodLevel, OcclusionPass, Hiz, VtMip, Albedo, LightCount, Shadow,
    Indirect,     // the probe GI alone (bounce + sky)
    Reflections,  // what specular surfaces reflect (screen space, the BVH, sky)
    RayTraced,    // camera rays through the ray tracing BVH, coloured as Triangles (a check of the tracer)
    Probes,       // the probe GI's finest grid as spheres (their light; inactive grey, fresh dark red)
    Count
};

// Reflections (reflections.slang), the editor's Lighting panel.
struct ReflectionSettings {
    bool screenSpace = true;     // trace the screen (off: the trace through the BVH only)
    bool halfRes = true;         // one screen-space ray per 2x2 pixels (off: one per pixel)
    float intensity = 1.0f;      // multiplies what reflections add
    float maxRoughness = 0.55f;  // screen-space traced below this roughness (fading out toward it)
    float thickness = 1.0f;      // how far behind a surface a ray may pass and still hit it (times the default)
    uint32_t steps = 32;         // screen-space steps per ray, at most
};

// Water (render/water_renderer.h): what it costs, the editor's Lighting panel.
struct WaterSettings {
    bool enabled = true;          // water bodies drawn (off: not at all)
    bool reflectionsSeeWater = false;  // rebuild the reflection source after water (water reflects water)
    bool caustics = true;         // caustics and sun attenuation under water
    bool localLightSpecular = false;  // local lights' highlights on water
    bool ripples = true;          // live ripples and wakes
    float rippleExtent = 64.0f;   // metres covered around the camera
    uint32_t rippleResolution = 512;
    uint32_t fftSize = 256;       // ocean: FFT texels per cascade side
    uint32_t fftCascades = 3;
};

struct FrameSettings {
    DebugView debugView = DebugView::Lit;
    float lodPixels = 1.0f;      // max projected simplification error, in pixels
    float cullPixels = 4.0f;     // instances whose bounds project smaller than this (radius, pixels) are skipped
    bool occlusion = true;       // two-pass HiZ occlusion culling
    bool coneCulling = true;     // backface cone culling (skipped for double-sided materials)
    bool freezeCulling = false;  // keep culling/LOD from the camera at the moment of freezing
    bool forceRetest = false;    // debug: send every pass-1 cluster to the occlusion re-test
    bool alphaMask = true;       // alpha-tested materials (off: drawn opaque, for geometry tests)
    bool shadowCache = true;     // keep virtual shadow map pages across frames (off: re-render all)
    float shadowBudgetMs = 2.0f;  // GPU ms a frame for re-rendering moved lights' shadows (0: no cap)
    bool reflections = true;     // screen-space reflections, the BVH off screen (off: ambient only)
    ReflectionSettings reflect;
    float terrainPixels = 1.0f;  // terrain patch levels: max projected height error, in pixels
    float detailDistance = 0.0f;  // nearer than this (world units): full-detail meshes and terrain, nothing culled for its size
    uint32_t hizViewLevel = 3;
    float frameTimeMs = 1000.0f / 60.0f;  // since the last frame (FSR)
    float seconds = 0.0f;                 // the frame's time (surface shaders' SurfaceIn.time; R4)
    PostSettings post;
    GiSettings gi;
    WaterSettings water;
};

// GPU counters from a previous frame (read back asynchronously, never stalls).
struct GpuStats {
    uint32_t chunks, visible1, visible2, candidates, triangles1, triangles2, overflow, instances, masked1, masked2;
    uint32_t vsmAllocs, vsmFree, vsmDirty, vsmRecords, vsmLevels, vsmOverflow, vsmRequested;
    uint32_t vsmMovingRecords;  // of vsmRecords: moving casters' (drawn over the pages' static layer)
    uint32_t transparentCandidates, transparent;  // alpha-blend clusters: in the frustum, drawn (overflow bit 4)
    uint64_t frame;  // frame the numbers belong to
    bool valid;
};

// M2: GPU-driven visibility-buffer renderer. Per frame, entirely on the GPU:
// instance cull -> cluster DAG cut + frustum + cone + HiZ(prev) -> index expansion -> draw,
// HiZ build -> re-test of occluded clusters -> second draw, HiZ for next frame, resolve.
// Four indexed indirect draws total (opaque + alpha-masked, per occlusion pass); CPU work is
// uniforms plus pass recording. With MSAA (PostSettings::msaa) the draws are replayed at N
// samples per pixel after the resolve, for edge coverage (renderMsaa).
class GpuDrivenRenderer {
public:
    static constexpr SDL_GPUTextureFormat kColorFormat = PostProcess::kHdrFormat;  // lit HDR, before post
    static constexpr SDL_GPUTextureFormat kVisFormat = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    static constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    bool init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt, LightSystem& lights,
              VirtualShadowMap& vsm, SkinningSystem& skinning);
    void shutdown();
    // The terrain to draw (nullptr: none), through the scene's reserved terrain slots, and its
    // scene (material layer images and their cache). Shadows and GI grow to cover it.
    void setTerrain(const Terrain* terrain, const std::string& scenePath);
    bool terrainBuilding() { return terrainSurface_.building(); }  // layer textures still being made
    // The water bodies to draw (nullptr: none).
    void setWater(const WaterSystem* water) { water_.setSystem(water); }
    const WaterRenderer& water() const { return water_; }
    ParticleRenderer& particles() { return particles_; }
    DecalRenderer& decals() { return decals_; }
    const TerrainRenderStats& terrainStats() const { return terrain_.stats(); }
    bool isTerrainSlot(uint32_t instance) const { return terrain_.ownsSlot(instance); }
    // Terrain scatter: the slot pools reserved for it (before setTerrain), its instances.
    void setScatterPools(std::vector<ScatterPool> pools, uint32_t spareFirst = 0, uint32_t spareCount = 0)
    {
        scatter_.setPools(std::move(pools), spareFirst, spareCount);
    }
    ScatterRenderer& scatter() { return scatter_; }
    // Live meshes (level meshes, scripts' meshes) in the scene buffer's live arena.
    LiveGeometry& live() { return live_; }
    const LiveGeometry& live() const { return live_; }
    // The scene's ray tracing BVH (render/ray_tracing/scene_bvh.h).
    const SceneBvh& bvh() const { return bvh_; }
    const ProbeGi& probes() const { return probes_; }
    // Layered level meshes (render/surface_layers.h): their masks and records, and an instance's
    // layer (-1: none), which its streamed rows carry (kInstanceLayered, InstanceGpu::skin).
    SurfaceLayers& surfaceLayers() { return layers_; }
    void setInstanceLayer(uint32_t slot, int32_t layer);
    // An instance not drawn whatever its rows say (imported art drawn as a cut copy instead).
    void setInstanceHidden(uint32_t slot, bool hidden);
    // Terrain generation on the GPU (its own command buffers, any time).
    TerrainGenerator& terrainGenerator() { return generator_; }
    const TerrainGenerator& terrainGenerator() const { return generator_; }
    const ScatterRenderer& scatter() const { return scatter_; }
    // width x height: the display (sceneColor) size. While upscaling (settings.post.upscaler)
    // the scene renders at renderWidth() x renderHeight() and is upscaled to it.
    void render(SDL_GPUCommandBuffer* cmd, uint32_t width, uint32_t height, const CameraState& camera,
                const FrameSettings& settings);
    uint64_t frameIndex() const { return frame_; }  // frames rendered (or skipped): the texture cache's clock
    uint32_t renderWidth() const { return width_; }
    uint32_t renderHeight() const { return height_; }
    // Frames per jitter cycle (TAA: 8; upscaling: AMD's 8 x (display / render)^2) and the phase
    // of the last rendered frame: a converged still frame depends only on the phase.
    uint32_t jitterPhaseCount() const { return jitterPhases_; }
    uint32_t jitterPhase() const { return jitterPhase_; }
    // Which upscaler ran in the last frame and at what size ("" when not upscaling).
    const std::string& upscalerStatus() const { return upscalerStatus_; }
    // Instead of render() while the scene view is hidden (the editor's Animator tab): readbacks,
    // skinning (the Animator's preview reads it) and texture streaming carry on; everything that
    // belongs to the view (history, HiZ, shadow pages, exposure) waits as it was. Posed characters'
    // shadow invalidations stay queued for the next render(). Don't stream instances meanwhile.
    void skipFrame(SDL_GPUCommandBuffer* cmd);
    // Before render(): stream instance rows into the scene buffer (rows equal to the current
    // ones are skipped), keep last frame's rows for motion vectors, and invalidate the cached
    // shadow pages under each changed instance's old and new bounds.
    void streamInstances(SDL_GPUCommandBuffer* cmd, const std::vector<InstanceUpdate>& rows);
    // Hand over the fence of the submission that contains this frame (enables stats readback).
    void frameSubmitted(SDL_GPUFence* fence);

    SDL_GPUTexture* sceneColor() const { return post_.output(); }  // display-ready RGBA8
    const GpuStats& stats() const { return stats_; }
    // No shadow pages were (re)rendered in the latest frame the GPU reported on.
    bool shadowsSettled() const
    {
        return stats_.valid && stats_.frame + 8 > frame_ && (stats_.vsmDirty == 0 || !shadowCacheEnabled_);
    }
    bool shadowCacheEnabled_ = true;
    // Every GI probe asleep: the indirect light no longer changes (or there is no probe GI).
    bool giSettled() const { return probes_.settled(); }
    uint32_t maxVisibleClusters() const { return maxVisible_; }
    // Scatter thinned to fit the GPU's budgets: its smallest size on screen (px, 0: not thinned)
    // and its smallest shadow caster (shadow texels, 1: not thinned).
    float scatterPixels() const { return scatterPixels_; }
    float scatterShadowTexels() const;
    // The MSAA sample count used for a requested one: the largest the GPU supports up to it
    // (1 = off).
    uint32_t msaaSamples(uint32_t requested) const;

    // Editor picking: the instance under a pixel of the next rendered frame (display
    // coordinates, mapped to the render size). The answer arrives a few frames later through pickResult(), which returns
    // true once per request: slot ~0u for sky. `cluster` and `triangle`: the scene cluster and the
    // triangle in it under the pixel (~0u when none: sky, a transparent surface).
    void requestPick(uint32_t x, uint32_t y);
    bool pickResult(uint32_t& slot, uint32_t* cluster = nullptr, uint32_t* triangle = nullptr);
    // This frame's targets, for editor overlays drawn after render() (render-sized: see
    // renderWidth / renderHeight).
    SDL_GPUTexture* depthTexture() const { return depth_; }
    SDL_GPUTexture* visibilityTexture() const { return visTexture_; }
    SDL_GPUBuffer* visibleClusters() const { return visible_; }
    SDL_GPUSampler* pointSampler() const { return pointSampler_; }

private:
    // The probe GI's atlas k (0 irradiance, 1 distance, 2 data) as passes sample it (without
    // probes: a stand-in, not read).
    SDL_GPUTextureSamplerBinding probeBinding(int k) const;
    bool ensureTargets(uint32_t width, uint32_t height);
    bool ensureMsaa(uint32_t samples);
    // Edge coverage at msaaSamples_ per pixel, resolved into colorAA_ (see msaa_edges.slang).
    void renderMsaa(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* scene, const float4x4& rasterViewProj, uint32_t width,
                    uint32_t height);
    void releaseMsaa();
    void buildHiz(SDL_GPUCommandBuffer* cmd);
    void pollStats();

    struct HizLevel {
        uint32_t offset, width, height;
    };
    static constexpr uint32_t kMaxHizLevels = 16;
    static constexpr uint32_t kReadbackSlots = 4;

    SDL_GPUDevice* device_ = nullptr;
    const GpuDevice* gpu_ = nullptr;
    const ScenePak* pak_ = nullptr;

    SDL_GPUComputePipeline *reset_ = nullptr, *instances_ = nullptr, *argsCull1_ = nullptr, *cullFirst_ = nullptr,
                           *argsDraw1_ = nullptr, *cullSecond_ = nullptr, *argsDraw2_ = nullptr, *expand_ = nullptr,
                           *hizDepth_ = nullptr, *hizDown_ = nullptr, *resolve_ = nullptr, *builtinResolve_ = nullptr, *projectResolve_ = nullptr, *reflect_ = nullptr, *reflectTrace_ = nullptr, *reflectFarPipe_ = nullptr, *reflectSourcePipe_ = nullptr, *viewHiz_ = nullptr, *rayView_ = nullptr, *probeView_ = nullptr,
                           *vtClear_ = nullptr, *pick_ = nullptr, *argsRaster1_ = nullptr, *argsRaster2_ = nullptr;
    VirtualTextureSystem* vt_ = nullptr;
    LightSystem* lights_ = nullptr;
    VirtualShadowMap* vsm_ = nullptr;
    SkinningSystem* skinning_ = nullptr;
    SDL_GPUGraphicsPipeline* visbuffer_ = nullptr;
    SDL_GPUGraphicsPipeline* visbufferMasked_ = nullptr;
    SDL_GPUSampler* pointSampler_ = nullptr;

    SDL_GPUBuffer *chunks_ = nullptr, *visible_ = nullptr, *candidates_ = nullptr, *counters_ = nullptr,
                  *args_ = nullptr, *indices_ = nullptr, *hiz_ = nullptr;
    SDL_GPUBuffer* triangleBase_ = nullptr;  // per visible slot: where its triangles start in its draw (cluster_expand)
    SDL_GPUTexture *visTexture_ = nullptr, *depth_ = nullptr, *color_ = nullptr, *reactive_ = nullptr;
    // Reflections: the resolve's specular weight + roughness and normal, a copy of its image with mips.
    SDL_GPUTexture *reflectWeight_ = nullptr, *reflectNormal_ = nullptr, *reflectSource_ = nullptr;
    SDL_GPUTexture* reflectMask_ = nullptr;  // FSR's transparency-and-composition mask (screen-space reflections)
    SDL_GPUTexture* reflectHits_ = nullptr;  // screen-space hits at half resolution (uv, mip, trust)
    SDL_GPUTexture* reflectFar_ = nullptr;   // the far reflections, traced through the BVH at the same resolution
    bool reflectMaskValid_ = false;
    SDL_GPUSampler* mipSampler_ = nullptr;  // trilinear, clamped (reflectSource_)
    uint32_t reflectLevels_ = 1;            // reflectSource_'s mips

    // MSAA: pipelines and targets for msaaSamples_, created on first use.
    bool msaaBroken_ = false;  // its pipelines failed: MSAA stays off
    uint32_t msaaSamples_ = 1, msaaWidth_ = 0, msaaHeight_ = 0;
    SDL_GPUGraphicsPipeline *msaaFill_ = nullptr, *msaaOpaque_ = nullptr, *msaaMasked_ = nullptr;
    SDL_GPUTexture *msaaColor_ = nullptr, *msaaDepth_ = nullptr, *colorAA_ = nullptr;

    uint32_t instanceCount_ = 0, maxChunks_ = 0, maxVisible_ = 0;
    float scatterPixels_ = 0.0f;  // scatter's smallest size on screen (px; 0: the settings'), while clusters overflow
    uint32_t scatterCalm_ = 0;    // readbacks well under the budget since the last change
    uint32_t width_ = 0, height_ = 0;  // render size
    uint32_t displayWidth_ = 0, displayHeight_ = 0;
    uint32_t jitterPhases_ = 8, jitterPhase_ = 0;
    std::string upscalerStatus_;
    HizLevel hizLevels_[kMaxHizLevels] = {};
    uint32_t hizLevelCount_ = 0;
    bool hizValid_ = false;
    float4x4 prevView_ = {};
    float4x4 prevViewProj_ = {};  // unjittered, for TAA reprojection
    bool havePrevViewProj_ = false;
    PostProcess post_;
    MeshRenderer* meshes_ = nullptr;  // (R5)

public:
    // (R4) A project's resolve (its surface shaders compiled in: a .fshader) instead of the
    // engine's while `path` is there; "" (or no file): the engine's again. Made again when called
    // after the file changed.
    void setProjectResolve(const std::string& path);
    // (T2) Both of the project's builds: the resolve and the transparent pass (each "" or missing:
    // the engine's).
    void setProjectShaders(const std::string& resolvePath, const std::string& transparentPath, const std::string& deformPath);
    // (R5) Runtime meshes (the process's): uploaded at the frame's start, drawn into the lit frame
    // before particles.
    void setMeshes(MeshRenderer* meshes) { meshes_ = meshes; }
    // (V1) A material's engine params (refraction, ior, vertex bound, vertex distance; nullptr: none),
    // for the vertex functions' culling bound and distance.
    void setMaterialParams(std::function<const float*(uint32_t)> engineParams) { engineParams_ = std::move(engineParams); }
    uint32_t deformedInstances() const { return uint32_t(deformOf_.size()); }
    uint32_t deformedVertices() const;
    // (R3) The project's post effects this frame (PostProcess::setEffects).
    void setPostEffects(std::vector<PostProcess::EffectRequest> effects, float time, float delta, uint64_t frame)
    {
        post_.setEffects(std::move(effects), time, delta, frame);
    }

private:
    SceneBvh bvh_;
    ProbeGi probes_;
    TerrainRenderer terrain_;
    LiveGeometry live_;
    SurfaceLayers layers_;
    std::unordered_map<uint32_t, uint32_t> layeredOf_;  // instance slot -> surface layer
    std::vector<uint32_t> layeredChanged_;             // slots whose layer changed (streamed again)
    std::unordered_set<uint32_t> hiddenSlots_;         // setInstanceHidden's (streamed again on change, too)
    std::unordered_map<uint32_t, bool> ownHidden_;     // whether their own rows hide them (back when unhidden)
    TerrainSurface terrainSurface_;
    WaterRenderer water_;
    ParticleRenderer particles_;
    TransparentRenderer transparent_;
    DecalRenderer decals_;
    ScatterRenderer scatter_;
    TerrainGenerator generator_;
    FsrUpscaler fsr_;
    DlssUpscaler dlss_;
    bool fsrLastFrame_ = false;  // FSR or DLSS made the previous frame (else its history restarts)
    uint32_t fsrRenderWidth_ = 0, fsrRenderHeight_ = 0;

    // Dynamic instances: CPU mirror of the GPU instance table, last frame's rows on the GPU.
    std::vector<fpak::InstanceGpu> mirror_;
    std::vector<uint32_t> lastStreamed_;
    // Moving casters (kInstanceDynamic): the stream frame each slot last changed, the flagged slots.
    std::vector<uint32_t> movedFrame_, movingSlots_;
    uint32_t streamFrame_ = 0;
    std::vector<float4> movedBounds_, skinnedBounds_;
    // (V1) Vertex functions: instances deformed (slot -> deform record), the candidates (their
    // material has one), what each mesh's clusters use, and when the candidates were last found.
    void updateDeformers(std::vector<InstanceUpdate>& rows);
    std::function<const float*(uint32_t)> engineParams_;
    std::unordered_map<uint32_t, uint32_t> deformOf_;
    std::vector<uint32_t> deformCandidates_;
    std::vector<std::vector<uint32_t>> meshVertex_;  // per mesh: its clusters' materials with a vertex function
    uint64_t deformSignature_ = ~0ull, deformScanFrame_ = 0;
    float3 lastCamera_ = {};
    SDL_GPUBuffer* prevInstances_ = nullptr;
    SDL_GPUTransferBuffer* instanceUpload_ = nullptr;
    CameraState frozen_ = {};
    bool wasFrozen_ = false;

    SDL_GPUTransferBuffer* readback_[kReadbackSlots] = {};
    SDL_GPUTransferBuffer* feedbackReadback_[kReadbackSlots] = {};
    SDL_GPUTransferBuffer* vsmReadback_[kReadbackSlots] = {};
    SDL_GPUFence* readbackFence_[kReadbackSlots] = {};
    SDL_GPUTransferBuffer* pickReadback_[kReadbackSlots] = {};
    uint64_t pickSlotRequest_[kReadbackSlots] = {};  // request read back through the slot (0 = none)
    SDL_GPUBuffer* pickBuffer_ = nullptr;
    uint32_t pickX_ = 0, pickY_ = 0;
    uint64_t pickRequest_ = 0, pickDispatched_ = 0, pickAnswered_ = 0;
    uint32_t pickSlot_ = ~0u, pickCluster_ = ~0u, pickTriangle_ = ~0u;
    uint64_t readbackFrame_[kReadbackSlots] = {};
    uint64_t readbackTicket_[kReadbackSlots] = {};  // GPU profiler frames done when the slot's fence is
    int32_t pendingSlot_ = -1;  // slot written by the frame being recorded
    uint64_t frame_ = 0;
    GpuStats stats_ = {};
};

}  // namespace fx
