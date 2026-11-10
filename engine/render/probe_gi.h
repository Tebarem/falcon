#pragma once

// Probe global illumination (DDGI, Majercik et al. 2019) on the compute-shader BVH
// (render/ray_tracing/scene_bvh.h): irradiance probes on grids round the camera, each update a
// probe's rays through the scene (probe_trace.slang), blended into its atlas tiles
// (probe_blend.slang) and used to move it out of walls and to tell whether anything is near it
// (probe_relocate.slang). probe_common.slang has the layout and the sampling.
//
// Grids: kCascades of kDims probes, spacing doubling from the first (giProbeSpacing, 1 m: 32 x
// 16 x 32 m, then 64 x 32 x 64, then 128 x 64 x 128), centred on the camera and moved in whole
// probes; the probes a move brings in are fresh (not sampled) until their first update.
// Updates: a budget of rays a frame (giProbeRays, kRays a probe). Fresh probes first, then the
// ones near a change (an edit, a drag, a live mesh or terrain written: kBoostUpdates updates in a
// row, their memory started again), then each grid in turn, the finest twice as often as the next.
// A probe's light is the mean of its updates since it was fresh or since a change near it (then
// at most kHysteresis of it kept), each update's rays turned by the probe's age, not the frame;
// after kSleepAge updates it sleeps until something wakes it (a change, something moving near it,
// the sun), so a still scene's light stops changing and comes out the same however many frames
// it took (captures wait for it: settled()). New grids (the first frame, another spacing) are
// lit at once: kWarmRounds rounds over every probe in that frame (kMaxEntries a batch), the rest
// of their updates (less noise) over the frames after.
// What a ray hits is lit by the sun, the point and spot lights round it (render/probe_lights.h:
// one of them tested with a shadow ray) and the probes' light there; a light that changes wakes
// the probes within its range.

#include "core/math.h"
#include "render/probe_lights.h"
#include "render/gi_settings.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;
class LightSystem;
class SceneBvh;
class ScenePak;
class VirtualTextureSystem;

// Mirrors ProbeVolume in probe_common.slang.
struct ProbeVolumeGpu {
    int32_t base[3][4];  // each grid's lowest probe (world index)
    float spacing[4];    // each grid's; w: 1 when the probes have light (0: none)
    float bias[4];       // x: along the normal, y: toward the eye (spacings); z: sky intensity
};

class ProbeGi {
public:
    static constexpr uint32_t kCascades = 3;
    static constexpr uint32_t kDims[3] = {32, 16, 32};
    static constexpr uint32_t kProbes = 32 * 16 * 32;  // a grid's
    static constexpr uint32_t kRays = 64;              // a probe update's
    static constexpr uint32_t kMaxEntries = 8192;      // probe updates a frame
    static constexpr uint32_t kBoostUpdates = 8;
    static constexpr uint32_t kSleepAge = 64;  // updates before a probe sleeps
    static constexpr uint32_t kWarmRounds = 24;  // new grids: updates every probe takes in their first frame
    static constexpr uint32_t kWakeAge = 10;   // something moved near it: updates again from (memory at its most)

    bool init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt);
    void shutdown();
    void setCamera(const float3& position) { camera_ = position; }
    // Everything changed (a new scene, the terrain made again): every probe's next updates forget faster.
    void invalidate() { boostAll_ = true; }
    // Something changed in a world region (min xyz, max xyz): the probes round it first.
    void invalidateRegion(const float region[6]);
    // Something moved in a world region: the probes round it wake.
    void wakeRegion(const float region[6]);
    // After the BVH's update: this frame's probe updates.
    void update(SDL_GPUCommandBuffer* cmd, const SceneBvh& bvh, const LightSystem& lights, const GiSettings& settings);

    bool ready() const { return ready_; }
    SDL_GPUTexture* irradiance() const { return irradiance_; }
    SDL_GPUTexture* distance() const { return distance_; }
    SDL_GPUTexture* data() const { return data_; }
    SDL_GPUSampler* linear() const { return linear_; }
    SDL_GPUSampler* point() const { return point_; }
    const ProbeVolumeGpu& volume() const { return volume_; }
    uint32_t lastUpdates() const { return lastUpdates_; }  // probes updated last frame
    uint32_t localLights() const { return localLights_.lights(); }  // point and spot lights the rays see
    // Every probe asleep (or no probe GI): the probes' light no longer changes.
    bool settled() const { return !ready_ || (awake_ == 0 && changedQueue_.empty() && freshQueue_.empty() && !pendingRegion_); }

private:
    struct Grid {
        int32_t base[3] = {};
        float spacing = 1.0f;
        uint32_t cursor = 0;  // round robin
        bool placed = false;
    };
    static uint32_t key(uint32_t cascade, uint32_t index) { return index | cascade << 16; }
    enum class Mark { Change, Wake };
    void resetState();
    void place(uint32_t cascade);  // follow the camera, renewing what comes in
    void setAge(uint32_t probe, uint32_t age);
    void markRegion(const float region[6], Mark mark);
    void schedule();
    void warmUp();

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    VirtualTextureSystem* vt_ = nullptr;
    SDL_GPUComputePipeline *trace_ = nullptr, *blend_ = nullptr, *relocate_ = nullptr, *reset_ = nullptr;
    SDL_GPUTexture *irradiance_ = nullptr, *distance_ = nullptr, *data_ = nullptr;
    SDL_GPUBuffer *rays_ = nullptr, *schedule_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUSampler *linear_ = nullptr, *point_ = nullptr;
    ProbeLights localLights_;

    float3 camera_ = {};
    float spacing_ = 0.0f;  // the first grid's (settings)
    Grid grids_[kCascades];
    ProbeVolumeGpu volume_ = {};
    // Per probe (cascade * kProbes + index): updates left at the front of the queue, waiting in it,
    // fresh (scrolled in, no update yet), its age (updates since fresh or a change; kSleepAge:
    // asleep), and the frame it was last scheduled.
    std::vector<uint8_t> boost_, queued_, fresh_, age_;
    std::vector<uint32_t> scheduled_;
    std::vector<uint32_t> freshQueue_, changedQueue_;  // keys
    size_t freshHead_ = 0;
    std::vector<uint32_t> entries_, resets_;
    bool pendingRegion_ = false, boostAll_ = false, pendingWake_ = false, warm_ = false;
    float region_[6] = {}, wake_[6] = {};
    float3 sun_ = {}, sunColor_ = {};
    uint32_t awake_ = 0;
    uint32_t revision_ = ~0u, regionRevision_ = 0;
    float intensity_ = -1.0f, sky_ = -1.0f;
    uint32_t frame_ = 0, budget_ = 0, lastUpdates_ = 0;
    bool ready_ = false;
};

}  // namespace fx
