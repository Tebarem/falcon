#pragma once

// Prefabs (.fprefab): a group of entities saved as an asset, placed as linked instances.
//
//   {"format": 1, "name": "Crate",
//    "entities": [{"id": "0", "name": "Crate", "kind": "empty"},
//                 {"id": "1", "parent": "0", "name": "Lid", "kind": "mesh", "mesh": "cube",
//                  "material": "Assets/Wood.fmat", "local": [12 floats, rows of a 3x4], "hidden": false},
//                 {"id": "2", "parent": "0", "kind": "light", "light": {...as in the edits}}]}
//
// Entity "0" is the root (its transform is the instance's own). Ids are stable within the prefab:
// an instance's overrides name them. In a scene an instance is one entry of the edits (kind
// "prefab": the prefab's path, the root's transform, and the overrides, by id: local, hidden,
// material, name, light, camera, body, collider, audioSource, audioListener, vfx); loading builds its entities from the prefab,
// then the overrides.
//
// Prefabs nest: an entity of kind "prefab" is an instance of another one ("prefab": its path,
// relative to the project; "overrides" as an instance's in the edits), placed where the entity is
// (its local, name, hidden). readPrefab() flattens them: the nested prefab's entities take ids
// "<entity id>/<their id>" (the nested root keeps the entity's own id), so an outer instance's
// overrides reach any depth by path ("3/1"). A prefab that contains itself (at any depth) stops
// there, with a warning. Level meshes (kind "level") keep their mesh inline ("levelMesh", base64).

#include "editor/scene_edits.h"
#include "scene/scene.h"
#include "script/script_entry.h"
#include "vfx/vfx_components.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fx {

struct PrefabEntity {
    std::string id, parent;  // parent: "" for the root
    std::string name;
    std::string kind = "empty";  // mesh | empty | light | camera | level | prefab (nested: readPrefabFile only)
    std::string mesh;            // kind mesh: the mesh's name
    std::string material;        // kind mesh: a material asset ("": the mesh's own)
    float local[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    bool hidden = false;
    std::optional<Light> light;
    std::optional<CameraComponent> camera;
    std::optional<PhysicsBody> body;  // "body" / "collider" as in the edits
    std::optional<Collider> collider;
    std::optional<AudioSource> audioSource;  // "audioSource" / "audioListener" as in the edits
    std::optional<AudioListener> audioListener;
    std::optional<AcousticSurface> acoustics;
    VfxSet vfx;  // particle and decal components ("particleEffect", ..., "decal": as in the edits)
    std::vector<ScriptEntry> scripts;  // script components ("scripts": as in the edits)
    std::shared_ptr<const level::PolyMesh> levelMesh;  // kind level: its mesh
    LevelParts level;  // the level tools' components ("cut", "wall", "opening", "modifiers", "spline", "snapPoint"...)
    std::string prefab;                     // kind prefab: the nested prefab (project relative)
    std::vector<PrefabOverride> overrides;  // kind prefab: what this nesting changes in it, by its ids
    // readPrefab: the nested prefab it came from ("" for the prefab's own entities) and its id
    // in there; for Apply, which writes changes to nested entities as overrides.
    std::string nestedIn, nestedId;
};

struct Prefab {
    std::string name;
    std::vector<PrefabEntity> entities;  // parents before their children; [0] is the root
    const PrefabEntity* find(const std::string& id) const;
    // Each entity's transform relative to the root (the root's: identity), in entities' order.
    std::vector<std::array<float, 12>> rootRelative() const;
};

// The prefab with its nested prefabs flattened in (what instances are made from). The project
// is found from the path (the folder above it with project.falcon) unless given.
bool readPrefab(const std::string& path, Prefab& out, std::string& error, const std::string& projectDir = "");
// The file as it is (nested prefabs as kind "prefab" entities): what Save and Apply write.
bool readPrefabFile(const std::string& path, Prefab& out, std::string& error);
// A prefab entity with an override's changes on it.
void applyOverride(PrefabEntity& e, const PrefabOverride& o);
bool writePrefab(const std::string& path, const Prefab& prefab, std::string& error);

}  // namespace fx
