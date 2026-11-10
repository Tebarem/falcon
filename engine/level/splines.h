#pragma once
// Splines for the level tools (level_meshes.h LevelSpline): a smooth curve through points (each one
// smooth or a corner), open or closed. Sampled into frames along its length; lofted into a mesh by
// a profile (a road, a kerb, a wall, a rail, a pipe, or one of your own); followed by the meshes
// under it (modifiers.h's curve modifier: bent along it, or copies along it).

#include "level/poly_mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {
struct Json;
class JsonWriter;
}  // namespace fx

namespace fx::level {

struct SplinePoint {
    float3 position = {0, 0, 0};
    bool corner = false;  // a sharp turn here (else the curve runs smoothly through it)
};

struct Spline {
    std::vector<SplinePoint> points;
    bool closed = false;
    bool operator==(const Spline& o) const;
};

// A place along it: where, which way it runs, which way is up and to its right (side = tangent x up).
struct SplineFrame {
    float3 position = {0, 0, 0}, tangent = {1, 0, 0}, up = {0, 1, 0}, side = {0, 0, 1};
    float distance = 0.0f;
};

// Its curve as points about `step` metres apart (every point it passes through included), and its length.
std::vector<float3> splineCurve(const Spline& s, float step = 0.25f);
float splineLength(const Spline& s);
// Frames every `step` metres (and at its end), up kept as near the world's (y) as the curve allows.
std::vector<SplineFrame> splineFrames(const Spline& s, float step = 0.25f);
// The frame `distance` metres along (between the sampled ones; past its ends: on along its end's tangent).
SplineFrame frameAt(const std::vector<SplineFrame>& frames, float distance);

// What it is lofted into: a profile (x across to its right, y up; counter-clockwise seen along it)
// swept along it.
//   road:  a slab `width` wide, its top on the curve, `height` thick
//   kerb:  `width` x `height` standing on the curve
//   wall:  the same (a thin, tall one by default)
//   rail:  `width` x `height` around the curve
//   pipe:  a tube `width` across round it, `sides` sides (smooth)
//   custom: `points`
struct LoftSettings {
    std::string profile;  // "": none (the spline draws nothing itself)
    float width = 4.0f, height = 0.2f;
    uint32_t sides = 12;
    std::vector<float2> points;  // (custom)
    float step = 0.5f;           // how finely it follows the curve (metres)
    float uvLength = 0.0f;       // metres a texture tile runs along it (0: its width)
    std::string material;
    bool operator==(const LoftSettings& o) const;
};
LoftSettings loftPreset(const std::string& profile);  // the profile's own width and height
PolyMesh loftSpline(const Spline& s, const LoftSettings& loft);

// As JSON ({"points": [[x, y, z, corner]...], "closed", "loft": {...}}) and back.
void readSpline(const Json& j, Spline& s, LoftSettings& loft);
void writeSpline(JsonWriter& w, const char* key, const Spline& s, const LoftSettings& loft);

}  // namespace fx::level
