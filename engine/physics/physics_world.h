#pragma once

#include "assets/fpak_format.h"
#include "core/math.h"
#include "scene/scene.h"
#include "terrain/terrain_scatter.h"

#include <box3d/box3d.h>
#include "physics/rollback/physics_snapshot.h"
#include <flecs.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <span>
#include <vector>

namespace fx {

class ScenePak;
class Scene;
class Terrain;
class WaterSystem;

struct PhysicsConfig {
    uint32_t maxBodies = 4096;  // dynamic bodies (half boxes, half spheres); the oldest is recycled
    uint32_t workers = 0;       // Box3D workers on the job system (0 = all job threads)
};

// What touches what, for sounds: each shape's owner (its user data: the kind in the top 2 bits,
// then an entity id or a cooked instance's slot; the ground (terrain, scatter) has none, its
// material being the contact's: a terrain layer + 1, 0 elsewhere).
enum class ContactOwner : uint8_t { None, Entity, Instance, Ground };
struct ContactSide {
    ContactOwner kind = ContactOwner::None;
    uint64_t id = 0;        // entity id, or instance slot
    uint32_t material = 0;  // (the ground's) terrain layer + 1; 0: none known
    float mass = 0.0f;      // 0: static
};
// A hit: the step's fastest approach of a pair that touched (Box3D's hit events).
struct PhysicsImpact {
    float3 point, normal;
    float speed;  // approach speed, m/s
    ContactSide a, b;
    uint64_t pair;  // the two shapes (cooldowns)
};
// A contact still touching after the last step: how fast it slides (slip) or rolls.
struct PhysicsContactMotion {
    uint64_t contact;
    float3 point;
    float slip;   // relative speed along the surface, m/s (0 while rolling)
    float roll;   // a ball's rolling speed, m/s (0: not rolling)
    float force;  // pressing the surfaces together, N
    ContactSide a, b;
};

// A body falling into water: where it broke the surface, and how hard.
struct WaterEntry {
    float3 point;   // on the surface
    float speed;    // down through it, m/s
    float size;     // its radius (a box: the cube of the same volume's half side), m
    float mass;
    int32_t water;  // the water body (WaterSystem id)
};
// A body at the surface after the last step: how fast it moves through the water there (it sloshes).
struct WaterStir {
    uint64_t body;  // the physics body (a key: follows it)
    float3 point;   // on the surface
    float speed;    // through the water (its flow taken out), m/s
    float size;     // the radius of the area the surface cuts, m
    int32_t water;
};

// A trigger (a Collider flagged one: a Box3D sensor) that something came into, or went out of.
struct TriggerEvent {
    uint64_t trigger;  // its entity
    ContactSide other;
    bool begin;        // came in (else went out)
};

// Scripts' queries: the layers they hit (bit i: Collider layer i; everything else is layer 0) and
// an entity whose shapes they pass through (0: none).
struct QueryFilter {
    uint32_t layers = ~0u;
    uint64_t ignore = 0;
};
// A shape to query with: a box (half extents), a sphere (size.x: its radius) or a capsule (size.x:
// its radius, size.y: its height, along its Y), at a place, turned by a quaternion (x, y, z, w).
struct QueryShape {
    enum Type : uint32_t { Box, Sphere, Capsule } type = Sphere;
    float3 size = {};
    float3 position = {};
    float rotation[4] = {0, 0, 0, 1};
};
// A capsule moved through the world (PhysicsWorld::moveCharacter).
struct CharacterMove {
    float3 position;      // where its base is now
    bool grounded;        // standing on something no steeper than its slope limit
    float3 groundNormal;  // that ground's normal (up when not grounded)
    ContactSide ground;   // what it stands on
    bool blocked;         // it met something it couldn't climb or slide along all the way
};

struct PhysicsStats {
    uint32_t bodies, awake, staticShapes, steps;
    uint32_t entityBodies;  // the scene's entities' (PhysicsBody, Collider) while the game runs
    float stepMs;  // CPU time of the last step
};

// M6: Box3D world for a scene. Static triangle meshes come straight from the .fpak collider
// blobs; dynamic bodies use the built-in cube/sphere meshes through instance pools, so the
// renderer only streams transforms. Steps at a fixed 60 Hz on the job system and renders an
// interpolated pose between the last two steps.
class PhysicsWorld {
public:
    enum class Shape : uint8_t { Box, Sphere };
    static constexpr float kStep = 1.0f / 60.0f;
    static constexpr int kSubSteps = 4;

