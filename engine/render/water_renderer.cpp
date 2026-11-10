#include "render/water_renderer.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "water/water_system.h"

#include "water_composite.shader.h"
#include "water_ripples.shader.h"
#include "water_sspr.shader.h"
#include "water_surface.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
constexpr SDL_GPUTextureFormat kTargetFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr uint32_t kBodyFloats = 116;  // water_common.slang kWaterBodyStride / 4
constexpr uint32_t kMaxSplats = 256;
constexpr uint32_t kDetailSize = 256;
constexpr uint32_t kCausticSize = 256;
constexpr float kLodFactor = 2.0f;     // a level's patches are used up to this many patch sizes away

struct SurfaceUniforms {
    float4x4 viewProj;
    float4 camera;
    float4 cascades;
};
struct SsprUniforms {
    float4x4 viewProj;
    float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
    uint32_t params[4];
    float4 plane;
};
struct CopyUniforms {
    float4 invSize;
};
struct CompositeUniforms {
    float4x4 prevViewProj;
    float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
    uint32_t params[4];
    float4 sunDirection, sunColor;
    VsmParamsGpu vsm;
    ProbeVolumeGpu probes;
    float4 giParams;
    float4 time;
    float4 underwater;
    float4 mirror;
    float4 terrainGrid, terrainHeights;
    uint32_t trace[4];  // VT page-table byte offset, the BVH is there, alpha-masked holes
};

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

float4 mulPoint(const float4x4& m, float3 p)
{
    float4 r;
    r.x = m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0];
    r.y = m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1];
    r.z = m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2];
    r.w = m.m[0][3] * p.x + m.m[1][3] * p.y + m.m[2][3] * p.z + m.m[3][3];
    return r;
}

// ---- the detail texture: tileable value noise

