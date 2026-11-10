#pragma once

// Particle sprites (vfx/particle_system.h's effects, their Sprite Renderers): built on the CPU each
// frame from the simulated particles, sorted back to front across every effect, uploaded, and
// drawn in one instanced pass into the HDR frame (shaders/particles.slang) after the scene's
// lighting, MSAA and reflections, before TAA / FSR and the post chain.
//
// A sprite's texture is its material's base colour (an .fmat, through the virtual texture). The
// shader can't write the VT's feedback (graphics stage), so the pages each sprite material needs
// are asked for here: the mip its flipbook frames show at, from the largest sprite on screen.

#include "core/math.h"
#include "render/virtual_shadow_map.h"
#include "vfx/particle_system.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;
class LightSystem;
class ParticleSystem;
class ScenePak;
class VirtualTextureSystem;
struct CameraState;

class ParticleRenderer {
public:
    struct Stats {
        uint32_t sprites = 0, materials = 0;
    };
    // The material index an .fmat (project relative) draws with, or -1 (MaterialLibrary::indexOf).
    using MaterialResolver = std::function<int32_t(const std::string& source)>;

    bool init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt);
    void shutdown();
    void setSource(const ParticleSystem* particles, MaterialResolver resolve)
    {
        particles_ = particles;
        resolve_ = std::move(resolve);
    }
    // Before any pass that draws (a copy pass): the sprites built, sorted and uploaded, their
    // pages asked for. False when there is nothing to draw.
    bool prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, uint64_t frame);
    struct DrawInputs {
        SDL_GPUTexture* target = nullptr;  // the HDR frame (render size)
        SDL_GPUTexture* depth = nullptr;   // its depth (water's included)
        float4x4 rasterViewProj;           // jittered, as the scene's raster
        const LightSystem* lights = nullptr;
        const VirtualShadowMap* vsm = nullptr;
        float mipBias = 0.0f;
        SDL_GPUTexture* reactive = nullptr;  // TAA's and FSR's reactive mask (R8, MAX): the coverage
        bool clearReactive = true;           // the first writer of the mask this frame
    };
    void draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, uint64_t frame, const DrawInputs& in);
    const Stats& stats() const { return stats_; }
    static constexpr SDL_GPUTextureFormat kReactiveFormat = SDL_GPU_TEXTUREFORMAT_R8_UNORM;

private:
    struct SpriteGpu {
        float positionSize[4];
        float color[4];
        float velocity[4];
        float params[4];
        uint32_t info[4];
    };
    static_assert(sizeof(SpriteGpu) == 80);

    bool ensureBuffer(uint32_t count);
    int32_t materialOf(uint32_t assetId);
    void requestPages(uint64_t frame);
    void addTrails(const ParticleSystem::Effect& fx, const ParticleTrailRenderer& r, const CameraState& camera, float pixelScale);

    SDL_GPUDevice* device_ = nullptr;
    const ScenePak* pak_ = nullptr;
    VirtualTextureSystem* vt_ = nullptr;
    const ParticleSystem* particles_ = nullptr;
    MaterialResolver resolve_;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* depthSampler_ = nullptr;
    SDL_GPUBuffer* buffer_ = nullptr;
    SDL_GPUTransferBuffer* upload_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t count_ = 0;
    std::vector<SpriteGpu> sprites_, sorted_;
    std::vector<std::pair<float, uint32_t>> order_;  // (view depth, sprite)
    std::unordered_map<uint32_t, int32_t> materials_;  // vfxAssetId -> material index (this frame)
    // The pages to ask for: per material, the most pixels a frame of it covers, and its sheet's size.
    struct Need {
        float pixels = 0.0f;
        uint32_t cells = 1;  // max(columns, rows)
    };
    std::unordered_map<uint32_t, Need> needs_;
    Stats stats_;
};

}  // namespace fx
