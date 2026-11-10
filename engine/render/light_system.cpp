#include "render/light_system.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "scene/scene.h"

#include "light_binning.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace fx {

namespace {

struct BinUniforms {
    float4x4 view;
    float proj[4];
    uint32_t screen[4];
    uint32_t grid[4];
};

// Range at which a light's contribution falls below a small fraction of the sun's.
// A light without a range (0) reaches until its light is too faint to see: its inverse-square
// falloff under 0.001 (a thousandth of a lux, a hundredth of the threshold before: lights faded
// out where they still lit things, as if culled past a distance). The window that takes it to
// zero there (lighting.slang distanceAttenuation) then changes nothing visible.
float rangeFromIntensity(const float color[3])
{
    const float i = std::max({color[0], color[1], color[2]});
    return std::sqrt(std::max(i, 1e-6f) / 0.001f);
}

}  // namespace

bool LightSystem::init(const GpuDevice& gpu, const ScenePak& pak, Scene& scene, uint32_t testLights, uint32_t shadowed,
                       const std::vector<fpak::LightGpu>& extra, float sunScale)
{
    FX_ZONE_CAT("LightSystem::init", Rendering);
    device_ = gpu.get();
    scene_ = &scene;
    clear_ = gpu.createComputePipeline(shaders::light_binning_cs_clear_tiles);
    bin_ = gpu.createComputePipeline(shaders::light_binning_cs_bin_lights);
    if (!clear_ || !bin_)
        return false;

    // Lights are entities (Scene::spawnLights), gathered every frame: the sun is the first
    // directional one.
    sunScale_ = sunScale;

    base_ = extra;
    extraCount_ = uint32_t(extra.size());
    // Stress-test lights: random colored points in the lower part of the scene (deterministic).
    const fpak::Header& h = pak.header();
    const float3 lo = {h.boundsMin[0], h.boundsMin[1], h.boundsMin[2]};
    const float3 ext = {h.boundsMax[0] - lo.x, h.boundsMax[1] - lo.y, h.boundsMax[2] - lo.z};
    const float size = std::max({ext.x, ext.y, ext.z});
    uint32_t rng = 0x9E3779B9u;
    auto rand01 = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return float(rng & 0xffffff) / float(0x1000000);
    };
    for (uint32_t i = 0; i < testLights && base_.size() < kMaxLights; ++i) {
        fpak::LightGpu l = {};
        l.positionRange[0] = lo.x + ext.x * rand01();
        l.positionRange[1] = lo.y + ext.y * (0.02f + 0.3f * rand01());
        l.positionRange[2] = lo.z + ext.z * rand01();
        const float range = size * (0.03f + 0.05f * rand01());
        l.positionRange[3] = range;
        const float hue = rand01() * 6.0f;
        const float rgb[3] = {std::clamp(std::fabs(hue - 3.0f) - 1.0f, 0.0f, 1.0f),
                              std::clamp(2.0f - std::fabs(hue - 2.0f), 0.0f, 1.0f),
                              std::clamp(2.0f - std::fabs(hue - 4.0f), 0.0f, 1.0f)};
        const float intensity = range * range * 0.12f;
        for (int c = 0; c < 3; ++c)
            l.color[c] = (0.25f + 0.75f * rgb[c]) * intensity;
        l.color[3] = float(fpak::kLightPoint);
        l.direction[3] = l.params[0] = -1.0f;
        base_.push_back(l);
    }
    // Shadows within `shadowed`: the extra lights, the scene's casters, then test lights.
    uint32_t sceneCasters = 0;
    scene.world().each([&](flecs::entity, const Light& l) { sceneCasters += (l.flags & 1u) != 0 && l.type != 2; });
    const uint32_t budget = std::min(shadowed, kMaxShadowSlots);
    baseShadows_.assign(base_.size(), false);
    for (uint32_t i = 0; i < base_.size(); ++i)
        baseShadows_[i] = i < extraCount_ ? i < budget : (i - extraCount_) + std::min(extraCount_, budget) + sceneCasters < budget;

    upload_.resize(kZBins + kMaxLights);
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = (kMaxLights + kMaxShadowSlots) * sizeof(fpak::LightGpu);  // (then the shadow slots' lights as rendered)
    lights_ = gpumem::createBuffer("Lights", device_, &info);
    // Staging: the z-bins + order each frame, then the light array when it changed.
    SDL_GPUTransferBufferCreateInfo t = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(upload_.size() * 4) + info.size, 0};
    staging_ = gpumem::createTransferBuffer("Lights", device_, &t, "light upload");
    if (!lights_ || !staging_)
        return false;
    gpumem::setBufferName(device_, lights_, "lights");
    gather();
    FX_INFO("lights: %u punctual (%u scene, %u test), %zu shadowed, sun dir (%.2f, %.2f, %.2f)", count_,
            count_ - uint32_t(base_.size()), testLights, slotKeys_.size(), double(sunDirection_.x), double(sunDirection_.y),
            double(sunDirection_.z));
    return true;
}

