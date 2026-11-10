#include "physics/physics_world.h"

#include "assets/scene_pak.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "scene/scene.h"
#include "terrain/terrain.h"
#include "water/water_system.h"

#include <TaskScheduler.h>
#include <box3d/collision.h>

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

// Box3D internals (box3d/src, compiled as C into its static library): the world behind an id and
// the hash of its bodies' state that its recorder checks replays with.
extern "C" {
struct b3World;
b3World* b3GetWorldFromId(b3WorldId id);
uint64_t b3HashWorldState(b3World* world);
}

namespace fx {

namespace {

// Collision layers (Collider.layer, .ignore): a shape's category is its layer's bit; the scene's
// ground, cooked colliders and spawned bodies are on layer 0.
constexpr uint64_t kLayer0 = 1;
b3Filter layerFilter(uint32_t layer, uint32_t ignore)
{
    b3Filter f = b3DefaultShapeDef().filter;
    f.categoryBits = uint64_t(1) << (layer & 31u);
    f.maskBits = ~uint64_t(ignore);
    return f;
}

// A shape's owner as its user data (ContactOwner in the top 2 bits).
void* ownerData(ContactOwner kind, uint64_t id) { return reinterpret_cast<void*>((uint64_t(kind) << 62) | (id & ((uint64_t(1) << 62) - 1))); }
uint64_t contactKey(b3ContactId c) { return (uint64_t(uint32_t(c.index1)) << 32) | uint64_t(c.generation); }
uint64_t bodyKey(b3BodyId b) { return (uint64_t(uint32_t(b.index1)) << 32) | uint64_t(b.generation); }
uint64_t pairKey(b3ShapeId a, b3ShapeId b)
{
    const uint32_t x = uint32_t(a.index1), y = uint32_t(b.index1);
    return x < y ? (uint64_t(x) << 32) | y : (uint64_t(y) << 32) | x;
}

// Box3D task callbacks on enkiTS. Box3D enqueues at most B3_MAX_TASKS per step and waits for
// all of them before b3World_Step returns, so a per-step pool of task objects suffices.
struct Box3dTask : enki::ITaskSet {
    b3TaskCallback* fn = nullptr;
    void* context = nullptr;
    uint32_t name = 0;  // profiler zone: Box3D's task name
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override
    {
        const profiler::ScopedZone zone(name);
        fn(context);
    }
};

struct TaskPool {
    Box3dTask tasks[B3_MAX_TASKS];
    std::atomic<int> count{0};
};

TaskPool g_tasks;

void* enqueueTask(b3TaskCallback* task, void* taskContext, void* userContext, const char* taskName)
{
    auto* pool = static_cast<TaskPool*>(userContext);
    const int i = pool->count.fetch_add(1);
    if (i >= B3_MAX_TASKS) {
        task(taskContext);  // out of task objects: run inline
        return nullptr;
    }
    Box3dTask& t = pool->tasks[i];
    t.fn = task;
    t.context = taskContext;
    t.name = profiler::intern(taskName ? taskName : "box3d task", profiler::Category::Physics);
    jobs::scheduler().AddTaskSetToPipe(&t);
    return &t;
}

void finishTask(void* userTask, void*)
{
    jobs::scheduler().WaitforTask(static_cast<Box3dTask*>(userTask));
}

// Box3D warnings (possibly from worker threads), each distinct message logged once.
void logBox3d(const char* message)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(message).second)
        FX_WARN("box3d: %s (further repeats not logged)", message);
}

b3Vec3 v3(float3 v) { return {v.x, v.y, v.z}; }
float3 f3(b3Vec3 v) { return {v.x, v.y, v.z}; }

// Deterministic PCG-style hash for pile layouts and colours.
uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float unit(uint32_t x) { return float(hash(x) & 0xffffff) / float(0x1000000); }

uint32_t packTint(float r, float g, float b)
{
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | c(g) << 8 | c(b) << 16 | 0xffu << 24;
}

// Saturated, well-separated colours (linear).
uint32_t paletteTint(uint32_t i)
{
    static const float kPalette[][3] = {{0.80f, 0.10f, 0.08f}, {0.95f, 0.55f, 0.05f}, {0.95f, 0.85f, 0.10f},
                                        {0.15f, 0.60f, 0.15f}, {0.05f, 0.35f, 0.80f}, {0.45f, 0.15f, 0.70f},
                                        {0.90f, 0.90f, 0.88f}, {0.10f, 0.10f, 0.11f}};
    const float* c = kPalette[i % (sizeof(kPalette) / sizeof(kPalette[0]))];
    return packTint(c[0], c[1], c[2]);
}

}  // namespace

bool PhysicsWorld::init(ScenePak& pak, Scene& scene, const PhysicsConfig& config)
{
    FX_ZONE_CAT("PhysicsWorld::init", Physics);
    b3SetLogFcn(logBox3d);
    const uint64_t t0 = SDL_GetTicksNS();
    scene_ = &scene;
    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = {0.0f, -9.81f, 0.0f};
    def.workerCount = config.workers ? std::min(config.workers, jobs::threadCount()) : jobs::threadCount();
    def.enqueueTask = enqueueTask;
    def.finishTask = finishTask;
    def.userTaskContext = &g_tasks;
    world_ = b3CreateWorld(&def);
    if (b3World_IsValid(world_))
        b3World_SetHitEventThreshold(world_, 0.3f);  // (soft taps sound too)
    if (!b3World_IsValid(world_)) {
        FX_ERROR("physics: cannot create the Box3D world");
        return false;
    }

    // Static collision: one static body per cooked instance whose mesh has a collider. The
    // instance matrix is split into a rotation (body) and a signed scale (mesh shape).
    const fpak::Header& h = pak.header();
    killY_ = h.boundsMin[1] - 20.0f;
    pak_ = &pak;
    for (uint32_t i = 0; i < pak.cookedInstanceCount(); ++i) {
        const fpak::InstanceGpu& inst = pak.instances()[i];
        const b3MeshData* mesh = pak.staticCollider(inst.mesh);
        if (!mesh || (inst.flags & fpak::kInstanceHidden))  // hidden: deleted in the editor
            continue;
        const b3BodyId body = createStaticBody(inst.mesh, inst.world, ownerData(ContactOwner::Instance, i));
        if (!b3Body_IsValid(body))
            continue;
        staticBodies_.push_back(body);
        cookedStatic_[i] = body;
        ++staticShapes_;
    }

    // Joints to the world hang from this.
    b3BodyDef anchor = b3DefaultBodyDef();
    worldAnchor_ = b3CreateBody(world_, &anchor);
    // While the game runs, an entity's body follows its components: set or removed (a script, the
    // Inspector), it is built again at the next step.
    const auto changed = [this](flecs::entity e) {
        if (entityBodiesBuilt_ && !writingBodyId_)
            dirtyBodies_.push_back(e.id());
    };
    bodyObserver_ = scene.world().observer<const PhysicsBody>().event(flecs::OnSet).event(flecs::OnRemove).each(
        [changed](flecs::entity e, const PhysicsBody&) { changed(e); });
    colliderObserver_ = scene.world().observer<const Collider>().event(flecs::OnSet).event(flecs::OnRemove).each(
        [changed](flecs::entity e, const Collider&) { changed(e); });

    // Dynamic pools: half boxes, half spheres, on the built-in meshes.
    const uint32_t perPool = std::max(config.maxBodies / 2, 1u);
    const uint32_t meshes[2] = {h.builtinMeshes[uint32_t(fpak::Builtin::Cube)], h.builtinMeshes[uint32_t(fpak::Builtin::Sphere)]};
    for (int p = 0; p < 2; ++p) {
        pools_[p].mesh = meshes[p];
        pools_[p].capacity = perPool;
        pools_[p].firstSlot = pak.addInstancePool(meshes[p], perPool);
        pools_[p].bodyOfSlot.assign(perPool, -1);
    }
    bodies_.reserve(perPool * 2);
    FX_INFO("physics: %u static shapes, pools of %u boxes + %u spheres, %u workers (%.1f ms)", staticShapes_, perPool,
            perPool, def.workerCount, double(SDL_GetTicksNS() - t0) * 1e-6);
    return true;
}

// A 3x4 world matrix as a rotation and a signed scale (a mirror flips x). False: degenerate.
static bool splitWorld(const float world[3][4], b3Quat& rotation, float s[3])
{
    float3 axis[3];
    for (int c = 0; c < 3; ++c) {
        axis[c] = {world[0][c], world[1][c], world[2][c]};
        s[c] = length(axis[c]);
    }
    if (s[0] <= 0.0f || s[1] <= 0.0f || s[2] <= 0.0f)
        return false;
    if (dot(axis[0], cross(axis[1], axis[2])) < 0.0f)
        s[0] = -s[0];
    b3Matrix3 rot = {v3(axis[0] * (1.0f / s[0])), v3(axis[1] * (1.0f / s[1])), v3(axis[2] * (1.0f / s[2]))};
    rotation = b3MakeQuatFromMatrix(&rot);
    return true;
}

b3BodyId PhysicsWorld::createStaticBody(uint32_t mesh, const float world[3][4], void* owner)
{
    ++staticRevision_;  // (rollback: what was kept has no such body)
    dropKept();
    const b3MeshData* collider = pak_->staticCollider(mesh);
    const uint32_t* builtins = pak_->header().builtinMeshes;
    const bool cube = mesh == builtins[uint32_t(fpak::Builtin::Cube)], sphere = mesh == builtins[uint32_t(fpak::Builtin::Sphere)];
    if (!collider && !cube && !sphere)
        return {};
    float s[3];
    b3BodyDef bd = b3DefaultBodyDef();
    if (!splitWorld(world, bd.rotation, s))
        return {};
    bd.type = b3_staticBody;
    bd.position = {world[0][3], world[1][3], world[2][3]};
    const b3BodyId body = b3CreateBody(world_, &bd);
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.filter.categoryBits = kLayer0;
    shapeDef.baseMaterial.friction = 0.6f;
    shapeDef.userData = owner ? owner : ownerData(ContactOwner::Ground, 0);
    if (collider) {
        b3CreateMeshShape(body, &shapeDef, collider, {s[0], s[1], s[2]});
    } else if (cube) {  // the built-in unit cube and sphere have no triangle collider: their shapes
        const b3BoxHull box = b3MakeBoxHull(0.5f * std::fabs(s[0]), 0.5f * s[1], 0.5f * s[2]);
        b3CreateHullShape(body, &shapeDef, &box.base);
    } else {
        const b3Sphere ball = {{0.0f, 0.0f, 0.0f}, 0.5f * std::max({std::fabs(s[0]), s[1], s[2]})};
        b3CreateSphereShape(body, &shapeDef, &ball);
    }
    return body;
}

void PhysicsWorld::syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed)
{
    FX_ZONE_CAT("PhysicsWorld::syncScatter", Physics);
    if (!valid() || (added.empty() && removed.empty()))
        return;
    auto drop = [&](uint64_t cell) {
        const auto it = scatterCells_.find(cell);
        if (it == scatterCells_.end())
            return;
        for (b3BodyId b : it->second)
            if (b3Body_IsValid(b))
                b3DestroyBody(b);
        scatterCells_.erase(it);
    };
    ++staticRevision_;
    dropKept();
    for (uint64_t cell : removed)
        drop(cell);
    for (const ScatterCellBodies& c : added) {
        drop(c.cell);
        std::vector<b3BodyId>& bodies = scatterCells_[c.cell];
        for (const ScatterBody& b : c.bodies)
            if (const b3BodyId id = createStaticBody(b.mesh, b.world); b3Body_IsValid(id))
                bodies.push_back(id);
    }
}

