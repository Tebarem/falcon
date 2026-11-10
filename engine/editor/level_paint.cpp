// The level paint brush (SceneTool::Paint): strokes on the selected level mesh that paint its mask
// (the weights of its layers 1..3, or erase them) or its vertex colours, live while the mouse is
// down and one undo step at the end. The mask is painted within a ball round the hit, on every face it
// reaches (level/mask_brush.h); colours go on the vertices within the brush. Or decals, stamped along
// the stroke on any surface (each a Decal entity), or damage cut into level meshes (each a lumpy cut
// volume under the mesh, on its edges or corners if the tool says), or prefab pieces scattered over
// any surface (editor/level_scatter.cpp): the stroke one undo step.

#include "editor/editor_bridge.h"

#include "core/json.h"
#include "editor/scene_view.h"
#include "level/level_mask.h"
#include "level/level_meshes.h"
#include "level/mask_brush.h"
#include "level/rubble.h"
#include "level/wear.h"
#include "vfx/vfx_components.h"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cmath>

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

// A stamp's randomness from where it is (to the millimetre): strokes don't repeat each other.
uint32_t stampHash(float3 p)
{
    uint32_t h = 2166136261u;
    for (const float v : {p.x, p.y, p.z})
        h = (h ^ uint32_t(int32_t(std::lround(v * 1000.0f)))) * 16777619u;
    return h;
}

}  // namespace

bool EditorBridge::paintRay(uint32_t target, float3 origin, float3 dir, uint32_t& face, uint32_t corners[3], float3& bary,
                            float3& point) const
{
    const auto mesh = levelMeshOf(target);
    float w[3][4];
    if (!mesh || !worldOf(target, w))
        return false;
    dir = normalize(dir);
    float best = 1e30f;
    for (uint32_t f = 0; f < mesh->faces.size(); ++f) {
        const level::Face& fc = mesh->faces[f];
        for (uint32_t k = 1; k + 1 < fc.count; ++k) {
            const float3 a = rows(w, mesh->positions[mesh->corners[fc.first]]), b = rows(w, mesh->positions[mesh->corners[fc.first + k]]),
                         c = rows(w, mesh->positions[mesh->corners[fc.first + k + 1]]);
            const float3 e1 = b - a, e2 = c - a, pv = cross(dir, e2);
            const float det = dot(e1, pv);
            if (std::fabs(det) < 1e-12f)
                continue;
            const float3 tv = origin - a;
            const float u = dot(tv, pv) / det;
            const float3 qv = cross(tv, e1);
            const float v = dot(dir, qv) / det, t = dot(e2, qv) / det;
            if (u < 0.0f || v < 0.0f || u + v > 1.0f || t <= 1e-4f || t >= best)
                continue;
            best = t;
            face = f;
            corners[0] = 0;
            corners[1] = k;
            corners[2] = k + 1;
            bary = {1.0f - u - v, u, v};
            point = origin + dir * t;
        }
    }
    return best < 1e30f;
}

bool EditorBridge::paintHover(float3 origin, float3 dir, float3& point, float3& normal) const
{
    if (view_ && view_->tools().paintMode >= 5)  // decals, damage: any surface
        return surfaceHit(origin, dir, {}, point, normal);
    const uint32_t target = paint_.active ? paint_.target : selected_;
    uint32_t face, corners[3];
    float3 bary;
    float w[3][4];
    if (target == ~0u || !paintRay(target, origin, dir, face, corners, bary, point) || !worldOf(target, w))
        return false;
    const auto mesh = levelMeshOf(target);
    const float3 n = mesh->faceNormal(face);
    normal = normalize(float3{w[0][0] * n.x + w[0][1] * n.y + w[0][2] * n.z, w[1][0] * n.x + w[1][1] * n.y + w[1][2] * n.z,
                              w[2][0] * n.x + w[2][1] * n.y + w[2][2] * n.z});
    return true;
}

