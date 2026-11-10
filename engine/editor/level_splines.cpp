// The spline tool (SceneTool::Spline): clicks on any surface add points to the selected spline (a
// new one, with the tool's profile, when none is selected), Ctrl+click puts one in between the two
// it is nearest, a point dragged slides over the surfaces under the mouse (live, one undo step),
// X takes the one under the mouse out, V makes it a corner (or smooth again), C closes or opens it. SceneView finds the points under the mouse; this
// changes the LevelSpline (LevelMeshes lofts it again).

#include "editor/editor_bridge.h"

#include "editor/scene_view.h"
#include "level/level_meshes.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

float3 rows(const float w[3][4], float3 p)
{
    return {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
            w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
}

bool invert(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20f)
        return false;
    const float s = 1.0f / det;
    const float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                           {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                           {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
    return true;
}

}  // namespace

bool EditorBridge::splineWorld(uint32_t index, std::vector<float3>* controls, std::vector<float3>* curve, bool* closed) const
{
    const flecs::entity e = index < records_.size() ? entityOf(index) : flecs::entity();
    const LevelSpline* s = e ? e.try_get<LevelSpline>() : nullptr;
    float w[3][4];
    if (!s || !worldOf(index, w))
        return false;
    if (controls) {
        controls->clear();
        for (const level::SplinePoint& p : s->spline.points)
            controls->push_back(rows(w, p.position));
    }
    if (curve) {
        curve->clear();
        for (const float3& p : level::splineCurve(s->spline, 0.25f))
            curve->push_back(rows(w, p));
    }
    if (closed)
        *closed = s->spline.closed;
    return true;
}

void EditorBridge::splineAdd(float3 origin, float3 dir, bool insert)
{
    // Where: on whatever is under the mouse but the spline's own loft.
    const uint32_t target = selected_ != ~0u && entityOf(selected_) && entityOf(selected_).has<LevelSpline>() ? selected_ : ~0u;
    float3 point, normal;
    std::vector<uint32_t> skip;
    if (target != ~0u)
        skip.push_back(target);
    if (!surfaceHit(origin, dir, skip, point, normal))
        return;
    if (target == ~0u) {  // a new spline there, with the tool's profile
        const std::string profile = view_ ? view_->tools().splineProfile : std::string();
        State s;
        const float world[3][4] = {{1, 0, 0, point.x}, {0, 1, 0, point.y}, {0, 0, 1, point.z}};
        std::memcpy(s.world, world, sizeof(world));
        const uint32_t created = spawn(EntityKind::Level, "box", s.world, profile.empty() ? "Spline" : profile == "kerb" ? "Kerb" :
                                       std::string(1, char(std::toupper(profile[0]))) + profile.substr(1));
        if (created == ~0u)
            return;
        s.alive = true;
        s.name = defaultName(created);
        LevelSpline spline;
        spline.spline.points.push_back({{0, 0, 0}});
        spline.loft = level::loftPreset(profile);
        s.spline = spline;
        s.levelMesh = std::make_shared<level::PolyMesh>();
        execute({{created, s}});
        select(created);
        sendState(true);
        return;
    }
    float w[3][4], inv[3][4];
    if (!worldOf(target, w) || !invert(w, inv))
        return;
    State s = capture(target);
    if (!s.spline)
        return;
    const float3 local = rows(inv, point);
    std::vector<level::SplinePoint>& pts = s.spline->spline.points;
    size_t at = pts.size();
    if (insert && pts.size() >= 2) {
        // Between the two it adds the least length to (a closed one's last and first too).
        float best = 1e30f;
        const size_t spans = s.spline->spline.closed ? pts.size() : pts.size() - 1;
        for (size_t i = 0; i < spans; ++i) {
            const float3 a = pts[i].position, b = pts[(i + 1) % pts.size()].position;
            if (const float extra = length(local - a) + length(b - local) - length(b - a); extra < best) {
                best = extra;
                at = i + 1;
            }
        }
    }
    pts.insert(pts.begin() + ptrdiff_t(at), level::SplinePoint{local});
    execute({{target, s}});
    sendState(true);
}

void EditorBridge::splineRemove(uint32_t point)
{
    if (selected_ == ~0u || !editable(selected_))
        return;
    State s = capture(selected_);
    if (!s.spline || point >= s.spline->spline.points.size() || s.spline->spline.points.size() <= 1)
        return;
    s.spline->spline.points.erase(s.spline->spline.points.begin() + ptrdiff_t(point));
    if (s.spline->spline.points.size() < 3)
        s.spline->spline.closed = false;
    execute({{selected_, s}});
    sendState(true);
}

void EditorBridge::splineToggle(int32_t corner)
{
    // A point a corner or smooth again (`corner` its index), or (-1) the spline closed or open.
    if (selected_ == ~0u || !editable(selected_))
        return;
    State s = capture(selected_);
    if (!s.spline)
        return;
    if (corner >= 0 && size_t(corner) < s.spline->spline.points.size())
        s.spline->spline.points[size_t(corner)].corner = !s.spline->spline.points[size_t(corner)].corner;
    else if (corner < 0 && s.spline->spline.points.size() >= 3)
        s.spline->spline.closed = !s.spline->spline.closed;
    else
        return;
    execute({{selected_, s}});
    sendState(true);
}

bool EditorBridge::splineDragBegin(uint32_t point)
{
    if (selected_ == ~0u || !editable(selected_))
        return false;
    State s = capture(selected_);
    if (!s.spline || point >= s.spline->spline.points.size())
        return false;
    splineDrag_ = {true, selected_, point, s};
    return true;
}

void EditorBridge::splineDragTo(float3 origin, float3 dir)
{
    if (!splineDrag_.active)
        return;
    const flecs::entity e = entityOf(splineDrag_.target);
    float3 point, normal;
    float w[3][4], inv[3][4];
    if (!e || !e.has<LevelSpline>() || !surfaceHit(origin, dir, {splineDrag_.target}, point, normal) || !worldOf(splineDrag_.target, w) ||
        !invert(w, inv))
        return;
    LevelSpline s = *e.try_get<LevelSpline>();
    if (splineDrag_.point >= s.spline.points.size())
        return;
    s.spline.points[splineDrag_.point].position = rows(inv, point);
    e.set<LevelSpline>(s);  // (live: LevelMeshes lofts it again; one undo step at the end)
}

void EditorBridge::splineDragEnd(bool cancel)
{
    if (!splineDrag_.active)
        return;
    splineDrag_.active = false;
    const uint32_t target = splineDrag_.target;
    if (!editable(target))
        return;
    if (cancel) {
        apply(target, splineDrag_.before);
        return;
    }
    const State after = capture(target);
    if (after.spline == splineDrag_.before.spline)
        return;
    undo_.push_back({{target, splineDrag_.before, after}});
    redo_.clear();
    ++commands_;
    sceneChanged();
    sendState(true);
}

}  // namespace fx
