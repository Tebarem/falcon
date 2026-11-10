#include "render/probe_gi.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"
#include "render/ray_tracing/scene_bvh.h"
#include "render/virtual_texture.h"

#include "probe_blend.shader.h"
#include "probe_relocate.shader.h"
#include "probe_trace.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

// Mirrors ProbeUpdate in probe_common.slang.
struct ProbeUpdateUniforms {
    ProbeVolumeGpu volume;
    float sun[4];
    float sunColor[4];
    uint32_t params[4];
    uint32_t batch[4];
    float memory[4];
};

constexpr uint32_t kTilesX = 32 * 16, kTilesY = 32 * ProbeGi::kCascades;
constexpr uint32_t kIrradianceTile = 8, kDistanceTile = 16;
constexpr uint32_t kEntryFresh = 1u << 18, kEntryAgeShift = 19;
constexpr float kHysteresis = 0.985f;  // the most of a probe's light an update keeps
constexpr float kMinFrontFace = 0.2f;  // spacings: nearer front faces push a probe away
constexpr float kNormalBias = 0.2f, kViewBias = 0.1f;
constexpr uint32_t kGridShare[ProbeGi::kCascades] = {4, 2, 1};  // round robin: the finest most often
constexpr uint32_t kMaxQueuedRegion = 8192;  // probes a region change queues per grid (more: just their memory started again)
constexpr uint32_t kAllProbes = ProbeGi::kCascades * ProbeGi::kProbes;
constexpr uint32_t kScheduleEntries = (ProbeGi::kWarmRounds + 1) * kAllProbes;  // a warm-up's rounds and resets

int32_t floorDiv(float v, float s) { return int32_t(std::floor(v / s)); }

uint32_t slotOf(int32_t w, uint32_t d) { return uint32_t(((w % int32_t(d)) + int32_t(d)) % int32_t(d)); }

}  // namespace

bool ProbeGi::init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt)
{
    FX_ZONE_CAT("ProbeGi::init", Rendering);
    resetState();
    device_ = gpu.get();
    pak_ = &pak;
    vt_ = &vt;
    const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    for (SDL_GPUTextureFormat f : {SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT})
        if (!SDL_GPUTextureSupportsFormat(device_, f, SDL_GPU_TEXTURETYPE_2D, usage)) {
            FX_WARN("probe gi: the GPU can't read and write 16-bit float storage textures in one pass; no probe GI");
            return false;
        }
    trace_ = gpu.createComputePipeline(shaders::probe_trace_cs_probe_trace);
    blend_ = gpu.createComputePipeline(shaders::probe_blend_cs_probe_blend);
    relocate_ = gpu.createComputePipeline(shaders::probe_relocate_cs_probe_relocate);
    reset_ = gpu.createComputePipeline(shaders::probe_relocate_cs_reset);
    auto texture = [&](SDL_GPUTextureFormat format, uint32_t w, uint32_t h, const char* name) {
        SDL_GPUTextureCreateInfo t = {};
        t.type = SDL_GPU_TEXTURETYPE_2D;
        t.format = format;
        t.usage = usage;
        t.width = w;
        t.height = h;
        t.layer_count_or_depth = 1;
        t.num_levels = 1;
        SDL_GPUTexture* tex = gpumem::createTexture("GI", device_, &t);
        if (tex)
            gpumem::setTextureName(device_, tex, name);
        return tex;
    };
    irradiance_ = texture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, kTilesX * kIrradianceTile, kTilesY * kIrradianceTile,
                          "probe irradiance");
    distance_ = texture(SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, kTilesX * kDistanceTile, kTilesY * kDistanceTile, "probe distance");
    data_ = texture(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, kTilesX, kTilesY, "probe data");
    SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                 kMaxEntries * kRays * 16, 0};
    rays_ = gpumem::createBuffer("GI", device_, &b);
    gpumem::setBufferName(device_, rays_, "probe rays");
    const uint32_t scheduleBytes = kScheduleEntries * 4;
    b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, scheduleBytes, 0};
    schedule_ = gpumem::createBuffer("GI", device_, &b);
    gpumem::setBufferName(device_, schedule_, "probe schedule");
    const SDL_GPUTransferBufferCreateInfo tb = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, scheduleBytes, 0};
    upload_ = gpumem::createTransferBuffer("GI", device_, &tb, "probe schedule upload");
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_ = SDL_CreateGPUSampler(device_, &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    point_ = SDL_CreateGPUSampler(device_, &s);
    const bool ok = trace_ && blend_ && relocate_ && reset_ && irradiance_ && distance_ && data_ && rays_ && schedule_ && upload_ &&
                    linear_ && point_ && localLights_.init(device_);
    if (!ok) {
        FX_ERROR("probe gi: %s", SDL_GetError());
        return false;
    }
    const double mb = (double(kTilesX) * kTilesY * (kIrradianceTile * kIrradianceTile * 8 + kDistanceTile * kDistanceTile * 4 + 8) +
                       double(kMaxEntries) * kRays * 16) / (1 << 20);
    FX_INFO("probe gi: %u grids of %ux%ux%u probes, %u rays each, %.1f MB", kCascades, kDims[0], kDims[1], kDims[2], kRays, mb);
    return true;
}