bool EditorBridge::paintBegin(float3 origin, float3 dir, bool* missed)
{
    paint_ = {};
    if (missed)
        *missed = false;
    if (playing_ || !view_)
        return false;
    if (view_->tools().paintMode >= 5) {  // decals, damage, scatter: on any surface, the stroke one undo step
        if (view_->tools().paintMode == 5 && view_->tools().paintDecal.empty()) {
            notice("Pick the decals' material first (the paint panel)", "warning");
            return false;
        }
        if (view_->tools().paintMode == 7 && view_->tools().scatterPrefabs.empty() && !(SDL_GetModState() & SDL_KMOD_SHIFT)) {
            notice("Put prefabs on the prefab shelf first (the scatter brush scatters them)", "warning");
            return false;
        }
        paint_.active = paint_.stamping = true;
        paint_.erasing = view_->tools().paintMode == 7 && (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        undoBegin();
        paintTo(origin, dir);
        return true;
    }
    // (off the selected level mesh, or none: an ordinary click, which picks what is under the mouse)
    uint32_t face, corners[3];
    float3 bary, point;
    const auto mesh = selected_ != ~0u ? levelMeshOf(selected_) : nullptr;
    if (!mesh || !paintRay(selected_, origin, dir, face, corners, bary, point)) {
        if (missed)
            *missed = true;
        return false;
    }
    if (!editable(selected_))
        return false;
    const uint32_t mode = view_->tools().paintMode;
    if (mode < 4 && (!mesh->layered() || mesh->cornerUv1.size() != mesh->corners.size())) {
        notice("Give the level mesh layers first (its mask paints them)", "warning");
        return false;
    }
    paint_.active = true;
    paint_.target = selected_;
    paint_.meshBefore = mesh;
    const State s = capture(selected_);
    paint_.maskBefore = s.levelMask;
    paint_.mask = s.levelMask && s.levelMask->size() == level::kMaskBytes ? *s.levelMask : std::vector<uint8_t>(level::kMaskBytes, 0);
    paint_.mesh = std::make_shared<level::PolyMesh>(*mesh);
    paint_.dabbed = false;
    paintTo(origin, dir);
    sendState(false);
    return true;
}

void EditorBridge::paintTo(float3 origin, float3 dir)
{
    if (!paint_.active || !view_)
        return;
    if (paint_.stamping && view_->tools().paintMode == 7) {  // scattered pieces (or erased) every brush radius along the stroke
        float3 point, normal;
        std::vector<uint32_t> skip;
        if (const uint32_t set = scatterSet({}, false); set != ~0u)
            skip = subtree(set);
        if (!surfaceHit(origin, dir, skip, point, normal))
            return;
        if (paint_.dabbed && length(point - paint_.last) < view_->tools().paintRadius)
            return;
        if (paint_.erasing)
            scatterErase(point);
        else
            scatterDab(point, normal);
        paint_.last = point;
        paint_.dabbed = true;
        return;
    }
    if (paint_.stamping) {  // a decal every 1.2 brush radii along the stroke, damage every 1.5
        const bool damage = view_->tools().paintMode == 6;
        float3 point, normal;
        uint32_t record = ~0u, face = ~0u;
        if (!surfaceHit(origin, dir, {}, point, normal, &record, &face) || (damage && record == ~0u))
            return;
        if (paint_.dabbed && length(point - paint_.last) < view_->tools().paintRadius * (damage ? 1.5f : 1.2f))
            return;
        if (damage)
            stampDamage(record, face, point, normal);
        else
            stampDecal(point, normal);
        paint_.last = point;
        paint_.dabbed = true;
        return;
    }
    uint32_t face, corners[3];
    float3 bary, point;
    if (!paintRay(paint_.target, origin, dir, face, corners, bary, point))
        return;
    // Dabs a quarter of the radius apart along the stroke: a fast drag gets the ones between too
    // (each found on the mesh along the camera's ray through its point).
    const float spacing = std::max(view_->tools().paintRadius * 0.25f, 1e-3f);
    const float gap = paint_.dabbed ? length(point - paint_.last) : 0.0f;
    if (paint_.dabbed && gap < spacing)
        return;
    const uint32_t between = paint_.dabbed ? std::min(uint32_t(gap / spacing), 64u) : 0u;
    for (uint32_t i = 1; i < between; ++i) {
        const float3 q = paint_.last + (point - paint_.last) * (float(i) / float(between));
        uint32_t f, c[3];
        float3 b, at;
        if (paintRay(paint_.target, origin, q - origin, f, c, b, at))
            paintDab(at, f, c, b);
    }
    paintDab(point, face, corners, bary);
    paint_.last = point;
    paint_.dabbed = true;
}

void EditorBridge::paintDab(float3 point, uint32_t face, const uint32_t corners[3], float3 bary)
{
    const SceneToolState& t = view_->tools();
    const flecs::entity e = entityOf(paint_.target);
    float w[3][4];
    if (!e || !worldOf(paint_.target, w))
        return;
    level::PolyMesh& mesh = *paint_.mesh;
    const level::Face& fc = mesh.faces[face];
    if (t.paintMode == 4) {
        // Vertex colours: each vertex within the brush toward the colour.
        if (mesh.vertexColors.empty())
            mesh.vertexColors.assign(mesh.positions.size(), 0xffffffffu);
        for (uint32_t v = 0; v < mesh.positions.size(); ++v) {
            const float k = t.paintStrength * level::brushFalloff(length(rows(w, mesh.positions[v]) - point), t.paintRadius);
            if (k <= 0.0f)
                continue;
            uint32_t c = mesh.vertexColors[v], out = 0;
            for (int ch = 0; ch < 4; ++ch) {
                const float from = float(c >> (8 * ch) & 0xff), to = float(t.paintColor >> (8 * ch) & 0xff);
                out |= uint32_t(std::lround(from + (to - from) * k)) << (8 * ch);
            }
            mesh.vertexColors[v] = out;
        }
        setLevelMeshLive(paint_.target, std::make_shared<level::PolyMesh>(mesh));
        return;
    }
    // The mask: a ball round the hit, wrapping round edges and corners onto every face it reaches
    // that doesn't look away from this one (level/mask_brush.h).
    const float3 a = rows(w, mesh.positions[mesh.corners[fc.first + corners[0]]]), b = rows(w, mesh.positions[mesh.corners[fc.first + corners[1]]]),
                 c = rows(w, mesh.positions[mesh.corners[fc.first + corners[2]]]);
    level::MaskDab dab;
    dab.center = point;
    dab.normal = normalize(cross(b - a, c - a));
    dab.radius = t.paintRadius;
    dab.strength = t.paintStrength;
    dab.channel = std::min(t.paintMode, 3u);
    (void)bary;
    if (!level::paintMaskDab(mesh, w, dab, paint_.mask, &paint_.stroke))
        return;
    e.set<LevelMask>({std::make_shared<const std::vector<uint8_t>>(paint_.mask)});
}

void EditorBridge::stampDecal(float3 point, float3 normal)
{
    // Its Y out of the surface (a decal projects along -Y), turned and sized at random (a hash of
    // where it is and the stroke's count: the same stroke stamps the same).
    const SceneToolState& t = view_->tools();
    const uint32_t n = ++paint_.stamps ^ stampHash(point);
    const auto random = [&](uint32_t salt) {
        uint32_t h = n * 2654435761u ^ salt * 2246822519u;
        h = (h ^ (h >> 15)) * 2246822519u;
        return float((h ^ (h >> 13)) & 0xffffu) / 65535.0f;
    };
    const float3 y = normalize(normal);
    const float3 any = std::fabs(y.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0};
    const float3 x = normalize(cross(any, y));
    const float3 z = cross(x, y);
    const float turn = random(1) * 6.2831853f, c = std::cos(turn), s = std::sin(turn);
    const float3 xr = x * c + z * s, zr = cross(xr, y);
    const float size = t.paintRadius * 2.0f * (0.8f + 0.4f * random(2)), depth = std::max(size * 0.5f, 0.1f);
    const float3 at = point;  // (its box half in front of the surface, half behind)
    State st;
    const float world[3][4] = {{xr.x, y.x, zr.x, at.x}, {xr.y, y.y, zr.y, at.y}, {xr.z, y.z, zr.z, at.z}};
    std::memcpy(st.world, world, sizeof(world));
    const uint32_t created = spawn(EntityKind::Empty, "", st.world, "Decal");
    if (created == ~0u)
        return;
    st.parent = ~0u;
    st.alive = true;
    st.name = defaultName(created);
    Decal& d = *static_cast<Decal*>(st.vfx.add(VfxIndexOf<Decal>::value));
    d.material = vfxAssetId(t.paintDecal);
    d.size[0] = d.size[2] = size;
    d.size[1] = depth;
    d.opacity = std::clamp(t.paintStrength, 0.05f, 1.0f);
    execute({{created, st}});
}

void EditorBridge::stampDamage(uint32_t record, uint32_t face, float3 point, float3 normal)
{
    // On the face's nearest sharp edge or corner (in the mesh's space), when the tool says; a lumpy
    // ball turned and stretched at random (a hash of where it is and the stroke's count).
    const SceneToolState& t = view_->tools();
    const auto mesh = levelMeshOf(record);
    float w[3][4], inv[3][4];
    if (!mesh || !worldOf(record, w))
        return;
    const auto snap = level::DamageSnap(std::min(t.paintDamage, 2u));
    if (snap != level::DamageSnap::Hole) {
        float3 at;
        if (face == ~0u || !invert(w, inv) || !level::damageSpot(*mesh, face, rows(inv, point), snap, at))
            return;
        point = rows(w, at);
        if (length(point - paint_.last) < 1e-4f && paint_.dabbed)
            return;  // (the same corner again)
    }
    damageCut(record, face, point, normal, snap, ++paint_.stamps ^ stampHash(point));
}

void EditorBridge::damageSettings(const Json& m)
{
    // Spacing, randomness and density of damage over the selection: from the paint panel
    // ("damageSpacing" ...) or the Level window's Damage Selection (its settings: "spacing",
    // "randomness", "density", in m and %); the paint panel's told to editor code's command options.
    if (!view_)
        return;
    SceneToolState& t = view_->tools();
    struct Field {
        const char *tools, *message, *option;
        float* value;
        float lo, hi, percent;
    };
    const Field fields[] = {{"damageSpacing", "spacing", "Spacing", &t.damageSpacing, 0.0f, 50.0f, 1.0f},
                            {"damageRandomness", "randomness", "Randomness", &t.damageRandomness, 0.0f, 1.0f, 100.0f},
                            {"damageDensity", "density", "Density", &t.damageDensity, 0.0f, 1.0f, 100.0f}};
    for (const Field& f : fields) {
        const Json* v = m.get(f.tools);
        const bool fromCode = !v && (v = m.get(f.message)) != nullptr;
        if (!v || v->type != Json::Type::Number || !std::isfinite(v->number))
            continue;
        *f.value = std::clamp(float(fromCode ? v->number / f.percent : v->number), f.lo, f.hi);
        if (fromCode)
            continue;
        JsonWriter w;
        w.beginObject().value("type", "option").value("menu", "Paint/Damage Selection").value("name", f.option)
            .value("value", double(*f.value * f.percent)).endObject();
        editorEvents_.push_back(w.str());
    }
}

void EditorBridge::damageCut(uint32_t record, uint32_t face, float3 point, float3 normal, level::DamageSnap snap, uint32_t n, bool withRubble,
                             float scale)
{
    const SceneToolState& t = view_->tools();
    const auto mesh = levelMeshOf(record);
    if (!mesh || !canHold(record))
        return;
    const auto random = [&](uint32_t salt) {
        uint32_t h = n * 2654435761u ^ salt * 2246822519u;
        h = (h ^ (h >> 15)) * 2246822519u;
        return float((h ^ (h >> 13)) & 0xffffu) / 65535.0f;
    };
    const float3 y = normalize(normal);
    const float3 any = std::fabs(y.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0};
    const float3 x = normalize(cross(any, y)), z = cross(x, y);
    const float turn = random(1) * 6.2831853f, c = std::cos(turn), s = std::sin(turn);
    const float3 xr = x * c + z * s, zr = cross(xr, y);
    // A hole as wide as the brush; a chip or a corner smaller.
    const float size = t.paintRadius * (snap == level::DamageSnap::Hole ? 2.0f : snap == level::DamageSnap::Chip ? 1.0f : 1.4f) *
                       (0.8f + 0.4f * random(2)) * std::max(scale, 0.05f);
    const float sx = size * (0.85f + 0.3f * random(3)), sy = size * (0.85f + 0.3f * random(4)), sz = size * (0.85f + 0.3f * random(5));
    State st;
    const float world[3][4] = {{xr.x * sx, y.x * sy, zr.x * sz, point.x}, {xr.y * sx, y.y * sy, zr.y * sz, point.y},
                               {xr.z * sx, y.z * sy, zr.z * sz, point.z}};
    std::memcpy(st.world, world, sizeof(world));
    const uint32_t created = spawn(EntityKind::Empty, "", st.world, "Damage");
    if (created == ~0u)
        return;
    st.parent = record;  // (it moves with what it broke)
    st.alive = true;
    st.name = defaultName(created);
    CutVolume cut;
    cut.shape = level::CutShape::Damage;
    cut.capMaterial = t.paintCap;
    cut.seed = n * 7919u + uint32_t(random(6) * 65535.0f);
    cut.roughness = 0.2f + 0.4f * std::clamp(t.paintStrength, 0.0f, 1.0f);
    st.cut = cut;
    execute({{created, st}});
    // A heap of what it knocked out on what is below, on the side it was hit from (none under a
    // hole in a floor: it would fall through). Its chunks get the broken faces' material.
    if (!withRubble || !t.paintRubble || y.y >= 0.7f)
        return;
    float3 ground, up;
    const float3 from = point + y * (size * 0.6f);
    if (!surfaceHit(from, {0, -1, 0}, {}, ground, up) || from.y - ground.y > 50.0f)
        return;
    level::RubbleSettings rubble;
    rubble.seed = cut.seed;
    rubble.count = 4 + uint32_t(random(7) * 5.0f);
    rubble.size = size * 0.4f;
    rubble.spread = size * 0.7f;
    rubble.material = !t.paintCap.empty() ? t.paintCap
                      : face < mesh->faces.size() && mesh->faces[face].material < mesh->materials.size() ? mesh->materials[mesh->faces[face].material]
                      : mesh->materials.empty() ? std::string() : mesh->materials[0];
    const float yaw = random(8) * 6.2831853f, cyaw = std::cos(yaw), syaw = std::sin(yaw);
    State heap;
    const float at[3][4] = {{cyaw, 0, syaw, ground.x}, {0, 1, 0, ground.y}, {-syaw, 0, cyaw, ground.z}};
    std::memcpy(heap.world, at, sizeof(at));
    const uint32_t made = spawn(EntityKind::Level, "box", heap.world, "Rubble");
    if (made == ~0u)
        return;
    heap.alive = true;
    heap.name = defaultName(made);
    heap.levelMesh = std::make_shared<level::PolyMesh>(level::makeRubble(rubble));
    execute({{made, heap}});
}

void EditorBridge::paintEnd()
{
    if (!paint_.active)
        return;
    if (paint_.stamping) {
        paint_.active = false;
        undoEnd();
        sendState(true);
        return;
    }
    const uint32_t target = paint_.target;
    paint_.active = false;
    if (!editable(target))
        return;
    // One undo step: what the stroke started from, and where it ended.
    const State after = capture(target);
    if (after.levelMask == paint_.maskBefore && after.levelMesh == paint_.meshBefore) {
        sendState(false);
        return;
    }
    State before = after;
    before.levelMask = paint_.maskBefore;
    before.levelMesh = paint_.meshBefore;
    undo_.push_back({{target, before, after}});
    redo_.clear();
    ++commands_;
    sceneChanged();
    paint_.mesh.reset();
    paint_.mask.clear();
    sendState(false);
}

bool EditorBridge::nearestLevelSurface(float3 p, float reach, uint32_t& record, uint32_t& face, float3& point, float3& normal) const
{
    float best = reach;
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const flecs::entity e = entityOf(i);
        float w[3][4];
        if (!e || !e.enabled() || hiddenBelow(i) || e.has<CutVolume>() || e.has<LevelVolume>() || !worldOf(i, w))
            continue;
        const auto mesh = levelMeshOf(i);
        if (!mesh)
            continue;
        for (uint32_t f = 0; f < mesh->faces.size(); ++f) {
            const level::Face& fc = mesh->faces[f];
            for (uint32_t k = 1; k + 1 < fc.count; ++k) {
                const float3 a = rows(w, mesh->positions[mesh->corners[fc.first]]), b = rows(w, mesh->positions[mesh->corners[fc.first + k]]),
                             c = rows(w, mesh->positions[mesh->corners[fc.first + k + 1]]);
                const float3 q = level::closestOnTriangle(p, a, b, c);
                const float d = length(q - p);
                const float3 n = cross(b - a, c - a);
                if (d >= best || length(n) < 1e-12f)
                    continue;
                best = d, record = i, face = f, point = q, normal = normalize(n);
            }
        }
    }
    return best < reach;
}

