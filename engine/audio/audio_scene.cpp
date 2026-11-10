#include "audio/audio_scene.h"

#include "assets/scene_pak.h"
#include "audio/steam_audio.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/material_library.h"
#include "scene/scene.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "terrain/terrain_scatter.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

#if FALCON_STEAM_AUDIO
#include <box3d/collision.h>
#include <phonon.h>
#endif

namespace fx {

#if FALCON_STEAM_AUDIO

namespace {

constexpr uint32_t kMaxSources = 64;
constexpr uint32_t kTerrainStep = 4;        // samples per audio quad (2 m at 0.5 m spacing)
constexpr float kTerrainRadius = 256.0f;     // tiles within this of the listener
constexpr uint32_t kResolveEvery = 15;       // frames between materials resolved again (moves: every frame)

// Materials by value (quantized): a changed one shows as another key.
struct MaterialKey {
    int32_t v[7];
    bool operator<(const MaterialKey& o) const { return std::memcmp(v, o.v, sizeof(v)) < 0; }
    bool operator==(const MaterialKey& o) const { return std::memcmp(v, o.v, sizeof(v)) == 0; }
};
MaterialKey keyOf(const AcousticParams& p)
{
    const float f[7] = {p.absorption[0], p.absorption[1], p.absorption[2], p.scattering, p.transmission[0], p.transmission[1], p.transmission[2]};
    MaterialKey k;
    for (int i = 0; i < 7; ++i)
        k.v[i] = int32_t(std::lround(double(f[i]) * 10000.0));
    return k;
}
IPLMaterial iplMaterial(const AcousticParams& p)
{
    IPLMaterial m{};
    for (int b = 0; b < 3; ++b) {
        m.absorption[b] = p.absorption[b];
        m.transmission[b] = p.transmission[b];
    }
    m.scattering = p.scattering;
    return m;
}
IPLVector3 v3(float3 v) { return {v.x, v.y, v.z}; }

// Fingerprints: FNV-1a over bytes, mixed (splitmix64) so that sums of them stay spread out.
uint64_t fnv(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
{
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
int32_t millimetres(float v) { return int32_t(std::lround(double(v) * 1000.0)); }

struct Triangles {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
};

// The built-in unit cube and a coarse unit-diameter ball (their physics shapes).
void cubeTriangles(Triangles& t)
{
    for (int i = 0; i < 8; ++i)
        t.vertices.push_back({(i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f});
    const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        t.triangles.push_back({{f[0], f[1], f[2]}});
        t.triangles.push_back({{f[0], f[2], f[3]}});
    }
}
void ballTriangles(Triangles& t)
{
    const int rings = 6, segments = 10;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const float th = 3.14159265f * float(r) / float(rings), ph = 6.2831853f * float(s) / float(segments);
            t.vertices.push_back({0.5f * std::sin(th) * std::cos(ph), 0.5f * std::cos(th), 0.5f * std::sin(th) * std::sin(ph)});
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const int a = r * segments + s, b = r * segments + (s + 1) % segments;
            const int c = a + segments, d = b + segments;
            t.triangles.push_back({{a, c, b}});
            t.triangles.push_back({{b, c, d}});
        }
}

}  // namespace

struct AudioScene::Impl {
    IPLContext context = nullptr;
    IPLScene scene = nullptr;
    IPLSimulator simulator = nullptr;
    // Steam Audio's own ray tracer while the game runs: Intel's Embree (inside phonon.dll) keeps a
    // thread per core spinning (~9 of 12 cores, idle or not) for a few tenths of a millisecond per
    // reflections run. Bakes make their own Embree device for as long as they run (audio_bake.cpp).
    IPLSceneSettings sceneSettings() const
    {
        IPLSceneSettings ss{};
        ss.type = IPL_SCENETYPE_DEFAULT;
        return ss;
    }
    MaterialResolver resolver;
    bool sceneDirty = false, simulatorDirty = false;
    // Reflections: their thread, which holds `simulation` while it runs (commits, adds and removes
    // wait for it: try_lock, else next frame); `snapshot` guards what it reads of the sources.
    bool reflections = false;
    SteamAudio* steam = nullptr;
    IPLSource listener = nullptr;
    ListenerPose listenerPose;
    std::thread thread;
    std::mutex simulation, snapshot, wake;
    std::condition_variable wakeup;
    bool stop = false;
    std::atomic<uint32_t> quality{0};
    std::atomic<float> reflectionsMs{0.0f};
    std::atomic<uint32_t> reflectionRuns{0};
    bool synchronous = false;
    uint64_t updates = 0;

    // Direct simulation: on a thread of its own, a frame behind the main thread (in step when
    // synchronous). The main thread hands it a run only while it is idle (`directBusy` false under
    // `direct`): then it takes the last run's results, commits, and hands it the sources as they
    // are now. Commits and adds and removes of sources happen only while it is idle.
    struct DirectItem {
        IPLSource source;
        uint32_t index;
        float3 position;
        bool occlusion, transmission;
        float out[4];
    };
    std::thread directThread;
    std::mutex direct;
    std::condition_variable directWake, directDone;
    bool directBusy = false, directStop = false;
    std::vector<DirectItem> directItems;  // the run's sources and (after it) what it found
    IPLCoordinateSpace3 directListener{};
    std::atomic<float> directMs{0.0f};

    ScenePak* pak = nullptr;
    Scene* ecs = nullptr;
    MaterialLibrary* materials = nullptr;
    const Terrain* terrain = nullptr;
    std::unique_ptr<flecs::query<const MeshInstance, const WorldTransform>> meshes;
    uint64_t frame = 0;
    bool resolveAll = true;

    std::unordered_map<uint32_t, Triangles> triangles;  // by mesh, in its own space (empty: no shape)
    // A placed mesh: its triangles in world space, a static mesh of its own; one that changes
    // material is placed again. One that has moved since it was placed (a held weapon, a door, an
    // occluding body) is instanced from then on: its triangles in their own space, in a scene of
    // their own, placed by a transform, so moving it costs a transform and the next commit instead
    // of a new mesh (a 4400-triangle pistol in a hand: 3.3 ms a frame). (Steam Audio's Embree scenes
    // lose instanced meshes added after one was removed; the runtime's own tracer does not use Embree.)
    struct Placed {
        IPLStaticMesh mesh = nullptr;
        IPLScene sub = nullptr;  // instanced: its scene, mesh and instance
        IPLStaticMesh local = nullptr;
        IPLInstancedMesh instance = nullptr;
        bool placed() const { return mesh || instance; }
        uint32_t meshIndex = ~0u;
        MaterialKey key = {};
        AcousticParams material;
        float rows[12] = {};
        uint32_t triangles = 0;
        uint64_t seen = 0;
        bool moving = false;  // a body (an occluder only while it moves): not baked
        float3 lo = {}, hi = {};  // world bounds (placed)
        uint32_t liveRevision = 0;  // a live mesh's (render/live_geometry.h) when placed: newer places it again
    };
    // Live meshes' revisions the cached triangles were made from; ScenePak::liveGeneration() then.
    std::unordered_map<uint32_t, uint32_t> liveRevisions;
    uint32_t liveGeneration = 0;
    std::unordered_map<uint32_t, uint64_t> meshHashes;  // by mesh: its triangles' fingerprint
    uint64_t geometryRevision = 0;
    bool hadTerrain = false;
    // Baked data (under `simulation`: the reflections runs read it), and what the last run did.
    std::unique_ptr<AudioBakedData> baked;
    std::atomic<bool> listenerBaked{false};
    std::atomic<uint32_t> pathed{0};
    std::unordered_map<flecs::entity_t, Placed> entities;
    std::unordered_map<uint64_t, std::vector<Placed>> scatter;

    struct Tile {
        IPLStaticMesh mesh = nullptr;
        uint32_t triangles = 0;
        bool stale = false;
    };
    std::vector<Tile> tiles;
    uint64_t terrainRevision = 0;
    std::string layersJson;
    std::vector<AcousticParams> layerAcoustics;

