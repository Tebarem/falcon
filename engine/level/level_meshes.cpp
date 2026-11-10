#include "level/level_meshes.h"

#include "assets/scene_pak.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/material_library.h"
#include "level/level_mask.h"
#include "level/pak_mesh.h"
#include "render/surface_layers.h"
#include "editor/scene_edits.h"
#include "render/live_geometry.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace fx {

static_assert(SurfaceLayers::kMaskSize == level::kMaskSize, "a LevelMask is one surface layer's mask");

namespace {

float3 transformPoint(const float m[3][4], float3 p)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

// A box (lo..hi, in an entity's space) in the world's: the bounds of its corners.
void worldBox(const float m[3][4], float3 lo, float3 hi, float3& wlo, float3& whi)
{
    wlo = {1e30f, 1e30f, 1e30f};
    whi = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        const float3 q = transformPoint(m, {k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        wlo = {std::min(wlo.x, q.x), std::min(wlo.y, q.y), std::min(wlo.z, q.z)};
        whi = {std::max(whi.x, q.x), std::max(whi.y, q.y), std::max(whi.z, q.z)};
    }
}

bool invertAffine(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f)
        return false;
    const float r = 1.0f / det;
    const float inv[3][3] = {{(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r},
                             {(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r},
                             {(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = inv[row][col];
        out[row][3] = -(inv[row][0] * m[0][3] + inv[row][1] * m[1][3] + inv[row][2] * m[2][3]);
    }
    return true;
}

}  // namespace

// Live wear made on a job thread: from what the mesh is drawn as, its rules, and the level meshes
// around it (their triangles, for the grime where they meet it). In the world's sizes and up (its
// rotation and scale applied; not where it is, so moving it doesn't change its noise): a box
// scaled into a wall wears as wide along it as across, streaks run down.
// A cut made on a worker thread: its inputs (shared, never changed in place) and its result.
// The result is made ready to draw there too (triangulated, then LiveGeometry::prepare: tangents,
// clusters, its BVH, its collider), so the main thread only uploads it: a damage brush dab cutting
// a big wall took 20-50 ms of the frame to build it again. Its materials are looked up when the
// job starts (`materialIndex`); one that wasn't (a cut mesh's own) leaves it to the main thread.
struct LevelMeshes::CutJob : enki::ITaskSet {
    std::shared_ptr<const level::PolyMesh> from, target;
    std::vector<uint32_t> modifiedFace;
    float world[3][4] = {};
    std::vector<level::Cut> cuts;
    uint64_t signature = 0;
    std::shared_ptr<const level::BakedCut> result;
    std::string error;
    float ms = 0.0f;
    bool baked = false;  // (from the scene's bake: never run)

    // Building it here: what it needs (set when the job starts), and what it made.
    bool prepare = false;
    std::unordered_map<std::string, uint32_t> materialIndex;
    uint32_t defaultMaterial = 0;
    std::vector<uint8_t> blended;  // LiveGeometry::blendedMaterials
    bool traced = false, collider = false;
    bool prepared = false;
    PreparedLiveMesh mesh;
    std::vector<uint32_t> faceOfTriangle;
    uint32_t skipped = 0;

    void ExecuteRange(enki::TaskSetPartition, uint32_t) override
    {
        FX_ZONE_CAT("LevelMeshes cut", Rendering);
        const auto t0 = std::chrono::steady_clock::now();
        level::CutResult cut;
        if (level::cutMesh(*target, world, cuts, cut) && cut.applied) {
            auto r = std::make_shared<level::BakedCut>();
            r->mesh = std::make_shared<const level::PolyMesh>(std::move(cut.mesh));
            r->sourceFace = std::move(cut.sourceFace);
            r->clipped = cut.clipped;
            result = std::move(r);
            if (prepare)
                prepareMesh(*result->mesh);
        } else {
            error = std::move(cut.error);
        }
        ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // As LevelMeshes::build would (its material lookup the same, from the names looked up).
    void prepareMesh(const level::PolyMesh& drawn)
    {
        std::vector<uint32_t> slots(drawn.materials.size());
        for (size_t s = 0; s < slots.size(); ++s) {
            const std::string& name = drawn.materials[s];
            if (name.starts_with("scene:")) {
                slots[s] = uint32_t(std::strtoul(name.c_str() + 6, nullptr, 10));
            } else if (name.empty()) {
                slots[s] = defaultMaterial;
            } else {
                const auto found = materialIndex.find(name);
                if (found == materialIndex.end())
                    return;  // (not looked up: the main thread builds it)
                slots[s] = found->second;
            }
        }
        level::BuildOptions options;
        std::memcpy(options.world, world, sizeof(options.world));
        options.material = [&](uint32_t slot) { return slot < slots.size() ? slots[slot] : defaultMaterial; };
        LiveMeshData data;
        if (!level::build(drawn, options, data, &faceOfTriangle, &skipped))
            return;
        prepared = LiveGeometry::prepare(data, blended, traced, collider, mesh);
    }
};

struct LevelMeshes::WearJob : enki::ITaskSet {
    std::shared_ptr<const level::PolyMesh> mesh;
    std::vector<level::WearSettings> rules;
    float world[3][4] = {};
    std::vector<std::pair<std::shared_ptr<const level::PolyMesh>, std::array<float, 12>>> around;
    uint64_t key = 0;
    std::vector<uint8_t> mask;
    float ms = 0.0f;

    void ExecuteRange(enki::TaskSetPartition, uint32_t) override
    {
        FX_ZONE_CAT("LevelMeshes wear", Rendering);
        const auto t0 = std::chrono::steady_clock::now();
        float reach = 0.0f;
        for (const level::WearSettings& r : rules)
            if (r.kind == level::WearKind::Grime)
                reach = std::max(reach, r.width);
        level::PolyMesh shaped = *mesh;
        const float3 at = {world[0][3], world[1][3], world[2][3]};
        for (float3& p : shaped.positions)
            p = transformPoint(world, p) - at;
        std::vector<float3> nearby;
        if (reach > 0.0f && !around.empty()) {
            float3 lo, hi;
            shaped.bounds(lo, hi);
            lo = {lo.x - reach, lo.y - reach, lo.z - reach};
            hi = {hi.x + reach, hi.y + reach, hi.z + reach};
            for (const auto& [other, rows] : around) {
                float m[3][4];
                std::memcpy(m, rows.data(), sizeof(m));
                for (uint32_t f = 0; f < other->faces.size(); ++f) {
                    const level::Face& face = other->faces[f];
                    const auto corner = [&](uint32_t k) { return transformPoint(m, other->positions[other->corners[face.first + k]]) - at; };
                    for (uint32_t k = 1; k + 1 < face.count; ++k) {
                        const float3 p[3] = {corner(0), corner(k), corner(k + 1)};
                        if (std::max({p[0].x, p[1].x, p[2].x}) < lo.x || std::min({p[0].x, p[1].x, p[2].x}) > hi.x ||
                            std::max({p[0].y, p[1].y, p[2].y}) < lo.y || std::min({p[0].y, p[1].y, p[2].y}) > hi.y ||
                            std::max({p[0].z, p[1].z, p[2].z}) < lo.z || std::min({p[0].z, p[1].z, p[2].z}) > hi.z)
                            continue;
                        nearby.insert(nearby.end(), p, p + 3);
                    }
                }
            }
        }
        mask.assign(level::kMaskBytes, 0);
        bool made = false;
        for (const level::WearSettings& r : rules)
            made |= level::generateWear(shaped, r, mask, &nearby);
        if (!made)
            mask.clear();
        ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};

void LevelMeshes::needs(const SceneEdits& edits, uint32_t& meshes, uint32_t& blocks)
{
    meshes = blocks = 0;
    std::unordered_map<std::string, const EntityEdit*> byKey;  // ("c" + uid: a parent's key)
    for (const EntityEdit& e : edits.created)
        byKey["c" + std::to_string(e.uid)] = &e;
    for (const EntityEdit& e : edits.created)
        if (e.levelMesh) {
            ++meshes;
            // About 90 triangles a cluster block once meshlets are made, and one block at least (its
            // modifiers applied: an array's copies).
            uint32_t triangles = e.levelMesh->triangleCount();
            level::Spline curve;  // (following its parent spline: the spline in its space)
            const auto parent = byKey.find(e.parent);
            float inverse[3][4];
            if (level::needsSpline(e.modifiers) && parent != byKey.end() && parent->second->spline && invertAffine(e.world, inverse)) {
                curve = parent->second->spline->spline;
                for (level::SplinePoint& q : curve.points)
                    q.position = transformPoint(inverse, q.position);
            }
            const level::ModifierContext context{curve.points.empty() ? nullptr : &curve};
            if (level::PolyMesh modified; !e.modifiers.empty() && level::applyModifiers(*e.levelMesh, e.modifiers, modified, nullptr, nullptr, &context))
                triangles = modified.triangleCount();
            blocks += triangles / 90 + 2;
        }
}

namespace {
LevelMeshes* g_current = nullptr;  // the attached one (LevelMeshes::current)
}  // namespace

void LevelMeshes::attach(Scene& scene, ScenePak& pak, LiveGeometry& live, MaterialLibrary* materials, SurfaceLayers* layers,
                         std::function<void(uint32_t, int32_t)> setInstanceLayer, std::function<void(uint32_t, bool)> setInstanceHidden)
{
    g_current = this;
    layers_ = layers;
    setInstanceLayer_ = std::move(setInstanceLayer);
    setInstanceHidden_ = std::move(setInstanceHidden);
    scene_ = &scene;
    pak_ = &pak;
    live_ = &live;
    materials_ = materials;
    entries_.clear();
    stats_ = {};
    warnings_ = 0;
    // Disabled entities too (deleted in the editor: hidden, kept for undo).
    query_ = scene.world().query_builder<const LevelMesh>().query_flags(EcsQueryMatchDisabled).build();
}

LevelMeshes* LevelMeshes::current() { return g_current; }

uint64_t LevelMeshes::geometryHash() const
{
    // By entity id order (the same ids on every machine: the scene's edits), each drawn mesh's hash
    // and where it is (to the millimetre); then the cut copies' signatures.
    std::vector<std::pair<flecs::entity_t, uint64_t>> parts;
    for (const auto& [id, entry] : entries_) {
        if (!entry.drawn || !entry.shown)
            continue;
        uint64_t h = entry.drawn->hash();
        for (const float* r = &entry.world[0][0]; r != &entry.world[0][0] + 12; ++r)
            h = (h ^ uint64_t(int64_t(std::llround(double(*r) * 1000.0)))) * 0x100000001b3ull;
        parts.push_back({id, h});
    }
    std::sort(parts.begin(), parts.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& [id, part] : parts)
        h = (h ^ part) * 0x100000001b3ull;
    std::vector<uint64_t> copies;
    for (const auto& [slot, copy] : copies_)
        if (copy.live >= 0)
            copies.push_back(copy.signature);
    std::sort(copies.begin(), copies.end());
    for (const uint64_t c : copies)
        h = (h ^ c) * 0x100000001b3ull;
    return h;
}

void LevelMeshes::detach()
{
    if (g_current == this)
        g_current = nullptr;
    // (wear and cut jobs still running hold no pointers into here, but are waited for)
    for (auto& [entity, entry] : entries_) {
        if (entry.wearJob)
            jobs::scheduler().WaitforTask(entry.wearJob.get());
        if (entry.cutJob)
            jobs::scheduler().WaitforTask(entry.cutJob.get());
    }
    for (auto& [slot, copy] : copies_)
        if (copy.job)
            jobs::scheduler().WaitforTask(copy.job.get());
    for (const auto& job : orphans_)
        jobs::scheduler().WaitforTask(job.get());
    orphans_.clear();
    if (query_)
        query_.destruct();
    query_ = {};
    for (auto& [entity, entry] : entries_)
        dropLayer(entry);
    entries_.clear();
    std::vector<InstanceUpdate> unused;
    for (auto& [slot, copy] : copies_)
        dropCopy(slot, copy, unused);
    copies_.clear();
    pakMeshes_.clear();
    bake_.results.clear();
    scene_ = nullptr;
    pak_ = nullptr;
    live_ = nullptr;
}

bool LevelMeshes::openingWorld(flecs::entity opening, float out[3][4])
{
    const WallOpening* o = opening.try_get<WallOpening>();
    const Parent* p = opening.try_get<Parent>();
    if (!o || !p || !p->entity || !opening.world().is_alive(p->entity))
        return false;
    const flecs::entity wallEntity(opening.world(), p->entity);
    const LevelWall* w = wallEntity.try_get<LevelWall>();
    const WorldTransform* wt = wallEntity.try_get<WorldTransform>();
    float2 at, dir;
    if (!w || !wt || !level::wallPathAt(w->wall, o->distance, at, dir))
        return false;
    // In the wall's space: along the path (x), up (y), across it (z: to the path's left), through the
    // wall's middle (its path may run along one face), a little deeper than the wall.
    const float t = w->wall.thickness, middle = w->wall.align == 1 ? -t * 0.5f : w->wall.align == 2 ? t * 0.5f : 0.0f;
    const float2 left = {-dir.y, dir.x};
    const float3 c = {at.x + left.x * middle, o->sill + o->height * 0.5f, at.y + left.y * middle};
    const float local[3][4] = {{dir.x * o->width, 0.0f, left.x * (t + 0.2f), c.x},
                               {0.0f, o->height, 0.0f, c.y},
                               {dir.y * o->width, 0.0f, left.y * (t + 0.2f), c.z}};
    const float* r = wt->rows;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            out[row][col] = r[row * 4] * local[0][col] + r[row * 4 + 1] * local[1][col] + r[row * 4 + 2] * local[2][col] + (col == 3 ? r[row * 4 + 3] : 0.0f);
    return true;
}

bool LevelMeshes::openingFrame(const level::Wall& wall, const float wallWorld[3][4], const WallOpening& o, float out[3][4])
{
    float2 at, dir;
    if (!level::wallPathAt(wall, o.distance, at, dir))
        return false;
    const float t = wall.thickness, middle = wall.align == 1 ? -t * 0.5f : wall.align == 2 ? t * 0.5f : 0.0f;
    const float2 left = {-dir.y, dir.x};
    const float local[3][4] = {{dir.x, 0.0f, left.x, at.x + left.x * middle}, {0.0f, 1.0f, 0.0f, o.sill}, {dir.y, 0.0f, left.y, at.y + left.y * middle}};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            out[row][col] = wallWorld[row][0] * local[0][col] + wallWorld[row][1] * local[1][col] + wallWorld[row][2] * local[2][col] +
                            (col == 3 ? wallWorld[row][3] : 0.0f);
    return true;
}

void LevelMeshes::updateWalls()
{
    // Each wall's (and roof's) mesh made again when the wall is another one (set in the component;
    // the editor's undo puts both back).
    flecs::world& world = scene_->world();
    std::vector<std::pair<flecs::entity, std::shared_ptr<level::PolyMesh>>> made;
    world.each([&](flecs::entity e, const LevelWall& w) {
        const auto it = wallsBuilt_.find(e.id());
        const LevelMesh* m = e.try_get<LevelMesh>();
        if (it != wallsBuilt_.end() && it->second == w.wall && m && m->mesh)
            return;
        wallsBuilt_[e.id()] = w.wall;
        made.push_back({e, std::make_shared<level::PolyMesh>(level::buildWall(w.wall))});
    });
    for (auto& [e, mesh] : made)
        e.set<LevelMesh>({std::move(mesh)});
    for (auto it = wallsBuilt_.begin(); it != wallsBuilt_.end();)
        it = world.is_alive(it->first) && flecs::entity(world, it->first).has<LevelWall>() ? std::next(it) : wallsBuilt_.erase(it);
    made.clear();
    world.each([&](flecs::entity e, const LevelRoof& r) {
        const auto it = roofsBuilt_.find(e.id());
        const LevelMesh* m = e.try_get<LevelMesh>();
        if (it != roofsBuilt_.end() && it->second == r.roof && m && m->mesh)
            return;
        roofsBuilt_[e.id()] = r.roof;
        made.push_back({e, std::make_shared<level::PolyMesh>(level::buildRoof(r.roof))});
    });
    for (auto& [e, mesh] : made)
        e.set<LevelMesh>({std::move(mesh)});
    for (auto it = roofsBuilt_.begin(); it != roofsBuilt_.end();)
        it = world.is_alive(it->first) && flecs::entity(world, it->first).has<LevelRoof>() ? std::next(it) : roofsBuilt_.erase(it);
}

void LevelMeshes::updateSplines()
{
    // Each spline's loft made again when the spline is another one (an empty mesh without a profile).
    flecs::world& world = scene_->world();
    std::vector<std::pair<flecs::entity, std::shared_ptr<level::PolyMesh>>> made;
    world.each([&](flecs::entity e, const LevelSpline& s) {
        const auto it = splinesBuilt_.find(e.id());
        const LevelMesh* m = e.try_get<LevelMesh>();
        if (it != splinesBuilt_.end() && it->second == s && m && m->mesh)
            return;
        splinesBuilt_[e.id()] = s;
        made.push_back({e, std::make_shared<level::PolyMesh>(level::loftSpline(s.spline, s.loft))});
    });
    for (auto& [e, mesh] : made)
        e.set<LevelMesh>({std::move(mesh)});
    for (auto it = splinesBuilt_.begin(); it != splinesBuilt_.end();)
        it = world.is_alive(it->first) && flecs::entity(world, it->first).has<LevelSpline>() ? std::next(it) : splinesBuilt_.erase(it);
}

bool LevelMeshes::curveOf(flecs::entity e, const float world[3][4], level::Spline& out) const
{
    const Parent* p = e.try_get<Parent>();
    if (!p || !p->entity || !e.world().is_alive(p->entity))
        return false;
    const flecs::entity parent(e.world(), p->entity);
    const LevelSpline* s = parent.try_get<LevelSpline>();
    const WorldTransform* pt = parent.try_get<WorldTransform>();
    float inverse[3][4], pw[3][4];
    if (!s || !pt || !invertAffine(world, inverse))
        return false;
    std::memcpy(pw, pt->rows, sizeof(pw));
    out = s->spline;
    for (level::SplinePoint& q : out.points)
        q.position = transformPoint(inverse, transformPoint(pw, q.position));
    return true;
}

void LevelMeshes::gatherCuts()
{
    cuts_.clear();
    scene_->world().each([&](flecs::entity c, const CutVolume& v, const WorldTransform& t) {
        if (!c.enabled())
            return;
        CutInfo info;
        info.entity = c.id();
        info.cut.shape = v.shape;
        info.cut.intersect = v.intersect;
        info.cut.capMaterial = v.capMaterial;
        info.cut.seed = v.seed;
        info.cut.roughness = v.roughness;
        std::memcpy(info.cut.world, t.rows, sizeof(info.cut.world));
        openingWorld(c, info.cut.world);  // (an opening: on its wall's path)
        if (v.shape == level::CutShape::Mesh) {  // (its own level mesh: the shape)
            const LevelMesh* lm = c.try_get<LevelMesh>();
            if (!lm || !lm->mesh)
                return;
            info.cut.mesh = lm->mesh;
        }
        level::cutBounds(info.cut, info.lo, info.hi);
        // What makes it cut differently: its shape and settings, where it is, its mesh.
        uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i)
                h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
        };
        mix(&v.shape, sizeof(v.shape));
        mix(&v.intersect, sizeof(v.intersect));
        mix(v.capMaterial.data(), v.capMaterial.size());
        mix(&v.seed, sizeof(v.seed));
        mix(&v.roughness, sizeof(v.roughness));
        mix(info.cut.world, sizeof(info.cut.world));
        const void* mesh = info.cut.mesh.get();
        mix(&mesh, sizeof(mesh));
        info.signature = h | 1u;
        cuts_.push_back(std::move(info));
    });
}

void LevelMeshes::build(flecs::entity_t id, Entry& entry, const std::shared_ptr<const level::PolyMesh>& from,
                        const std::shared_ptr<const level::PolyMesh>& shape, const float world[3][4], const std::vector<level::Cut>& cuts,
                        const std::shared_ptr<const level::BakedCut>& result, const std::vector<uint32_t>* modifiedFace,
                        CutJob* ready)
{
    const auto t0 = std::chrono::steady_clock::now();
    level::BuildOptions options;
    std::memcpy(options.world, world, sizeof(options.world));
    // With cut volumes over it: drawn cut (the mesh itself stays as it is); its faces still the mesh's.
    entry.cutTarget = result ? shape : nullptr;
    entry.cutWith = result ? cuts : std::vector<level::Cut>{};
    if (bool(entry.cutResult && entry.cutResult->clipped) != bool(result && result->clipped))
        ++clippedVersion_;  // (the editor's badge)
    entry.cutResult = result;
    const level::PolyMesh& mesh = result ? *result->mesh : *shape;
    options.material = [&](uint32_t slot) -> uint32_t {
        if (slot < mesh.materials.size() && mesh.materials[slot].starts_with("scene:"))
            return uint32_t(std::strtoul(mesh.materials[slot].c_str() + 6, nullptr, 10));
        if (!materials_ || slot >= mesh.materials.size() || mesh.materials[slot].empty())
            return uint32_t(std::max(defaultMaterialIndex(), 0));
        const int32_t index = materials_->indexOf(mesh.materials[slot]);
        return index >= 0 ? uint32_t(index) : 0u;
    };
    LiveMeshData data;
    uint32_t skipped = 0;
    bool made = false;
    if (ready && ready->prepared) {  // (built on the cut's job thread: only uploaded here)
        ready->prepared = false;
        entry.faceOfTriangle = std::move(ready->faceOfTriangle);
        skipped = ready->skipped;
        made = live_->commit(uint32_t(entry.live), std::move(ready->mesh), world);
    } else {
        made = level::build(mesh, options, data, &entry.faceOfTriangle, &skipped) && live_->set(uint32_t(entry.live), data, world);
    }
    if (!made) {
        if (warnings_++ < 8)
            FX_WARN("level: mesh of entity %llu not built (%s)", (unsigned long long)id,
                    mesh.valid() ? "no room left in the live meshes" : "invalid");
        entry.built = from;  // (not again until it changes)
        entry.builtShape = shape;
        entry.drawn = nullptr;
        return;
    }
    if (skipped && warnings_++ < 8)
        FX_WARN("level: %u degenerate faces skipped", skipped);
    if (result)  // (picks: the mesh's own faces; a cut's caps none)
        for (uint32_t& f : entry.faceOfTriangle)
            f = f < result->sourceFace.size() ? result->sourceFace[f] : ~0u;
    if (modifiedFace && !modifiedFace->empty())  // (and through its modifiers: a copy's faces are the mesh's)
        for (uint32_t& f : entry.faceOfTriangle)
            f = f < modifiedFace->size() ? (*modifiedFace)[f] : ~0u;
    entry.built = from;
    entry.builtShape = shape;
    std::memcpy(entry.world, world, sizeof(entry.world));
    entry.worldUvs = false;
    for (const level::Face& f : mesh.faces)
        entry.worldUvs |= f.uv.mode == level::UvMode::World;
    entry.drawn = result ? result->mesh : shape;
    entry.drawn->bounds(entry.lo, entry.hi);
    ++stats_.builds;
    stats_.lastBuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void LevelMeshes::update(std::vector<InstanceUpdate>& rows)
{
    if (!scene_ || !live_ || !live_->valid() || !query_)
        return;
    FX_ZONE_CAT("LevelMeshes::update", Rendering);
    for (auto& [id, entry] : entries_)
        entry.seen = false;
    flecs::world& world = scene_->world();
    stats_.meshes = stats_.starved = 0;
    updateWalls();
    updateSplines();
    gatherCuts();
    std::vector<flecs::entity_t> firstCuts;  // (meshes not drawn yet: their cuts waited for below)
    world.defer_begin();  // (MeshInstance added inside the iteration)
    query_.each([&](flecs::entity e, const LevelMesh& m) {
        Entry& entry = entries_[e.id()];
        entry.seen = true;
        if (entry.live < 0) {
            const int32_t live = live_->create();
            if (live < 0) {
                ++stats_.starved;
                if (warnings_++ < 8)
                    FX_WARN("level: no live mesh left for entity %llu (--live-meshes)", (unsigned long long)e.id());
                return;
            }
            entry.live = live;
            const uint32_t slot = live_->instanceOf(uint32_t(live));
            e.set<MeshInstance>({live_->meshOf(uint32_t(live)), slot});
            scene_->bindInstance(slot, e.id());
            entry.hidden = true;  // (shown below, its row with it)
        }
        ++stats_.meshes;
        float w[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        if (const WorldTransform* t = e.try_get<WorldTransform>())
            std::memcpy(w, t->rows, sizeof(w));
        // Its modifiers applied (again when the mesh, the stack, or the spline it follows is another).
        static const std::vector<level::Modifier> kNoModifiers;
        const LevelModifiers* mods = e.try_get<LevelModifiers>();
        const std::vector<level::Modifier>& stack = mods ? mods->stack : kNoModifiers;
        float w0[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        if (const WorldTransform* t = e.try_get<WorldTransform>())
            std::memcpy(w0, t->rows, sizeof(w0));
        level::Spline curve;
        const bool follows = level::needsSpline(stack) && curveOf(e, w0, curve);
        bool remodified = false;
        const uint32_t scripts = level::hasScript(stack) ? level::scriptModifierGeneration() : 0u;
        if (m.mesh && (entry.modifiedFrom != m.mesh || entry.modifiers != stack || !(entry.curve == curve) || entry.scripts != scripts)) {
            entry.curve = curve;
            entry.scripts = scripts;
            entry.modifiedFrom = m.mesh;
            entry.modifiers = stack;
            entry.modifiedFace.clear();
            entry.modified = m.mesh;
            if (!stack.empty()) {
                auto out = std::make_shared<level::PolyMesh>();
                std::string error;
                level::ModifierContext context{follows ? &entry.curve : nullptr};
                if (level::applyModifiers(*m.mesh, stack, *out, &entry.modifiedFace, &error, &context))
                    entry.modified = std::move(out);
                else if (warnings_++ < 8)
                    FX_WARN("level: entity %llu's modifiers not applied: %s", (unsigned long long)e.id(), error.c_str());
            }
            remodified = true;
        }
        const std::shared_ptr<const level::PolyMesh>& shape = entry.modified ? entry.modified : m.mesh;
        // The cut volumes over it (by bounds; not itself), and what they come to.
        std::vector<level::Cut> cuts;
        uint64_t cutSignature = 0;
        if (shape && !cuts_.empty()) {
            float3 lo, hi, wlo = {1e30f, 1e30f, 1e30f}, whi = {-1e30f, -1e30f, -1e30f};
            shape->bounds(lo, hi);
            for (int k = 0; k < 8; ++k) {
                const float3 p = {k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
                const float3 q = {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                                  w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
                wlo = {std::min(wlo.x, q.x), std::min(wlo.y, q.y), std::min(wlo.z, q.z)};
                whi = {std::max(whi.x, q.x), std::max(whi.y, q.y), std::max(whi.z, q.z)};
            }
            for (const CutInfo& c : cuts_)
                if (c.entity != e.id() && c.lo.x <= whi.x && c.hi.x >= wlo.x && c.lo.y <= whi.y && c.hi.y >= wlo.y && c.lo.z <= whi.z &&
                    c.hi.z >= wlo.z) {
                    cuts.push_back(c.cut);
                    cutSignature = (cutSignature ^ c.signature) * 0x100000001b3ull;
                }
            if (!cuts.empty())  // (in world space: moving the mesh cuts it again)
                for (const float* r = &w[0][0]; r != &w[0][0] + 12; ++r) {
                    uint32_t bits;
                    std::memcpy(&bits, r, 4);
                    cutSignature = (cutSignature ^ bits) * 0x100000001b3ull;
                }
        }
        // Its newest cut, made: drawn now (what it was made from may be older than what is wanted).
        if (entry.cutJob && entry.cutJob->GetIsComplete()) {
            const std::shared_ptr<CutJob> job = std::move(entry.cutJob);
            applyCut(e.id(), entry, *job);
        }
        const std::vector<uint32_t>* modifiedFace = shape != m.mesh ? &entry.modifiedFace : nullptr;
        if (m.mesh && (remodified || entry.built != m.mesh || entry.builtShape != shape || entry.cuts != cutSignature ||
                       (entry.worldUvs && std::memcmp(w, entry.world, sizeof(w)) != 0))) {
            if (cuts.empty()) {  // (uncut: built now; a cut still being made is let go)
                orphan(entry.cutJob);
                entry.cuts = cutSignature;
                build(e.id(), entry, m.mesh, shape, w, {}, nullptr, modifiedFace);
            } else if (!entry.cutJob) {  // (one at a time: the next starts when it is drawn)
                entry.cutJob = startCut(m.mesh, shape, modifiedFace, w, cuts, cutSignature);
                if (entry.cutJob->GetIsComplete()) {  // (baked)
                    const std::shared_ptr<CutJob> job = std::move(entry.cutJob);
                    applyCut(e.id(), entry, *job);
                } else if (!entry.drawn) {
                    firstCuts.push_back(e.id());
                }
            }
        }
        updateLayer(e, entry, m);
        // Shown while it and what is above it are enabled (the editor disables deleted ones).
        bool enabled = true;
        for (flecs::entity at = e; at;) {
            enabled &= at.enabled();
            const Parent* p = at.try_get<Parent>();
            if (!p || !p->entity || !world.is_alive(p->entity))
                break;
            at = flecs::entity(world, p->entity);
        }
        const LevelVolume* volume = e.try_get<LevelVolume>();
        const bool hidden = !enabled || !shape || shape->empty() || volume;  // (volumes aren't drawn)
        // A trigger volume: a box trigger over its mesh's bounds (fitted again when the mesh changes).
        if (volume && volume->trigger() && shape && !shape->empty() && entry.volumeOf != shape.get()) {
            entry.volumeOf = shape.get();
            float3 lo, hi;
            shape->bounds(lo, hi);
            Collider c = {};
            if (const Collider* had = e.try_get<Collider>())
                c = *had;
            c.shape = 0;
            for (int a = 0; a < 3; ++a) {
                c.center[a] = ((&lo.x)[a] + (&hi.x)[a]) * 0.5f;
                c.size[a] = std::max((&hi.x)[a] - (&lo.x)[a], 0.01f);
            }
            c.flags |= 1u;
            e.set<Collider>(c);
        }
        entry.shown = !hidden;
        if (manageVisibility_ && hidden != entry.hidden) {
            entry.hidden = hidden;
            const uint32_t slot = live_->instanceOf(uint32_t(entry.live));
            fpak::InstanceGpu& row = pak_->mutableInstances()[slot];
            std::memcpy(row.world, w, sizeof(row.world));
            row.flags = (row.flags & ~uint32_t(fpak::kInstanceHidden)) | (hidden ? fpak::kInstanceHidden : 0u);
            rows.push_back({slot, row});
        }
    });
    world.defer_end();
    // A mesh's first cut (a scene loading, a mesh made): waited for, so it doesn't show uncut first.
    for (const flecs::entity_t id : firstCuts) {
        Entry& entry = entries_[id];
        if (!entry.cutJob)
            continue;
        jobs::scheduler().WaitforTask(entry.cutJob.get());
        const std::shared_ptr<CutJob> job = std::move(entry.cutJob);
        applyCut(id, entry, *job);
    }
    updateCopies(rows);
    updateWear();
    // Gone (the entity, or its LevelMesh): its live mesh freed, its row hidden.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.wearJob && !it->second.wearJob->GetIsComplete())
            orphans_.push_back(std::move(it->second.wearJob));
        orphan(it->second.cutJob);
        if (it->second.cutResult && it->second.cutResult->clipped)
            ++clippedVersion_;
        if (it->second.live >= 0) {
            dropLayer(it->second);
            const uint32_t live = uint32_t(it->second.live), slot = live_->instanceOf(live);
            live_->destroy(live, it->second.world);
            fpak::InstanceGpu& row = pak_->mutableInstances()[slot];
            row.flags |= fpak::kInstanceHidden;
            rows.push_back({slot, row});
            scene_->forgetInstance(slot);
            if (world.is_alive(it->first))
                flecs::entity(world, it->first).remove<MeshInstance>();
        }
        it = entries_.erase(it);
    }
}

void LevelMeshes::updateLayer(flecs::entity e, Entry& entry, const LevelMesh& m)
{
    if (!layers_ || entry.live < 0 || !m.mesh || !m.mesh->layered()) {
        dropLayer(entry);
        return;
    }
    if (entry.layer < 0) {
        entry.layer = layers_->acquire();
        if (entry.layer < 0) {
            if (warnings_++ < 8)
                FX_WARN("level: no surface layer left for entity %llu (its layers not drawn)", (unsigned long long)e.id());
            return;
        }
        entry.mask = nullptr;
        entry.maskVersion = ~0u;
        std::fill(std::begin(entry.record), std::end(entry.record), ~0u);
        if (setInstanceLayer_)
            setInstanceLayer_(live_->instanceOf(uint32_t(entry.live)), entry.layer);
    }
    // Its record: the layer materials' indices (the library makes room for ones not used yet) and the blend.
    uint32_t record[4];
    for (int k = 0; k < 3; ++k) {
        const std::string& path = m.mesh->layerMaterials[k];
        const int32_t index = path.empty() || !materials_ ? -1 : materials_->indexOf(path);
        record[k] = index < 0 ? SurfaceLayers::kNone : uint32_t(index);
    }
    std::memcpy(&record[3], &m.mesh->layerBlend, 4);
    if (std::memcmp(record, entry.record, sizeof(record)) != 0) {
        layers_->setRecord(entry.layer, record, m.mesh->layerBlend);
        std::memcpy(entry.record, record, sizeof(record));
    }
    uploadMask(e, entry);
}

void LevelMeshes::uploadMask(flecs::entity e, Entry& entry)
{
    if (!layers_ || entry.layer < 0)
        return;
    const LevelMask* mask = e.try_get<LevelMask>();
    const std::vector<uint8_t>* image = mask && mask->rgba && mask->rgba->size() == level::kMaskBytes ? mask->rgba.get() : nullptr;
    if (image == entry.mask && entry.maskVersion == entry.wearVersion)
        return;
    entry.mask = image;
    entry.maskVersion = entry.wearVersion;
    if (entry.wear.size() != level::kMaskBytes) {
        layers_->setMask(entry.layer, image ? image->data() : nullptr);
        return;
    }
    // The painted mask over the live wear: the higher of the two.
    entry.composed = entry.wear;
    if (image)
        for (size_t i = 0; i < entry.composed.size(); ++i)
            entry.composed[i] = std::max(entry.composed[i], (*image)[i]);
    layers_->setMask(entry.layer, entry.composed.data());
}

void LevelMeshes::updateWear()
{
    FX_ZONE_CAT("LevelMeshes::updateWear", Rendering);
    wearPending_ = 0;
    std::erase_if(orphans_, [](const std::shared_ptr<enki::ITaskSet>& job) { return job->GetIsComplete(); });
    flecs::world& world = scene_->world();
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto rowsOf = [&](flecs::entity_t id, float out[3][4]) {
        const WorldTransform* t = flecs::entity(world, id).try_get<WorldTransform>();
        const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        std::memcpy(out, t ? &t->rows[0] : &identity[0][0], sizeof(float) * 12);
    };
    for (auto& [id, entry] : entries_) {
        if (!entry.seen || entry.live < 0)
            continue;
        const flecs::entity e(world, id);
        const uint32_t version = entry.wearVersion;
        if (entry.wearJob && entry.wearJob->GetIsComplete()) {  // (made: drawn now)
            entry.wear = std::move(entry.wearJob->mask);
            entry.wearKey = entry.wearJob->key;
            stats_.lastWearMs = entry.wearJob->ms;
            ++stats_.wearMade;
            entry.wearJob.reset();
            ++entry.wearVersion;
        }
        const LevelWear* lw = e.try_get<LevelWear>();
        if (!lw || lw->rules.empty() || !layers_ || !entry.drawn || !entry.built || !entry.built->layered()) {
            if (!entry.wear.empty() || entry.wearKey) {
                entry.wear.clear();
                entry.wearKey = 0;
                ++entry.wearVersion;
            }
            wearPending_ += entry.wearJob ? 1u : 0u;
        } else {
            // What it is made from: what the mesh is drawn as, the rules, and (grime) where it is
            // and the level meshes that come within reach of it.
            uint64_t h = 0xcbf29ce484222325ull;
            const auto mix = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; ++i)
                    h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
            };
            const void* drawn = entry.drawn.get();
            mix(&drawn, sizeof(drawn));
            float reach = 0.0f;
            for (const level::WearSettings& r : lw->rules) {
                mix(&r.kind, sizeof(r.kind));
                mix(&r.channel, sizeof(r.channel));
                mix(&r.width, sizeof(r.width));
                mix(&r.strength, sizeof(r.strength));
                mix(&r.sharp, sizeof(r.sharp));
                mix(&r.foot, sizeof(r.foot));
                mix(&r.seed, sizeof(r.seed));
                if (r.kind == level::WearKind::Grime)
                    reach = std::max(reach, r.width);
            }
            float w[3][4];
            rowsOf(id, w);
            for (int row = 0; row < 3; ++row)  // (its rotation and scale: wear in the world's sizes)
                mix(w[row], sizeof(float) * 3);
            std::vector<std::pair<std::shared_ptr<const level::PolyMesh>, std::array<float, 12>>> around;
            if (reach > 0.0f) {
                mix(w, sizeof(w));
                float3 lo, hi;
                worldBox(w, entry.lo, entry.hi, lo, hi);
                for (const auto& [otherId, other] : entries_) {
                    if (otherId == id || !other.seen || !other.drawn || other.drawn->empty())
                        continue;
                    float ow[3][4];
                    rowsOf(otherId, ow);
                    float3 olo, ohi;
                    worldBox(ow, other.lo, other.hi, olo, ohi);
                    if (olo.x > hi.x + reach || ohi.x < lo.x - reach || olo.y > hi.y + reach || ohi.y < lo.y - reach ||
                        olo.z > hi.z + reach || ohi.z < lo.z - reach)
                        continue;
                    std::array<float, 12> rows;
                    std::memcpy(rows.data(), ow, sizeof(ow));
                    around.push_back({other.drawn, rows});
                    const void* p = other.drawn.get();
                    mix(&p, sizeof(p));
                    mix(ow, sizeof(ow));
                }
            }
            h |= 1u;
            if (h != entry.wearKey) {
                ++wearPending_;
                // One job at a time each, and (while it keeps changing: dragged) not more often than
                // every 150 ms; the newest asked for once the last is done.
                if (!entry.wearJob && (entry.wear.empty() || now - entry.wearStartNs >= 150'000'000ull)) {
                    auto job = std::make_shared<WearJob>();
                    job->mesh = entry.drawn;
                    job->rules = lw->rules;
                    std::memcpy(job->world, w, sizeof(w));
                    job->around = std::move(around);
                    job->key = h;
                    entry.wearJob = job;
                    entry.wearStartNs = now;
                    jobs::scheduler().AddTaskSetToPipe(job.get());
                }
            }
        }
        if (entry.wearVersion != version)
            uploadMask(e, entry);
    }
}

const std::vector<uint8_t>* LevelMeshes::wearOf(flecs::entity_t entity) const
{
    const auto it = entries_.find(entity);
    return it != entries_.end() && it->second.wear.size() == level::kMaskBytes ? &it->second.wear : nullptr;
}

void LevelMeshes::dropLayer(Entry& entry)
{
    if (entry.layer < 0)
        return;
    layers_->release(entry.layer);
    if (setInstanceLayer_ && entry.live >= 0)
        setInstanceLayer_(live_->instanceOf(uint32_t(entry.live)), -1);
    entry.layer = -1;
}

std::shared_ptr<LevelMeshes::CutJob> LevelMeshes::startCut(const std::shared_ptr<const level::PolyMesh>& from,
                                                          const std::shared_ptr<const level::PolyMesh>& target,
                                                          const std::vector<uint32_t>* modifiedFace, const float world[3][4],
                                                          const std::vector<level::Cut>& cuts, uint64_t signature)
{
    auto job = std::make_shared<CutJob>();
    job->from = from;
    job->target = target;
    if (modifiedFace)
        job->modifiedFace = *modifiedFace;
    std::memcpy(job->world, world, sizeof(job->world));
    job->cuts = cuts;
    job->signature = signature;
    if (!bake_.results.empty()) {
        const auto found = bake_.results.find(level::cutKey(level::meshKey(*target), world, cuts));
        if (found != bake_.results.end()) {
            job->result = found->second;
            job->baked = true;
            return job;
        }
    }
    // What building its result needs, looked up now: the names it can have (the target's, the
    // cuts' caps), as build() looks them up.
    job->prepare = live_ != nullptr;
    if (job->prepare) {
        job->defaultMaterial = uint32_t(std::max(defaultMaterialIndex(), 0));
        const auto look = [&](const std::string& name) {
            if (name.empty() || name.starts_with("scene:") || job->materialIndex.count(name))
                return;
            const int32_t index = materials_ ? materials_->indexOf(name) : -1;
            job->materialIndex[name] = !materials_ ? job->defaultMaterial : index >= 0 ? uint32_t(index) : 0u;
        };
        for (const std::string& name : target->materials)
            look(name);
        for (const level::Cut& cut : cuts)
            look(cut.capMaterial);
        job->blended = live_->blendedMaterials();
        job->traced = live_->traced();
        job->collider = live_->makesColliders();
    }
    jobs::scheduler().AddTaskSetToPipe(job.get());
    return job;
}

void LevelMeshes::applyCut(flecs::entity_t id, Entry& entry, CutJob& job)
{
    if (job.result) {
        ++stats_.cuts;
        if (job.baked)
            ++stats_.bakedCuts;
        else
            stats_.lastCutMs = job.ms;
    } else if (!job.error.empty() && cutWarnings_++ < 8) {
        FX_WARN("level: entity %llu not cut: %s", (unsigned long long)id, job.error.c_str());
    }
    entry.cuts = job.signature;
    build(id, entry, job.from, job.target, job.world, job.cuts, job.result, job.modifiedFace.empty() ? nullptr : &job.modifiedFace,
          &job);
}

void LevelMeshes::applyCopy(uint32_t slot, flecs::entity_t id, Copy& copy, CutJob& job, std::vector<InstanceUpdate>& rows)
{
    copy.signature = job.signature;
    if (bool(copy.result && copy.result->clipped) != bool(job.result && job.result->clipped))
        ++clippedVersion_;
    if (!job.result) {
        if (!job.error.empty() && cutWarnings_++ < 8)
            FX_WARN("level: instance %u not cut: %s", slot, job.error.c_str());
        dropCopy(slot, copy, rows);
        return;
    }
    ++stats_.cuts;
    if (job.baked)
        ++stats_.bakedCuts;
    else
        stats_.lastCutMs = job.ms;
    copy.target = job.target;
    std::memcpy(copy.world, job.world, sizeof(copy.world));
    copy.cutWith = job.cuts;
    copy.result = job.result;
    if (copy.live < 0 && (copy.live = live_->create()) < 0) {
        if (warnings_++ < 8)
            FX_WARN("level: no live mesh left for a cut copy (--live-meshes)");
        return;
    }
    Entry scratch;
    scratch.live = copy.live;
    build(id, scratch, job.result->mesh, job.result->mesh, job.world, {}, nullptr, nullptr, &job);
    const uint32_t copySlot = live_->instanceOf(uint32_t(copy.live));
    fpak::InstanceGpu& row = pak_->mutableInstances()[copySlot];
    const fpak::InstanceGpu& original = pak_->instances()[slot];
    std::memcpy(row.world, job.world, sizeof(row.world));
    row.mesh = live_->meshOf(uint32_t(copy.live));
    row.flags = (original.flags & 0xffff0000u) | (original.flags & fpak::kInstanceNoShadow);  // (its material override, shadows)
    row.tint = original.tint;
    rows.push_back({copySlot, row});
    if (setInstanceHidden_)
        setInstanceHidden_(slot, true);
}

void LevelMeshes::orphan(std::shared_ptr<CutJob>& job)
{
    if (job && !job->GetIsComplete())
        orphans_.push_back(std::move(job));
    job.reset();
}

int32_t LevelMeshes::defaultMaterialIndex() const
{
    if (defaultIndex_ == -2)
        defaultIndex_ = materials_ && !defaultMaterial_.empty() ? materials_->indexOf(defaultMaterial_) : -1;
    return defaultIndex_;
}

bool LevelMeshes::clipped(flecs::entity_t entity) const
{
    const auto it = entries_.find(entity);
    return it != entries_.end() && it->second.cutResult && it->second.cutResult->clipped;
}

bool LevelMeshes::copyClipped(uint32_t slot) const
{
    const auto it = copies_.find(slot);
    return it != copies_.end() && it->second.live >= 0 && it->second.result && it->second.result->clipped;
}

uint32_t LevelMeshes::cutsPending() const
{
    uint32_t n = 0;
    for (const auto& [id, entry] : entries_)
        n += entry.cutJob ? 1u : 0u;
    for (const auto& [slot, copy] : copies_)
        n += copy.job ? 1u : 0u;
    return n;
}

bool LevelMeshes::loadBake(const std::string& scenePath)
{
    if (scenePath.empty() || !bake_.loadFile(level::cutBakePathFor(scenePath)))
        return false;
    FX_INFO("level: %zu baked cuts", bake_.results.size());
    return true;
}

bool LevelMeshes::saveBake(const std::string& scenePath) const
{
    if (scenePath.empty())
        return false;
    // What is drawn cut now, each under the key of what it was cut from (a target's key once).
    level::CutBake bake;
    std::unordered_map<const level::PolyMesh*, uint64_t> keys;
    const auto add = [&](const std::shared_ptr<const level::PolyMesh>& target, const float world[3][4], const std::vector<level::Cut>& cuts,
                         const std::shared_ptr<const level::BakedCut>& result) {
        auto [key, fresh] = keys.try_emplace(target.get());
        if (fresh)
            key->second = level::meshKey(*target);
        bake.results[level::cutKey(key->second, world, cuts)] = result;
    };
    for (const auto& [id, entry] : entries_)
        if (entry.cutResult && entry.cutTarget)
            add(entry.cutTarget, entry.world, entry.cutWith, entry.cutResult);
    for (const auto& [slot, copy] : copies_)
        if (copy.live >= 0 && copy.result && copy.target)
            add(copy.target, copy.world, copy.cutWith, copy.result);
    return bake.saveFile(level::cutBakePathFor(scenePath));
}

void LevelMeshes::dropCopy(uint32_t slot, Copy& copy, std::vector<InstanceUpdate>& rows)
{
    orphan(copy.job);
    if (copy.result && copy.result->clipped)
        ++clippedVersion_;
    copy.result = nullptr;
    if (copy.live >= 0 && live_) {
        const uint32_t live = uint32_t(copy.live), copySlot = live_->instanceOf(live);
        const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        live_->destroy(live, identity);
        fpak::InstanceGpu& row = pak_->mutableInstances()[copySlot];
        row.flags |= fpak::kInstanceHidden;
        rows.push_back({copySlot, row});
    }
    copy.live = -1;
    if (setInstanceHidden_)
        setInstanceHidden_(slot, false);
}

void LevelMeshes::updateCopies(std::vector<InstanceUpdate>& rows)
{
    if (cuts_.empty() && copies_.empty())
        return;
    FX_ZONE_CAT("LevelMeshes::updateCopies", Rendering);
    for (auto& [slot, copy] : copies_)
        copy.seen = false;
    const auto& meshes = pak_->meshes();
    std::vector<std::pair<uint32_t, flecs::entity_t>> firstCopies;
    scene_->world().each([&](flecs::entity e, const MeshInstance& mi, const WorldTransform& t) {
        if (e.has<LevelMesh>() || live_->liveOfInstance(mi.gpuSlot) >= 0 || mi.mesh >= meshes.size() || !e.enabled())
            return;
        // Its world bounds, and the cut volumes over it.
        const fpak::MeshGpu& mg = meshes[mi.mesh];
        float3 wlo = {1e30f, 1e30f, 1e30f}, whi = {-1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 8; ++k) {
            const float3 p = {mg.posOffset[0] + (k & 1 ? mg.posScale[0] * 65535.0f : 0.0f), mg.posOffset[1] + (k & 2 ? mg.posScale[1] * 65535.0f : 0.0f),
                              mg.posOffset[2] + (k & 4 ? mg.posScale[2] * 65535.0f : 0.0f)};
            const float* r = t.rows;
            const float3 q = {r[0] * p.x + r[1] * p.y + r[2] * p.z + r[3], r[4] * p.x + r[5] * p.y + r[6] * p.z + r[7],
                              r[8] * p.x + r[9] * p.y + r[10] * p.z + r[11]};
            wlo = {std::min(wlo.x, q.x), std::min(wlo.y, q.y), std::min(wlo.z, q.z)};
            whi = {std::max(whi.x, q.x), std::max(whi.y, q.y), std::max(whi.z, q.z)};
        }
        std::vector<level::Cut> cuts;
        uint64_t signature = 0xcbf29ce484222325ull ^ mi.mesh;
        for (const CutInfo& c : cuts_)
            if (c.entity != e.id() && c.lo.x <= whi.x && c.hi.x >= wlo.x && c.lo.y <= whi.y && c.hi.y >= wlo.y && c.lo.z <= whi.z &&
                c.hi.z >= wlo.z) {
                cuts.push_back(c.cut);
                signature = (signature ^ c.signature) * 0x100000001b3ull;
            }
        const auto existing = copies_.find(mi.gpuSlot);
        if (cuts.empty()) {
            if (existing != copies_.end()) {
                dropCopy(mi.gpuSlot, existing->second, rows);
                copies_.erase(existing);
            }
            return;
        }
        for (int k = 0; k < 12; ++k) {
            uint32_t bits;
            std::memcpy(&bits, &t.rows[k], 4);
            signature = (signature ^ bits) * 0x100000001b3ull;
        }
        Copy& copy = copies_[mi.gpuSlot];
        copy.seen = true;
        if (copy.job && copy.job->GetIsComplete()) {  // (its newest cut, made: drawn now)
            const std::shared_ptr<CutJob> job = std::move(copy.job);
            applyCopy(mi.gpuSlot, e.id(), copy, *job, rows);
        }
        if (copy.signature == signature || copy.job)
            return;
        // The cooked mesh read back once, cut where it is.
        auto [cached, fresh] = pakMeshes_.try_emplace(mi.mesh);
        if (fresh) {
            auto polygon = std::make_shared<level::PolyMesh>();
            if (level::meshFromPak(*pak_, mi.mesh, *polygon))
                cached->second = std::move(polygon);
            else if (cutWarnings_++ < 8)
                FX_WARN("level: mesh %u can't be read back for cuts", mi.mesh);
        }
        if (!cached->second) {
            copy.signature = signature;
            dropCopy(mi.gpuSlot, copy, rows);
            return;
        }
        float w[3][4];
        std::memcpy(w, t.rows, sizeof(w));
        copy.job = startCut(nullptr, cached->second, nullptr, w, cuts, signature);
        if (copy.job->GetIsComplete()) {  // (baked)
            const std::shared_ptr<CutJob> job = std::move(copy.job);
            applyCopy(mi.gpuSlot, e.id(), copy, *job, rows);
        } else if (copy.live < 0) {
            firstCopies.push_back({mi.gpuSlot, e.id()});
        }
    });
    // A copy's first cut: waited for, so the art doesn't show uncut first.
    for (const auto& [slot, id] : firstCopies) {
        const auto it = copies_.find(slot);
        if (it == copies_.end() || !it->second.job)
            continue;
        jobs::scheduler().WaitforTask(it->second.job.get());
        const std::shared_ptr<CutJob> job = std::move(it->second.job);
        applyCopy(slot, id, it->second, *job, rows);
    }
    for (auto it = copies_.begin(); it != copies_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        dropCopy(it->first, it->second, rows);
        it = copies_.erase(it);
    }
}

int32_t LevelMeshes::copyOf(uint32_t slot) const
{
    const auto it = copies_.find(slot);
    return it != copies_.end() && it->second.live >= 0 && live_ ? int32_t(live_->instanceOf(uint32_t(it->second.live))) : -1;
}

int32_t LevelMeshes::originalOf(uint32_t liveSlot) const
{
    for (const auto& [slot, copy] : copies_)
        if (copy.live >= 0 && live_ && live_->instanceOf(uint32_t(copy.live)) == liveSlot)
            return int32_t(slot);
    return -1;
}

int32_t LevelMeshes::faceOf(flecs::entity_t entity, uint32_t cluster, uint32_t triangle) const
{
    const auto it = entries_.find(entity);
    if (it == entries_.end() || it->second.live < 0 || !live_)
        return -1;
    const int32_t t = live_->sourceTriangle(uint32_t(it->second.live), cluster, triangle);
    return t >= 0 && size_t(t) < it->second.faceOfTriangle.size() ? int32_t(it->second.faceOfTriangle[size_t(t)]) : -1;
}

std::shared_ptr<const level::PolyMesh> LevelMeshes::drawnOf(flecs::entity_t entity) const
{
    const auto it = entries_.find(entity);
    return it == entries_.end() ? nullptr : it->second.drawn;
}

int32_t LevelMeshes::liveOf(flecs::entity_t entity) const
{
    const auto it = entries_.find(entity);
    return it == entries_.end() ? -1 : it->second.live;
}

}  // namespace fx
