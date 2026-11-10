#include "app/preview.h"

#include "assets/fpak_format.h"
#include "assets/scene_pak.h"
#include "core/json.h"
#include "core/log.h"
#include "editor/material_library.h"
#include "editor/prefab.h"
#include "scene/scene.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fx {

namespace {

constexpr uint32_t kMinFramesPerJob = 12;  // VT feedback and shadow pages arrive a few frames late
constexpr uint32_t kIdleFrames = 8;        // then streaming and shadows idle this long
constexpr uint32_t kMaxFramesPerJob = 600;
constexpr uint32_t kGroundTint = 0xff5e5e5eu;  // RGBA8 (r in the low byte): a mid grey

}  // namespace

bool PreviewSession::load(const char* path)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path, &size);
    if (!text) {
        FX_ERROR("preview: cannot read %s", path);
        return false;
    }
    try {
        const Json root = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        scene_ = root.getString("scene");
        root_ = root.getString("root");
        edits_ = root.getBool("edits");
        terrain_ = root.getBool("terrain");
        if (const Json* list = root.get("jobs")) {
            for (const Json& j : list->array) {
                PreviewJob job;
                job.kind = j.getString("kind", "mesh");
                job.mesh = j.getString("mesh");
                job.model = j.getString("model");
                job.material = j.getString("material");
                job.prefab = j.getString("prefab");
                job.out = j.getString("out");
                job.size = uint32_t(std::clamp(j.getNumber("size", 256), 16.0, double(kRenderSize)));
                if (const Json* p = j.get("pose"); p && p->isObject()) {
                    const Json* pos = p->get("position");
                    if (pos && pos->array.size() == 3) {
                        job.hasPose = true;
                        job.pose.position = {float(pos->array[0].number), float(pos->array[1].number), float(pos->array[2].number)};
                        job.pose.yaw = float(p->getNumber("yaw"));
                        job.pose.pitch = float(p->getNumber("pitch"));
                        job.pose.pivotDistance = float(p->getNumber("pivotDistance", 10.0));
                        job.pose.speed = float(p->getNumber("speed", 5.0));
                    }
                }
                if (job.out.empty() || (job.kind != "mesh" && job.kind != "model" && job.kind != "material" && job.kind != "prefab" && job.kind != "scene")) {
                    FX_WARN("preview: skipping a job without an output or of an unknown kind (%s)", job.kind.c_str());
                    continue;
                }
                jobs_.push_back(std::move(job));
            }
        }
    } catch (const std::exception& e) {
        FX_ERROR("preview: %s: %s", path, e.what());
        return false;
    }
    if (scene_.empty()) {
        FX_ERROR("preview: %s names no scene", path);
        return false;
    }
    FX_INFO("preview: %zu jobs on %s", jobs_.size(), scene_.c_str());
    return true;
}

std::vector<PreviewSession::Part> PreviewSession::partsOf(const ScenePak& pak, const PreviewJob& job) const
{
    std::vector<Part> parts;
    if (job.kind == "mesh") {
        const int32_t mesh = pak.findMesh(job.mesh);
        if (mesh >= 0)
            parts.push_back({uint32_t(mesh), {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}});
    } else if (job.kind == "prefab") {
        return job.parts;
    } else if (job.kind == "material") {  // a sphere (displaced by it, when the scene cooked that)
        const int32_t displaced = pak.findMesh("sphere~" + job.material);
        parts.push_back({displaced >= 0 ? uint32_t(displaced) : pak.header().builtinMeshes[uint32_t(fpak::Builtin::Sphere)],
                         {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}});
    } else if (job.kind == "model") {
        if (const ScenePak::Library* lib = pak.library(job.model))
            for (const ScenePak::LibraryNode& n : lib->nodes) {
                Part part;
                part.mesh = n.mesh;
                std::memcpy(part.world, n.world, sizeof(part.world));
                parts.push_back(part);
            }
    }
    return parts;
}

uint32_t PreviewSession::spareMaterials() const
{
    uint32_t n = 0;
    for (const PreviewJob& j : jobs_) {
        if (j.kind == "material")
            ++n;
        Prefab def;
        std::string error;
        if (j.kind == "prefab" && readPrefab(root_ + "/" + j.prefab, def, error))
            for (const PrefabEntity& e : def.entities)
                n += e.material.empty() ? 0u : 1u;
    }
    return n;
}

