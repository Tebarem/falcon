#pragma once

// The point and spot lights the probe GI's rays see (probe_lights.slang): a world grid of cells
// round the camera (kDims cells of twice the first probe grid's spacing: 64 x 32 x 64 m by
// default, the second grid's reach), each listing up to kPerCell of the lights whose range
// reaches into it, with the lights it lists. Built on the CPU from the light system's lights
// (the scene's, debug and test lights; not the frame's short-lived ones, which flicker) when one
// changes or the camera moves a cell, and uploaded as one buffer. A light that changed (added,
// removed, moved, recoloured) is reported by its range, so the probes round it update first.

#include "assets/fpak_format.h"
#include "core/math.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace fx {

class LightSystem;

class ProbeLights {
public:
    static constexpr uint32_t kDims[3] = {32, 16, 32};
    static constexpr uint32_t kCells = 32 * 16 * 32;
    static constexpr uint32_t kPerCell = 32;
    static constexpr uint32_t kHeaderWords = 8;

    bool init(SDL_GPUDevice* device);
    void shutdown();
    // Rebuilds and uploads (in a copy pass on cmd) when the lights or the grid's place changed;
    // `changed` gets each changed light's range box (min xyz, max xyz).
    void update(SDL_GPUCommandBuffer* cmd, const LightSystem& lights, const float3& camera, float cell,
                const std::function<void(const float box[6])>& changed);
    void reset() { built_ = false; }

    SDL_GPUBuffer* buffer() const { return buffer_; }
    uint32_t lights() const { return listed_; }  // lights in the grid

private:
    bool ensure(uint32_t bytes);
    void build();

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUBuffer* buffer_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t capacity_ = 0;
    std::vector<fpak::LightGpu> lights_;  // this build's (the light system's, shadow slots cleared)
    std::vector<uint64_t> keys_;
    std::unordered_map<uint64_t, fpak::LightGpu> last_;
    std::vector<uint32_t> words_;
    int32_t origin_[3] = {};
    float cell_ = 0.0f;
    uint32_t listed_ = 0;
    bool built_ = false;
};

}  // namespace fx
