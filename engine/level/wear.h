#pragma once
// Wear and tear generated into a layered level mesh's mask (level_meshes.h LevelMask): edge wear
// along its convex sharp edges (chipped paint, worn corners) and grime in its concave ones and near
// its foot (dirt where walls meet floors), each broken up by noise. Written into a layer's channel
// of the mask (the higher of what is painted and what is made), so a layer material (bare metal,
// dirt) shows there; painting over it later still works. Streaks: dirt running down the sides from
// the ledges above them (a wall's top, a window's sill). Grime also gathers where other surfaces
// come near (a floor under a wall: `nearby`). Live wear (level_meshes.h LevelWear) is made again
// whenever the mesh is drawn differently (edited, cut).

#include "level/poly_mesh.h"

#include <cstdint>
#include <vector>

namespace fx {
struct Json;
class JsonWriter;
}  // namespace fx

namespace fx::level {

enum class WearKind : uint8_t { Edges, Grime, Streaks };
inline constexpr const char* kWearKindNames[] = {"edges", "grime", "streaks"};
inline constexpr uint32_t kWearKinds = 3;

struct WearSettings {
    WearKind kind = WearKind::Edges;
    uint32_t channel = 0;   // the mask channel (layer 1..3: 0..2)
    float width = 0.08f;    // how far from an edge it reaches (metres; streaks: how far down they run)
    float strength = 1.0f;  // 0..1
    float sharp = 30.0f;    // edges whose faces turn by more than this (degrees) count
    float foot = 0.3f;      // (grime) how far up from the mesh's lowest point it reaches (metres; 0: none)
    uint32_t seed = 1;
    bool operator==(const WearSettings&) const = default;
};

// `mask`: kMaskSize² RGBA8 (level_mask.h), changed in place (made all zero when empty). The mesh
// needs its mask UVs (cornerUv1); false without them. `nearby`: other surfaces' triangles (three
// points each, in the mesh's space): grime where they come within its width.
bool generateWear(const PolyMesh& mesh, const WearSettings& settings, std::vector<uint8_t>& mask,
                  const std::vector<float3>* nearby = nullptr);

// A rule as JSON ({"kind", "layer", "width", "strength", "foot", "seed"}; `kind` unless it says)
// and back: smart materials, the scene's edits, the editor.
WearSettings wearFromJson(const Json& j, WearKind kind);
void writeWearJson(JsonWriter& w, const char* key, const WearSettings& s);

// Smooth 3D value noise (cells of `cell` metres), 0..1.
float valueNoise(float3 p, float cell, uint32_t seed);

// Where a damage brush dab goes on a face (`p` a point on it, in the mesh's space): `p` itself
// (Hole), the nearest point of the face's convex sharp edges (Chip), or the nearest corner on one
// (Corner). False when the face has no such edge.
enum class DamageSnap : uint8_t { Hole, Chip, Corner };
bool damageSpot(const PolyMesh& mesh, uint32_t face, float3 p, DamageSnap snap, float3& out);

// Where damage goes over a mesh's chosen elements, in the world (`world`: the mesh's rows), each
// with the way out of the surface there and the face it is on: a broken corner at each vertex
// (mode 1), chips `spacing` apart along each edge (2; its two vertices' pair key, lo << 32 | hi),
// holes scattered over each face about `spacing` apart (3), or chips along every convex sharp edge
// (0: the whole mesh). `randomness` (0..1): how far each chip strays along its edge from its even
// place (up to half the spacing either way) and how much its size varies (its `scale`, 1 ± half
// that); `density` (0..1): the share of the places that get one (at random). At most `limit`; the
// same for one seed.
struct DamageSpotAt {
    float3 at, normal;
    DamageSnap kind = DamageSnap::Hole;
    uint32_t face = 0;
    float scale = 1.0f;  // (its size, times the brush's)
};
std::vector<DamageSpotAt> damageSpots(const PolyMesh& mesh, const float world[3][4], uint32_t mode, const std::vector<uint64_t>& items,
                                      float spacing, uint32_t seed, float randomness = 0.0f, float density = 1.0f, uint32_t limit = 256);

// The nearest point of a triangle to `p`.
float3 closestOnTriangle(float3 p, float3 a, float3 b, float3 c);

}  // namespace fx::level
