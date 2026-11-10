#include "render/virtual_texture.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace fx {

namespace {

constexpr uint32_t kPageResident = 0x80000000u;  // page-table flag (not the per-entry state)
constexpr uint32_t kMaxUploadsPerFrame = 48;  // ~2.6 MB of BC data per frame
constexpr uint32_t kReadSlots = 128;          // concurrent async page reads
constexpr uint64_t kEvictAge = 3;             // never evict a page requested in the last frames
constexpr uint32_t kMaxLayers = 2048;         // D3D12 array size limit

const SDL_GPUTextureFormat kChannelFormats[3] = {SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB,
                                                 SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM,
                                                 SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM};

}  // namespace

bool VirtualTextureSystem::init(const GpuDevice& gpu, const ScenePak& pak, const char* fvtPath, uint32_t physicalPages)
{
    FX_ZONE_CAT("VirtualTextureSystem::init", Rendering);
    device_ = gpu.get();
    const fpak::Header& h = pak.header();
    // Cooked materials, then the runtime ones (the terrain's).
    std::vector<fpak::MaterialGpu> materials = pak.materials();
    const uint32_t materialCount = uint32_t(materials.size());
    pageCount_ = h.vtPageCount;

    // Validate the page file; without it every material falls back to its factors.
    if (pageCount_) {
        SDL_IOStream* io = SDL_IOFromFile(fvtPath, "rb");
        fpak::VtFileHeader vh = {};
        const bool ok = io && SDL_ReadIO(io, &vh, sizeof(vh)) == sizeof(vh) && !std::memcmp(vh.magic, "FVT1", 4) &&
                        vh.pageCount == pageCount_ && vh.pageBytes == fpak::kVtPageBytes && vh.sourceHash == h.sourceHash;
        if (io)
            SDL_CloseIO(io);
        if (ok) {
            dataOffset_ = vh.dataOffset;
            file_ = SDL_AsyncIOFromFile(fvtPath, "r");
            ioQueue_ = SDL_CreateAsyncIOQueue();
        }
        if (!ok || !file_ || !ioQueue_) {
            FX_WARN("vt: %s missing or out of date (re-cook the scene); rendering without textures", fvtPath);
            pageCount_ = 0;
        }
    }
    if (!pageCount_)
        for (auto& m : materials)
            m.vtPageTable = fpak::kVtNoTexture;

    // Per-entry mip levels (request priority) and the pinned last mip of each material.
    pageTable_.assign(pageCount_, 0);
    state_.assign(pageCount_, kIdle);
    entryMip_.assign(pageCount_, 0);
    std::vector<uint32_t> pinnedEntries;
    for (const auto& m : materials) {
        if (m.vtPageTable == fpak::kVtNoTexture)
            continue;
        const uint32_t lw = m.vtSize & 0xff, lh = (m.vtSize >> 8) & 0xff, mips = (m.vtSize >> 16) & 0xff;
        uint32_t e = m.vtPageTable;
        for (uint32_t mip = 0; mip < mips; ++mip)
            for (uint32_t n = fpak::vtLevelPages(lw, mip) * fpak::vtLevelPages(lh, mip); n; --n)
                entryMip_[e++] = uint8_t(mip);
        pinnedEntries.push_back(e - 1);  // the single page of the last mip
    }
    const uint32_t pinned = uint32_t(pinnedEntries.size());

    layers_ = std::clamp(std::max(physicalPages, pinned + kMaxUploadsPerFrame), 1u, kMaxLayers);
    if (pinned >= layers_) {
        FX_ERROR("vt: %u textured materials exceed the %u-page physical cache", pinned, layers_);
        return false;
    }
    for (uint32_t c = 0; c < 3; ++c) {
        if (!SDL_GPUTextureSupportsFormat(device_, kChannelFormats[c], SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            FX_ERROR("vt: BC7/BC5 texture arrays are not supported on this device");
            return false;
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = kChannelFormats[c];
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = info.height = fpak::kVtPhysicalTexels;
        info.layer_count_or_depth = layers_;
        info.num_levels = 1;
        physical_[c] = gpumem::createTexture("Virtual Texturing", device_, &info);
        if (!physical_[c]) {
            FX_ERROR("vt: physical cache: %s", SDL_GetError());
            return false;
        }
    }
    gpumem::setTextureName(device_, physical_[0], "vt albedo");
    gpumem::setTextureName(device_, physical_[1], "vt normal");
    gpumem::setTextureName(device_, physical_[2], "vt orm");

    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &s);

    pageTableOffset_ = materialCount * uint32_t(sizeof(fpak::MaterialGpu));
    paramsOffset_ = (pageTableOffset_ + pageCount_ * 4 + 15) & ~15u;  // (R4: surface shaders' params, zero)
    const uint32_t bufferBytes = paramsOffset_ + materialCount * 32;  // (8 floats each: editor/material_library.h)
    maskWords_ = std::max(1u, (pageCount_ + 31) / 32);
    auto buffer = [&](SDL_GPUBufferUsageFlags usage, uint32_t size, const char* name) {
        SDL_GPUBufferCreateInfo info = {usage, std::max(size, 16u), 0};
        SDL_GPUBuffer* b = gpumem::createBuffer("Virtual Texturing", device_, &info);
        if (b)
            gpumem::setBufferName(device_, b, name);
        return b;
    };
    const auto rw = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    materialData_ = buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, bufferBytes,
                           "vt materials + page table + params");
    feedbackList_ = buffer(rw, (1 + kFeedbackCapacity) * 4, "vt feedback list");
    feedbackMask_ = buffer(rw, maskWords_ * 4, "vt feedback mask");
    SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kMaxUploadsPerFrame * fpak::kVtPageBytes, 0};
    pageStaging_ = gpumem::createTransferBuffer("Virtual Texturing", device_, &t, "vt page staging");
    t.size = std::max(bufferBytes, 16u);
    tableStaging_ = gpumem::createTransferBuffer("Virtual Texturing", device_, &t, "vt page table staging");
    if (!sampler_ || !materialData_ || !feedbackList_ || !feedbackMask_ || !pageStaging_ || !tableStaging_)
        return false;

    layerOwner_.assign(layers_, -1);
    layerUsed_.assign(layers_, 0);
    layerPinned_.assign(layers_, 0);
    slots_.resize(kReadSlots);
    for (Slot& slot : slots_)
        slot.data.resize(fpak::kVtPageBytes);

    // Materials + an empty page table, then the pinned pages (published with the table).
    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tableStaging_, false));
    std::memcpy(mapped, materials.data(), pageTableOffset_);
    std::memset(mapped + pageTableOffset_, 0, bufferBytes - pageTableOffset_);
    SDL_UnmapGPUTransferBuffer(device_, tableStaging_);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {tableStaging_, 0};
    const SDL_GPUBufferRegion dst = {materialData_, 0, bufferBytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    stats_.capacity = layers_;
    if (pageCount_ && !loadPinned(fvtPath, pinnedEntries))
        return false;
    FX_INFO("vt: %u pages in %s, %u-page physical cache (%.0f MB), %u pinned", pageCount_, fvtPath, layers_,
            double(layers_) * fpak::kVtPageBytes / (1 << 20), pinned);
    return true;
}