    struct Source {
        IPLSource source = nullptr;
        bool used = false, added = false, removing = false;  // (added / removed between reflection runs)
        bool occlusion = true, transmission = true, reflections = false, pathing = false;
        float3 position = {};
        float out[4] = {1, 1, 1, 1};
        bool pathed = false;  // the last run found its paths (eq: what they let through)
        float path[3] = {0, 0, 0};
    };
    std::vector<Source> sources;
    AudioSceneStats stats;

    const Triangles& trianglesOf(uint32_t mesh)
    {
        auto [it, added] = triangles.try_emplace(mesh);
        if (!added)
            return it->second;
        Triangles& t = it->second;
        const uint32_t* builtins = pak->header().builtinMeshes;
        if (const b3MeshData* c = pak->staticCollider(mesh)) {
            const b3Vec3* v = b3GetMeshVertices(c);
            const b3MeshTriangle* tri = b3GetMeshTriangles(c);
            for (int i = 0; i < c->vertexCount; ++i)
                t.vertices.push_back({v[i].x, v[i].y, v[i].z});
            for (int i = 0; i < c->triangleCount; ++i)
                t.triangles.push_back({{tri[i].index1, tri[i].index2, tri[i].index3}});
        } else if (mesh == builtins[uint32_t(fpak::Builtin::Cube)]) {
            cubeTriangles(t);
        } else if (mesh == builtins[uint32_t(fpak::Builtin::Sphere)]) {
            ballTriangles(t);
        }
        return t;
    }
    uint64_t meshHash(uint32_t mesh)
    {
        if (auto it = meshHashes.find(mesh); it != meshHashes.end())
            return it->second;
        const Triangles& t = trianglesOf(mesh);
        uint64_t h = fnv(nullptr, 0);
        for (const IPLVector3& v : t.vertices) {
            const int32_t q[3] = {millimetres(v.x), millimetres(v.y), millimetres(v.z)};
            h = fnv(q, sizeof(q), h);
        }
        h = fnv(t.triangles.data(), t.triangles.size() * sizeof(IPLTriangle), h);
        return meshHashes[mesh] = h;
    }

