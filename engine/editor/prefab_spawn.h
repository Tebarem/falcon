#pragma once

// Prefabs made while a game runs (scripts' Prefab.Spawn, particles' collision prefabs): a prefab's
// entities created in the scene at a transform: its meshes (in slots kept for them when the scene
// loaded: vfx/runtime_meshes.h), empties, lights, cameras, audio, bodies and colliders, and
// particle and decal components (vfx/vfx_components.h). A mesh without a free slot is left out
// (its children hang from the nearest entity kept above it). The editor's own prefab instances
// (editor/prefab.h) are something else: linked, saved, edited.

#include "editor/prefab.h"

#include <functional>
#include <flecs.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fx {

class PhysicsWorld;
class RuntimeMeshes;
class Scene;
class ScenePak;

class PrefabSpawner {
public:
    void setProject(const std::string& dir) { projectDir_ = dir; }
    // Meshes' slots (none: meshes are left out), and the physics world the game's bodies are in.
    void setMeshes(RuntimeMeshes* meshes, ScenePak* pak)
    {
        meshes_ = meshes;
        pak_ = pak;
    }
    void setPhysics(PhysicsWorld* physics) { physics_ = physics; }
    // Where a spawned entity's script components go (the script host: they start with it).
    void setScripts(std::function<void(uint64_t entity, const std::vector<ScriptEntry>&)> add) { addScripts_ = std::move(add); }
    // The prefab's root (0 when it can't be read): at `world`, under `parent` when it is one.
    // `created`: every entity made, the root first.
    flecs::entity spawn(Scene& scene, const std::string& path, const float world[3][4], flecs::entity parent,
                        std::vector<uint64_t>* created = nullptr);

private:
    const Prefab* load(const std::string& path);

    std::string projectDir_;
    RuntimeMeshes* meshes_ = nullptr;
    ScenePak* pak_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    std::function<void(uint64_t, const std::vector<ScriptEntry>&)> addScripts_;
    struct Cached {
        std::filesystem::file_time_type time;
        Prefab prefab;
        bool ok = false;
    };
    std::unordered_map<std::string, Cached> cache_;
    std::unordered_set<std::string> warned_;
};

}  // namespace fx