void PhysicsWorld::dropTerrainTile(TerrainTile& t)
{
    if (b3Body_IsValid(t.body)) {
        ++staticRevision_;
        dropKept();  // (kept states point at its height field)
    }
    if (b3Body_IsValid(t.body))
        b3DestroyBody(t.body);  // before its height field: the shape points into it
    if (t.data)
        b3DestroyHeightField(t.data);
    t = {};
}

void PhysicsWorld::syncTerrain(const Terrain* terrain, float3 focus, float radius, bool rebuild)
{
    syncTerrain(terrain, std::span<const float3>(&focus, 1), radius, rebuild);
}

void PhysicsWorld::syncTerrain(const Terrain* terrain, std::span<const float3> foci, float radius, bool rebuild)
{
    FX_ZONE_CAT("PhysicsWorld::syncTerrain", Physics);
    if (!valid())
        return;
    if (terrain != terrain_ || !terrain || !terrain->valid() ||
        terrainTiles_.size() != size_t(terrain->tilesPerSide()) * terrain->tilesPerSide()) {
        for (TerrainTile& t : terrainTiles_)
            dropTerrainTile(t);
        terrainTiles_.clear();
        terrain_ = terrain && terrain->valid() ? terrain : nullptr;
        if (!terrain_)
            return;
        terrainTiles_.resize(size_t(terrain_->tilesPerSide()) * terrain_->tilesPerSide());
        terrainRevision_ = terrain_->revision();
        killY_ = std::min(killY_, terrain_->settings().heightMin - 20.0f);
    }
    // Tiles whose shape changed (heights or holes; paint doesn't matter) go stale. An edit on a
    // shared edge touches both tiles.
    std::vector<TerrainChange> changed;
    const bool all = !terrain_->changesSince(terrainRevision_, changed);
    terrainRevision_ = terrain_->revision();
    const uint32_t T = terrain_->tilesPerSide();
    for (const TerrainChange& c : changed) {
        if (!(c.kinds & (terrain::kChangeHeights | terrain::kChangeHoles)))
            continue;
        const TerrainRect& r = c.rect;
        const uint32_t tx0 = r.x0 > 0 ? uint32_t(r.x0 - 1) / terrain::kTileQuads : 0;
        const uint32_t tz0 = r.z0 > 0 ? uint32_t(r.z0 - 1) / terrain::kTileQuads : 0;
        for (uint32_t tz = tz0; tz <= std::min(uint32_t(r.z1) / terrain::kTileQuads, T - 1); ++tz)
            for (uint32_t tx = tx0; tx <= std::min(uint32_t(r.x1) / terrain::kTileQuads, T - 1); ++tx)
                terrainTiles_[tz * T + tx].stale = true;
    }
    const TerrainSettings& s = terrain_->settings();
    const float tileSize = float(terrain::kTileQuads) * s.spacing;
    std::vector<float> heights;
    std::vector<uint32_t> materials;
    std::vector<uint8_t> cells(size_t(terrain::kTileQuads) * terrain::kTileQuads);
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.filter.categoryBits = kLayer0;
    shapeDef.baseMaterial.friction = 0.6f;
    // Its layers as surface materials (user material: the layer + 1, which contacts report).
    b3SurfaceMaterial layerMaterials[terrain::kMaxLayers];
    for (uint32_t l = 0; l < terrain::kMaxLayers; ++l) {
        layerMaterials[l] = shapeDef.baseMaterial;
        layerMaterials[l].userMaterialId = l + 1;
    }
    shapeDef.materials = layerMaterials;
    shapeDef.materialCount = int(terrain::kMaxLayers);
    shapeDef.userData = ownerData(ContactOwner::Ground, 0);
    for (uint32_t tz = 0; tz < T; ++tz)
        for (uint32_t tx = 0; tx < T; ++tx) {
            TerrainTile& t = terrainTiles_[tz * T + tx];
            t.stale = t.stale || all;
            const float x0 = s.originX + float(tx) * tileSize, z0 = s.originZ + float(tz) * tileSize;
            float d = FLT_MAX;
            for (const float3& focus : foci) {
                const float dx = std::max({x0 - focus.x, 0.0f, focus.x - (x0 + tileSize)});
                const float dz = std::max({z0 - focus.z, 0.0f, focus.z - (z0 + tileSize)});
                d = std::min(d, std::sqrt(dx * dx + dz * dz));
            }
            const bool wanted = d <= radius && terrain_->tileResident(tx, tz);
            if (!wanted) {
                if (t.data)
                    dropTerrainTile(t);
                continue;
            }
            if (t.data && !(t.stale && rebuild))
                continue;
            dropTerrainTile(t);
            // The tile's samples, quantized like the terrain itself (same range): identical heights.
            const TerrainRect rect = {int32_t(tx * terrain::kTileQuads), int32_t(tz * terrain::kTileQuads),
                                      int32_t((tx + 1) * terrain::kTileQuads), int32_t((tz + 1) * terrain::kTileQuads)};
            terrain_->read(rect, heights);
            // Holes: a cell is cut where its minimum corner's sample says so.
            terrain_->readMaterials(rect, materials);
            // Each cell's layer (the stronger of its minimum corner's two), or a hole.
            for (uint32_t z = 0; z < terrain::kTileQuads; ++z)
                for (uint32_t x = 0; x < terrain::kTileQuads; ++x) {
                    const uint32_t m = materials[z * terrain::kTileSamples + x];
                    const uint32_t layer = terrain::materialWeightB(m) > 127 ? terrain::materialB(m) : terrain::materialA(m);
                    cells[z * terrain::kTileQuads + x] = (m & terrain::kHole) ? B3_HEIGHT_FIELD_HOLE
                                                                                 : uint8_t(std::min(layer, terrain::kMaxLayers - 1));
                }
            b3HeightFieldDef def = {};
            def.heights = heights.data();
            def.materialIndices = cells.data();
            def.scale = {s.spacing, 1.0f, s.spacing};
            def.countX = int(terrain::kTileSamples);
            def.countZ = int(terrain::kTileSamples);
            def.globalMinimumHeight = s.heightMin;
            def.globalMaximumHeight = s.heightMax;
            t.data = b3CreateHeightField(&def);
            b3BodyDef bd = b3DefaultBodyDef();
            bd.type = b3_staticBody;
            bd.position = {x0, 0.0f, z0};
            ++staticRevision_;  // (rollback: what was kept has no such tile)
            dropKept();
            t.body = b3CreateBody(world_, &bd);
            b3CreateHeightFieldShape(t.body, &shapeDef, t.data);
            t.stale = false;
        }
}

void PhysicsWorld::addEntityBody(flecs::entity e, const PhysicsBody* body, const Collider& collider, int32_t reuse)
{
    const WorldTransform* t = e.try_get<WorldTransform>();
    if (!t)
        return;
    const float(*world)[4] = rows3x4(t->rows);
    EntityBody eb;
    eb.entity = e;
    float s[3];
    b3BodyDef bd = b3DefaultBodyDef();
    if (!splitWorld(world, bd.rotation, s))
        return;
    eb.scale = {s[0], s[1], s[2]};
    eb.dynamic = body && !(body->flags & 1u);
    eb.kinematic = body && (body->flags & 1u);
    bd.type = eb.dynamic ? b3_dynamicBody : eb.kinematic ? b3_kinematicBody : b3_staticBody;
    bd.position = {world[0][3], world[1][3], world[2][3]};
    if (body) {
        bd.linearDamping = std::max(body->linearDamping, 0.0f);
        bd.angularDamping = std::max(body->angularDamping, 0.0f);
        bd.gravityScale = body->gravityScale;
    }
    const uint32_t index = reuse >= 0 ? uint32_t(reuse) : uint32_t(entityBodies_.size());
    bd.userData = reinterpret_cast<void*>(kEntityBodyTag | uintptr_t(index));
    eb.id = b3CreateBody(world_, &bd);
    // Its shape, in the entity's space: scaled with it (its Collider's, or the shapes scripts gave it).
    bool shaped = false;
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 1.0f;  // (the body's mass is set below)
    sd.baseMaterial.friction = std::max(collider.friction, 0.0f);
    sd.baseMaterial.restitution = std::clamp(collider.bounciness, 0.0f, 1.0f);
    sd.userData = ownerData(ContactOwner::Entity, e.id());
    sd.enableHitEvents = sd.enableContactEvents = bd.type != b3_staticBody;  // (sounds: what moves reports)
    sd.filter = layerFilter(collider.layer, collider.ignore);
    sd.isSensor = (collider.flags & 1u) != 0;  // a trigger: reports, collides with nothing
    sd.enableSensorEvents = true;              // (triggers see it)
    if (const auto ss = scriptShapes_.find(e.id()); ss != scriptShapes_.end()) {
        shaped = addScriptShapes(eb.id, sd, ss->second, s, eb.half, eb.offset);
    } else if (e.has<Collider>()) {
        const float as[3] = {std::fabs(s[0]), std::fabs(s[1]), std::fabs(s[2])};
        const b3Vec3 center = {collider.center[0] * s[0], collider.center[1] * s[1], collider.center[2] * s[2]};
        const MeshInstance* mi = e.try_get<MeshInstance>();
        const uint32_t mesh = mi ? pak_->instances()[mi->gpuSlot].mesh : ~0u;
        const b3MeshData* triangles = collider.shape == 2 && mi ? pak_->staticCollider(mesh) : nullptr;
        if (collider.shape == 2 && mi && pak_->liveSlots().ownsMesh(mesh)) {  // (rebuilt when it changes)
            eb.liveMesh = mesh;
            eb.liveRevision = pak_->liveRevision(mesh);
        }
        const uint32_t* builtins = pak_->header().builtinMeshes;
        uint32_t shape = collider.shape;
        float size[3] = {collider.size[0], collider.size[1], collider.size[2]}, offset[3] = {center.x, center.y, center.z};
        // A mesh without a triangle collider, or on a moving body (Box3D moves no triangle meshes,
        // and its hull builder asserts on some meshes): its bounds (the built-in sphere: a ball).
        if (shape == 2 && (!triangles || bd.type != b3_staticBody)) {
            uint32_t meshCount = 0;
            const fpak::MeshGpu* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
            shape = mi && mesh == builtins[uint32_t(fpak::Builtin::Sphere)] ? 1u : 0u;
            for (int a = 0; a < 3; ++a) {
                const float half = mesh < meshCount ? meshes[mesh].posScale[a] * 65535.0f * 0.5f : 0.5f;
                size[a] = shape == 1 ? half : half * 2.0f;
                offset[a] = (mesh < meshCount ? meshes[mesh].posOffset[a] + half : 0.0f) * s[a];
            }
        }
        if (shape == 2) {
            b3CreateMeshShape(eb.id, &sd, triangles, {s[0], s[1], s[2]});
            shaped = true;
        } else if (shape == 3) {  // a capsule along Y: size[0] its radius, size[1] its height
            const float radius = std::max(size[0], 1e-3f) * std::max(as[0], as[2]);
            const float half = std::max(0.5f * size[1] * as[1] - radius, 0.0f);
            const b3Capsule capsule = {{offset[0], offset[1] - half, offset[2]}, {offset[0], offset[1] + half, offset[2]}, radius};
            b3CreateCapsuleShape(eb.id, &sd, &capsule);
            shaped = true;
            eb.half = {radius, half + radius, radius};
        } else if (shape == 1) {
            const b3Sphere ball = {{offset[0], offset[1], offset[2]}, std::max(size[0], 1e-3f) * std::max({as[0], as[1], as[2]})};
            b3CreateSphereShape(eb.id, &sd, &ball);
            shaped = true;
            eb.sphere = true;
            eb.half = {ball.radius, ball.radius, ball.radius};
        } else {
            const b3BoxHull box = b3MakeOffsetBoxHull(std::max(0.5f * size[0] * as[0], 1e-3f), std::max(0.5f * size[1] * as[1], 1e-3f),
                                                      std::max(0.5f * size[2] * as[2], 1e-3f), {offset[0], offset[1], offset[2]});
            b3CreateHullShape(eb.id, &sd, &box.base);
            shaped = true;
            eb.half = {std::max(0.5f * size[0] * as[0], 1e-3f), std::max(0.5f * size[1] * as[1], 1e-3f), std::max(0.5f * size[2] * as[2], 1e-3f)};
        }
        eb.offset = {offset[0], offset[1], offset[2]};
        eb.floats = eb.dynamic && (shape == 0 || shape == 1) && !sd.isSensor;  // (water lifts its box or ball)
    }
    if (eb.dynamic) {  // its mass, as set (the shape's distribution kept; none: a 1 m cube's)
        const float mass = std::max(body->mass, 1e-4f);
        b3MassData md = b3Body_GetMassData(eb.id);
        if (!shaped || md.mass <= 0.0f) {
            md = {};
            md.mass = 1.0f;
            md.inertia = {{1.0f / 6.0f, 0, 0}, {0, 1.0f / 6.0f, 0}, {0, 0, 1.0f / 6.0f}};
        }
        const float k = mass / md.mass;
        md.mass = mass;
        md.inertia.cx = b3MulSV(k, md.inertia.cx);
        md.inertia.cy = b3MulSV(k, md.inertia.cy);
        md.inertia.cz = b3MulSV(k, md.inertia.cz);
        b3Body_SetMassData(eb.id, md);
    }
    eb.prev = eb.curr = eb.written = b3Body_GetTransform(eb.id);
    // A cooked instance's own static collider steps aside (this one stands for it).
    if (const MeshInstance* mi = e.try_get<MeshInstance>())
        if (const auto it = cookedStatic_.find(mi->gpuSlot); it != cookedStatic_.end() && b3Body_IsEnabled(it->second)) {
            b3Body_Disable(it->second);
            setAside_.push_back(it->second);
        }
    if (body) {  // the scripts' body API finds it through the entity
        PhysicsBody b = *body;
        static_assert(sizeof(b3BodyId) == sizeof(b.bodyId));
        std::memcpy(b.bodyId, &eb.id, sizeof(eb.id));
        writingBodyId_ = true;
        e.set<PhysicsBody>(b);
        writingBodyId_ = false;
    }
    entityBodyOf_[e.id()] = index;
    if (reuse >= 0)
        entityBodies_[index] = eb;
    else
        entityBodies_.push_back(eb);
}

