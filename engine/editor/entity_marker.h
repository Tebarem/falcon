#pragma once

#include "core/math.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace fx {

// What an editor entity is: a cooked mesh instance, a created built-in primitive, an empty, a
// point / spot light (cooked or created), a camera, a body of water, a bone of an expanded
// skeleton (or its Skeleton group: driven by the animation, never saved), or a level mesh (an
// editable mesh of the level tools: level/level_meshes.h).
enum class EntityKind : uint8_t { Mesh, Primitive, Empty, Light, Camera, Water, Bone, Level, Count };

// Kind names, as the page and the edits files write them ("primitive": a created mesh).
inline const char* entityKindName(EntityKind k)
{
    static const char* const kNames[] = {"mesh", "primitive", "empty", "light", "camera", "water", "bone", "level"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(EntityKind::Count));
    return k < EntityKind::Count ? kNames[size_t(k)] : "primitive";
}

// A created entity's kind from its name: "mesh" (prefabs), "primitive" and unknown names are
// created meshes.
inline EntityKind entityKindFromName(std::string_view name)
{
    for (uint8_t k = uint8_t(EntityKind::Empty); k < uint8_t(EntityKind::Bone); ++k)
        if (name == entityKindName(EntityKind(k)))
            return EntityKind(k);
    if (name == entityKindName(EntityKind::Level))
        return EntityKind::Level;
    return EntityKind::Primitive;
}

// An editor entity without a mesh, drawn and picked as an icon in the scene view.
struct EntityMarker {
    uint32_t index;  // EditorBridge entity index
    EntityKind kind;
    float world[3][4];
    uint32_t lightType = 0;      // lights: 0 point, 1 spot
    float range = 0.0f;          // lights: effective range
    float outerAngle = 0.0f;     // spot lights: half angle, degrees
    float color[3] = {1, 1, 1};  // lights
    float fov = 60.0f;           // cameras: vertical field of view, degrees
    bool mainCamera = false;
    uint32_t waterShape = 0;     // water: WaterShape
    float waterSize[3] = {};     // water: width, length, depth
    const float* waterPoints = nullptr;  // water: polygon outline (local x, z pairs; the water system's)
    uint32_t waterPointCount = 0;
    bool audio = false;          // an Audio Source: a speaker (on an entity with no icon of its own)
    bool audioSpatial = false;   // 3D: selected, its min and max distances show
    float audioMin = 0.0f, audioMax = 0.0f;
    float audioOcclusion = -1.0f;  // playing: how clear its path to the listener is (1 clear .. 0 blocked; < 0: not simulated)
    // Particles and decals (vfx/vfx_components.h): an effect's sparkle, a decal's icon; selected,
    // an emitter's shape (1 sphere, 2 box, 3 cone, 4 ring: its sizes in `shape`) or a decal's box.
    bool effect = false, decal = false;
    uint32_t emitterShape = 0;
    float shape[4] = {};     // sphere: radius; box: x, y, z; cone: angle (degrees), radius, length; ring: radius, arc
    float decalSize[3] = {};
    bool opening = false;  // a wall's opening: its cut is at openingWorld (on the wall's path), not at world
    float openingWorld[3][4] = {};
    bool snapPoint = false;  // a kit piece's snap point (level/level_meshes.h): its out (+z) and up drawn
    // A gameplay volume (level/level_meshes.h LevelVolume): its kind + 1 (0: none), its mesh's edges
    // as line pairs (world).
    uint32_t volumeKind = 0;
    std::vector<float3> volumeEdges;
    uint32_t cutShape = 0;  // a cut volume (level/level_meshes.h): 1 box, 2 cylinder, 3 sphere, 4 mesh, 5 damage; its unit shape, drawn by its transform
    // What it hangs under (~0u: nothing): a damage cut shows only with it, or itself, selected.
    uint32_t parent = ~0u;
    // A spline (level/level_meshes.h LevelSpline): its curve and its points, in world space.
    std::vector<float3> splineCurve, splinePoints;
};

}  // namespace fx