    // Meshes taken out of the scene are kept, not released, while it lives: with Embree, releasing
    // one leaves meshes added later unseen by occlusion rays (Steam Audio 4.8.1). When many have
    // piled up, the scene is built afresh and the old one released whole (compact()).
    std::vector<IPLStaticMesh> retiredMeshes;
    struct RetiredInstance {
        IPLInstancedMesh instance;
        IPLScene sub;
        IPLStaticMesh local;
    };
    std::vector<RetiredInstance> retiredInstances;
    uint64_t retiredTriangles = 0;
    std::vector<flecs::entity_t> moved;  // instanced entities moved since the last commit
    void retire()  // (after a commit, under the simulation lock)
    {
        if (retiredMeshes.size() + retiredInstances.size() > 256 || retiredTriangles > 1000000)
            compact();
    }
    static void release(RetiredInstance& r)
    {
        if (r.instance)
            iplInstancedMeshRelease(&r.instance);
        iplStaticMeshRelease(&r.local);
        iplSceneRelease(&r.sub);
    }
    void compact()
    {
        IPLSceneSettings ss = sceneSettings();
        IPLScene fresh = nullptr;
        if (iplSceneCreate(context, &ss, &fresh) != IPL_STATUS_SUCCESS)
            return;
        IPLScene old = scene;
        std::vector<IPLStaticMesh> oldMeshes;
        oldMeshes.swap(retiredMeshes);
        std::vector<RetiredInstance> oldInstances;
        oldInstances.swap(retiredInstances);
        std::vector<IPLInstancedMesh> oldInstanced;
        scene = fresh;
        const auto again = [&](Placed& p) {
            if (p.instance) {  // (its own scene kept: instanced again in the new one)
                oldInstanced.push_back(p.instance);
                p.instance = nullptr;
                if (!instance(p)) {
                    retiredInstances.push_back({nullptr, p.sub, p.local});
                    p.sub = nullptr;
                    p.local = nullptr;
                    p.triangles = 0;
                }
                return;
            }
            if (p.mesh)
                oldMeshes.push_back(p.mesh);
            p.mesh = nullptr;
            p.triangles = 0;
            place(p, p.meshIndex, p.material, p.rows);
        };
        for (auto& [id, p] : entities)
            again(p);
        for (auto& [cell, list] : scatter)
            for (Placed& p : list)
                again(p);
        for (Tile& t : tiles)  // (built again over the next frames)
            if (t.mesh) {
                oldMeshes.push_back(t.mesh);
                t.mesh = nullptr;
                t.triangles = 0;
                t.stale = true;
            }
        moved.clear();  // (instanced where they are now)
        iplSceneCommit(scene);
        iplSimulatorSetScene(simulator, scene);
        iplSimulatorCommit(simulator);
        for (IPLInstancedMesh& mesh : oldInstanced)
            iplInstancedMeshRelease(&mesh);
        for (RetiredInstance& r : oldInstances)
            release(r);
        for (IPLStaticMesh& mesh : oldMeshes)
            iplStaticMeshRelease(&mesh);
        iplSceneRelease(&old);
        retiredTriangles = 0;
        sceneDirty = simulatorDirty = false;
    }
    // Its world bounds at `rows`, and (world: not null) its vertices there.
    static void bounds(Placed& p, const Triangles& t, const float rows[12], std::vector<IPLVector3>* world)
    {
        if (world)
            world->resize(t.vertices.size());
        p.lo = {1e30f, 1e30f, 1e30f};
        p.hi = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < t.vertices.size(); ++i) {
            const IPLVector3 v = t.vertices[i];
            const IPLVector3 w = {rows[0] * v.x + rows[1] * v.y + rows[2] * v.z + rows[3], rows[4] * v.x + rows[5] * v.y + rows[6] * v.z + rows[7],
                                  rows[8] * v.x + rows[9] * v.y + rows[10] * v.z + rows[11]};
            if (world)
                (*world)[i] = w;
            p.lo = {std::min(p.lo.x, w.x), std::min(p.lo.y, w.y), std::min(p.lo.z, w.z)};
            p.hi = {std::max(p.hi.x, w.x), std::max(p.hi.y, w.y), std::max(p.hi.z, w.z)};
        }
    }
    static IPLMatrix4x4 matrixOf(const float rows[12])
    {
        IPLMatrix4x4 m{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                m.elements[r][c] = rows[r * 4 + c];
        m.elements[3][3] = 1.0f;
        return m;
    }
    // instanced: in a scene of its own (its triangles as they are), placed by `rows`.
    bool place(Placed& p, uint32_t mesh, const AcousticParams& material, const float rows[12], bool instanced = false)
    {
        p.meshIndex = mesh;
        p.key = keyOf(material);
        p.material = material;
        std::memcpy(p.rows, rows, sizeof(p.rows));
        const Triangles& t = trianglesOf(mesh);
        if (t.triangles.empty())
            return false;  // (no shape: sound passes)
        std::vector<IPLVector3> world;
        bounds(p, t, rows, instanced ? nullptr : &world);
        IPLMaterial m = iplMaterial(material);
        std::vector<IPLint32> indices(t.triangles.size(), 0);
        IPLStaticMeshSettings ms{};
        ms.numVertices = IPLint32(t.vertices.size());
        ms.numTriangles = IPLint32(t.triangles.size());
        ms.numMaterials = 1;
        ms.vertices = instanced ? const_cast<IPLVector3*>(t.vertices.data()) : world.data();
        ms.triangles = const_cast<IPLTriangle*>(t.triangles.data());
        ms.materialIndices = indices.data();
        ms.materials = &m;
        if (instanced) {
            IPLSceneSettings ss = sceneSettings();
            if (iplSceneCreate(context, &ss, &p.sub) != IPL_STATUS_SUCCESS) {
                p.sub = nullptr;
                return false;
            }
            if (iplStaticMeshCreate(p.sub, &ms, &p.local) != IPL_STATUS_SUCCESS) {
                p.local = nullptr;
                iplSceneRelease(&p.sub);
                return false;
            }
            iplStaticMeshAdd(p.local, p.sub);
            iplSceneCommit(p.sub);
            if (!instance(p)) {
                iplStaticMeshRelease(&p.local);
                iplSceneRelease(&p.sub);
                return false;
            }
        } else {
            if (iplStaticMeshCreate(scene, &ms, &p.mesh) != IPL_STATUS_SUCCESS) {
                p.mesh = nullptr;
                return false;
            }
            iplStaticMeshAdd(p.mesh, scene);
        }
        p.triangles = uint32_t(t.triangles.size());
        sceneDirty = true;
        return true;
    }
    bool instance(Placed& p)  // (its own scene made): instanced in `scene` at its rows
    {
        IPLInstancedMeshSettings is{};
        is.subScene = p.sub;
        is.transform = matrixOf(p.rows);
        if (iplInstancedMeshCreate(scene, &is, &p.instance) != IPL_STATUS_SUCCESS) {
            p.instance = nullptr;
            return false;
        }
        iplInstancedMeshAdd(p.instance, scene);
        sceneDirty = true;
        return true;
    }
    // A placed entity moved: an instance's transform (applied at the next commit); a static mesh
    // instanced from now on.
    void move(flecs::entity_t id, Placed& p, const float rows[12])
    {
        if (p.instance) {
            std::memcpy(p.rows, rows, sizeof(p.rows));
            bounds(p, trianglesOf(p.meshIndex), rows, nullptr);
            moved.push_back(id);
            sceneDirty = true;
        } else if (p.mesh) {
            const AcousticParams material = p.material;
            unplace(p);
            place(p, p.meshIndex, material, rows, true);
        } else {
            std::memcpy(p.rows, rows, sizeof(p.rows));  // (no shape)
        }
    }
    void unplace(Placed& p)
    {
        if (p.instance) {
            iplInstancedMeshRemove(p.instance, scene);
            retiredInstances.push_back({p.instance, p.sub, p.local});
            p.instance = nullptr;
            p.sub = nullptr;
            p.local = nullptr;
        } else if (p.mesh) {
            iplStaticMeshRemove(p.mesh, scene);
            retiredMeshes.push_back(p.mesh);
            p.mesh = nullptr;
        } else {
            return;
        }
        retiredTriangles += p.triangles;
        p.triangles = 0;
        sceneDirty = true;
    }

    // An entity's acoustic material: its own (AcousticSurface), else its .fmat's, else generic.
    AcousticParams materialOf(flecs::entity e, const MeshInstance& mi)
    {
        AcousticParams p;
        if (const AcousticSurface* a = e.try_get<AcousticSurface>(); a && (a->flags & 2u)) {
            std::memcpy(p.absorption, a->absorption, sizeof(p.absorption));
            p.scattering = a->scattering;
            std::memcpy(p.transmission, a->transmission, sizeof(p.transmission));
            return p;
        }
        const uint32_t m = mi.gpuSlot < pak->instances().size() ? pak->instances()[mi.gpuSlot].flags >> fpak::kInstanceMaterialShift : 0u;
        if (m && materials && resolver) {
            const std::string source = materials->sourceOf(m - 1);
            if (!source.empty() && resolver(source, p))
                return p;
        }
        acousticPreset("generic", p);
        return p;
    }

    void syncEntities()
    {
        if (!meshes)
            return;
        ++frame;
        // Live meshes changed since: their cached triangles are made again.
        if (pak->liveGeneration() != liveGeneration) {
            liveGeneration = pak->liveGeneration();
            for (auto it = liveRevisions.begin(); it != liveRevisions.end();)
                if (pak->liveRevision(it->first) != it->second) {
                    triangles.erase(it->first);
                    meshHashes.erase(it->first);
                    it = liveRevisions.erase(it);
                } else {
                    ++it;
                }
        }
        const bool resolve = resolveAll || frame % kResolveEvery == 0;
        resolveAll = false;
        meshes->each([&](flecs::entity e, const MeshInstance& mi, const WorldTransform& w) {
            const AcousticSurface* a = e.try_get<AcousticSurface>();
            const bool hidden = mi.gpuSlot < pak->instances().size() && (pak->instances()[mi.gpuSlot].flags & fpak::kInstanceHidden);
            const bool moving = e.has<RigidBody>() || e.has<PhysicsBody>();
            if (hidden || (a && (a->flags & 1u)) || (moving && !(a && (a->flags & 4u))))
                return;  // (unseen this frame: taken out below)
            auto [it, added] = entities.try_emplace(e.id());
            Placed& p = it->second;
            p.seen = frame;
            const uint32_t live = pak->liveRevision(mi.mesh);  // (0: not a live mesh)
            const bool changed = added || p.meshIndex != mi.mesh || p.moving != moving || p.liveRevision != live;
            const bool moves = !changed && std::memcmp(p.rows, w.rows, sizeof(p.rows)) != 0;
            if (!changed && !resolve) {
                if (moves) {
                    move(e.id(), p, w.rows);
                    ++geometryRevision;
                }
                return;
            }
            const AcousticParams material = materialOf(e, mi);
            if (!changed && keyOf(material) == p.key) {
                if (moves) {
                    move(e.id(), p, w.rows);
                    ++geometryRevision;
                }
                return;
            }
            const bool instanced = !added && (p.instance || moves);  // (one that has moved stays instanced)
            unplace(p);
            place(p, mi.mesh, material, w.rows, instanced);
            p.moving = moving;
            p.liveRevision = live;
            if (pak->liveSlots().ownsMesh(mi.mesh))
                liveRevisions[mi.mesh] = live;
            ++geometryRevision;
        });
        for (auto it = entities.begin(); it != entities.end();)
            if (it->second.seen != frame) {
                unplace(it->second);
                it = entities.erase(it);
                ++geometryRevision;
            } else {
                ++it;
            }
    }

    // The terrain layers' acoustics (each its preset, else its look's).
    static std::vector<AcousticParams> acousticsOf(const std::string& json)
    {
        std::vector<AcousticParams> out;
        for (const TerrainLayer& l : parseTerrainLayers(json)) {
            AcousticParams p;
            if (!acousticPreset(l.acoustic, p))
                acousticPreset(acousticPresetForLook(l.generated), p);
            out.push_back(p);
        }
        return out;
    }
    void refreshLayers()
    {
        layersJson = terrain->layersJson();
        layerAcoustics = acousticsOf(layersJson);
    }
    // A terrain sample's layer (the heavier of its two), clamped to the `count` there are.
    static uint32_t layerOf(uint32_t m, size_t count)
    {
        const uint32_t layer = terrain::materialWeightB(m) > 127 ? terrain::materialB(m) : terrain::materialA(m);
        return uint32_t(std::min<size_t>(layer, count ? count - 1 : 0));
    }

    // The terrain's coarse samples under a box (clamped to the map); false: none (no terrain, or
    // the box beside it).
    bool coarseRect(float3 lo, float3 hi, uint32_t& x0, uint32_t& z0, uint32_t& x1, uint32_t& z1) const
    {
        if (!terrain || !terrain->valid())
            return false;
        const TerrainSettings& s = terrain->settings();
        const float step = s.spacing * float(terrain::kCoarseStep), last = float(terrain->coarseSide() - 1);
        const float fx0 = std::floor((lo.x - s.originX) / step), fx1 = std::ceil((hi.x - s.originX) / step);
        const float fz0 = std::floor((lo.z - s.originZ) / step), fz1 = std::ceil((hi.z - s.originZ) / step);
        if (fx1 < 0.0f || fz1 < 0.0f || fx0 > last || fz0 > last)
            return false;
        x0 = uint32_t(std::clamp(fx0, 0.0f, last));
        x1 = uint32_t(std::clamp(fx1, 0.0f, last));
        z0 = uint32_t(std::clamp(fz0, 0.0f, last));
        z1 = uint32_t(std::clamp(fz1, 0.0f, last));
        return x1 > x0 && z1 > z0;
    }

    void dropTile(Tile& t)
    {
        if (!t.mesh)
            return;
        iplStaticMeshRemove(t.mesh, scene);
        retiredMeshes.push_back(t.mesh);
        retiredTriangles += t.triangles;
        t.mesh = nullptr;
        t.triangles = 0;
        sceneDirty = true;
    }

    void syncTerrain(float3 focus)
    {
        if (!terrain || !terrain->valid()) {
            for (Tile& t : tiles)
                dropTile(t);
            tiles.clear();
            if (hadTerrain)
                ++geometryRevision;
            hadTerrain = false;
            return;
        }
        if (!hadTerrain)
            ++geometryRevision;
        hadTerrain = true;
        const uint32_t T = terrain->tilesPerSide();
        if (tiles.size() != size_t(T) * T) {
            for (Tile& t : tiles)
                dropTile(t);
            tiles.assign(size_t(T) * T, Tile{});
        }
        // Layers' acoustics, and edits since: tiles built again (one a frame).
        if (terrain->layersJson() != layersJson) {
            refreshLayers();
            for (Tile& t : tiles)
                t.stale = true;
            ++geometryRevision;
        }
        if (terrain->revision() != terrainRevision) {
            ++geometryRevision;
            std::vector<TerrainChange> changed;
            const bool all = !terrain->changesSince(terrainRevision, changed);
            terrainRevision = terrain->revision();
            for (const TerrainChange& c : changed) {
                const TerrainRect& r = c.rect;
                const uint32_t tx0 = r.x0 > 0 ? uint32_t(r.x0 - 1) / terrain::kTileQuads : 0;
                const uint32_t tz0 = r.z0 > 0 ? uint32_t(r.z0 - 1) / terrain::kTileQuads : 0;
                for (uint32_t tz = tz0; tz <= std::min(uint32_t(r.z1) / terrain::kTileQuads, T - 1); ++tz)
                    for (uint32_t tx = tx0; tx <= std::min(uint32_t(r.x1) / terrain::kTileQuads, T - 1); ++tx)
                        tiles[tz * T + tx].stale = true;
            }
            if (all)
                for (Tile& t : tiles)
                    t.stale = true;
        }
        const TerrainSettings& s = terrain->settings();
        const float tileSize = float(terrain::kTileQuads) * s.spacing;
        bool built = false;
        std::vector<float> heights;
        std::vector<uint32_t> mats;
        for (uint32_t tz = 0; tz < T; ++tz)
            for (uint32_t tx = 0; tx < T; ++tx) {
                Tile& t = tiles[tz * T + tx];
                const float x0 = s.originX + float(tx) * tileSize, z0 = s.originZ + float(tz) * tileSize;
                const float dx = std::max({x0 - focus.x, 0.0f, focus.x - (x0 + tileSize)});
                const float dz = std::max({z0 - focus.z, 0.0f, focus.z - (z0 + tileSize)});
                if (std::sqrt(dx * dx + dz * dz) > kTerrainRadius || !terrain->tileResident(tx, tz)) {
                    dropTile(t);
                    continue;
                }
                if ((t.mesh && !t.stale) || built)
                    continue;
                built = true;  // (one a frame: a tile is ~8000 triangles)
                dropTile(t);
                t.stale = false;
                const TerrainRect rect = {int32_t(tx * terrain::kTileQuads), int32_t(tz * terrain::kTileQuads),
                                          int32_t((tx + 1) * terrain::kTileQuads), int32_t((tz + 1) * terrain::kTileQuads)};
                terrain->read(rect, heights);
                terrain->readMaterials(rect, mats);
                const uint32_t n = terrain::kTileQuads / kTerrainStep + 1;
                std::vector<IPLVector3> vertices;
                vertices.reserve(size_t(n) * n);
                for (uint32_t z = 0; z < n; ++z)
                    for (uint32_t x = 0; x < n; ++x) {
                        const uint32_t sx = x * kTerrainStep, sz = z * kTerrainStep;
                        vertices.push_back({x0 + float(sx) * s.spacing, heights[size_t(sz) * terrain::kTileSamples + sx], z0 + float(sz) * s.spacing});
                    }
                std::vector<IPLTriangle> tris;
                std::vector<IPLint32> materialIndex;
                for (uint32_t z = 0; z + 1 < n; ++z)
                    for (uint32_t x = 0; x + 1 < n; ++x) {
                        const uint32_t m = mats[size_t(z * kTerrainStep) * terrain::kTileSamples + x * kTerrainStep];
                        if (m & terrain::kHole)
                            continue;
                        const IPLint32 mi = IPLint32(layerOf(m, layerAcoustics.size()));
                        const IPLint32 a = IPLint32(z * n + x), b = a + 1, c = a + IPLint32(n), d = c + 1;
                        tris.push_back({{a, c, b}});
                        tris.push_back({{b, c, d}});
                        materialIndex.push_back(mi);
                        materialIndex.push_back(mi);
                    }
                if (tris.empty())
                    continue;
                std::vector<IPLMaterial> materialsOut;
                for (const AcousticParams& p : layerAcoustics)
                    materialsOut.push_back(iplMaterial(p));
                if (materialsOut.empty()) {
                    AcousticParams p;
                    acousticPreset("generic", p);
                    materialsOut.push_back(iplMaterial(p));
                }
                IPLStaticMeshSettings ms{};
                ms.numVertices = IPLint32(vertices.size());
                ms.numTriangles = IPLint32(tris.size());
                ms.numMaterials = IPLint32(materialsOut.size());
                ms.vertices = vertices.data();
                ms.triangles = tris.data();
                ms.materialIndices = materialIndex.data();
                ms.materials = materialsOut.data();
                if (iplStaticMeshCreate(scene, &ms, &t.mesh) != IPL_STATUS_SUCCESS) {
                    t.mesh = nullptr;
                    continue;
                }
                iplStaticMeshAdd(t.mesh, scene);
                t.triangles = uint32_t(tris.size());
                sceneDirty = true;
            }
    }

    // Reflections for the sources marked for them and the listener's reverb, a few times a second.
    void reflectionsLoop()
    {
        while (true) {
            const ReflectionQuality q = reflectionQuality(quality.load());
            {
                std::unique_lock<std::mutex> lock(wake);
                const auto period = std::chrono::microseconds(q.rate > 0.0f ? int64_t(1e6f / q.rate) : 100000);
                wakeup.wait_for(lock, period, [&] { return stop; });
                if (stop)
                    return;
            }
            if (q.rays == 0)
                continue;
            std::lock_guard<std::mutex> running(simulation);
            runReflections(q);
        }
    }

    // One reflections run (`simulation` held).
    void runReflections(const ReflectionQuality& q)
    {
        struct Item {
            IPLSource source;
            float3 position;
            bool on, pathing;
            uint32_t index;
        };
        std::vector<Item> items;
        {
            const uint64_t t0 = SDL_GetTicksNS();
            ListenerPose l;
            items.clear();
            {
                std::lock_guard<std::mutex> lock(snapshot);
                l = listenerPose;
                for (uint32_t i = 0; i < sources.size(); ++i)
                    if (const Source& s = sources[i]; s.used && s.added)
                        items.push_back({s.source, s.position, s.reflections, s.pathing, i});
            }
            // Baked: the listener's reverb from the probes around it (realtime where none reach),
            // and paths between the probes nearest each sound and the listener.
            const bool bakedReverb = baked && baked->info.reverb && baked->covers(l.position);
            const bool paths = baked && baked->info.pathing && baked->covers(l.position);
            const IPLSimulationFlags both = IPLSimulationFlags(IPL_SIMULATIONFLAGS_REFLECTIONS | IPL_SIMULATIONFLAGS_PATHING);
            IPLSimulationInputs in{};
            in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
            in.hybridReverbTransitionTime = 1.0f;
            in.hybridReverbOverlapPercent = 0.25f;
            if (baked) {
                in.pathingProbes = static_cast<IPLProbeBatch>(baked->batch);
                in.visRadius = std::min(0.5f, 0.25f * baked->info.spacing);
                in.visThreshold = 0.1f;
                in.visRange = baked->info.visRange;
                in.pathingOrder = int(steam->reflections().order);
                in.enableValidation = IPL_TRUE;  // (against the scene now, not as baked: a door shut since blocks)
                in.findAlternatePaths = IPL_FALSE;
            }
            uint32_t pathing = 0;
            for (Item& it : items) {
                it.pathing = it.pathing && paths && baked->covers(it.position);
                pathing += it.pathing ? 1u : 0u;
                in.flags = IPLSimulationFlags((it.on ? IPL_SIMULATIONFLAGS_REFLECTIONS : 0) | (it.pathing ? IPL_SIMULATIONFLAGS_PATHING : 0));
                in.source = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, v3(it.position)};
                iplSourceSetInputs(it.source, both, &in);
            }
            in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            in.source = {v3(l.right), v3(l.up), v3(l.forward), v3(l.position)};
            in.baked = bakedReverb ? IPL_TRUE : IPL_FALSE;
            in.bakedDataIdentifier = {IPL_BAKEDDATATYPE_REFLECTIONS, IPL_BAKEDDATAVARIATION_REVERB, {}};
            iplSourceSetInputs(listener, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);
            listenerBaked = bakedReverb;
            IPLSimulationSharedInputs shared{};
            shared.listener = {v3(l.right), v3(l.up), v3(l.forward), v3(l.position)};
            shared.numRays = int(q.rays);
            shared.numBounces = int(q.bounces);
            shared.duration = std::min(q.seconds, steam->reflections().seconds);
            shared.order = int(steam->reflections().order);
            shared.irradianceMinDistance = 1.0f;
            iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
            iplSimulatorRunReflections(simulator);
            if (pathing) {
                iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_PATHING, &shared);
                iplSimulatorRunPathing(simulator);
            }
            uint32_t found = 0;
            {
                std::lock_guard<std::mutex> lock(snapshot);
                for (const Item& it : items) {
                    Source& s = sources[it.index];
                    if (s.source != it.source)
                        continue;  // (replaced meanwhile)
                    s.pathed = false;
                    if (!it.pathing)
                        continue;
                    IPLSimulationOutputs out{};
                    iplSourceGetOutputs(it.source, IPL_SIMULATIONFLAGS_PATHING, &out);
                    for (int b = 0; b < 3; ++b)
                        s.path[b] = out.pathing.eqCoeffs[b];
                    s.pathed = out.pathing.shCoeffs && (s.path[0] + s.path[1] + s.path[2]) > 1e-5f;
                    found += s.pathed ? 1u : 0u;
                }
            }
            pathed = found;
            reflectionsMs = float(double(SDL_GetTicksNS() - t0) * 1e-6);
            ++reflectionRuns;
        }
    }