void PhysicsWorld::buildEntityBodies()
{
    FX_ZONE_CAT("PhysicsWorld::buildEntityBodies", Physics);
    if (!valid())
        return;
    clearEntityBodies();
    entityBodiesBuilt_ = true;
    scene_->propagate();  // (children's world transforms as they are now)
    // In entity order: the same bodies in the same order every run.
    std::vector<flecs::entity_t> ids;
    scene_->world().each([&](flecs::entity e, const PhysicsBody&) { ids.push_back(e.id()); });
    scene_->world().each([&](flecs::entity e, const Collider&) {
        if (!e.has<PhysicsBody>())
            ids.push_back(e.id());
    });
    std::sort(ids.begin(), ids.end());
    for (const flecs::entity_t id : ids) {
        const flecs::entity e(scene_->world(), id);
        const Collider* c = e.try_get<Collider>();
        addEntityBody(e, e.try_get<PhysicsBody>(), c ? *c : Collider{});
    }
    if (!entityBodies_.empty())
        FX_INFO("physics: %zu bodies of the scene's entities (%zu cooked colliders set aside)", entityBodies_.size(), setAside_.size());
}

void PhysicsWorld::addEntityBodies(const std::vector<flecs::entity>& entities)
{
    if (!valid() || !entityBodiesBuilt_)
        return;
    for (const flecs::entity e : entities) {
        if (!e.is_alive() || (!e.has<PhysicsBody>() && !e.has<Collider>()))
            continue;
        const Collider* c = e.try_get<Collider>();
        addEntityBody(e, e.try_get<PhysicsBody>(), c ? *c : Collider{});
    }
}

void PhysicsWorld::clearEntityBodies()
{
    entityBodiesBuilt_ = false;
    motions_.clear();  // (the game stopped: nothing slides or rolls any more)
    impacts_.clear();
    waterEntries_.clear();
    waterStirs_.clear();
    wetStep_.clear();
    for (EntityBody& eb : entityBodies_) {
        if (b3Body_IsValid(eb.id))
            b3DestroyBody(eb.id);
        if (eb.entity.is_alive())
            if (const PhysicsBody* b = eb.entity.try_get<PhysicsBody>()) {
                PhysicsBody cleared = *b;
                std::memset(cleared.bodyId, 0, sizeof(cleared.bodyId));
                writingBodyId_ = true;
                eb.entity.set<PhysicsBody>(cleared);
                writingBodyId_ = false;
            }
    }
    entityBodies_.clear();
    entityBodyOf_.clear();
    dirtyBodies_.clear();
    scriptShapes_.clear();  // (X4) the game's
    triggers_.clear();
    clearJoints();
    for (const b3BodyId id : setAside_)
        if (b3Body_IsValid(id))
            b3Body_Enable(id);
    setAside_.clear();
}

void PhysicsWorld::writeEntityBodies(float alpha)
{
    for (EntityBody& eb : entityBodies_) {
        if (!eb.dynamic || !eb.entity.is_alive())
            continue;
        b3WorldTransform t = eb.curr;
        if (alpha < 1.0f) {
            t.p = b3Lerp(eb.prev.p, eb.curr.p, alpha);
            t.q = b3NLerp(eb.prev.q, eb.curr.q, alpha);
        }
        if (std::memcmp(&t, &eb.written, sizeof(t)) == 0)
            continue;  // at rest (or paused): the entity keeps its transform
        eb.written = t;
        const b3Matrix3 m = b3MakeMatrixFromQuat(t.q);
        const float3 cols[3] = {f3(m.cx) * eb.scale.x, f3(m.cy) * eb.scale.y, f3(m.cz) * eb.scale.z};
        float world[3][4];
        for (int row = 0; row < 3; ++row) {
            world[row][0] = (&cols[0].x)[row];
            world[row][1] = (&cols[1].x)[row];
            world[row][2] = (&cols[2].x)[row];
        }
        world[0][3] = float(t.p.x);
        world[1][3] = float(t.p.y);
        world[2][3] = float(t.p.z);
        scene_->setWorld(eb.entity, world);  // (a child's LocalTransform follows)
    }
}

void PhysicsWorld::shutdown()
{
    dropKept();
    kept_.clear();
    stepHashes_.clear();
    for (TerrainTile& t : terrainTiles_)
        dropTerrainTile(t);
    terrainTiles_.clear();
    terrain_ = nullptr;
    scatterCells_.clear();  // (destroyed with the world)
    entityBodies_.clear();  // (destroyed with the world)
    entityBodyOf_.clear();
    dirtyBodies_.clear();
    joints_.clear();
    if (bodyObserver_)
        bodyObserver_.destruct();
    if (colliderObserver_)
        colliderObserver_.destruct();
    setAside_.clear();
    cookedStatic_.clear();
    if (b3World_IsValid(world_))
        b3DestroyWorld(world_);
    world_ = {};
    bodies_.clear();
    active_.clear();
    staticBodies_.clear();
}

uint64_t PhysicsWorld::spawn(Shape shape, float3 position, float3 size, float3 velocity, float3 angularVelocity,
                             uint32_t tint)
{
    Pool& pool = pools_[shape == Shape::Box ? 0 : 1];
    const uint32_t local = pool.next;
    pool.next = (pool.next + 1) % pool.capacity;

    // Recycle the slot's previous body (oldest first): its instance is overwritten below.
    uint32_t index;
    if (pool.bodyOfSlot[local] >= 0) {
        index = uint32_t(pool.bodyOfSlot[local]);
        Body& old = bodies_[index];
        if (old.live) {
            old.live = false;
            b3DestroyBody(old.id);
            if (old.entity.is_alive())
                old.entity.destruct();
        }
    } else {
        index = uint32_t(bodies_.size());
        bodies_.emplace_back();
    }
    pool.bodyOfSlot[local] = int32_t(index);

    Body& b = bodies_[index];
    if (b.active)  // still in the active list: it keeps its entry
        b.active = false, active_.erase(std::find(active_.begin(), active_.end(), index));
    b = Body{};
    b.slot = pool.firstSlot + local;
    b.shape = shape;
    b.tint = tint;
    b.live = true;
    b.serial = spawnCount_++;

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = v3(position);
    bd.linearVelocity = v3(velocity);
    bd.angularVelocity = v3(angularVelocity);
    bd.userData = reinterpret_cast<void*>(uintptr_t(index));
    bd.angularDamping = 0.1f;  // with rolling resistance below: piles come to rest and sleep
    b.id = b3CreateBody(world_, &bd);
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 500.0f;  // kg/m^3, wood-like
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.15f;
    sd.baseMaterial.rollingResistance = 0.05f;
    sd.enableHitEvents = sd.enableContactEvents = true;
    sd.enableSensorEvents = true;  // (triggers see it)
    sd.filter.categoryBits = kLayer0;
    b3ShapeId shapeId;
    if (shape == Shape::Box) {
        const b3BoxHull box = b3MakeBoxHull(size.x, size.y, size.z);
        shapeId = b3CreateHullShape(b.id, &sd, &box.base);
        b.scale = size * 2.0f;
    } else {
        const b3Sphere sphere = {{0.0f, 0.0f, 0.0f}, size.x};
        shapeId = b3CreateSphereShape(b.id, &sd, &sphere);
        b.scale = {size.x * 2.0f, size.x * 2.0f, size.x * 2.0f};
    }
    b.prev = b.curr = b3Body_GetTransform(b.id);

    WorldTransform wt = {};
    const fpak::InstanceGpu r = row(b, b.curr);
    std::memcpy(wt.rows, r.world, sizeof(wt.rows));
    // ECS mirror (transform updated per physics step); RigidBody.bodyId holds the b3BodyId bits.
    RigidBody rb = {};
    static_assert(sizeof(b3BodyId) == sizeof(rb.bodyId));
    std::memcpy(rb.bodyId, &b.id, sizeof(b.id));
    rb.index = index;
    b.entity = scene_->world().entity().set<WorldTransform>(wt).set<MeshInstance>({pool.mesh, b.slot}).set<RigidBody>(rb);
    b3Shape_SetUserData(shapeId, ownerData(ContactOwner::Entity, b.entity.id()));
    activate(index);
    return b.entity.id();
}

void PhysicsWorld::removeSpawnedSince(uint64_t count)
{
    for (uint32_t index = 0; index < bodies_.size(); ++index) {
        Body& b = bodies_[index];
        if (!b.live || b.serial < count)
            continue;
        b3DestroyBody(b.id);
        if (b.entity.is_alive())
            b.entity.destruct();
        b.live = false;
        removed_.push_back(index);
    }
}

int32_t PhysicsWorld::poolBodyOf(uint64_t entity) const
{
    const flecs::world& world = scene_->world();
    if (!entity || !world.is_alive(entity))
        return -1;
    const RigidBody* rb = flecs::entity(world, entity).try_get<RigidBody>();
    if (!rb || rb->index >= bodies_.size() || !bodies_[rb->index].live || bodies_[rb->index].entity.id() != entity)
        return -1;
    return int32_t(rb->index);
}

bool PhysicsWorld::removeBody(uint64_t entity)
{
    const int32_t index = poolBodyOf(entity);
    if (index < 0)
        return false;
    Body& b = bodies_[uint32_t(index)];
    b.live = false;
    b3DestroyBody(b.id);
    removed_.push_back(uint32_t(index));
    return true;
}

