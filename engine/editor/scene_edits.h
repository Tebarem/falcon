#pragma once

// Editor changes to a cooked scene, saved next to it as <scene>.edits.json and applied at boot
// (editor and game) before the instance table is uploaded and physics is built, so moved and
// deleted instances also move or lose their static colliders.
//
//   {"version":2, "nextUid":4,
//    "instances":[{"slot":12,"name":"Lamp","parent":"c1","world":[12 floats, rows of 3x4],"hidden":false,"deleted":false}],
//    "created":[{"uid":1,"kind":"empty","name":"Empty 1","world":[...],"hidden":false},
//               {"uid":2,"kind":"primitive","primitive":"cube","name":"Cube 1","parent":"c1","local":[...],"hidden":false},
//               {"uid":3,"kind":"light","name":"Spot Light 1","world":[...],"hidden":false,
//                "light":{"type":"spot","color":[1,0.9,0.8],"intensity":10,"range":0,"inner":20,"outer":30,"shadows":true}}],
//               {"uid":4,"kind":"camera","name":"Camera 1","world":[...],"hidden":false,"camera":{"fov":60,"near":0.05,"main":true}}],
//    "lights":[{"index":2,"world":[...],"hidden":false,"light":{...}}]}
//
// "instances" edit cooked instances by slot; "created" are entities added in the editor, each
// with a uid that stays the same across saves: built-in primitives (fpak::kBuiltinNames, each
// in a pool of instance slots appended to the scene), empties, point / spot lights and cameras. "lights"
// edit the scene's cooked lights by index. "parent" names the entity's parent by key: "i<slot>"
// (a cooked instance), "l<index>" (a cooked light) or "c<uid>"; children store "local" (relative
// to the parent, exactly as LocalTransform holds it) instead of "world".
// Any entry may carry a "light" or "camera" its kind does not have (a mesh, an empty, a cooked
// instance): a component added in the Inspector (Add Component); so may "body" (a PhysicsBody),
// "collider", "audioSource", "audioListener" and "acoustics" (an AcousticSurface).
// Version 1 files (no uids, kinds or parents; every created entity a primitive) still load.

#include "anim/anim_ik_settings.h"
#include "components.gen.h"
#include "level/level_meshes.h"
#include "script/script_entry.h"
#include "vfx/vfx_components.h"
#include "water/water_system.h"

#include <flecs.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class MaterialLibrary;
class JsonWriter;
struct Json;

// Lights as the edits (and prefabs) write them: {"type", "color", "intensity", "range", "inner", "outer", "shadows"}.
void readLightJson(const Json& j, Light& l);
void writeLightJson(JsonWriter& w, const Light& l);  // as "light": {...}
// Physics components: "body": {"mass", "linearDamping", "angularDamping", "gravityScale",
// "kinematic"} and "collider": {"shape": box|sphere|mesh|capsule, "center", "size", "friction", "bounciness",
// "layer" (0..31), "ignore" [layers], "trigger"} (the last three written when set).
void readBodyJson(const Json& j, PhysicsBody& b);
void writeBodyJson(JsonWriter& w, const PhysicsBody& b);  // as "body": {...}
void readColliderJson(const Json& j, Collider& c);
void writeColliderJson(JsonWriter& w, const Collider& c);  // as "collider": {...}
const char* colliderShapeName(uint32_t shape);
uint32_t colliderShapeOf(const std::string& name);
// Audio components: "audioSource": {"sound" (a project path), "playOnStart", "loop", "spatial",
// "ownSettings", "airAbsorption", "rolloff": inverse|linear, "volume", "pitch", "minDistance",
// "maxDistance", "bus": sfx|physics|ambience|music|ui} and "audioListener": {"gain"}.
void readAudioSourceJson(const Json& j, AudioSource& a);
void writeAudioSourceJson(JsonWriter& w, const AudioSource& a);  // as "audioSource": {...}
// An Animator: one graph instance drives every skinned mesh of its entity and its children.
// "animator": {"graph": "<.fgraph, relative to the project>", "params": {"Speed": 2, "Alert": true},
// "entities": {"AimHold": "<entity key>"}, "ik": {...}} (parameter values over the graph's defaults,
// bools as 0/1; entities: the graph's entity parameters bound, which its IK follows; ik: the
// Animator's own IK, an "ik" section as a graph's (anim/anim_ik_settings.h), used while its graph has
// none). Older scenes' "footIk", "footReach" and "targets" {look, aim, hold, aimBone, aimAxis,
// leftHand, rightHand} are read into its own IK and bindings named LookTarget, AimTarget, AimHold,
// LeftHandTarget and RightHandTarget (migrateAnimatorIk). "editorPreview": it plays in the editor's
// edit mode too (the editor's; a game ignores it).
struct AnimatorEdit {
    std::string graph;
    std::vector<std::pair<std::string, float>> params;
    std::vector<std::pair<std::string, std::string>> entities;  // entity parameter -> entity key
    std::string ik;                                             // its own IK's JSON ("": none)
    bool editorPreview = false;                                 // plays in the editor's edit mode
    bool operator==(const AnimatorEdit&) const = default;
};
// Older Animator fields (footIk, footReach, targets) as its own IK and bindings; false when it
// has none of them.
bool migrateAnimatorIk(const Json& j, AnimatorEdit& a);
// An Animator's own IK parsed (null: none, or it has errors: logged).
std::shared_ptr<const anim::IkSettings> ownIkOf(const AnimatorEdit& a);
void readAnimatorJson(const Json& j, AnimatorEdit& a);
void writeAnimatorJson(JsonWriter& w, const AnimatorEdit& a);  // as "animator": {...}
void readAudioListenerJson(const Json& j, AudioListener& l);
void writeAudioListenerJson(JsonWriter& w, const AudioListener& l);  // as "audioListener": {...}
AudioSource defaultAudioSource();  // Add Component's: plays on start, 3D, air absorption, 1 - 50 m
// "acoustics": {"excluded", "own" (its values, not the material's), "moving" (occludes while it
// moves), "preset", "absorption" [3], "scattering", "transmission" [3], "surface" (a surface
// sound preset of its own, else its material's), "impactGain", "impactPitch"}.
void readAcousticSurfaceJson(const Json& j, AcousticSurface& a);
void writeAcousticSurfaceJson(JsonWriter& w, const AcousticSurface& a);  // as "acoustics": {...}