bool VirtualTextureSystem::loadPinned(const char* path, const std::vector<uint32_t>& entries)
{
    // One page per material, read synchronously at boot (tens of KB each) and never evicted.
    SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(entries.size()) * fpak::kVtPageBytes, 0};
    SDL_GPUTransferBuffer* staging = gpumem::createTransferBuffer("Virtual Texturing", device_, &t, "vt pinned pages upload");
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (!staging || !io) {
        FX_ERROR("vt: pinned pages: %s", SDL_GetError());
        if (io)
            SDL_CloseIO(io);
        gpumem::releaseTransferBuffer(device_, staging);
        return false;
    }
    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, staging, false));
    bool ok = mapped != nullptr;
    for (size_t i = 0; ok && i < entries.size(); ++i)
        ok = SDL_SeekIO(io, Sint64(dataOffset_ + uint64_t(entries[i]) * fpak::kVtPageBytes), SDL_IO_SEEK_SET) >= 0 &&
             SDL_ReadIO(io, mapped + i * fpak::kVtPageBytes, fpak::kVtPageBytes) == fpak::kVtPageBytes;
    SDL_CloseIO(io);
    if (mapped)
        SDL_UnmapGPUTransferBuffer(device_, staging);
    if (!ok) {
        FX_ERROR("vt: cannot read pinned pages from %s", path);
        gpumem::releaseTransferBuffer(device_, staging);
        return false;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t i = 0; i < entries.size(); ++i) {
        for (uint32_t c = 0; c < 3; ++c) {
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = staging;
            src.offset = i * fpak::kVtPageBytes + c * fpak::kVtChannelBytes;
            src.pixels_per_row = src.rows_per_layer = fpak::kVtPhysicalTexels;
            SDL_GPUTextureRegion dst = {};
            dst.texture = physical_[c];
            dst.layer = i;
            dst.w = dst.h = fpak::kVtPhysicalTexels;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        const uint32_t e = entries[i];
        layerOwner_[i] = int32_t(e);
        layerPinned_[i] = 1;
        pageTable_[e] = kPageResident | i;
        state_[e] = kResident;
        dirtyMin_ = std::min(dirtyMin_, e);
        dirtyMax_ = std::max(dirtyMax_, e);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    gpumem::releaseTransferBuffer(device_, staging);  // freed once the copy retires
    return true;
}

void VirtualTextureSystem::shutdown()
{
    if (!device_)
        return;
    if (ioQueue_) {
        // Only reads still in flight produce a result. A slot stays busy after its read has
        // completed until its page is uploaded, so waiting per busy slot can block forever.
        for (; pendingReads_ > 0; --pendingReads_) {
            SDL_AsyncIOOutcome o;
            SDL_WaitAsyncIOResult(ioQueue_, &o, -1);
        }
        if (file_) {
            SDL_CloseAsyncIO(file_, false, ioQueue_, nullptr);
            SDL_AsyncIOOutcome o;
            SDL_WaitAsyncIOResult(ioQueue_, &o, -1);
        }
        SDL_DestroyAsyncIOQueue(ioQueue_);
    }
    for (SDL_GPUTexture* t : physical_)
        gpumem::releaseTexture(device_, t);
    for (SDL_GPUBuffer* b : {materialData_, feedbackList_, feedbackMask_})
        gpumem::releaseBuffer(device_, b);
    gpumem::releaseTransferBuffer(device_, pageStaging_);
    gpumem::releaseTransferBuffer(device_, tableStaging_);
    SDL_ReleaseGPUSampler(device_, sampler_);
    device_ = nullptr;
}

int32_t VirtualTextureSystem::allocateLayer(uint64_t frame)
{
    int32_t best = -1;
    uint64_t bestUsed = UINT64_MAX;
    for (uint32_t l = 0; l < layers_; ++l) {
        if (layerPinned_[l])
            continue;
        if (layerOwner_[l] < 0)
            return int32_t(l);
        if (layerUsed_[l] < bestUsed) {
            bestUsed = layerUsed_[l];
            best = int32_t(l);
        }
    }
    if (best < 0 || bestUsed + kEvictAge > frame)
        return -1;  // everything is in active use: the cache is too small for this view
    const uint32_t old = uint32_t(layerOwner_[best]);
    pageTable_[old] = 0;
    state_[old] = kIdle;
    dirtyMin_ = std::min(dirtyMin_, old);
    dirtyMax_ = std::max(dirtyMax_, old);
    layerOwner_[best] = -1;
    ++stats_.evictedLastFrame;
    return best;
}

void VirtualTextureSystem::recordUploads(SDL_GPUCommandBuffer* cmd, const std::vector<uint32_t>& slots,
                                         const std::vector<int32_t>& layers)
{
    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, pageStaging_, true));
    for (size_t i = 0; i < slots.size(); ++i)
        std::memcpy(mapped + i * fpak::kVtPageBytes, slots_[slots[i]].data.data(), fpak::kVtPageBytes);
    SDL_UnmapGPUTransferBuffer(device_, pageStaging_);
    FX_GPU_PASS(cmd, "vt page upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (size_t i = 0; i < slots.size(); ++i)
        for (uint32_t c = 0; c < 3; ++c) {
            SDL_GPUTextureTransferInfo src = {};
            src.transfer_buffer = pageStaging_;
            src.offset = uint32_t(i * fpak::kVtPageBytes + c * fpak::kVtChannelBytes);
            src.pixels_per_row = fpak::kVtPhysicalTexels;
            src.rows_per_layer = fpak::kVtPhysicalTexels;
            SDL_GPUTextureRegion dst = {};
            dst.texture = physical_[c];
            dst.layer = uint32_t(layers[i]);
            dst.w = dst.h = fpak::kVtPhysicalTexels;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
    SDL_EndGPUCopyPass(copy);
}

void VirtualTextureSystem::update(SDL_GPUCommandBuffer* cmd, uint64_t frame)
{
    FX_ZONE_CAT("VirtualTextureSystem::update", Rendering);
    FX_GPU_SECTION(cmd, "Virtual Texturing");
    stats_.uploadedLastFrame = 0;
    stats_.evictedLastFrame = 0;
    if (!pageCount_)
        return;

    // 1. Completed reads.
    SDL_AsyncIOOutcome o;
    while (SDL_GetAsyncIOResult(ioQueue_, &o)) {
        --pendingReads_;
        const uint32_t s = uint32_t(uintptr_t(o.userdata));
        if (s >= slots_.size())
            continue;
        Slot& slot = slots_[s];
        if (o.result == SDL_ASYNCIO_COMPLETE && o.bytes_transferred == fpak::kVtPageBytes) {
            state_[slot.entry] = kReady;
            ready_.push_back(s);
        } else {
            FX_WARN("vt: reading page %u failed: %s", slot.entry, SDL_GetError());
            state_[slot.entry] = kIdle;
            slot.busy = false;
        }
    }

    // 2. Upload ready pages into free or least-recently-used physical layers.
    std::vector<uint32_t> uploadSlots;
    std::vector<int32_t> uploadLayers;
    bool cacheFull = false;
    while (!ready_.empty() && uploadSlots.size() < kMaxUploadsPerFrame) {
        const uint32_t s = ready_.back();
        const int32_t layer = allocateLayer(frame);
        if (layer < 0) {
            cacheFull = true;
            break;
        }
        ready_.pop_back();
        const uint32_t e = slots_[s].entry;
        layerOwner_[layer] = int32_t(e);
        layerUsed_[layer] = frame;
        pageTable_[e] = kPageResident | uint32_t(layer);
        state_[e] = kResident;
        dirtyMin_ = std::min(dirtyMin_, e);
        dirtyMax_ = std::max(dirtyMax_, e);
        uploadSlots.push_back(s);
        uploadLayers.push_back(layer);
    }
    if (!uploadSlots.empty()) {
        recordUploads(cmd, uploadSlots, uploadLayers);
        for (uint32_t s : uploadSlots)
            slots_[s].busy = false;
        stats_.uploadedLastFrame = uint32_t(uploadSlots.size());
        stats_.uploadedTotal += uploadSlots.size();
    }

    // Under cache pressure (every page in active use), trade sharpness for coverage with a
    // global mip bias; relax it again once there is room.
    if (cacheFull)
        mipBias_ = std::min(mipBias_ + 0.25f, 4.0f);
    else if (mipBias_ > 0.0f && stats_.resident < layers_ * 7 / 8)
        mipBias_ = std::max(mipBias_ - 0.02f, 0.0f);

    // 3. Publish page-table changes (after the page data in the same command buffer).
    if (dirtyMin_ <= dirtyMax_) {
        const uint32_t count = dirtyMax_ - dirtyMin_ + 1;
        uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tableStaging_, true));
        std::memcpy(mapped, pageTable_.data() + dirtyMin_, count * 4);
        SDL_UnmapGPUTransferBuffer(device_, tableStaging_);
        FX_GPU_PASS(cmd, "vt page table");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src = {tableStaging_, 0};
        const SDL_GPUBufferRegion dst = {materialData_, pageTableOffset_ + dirtyMin_ * 4, count * 4};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        dirtyMin_ = UINT32_MAX;
        dirtyMax_ = 0;
    }

    // 4. Start new reads, coarsest mips first (queue is sorted so the back is coarsest).
    uint32_t inFlight = 0;
    for (uint32_t s = 0; s < slots_.size(); ++s) {
        Slot& slot = slots_[s];
        if (slot.busy) {
            ++inFlight;
            continue;
        }
        while (!queue_.empty() && state_[queue_.back()] != kQueued)
            queue_.pop_back();
        if (queue_.empty())
            break;
        const uint32_t e = queue_.back();
        queue_.pop_back();
        if (!SDL_ReadAsyncIO(file_, slot.data.data(), dataOffset_ + uint64_t(e) * fpak::kVtPageBytes, fpak::kVtPageBytes,
                             ioQueue_, reinterpret_cast<void*>(uintptr_t(s)))) {
            FX_WARN("vt: cannot start reading page %u: %s", e, SDL_GetError());
            state_[e] = kIdle;
            continue;
        }
        ++pendingReads_;
        slot.busy = true;
        slot.entry = e;
        state_[e] = kLoading;
        ++inFlight;
    }
    uint32_t resident = 0;
    std::memset(stats_.residentPerMip, 0, sizeof(stats_.residentPerMip));
    for (int32_t owner : layerOwner_)
        if (owner >= 0) {
            ++resident;
            ++stats_.residentPerMip[std::min<uint32_t>(entryMip_[owner], 7)];
        }
    stats_.resident = resident;
    stats_.inFlight = inFlight;
    stats_.queued = uint32_t(queue_.size());
}

