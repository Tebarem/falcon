#pragma once

#include "assets/fpak_format.h"
#include "components.gen.h"

#include <flecs.h>

#include <cstdint>
#include <vector>

namespace fx {

class ScenePak;

// ECS components (WorldTransform, LocalTransform, Parent, MeshInstance, Light, CameraComponent, RigidBody,
// PhysicsBody, Collider, AudioSource, AudioListener, AcousticSurface) are generated
// from scene/components.fschema and shared with C# scripts. Gameplay state lives in flecs; the
// renderer reads GPU-ready copies.

// 3x4 affine transforms (rows of a matrix whose last row is 0 0 0 1), as WorldTransform stores them.
void affineMultiply(const float a[3][4], const float b[3][4], float out[3][4]);  // out = a * b
void affineInverse(const float m[3][4], float out[3][4]);                       // identity if singular
// A component's `float rows[12]` seen as 3x4 rows.
inline float (*rows3x4(float* rows))[4] { return reinterpret_cast<float (*)[4]>(rows); }
inline const float (*rows3x4(const float* rows))[4] { return reinterpret_cast<const float (*)[4]>(rows); }

// One instance-table row to stream to the GPU (dynamic instances).
struct InstanceUpdate {
    uint32_t slot;
    fpak::InstanceGpu row;
};

class Scene {
public:
    // Cheap enough to run while the GPU device is still being created.
    void init();
    // One entity per cooked instance.
    void spawnFromPak(const ScenePak& pak);
    // A rendered, non-physics entity for a runtime instance slot (crowds, editor-created).
    flecs::entity spawnInstance(uint32_t slot, const fpak::InstanceGpu& row);
    // A runtime slot given back (its entity gone): no entity renders it.
    // An existing entity drawing through a slot from now on (it has the MeshInstance already).
    void bindInstance(uint32_t slot, flecs::entity_t entity)
    {
        if (slot >= slots_.size())
            slots_.resize(slot + 1, 0);
        slots_[slot] = entity;
    }
    void forgetInstance(uint32_t slot)
    {
        if (slot < slots_.size())
            slots_[slot] = 0;
    }
    // An entity with a transform only (editor empties; lights and cameras add their component).
    flecs::entity spawnTransform(const float world[3][4]);
    // One entity per cooked light: points and spots (the first `shadowBudget` of them, spots
    // first and brightest first, cast shadows), and the first directional light, the sun.
    // Without one the scene gets a default sun, as cooked light index `count` (one past the
    // pak's), so it is edited and saved like a cooked light.
    void spawnLights(const ScenePak& pak, uint32_t shadowBudget);
    // The entity of cooked light `index` (0 for directional lights).
    flecs::entity lightEntity(uint32_t index) const
    {
        return index < lights_.size() && lights_[index] ? flecs::entity(world_, lights_[index]) : flecs::entity();
    }
    uint32_t cookedLightCount() const { return uint32_t(lights_.size()); }
    // The main camera (an enabled CameraComponent entity flagged main; the oldest if several).
    bool mainCamera(float world[3][4], CameraComponent& camera) const;
    // Cooked light `index` as spawned (before edits): false for directional lights.
    bool cookedLight(uint32_t index, Light& light, float world[3][4]) const;

    // Hierarchy: a child has Parent + LocalTransform, and propagate() derives its WorldTransform.
    // setParent keeps the child's world transform (0 = make it a root again); setWorld moves an
    // entity in world space (updating its LocalTransform when it has a parent).
    void setParent(flecs::entity child, flecs::entity parent);
    // Links `child` under `parent` with this local transform (its world follows at propagate()).
    void setLocal(flecs::entity child, flecs::entity parent, const float local[3][4]);
    void setWorld(flecs::entity e, const float world[3][4]);
    // The parent of `e` (0 for roots).
    flecs::entity parentOf(flecs::entity e) const;
    // The children of `e` (disabled ones too), and every entity below it (children first, then
    // theirs): walks every child in the scene.
    void childrenOf(flecs::entity_t e, std::vector<flecs::entity_t>& out) const;
    void descendantsOf(flecs::entity_t e, std::vector<flecs::entity_t>& out) const;
    // Children's world transforms from their parents', parents first. Only changed results are
    // written, so unchanged children are not re-streamed.
    void propagate();
    // The entity rendering an instance slot (0 for physics pool slots and unused slots).
    flecs::entity entity(uint32_t slot) const
    {
        return slot < slots_.size() && slots_[slot] ? flecs::entity(world_, slots_[slot]) : flecs::entity();
    }

    // Rows of rendered, non-physics instances whose WorldTransform changed since the last call
    // (scripts write transforms in place; flecs tracks changes per table, so a changed table
    // is re-streamed whole). Physics bodies stream their interpolated pose themselves.
    void extractChanged(const ScenePak& pak, std::vector<InstanceUpdate>& rows);

    flecs::world& world() { return world_; }
    uint32_t instanceCount() const { return instanceCount_; }

private:
    flecs::world world_;
    flecs::query<const WorldTransform, const MeshInstance> extract_;
    flecs::query<const Parent> children_;
    // propagate's children (child, parent) as found, and in parent-first order (made again only
    // when what is found changes).
    std::vector<std::pair<flecs::entity_t, flecs::entity_t>> found_, order_;
    uint32_t instanceCount_ = 0;
    std::vector<flecs::entity_t> slots_;
    std::vector<flecs::entity_t> lights_;  // cooked light index -> entity
    struct SpawnedLight {
        Light light;
        float world[3][4];
    };
    std::vector<SpawnedLight> spawnedLights_;
};

}  // namespace fx