void PreviewSession::reserve(ScenePak& pak, MaterialLibrary& materials)
{
    for (PreviewJob& job : jobs_) {
        if (job.kind == "material" && (job.materialIndex = materials.indexOf(job.material)) < 0)
            FX_WARN("preview: material %s could not be loaded", job.material.c_str());
        if (job.kind != "prefab")
            continue;
        // A prefab's meshes, relative to its root, with their materials.
        Prefab def;
        std::string error;
        if (!readPrefab(root_ + "/" + job.prefab, def, error)) {
            FX_WARN("preview: %s", error.c_str());
            continue;
        }
        const std::vector<std::array<float, 12>> rel = def.rootRelative();
        for (size_t k = 0; k < def.entities.size(); ++k) {
            const PrefabEntity& e = def.entities[k];
            const int32_t displaced = e.kind == "mesh" && !e.material.empty() ? pak.findMesh(e.mesh + "~" + e.material) : -1;
            const int32_t mesh = displaced >= 0 ? displaced : e.kind == "mesh" ? pak.findMesh(e.mesh) : -1;
            if (mesh < 0 || e.hidden)
                continue;
            Part part;
            part.mesh = uint32_t(mesh);
            std::memcpy(part.world, rel[k].data(), sizeof(part.world));
            part.material = e.material.empty() ? -1 : materials.indexOf(e.material);
            job.parts.push_back(part);
        }
    }
    // As many slots per mesh as the most any one job shows.
    std::vector<std::pair<uint32_t, uint32_t>> need;  // mesh, count
    for (const PreviewJob& job : jobs_) {
        const std::vector<Part> parts = partsOf(pak, job);
        if (parts.empty() && job.kind != "scene")
            FX_WARN("preview: no %s %s in %s", job.kind.c_str(), (job.kind == "mesh" ? job.mesh : job.model).c_str(), scene_.c_str());
        std::vector<std::pair<uint32_t, uint32_t>> counts;
        for (const Part& p : parts) {
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == p.mesh; });
            if (it == counts.end())
                counts.push_back({p.mesh, 1});
            else
                ++it->second;
        }
        for (const auto& [mesh, count] : counts) {
            auto it = std::find_if(need.begin(), need.end(), [&](const auto& n) { return n.first == mesh; });
            if (it == need.end())
                need.push_back({mesh, count});
            else
                it->second = std::max(it->second, count);
        }
    }
    for (const auto& [mesh, count] : need) {
        const uint32_t first = pak.addInstancePool(mesh, count);
        std::vector<uint32_t> slots;
        for (uint32_t k = 0; k < count; ++k)
            slots.push_back(first + k);
        slots_.push_back({mesh, std::move(slots)});
    }
    if (!slots_.empty())
        groundSlot_ = pak.addInstancePool(pak.header().builtinMeshes[uint32_t(fpak::Builtin::Plane)], 1);
}

void PreviewSession::hide(const ScenePak& pak, std::vector<InstanceUpdate>& rows)
{
    for (const uint32_t slot : shown_) {
        fpak::InstanceGpu row = pak.instances()[slot];
        row.flags |= fpak::kInstanceHidden;
        rows.push_back({slot, row});
    }
    shown_.clear();
}

