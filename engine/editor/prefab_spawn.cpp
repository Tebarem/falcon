#include "editor/prefab_spawn.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "editor/scene_edits.h"
#include "level/level_meshes.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "vfx/runtime_meshes.h"
#include "vfx/vfx_components.h"

#include <cstring>

namespace fx {

const Prefab* PrefabSpawner::load(const std::string& path)
{
    // Read again when the file changed (the editor saved it while a game runs).
    const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    const std::filesystem::path file = projectDir_.empty() ? u8(path) : u8(projectDir_) / u8(path);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec)
        return nullptr;
    Cached& c = cache_[path];
    if (c.time != time || (!c.ok && c.prefab.entities.empty())) {
        c.time = time;
        std::string error;
        const std::u8string name = file.u8string();
        c.ok = readPrefab(std::string(name.begin(), name.end()), c.prefab, error);
        if (!c.ok)
            FX_WARN("prefab: %s", error.c_str());
    }
    return c.ok ? &c.prefab : nullptr;
}

flecs::entity PrefabSpawner::spawn(Scene& scene, const std::string& path, const float world[3][4], flecs::entity parent,
                                   std::vector<uint64_t>* created)
{
    const Prefab* def = load(path);
    if (!def)
        return {};
    // Each prefab entity's entity, and its transform from that one (a mesh left out: its parent's
    // entity, and the mesh's own local).
    struct Made {
        flecs::entity e;
        float offset[3][4];
    };
    std::unordered_map<std::string, Made> made;
    std::vector<flecs::entity> bodies;  // (with a body or a collider)
    const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    flecs::entity root;
    for (const PrefabEntity& p : def->entities) {
        const bool isRoot = p.parent.empty();
        const auto up = made.find(p.parent);
        if (!isRoot && up == made.end())
            continue;  // (under something not made)
        float local[3][4], w[3][4];
        if (isRoot) {
            std::memcpy(local, identity, sizeof(local));
            std::memcpy(w, world, sizeof(w));
        } else {
            affineMultiply(up->second.offset, p.local, local);
            const WorldTransform* pw = up->second.e.try_get<WorldTransform>();
            affineMultiply(pw ? reinterpret_cast<const float(*)[4]>(pw->rows) : identity, local, w);
        }
        flecs::entity e;
        if (p.kind == "mesh") {
            // Its mesh (the material's displaced copy when the scene has slots for it) in a runtime slot.
            int32_t mesh = -1;
            if (meshes_ && pak_) {
                const int32_t displaced = p.material.empty() ? -1 : pak_->findMesh(displacedMeshName(p.mesh, p.material));
                mesh = displaced >= 0 && meshes_->has(uint32_t(displaced)) ? displaced : pak_->findMesh(p.mesh);
            }
            if (mesh >= 0)
                e = meshes_->spawn(scene, *pak_, uint32_t(mesh), w, meshes_->materialOf(p.material), p.hidden);
            if (!e) {
                if (warned_.insert(path + "|" + p.mesh).second)
                    FX_WARN("prefab: %s: mesh %s has no free runtime slot (left out; a mesh new to the scene: reload it)", path.c_str(),
                            p.mesh.c_str());
                if (!isRoot) {
                    Made m;
                    m.e = up->second.e;
                    std::memcpy(m.offset, local, sizeof(local));
                    made[p.id] = m;
                    continue;
                }
            }
        }
        if (!e)
            e = scene.spawnTransform(w);
        if (p.kind == "level" && p.levelMesh)  // (LevelMeshes gives it its live mesh)
            e.set<LevelMesh>({p.levelMesh});
        if (isRoot) {
            if (parent)
                scene.setParent(e, parent);
            root = e;
        } else {
            scene.setLocal(e, up->second.e, local);
        }
        if (p.light)
            e.set<Light>(*p.light);
        if (p.camera)
            e.set<CameraComponent>(*p.camera);
        if (p.audioSource) {
            AudioSource a = *p.audioSource;
            a.voice = 0;
            e.set<AudioSource>(a);
        }
        if (p.audioListener)
            e.set<AudioListener>(*p.audioListener);
        if (p.body)
            e.set<PhysicsBody>(*p.body);
        if (p.collider)
            e.set<Collider>(*p.collider);
        if (p.body || p.collider)
            bodies.push_back(e);
        if (!p.vfx.empty())
            setVfx(e, p.vfx);
        if (!p.scripts.empty() && addScripts_)
            addScripts_(e.id(), p.scripts);
        if (created)
            created->push_back(e.id());
        Made m;
        m.e = e;
        std::memcpy(m.offset, identity, sizeof(identity));
        made[p.id] = m;
    }
    scene.propagate();
    if (physics_ && !bodies.empty())
        physics_->addEntityBodies(bodies);
    return root;
}

}  // namespace fx
