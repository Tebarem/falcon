#pragma once

// (R5) Meshes made at runtime (scripts' Mesh): their own vertex and index buffers, and the draws
// asked for each frame. Drawn after the resolve into the lit frame, before particles (runtime_meshes):
// lit like the scene (the sun and its shadow, the local lights, the sky), tested against the
// scene's depth in the fragment shader. They write no depth and are in no shadow, GI, reflection
// or velocity (TAA may smear one that moves fast). Opaque draws first, then the transparent ones
// (colour alpha under 1) back to front, blended.
//
// The renderer belongs to the process, so a game's meshes outlive scene loads.

#include "core/math.h"
#include "scene/fly_camera.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fx {

class GpuDevice;
class LightSystem;
class VirtualShadowMap;

class MeshRenderer {
public:
    struct Vertex {
        float position[3], normal[3], uv[2], color[4];
    };
    static_assert(sizeof(Vertex) == 48);

    bool init(const GpuDevice& gpu);
    void shutdown();
    // A mesh's vertices and triangles (3 indices each): its id (0: failed); new data for it; gone.
    uint32_t create(const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);
    bool update(uint32_t mesh, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);
    void destroy(uint32_t mesh);
    uint32_t count() const { return uint32_t(meshes_.size()); }

    struct Draw {
        uint32_t mesh = 0;
        float world[16];  // System.Numerics' Matrix4x4, as it is in memory
        float color[4], emissive[4], surface[4];  // surface: roughness, metallic
    };
    void queue(const Draw& d) { draws_.push_back(d); }

    struct Inputs {
        SDL_GPUTexture* target = nullptr;  // the lit HDR frame (render size)
        SDL_GPUTexture* depth = nullptr;   // its depth (reverse-Z)
        float4x4 rasterViewProj;
        const LightSystem* lights = nullptr;
        const VirtualShadowMap* vsm = nullptr;
    };
    // This frame's uploads (before any pass), then its draws (into `target`); the queue empties.
    void upload(SDL_GPUCommandBuffer* cmd);
    void draw(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height, const Inputs& in);
    bool hasDraws() const { return !draws_.empty(); }

private:
    struct Mesh {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        uint32_t vertexCapacity = 0, indexCapacity = 0, indexCount = 0;
        std::vector<Vertex> pendingVertices;  // to upload (upload())
        std::vector<uint32_t> pendingIndices;
        bool pending = false;
    };
    bool write(Mesh& m, const Vertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* depthSampler_ = nullptr;
    std::unordered_map<uint32_t, Mesh> meshes_;
    uint32_t next_ = 1;
    std::vector<Draw> draws_;
};

}  // namespace fx