bool PhysicsWorld::setBodyEnabled(uint64_t entity, bool enabled)
{
    const int32_t index = poolBodyOf(entity);
    if (index < 0)
        return false;
    Body& b = bodies_[uint32_t(index)];
    if (b.hidden == !enabled)
        return true;
    b.hidden = !enabled;
    if (enabled)
        b3Body_Enable(b.id);
    else
        b3Body_Disable(b.id);
    b.fresh = true;  // (streamed once more: hidden, or shown again)
    activate(uint32_t(index));
    return true;
}

void PhysicsWorld::dropPile(float3 center, uint32_t count, uint32_t seed)
{
    const uint32_t side = std::max(1u, uint32_t(std::ceil(std::sqrt(float(count) / 4.0f))));
    const float spacing = 0.8f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t layer = i / (side * side), cell = i % (side * side);
        const float3 p = {center.x + (float(cell % side) - 0.5f * float(side - 1)) * spacing,
                          center.y + float(layer) * spacing,
                          center.z + (float(cell / side) - 0.5f * float(side - 1)) * spacing};
        const uint32_t k = seed * 7919u + i;
        const float3 jitter = {(unit(k * 3 + 0) - 0.5f) * 0.1f, 0.0f, (unit(k * 3 + 1) - 0.5f) * 0.1f};
        const float3 spin = {unit(k * 5 + 2) - 0.5f, unit(k * 5 + 3) - 0.5f, unit(k * 5 + 4) - 0.5f};
        const float s = 0.18f + 0.14f * unit(k * 7 + 5);
        if (hash(k) & 1)
            spawn(Shape::Box, p + jitter, {s, s * (0.7f + 0.6f * unit(k * 11)), s}, {0, 0, 0}, spin * 2.0f,
                  paletteTint(hash(k + 1)));
        else
            spawn(Shape::Sphere, p + jitter, {s * 1.1f, 0, 0}, {0, 0, 0}, spin, paletteTint(hash(k + 1)));
    }
}

void PhysicsWorld::fire(float3 origin, float3 direction)
{
    static uint32_t shots = 0;
    ++shots;
    spawn(Shape::Sphere, origin + direction * 0.8f, {0.25f, 0, 0}, direction * 25.0f, {0, 0, 0}, paletteTint(shots));
}

void PhysicsWorld::activate(uint32_t index)
{
    if (!bodies_[index].active) {
        bodies_[index].active = true;
        active_.push_back(index);
    }
}

// Buoyancy: each body split into cells (boxes: 2^3, 3^3 when longer than 2 m; spheres: their
// submerged cap), each pushed up by the water it displaces where it is, at its own centre (so a
// tilted box rights itself and floats at its density's depth). Drag on each cell's velocity
// relative to the flow (quadratic, plus a little linear so bodies settle), never more than
// stops it in one step. Bodies in moving water stay awake. In body order: deterministic.
static b3Pos toPos(float3 p)
{
    b3Pos r;  // (float or double, as Box3D is built)
    r.x = p.x;
    r.y = p.y;
    r.z = p.z;
    return r;
}

void PhysicsWorld::applyWater()
{
    FX_ZONE_CAT("PhysicsWorld::applyWater", Physics);
    waterStirs_.clear();
    for (const Body& b : bodies_)
        if (b.live)
            floatBody(b.id, b.shape == Shape::Sphere, b.scale * 0.5f, {0.0f, 0.0f, 0.0f});
    for (const EntityBody& eb : entityBodies_)
        if (eb.floats && b3Body_IsEnabled(eb.id))
            floatBody(eb.id, eb.sphere, eb.half, eb.offset);
}

void PhysicsWorld::floatBody(b3BodyId id, bool sphere, float3 half, float3 offset)
{
    constexpr float kG = 9.81f;
    const b3WorldTransform t = b3Body_GetTransform(id);
    const b3Vec3 o0 = b3RotateVector(t.q, {offset.x, offset.y, offset.z});
    const float3 c = {float(t.p.x) + o0.x, float(t.p.y) + o0.y, float(t.p.z) + o0.z};  // the shape's centre
    const float reach = 2.0f * std::max(half.x, std::max(half.y, half.z));  // (full extent)
    const WaterSample centre = water_->sample(c);
    if (centre.body < 0 || centre.depth < -reach)
        return;  // not in water (or well above it)
    const WaterSystem::Body& wb = *water_->body(uint32_t(centre.body));
    if (!wb.desc.buoyancy)
        return;
    const WaterMaterial& m = wb.material;
    const float mass = b3Body_GetMass(id);
    const bool moving = wb.waveCount > 0 || m.flowSpeed != 0.0f;
    if (moving && !b3Body_IsAwake(id))
        b3Body_SetAwake(id, true);
    float own = 1.0f;  // the share of the water's push the body itself takes (see below)
    auto dragAt = [&](float3 p, float share, float volume) {
        const b3Vec3 v = b3Body_GetWorldPointVelocity(id, toPos(p));
        const WaterSample s = water_->sample(p);
        const float3 rel = {v.x - s.flow.x * m.current, v.y, v.z - s.flow.z * m.current};
        const float speed = length(rel);
        if (speed < 1e-4f)
            return;
        const float area = std::cbrt(volume * volume);
        float f = m.drag * share * m.density * (0.5f * area * speed + 2.0f * volume) * speed;
        f = std::min(f, mass * share * speed / kStep) * own;  // (no reversal in one step)
        const float3 force = rel * (-f / speed);
        b3Body_ApplyForce(id, {force.x, force.y, force.z}, toPos(p), false);
    };
    // The lift at points (the ball's centre, or each cell's), and how stiff it is: newtons per
    // metre the body sinks at the waterline. Each point's depth is taken where it will be at the
    // step's end: a spring pushing from where the body was adds energy at every bounce (a step
    // is long next to a light body's bob), one from where it is going takes it out.
    auto ahead = [&](float3 p) {
        const b3Vec3 v = b3Body_GetWorldPointVelocity(id, toPos(p));
        return float3{p.x + v.x * kStep, p.y + v.y * kStep, p.z + v.z * kStep};
    };
    float3 at[27];
    float up[27], shares[27];
    uint32_t count = 0;
    float volume = 0.0f, stiffness = 0.0f, waterline = 0.0f;  // (waterline: the area cut by the surface, m^2)
    if (sphere) {
        const float r = half.x;
        const float3 q = ahead(c);
        const float h = std::clamp(water_->sample(q).height - (q.y - r), 0.0f, 2.0f * r);
        const float submerged = kPi * h * h * (3.0f * r - h) / 3.0f;
        volume = 4.0f / 3.0f * kPi * r * r * r;
        if (submerged <= 0.0f)
            return;
        at[0] = c;
        up[0] = m.density * kG * submerged;
        shares[0] = submerged / volume;
        count = 1;
        stiffness = m.density * kG * kPi * r * r;
        waterline = h < 2.0f * r ? kPi * h * (2.0f * r - h) : 0.0f;
    } else {
        const uint32_t n = reach > 2.0f ? 3u : 2u;
        volume = 8.0f * half.x * half.y * half.z / float(n * n * n);  // (a cell's)
        // The cell's vertical half extent in the world (its box turned by the body).
        const b3Vec3 ax = b3RotateVector(t.q, {half.x / float(n), 0.0f, 0.0f});
        const b3Vec3 ay = b3RotateVector(t.q, {0.0f, half.y / float(n), 0.0f});
        const b3Vec3 az = b3RotateVector(t.q, {0.0f, 0.0f, half.z / float(n)});
        const float cellHalfY = std::max(std::fabs(ax.y) + std::fabs(ay.y) + std::fabs(az.y), 1e-3f);
        for (uint32_t i = 0; i < n * n * n; ++i) {
            const float3 local = {half.x * ((float(i % n) + 0.5f) * 2.0f / float(n) - 1.0f),
                                  half.y * ((float((i / n) % n) + 0.5f) * 2.0f / float(n) - 1.0f),
                                  half.z * ((float(i / (n * n)) + 0.5f) * 2.0f / float(n) - 1.0f)};
            const b3Vec3 o = b3RotateVector(t.q, {local.x, local.y, local.z});
            const float3 p = {c.x + o.x, c.y + o.y, c.z + o.z};
            const float3 q = ahead(p);
            const WaterSample s = water_->sample(q);
            if (s.body < 0)
                continue;
            const float share = std::clamp((s.height - (q.y - cellHalfY)) / (2.0f * cellHalfY), 0.0f, 1.0f);
            if (share <= 0.0f)
                continue;
            if (share < 1.0f)
                waterline += volume / (2.0f * cellHalfY);
            at[count] = p;
            up[count] = m.density * kG * volume * share;
            shares[count++] = share / float(n * n * n);
        }
        if (count == 0)
            return;
        stiffness = m.density * kG * volume * float(n * n) / (2.0f * cellHalfY);  // (one cell per column)
    }
    // Added mass: what the body pushes also moves the water around it (half its displaced volume
    // for a ball), so it answers the water as if that much heavier: a light body plunges by how
    // fast it came in, comes back up and bobs instead of being stopped dead. Never lighter than
    // a step resolves (a beach ball, lifted by thousands of times its weight, would ring). The
    // water's push (lift, drag, damping) is scaled to the body's own share of that mass, about
    // its weight: the depth it floats at is kept.
    float lift = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
        lift += up[i];
    const float added = 0.5f * lift / kG;
    const float inertia = std::max(mass + added, stiffness * kStep * kStep);
    own = mass / inertia;
    const float weight = mass * kG * b3Body_GetGravityScale(id);
    const float k = own + (1.0f - own) * std::max(weight, 0.0f) / lift;
    for (uint32_t i = 0; i < count; ++i) {
        b3Body_ApplyForce(id, {0.0f, up[i] * k, 0.0f}, toPos(at[i]), false);
        dragAt(at[i], shares[i], volume);
    }
    // Heave: moving up or down through the surface makes waves that carry energy off: damped by
    // how much the waterline cuts, at 0.15 of critical (drag 1), so a drop bobs two or three
    // times, each smaller.
    const b3Vec3 v = b3Body_GetLinearVelocity(id);
    // Sounds: falling in (in the water this step and not the last, moving down), and stirring it
    // while at the surface.
    {
        const uint64_t key = bodyKey(id);
        const auto it = wetStep_.find(key);
        const bool was = it != wetStep_.end() && it->second + 1 >= stepIndex_;
        const float size = sphere ? half.x : std::cbrt(half.x * half.y * half.z);
        const float3 surface = {c.x, centre.height, c.z};
        if (!was && v.y < -0.5f)
            waterEntries_.push_back({surface, -v.y, size, mass, centre.body});
        wetStep_[key] = stepIndex_;
        if (waterline > 0.0f) {
            const float3 rel = {v.x - centre.flow.x * m.current, v.y, v.z - centre.flow.z * m.current};
            waterStirs_.push_back({key, surface, length(rel), std::sqrt(waterline / kPi), centre.body});
        }
    }
    if (waterline > 0.0f) {
        const float c = std::min(0.3f * m.drag * std::sqrt(m.density * kG * waterline * inertia), inertia / kStep);
        b3Body_ApplyForceToCenter(id, {0.0f, -v.y * c * own, 0.0f}, false);
    }
    // Its wake (ripples): the water it pushes through the surface in a step (waterline area x
    // vertical speed) raised around it, over twice its width (a whole plunge: a few cm to a few
    // tens), plus a little for sliding along the surface. Each bob makes rings again.
    if (m.ripples && waterline > 0.0f) {
        const float radius = 2.0f * std::max(std::sqrt(waterline / kPi), 0.5f * reach);
        const float pushed = waterline * std::min(std::fabs(v.y), 8.0f) * kStep / (kPi * radius * radius);
        const float slide = std::min(std::sqrt(v.x * v.x + v.z * v.z), 8.0f) * 0.004f * reach;
        if (pushed + slide > 2e-4f)
            water_->splash(c.x, c.z, radius, std::min(pushed + slide, 0.05f) * m.rippleStrength);
    }
    if (sphere) {
        const b3Vec3 w = b3Body_GetAngularVelocity(id);
        const float r = half.x, damp = 0.5f * m.drag * shares[0] * mass * r * r;
        b3Body_ApplyTorque(id, {-w.x * damp, -w.y * damp, -w.z * damp}, false);
    }
}