uint32_t EditorBridge::damageSelection(const ElementSelection* named)
{
    if (!view_ || selected_ == ~0u)
        return 0;
    const SceneToolState& t = view_->tools();
    const float radius = std::max(t.paintRadius, 0.02f);
    uint32_t made = 0;
    undoBegin();
    if (const flecs::entity e = entityOf(selected_); e && e.has<LevelSpline>()) {
        // Along the spline's curve, every 1.5 brush radii (or the spacing set), on the level mesh
        // nearest each spot (as the tool's kind sizes it: a hole, a chip, a corner); each strays
        // along it and varies in size by the randomness, and the density's share of them are made.
        std::vector<float3> curve;
        const auto kind = level::DamageSnap(std::min(t.paintDamage, 2u));
        if (splineWorld(selected_, nullptr, &curve) && !curve.empty()) {
            const float spacing = t.damageSpacing > 0.0f ? t.damageSpacing : radius * 1.5f;
            const uint32_t seed = ++damageRuns_ * 2654435761u;
            const auto chance = [&](uint32_t i, uint32_t salt) {
                uint32_t h = i * 374761393u + salt * 668265263u + seed * 3266489917u;
                h = (h ^ (h >> 13)) * 1274126177u;
                return float((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
            };
            uint32_t place = 0;
            float travelled = 0.0f, next = 0.0f;  // (along the curve: to this segment's start, to the next spot)
            uint32_t tried = 0;
            for (size_t k = 0; k + 1 < curve.size() && made < 256 && tried < 4096; ++k) {
                const float3 a = curve[k], b = curve[k + 1];
                const float l = length(b - a);
                for (; next <= travelled + l && made < 256 && tried < 4096; next += spacing, ++tried) {
                    const uint32_t i = place++;
                    if (t.damageDensity < 1.0f && chance(i, 101) >= t.damageDensity)
                        continue;
                    const float along = std::clamp(next + t.damageRandomness * (chance(i, 103) - 0.5f) * spacing - travelled, 0.0f, l);
                    const float3 p = a + (b - a) * (along / std::max(l, 1e-6f));
                    uint32_t record, face;
                    float3 at, out;
                    if (nearestLevelSurface(p, std::max(radius * 2.0f, 0.5f), record, face, at, out)) {
                        damageCut(record, face, at, out, kind, stampHash(at) ^ made * 7919u, kind == level::DamageSnap::Hole || made % 3 == 0,
                                  1.0f + t.damageRandomness * (chance(i, 102) - 0.5f));
                        ++made;
                    }
                }
                travelled += l;
            }
        }
    } else if (uint32_t target; levelMeshOf(selected_)) {
        // The chosen vertices, edges or faces (none in the object mode: every sharp outside edge).
        ElementSelection chosen;
        float w[3][4];
        levelTarget(target, chosen);
        if (named)
            chosen = *named, chosen.entity = target;
        if ((chosen.mode == 0 || !chosen.items.empty()) && worldOf(target, w)) {
            const float spacing = t.damageSpacing > 0.0f ? t.damageSpacing : radius * (chosen.mode == 3 ? 2.5f : 1.5f);
            const uint32_t seed = stampHash({w[0][3], w[1][3], w[2][3]}) ^ ++damageRuns_ * 2654435761u;
            const auto spots = level::damageSpots(*levelMeshOf(target), w, chosen.mode, chosen.items, spacing, seed, t.damageRandomness, t.damageDensity);
            for (const level::DamageSpotAt& s : spots) {
                damageCut(target, s.face, s.at, s.normal, s.kind, seed ^ stampHash(s.at), s.kind == level::DamageSnap::Hole || made % 3 == 0, s.scale);
                ++made;
            }
        }
    }
    undoEnd();
    sendState(true);
    return made;
}

}  // namespace fx
