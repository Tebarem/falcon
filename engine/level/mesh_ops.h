#pragma once
// The level tools' mesh operations (ProBuilder's set) on a PolyMesh, by name with JSON arguments,
// so editor code (C#) reaches every one through one call of the level API (FxLevelApi::meshOp).
//
// Arguments name what they act on: "faces": [face indices], "edges": [edge keys, lo << 32 | hi],
// "vertices": [vertex indices], plus each operation's numbers. The result is the new mesh and what
// to select next (in its mode: 1 vertices, 2 edges, 3 faces); detach also gives the faces it took
// as a mesh of their own.
//
//   extrude      faces, distance        the faces moved out along their normals, walls around them
//   extrudeEdges edges, distance        a quad off each (open) edge, outward in its face's plane
//   inset        faces, amount          each face shrunk inward, a ring of faces around it
//   bevel        edges, amount, segments?, curve?   each edge cut into a face (a chamfer), or into
//                                       `segments` faces round it (curve 1: round, 0: flat, -1: curved in)
//   connect      edges | vertices       faces split between the middles of two of their edges, or two of their vertices
//   loop / ring  edges                  (selection) the edge loop / ring through each edge (quads)
//   loopCut      edges                  a new edge loop across the ring through each edge
//   subdivide    faces                  each face into quads (edge middles and its centre)
//   merge        vertices               the vertices into one at their centre
//   weld         vertices?, distance    vertices closer than distance into one (all without vertices)
//   fill         edges | vertices       two edges bridged, an open edge's hole filled, or a face over the vertices
//   delete       faces | edges | vertices
//   detach       faces                  the faces taken out as a mesh of their own
//   flip         faces?                 winding (and normals) reversed (all without faces: inside out)
//   conform      -                      every face wound as its neighbours are
//   triangulate  faces?                 into triangles;  quadrangulate: triangle pairs into quads
//   mergeFaces   faces                  adjacent faces into one
//   smooth       faces, group           their smoothing group (0: flat); autoSmooth: angle (degrees)
//   material     faces?, slot | path, size?  their material slot (a path: its slot, added when new; no faces: all);
//                                     size [w, h]: world / local UV faces' metres per tile (the .fmat's "size")
//   uv           faces, mode, offset, scale, rotation   their UV settings (any of them; to "manual":
//                                       the UVs they show become their corners' own)
//   uvNudge      faces, offset, scale, rotation         moved, scaled (times) and turned (degrees) by
//   uvReset      faces                  back to a Local projection, 1 m tiles
//   uvFit        faces                  the texture once across each face (its UVs' bounds 0..1)
//   uvAlign      faces, edges?          turned so u runs along the given edge (else the longest)
//   uvMatch      faces                  the first face's texture carried on across shared edges
//   uvUnwrap     faces?                 unfolded into charts (xatlas): Manual UVs, a metre a tile
//   trim         faces, v0, v1, aspect?, edges?, material?   onto a trim strip (level/trim_sheet.h): the face's
//                                       height across its longest edge fills v0..v1, u runs along
//                                       the edge at square texels, a run of faces carries u on
//   layers       materials [a, b, c], blend?   the layers over the faces (poly_mesh.h); the mask UVs
//                                       made when the mesh has none
//   maskUnwrap   -                      the mask's UVs (cornerUv1) again: charts packed into 0..1
//   vertexColor  vertices | faces?, color [r, g, b, a]   their vertices' colour (none: all)
//   (UV operations take "world": the entity's 3x4 rows, for World faces; without faces: all)
//   grow / shrink / byMaterial / byNormal (angle)       (selection) from the given elements
//   wall         points [[x, z]...], closed, height, thickness, align, floor, ceiling   a wall's mesh (level/walls.h)
//   shape        name, size...          a new mesh: box, plane, cylinder, cone, sphere, torus, pipe,
//                                       prism, arch, stairs, curvedStairs, polygon (points, height)

#include "level/poly_mesh.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fx {
struct Json;
}

namespace fx::level {

struct OpResult {
    PolyMesh mesh;
    uint32_t selectionMode = 0;  // 0: keep the caller's; 1 vertices, 2 edges, 3 faces
    std::vector<uint64_t> selection;
    std::optional<PolyMesh> detached;
    std::string error;  // why it did nothing ("" when it ran)
};

inline uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a; }

bool runOp(const std::string& op, const PolyMesh& in, const Json& args, OpResult& out);
// A bevel's cross-section from p to q round the corner it cuts off (where the edge was), at t (0..1):
// on the arc touching both faces (curve 1; a circle's when p and q are as far from the corner), on
// the flat cut (0), or curved in as far (-1, a cove).
float3 bevelProfile(float3 p, float3 corner, float3 q, float t, float curve);
// The point of the line through a and b nearest p (a bevel's corner: the edge's old place).
float3 nearestOnLine(float3 p, float3 a, float3 b);
// Faces without mask UVs of their own (every corner on one point: what an operation or a modifier
// made) given charts in the mask's free space, at about the texel density of the others, which
// keep theirs (so paint stays where it is). `before`: a mesh whose charts count as taken too (the
// one an operation started from). Laid out afresh (maskUnwrap: its charts in the lower 80% each way,
// room for later ones) when they don't fit at a sixteenth of that density. False: nothing to do (no mask UVs, or none missing).
bool maskFill(PolyMesh& m, const PolyMesh* before = nullptr);

// A shape by name ("box", ...) from its arguments (sizes in metres, segment counts).
PolyMesh makeShape(const std::string& name, const Json& args, std::string* error = nullptr);

}  // namespace fx::level
