#pragma once

#include "core/math.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;
class ScenePak;
class LightSystem;
class SkinningSystem;
class VirtualTextureSystem;

// Mirrors VsmParams in engine/shaders/vsm.slang.
struct VsmParamsGpu {
    float4 lightX, lightY, lightZ, world;
    int32_t centers[16][4];
    uint32_t config[4], layout[4], layout2[4], config2[4];
    float4 lod;
    uint32_t local[4];          // shadowed light count, first local entry, spheres, slots to invalidate (bit each)
    uint32_t slotLights[8][4];  // light index per shadow slot
};
static_assert(sizeof(VsmParamsGpu) == 4 * 16 + 256 + 4 * 16 + 16 + 16 + 128);

struct VsmStats {
    uint32_t allocs, freePages, dirty, records, levelMask, overflow, requested;
};

// Virtual shadow maps for the sun (clipmap) and up to 32 local lights (6 cube / 1 spot
// perspective faces with 6 mips each), sharing one page table and one 8192^2 physical atlas
// (see engine/shaders/vsm.slang). Each frame, entirely on the GPU and after the visibility
// buffer: request pages from depth, keep cached pages, allocate missing ones (budgeted), cull
// casters per dirty page with a DAG cut sized to that page's texels, clear + render only the
// new pages. Static scene + lights => steady-state cost is the request pass.
//
// A time budget (setBudget, ms of GPU a frame) caps what moving lights cost: a moved point or spot
// light re-renders all its pages, the sun every page. Their costs are pages (each one's requested
// pages, read back) times the measured time a page (the Shadows section's GPU time over the pages
// it rendered). Each frame the ones waiting longest go first while they fit; one that doesn't fit
// even alone goes alone once it has waited long enough to average within the budget. A light that
// waits keeps the pose its pages were rendered with (LightSystem's slot lights, the sun's basis
// here): its shadow lags it by a few frames but stays right for that pose.
class VirtualShadowMap {
public:
    static constexpr uint32_t kLevels = 12;
    static constexpr uint32_t kPagesPerSide = 32;  // per clipmap level window / local face mip 0
    static constexpr uint32_t kAtlasPagesPerSide = 64;
    static constexpr uint32_t kPhysicalPages = kAtlasPagesPerSide * kAtlasPagesPerSide;
    static constexpr uint32_t kAtlasTexels = kAtlasPagesPerSide * 128;
    static constexpr uint32_t kDirectionalEntries = kLevels * kPagesPerSide * kPagesPerSide;
    static constexpr uint32_t kEntriesPerFace = 1365;  // 6 mips: 32^2 .. 1^2 pages
    static constexpr uint32_t kMaxRecords = 1u << 16;
    static constexpr uint32_t kAllocBudget = 256;  // newly rendered pages per frame
    static constexpr uint32_t kMaxInvalidations = 8192;  // spheres per frame (more: re-render everything)
    static constexpr uint32_t kCounterWords = 64;         // counters read back (stats, budget costs)

    // `maxSlots`: local shadow slots to make room for (the editor's lights can come and go: 32).
    bool init(const GpuDevice& gpu, const ScenePak& pak, const LightSystem& lights, const SkinningSystem& skinning,
              const VirtualTextureSystem& vt,
              uint32_t maxSlots);
    // A new sun direction (render() follows LightSystem's by itself): the light basis is
    // rebuilt and every page re-rendered.
    void setSunDirection(float3 towardSun);
    // What the clipmap must cover (the scene's bounds, grown by a terrain): every page re-rendered.
    void setSceneBounds(const float lo[3], const float hi[3]);
    void shutdown();
    // Records the whole page pipeline; `depth` is this frame's final visibility depth.
    // cache = false re-renders every requested page each frame (reference for cache tests).
    // extraDepth: more surfaces to shadow (water), their pages requested too.
    void render(SDL_GPUCommandBuffer* cmd, const CameraState& camera, SDL_GPUTexture* depth, SDL_GPUSampler* pointSampler,
                uint32_t width, uint32_t height, uint32_t displayHeight, uint64_t frame, bool cache,
                SDL_GPUTexture* extraDepth = nullptr);

