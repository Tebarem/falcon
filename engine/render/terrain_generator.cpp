#include "render/terrain_generator.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"

#include "terrain_generate.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

struct GenUniforms {
    uint32_t dims[4], op[4], misc[4];
    float grid[4], noise[4], noise2[4], shape[4], shape2[4], maskH[4], maskS[4], maskC[4], maskF[4], erode1[4], erode2[4],
        erode3[4];
};

enum Buffer : uint32_t { kHeightsA, kHeightsB, kMaterials, kMasks, kCellA, kCellFlux, kCellB, kCellS };

uint32_t erosionFactor(const GenStep& s, float spacing)
{
    return std::clamp(uint32_t(std::lround(s.cellSize / spacing)), 1u, 64u);
}

// The step's settings and masks (everything but the dimensions).
void fillStep(const GenStep& s, GenUniforms& u)
{
    u.op[0] = uint32_t(s.op);
    u.op[1] = uint32_t(s.noise);
    u.op[2] = s.octaves;
    u.misc[0] = s.seed * 0x9e3779b9u + 0x632be5abu;
    u.misc[1] = s.layer;
    u.misc[2] = uint32_t(s.blend);
    const float n[4] = {s.scale, s.height, s.lacunarity, s.gain}, n2[4] = {s.warp, s.warpScale, s.offset, s.sharpness};
    const float sh[4] = {s.step, s.stepSharpness, s.strata, s.amount}, sh2[4] = {s.level, s.knee, s.curvatureScale, 0.0f};
    const float mh[4] = {s.heightMin, s.heightMax, s.heightSoft, s.maskNoise}, ms[4] = {s.slopeMin, s.slopeMax, s.slopeSoft, s.maskNoiseScale};
    const float mc[4] = {s.curvatureMin, s.curvatureMax, 0.0f, 0.0f}, mf[4] = {s.flowMin, s.flowMax, s.depositMin, s.depositMax};
    std::memcpy(u.noise, n, 16);
    std::memcpy(u.noise2, n2, 16);
    std::memcpy(u.shape, sh, 16);
    std::memcpy(u.shape2, sh2, 16);
    std::memcpy(u.maskH, mh, 16);
    std::memcpy(u.maskS, ms, 16);
    std::memcpy(u.maskC, mc, 16);
    std::memcpy(u.maskF, mf, 16);
}

float unitHash(int32_t x, int32_t z)
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xffffffu) / 16777216.0f;
}

}  // namespace

bool TerrainGenerator::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    auto make = [&](const ShaderBlob& blob) {
        SDL_GPUComputePipeline* p = gpu.createComputePipeline(blob);
        rwCounts_.push_back({p, blob.numRwStorageBuffers});
        return p;
    };
    clear_ = make(shaders::terrain_generate_cs_clear);
    stepPass_ = make(shaders::terrain_generate_cs_step);
    blur_ = make(shaders::terrain_generate_cs_blur);
    erodeInit_ = make(shaders::terrain_generate_cs_erode_init);
    erodeFlux_ = make(shaders::terrain_generate_cs_erode_flux);
    erodeWater_ = make(shaders::terrain_generate_cs_erode_water);
    erodeTransport_ = make(shaders::terrain_generate_cs_erode_transport);
    erodeSettle_ = make(shaders::terrain_generate_cs_erode_settle);
    erodeApply_ = make(shaders::terrain_generate_cs_erode_apply);
    return clear_ && stepPass_ && blur_ && erodeInit_ && erodeFlux_ && erodeWater_ && erodeTransport_ && erodeSettle_ && erodeApply_;
}