    // One direct run over `directItems` (the direct thread's, while busy).
    void runDirect()
    {
        FX_ZONE_CAT("AudioScene run direct", Other);
        const uint64_t t0 = SDL_GetTicksNS();
        IPLSimulationSharedInputs shared{};
        shared.listener = directListener;
        iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
        for (const DirectItem& it : directItems) {
            IPLSimulationInputs in{};
            in.flags = IPL_SIMULATIONFLAGS_DIRECT;
            in.directFlags = IPLDirectSimulationFlags((it.occlusion ? IPL_DIRECTSIMULATIONFLAGS_OCCLUSION : 0) |
                                                      (it.transmission ? IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION : 0));
            in.source = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, v3(it.position)};
            in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            in.occlusionRadius = 0.5f;
            in.numOcclusionSamples = 16;
            in.numTransmissionRays = 1;
            iplSourceSetInputs(it.source, IPL_SIMULATIONFLAGS_DIRECT, &in);
        }
        if (!directItems.empty())
            iplSimulatorRunDirect(simulator);
        for (DirectItem& it : directItems) {
            IPLSimulationOutputs out{};
            iplSourceGetOutputs(it.source, IPL_SIMULATIONFLAGS_DIRECT, &out);
            it.out[0] = it.occlusion ? out.direct.occlusion : 1.0f;
            for (int b = 0; b < 3; ++b)
                it.out[1 + b] = it.transmission && it.occlusion ? out.direct.transmission[b] : (it.occlusion ? 0.0f : 1.0f);
        }
        directMs = float(double(SDL_GetTicksNS() - t0) * 1e-6);
    }
    void directLoop()
    {
        profiler::nameThread("Audio direct");
        std::unique_lock<std::mutex> lock(direct);
        while (true) {
            directWake.wait(lock, [&] { return directStop || directBusy; });
            if (directStop)
                return;
            lock.unlock();
            runDirect();
            lock.lock();
            directBusy = false;
            directDone.notify_all();
        }
    }
    void waitDirect()  // (the main thread's: the direct thread idle until it hands it another run)
    {
        std::unique_lock<std::mutex> lock(direct);
        directDone.wait(lock, [&] { return !directBusy; });
    }
    // The last run's results into the sources (idle).
    void collectDirect()
    {
        for (const DirectItem& it : directItems)
            if (Source& s = sources[it.index]; s.used && s.source == it.source && !s.removing)
                std::memcpy(s.out, it.out, sizeof(s.out));
        directItems.clear();
    }

    void stopThread()
    {
        if (directThread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(direct);
                directStop = true;
            }
            directWake.notify_all();
            directThread.join();
            directBusy = false;
            directItems.clear();
        }
        if (!thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(wake);
            stop = true;
        }
        wakeup.notify_all();
        thread.join();
    }

    void clearGeometry()
    {
        for (auto& [id, p] : entities)
            unplace(p);
        entities.clear();
        for (auto& [cell, list] : scatter)
            for (Placed& p : list)
                unplace(p);
        scatter.clear();
        for (Tile& t : tiles)
            dropTile(t);
        tiles.clear();
        layersJson.clear();
        triangles.clear();
        compact();  // (no simulation runs: under the lock, or the thread stopped): an empty scene, the rest released
    }
};