    // Inside a copy pass before render(): world spheres around moved casters (old + new pose).
    // `append`: add to the spheres already uploaded this frame.
    // The GPU's record count overflowed in a recent frame (renderer stats, read back a few frames
    // late): fewer new pages per frame until their records fit, then back up.
    void noteRecordOverflow(bool overflow);
    // The budget: GPU milliseconds a frame for re-rendering moved lights' pages (0: none).
    void setBudget(float ms) { budgetMs_ = ms; }
    float budget() const { return budgetMs_; }
    // A finished frame's counters (kCounterWords from counterOffset(), read back) and its Shadows
    // section's GPU time (< 0: not timed): what a page costs, and each light's pages.
    void noteFrame(const uint32_t* counters, float shadowMs);
    float msPerPage() const { return msPerPage_; }
    // The Shadows section's GPU time since the last call: mean and worst of the frames timed (false: none).
    bool takeTiming(float& meanMs, float& worstMs);
    // LightSystem's slot chooser: the slots that re-render this frame (see the class comment).
    uint32_t chooseSlots(uint32_t waiting, uint32_t forced);
    // Something moves: from now on casters flagged kInstanceDynamic are drawn over each page's
    // cached static layer (a second atlas, made here), so re-rendering a page under them costs
    // only them. Every page is rendered again once. Invalidation spheres with a negative radius
    // are a moving caster's that was already moving: the static layer under them stays.
    void enableMovingCasters();
    bool movingCasters() const { return staticAtlas_ != nullptr; }
    float scatterTexels() const { return params_.lod.z; }  // smallest scatter caster, shadow texels
    void uploadInvalidations(SDL_GPUCopyPass* copy, const std::vector<float4>& spheres, bool append = false);
    // Inside a copy pass before render(): the live meshes' caster chunks ((instance, first cluster)
    // pairs, 64 clusters each), after the cooked ones. Their clusters move whenever one is built
    // again, so LiveGeometry sends the whole list when any changes. Room: liveChunkCapacity().
    void setLiveChunks(SDL_GPUCopyPass* copy, const std::vector<uint32_t>& chunks);
    uint32_t liveChunkCapacity() const { return liveChunkCapacity_; }
    uint32_t casterChunks() const { return chunkCount_; }

    const VsmParamsGpu& params() const { return params_; }
    SDL_GPUBuffer* state() const { return state_; }
    SDL_GPUTexture* atlas() const { return atlas_; }
    SDL_GPUSampler* compareSampler() const { return compare_; }
    uint32_t counterOffset() const { return counterOffset_; }  // words, for stats readback

private:
    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    SDL_GPUComputePipeline *clear_ = nullptr, *mark_ = nullptr, *touch_ = nullptr, *free_ = nullptr, *alloc_ = nullptr,
                           *args_ = nullptr, *cull_ = nullptr, *args2_ = nullptr, *expand_ = nullptr,
                           *invalidate_ = nullptr, *invalidateSlots_ = nullptr, *args3_ = nullptr;
    SDL_GPUGraphicsPipeline *clearPages_ = nullptr, *raster_ = nullptr, *rasterInside_ = nullptr, *rasterEdge_ = nullptr,
                            *copyPages_ = nullptr;
    SDL_GPUTexture* atlas_ = nullptr;
    SDL_GPUTexture* staticAtlas_ = nullptr;  // each page's static casters (with moving ones)
    SDL_GPUSampler *compare_ = nullptr, *point_ = nullptr;
    SDL_GPUBuffer *state_ = nullptr, *chunks_ = nullptr, *records_ = nullptr, *indices_ = nullptr, *argsBuffer_ = nullptr;
    SDL_GPUBuffer* crossing_ = nullptr;  // index list of the triangles crossing their page's edge
    SDL_GPUBuffer* invalidations_ = nullptr;
    SDL_GPUTransferBuffer* invalidationUpload_ = nullptr;
    uint32_t invalidationCount_ = 0;
    bool invalidateAll_ = false;
    bool rebuildAll_ = false;  // every page is stale (no static layer yet)
    bool rebuildSun_ = false;  // the sun moved (or the scene's bounds changed): the sun's pages are stale
    const LightSystem* lights_ = nullptr;
    const SkinningSystem* skinning_ = nullptr;
    const VirtualTextureSystem* vt_ = nullptr;  // alpha-masked casters
    uint32_t chunkCount_ = 0, counterOffset_ = 0, entries_ = 0, maxSlots_ = 0;
    uint32_t staticChunks_ = 0, liveChunkCapacity_ = 0;  // the cooked instances' chunks, then room for the live meshes'
    SDL_GPUTransferBuffer* liveChunkUpload_ = nullptr;
    VsmParamsGpu params_ = {};
    float3 lightX_ = {}, lightY_ = {}, lightZ_ = {};
    float3 sunDirection_ = {};
    float2 sceneLightMin_ = {}, sceneLightMax_ = {};  // the scene bounds' light-space XY extent
    float boundsMin_[3] = {}, boundsMax_[3] = {};
    // Budget: pages this frame may re-render for moved lights; whether an item (cost in pages, how
    // many frames it has waited) re-renders now.
    float pageBudget() const;
    uint32_t sunPages() const;  // the sun's requested pages (what turning it re-renders)
    bool grant(float cost, uint32_t& waited);
    void chooseSun();
    float budgetMs_ = 0.0f;
    float msPerPage_ = 0.01f, baseMs_ = 0.05f;  // measured: a page's GPU time, and the rest of the pass
    uint32_t requested_ = 0;                    // pages requested (all of them re-render when the sun turns)
    uint32_t slotPages_[32] = {};               // each slot's requested pages
    uint32_t slotWaited_[32] = {};              // frames each slot has waited
    uint32_t sunWaited_ = 0;
    float spent_ = 0.0f;                        // pages granted this frame
    double timedMs_ = 0.0;                      // takeTiming: sum, worst and count of the frames timed
    float worstMs_ = 0.0f;
    uint32_t timed_ = 0;
    bool chosen_ = false, sunGranted_ = false, cache_ = true;
    uint32_t allocBudget_ = kAllocBudget;  // new pages per frame, lowered while records overflow
    uint32_t scatterCalm_ = 0;             // frames without overflow since the scatter casters' size changed
};

}  // namespace fx
