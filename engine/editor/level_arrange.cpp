// Placing and arranging (the level tools): a prefab's instance spawned (a drop, the prefab shelf,
// Replace, the scatter brush), and the selection arranged: aligned to the primary one's bounds,
// spread evenly, dropped onto what is under it, grouped under a new empty or ungrouped, replaced
// with a prefab. Each one undo step. An entity's bounds here are its subtree's meshes' (world), or
// its origin.

#include "editor/editor_bridge.h"

#include "editor/prefab.h"
#include "editor/scene_view.h"
#include "level/level_meshes.h"
#include "scene/fly_camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fx {

namespace {

float axisOf(float3 p, uint32_t a) { return a == 0 ? p.x : a == 1 ? p.y : p.z; }

void affine(const float a[3][4], const float b[3][4], float out[3][4])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
}

}  // namespace

LevelParts EditorBridge::levelPartsOfState(const State& s)
{
    LevelParts p;
    p.cut = s.cut;
    p.wall = s.wall;
    p.opening = s.opening;
    if (s.modifiers)
        p.modifiers = s.modifiers->stack;
    p.spline = s.spline;
    if (s.wear)
        p.levelWear = s.wear->rules;
    p.scatterSet = s.scatter;
    p.snapPoint = s.snap;
    p.volume = s.volume;
    p.roof = s.roof;
    return p;
}

void EditorBridge::setLevelPartsOfState(State& s, const LevelParts& p)
{
    s.cut = p.cut;
    s.wall = p.wall;
    s.roof = p.roof;
    s.opening = p.opening;
    s.modifiers.reset();
    if (!p.modifiers.empty())
        s.modifiers = LevelModifiers{p.modifiers};
    s.spline = p.spline;
    s.wear.reset();
    if (!p.levelWear.empty())
        s.wear = LevelWear{p.levelWear};
    s.scatter = p.scatterSet;
    s.snap = p.snapPoint;
    s.volume = p.volume;
}

