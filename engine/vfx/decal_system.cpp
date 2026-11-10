#include "vfx/decal_system.h"

#include "core/math.h"
#include "scene/scene.h"
#include "vfx/vfx_components.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

void inverse(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
    const float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                           {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                           {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
}

}  // namespace

bool DecalSystem::place(Scene& scene, const std::string& material, const float point[3], const float normal[3], float size, float depth,
                        float angle, float lifetime, flecs::entity attach)
{
    float3 up = {normal[0], normal[1], normal[2]};
    if (material.empty() || length(up) < 1e-6f || size <= 0.0f)
        return false;
    up = normalize(up);
    float3 x = normalize(cross(std::fabs(up.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}, up));
    float3 z = cross(x, up);
    const float a = angle * (kPi / 180.0f), c = std::cos(a), s = std::sin(a);
    const float3 xr = x * c + z * s, zr = z * c - x * s;
    x = xr * size;
    z = zr * size;
    const float3 y = up * std::max(depth, 1e-3f);
    Placed p;
    const float cols[4][3] = {{x.x, x.y, x.z}, {y.x, y.y, y.z}, {z.x, z.y, z.z}, {point[0], point[1], point[2]}};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 4; ++col)
            p.decal.world[r][col] = cols[col][r];
    p.decal.material = vfxAssetId(material);
    p.decal.flags = kDecalParallax;
    p.decal.angleFade = 0.35f;
    p.decal.edgeFade = 0.2f;
    p.decal.cavity = 0.6f;
    p.decal.parallaxDepth = size * 0.15f;
    p.decal.sequence = ++sequence_;
    p.lifetime = std::max(lifetime, 0.0f);
    // Attached: kept in the entity's space.
    if (attach && attach.is_alive()) {
        if (const WorldTransform* w = attach.try_get<WorldTransform>()) {
            float inv[3][4];
            inverse(reinterpret_cast<const float(*)[4]>(w->rows), inv);
            affineMultiply(inv, p.decal.world, p.local);
            p.attach = attach.id();
        }
    }
    (void)scene;
    if (placed_.size() >= kMaxPlaced)
        placed_.erase(placed_.begin());  // (the oldest)
    placed_.push_back(p);
    return true;
}

void DecalSystem::update(Scene& scene, float dt)
{
    flecs::world& world = scene.world();
    for (Placed& p : placed_) {
        p.age += dt;
        if (p.attach) {
            const flecs::entity e(world, p.attach);
            const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr;
            if (!w) {
                p.lifetime = std::max(p.age, 1e-3f);  // (what it was on is gone: so is it)
                continue;
            }
            affineMultiply(reinterpret_cast<const float(*)[4]>(w->rows), p.local, p.decal.world);
        }
        // The last second of its life: fading out.
        if (p.lifetime > 0.0f)
            p.decal.tint[3] = std::clamp(p.lifetime - p.age, 0.0f, 1.0f);
    }
    placed_.erase(std::remove_if(placed_.begin(), placed_.end(), [](const Placed& p) { return p.lifetime > 0.0f && p.age >= p.lifetime; }),
                  placed_.end());
}

void DecalSystem::gather(Scene& scene, std::vector<DecalInstance>& out) const
{
    out.clear();
    scene.world().each([&](flecs::entity, const Decal& d, const WorldTransform& w) {
        DecalInstance di;
        // The box: the entity's axes times the decal's size (x, z across; y its depth), at its origin.
        for (int r = 0; r < 3; ++r) {
            di.world[r][0] = w.rows[r * 4 + 0] * d.size[0];
            di.world[r][1] = w.rows[r * 4 + 1] * d.size[1];
            di.world[r][2] = w.rows[r * 4 + 2] * d.size[2];
            di.world[r][3] = w.rows[r * 4 + 3];
        }
        di.material = d.material;
        di.flags = d.flags;
        std::memcpy(di.tint, d.tint, sizeof(di.tint));
        di.tint[3] *= d.opacity;
        di.angleFade = d.angleFade;
        di.edgeFade = d.edgeFade;
        di.cavity = d.cavity;
        di.parallaxDepth = d.parallaxDepth;
        di.steps = d.parallaxSteps;
        di.order = d.order;
        out.push_back(di);
    });
    for (const Placed& p : placed_)
        out.push_back(p.decal);
}

}  // namespace fx
