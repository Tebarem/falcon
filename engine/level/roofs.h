#pragma once
// Roofs for the level tools (level_meshes.h LevelRoof): made from an outline (a room's, usually)
// as a closed mesh: flat (a slab), shed (one slope), gable (two slopes up to a ridge, gable ends),
// hip (four slopes: the ridge shortened, triangles at its ends) or pyramid (every edge sloping up
// to a point). The sloped kinds take the outline's bounding rectangle along its longest edge.

#include "level/poly_mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {
struct Json;
class JsonWriter;
}  // namespace fx

namespace fx::level {

enum class RoofStyle : uint8_t { Flat, Shed, Gable, Hip, Pyramid };
inline constexpr const char* kRoofStyleNames[] = {"flat", "shed", "gable", "hip", "pyramid"};
inline constexpr uint32_t kRoofStyles = 5;

struct Roof {
    std::vector<float2> outline;  // (x, z), the entity's space, either winding
    RoofStyle style = RoofStyle::Gable;
    float base = 3.0f;       // the eaves' height
    float pitch = 35.0f;     // degrees (sloped kinds)
    float overhang = 0.3f;   // past the outline, all round (metres)
    float thickness = 0.2f;  // (flat) the slab's
    std::string material, under;  // its top's and its underside's ("": the default)
    bool operator==(const Roof& o) const;
};

PolyMesh buildRoof(const Roof& roof);

void readRoof(const Json& j, Roof& out);
void writeRoof(JsonWriter& w, const char* key, const Roof& roof);

}  // namespace fx::level
