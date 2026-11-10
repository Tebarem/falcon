#include "scene/scene.h"

#include "assets/scene_pak.h"
#include "core/profile.h"
#include "vfx/vfx_components.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace fx {

void affineMultiply(const float a[3][4], const float b[3][4], float out[3][4])
{
    float r[3][4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
    std::memcpy(out, r, sizeof(r));
}

void affineInverse(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
                k = m[2][2];
    const float co[3][3] = {{e * k - f * h, c * h - b * k, b * f - c * e},
                            {f * g - d * k, a * k - c * g, c * d - a * f},
                            {d * h - e * g, b * g - a * h, a * e - b * d}};
    const float det = a * co[0][0] + b * co[1][0] + c * co[2][0];
    float r[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    if (std::fabs(det) > 1e-20f) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = co[i][j] / det;
        for (int i = 0; i < 3; ++i)
            r[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]);
    }
    std::memcpy(out, r, sizeof(r));
}

void Scene::init()
{
    FX_ZONE_CAT("Scene::init", Rendering);
    world_.component<WorldTransform>("WorldTransform");
    world_.component<LocalTransform>("LocalTransform");
    world_.component<Parent>("Parent");
    world_.component<MeshInstance>("MeshInstance");
    world_.component<Light>("Light");
    world_.component<CameraComponent>("CameraComponent");
    world_.component<RigidBody>("RigidBody");
    world_.component<PhysicsBody>("PhysicsBody");
    world_.component<Collider>("Collider");
    world_.component<AudioSource>("AudioSource");
    world_.component<AudioListener>("AudioListener");
    world_.component<AcousticSurface>("AcousticSurface");
    registerVfxComponents(world_);
    extract_ = world_.query_builder<const WorldTransform, const MeshInstance>()
                   .without<RigidBody>()
                   .cached()
                   .detect_changes()
                   .build();
    // Disabled children too: they move with their parent while deleted, so undo brings them
    // back in place.
    children_ = world_.query_builder<const Parent>().query_flags(EcsQueryMatchDisabled).cached().build();
}

void Scene::extractChanged(const ScenePak& pak, std::vector<InstanceUpdate>& rows)
{
    FX_ZONE_CAT("Scene::extractChanged", Rendering);
    const auto& table = pak.instances();
    extract_.run([&](flecs::iter& it) {
        while (it.next()) {
            if (!it.changed()) {
                it.skip();
                continue;
            }
            auto t = it.field<const WorldTransform>(0);
            auto m = it.field<const MeshInstance>(1);
            for (auto i : it) {
                fpak::InstanceGpu r = table[m[i].gpuSlot];
                std::memcpy(r.world, t[i].rows, sizeof(r.world));
                rows.push_back({m[i].gpuSlot, r});
            }
        }
    });
}

void Scene::spawnFromPak(const ScenePak& pak)
{
    FX_ZONE_CAT("Scene::spawnFromPak", Rendering);
    uint32_t count = 0;
    const fpak::InstanceGpu* instances = pak.view<fpak::InstanceGpu>(fpak::Section::Instances, &count);
    for (uint32_t i = 0; i < count; ++i)
        spawnInstance(i, instances[i]);
    instanceCount_ = count;
}

flecs::entity Scene::spawnInstance(uint32_t slot, const fpak::InstanceGpu& row)
{
    WorldTransform t;
    std::memcpy(t.rows, row.world, sizeof(t.rows));
    const flecs::entity e = world_.entity().set<WorldTransform>(t).set<MeshInstance>({row.mesh, slot});
    if (slot >= slots_.size())
        slots_.resize(slot + 1, 0);
    slots_[slot] = e.id();
    return e;
}

flecs::entity Scene::spawnTransform(const float world[3][4])
{
    WorldTransform t;
    std::memcpy(t.rows, world, sizeof(t.rows));
    return world_.entity().set<WorldTransform>(t);
}