AudioScene::AudioScene() : impl_(std::make_unique<Impl>()) {}
AudioScene::~AudioScene() { shutdown(); }

bool AudioScene::init(SteamAudio& steam, MaterialResolver resolver, bool reflections, bool synchronous)
{
    shutdown();
    Impl& m = *impl_;
    m.context = static_cast<IPLContext>(steam.context());
    if (!m.context)
        return false;
    m.steam = &steam;
    m.resolver = std::move(resolver);
    m.reflections = reflections && steam.reflectionSlots() > 0;
    m.synchronous = synchronous;
    m.updates = 0;
    FX_INFO("audio: acoustics traced by Steam Audio's own ray tracer%s",
            m.reflections ? (synchronous ? ", reflections in step with the frame clock" : ", reflections on their own thread") : "");
    IPLSceneSettings ss = m.sceneSettings();
    IPLSimulationSettings sim{};
    sim.flags = m.reflections ? IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS | IPL_SIMULATIONFLAGS_PATHING)
                              : IPL_SIMULATIONFLAGS_DIRECT;
    sim.numVisSamples = 1;  // (pathing's checks of the baked paths against the scene now)
    sim.sceneType = ss.type;
    sim.maxNumOcclusionSamples = 16;
    sim.maxNumSources = int(kMaxSources) + 1;  // (and the listener's reverb)
    sim.numThreads = int(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));  // (rays are sampled at random: runs differ slightly)
    if (m.reflections) {
        const SteamAudio::Reflections& r = steam.reflections();
        sim.reflectionType = r.type == 0 ? IPL_REFLECTIONEFFECTTYPE_CONVOLUTION
                             : r.type == 1 ? IPL_REFLECTIONEFFECTTYPE_PARAMETRIC
                                           : IPL_REFLECTIONEFFECTTYPE_HYBRID;
        sim.maxNumRays = int(reflectionQuality(3).rays);
        sim.numDiffuseSamples = 32;
        sim.maxDuration = r.seconds;
        sim.maxOrder = int(r.order);
    }
    sim.samplingRate = int(kAudioSampleRate);
    sim.frameSize = int(kAudioFrameSize);
    if (iplSceneCreate(m.context, &ss, &m.scene) != IPL_STATUS_SUCCESS || iplSimulatorCreate(m.context, &sim, &m.simulator) != IPL_STATUS_SUCCESS) {
        FX_WARN("audio: Steam Audio's scene did not start: no occlusion");
        shutdown();
        return false;
    }
    iplSceneCommit(m.scene);
    iplSimulatorSetScene(m.simulator, m.scene);
    m.sources.assign(kMaxSources, {});
    if (m.reflections) {
        IPLSourceSettings ls{IPL_SIMULATIONFLAGS_REFLECTIONS};
        if (iplSourceCreate(m.simulator, &ls, &m.listener) == IPL_STATUS_SUCCESS) {
            iplSourceAdd(m.listener, m.simulator);
            steam.setListenerSource(m.listener);
        } else {
            m.reflections = false;
        }
    }
    iplSimulatorCommit(m.simulator);
    m.directStop = m.directBusy = false;
    m.directThread = std::thread([&m] { m.directLoop(); });
    if (m.reflections && !m.synchronous) {
        m.stop = false;
        m.thread = std::thread([&m] { m.reflectionsLoop(); });
    }
    return true;
}

