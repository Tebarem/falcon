#pragma once

// Decals for the resolve (shaders/visbuffer_resolve.slang): each frame the visible ones (vfx/decal_system.h),
// in their order, as rows of a float texture, and which fall in each 32x32 pixel tile of the
// screen as a bitmask texture. Both are read-only storage textures: the resolve's storage-buffer
// slots are all taken. Their materials are sampled from the virtual texture by the resolve itself
// (its feedback asks for their pages).

#include "core/math.h"
#include "vfx/decal_system.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;
class Scene;
struct CameraState;

class DecalRenderer {
public:
    static constexpr uint32_t kMaxVisible = 1024;
    static constexpr uint32_t kTile = 32;
    static constexpr uint32_t kRows = 9;  // texels per decal in data()
    using MaterialResolver = std::function<int32_t(const std::string& source)>;

    bool init(const GpuDevice& gpu);
    void shutdown();
    void setSource(DecalSystem* decals, Scene* scene, MaterialResolver resolve)
    {
        decals_ = decals;
        scene_ = scene;
        resolve_ = std::move(resolve);
    }
    // A copy pass: this frame's visible decals and their tiles uploaded.
    void prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height);
    SDL_GPUTexture* data() const { return data_; }
    SDL_GPUTexture* bins() const { return bins_; }
    uint32_t count() const { return count_; }
    uint32_t words() const { return words_; }
    uint32_t tilesX() const { return tilesX_; }

private:
    bool ensureBins(uint32_t width, uint32_t height);

    SDL_GPUDevice* device_ = nullptr;
    DecalSystem* decals_ = nullptr;
    Scene* scene_ = nullptr;
    MaterialResolver resolve_;
    SDL_GPUTexture* data_ = nullptr;
    SDL_GPUTexture* bins_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    uint32_t tilesX_ = 0, tilesY_ = 0, count_ = 0, words_ = 1;
    bool cleared_ = false;
    std::vector<DecalInstance> all_;
    std::vector<float> rows_;
    std::vector<uint32_t> binWords_;
    std::unordered_map<uint32_t, int32_t> materials_;
};

}  // namespace fx
