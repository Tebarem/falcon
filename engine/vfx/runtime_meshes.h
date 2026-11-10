#pragma once

// Meshes drawn while a scene runs that it wasn't cooked or edited with: Particle Model Renderers'
// particles (vfx/vfx_components.h) and the meshes of prefabs spawned at runtime (Prefab.Spawn,
// collision prefabs: editor/prefab_spawn.h). The GPU's instance table is fixed when the scene is
// uploaded, so each mesh they may use gets a pool of hidden slots then (reserve()); a slot keeps
// its mesh.
//
// Each frame (update()): every Model Renderer's live particles take slots of its mesh's pool, a
// particle the same slot while it lives (its motion vectors and shadows follow it), the oldest
// first while slots last; slots of gone particles are hidden and given back. Spawned meshes keep
// theirs while their entity lives: hidden while it or an entity above it is disabled, given back
// when it goes, and destroyed with an entity above it.

#include "assets/fpak_format.h"
#include "scene/scene.h"

#include <flecs.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class ParticleSystem;
class ScenePak;
struct SceneEdits;  // (as defined: MSVC mangles class and struct apart)

class RuntimeMeshes {
public:
    struct Need {
        std::string mesh;  // its name (ScenePak::findMesh)
        uint32_t count;
    };
    // What a scene may draw at runtime: the Model Renderers of its edits and of the project's
    // prefabs (`projectDir`'s .fprefab files), and those prefabs' meshes. `editor`: the built-in
    // shapes too (a Model Renderer added while editing shows without a reload).
    static std::vector<Need> needs(const SceneEdits& edits, const std::string& projectDir, bool editor);
    // Before ScenePak::upload(): a pool per mesh (skinned meshes and unknown names left out).
    void reserve(ScenePak& pak, const std::vector<Need>& needs);
    // The material index an .fmat (project relative) draws with, or -1 (MaterialLibrary::indexOf).
    void setMaterials(std::function<int32_t(const std::string&)> resolve) { materialOf_ = std::move(resolve); }
    int32_t materialOf(const std::string& path) const { return materialOf_ && !path.empty() ? materialOf_(path) : -1; }

    bool has(uint32_t mesh) const { return pools_.count(mesh) != 0; }
    // An entity drawing `mesh` at `world` (a slot of its pool), with a material index (-1: the
    // mesh's own); invalid when the mesh has no pool or it is full.
    flecs::entity spawn(Scene& scene, ScenePak& pak, uint32_t mesh, const float world[3][4], int32_t material, bool hidden);

    // A spawned mesh's own visibility (a script's Renderer.Visible), apart from its entities'
    // being enabled. False when the entity is not a spawned mesh.
    bool setHidden(flecs::entity_t entity, bool hidden);
    bool hidden(flecs::entity_t entity) const
    {
        for (const Spawned& s : spawned_)
            if (s.entity == entity)
                return s.ownHidden;
        return false;
    }
    // A slot of the pools (its rows are this class's to stream). Other pools (the editor's shapes)
    // may lie between them.
    bool owns(uint32_t slot) const
    {
        return firstSlot_ != ~0u && slot >= firstSlot_ && slot - firstSlot_ < slotMesh_.size() && slotMesh_[slot - firstSlot_] != ~0u;
    }

    // Each frame, before the rows are streamed: the particles' models and the spawned meshes' slots.
    void update(Scene& scene, ScenePak& pak, const ParticleSystem& particles, std::vector<InstanceUpdate>& rows);

    struct Stats {
        uint32_t pools = 0, slots = 0;
        uint32_t models = 0, spawned = 0;  // drawn this frame: particles' models, spawned meshes
        uint32_t starved = 0;              // particles without a slot this frame (a pool full)
    };
    const Stats& stats() const { return stats_; }
    // A Model Renderer's mesh (by its vfxAssetId) that can't be drawn this session, or "" (the editor's warning).
    std::string missing(const ScenePak& pak, uint32_t meshId) const;

private:
    struct Pool {
        uint32_t first = 0, size = 0;
        std::vector<uint32_t> free;  // slots not taken (the last given first)
    };
    uint32_t acquire(uint32_t mesh);
    void release(ScenePak& pak, uint32_t slot, std::vector<InstanceUpdate>& rows);
    int32_t meshOf(const ScenePak& pak, uint32_t meshId);

    std::unordered_map<uint32_t, Pool> pools_;  // by mesh
    std::vector<uint32_t> slotMesh_;            // slot - firstSlot_ -> its mesh
    uint32_t firstSlot_ = ~0u;
    std::function<int32_t(const std::string&)> materialOf_;
    std::unordered_map<uint32_t, int32_t> meshIds_;  // vfxAssetId -> mesh (-1: none)
    std::unordered_map<uint32_t, int32_t> materialIds_;  // vfxAssetId -> material index, this frame
    // A particle's model: by (renderer entity, particle seed).
    struct Held {
        uint32_t slot, mesh;
        uint64_t frame;
    };
    std::unordered_map<uint64_t, Held> held_;
    struct Spawned {
        flecs::entity_t entity;
        uint32_t slot;
        bool hidden;  // as streamed (the entity's own flag: `ownHidden`)
        bool ownHidden;
    };
    std::vector<Spawned> spawned_;
    std::vector<InstanceUpdate> pending_;  // spawned since the last update()
    uint64_t frame_ = 0;
    Stats stats_;
};

}  // namespace fx