void TerrainGenerator::shutdown()
{
    if (!device_)
        return;
    cancel();
    releaseBuffers();
    for (SDL_GPUComputePipeline* p : {clear_, stepPass_, blur_, erodeInit_, erodeFlux_, erodeWater_, erodeTransport_, erodeSettle_, erodeApply_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    rwCounts_.clear();
    device_ = nullptr;
}

void TerrainGenerator::releaseBuffers()
{
    for (SDL_GPUBuffer*& b : buffers_) {
        gpumem::releaseBuffer(device_, b);
        b = nullptr;
    }
    sampleCapacity_ = cellCapacity_ = 0;
    gpumem::releaseTransferBuffer(device_, upload_);
    gpumem::releaseTransferBuffer(device_, download_);
    upload_ = download_ = nullptr;
    uploadBytes_ = downloadBytes_ = 0;
}

bool TerrainGenerator::ensureBuffers(uint32_t samples, uint32_t cells)
{
    auto make = [&](SDL_GPUBuffer*& b, uint32_t bytes, const char* name) {
        gpumem::releaseBuffer(device_, b);
        SDL_GPUBufferCreateInfo info = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, bytes, 0};
        b = gpumem::createBuffer("TerrainGen", device_, &info);
        if (b)
            gpumem::setBufferName(device_, b, name);
        return b != nullptr;
    };
    bool ok = true;
    if (samples > sampleCapacity_) {
        ok = make(buffers_[kHeightsA], samples * 4, "terrain gen heights A") && make(buffers_[kHeightsB], samples * 4, "terrain gen heights B") &&
             make(buffers_[kMaterials], samples * 4, "terrain gen materials") && make(buffers_[kMasks], samples * 8, "terrain gen masks");
        sampleCapacity_ = ok ? samples : 0;
    }
    cells = std::max(cells, 1u);
    if (ok && cells > cellCapacity_) {
        ok = make(buffers_[kCellA], cells * 16, "terrain gen cells") && make(buffers_[kCellFlux], cells * 16, "terrain gen flux") &&
             make(buffers_[kCellB], cells * 16, "terrain gen velocity") && make(buffers_[kCellS], cells * 16, "terrain gen sediment");
        cellCapacity_ = ok ? cells : 0;
    }
    if (!ok)
        FX_ERROR("terrain: cannot allocate generation buffers (%u samples): %s", samples, SDL_GetError());
    return ok;
}

bool TerrainGenerator::start(Terrain& terrain, const TerrainGenStack& stack, std::function<void(TerrainRect)> beforeWrite,
                             const std::vector<float>* weights, TerrainRect weightRect)
{
    cancel();
    if (!device_ || !terrain.valid())
        return false;
    stack_ = stack;
    const TerrainSettings& ts = terrain.settings();
    const GenRegion& g = stack.region;
    if (weights) {
        rect_ = weightRect;
        weights_ = *weights;
    } else {
        rect_ = g.all ? terrain.clampRect({0, 0, int32_t(terrain.samplesPerSide()), int32_t(terrain.samplesPerSide())})
                      : terrain.worldRect(g.x - g.size * 0.5f, g.z - g.size * 0.5f, g.x + g.size * 0.5f, g.z + g.size * 0.5f);
        weights_.clear();
    }
    if (rect_.empty())
        return false;
    // What it writes, and how far its steps reach past a sample.
    writeHeights_ = g.flat;
    writeMaterials_ = false;
    margin_ = 4;
    for (const GenStep& s : stack.steps) {
        if (!s.enabled)
            continue;
        if (s.op == GenOp::Paint)
            writeMaterials_ = true;
        else
            writeHeights_ = true;
        if (s.curvatureMin > -1.0f || s.curvatureMax < 1.0f)
            margin_ = std::max(margin_, int32_t(std::ceil(s.curvatureScale / ts.spacing)) + 2);
        if (s.op == GenOp::Smooth)
            margin_ = std::max(margin_, int32_t(std::ceil(s.radius / ts.spacing)) * 4 + 2);
        if (s.op == GenOp::Erode)
            margin_ = std::max(margin_, int32_t(std::max(256u, erosionFactor(s, ts.spacing) * 48)));
    }
    margin_ = std::min(margin_, 1024);
    // Even chunks of up to kChunk + 1 samples (a terrain's 2^n + 1 samples leave no sliver).
    chunks_.clear();
    const int32_t w = rect_.x1 - rect_.x0 + 1, d = rect_.z1 - rect_.z0 + 1;
    const int32_t nx = std::max((w - 2) / kChunk + 1, 1), nz = std::max((d - 2) / kChunk + 1, 1);
    for (int32_t cz = 0; cz < nz; ++cz)
        for (int32_t cx = 0; cx < nx; ++cx) {
            Chunk c;
            c.interior = {rect_.x0 + w * cx / nx, rect_.z0 + d * cz / nz, rect_.x0 + w * (cx + 1) / nx - 1, rect_.z0 + d * (cz + 1) / nz - 1};
            c.buffer = {c.interior.x0 - margin_, c.interior.z0 - margin_, c.interior.x1 + margin_, c.interior.z1 + margin_};
            chunks_.push_back(c);
        }
    terrain_ = &terrain;
    beforeWrite_ = std::move(beforeWrite);
    chunk_ = 0;
    phase_ = Phase::Load;
    startNs_ = SDL_GetTicksNS();
    status_ = "Loading";
    return true;
}

void TerrainGenerator::cancel()
{
    if (fence_) {
        SDL_WaitForGPUFences(device_, true, &fence_, 1);
        SDL_ReleaseGPUFence(device_, fence_);
        fence_ = nullptr;
    }
    if (terrain_)
        terrain_->pin({0, 0, -1, -1});
    terrain_ = nullptr;
    phase_ = Phase::Done;
    chunks_.clear();
    heights_ = {};
    materials_ = {};
    if (device_)
        releaseBuffers();
}

float TerrainGenerator::progress() const
{
    if (!terrain_ || chunks_.empty())
        return 0.0f;
    float within = 0.0f;
    if (phase_ == Phase::Steps || phase_ == Phase::Wait) {
        float total = 0.0f, done = 0.0f;
        for (size_t i = 0; i < stack_.steps.size(); ++i) {
            const GenStep& s = stack_.steps[i];
            const float cost = !s.enabled ? 0.0f : s.op == GenOp::Erode ? 4.0f : 1.0f;
            total += cost;
            if (i < step_)
                done += cost;
            else if (i == step_ && s.op == GenOp::Erode)
                done += cost * float(iteration_) / float(std::max(s.iterations, 1u));
        }
        within = phase_ == Phase::Wait ? 1.0f : done / std::max(total, 1.0f);
    }
    return (float(chunk_) + within * 0.95f) / float(chunks_.size());
}

void TerrainGenerator::dispatch(SDL_GPUCommandBuffer* cmd, SDL_GPUComputePipeline* p, uint32_t w, uint32_t h, const void* uniforms)
{
    // Exactly the buffers the kernel declares (D3D12 copies as many descriptors as were bound).
    uint32_t count = 8;
    for (const auto& [pipeline, n] : rwCounts_)
        if (pipeline == p)
            count = n;
    SDL_PushGPUComputeUniformData(cmd, 0, uniforms, sizeof(GenUniforms));
    SDL_GPUStorageBufferReadWriteBinding rw[8] = {};
    rw[0].buffer = buffers_[current_ ? kHeightsB : kHeightsA];
    rw[1].buffer = buffers_[current_ ? kHeightsA : kHeightsB];
    for (uint32_t i = 2; i < 8; ++i)
        rw[i].buffer = buffers_[i];
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, count);
    SDL_BindGPUComputePipeline(pass, p);
    SDL_DispatchGPUCompute(pass, (w + 7) / 8, (h + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
}

void TerrainGenerator::upload(SDL_GPUCommandBuffer* cmd)
{
    const uint32_t samples = uint32_t(heights_.size());
    const uint32_t bytes = samples * 8;
    if (bytes > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
        upload_ = gpumem::createTransferBuffer("TerrainGen", device_, &info, "terrain gen upload");
        uploadBytes_ = upload_ ? bytes : 0;
    }
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    std::memcpy(mapped, heights_.data(), samples * 4);
    std::memcpy(mapped + samples * 4, materials_.data(), samples * 4);
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation h = {upload_, 0}, m = {upload_, samples * 4};
    const SDL_GPUBufferRegion hd = {buffers_[kHeightsA], 0, samples * 4}, md = {buffers_[kMaterials], 0, samples * 4};
    SDL_UploadToGPUBuffer(copy, &h, &hd, false);
    SDL_UploadToGPUBuffer(copy, &m, &md, false);
    SDL_EndGPUCopyPass(copy);
    current_ = 0;
}

bool TerrainGenerator::recordSteps(SDL_GPUCommandBuffer* cmd)
{
    const Chunk& c = chunks_[chunk_];
    const uint32_t W = uint32_t(c.buffer.x1 - c.buffer.x0 + 1), H = uint32_t(c.buffer.z1 - c.buffer.z0 + 1);
    const TerrainSettings& ts = terrain_->settings();
    GenUniforms u = {};
    auto base = [&](const GenStep& s) {
        u = {};
        fillStep(s, u);
        u.dims[0] = W;
        u.dims[1] = H;
        u.dims[2] = cellsW_;
        u.dims[3] = cellsH_;
        u.op[3] = factor_;
        u.grid[0] = ts.originX + float(c.buffer.x0) * ts.spacing;
        u.grid[1] = ts.originZ + float(c.buffer.z0) * ts.spacing;
        u.grid[2] = ts.spacing;
        u.grid[3] = 1.0f / ts.spacing;
    };
    for (; step_ < stack_.steps.size(); ++step_) {
        const GenStep& s = stack_.steps[step_];
        if (!s.enabled)
            continue;
        if (s.op == GenOp::Noise || s.op == GenOp::Terrace || s.op == GenOp::Plateau) {
            base(s);
            dispatch(cmd, stepPass_, W, H, &u);
            current_ ^= 1;
        } else if (s.op == GenOp::Paint) {
            base(s);
            dispatch(cmd, stepPass_, W, H, &u);
        } else if (s.op == GenOp::Smooth) {
            const uint32_t r = std::max(uint32_t(std::lround(s.radius / ts.spacing)), 1u);
            for (uint32_t pass = 0; pass < 4; ++pass) {
                base(s);
                u.misc[3] = r << 1 | (pass & 1);
                dispatch(cmd, blur_, W, H, &u);
                current_ ^= 1;
            }
        } else if (s.op == GenOp::Erode) {
            if (iteration_ == 0) {
                factor_ = erosionFactor(s, ts.spacing);
                cellsW_ = (W + factor_ - 1) / factor_;
                cellsH_ = (H + factor_ - 1) / factor_;
                if (!ensureBuffers(W * H, cellsW_ * cellsH_))
                    continue;
                base(s);
                dispatch(cmd, erodeInit_, cellsW_, cellsH_, &u);
            }
            base(s);
            const float l = float(factor_) * ts.spacing;
            const float e1[4] = {0.25f * std::sqrt(l), s.rain, s.capacity, s.solubility};
            const float e2[4] = {s.deposition, s.evaporation, std::tan(s.talus * (kPi / 180.0f)) * l, s.thermal};
            const float e3[4] = {l, 0.0f, 0.0f, 0.0f};
            std::memcpy(u.erode1, e1, 16);
            std::memcpy(u.erode2, e2, 16);
            std::memcpy(u.erode3, e3, 16);
            // A batch of iterations per submission (fewer on large grids).
            const uint32_t batch = std::clamp(uint32_t(8000000u / std::max(cellsW_ * cellsH_, 1u)), 4u, 200u);
            const uint32_t end = std::min(iteration_ + batch, s.iterations);
            for (; iteration_ < end; ++iteration_)
                for (SDL_GPUComputePipeline* p : {erodeFlux_, erodeWater_, erodeTransport_, erodeSettle_})
                    dispatch(cmd, p, cellsW_, cellsH_, &u);
            if (iteration_ < s.iterations)
                return false;  // more next time
            dispatch(cmd, erodeApply_, W, H, &u);
            current_ ^= 1;
            iteration_ = 0;
            ++step_;
            return step_ >= stack_.steps.size();
        }
    }
    return true;
}

void TerrainGenerator::recordDownload(SDL_GPUCommandBuffer* cmd)
{
    const Chunk& c = chunks_[chunk_];
    const uint32_t W = uint32_t(c.buffer.x1 - c.buffer.x0 + 1);
    const uint32_t iw = uint32_t(c.interior.x1 - c.interior.x0 + 1), ih = uint32_t(c.interior.z1 - c.interior.z0 + 1);
    const uint32_t bytes = iw * ih * 8;
    if (bytes > downloadBytes_) {
        gpumem::releaseTransferBuffer(device_, download_);
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0};
        download_ = gpumem::createTransferBuffer("TerrainGen", device_, &info, "terrain gen download");
        downloadBytes_ = download_ ? bytes : 0;
    }
    // Row by row: the interior of the buffer rectangle.
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const uint32_t ox = uint32_t(c.interior.x0 - c.buffer.x0), oz = uint32_t(c.interior.z0 - c.buffer.z0);
    for (uint32_t z = 0; z < ih; ++z) {
        const uint32_t src = ((oz + z) * W + ox) * 4;
        const SDL_GPUBufferRegion h = {buffers_[current_ ? kHeightsB : kHeightsA], src, iw * 4};
        const SDL_GPUTransferBufferLocation hd = {download_, z * iw * 4};
        SDL_DownloadFromGPUBuffer(copy, &h, &hd);
        const SDL_GPUBufferRegion m = {buffers_[kMaterials], src, iw * 4};
        const SDL_GPUTransferBufferLocation md = {download_, (ih + z) * iw * 4};
        SDL_DownloadFromGPUBuffer(copy, &m, &md);
    }
    SDL_EndGPUCopyPass(copy);
}

float TerrainGenerator::weightAt(int32_t gx, int32_t gz) const
{
    if (!weights_.empty()) {
        const int32_t w = rect_.x1 - rect_.x0 + 1;
        return weights_[size_t(gz - rect_.z0) * w + (gx - rect_.x0)];
    }
    const GenRegion& g = stack_.region;
    if (g.all)
        return 1.0f;
    const float3 p = terrain_->samplePosition(gx, gz);
    const float d = std::max(std::fabs(p.x - g.x), std::fabs(p.z - g.z));
    const float half = g.size * 0.5f;
    if (g.falloff <= 0.0f)
        return d <= half ? 1.0f : 0.0f;
    const float t = std::clamp((half - d) / g.falloff, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void TerrainGenerator::writeChunk()
{
    FX_ZONE_CAT("TerrainGenerator::writeChunk", Rendering);
    const Chunk& c = chunks_[chunk_];
    const TerrainRect r = terrain_->clampRect(c.interior);
    if (r.empty())
        return;
    const int32_t iw = c.interior.x1 - c.interior.x0 + 1, ih = c.interior.z1 - c.interior.z0 + 1;
    const auto* mapped = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device_, download_, false));
    if (!mapped)
        return;
    const float* h = reinterpret_cast<const float*>(mapped);
    const uint32_t* m = reinterpret_cast<const uint32_t*>(mapped + size_t(iw) * ih * 4);
    if (beforeWrite_)
        beforeWrite_(r);
    const int32_t w = r.x1 - r.x0 + 1, d = r.z1 - r.z0 + 1;
    std::vector<float> heights;
    std::vector<uint32_t> materials;
    if (writeHeights_)
        terrain_->read(r, heights);
    if (writeMaterials_)
        terrain_->readMaterials(r, materials);
    for (int32_t z = 0; z < d; ++z)
        for (int32_t x = 0; x < w; ++x) {
            const int32_t gx = r.x0 + x, gz = r.z0 + z;
            const float wt = weightAt(gx, gz);
            if (wt <= 0.0f)
                continue;
            const size_t src = size_t(gz - c.interior.z0) * iw + (gx - c.interior.x0), dst = size_t(z) * w + x;
            if (writeHeights_)
                heights[dst] += (h[src] - heights[dst]) * wt;
            // Materials don't blend: soft edges are dithered.
            if (writeMaterials_ && wt > unitHash(gx, gz))
                materials[dst] = (m[src] & 0x00ffffffu) | (materials[dst] & 0xff000000u);
        }
    SDL_UnmapGPUTransferBuffer(device_, download_);
    if (writeHeights_)
        terrain_->write(r, heights);
    if (writeMaterials_)
        terrain_->writeMaterials(r, materials);
}

bool TerrainGenerator::tick()
{
    if (!terrain_)
        return false;
    FX_ZONE_CAT("TerrainGenerator::tick", Rendering);
    if (fence_) {
        if (!SDL_QueryGPUFence(device_, fence_))
            return false;
        SDL_ReleaseGPUFence(device_, fence_);
        fence_ = nullptr;
        if (phase_ == Phase::Wait) {
            writeChunk();
            ++chunk_;
            phase_ = chunk_ < chunks_.size() ? Phase::Load : Phase::Done;
        }
    }
    if (phase_ == Phase::Done) {
        terrain_->pin({0, 0, -1, -1});
        lastMs_ = double(SDL_GetTicksNS() - startNs_) * 1e-6;
        FX_INFO("terrain: generated %d x %d samples in %zu chunk%s (%.0f ms)", rect_.x1 - rect_.x0 + 1, rect_.z1 - rect_.z0 + 1,
                chunks_.size(), chunks_.size() == 1 ? "" : "s", lastMs_);
        terrain_ = nullptr;
        chunks_.clear();
        heights_ = {};
        materials_ = {};
        releaseBuffers();  // (over 100 MB for a full chunk; the next run makes them again)
        status_ = "Done";
        return true;
    }
    SDL_GPUCommandBuffer* cmd = nullptr;
    if (phase_ == Phase::Load) {
        const Chunk& c = chunks_[chunk_];
        const TerrainRect clamped = terrain_->clampRect(c.buffer);
        terrain_->pin(clamped);
        if (!terrain_->rectResident(clamped)) {
            status_ = "Loading tiles";
            return false;
        }
        // The buffer rectangle, clamped to the grid at its edges (heights, materials).
        const int32_t W = c.buffer.x1 - c.buffer.x0 + 1, H = c.buffer.z1 - c.buffer.z0 + 1;
        const int32_t cw = clamped.x1 - clamped.x0 + 1;
        std::vector<float> heights;
        std::vector<uint32_t> materials;
        terrain_->read(clamped, heights);
        terrain_->readMaterials(clamped, materials);
        heights_.resize(size_t(W) * H);
        materials_.resize(size_t(W) * H);
        const GenRegion& g = stack_.region;
        for (int32_t z = 0; z < H; ++z)
            for (int32_t x = 0; x < W; ++x) {
                const int32_t sx = std::clamp(c.buffer.x0 + x, clamped.x0, clamped.x1) - clamped.x0;
                const int32_t sz = std::clamp(c.buffer.z0 + z, clamped.z0, clamped.z1) - clamped.z0;
                const size_t i = size_t(z) * W + x;
                heights_[i] = g.flat && weights_.empty() ? g.base : heights[size_t(sz) * cw + sx];
                materials_[i] = materials[size_t(sz) * cw + sx];
            }
        cellsW_ = cellsH_ = 1;
        factor_ = 1;
        if (!ensureBuffers(uint32_t(W * H), 1)) {
            cancel();
            status_ = "Out of GPU memory";
            return true;
        }
        cmd = SDL_AcquireGPUCommandBuffer(device_);
        upload(cmd);
        GenUniforms u = {};
        u.dims[0] = uint32_t(W);
        u.dims[1] = uint32_t(H);
        dispatch(cmd, clear_, uint32_t(W), uint32_t(H), &u);
        step_ = 0;
        iteration_ = 0;
        phase_ = Phase::Steps;
    }
    if (phase_ == Phase::Steps) {
        if (!cmd)
            cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (recordSteps(cmd)) {
            recordDownload(cmd);
            phase_ = Phase::Wait;
        }
        char text[96];
        const GenStep* s = step_ < stack_.steps.size() ? &stack_.steps[step_] : nullptr;
        SDL_snprintf(text, sizeof(text), "Chunk %zu of %zu: %s", chunk_ + 1, chunks_.size(),
                     phase_ == Phase::Wait ? "writing" : s ? (s->name.empty() ? kGenOpNames[uint32_t(s->op)] : s->name.c_str()) : "");
        status_ = text;
    }
    if (cmd)
        fence_ = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    return false;
}

bool TerrainGenerator::run(Terrain& terrain, const TerrainGenStack& stack)
{
    if (!start(terrain, stack, nullptr))
        return false;
    for (;;) {
        if (tick())
            return true;
        if (fence_)
            SDL_WaitForGPUFences(device_, true, &fence_, 1);
        else {
            terrain.stream({0, 0, 0}, 0.0f);  // pinned tiles loading
            SDL_Delay(1);
        }
        if (!terrain_)
            return false;
    }
}

}  // namespace fx