uint32_t hash2(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

// Value noise in [0, 1] repeating every `cells` lattice cells across the unit square.
float periodicNoise(float u, float v, int32_t cells, uint32_t seed)
{
    const float x = u * float(cells), y = v * float(cells);
    const int32_t x0 = int32_t(std::floor(x)), y0 = int32_t(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    auto at = [&](int32_t i, int32_t j) {
        return float(hash2(((i % cells) + cells) % cells, ((j % cells) + cells) % cells, seed) & 0xffff) / 65535.0f;
    };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float fbm(float u, float v, int32_t cells, int32_t octaves, uint32_t seed)
{
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int32_t o = 0; o < octaves; ++o, cells *= 2, amp *= 0.5f) {
        sum += periodicNoise(u, v, cells, seed + uint32_t(o) * 31u) * amp;
        norm += amp;
    }
    return sum / norm;
}

}  // namespace

bool WaterRenderer::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    composite_ = gpu.createComputePipeline(shaders::water_composite_cs_composite);
    SDL_GPUShader* vsCopy = gpu.createShader(shaders::water_surface_vs_copy);
    SDL_GPUShader* fsCopy = gpu.createShader(shaders::water_surface_fs_copy);
    SDL_GPUShader* vs = gpu.createShader(shaders::water_surface_vs_surface);
    SDL_GPUShader* fs = gpu.createShader(shaders::water_surface_fs_surface);
    if (!composite_ || !vsCopy || !fsCopy || !vs || !fs) {
        FX_ERROR("water: shaders failed: %s", SDL_GetError());
        return false;
    }
    SDL_GPUColorTargetDescription targets[2] = {};
    targets[0].format = targets[1].format = kTargetFormat;
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // seen from below too
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = 2;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;
    // The scene's depth copied in: depth only (the colour targets keep their clear).
    info.vertex_shader = vsCopy;
    info.fragment_shader = fsCopy;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    targets[0].blend_state.enable_color_write_mask = targets[1].blend_state.enable_color_write_mask = true;
    targets[0].blend_state.color_write_mask = targets[1].blend_state.color_write_mask = 0;
    copy_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    targets[0].blend_state = targets[1].blend_state = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;  // reverse-Z
    surface_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    for (SDL_GPUShader* s : {vsCopy, fsCopy, vs, fs})
        SDL_ReleaseGPUShader(device_, s);
    if (!copy_ || !surface_) {
        FX_ERROR("water: pipelines failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler = {};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.max_lod = 16.0f;
    repeat_ = SDL_CreateGPUSampler(device_, &sampler);

    uint32_t mips = 0;
    for (uint32_t s = kDetailSize; s; s >>= 1)
        ++mips;
    SDL_GPUTextureCreateInfo tex = {};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex.width = tex.height = kDetailSize;
    tex.layer_count_or_depth = 1;
    tex.num_levels = mips;
    detail_ = gpumem::createTexture("Water", device_, &tex);
    gpumem::setTextureName(device_, detail_, "water detail");
    tex.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    caustics_ = gpumem::createTexture("Water", device_, &tex);
    gpumem::setTextureName(device_, caustics_, "water caustics");

    SDL_GPUBufferCreateInfo buf = {};
    buf.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    buf.size = kMaxPatches * sizeof(PatchGpu);
    patches_ = gpumem::createBuffer("Water", device_, &buf);
    gpumem::setBufferName(device_, patches_, "water patches");
    buf.size = WaterSystem::kMaxBodies * kBodyFloats * 4;
    bodies_ = gpumem::createBuffer("Water", device_, &buf);
    gpumem::setBufferName(device_, bodies_, "water bodies");
    buf.size = kMaxOutlinePoints * 8;
    outlines_ = gpumem::createBuffer("Water", device_, &buf);
    gpumem::setBufferName(device_, outlines_, "water outlines");
    static_assert(sizeof(PatchGpu) == 32);
    if (!ocean_.init(gpu))
        FX_WARN("water: ocean waves unavailable");
    rippleStep_ = gpu.createComputePipeline(shaders::water_ripples_cs_step);
    ssprClear_ = gpu.createComputePipeline(shaders::water_sspr_cs_clear);
    ssprProject_ = gpu.createComputePipeline(shaders::water_sspr_cs_project);
    ssprResolve_ = gpu.createComputePipeline(shaders::water_sspr_cs_resolve);
    SDL_GPUBufferCreateInfo splatInfo = {};
    splatInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    splatInfo.size = kMaxSplats * 16;
    splats_ = gpumem::createBuffer("Water", device_, &splatInfo);
    SDL_GPUSamplerCreateInfo clampInfo = {};
    clampInfo.min_filter = clampInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    clampInfo.address_mode_u = clampInfo.address_mode_v = clampInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    clamp_ = SDL_CreateGPUSampler(device_, &clampInfo);
    return repeat_ && detail_ && caustics_ && patches_ && bodies_ && outlines_;
}

void WaterRenderer::shutdown()
{
    if (!device_)
        return;
    SDL_ReleaseGPUGraphicsPipeline(device_, copy_);
    SDL_ReleaseGPUGraphicsPipeline(device_, surface_);
    SDL_ReleaseGPUComputePipeline(device_, composite_);
    SDL_ReleaseGPUSampler(device_, repeat_);
    for (SDL_GPUTexture* t : {depth_, normals_, extra_, detail_, caustics_})
        gpumem::releaseTexture(device_, t);
    gpumem::releaseBuffer(device_, patches_);
    gpumem::releaseBuffer(device_, bodies_);
    gpumem::releaseBuffer(device_, outlines_);
    gpumem::releaseTransferBuffer(device_, upload_);
    ocean_.shutdown();
    SDL_ReleaseGPUComputePipeline(device_, rippleStep_);
    for (SDL_GPUComputePipeline* p : {ssprClear_, ssprProject_, ssprResolve_})
        SDL_ReleaseGPUComputePipeline(device_, p);
    gpumem::releaseBuffer(device_, ssprKeys_);
    gpumem::releaseBuffer(device_, ssprSources_);
    for (SDL_GPUTexture* t : rippleState_)
        gpumem::releaseTexture(device_, t);
    gpumem::releaseBuffer(device_, splats_);
    SDL_ReleaseGPUSampler(device_, clamp_);
    device_ = nullptr;
}

void WaterRenderer::setRipples(bool on, float extent, uint32_t resolution)
{
    ripplesOn_ = on;
    rippleExtent_ = std::clamp(extent, 8.0f, 1024.0f);
    resolution = std::clamp(resolution, 64u, 2048u);
    if (resolution != rippleN_) {
        for (SDL_GPUTexture*& t : rippleState_) {
            gpumem::releaseTexture(device_, t);
            t = nullptr;
        }
        rippleN_ = resolution;
        rippleValid_ = false;
    }
}

// One frame of ripples: the square follows the camera in whole texels; splats from the water
// system (physics, scripts) and the camera at the surface; steps of 1/60 s for the water time
// that passed (none while it stands: captures stay deterministic). Splats wait for a frame that
// steps (above 60 fps most don't: a short impact's would be lost).
void WaterRenderer::stepRipples(SDL_GPUCommandBuffer* cmd, const CameraState& camera)
{
    ripplesActive_ = false;
    std::vector<WaterSystem::Splash>& splashes = splashes_;
    {
        const std::vector<WaterSystem::Splash> fresh = system_->takeSplashes();
        if (splashes.size() < kMaxSplats)
            splashes.insert(splashes.end(), fresh.begin(), fresh.end());
    }
    if (!ripplesOn_ || !rippleStep_ || !splats_) {
        splashes.clear();
        return;
    }
    bool any = false;
    for (const WaterSystem::Body& b : system_->bodies())
        any |= b.alive && b.visible && !b.hidden && b.material.ripples && b.material.rippleStrength > 0.0f;
    if (!any) {
        splashes.clear();
        return;
    }
    if (!rippleState_[0]) {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        info.width = info.height = rippleN_;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        for (SDL_GPUTexture*& t : rippleState_) {
            t = gpumem::createTexture("Water", device_, &info);
            if (!t)
                return;
        }
        gpumem::setTextureName(device_, rippleState_[0], "water ripples A");
        gpumem::setTextureName(device_, rippleState_[1], "water ripples B");
        rippleValid_ = false;
    }
    const float texel = rippleExtent_ / float(rippleN_);
    const int32_t origin[2] = {int32_t(std::floor(camera.position.x / texel)) - int32_t(rippleN_ / 2),
                               int32_t(std::floor(camera.position.z / texel)) - int32_t(rippleN_ / 2)};
    // The camera at the surface (wading, a boat's view): its own wake.
    if (const WaterSample s = system_->sample(camera.position); s.body >= 0 && std::fabs(s.depth) < 0.4f) {
        const float moved = length(camera.position - lastCamera_);
        if (moved > 0.01f && moved < 2.0f && splashes.size() < kMaxSplats)
            splashes.push_back({camera.position.x, camera.position.z, 0.35f, std::min(moved, 0.3f) * 0.05f});
    }
    lastCamera_ = camera.position;
    // Steps for the time that passed (at most 4 a frame).
    const float dt = rippleTime_ < 0.0f ? 0.0f : std::clamp(time_ - rippleTime_, 0.0f, 0.25f);
    rippleTime_ = time_;
    rippleCarry_ += dt * 60.0f;
    uint32_t steps = uint32_t(rippleCarry_);
    rippleCarry_ -= float(steps);
    steps = std::min(steps, 4u);
    if (!rippleValid_)  // (new: still water, the square where the camera is)
        steps = std::max(steps, 1u);
    if (steps == 0) {
        ripplesActive_ = rippleValid_;
        return;
    }
    if (splashes.size() > kMaxSplats)
        splashes.resize(kMaxSplats);
    if (!splashes.empty()) {
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(splashes.size() * 16), 0};
        if (SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &info)) {
            std::memcpy(SDL_MapGPUTransferBuffer(device_, tb, false), splashes.data(), splashes.size() * 16);
            SDL_UnmapGPUTransferBuffer(device_, tb);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTransferBufferLocation src = {tb, 0};
            const SDL_GPUBufferRegion dst = {splats_, 0, uint32_t(splashes.size() * 16)};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(device_, tb);
        }
    }
    FX_GPU_PASS(cmd, "water ripples");
    for (uint32_t s = 0; s < steps; ++s) {
        struct {
            int32_t shift[4];
            float area[4], wave[4];
        } u = {};
        // The first step moves the square (what was at texel t + shift is at t now); the splats
        // go in once, spread over the frame's steps.
        u.shift[0] = s == 0 && rippleValid_ ? origin[0] - rippleOrigin_[0] : 0;
        u.shift[1] = s == 0 && rippleValid_ ? origin[1] - rippleOrigin_[1] : 0;
        if (s == 0 && !rippleValid_)
            u.shift[0] = int32_t(rippleN_) * 4;  // (everything comes in still)
        u.shift[2] = s == 0 ? int32_t(splashes.size()) : 0;
        u.shift[3] = int32_t(rippleN_);
        u.area[0] = float(origin[0]) * texel;
        u.area[1] = float(origin[1]) * texel;
        u.area[2] = texel;
        u.area[3] = 0.985f;
        u.wave[0] = 0.45f;
        u.wave[1] = 1.0f;
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
        const SDL_GPUStorageTextureReadWriteBinding rw[2] = {{rippleState_[rippleCurrent_], 0, 0, false},
                                                             {rippleState_[1 - rippleCurrent_], 0, 0, false}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, rw, 2, nullptr, 0);
        SDL_BindGPUComputePipeline(pass, rippleStep_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &splats_, 1);
        SDL_DispatchGPUCompute(pass, (rippleN_ + 7) / 8, (rippleN_ + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
        rippleCurrent_ = 1 - rippleCurrent_;
    }
    splashes.clear();
    rippleOrigin_[0] = origin[0];
    rippleOrigin_[1] = origin[1];
    rippleValid_ = true;
    ripplesActive_ = true;
}

bool WaterRenderer::ensureTargets(uint32_t width, uint32_t height)
{
    if (depth_ && targetW_ == width && targetH_ == height)
        return true;
    for (SDL_GPUTexture* t : {depth_, normals_, extra_})
        gpumem::releaseTexture(device_, t);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depth_ = gpumem::createTexture("Water", device_, &info);
    info.format = kTargetFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    normals_ = gpumem::createTexture("Water", device_, &info);
    extra_ = gpumem::createTexture("Water", device_, &info);
    if (!depth_ || !normals_ || !extra_)
        return false;
    gpumem::setTextureName(device_, depth_, "water depth");
    gpumem::setTextureName(device_, normals_, "water normals");
    gpumem::setTextureName(device_, extra_, "water foam + body");
    targetW_ = width;
    targetH_ = height;
    return true;
}

// Once: the tileable detail texture (rg: ripple slope, b: foam pattern, a: film pattern), with its
// mips (2x2 averages, wrapping).
void WaterRenderer::buildDetail(SDL_GPUCommandBuffer* cmd)
{
    std::vector<float> height(kDetailSize * kDetailSize);
    std::vector<uint8_t> texels(kDetailSize * kDetailSize * 4);
    const float inv = 1.0f / float(kDetailSize);
    for (uint32_t y = 0; y < kDetailSize; ++y)
        for (uint32_t x = 0; x < kDetailSize; ++x) {
            const float u = (float(x) + 0.5f) * inv, v = (float(y) + 0.5f) * inv;
            // Ripples: noise stretched across the wind a little, sharpened toward crests.
            const float n = fbm(u, v, 6, 4, 11);
            height[y * kDetailSize + x] = 1.0f - std::fabs(n * 2.0f - 1.0f) * 0.6f + fbm(u, v, 12, 3, 23) * 0.4f;
        }
    for (uint32_t y = 0; y < kDetailSize; ++y)
        for (uint32_t x = 0; x < kDetailSize; ++x) {
            auto h = [&](int32_t i, int32_t j) {
                return height[((j + kDetailSize) % kDetailSize) * kDetailSize + (i + kDetailSize) % kDetailSize];
            };
            const float gain = float(kDetailSize) * 0.01f;
            const float sx = (h(int32_t(x) + 1, int32_t(y)) - h(int32_t(x) - 1, int32_t(y))) * 0.5f * gain;
            const float sy = (h(int32_t(x), int32_t(y) + 1) - h(int32_t(x), int32_t(y) - 1)) * 0.5f * gain;
            const float u = (float(x) + 0.5f) * inv, v = (float(y) + 0.5f) * inv;
            const float foam = std::clamp((fbm(u, v, 8, 4, 41) - 0.5f) * 2.2f + 0.5f, 0.0f, 1.0f);
            const float film = std::clamp((fbm(u, v, 3, 4, 53) - 0.5f) * 2.0f + 0.5f, 0.0f, 1.0f);
            uint8_t* t = &texels[(y * kDetailSize + x) * 4];
            t[0] = uint8_t(std::clamp(0.5f + sx, 0.0f, 1.0f) * 255.0f + 0.5f);
            t[1] = uint8_t(std::clamp(0.5f + sy, 0.0f, 1.0f) * 255.0f + 0.5f);
            t[2] = uint8_t(foam * 255.0f + 0.5f);
            t[3] = uint8_t(film * 255.0f + 0.5f);
        }
    // Caustics: sunlight through a rippled surface (its slopes bend each ray) onto a floor below,
    // gathered per texel (photons, wrapping), blurred a little; 1 = the light that fell on the
    // surface, stored as a quarter.
    std::vector<uint8_t> caustic(kCausticSize * kCausticSize);
    {
        const uint32_t n = kCausticSize;
        std::vector<float> h(n * n), light(n * n, 0.0f), blurred(n * n);
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x)
                h[y * n + x] = fbm((float(x) + 0.5f) / float(n), (float(y) + 0.5f) / float(n), 6, 3, 71);
        // How far below the light lands: where the steepest slopes bend it about half a ripple
        // (6 across), the rays cross into bright lines.
        float steepest = 1e-6f;
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x)
                steepest = std::max(steepest, std::fabs(h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n]) * 0.5f);
        const float focus = float(n) / 6.0f * 0.55f / steepest;
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x) {
                const float sx = (h[y * n + (x + 1) % n] - h[y * n + (x + n - 1) % n]) * 0.5f;
                const float sy = (h[((y + 1) % n) * n + x] - h[((y + n - 1) % n) * n + x]) * 0.5f;
                const float fx = float(x) - sx * focus, fy = float(y) - sy * focus;
                const int32_t ix = int32_t(std::floor(fx)), iy = int32_t(std::floor(fy));
                const float ax = fx - float(ix), ay = fy - float(iy);
                auto add = [&](int32_t i, int32_t j, float w) {
                    light[size_t(((j % int32_t(n)) + int32_t(n)) % int32_t(n)) * n + size_t(((i % int32_t(n)) + int32_t(n)) % int32_t(n))] += w;
                };
                add(ix, iy, (1 - ax) * (1 - ay));
                add(ix + 1, iy, ax * (1 - ay));
                add(ix, iy + 1, (1 - ax) * ay);
                add(ix + 1, iy + 1, ax * ay);
            }
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x) {
                float s = 0.0f;
                for (int32_t j = -1; j <= 1; ++j)
                    for (int32_t i = -1; i <= 1; ++i)
                        s += light[((y + n + uint32_t(j)) % n) * n + (x + n + uint32_t(i)) % n] * (i == 0 && j == 0 ? 4.0f : (i == 0 || j == 0) ? 2.0f : 1.0f);
                blurred[y * n + x] = s / 16.0f;
            }
        for (uint32_t i = 0; i < n * n; ++i)
            caustic[i] = uint8_t(std::clamp(blurred[i] * 0.25f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    std::vector<uint8_t> causticAll = caustic;
    std::vector<uint32_t> causticOffsets = {0};
    for (uint32_t size = kCausticSize; size > 1; size /= 2) {
        const uint8_t* above = &causticAll[causticOffsets.back()];
        const uint32_t half = size / 2;
        causticOffsets.push_back(uint32_t(causticAll.size()));
        std::vector<uint8_t> level(half * half);
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x)
                level[y * half + x] = uint8_t((above[(y * 2) * size + x * 2] + above[(y * 2) * size + x * 2 + 1] + above[(y * 2 + 1) * size + x * 2] +
                                               above[(y * 2 + 1) * size + x * 2 + 1] + 2) / 4);
        causticAll.insert(causticAll.end(), level.begin(), level.end());
    }

    std::vector<uint8_t> all = texels;
    std::vector<uint32_t> offsets = {0};
    for (uint32_t size = kDetailSize; size > 1; size /= 2) {
        const uint8_t* above = &all[offsets.back()];
        const uint32_t half = size / 2;
        offsets.push_back(uint32_t(all.size()));
        std::vector<uint8_t> level(half * half * 4);
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x)
                for (uint32_t c = 0; c < 4; ++c) {
                    const uint32_t sum = above[((y * 2) * size + x * 2) * 4 + c] + above[((y * 2) * size + x * 2 + 1) * 4 + c] +
                                         above[((y * 2 + 1) * size + x * 2) * 4 + c] + above[((y * 2 + 1) * size + x * 2 + 1) * 4 + c];
                    level[(y * half + x) * 4 + c] = uint8_t((sum + 2) / 4);
                }
        all.insert(all.end(), level.begin(), level.end());
    }
    const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(all.size() + causticAll.size()), 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &info);
    if (!tb)
        return;
    uint8_t* mappedDetail = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
    std::memcpy(mappedDetail, all.data(), all.size());
    std::memcpy(mappedDetail + all.size(), causticAll.data(), causticAll.size());
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (uint32_t mip = 0, size = kCausticSize; mip < causticOffsets.size(); ++mip, size /= 2) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = tb;
        src.offset = uint32_t(all.size()) + causticOffsets[mip];
        src.pixels_per_row = src.rows_per_layer = size;
        SDL_GPUTextureRegion dst = {};
        dst.texture = caustics_;
        dst.mip_level = mip;
        dst.w = dst.h = size;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    for (uint32_t mip = 0, size = kDetailSize; mip < offsets.size(); ++mip, size /= 2) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = tb;
        src.offset = offsets[mip];
        src.pixels_per_row = src.rows_per_layer = size;
        SDL_GPUTextureRegion dst = {};
        dst.texture = detail_;
        dst.mip_level = mip;
        dst.w = dst.h = size;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
    detailReady_ = true;
}