void PhysicsWorld::step()
{
    FX_ZONE_CAT("PhysicsWorld::step", Physics);
    stepBegin();
    stepEnd();
}

void PhysicsWorld::stepBegin()
{
    if (renderPosed_) {  // (the tick sees the simulation's poses, not the render's in-between ones)
        writeEntityBodies(1.0f);
        renderPosed_ = false;
    }
    // Live meshes whose triangles changed: the bodies on them are built again (their new collider).
    if (pak_ && pak_->liveGeneration() != liveGeneration_) {
        liveGeneration_ = pak_->liveGeneration();
        for (const EntityBody& eb : entityBodies_)
            if (eb.liveMesh != ~0u && eb.entity.is_alive() && pak_->liveRevision(eb.liveMesh) != eb.liveRevision)
                dirtyBodies_.push_back(eb.entity.id());
    }
    if (!dirtyBodies_.empty()) {  // (components set or removed since the last step)
        std::vector<flecs::entity_t> dirty;
        dirty.swap(dirtyBodies_);
        std::sort(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        for (const flecs::entity_t id : dirty)
            rebuild(id);
    }
    for (uint32_t i : active_)
        bodies_[i].prev = bodies_[i].curr;  // bodies without a move event stop interpolating
    for (EntityBody& eb : entityBodies_) {
        if (!eb.entity.is_alive() && b3Body_IsValid(eb.id)) {  // (its entity went: a script's, a spawned prefab's)
            b3DestroyBody(eb.id);
            eb.id = b3_nullBodyId;
            eb.dynamic = eb.kinematic = eb.floats = false;
            continue;
        }
        // A disabled entity's body stops with it (a script's Enabled; a scene's entity destroyed while
        // the editor plays), and carries on when it is enabled again.
        if (const bool disabled = !eb.entity.enabled(); disabled != eb.disabled && b3Body_IsValid(eb.id)) {
            eb.disabled = disabled;
            if (disabled)
                b3Body_Disable(eb.id);
            else
                b3Body_Enable(eb.id);
        }
        eb.prev = eb.curr;
        // Kinematic bodies follow their entity (scripts, animation): reaching it over the step.
        if (eb.kinematic && eb.entity.is_alive())
            if (const WorldTransform* t = eb.entity.try_get<WorldTransform>()) {
                b3WorldTransform target;
                float s[3];
                if (!splitWorld(rows3x4(t->rows), target.q, s))
                    continue;
                target.p = toPos({t->rows[3], t->rows[7], t->rows[11]});
                b3Body_SetTargetTransform(eb.id, target, kStep, true);
            }
    }
    if (onStep_)
        onStep_(kStep);  // (scripts' OnFixedUpdate: forces for this step)
}

void PhysicsWorld::stepEnd()
{
    const uint64_t t0 = SDL_GetTicksNS();
    if (water_ && water_->count())
        applyWater();
    g_tasks.count = 0;
    stepImpactsBegin_ = impacts_.size();
    stepTriggersBegin_ = triggers_.size();
    b3World_Step(world_, kStep, kSubSteps);
    ++stepIndex_;
    collectContacts();
    collectTriggers();
    if ((stepIndex_ & 63) == 0)  // (forget bodies out of the water a while: gone, or dry)
        std::erase_if(wetStep_, [&](const auto& kv) { return kv.second + 2 < stepIndex_; });
    const b3BodyEvents events = b3World_GetBodyEvents(world_);
    for (int e = 0; e < events.moveCount; ++e) {
        const b3BodyMoveEvent& m = events.moveEvents[e];
        if (const uintptr_t tag = uintptr_t(m.userData); tag & kEntityBodyTag) {
            EntityBody& eb = entityBodies_[tag & ~kEntityBodyTag];
            eb.curr = m.transform;
            if (eb.dynamic && float(m.transform.p.y) < killY_)
                b3Body_Disable(eb.id);  // fell out of the world: it stays there until Stop
            continue;
        }
        const uint32_t index = uint32_t(uintptr_t(m.userData));
        Body& b = bodies_[index];
        if (!b.live)
            continue;
        if (m.transform.p.y < killY_) {
            // Fell out of the world: remove it; extract() hides the slot.
            b.live = false;
            b3DestroyBody(b.id);
            if (b.entity.is_alive())
                b.entity.destruct();
            removed_.push_back(index);
            continue;
        }
        b.prev = b.curr;  // also for bodies that were inactive before this step
        b.curr = m.transform;
        activate(index);
        // ECS mirror (per step, not interpolated).
        WorldTransform wt = {};
        const fpak::InstanceGpu r = row(b, b.curr);
        std::memcpy(wt.rows, r.world, sizeof(wt.rows));
        b.entity.set<WorldTransform>(wt);
    }
    writeEntityBodies(1.0f);  // (this step's poses, for what runs after it and the next step's scripts)
    stepMs_ = float(double(SDL_GetTicksNS() - t0) * 1e-6);
    if (afterStep_)
        afterStep_();
    if (!kept_.empty()) {  // (rollback: each step's hash, and a step simulated again compared)
        const uint64_t hash = stateHash();
        auto& slot = stepHashes_[stepIndex_ % stepHashes_.size()];
        if (resimulating_ && slot.first == stepIndex_) {
            ++rollbackChecks_;
            if (slot.second != hash)
                ++rollbackMismatches_;
        }
        slot = {stepIndex_, hash};
        keep();  // (the state after the step and its events: where the scripts keep theirs)
    }
}

void PhysicsWorld::setRollbackWindow(uint32_t steps)
{
    dropKept();
    kept_.resize(steps);
    stepHashes_.assign(steps ? steps + 2 : 0, {~0ull, 0});
}

void PhysicsWorld::dropKept()
{
    for (Kept& k : kept_) {
        if (k.world)
            fxDestroyPhysicsSnapshot(world_, k.world);
        k = {};
    }
}

void PhysicsWorld::keep()
{
    const uint64_t t0 = SDL_GetTicksNS();
    Kept& k = kept_[stepIndex_ % kept_.size()];
    if (!k.world)
        k.world = fxCreatePhysicsSnapshot();
    keptBytes_ = size_t(fxCapturePhysics(world_, k.world));
    k.step = stepIndex_;
    k.staticRevision = staticRevision_;
    k.entityBodies = entityBodies_;
    k.entityBodyOf = entityBodyOf_;
    k.bodies = bodies_;
    k.pools[0] = pools_[0];
    k.pools[1] = pools_[1];
    k.active = active_;
    k.removed = removed_;
    k.spawnCount = spawnCount_;
    k.touching = touching_;
    k.wetStep = wetStep_;
    k.joints = joints_;
    keepNs_ += SDL_GetTicksNS() - t0;
    ++keeps_;
}

bool PhysicsWorld::rollbackTo(uint64_t step)
{
    if (kept_.empty() || !valid())
        return false;
    const Kept& k = kept_[step % kept_.size()];
    if (k.step != step || !k.world || k.staticRevision != staticRevision_)
        return false;
    const uint64_t t0 = SDL_GetTicksNS();
    if (!fxRestorePhysics(world_, k.world))
        return false;
    entityBodies_ = k.entityBodies;
    entityBodyOf_ = k.entityBodyOf;
    bodies_ = k.bodies;
    pools_[0] = k.pools[0];
    pools_[1] = k.pools[1];
    active_ = k.active;
    removed_ = k.removed;
    spawnCount_ = k.spawnCount;
    touching_ = k.touching;
    wetStep_ = k.wetStep;
    joints_ = k.joints;
    dirtyBodies_.clear();
    stepIndex_ = k.step;
    renderPosed_ = false;
    writeEntityBodies(1.0f);  // (the entities where their bodies were: as the scripts' state is)
    restoreNs_ += SDL_GetTicksNS() - t0;
    ++restores_;
    return true;
}

void PhysicsWorld::rollbackDone()
{
    if (alpha_ < 1.0f) {
        writeEntityBodies(alpha_);
        renderPosed_ = true;
    }
}

// A step simulated again: its events were heard the first time.
void PhysicsWorld::stepOnce()
{
    const size_t impacts = impacts_.size(), triggers = triggers_.size(), entries = waterEntries_.size();
    resimulating_ = true;
    stepBegin();
    stepEnd();
    resimulating_ = false;
    impacts_.resize(impacts);
    triggers_.resize(triggers);
    waterEntries_.resize(entries);
}

bool PhysicsWorld::castRay(float3 from, float3 translation, ContactSide& side, float3& point, float3& normal, float& fraction) const
{
    if (!b3World_IsValid(world_))
        return false;
    const b3RayResult r = b3World_CastRayClosest(world_, toPos(from), {translation.x, translation.y, translation.z}, b3DefaultQueryFilter());
    if (!r.hit || !b3Shape_IsValid(r.shapeId))
        return false;
    side = sideOf(r.shapeId, r.userMaterialId);
    point = {float(r.point.x), float(r.point.y), float(r.point.z)};
    normal = {r.normal.x, r.normal.y, r.normal.z};
    fraction = r.fraction;
    return true;
}

bool PhysicsWorld::castSphere(float3 from, float radius, float3 translation, ContactSide& side, float3& point, float3& normal,
                              float& fraction, uint64_t* body) const
{
    if (!b3World_IsValid(world_))
        return false;
    if (radius <= 0.0f) {
        if (body)
            *body = 0;
        return castRay(from, translation, side, point, normal, fraction);
    }
    struct Closest {
        b3ShapeId shape = b3_nullShapeId;
        b3Pos point = {};
        b3Vec3 normal = {};
        float fraction = 1.0f;
        uint64_t material = 0;
        bool hit = false;
    } closest;
    const b3Vec3 center = {0.0f, 0.0f, 0.0f};
    const b3ShapeProxy proxy = {&center, 1, radius};
    b3World_CastShape(world_, toPos(from), &proxy, {translation.x, translation.y, translation.z}, b3DefaultQueryFilter(),
                      [](b3ShapeId shape, b3Pos p, b3Vec3 n, float f, uint64_t material, int, int, void* context) {
                          Closest& c = *static_cast<Closest*>(context);
                          if (f < c.fraction) {
                              c = {shape, p, n, f, material, true};
                          }
                          return f;  // (clip the cast: only nearer hits from here)
                      },
                      &closest);
    if (!closest.hit || !b3Shape_IsValid(closest.shape))
        return false;
    side = sideOf(closest.shape, closest.material);
    point = {float(closest.point.x), float(closest.point.y), float(closest.point.z)};
    normal = {closest.normal.x, closest.normal.y, closest.normal.z};
    fraction = closest.fraction;
    if (body) {
        const b3BodyId b = b3Shape_GetBody(closest.shape);
        *body = b3Body_GetType(b) == b3_dynamicBody ? b3StoreBodyId(b) : 0;
    }
    return true;
}

void PhysicsWorld::collectTriggers()
{
    const b3SensorEvents se = b3World_GetSensorEvents(world_);
    const auto add = [&](b3ShapeId sensor, b3ShapeId visitor, bool begin) {
        if (!b3Shape_IsValid(sensor))
            return;
        const ContactSide trigger = sideOf(sensor, 0);
        ContactSide other;
        if (b3Shape_IsValid(visitor))
            other = sideOf(visitor, 0);
        if (trigger.kind == ContactOwner::Entity)
            triggers_.push_back({trigger.id, other, begin});
    };
    for (int i = 0; i < se.beginCount; ++i)
        add(se.beginEvents[i].sensorShapeId, se.beginEvents[i].visitorShapeId, true);
    for (int i = 0; i < se.endCount; ++i)
        add(se.endEvents[i].sensorShapeId, se.endEvents[i].visitorShapeId, false);
}

b3BodyId PhysicsWorld::bodyOfEntity(uint64_t entity) const
{
    if (const int32_t index = poolBodyOf(entity); index >= 0)
        return bodies_[uint32_t(index)].id;
    if (const auto it = entityBodyOf_.find(entity); it != entityBodyOf_.end() && b3Body_IsValid(entityBodies_[it->second].id))
        return entityBodies_[it->second].id;
    return b3_nullBodyId;
}

bool PhysicsWorld::teleport(uint64_t entity, float3 position, const float* rotation)
{
    const b3BodyId id = bodyOfEntity(entity);
    if (!b3Body_IsValid(id))
        return false;
    b3Transform t = b3Body_GetTransform(id);
    t.p = toPos(position);
    if (rotation)
        t.q = b3NormalizeQuat(b3Quat{{rotation[0], rotation[1], rotation[2]}, rotation[3]});
    b3Body_SetTransform(id, t.p, t.q);
    b3Body_SetAwake(id, true);
    const flecs::entity e(scene_->world(), entity);
    if (const int32_t index = poolBodyOf(entity); index >= 0) {
        Body& b = bodies_[uint32_t(index)];
        b.prev = b.curr = t;
        b.fresh = true;
        activate(uint32_t(index));
        WorldTransform wt = {};
        const fpak::InstanceGpu r = row(b, t);
        std::memcpy(wt.rows, r.world, sizeof(wt.rows));
        e.set<WorldTransform>(wt);
        return true;
    }
    EntityBody& eb = entityBodies_[entityBodyOf_.at(entity)];
    eb.prev = eb.curr = eb.written = b3Body_GetTransform(id);
    // The entity there too (its scale kept; a child's transform under its parent follows), so a
    // kinematic body's target is where it now is.
    const b3Matrix3 m = b3MakeMatrixFromQuat(t.q);
    const float3 cols[3] = {f3(m.cx) * eb.scale.x, f3(m.cy) * eb.scale.y, f3(m.cz) * eb.scale.z};
    float world[3][4];
    for (int r = 0; r < 3; ++r) {
        world[r][0] = (&cols[0].x)[r];
        world[r][1] = (&cols[1].x)[r];
        world[r][2] = (&cols[2].x)[r];
    }
    world[0][3] = position.x, world[1][3] = position.y, world[2][3] = position.z;
    scene_->setWorld(e, world);
    return true;
}

bool PhysicsWorld::setBodyType(uint64_t entity, uint32_t type)
{
    const b3BodyId id = bodyOfEntity(entity);
    if (!b3Body_IsValid(id) || type > 2)
        return false;
    b3Body_SetType(id, type == 2 ? b3_dynamicBody : type == 1 ? b3_kinematicBody : b3_staticBody);
    if (const auto it = entityBodyOf_.find(entity); it != entityBodyOf_.end()) {
        EntityBody& eb = entityBodies_[it->second];
        eb.dynamic = type == 2;
        eb.kinematic = type == 1;
        eb.prev = eb.curr = eb.written = b3Body_GetTransform(id);
    }
    return true;
}

bool PhysicsWorld::addScriptShape(uint64_t entity, ScriptShape shape)
{
    if (shape.kind > 3 || (shape.kind == 3 && shape.points.size() < 12))
        return false;
    scriptShapes_[entity].push_back(std::move(shape));
    if (entityBodiesBuilt_)
        dirtyBodies_.push_back(entity);
    return true;
}

void PhysicsWorld::clearScriptShapes(uint64_t entity)
{
    if (scriptShapes_.erase(entity) && entityBodiesBuilt_)
        dirtyBodies_.push_back(entity);
}

bool PhysicsWorld::addScriptShapes(b3BodyId body, const b3ShapeDef& sd, const std::vector<ScriptShape>& shapes, const float s[3],
                                   float3& half, float3& center)
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const auto grow = [&](float x, float y, float z, float r) {
        const float p[3] = {x, y, z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a] - r);
            hi[a] = std::max(hi[a], p[a] + r);
        }
    };
    const float as[3] = {std::fabs(s[0]), std::fabs(s[1]), std::fabs(s[2])};
    bool made = false;
    for (const ScriptShape& sh : shapes) {
        const b3Quat q = {{sh.rotation[0], sh.rotation[1], sh.rotation[2]}, sh.rotation[3]};
        const b3Vec3 c = {sh.center[0] * s[0], sh.center[1] * s[1], sh.center[2] * s[2]};
        // Points in the entity's space: turned, moved, then scaled with it.
        const auto place = [&](float x, float y, float z) {
            const b3Vec3 r = b3RotateVector(q, {x, y, z});
            return b3Vec3{(r.x + sh.center[0]) * s[0], (r.y + sh.center[1]) * s[1], (r.z + sh.center[2]) * s[2]};
        };
        if (sh.kind == 1) {
            const b3Sphere ball = {c, std::max(sh.size[0], 1e-3f) * std::max({as[0], as[1], as[2]})};
            made |= B3_IS_NON_NULL(b3CreateSphereShape(body, &sd, &ball));
            grow(c.x, c.y, c.z, ball.radius);
        } else if (sh.kind == 2) {
            const float radius = std::max(sh.size[0], 1e-3f) * std::max(as[0], as[2]);
            const float h = std::max(0.5f * sh.size[1] * as[1] - radius, 0.0f);
            const b3Vec3 up = b3RotateVector(q, {0.0f, h, 0.0f});
            const b3Capsule capsule = {{c.x - up.x, c.y - up.y, c.z - up.z}, {c.x + up.x, c.y + up.y, c.z + up.z}, radius};
            made |= B3_IS_NON_NULL(b3CreateCapsuleShape(body, &sd, &capsule));
            grow(capsule.center1.x, capsule.center1.y, capsule.center1.z, radius);
            grow(capsule.center2.x, capsule.center2.y, capsule.center2.z, radius);
        } else {
            std::vector<b3Vec3> points;
            if (sh.kind == 3) {
                for (size_t i = 0; i + 2 < sh.points.size(); i += 3)
                    points.push_back(place(sh.points[i], sh.points[i + 1], sh.points[i + 2]));
            } else {  // a box: its 8 corners (a hull: turned and scaled exactly)
                for (int k = 0; k < 8; ++k)
                    points.push_back(place((k & 1 ? 0.5f : -0.5f) * sh.size[0], (k & 2 ? 0.5f : -0.5f) * sh.size[1],
                                           (k & 4 ? 0.5f : -0.5f) * sh.size[2]));
            }
            b3HullData* hull = points.size() >= 4 ? b3CreateHull(points.data(), int(points.size()), 64) : nullptr;
            if (!hull) {
                FX_WARN("physics: a script's hull of %zu points is not a solid (flat, or too few points)", points.size());
                continue;
            }
            made |= B3_IS_NON_NULL(b3CreateHullShape(body, &sd, hull));  // (Box3D keeps its own copy)
            b3DestroyHull(hull);
            for (const b3Vec3& p : points)
                grow(p.x, p.y, p.z, 0.0f);
        }
    }
    if (made) {
        half = {0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]), 0.5f * (hi[2] - lo[2])};
        center = {0.5f * (hi[0] + lo[0]), 0.5f * (hi[1] + lo[1]), 0.5f * (hi[2] + lo[2])};
    }
    return made;
}