void AudioScene::setReflectionQuality(uint32_t level) { impl_->quality = level; }

void AudioScene::shutdown()
{
    Impl& m = *impl_;
    if (!m.context)
        return;
    m.stopThread();
    if (m.steam)
        m.steam->setListenerSource(nullptr);  // (the device is closed before this: nothing reads it)
    for (Impl::Source& s : m.sources)
        if (s.source) {
            if (s.added)
                iplSourceRemove(s.source, m.simulator);
            iplSourceRelease(&s.source);
        }
    m.sources.clear();
    if (m.listener) {
        iplSourceRemove(m.listener, m.simulator);
        iplSourceRelease(&m.listener);
    }
    if (m.scene)
        m.clearGeometry();
    m.meshes.reset();
    if (m.baked && m.simulator)
        iplSimulatorRemoveProbeBatch(m.simulator, static_cast<IPLProbeBatch>(m.baked->batch));
    m.baked.reset();
    m.meshHashes.clear();
    if (m.simulator)
        iplSimulatorRelease(&m.simulator);
    if (m.scene)
        iplSceneRelease(&m.scene);
    m.context = nullptr;
}

bool AudioScene::active() const { return impl_->scene != nullptr; }

void AudioScene::attach(ScenePak* pak, Scene* scene, MaterialLibrary* materials, const Terrain* terrain)
{
    Impl& m = *impl_;
    if (!m.scene)
        return;
    std::lock_guard<std::mutex> idle(m.simulation);  // (a reflections run finishes first, and a direct one)
    m.waitDirect();
    m.clearGeometry();
    m.meshHashes.clear();
    ++m.geometryRevision;
    m.pak = pak;
    m.ecs = scene;
    m.materials = materials;
    m.terrain = terrain;
    m.meshes.reset();
    if (pak && scene)
        m.meshes = std::make_unique<flecs::query<const MeshInstance, const WorldTransform>>(
            scene->world().query_builder<const MeshInstance, const WorldTransform>().cached().build());
    m.resolveAll = true;
}

void AudioScene::syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed)
{
    Impl& m = *impl_;
    if (!m.scene || !m.pak)
        return;
    for (uint64_t cell : removed)
        if (auto it = m.scatter.find(cell); it != m.scatter.end()) {
            for (Impl::Placed& p : it->second)
                m.unplace(p);
            m.scatter.erase(it);
        }
    AcousticParams rock;
    acousticPreset("rock", rock);
    for (const ScatterCellBodies& c : added) {
        std::vector<Impl::Placed>& list = m.scatter[c.cell];
        for (const ScatterBody& b : c.bodies) {
            Impl::Placed p;
            if (m.place(p, b.mesh, rock, &b.world[0][0]))
                list.push_back(p);
        }
    }
}

void AudioScene::materialsChanged()
{
    impl_->resolveAll = true;
    impl_->layersJson.clear();  // (terrain tiles too)
}

void AudioScene::update(float3 focus, const ListenerPose& listener)
{
    Impl& m = *impl_;
    if (!m.scene)
        return;
    FX_ZONE_CAT("AudioScene::update", Other);
    const uint64_t t0 = SDL_GetTicksNS();
    if (m.pak) {
        {
            FX_ZONE_CAT("AudioScene sync entities", Other);
            m.syncEntities();
        }
        FX_ZONE_CAT("AudioScene sync terrain", Other);
        m.syncTerrain(focus);
    }
    // The direct thread: if it is still on the last run (it took longer than a frame), this frame
    // hands it nothing (its sources' results stay a frame older). Fixed-time runs wait for it.
    bool idle = true;
    if (m.directThread.joinable()) {
        std::unique_lock<std::mutex> lock(m.direct);
        if (m.synchronous)
            m.directDone.wait(lock, [&] { return !m.directBusy; });
        idle = !m.directBusy;
    }
    if (idle) {
        m.collectDirect();
        // Between reflection runs (and direct ones): sources in and out, the scene's and
        // simulator's changes.
        bool pending = m.sceneDirty || m.simulatorDirty;
        for (const Impl::Source& s : m.sources)
            pending = pending || (s.used && (!s.added || s.removing));
        if (pending) {
            FX_ZONE_CAT("AudioScene commit", Other);
            std::unique_lock<std::mutex> running(m.simulation, std::try_to_lock);
            if (running.owns_lock()) {
                std::lock_guard<std::mutex> lock(m.snapshot);
                for (Impl::Source& s : m.sources) {
                    if (!s.used)
                        continue;
                    if (s.removing) {
                        if (s.added)
                            iplSourceRemove(s.source, m.simulator);
                        iplSourceRelease(&s.source);
                        s = {};
                        m.simulatorDirty = true;
                    } else if (!s.added) {
                        iplSourceAdd(s.source, m.simulator);
                        s.added = true;
                        m.simulatorDirty = true;
                    }
                }
                if (m.sceneDirty) {
                    for (flecs::entity_t id : m.moved)  // (no run reads the scene now)
                        if (auto it = m.entities.find(id); it != m.entities.end() && it->second.instance)
                            iplInstancedMeshUpdateTransform(it->second.instance, m.scene, Impl::matrixOf(it->second.rows));
                    m.moved.clear();
                    iplSceneCommit(m.scene);
                    m.retire();  // (what the commit let go of)
                    m.simulatorDirty = true;
                    m.sceneDirty = false;
                }
                if (m.simulatorDirty) {
                    iplSimulatorCommit(m.simulator);
                    m.simulatorDirty = false;
                }
            }
        }
        // The next run: the listener and the sources as they are now.
        m.directListener = {v3(listener.right), v3(listener.up), v3(listener.forward), v3(listener.position)};
        for (uint32_t i = 0; i < m.sources.size(); ++i)
            if (const Impl::Source& s = m.sources[i]; s.used && s.added && !s.removing)
                m.directItems.push_back({s.source, i, s.position, s.occlusion, s.transmission, {1, 1, 1, 1}});
        m.stats.sources = uint32_t(m.directItems.size());
        if (!m.directThread.joinable()) {
            m.runDirect();
            m.collectDirect();
        } else if (!m.directItems.empty()) {
            {
                std::lock_guard<std::mutex> lock(m.direct);
                m.directBusy = true;
            }
            m.directWake.notify_one();
            if (m.synchronous) {  // (in step: its results this frame)
                m.waitDirect();
                m.collectDirect();
            }
        } else {
            m.directMs = 0.0f;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m.snapshot);
        m.listenerPose = listener;
    }
    m.stats.syncMs = float(double(SDL_GetTicksNS() - t0) * 1e-6);
    m.stats.simulateMs = m.directMs.load();
    // Synchronous reflections: at their rate in frames of the (60 Hz) frame clock.
    if (m.reflections && m.synchronous) {
        const ReflectionQuality q = reflectionQuality(m.quality.load());
        const uint64_t every = q.rate > 0.0f ? uint64_t(std::max(1.0f, std::round(60.0f / q.rate))) : 0;
        if (q.rays > 0 && every && m.updates++ % every == 0) {
            std::lock_guard<std::mutex> running(m.simulation);
            m.runReflections(q);
        }
    }
    m.stats.reflecting = 0;
    for (const Impl::Source& s : m.sources)
        m.stats.reflecting += s.used && s.reflections ? 1u : 0u;
}