uint32_t EditorBridge::spawnPrefab(const std::string& path, const float place[3][4], bool standOn, uint32_t parent,
                                   std::vector<std::pair<uint32_t, State>>& after, std::string& error)
{
    const Prefab* def = prefabDef(path);
    if (!def) {
        error = "Cannot read the prefab " + path;
        return ~0u;
    }
    // Standing on `place` by its meshes' bounds (relative to its root): their bottom centre there.
    const std::vector<std::array<float, 12>> rel = def->rootRelative();
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t k = 0; k < def->entities.size() && standOn; ++k) {
        float3 c, h;
        const int32_t mesh = def->entities[k].kind == "mesh" ? pak_->findMesh(def->entities[k].mesh) : -1;
        if (const auto& lm = def->entities[k].levelMesh; lm && !lm->empty()) {
            float3 llo, lhi;
            lm->bounds(llo, lhi);
            c = (llo + lhi) * 0.5f;
            h = (lhi - llo) * 0.5f;
        } else if (mesh < 0 || !meshBoundsOf(uint32_t(mesh), c, h)) {
            continue;
        }
        for (int corner = 0; corner < 8; ++corner) {
            const float p[3] = {corner & 1 ? c.x + h.x : c.x - h.x, corner & 2 ? c.y + h.y : c.y - h.y, corner & 4 ? c.z + h.z : c.z - h.z};
            for (int r = 0; r < 3; ++r) {
                const float* row = rel[k].data() + r * 4;
                const float v = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
                lo[r] = std::min(lo[r], v);
                hi[r] = std::max(hi[r], v);
            }
        }
    }
    if (lo[0] > hi[0])
        lo[0] = hi[0] = lo[1] = hi[1] = lo[2] = hi[2] = 0.0f;
    const float offset[3][4] = {{1, 0, 0, -(lo[0] + hi[0]) * 0.5f}, {0, 1, 0, -lo[1]}, {0, 0, 1, -(lo[2] + hi[2]) * 0.5f}};
    float rootWorld[3][4];
    affine(place, offset, rootWorld);
    // Unity's naming for another one: "Crate (1)", "Crate (2)", ...
    std::string name = def->name;
    uint32_t n = 0;
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (const std::string v = defaultName(i); entityOf(i) && capture(i).alive && (v == def->name || v.starts_with(def->name + " (")))
            n = std::max(n, v == def->name ? 1u : uint32_t(std::atoi(v.c_str() + def->name.size() + 2)) + 1);
    for (const auto& [index, s] : after)  // (and the ones made with it, not alive yet)
        if (s.name == def->name || s.name.starts_with(def->name + " ("))
            n = std::max(n, s.name == def->name ? 1u : uint32_t(std::atoi(s.name.c_str() + def->name.size() + 2)) + 1);
    if (n)
        name += " (" + std::to_string(n) + ")";
    std::unordered_map<std::string, uint32_t> made;
    uint32_t root = ~0u;
    for (size_t k = 0; k < def->entities.size(); ++k) {
        const PrefabEntity& p = def->entities[k];
        const EntityKind named = entityKindFromName(p.kind);  // (unknown kinds: empties)
        const EntityKind kind = p.kind == "mesh" ? EntityKind::Primitive : named == EntityKind::Primitive ? EntityKind::Empty : named;
        float world[3][4], relative[3][4];
        std::memcpy(relative, rel[k].data(), sizeof(relative));
        affine(rootWorld, relative, world);
        const auto up = made.find(p.parent);
        const std::string primitive = kind == EntityKind::Light ? (p.light && p.light->type == 2 ? "directional" : p.light && p.light->type == 1 ? "spot" : "point")
                                      : kind == EntityKind::Level ? "box" : p.mesh;
        const uint32_t created = spawn(kind, primitive, world, k == 0 ? name : p.name, p.material);
        if (created == ~0u)
            break;
        State s;
        std::memcpy(s.world, world, sizeof(s.world));
        s.parent = k == 0 ? parent : up != made.end() ? up->second : ~0u;
        s.alive = true;
        s.name = k == 0 ? name : defaultName(created);
        s.material = p.material;
        s.hidden = p.hidden;
        if (p.light)
            s.light = *p.light;
        if (p.camera)
            s.camera = *p.camera;
        s.body = p.body;
        s.collider = p.collider;
        s.audioSource = p.audioSource;
        s.audioListener = p.audioListener;
        s.acoustics = p.acoustics;
        s.vfx = p.vfx;
        s.scripts = p.scripts;
        s.levelMesh = p.levelMesh;
        setLevelPartsOfState(s, p.level);
        after.emplace_back(created, std::move(s));
        records_[created].prefabRoot = made.empty() ? created : made.at(def->entities[0].id);
        records_[created].prefabChild = p.id;
        if (k == 0) {
            records_[created].prefab = path;
            root = created;
        }
        made[p.id] = created;
    }
    if (root == ~0u)
        error = "No room for " + path;
    return root;
}

void EditorBridge::measuredTo(float3 a, float3 b)
{
    const float3 d = b - a;
    measuredLength_ = length(d);
    char text[160];
    std::snprintf(text, sizeof(text), "%.3f m   (x %.3f, y %.3f, z %.3f)   drag between two points · V: onto vertices · Esc: done", measuredLength_,
                  std::fabs(d.x), std::fabs(d.y), std::fabs(d.z));
    viewportHint(text);
}