bool PhysicsWorld::rebuild(uint64_t entity)
{
    if (!valid() || !entityBodiesBuilt_ || poolBodyOf(entity) >= 0)
        return false;
    const flecs::entity e(scene_->world(), entity);
    const bool alive = scene_->world().is_alive(entity);
    const auto it = entityBodyOf_.find(entity);
    int32_t index = -1;
    if (it != entityBodyOf_.end()) {
        index = int32_t(it->second);
        EntityBody& eb = entityBodies_[it->second];
        if (b3Body_IsValid(eb.id))
            b3DestroyBody(eb.id);  // (its joints with it)
        eb.id = b3_nullBodyId;
        eb.dynamic = eb.kinematic = eb.floats = false;
    }
    if (!alive || (!e.has<PhysicsBody>() && !e.has<Collider>()))
        return it != entityBodyOf_.end();
    const Collider* c = e.try_get<Collider>();
    addEntityBody(e, e.try_get<PhysicsBody>(), c ? *c : Collider{}, index);
    return true;
}

namespace {
b3QueryFilter queryFilter(const QueryFilter& f)
{
    b3QueryFilter q = b3DefaultQueryFilter();
    q.maskBits = f.layers;  // (shapes' categories: their layers' bits)
    return q;
}

// A query shape as Box3D's proxy, relative to its position.
struct Proxy {
    b3Vec3 points[8];
    b3ShapeProxy proxy = {};
};
Proxy proxyOf(const QueryShape& s)
{
    Proxy p;
    const b3Quat q = b3NormalizeQuat(b3Quat{{s.rotation[0], s.rotation[1], s.rotation[2]}, s.rotation[3]});
    if (s.type == QueryShape::Box) {
        int n = 0;
        for (int i = 0; i < 8; ++i)
            p.points[n++] = b3RotateVector(q, {i & 1 ? s.size.x : -s.size.x, i & 2 ? s.size.y : -s.size.y, i & 4 ? s.size.z : -s.size.z});
        p.proxy = {p.points, 8, 0.0f};
    } else if (s.type == QueryShape::Capsule) {
        const float half = std::max(0.5f * s.size.y - s.size.x, 0.0f);
        p.points[0] = b3RotateVector(q, {0.0f, -half, 0.0f});
        p.points[1] = b3RotateVector(q, {0.0f, half, 0.0f});
        p.proxy = {p.points, 2, s.size.x};
    } else {
        p.points[0] = {0.0f, 0.0f, 0.0f};
        p.proxy = {p.points, 1, s.size.x};
    }
    return p;
}

// The nearest hit of a cast, passing through an entity's shapes.
struct Nearest {
    uint64_t ignore = 0;
    b3ShapeId shape = b3_nullShapeId;
    b3Pos point = {};
    b3Vec3 normal = {};
    float fraction = 1.0f;
    uint64_t material = 0;
    bool hit = false;
};
float nearestHit(b3ShapeId shape, b3Pos p, b3Vec3 n, float f, uint64_t material, int, int, void* context)
{
    Nearest& c = *static_cast<Nearest*>(context);
    const uint64_t data = uint64_t(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape)));
    if (c.ignore && ContactOwner(data >> 62) == ContactOwner::Entity && (data & ((uint64_t(1) << 62) - 1)) == c.ignore)
        return -1.0f;  // (passed through)
    if (b3Shape_IsSensor(shape))
        return -1.0f;  // (triggers are not hit)
    if (f < c.fraction)
        c = {c.ignore, shape, p, n, f, material, true};
    return f;
}
}  // namespace

bool PhysicsWorld::castRayFiltered(float3 from, float3 translation, const QueryFilter& filter, ContactSide& side, float3& point,
                                   float3& normal, float& fraction) const
{
    if (!b3World_IsValid(world_))
        return false;
    Nearest nearest;
    nearest.ignore = filter.ignore;
    b3World_CastRay(world_, toPos(from), {translation.x, translation.y, translation.z}, queryFilter(filter), nearestHit, &nearest);
    if (!nearest.hit || !b3Shape_IsValid(nearest.shape))
        return false;
    side = sideOf(nearest.shape, nearest.material);
    point = {float(nearest.point.x), float(nearest.point.y), float(nearest.point.z)};
    normal = {nearest.normal.x, nearest.normal.y, nearest.normal.z};
    fraction = nearest.fraction;
    return true;
}