bool PreviewSession::place(const ScenePak& pak, FlyCamera& camera, std::vector<InstanceUpdate>& rows)
{
    hide(pak, rows);
    const PreviewJob& job = jobs_[size_t(current_)];
    if (job.kind == "scene") {
        camera.setPose(job.hasPose ? job.pose : startPose_);
        return true;
    }
    const std::vector<Part> parts = partsOf(pak, job);
    if (parts.empty())
        return false;
    // The bounds of every part (model space).
    uint32_t meshCount = 0;
    const fpak::MeshGpu* meshes = pak.view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Part& part : parts) {
        if (part.mesh >= meshCount)
            continue;
        const fpak::MeshGpu& m = meshes[part.mesh];
        for (int corner = 0; corner < 8; ++corner) {
            float p[3];
            for (int a = 0; a < 3; ++a)
                p[a] = m.posOffset[a] + ((corner >> a) & 1 ? m.posScale[a] * 65535.0f : 0.0f);
            for (int r = 0; r < 3; ++r) {
                const float v = part.world[r][0] * p[0] + part.world[r][1] * p[1] + part.world[r][2] * p[2] + part.world[r][3];
                lo[r] = std::min(lo[r], v);
                hi[r] = std::max(hi[r], v);
            }
        }
    }
    if (lo[0] > hi[0])
        return false;
    // Standing on the ground at the origin: the bounds' bottom center there.
    const float shift[3] = {-(lo[0] + hi[0]) * 0.5f, -lo[1], -(lo[2] + hi[2]) * 0.5f};
    std::vector<std::pair<uint32_t, uint32_t>> used;  // mesh, slots taken this job
    for (const Part& part : parts) {
        auto slots = std::find_if(slots_.begin(), slots_.end(), [&](const auto& s) { return s.first == part.mesh; });
        if (slots == slots_.end())
            continue;
        auto u = std::find_if(used.begin(), used.end(), [&](const auto& x) { return x.first == part.mesh; });
        if (u == used.end())
            u = used.insert(used.end(), {part.mesh, 0u});
        if (u->second >= slots->second.size())
            continue;
        const uint32_t slot = slots->second[u->second++];
        fpak::InstanceGpu row = pak.instances()[slot];
        std::memcpy(row.world, part.world, sizeof(row.world));
        for (int r = 0; r < 3; ++r)
            row.world[r][3] += shift[r];
        row.flags &= ~uint32_t(fpak::kInstanceHidden);
        const int32_t material = part.material >= 0 ? part.material : job.materialIndex;
        row.flags = (row.flags & 0xffffu) | (material >= 0 ? (uint32_t(material) + 1) << fpak::kInstanceMaterialShift : 0u);
        rows.push_back({slot, row});
        shown_.push_back(slot);
    }
    const float3 half = {std::max((hi[0] - lo[0]) * 0.5f, 1e-3f), std::max((hi[1] - lo[1]) * 0.5f, 1e-3f),
                         std::max((hi[2] - lo[2]) * 0.5f, 1e-3f)};
    const float radius = length(half);
    if (groundSlot_ != ~0u) {  // the built-in plane is 10 x 10: wide enough to reach the horizon
        fpak::InstanceGpu ground = pak.instances()[groundSlot_];
        const float s = radius * 4.0f;
        std::memset(ground.world, 0, sizeof(ground.world));
        ground.world[0][0] = ground.world[1][1] = ground.world[2][2] = s;
        ground.flags &= ~uint32_t(fpak::kInstanceHidden);
        ground.tint = kGroundTint;
        rows.push_back({groundSlot_, ground});
        shown_.push_back(groundSlot_);
    }
    // From the front-right, a little above; the bounding sphere fills the 60 degree view.
    const float3 target = {0.0f, half.y, 0.0f};
    const float3 dir = normalize(float3{-0.62f, -0.42f, -0.66f});  // where the camera looks
    const float distance = radius / std::sin(30.0f * 3.14159265f / 180.0f) * 1.08f;
    FlyCamera::Pose pose;
    pose.position = target - dir * distance;
    pose.yaw = std::atan2(-dir.x, -dir.z);
    pose.pitch = std::asin(dir.y);
    pose.pivotDistance = distance;
    pose.speed = 5.0f;
    camera.setPose(pose);
    return true;
}

void PreviewSession::update(const ScenePak& pak, FlyCamera& camera, std::vector<InstanceUpdate>& rows, uint32_t idleFrames)
{
    if (!hasStartPose_) {
        startPose_ = camera.pose();
        hasStartPose_ = true;
    }
    if (current_ < 0) {
        if (next_ >= jobs_.size())
            return;
        current_ = int(next_++);
        framesPlaced_ = 0;
        capture_ = false;
        if (!place(pak, camera, rows)) {  // (its mesh is not in the scene: logged at reserve())
            ++failures_;
            current_ = -1;
        }
        return;
    }
    ++framesPlaced_;
    capture_ = (framesPlaced_ >= kMinFramesPerJob && idleFrames >= kIdleFrames) || framesPlaced_ >= kMaxFramesPerJob;
}

std::string PreviewSession::captureTarget() const
{
    return capture_ && current_ >= 0 ? jobs_[size_t(current_)].out + ".tmp" : std::string();
}

uint32_t PreviewSession::captureSize() const { return current_ >= 0 ? jobs_[size_t(current_)].size : 0; }

void PreviewSession::captured(bool ok)
{
    if (current_ < 0)
        return;
    const PreviewJob& job = jobs_[size_t(current_)];
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(job.out + ".tmp", job.out, ec);
        ok = !ec;
    }
    if (!ok) {
        ++failures_;
        FX_ERROR("preview: could not write %s", job.out.c_str());
    } else {
        FX_INFO("preview: %s %s -> %s", job.kind.c_str(),
                job.kind == "mesh" ? job.mesh.c_str() : job.kind == "model" ? job.model.c_str() : job.kind == "material" ? job.material.c_str() : scene_.c_str(),
                job.out.c_str());
    }
    current_ = -1;
    capture_ = false;
}

}  // namespace fx
