#include "vfx/runtime_meshes.h"

#include "assets/scene_pak.h"
#include "core/log.h"
#include "core/math.h"
#include "editor/prefab.h"
#include "editor/scene_edits.h"
#include "vfx/particle_system.h"
#include "vfx/vfx_components.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>

namespace fx {

namespace {

// Pool sizes: a scene's Model Renderer draws its most; a prefab's may play several times at once
// (impacts), and a prefab's mesh be spawned many times.
constexpr uint32_t kPrefabCopies = 8, kPrefabMeshSlots = 32, kEditorShapeSlots = 64;
constexpr uint32_t kMaxPoolSlots = 8192, kMaxSlots = 32768;

void addNeed(std::map<std::string, uint32_t>& out, const std::string& mesh, uint32_t count)
{
    if (!mesh.empty() && count > 0)
        out[mesh] += count;
}

void addModels(std::map<std::string, uint32_t>& out, const VfxSet& vfx, uint32_t copies)
{
    if (const ParticleModelRenderer* m = vfx.get<ParticleModelRenderer>())
        addNeed(out, vfxAssetPath(m->mesh), std::max(m->maxModels, 1u) * copies);
}

// Rotation (degrees) as s&box's angles: yaw about +Y, then pitch about +X, then roll about +Z.
void eulerRows(const float a[3], float r[3][3])
{
    constexpr float k = 3.14159265f / 180.0f;
    const float cp = std::cos(a[0] * k), sp = std::sin(a[0] * k);
    const float cy = std::cos(a[1] * k), sy = std::sin(a[1] * k);
    const float cr = std::cos(a[2] * k), sr = std::sin(a[2] * k);
    // Ry * Rx * Rz
    r[0][0] = cy * cr + sy * sp * sr, r[0][1] = -cy * sr + sy * sp * cr, r[0][2] = sy * cp;
    r[1][0] = cp * sr, r[1][1] = cp * cr, r[1][2] = -sp;
    r[2][0] = -sy * cr + cy * sp * sr, r[2][1] = sy * sr + cy * sp * cr, r[2][2] = cy * cp;
}

uint8_t unorm8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

std::vector<RuntimeMeshes::Need> RuntimeMeshes::needs(const SceneEdits& edits, const std::string& projectDir, bool editor)
{
    std::map<std::string, uint32_t> out;
    for (const auto* list : {&edits.instances, &edits.created})
        for (const EntityEdit& e : *list)
            if (!e.deleted)
                addModels(out, e.vfx, 1);
    // The project's prefabs (any may be spawned): under Assets.
    if (!projectDir.empty()) {
        const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(u8(projectDir) / "Assets",
                                                                     std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".fprefab")
                continue;
            const std::u8string name = it->path().u8string();
            Prefab prefab;
            std::string error;
            if (!readPrefab(std::string(name.begin(), name.end()), prefab, error))
                continue;
            for (const PrefabEntity& p : prefab.entities) {
                addModels(out, p.vfx, kPrefabCopies);
                if (p.kind == "mesh") {
                    addNeed(out, p.mesh, kPrefabMeshSlots);
                    if (!p.material.empty())
                        addNeed(out, displacedMeshName(p.mesh, p.material), kPrefabMeshSlots);
                }
            }
        }
    }
    if (editor)
        for (const char* shape : {"cube", "sphere", "cylinder", "capsule", "cone", "plane"})
            addNeed(out, shape, kEditorShapeSlots);
    std::vector<Need> needs;
    for (const auto& [mesh, count] : out)
        needs.push_back({mesh, count});
    return needs;
}

void RuntimeMeshes::reserve(ScenePak& pak, const std::vector<Need>& needs)
{
    uint32_t skinCount = 0;
    const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
    uint32_t total = 0;
    for (const Need& n : needs) {
        const int32_t mesh = pak.findMesh(n.mesh);
        if (mesh < 0 || pools_.count(uint32_t(mesh)))
            continue;
        if (skins && uint32_t(mesh) < skinCount && skins[mesh].firstSkinVertex != fpak::kNotSkinned)
            continue;  // (a skinned slot carries a character)
        const uint32_t size = std::min({n.count, kMaxPoolSlots, kMaxSlots - total});
        if (size == 0)
            break;
        Pool p;
        p.first = pak.addInstancePool(uint32_t(mesh), size);
        p.size = size;
        for (uint32_t k = size; k-- > 0;)
            p.free.push_back(p.first + k);
        if (firstSlot_ == ~0u)
            firstSlot_ = p.first;
        slotMesh_.resize(p.first + size - firstSlot_, ~0u);
        std::fill(slotMesh_.begin() + (p.first - firstSlot_), slotMesh_.end(), uint32_t(mesh));
        pools_[uint32_t(mesh)] = std::move(p);
        total += size;
    }
    stats_.pools = uint32_t(pools_.size());
    stats_.slots = total;
    if (total)
        FX_INFO("runtime meshes: %u slots for %zu meshes (particle models, spawned prefabs)", total, pools_.size());
}

uint32_t RuntimeMeshes::acquire(uint32_t mesh)
{
    const auto it = pools_.find(mesh);
    if (it == pools_.end() || it->second.free.empty())
        return ~0u;
    const uint32_t slot = it->second.free.back();
    it->second.free.pop_back();
    return slot;
}

void RuntimeMeshes::release(ScenePak& pak, uint32_t slot, std::vector<InstanceUpdate>& rows)
{
    fpak::InstanceGpu& row = pak.mutableInstances()[slot];
    row.flags |= fpak::kInstanceHidden;
    rows.push_back({slot, row});
    if (slot >= firstSlot_ && slot - firstSlot_ < slotMesh_.size())
        if (const auto it = pools_.find(slotMesh_[slot - firstSlot_]); it != pools_.end())
            it->second.free.push_back(slot);
}

int32_t RuntimeMeshes::meshOf(const ScenePak& pak, uint32_t meshId)
{
    const auto [it, added] = meshIds_.try_emplace(meshId, -1);
    if (added)
        it->second = meshId ? pak.findMesh(vfxAssetPath(meshId)) : -1;
    return it->second;
}

std::string RuntimeMeshes::missing(const ScenePak& pak, uint32_t meshId) const
{
    if (!meshId)
        return "No mesh chosen";
    const std::string& name = vfxAssetPath(meshId);
    const int32_t mesh = pak.findMesh(name);
    if (mesh < 0)
        return "No mesh " + name + " in this scene";
    if (!has(uint32_t(mesh))) {
        uint32_t skinCount = 0;
        const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins, &skinCount);
        if (skins && uint32_t(mesh) < skinCount && skins[mesh].firstSkinVertex != fpak::kNotSkinned)
            return "Skinned meshes can't be particles";
        return name + " has no model slots this session: save and Reload the scene";
    }
    return {};
}

