#include "script/script_level.h"

#include "assets/scene_pak.h"
#include "core/json.h"
#include "level/level_meshes.h"
#include "level/mesh_ops.h"
#include "level/navmesh.h"
#include "render/live_geometry.h"
#include "scene/scene.h"

#include <flecs.h>

#include <algorithm>
#include <cstring>

namespace fx {
namespace {

using Context = ScriptHost::Context;

Context& ctxOf(void* c) { return *static_cast<Context*>(c); }

const float kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

// The world transform a live mesh's entity has now (identity without one).
const float (*worldOfLive(Context& x, int32_t live))[4]
{
    if (const auto it = x.liveEntities.find(live); it != x.liveEntities.end()) {
        const flecs::entity e(x.scene->world(), it->second);
        if (e.is_alive())
            if (const WorldTransform* t = e.try_get<WorldTransform>())
                return rows3x4(t->rows);
    }
    return kIdentity;
}

void setHidden(Context& x, uint32_t slot, bool hidden)
{
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    row.flags = hidden ? row.flags | fpak::kInstanceHidden : row.flags & ~uint32_t(fpak::kInstanceHidden);
    x.touched.push_back(slot);
}

void freeLive(Context& x, int32_t live, bool deleteEntity)
{
    const auto it = x.liveEntities.find(live);
    x.live->destroy(uint32_t(live), worldOfLive(x, live));
    setHidden(x, x.live->instanceOf(uint32_t(live)), true);
    if (it != x.liveEntities.end()) {
        if (deleteEntity && x.scene->world().is_alive(it->second))
            flecs::entity(x.scene->world(), it->second).destruct();
        x.liveEntities.erase(it);
    }
}

int32_t apiLiveCreate(void* c, const float* rows, uint64_t parent, uint64_t* entity)
{
    Context& x = ctxOf(c);
    if (entity)
        *entity = 0;
    if (!x.live || !x.live->valid() || !x.scene || !x.pak)
        return -1;
    const int32_t live = x.live->create();
    if (live < 0)
        return -1;
    const uint32_t slot = x.live->instanceOf(uint32_t(live));
    fpak::InstanceGpu& row = x.pak->mutableInstances()[slot];
    std::memcpy(row.world, rows ? rows : &kIdentity[0][0], sizeof(row.world));
    row.flags &= ~uint32_t(fpak::kInstanceHidden);
    row.tint = 0;
    const flecs::entity e = x.scene->spawnInstance(slot, row);
    if (parent && x.scene->world().is_alive(parent))
        x.scene->setParent(e, flecs::entity(x.scene->world(), parent));
    x.touched.push_back(slot);
    x.liveEntities[live] = e.id();
    if (x.tracking && x.createdSet.insert(e.id()).second)  // (the editor's Stop deletes it)
        x.created.push_back(e.id());
    if (entity)
        *entity = e.id();
    return live;
}

int32_t apiLiveSet(void* c, int32_t live, const FxLiveMeshData* d)
{
    Context& x = ctxOf(c);
    if (!x.live || live < 0 || !d || d->vertexCount < 0 || d->indexCount < 0 || !x.liveEntities.count(live))
        return 0;
    const size_t n = size_t(d->vertexCount);
    LiveMeshData data;
    if (n && (!d->positions || !d->normals))
        return 0;
    if (n) {
        data.positions.assign(d->positions, d->positions + n * 3);
        data.normals.assign(d->normals, d->normals + n * 3);
        if (d->uvs)
            data.uvs.assign(d->uvs, d->uvs + n * 2);
        if (d->tangents)
            data.tangents.assign(d->tangents, d->tangents + n * 4);
    }
    if (d->indexCount && !d->indices)
        return 0;
    data.indices.assign(d->indices, d->indices + d->indexCount);
    if (d->materials)
        data.materials.assign(d->materials, d->materials + d->indexCount / 3);
    return x.live->set(uint32_t(live), data, worldOfLive(x, live), !(d->flags & 1u)) ? 1 : 0;
}

void apiLiveDestroy(void* c, int32_t live)
{
    Context& x = ctxOf(c);
    if (x.live && live >= 0 && x.liveEntities.count(live))
        freeLive(x, live, true);
}

int32_t apiLiveOf(void* c, uint64_t entity)
{
    Context& x = ctxOf(c);
    for (const auto& [live, e] : x.liveEntities)
        if (e == entity)
            return live;
    return -1;
}

int32_t apiLiveTriangle(void* c, int32_t live, uint32_t cluster, uint32_t triangle)
{
    Context& x = ctxOf(c);
    return x.live && live >= 0 ? x.live->sourceTriangle(uint32_t(live), cluster, triangle) : -1;
}

void apiLiveStats(void* c, FxLiveStats* out)
{
    Context& x = ctxOf(c);
    if (!out)
        return;
    *out = {};
    if (!x.live)
        return;
    const LiveGeometryStats& s = x.live->stats();
    *out = {s.meshes, s.meshCapacity, s.blocks, s.blockCapacity, s.triangles, s.uploads, s.lastBuildMs, 0};
}

std::shared_ptr<const level::PolyMesh> meshFromBlob(const uint8_t* blob, int32_t size)
{
    auto mesh = std::make_shared<level::PolyMesh>();
    if (!blob || size <= 0)
        return nullptr;
    return mesh->load(blob, size_t(size)) ? mesh : nullptr;
}

int32_t apiMeshGet(void* c, uint64_t entity, uint8_t* buffer, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (!x.scene || !x.scene->world().is_alive(entity))
        return 0;
    const LevelMesh* m = flecs::entity(x.scene->world(), entity).try_get<LevelMesh>();
    if (!m || !m->mesh)
        return 0;
    const std::vector<uint8_t> blob = m->mesh->save();
    if (int32_t(blob.size()) > capacity || !buffer)
        return -int32_t(blob.size());
    std::memcpy(buffer, blob.data(), blob.size());
    return int32_t(blob.size());
}

int32_t apiMeshSet(void* c, uint64_t entity, const uint8_t* blob, int32_t size)
{
    Context& x = ctxOf(c);
    std::shared_ptr<const level::PolyMesh> mesh = meshFromBlob(blob, size);
    if (!mesh || !x.scene || !x.scene->world().is_alive(entity))
        return 0;
    if (x.levelEdit && x.levelEdit(entity, mesh))
        return 1;
    flecs::entity(x.scene->world(), entity).set<LevelMesh>({std::move(mesh)});
    return 1;
}

uint64_t apiMeshCreate(void* c, const float* rows, uint64_t parent, const uint8_t* blob, int32_t size)
{
    Context& x = ctxOf(c);
    if (!x.scene)
        return 0;
    std::shared_ptr<const level::PolyMesh> mesh = blob ? meshFromBlob(blob, size)
                                                       : std::make_shared<level::PolyMesh>(level::PolyMesh::box({1, 1, 1}));
    if (!mesh)
        return 0;
    if (x.levelCreate)
        if (const uint64_t e = x.levelCreate(rows ? rows : &kIdentity[0][0], parent, mesh))
            return e;
    const flecs::entity e = x.scene->spawnTransform(rows ? reinterpret_cast<const float(*)[4]>(rows) : kIdentity);
    e.set<LevelMesh>({std::move(mesh)});
    if (parent && x.scene->world().is_alive(parent))
        x.scene->setParent(e, flecs::entity(x.scene->world(), parent));
    if (x.tracking && x.createdSet.insert(e.id()).second)  // (the editor's Stop deletes it)
        x.created.push_back(e.id());
    return e.id();
}

int32_t apiMeshSetLive(void* c, uint64_t entity, const uint8_t* blob, int32_t size)
{
    Context& x = ctxOf(c);
    std::shared_ptr<const level::PolyMesh> mesh = meshFromBlob(blob, size);
    if (!mesh || !x.scene || !x.scene->world().is_alive(entity))
        return 0;
    if (x.levelEditor && x.levelEditor->meshLive(entity, mesh))
        return 1;
    flecs::entity(x.scene->world(), entity).set<LevelMesh>({std::move(mesh)});
    return 1;
}

int32_t apiMeshCommit(void* c, uint64_t entity, const uint8_t* before, int32_t size)
{
    Context& x = ctxOf(c);
    std::shared_ptr<const level::PolyMesh> mesh = meshFromBlob(before, size);
    if (!mesh || !x.scene || !x.scene->world().is_alive(entity))
        return 0;
    if (x.levelEditor)  // (a game: what is set stays, no undo)
        x.levelEditor->meshCommit(entity, mesh);
    return 1;
}

int32_t apiViewport(void* c, FxViewportState* out)
{
    Context& x = ctxOf(c);
    if (!out)
        return 0;
    *out = {};
    return x.levelEditor && x.levelEditor->viewportState(*out) ? 1 : 0;
}

void apiViewportCapture(void* c, int32_t on)
{
    if (LevelEditorLink* l = ctxOf(c).levelEditor)
        l->viewportCapture(on != 0, (on & 2) != 0);
}

void apiViewportHint(void* c, const char* text)
{
    if (LevelEditorLink* l = ctxOf(c).levelEditor)
        l->viewportHint(text);
}

void apiToolState(void* c, const char* json)
{
    if (LevelEditorLink* l = ctxOf(c).levelEditor)
        l->toolState(json);
}

int32_t apiElementsGet(void* c, uint64_t* entity, uint32_t* mode, uint64_t* items, int32_t capacity)
{
    uint64_t e = 0;
    uint32_t m = 0;
    const int32_t n = ctxOf(c).levelEditor ? ctxOf(c).levelEditor->elementsGet(e, m, items, items ? capacity : 0) : 0;
    if (entity)
        *entity = e;
    if (mode)
        *mode = m;
    return n;
}

void apiElementsSet(void* c, uint64_t entity, uint32_t mode, const uint64_t* items, int32_t count)
{
    if (LevelEditorLink* l = ctxOf(c).levelEditor)
        l->elementsSet(entity, mode, items, items ? count : 0);
}

int32_t apiMeshOp(void*, const char* op, const uint8_t* blob, int32_t size, const char* argsJson, uint8_t* buffer, int32_t capacity)
{
    if (!op)
        return 0;
    level::PolyMesh in;
    if (blob && size > 0 && !in.load(blob, size_t(size)))
        return 0;
    Json args;
    try {
        args = Json::parse(argsJson && *argsJson ? argsJson : "{}");
    } catch (const std::exception&) {
        return 0;
    }
    level::OpResult out;
    const bool ok = level::runOp(op, in, args, out);
    const std::vector<uint8_t> mesh = ok ? out.mesh.save() : std::vector<uint8_t>{};
    const std::vector<uint8_t> detached = ok && out.detached ? out.detached->save() : std::vector<uint8_t>{};
    JsonWriter w;
    w.beginObject().value("ok", ok).value("mode", out.selectionMode).beginArray("selection");
    for (const uint64_t item : out.selection)
        w.value(nullptr, item);
    w.endArray().value("error", out.error).endObject();
    const std::string& json = w.str();
    const size_t needed = 8 + mesh.size() + detached.size() + json.size();
    if (!buffer || needed > size_t(std::max(capacity, 0)))
        return -int32_t(needed);
    uint8_t* p = buffer;
    const auto put = [&](const void* data, size_t n) {
        if (n)
            std::memcpy(p, data, n);
        p += n;
    };
    const uint32_t meshSize = uint32_t(mesh.size()), detachedSize = uint32_t(detached.size());
    put(&meshSize, 4);
    put(mesh.data(), mesh.size());
    put(&detachedSize, 4);
    put(detached.data(), detached.size());
    put(json.data(), json.size());
    return int32_t(needed);
}

int32_t apiNavPath(void*, const float* from, const float* to, float* out, int32_t capacity)
{
    const auto nav = level::navigation();
    if (!nav || !from || !to)
        return 0;
    std::vector<float3> path;
    const bool whole = nav->findPath({from[0], from[1], from[2]}, {to[0], to[1], to[2]}, path);
    const int32_t n = std::min(int32_t(path.size()), std::max(capacity, 0));
    for (int32_t i = 0; i < n && out; ++i) {
        out[i * 3] = path[size_t(i)].x;
        out[i * 3 + 1] = path[size_t(i)].y;
        out[i * 3 + 2] = path[size_t(i)].z;
    }
    return whole ? n : -n;
}

int32_t apiNavRaycast(void*, const float* from, const float* to, float* hit)
{
    const auto nav = level::navigation();
    if (!nav || !from || !to)
        return -1;
    float t;
    float3 at;
    const bool stopped = nav->raycast({from[0], from[1], from[2]}, {to[0], to[1], to[2]}, t, at);
    if (hit) {
        hit[0] = at.x;
        hit[1] = at.y;
        hit[2] = at.z;
    }
    return stopped ? 1 : 0;
}

int32_t apiNavNearest(void*, const float* point, const float* extents, float* out)
{
    const auto nav = level::navigation();
    if (!nav)
        return -1;
    if (!point)
        return 0;
    float3 at;
    const float3 reach = extents ? float3{extents[0], extents[1], extents[2]} : float3{2.0f, 4.0f, 2.0f};
    if (!nav->nearest({point[0], point[1], point[2]}, at, reach))
        return 0;
    if (out) {
        out[0] = at.x;
        out[1] = at.y;
        out[2] = at.z;
    }
    return 1;
}

int32_t apiVolumeKind(void* c, uint64_t entity, char* tag, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (!x.scene || !x.scene->world().is_alive(entity))
        return -1;
    const LevelVolume* v = flecs::entity(x.scene->world(), entity).try_get<LevelVolume>();
    if (!v)
        return -1;
    if (tag && capacity > 0) {
        const size_t n = std::min(v->tag.size(), size_t(capacity - 1));
        std::memcpy(tag, v->tag.data(), n);
        tag[n] = 0;
    }
    return int32_t(v->kind);
}

int32_t apiVolumesAt(void* c, const float* point, uint64_t* entities, int32_t* kinds, int32_t capacity)
{
    Context& x = ctxOf(c);
    if (!x.scene || !point)
        return 0;
    int32_t n = 0;
    x.scene->world().each([&](flecs::entity e, const LevelVolume& v, const LevelMesh& m, const WorldTransform& t) {
        if (!m.mesh || m.mesh->empty())
            return;
        // The point in its space, inside its mesh's bounds.
        const float* r = t.rows;
        const float a = r[0], b = r[1], cc = r[2], d = r[4], ee = r[5], f = r[6], g = r[8], h = r[9], i = r[10];
        const float det = a * (ee * i - f * h) - b * (d * i - f * g) + cc * (d * h - ee * g);
        if (std::fabs(det) < 1e-12f)
            return;
        const float px = point[0] - r[3], py = point[1] - r[7], pz = point[2] - r[11], s = 1.0f / det;
        const float3 local = {((ee * i - f * h) * px + (cc * h - b * i) * py + (b * f - cc * ee) * pz) * s,
                              ((f * g - d * i) * px + (a * i - cc * g) * py + (cc * d - a * f) * pz) * s,
                              ((d * h - ee * g) * px + (b * g - a * h) * py + (a * ee - b * d) * pz) * s};
        float3 lo, hi;
        m.mesh->bounds(lo, hi);
        if (local.x < lo.x || local.y < lo.y || local.z < lo.z || local.x > hi.x || local.y > hi.y || local.z > hi.z)
            return;
        if (n < capacity && entities && kinds) {
            entities[n] = e.id();
            kinds[n] = int32_t(v.kind);
        }
        ++n;
    });
    return n;
}

int32_t apiCrowdAdd(void*, const float* p, float radius, float height, float speed, float acceleration)
{
    level::NavCrowd* crowd = level::navCrowd();
    return crowd && p ? crowd->add({p[0], p[1], p[2]}, radius, height, speed, acceleration) : -1;
}

void apiCrowdRemove(void*, int32_t id)
{
    if (level::NavCrowd* crowd = level::navCrowd())
        crowd->remove(id);
}

int32_t apiCrowdTarget(void*, int32_t id, const float* to)
{
    level::NavCrowd* crowd = level::navCrowd();
    if (!crowd)
        return 0;
    if (!to) {
        crowd->stop(id);
        return 1;
    }
    return crowd->target(id, {to[0], to[1], to[2]}) ? 1 : 0;
}

void apiCrowdUpdate(void*, float dt)
{
    if (level::NavCrowd* crowd = level::navCrowd())
        crowd->update(dt);
}

float apiCrowdState(void*, int32_t id, float* position, float* velocity)
{
    level::NavCrowd* crowd = level::navCrowd();
    float3 p, v;
    const float left = crowd ? crowd->state(id, p, v) : -1.0f;
    if (left >= 0.0f && position && velocity) {
        position[0] = p.x, position[1] = p.y, position[2] = p.z;
        velocity[0] = v.x, velocity[1] = v.y, velocity[2] = v.z;
    }
    return left;
}

int32_t apiCutSet(void* c, uint64_t entity, int32_t shape, int32_t intersect, uint32_t seed, float roughness, const char* cap)
{
    Context& x = ctxOf(c);
    if (!x.scene || !x.scene->world().is_alive(entity))
        return 0;
    flecs::entity e(x.scene->world(), entity);
    if (shape < 0 || shape >= int32_t(level::kCutShapes)) {
        e.remove<CutVolume>();
        return 1;
    }
    CutVolume v;
    v.shape = level::CutShape(shape);
    v.intersect = intersect != 0;
    v.seed = seed;
    v.roughness = std::clamp(roughness, 0.0f, 0.9f);
    v.capMaterial = cap ? cap : "";
    e.set<CutVolume>(v);
    return 1;
}

uint64_t apiGeometryHash(void*)
{
    const LevelMeshes* m = LevelMeshes::current();
    return m ? m->geometryHash() : 0;
}

int32_t apiCutsPending(void*)
{
    const LevelMeshes* m = LevelMeshes::current();
    return m ? int32_t(m->cutsPending()) : 0;
}

void apiModifierHandler(void*, FxModifierHandler handler)
{
    level::setScriptModifierHandler(handler);
}

}  // namespace

void fillLevelApi(FxLevelApi& api, ScriptHost::Context* context)
{
    api = {};
    api.version = kFxLevelApiVersion;
    api.size = sizeof(FxLevelApi);
    api.context = context;
    api.liveCreate = apiLiveCreate;
    api.liveSet = apiLiveSet;
    api.liveDestroy = apiLiveDestroy;
    api.liveOf = apiLiveOf;
    api.liveTriangle = apiLiveTriangle;
    api.liveStats = apiLiveStats;
    api.meshGet = apiMeshGet;
    api.meshSet = apiMeshSet;
    api.meshCreate = apiMeshCreate;
    api.viewport = apiViewport;
    api.viewportCapture = apiViewportCapture;
    api.viewportHint = apiViewportHint;
    api.elementsGet = apiElementsGet;
    api.elementsSet = apiElementsSet;
    api.meshSetLive = apiMeshSetLive;
    api.meshCommit = apiMeshCommit;
    api.meshOp = apiMeshOp;
    api.modifierHandler = apiModifierHandler;
    api.navPath = apiNavPath;
    api.navRaycast = apiNavRaycast;
    api.navNearest = apiNavNearest;
    api.volumeKind = apiVolumeKind;
    api.volumesAt = apiVolumesAt;
    api.crowdAdd = apiCrowdAdd;
    api.crowdRemove = apiCrowdRemove;
    api.crowdTarget = apiCrowdTarget;
    api.crowdUpdate = apiCrowdUpdate;
    api.crowdState = apiCrowdState;
    api.cutSet = apiCutSet;
    api.geometryHash = apiGeometryHash;
    api.cutsPending = apiCutsPending;
    api.toolState = apiToolState;
}

void levelUpdate(ScriptHost::Context& x)
{
    if (!x.live || x.liveEntities.empty())
        return;
    std::vector<int32_t> gone;
    for (const auto& [live, e] : x.liveEntities)
        if (!x.scene->world().is_alive(e))
            gone.push_back(live);
    for (const int32_t live : gone)
        freeLive(x, live, false);
}

void levelGameEnded(ScriptHost::Context& x)
{
    if (!x.live)
        return;
    while (!x.liveEntities.empty())
        freeLive(x, x.liveEntities.begin()->first, true);
}

}  // namespace fx
