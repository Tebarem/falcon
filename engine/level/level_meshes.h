#pragma once
// Level meshes in the scene: entities with a LevelMesh component (an editable PolyMesh), each drawn
// as a live mesh (render/live_geometry.h) and given its MeshInstance. The same in the editor (the
// level tools change the component, undo puts an older one back) and in a game (the scene's edits
// bring them, scripts change them). A changed mesh (another PolyMesh; shared, never written in
// place) or, for faces with world UVs, a moved entity, is built again.

#include "level/cut_bake.h"
#include "level/mesh_cut.h"
#include "level/modifiers.h"
#include "level/poly_mesh.h"
#include "level/splines.h"
#include "level/roofs.h"
#include "level/walls.h"
#include "level/wear.h"
#include "scene/scene.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace enki {
class ITaskSet;
}

namespace fx {

class LiveGeometry;
class MaterialLibrary;
class SurfaceLayers;
class ScenePak;
struct SceneEdits;  // (as defined: MSVC mangles class and struct apart)

// The component: the entity's mesh, immutable (a change is a new PolyMesh).
struct LevelMesh {
    std::shared_ptr<const level::PolyMesh> mesh;
};

// A gameplay volume: a level mesh (drawn with the level tools, any shape) that isn't drawn but
// marks space. Trigger, kill, ladder and audio volumes are triggers (a box collider over the mesh's
// bounds, kept up as it changes: scripts' OnTriggerEnter / Exit, Falcon.Level.Volumes); nav
// blockers keep the navigation mesh out, nav areas cost `cost` times as much to walk through.
enum class VolumeKind : uint8_t { Trigger, Kill, Ladder, Audio, NavBlocker, NavArea };
inline constexpr const char* kVolumeKindNames[] = {"trigger", "kill", "ladder", "audio", "navBlocker", "navArea"};
inline constexpr uint32_t kVolumeKinds = 6;
struct LevelVolume {
    VolumeKind kind = VolumeKind::Trigger;
    std::string tag;     // what the game makes of it (a checkpoint's name, a sound zone's)
    float cost = 3.0f;   // (nav area)
    bool operator==(const LevelVolume&) const = default;
    bool trigger() const { return kind <= VolumeKind::Audio; }
};

// A snap point of a modular kit piece (an empty under the piece, usually in its prefab): where the
// piece joins another, facing out along its +z with +y up. A piece dragged or placed near another's
// free snap point (of a matching `tag`; "" matches any) clicks onto it, the two facing each other.
struct SnapPoint {
    std::string tag;
    bool operator==(const SnapPoint&) const = default;
};

// A scatter set: an empty whose children the scatter brush scattered (editor/level_scatter.cpp), and
// where its strokes go while it (or one of them) is selected.
struct ScatterSet {
    uint32_t seed = 1;
    bool operator==(const ScatterSet&) const = default;
};

// A spline (level/splines.h) on a level entity: drawn as its loft (its LevelMesh made from it
// whenever it changes; no profile: an empty mesh), and followed by the level meshes under it that
// have a curve modifier. Its points are in its entity's space.
struct LevelSpline {
    level::Spline spline;
    level::LoftSettings loft;
    bool operator==(const LevelSpline&) const = default;
};

// A level mesh's modifier stack (level/modifiers.h): applied in order to its LevelMesh before it is
// drawn and cut (and collided with). The mesh stays as it was edited: the element tools edit it, and
// a mirror's other half or an array's copies follow; picks of the copies give its faces.
struct LevelModifiers {
    std::vector<level::Modifier> stack;
    bool operator==(const LevelModifiers&) const = default;
};

// A cut volume (level/mesh_cut.h): the level meshes it overlaps are drawn (and collided with) with
// it applied, made again as either moves or changes. A unit shape (a 1 m box, a 1 m-wide cylinder
// standing on y, a 1 m sphere) sized by its entity's transform; Mesh: its entity's own LevelMesh,
// which then isn't drawn.
struct CutVolume {
    level::CutShape shape = level::CutShape::Box;
    bool intersect = false;
    std::string capMaterial;  // the faces it makes ("": the target's first material)
    uint32_t seed = 0;        // (damage) its lumps
    float roughness = 0.35f;  // (damage) how far they push in and out
    bool operator==(const CutVolume&) const = default;
};

// A parametric wall or room (level/walls.h): its LevelMesh is made from it whenever it changes.
struct LevelWall {
    level::Wall wall;
    bool operator==(const LevelWall&) const = default;
};

// A parametric roof (level/roofs.h): its LevelMesh is made from it whenever it changes.
struct LevelRoof {
    level::Roof roof;
    bool operator==(const LevelRoof&) const = default;
};

// An opening in a wall: a cut volume (CutVolume, a box or a cylinder) under a LevelWall entity,
// placed on its parent's path `distance` metres along it, `sill` above its foot, `width` x
// `height`, through its thickness. It slides with the wall; its own transform is not used.
struct WallOpening {
    float distance = 1.0f, sill = 0.0f, width = 1.0f, height = 2.1f;
    bool operator==(const WallOpening&) const = default;
};

// A layered level mesh's painted mask (render/surface_layers.h): SurfaceLayers::kMaskSize² RGBA8,
// r g b the weights of its layers 1..3, a the wear. Immutable like the mesh; none: all zero (its own
// materials only). Apart from the mesh: mesh operations don't carry a megabyte along.
struct LevelMask {
    std::shared_ptr<const std::vector<uint8_t>> rgba;
};

// Live wear and tear on a layered level mesh (level/wear.h): its rules' edge wear, grime and
// streaks made again, off the main thread, whenever it is drawn differently: edited, cut (a door's
// edges worn as it is cut), turned or scaled (widths are the world's metres, streaks run down the
// world's up), or the level meshes around it change (grime where they meet it). Drawn under its
// painted mask (the higher of the two); what is made isn't saved.
struct LevelWear {
    std::vector<level::WearSettings> rules;
    bool operator==(const LevelWear&) const = default;
};

struct LevelMeshStats {
    uint32_t meshes = 0;     // entities drawn
    uint32_t starved = 0;    // without a live mesh (none left)
    uint32_t builds = 0;     // since attach
    uint32_t cuts = 0;       // builds with cut volumes applied, since attach
    uint32_t bakedCuts = 0;  // of those, taken from the scene's baked cuts (not cut again)
    float lastCutMs = 0.0f;  // the last cut made (on a worker thread)
    float lastBuildMs = 0.0f;
    uint32_t wearMade = 0;   // live wear masks made, since attach
    float lastWearMs = 0.0f;
};

class LevelMeshes {
public:
    // Live room a scene's edits' level meshes need (meshes, cluster blocks), reserved on top of the
    // room for what is made while it runs.
    static void needs(const SceneEdits& edits, uint32_t& meshes, uint32_t& blocks);