void LightSystem::gather()
{
    // Keys: extra lights 1<<62 | i, test lights 1<<61 | i, entities their flecs id.
    constexpr uint64_t kExtraKey = 1ull << 62, kTestKey = 1ull << 61;
    cpuLights_.clear();
    keys_.clear();
    wantsShadow_.clear();
    const auto moved = [&](uint32_t i) {
        fpak::LightGpu l = base_[i];
        if (motion_ >= 0.0f) {
            const float a = motion_ * 6.2832f + float(i);
            l.positionRange[0] += std::cos(a);
            l.positionRange[2] += std::sin(a);
        }
        return l;
    };
    for (uint32_t i = 0; i < extraCount_; ++i) {
        cpuLights_.push_back(moved(i));
        keys_.push_back(kExtraKey | i);
        wantsShadow_.push_back(baseShadows_[i]);
    }
    // Entities in id order (deterministic across runs). The sun: the oldest directional light
    // (none, or hidden: no sun).
    std::vector<std::pair<flecs::entity_t, fpak::LightGpu>> entities;
    flecs::entity_t sun = 0;
    sunColor_ = {0, 0, 0};
    scene_->world().each([&](flecs::entity e, const Light& l, const WorldTransform& t) {
        if (l.type == 2) {
            if (sun && e.id() > sun)
                return;
            sun = e.id();
            const float* m = t.rows;
            const float3 toward = normalize(float3{m[2], m[6], m[10]});  // +Z: back toward the sun
            if (length(toward) > 0.5f)
                sunDirection_ = toward;
            const float k = (l.flags & 2u) ? 0.0f : std::max(l.intensity, 0.0f) * sunScale_;
            sunColor_ = {std::max(l.color[0], 0.0f) * k, std::max(l.color[1], 0.0f) * k, std::max(l.color[2], 0.0f) * k};
            return;
        }
        if (l.flags & 2u)
            return;  // hidden in the editor
        fpak::LightGpu g = {};
        const float* m = t.rows;  // rows of 3x4: column 3 = position, -column 2 = direction
        g.positionRange[0] = m[3];
        g.positionRange[1] = m[7];
        g.positionRange[2] = m[11];
        for (int c = 0; c < 3; ++c)
            g.color[c] = std::max(l.color[c], 0.0f) * std::max(l.intensity, 0.0f);
        g.positionRange[3] = l.range > 0.0f ? l.range : rangeFromIntensity(g.color);
        g.color[3] = float(l.type == 1 ? fpak::kLightSpot : fpak::kLightPoint);
        const float3 d = normalize(float3{-m[2], -m[6], -m[10]});
        g.direction[0] = d.x;
        g.direction[1] = d.y;
        g.direction[2] = d.z;
        if (l.type == 1) {
            const float outer = std::clamp(l.outerAngle, 0.5f, 89.0f), inner = std::clamp(l.innerAngle, 0.0f, outer - 0.25f);
            g.direction[3] = std::cos(outer / 57.29578f);
            g.params[0] = std::cos(inner / 57.29578f);
        } else {
            g.direction[3] = g.params[0] = -1.0f;
        }
        g.params[2] = (l.flags & 1u) ? 1.0f : 0.0f;  // wants a shadow (cleared below)
        entities.emplace_back(e.id(), g);
    });
    std::sort(entities.begin(), entities.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, g] : entities) {
        if (cpuLights_.size() >= kMaxLights)
            break;
        cpuLights_.push_back(g);
        cpuLights_.back().params[2] = 0.0f;
        keys_.push_back(id);
        wantsShadow_.push_back(g.params[2] > 0.0f);
    }
    for (uint32_t i = extraCount_; i < base_.size() && cpuLights_.size() < kMaxLights; ++i) {
        cpuLights_.push_back(moved(i));
        keys_.push_back(kTestKey | (i - extraCount_));
        wantsShadow_.push_back(baseShadows_[i]);
    }
    persistent_ = uint32_t(cpuLights_.size());
    constexpr uint64_t kTransientKey = 1ull << 60;
    for (uint32_t i = 0; i < transient_.size() && cpuLights_.size() < kMaxLights; ++i) {
        cpuLights_.push_back(transient_[i]);
        keys_.push_back(kTransientKey | i);
        wantsShadow_.push_back(false);
    }
    count_ = uint32_t(cpuLights_.size());
    assignSlots();
    if (cpuLights_.size() != uploaded_.size() ||
        std::memcmp(cpuLights_.data(), uploaded_.data(), cpuLights_.size() * sizeof(fpak::LightGpu)) != 0)
        uploadNeeded_ = true;
}