uint32_t AudioScene::addSource()
{
    Impl& m = *impl_;
    if (!m.simulator)
        return 0;
    for (uint32_t i = 0; i < m.sources.size(); ++i) {
        Impl::Source& s = m.sources[i];
        if (s.used)
            continue;
        IPLSourceSettings ss{m.reflections ? IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS |
                                                                IPL_SIMULATIONFLAGS_PATHING)
                                           : IPL_SIMULATIONFLAGS_DIRECT};
        if (iplSourceCreate(m.simulator, &ss, &s.source) != IPL_STATUS_SUCCESS)
            return 0;
        std::lock_guard<std::mutex> lock(m.snapshot);
        s.used = true;  // (added to the simulator between reflection runs)
        s.added = s.removing = s.reflections = s.pathing = s.pathed = false;
        s.out[0] = s.out[1] = s.out[2] = s.out[3] = 1.0f;
        return i + 1;
    }
    return 0;
}

void AudioScene::removeSource(uint32_t source)
{
    Impl& m = *impl_;
    if (source == 0 || source > m.sources.size() || !m.sources[source - 1].used)
        return;
    std::lock_guard<std::mutex> lock(m.snapshot);
    m.sources[source - 1].removing = true;  // (removed between reflection runs)
    m.sources[source - 1].reflections = m.sources[source - 1].pathing = false;
}

void AudioScene::setSource(uint32_t source, float3 position, bool occlusion, bool transmission, bool reflections, bool pathing)
{
    Impl& m = *impl_;
    if (source == 0 || source > m.sources.size())
        return;
    std::lock_guard<std::mutex> lock(m.snapshot);
    Impl::Source& s = m.sources[source - 1];
    s.position = position;
    s.occlusion = occlusion;
    s.transmission = transmission;
    s.reflections = reflections && !s.removing;
    s.pathing = pathing && !s.removing;
}

void* AudioScene::sourceHandle(uint32_t source) const
{
    const Impl& m = *impl_;
    return source && source <= m.sources.size() && m.sources[source - 1].used ? m.sources[source - 1].source : nullptr;
}

void* AudioScene::listenerSource() const { return impl_->listener; }