    // After the renderer is up (the live meshes exist). `materials`: .fmat paths to material
    // indices (null: slot index as is).
    // `layers` and `setInstanceLayer` (the renderer's): layered meshes' masks and records, and an
    // instance slot's layer (-1: none); without them, layers aren't drawn.
    // `setInstanceHidden`: an instance drawn or not whatever its row says (imported art a cut
    // volume overlaps: drawn as a cut copy instead).
    void attach(Scene& scene, ScenePak& pak, LiveGeometry& live, MaterialLibrary* materials, SurfaceLayers* layers = nullptr,
                std::function<void(uint32_t slot, int32_t layer)> setInstanceLayer = {},
                std::function<void(uint32_t slot, bool hidden)> setInstanceHidden = {});
    void detach();
    // The scene's baked cuts (level/cut_bake.h): read after attach, so what was cut when it was
    // saved isn't cut again; written when the editor saves it (what is drawn cut now). False: none
    // read (no file), or not written.
    bool loadBake(const std::string& scenePath);
    bool saveBake(const std::string& scenePath) const;
    size_t bakeSize() const { return bake_.results.size(); }
    // Cuts are made on worker threads: a mesh keeps being drawn as it was (its last cut) until its
    // new one is made, then the newest is drawn. A scene's first cuts are waited for (all at once)
    // in the update that finds them. Cuts being made now:
    uint32_t cutsPending() const;
    // Drawn clipped (cut without caps: its mesh, or the art, isn't closed): the editor's badge.
    // The version moves whenever which ones are changes.
    bool clipped(flecs::entity_t entity) const;
    bool copyClipped(uint32_t slot) const;
    uint32_t clippedVersion() const { return clippedVersion_; }
    // Whether it shows and hides the rows itself (a game: by the entities being enabled), or the
    // editor does (its eye, Delete; EditorBridge::refreshRows once the entity has its slot).
    void setManageVisibility(bool manage) { manageVisibility_ = manage; }
    // The material faces without one draw with (a project's .fmat: its blockout grid; "": the
    // scene's first). Set after attach.
    void setDefaultMaterial(const std::string& path)
    {
        defaultMaterial_ = path;
        defaultIndex_ = -2;
    }
    int32_t defaultMaterialIndex() const;  // (-1: none, the scene's first)
    // Each frame, before the rows stream: entities that got a mesh, changed or went; rows to stream.
    void update(std::vector<InstanceUpdate>& rows);
    const LevelMeshStats& stats() const { return stats_; }
    // The face a pick of a level mesh entity hit (from its live mesh's triangle; -1: none).
    int32_t faceOf(flecs::entity_t entity, uint32_t cluster, uint32_t triangle) const;
    int32_t liveOf(flecs::entity_t entity) const;
    // Imported art (cooked or created mesh instances) a cut volume overlaps is drawn as a cut copy
    // (a live mesh) and hidden itself: the instance a copy's slot stands for (-1: not a copy).
    int32_t originalOf(uint32_t liveSlot) const;
    int32_t copyOf(uint32_t slot) const;  // the cut copy's instance slot of an instance (-1: none)
    // Where an opening's cut is (its wall's path; false: not an opening under a wall).
    static bool openingWorld(flecs::entity opening, float out[3][4]);
    // An opening's frame (world): at its foot's middle on its wall's path, x along the wall, y up, z
    // across it, unscaled. The editor keeps an opening's own transform there, so what is under it (a
    // door prefab's frame and leaf) goes with it. False: its distance is off the path.
    static bool openingFrame(const level::Wall& wall, const float wallWorld[3][4], const WallOpening& o, float out[3][4]);
    uint32_t cutCopies() const { return uint32_t(copies_.size()); }
    // The level as it is drawn (each level mesh's drawn geometry where it is, the cut copies of
    // imported art): equal on machines that cut the same way. The running scene's LevelMeshes.
    uint64_t geometryHash() const;
    static LevelMeshes* current();
    // What an entity's level mesh is drawn as (its modifiers and cuts applied; null: not built).
    std::shared_ptr<const level::PolyMesh> drawnOf(flecs::entity_t entity) const;
    // An entity's live wear as made (LevelWear; null: none yet), and whether any is still to make.
    const std::vector<uint8_t>* wearOf(flecs::entity_t entity) const;
    bool wearPending() const { return wearPending_ > 0; }

private:
    struct WearJob;
    struct CutJob;
    struct Entry {
        int32_t live = -1;
        std::shared_ptr<const level::PolyMesh> built;  // what it was built from
        std::shared_ptr<const level::PolyMesh> builtShape;  // (and drawn as: its modifiers applied)
        // Its modifiers applied (LevelModifiers): to what (its mesh, the stack), the result and each
        // of its faces' face in the mesh (empty: none applied).
        std::shared_ptr<const level::PolyMesh> modifiedFrom, modified;
        std::vector<level::Modifier> modifiers;
        std::vector<uint32_t> modifiedFace;
        level::Spline curve;  // the spline it was modified along (its parent's, in its space)
        uint32_t scripts = 0;  // the C# modifiers' code it was modified with (level::scriptModifierGeneration)
        const void* volumeOf = nullptr;  // (a volume: the mesh its trigger collider was fitted to)
        float world[3][4] = {};
        bool worldUvs = false;  // a face with world UVs: moving the entity builds it again
        bool hidden = true;  // its row's flag (kept only while managing visibility)
        bool shown = false;  // drawn: enabled, not empty, not a volume (geometryHash)
        std::vector<uint32_t> faceOfTriangle;
        bool seen = false;
        uint64_t cuts = 0;                 // the cuts it was built with (their signature; 0: none)
        int32_t layer = -1;                // its surface layer (layered meshes)
        uint32_t record[4] = {~0u, ~0u, ~0u, ~0u};  // what the layer's record was set to
        const void* mask = nullptr;        // the LevelMask's image it got
        std::shared_ptr<const level::PolyMesh> drawn;  // what it is drawn as (cut)
        // Cut: from what (the shape, where it was: `world`, the cuts) and the result (for the bake).
        std::shared_ptr<const level::PolyMesh> cutTarget;
        std::vector<level::Cut> cutWith;
        std::shared_ptr<const level::BakedCut> cutResult;
        std::shared_ptr<CutJob> cutJob;  // its newest cut, being made
        float3 lo = {}, hi = {};                       // its bounds, drawn (in its space)
        // Live wear (LevelWear): made from what (a hash), the job making it, what it made, when
        // the last one started; what the layer got (the paint and the wear).
        uint64_t wearKey = 0;
        std::shared_ptr<WearJob> wearJob;
        std::vector<uint8_t> wear, composed;
        uint64_t wearStartNs = 0;
        uint32_t wearVersion = 0, maskVersion = ~0u;
    };
    void updateWear();
    std::vector<std::shared_ptr<enki::ITaskSet>> orphans_;  // jobs of entries gone (or overtaken), still running
    uint32_t wearPending_ = 0;
    // `shape`: what is drawn (the mesh with its modifiers applied; `modifiedFace`: its faces' faces
    // in the mesh); `from`: the mesh itself; `result`: the shape with `cuts` (null: uncut).
    // `ready`: a cut job that built what is drawn already (taken from it when it did).
    void build(flecs::entity_t id, Entry& entry, const std::shared_ptr<const level::PolyMesh>& from,
               const std::shared_ptr<const level::PolyMesh>& shape, const float world[3][4], const std::vector<level::Cut>& cuts,
               const std::shared_ptr<const level::BakedCut>& result, const std::vector<uint32_t>* modifiedFace,
               CutJob* ready = nullptr);
    // This frame's cut volumes (their world shapes, bounds, signatures).
    void gatherCuts();
    struct CutInfo {
        level::Cut cut;
        float3 lo, hi;
        uint64_t signature;
        flecs::entity_t entity;
    };
    std::vector<CutInfo> cuts_;
    struct Copy {
        int32_t live = -1;
        uint64_t signature = 0;
        bool seen = false;
        // What it was cut from (the read-back mesh, where, the cuts) and the result (for the bake).
        std::shared_ptr<const level::PolyMesh> target;
        float world[3][4] = {};
        std::vector<level::Cut> cutWith;
        std::shared_ptr<const level::BakedCut> result;
        std::shared_ptr<CutJob> job;  // its newest cut, being made
    };
    // `target` at `world` with `cuts` applied: the scene's baked result when it has one (the job
    // done already), else a job on the workers. `from`/`modifiedFace`: as for build.
    std::shared_ptr<CutJob> startCut(const std::shared_ptr<const level::PolyMesh>& from, const std::shared_ptr<const level::PolyMesh>& target,
                                     const std::vector<uint32_t>* modifiedFace, const float world[3][4], const std::vector<level::Cut>& cuts,
                                     uint64_t signature);
    void applyCut(flecs::entity_t id, Entry& entry, CutJob& job);
    void applyCopy(uint32_t slot, flecs::entity_t id, Copy& copy, CutJob& job, std::vector<InstanceUpdate>& rows);
    void orphan(std::shared_ptr<CutJob>& job);  // (let go of: waited for at detach while it runs)
    level::CutBake bake_;
    void updateCopies(std::vector<InstanceUpdate>& rows);
    void updateWalls();
    std::unordered_map<flecs::entity_t, level::Wall> wallsBuilt_;  // what each wall's mesh was made from
    std::unordered_map<flecs::entity_t, level::Roof> roofsBuilt_;  // (and each roof's)
    void updateSplines();
    std::unordered_map<flecs::entity_t, LevelSpline> splinesBuilt_;  // what each spline's loft was made from
    // The spline a mesh's curve modifiers follow: its parent's, brought into its space (false: none).
    bool curveOf(flecs::entity e, const float world[3][4], level::Spline& out) const;
    void dropCopy(uint32_t slot, Copy& copy, std::vector<InstanceUpdate>& rows);
    std::unordered_map<uint32_t, Copy> copies_;  // original instance slot -> its cut copy
    std::unordered_map<uint32_t, std::shared_ptr<const level::PolyMesh>> pakMeshes_;  // cooked mesh read back (null: can't)
    std::function<void(uint32_t, bool)> setInstanceHidden_;
    uint32_t cutWarnings_ = 0;
    void updateLayer(flecs::entity e, Entry& entry, const LevelMesh& m);
    void uploadMask(flecs::entity e, Entry& entry);  // its painted mask and live wear, to its layer
    void dropLayer(Entry& entry);

    Scene* scene_ = nullptr;
    ScenePak* pak_ = nullptr;
    LiveGeometry* live_ = nullptr;
    MaterialLibrary* materials_ = nullptr;
    SurfaceLayers* layers_ = nullptr;
    std::function<void(uint32_t, int32_t)> setInstanceLayer_;
    std::unordered_map<flecs::entity_t, Entry> entries_;
    flecs::query<const LevelMesh> query_;
    LevelMeshStats stats_;
    uint32_t warnings_ = 0;
    bool manageVisibility_ = true;
    uint32_t clippedVersion_ = 0;
    std::string defaultMaterial_;
    mutable int32_t defaultIndex_ = -2;  // (-2: not looked up yet)
};

}  // namespace fx