void LightSystem::assignSlots()
{
    std::unordered_map<uint64_t, uint32_t> indexOf;
    for (uint32_t i = 0; i < count_; ++i)
        indexOf[keys_[i]] = i;
    // Keep the slots whose light still casts; a freed slot takes the last slot's light.
    std::vector<uint64_t> slots = slotKeys_;
    std::vector<fpak::LightGpu> shapes = slotShape_;
    uint32_t forced = 0;
    for (uint32_t s = 0; s < slots.size();) {
        auto it = indexOf.find(slots[s]);
        if (it != indexOf.end() && wantsShadow_[it->second]) {
            ++s;
            continue;
        }
        slots[s] = slots.back();
        shapes[s] = shapes.back();
        slots.pop_back();
        shapes.pop_back();
        if (s < slots.size())
            forced |= 1u << s;  // a different light now: its pages are someone else's
    }
    // New casters, in light order, while slots are free.
    std::unordered_map<uint64_t, bool> slotted;
    for (const uint64_t k : slots)
        slotted[k] = true;
    for (uint32_t i = 0; i < count_ && slots.size() < kMaxShadowSlots; ++i)
        if (wantsShadow_[i] && !slotted.count(keys_[i])) {
            forced |= 1u << uint32_t(slots.size());
            slots.push_back(keys_[i]);
            shapes.push_back({});
        }
    for (fpak::LightGpu& l : cpuLights_)
        l.params[1] = -1.0f;
    shadowSlots_.assign(slots.size(), 0);
    uint32_t waiting = 0;
    for (uint32_t s = 0; s < slots.size(); ++s) {
        const uint32_t i = indexOf[slots[s]];
        fpak::LightGpu& l = cpuLights_[i];
        l.params[1] = float(s);
        shadowSlots_[s] = i;
        // What the slot's pages depend on: position, range, direction, cone (not colour).
        const fpak::LightGpu& was = shapes[s];
        if (std::memcmp(l.positionRange, was.positionRange, sizeof(l.positionRange)) != 0 ||
            std::memcmp(l.direction, was.direction, sizeof(l.direction)) != 0 || l.color[3] != was.color[3])
            waiting |= 1u << s;
    }
    waiting &= ~forced;
    changedSlots_ = chooser_ ? (chooser_(waiting, forced) & (waiting | forced)) | forced : waiting | forced;
    for (uint32_t s = 0; s < slots.size(); ++s)
        if (changedSlots_ & (1u << s)) {
            shapes[s] = cpuLights_[shadowSlots_[s]];
            slotShapesChanged_ = true;
        }
    slotKeys_ = std::move(slots);
    slotShape_ = std::move(shapes);
}

void LightSystem::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseBuffer(device_, lights_);
    gpumem::releaseBuffer(device_, grid_);
    gpumem::releaseTransferBuffer(device_, staging_);
    SDL_ReleaseGPUComputePipeline(device_, clear_);
    SDL_ReleaseGPUComputePipeline(device_, bin_);
    device_ = nullptr;
}

bool LightSystem::ensureGrid(uint32_t width, uint32_t height)
{
    if (grid_ && width == width_ && height == height_ && words_ == std::max(1u, (count_ + 31) / 32))
        return true;
    gpumem::releaseBuffer(device_, grid_);
    tilesX_ = (width + kTileSize - 1) / kTileSize;
    tilesY_ = (height + kTileSize - 1) / kTileSize;
    words_ = std::max(1u, (count_ + 31) / 32);
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    info.size = (tileOffset() + tilesX_ * tilesY_ * words_) * 4;
    grid_ = gpumem::createBuffer("Lights", device_, &info);
    gpumem::setBufferName(device_, grid_, "light grid");
    if (grid_)
        gpumem::setBufferName(device_, grid_, "light grid (z-bins, order, tile masks)");
    width_ = width;
    height_ = height;
    return grid_ != nullptr;
}