void Scene::spawnLights(const ScenePak& pak, uint32_t shadowBudget)
{
    uint32_t cooked = 0;
    const fpak::LightGpu* pakLights = pak.view<fpak::LightGpu>(fpak::Section::Lights, &cooked);
    std::vector<fpak::LightGpu> src(pakLights, pakLights + cooked);
    bool haveSun = false;
    for (const fpak::LightGpu& g : src)
        haveSun |= uint32_t(g.color[3]) == fpak::kLightDirectional;
    if (!haveSun) {
        // The default sun: high in the sky, a clear day's warm white. Its entity sits at the
        // scene's center, where its icon is usually in view (a directional light's position
        // does not matter).
        const fpak::Header& h = pak.header();
        fpak::LightGpu g = {};
        const float3 toward = normalize(float3{0.35f, 0.9f, 0.25f});
        g.positionRange[0] = (h.boundsMin[0] + h.boundsMax[0]) * 0.5f;
        g.positionRange[1] = (h.boundsMin[1] + h.boundsMax[1]) * 0.5f;
        g.positionRange[2] = (h.boundsMin[2] + h.boundsMax[2]) * 0.5f;
        g.color[0] = 3.2f;
        g.color[1] = 3.04f;
        g.color[2] = 2.72f;
        g.color[3] = float(fpak::kLightDirectional);
        g.direction[0] = -toward.x;
        g.direction[1] = -toward.y;
        g.direction[2] = -toward.z;
        src.push_back(g);
    }
    const uint32_t count = uint32_t(src.size());
    lights_.assign(count, 0);
    std::vector<uint32_t> shadowOrder;
    haveSun = false;
    for (uint32_t i = 0; i < count; ++i) {
        const fpak::LightGpu& g = src[i];
        const uint32_t type = uint32_t(g.color[3]);
        const float intensity = std::max({g.color[0], g.color[1], g.color[2]});
        if (type == fpak::kLightDirectional && haveSun)
            continue;  // one sun
        if (type != fpak::kLightDirectional && intensity <= 0.0f)
            continue;  // authored with zero intensity
        Light l = {};
        l.type = type == fpak::kLightDirectional ? 2u : type == fpak::kLightSpot ? 1u : 0u;
        haveSun |= l.type == 2;
        for (int c = 0; c < 3; ++c)
            l.color[c] = g.color[c] / intensity;
        l.intensity = intensity;
        if (intensity <= 0.0f)
            l.color[0] = l.color[1] = l.color[2] = 1.0f;
        l.range = g.positionRange[3];
        l.flags = l.type == 2 ? 1u : 0u;  // the sun's shadows
        l.outerAngle = l.type ? std::acos(std::clamp(g.direction[3], -1.0f, 1.0f)) * 57.29578f : 30.0f;
        l.innerAngle = l.type ? std::acos(std::clamp(g.params[0], -1.0f, 1.0f)) * 57.29578f : 20.0f;
        // World transform: -Z along the light's direction.
        const float3 f = normalize(float3{g.direction[0], g.direction[1], g.direction[2]});
        const float3 z = length(f) > 0.5f ? f * -1.0f : float3{0, 0, 1};
        const float3 x = normalize(cross(std::fabs(z.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}, z));
        const float3 y = cross(z, x);
        const float world[3][4] = {{x.x, y.x, z.x, g.positionRange[0]}, {x.y, y.y, z.y, g.positionRange[1]},
                                   {x.z, y.z, z.z, g.positionRange[2]}};
        lights_[i] = spawnTransform(world).set<Light>(l).id();
        if (l.type != 2)
            shadowOrder.push_back(i);
    }
    // Shadow casters within the budget: spots first, then the brightest (as LightSystem always did).
    std::stable_sort(shadowOrder.begin(), shadowOrder.end(), [&](uint32_t a, uint32_t b) {
        const bool sa = uint32_t(src[a].color[3]) == fpak::kLightSpot, sb = uint32_t(src[b].color[3]) == fpak::kLightSpot;
        const float ia = std::max({src[a].color[0], src[a].color[1], src[a].color[2]});
        const float ib = std::max({src[b].color[0], src[b].color[1], src[b].color[2]});
        return sa != sb ? sa : ia > ib;
    });
    for (uint32_t k = 0; k < shadowOrder.size() && k < shadowBudget; ++k) {
        flecs::entity e(world_, lights_[shadowOrder[k]]);
        Light l = *e.try_get<Light>();
        l.flags |= 1u;
        e.set<Light>(l);
    }
    spawnedLights_.assign(count, {});
    for (uint32_t i = 0; i < count; ++i)
        if (lights_[i]) {
            const flecs::entity e(world_, lights_[i]);
            spawnedLights_[i].light = *e.try_get<Light>();
            std::memcpy(spawnedLights_[i].world, e.try_get<WorldTransform>()->rows, sizeof(spawnedLights_[i].world));
        }
}

bool Scene::mainCamera(float world[3][4], CameraComponent& camera) const
{
    flecs::entity_t best = 0;
    world_.each([&](flecs::entity e, const CameraComponent& c, const WorldTransform& t) {
        if (!(c.flags & 1u) || (best && e.id() > best))
            return;
        best = e.id();
        camera = c;
        std::memcpy(world, t.rows, sizeof(t.rows));
    });
    return best != 0;
}

bool Scene::cookedLight(uint32_t index, Light& light, float world[3][4]) const
{
    if (index >= lights_.size() || !lights_[index])
        return false;
    light = spawnedLights_[index].light;
    std::memcpy(world, spawnedLights_[index].world, sizeof(float) * 12);
    return true;
}