    // Before pak.upload(): reserves the dynamic instance pools.
    bool init(ScenePak& pak, Scene& scene, const PhysicsConfig& config);
    void shutdown();

    // `size`: box half extents, or the radius in x for spheres.
    // Returns the body's ECS entity.
    uint64_t spawn(Shape shape, float3 position, float3 size, float3 velocity, float3 angularVelocity, uint32_t tint);
    // A deterministic grid of mixed boxes and spheres, `count` bodies, centred above `center`.
    void dropPile(float3 center, uint32_t count, uint32_t seed);
    void fire(float3 origin, float3 direction);
    // Bodies spawned so far (a counter), and those spawned since a count removed (the editor's
    // Stop: what Play spawned): entities deleted, slots hidden on the next extract.
    uint64_t spawnCount() const { return spawnCount_; }
    void removeSpawnedSince(uint64_t count);
    // A spawned body's entity going (a script destroyed it): its body goes and its slot is hidden
    // on the next extract; the caller deletes the entity. False when the entity has no pool body.
    bool removeBody(uint64_t entity);
    // A spawned body's entity disabled (a script: Enabled, or a scene's body destroyed while the
    // editor plays) or enabled again: the body stops and is hidden, or carries on as it was.
    bool setBodyEnabled(uint64_t entity, bool enabled);
    // (scripts) An entity's body: a spawned one's, or a scene entity's while the game runs.
    b3BodyId bodyOfEntity(uint64_t entity) const;
    // Moves an entity's body there at once (nothing swept on the way, no interpolation from
    // where it was), and the entity with it. rotation: x, y, z, w (null: as it is).
    bool teleport(uint64_t entity, float3 position, const float* rotation);
    // While the game runs, an entity's body built again from its PhysicsBody and Collider as they
    // are now (or removed: it has neither). Setting or removing either does this by itself at
    // the next step (an observer), so scripts and the Inspector can change bodies while playing.
    bool rebuild(uint64_t entity);
    // A body's type: 0 static, 1 kinematic (follows its entity), 2 dynamic.
    bool setBodyType(uint64_t entity, uint32_t type);
    // The triggers things came into or went out of during this update()'s steps.
    const std::vector<TriggerEvent>& triggers() const { return triggers_; }
    // Queries for scripts (Collider layers; an entity passed through).
    bool castRayFiltered(float3 from, float3 translation, const QueryFilter& filter, ContactSide& side, float3& point, float3& normal,
                         float& fraction) const;
    bool castShape(const QueryShape& shape, float3 translation, const QueryFilter& filter, ContactSide& side, float3& point,
                   float3& normal, float& fraction) const;
    // The entities whose shapes overlap the shape, each once (the scene's ground and cooked colliders,
    // whose shapes are no entity's, left out): copies up to `capacity`, returns how many there are.
    int overlap(const QueryShape& shape, const QueryFilter& filter, uint64_t* out, int capacity) const;
    // A character's capsule (base at `position`, `radius`, `height`) moved by `delta`: sliding
    // along what it meets (Box3D's mover: collide, solve the planes, cast), climbing steps up to
    // `stepHeight`, kept on the ground it walks down (snapped down to stepHeight). Ground steeper
    // than maxSlopeCos (the cosine of the slope limit) is a wall.
    CharacterMove moveCharacter(float3 position, float radius, float height, float stepHeight, float maxSlopeCos, float3 delta,
                                const QueryFilter& filter) const;
    // Joints between two entities' bodies (`b` 0: the world), anchored at a world point, about a
    // world axis (hinge, slider, wheel: its turning or sliding axis). Returns a handle (0: the
    // bodies aren't there). Gone with either body, with destroyJoint, or with clearJoints (Stop).
    enum class JointType : uint32_t { Fixed, Hinge, Ball, Slider, Distance, Wheel };
    struct JointSettings {
        float3 anchor = {}, axis = {0, 1, 0};
        float3 anchorB = {};       // Distance: the point on b (world)
        bool limit = false;        // Hinge: angles (radians); Slider: translations; Distance: lengths
        float lower = 0.0f, upper = 0.0f;
        bool motor = false;        // Hinge, Wheel: rad/s and N m; Slider: m/s and N
        float motorSpeed = 0.0f, motorMax = 0.0f;
        float spring = 0.0f, damping = 0.0f;  // spring hertz (0: rigid), damping ratio
        bool collide = false;      // the two bodies still collide
    };
    uint64_t createJoint(JointType type, uint64_t a, uint64_t b, const JointSettings& settings);
    bool destroyJoint(uint64_t joint);
    void setJointMotor(uint64_t joint, float speed, float maxForce);
    void clearJoints();
    float3 gravity() const;
    void setGravity(float3 g);