bool PhysicsWorld::castShape(const QueryShape& shape, float3 translation, const QueryFilter& filter, ContactSide& side, float3& point,
                             float3& normal, float& fraction) const
{
    if (!b3World_IsValid(world_))
        return false;
    const Proxy p = proxyOf(shape);
    Nearest nearest;
    nearest.ignore = filter.ignore;
    b3World_CastShape(world_, toPos(shape.position), &p.proxy, {translation.x, translation.y, translation.z}, queryFilter(filter),
                      nearestHit, &nearest);
    if (!nearest.hit || !b3Shape_IsValid(nearest.shape))
        return false;
    side = sideOf(nearest.shape, nearest.material);
    point = {float(nearest.point.x), float(nearest.point.y), float(nearest.point.z)};
    normal = {nearest.normal.x, nearest.normal.y, nearest.normal.z};
    fraction = nearest.fraction;
    return true;
}

int PhysicsWorld::overlap(const QueryShape& shape, const QueryFilter& filter, uint64_t* out, int capacity) const
{
    if (!b3World_IsValid(world_))
        return 0;
    struct Found {
        uint64_t ignore;
        std::vector<uint64_t> entities;
    } found{filter.ignore, {}};
    const Proxy p = proxyOf(shape);
    b3World_OverlapShape(world_, toPos(shape.position), &p.proxy, queryFilter(filter),
                         [](b3ShapeId s, void* context) {
                             Found& f = *static_cast<Found*>(context);
                             const uint64_t data = uint64_t(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(s)));
                             const uint64_t id = data & ((uint64_t(1) << 62) - 1);
                             if (ContactOwner(data >> 62) == ContactOwner::Entity && id != f.ignore &&
                                 std::find(f.entities.begin(), f.entities.end(), id) == f.entities.end())
                                 f.entities.push_back(id);
                             return true;
                         },
                         &found);
    for (int i = 0; out && i < capacity && size_t(i) < found.entities.size(); ++i)
        out[i] = found.entities[size_t(i)];
    return int(found.entities.size());
}

CharacterMove PhysicsWorld::moveCharacter(float3 position, float radius, float height, float stepHeight, float maxSlopeCos, float3 delta,
                                          const QueryFilter& filter) const
{
    CharacterMove result = {position, false, {0, 1, 0}, {}, false};
    if (!b3World_IsValid(world_) || radius <= 0.0f)
        return result;
    height = std::max(height, 2.0f * radius);
    const b3Capsule capsule = {{0.0f, radius, 0.0f}, {0.0f, height - radius, 0.0f}, radius};
    struct Planes {
        uint64_t ignore;
        float maxSlopeCos;
        b3CollisionPlane planes[32];
        ContactSide sides[32];
        int count = 0;
        const PhysicsWorld* world;
    } planes{filter.ignore, maxSlopeCos, {}, {}, 0, this};
    const b3QueryFilter q = queryFilter(filter);
    const auto mine = [](b3ShapeId shape, void* context) {
        const uint64_t ignore = *static_cast<const uint64_t*>(context);
        const uint64_t data = uint64_t(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape)));
        return !b3Shape_IsSensor(shape) &&
               !(ignore && ContactOwner(data >> 62) == ContactOwner::Entity && (data & ((uint64_t(1) << 62) - 1)) == ignore);
    };
    const auto collide = [&](float3 at) {
        planes.count = 0;
        b3World_CollideMover(world_, toPos(at), &capsule, q,
                             [](b3ShapeId shape, const b3PlaneResult* results, int count, void* context) {
                                 Planes& p = *static_cast<Planes*>(context);
                                 const uint64_t data = uint64_t(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape)));
                                 if (b3Shape_IsSensor(shape) ||
                                     (p.ignore && ContactOwner(data >> 62) == ContactOwner::Entity &&
                                      (data & ((uint64_t(1) << 62) - 1)) == p.ignore))
                                     return true;
                                 for (int i = 0; i < count && p.count < 32; ++i) {
                                     p.planes[p.count] = {results[i].plane, FLT_MAX, 0.0f, true};
                                     p.sides[p.count++] = p.world->sideOf(shape, 0);
                                 }
                                 return true;
                             },
                             &planes);
    };
    // Slides toward `target` (absolute), as Box3D's mover sample does: collide, solve, cast.
    const auto slide = [&](float3 from, float3 target) {
        float3 at = from;
        for (int iteration = 0; iteration < 5; ++iteration) {
            collide(at);
            const b3Vec3 want = {target.x - at.x, target.y - at.y, target.z - at.z};
            const b3PlaneSolverResult solved = b3SolvePlanes(want, planes.planes, planes.count);
            uint64_t ignore = filter.ignore;
            const float f = b3World_CastMover(world_, toPos(at), &capsule, solved.delta, q, mine, &ignore);
            const float3 step = {solved.delta.x * f, solved.delta.y * f, solved.delta.z * f};
            at = at + step;
            // (All of the solved move taken: what's left the planes block, so another pass would
            // find nothing more; stopping saves its two queries.)
            if (f >= 1.0f || length(step) < 1e-4f)
                break;
        }
        return at;
    };
    const auto groundAt = [&](float3 at, ContactSide& side, float3& normal) {
        collide(at - float3{0.0f, 0.05f, 0.0f});  // (a little lower: the planes of what it stands on)
        bool found = false;
        for (int i = 0; i < planes.count; ++i) {
            const b3Vec3 n = planes.planes[i].plane.normal;
            if (n.y >= maxSlopeCos && (!found || n.y > normal.y)) {
                normal = {n.x, n.y, n.z};
                side = planes.sides[i];
                found = true;
            }
        }
        return found;
    };

    const float3 horizontal = {delta.x, 0.0f, delta.z};
    float3 at = slide(position, position + delta);
    // Blocked going sideways: a step? Up by the step height, then a radius further than the move to
    // find walkable ground there (an edge the capsule's round bottom can't reach this move); if
    // there is, no lower than here, the move is taken at that height.
    if (stepHeight > 0.0f && length(horizontal) > 1e-4f) {
        const float3 dir = normalize(horizontal);
        const float3 gotten = at - position;
        const float along = dot(float3{gotten.x, 0.0f, gotten.z}, dir);
        if (along < length(horizontal) * 0.9f) {
            const float3 up = slide(position, position + float3{0.0f, stepHeight, 0.0f});
            const float3 probe = slide(up, up + horizontal + dir * radius);
            const float3 landed = slide(probe, probe - float3{0.0f, up.y - position.y + 0.05f, 0.0f});
            ContactSide side;
            float3 normal;
            // (Landing no lower than here: a step, or the rest of one it is crossing, its round bottom
            // not over the step yet.)
            if (dot(probe - up, dir) > along + 1e-3f && landed.y > position.y - 0.01f && groundAt(landed, side, normal)) {
                const float3 over = slide(up, up + horizontal);
                at = slide(over, float3{over.x, landed.y, over.z});
            } else {
                result.blocked = true;
            }
        }
    }
    // Walking down a slope or off a step: snapped to the ground below (within the step height).
    ContactSide side;
    float3 normal = {0, 1, 0};
    bool grounded = groundAt(at, side, normal);
    if (!grounded && delta.y <= 0.0f && stepHeight > 0.0f) {
        const float3 down = slide(at, at - float3{0.0f, stepHeight, 0.0f});
        const float3 moved = down - at;  // (straight down only: not slid off an edge it is crossing)
        if (std::fabs(moved.x) + std::fabs(moved.z) < 0.01f && groundAt(down, side, normal)) {
            at = down;
            grounded = true;
        }
    }
    result.grounded = grounded;
    if (result.grounded) {
        result.groundNormal = normal;
        result.ground = side;
    }
    result.position = at;
    return result;
}

uint64_t PhysicsWorld::createJoint(JointType type, uint64_t a, uint64_t b, const JointSettings& js)
{
    const b3BodyId ba = bodyOfEntity(a), bb = b ? bodyOfEntity(b) : worldAnchor_;
    if (!b3Body_IsValid(ba) || !b3Body_IsValid(bb))
        return 0;
    // Frames at the anchor, their X (slider, wheel) or Z (hinge) along the axis, in each body's space.
    const float3 axis = length(js.axis) > 1e-6f ? normalize(js.axis) : float3{0, 1, 0};
    const auto turnTo = [](float3 from, float3 to) {  // the shortest turn of `from` onto `to`
        const float d = dot(from, to);
        if (d < -0.9999f)
            return std::fabs(from.x) < 0.9f ? b3Quat{{1.0f, 0.0f, 0.0f}, 0.0f} : b3Quat{{0.0f, 1.0f, 0.0f}, 0.0f};
        const float3 c = cross(from, to);
        return b3NormalizeQuat(b3Quat{{c.x, c.y, c.z}, 1.0f + d});
    };
    const b3Quat worldFrame = turnTo(type == JointType::Hinge ? float3{0, 0, 1} : float3{1, 0, 0}, axis);
    const auto local = [&](b3BodyId body, float3 point) {
        const b3Transform t = b3Body_GetTransform(body);
        b3Transform f;
        f.p = b3Body_GetLocalPoint(body, toPos(point));
        f.q = b3InvMulQuat(t.q, worldFrame);
        return f;
    };
    b3JointDef base = b3DefaultRevoluteJointDef().base;
    base.bodyIdA = ba;
    base.bodyIdB = bb;
    base.localFrameA = local(ba, js.anchor);
    base.localFrameB = local(bb, type == JointType::Distance ? js.anchorB : js.anchor);
    base.collideConnected = js.collide;
    b3JointId id = b3_nullJointId;
    switch (type) {
    case JointType::Fixed: {
        b3WeldJointDef d = b3DefaultWeldJointDef();
        d.base = base;
        d.linearHertz = d.angularHertz = js.spring;
        d.linearDampingRatio = d.angularDampingRatio = js.damping;
        id = b3CreateWeldJoint(world_, &d);
        break;
    }
    case JointType::Hinge: {
        b3RevoluteJointDef d = b3DefaultRevoluteJointDef();
        d.base = base;
        d.enableLimit = js.limit, d.lowerAngle = js.lower, d.upperAngle = js.upper;
        d.enableMotor = js.motor, d.motorSpeed = js.motorSpeed, d.maxMotorTorque = js.motorMax;
        d.enableSpring = js.spring > 0.0f, d.hertz = js.spring, d.dampingRatio = js.damping;
        id = b3CreateRevoluteJoint(world_, &d);
        break;
    }
    case JointType::Ball: {
        b3SphericalJointDef d = b3DefaultSphericalJointDef();
        d.base = base;
        id = b3CreateSphericalJoint(world_, &d);
        break;
    }
    case JointType::Slider: {
        b3PrismaticJointDef d = b3DefaultPrismaticJointDef();
        d.base = base;
        d.enableLimit = js.limit, d.lowerTranslation = js.lower, d.upperTranslation = js.upper;
        d.enableMotor = js.motor, d.motorSpeed = js.motorSpeed, d.maxMotorForce = js.motorMax;
        d.enableSpring = js.spring > 0.0f, d.hertz = js.spring, d.dampingRatio = js.damping;
        id = b3CreatePrismaticJoint(world_, &d);
        break;
    }
    case JointType::Distance: {
        b3DistanceJointDef d = b3DefaultDistanceJointDef();
        d.base = base;
        d.length = std::max(length(js.anchorB - js.anchor), 0.005f);
        d.enableLimit = js.limit, d.minLength = js.lower, d.maxLength = js.upper;
        d.enableSpring = js.spring > 0.0f, d.hertz = js.spring, d.dampingRatio = js.damping;
        d.enableMotor = js.motor, d.motorSpeed = js.motorSpeed, d.maxMotorForce = js.motorMax;
        id = b3CreateDistanceJoint(world_, &d);
        break;
    }
    case JointType::Wheel: {
        b3WheelJointDef d = b3DefaultWheelJointDef();
        d.base = base;
        d.enableSpinMotor = js.motor, d.spinSpeed = js.motorSpeed, d.maxSpinTorque = js.motorMax;
        d.enableSuspensionSpring = js.spring > 0.0f, d.suspensionHertz = js.spring, d.suspensionDampingRatio = js.damping;
        id = b3CreateWheelJoint(world_, &d);
        break;
    }
    }
    if (!b3Joint_IsValid(id))
        return 0;
    b3Body_SetAwake(ba, true);
    joints_.push_back(id);
    return b3StoreJointId(id);
}

