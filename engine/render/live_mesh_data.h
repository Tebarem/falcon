#pragma once
// A live mesh's triangles (render/live_geometry.h), on their own so the level kernel and tools can
// make them without the renderer.

#include <cstdint>
#include <vector>

namespace fx {

// Triangles in the mesh's own space. Normals are required; tangents are optional (MikkTSpace from
// the UVs when empty). One material per triangle (a scene material index; empty: all material 0).
struct LiveMeshData {
    std::vector<float> positions;     // xyz per vertex
    std::vector<float> normals;       // xyz per vertex
    std::vector<float> uvs;           // uv per vertex (empty: 0, 0)
    std::vector<float> tangents;      // xyzw per vertex (w: bitangent sign); empty: generated
    std::vector<uint32_t> indices;    // 3 per triangle
    std::vector<uint32_t> materials;  // per triangle (empty: every triangle material 0)
    // The extra stream (layered level meshes, render/surface_layers.h): the mask's UVs (0..1) and
    // a colour (RGBA8: tint, occlusion) per vertex; empty: (0, 0) and white.
    std::vector<float> uv1;           // uv per vertex
    std::vector<uint32_t> colors;     // per vertex
};

}  // namespace fx