    // Called before each step with its length (the scripts' OnFixedUpdate). The simulation's
    // tick: dynamic entity bodies' entities hold the step's pose (not the render's interpolated
    // one) from before this callback until the frame's last step is done.
    void setStepCallback(std::function<void(float)> onStep) { onStep_ = std::move(onStep); }
    // Called after each step, its events collected (the scripts' collision callbacks, the
    // simulation's hash): impacts() and triggers() from stepImpactsBegin() / stepTriggersBegin()
    // on are this step's.
    void setAfterStepCallback(std::function<void()> afterStep) { afterStep_ = std::move(afterStep); }
    size_t stepImpactsBegin() const { return stepImpactsBegin_; }
    size_t stepTriggersBegin() const { return stepTriggersBegin_; }
    // Steps taken since the world was made (the simulation's tick count), and their time.
    uint64_t stepIndex() const { return stepIndex_; }
    double simTime() const { return double(stepIndex_) * double(kStep); }
    // Every body's pose and velocities hashed (Box3D's recorder's hash): equal on every PC that
    // ran the same steps (the simulation's determinism checks).
    uint64_t stateHash() const;
    // Advances in fixed steps. `fixedStep`: exactly one step per call (captures, tests).
    // `maxSteps`: at most this many steps (the time left waits for the next update; a rollback
    // session waiting for inputs).
    void update(float dt, bool fixedStep, uint32_t maxSteps = ~0u);

    // Rollback (the whole world simulated again from inputs): each step's state is kept for the
    // last `steps` steps (0: none), taken after each step and its events (where the scripts keep
    // theirs): Box3D's whole world and the engine's bodies. Static collision changing (terrain or
    // scatter streamed) drops what was kept.
    void setRollbackWindow(uint32_t steps);
    // Back to the state kept after the step that made stepIndex() `step`: false when it isn't kept.
    // The world's later steps are then taken again with stepOnce.
    bool rollbackTo(uint64_t step);
    // One step now (a rollback's later steps; their events are dropped: they were heard already).
    void stepOnce();
    // A rollback's step is being taken again (its after-step callback: not a new tick).
    bool resimulating() const { return resimulating_; }
    // A rollback's steps done: the render's in-between pose put back (as the frame's update left it).
    void rollbackDone();
    // Steps simulated again (rollbackTo/stepOnce) whose state came out differently from the first
    // time (the hash after the step), and how many were compared.
    uint64_t rollbackMismatches() const { return rollbackMismatches_; }
    uint64_t rollbackChecks() const { return rollbackChecks_; }
    // The hash after step `step` (stepIndex() was `step` after it), while kept (0: not).
    uint64_t stepHash(uint64_t step) const
    {
        if (stepHashes_.empty())
            return 0;
        const auto& slot = stepHashes_[step % stepHashes_.size()];
        return slot.first == step ? slot.second : 0;
    }
    // What keeping and restoring cost on average (ms), and a kept state's size (bytes).
    double keepMs() const { return keeps_ ? double(keepNs_) * 1e-6 / double(keeps_) : 0.0; }
    double restoreMs() const { return restores_ ? double(restoreNs_) * 1e-6 / double(restores_) : 0.0; }
    size_t keptBytes() const { return keptBytes_; }
    // Rows whose rendered pose changed since the last call (appended).
    void extract(std::vector<InstanceUpdate>& rows);
    // Terrain collision: one static height-field shape per resident tile within `radius` of
    // `focus`, rebuilt where the terrain changed. `rebuild` false: changed tiles wait (sculpting
    // in edit mode, where nothing simulates) and are rebuilt by a later call with true.
    // nullptr removes them all.
    void syncTerrain(const Terrain* terrain, float3 focus, float radius, bool rebuild);
    // Around several points (a server's players): a tile is wanted near any of them.
    void syncTerrain(const Terrain* terrain, std::span<const float3> foci, float radius, bool rebuild);
    // Terrain scatter collision (ScatterRenderer::takeCollision): static bodies per cell, with
    // their mesh's collider. Removed cells first, then added ones (replacing what a cell had).
    void syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed);

