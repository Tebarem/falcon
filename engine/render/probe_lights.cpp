#include "render/probe_lights.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

int32_t floorDiv(float v, float s) { return int32_t(std::floor(v / s)); }

bool same(const fpak::LightGpu& a, const fpak::LightGpu& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

void rangeBox(const fpak::LightGpu& l, float box[6])
{
    for (int a = 0; a < 3; ++a) {
        box[a] = l.positionRange[a] - l.positionRange[3];
        box[3 + a] = l.positionRange[a] + l.positionRange[3];
    }
}

}  // namespace

bool ProbeLights::init(SDL_GPUDevice* device)
{
    device_ = device;
    built_ = false;
    return ensure(64 * 1024);
}

void ProbeLights::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseBuffer(device_, buffer_);
    gpumem::releaseTransferBuffer(device_, upload_);
    buffer_ = nullptr;
    upload_ = nullptr;
    capacity_ = 0;
    last_.clear();
    built_ = false;
    device_ = nullptr;
}

bool ProbeLights::ensure(uint32_t bytes)
{
    if (bytes <= capacity_ && buffer_ && upload_)
        return true;
    const uint32_t size = std::max<uint32_t>(std::bit_ceil(bytes), 64 * 1024);
    gpumem::releaseBuffer(device_, buffer_);
    gpumem::releaseTransferBuffer(device_, upload_);
    const SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, size, 0};
    buffer_ = gpumem::createBuffer("GI", device_, &b);
    if (buffer_)
        gpumem::setBufferName(device_, buffer_, "probe lights");
    const SDL_GPUTransferBufferCreateInfo tb = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    upload_ = gpumem::createTransferBuffer("GI", device_, &tb, "probe lights upload");
    capacity_ = buffer_ && upload_ ? size : 0;
    if (!capacity_)
        FX_ERROR("probe lights: %s", SDL_GetError());
    return capacity_ != 0;
}

// The buffer (words): the header (the grid's lowest cell xyz, the cell size, the lights listed,
// where the cells, the lists and the lights start), a word per cell (its list's start << 6 | its
// count), the lists (indices into the lights), the lights (LightGpu, 16 words each).
void ProbeLights::build()
{
    std::vector<uint32_t> count(kCells, 0);
    std::vector<std::pair<uint32_t, uint32_t>> entries;  // cell, listed light
    std::vector<uint32_t> listed;                        // indices into lights_
    for (uint32_t i = 0; i < lights_.size(); ++i) {
        const fpak::LightGpu& l = lights_[i];
        const float r = l.positionRange[3];
        if (!(r > 0.0f) || (l.color[0] <= 0.0f && l.color[1] <= 0.0f && l.color[2] <= 0.0f))
            continue;
        int32_t lo[3], hi[3];
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::max(floorDiv(l.positionRange[a] - r, cell_) - origin_[a], 0);
            hi[a] = std::min(floorDiv(l.positionRange[a] + r, cell_) - origin_[a], int32_t(kDims[a]) - 1);
            inside = inside && lo[a] <= hi[a];
        }
        if (!inside)
            continue;
        uint32_t local = ~0u;
        for (int32_t z = lo[2]; z <= hi[2]; ++z)
            for (int32_t y = lo[1]; y <= hi[1]; ++y)
                for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                    const int32_t c[3] = {x, y, z};
                    float d2 = 0.0f;  // the light's distance from the cell, squared
                    for (int a = 0; a < 3; ++a) {
                        const float cmin = float(origin_[a] + c[a]) * cell_, cmax = cmin + cell_;
                        const float d = std::max({cmin - l.positionRange[a], 0.0f, l.positionRange[a] - cmax});
                        d2 += d * d;
                    }
                    if (d2 > r * r)
                        continue;
                    const uint32_t cell = uint32_t(x) + kDims[0] * (uint32_t(y) + kDims[1] * uint32_t(z));
                    if (count[cell] >= kPerCell)
                        continue;
                    if (local == ~0u) {
                        local = uint32_t(listed.size());
                        listed.push_back(i);
                    }
                    ++count[cell];
                    entries.emplace_back(cell, local);
                }
    }
    listed_ = uint32_t(listed.size());
    const uint32_t cellsAt = kHeaderWords, listsAt = cellsAt + kCells, lightsAt = listsAt + uint32_t(entries.size());
    words_.assign(lightsAt + listed_ * 16, 0);
    for (int a = 0; a < 3; ++a)
        words_[a] = uint32_t(origin_[a]);
    std::memcpy(&words_[3], &cell_, 4);
    words_[4] = listed_;
    words_[5] = cellsAt;
    words_[6] = listsAt;
    words_[7] = lightsAt;
    std::vector<uint32_t> cursor(kCells);
    uint32_t offset = 0;
    for (uint32_t c = 0; c < kCells; ++c) {
        words_[cellsAt + c] = offset << 6 | count[c];
        cursor[c] = offset;
        offset += count[c];
    }
    for (const auto& [cell, local] : entries)
        words_[listsAt + cursor[cell]++] = local;
    for (uint32_t k = 0; k < listed_; ++k)
        std::memcpy(&words_[lightsAt + k * 16], &lights_[listed[k]], sizeof(fpak::LightGpu));
}

