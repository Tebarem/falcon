#pragma once
// Modifiers on a level mesh (level_meshes.h LevelModifiers): a stack applied in order to what its
// LevelMesh holds before it is drawn and cut. The mesh stays as it was edited (the element tools
// edit it, and a mirror's other half follows), until a modifier is applied into it.
//
//   mirror    axis, offset, weld           the mesh and its mirror image across a plane (the seam welded)
//   array     mode linear | radial | grid  copies: moved by `step` (relative: times its size), turned
//             count[3], step, relative,    about `axis` through `angle` (radial; pushed `radius` out
//             axis, angle, radius,         first), or a grid; each moved, turned (about y) and scaled
//             jitter [move, turn, scale],  at random by up to `jitter` (`seed`); copies that meet
//             seed, weld                   welded, faces back to back gone
//   solidify  thickness, offset            a surface given thickness (-1: inward, 1: outward; a closed
//                                          mesh: a hollow shell), its open edges closed by a rim
//   bevel     width, sharp, segments,      its edges whose faces turn by more than `sharp` degrees cut
//             curve                        into faces (a chamfer; `segments` of them round the edge:
//                                          curve 1 round, 0 flat, -1 curved in), its corners filled
//   deform    type bend | twist | taper,   bent through `angle` degrees along `axis` (towards the next
//             axis, angle, factor          axis), twisted through `angle` about it, or tapered to
//                                          `factor` of its size at its far end
//   curve     follow (bend | copies),      along the spline it is under (level/splines.h; its length
//             axis, fit, spacing, align    along `axis`, its y up, standing on the curve): bent along
//                                          it (`fit`: copies end to end, stretched to fill it), or
//                                          copies every `spacing` metres (0: its own length), turned
//                                          with the curve (`align`) or not
//   script    class, settings              one written in C# (a Falcon.Level.MeshModifier class: its
//                                          public fields the settings, as JSON), run through the
//                                          handler the scripts give (none loaded: the mesh as it is)

#include "level/poly_mesh.h"
#include "level/splines.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {
struct Json;
class JsonWriter;
}  // namespace fx

namespace fx::level {

enum class ModifierKind : uint8_t { Mirror, Array, Solidify, Bevel, Deform, Curve, Script };
inline constexpr const char* kModifierNames[] = {"mirror", "array", "solidify", "bevel", "deform", "curve", "script"};
inline constexpr uint32_t kModifierKinds = 7;
enum class ArrayMode : uint8_t { Linear, Radial, Grid };
inline constexpr const char* kArrayModeNames[] = {"linear", "radial", "grid"};
enum class DeformType : uint8_t { Bend, Twist, Taper };
inline constexpr const char* kDeformNames[] = {"bend", "twist", "taper"};

struct Modifier {
    ModifierKind kind = ModifierKind::Mirror;
    bool enabled = true;
    uint32_t axis = 0;        // mirror: across x / y / z; radial array: about; deform: along
    float offset = 0.0f;      // mirror: the plane on its axis; solidify: -1 inward .. 1 outward
    bool weld = true;         // mirror, array
    ArrayMode mode = ArrayMode::Linear;
    uint32_t count[3] = {3, 1, 1};
    float3 step = {1, 0, 0};  // linear: each copy's move; grid: along x, y, z (relative: times its size)
    bool relative = true;
    float angle = 360.0f;     // radial array; deform bend and twist (degrees)
    float radius = 0.0f;      // radial array
    float3 jitter = {0, 0, 0};  // array: move (metres), turn (degrees), scale (a fraction)
    uint32_t seed = 1;
    float amount = 0.1f;      // solidify: thickness; bevel: width; taper: its far end's scale
    float sharp = 30.0f;      // bevel (degrees)
    uint32_t segments = 1;    // bevel: faces across each edge (1: a flat chamfer)
    float curve = 1.0f;       // bevel: 1 round, 0 flat, -1 curved in
    DeformType deform = DeformType::Bend;
    bool copies = false;      // curve: copies along it (else bent along it)
    bool fit = true;          // curve, bent: end to end to fill it
    float spacing = 0.0f;     // curve, copies (metres; 0: its own length)
    bool align = true;        // curve, copies: turned with it
    std::string script;       // script: its class ("Game.Wobble")
    std::string settings = "{}";  // script: its fields as a JSON object
    bool operator==(const Modifier& o) const;  // (float3 has none: by hand)
};

// What a modifier may need from around the mesh: the spline it is under (in the mesh's space).
struct ModifierContext {
    const Spline* spline = nullptr;
};
bool needsSpline(const std::vector<Modifier>& stack);
bool hasScript(const std::vector<Modifier>& stack);

// Modifiers written in C#: run and listed through the handler the scripts give the level API
// (script/level_api.h FxLevelApi::modifierHandler; null: none loaded). op 0: `cls` run with its
// `settings` (JSON) on the mesh blob, the result's blob into `out`; op 1: the classes as JSON
// [{name, label, properties, defaults}] into `out`. Either returns the bytes written, -(bytes
// needed) when `capacity` is short, 0 when it failed. `scriptGeneration` counts handlers given (a
// reload of the code makes them run again).
using ScriptModifierHandler = int32_t (*)(int32_t op, const char* cls, const char* settings, const uint8_t* blob, int32_t size, uint8_t* out,
                                          int32_t capacity);
void setScriptModifierHandler(ScriptModifierHandler handler);
uint32_t scriptModifierGeneration();
bool runScriptModifier(const std::string& cls, const std::string& settings, const PolyMesh& in, PolyMesh& out, std::string* error = nullptr);
std::string describeScriptModifiers();  // "[]": none

// One of a kind with its defaults ("array" with `mode`; "deform" with `deform`).
Modifier makeModifier(ModifierKind kind, ArrayMode mode = ArrayMode::Linear, DeformType deform = DeformType::Bend);
// As JSON ({"kind", "enabled", and its kind's fields}) and back (what is missing: its defaults).
Modifier modifierFromJson(const Json& j);
void writeModifierJson(JsonWriter& w, const char* key, const Modifier& m);
std::string modifierTitle(const Modifier& m);  // "Mirror X", "Radial Array", "Bend"...

// `in` with the stack applied (disabled ones skipped) into `out`. `sourceFace`: each of out's faces'
// face in `in` (~0u: none, made by a modifier). False (`error` says why): `out` is `in`.
bool applyModifiers(const PolyMesh& in, const std::vector<Modifier>& stack, PolyMesh& out, std::vector<uint32_t>* sourceFace = nullptr,
                    std::string* error = nullptr, const ModifierContext* context = nullptr);

}  // namespace fx::level
