#pragma once
// The level tools' editable mesh (ProBuilder-style): shared vertices, faces as loops of them
// (n-gons, any convexity, no holes), each face with its material slot, UV projection and
// smoothing group. build() turns it into triangles for a live mesh (render/live_geometry.h):
// faces triangulated in their plane (Manifold's triangulator), normals flat or smoothed within a
// group, UVs projected per face, tangents left to MikkTSpace.
//
// Saved as a small binary blob (save/load; base64 in the scene's edits and prefabs, which are JSON),
// shared immutably between undo steps, and across the C# API.

#include "core/math.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fx {
struct LiveMeshData;
}

namespace fx::level {

// How a face's UVs come about.
//   Local:  projected onto the face's plane in the mesh's own space (the texture moves with it)
//   World:  the same in world space (the texture stays where it is while the mesh moves: neighbouring
//           meshes line up)
//   Manual: the corners' own UVs (PolyMesh::cornerUvs), then offset, scale and rotation
enum class UvMode : uint8_t { Local = 0, World = 1, Manual = 2 };

struct FaceUv {
    UvMode mode = UvMode::Local;
    float offset[2] = {0.0f, 0.0f};
    float scale[2] = {1.0f, 1.0f};  // metres per tile (Local / World); a multiplier (Manual)
    float rotation = 0.0f;          // degrees
};

struct Face {
    uint32_t first = 0;     // its corners: PolyMesh::corners[first, first + count)
    uint32_t count = 0;
    uint32_t material = 0;  // a slot of PolyMesh::materials
    uint32_t smoothing = 0; // 0: flat; faces of one group share normals at shared vertices
    FaceUv uv;
};

class PolyMesh {
public:
    std::vector<float3> positions;
    std::vector<uint32_t> corners;      // a vertex index per face corner, faces' loops back to back
    std::vector<float2> cornerUvs;      // per corner (parallel to corners): Manual faces' UVs
    std::vector<Face> faces;
    std::vector<std::string> materials;  // material slots (.fmat paths; "" the default)
    // Layered surfaces: up to three materials over the faces' own, blended by height where a painted
    // mask (the entity's LevelMask, read through cornerUv1) says; vertex colours tint (rgb) and
    // darken (a: occlusion) the result.
    std::vector<float2> cornerUv1;       // per corner (parallel to corners): the mask's UVs, 0..1 (maskUnwrap)
    std::vector<uint32_t> vertexColors;  // per vertex (parallel to positions): RGBA8; empty: white
    std::string layerMaterials[3];       // "" none
    float layerBlend = 0.15f;            // the height blend's width (0..1 of the layers' heights)

    bool layered() const { return !layerMaterials[0].empty() || !layerMaterials[1].empty() || !layerMaterials[2].empty(); }

    bool empty() const { return faces.empty(); }
    uint32_t triangleCount() const;  // after triangulation (n-gon: n - 2)
    // Corners in range, faces of 3+ distinct corners, cornerUvs parallel, slots in range. `why`: the first problem.
    bool valid(std::string* why = nullptr) const;
    // Adds a face over existing vertices (counter-clockwise seen from its front); its index.
    uint32_t addFace(const std::vector<uint32_t>& loop, uint32_t material = 0, uint32_t smoothing = 0);
    // A face's normal (Newell's method: right for non-planar and concave loops) and centre.
    float3 faceNormal(uint32_t face) const;
    float3 faceCenter(uint32_t face) const;
    void bounds(float3& lo, float3& hi) const;
    uint64_t hash() const;

    std::vector<uint8_t> save() const;
    bool load(const uint8_t* data, size_t size);
    // The same as base64 text (in the scene's edits and prefabs, which are JSON).
    std::string saveText() const;
    bool loadText(std::string_view text);

    // Shapes to start from (centred on the origin).
    static PolyMesh box(float3 size);
    static PolyMesh plane(float width, float depth, uint32_t xSegments = 1, uint32_t zSegments = 1);
};

struct BuildOptions {
    float world[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};  // the entity's (World UVs)
    // A face's material slot to a scene material index (null: slot index as is).
    std::function<uint32_t(uint32_t slot)> material;
};

// Triangles for a live mesh. Faces that can't be triangulated (degenerate) are skipped; `skipped`
// counts them. Each triangle's source face: faceOfTriangle (for picks).
bool build(const PolyMesh& mesh, const BuildOptions& options, LiveMeshData& out, std::vector<uint32_t>* faceOfTriangle = nullptr,
           uint32_t* skipped = nullptr);

// A face's projected UV at a point (Local / World projection; Manual faces: none).
float2 projectUv(const FaceUv& uv, float3 normal, float3 point);
// A face's UVs as drawn, one per corner: projected (World faces through `world`, the entity's
// 3x4 transform; null: as Local) or its corners' own, with its offset, scale and rotation.
std::vector<float2> faceUvs(const PolyMesh& mesh, uint32_t face, const float (*world)[4] = nullptr);
// The same for a FaceUv other than the face's own (what a change would give).
std::vector<float2> faceUvs(const PolyMesh& mesh, uint32_t face, const FaceUv& uv, const float (*world)[4] = nullptr);

}  // namespace fx::level
