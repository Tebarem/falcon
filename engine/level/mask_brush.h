#pragma once
// The level paint brush's dab on a layered mesh's mask (level_mask.h): a ball round the point under
// the mouse, in the world. Every texel whose point on the mesh lies inside it is painted, on whatever
// faces it reaches (round edges and corners, not only the face under the mouse), except faces turned
// away from the one under the mouse (a thin wall's far side).

#include "level/poly_mesh.h"

#include <cstdint>
#include <vector>

namespace fx::level {

struct MaskDab {
    float3 center;          // the point under the mouse (world)
    float3 normal;          // the surface's there (world)
    float radius = 0.5f;    // metres
    float strength = 0.5f;  // 0..1 at the middle (full within half the radius, to nothing at its edge)
    uint32_t channel = 0;   // layer 1..3: 0..2; 3 erases all three
};

// One stroke's dabs: each texel takes the most any one dab gave it, over the mask as it was when the
// stroke began, so dabs along a stroke don't pile up into a ridged edge and the strength is how far
// one stroke goes. Starts empty (the first dab takes the mask as it is).
struct MaskStroke {
    std::vector<uint8_t> start;  // the mask before the stroke
    std::vector<float> cover;    // kMaskSize²: the most each texel got so far
};

// `world`: the mesh's rows (mesh to world). `mask`: kMaskSize² RGBA8, changed in place (made all
// zero when empty). `stroke`: the stroke it is part of (none: a dab on its own). False without mask
// UVs, or when the ball reached no texel.
bool paintMaskDab(const PolyMesh& mesh, const float world[3][4], const MaskDab& dab, std::vector<uint8_t>& mask,
                  MaskStroke* stroke = nullptr);

// The brush's falloff at distance `d`: 1 within half the radius, smoothly to 0 at its edge.
float brushFalloff(float d, float radius);

}  // namespace fx::level
