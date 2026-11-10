#include "render/surface_layers.h"

#include "core/log.h"
#include "gpu/gpu_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr uint32_t kMaskBytes = SurfaceLayers::kMaskSize * SurfaceLayers::kMaskSize * 4;

}  // namespace

bool SurfaceLayers::init(SDL_GPUDevice* device, uint32_t maxLayers)
{
    device_ = device;
    maxLayers_ = std::max(maxLayers, 1u);
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device, &s);
    return sampler_ && make(1);
}

void SurfaceLayers::shutdown()
{
    if (!device_)
        return;
    if (masks_)
        gpumem::releaseTexture(device_, masks_);
    if (records_)
        gpumem::releaseTexture(device_, records_);
    if (sampler_)
        SDL_ReleaseGPUSampler(device_, sampler_);
    if (upload_)
        SDL_ReleaseGPUTransferBuffer(device_, upload_);
    *this = SurfaceLayers{};
}

bool SurfaceLayers::make(uint32_t capacity)
{
    if (masks_)
        gpumem::releaseTexture(device_, masks_);
    if (records_)
        gpumem::releaseTexture(device_, records_);
    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    t.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    t.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    t.width = t.height = kMaskSize;
    t.layer_count_or_depth = capacity;
    t.num_levels = 1;
    masks_ = gpumem::createTexture("Surface layers", device_, &t);
    t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
    t.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    t.width = capacity;
    t.height = 1;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    records_ = gpumem::createTexture("Surface layers", device_, &t);
    if (!masks_ || !records_)
        return false;
    gpumem::setTextureName(device_, masks_, "surface layer masks");
    gpumem::setTextureName(device_, records_, "surface layer records");
    capacity_ = capacity;
    taken_.resize(capacity, 0);
    cpuMasks_.resize(capacity);
    cpuRecords_.resize(size_t(capacity) * 4, 0);
    // Everything again (a new array starts undefined): every layer's mask (empty ones too) and the records.
    dirtyMasks_.clear();
    for (uint32_t l = 0; l < capacity; ++l)
        dirtyMasks_.push_back(l);
    recordsDirty_ = true;
    return true;
}

int32_t SurfaceLayers::acquire()
{
    for (uint32_t l = 0; l < maxLayers_; ++l) {
        if (l < taken_.size() && taken_[l])
            continue;
        if (l >= capacity_)
            grow_ = true;  // (made bigger at the next upload)
        if (l >= taken_.size()) {
            taken_.resize(l + 1, 0);
            cpuMasks_.resize(l + 1);
            cpuRecords_.resize(size_t(l + 1) * 4, 0);
        }
        taken_[l] = 1;
        ++used_;
        const uint32_t none[3] = {kNone, kNone, kNone};
        setRecord(int32_t(l), none, 0.15f);
        setMask(int32_t(l), nullptr);
        return int32_t(l);
    }
    return -1;
}

void SurfaceLayers::release(int32_t layer)
{
    if (layer < 0 || uint32_t(layer) >= taken_.size() || !taken_[size_t(layer)])
        return;
    taken_[size_t(layer)] = 0;
    cpuMasks_[size_t(layer)].clear();
    --used_;
}

void SurfaceLayers::setRecord(int32_t layer, const uint32_t materials[3], float blend)
{
    if (layer < 0 || uint32_t(layer) >= taken_.size())
        return;
    uint32_t* r = &cpuRecords_[size_t(layer) * 4];
    const uint32_t b = uint32_t(std::lround(std::clamp(blend, 0.0f, 1.0f) * 65535.0f));
    r[0] = (materials[0] & 0xffffu) | (materials[1] & 0xffffu) << 16;
    r[1] = (materials[2] & 0xffffu) | b << 16;
    r[2] = uint32_t(layer);
    r[3] = 0;
    recordsDirty_ = true;
}

void SurfaceLayers::setMask(int32_t layer, const uint8_t* rgba)
{
    if (layer < 0 || uint32_t(layer) >= taken_.size())
        return;
    std::vector<uint8_t>& m = cpuMasks_[size_t(layer)];
    if (rgba)
        m.assign(rgba, rgba + kMaskBytes);
    else
        m.clear();
    if (std::find(dirtyMasks_.begin(), dirtyMasks_.end(), uint32_t(layer)) == dirtyMasks_.end())
        dirtyMasks_.push_back(uint32_t(layer));
}

void SurfaceLayers::upload(SDL_GPUCommandBuffer* cmd)
{
    if (!device_)
        return;
    if (grow_) {
        grow_ = false;
        uint32_t want = capacity_;
        while (want < taken_.size())
            want *= 2;
        if (!make(std::min(want, maxLayers_)))
            FX_WARN("surface layers: no room for %u masks", want);
    }
    if (dirtyMasks_.empty() && !recordsDirty_)
        return;
    // One transfer buffer: the dirty masks (a few a frame at most), then the records.
    std::sort(dirtyMasks_.begin(), dirtyMasks_.end());
    const uint32_t masks = std::min<uint32_t>(uint32_t(dirtyMasks_.size()), 4u);  // (the rest next frame)
    const uint32_t bytes = masks * kMaskBytes + capacity_ * 16;
    if (bytes > uploadBytes_) {
        if (upload_)
            SDL_ReleaseGPUTransferBuffer(device_, upload_);
        SDL_GPUTransferBufferCreateInfo tb = {};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = bytes;
        upload_ = SDL_CreateGPUTransferBuffer(device_, &tb);
        uploadBytes_ = upload_ ? bytes : 0;
        if (!upload_)
            return;
    }
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    if (!mapped)
        return;
    for (uint32_t i = 0; i < masks; ++i) {
        const uint32_t l = dirtyMasks_[i];
        const std::vector<uint8_t>& m = l < cpuMasks_.size() ? cpuMasks_[l] : std::vector<uint8_t>{};
        if (m.size() == kMaskBytes)
            std::memcpy(mapped + size_t(i) * kMaskBytes, m.data(), kMaskBytes);
        else
            std::memset(mapped + size_t(i) * kMaskBytes, 0, kMaskBytes);
    }
    std::memcpy(mapped + size_t(masks) * kMaskBytes, cpuRecords_.data(), std::min<size_t>(cpuRecords_.size() * 4, size_t(capacity_) * 16));
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t i = 0; i < masks; ++i) {
        SDL_GPUTextureTransferInfo src = {upload_, i * kMaskBytes, kMaskSize, kMaskSize};
        SDL_GPUTextureRegion dst = {};
        dst.texture = masks_;
        dst.layer = dirtyMasks_[i];
        dst.w = dst.h = kMaskSize;
        dst.d = 1;
        if (dirtyMasks_[i] < capacity_)
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    SDL_GPUTextureTransferInfo src = {upload_, masks * kMaskBytes, capacity_, 1};
    SDL_GPUTextureRegion dst = {};
    dst.texture = records_;
    dst.w = capacity_;
    dst.h = dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    dirtyMasks_.erase(dirtyMasks_.begin(), dirtyMasks_.begin() + masks);
    recordsDirty_ = false;
}

}  // namespace fx
