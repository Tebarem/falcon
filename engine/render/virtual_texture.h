#pragma once

#include "assets/fpak_format.h"

#include <SDL3/SDL_asyncio.h>
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace fx {

class GpuDevice;
class ScenePak;

struct VtStats {
    uint32_t resident, capacity, queued, inFlight;
    uint32_t uploadedLastFrame, evictedLastFrame, requestsLastFeedback;
    uint64_t uploadedTotal;
    uint32_t residentPerMip[8];
};

// Virtual texturing runtime ("megatexture"). All textured materials share one physical page
// cache: three Texture2DArrays (albedo BC7 sRGB, normal BC5, ORM BC7) of 136x136 pages, one
// layer per page, indexed by a page table that lives right after the materials in one buffer.
//
// Each frame the resolve pass reports the pages it wanted (deduplicated, 1/16 of pixels in a
// rotating pattern). That list comes back to the CPU a few frames later without stalling;
// missing pages are read asynchronously from the .fvt file (coarse mips first), uploaded, and
// published in the page table. Least-recently-used pages are evicted; the last mip of every
// material is pinned so sampling always finds something.
class VirtualTextureSystem {
public:
    static constexpr uint32_t kFeedbackCapacity = 8192;

    bool init(const GpuDevice& gpu, const ScenePak& pak, const char* fvtPath, uint32_t physicalPages);
    void shutdown();

    // Start of frame: finish completed reads, upload them, publish page-table changes, issue new reads.
    void update(SDL_GPUCommandBuffer* cmd, uint64_t frame);
    // Clears this frame's feedback; record before the resolve pass.
    void recordFeedbackClear(SDL_GPUCommandBuffer* cmd, SDL_GPUComputePipeline* clearPipeline);
    // Feedback that came back from a finished frame.
    void consumeFeedback(const uint32_t* list, uint64_t frame);
    // Pages wanted by a pass that can't write feedback (the Animator's preview): queued after the
    // feedback's (coarse mips first), those already in refreshed as used at `frame`.
    void request(const std::vector<uint32_t>& entries, uint64_t frame);

    SDL_GPUBuffer* materialData() const { return materialData_; }  // MaterialGpu[] then page table
    uint32_t pageTableOffset() const { return pageTableOffset_; }  // bytes
    // (R4) Then each material's 4 surface-shader params (16 bytes), from here (bytes).
    uint32_t paramsOffset() const { return paramsOffset_; }
    SDL_GPUTexture* physical(uint32_t channel) const { return physical_[channel]; }
    SDL_GPUSampler* sampler() const { return sampler_; }
    SDL_GPUBuffer* feedbackList() const { return feedbackList_; }
    SDL_GPUBuffer* feedbackMask() const { return feedbackMask_; }
    uint32_t pageCount() const { return pageCount_; }
    const VtStats& stats() const { return stats_; }
    // Nothing requested, reading or waiting for upload (as of the last update()).
    bool idle() const { return !pageCount_ || (queue_.empty() && ready_.empty() && stats_.inFlight == 0 && stats_.uploadedLastFrame == 0); }
    float mipBias() const { return mipBias_; }

private:
    struct Slot {
        std::vector<uint8_t> data;
        uint32_t entry = 0;
        bool busy = false;
    };
    enum : uint8_t { kIdle, kQueued, kLoading, kReady, kResident };

    bool loadPinned(const char* path, const std::vector<uint32_t>& entries);
    int32_t allocateLayer(uint64_t frame);
    void recordUploads(SDL_GPUCommandBuffer* cmd, const std::vector<uint32_t>& slots, const std::vector<int32_t>& layers);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* physical_[3] = {};
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUBuffer* materialData_ = nullptr;
    SDL_GPUBuffer* feedbackList_ = nullptr;
    SDL_GPUBuffer* feedbackMask_ = nullptr;
    SDL_GPUTransferBuffer* pageStaging_ = nullptr;
    SDL_GPUTransferBuffer* tableStaging_ = nullptr;
    uint32_t pageTableOffset_ = 0, pageCount_ = 0, maskWords_ = 0, layers_ = 0;
    uint32_t paramsOffset_ = 0;

    std::vector<uint32_t> pageTable_;   // CPU copy of the GPU page table
    std::vector<uint8_t> state_;        // per entry
    std::vector<uint8_t> entryMip_;     // per entry, for request priority
    std::vector<int32_t> layerOwner_;   // per physical layer: entry or -1
    std::vector<uint64_t> layerUsed_;   // per physical layer: last frame requested
    std::vector<uint8_t> layerPinned_;
    std::vector<uint32_t> queue_;       // requested, not yet reading
    uint32_t dirtyMin_ = UINT32_MAX, dirtyMax_ = 0;

    SDL_AsyncIO* file_ = nullptr;
    SDL_AsyncIOQueue* ioQueue_ = nullptr;
    uint32_t pendingReads_ = 0;  // started reads whose result has not been taken from ioQueue_
    uint64_t dataOffset_ = 0;
    std::vector<Slot> slots_;
    std::vector<uint32_t> ready_;       // slots with completed reads
    float mipBias_ = 0.0f;
    VtStats stats_ = {};
};

}  // namespace fx
