#pragma once

// Water on the GPU (shaders water_surface.slang, water_composite.slang):
//   prepare()    per frame, on the CPU: each visible body's surface as quadtree patches around
//                the camera (CDLOD: 32 x 32 quads each, finer near the camera, morphing between
//                levels), the bodies' parameter blocks, the screen rectangle water may cover;
//                uploaded in one copy pass. Nothing to draw: every pass below is skipped.
//   raster()     after the visibility buffer: the scene's depth copied into the water's depth
//                target, then every patch in one instanced draw (waves in the vertex shader),
//                writing the water's depth, normals, foam and body per pixel
//   composite()  after the resolve and the reflection pass's mip pyramid (the frame before water,
//                the refraction source): each water pixel's colour, and its reflection weight and
//                normals for the reflection pass, which then starts from the water's depth
// The surface also marks shadow pages (VirtualShadowMap::render's extra depth).

#include "core/math.h"
#include "render/probe_gi.h"
#include "render/virtual_shadow_map.h"
#include "render/water_ocean.h"
#include "water/water_system.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;

class WaterRenderer {
public:
    static constexpr uint32_t kMaxPatches = 8192;
    static constexpr uint32_t kMaxOutlinePoints = 16384;  // polygon points over all bodies
    static constexpr float kLeafSize = 8.0f;  // metres per finest patch (32 quads: 25 cm)

    bool init(const GpuDevice& gpu);
    // What it may cost (FrameSettings::water): ripples on or off, their square and resolution.
    void setRipples(bool on, float extent, uint32_t resolution);
    void shutdown();
    void setSystem(const WaterSystem* system) { system_ = system; }

    // Before the frame's passes (outside any pass): true when there is water to draw.
    bool prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height);
    bool active() const { return active_; }

    void raster(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* sceneDepth, SDL_GPUSampler* pointSampler, const float4x4& rasterViewProj,
                uint32_t width, uint32_t height);

    struct CompositeInputs {
        SDL_GPUTexture* sceneDepth = nullptr;
        SDL_GPUTexture* refractSource = nullptr;  // the frame before water, with mips
        uint32_t refractMips = 1;
        SDL_GPUSampler* pointSampler = nullptr;
        SDL_GPUSampler* mipSampler = nullptr;
        SDL_GPUTexture* color = nullptr;          // RGBA16F, read-write
        SDL_GPUTexture* velocity = nullptr;
        SDL_GPUTexture* reflectWeight = nullptr;
        SDL_GPUTexture* reflectNormal = nullptr;
        SDL_GPUTexture* vsmAtlas = nullptr;
        SDL_GPUSampler* vsmSampler = nullptr;
        SDL_GPUBuffer* vsmState = nullptr;
        const VsmParamsGpu* vsm = nullptr;
        // Probe GI: its atlases (irradiance, distance, data) and grids; giOn: sampled.
        SDL_GPUTextureSamplerBinding probes[3] = {};
        ProbeVolumeGpu probeVolume = {};
        bool giOn = false;
        float bounce = 1.0f, sky = 1.0f;  // GI intensities
        // Reflections off screen: the BVH (null: none), the scene, its materials and their virtual
        // texture's colour.
        SDL_GPUBuffer *bvh = nullptr, *scene = nullptr, *materials = nullptr;
        SDL_GPUTextureSamplerBinding vtAlbedo = {};
        uint32_t pageTable = 0;
        bool alphaMask = true;
        bool reflectionsFollow = false;           // the reflection pass runs after
        float3 sunDirection = {0, 1, 0}, sunColor = {1, 1, 1};
        float4x4 prevViewProj = {};
        float2 jitter = {0, 0};
        float4x4 rasterViewProj = {};             // (jittered: the planar reflections' projection)
        // The terrain, for what mirrors reflect off screen (TerrainSurface::coarseLayout; no
        // heights: none).
        SDL_GPUTexture *terrainHeights = nullptr, *terrainMaterials = nullptr;
        SDL_GPUBuffer* terrainData = nullptr;
        float4 terrainGrid = {}, terrainRange = {};
    };
    void composite(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, const CompositeInputs& in);

    // The depth with the water's surface in it (the scene's where there is none): for the
    // reflections, TAA and upscaling. Valid after raster().
    SDL_GPUTexture* depth() const { return depth_; }
    uint32_t patchCount() const { return patchCount_; }
    // The body the camera is in (under its surface, inside its outline and depth), or -1.
    int32_t cameraBody() const { return cameraBody_; }

private:
    struct PatchGpu {
        float originX, originZ, size, morphStart, morphEnd;
        uint32_t body, face, pad;  // face: 0 the surface, 1..4 a tank's sides, 5 its bottom
    };
    void selectPatches(uint32_t body, const CameraState& camera, const float4x4& viewProj);
    void stepRipples(SDL_GPUCommandBuffer* cmd, const CameraState& camera);
    bool ensureTargets(uint32_t width, uint32_t height);
    void buildDetail(SDL_GPUCommandBuffer* cmd);

    SDL_GPUDevice* device_ = nullptr;
    const WaterSystem* system_ = nullptr;
    WaterOcean ocean_;
    SDL_GPUGraphicsPipeline *copy_ = nullptr, *surface_ = nullptr;
    SDL_GPUComputePipeline* composite_ = nullptr;
    SDL_GPUSampler* repeat_ = nullptr;
    SDL_GPUTexture *depth_ = nullptr, *normals_ = nullptr, *extra_ = nullptr, *detail_ = nullptr, *caustics_ = nullptr;
    int32_t cameraBody_ = -1;  // the body the camera is in this frame
    // Planar reflections: the body whose plane they mirror (the one filling most of the view), the
    // pixels' winners and what they reflect.
    int32_t mirrorBody_ = -1;
    SDL_GPUComputePipeline *ssprClear_ = nullptr, *ssprProject_ = nullptr, *ssprResolve_ = nullptr;
    SDL_GPUBuffer *ssprKeys_ = nullptr, *ssprSources_ = nullptr;
    uint32_t ssprPixels_ = 0;
    // Live ripples: two states (h, h_prev) ping-ponged, the square's corner in world texels.
    SDL_GPUComputePipeline* rippleStep_ = nullptr;
    SDL_GPUTexture* rippleState_[2] = {};
    SDL_GPUBuffer* splats_ = nullptr;
    SDL_GPUSampler* clamp_ = nullptr;
    bool ripplesOn_ = true, ripplesActive_ = false;
    float rippleExtent_ = 64.0f;
    uint32_t rippleN_ = 512, rippleCurrent_ = 0;
    int32_t rippleOrigin_[2] = {0, 0};
    bool rippleValid_ = false;
    float rippleTime_ = -1.0f, rippleCarry_ = 0.0f;
    float3 lastCamera_ = {0.0f, 0.0f, 0.0f};
    std::vector<WaterSystem::Splash> splashes_;  // waiting for the next ripple step
    uint32_t targetW_ = 0, targetH_ = 0;
    bool detailReady_ = false;
    SDL_GPUBuffer *patches_ = nullptr, *bodies_ = nullptr, *outlines_ = nullptr;
    std::vector<float> outlineData_;  // polygons' points, float2 each
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    std::vector<PatchGpu> patchList_;
    std::vector<float> bodyData_;
    uint32_t patchCount_ = 0, bodyCount_ = 0, loggedPatches_ = ~0u;
    int32_t rect_[4] = {};  // screen pixels the water may cover: x0, y0, x1, y1
    bool active_ = false;
    float time_ = 0.0f;
};

}  // namespace fx