void ProbeGi::shutdown()
{
    if (!device_)
        return;
    for (SDL_GPUComputePipeline* p : {trace_, blend_, relocate_, reset_})
        if (p)
            SDL_ReleaseGPUComputePipeline(device_, p);
    for (SDL_GPUTexture* t : {irradiance_, distance_, data_})
        gpumem::releaseTexture(device_, t);
    gpumem::releaseBuffer(device_, rays_);
    gpumem::releaseBuffer(device_, schedule_);
    gpumem::releaseTransferBuffer(device_, upload_);
    localLights_.shutdown();
    if (linear_)
        SDL_ReleaseGPUSampler(device_, linear_);
    if (point_)
        SDL_ReleaseGPUSampler(device_, point_);
    trace_ = blend_ = relocate_ = reset_ = nullptr;
    irradiance_ = distance_ = data_ = nullptr;
    rays_ = schedule_ = nullptr;
    upload_ = nullptr;
    linear_ = point_ = nullptr;
    ready_ = false;
    device_ = nullptr;
}

void ProbeGi::resetState()
{
    camera_ = {};
    spacing_ = 0.0f;
    for (Grid& g : grids_)
        g = Grid{};
    volume_ = {};
    boost_.assign(kAllProbes, 0);
    queued_.assign(kAllProbes, 0);
    fresh_.assign(kAllProbes, 0);
    age_.assign(kAllProbes, uint8_t(kSleepAge));
    scheduled_.assign(kAllProbes, ~0u);
    freshQueue_.clear();
    changedQueue_.clear();
    freshHead_ = 0;
    entries_.clear();
    resets_.clear();
    pendingRegion_ = boostAll_ = pendingWake_ = warm_ = false;
    revision_ = ~0u;
    regionRevision_ = 0;
    intensity_ = sky_ = -1.0f;
    sun_ = sunColor_ = {};
    frame_ = budget_ = lastUpdates_ = awake_ = 0;
    ready_ = false;
}

void ProbeGi::wakeRegion(const float region[6])
{
    for (int a = 0; a < 3; ++a) {
        wake_[a] = pendingWake_ ? std::min(wake_[a], region[a]) : region[a];
        wake_[3 + a] = pendingWake_ ? std::max(wake_[3 + a], region[3 + a]) : region[3 + a];
    }
    pendingWake_ = true;
}

void ProbeGi::setAge(uint32_t p, uint32_t age)
{
    age = std::min(age, kSleepAge);
    const bool was = age_[p] < kSleepAge, is = age < kSleepAge;
    awake_ = awake_ + (is ? 1 : 0) - (was ? 1 : 0);
    age_[p] = uint8_t(age);
}

void ProbeGi::invalidateRegion(const float region[6])
{
    for (int a = 0; a < 3; ++a) {
        region_[a] = pendingRegion_ ? std::min(region_[a], region[a]) : region[a];
        region_[3 + a] = pendingRegion_ ? std::max(region_[3 + a], region[3 + a]) : region[3 + a];
    }
    pendingRegion_ = true;
}

void ProbeGi::place(uint32_t c)
{
    Grid& g = grids_[c];
    int32_t want[3];
    for (int a = 0; a < 3; ++a)
        want[a] = floorDiv((&camera_.x)[a], g.spacing) - int32_t(kDims[a] / 2);
    if (g.placed && want[0] == g.base[0] && want[1] == g.base[1] && want[2] == g.base[2])
        return;
    if (!g.placed)
        warm_ = true;  // every probe new: lit at once
    // The probes the move brings in: fresh, at the front of the queue, reset (not sampled).
    for (uint32_t i = 0; i < kProbes; ++i) {
        const uint32_t s[3] = {i % kDims[0], (i / kDims[0]) % kDims[1], i / (kDims[0] * kDims[1])};
        bool kept = g.placed;
        for (int a = 0; a < 3 && kept; ++a) {
            const int32_t d = int32_t(kDims[a]);
            const int32_t w = want[a] + ((int32_t(s[a]) - want[a]) % d + d) % d;  // its world index in the new grid
            kept = w >= g.base[a] && w < g.base[a] + d;
        }
        if (kept)
            continue;
        const uint32_t p = c * kProbes + i;
        if (!fresh_[p]) {
            fresh_[p] = 1;
            freshQueue_.push_back(key(c, i));
        }
        setAge(p, 0);
        resets_.push_back(key(c, i));
    }
    std::memcpy(g.base, want, sizeof(want));
    g.placed = true;
}