// What a prefab instance changes on one of its prefab's entities (by the entity's id).
struct PrefabOverride {
    std::string id;
    bool hasLocal = false;
    float local[3][4] = {};
    std::optional<bool> hidden;
    std::optional<std::string> material, name;
    std::optional<Light> light;
    std::optional<CameraComponent> camera;
    std::optional<PhysicsBody> body;  // (added on the instance: a prefab's own stay)
    std::optional<Collider> collider;
    std::optional<AudioSource> audioSource;
    std::optional<AudioListener> audioListener;
    std::optional<AcousticSurface> acoustics;
    std::optional<VfxSet> vfx;  // its particle and decal components, all of them (vfx/vfx_components.h)
    std::optional<std::vector<ScriptEntry>> scripts;  // its script components, all of them
    bool removed = false;  // deleted in the instance ("removed": true): not made, nor what hangs under it
    bool empty() const
    {
        return !hasLocal && !hidden && !material && !name && !light && !camera && !body && !collider && !audioSource && !audioListener &&
               !acoustics && !vfx && !scripts && !removed;
    }
};
// Overrides as JSON: an object of them by id ({"<id>": {...}}), as an instance's "overrides" (the
// edits; a nested prefab in a prefab).
void readOverrides(const Json& j, std::vector<PrefabOverride>& out);
void writeOverrides(JsonWriter& w, const std::vector<PrefabOverride>& list);  // as "overrides": {...}

class AnimationSystem;
class BoneEntities;
class Scene;
class ScenePak;
class SkinningSystem;

// (X2) One of the game's data components (a C# [Component] struct) on an entity: its name, layout
// and bytes. Set in the world by name (registered with that layout when the world has none yet,
// as the scripts would: plain data, no hooks), so a game's query finds it.
struct DataComponent {
    std::string name;
    uint32_t size = 0, align = 0;
    std::vector<uint8_t> bytes;
    bool operator==(const DataComponent&) const = default;
};
// Its id in the world (0: a component of that name with another layout, or none and !create).
uint64_t dataComponentId(flecs::world& world, const DataComponent& d, bool create);
void setDataComponents(flecs::entity e, const std::vector<DataComponent>& data);

namespace level {
class PolyMesh;
}