flecs::entity Scene::parentOf(flecs::entity e) const
{
    const Parent* p = e ? e.try_get<Parent>() : nullptr;
    return p && p->entity && world_.is_alive(p->entity) ? flecs::entity(world_, p->entity) : flecs::entity();
}

void Scene::childrenOf(flecs::entity_t e, std::vector<flecs::entity_t>& out) const
{
    if (!e)
        return;
    children_.each([&](flecs::entity child, const Parent& p) {
        if (p.entity == e)
            out.push_back(child.id());
    });
}

void Scene::descendantsOf(flecs::entity_t e, std::vector<flecs::entity_t>& out) const
{
    if (!e)
        return;
    std::unordered_multimap<flecs::entity_t, flecs::entity_t> below;  // parent -> child
    children_.each([&](flecs::entity child, const Parent& p) { below.emplace(p.entity, child.id()); });
    std::unordered_set<flecs::entity_t> seen = {e};  // (a cycle: each once)
    const size_t first = out.size();
    out.push_back(e);
    for (size_t i = first; i < out.size(); ++i)
        for (auto [it, end] = below.equal_range(out[i]); it != end; ++it)
            if (seen.insert(it->second).second)
                out.push_back(it->second);
    out.erase(out.begin() + ptrdiff_t(first));
}

void Scene::setParent(flecs::entity child, flecs::entity parent)
{
    if (!child)
        return;
    const WorldTransform* w = child.try_get<WorldTransform>();
    float world[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    if (w)
        std::memcpy(world, w->rows, sizeof(world));
    if (!parent) {
        child.remove<Parent>().remove<LocalTransform>();
        return;
    }
    child.set<Parent>({parent.id()});
    setWorld(child, world);
}

void Scene::setLocal(flecs::entity child, flecs::entity parent, const float local[3][4])
{
    LocalTransform l;
    std::memcpy(l.rows, local, sizeof(l.rows));
    child.set<Parent>({parent.id()}).set<LocalTransform>(l);
}

void Scene::setWorld(flecs::entity e, const float world[3][4])
{
    WorldTransform t;
    std::memcpy(t.rows, world, sizeof(t.rows));
    e.set<WorldTransform>(t);
    const flecs::entity parent = parentOf(e);
    if (!parent)
        return;
    float parentWorld[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, inverse[3][4];
    if (const WorldTransform* pw = parent.try_get<WorldTransform>())
        std::memcpy(parentWorld, pw->rows, sizeof(parentWorld));
    affineInverse(parentWorld, inverse);
    LocalTransform l;
    affineMultiply(inverse, world, rows3x4(l.rows));
    e.set<LocalTransform>(l);
}

void Scene::propagate()
{
    FX_ZONE_CAT("Scene::propagate", Rendering);
    // The children and their parents; in parent-first order (by depth below a root), walked again
    // only when they change.
    const size_t before = found_.size();
    size_t n = 0;
    bool same = true;
    children_.each([&](flecs::entity e, const Parent& p) {
        const std::pair<flecs::entity_t, flecs::entity_t> pair = {e.id(), p.entity};
        if (n < before && same && found_[n] == pair) {
            ++n;
            return;
        }
        if (same) {
            same = false;
            found_.resize(n);
        }
        found_.push_back(pair);
        ++n;
    });
    if (!same || n != before) {
        found_.resize(n);
        std::vector<std::pair<uint32_t, size_t>> depths;  // (depth, index in found_)
        depths.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            uint32_t depth = 0;
            for (flecs::entity p = parentOf(flecs::entity(world_, found_[i].first)); p && depth < 256; p = parentOf(p))
                ++depth;
            depths.push_back({depth, i});
        }
        std::stable_sort(depths.begin(), depths.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        order_.clear();
        for (const auto& [depth, i] : depths)
            order_.push_back(found_[i]);
    }
    for (const auto& [child, parentId] : order_) {
        if (!world_.is_alive(child) || !parentId || !world_.is_alive(parentId))
            continue;
        const flecs::entity e(world_, child);
        const LocalTransform* local = e.try_get<LocalTransform>();
        const WorldTransform* pw = flecs::entity(world_, parentId).try_get<WorldTransform>();
        if (!local || !pw)
            continue;
        WorldTransform t;
        affineMultiply(rows3x4(pw->rows), rows3x4(local->rows), rows3x4(t.rows));
        const WorldTransform* now = e.try_get<WorldTransform>();
        if (!now || std::memcmp(now->rows, t.rows, sizeof(t.rows)) != 0)
            e.set<WorldTransform>(t);
    }
}

}  // namespace fx