void VirtualTextureSystem::recordFeedbackClear(SDL_GPUCommandBuffer* cmd, SDL_GPUComputePipeline* clearPipeline)
{
    const uint32_t u[4] = {maskWords_, 0, 0, 0};
    SDL_PushGPUComputeUniformData(cmd, 0, u, sizeof(u));
    SDL_GPUStorageBufferReadWriteBinding rw[2] = {{feedbackList_, false}, {feedbackMask_, false}};
    FX_GPU_PASS(cmd, "vt feedback clear");
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, 2);
    SDL_BindGPUComputePipeline(pass, clearPipeline);
    SDL_DispatchGPUCompute(pass, (maskWords_ + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(pass);
}

void VirtualTextureSystem::consumeFeedback(const uint32_t* list, uint64_t frame)
{
    if (!pageCount_)
        return;
    // The newest feedback replaces the queue: pages nobody asked for again are dropped.
    for (uint32_t e : queue_)
        if (state_[e] == kQueued)
            state_[e] = kIdle;
    queue_.clear();
    const uint32_t count = std::min(list[0], kFeedbackCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = list[1 + i];
        if (e >= pageCount_)
            continue;
        if (state_[e] == kResident)
            layerUsed_[pageTable_[e] & 0xffff] = std::max(layerUsed_[pageTable_[e] & 0xffff], frame);
        else if (state_[e] == kIdle) {
            state_[e] = kQueued;
            queue_.push_back(e);
        }
    }
    std::stable_sort(queue_.begin(), queue_.end(), [&](uint32_t a, uint32_t b) { return entryMip_[a] < entryMip_[b]; });
    stats_.requestsLastFeedback = count;
}

void VirtualTextureSystem::request(const std::vector<uint32_t>& entries, uint64_t frame)
{
    if (!pageCount_)
        return;
    const size_t before = queue_.size();
    for (const uint32_t e : entries) {
        if (e >= pageCount_)
            continue;
        if (state_[e] == kResident)
            layerUsed_[pageTable_[e] & 0xffff] = std::max(layerUsed_[pageTable_[e] & 0xffff], frame);
        else if (state_[e] == kIdle) {
            state_[e] = kQueued;
            queue_.push_back(e);
        }
    }
    std::stable_sort(queue_.begin() + std::ptrdiff_t(before), queue_.end(),
                     [&](uint32_t a, uint32_t b) { return entryMip_[a] > entryMip_[b]; });
}
}  // namespace fx
