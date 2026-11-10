#pragma once

// Transparent (alpha-blend) scene materials (transparent_cull.slang, transparent.slang).
// The first cluster cull (cluster_cull.slang) sends their clusters here instead of the visibility buffer;
// after the opaque passes:
//   cull    candidates vs this frame's final HiZ (glass behind a wall costs nothing more)
//   sort    one group, farthest first, with each cluster's first triangle in the draw
//   expand  the index buffer in that order (back faces dropped, or first for double-sided)
//   draw    one indexed indirect draw over the lit frame, blended (premultiplied), tested in
//           hardware against the opaque depth (never written)
// Up to kMaxClusters clusters a frame (more: dropped, and the overflow counted). Graphics stages
// write no VT feedback, so the material pages of the transparent instances in view are asked for
// here (requestPages), as particles do.

#include "assets/fpak_format.h"
#include "core/math.h"
#include "render/probe_gi.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;
class LightSystem;
class VirtualShadowMap;
class VirtualTextureSystem;
class ScenePak;
struct ShaderFile;
struct ShaderBlob;

class TransparentRenderer {
public:
    static constexpr uint32_t kMaxCandidates = 16384;  // culling.slang kMaxTransparentCandidates
    static constexpr uint32_t kMaxClusters = 4096;     // culling.slang kMaxTransparent

    bool init(const GpuDevice& gpu);
    void shutdown();
    // (T2) The project's build of the draw (its surface shaders: Cache/Shaders/transparent.fshader)
    // instead of the engine's; nullptr (or a file without it): the engine's again.
    void setProjectShader(const ShaderFile* file);

    // cs_cull_first writes the blend clusters here.
    SDL_GPUBuffer* candidates() const { return candidates_; }

    // Whether the scene has any alpha-blend material (else none of this runs).
    bool active() const { return active_; }
    void setActive(bool a) { active_ = a; }

    struct CullInputs {
        const void* cullUniforms = nullptr;  // CullUniforms (occlusion flag: this frame's final HiZ)
        uint32_t cullUniformsSize = 0;
        SDL_GPUBuffer *counters = nullptr, *args = nullptr, *scene = nullptr, *materials = nullptr, *hiz = nullptr,
                      *skin = nullptr;
    };
    // Cull, sort and expand (compute passes; args at culling.slang's kArg*Transparent).
    void prepare(SDL_GPUCommandBuffer* cmd, const CullInputs& in);

    struct DrawInputs {
        SDL_GPUTexture* target = nullptr;    // the lit HDR frame (render size)
        SDL_GPUTexture* depth = nullptr;     // the opaque depth (D32, reverse-Z): tested, not written
        SDL_GPUTexture* reactive = nullptr;  // TAA's / FSR's reactive mask (R8, MAX)
        bool clearReactive = false;          // the first writer of the mask this frame
        SDL_GPUBuffer *args = nullptr, *scene = nullptr, *skin = nullptr;
        float4x4 rasterViewProj;
        const LightSystem* lights = nullptr;
        const VirtualShadowMap* vsm = nullptr;
        const VirtualTextureSystem* vt = nullptr;
        // Probe GI: its atlases (irradiance, distance, data) and grids; giOn: sampled.
        SDL_GPUTextureSamplerBinding probes[3] = {};
        ProbeVolumeGpu probeVolume = {};
        bool giOn = false;
        float mipBias = 0.0f, seconds = 0.0f;
        // (T2) Refraction: the frame before transparents with mips, and this frame's HiZ.
        SDL_GPUTexture* sceneColor = nullptr;
        SDL_GPUSampler* mipSampler = nullptr;
        uint32_t sceneMips = 1;
        SDL_GPUBuffer* hiz = nullptr;
        bool hizValid = false;
        uint32_t width = 0, height = 0;
    };
    void draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, const DrawInputs& in);

    // Editor picking, after the visibility buffer's (editor_pick wrote `result`): the pixel's ray
    // against this frame's transparent triangles; the nearest hit in front of the opaque depth
    // replaces the instance there.
    struct PickInputs {
        SDL_GPUBuffer *result = nullptr, *counters = nullptr, *scene = nullptr, *skin = nullptr;
        SDL_GPUTexture* depth = nullptr;
        SDL_GPUSampler* pointSampler = nullptr;
        uint32_t x = 0, y = 0, width = 0, height = 0;  // render pixels
    };
    void pick(SDL_GPUCommandBuffer* cmd, const CameraState& camera, const PickInputs& in);

    // The material pages the transparent instances in view need (mip from their size on screen),
    // asked of the virtual textures; once a frame before the frame's VT update.
    void requestPages(const ScenePak& pak, const std::vector<fpak::InstanceGpu>& instances, VirtualTextureSystem& vt,
                      const CameraState& camera, uint32_t height, uint64_t frame);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline *cull_ = nullptr, *sort_ = nullptr, *expand_ = nullptr, *pick_ = nullptr;
    SDL_GPUGraphicsPipeline* makePipeline(const ShaderBlob& vs, const ShaderBlob& fs) const;
    const GpuDevice* gpu_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;  // the engine's, or the project's while it has one
    SDL_GPUGraphicsPipeline *builtin_ = nullptr, *project_ = nullptr;
    SDL_GPUBuffer *candidates_ = nullptr, *list_ = nullptr, *draws_ = nullptr, *indices_ = nullptr;
    bool active_ = false;
    uint64_t signature_ = ~0ull;                    // the blend materials meshBlend_ was found for
    std::vector<std::vector<uint32_t>> meshBlend_;  // per mesh: the blend materials of its clusters
    std::vector<float> need_;                       // per material: most pixels across this frame
    std::vector<uint32_t> pageEntries_;
};

}  // namespace fx