void LightSystem::update(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height)
{
    FX_ZONE_CAT("LightSystem::update", Rendering);
    gather();
    order_.resize(count_);
    zMin_.resize(count_);
    zMax_.resize(count_);
    if (!ensureGrid(width, height))
        return;

    // Sort by view depth of the sphere's near side; z-bins store the sorted-index range.
    float maxDepth = 1.0f;
    for (uint32_t i = 0; i < count_; ++i) {
        const fpak::LightGpu& l = cpuLights_[i];
        const float z = dot(float3{l.positionRange[0], l.positionRange[1], l.positionRange[2]} - camera.position,
                            camera.forward);
        zMin_[i] = std::max(z - l.positionRange[3], 0.0f);
        zMax_[i] = z + l.positionRange[3];
        maxDepth = std::max(maxDepth, zMax_[i]);
    }
    std::iota(order_.begin(), order_.end(), 0u);
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return zMin_[a] < zMin_[b]; });
    binScale_ = float(kZBins) / maxDepth;
    for (uint32_t b = 0; b < kZBins; ++b)
        upload_[b] = 0xffffu;  // empty: first > last
    for (uint32_t s = 0; s < count_; ++s) {
        const uint32_t i = order_[s];
        upload_[kZBins + s] = i;
        if (zMax_[i] <= 0.0f)
            continue;
        const uint32_t b0 = std::min(uint32_t(zMin_[i] * binScale_), kZBins - 1);
        const uint32_t b1 = std::min(uint32_t(zMax_[i] * binScale_), kZBins - 1);
        for (uint32_t b = b0; b <= b1; ++b) {
            uint32_t& r = upload_[b];
            const uint32_t first = std::min(r & 0xffff, s), last = r == 0xffffu ? s : std::max(r >> 16, s);
            r = first | last << 16;
        }
    }

    const uint32_t uploadWords = kZBins + count_;
    const uint32_t lightsAt = uint32_t(upload_.size() * 4), lightBytes = count_ * uint32_t(sizeof(fpak::LightGpu));
    const bool lightsChanged = uploadNeeded_ && count_ > 0;
    const bool shapesChanged = slotShapesChanged_ && !slotShape_.empty();
    const uint32_t shapesAt = lightsAt + kMaxLights * uint32_t(sizeof(fpak::LightGpu));
    const uint32_t shapeBytes = uint32_t(slotShape_.size() * sizeof(fpak::LightGpu));
    auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, staging_, true));
    std::memcpy(p, upload_.data(), uploadWords * 4);
    if (lightsChanged)
        std::memcpy(p + lightsAt, cpuLights_.data(), lightBytes);
    if (shapesChanged)
        std::memcpy(p + shapesAt, slotShape_.data(), shapeBytes);
    SDL_UnmapGPUTransferBuffer(device_, staging_);
    FX_GPU_SECTION(cmd, "Lights");
    FX_GPU_PASS(cmd, "light bins upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {staging_, 0};
    const SDL_GPUBufferRegion dst = {grid_, 0, uploadWords * 4};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    if (lightsChanged) {
        const SDL_GPUTransferBufferLocation from = {staging_, lightsAt};
        const SDL_GPUBufferRegion to = {lights_, 0, lightBytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        uploaded_ = cpuLights_;
    }
    if (shapesChanged) {
        const SDL_GPUTransferBufferLocation from = {staging_, shapesAt};
        const SDL_GPUBufferRegion to = {lights_, kMaxLights * uint32_t(sizeof(fpak::LightGpu)), shapeBytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
    }
    slotShapesChanged_ = false;
    uploadNeeded_ = false;
    SDL_EndGPUCopyPass(copy);

    BinUniforms u = {};
    u.view = camera.view;
    u.proj[0] = camera.p00;
    u.proj[1] = camera.p11;
    u.proj[2] = camera.zNear;
    u.screen[0] = width;
    u.screen[1] = height;
    u.screen[2] = tilesX_;
    u.screen[3] = tilesY_;
    u.grid[0] = orderOffset();
    u.grid[1] = tileOffset();
    u.grid[2] = words_;
    u.grid[3] = count_;
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    SDL_GPUStorageBufferReadWriteBinding rw = {grid_, false};
    FX_GPU_PASS(cmd, "light tiles clear");
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
    SDL_BindGPUComputePipeline(pass, clear_);
    SDL_DispatchGPUCompute(pass, (tilesX_ * tilesY_ * words_ + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(pass);
    if (count_) {
        FX_GPU_PASS(cmd, "light binning");
        pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, bin_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &lights_, 1);
        SDL_DispatchGPUCompute(pass, (count_ + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
}

}  // namespace fx