void ProbeLights::update(SDL_GPUCommandBuffer* cmd, const LightSystem& lights, const float3& camera, float cell,
                         const std::function<void(const float box[6])>& changed)
{
    if (!device_)
        return;
    FX_ZONE_CAT("ProbeLights::update", Rendering);
    const std::vector<fpak::LightGpu>& packed = lights.packed();
    const std::vector<uint64_t>& keys = lights.keys();
    const uint32_t n = std::min<uint32_t>(lights.persistentCount(), uint32_t(std::min(packed.size(), keys.size())));
    std::vector<fpak::LightGpu> now(packed.begin(), packed.begin() + n);
    for (fpak::LightGpu& l : now)
        l.params[1] = l.params[2] = l.params[3] = 0.0f;  // (shadow slots: not the probes' business)
    int32_t origin[3];
    for (int a = 0; a < 3; ++a)
        origin[a] = floorDiv((&camera.x)[a], cell) - int32_t(kDims[a] / 2);
    const bool lightsSame = now.size() == lights_.size() && std::equal(keys.begin(), keys.begin() + n, keys_.begin(), keys_.end()) &&
                            (n == 0 || std::memcmp(now.data(), lights_.data(), n * sizeof(fpak::LightGpu)) == 0);
    const bool placeSame = cell == cell_ && std::equal(origin, origin + 3, origin_);
    if (built_ && lightsSame && placeSame)
        return;
    if (!lightsSame || !built_) {
        std::unordered_map<uint64_t, fpak::LightGpu> next;
        next.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            next[keys[i]] = now[i];
        if (built_) {
            float box[6];
            for (const auto& [key, l] : next) {
                const auto it = last_.find(key);
                if (it != last_.end() && same(it->second, l))
                    continue;
                rangeBox(l, box);
                changed(box);
                if (it != last_.end()) {
                    rangeBox(it->second, box);
                    changed(box);
                }
            }
            for (const auto& [key, l] : last_)
                if (!next.count(key)) {
                    rangeBox(l, box);
                    changed(box);
                }
        }
        last_ = std::move(next);
        lights_ = std::move(now);
        keys_.assign(keys.begin(), keys.begin() + n);
    }
    std::copy(origin, origin + 3, origin_);
    cell_ = cell;
    built_ = true;
    build();
    const uint32_t bytes = uint32_t(words_.size() * 4);
    if (!ensure(bytes))
        return;
    void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
    std::memcpy(mapped, words_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "probe lights upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {upload_, 0};
    const SDL_GPUBufferRegion dst = {buffer_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
}

}  // namespace fx
