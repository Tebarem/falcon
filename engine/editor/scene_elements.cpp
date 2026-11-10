// The scene view's selection by box and the level tools' element modes: vertices, edges and faces
// of the selected level mesh, picked on the CPU (its PolyMesh), drawn over the view, moved by the
// gizmo (one undo step a drag). Snapping targets for moves too.

#include "editor/scene_view.h"

#include "editor/editor_bridge.h"
#include "level/poly_mesh.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace fx {

namespace {

float2 sub(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
float dist(float2 a, float2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

float segmentDistance(float2 p, float2 a, float2 b, float* along = nullptr)
{
    const float2 ab = sub(b, a), ap = sub(p, a);
    const float l = ab.x * ab.x + ab.y * ab.y;
    const float t = l > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l, 0.0f, 1.0f) : 0.0f;
    if (along)
        *along = t;
    return dist(p, {a.x + ab.x * t, a.y + ab.y * t});
}

float3 transform(const float m[3][4], float3 p)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}

uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a; }

constexpr uint32_t kEdge = 0xb0a0a0a0u;      // (ABGR) the mesh's edges in an element mode
constexpr uint32_t kChosen = 0xff20b4ffu;    // selected elements (orange)

// The vertices an element selection moves.
std::vector<uint32_t> movedVertices(const level::PolyMesh& m, const EditorBridge::ElementSelection& sel)
{
    std::set<uint32_t> out;
    for (const uint64_t item : sel.items) {
        if (sel.mode == 1) {
            out.insert(uint32_t(item));
        } else if (sel.mode == 2) {
            out.insert(uint32_t(item >> 32));
            out.insert(uint32_t(item & 0xffffffffu));
        } else if (sel.mode == 3 && item < m.faces.size()) {
            const level::Face& f = m.faces[size_t(item)];
            for (uint32_t k = 0; k < f.count; ++k)
                out.insert(m.corners[f.first + k]);
        }
    }
    std::erase_if(out, [&](uint32_t v) { return v >= m.positions.size(); });
    return {out.begin(), out.end()};
}

}  // namespace

bool SceneView::elementTarget(uint32_t& index, float world[3][4]) const
{
    index = bridge_->selected();
    return tools_.element != ElementMode::Object && index != ~0u && bridge_->levelMeshOf(index) && bridge_->worldOf(index, world);
}

bool SceneView::elementCenter(float3& center) const
{
    uint32_t index;
    float w[3][4];
    const EditorBridge::ElementSelection& sel = bridge_->elements();
    if (!elementTarget(index, w) || sel.entity != index || sel.mode != uint32_t(tools_.element) || sel.items.empty())
        return false;
    const auto mesh = bridge_->levelMeshOf(index);
    const std::vector<uint32_t> verts = movedVertices(*mesh, sel);
    if (verts.empty())
        return false;
    float3 c = {0, 0, 0};
    for (const uint32_t v : verts)
        c += transform(w, mesh->positions[v]);
    center = c * (1.0f / float(verts.size()));
    return true;
}