    // The scene's entities with a PhysicsBody and / or a Collider (one alone: static), as bodies
    // while the game runs (the editor's Play): built from their components and transforms, they
    // move their entities (dynamic) or follow them (kinematic). A cooked instance with one gives
    // up its own static collider meanwhile. A mesh collider on a moving body is its mesh's bounds.
    // Clearing destroys them (the editor's Stop).
    void buildEntityBodies();
    void clearEntityBodies();
    // Entities made while the game runs (spawned prefabs): bodies for those with a PhysicsBody or
    // a Collider, as buildEntityBodies() makes (nothing before it, or after clearEntityBodies()).
    // An entity's body goes when the entity does.
    void addEntityBodies(const std::vector<flecs::entity>& entities);
    // (X4) Shapes scripts give an entity (a PhysicsBody or Collider one), in its space (scaled
    // with it), in place of its Collider's shape (whose friction, bounce, layers and trigger flag
    // they keep): kind 0 a box (size: its full size), 1 a sphere (size[0]: radius), 2 a capsule
    // (size[0] radius, size[1] height, along Y), 3 a convex hull of `points` (x, y, z each); each
    // at `center`, turned by `rotation` (x, y, z, w). Added one by one (a compound) or cleared
    // (its Collider's again); its body is built again at the next step. They go with the bodies.
    struct ScriptShape {
        uint32_t kind = 0;
        float center[3] = {0, 0, 0}, rotation[4] = {0, 0, 0, 1}, size[3] = {1, 1, 1};
        std::vector<float> points;
    };
    bool addScriptShape(uint64_t entity, ScriptShape shape);
    void clearScriptShapes(uint64_t entity);

    // Water bodies (nullptr: none): what is in them floats (buoyancy at cells of each body, so
    // they bob and right themselves), is slowed (drag) and carried by the flow.
    void setWater(const WaterSystem* water) { water_ = water; }