void ProbeGi::markRegion(const float region[6], Mark mark)
{
    for (uint32_t c = 0; c < kCascades; ++c) {
        const Grid& g = grids_[c];
        int32_t lo[3], hi[3];
        uint64_t count = 1;
        for (int a = 0; a < 3; ++a) {
            // Two probes past it: what lit it, or was lit by it, changes round it too. (Probes sit
            // half a spacing off its multiples: probe_common.slang kProbeGridOffset.)
            lo[a] = std::max(floorDiv(region[a] - 0.5f * g.spacing, g.spacing) - 1, g.base[a]);
            hi[a] = std::min(floorDiv(region[3 + a] - 0.5f * g.spacing, g.spacing) + 2, g.base[a] + int32_t(kDims[a]) - 1);
            if (lo[a] > hi[a])
                count = 0;
            count *= uint64_t(std::max(hi[a] - lo[a] + 1, 0));
        }
        if (count == 0)
            continue;
        const bool queue = mark == Mark::Change && count <= kMaxQueuedRegion;
        for (int32_t z = lo[2]; z <= hi[2]; ++z)
            for (int32_t y = lo[1]; y <= hi[1]; ++y)
                for (int32_t x = lo[0]; x <= hi[0]; ++x) {
                    const uint32_t i = slotOf(x, kDims[0]) + kDims[0] * (slotOf(y, kDims[1]) + kDims[1] * slotOf(z, kDims[2]));
                    const uint32_t p = c * kProbes + i;
                    if (mark == Mark::Wake) {
                        setAge(p, std::min<uint32_t>(age_[p], kWakeAge));
                        continue;
                    }
                    setAge(p, std::min<uint32_t>(age_[p], 1));  // (its memory started again)
                    boost_[p] = uint8_t(kBoostUpdates);
                    if (queue && !queued_[p]) {
                        queued_[p] = 1;
                        changedQueue_.push_back(key(c, i));
                    }
                }
    }
}

void ProbeGi::schedule()
{
    entries_.clear();
    const uint32_t stamp = frame_;
    auto add = [&](uint32_t k) {
        const uint32_t p = (k >> 16) * kProbes + (k & 0xffff);
        if (scheduled_[p] == stamp)
            return;
        scheduled_[p] = stamp;
        uint32_t entry = k;
        if (fresh_[p]) {
            fresh_[p] = 0;
            setAge(p, 0);
            entry |= kEntryFresh;
        }
        if (boost_[p] > 0)
            --boost_[p];
        entries_.push_back(entry | uint32_t(age_[p]) << kEntryAgeShift);
        setAge(p, age_[p] + 1u);
    };
    // Fresh probes first (the grids just moved over them), nearest grid first (the queue's order).
    while (freshHead_ < freshQueue_.size() && entries_.size() < budget_) {
        const uint32_t k = freshQueue_[freshHead_++];
        if (fresh_[(k >> 16) * kProbes + (k & 0xffff)])
            add(k);
    }
    if (freshHead_ == freshQueue_.size()) {
        freshQueue_.clear();
        freshHead_ = 0;
    }
    // Then those near a change, each kBoostUpdates times in a row.
    std::vector<uint32_t> again;
    size_t taken = 0;
    for (; taken < changedQueue_.size() && entries_.size() < budget_; ++taken) {
        const uint32_t k = changedQueue_[taken], p = (k >> 16) * kProbes + (k & 0xffff);
        add(k);
        if (boost_[p] > 0 && age_[p] < kSleepAge)
            again.push_back(k);
        else
            queued_[p] = 0, boost_[p] = 0;
    }
    changedQueue_.erase(changedQueue_.begin(), changedQueue_.begin() + ptrdiff_t(taken));
    changedQueue_.insert(changedQueue_.end(), again.begin(), again.end());
    // The rest of the awake probes round robin, the finest grid most often (a grid with fewer
    // awake than its share leaves the rest to the others).
    auto robin = [&](uint32_t c, uint32_t n) {
        Grid& g = grids_[c];
        for (uint32_t scanned = 0; scanned < kProbes && n > 0 && awake_ > 0; ++scanned) {
            const uint32_t p = c * kProbes + g.cursor;
            if (age_[p] < kSleepAge && scheduled_[p] != stamp) {
                add(key(c, g.cursor));
                --n;
            }
            g.cursor = (g.cursor + 1) % kProbes;
        }
    };
    uint32_t shares = 0;
    for (uint32_t share : kGridShare)
        shares += share;
    const uint32_t left = budget_ - uint32_t(entries_.size());
    for (uint32_t c = 0; c < kCascades; ++c)
        robin(c, left * kGridShare[c] / shares);
    for (uint32_t c = 0; c < kCascades && entries_.size() < budget_; ++c)
        robin(c, budget_ - uint32_t(entries_.size()));
}

