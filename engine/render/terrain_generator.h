#pragma once

// Runs a terrain generator stack (terrain/terrain_generate.h) on the GPU
// (terrain_generate.slang) and writes the result into the terrain.
//
// The region is split into chunks of up to kChunk^2 samples. For each chunk, with a margin
// wide enough for the steps' reach (blur radius, curvature, erosion):
//   1. its tiles are pinned and loaded; heights (and materials, for paint steps) go up;
//   2. the steps run, a few GPU submissions per frame (erosion in batches of iterations), each
//      on its own command buffer and fence, so the editor stays responsive;
//   3. the chunk comes back and is written into the terrain, blended into the ground around by
//      the region's falloff (or by the caller's weights: the erosion brush).
// Chunks are written in order and read what the ones before wrote, so their edges meet.

#include "terrain/terrain.h"
#include "terrain/terrain_generate.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fx {

class GpuDevice;

class TerrainGenerator {
public:
    static constexpr int32_t kChunk = 2048;  // samples per side of a chunk (its interior)

    bool init(const GpuDevice& gpu);
    void shutdown();

    // A run over the stack's region of `terrain`. `weights` (optional): how much of the result
    // each sample of `weightRect` takes (the region is then that rectangle). `beforeWrite` is
    // called with each chunk's rectangle before it is written (undo snapshots). Replaces a run
    // in progress (what it wrote stays).
    bool start(Terrain& terrain, const TerrainGenStack& stack, std::function<void(TerrainRect)> beforeWrite,
               const std::vector<float>* weights = nullptr, TerrainRect weightRect = {0, 0, -1, -1});
    // Once per frame (main thread): GPU work submitted, finished chunks written. True when the
    // run finished in this call.
    bool tick();
    // start() and tick() until done, waiting on the GPU (start-up, tests). Streams the terrain's
    // tiles meanwhile.
    bool run(Terrain& terrain, const TerrainGenStack& stack);
    void cancel();

    bool busy() const { return terrain_ != nullptr; }
    float progress() const;      // 0..1 of the current run
    const std::string& status() const { return status_; }
    TerrainRect region() const { return rect_; }  // the last run's samples
    double lastMs() const { return lastMs_; }     // the last run's time

private:
    struct Chunk {
        TerrainRect interior, buffer;  // buffer: interior + margin (may reach past the grid)
    };
    enum class Phase { Load, Steps, Download, Wait, Done };

    bool ensureBuffers(uint32_t samples, uint32_t cells);
    void releaseBuffers();
    void upload(SDL_GPUCommandBuffer* cmd);
    void dispatch(SDL_GPUCommandBuffer* cmd, SDL_GPUComputePipeline* p, uint32_t w, uint32_t h, const void* uniforms);
    bool recordSteps(SDL_GPUCommandBuffer* cmd);  // true when the last step is recorded
    void recordDownload(SDL_GPUCommandBuffer* cmd);
    void writeChunk();
    float weightAt(int32_t gx, int32_t gz) const;

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUComputePipeline *clear_ = nullptr, *stepPass_ = nullptr, *blur_ = nullptr, *erodeInit_ = nullptr, *erodeFlux_ = nullptr,
                           *erodeWater_ = nullptr, *erodeTransport_ = nullptr, *erodeSettle_ = nullptr, *erodeApply_ = nullptr;
    std::vector<std::pair<SDL_GPUComputePipeline*, uint32_t>> rwCounts_;  // read-write buffers each declares
    // heights x2, materials, masks; erosion cells: A, flux, B, S.
    SDL_GPUBuffer* buffers_[8] = {};
    uint32_t sampleCapacity_ = 0, cellCapacity_ = 0;
    SDL_GPUTransferBuffer *upload_ = nullptr, *download_ = nullptr;
    uint32_t uploadBytes_ = 0, downloadBytes_ = 0;
    SDL_GPUFence* fence_ = nullptr;

    // The run.
    Terrain* terrain_ = nullptr;
    TerrainGenStack stack_;
    std::function<void(TerrainRect)> beforeWrite_;
    std::vector<float> weights_;
    TerrainRect rect_ = {0, 0, -1, -1};
    bool writeHeights_ = false, writeMaterials_ = false;
    int32_t margin_ = 0;
    std::vector<Chunk> chunks_;
    size_t chunk_ = 0;
    Phase phase_ = Phase::Done;
    size_t step_ = 0;
    uint32_t iteration_ = 0;
    uint32_t current_ = 0;           // which height buffer holds the current heights
    uint32_t factor_ = 1, cellsW_ = 0, cellsH_ = 0;
    std::string status_;
    uint64_t startNs_ = 0;
    double lastMs_ = 0.0;
    std::vector<float> heights_;     // the chunk's buffer rect (staging)
    std::vector<uint32_t> materials_;
};

}  // namespace fx