bool AudioScene::listenerReverb(float rt60[3]) const
{
    const Impl& m = *impl_;
    if (!m.listener || m.reflectionRuns.load() == 0)
        return false;
    IPLSimulationOutputs out{};
    iplSourceGetOutputs(m.listener, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
    for (int b = 0; b < 3; ++b)
        rt60[b] = out.reflections.reverbTimes[b];
    return true;
}

bool AudioScene::direct(uint32_t source, float& occlusion, float transmission[3]) const
{
    const Impl& m = *impl_;
    if (source == 0 || source > m.sources.size() || !m.sources[source - 1].used)
        return false;
    const Impl::Source& s = m.sources[source - 1];
    occlusion = s.out[0];
    for (int b = 0; b < 3; ++b)
        transmission[b] = s.out[1 + b];
    return true;
}

bool AudioScene::pathing(uint32_t source, float eq[3]) const
{
    Impl& m = *impl_;
    if (source == 0 || source > m.sources.size())
        return false;
    std::lock_guard<std::mutex> lock(m.snapshot);
    const Impl::Source& s = m.sources[source - 1];
    if (!s.used || !s.pathed)
        return false;
    for (int b = 0; b < 3; ++b)
        eq[b] = s.path[b];
    return true;
}

void AudioScene::setBaked(std::unique_ptr<AudioBakedData> baked)
{
    Impl& m = *impl_;
    if (!m.simulator)
        return;
    std::lock_guard<std::mutex> idle(m.simulation);  // (a reflections run finishes first, and a direct one)
    m.waitDirect();
    if (m.baked)
        iplSimulatorRemoveProbeBatch(m.simulator, static_cast<IPLProbeBatch>(m.baked->batch));
    m.baked = std::move(baked);
    if (m.baked)
        iplSimulatorAddProbeBatch(m.simulator, static_cast<IPLProbeBatch>(m.baked->batch));
    iplSimulatorCommit(m.simulator);
    m.listenerBaked = false;
    m.pathed = 0;
    std::lock_guard<std::mutex> lock(m.snapshot);
    for (Impl::Source& s : m.sources)
        s.pathed = false;
}

const AudioBakedData* AudioScene::baked() const { return impl_->baked.get(); }
uint64_t AudioScene::geometryRevision() const { return impl_->geometryRevision; }
void* AudioScene::context() const { return impl_->context; }

void AudioScene::fingerprint(float3 lo, float3 hi, uint64_t& objects, uint64_t& terrain, uint32_t& count)
{
    Impl& m = *impl_;
    objects = terrain = 0;
    count = 0;
    lo = lo - float3{kAudioBakeMargin, kAudioBakeMargin, kAudioBakeMargin};
    hi = hi + float3{kAudioBakeMargin, kAudioBakeMargin, kAudioBakeMargin};
    const auto inRegion = [&](const Impl::Placed& p) {
        return p.lo.x <= hi.x && p.hi.x >= lo.x && p.lo.y <= hi.y && p.hi.y >= lo.y && p.lo.z <= hi.z && p.hi.z >= lo.z;
    };
    for (const auto& [id, p] : m.entities) {
        if (p.moving || !p.placed() || !inRegion(p))
            continue;
        int32_t q[12 + 7];
        for (int i = 0; i < 12; ++i)
            q[i] = millimetres(p.rows[i]);
        std::memcpy(q + 12, p.key.v, sizeof(p.key.v));
        objects += mix(fnv(q, sizeof(q), m.meshHash(p.meshIndex)));  // (a sum: in no particular order)
        ++count;
    }
    const Terrain* t = m.terrain;
    if (!t || !t->valid())
        return;
    const std::vector<AcousticParams> layerAcoustics = Impl::acousticsOf(t->layersJson());
    // Its coarse map within the region: heights, and each sample's hole or acoustics (as the
    // layer it shows).
    const TerrainSettings& s = t->settings();
    uint32_t x0, z0, x1, z1;
    if (!m.coarseRect(lo, hi, x0, z0, x1, z1))
        return;
    const float head[9] = {s.originX, s.originZ, s.spacing, float(s.tiles), float(t->coarseSide()), float(x0), float(z0), float(x1), float(z1)};
    uint64_t h = fnv(head, sizeof(head));
    std::vector<MaterialKey> keys;
    for (const AcousticParams& p : layerAcoustics)
        keys.push_back(keyOf(p));
    h = fnv(keys.data(), keys.size() * sizeof(MaterialKey), h);
    const uint32_t side = t->coarseSide();
    std::vector<uint8_t> layers;
    for (uint32_t z = z0; z <= z1; ++z) {
        h = fnv(t->coarseHeights().data() + size_t(z) * side + x0, size_t(x1 - x0 + 1) * sizeof(uint16_t), h);
        for (uint32_t x = x0; x <= x1; ++x) {
            const uint32_t mat = t->coarseMaterials().empty() ? 0u : t->coarseMaterials()[size_t(z) * side + x];
            layers.push_back((mat & terrain::kHole) ? uint8_t(255) : uint8_t(Impl::layerOf(mat, layerAcoustics.size())));
        }
    }
    terrain = mix(fnv(layers.data(), layers.size(), h));
}

void AudioScene::bakeGeometry(const AudioBakeSettings& settings, AudioBakeGeometry& out)
{
    Impl& m = *impl_;
    out = {};
    std::map<MaterialKey, uint32_t> materialIndex;
    const auto materialOf = [&](const AcousticParams& p) {
        auto [it, added] = materialIndex.try_emplace(keyOf(p), uint32_t(out.materials.size()));
        if (added)
            out.materials.push_back(p);
        return it->second;
    };
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (const auto& [id, p] : m.entities)
        if (!p.moving && p.placed()) {
            lo = {std::min(lo.x, p.lo.x), std::min(lo.y, p.lo.y), std::min(lo.z, p.lo.z)};
            hi = {std::max(hi.x, p.hi.x), std::max(hi.y, p.hi.y), std::max(hi.z, p.hi.z)};
        }
    // The region: the static meshes' bounds (a probe's spacing beyond them, and room above for
    // the probes' height), else the terrain's; or as set.
    const Terrain* terrain = m.terrain && m.terrain->valid() ? m.terrain : nullptr;
    if (!settings.autoRegion) {
        out.lo = settings.center - settings.size * 0.5f;
        out.hi = settings.center + settings.size * 0.5f;
    } else if (lo.x <= hi.x) {
        const float g = settings.spacing;
        out.lo = lo - float3{g, g, g};
        out.hi = hi + float3{g, settings.height + g, g};
    } else if (terrain) {
        float tlo[3], thi[3];
        terrain->bounds(tlo, thi);
        out.lo = {tlo[0], tlo[1] - 1.0f, tlo[2]};
        out.hi = {thi[0], thi[1] + settings.height + 4.0f, thi[2]};
    }
    if (!(out.lo.x <= out.hi.x))
        return;
    fingerprint(out.lo, out.hi, out.objectsHash, out.terrainHash, out.objects);
    // The static meshes in the region and some way beyond it (what sound bounces off just outside).
    const float3 near0 = out.lo - float3{kAudioBakeMargin, kAudioBakeMargin, kAudioBakeMargin};
    const float3 near1 = out.hi + float3{kAudioBakeMargin, kAudioBakeMargin, kAudioBakeMargin};
    for (const auto& [id, p] : m.entities) {
        if (p.moving || !p.placed() || p.lo.x > near1.x || p.hi.x < near0.x || p.lo.y > near1.y || p.hi.y < near0.y || p.lo.z > near1.z ||
            p.hi.z < near0.z)
            continue;
        const Triangles& t = m.trianglesOf(p.meshIndex);
        const uint32_t base = uint32_t(out.vertices.size()), material = materialOf(p.material);
        for (const IPLVector3& v : t.vertices) {
            const float* r = p.rows;
            out.vertices.push_back({r[0] * v.x + r[1] * v.y + r[2] * v.z + r[3], r[4] * v.x + r[5] * v.y + r[6] * v.z + r[7],
                                    r[8] * v.x + r[9] * v.y + r[10] * v.z + r[11]});
        }
        for (const IPLTriangle& tri : t.triangles) {
            for (int k = 0; k < 3; ++k)
                out.triangles.push_back(base + uint32_t(tri.indices[k]));
            out.materialIndex.push_back(material);
        }
    }
    // The terrain from its coarse map (one quad per coarse sample, holes left open), within the
    // region and as far beyond it.
    uint32_t x0, z0, x1, z1;
    if (terrain && m.coarseRect(near0, near1, x0, z0, x1, z1)) {
        std::vector<uint32_t> layerMaterial;
        for (const AcousticParams& p : Impl::acousticsOf(terrain->layersJson()))
            layerMaterial.push_back(materialOf(p));
        if (layerMaterial.empty()) {
            AcousticParams p;
            acousticPreset("generic", p);
            layerMaterial.push_back(materialOf(p));
        }
        const TerrainSettings& s = terrain->settings();
        const uint32_t side = terrain->coarseSide();
        const float step = s.spacing * float(terrain::kCoarseStep);
        {
            const uint32_t base = uint32_t(out.vertices.size()), w = x1 - x0 + 1;
            const std::vector<uint16_t>& heights = terrain->coarseHeights();
            const std::vector<uint32_t>& mats = terrain->coarseMaterials();
            for (uint32_t z = z0; z <= z1; ++z)
                for (uint32_t x = x0; x <= x1; ++x)
                    out.vertices.push_back({s.originX + float(x) * step, terrain->toWorld(heights[size_t(z) * side + x]), s.originZ + float(z) * step});
            for (uint32_t z = z0; z < z1; ++z)
                for (uint32_t x = x0; x < x1; ++x) {
                    const uint32_t mat = mats.empty() ? 0u : mats[size_t(z) * side + x];
                    if (mat & terrain::kHole)
                        continue;
                    const uint32_t a = base + (z - z0) * w + (x - x0), b = a + 1, c = a + w, d = c + 1;
                    const uint32_t material = layerMaterial[Impl::layerOf(mat, layerMaterial.size())];
                    for (uint32_t i : {a, c, b, b, c, d})
                        out.triangles.push_back(i);
                    out.materialIndex.push_back(material);
                    out.materialIndex.push_back(material);
                }
            out.terrain = true;
        }
    }
}

AudioSceneStats AudioScene::stats() const
{
    const Impl& m = *impl_;
    AudioSceneStats st = m.stats;
    st.reflectionsMs = m.reflectionsMs.load();
    st.reflectionRuns = m.reflectionRuns.load();
    st.bakedProbes = m.baked ? uint32_t(m.baked->probes.size()) : 0u;
    st.listenerBaked = m.listenerBaked.load();
    st.pathing = m.pathed.load();
    st.meshes = uint32_t(m.triangles.size());
    st.instances = uint32_t(m.entities.size());
    for (const auto& [id, p] : m.entities)
        st.triangles += p.triangles;
    for (const auto& [cell, list] : m.scatter) {
        st.instances += uint32_t(list.size());
        for (const Impl::Placed& p : list)
            st.triangles += p.triangles;
    }
    for (const Impl::Tile& t : m.tiles) {
        st.terrainTiles += t.mesh ? 1u : 0u;
        st.triangles += t.triangles;
    }
    return st;
}

#else  // no Steam Audio in this build: nothing is simulated

struct AudioScene::Impl {};
AudioScene::AudioScene() : impl_(std::make_unique<Impl>()) {}
AudioScene::~AudioScene() = default;
bool AudioScene::init(SteamAudio&, MaterialResolver, bool, bool) { return false; }
void AudioScene::setReflectionQuality(uint32_t) {}
void* AudioScene::sourceHandle(uint32_t) const { return nullptr; }
void* AudioScene::listenerSource() const { return nullptr; }
bool AudioScene::listenerReverb(float*) const { return false; }
void AudioScene::shutdown() {}
bool AudioScene::active() const { return false; }
void AudioScene::attach(ScenePak*, Scene*, MaterialLibrary*, const Terrain*) {}
void AudioScene::syncScatter(const std::vector<ScatterCellBodies>&, const std::vector<uint64_t>&) {}
void AudioScene::materialsChanged() {}
void AudioScene::update(float3, const ListenerPose&) {}
uint32_t AudioScene::addSource() { return 0; }
void AudioScene::removeSource(uint32_t) {}
void AudioScene::setSource(uint32_t, float3, bool, bool, bool, bool) {}
bool AudioScene::direct(uint32_t, float&, float*) const { return false; }
bool AudioScene::pathing(uint32_t, float*) const { return false; }
void AudioScene::setBaked(std::unique_ptr<AudioBakedData>) {}
const AudioBakedData* AudioScene::baked() const { return nullptr; }
void AudioScene::bakeGeometry(const AudioBakeSettings&, AudioBakeGeometry& out) { out = {}; }
void AudioScene::fingerprint(float3, float3, uint64_t& objects, uint64_t& terrain, uint32_t& count) { objects = terrain = 0; count = 0; }
uint64_t AudioScene::geometryRevision() const { return 0; }
void* AudioScene::context() const { return nullptr; }
AudioSceneStats AudioScene::stats() const { return {}; }

#endif

}  // namespace fx