flecs::entity RuntimeMeshes::spawn(Scene& scene, ScenePak& pak, uint32_t mesh, const float world[3][4], int32_t material, bool hidden)
{
    const uint32_t slot = acquire(mesh);
    if (slot == ~0u)
        return {};
    fpak::InstanceGpu& row = pak.mutableInstances()[slot];
    std::memcpy(row.world, world, sizeof(row.world));
    row.flags = (row.flags & ~uint32_t(fpak::kInstanceHidden) & 0xffffu) | (hidden ? fpak::kInstanceHidden : 0u) |
                (material >= 0 ? (uint32_t(material) + 1) << fpak::kInstanceMaterialShift : 0u);
    row.tint = 0;
    const flecs::entity e = scene.spawnInstance(slot, row);
    spawned_.push_back({e.id(), slot, hidden, hidden});
    pending_.push_back({slot, row});
    return e;
}

bool RuntimeMeshes::setHidden(flecs::entity_t entity, bool hidden)
{
    for (Spawned& s : spawned_)
        if (s.entity == entity) {
            s.ownHidden = hidden;  // (update() streams the change)
            return true;
        }
    return false;
}

void RuntimeMeshes::update(Scene& scene, ScenePak& pak, const ParticleSystem& particles, std::vector<InstanceUpdate>& rows)
{
    if (pools_.empty())
        return;
    ++frame_;
    stats_.models = stats_.starved = 0;
    flecs::world& world = scene.world();
    rows.insert(rows.end(), pending_.begin(), pending_.end());
    pending_.clear();

    // Spawned meshes: shown while they and what is above them are enabled; gone with them.
    for (size_t i = 0; i < spawned_.size();) {
        Spawned& s = spawned_[i];
        bool alive = world.is_alive(s.entity), enabled = true;
        for (flecs::entity at(world, s.entity); alive && at;) {
            enabled &= at.enabled();
            const Parent* p = at.try_get<Parent>();
            if (!p || !p->entity)
                break;
            if (!world.is_alive(p->entity)) {  // (an entity above it went: a Temporary effect's root)
                flecs::entity(world, s.entity).destruct();
                alive = false;
                break;
            }
            at = flecs::entity(world, p->entity);
        }
        if (!alive) {
            release(pak, s.slot, rows);
            scene.forgetInstance(s.slot);
            s = spawned_.back();
            spawned_.pop_back();
            continue;
        }
        const bool hidden = s.ownHidden || !enabled;
        if (hidden != s.hidden) {
            s.hidden = hidden;
            fpak::InstanceGpu& row = pak.mutableInstances()[s.slot];
            if (const WorldTransform* w = flecs::entity(world, s.entity).try_get<WorldTransform>())
                std::memcpy(row.world, w->rows, sizeof(row.world));
            row.flags = (row.flags & ~uint32_t(fpak::kInstanceHidden)) | (hidden ? fpak::kInstanceHidden : 0u);
            rows.push_back({s.slot, row});
        }
        ++i;
    }
    stats_.spawned = uint32_t(spawned_.size());

    // Particles' models.
    materialIds_.clear();
    for (const auto& [id, fx] : particles.effects()) {
        if (!fx.effect)
            continue;
        for (const ParticleSystem::Effect::Part& part : fx.parts) {
            if (part.component != VfxIndexOf<ParticleModelRenderer>::value)
                continue;
            const auto& r = *static_cast<const ParticleModelRenderer*>(part.data);
            const int32_t mesh = meshOf(pak, r.mesh);
            if (mesh < 0 || !has(uint32_t(mesh)))
                continue;
            int32_t material = -1;
            if (r.material) {
                const auto [it, added] = materialIds_.try_emplace(r.material, -1);
                if (added)
                    it->second = materialOf(vfxAssetPath(r.material));
                material = it->second;
            }
            float offset[3][3];
            eulerRows(r.rotationOffset, offset);
            uint32_t n = 0;
            for (const Particle& p : fx.particles) {
                if (p.flags & (kParticleDelayed | kParticleDead))
                    continue;
                if (n >= r.maxModels)
                    break;
                const uint64_t key = uint64_t(part.entity) * 0x9E3779B97F4A7C15ull ^ p.seed;
                auto it = held_.find(key);
                if (it != held_.end() && (it->second.mesh != uint32_t(mesh) || it->second.frame == frame_)) {
                    if (it->second.frame == frame_)  // (two particles with one key: the second goes without)
                        continue;
                    release(pak, it->second.slot, rows);
                    held_.erase(it);
                    it = held_.end();
                }
                if (it == held_.end()) {
                    const uint32_t slot = acquire(uint32_t(mesh));
                    if (slot == ~0u) {
                        ++stats_.starved;
                        continue;
                    }
                    it = held_.emplace(key, Held{slot, uint32_t(mesh), frame_}).first;
                }
                it->second.frame = frame_;
                ++n;
                // Its rotation: the offset, the particle's angles, then turned along its velocity.
                float spin[3][3], rot[3][3];
                eulerRows(p.angles, spin);
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        rot[a][b] = spin[a][0] * offset[0][b] + spin[a][1] * offset[1][b] + spin[a][2] * offset[2][b];
                const float3 v = {p.velocity[0], p.velocity[1], p.velocity[2]};
                if ((r.flags & kModelFaceVelocity) && length(v) > 1e-3f) {
                    const float3 z = normalize(v);
                    const float3 up = std::fabs(z.y) > 0.99f ? float3{1, 0, 0} : float3{0, 1, 0};
                    const float3 x = normalize(cross(up, z)), y = cross(z, x);
                    const float basis[3][3] = {{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}};
                    float turned[3][3];
                    for (int a = 0; a < 3; ++a)
                        for (int b = 0; b < 3; ++b)
                            turned[a][b] = basis[a][0] * rot[0][b] + basis[a][1] * rot[1][b] + basis[a][2] * rot[2][b];
                    std::memcpy(rot, turned, sizeof(rot));
                }
                const float scale[3] = {p.size[0] * r.scale, p.size[1] * r.scale, p.size[0] * r.scale};
                fpak::InstanceGpu& row = pak.mutableInstances()[it->second.slot];
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b)
                        row.world[a][b] = rot[a][b] * scale[b];
                    row.world[a][3] = p.position[a];
                }
                const bool tiny = std::max(scale[0], scale[1]) < 1e-5f;
                row.flags = (tiny ? fpak::kInstanceHidden : 0u) | ((r.flags & kModelShadows) ? 0u : fpak::kInstanceNoShadow) |
                            (material >= 0 ? (uint32_t(material) + 1) << fpak::kInstanceMaterialShift : 0u);
                row.tint = (r.flags & kModelParticleColor)
                               ? uint32_t(unorm8(p.color[0])) | uint32_t(unorm8(p.color[1])) << 8 | uint32_t(unorm8(p.color[2])) << 16 | 0xff000000u
                               : 0u;
                rows.push_back({it->second.slot, row});
            }
            stats_.models += n;
        }
    }
    // The models of particles gone this frame (or of renderers gone, or over their most).
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->second.frame == frame_) {
            ++it;
            continue;
        }
        release(pak, it->second.slot, rows);
        it = held_.erase(it);
    }
}

}  // namespace fx
