// The navigation mesh in the editor (level/navmesh.h): baked from what the scene has to walk on, its
// level meshes as they are drawn (modifiers and cuts applied; cut volumes themselves not) and its
// meshes (each read back from the pak once), for the project's agent; saved beside the scene
// ("<scene>.nav", what games load) and the scene's navigation from then on. Once a scene has one,
// it is baked again half a second after the level stops changing (the project's navAutoBake).

#include "editor/editor_bridge.h"

#include "core/log.h"
#include "editor/scene_view.h"
#include "editor/terrain_editor.h"
#include "level/level_meshes.h"
#include "level/navmesh.h"
#include "level/pak_mesh.h"
#include "terrain/terrain.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unordered_map>

namespace fx {

bool EditorBridge::navBake(std::string& error)
{
    if (!scene_ || scenePath_.empty()) {
        error = "No scene to bake a navigation mesh for";
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    level::NavInput in;
    const auto add = [&](const level::PolyMesh& mesh, const float w[3][4]) {
        const uint32_t base = uint32_t(in.vertices.size());
        for (const float3& p : mesh.positions)
            in.vertices.push_back({w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                                   w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]});
        for (const level::Face& f : mesh.faces)
            for (uint32_t k = 1; k + 1 < f.count; ++k)
                in.indices.insert(in.indices.end(), {base + mesh.corners[f.first], base + mesh.corners[f.first + k], base + mesh.corners[f.first + k + 1]});
    };
    std::unordered_map<uint32_t, std::shared_ptr<level::PolyMesh>> pakMeshes;  // (each mesh read back once)
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const flecs::entity e = entityOf(i);
        float w[3][4];
        if (!e || !capture(i).alive || hiddenBelow(i) || e.has<CutVolume>() || !worldOf(i, w))
            continue;
        if (const LevelVolume* vol = e.try_get<LevelVolume>()) {  // (a volume: not walked on; nav ones marked)
            const auto mesh = levelMeshOf(i);
            if (!mesh || mesh->empty() || (vol->kind != VolumeKind::NavBlocker && vol->kind != VolumeKind::NavArea))
                continue;
            float3 lo, hi;
            mesh->bounds(lo, hi);
            const auto at = [&](float x, float y, float z) {
                return float3{w[0][0] * x + w[0][1] * y + w[0][2] * z + w[0][3], w[1][0] * x + w[1][1] * y + w[1][2] * z + w[1][3],
                              w[2][0] * x + w[2][1] * y + w[2][2] * z + w[2][3]};
            };
            level::NavVolume nv;
            nv.blocks = vol->kind == VolumeKind::NavBlocker;
            nv.cost = vol->cost;
            // Its bounds' footprint (counter-clockwise from above), between its lowest and highest corner.
            nv.outline = {at(lo.x, lo.y, lo.z), at(lo.x, lo.y, hi.z), at(hi.x, lo.y, hi.z), at(hi.x, lo.y, lo.z)};
            const float3 n = cross(nv.outline[1] - nv.outline[0], nv.outline[2] - nv.outline[0]);
            if (n.y < 0.0f)
                std::reverse(nv.outline.begin(), nv.outline.end());
            nv.bottom = 1e30f;
            nv.top = -1e30f;
            for (int k = 0; k < 8; ++k) {
                const float3 p = at(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z);
                nv.bottom = std::min(nv.bottom, p.y);
                nv.top = std::max(nv.top, p.y);
            }
            in.volumes.push_back(std::move(nv));
            continue;
        }
        if (e.has<LevelMesh>()) {
            std::shared_ptr<const level::PolyMesh> mesh = levelMeshes_ ? levelMeshes_->drawnOf(e.id()) : nullptr;
            if (!mesh)
                mesh = levelMeshOf(i);
            if (mesh)
                add(*mesh, w);
        } else if (records_[i].slot != ~0u && (records_[i].kind == EntityKind::Mesh || records_[i].kind == EntityKind::Primitive)) {
            const uint32_t index = pak_->instances()[records_[i].slot].mesh;
            auto [it, fresh] = pakMeshes.try_emplace(index);
            if (fresh) {
                auto mesh = std::make_shared<level::PolyMesh>();
                if (level::meshFromPak(*pak_, index, *mesh))
                    it->second = std::move(mesh);
            }
            if (it->second)
                add(*it->second, w);
        }
    }
    // The terrain: its triangles within 20 m of the rest (all of it when there is nothing else), at
    // most 512 quads a side (coarser beyond that: a big terrain's far reaches), its holes left out.
    if (Terrain* terrain = terrain_ ? terrain_->terrain() : nullptr; terrain && terrain->valid()) {
        float lo[3], hi[3];
        terrain->bounds(lo, hi);
        if (!in.vertices.empty()) {
            float3 a = in.vertices[0], b = a;
            for (const float3& p : in.vertices) {
                a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
                b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
            }
            lo[0] = std::max(lo[0], a.x - 20.0f), hi[0] = std::min(hi[0], b.x + 20.0f);
            lo[2] = std::max(lo[2], a.z - 20.0f), hi[2] = std::min(hi[2], b.z + 20.0f);
        }
        const TerrainSettings& ts = terrain->settings();
        const int32_t last = int32_t(terrain->samplesPerSide()) - 1;
        const auto column = [&](float v, float origin, bool up) {
            const float g = (v - origin) / ts.spacing;
            return std::clamp(int32_t(up ? std::ceil(g) : std::floor(g)), 0, last);
        };
        const int32_t x0 = column(lo[0], ts.originX, false), x1 = column(hi[0], ts.originX, true);
        const int32_t z0 = column(lo[2], ts.originZ, false), z1 = column(hi[2], ts.originZ, true);
        if (x1 > x0 && z1 > z0) {
            const int32_t step = std::max(1, int32_t(std::ceil(float(std::max(x1 - x0, z1 - z0)) / 512.0f)));
            const int32_t nx = (x1 - x0 + step - 1) / step + 1, nz = (z1 - z0 + step - 1) / step + 1;
            const uint32_t base = uint32_t(in.vertices.size());
            for (int32_t j = 0; j < nz; ++j)
                for (int32_t i = 0; i < nx; ++i)
                    in.vertices.push_back(terrain->samplePosition(std::min(x0 + i * step, x1), std::min(z0 + j * step, z1)));
            for (int32_t j = 0; j + 1 < nz; ++j)
                for (int32_t i = 0; i + 1 < nx; ++i) {
                    if (terrain->material(std::min(x0 + i * step, x1), std::min(z0 + j * step, z1)) & terrain::kHole)
                        continue;
                    const uint32_t a = base + uint32_t(j * nx + i), b = a + 1, d = a + uint32_t(nx), c = d + 1;
                    in.indices.insert(in.indices.end(), {a, d, b, b, d, c});  // (facing up)
                }
        }
    }
    auto nav = std::make_shared<level::NavMesh>();
    if (!nav->build(in, project_.navigation, &error)) {
        error = "Navigation mesh not baked: " + error;
        return false;
    }
    const std::vector<uint8_t> blob = nav->save();
    const std::string path = level::navMeshPathFor(scenePath_);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()))) {
        error = "Can't write " + path;
        return false;
    }
    nav->edges(navLines_);
    FX_INFO("navigation: baked %u polygons in %u tiles from %zu triangles, %.0f ms", nav->polygonCount(), nav->tileCount(), in.indices.size() / 3,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    level::setNavigation(std::move(nav));
    navStale_ = false;
    navCommands_ = sceneChanges_;
    return true;
}

void EditorBridge::navClear()
{
    level::setNavigation(nullptr);
    navLines_.clear();
    navStale_ = false;
    if (!scenePath_.empty())
        std::remove(level::navMeshPathFor(scenePath_).c_str());
}

void EditorBridge::navUpdate()
{
    // The loaded scene's (its sidecar, read as it started): its lines for the overlay.
    const auto nav = level::navigation();
    if (nav && navLines_.empty())
        nav->edges(navLines_);
    if (!nav) {
        navLines_.clear();
        return;
    }
    // Changed (an edit, undo or redo since the bake): baked again once it has been still for half a second.
    const uint64_t now = SDL_GetTicksNS();
    if (sceneChanges_ != navCommands_) {
        navCommands_ = sceneChanges_;
        navChangedNs_ = now;
        navStale_ = true;
    }
    if (navStale_ && project_.navAutoBake && !playing_ && now - navChangedNs_ > 500'000'000ull) {
        std::string error;
        if (!navBake(error)) {
            navStale_ = false;  // (not again until the next change)
            FX_WARN("%s", error.c_str());
        }
        sendState(false);
    }
}

}  // namespace fx