    PhysicsStats stats() const;
    bool valid() const { return b3World_IsValid(world_); }
    // The hits of this update()'s steps, and the contacts touching after its last step.
    const std::vector<PhysicsImpact>& impacts() const { return impacts_; }
    const std::vector<PhysicsContactMotion>& motions() const { return motions_; }
    // Water: the bodies that fell in during this update()'s steps, and those at the surface as of
    // its last step.
    const std::vector<WaterEntry>& waterEntries() const { return waterEntries_; }
    const std::vector<WaterStir>& waterStirs() const { return waterStirs_; }
    // The first shape a ray from `from` along `translation` hits: whose it is (its material: the
    // ground's terrain layer), where, its normal, and how far along the ray (0 .. 1).
    bool castRay(float3 from, float3 translation, ContactSide& side, float3& point, float3& normal, float& fraction) const;
    // The same with a sphere of `radius` (a ray when 0); `body`: the hit body (b3StoreBodyId; 0 for
    // none), for applyImpulse. Safe from several threads at once while the world doesn't step
    // (Box3D's queries read it only).
    bool castSphere(float3 from, float radius, float3 translation, ContactSide& side, float3& point, float3& normal, float& fraction,
                    uint64_t* body = nullptr) const;
    // An impulse (N s) at a world point on a dynamic body (castSphere's), waking it.
    void applyImpulse(uint64_t body, float3 impulse, float3 point);
    // Box3D's timings of the last step (ms per stage).
    b3Profile profile() const { return valid() ? b3World_GetProfile(world_) : b3Profile{}; }

private:
    struct Body {
        b3BodyId id = {};
        flecs::entity entity;
        uint32_t slot = 0;
        Shape shape = Shape::Box;
        float3 scale = {1, 1, 1};  // instance scale of the unit mesh
        uint32_t tint = 0;
        b3Transform prev = {}, curr = {};
        b3Transform extracted = {};  // last extracted pose (unchanged => at rest, or physics paused)
        bool active = false, live = false, fresh = true;  // fresh: never extracted
        bool hidden = false;  // its entity disabled: the body too, the slot drawn hidden
        uint64_t serial = 0;  // spawnCount_ before it
    };
    struct Pool {
        uint32_t mesh = 0, firstSlot = 0, capacity = 0, next = 0;
        std::vector<int32_t> bodyOfSlot;
    };

    void step();
    void stepBegin();  // to the scripts' tick (and the state kept for rollback)
    void stepEnd();    // Box3D's step, its events, the after-step callback
    void collectContacts();  // after each step: its hits, and how touching contacts move
    ContactSide sideOf(b3ShapeId shape, uint64_t userMaterial) const;
    std::vector<PhysicsImpact> impacts_;
    std::vector<PhysicsContactMotion> motions_;
    std::unordered_map<uint64_t, b3ContactId> touching_;  // begun and not yet ended
    std::vector<WaterEntry> waterEntries_;
    std::vector<WaterStir> waterStirs_;
    std::unordered_map<uint64_t, uint64_t> wetStep_;  // by body: the last step it was in water
    void applyWater();  // before each step: buoyancy, drag and currents
    // One body's buoyancy, drag and wake: a ball (radius half.x) or a box (half extents), its
    // centre `offset` from the body's origin in the body's space.
    void floatBody(b3BodyId id, bool sphere, float3 half, float3 offset);
    // A static body for an instance of a mesh with a collider (rotation in the body, signed scale
    // in the shape; the built-in cube and sphere as a box and a ball). Invalid: none.
    b3BodyId createStaticBody(uint32_t mesh, const float world[3][4], void* owner = nullptr);
    fpak::InstanceGpu row(const Body& b, const b3Transform& t) const;
    void activate(uint32_t index);
    int32_t poolBodyOf(uint64_t entity) const;  // its live spawned body (RigidBody.index), or -1
    void collectTriggers();  // after each step: triggers things came into or went out of
    std::function<void(float)> onStep_;
    std::function<void()> afterStep_;
    size_t stepImpactsBegin_ = 0, stepTriggersBegin_ = 0;
    bool renderPosed_ = false;  // entities hold interpolated poses (update's end) until the next step
    std::vector<TriggerEvent> triggers_;
    // Entity bodies: by entity (an index into entityBodies_), the ones to build again at the next
    // step (their components set or removed while the game runs), and the joints scripts made.
    std::unordered_map<flecs::entity_t, uint32_t> entityBodyOf_;
    std::vector<flecs::entity_t> dirtyBodies_;
    std::unordered_map<flecs::entity_t, std::vector<ScriptShape>> scriptShapes_;  // (X4)
    // Creates them on a body (s: the entity's scale); their bounds' half extents and centre.
    bool addScriptShapes(b3BodyId body, const b3ShapeDef& sd, const std::vector<ScriptShape>& shapes, const float s[3], float3& half,
                         float3& center);
    bool writingBodyId_ = false;  // (addEntityBody's own PhysicsBody write: not a change to rebuild)
    flecs::observer bodyObserver_, colliderObserver_;
    std::vector<b3JointId> joints_;
    b3BodyId worldAnchor_ = {};  // static: joints to the world