struct EntityEdit {
    uint32_t slot = ~0u;          // instances: the cooked slot; lights: the cooked light index; created primitives: their pool slot (apply)
    uint32_t uid = 0;             // created: stable id
    std::string kind = "primitive";  // created: primitive | empty | light | camera | water | level
    std::string primitive;        // kind primitive: the mesh's name (any mesh of the scene; "mesh" in the file)
    std::string name;             // instances: empty keeps the cooked name
    std::string parent;           // parent key ("i12", "c3"), empty for roots
    std::string material;         // meshes: a material asset drawn with instead of the mesh's own (.fmat, relative to the project)
    // Placed from a model: which of its nodes (or cameras) this is, so it plays the model's
    // animation (ScenePak::nodeTrack).
    std::string model;            // the model (library source, relative to the project)
    int32_t modelNode = -1, modelCamera = -1;
    // Prefab instances: kind "prefab" in the file (the prefab's path, the root's transform, the
    // overrides). expandPrefabs() turns one into its entities: the root (this edit, its kind now
    // the prefab root's) and one edit per other prefab entity, linked by prefabRoot / prefabChild.
    std::string prefab;           // the instance root: the prefab (.fprefab, relative to the project)
    std::vector<PrefabOverride> overrides;
    uint32_t prefabRoot = 0;      // expanded: the instance root's uid (0: not in a prefab instance)
    std::string prefabChild;      // expanded: the prefab entity's id ("0": the root)
    float world[3][4] = {};       // world space for roots, local (relative to the parent) for children
    bool hidden = false;
    bool deleted = false;
    bool hasLight = false;        // lights: the Light component ("light")
    Light light = {};
    bool hasCamera = false;       // cameras: the CameraComponent ("camera")
    CameraComponent camera = {};
    bool hasWater = false;        // water: the body ("water")
    WaterBodyDesc water;
    bool hasBody = false;         // any: a PhysicsBody ("body")
    PhysicsBody body = {};
    bool hasCollider = false;     // any: a Collider ("collider")
    Collider collider = {};
    bool hasAudioSource = false;  // any: an AudioSource ("audioSource")
    AudioSource audioSource = {};
    bool hasAudioListener = false;  // any: an AudioListener ("audioListener")
    AudioListener audioListener = {};
    bool hasAcoustics = false;    // a mesh: an AcousticSurface ("acoustics")
    AcousticSurface acoustics = {};
    VfxSet vfx;                   // any: particle and decal components ("particleEffect", "decal", ...)
    std::vector<ScriptEntry> scripts;  // any: script components ("scripts": script/script_entry.h)
    std::vector<DataComponent> data;   // any: the game's data components ("data": name, size, align, bytes as hex)
    bool hasAnimator = false;     // any: an Animator ("animator"; not in prefabs yet)
    AnimatorEdit animator;
    bool bones = false;           // a character's root: its skeleton expanded into entities ("bones")
    // Kind level: its editable mesh (level/poly_mesh.h; "levelMesh": base64 of its blob).
    std::shared_ptr<const level::PolyMesh> levelMesh;
    // A cut volume on it (level/level_meshes.h CutVolume; "cut": {"shape", "intersect", "cap", and
    // damage's "seed", "roughness"}).
    std::optional<CutVolume> cut;
    // A parametric wall ("wall": its parameters) and an opening in one ("opening").
    std::optional<level::Wall> wall;
    std::optional<WallOpening> opening;
    // A parametric roof (LevelRoof; "roof": {"outline", "style", "base", "pitch", "overhang", ...}).
    std::optional<level::Roof> roof;
    // Its painted mask (level/level_meshes.h LevelMask; "levelMask": base64 of a PNG).
    std::shared_ptr<const std::vector<uint8_t>> levelMask;
    // Its live wear's rules (LevelWear; "levelWear": [{"kind", "layer", "width", ...}]).
    std::vector<level::WearSettings> levelWear;
    // Its modifier stack (LevelModifiers; "modifiers": [{"kind", "enabled", ...}]).
    std::vector<level::Modifier> modifiers;
    // A spline (LevelSpline; "spline": {"points": [[x, y, z, corner]...], "closed", "loft": {...}}).
    std::optional<LevelSpline> spline;
    // A scatter set (ScatterSet; "scatterSet": {"seed"}).
    std::optional<ScatterSet> scatterSet;
    // A snap point (SnapPoint; "snapPoint": {"tag"}).
    std::optional<SnapPoint> snapPoint;
    // A gameplay volume (LevelVolume; "volume": {"kind", "tag", "cost"}).
    std::optional<LevelVolume> volume;
    flecs::entity_t entity = 0;   // set by apply()
};