bool PhysicsWorld::destroyJoint(uint64_t joint)
{
    const b3JointId id = b3LoadJointId(joint);
    if (!joint || !b3Joint_IsValid(id))
        return false;
    b3DestroyJoint(id, true);
    return true;
}

void PhysicsWorld::setJointMotor(uint64_t joint, float speed, float maxForce)
{
    const b3JointId id = b3LoadJointId(joint);
    if (!joint || !b3Joint_IsValid(id))
        return;
    switch (b3Joint_GetType(id)) {
    case b3_revoluteJoint:
        b3RevoluteJoint_EnableMotor(id, true), b3RevoluteJoint_SetMotorSpeed(id, speed), b3RevoluteJoint_SetMaxMotorTorque(id, maxForce);
        break;
    case b3_prismaticJoint:
        b3PrismaticJoint_EnableMotor(id, true), b3PrismaticJoint_SetMotorSpeed(id, speed), b3PrismaticJoint_SetMaxMotorForce(id, maxForce);
        break;
    case b3_wheelJoint:
        b3WheelJoint_EnableSpinMotor(id, true), b3WheelJoint_SetSpinMotorSpeed(id, speed), b3WheelJoint_SetMaxSpinTorque(id, maxForce);
        break;
    case b3_distanceJoint:
        b3DistanceJoint_EnableMotor(id, true), b3DistanceJoint_SetMotorSpeed(id, speed), b3DistanceJoint_SetMaxMotorForce(id, maxForce);
        break;
    default: break;
    }
    b3Body_SetAwake(b3Joint_GetBodyA(id), true);
}

void PhysicsWorld::clearJoints()
{
    for (const b3JointId id : joints_)
        if (b3Joint_IsValid(id))
            b3DestroyJoint(id, false);
    joints_.clear();
}

float3 PhysicsWorld::gravity() const
{
    if (!valid())
        return {};
    const b3Vec3 g = b3World_GetGravity(world_);
    return {g.x, g.y, g.z};
}

void PhysicsWorld::setGravity(float3 g)
{
    if (valid())
        b3World_SetGravity(world_, {g.x, g.y, g.z});
}

void PhysicsWorld::applyImpulse(uint64_t body, float3 impulse, float3 point)
{
    if (!body || !b3World_IsValid(world_))
        return;
    const b3BodyId b = b3LoadBodyId(body);
    if (b3Body_IsValid(b))
        b3Body_ApplyLinearImpulse(b, {impulse.x, impulse.y, impulse.z}, toPos(point), true);
}

ContactSide PhysicsWorld::sideOf(b3ShapeId shape, uint64_t userMaterial) const
{
    ContactSide side;
    const uint64_t data = uint64_t(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape)));
    side.kind = ContactOwner(data >> 62);
    side.id = data & ((uint64_t(1) << 62) - 1);
    side.material = uint32_t(userMaterial);
    const b3BodyId body = b3Shape_GetBody(shape);
    side.mass = b3Body_GetType(body) == b3_dynamicBody ? b3Body_GetMass(body) : 0.0f;
    return side;
}

void PhysicsWorld::collectContacts()
{
    FX_ZONE_CAT("PhysicsWorld::collectContacts", Physics);
    const b3ContactEvents ce = b3World_GetContactEvents(world_);
    for (int i = 0; i < ce.hitCount; ++i) {
        const b3ContactHitEvent& h = ce.hitEvents[i];
        if (!b3Shape_IsValid(h.shapeIdA) || !b3Shape_IsValid(h.shapeIdB))
            continue;
        impacts_.push_back({{float(h.point.x), float(h.point.y), float(h.point.z)}, {h.normal.x, h.normal.y, h.normal.z}, h.approachSpeed,
                            sideOf(h.shapeIdA, h.userMaterialIdA), sideOf(h.shapeIdB, h.userMaterialIdB), pairKey(h.shapeIdA, h.shapeIdB)});
    }
    for (int i = 0; i < ce.beginCount; ++i)
        touching_[contactKey(ce.beginEvents[i].contactId)] = ce.beginEvents[i].contactId;
    for (int i = 0; i < ce.endCount; ++i)
        touching_.erase(contactKey(ce.endEvents[i].contactId));
    // Touching contacts: how fast they slide along each other, or roll.
    motions_.clear();
    for (auto it = touching_.begin(); it != touching_.end();) {
        const b3ContactId id = it->second;
        if (!b3Contact_IsValid(id)) {
            it = touching_.erase(it);
            continue;
        }
        ++it;
        const b3ContactData d = b3Contact_GetData(id);
        if (d.manifoldCount <= 0 || !b3Shape_IsValid(d.shapeIdA) || !b3Shape_IsValid(d.shapeIdB))
            continue;
        const b3Manifold& m = d.manifolds[0];
        if (m.pointCount <= 0)
            continue;
        const b3BodyId ba = b3Shape_GetBody(d.shapeIdA), bb = b3Shape_GetBody(d.shapeIdB);
        float impulse = 0.0f;
        float3 anchor = {0, 0, 0};
        for (int p = 0; p < m.pointCount; ++p) {
            impulse += m.points[p].normalImpulse;
            anchor = anchor + float3{m.points[p].anchorA.x, m.points[p].anchorA.y, m.points[p].anchorA.z} * (1.0f / float(m.pointCount));
        }
        const b3Pos origin = b3Body_GetPosition(ba);
        const float3 point = float3{float(origin.x), float(origin.y), float(origin.z)} + anchor;
        const float3 n = {m.normal.x, m.normal.y, m.normal.z};
        const b3Vec3 va = b3Body_GetWorldPointVelocity(ba, toPos(point)), vb = b3Body_GetWorldPointVelocity(bb, toPos(point));
        float3 rel = {va.x - vb.x, va.y - vb.y, va.z - vb.z};
        rel = rel - n * dot(rel, n);
        PhysicsContactMotion motion;
        motion.contact = contactKey(id);
        motion.point = point;
        motion.slip = length(rel);
        motion.roll = 0.0f;
        motion.force = std::max(impulse, 0.0f) / kStep;
        motion.a = sideOf(d.shapeIdA, 0);
        motion.b = sideOf(d.shapeIdB, 0);
        // A ball rolls when its surface keeps pace with the ground (little slip at the contact).
        for (const b3ShapeId shape : {d.shapeIdA, d.shapeIdB}) {
            if (b3Shape_GetType(shape) != b3_sphereShape)
                continue;
            const b3BodyId body = b3Shape_GetBody(shape);
            if (b3Body_GetType(body) != b3_dynamicBody)
                continue;
            const b3Vec3 w = b3Body_GetAngularVelocity(body);
            const float rolling = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z) * b3Shape_GetSphere(shape).radius;
            if (rolling > 0.05f && motion.slip < 0.25f * rolling + 0.05f) {
                motion.roll = rolling;
                motion.slip = 0.0f;
            }
        }
        motions_.push_back(motion);
    }
}

uint64_t PhysicsWorld::stateHash() const
{
    return b3World_IsValid(world_) ? b3HashWorldState(b3GetWorldFromId(world_)) : 0;
}

void PhysicsWorld::update(float dt, bool fixedStep, uint32_t maxSteps)
{
    if (!b3World_IsValid(world_))
        return;
    impacts_.clear();  // (this update's steps': motions and stirs stay as of the last step)
    triggers_.clear();
    waterEntries_.clear();
    if (fixedStep) {
        if (maxSteps > 0)
            step();
        alpha_ = 1.0f;
    } else {
        accumulator_ = std::min(accumulator_ + dt, 0.25f);  // drop time after hitches instead of spiralling
        for (uint32_t steps = 0; accumulator_ >= kStep && steps < maxSteps; ++steps) {
            step();
            accumulator_ -= kStep;
        }
        alpha_ = accumulator_ / kStep;
    }
    if (alpha_ < 1.0f) {  // (the render's pose between the last two steps, until the next step)
        writeEntityBodies(alpha_);
        renderPosed_ = true;
    }
}

fpak::InstanceGpu PhysicsWorld::row(const Body& b, const b3Transform& t) const
{
    const b3Matrix3 m = b3MakeMatrixFromQuat(t.q);
    const float3 cols[3] = {f3(m.cx) * b.scale.x, f3(m.cy) * b.scale.y, f3(m.cz) * b.scale.z};
    fpak::InstanceGpu r = {};
    for (int row = 0; row < 3; ++row) {
        r.world[row][0] = (&cols[0].x)[row];
        r.world[row][1] = (&cols[1].x)[row];
        r.world[row][2] = (&cols[2].x)[row];
    }
    r.world[0][3] = t.p.x;
    r.world[1][3] = t.p.y;
    r.world[2][3] = t.p.z;
    r.mesh = pools_[b.shape == Shape::Box ? 0 : 1].mesh;
    r.tint = b.tint;
    r.flags = b.hidden ? fpak::kInstanceHidden : 0u;
    return r;
}

void PhysicsWorld::extract(std::vector<InstanceUpdate>& rows)
{
    FX_ZONE_CAT("PhysicsWorld::extract", Physics);
    for (uint32_t index : removed_) {
        const Body& b = bodies_[index];
        fpak::InstanceGpu hidden = row(b, b.curr);
        hidden.flags = fpak::kInstanceHidden;
        rows.push_back({b.slot, hidden});
    }
    removed_.clear();
    size_t keep = 0;
    for (uint32_t index : active_) {
        Body& b = bodies_[index];
        if (!b.live) {
            b.active = false;
            continue;
        }
        b3Transform t = b.curr;
        if (alpha_ < 1.0f) {
            t.p = b3Lerp(b.prev.p, b.curr.p, alpha_);
            t.q = b3NLerp(b.prev.q, b.curr.q, alpha_);
        }
        // Same pose as last extracted (at rest, or physics paused/frozen): nothing to stream.
        if (!b.fresh && std::memcmp(&t, &b.extracted, sizeof(t)) == 0) {
            b.active = false;
            continue;
        }
        b.fresh = false;
        b.extracted = t;
        rows.push_back({b.slot, row(b, t)});
        active_[keep++] = index;
    }
    active_.resize(keep);
}

PhysicsStats PhysicsWorld::stats() const
{
    PhysicsStats s = {};
    for (const Body& b : bodies_)
        s.bodies += b.live ? 1 : 0;
    s.awake = b3World_IsValid(world_) ? uint32_t(b3World_GetAwakeBodyCount(world_)) : 0;
    s.staticShapes = staticShapes_;
    for (const auto& [cell, bodies] : scatterCells_)
        s.staticShapes += uint32_t(bodies.size());
    s.steps = uint32_t(stepIndex_);
    s.entityBodies = uint32_t(entityBodies_.size());
    s.stepMs = stepMs_;
    return s;
}

}  // namespace fx