void SceneView::elementClick(float2 mouse, uint32_t mode)
{
    uint32_t index;
    float w[3][4];
    if (!elementTarget(index, w))
        return;
    const auto mesh = bridge_->levelMeshOf(index);
    EditorBridge::ElementSelection& sel = bridge_->elements();
    if (sel.entity != index || sel.mode != uint32_t(tools_.element))
        sel = {index, uint32_t(tools_.element), {}};
    uint64_t hit = ~0ull;
    if (tools_.element == ElementMode::Vertex) {
        float best = 12.0f;
        for (uint32_t v = 0; v < mesh->positions.size(); ++v)
            if (float2 p; project(transform(w, mesh->positions[v]), p) && dist(p, mouse) < best) {
                best = dist(p, mouse);
                hit = v;
            }
    } else if (tools_.element == ElementMode::Edge) {
        float best = 8.0f;
        for (const level::Face& f : mesh->faces)
            for (uint32_t k = 0; k < f.count; ++k) {
                const uint32_t a = mesh->corners[f.first + k], b = mesh->corners[f.first + (k + 1) % f.count];
                float2 pa, pb;
                if (!project(transform(w, mesh->positions[a]), pa) || !project(transform(w, mesh->positions[b]), pb))
                    continue;
                if (const float d = segmentDistance(mouse, pa, pb); d < best) {
                    best = d;
                    hit = edgeKey(a, b);
                }
            }
    } else {
        // Faces: the nearest one the mouse's ray meets (each face a fan of triangles).
        const float3 o = camPos_, d = normalize(rayDirection(mouse));
        float best = 1e30f;
        for (uint32_t fi = 0; fi < mesh->faces.size(); ++fi) {
            const level::Face& f = mesh->faces[fi];
            for (uint32_t k = 1; k + 1 < f.count; ++k) {
                const float3 a = transform(w, mesh->positions[mesh->corners[f.first]]);
                const float3 e1 = transform(w, mesh->positions[mesh->corners[f.first + k]]) - a;
                const float3 e2 = transform(w, mesh->positions[mesh->corners[f.first + k + 1]]) - a;
                const float3 pv = cross(d, e2);
                const float det = dot(e1, pv);
                if (std::fabs(det) < 1e-12f)
                    continue;
                const float3 tv = o - a, qv = cross(tv, e1);
                const float u = dot(tv, pv) / det, v = dot(d, qv) / det, t = dot(e2, qv) / det;
                if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && t > 0.0f && t < best) {
                    best = t;
                    hit = fi;
                }
            }
        }
    }
    const auto it = std::find(sel.items.begin(), sel.items.end(), hit);
    using M = EditorBridge::SelectMode;
    if (M(mode) == M::Replace)
        sel.items.clear();
    if (hit == ~0ull)
        return bridge_->toolsChanged();
    if (M(mode) == M::Toggle && it != sel.items.end())
        sel.items.erase(std::find(sel.items.begin(), sel.items.end(), hit));
    else if (std::find(sel.items.begin(), sel.items.end(), hit) == sel.items.end())
        sel.items.push_back(hit);
    bridge_->toolsChanged();
}

void SceneView::elementBox(float2 a, float2 b, uint32_t mode)
{
    uint32_t index;
    float w[3][4];
    if (!elementTarget(index, w))
        return;
    const auto mesh = bridge_->levelMeshOf(index);
    const float2 lo = {std::min(a.x, b.x), std::min(a.y, b.y)}, hi = {std::max(a.x, b.x), std::max(a.y, b.y)};
    auto inBox = [&](float3 world) {
        float2 p;
        return project(world, p) && p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y;
    };
    EditorBridge::ElementSelection& sel = bridge_->elements();
    if (sel.entity != index || sel.mode != uint32_t(tools_.element) || EditorBridge::SelectMode(mode) == EditorBridge::SelectMode::Replace)
        sel = {index, uint32_t(tools_.element), {}};
    std::vector<uint64_t> found;
    if (tools_.element == ElementMode::Vertex) {
        for (uint32_t v = 0; v < mesh->positions.size(); ++v)
            if (inBox(transform(w, mesh->positions[v])))
                found.push_back(v);
    } else if (tools_.element == ElementMode::Edge) {
        std::set<uint64_t> seen;
        for (const level::Face& f : mesh->faces)
            for (uint32_t k = 0; k < f.count; ++k) {
                const uint32_t va = mesh->corners[f.first + k], vb = mesh->corners[f.first + (k + 1) % f.count];
                if (seen.insert(edgeKey(va, vb)).second &&
                    inBox((transform(w, mesh->positions[va]) + transform(w, mesh->positions[vb])) * 0.5f))
                    found.push_back(edgeKey(va, vb));
            }
    } else {
        for (uint32_t f = 0; f < mesh->faces.size(); ++f)
            if (inBox(transform(w, mesh->faceCenter(f))))
                found.push_back(f);
    }
    for (const uint64_t item : found) {
        const auto it = std::find(sel.items.begin(), sel.items.end(), item);
        if (EditorBridge::SelectMode(mode) == EditorBridge::SelectMode::Toggle && it != sel.items.end())
            sel.items.erase(it);
        else if (it == sel.items.end())
            sel.items.push_back(item);
    }
    bridge_->toolsChanged();
}