bool EditorBridge::worldBounds(uint32_t root, float3& lo, float3& hi) const
{
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (const uint32_t k : subtree(root)) {
        float3 c, h;
        float w[3][4];
        if (!entityOf(k) || !capture(k).alive || !worldOf(k, w))
            continue;
        if (const auto lm = levelMeshOf(k); lm && !lm->empty()) {  // (a level mesh: its own bounds; its live mesh has none packed)
            float3 llo, lhi;
            lm->bounds(llo, lhi);
            c = (llo + lhi) * 0.5f;
            h = (lhi - llo) * 0.5f;
        } else if (!meshBounds(k, c, h)) {
            continue;
        }
        for (int corner = 0; corner < 8; ++corner) {
            const float3 p = {corner & 1 ? c.x + h.x : c.x - h.x, corner & 2 ? c.y + h.y : c.y - h.y, corner & 4 ? c.z + h.z : c.z - h.z};
            const float3 q = {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                              w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
    }
    if (lo.x <= hi.x)
        return true;
    float w[3][4];
    if (!worldOf(root, w))
        return false;
    lo = hi = {w[0][3], w[1][3], w[2][3]};
    return true;
}

void EditorBridge::arrange(const std::string& op, uint32_t axis, int32_t edge)
{
    // The roots that can move, each moved by a world offset (live), then one undo step for all.
    std::vector<uint32_t> roots;
    for (const uint32_t r : selectionRoots())
        if (editable(r))
            roots.push_back(r);
    axis = std::min(axis, 2u);
    std::vector<std::pair<uint32_t, std::array<float, 12>>> before;
    const auto move = [&](uint32_t r, float3 by) {
        float w[3][4];
        if (!worldOf(r, w) || (by.x == 0.0f && by.y == 0.0f && by.z == 0.0f))
            return;
        std::array<float, 12> was;
        std::memcpy(was.data(), w, sizeof(w));
        before.push_back({r, was});
        w[0][3] += by.x, w[1][3] += by.y, w[2][3] += by.z;
        setWorldLive(r, w);
    };
    const auto along = [&](float d) { return float3{axis == 0 ? d : 0.0f, axis == 1 ? d : 0.0f, axis == 2 ? d : 0.0f}; };
    const auto edgeOf = [&](float3 lo, float3 hi) {
        return edge < 0 ? axisOf(lo, axis) : edge > 0 ? axisOf(hi, axis) : (axisOf(lo, axis) + axisOf(hi, axis)) * 0.5f;
    };
    if (op == "align") {
        // To the primary one (the last selected): its bounds' min, centre or max on the axis.
        float3 lo, hi;
        if (roots.size() < 2 || selected_ == ~0u || !worldBounds(selected_, lo, hi))
            return notice("Align: select two or more (the last one selected stays)", "warning");
        const float target = edgeOf(lo, hi);
        for (const uint32_t r : roots)
            if (r != selected_ && worldBounds(r, lo, hi))
                move(r, along(target - edgeOf(lo, hi)));
    } else if (op == "distribute") {
        // Their centres spread evenly between the first and the last along the axis.
        if (roots.size() < 3)
            return notice("Distribute: select three or more", "warning");
        std::vector<std::pair<float, uint32_t>> order;
        for (const uint32_t r : roots)
            if (float3 lo, hi; worldBounds(r, lo, hi))
                order.push_back({(axisOf(lo, axis) + axisOf(hi, axis)) * 0.5f, r});
        std::sort(order.begin(), order.end());
        const float first = order.front().first, step = (order.back().first - first) / float(order.size() - 1);
        for (size_t k = 1; k + 1 < order.size(); ++k)
            move(order[k].second, along(first + step * float(k) - order[k].first));
    } else if (op == "snap") {
        // Each onto the snap point nearest one of its own (the drag's way, where it is now).
        for (const uint32_t r : roots) {
            float w[3][4], was[3][4];
            if (!worldOf(r, w))
                continue;
            std::memcpy(was, w, sizeof(w));
            if (!snapPiece(r, w))
                continue;
            std::array<float, 12> keep;
            std::memcpy(keep.data(), was, sizeof(was));
            before.push_back({r, keep});
            setWorldLive(r, w);
        }
    } else if (op == "ground") {
        // Each dropped onto what is under its bounds' bottom centre (itself not).
        for (const uint32_t r : roots) {
            float3 lo, hi, point, normal;
            if (!worldBounds(r, lo, hi))
                continue;
            const float3 from = {(lo.x + hi.x) * 0.5f, lo.y + 0.05f, (lo.z + hi.z) * 0.5f};
            if (surfaceHit(from, {0, -1, 0}, subtree(r), point, normal))
                move(r, {0.0f, point.y - lo.y, 0.0f});
        }
    }
    commitWorlds(before);
    sendState(true);
}

void EditorBridge::group()
{
    // A new empty at the roots' bounds' bottom centre (under their parent when they share one),
    // they under it, keeping where they are: one undo step, the group selected.
    std::vector<uint32_t> roots;
    for (const uint32_t r : selectionRoots())
        if (editable(r) && !linkedChild(r))
            roots.push_back(r);
    if (roots.empty())
        return notice("Group: select what goes in it", "warning");
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    uint32_t parent = capture(roots[0]).parent;
    for (const uint32_t r : roots) {
        float3 a, b;
        if (worldBounds(r, a, b)) {
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
            hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
        }
        if (capture(r).parent != parent)
            parent = ~0u;
    }
    State g;
    const float world[3][4] = {{1, 0, 0, (lo.x + hi.x) * 0.5f}, {0, 1, 0, lo.y}, {0, 0, 1, (lo.z + hi.z) * 0.5f}};
    std::memcpy(g.world, world, sizeof(world));
    const uint32_t made = spawn(EntityKind::Empty, "", g.world, "Group");
    if (made == ~0u)
        return;
    g.parent = parent;
    g.alive = true;
    g.name = defaultName(made);
    std::vector<std::pair<uint32_t, State>> changes = {{made, g}};
    for (const uint32_t r : roots) {
        State s = capture(r);
        s.parent = made;  // (its world stays)
        s.hasLocal = false;
        changes.push_back({r, s});
    }
    execute(changes);
    select(made);
    sendState(true);
}

void EditorBridge::ungroup()
{
    // The selected entity's children under its parent instead, keeping where they are; it goes
    // when it is an empty with nothing else on it. One undo step, the children selected.
    const uint32_t g = selected_;
    if (g == ~0u || !editable(g) || linkedChild(g))
        return notice("Ungroup: select a group", "warning");
    const State gs = capture(g);
    std::vector<std::pair<uint32_t, State>> changes;
    std::vector<uint32_t> children;
    for (uint32_t k = 0; k < records_.size(); ++k)
        if (k != g && entityOf(k) && capture(k).alive && capture(k).parent == g && !linkedChild(k)) {
            State s = capture(k);
            s.parent = gs.parent;
            s.hasLocal = false;
            changes.push_back({k, s});
            children.push_back(k);
        }
    if (children.empty())
        return notice("Ungroup: it has nothing under it", "warning");
    const bool bare = records_[g].kind == EntityKind::Empty && !gs.light && !gs.camera && !gs.body && !gs.collider && !gs.audioSource &&
                      gs.scripts.empty() && gs.vfx.empty() && records_[g].prefab.empty();
    if (bare) {
        State gone = gs;
        gone.alive = false;
        changes.push_back({g, gone});
    }
    execute(changes);
    selectMany(children, SelectMode::Replace);
    sendState(true);
}

void EditorBridge::replaceWith(const std::string& prefab)
{
    // Each root an instance of the prefab where it was (its rotation and scale too), under its
    // parent; the old ones deleted. One undo step, the new ones selected.
    std::vector<uint32_t> roots;
    for (const uint32_t r : selectionRoots())
        if (editable(r) && !linkedChild(r))
            roots.push_back(r);
    if (roots.empty() || prefab.empty())
        return notice("Replace: select what to replace, and give a prefab", "warning");
    std::vector<uint32_t> made;
    undoBegin();
    for (const uint32_t r : roots) {
        float w[3][4];
        if (!worldOf(r, w))
            continue;
        std::vector<std::pair<uint32_t, State>> after;
        std::string error;
        const uint32_t root = spawnPrefab(prefab, w, false, capture(r).parent, after, error);
        if (root == ~0u) {
            notice(error.c_str(), "error");
            break;
        }
        execute(after);
        made.push_back(root);
        Json gone = Json::parse("{\"type\":\"editor.entity.delete\",\"id\":" + std::to_string(r + 1) + "}");
        handle(gone, CameraState{});  // (a delete needs no camera)
    }
    undoEnd();
    selectMany(made, SelectMode::Replace);
    sendState(true);
}

}  // namespace fx
