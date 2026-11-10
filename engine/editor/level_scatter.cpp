// The scatter brush (paint mode 7): prefab instances scattered over any surface along a stroke,
// under a scatter set (an empty with a ScatterSet: the selected one, or a new "Scatter" at the
// stroke's start). In each dab, about `scatterDensity` pieces a square metre land at random in the
// brush's disc, each found on the surface under it, kept when the surface isn't steeper than
// `scatterSlope` and no piece of the set is within `scatterSpacing`; a random one of the tool's
// prefabs, turned at random about the surface's up (or the world's), sized at random in
// `scatterScale`. Shift erases the set's pieces under the brush. The stroke is one undo step.

#include "editor/editor_bridge.h"

#include "editor/scene_view.h"
#include "level/level_meshes.h"
#include "scene/fly_camera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

uint32_t hashPoint(float3 p)
{
    uint32_t h = 2166136261u;
    for (const float v : {p.x, p.y, p.z})
        h = (h ^ uint32_t(int32_t(std::lround(v * 1000.0f)))) * 16777619u;
    return h;
}

struct Random {
    uint32_t state;
    float next()  // 0..1
    {
        state = state * 1664525u + 1013904223u;
        uint32_t h = (state ^ (state >> 16)) * 2246822519u;
        return float((h ^ (h >> 13)) & 0xffffu) / 65535.0f;
    }
};

}  // namespace

uint32_t EditorBridge::scatterSet(float3 at, bool make)
{
    // The selected scatter set, or the one the selected piece is in; else (`make`) a new one there.
    for (uint32_t k = selected_; k != ~0u && k < records_.size();) {
        if (entityOf(k) && entityOf(k).has<ScatterSet>() && editable(k))
            return k;
        const uint32_t up = capture(k).parent;
        if (up == k)
            break;
        k = up;
    }
    if (!make)
        return ~0u;
    State s;
    const float world[3][4] = {{1, 0, 0, at.x}, {0, 1, 0, at.y}, {0, 0, 1, at.z}};
    std::memcpy(s.world, world, sizeof(world));
    const uint32_t made = spawn(EntityKind::Empty, "", s.world, "Scatter");
    if (made == ~0u)
        return ~0u;
    s.alive = true;
    s.name = defaultName(made);
    s.scatter = ScatterSet{};
    execute({{made, s}});
    select(made);
    return made;
}

void EditorBridge::scatterDab(float3 point, float3 normal)
{
    const SceneToolState& t = view_->tools();
    if (t.scatterPrefabs.empty())
        return;
    const uint32_t set = scatterSet(point, true);
    if (set == ~0u)
        return;
    // Where the set's pieces are (none closer than the spacing to a new one).
    std::vector<float3> taken;
    for (uint32_t k = 0; k < records_.size(); ++k)
        if (entityOf(k) && capture(k).alive && capture(k).parent == set) {
            const State s = capture(k);
            taken.push_back({s.world[0][3], s.world[1][3], s.world[2][3]});
        }
    std::vector<uint32_t> skip = subtree(set);  // (the set's pieces, and each one placed now: none lands on another)
    Random r{hashPoint(point) ^ (++paint_.stamps * 2654435761u)};
    const float radius = std::max(t.paintRadius, 0.01f);
    const float wanted = std::max(t.scatterDensity, 0.0f) * 3.14159265f * radius * radius * 0.5f;  // (dabs overlap: half the disc each)
    uint32_t count = uint32_t(wanted);
    if (r.next() < wanted - float(count))
        ++count;
    const float3 any = std::fabs(normal.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0};
    const float3 u = normalize(cross(any, normal)), v = cross(normal, u);
    const float cosSlope = std::cos(std::clamp(t.scatterSlope, 0.0f, 90.0f) * 3.14159265f / 180.0f);
    for (uint32_t i = 0; i < std::min(count, 64u); ++i) {
        const float d = radius * std::sqrt(r.next()), a = r.next() * 6.2831853f;
        const float3 above = point + (u * std::cos(a) + v * std::sin(a)) * d + normal * radius;
        float3 at, n;
        if (!surfaceHit(above, normal * -1.0f, skip, at, n) || length(at - point) > radius * 2.0f || n.y < cosSlope - 1e-4f)
            continue;
        bool crowded = false;
        for (const float3& p : taken)
            crowded |= length(p - at) < t.scatterSpacing;
        if (crowded)
            continue;
        // Its frame: up (the surface's or the world's), turned at random about it, sized at random.
        const float3 up = t.scatterAlign ? n : float3{0, 1, 0};
        const float3 side = std::fabs(up.y) < 0.9f ? float3{0, 1, 0} : float3{1, 0, 0};
        const float3 x0 = normalize(cross(side, up)), z0 = cross(x0, up);
        const float turn = r.next() * 6.2831853f, c = std::cos(turn), s = std::sin(turn);
        const float3 x = x0 * c + z0 * s, z = cross(x, up);
        const float lo = std::min(t.scatterScale[0], t.scatterScale[1]), hi = std::max(t.scatterScale[0], t.scatterScale[1]);
        const float k = std::max(lo + (hi - lo) * r.next(), 0.01f);
        const float place[3][4] = {{x.x * k, up.x * k, z.x * k, at.x}, {x.y * k, up.y * k, z.y * k, at.y}, {x.z * k, up.z * k, z.z * k, at.z}};
        const std::string& prefab = t.scatterPrefabs[std::min(size_t(r.next() * float(t.scatterPrefabs.size())), t.scatterPrefabs.size() - 1)];
        std::vector<std::pair<uint32_t, State>> after;
        std::string error;
        if (spawnPrefab(prefab, place, true, set, after, error) == ~0u) {
            notice(error.c_str(), "error");
            return;
        }
        execute(after);
        taken.push_back(at);
        for (const auto& [index, state] : after)
            skip.push_back(index);
    }
}

void EditorBridge::scatterErase(float3 point)
{
    // The set's pieces within the brush (by where they are) deleted.
    const uint32_t set = scatterSet(point, false);
    if (set == ~0u)
        return;
    const float radius = std::max(view_->tools().paintRadius, 0.01f);
    std::vector<uint32_t> gone;
    for (uint32_t k = 0; k < records_.size(); ++k)
        if (entityOf(k) && capture(k).alive && capture(k).parent == set) {
            const State s = capture(k);
            if (length(float3{s.world[0][3], s.world[1][3], s.world[2][3]} - point) <= radius)
                gone.push_back(k);
        }
    for (const uint32_t k : gone) {
        Json del = Json::parse("{\"type\":\"editor.entity.delete\",\"id\":" + std::to_string(k + 1) + "}");
        handle(del, CameraState{});
    }
    select(set);
}

}  // namespace fx
