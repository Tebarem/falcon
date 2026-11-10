#include "render/mesh_renderer.h"

#include "core/log.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/light_system.h"
#include "render/post_process.h"
#include "render/virtual_shadow_map.h"

#include "runtime_meshes.shader.h"

#include <algorithm>
#include <cstring>

namespace fx {

namespace {

struct MeshUniforms {
    float4x4 viewProj;
    float world[16];
    float4 cameraPos, cameraForward, sunDirection, sunColor, color, emissive, surface;
    uint32_t screen[4];
    uint32_t gridOrderOffset, gridTileOffset, gridTilesX, gridWords;
    float gridBinScale;
    uint32_t lightCount, gridPad[2];
    VsmParamsGpu vsm;
};

float4 f4(float3 v, float w) { return {v.x, v.y, v.z, w}; }

}  // namespace

bool MeshRenderer::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    SDL_GPUShader* vs = gpu.createShader(shaders::runtime_meshes_vs_mesh);
    SDL_GPUShader* fs = gpu.createShader(shaders::runtime_meshes_fs_mesh);
    if (!vs || !fs) {
        FX_ERROR("meshes: shaders failed: %s", SDL_GetError());
        if (vs)
            SDL_ReleaseGPUShader(device_, vs);
        if (fs)
            SDL_ReleaseGPUShader(device_, fs);
        return false;
    }
    // Premultiplied: an opaque draw (alpha 1) replaces what is there, a transparent one blends.
    SDL_GPUColorTargetDescription color = {};
    color.format = PostProcess::kHdrFormat;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUVertexBufferDescription vb = {0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute attributes[4] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                                                  {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                                                  {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
                                                  {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32}};
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.vertex_input_state.vertex_buffer_descriptions = &vb;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 4;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vs);
    SDL_ReleaseGPUShader(device_, fs);
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    depthSampler_ = SDL_CreateGPUSampler(device_, &si);
    if (!pipeline_ || !depthSampler_) {
        FX_ERROR("meshes: pipeline failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void MeshRenderer::shutdown()
{
    if (!device_)
        return;
    for (auto& [id, m] : meshes_) {
        gpumem::releaseBuffer(device_, m.vertices);
        gpumem::releaseBuffer(device_, m.indices);
    }
    meshes_.clear();
    draws_.clear();
    if (pipeline_)
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (depthSampler_)
        SDL_ReleaseGPUSampler(device_, depthSampler_);
    pipeline_ = nullptr;
    depthSampler_ = nullptr;
    device_ = nullptr;
}

bool MeshRenderer::write(Mesh& m, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    if (!vertices || !indices || !vertexCount || indexCount < 3 || indexCount % 3)
        return false;
    for (uint32_t i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount)
            return false;
    m.pendingVertices.assign(vertices, vertices + vertexCount);
    m.pendingIndices.assign(indices, indices + indexCount);
    m.pending = true;
    return true;
}

uint32_t MeshRenderer::create(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    Mesh m;
    if (!device_ || !write(m, vertices, vertexCount, indices, indexCount))
        return 0;
    const uint32_t id = next_++;
    meshes_[id] = std::move(m);
    return id;
}

bool MeshRenderer::update(uint32_t mesh, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    const auto it = meshes_.find(mesh);
    return it != meshes_.end() && write(it->second, vertices, vertexCount, indices, indexCount);
}

void MeshRenderer::destroy(uint32_t mesh)
{
    const auto it = meshes_.find(mesh);
    if (it == meshes_.end())
        return;
    gpumem::releaseBuffer(device_, it->second.vertices);  // (released once the frames using it are done)
    gpumem::releaseBuffer(device_, it->second.indices);
    meshes_.erase(it);
}

void MeshRenderer::upload(SDL_GPUCommandBuffer* cmd)
{
    for (auto& [id, m] : meshes_) {
        if (!m.pending)
            continue;
        m.pending = false;
        const uint32_t vertexCount = uint32_t(m.pendingVertices.size()), indexCount = uint32_t(m.pendingIndices.size());
        if (vertexCount > m.vertexCapacity || indexCount > m.indexCapacity) {  // (grown: new buffers)
            gpumem::releaseBuffer(device_, m.vertices);
            gpumem::releaseBuffer(device_, m.indices);
            SDL_GPUBufferCreateInfo vi = {SDL_GPU_BUFFERUSAGE_VERTEX, vertexCount * uint32_t(sizeof(Vertex)), 0};
            SDL_GPUBufferCreateInfo ii = {SDL_GPU_BUFFERUSAGE_INDEX, indexCount * 4, 0};
            m.vertices = gpumem::createBuffer("Meshes", device_, &vi);
            m.indices = gpumem::createBuffer("Meshes", device_, &ii);
            m.vertexCapacity = vertexCount;
            m.indexCapacity = indexCount;
        }
        m.indexCount = indexCount;
        const uint32_t vbytes = vertexCount * uint32_t(sizeof(Vertex)), ibytes = indexCount * 4;
        SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vbytes + ibytes, 0};
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (!tb || !m.vertices || !m.indices)
            continue;
        auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
        std::memcpy(mapped, m.pendingVertices.data(), vbytes);
        std::memcpy(mapped + vbytes, m.pendingIndices.data(), ibytes);
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation vs = {tb, 0}, is = {tb, vbytes};
        const SDL_GPUBufferRegion vdst = {m.vertices, 0, vbytes}, idst = {m.indices, 0, ibytes};
        SDL_UploadToGPUBuffer(copy, &vs, &vdst, false);
        SDL_UploadToGPUBuffer(copy, &is, &idst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        m.pendingVertices.clear();
        m.pendingIndices.clear();
    }
}

void MeshRenderer::draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, const Inputs& in)
{
    std::vector<Draw> draws = std::move(draws_);
    draws_.clear();
    if (draws.empty() || !pipeline_ || !in.target || !in.depth || !in.lights || !in.vsm)
        return;
    // Opaque first (as asked), then the transparent ones farthest first.
    const auto distance = [&](const Draw& d) {
        const float3 p = {d.world[12], d.world[13], d.world[14]};
        return length(p - camera.position);
    };
    std::stable_sort(draws.begin(), draws.end(), [&](const Draw& a, const Draw& b) {
        const bool ta = a.color[3] < 1.0f, tb = b.color[3] < 1.0f;
        if (ta != tb)
            return !ta;
        return ta && distance(a) > distance(b);
    });
    FX_GPU_SECTION(cmd, "Meshes");
    FX_GPU_PASS(cmd, "runtime meshes");
    SDL_GPUColorTargetInfo target = {};
    target.texture = in.target;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    const SDL_GPUTextureSamplerBinding samplers[2] = {{in.vsm->atlas(), in.vsm->compareSampler()}, {in.depth, depthSampler_}};
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
    SDL_GPUBuffer* buffers[3] = {in.lights->lights(), in.lights->grid(), in.vsm->state()};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 3);
    MeshUniforms u = {};
    u.viewProj = in.rasterViewProj;
    u.cameraPos = f4(camera.position, camera.zNear);
    u.cameraForward = f4(camera.forward, 0.0f);
    u.sunDirection = f4(in.lights->sunDirection(), 0.0f);
    u.sunColor = f4(in.lights->sunColor(), 0.0f);
    u.screen[0] = width;
    u.screen[1] = height;
    u.gridOrderOffset = in.lights->orderOffset();
    u.gridTileOffset = in.lights->tileOffset();
    u.gridTilesX = in.lights->tilesX();
    u.gridWords = in.lights->words();
    u.gridBinScale = in.lights->binScale();
    u.lightCount = in.lights->count();
    u.vsm = in.vsm->params();
    for (const Draw& d : draws) {
        const auto it = meshes_.find(d.mesh);
        if (it == meshes_.end() || !it->second.vertices || !it->second.indexCount)
            continue;
        std::memcpy(u.world, d.world, sizeof(u.world));
        u.color = {d.color[0], d.color[1], d.color[2], d.color[3]};
        u.emissive = {d.emissive[0], d.emissive[1], d.emissive[2], d.emissive[3]};
        u.surface = {d.surface[0], d.surface[1], 1.0f, 0.0f};
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        const SDL_GPUBufferBinding vb = {it->second.vertices, 0}, ib = {it->second.indices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
        SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, it->second.indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

}  // namespace fx