    // An entity's body (buildEntityBodies); its index tagged in the body's user data.
    struct EntityBody {
        b3BodyId id = {};
        flecs::entity entity;
        float3 scale = {1, 1, 1};  // the entity's (signed) scale: the body moves it rigidly
        b3WorldTransform prev = {}, curr = {}, written = {};
        bool dynamic = false, kinematic = false;
        bool disabled = false;  // its entity disabled (a script): the body too, until enabled again
        // Its shape for buoyancy (dynamic, with a box or ball collider): half extents, centre.
        bool floats = false, sphere = false;
        float3 half = {}, offset = {};
        // On a live mesh's triangles (render/live_geometry.h): that mesh and its revision when
        // built; a newer revision builds the body again.
        uint32_t liveMesh = ~0u, liveRevision = 0;
    };
    static constexpr uintptr_t kEntityBodyTag = uintptr_t(1) << 30;
    // `reuse`: the index of an entity body built again (else a new one).
    void addEntityBody(flecs::entity e, const PhysicsBody* body, const Collider& collider, int32_t reuse = -1);
    bool entityBodiesBuilt_ = false;
    void writeEntityBodies(float alpha);  // dynamic bodies' poses into their entities (1: the last step's)
    std::vector<EntityBody> entityBodies_;
    uint32_t liveGeneration_ = 0;  // ScenePak::liveGeneration() when entity bodies were last checked
    std::unordered_map<uint32_t, b3BodyId> cookedStatic_;  // cooked instance slot -> its static body
    std::vector<b3BodyId> setAside_;                       // cooked static bodies disabled meanwhile

    b3WorldId world_ = {};
    Scene* scene_ = nullptr;
    Pool pools_[2];
    std::vector<Body> bodies_;
    std::vector<uint32_t> active_;
    std::vector<uint32_t> removed_;  // bodies that fell below the kill plane: hide their slot
    uint64_t spawnCount_ = 0;
    std::vector<b3BodyId> staticBodies_;
    uint32_t staticShapes_ = 0;
    struct TerrainTile {
        b3BodyId body = {};
        b3HeightFieldData* data = nullptr;
        bool stale = false;  // the terrain changed under it
    };
    const Terrain* terrain_ = nullptr;
    std::vector<TerrainTile> terrainTiles_;  // tiles per side ^ 2
    const ScenePak* pak_ = nullptr;
    std::unordered_map<uint64_t, std::vector<b3BodyId>> scatterCells_;
    uint64_t terrainRevision_ = 0;
    void dropTerrainTile(TerrainTile& t);
    uint64_t stepIndex_ = 0;
    float accumulator_ = 0.0f, alpha_ = 1.0f, stepMs_ = 0.0f;
    float killY_ = -1e30f;  // below the scene bounds
    const WaterSystem* water_ = nullptr;

    // Rollback: what each of the last steps kept (a ring by step), and each step's hash after it.
    struct Kept {
        uint64_t step = ~0ull;
        uint64_t staticRevision = 0;
        fxPhysicsSnapshot* world = nullptr;
        std::vector<EntityBody> entityBodies;
        std::unordered_map<flecs::entity_t, uint32_t> entityBodyOf;
        std::vector<Body> bodies;
        Pool pools[2];
        std::vector<uint32_t> active, removed;
        uint64_t spawnCount = 0;
        std::unordered_map<uint64_t, b3ContactId> touching;
        std::unordered_map<uint64_t, uint64_t> wetStep;
        std::vector<b3JointId> joints;
    };
    void keep();
    void dropKept();
    std::vector<Kept> kept_;
    std::vector<std::pair<uint64_t, uint64_t>> stepHashes_;  // (step, hash after it), by step
    uint64_t staticRevision_ = 0;  // static collision's changes (kept states before one are no use)
    bool resimulating_ = false;
    uint64_t rollbackMismatches_ = 0, rollbackChecks_ = 0;
    uint64_t keepNs_ = 0, restoreNs_ = 0, keeps_ = 0, restores_ = 0;
    size_t keptBytes_ = 0;
};

}  // namespace fx
