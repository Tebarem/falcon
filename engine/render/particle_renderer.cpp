#include "render/particle_renderer.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"
#include "render/post_process.h"
#include "render/virtual_texture.h"
#include "scene/fly_camera.h"
#include "vfx/particle_system.h"

#include "particles.shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

struct ParticleUniforms {
    float4x4 viewProj;
    float4 cameraPos, cameraRight, cameraUp, cameraForward, sunDirection, sunColor;
    uint32_t params[4];  // width, height, page table offset, frame
    float paramsF[4];    // mip bias, ambient scale
    uint32_t gridOrderOffset, gridTileOffset, gridTilesX, gridWords;
    float gridBinScale;
    uint32_t lightCount, gridPad[2];
    VsmParamsGpu vsm;
};

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

}  // namespace

bool ParticleRenderer::init(const GpuDevice& gpu, const ScenePak& pak, VirtualTextureSystem& vt)
{
    device_ = gpu.get();
    pak_ = &pak;
    vt_ = &vt;
    SDL_GPUShader* vs = gpu.createShader(shaders::particles_vs_sprite);
    SDL_GPUShader* fs = gpu.createShader(shaders::particles_fs_sprite);
    if (!vs || !fs) {
        FX_ERROR("particles: shaders failed: %s", SDL_GetError());
        if (vs)
            SDL_ReleaseGPUShader(device_, vs);
        if (fs)
            SDL_ReleaseGPUShader(device_, fs);
        return false;
    }
    // Premultiplied: alpha blending is (rgb * a, a), additive (rgb * a, 0).
    SDL_GPUColorTargetDescription targets[2] = {};
    SDL_GPUColorTargetDescription& color = targets[0];
    color.format = PostProcess::kHdrFormat;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUColorTargetDescription& reactive = targets[1];  // the most coverage
    reactive.format = kReactiveFormat;
    reactive.blend_state.enable_blend = true;
    reactive.blend_state.src_color_blendfactor = reactive.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    reactive.blend_state.src_alpha_blendfactor = reactive.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    reactive.blend_state.color_blend_op = reactive.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = 2;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    depthSampler_ = SDL_CreateGPUSampler(device_, &si);
    if (!pipeline_ || !depthSampler_) {
        FX_ERROR("particles: pipeline failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void ParticleRenderer::shutdown()
{
    if (!device_)
        return;
    if (pipeline_)
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (depthSampler_)
        SDL_ReleaseGPUSampler(device_, depthSampler_);
    gpumem::releaseBuffer(device_, buffer_);
    gpumem::releaseTransferBuffer(device_, upload_);
    pipeline_ = nullptr;
    depthSampler_ = nullptr;
    buffer_ = nullptr;
    upload_ = nullptr;
    capacity_ = 0;
    device_ = nullptr;
}

bool ParticleRenderer::ensureBuffer(uint32_t count)
{
    if (count <= capacity_ && buffer_ && upload_)
        return true;
    gpumem::releaseBuffer(device_, buffer_);
    gpumem::releaseTransferBuffer(device_, upload_);
    capacity_ = std::max(count + count / 2, 1024u);
    SDL_GPUBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = capacity_ * uint32_t(sizeof(SpriteGpu));
    buffer_ = gpumem::createBuffer("Particles", device_, &bi);
    gpumem::setBufferName(device_, buffer_, "particle sprites");
    const SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bi.size, 0};
    upload_ = gpumem::createTransferBuffer("Particles", device_, &ti, "particle upload");
    if (!buffer_ || !upload_) {
        capacity_ = 0;
        return false;
    }
    return true;
}

int32_t ParticleRenderer::materialOf(uint32_t assetId)
{
    if (auto it = materials_.find(assetId); it != materials_.end())
        return it->second;
    const std::string& path = vfxAssetPath(assetId);
    const int32_t index = path.empty() || !resolve_ ? -1 : resolve_(path);
    materials_[assetId] = index;
    return index;
}

void ParticleRenderer::addTrails(const ParticleSystem::Effect& fx, const ParticleTrailRenderer& r, const CameraState& camera,
                                 float pixelScale)
{
    // Each segment between a particle's points (from the particle back along its trail): a sprite
    // stretched along it (velocity alignment, the segment's length as its velocity).
    const int32_t material = materialOf(r.material);
    const uint32_t flags = std::min(r.blend, 2u) | (1u << 2) | (material < 0 ? 64u : 0u) | 128u;
    Need& need = needs_[material >= 0 ? uint32_t(material) : ~0u];
    for (size_t i = 0; i < fx.particles.size(); ++i) {
        const Particle& p = fx.particles[i];
        const ParticleSystem::Effect::Trail& t = fx.trails[i];
        if ((p.flags & kParticleDelayed) || t.count == 0)
            continue;
        float3 a = {p.position[0], p.position[1], p.position[2]};
        const float n = float(t.count);
        for (uint32_t k = 0; k < t.count; ++k) {
            const float3 b = {t.points[k][0], t.points[k][1], t.points[k][2]};
            const float3 d = a - b;
            const float len = length(d);
            if (len > 1e-5f) {
                const float along = (float(k) + 0.5f) / n;  // 0 at the particle .. 1 at its trail's end
                const float rs = particleRandom(p.seed, 201), rf = particleRandom(p.seed, 202);
                const float width = std::max(r.width.evaluate(along, rs, rf), 0.0f);
                float c[4];
                r.color.evaluate(along, rs, rf, c);
                const float3 mid = (a + b) * 0.5f;
                const float depth = dot(mid - camera.position, camera.forward);
                if (width > 0.0f && depth > -len) {
                    SpriteGpu s = {};
                    s.positionSize[0] = mid.x, s.positionSize[1] = mid.y, s.positionSize[2] = mid.z, s.positionSize[3] = width;
                    for (int q = 0; q < 4; ++q)
                        s.color[q] = p.color[q] * c[q];
                    s.velocity[0] = d.x, s.velocity[1] = d.y, s.velocity[2] = d.z, s.velocity[3] = 0.0f;
                    s.params[3] = 1.0f;  // its length: the segment's
                    s.info[0] = material >= 0 ? uint32_t(material) : 0u;
                    s.info[1] = flags;
                    s.info[2] = 1u | (1u << 8);
                    if (depth > 0.0f)
                        need.pixels = std::max(need.pixels, std::max(width, len) * pixelScale / depth);
                    order_.push_back({depth, uint32_t(sprites_.size())});
                    sprites_.push_back(s);
                }
            }
            a = b;
        }
    }
}

void ParticleRenderer::requestPages(uint64_t frame)
{
    // For each material: its pages at the mip a frame of it shows at (and the next coarser, for
    // the trilinear blend), from the largest sprite on screen.
    const auto& materials = pak_->materials();
    std::vector<uint32_t> entries;
    for (const auto& [index, need] : needs_) {
        if (index >= materials.size())
            continue;
        const fpak::MaterialGpu& m = materials[index];
        if (m.vtPageTable == fpak::kVtNoTexture)
            continue;
        const uint32_t lw = m.vtSize & 0xff, lh = (m.vtSize >> 8) & 0xff, mips = (m.vtSize >> 16) & 0xff;
        const float texels = float(1u << std::max(lw, lh)) / float(std::max(need.cells, 1u));
        const uint32_t mip = std::min(uint32_t(std::max(0.0f, std::floor(std::log2(texels / std::max(need.pixels, 1.0f))))), mips - 1);
        uint32_t offset = 0;
        for (uint32_t k = 0; k < mips; ++k) {
            const uint32_t pages = std::max((1u << lw) >> k, 128u) / 128u * (std::max((1u << lh) >> k, 128u) / 128u);
            if (k == mip || k == mip + 1)
                for (uint32_t e = 0; e < pages; ++e)
                    entries.push_back(m.vtPageTable + offset + e);
            offset += pages;
        }
    }
    if (!entries.empty())
        vt_->request(entries, frame);
}

bool ParticleRenderer::prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t, uint32_t height, uint64_t frame)
{
    FX_ZONE_CAT("ParticleRenderer::prepare", Rendering);
    count_ = 0;
    stats_ = {};
    if (!particles_ || !pipeline_)
        return false;
    sprites_.clear();
    order_.clear();
    materials_.clear();
    needs_.clear();
    const float pixelScale = camera.p11 * 0.5f * float(height);  // pixels per metre at 1 m
    const uint32_t spriteIndex = VfxIndexOf<ParticleSpriteRenderer>::value, trailIndex = VfxIndexOf<ParticleTrailRenderer>::value;
    for (const auto& [id, fx] : particles_->effects()) {
        if (!fx.effect || fx.particles.empty())
            continue;
        const bool sheet = (fx.effect->flags & kEffectSheet) != 0, snap = (fx.effect->flags & kEffectSheetSnap) != 0;
        for (const ParticleSystem::Effect::Part& part : fx.parts) {
            if (part.component == trailIndex && fx.trails.size() == fx.particles.size())
                addTrails(fx, *static_cast<const ParticleTrailRenderer*>(part.data), camera, pixelScale);
            if (part.component != spriteIndex)
                continue;
            const auto& r = *static_cast<const ParticleSpriteRenderer*>(part.data);
            const int32_t material = materialOf(r.material);
            const uint32_t cols = std::clamp(r.columns, 1u, 255u), rows = std::clamp(r.rows, 1u, 255u), frames = cols * rows;
            const uint32_t flags = std::min(r.blend, 2u) | (std::min(r.alignment, 3u) << 2) | ((r.flags & kSpriteLit) ? 16u : 0u) |
                                   ((r.flags & kSpriteFaceVelocity) ? 32u : 0u);
            const uint32_t fade = uint32_t(std::clamp(r.cameraFadeNear * 100.0f, 0.0f, 65535.0f)) |
                                  (uint32_t(std::clamp(r.cameraFadeFar * 100.0f, 0.0f, 65535.0f)) << 16);
            Need& need = needs_[material >= 0 ? uint32_t(material) : ~0u];
            need.cells = std::max(need.cells, std::max(cols, rows));
            for (const Particle& p : fx.particles) {
                if (p.flags & kParticleDelayed)
                    continue;
                const float3 pos = {p.position[0], p.position[1], p.position[2]};
                const float depth = dot(pos - camera.position, camera.forward);
                const float w = p.size[0] * r.scale, h = p.size[1] * r.scale;
                if (depth < -std::max(w, h) || p.color[3] <= 0.0f || w <= 0.0f || h <= 0.0f)
                    continue;
                SpriteGpu s = {};
                std::memcpy(s.positionSize, p.position, 12);
                s.positionSize[3] = w;
                std::memcpy(s.color, p.color, 16);
                std::memcpy(s.velocity, p.velocity, 12);
                s.velocity[3] = h;
                s.params[0] = (p.angles[2] + r.rotationOffset) * (kPi / 180.0f);
                float f = 0.0f;
                if (sheet && frames > 1) {
                    // Sheets per unit of sequence: its frames once through (looping).
                    const float t = p.sequence - std::floor(p.sequence);
                    f = t * float(frames);
                    if (snap)
                        f = std::floor(f);
                }
                s.params[1] = f;
                s.params[2] = r.depthFeather;
                s.params[3] = r.stretch;
                s.info[0] = material >= 0 ? uint32_t(material) : 0u;
                s.info[1] = flags;
                s.info[2] = cols | (rows << 8);
                s.info[3] = fade;
                if (material < 0)
                    s.info[1] |= 64u;  // no material: a plain white sprite
                if (depth > 0.0f)
                    need.pixels = std::max(need.pixels, std::max(w, h) * pixelScale / depth);
                order_.push_back({depth, uint32_t(sprites_.size())});
                sprites_.push_back(s);
            }
        }
    }
    needs_.erase(~0u);
    if (sprites_.empty())
        return false;
    // Back to front, across every effect (ties keep their order: an effect's own sorting).
    std::stable_sort(order_.begin(), order_.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    sorted_.resize(sprites_.size());
    for (size_t i = 0; i < order_.size(); ++i)
        sorted_[i] = sprites_[order_[i].second];
    count_ = uint32_t(sorted_.size());
    if (!ensureBuffer(count_)) {
        count_ = 0;
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
    std::memcpy(mapped, sorted_.data(), count_ * sizeof(SpriteGpu));
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "particle upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src = {upload_, 0};
    const SDL_GPUBufferRegion dst = {buffer_, 0, count_ * uint32_t(sizeof(SpriteGpu))};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);
    requestPages(frame);
    stats_.sprites = count_;
    stats_.materials = uint32_t(needs_.size());
    return true;
}

void ParticleRenderer::draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, uint64_t frame,
                            const DrawInputs& in)
{
    if (!count_ || !in.target || !in.depth || !in.lights || !in.vsm)
        return;
    FX_GPU_SECTION(cmd, "Particles");
    FX_GPU_PASS(cmd, "particle sprites");
    ParticleUniforms u = {};
    u.viewProj = in.rasterViewProj;
    u.cameraPos = f4(camera.position, camera.zNear);
    u.cameraRight = f4(camera.right, 0.0f);
    u.cameraUp = f4(camera.up, 0.0f);
    u.cameraForward = f4(camera.forward, 0.0f);
    u.sunDirection = f4(in.lights->sunDirection(), 0.0f);
    u.sunColor = f4(in.lights->sunColor(), 0.0f);
    u.params[0] = width;
    u.params[1] = height;
    u.params[2] = vt_->pageTableOffset();
    u.params[3] = uint32_t(frame);
    u.paramsF[0] = in.mipBias;
    u.paramsF[1] = 1.0f;
    u.gridOrderOffset = in.lights->orderOffset();
    u.gridTileOffset = in.lights->tileOffset();
    u.gridTilesX = in.lights->tilesX();
    u.gridWords = in.lights->words();
    u.gridBinScale = in.lights->binScale();
    u.lightCount = in.lights->count();
    u.vsm = in.vsm->params();
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
    if (!in.reactive)
        return;
    SDL_GPUColorTargetInfo targets[2] = {};
    targets[0].texture = in.target;
    targets[0].load_op = SDL_GPU_LOADOP_LOAD;
    targets[0].store_op = SDL_GPU_STOREOP_STORE;
    targets[1].texture = in.reactive;
    targets[1].load_op = in.clearReactive ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    targets[1].store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, targets, 2, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    const SDL_GPUTextureSamplerBinding vsmBinding = {in.vsm->atlas(), in.vsm->compareSampler()};
    SDL_BindGPUVertexSamplers(pass, 0, &vsmBinding, 1);
    SDL_GPUBuffer* vbuf[4] = {buffer_, in.lights->lights(), in.lights->grid(), in.vsm->state()};
    SDL_BindGPUVertexStorageBuffers(pass, 0, vbuf, 4);
    const SDL_GPUTextureSamplerBinding fsamplers[2] = {{vt_->physical(0), vt_->sampler()}, {in.depth, depthSampler_}};
    SDL_BindGPUFragmentSamplers(pass, 0, fsamplers, 2);
    SDL_GPUBuffer* fbuf[1] = {vt_->materialData()};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fbuf, 1);
    SDL_DrawGPUPrimitives(pass, 6, count_, 0, 0);
    SDL_EndGPURenderPass(pass);
}

}  // namespace fx
