#pragma once
// Live cuts: a level mesh with cut volumes applied (Manifold booleans), non-destructively: the
// mesh stays as it is, what is drawn (and collided with) is the cut copy, made again whenever the
// mesh or a cut overlapping it changes. Faces keep their material, UV settings, smoothing, mask
// UVs and colours; the faces a cut makes (its inside, where it went through) get its cap material.
// A target that isn't closed (Manifold needs a solid) is clipped instead: its triangles split
// against the cutter, what is inside (or outside, intersecting) dropped, with no caps (there is no
// inside to close). The cutter is split into convex pieces for that: itself when it is convex,
// else a tetrahedron per face from its centre (it must be star-shaped from there: boxes,
// cylinders, spheres, damage; a mesh cutter that isn't can't clip).

#include "level/poly_mesh.h"

#include <memory>
#include <string>
#include <vector>

namespace fx::level {

// Damage: a lumpy ball (a sphere pushed in and out by noise; its seed and roughness), for broken
// holes, chipped edges and snapped corners.
enum class CutShape : uint8_t { Box, Cylinder, Sphere, Mesh, Damage };
inline constexpr const char* kCutShapeNames[] = {"box", "cylinder", "sphere", "mesh", "damage"};
inline constexpr uint32_t kCutShapes = 5;

struct Cut {
    CutShape shape = CutShape::Box;
    bool intersect = false;  // keep only what is inside it (else take it away)
    float world[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};  // the cut's own transform
    // Its size in its own space: a box's sides; a cylinder's diameter (x, z) and height (y); a
    // sphere's diameter (x); damage's diameters. A mesh cut is its mesh (in its own space, closed)
    // as it is.
    float size[3] = {1, 1, 1};
    uint32_t seed = 0;        // (damage) its lumps
    float roughness = 0.35f;  // (damage) how far they push in and out (of its radius)
    std::shared_ptr<const PolyMesh> mesh;
    std::string capMaterial;  // the faces it makes ("": the target's first slot)
};

struct CutResult {
    PolyMesh mesh;
    std::vector<uint32_t> sourceFace;  // per face of the result: the target's face it came from (~0u: a cut's cap)
    uint32_t applied = 0;  // cuts that changed it
    bool clipped = false;  // the target isn't closed: clipped, without caps
    std::string error;     // why it was left as it is ("" when cut)
};

// The target (its entity's transform: `world`) with the cuts (in world space) applied in order.
bool cutMesh(const PolyMesh& target, const float world[3][4], const std::vector<Cut>& cuts, CutResult& out);
// A cut's world bounds (for which targets it may touch).
void cutBounds(const Cut& cut, float3& lo, float3& hi);

}  // namespace fx::level