void SceneView::boxSelect(float2 a, float2 b, uint32_t mode)
{
    if (tools_.element != ElementMode::Object && bridge_->selected() != ~0u && bridge_->levelMeshOf(bridge_->selected()))
        return elementBox(a, b, mode);
    const float2 lo = {std::min(a.x, b.x), std::min(a.y, b.y)}, hi = {std::max(a.x, b.x), std::max(a.y, b.y)};
    std::vector<std::pair<uint32_t, float3>> centers;
    bridge_->selectableCenters(centers);
    std::vector<uint32_t> inside;
    for (const auto& [index, c] : centers)
        if (float2 p; project(c, p) && p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y)
            inside.push_back(index);
    bridge_->selectMany(inside, EditorBridge::SelectMode(mode));
    bridge_->toolsChanged();
}

void SceneView::elementsMoved(const float delta[3][4], bool commit, bool cancel)
{
    const uint32_t index = drag_.entity;
    if (!drag_.mesh0)
        return;
    if (cancel) {
        bridge_->setLevelMeshLive(index, drag_.mesh0);
        return;
    }
    if (commit) {
        bridge_->commitLevelMesh(index, drag_.mesh0);
        return;
    }
    // Each moved vertex: to the world, through the drag's change, back to the mesh's space.
    float w[3][4], inv[3][4];
    if (!bridge_->worldOf(index, w))
        return;
    affineInverse(w, inv);
    auto mesh = std::make_shared<level::PolyMesh>(*drag_.mesh0);
    for (const uint32_t v : movedVertices(*mesh, bridge_->elements()))
        mesh->positions[v] = transform(inv, transform(delta, transform(w, drag_.mesh0->positions[v])));
    bridge_->setLevelMeshLive(index, std::move(mesh));
}

bool SceneView::snapTarget(float2 mouse, bool surface, float3& target) const
{
    if (surface) {
        std::vector<uint32_t> skip;
        for (const auto& [index, world] : drag_.roots)
            skip.push_back(index);
        if (drag_.elements)
            skip.push_back(drag_.entity);
        float3 normal;
        return bridge_->surfaceHit(camPos_, rayDirection(mouse), skip, target, normal);
    }
    // The nearest of the points (vertices, level mesh edges' middles, mesh bounds' corners) on
    // screen, within 16 px.
    float best = 16.0f;
    bool found = false;
    for (const float3& p : drag_.snapPoints)
        if (float2 s; project(p, s) && dist(s, mouse) < best) {
            best = dist(s, mouse);
            target = p;
            found = true;
        }
    return found;
}

void SceneView::buildElements()
{
    uint32_t index;
    float w[3][4];
    if (!elementTarget(index, w) || tools_.gameView)
        return;
    const auto mesh = bridge_->levelMeshOf(index);
    const EditorBridge::ElementSelection& sel = bridge_->elements();
    const bool mine = sel.entity == index && sel.mode == uint32_t(tools_.element);
    std::set<uint64_t> chosenEdges;
    if (mine && sel.mode == 2)
        chosenEdges.insert(sel.items.begin(), sel.items.end());
    // Every edge, thin (selected edges and the selected faces' outlines thick).
    std::set<uint64_t> drawn;
    for (uint32_t fi = 0; fi < mesh->faces.size(); ++fi) {
        const level::Face& f = mesh->faces[fi];
        const bool faceChosen = mine && sel.mode == 3 && std::find(sel.items.begin(), sel.items.end(), uint64_t(fi)) != sel.items.end();
        for (uint32_t k = 0; k < f.count; ++k) {
            const uint32_t a = mesh->corners[f.first + k], b = mesh->corners[f.first + (k + 1) % f.count];
            const uint64_t key = edgeKey(a, b);
            const bool chosen = faceChosen || chosenEdges.count(key);
            if (!chosen && !drawn.insert(key).second)
                continue;
            float2 pa, pb;
            if (project(transform(w, mesh->positions[a]), pa) && project(transform(w, mesh->positions[b]), pb))
                line(pa, pb, chosen ? 3.0f : 1.0f, chosen ? kChosen : kEdge);
        }
        if (faceChosen)
            if (float2 c; project(transform(w, mesh->faceCenter(fi)), c))
                disc(c, 4.0f, kChosen, 10);
    }
    if (tools_.element == ElementMode::Vertex)
        for (uint32_t v = 0; v < mesh->positions.size(); ++v) {
            const bool chosen = mine && std::find(sel.items.begin(), sel.items.end(), uint64_t(v)) != sel.items.end();
            if (float2 p; project(transform(w, mesh->positions[v]), p))
                disc(p, chosen ? 5.0f : 3.0f, chosen ? kChosen : kEdge, 8);
        }
}

}  // namespace fx
