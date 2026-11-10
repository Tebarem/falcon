#pragma once

#include "assets/fpak_format.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace fx {

class GpuDevice;
class Scene;
class ScenePak;

// Punctual lights with binned clustered culling (see engine/shaders/lighting.slang).
// Per frame on the CPU: gather the lights, sort them by view depth and build 1024 depth bins (a
// contiguous sorted-index range each). On the GPU: rasterize light bounds into 16x16-pixel tile
// bitmasks. The resolve pass then visits only lights in both its tile and its depth bin.
//
// The lights: `extra` (debug) lights, then every enabled entity with a Light component (cooked
// lights are entities, the editor creates, moves and edits them), then test lights. The packed
// array is uploaded when it changes. Lights flagged to cast shadows get one of the 32 shadow
// slots: slots stick to their light while it keeps casting, and stay contiguous (a freed slot
// takes the last one's light). Each slot also keeps its light as its shadow pages were last
// rendered, after the kMaxLights lights in lights() (vsm.slang's slotLight): a slot whose light
// moved or changed shape is re-rendered when the slot chooser (the shadow map's budget) lets it,
// one whose owner changed at once; changedSlots() says which re-render this frame.
class LightSystem {
public:
    static constexpr uint32_t kMaxLights = 16384;
    static constexpr uint32_t kZBins = 1024;
    static constexpr uint32_t kTileSize = 16;
    static constexpr uint32_t kMaxShadowSlots = 32;

    // testLights > 0 adds that many random point lights inside the scene bounds (stress test).
    // `extra` lights (e.g. debug lights) come first and are first in line for shadows; test
    // lights cast shadows within what is left of `shadowed` after them and the scene's casters.
    bool init(const GpuDevice& gpu, const ScenePak& pak, Scene& scene, uint32_t testLights, uint32_t shadowed,
              const std::vector<fpak::LightGpu>& extra = {}, float sunScale = 1.0f);
    void shutdown();
    void update(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height);
    // This frame's short-lived lights (particles' Light Renderers): after the scene's, no shadows.
    void setTransientLights(const std::vector<fpak::LightGpu>& lights) { transient_ = lights; }
    // Benchmark (--moving-lights): the extra and test lights circle where they are, 1 m out, once a
    // second, at `seconds` (< 0: still).
    void setMotion(float seconds) { motion_ = seconds; }
    // Shadow slots re-rendered this frame (bit per slot): their light as it is now is kept.
    uint32_t changedSlots() const { return changedSlots_; }
    // Which slots re-render this frame, of those whose light moved or changed shape (`waiting`)
    // and those that must (`forced`: a new light in the slot). Without it, all of them.
    using SlotChooser = std::function<uint32_t(uint32_t waiting, uint32_t forced)>;
    void setSlotChooser(SlotChooser chooser) { chooser_ = std::move(chooser); }

    SDL_GPUBuffer* lights() const { return lights_; }
    SDL_GPUBuffer* grid() const { return grid_; }
    uint32_t count() const { return count_; }
    uint32_t orderOffset() const { return kZBins; }
    uint32_t tileOffset() const { return kZBins + kMaxLights; }
    uint32_t tilesX() const { return tilesX_; }
    uint32_t words() const { return words_; }
    float binScale() const { return binScale_; }

    float3 sunDirection() const { return sunDirection_; }  // toward the sun (the first directional light)
    const std::vector<uint32_t>& shadowSlots() const { return shadowSlots_; }  // light index per slot
    float3 sunColor() const { return sunColor_; }  // irradiance; black without a sun
    // The lights as packed (the order lights() holds), who each one is (sticks across frames), and
    // how many come before the frame's short-lived ones.
    const std::vector<fpak::LightGpu>& packed() const { return cpuLights_; }
    const std::vector<uint64_t>& keys() const { return keys_; }
    uint32_t persistentCount() const { return persistent_; }

private:
    bool ensureGrid(uint32_t width, uint32_t height);
    void gather();       // packs the lights (cpuLights_, keys_), assigns shadow slots
    void assignSlots();  // sticky, contiguous shadow slots for the lights that cast shadows

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline* clear_ = nullptr;
    SDL_GPUComputePipeline* bin_ = nullptr;
    SDL_GPUBuffer* lights_ = nullptr;
    SDL_GPUBuffer* grid_ = nullptr;
    SDL_GPUTransferBuffer* staging_ = nullptr;
    Scene* scene_ = nullptr;
    std::vector<fpak::LightGpu> base_;  // extra lights, then test lights (static)
    std::vector<fpak::LightGpu> transient_;  // setTransientLights()
    uint32_t extraCount_ = 0;
    std::vector<bool> baseShadows_;     // which base lights cast shadows
    std::vector<fpak::LightGpu> cpuLights_, uploaded_;
    std::vector<uint64_t> keys_;        // per packed light: who it is (slots stick to keys)
    std::vector<bool> wantsShadow_;
    std::vector<uint64_t> slotKeys_;    // per shadow slot: the key of its light
    std::vector<fpak::LightGpu> slotShape_;  // per shadow slot: its light as its pages were rendered
    uint32_t changedSlots_ = 0;
    bool slotShapesChanged_ = true;          // (uploaded after the lights)
    SlotChooser chooser_;
    bool uploadNeeded_ = true;
    std::vector<uint32_t> order_;
    std::vector<float> zMin_, zMax_;
    std::vector<uint32_t> upload_;  // z-bins + order, staged each frame
    std::vector<uint32_t> shadowSlots_;
    uint32_t persistent_ = 0;
    uint32_t count_ = 0, tilesX_ = 0, tilesY_ = 0, words_ = 1, width_ = 0, height_ = 0;
    float binScale_ = 1.0f;
    float3 sunDirection_ = normalize(float3{0.35f, 0.9f, 0.25f});
    float3 sunColor_ = {3.2f, 3.04f, 2.72f};
    float sunScale_ = 1.0f;
    float motion_ = -1.0f;
};

}  // namespace fx
