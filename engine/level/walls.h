#pragma once
// Parametric architecture (LV11): walls drawn as a path on the floor plan, rooms (a closed path
// with a floor and a ceiling), and the openings in them. A Wall is turned into a level mesh
// (buildWall: closed, so cut volumes cut it) whenever it changes; openings are cut volumes placed
// along its path (wallOpeningPlace), so they slide with it. "Convert to mesh" keeps the mesh and
// drops the parameters.

#include "level/poly_mesh.h"

#include <string>
#include <vector>

namespace fx::level {

struct Wall {
    std::vector<float2> points;  // the path on its entity's floor (x, z), metres
    bool closed = false;         // the last point joins the first (a room's walls)
    float height = 3.0f, thickness = 0.2f;
    uint8_t align = 0;           // where the path runs: 0 the wall's middle, 1 its left face, 2 its right face
    bool floor = false, ceiling = false;  // (closed paths) a floor and a ceiling slab inside, `slab` thick
    float slab = 0.2f;
    // Materials (.fmat paths, "" the default): the side to the path's left, its right, the top and
    // ends, the floor's top, the ceiling's underside.
    std::string sideA, sideB, top, floorMaterial, ceilingMaterial;
    bool operator==(const Wall& o) const;  // (walls.cpp: points compared by value)
};

// The wall as a closed mesh in its entity's space: per segment its two sides and top, mitred where
// segments meet, its ends capped (open paths), a floor and a ceiling for rooms. Each side face in
// a smoothing group of its own (flat); UVs projected in world space (bricks line up along it).
PolyMesh buildWall(const Wall& wall);

// A point at `distance` metres along the path (wrapping round a closed one), and the direction it
// runs there (x, z). False: the path has no length.
bool wallPathAt(const Wall& wall, float distance, float2& point, float2& direction);
float wallLength(const Wall& wall);
// The distance along the path of the point on it nearest `p` (x, z).
float wallNearest(const Wall& wall, float2 p);

}  // namespace fx::level
