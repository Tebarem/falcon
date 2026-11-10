#pragma once
// The layered level meshes' masks and records, read by the resolve (visbuffer_resolve.slang
// blendLayers): a level mesh with layers (level/poly_mesh.h layerMaterials) gets a layer here, its
// instance the kInstanceLayered flag with the layer in InstanceGpu::skin.
//   - masks: one RGBA8 layer of a 2D array each (kMaskSize square): r g b the weights of layers
//     1..3 over the mesh's own materials, a the wear (generated, LV12); read through the mesh's
//     second UVs (its vertices' extra stream, render/live_geometry.h)
//   - records: an RGBA32UI texel each: the three layer materials (scene material indices, 0xffff:
//     none), the height blend's width, the mask's layer
// Kept on the CPU too: the array grows (doubling) as meshes take layers, and is filled again.

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class SurfaceLayers {
public:
    static constexpr uint32_t kMaskSize = 512;
    static constexpr uint32_t kNone = 0xffffu;

    bool init(SDL_GPUDevice* device, uint32_t maxLayers);
    void shutdown();

    // A free layer (its mask empty, no materials), or -1 when all `maxLayers` are taken.
    int32_t acquire();
    void release(int32_t layer);
    // Its materials (scene material indices; kNone: none) and blend width (0..1 of their heights).
    void setRecord(int32_t layer, const uint32_t materials[3], float blend);
    // Its mask: kMaskSize² RGBA8 (null: empty).
    void setMask(int32_t layer, const uint8_t* rgba);

    // A copy pass with what changed (before the resolve); the array made bigger first if needed.
    void upload(SDL_GPUCommandBuffer* cmd);
    SDL_GPUTexture* masks() const { return masks_; }
    SDL_GPUSampler* sampler() const { return sampler_; }
    SDL_GPUTexture* records() const { return records_; }
    uint32_t used() const { return used_; }

private:
    bool make(uint32_t capacity);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* masks_ = nullptr;
    SDL_GPUTexture* records_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t uploadBytes_ = 0;
    uint32_t capacity_ = 0, maxLayers_ = 0, used_ = 0;
    std::vector<uint8_t> taken_;
    std::vector<std::vector<uint8_t>> cpuMasks_;  // per layer (empty: all zero)
    std::vector<uint32_t> cpuRecords_;            // 4 per layer
    std::vector<uint32_t> dirtyMasks_;
    bool recordsDirty_ = true, grow_ = false;
};

}  // namespace fx