void ProbeGi::warmUp()
{
    // kWarmRounds rounds over every probe, the first taking each whole (fresh): each round's
    // bounces see the previous round's light, so the last ones have the bounces settled. The
    // probes' remaining updates (to kSleepAge: averaging away the rays' noise) come in the frames
    // after (all of them in the first frame cost 565 ms in Sponza, 250 for these).
    entries_.clear();
    for (uint32_t r = 0; r < kWarmRounds; ++r)
        for (uint32_t c = 0; c < kCascades; ++c)
            for (uint32_t i = 0; i < kProbes; ++i)
                entries_.push_back(key(c, i) | (r == 0 ? kEntryFresh : 0u) | r << kEntryAgeShift);
    for (uint32_t p = 0; p < kAllProbes; ++p) {
        fresh_[p] = 0;
        boost_[p] = queued_[p] = 0;
        setAge(p, kWarmRounds);
    }
    freshQueue_.clear();
    freshHead_ = 0;
    changedQueue_.clear();
}

void ProbeGi::update(SDL_GPUCommandBuffer* cmd, const SceneBvh& bvh, const LightSystem& lights, const GiSettings& settings)
{
    ready_ = false;
    if (!device_ || !trace_ || !settings.enabled || !bvh.ready())
        return;
    FX_ZONE_CAT("ProbeGi::update", Rendering);
    ++frame_;
    const float spacing = std::clamp(settings.probeSpacing, 0.25f, 8.0f);
    if (spacing != spacing_) {  // new grids: every probe fresh
        spacing_ = spacing;
        for (uint32_t c = 0; c < kCascades; ++c) {
            grids_[c].spacing = spacing * float(1u << c);
            grids_[c].placed = false;
        }
    }
    budget_ = std::clamp(settings.probeRays / kRays, 64u, kMaxEntries);
    // The point and spot lights the rays see (cells twice the first grid's spacing); the probes
    // round a light that changed update first.
    localLights_.update(cmd, lights, camera_, spacing_ * 2.0f, [this](const float box[6]) { invalidateRegion(box); });
    if (settings.sceneRevision != revision_ || settings.intensity != intensity_ || settings.skyIntensity != sky_)
        boostAll_ = true;
    const float3 sun = lights.sunDirection(), color = lights.sunColor();
    const bool sunMoved = length(sun - sun_) > 1e-4f || length(color - sunColor_) > 1e-4f * std::max(length(color), 1.0f);
    sun_ = sun;
    sunColor_ = color;
    revision_ = settings.sceneRevision;
    intensity_ = settings.intensity;
    sky_ = settings.skyIntensity;
    resets_.clear();
    warm_ = false;
    for (uint32_t c = 0; c < kCascades; ++c)
        place(c);
    const bool warm = warm_;
    if (warm) {
        warmUp();
        boostAll_ = pendingRegion_ = pendingWake_ = false;
        regionRevision_ = settings.regionRevision;
    } else {
        // A new scene, or the light's strength changed: every probe's memory starts again; the sun
        // moved: every probe wakes.
        if (boostAll_ || sunMoved)
            for (uint32_t p = 0; p < kAllProbes; ++p)
                setAge(p, std::min<uint32_t>(age_[p], boostAll_ ? 1u : kWakeAge));
        boostAll_ = false;
        if (settings.regionRevision != regionRevision_) {
            markRegion(settings.region, Mark::Change);
            regionRevision_ = settings.regionRevision;
        }
        if (pendingRegion_) {
            markRegion(region_, Mark::Change);
            pendingRegion_ = false;
        }
        if (pendingWake_) {
            markRegion(wake_, Mark::Wake);
            pendingWake_ = false;
        }
        schedule();
    }
    lastUpdates_ = uint32_t(entries_.size());

    for (uint32_t c = 0; c < kCascades; ++c) {
        for (int a = 0; a < 3; ++a)
            volume_.base[c][a] = grids_[c].base[a];
        volume_.base[c][3] = 0;
        volume_.spacing[c] = grids_[c].spacing;
    }
    volume_.spacing[3] = 1.0f;
    volume_.bias[0] = kNormalBias;
    volume_.bias[1] = kViewBias;
    volume_.bias[2] = settings.skyIntensity;
    volume_.bias[3] = 0.0f;

    const uint32_t entries = uint32_t(entries_.size()), resets = uint32_t(resets_.size());
    if (entries + resets > 0) {
        auto* mapped = static_cast<uint32_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
        std::memcpy(mapped, entries_.data(), entries * 4);
        std::memcpy(mapped + entries, resets_.data(), resets * 4);
        SDL_UnmapGPUTransferBuffer(device_, upload_);
        FX_GPU_PASS(cmd, "probe schedule upload");
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src = {upload_, 0};
        const SDL_GPUBufferRegion dst = {schedule_, 0, (entries + resets) * 4};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
    }

    ProbeUpdateUniforms u = {};
    u.volume = volume_;
    u.sun[0] = sun.x, u.sun[1] = sun.y, u.sun[2] = sun.z, u.sun[3] = settings.intensity;
    u.sunColor[0] = color.x, u.sunColor[1] = color.y, u.sunColor[2] = color.z, u.sunColor[3] = settings.skyIntensity;
    u.params[1] = vt_->pageTableOffset();
    u.params[2] = 1;
    u.params[3] = resets;
    u.batch[1] = entries;  // (the slots to reset follow the entries)
    u.memory[0] = kHysteresis;
    u.memory[2] = kMinFrontFace;

    FX_GPU_SECTION(cmd, "probe GI");
    u.params[0] = std::min(entries, kMaxEntries);
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    if (resets > 0) {
        FX_GPU_PASS(cmd, "probe reset");
        const SDL_GPUStorageTextureReadWriteBinding out = {data_, 0, 0, false};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, reset_);
        SDL_GPUBuffer* ro[2] = {rays_, schedule_};
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);
        SDL_DispatchGPUCompute(pass, (resets + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    // Batches of kMaxEntries (more than one only in a warm-up), each after the previous one's light.
    for (uint32_t first = 0; first < entries; first += kMaxEntries) {
        const uint32_t n = std::min(entries - first, kMaxEntries);
        if (first > 0) {
            u.params[0] = n;
            u.batch[0] = first;
            SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
        }
        FX_GPU_PASS(cmd, "probe trace");
        const SDL_GPUStorageBufferReadWriteBinding rw = {rays_, false};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, trace_);
        const SDL_GPUTextureSamplerBinding samplers[4] = {
            {vt_->physical(0), vt_->sampler()}, {irradiance_, linear_}, {distance_, linear_}, {data_, point_}};
        SDL_BindGPUComputeSamplers(pass, 0, samplers, 4);
        SDL_GPUBuffer* ro[5] = {bvh.buffer(), pak_->sceneBuffer(), vt_->materialData(), schedule_, localLights_.buffer()};
        SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 5);
        SDL_DispatchGPUCompute(pass, n, 1, 1);
        SDL_EndGPUComputePass(pass);

        FX_GPU_PASS(cmd, "probe blend");
        const SDL_GPUStorageTextureReadWriteBinding atlases[2] = {{irradiance_, 0, 0, false}, {distance_, 0, 0, false}};
        pass = SDL_BeginGPUComputePass(cmd, atlases, 2, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, blend_);
        const SDL_GPUTextureSamplerBinding data = {data_, point_};
        SDL_BindGPUComputeSamplers(pass, 0, &data, 1);
        SDL_GPUBuffer* blendIn[2] = {rays_, schedule_};
        SDL_BindGPUComputeStorageBuffers(pass, 0, blendIn, 2);
        SDL_DispatchGPUCompute(pass, n, 1, 1);
        SDL_EndGPUComputePass(pass);

        FX_GPU_PASS(cmd, "probe relocate");
        const SDL_GPUStorageTextureReadWriteBinding out = {data_, 0, 0, false};
        pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, relocate_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, blendIn, 2);
        SDL_DispatchGPUCompute(pass, (n + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    if (warm)
        FX_INFO("probe gi: lit %u probes (%u updates), %u point and spot lights", kAllProbes, entries, localLights_.lights());
    ready_ = true;
}

}  // namespace fx