// The level tools' components an entity carries, in the edits and in prefabs (so in their
// instances: a door prefab's cut, a kit piece's snap points): read and written by the same keys.
struct LevelParts {
    std::optional<CutVolume> cut;
    std::optional<level::Wall> wall;
    std::optional<WallOpening> opening;
    std::vector<level::Modifier> modifiers;
    std::optional<LevelSpline> spline;
    std::vector<level::WearSettings> levelWear;
    std::optional<ScatterSet> scatterSet;
    std::optional<SnapPoint> snapPoint;
    std::optional<LevelVolume> volume;
    std::optional<level::Roof> roof;
};
void readLevelParts(const Json& j, LevelParts& out);
void writeLevelParts(JsonWriter& w, const LevelParts& parts);
LevelParts levelPartsOf(const EntityEdit& e);
void setLevelParts(EntityEdit& e, const LevelParts& parts);

// A wall's parameters from JSON ("points": [[x, z], ...], "closed", "height", ...; what is missing
// left as it is): the edits and the editor's messages.
void readWallFields(const Json& j, level::Wall& wall);
void writeWall(JsonWriter& w, const char* key, const level::Wall& wall);

// Instance slots for one mesh the editor can create (any cooked mesh: built-ins, rocks, models).
struct PrimitivePool {
    std::string name;  // the mesh's name ("cube", "rock_boulder_a", a model's node)
    uint32_t mesh = 0, first = 0, size = 0, used = 0;
};

// A mesh with a displaced material: "<mesh>~<material>", cooked with the material's height map
// pushed into its geometry (tools/cook/build_displace.cpp). An entity with that mesh and material
// is drawn with it when the scene has it.
inline std::string displacedMeshName(const std::string& mesh, const std::string& material) { return mesh + "~" + material; }

struct SceneEdits {
    std::vector<EntityEdit> instances;
    std::vector<EntityEdit> created;
    std::vector<EntityEdit> lights;  // cooked lights, by index in `slot`
    uint32_t nextUid = 1;
    std::unordered_map<std::string, uint32_t> prefabKeys;  // prefabKey() -> its expanded uid

    static std::string pathFor(const char* scenePath);
    static std::string instanceKey(uint32_t slot) { return "i" + std::to_string(slot); }
    static std::string createdKey(uint32_t uid) { return "c" + std::to_string(uid); }
    static std::string lightKey(uint32_t index) { return "l" + std::to_string(index); }
    // An entity of a prefab instance, as a parent key: stable across saves (its uid is not).
    static std::string prefabKey(uint32_t rootUid, const std::string& id) { return "p" + std::to_string(rootUid) + ":" + id; }
    // A bone of an expanded skeleton (anim/bone_entities.h), by name under its character's root
    // (any other key): "b<root key>#<bone>" ("b<root key>#": the Skeleton group). Bones are not
    // saved (the root's "bones" brings them back); what hangs under one names it so.
    static std::string boneKey(const std::string& rootKey, const std::string& bone) { return "b" + rootKey + "#" + bone; }
    // After load(), before reservePools(): each prefab instance built from its prefab (read from
    // `projectDir`) and its overrides. Expanded entities get uids from kExpandedUids up (not saved).
    static constexpr uint32_t kExpandedUids = 0x40000000u;
    void expandPrefabs(const std::string& projectDir);
    // False (and empty edits) when the file is missing; logs malformed files.
    bool load(const std::string& path);
    // The same from text (`name`: for messages), and the file's text.
    bool loadText(std::string_view text, const std::string& name);
    std::string text() const;
    bool save(const std::string& path) const;

    // Before ScenePak::upload(): one pool per cooked mesh, with room for the created entities
    // plus spare slots (the editor's `spare` for built-ins, fewer for other meshes; 0 for the game).
    std::vector<PrimitivePool> reservePools(ScenePak& pak, uint32_t spare) const;
    // Before ScenePak::upload(): writes edited rows into the runtime instance table, spawns the
    // created entities (primitives take pool slots) and links parents. Records each entity (and
    // primitive slot) in its edit.
    // `materials` (may be null) resolves the entities' material assets; water bodies go into
    // `water` (may be null).
    void apply(ScenePak& pak, Scene& scene, std::vector<PrimitivePool>& pools, MaterialLibrary* materials = nullptr,
               WaterSystem* water = nullptr);
    // Once the characters can be posed (after AnimationSystem::init): the roots marked "bones"
    // expanded, and what hangs under their bones linked to them (apply() leaves bone parents out).
    // Returns how many skeletons.
    uint32_t expandBones(const ScenePak& pak, Scene& scene, BoneEntities& bones, const AnimationSystem& animation,
                         const SkinningSystem& skinning);
    // The entity a parent key names (after apply()); 0 when none.
    flecs::entity_t entityOfKey(const ScenePak& pak, Scene& scene, const std::string& key) const;
};

}  // namespace fx
