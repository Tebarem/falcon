#pragma once

// The project's material assets (.fmat) in a loaded scene: which material index each draws with.
// Cooked ones (falcon cook --material) are the scene's own, with their textures; their factors
// are read again from the file at load (edits since the cook show without one). In the editor,
// a material made (or first used) since the scene was cooked takes a spare slot: its factors
// show at once, its textures after the scene is cooked again.

#include "assets/material_asset.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class ScenePak;
class VirtualTextureSystem;

class MaterialLibrary {
public:
    // Before ScenePak::upload(): the cooked materials' factors from their files, `spare` slots
    // (the editor's) for the others, and `runtime` slots for scripts' copies (MaterialInstance).
    // `projectDir`: where the sources' paths start.
    void init(ScenePak& pak, const std::string& projectDir, uint32_t spare, uint32_t runtime = 0);
    // After upload: changes go to the GPU too (the scene and the VT's material buffer, which the
    // renderer draws with).
    void setGpu(SDL_GPUDevice* device, VirtualTextureSystem* vt)
    {
        device_ = device;
        vt_ = vt;
    }
    // The material index `source` (relative to the project) draws with: cooked, or a spare slot
    // taken now (its factors from the file); -1 when it has neither.
    int32_t indexOf(const std::string& source);
    // The file changed (the editor wrote it): its factors again, live.
    bool reload(const std::string& source);
    bool cooked(const std::string& source) const;
    // The material asset drawing as material `index` ("" when none of the project's does).
    std::string sourceOf(uint32_t index) const;

    // Changes reach the GPU once a frame, in the frame's command buffer: the scene's copy (the
    // raster's) and the VT's (shading's), both from one transfer buffer.
    void flush(SDL_GPUCommandBuffer* cmd);
    // (R1) Scripts: a material as the GPU has it (null: no such material), changed for every user
    // of it; a copy in a runtime slot (-1: none left); and everything scripts changed put back,
    // the copies freed (the editor's Stop).
    const fpak::MaterialGpu* gpu(uint32_t index) const;
    bool set(uint32_t index, const fpak::MaterialGpu& m);
    int32_t clone(uint32_t index);
    void restoreScripted();
    // (R4) A material's surface-shader params (4 floats; scripts: changed like the rest), and every
    // material read again from its file (the project's surface shaders changed: their ids).
    const float* params(uint32_t index) const;
    bool setParams(uint32_t index, const float values[4]);
    // The engine's params of a material: refraction, ior, vertex bound, vertex distance (nullptr: none).
    const float* engineParams(uint32_t index) const;
    void reloadAll();

private:
    bool load(const std::string& source, uint32_t index);
    void write(uint32_t index, const fpak::MaterialGpu& m);  // here, and queued for the GPU

    ScenePak* pak_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    VirtualTextureSystem* vt_ = nullptr;
    std::string projectDir_;
    std::unordered_map<std::string, uint32_t> index_;  // source -> material
    uint32_t spareFirst_ = 0, spareUsed_ = 0, spareCount_ = 0;
    uint32_t runtimeFirst_ = 0, runtimeUsed_ = 0, runtimeCount_ = 0;
    std::vector<uint32_t> dirty_;                                     // to upload (flush)
    std::unordered_map<uint32_t, fpak::MaterialGpu> scriptedBefore_;  // what scripts changed, as it was
    // kParamFloats per material: the surface shader's 4 (R4), then the engine's (T2, V1): refraction,
    // ior, vertex bound, vertex distance. The GPU copy: 32 bytes each after the VT's page table.
    static constexpr uint32_t kParamFloats = 8;
    std::vector<float> params_;
    std::unordered_map<uint32_t, std::array<float, 4>> paramsBefore_;  // scripts' changes, as they were
    std::vector<std::string> surfaces_;                                // the project's surface shaders (ids)
    std::vector<std::string> vertexShaders_;                           // (V1) those with a vertex function
    void findSurfaces();
    // An image's average colour (linear; white when it can't be read), kept until the file changes:
    // an uncooked material's stand-in for its colour texture.
    struct Average {
        uint64_t stamp = 0;
        std::array<float, 3> rgb{};
    };
    std::unordered_map<std::string, Average> averages_;
    const float* averageColour(const std::string& image);
    void writeParams(uint32_t index, const float values[4], uint32_t first = 0);
};

}  // namespace fx
