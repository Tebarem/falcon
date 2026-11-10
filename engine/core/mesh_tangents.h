#pragma once
// MikkTSpace tangents: the convention glTF, Blender and Substance bake normal maps against, so a
// baked map shades without seams. Shared by the cook (glTF primitives without tangents) and the
// level-mesh kernel.

#include <cstddef>
#include <cstdint>

namespace fx {

// Strided float attribute: element i is at (const char*)data + i * stride.
struct MeshAttribute {
    const float* data = nullptr;
    size_t stride = 0;  // bytes
};

// One tangent per triangle corner: corner c (of indexCount, 3 per triangle) is vertex indices[c],
// and its tangent goes to outTangents[c * 4 .. c * 4 + 3] (xyz, then the bitangent sign +-1).
// Corners of one vertex can differ (mirrored UVs), so callers split vertices where they do.
// Degenerate triangles get a tangent from the normal. Returns false if MikkTSpace failed.
bool generateTangents(const uint32_t* indices, size_t indexCount, MeshAttribute positions, MeshAttribute normals,
                      MeshAttribute uvs, float* outTangents);

}  // namespace fx