// CDLOD selection over a body's square root node (in its local plane): a node is drawn at its
// level where the camera is within its level's range but not its children's; children out of
// their range are drawn at their size with their vertices fully morphed (their parent's grid).
void WaterRenderer::selectPatches(uint32_t bodyId, const CameraState& camera, const float4x4& viewProj)
{
    const WaterSystem::Body& b = system_->bodies()[bodyId];
    const WaterBodyDesc& d = b.desc;
    if (d.shape == WaterShape::River) {
        // Stretches of up to 32 spans of the path, each culled by its box.
        const uint32_t count = uint32_t(b.path.size());
        const float pad = b.waveAmplitude * 2.0f + d.width * 0.5f;
        for (uint32_t first = 0; first + 1 < count && patchList_.size() < kMaxPatches; first += 32) {
            const uint32_t spans = std::min(32u, count - 1 - first);
            float lo3[3] = {1e30f, 1e30f, 1e30f}, hi3[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t k = first; k <= first + spans; ++k) {
                const float2 w = waterBodyToWorld(b, {b.path[k].x, b.path[k].z});
                const float y = b.position.y + b.path[k].y;
                lo3[0] = std::min(lo3[0], w.x), hi3[0] = std::max(hi3[0], w.x);
                lo3[1] = std::min(lo3[1], y), hi3[1] = std::max(hi3[1], y);
                lo3[2] = std::min(lo3[2], w.y), hi3[2] = std::max(hi3[2], w.y);
            }
            int out[5] = {};
            for (int c = 0; c < 8; ++c) {
                const float4 clip = mulPoint(viewProj, {(c & 1) ? hi3[0] + pad : lo3[0] - pad, (c & 2) ? hi3[1] + pad : lo3[1] - pad,
                                                        (c & 4) ? hi3[2] + pad : lo3[2] - pad});
                out[0] += clip.x > clip.w;
                out[1] += clip.x < -clip.w;
                out[2] += clip.y > clip.w;
                out[3] += clip.y < -clip.w;
                out[4] += clip.w <= 0.0f;
            }
            if (out[0] == 8 || out[1] == 8 || out[2] == 8 || out[3] == 8 || out[4] == 8)
                continue;
            patchList_.push_back({float(first), float(spans), d.width, 1e30f, 1e30f, bodyId, 6, 0});
        }
        return;
    }
    float lo[2], hi[2];
    switch (d.shape) {
    case WaterShape::Ocean: {
        // Endless: a square around the camera, 16 km across, moved in steps of a quarter of it
        // (every level's patches stay on their own grid; only the coarsest two shift).
        const float root = kLeafSize * float(1u << 11), step = root * 0.25f;
        const float2 c = waterWorldToBody(b, camera.position.x, camera.position.z);
        lo[0] = std::floor(c.x / step) * step - root * 0.5f;
        lo[1] = std::floor(c.y / step) * step - root * 0.5f;
        hi[0] = lo[0] + root;
        hi[1] = lo[1] + root;
        break;
    }
    case WaterShape::Circle: lo[0] = lo[1] = -d.width * 0.5f; hi[0] = hi[1] = d.width * 0.5f; break;
    case WaterShape::Polygon:
        lo[0] = lo[1] = 1e30f;
        hi[0] = hi[1] = -1e30f;
        for (size_t i = 0; i + 1 < d.points.size(); i += 2)
            for (int a = 0; a < 2; ++a) {
                lo[a] = std::min(lo[a], d.points[i + a]);
                hi[a] = std::max(hi[a], d.points[i + a]);
            }
        if (lo[0] > hi[0])
            return;
        break;
    default:
        lo[0] = -d.width * 0.5f;
        lo[1] = -d.length * 0.5f;
        hi[0] = d.width * 0.5f;
        hi[1] = d.length * 0.5f;
    }
    const float extent = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    uint32_t levels = 0;
    while (kLeafSize * float(1u << levels) < extent && levels < 14)
        ++levels;
    const float rootSize = kLeafSize * float(1u << levels);
    const float cx = (lo[0] + hi[0]) * 0.5f, cz = (lo[1] + hi[1]) * 0.5f;
    const float pad = system_->usesOcean(b) ? system_->ocean().maxAmplitude * (1.0f + system_->ocean().choppiness)
                                             : b.waveAmplitude * 2.0f;  // waves move the surface up, down and sideways
    auto range = [&](uint32_t level) { return level >= levels ? 1e30f : kLeafSize * float(1u << level) * kLodFactor; };

    // A node's world box (its square, the waves' height) against the view.
    auto visible = [&](float ox, float oz, float size, float& distance) {
        const float qx[2] = {std::max(ox, lo[0]) - pad, std::min(ox + size, hi[0]) + pad};
        const float qz[2] = {std::max(oz, lo[1]) - pad, std::min(oz + size, hi[1]) + pad};
        if (qx[0] > qx[1] || qz[0] > qz[1])
            return false;  // outside the shape's bounds
        int out[5] = {};
        float3 corners[8];
        for (int c = 0; c < 8; ++c) {
            const float2 w = waterBodyToWorld(b, {qx[c & 1], qz[(c >> 1) & 1]});
            corners[c] = {w.x, b.position.y + ((c & 4) ? pad : -pad), w.y};
            const float4 clip = mulPoint(viewProj, corners[c]);
            out[0] += clip.x > clip.w;
            out[1] += clip.x < -clip.w;
            out[2] += clip.y > clip.w;
            out[3] += clip.y < -clip.w;
            out[4] += clip.w <= 0.0f;
        }
        for (int k = 0; k < 5; ++k)
            if (out[k] == 8)
                return false;
        // Distance from the camera to the node's box (the morph uses vertex distances).
        const float2 c = waterBodyToWorld(b, {(qx[0] + qx[1]) * 0.5f, (qz[0] + qz[1]) * 0.5f});
        const float half = 0.70710678f * std::max(qx[1] - qx[0], qz[1] - qz[0]);
        const float3 dc = float3{c.x, b.position.y, c.y} - camera.position;
        distance = std::max(length(dc) - half - pad, 0.0f);
        return true;
    };
    auto emit = [&](float ox, float oz, float size, uint32_t level) {
        if (patchList_.size() >= kMaxPatches)
            return;
        const float r = range(level);
        patchList_.push_back({ox, oz, size, r >= 1e29f ? 1e30f : r * 0.7f, r, bodyId, 0, 0});
    };
    struct Node {
        float ox, oz, size;
        uint32_t level;
    };
    // A tank: its four sides and its bottom, one patch each (32 x 32 quads, no levels; five
    // patches, not worth culling: its top's box test would miss walls seen from inside).
    if (d.shape == WaterShape::Box && patchList_.size() + 5 <= kMaxPatches) {
        patchList_.push_back({lo[1], 0.0f, hi[1] - lo[1], 1e30f, 1e30f, bodyId, 1, 0});
        patchList_.push_back({lo[1], 0.0f, hi[1] - lo[1], 1e30f, 1e30f, bodyId, 2, 0});
        patchList_.push_back({lo[0], 0.0f, hi[0] - lo[0], 1e30f, 1e30f, bodyId, 3, 0});
        patchList_.push_back({lo[0], 0.0f, hi[0] - lo[0], 1e30f, 1e30f, bodyId, 4, 0});
        patchList_.push_back({lo[0], lo[1], hi[0] - lo[0], hi[1] - lo[1], 1e30f, bodyId, 5, 0});
    }
    std::vector<Node> stack = {{cx - rootSize * 0.5f, cz - rootSize * 0.5f, rootSize, levels}};
    while (!stack.empty()) {
        const Node n = stack.back();
        stack.pop_back();
        float dist;
        if (!visible(n.ox, n.oz, n.size, dist))
            continue;
        if (n.level == 0 || dist > range(n.level - 1)) {
            emit(n.ox, n.oz, n.size, n.level);
            continue;
        }
        const float h = n.size * 0.5f;
        for (int c = 0; c < 4; ++c) {
            const Node child = {n.ox + ((c & 1) ? h : 0.0f), n.oz + ((c & 2) ? h : 0.0f), h, n.level - 1};
            float cd;
            if (!visible(child.ox, child.oz, child.size, cd))
                continue;
            if (cd > range(child.level))
                emit(child.ox, child.oz, child.size, child.level);  // (its vertices fully morphed)
            else
                stack.push_back(child);
        }
    }
}

bool WaterRenderer::prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height)
{
    active_ = false;
    if (!system_ || !surface_ || system_->count() == 0)
        return false;
    FX_ZONE_CAT("WaterRenderer::prepare", Rendering);
    if (!detailReady_)
        buildDetail(cmd);
    time_ = system_->time();

    // Bodies: their parameter blocks (water_common.slang WaterBody).
    const std::vector<WaterSystem::Body>& bodies = system_->bodies();
    bodyCount_ = uint32_t(bodies.size());
    bodyData_.assign(size_t(bodyCount_) * kBodyFloats, 0.0f);
    outlineData_.clear();
    for (uint32_t i = 0; i < bodyCount_; ++i) {
        const WaterSystem::Body& b = bodies[i];
        if (!b.alive)
            continue;
        const WaterMaterial& m = b.material;
        float* o = &bodyData_[size_t(i) * kBodyFloats];
        auto put = [&](uint32_t slot, float a, float bb, float c, float dd) {
            o[slot * 4 + 0] = a;
            o[slot * 4 + 1] = bb;
            o[slot * 4 + 2] = c;
            o[slot * 4 + 3] = dd;
        };
        const float flowAngle = m.windDirection * 3.14159265f / 180.0f;
        put(0, b.position.x, b.position.y, b.position.z, b.yaw);
        put(1, float(uint32_t(b.desc.shape)), b.desc.width * 0.5f, b.desc.depth, b.desc.length * 0.5f);
        put(2, m.deepColor[0], m.deepColor[1], m.deepColor[2], m.clarity);
        put(3, m.shallowColor[0], m.shallowColor[1], m.shallowColor[2], m.scattering);
        put(4, m.murk, m.refraction, m.roughness, m.reflection);
        put(5, m.normalStrength, m.detailScale, m.detailSpeed, m.film);
        put(6, m.filmColor[0], m.filmColor[1], m.filmColor[2], m.filmScale);
        put(7, m.foamColor[0], m.foamColor[1], m.foamColor[2], m.shoreFoam);
        put(8, m.crestFoam, m.foamScale, m.edgeFade, m.shoreFade);
        put(9, m.caustics, m.causticScale, m.causticDepth, m.flowSpeed);
        put(10, m.fogColor[0], m.fogColor[1], m.fogColor[2], m.fogDensity);
        put(11, std::cos(flowAngle), std::sin(flowAngle), system_->usesOcean(b) ? -1.0f : float(b.waveCount), -1.0f);
        put(28, m.ripples ? m.rippleStrength : 0.0f, m.rippleDamping, 0.0f, 0.0f);
        if (b.desc.shape == WaterShape::River) {  // its path (x, z, height, distance: two slots a point)
            const uint32_t first = uint32_t(outlineData_.size() / 2), count = uint32_t(b.path.size());
            if (count >= 2 && first + count * 2 <= kMaxOutlinePoints) {
                for (const WaterSystem::Body::PathPoint& q : b.path)
                    outlineData_.insert(outlineData_.end(), {q.x, q.z, q.y, q.s});
                o[1 * 4 + 1] = float(count);
                o[11 * 4 + 3] = float(first);
            }
        }
        if (b.desc.shape == WaterShape::Polygon) {  // its points into the outline buffer
            const uint32_t first = uint32_t(outlineData_.size() / 2), count = uint32_t(b.desc.points.size() / 2);
            if (count >= 3 && first + count <= kMaxOutlinePoints) {
                outlineData_.insert(outlineData_.end(), b.desc.points.begin(), b.desc.points.begin() + count * 2);
                o[1 * 4 + 1] = float(count);
                o[11 * 4 + 3] = float(first);
            } else {
                o[1 * 4 + 1] = 0.0f;  // (nothing inside)
                o[11 * 4 + 3] = 0.0f;
            }
        }
        for (uint32_t w = 0; w < b.waveCount; ++w) {
            const GerstnerWave& g = b.waves[w];
            put(12 + w, g.dirX, g.dirZ, g.amplitude, g.k);
            put(20 + w, g.omega, g.steepness, g.phase, 0.0f);
        }
    }

    // Patches of every visible body, and the screen rectangle they may cover.
    patchList_.clear();
    for (uint32_t i = 0; i < bodyCount_; ++i)
        if (bodies[i].alive && bodies[i].visible && !bodies[i].hidden)
            selectPatches(i, camera, camera.viewProj);
    patchCount_ = uint32_t(patchList_.size());
    if (patchCount_ != loggedPatches_) {
        FX_INFO("water: %u bodies, %u patches", system_->count(), patchCount_);
        loggedPatches_ = patchCount_;
    }
    // The planar reflections' body: the one with the most surface patches in view (a river's
    // surface is not one plane).
    {
        std::vector<uint32_t> count(bodyCount_, 0);
        for (const PatchGpu& p : patchList_)
            if (p.face == 0)
                ++count[p.body];
        mirrorBody_ = -1;
        uint32_t most = 0;
        for (uint32_t i = 0; i < bodyCount_; ++i)
            if (count[i] > most) {
                most = count[i];
                mirrorBody_ = int32_t(i);
            }
    }
    // The body the camera is in: under its surface, within its outline and depth (oceans: any).
    cameraBody_ = -1;
    if (const WaterSample s = system_->sample(camera.position); s.body >= 0 && s.depth > 0.0f) {
        const WaterSystem::Body& b = bodies[size_t(s.body)];
        if (b.desc.shape == WaterShape::Ocean || s.depth < b.desc.depth)
            cameraBody_ = s.body;
    }
    if (!patchCount_ && cameraBody_ < 0)
        return false;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool fullScreen = cameraBody_ >= 0;  // (under water: every pixel)
    for (const PatchGpu& p : patchList_) {
        const WaterSystem::Body& b = bodies[p.body];
        if (p.face == 6) {  // (a river: the whole screen, cheaply right; its stretches are culled above)
            fullScreen = true;
            break;
        }
        const float pad = system_->usesOcean(b) ? system_->ocean().maxAmplitude * 2.0f : b.waveAmplitude * 2.0f;
        for (int c = 0; c < 8 && !fullScreen; ++c) {
            const float2 w = waterBodyToWorld(b, {p.originX + ((c & 1) ? p.size + pad : -pad), p.originZ + ((c & 2) ? p.size + pad : -pad)});
            const float4 clip = mulPoint(camera.viewProj, {w.x, b.position.y + ((c & 4) ? pad : -pad), w.y});
            if (clip.w <= camera.zNear) {
                fullScreen = true;
                break;
            }
            const float sx = (clip.x / clip.w * 0.5f + 0.5f) * float(width), sy = (0.5f - clip.y / clip.w * 0.5f) * float(height);
            x0 = std::min(x0, sx);
            y0 = std::min(y0, sy);
            x1 = std::max(x1, sx);
            y1 = std::max(y1, sy);
        }
    }
    if (fullScreen) {
        rect_[0] = rect_[1] = 0;
        rect_[2] = int32_t(width);
        rect_[3] = int32_t(height);
    } else {
        rect_[0] = std::clamp(int32_t(std::floor(x0)) - 2, 0, int32_t(width));
        rect_[1] = std::clamp(int32_t(std::floor(y0)) - 2, 0, int32_t(height));
        rect_[2] = std::clamp(int32_t(std::ceil(x1)) + 2, 0, int32_t(width));
        rect_[3] = std::clamp(int32_t(std::ceil(y1)) + 2, 0, int32_t(height));
        if (rect_[2] <= rect_[0] || rect_[3] <= rect_[1])
            return false;
    }
    if (!ensureTargets(width, height))
        return false;
    // The ocean's waves for this frame (when a body in view has them).
    bool oceanInView = false;
    for (const PatchGpu& p : patchList_)
        oceanInView |= system_->usesOcean(bodies[p.body]);
    if (oceanInView)
        ocean_.update(cmd, system_->ocean(), time_, bodies[size_t(std::max(system_->oceanBody(), 0))].material.crestFoam);
    stepRipples(cmd, camera);

    // Upload: bodies, then patches.
    const uint32_t bodyBytes = bodyCount_ * kBodyFloats * 4, patchBytes = patchCount_ * uint32_t(sizeof(PatchGpu));
    const uint32_t outlineBytes = uint32_t(outlineData_.size() * 4);
    const uint32_t bytes = bodyBytes + patchBytes + outlineBytes;
    if (bytes > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = std::max(bytes + bytes / 2, 64u << 10);
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Water", device_, &info, "water upload");
        if (!upload_) {
            uploadBytes_ = 0;
            return false;
        }
    }
    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    std::memcpy(mapped, bodyData_.data(), bodyBytes);
    std::memcpy(mapped + bodyBytes, patchList_.data(), patchBytes);
    if (outlineBytes)
        std::memcpy(mapped + bodyBytes + patchBytes, outlineData_.data(), outlineBytes);
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "water upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {upload_, 0};
    SDL_GPUBufferRegion dst = {bodies_, 0, bodyBytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    src.offset = bodyBytes;
    dst = {patches_, 0, patchBytes};
    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    if (outlineBytes) {
        src.offset = bodyBytes + patchBytes;
        dst = {outlines_, 0, outlineBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(copy);
    active_ = true;
    return true;
}

void WaterRenderer::raster(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* sceneDepth, SDL_GPUSampler* pointSampler,
                           const float4x4& rasterViewProj, uint32_t width, uint32_t height)
{
    if (!active_)
        return;
    FX_ZONE_CAT("WaterRenderer::raster", Rendering);
    FX_GPU_SECTION(cmd, "Water");
    FX_GPU_PASS(cmd, "water surface");
    SDL_GPUColorTargetInfo colors[2] = {};
    for (int i = 0; i < 2; ++i) {
        colors[i].texture = i == 0 ? normals_ : extra_;
        colors[i].load_op = SDL_GPU_LOADOP_CLEAR;
        colors[i].store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depth_;
    depth.load_op = SDL_GPU_LOADOP_DONT_CARE;  // every pixel written by the copy
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, colors, 2, &depth);
    const SDL_GPUViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);

    SDL_BindGPUGraphicsPipeline(pass, copy_);
    const CopyUniforms cu = {{1.0f / float(width), 1.0f / float(height), 0.0f, 0.0f}};
    SDL_PushGPUFragmentUniformData(cmd, 0, &cu, sizeof(cu));
    const SDL_GPUTextureSamplerBinding sceneBinding = {sceneDepth, pointSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sceneBinding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    SDL_BindGPUGraphicsPipeline(pass, surface_);
    const OceanSpectrum& spectrum = system_->ocean();
    const float4 cascades = {spectrum.sizes[0] > 0.0f ? spectrum.sizes[0] : 1.0f, spectrum.sizes[1] > 0.0f ? spectrum.sizes[1] : 1.0f,
                             spectrum.sizes[2] > 0.0f ? spectrum.sizes[2] : 1.0f, float(std::max(spectrum.n, 1u))};
    SurfaceUniforms su = {};
    su.viewProj = rasterViewProj;
    su.camera = {0.0f, 0.0f, 0.0f, time_};
    su.cascades = cascades;
    SDL_PushGPUVertexUniformData(cmd, 0, &su, sizeof(su));
    const float texel = rippleExtent_ / float(rippleN_);
    const float4 fu[3] = {{0.0f, 0.0f, 0.0f, time_}, cascades,
                          {float(rippleOrigin_[0]) * texel, float(rippleOrigin_[1]) * texel, ripplesActive_ ? rippleExtent_ : 0.0f,
                           1.0f / float(rippleN_)}};
    SDL_PushGPUFragmentUniformData(cmd, 0, fu, sizeof(fu));
    const bool ocean = ocean_.ready();
    SDL_GPUTextureSamplerBinding displacement[3], slopes[5] = {{detail_, repeat_}};
    for (uint32_t c = 0; c < 3; ++c) {
        displacement[c] = {ocean ? ocean_.displacement(c) : ocean_.placeholder(), repeat_};
        slopes[1 + c] = {ocean ? ocean_.slopes(c) : ocean_.placeholder(), repeat_};
    }
    SDL_BindGPUVertexSamplers(pass, 0, displacement, 3);
    SDL_GPUBuffer* vertexBuffers[3] = {patches_, bodies_, outlines_};
    SDL_BindGPUVertexStorageBuffers(pass, 0, vertexBuffers, 3);
    slopes[4] = {ripplesActive_ ? rippleState_[rippleCurrent_] : ocean_.placeholder(), clamp_};
    SDL_BindGPUFragmentSamplers(pass, 0, slopes, 5);
    SDL_GPUBuffer* fragmentBuffers[2] = {bodies_, outlines_};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, fragmentBuffers, 2);
    if (patchCount_)
        SDL_DrawGPUPrimitives(pass, 32 * 32 * 6, patchCount_, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void WaterRenderer::composite(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height,
                              const CompositeInputs& in)
{
    if (!active_)
        return;
    FX_ZONE_CAT("WaterRenderer::composite", Rendering);
    // Planar reflections for the mirror body's plane (not from under the water).
    const uint32_t pixels = width * height;
    bool mirror = mirrorBody_ >= 0 && cameraBody_ < 0 && ssprClear_ && ssprProject_ && ssprResolve_;
    if (mirror && ssprPixels_ < pixels) {
        gpumem::releaseBuffer(device_, ssprKeys_);
        gpumem::releaseBuffer(device_, ssprSources_);
        SDL_GPUBufferCreateInfo info = {};
        info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        info.size = pixels * 4;
        ssprKeys_ = gpumem::createBuffer("Water", device_, &info);
        ssprSources_ = gpumem::createBuffer("Water", device_, &info);
        ssprPixels_ = ssprKeys_ && ssprSources_ ? pixels : 0;
        if (ssprPixels_) {
            gpumem::setBufferName(device_, ssprKeys_, "water mirror keys");
            gpumem::setBufferName(device_, ssprSources_, "water mirror sources");
        }
    }
    mirror = mirror && ssprPixels_ >= pixels;
    if (mirror) {
        SsprUniforms su = {};
        su.viewProj = in.rasterViewProj;
        su.cameraPos = f4(camera.position, camera.zNear);
        su.cameraRight = f4(camera.right, 1.0f / camera.p00);
        su.cameraUp = f4(camera.up, 1.0f / camera.p11);
        su.cameraForward = f4(camera.forward, 0.0f);
        su.jitter = {in.jitter.x, in.jitter.y, 0.0f, 0.0f};
        su.params[0] = width;
        su.params[1] = height;
        su.plane = {system_->bodies()[size_t(mirrorBody_)].position.y, 0.0f, 0.0f, 0.0f};
        SDL_PushGPUComputeUniformData(cmd, 0, &su, sizeof(su));
        FX_GPU_PASS(cmd, "water mirror");
        const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{ssprKeys_, false}, {ssprSources_, false}};
        const SDL_GPUTextureSamplerBinding depth = {in.sceneDepth, in.pointSampler};
        for (SDL_GPUComputePipeline* p : {ssprClear_, ssprProject_, ssprResolve_}) {
            // (The projection writes only the keys: its pipeline takes one buffer.)
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, p == ssprProject_ ? 1 : 2);
            SDL_BindGPUComputePipeline(pass, p);
            SDL_BindGPUComputeSamplers(pass, 0, &depth, 1);  // (the clear's pipeline counts it too)
            SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
            SDL_EndGPUComputePass(pass);
        }
    }
    CompositeUniforms u = {};
    u.prevViewProj = in.prevViewProj;
    u.cameraPos = f4(camera.position, camera.zNear);
    u.cameraRight = f4(camera.right, 1.0f / camera.p00);
    u.cameraUp = f4(camera.up, 1.0f / camera.p11);
    u.cameraForward = f4(camera.forward, 0.0f);
    u.jitter = {in.jitter.x, in.jitter.y, 0.0f, 0.0f};
    u.params[0] = width;
    u.params[1] = height;
    u.params[2] = uint32_t(rect_[0]);
    u.params[3] = uint32_t(rect_[1]);
    u.sunDirection = f4(in.sunDirection, 0.0f);
    u.sunColor = f4(in.sunColor, 0.0f);
    u.vsm = *in.vsm;
    u.probes = in.probeVolume;
    u.giParams = {in.bounce, in.sky, in.giOn ? 1.0f : 0.0f, in.reflectionsFollow ? 1.0f : 0.0f};
    u.trace[0] = in.pageTable;
    u.trace[1] = in.bvh ? 1u : 0u;
    u.trace[2] = in.alphaMask ? 1u : 0u;
    u.time = {time_, float(in.refractMips - 1), 1.0f, 0.0f};
    u.underwater = {float(cameraBody_ + 1), 0.0f, 0.0f, 0.0f};
    u.mirror = {mirror ? float(mirrorBody_ + 1) : 0.0f, 0.0f, 0.0f, 0.0f};
    const bool terrain = in.terrainHeights && in.terrainMaterials && in.terrainData;
    if (terrain) {
        u.terrainGrid = in.terrainGrid;
        u.terrainHeights = in.terrainRange;
    }
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    FX_GPU_PASS(cmd, "water composite");
    const SDL_GPUStorageTextureReadWriteBinding rw[4] = {
        {in.color, 0, 0, false}, {in.velocity, 0, 0, false}, {in.reflectWeight, 0, 0, false}, {in.reflectNormal, 0, 0, false}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, rw, 4, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, composite_);
    SDL_GPUTextureSamplerBinding samplers[16] = {
        {in.sceneDepth, in.pointSampler}, {depth_, in.pointSampler},     {normals_, in.pointSampler},
        {extra_, in.pointSampler},        {in.refractSource, in.mipSampler}, {in.vsmAtlas, in.vsmSampler}};
    for (int k = 0; k < 3; ++k)
        samplers[6 + k] = in.probes[k];
    samplers[9] = {detail_, repeat_};
    samplers[10] = {caustics_, repeat_};
    samplers[11] = {terrain ? in.terrainHeights : caustics_, in.mipSampler};  // (stand-ins: not read)
    samplers[12] = {terrain ? in.terrainMaterials : caustics_, in.pointSampler};
    samplers[13] = in.vtAlbedo;
    SDL_BindGPUComputeSamplers(pass, 0, samplers, 14);
    // (Without the BVH the scene buffer stands in: not read.)
    SDL_GPUBuffer* buffers[7] = {bodies_, in.vsmState, mirror ? ssprSources_ : outlines_, terrain ? in.terrainData : patches_,
                                 in.bvh ? in.bvh : in.scene, in.scene, in.materials};
    SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, 7);
    SDL_DispatchGPUCompute(pass, uint32_t(rect_[2] - rect_[0] + 7) / 8, uint32_t(rect_[3] - rect_[1] + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
}

}  // namespace fx
