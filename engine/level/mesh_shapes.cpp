// The shape tool's shapes (mesh_ops.h makeShape): centred on the origin, faces wound outward,
// curved sides in smoothing groups.

#include "level/mesh_ops.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace fx::level {

namespace {

constexpr float kPi = 3.14159265358979f;

float num(const Json& args, const char* key, float fallback, float lo = 1e-4f, float hi = 1e4f)
{
    return std::clamp(float(args.getNumber(key, fallback)), lo, hi);
}

uint32_t count(const Json& args, const char* key, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    return std::clamp(uint32_t(std::max(0.0, args.getNumber(key, fallback))), lo, hi);
}

float3 size3(const Json& args, float3 fallback)
{
    if (const Json* s = args.get("size"); s && s->array.size() == 3)
        return {std::max(float(s->array[0].number), 1e-4f), std::max(float(s->array[1].number), 1e-4f),
                std::max(float(s->array[2].number), 1e-4f)};
    return fallback;
}

// Rings of points (all the same count) joined in order by quads. The quad between ring edge k of
// two rings goes in groups[k]'s smoothing group. closedRing: each ring's last point joins its first;
// closedPath: the last ring joins the first; caps (open paths): the first and last rings as faces.
// The rings' first point's index.
uint32_t loft(PolyMesh& m, const std::vector<std::vector<float3>>& rings, bool closedRing, bool closedPath, bool caps,
              const std::vector<uint32_t>& groups = {})
{
    const uint32_t base = uint32_t(m.positions.size());
    const size_t n = rings[0].size();
    for (const auto& ring : rings)
        m.positions.insert(m.positions.end(), ring.begin(), ring.end());
    const auto at = [&](size_t i, size_t k) { return base + uint32_t(i * n + k % n); };
    const size_t spans = closedPath ? rings.size() : rings.size() - 1;
    for (size_t i = 0; i < spans; ++i) {
        const size_t j = (i + 1) % rings.size();
        for (size_t k = 0; k < (closedRing ? n : n - 1); ++k)
            m.addFace({at(i, k), at(i, k + 1), at(j, k + 1), at(j, k)}, 0, k < groups.size() ? groups[k] : 0);
    }
    if (caps && !closedPath) {
        std::vector<uint32_t> first, last;
        for (size_t k = 0; k < n; ++k) {
            first.push_back(at(0, n - 1 - k));
            last.push_back(at(rings.size() - 1, k));
        }
        m.addFace(first);
        m.addFace(last);
    }
    return base;
}

std::vector<float3> circle(float radius, float y, uint32_t sides)
{
    std::vector<float3> ring;
    for (uint32_t k = 0; k < sides; ++k) {
        const float a = 2.0f * kPi * float(k) / float(sides);
        ring.push_back({std::cos(a) * radius, y, std::sin(a) * radius});
    }
    return ring;
}

// Each closed piece (faces joined through shared vertices) turned inside out when its volume is
// negative: the generators wind each piece consistently, this makes it face out.
void orientOutward(PolyMesh& m)
{
    std::vector<uint32_t> parent(m.positions.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](uint32_t v) {
        while (parent[v] != v)
            v = parent[v] = parent[parent[v]];
        return v;
    };
    for (const Face& f : m.faces)
        for (uint32_t k = 1; k < f.count; ++k)
            parent[root(m.corners[f.first + k])] = root(m.corners[f.first]);
    std::vector<double> volume(m.positions.size(), 0.0);
    for (const Face& f : m.faces) {
        const float3 p0 = m.positions[m.corners[f.first]];
        for (uint32_t k = 1; k + 1 < f.count; ++k)
            volume[root(m.corners[f.first])] +=
                double(dot(p0, cross(m.positions[m.corners[f.first + k]], m.positions[m.corners[f.first + k + 1]])));
    }
    for (const Face& f : m.faces)
        if (volume[root(m.corners[f.first])] < 0.0) {
            std::reverse(m.corners.begin() + f.first, m.corners.begin() + f.first + f.count);
            if (!m.cornerUvs.empty())
                std::reverse(m.cornerUvs.begin() + f.first, m.cornerUvs.begin() + f.first + f.count);
        }
}

void centre(PolyMesh& m)
{
    float3 lo, hi;
    m.bounds(lo, hi);
    const float3 c = (lo + hi) * 0.5f;
    for (float3& p : m.positions)
        p = p - c;
}

PolyMesh sphere(float radius, uint32_t rings, uint32_t sides)
{
    PolyMesh m;
    m.materials = {""};
    std::vector<std::vector<float3>> bands;
    for (uint32_t i = 1; i < rings; ++i) {
        const float t = kPi * float(i) / float(rings);
        bands.push_back(circle(std::sin(t) * radius, std::cos(t) * radius, sides));
    }
    const uint32_t base = loft(m, bands, true, false, false, std::vector<uint32_t>(sides, 1));
    const uint32_t top = uint32_t(m.positions.size());
    m.positions.push_back({0, radius, 0});
    m.positions.push_back({0, -radius, 0});
    const uint32_t last = base + (rings - 2) * sides;
    for (uint32_t k = 0; k < sides; ++k) {
        const uint32_t n = (k + 1) % sides;
        m.addFace({top, base + n, base + k}, 0, 1);
        m.addFace({top + 1, last + k, last + n}, 0, 1);
    }
    return m;
}

}  // namespace

PolyMesh makeShape(const std::string& name, const Json& args, std::string* error)
{
    PolyMesh m;
    m.materials = {""};
    bool closed = true;
    if (name == "box") {
        return PolyMesh::box(size3(args, {1, 1, 1}));
    } else if (name == "plane") {
        return PolyMesh::plane(num(args, "width", 1), num(args, "depth", 1), count(args, "xSegments", 1, 1, 256),
                               count(args, "zSegments", 1, 1, 256));
    } else if (name == "cylinder") {
        const float r = num(args, "radius", 0.5f), h = num(args, "height", 1);
        const uint32_t sides = count(args, "sides", 16, 3, 256);
        loft(m, {circle(r, -h * 0.5f, sides), circle(r, h * 0.5f, sides)}, true, false, true, std::vector<uint32_t>(sides, 1));
    } else if (name == "cone") {
        const float r = num(args, "radius", 0.5f), h = num(args, "height", 1);
        const uint32_t sides = count(args, "sides", 16, 3, 256);
        m.positions = circle(r, -h * 0.5f, sides);
        m.positions.push_back({0, h * 0.5f, 0});
        std::vector<uint32_t> base;
        for (uint32_t k = 0; k < sides; ++k) {
            base.push_back(sides - 1 - k);
            m.addFace({k, (k + 1) % sides, sides}, 0, 1);
        }
        m.addFace(base);
    } else if (name == "sphere") {
        m = sphere(num(args, "radius", 0.5f), count(args, "rings", 8, 2, 128), count(args, "sides", 16, 3, 256));
    } else if (name == "torus") {
        const float r = num(args, "radius", 0.5f), tube = std::min(num(args, "tube", 0.15f), r * 0.99f);
        const uint32_t sides = count(args, "sides", 16, 3, 256), rings = count(args, "rings", 8, 3, 128);
        std::vector<std::vector<float3>> loops;
        for (uint32_t i = 0; i < sides; ++i) {
            const float a = 2.0f * kPi * float(i) / float(sides);
            std::vector<float3> ring;
            for (uint32_t k = 0; k < rings; ++k) {
                const float b = 2.0f * kPi * float(k) / float(rings);
                const float d = r + std::cos(b) * tube;
                ring.push_back({std::cos(a) * d, std::sin(b) * tube, std::sin(a) * d});
            }
            loops.push_back(ring);
        }
        loft(m, loops, true, true, false, std::vector<uint32_t>(rings, 1));
    } else if (name == "pipe") {
        const float r = num(args, "radius", 0.5f), h = num(args, "height", 1);
        const float inner = r - std::min(num(args, "thickness", 0.1f), r * 0.99f);
        const uint32_t sides = count(args, "sides", 16, 3, 256);
        std::vector<std::vector<float3>> sections;
        for (uint32_t i = 0; i < sides; ++i) {
            const float a = 2.0f * kPi * float(i) / float(sides), c = std::cos(a), s = std::sin(a);
            sections.push_back({{c * inner, -h * 0.5f, s * inner}, {c * r, -h * 0.5f, s * r}, {c * r, h * 0.5f, s * r},
                                {c * inner, h * 0.5f, s * inner}});
        }
        loft(m, sections, true, true, false, {0, 1, 0, 2});
    } else if (name == "prism") {
        const float3 s = size3(args, {1, 1, 1});
        const std::vector<float3> tri = {{-s.x * 0.5f, -s.y * 0.5f, 0}, {s.x * 0.5f, -s.y * 0.5f, 0}, {0, s.y * 0.5f, 0}};
        std::vector<float3> back = tri, front = tri;
        for (float3& p : back)
            p.z = -s.z * 0.5f;
        for (float3& p : front)
            p.z = s.z * 0.5f;
        loft(m, {back, front}, true, false, true);
    } else if (name == "arch") {
        // A rectangle (radial thickness x depth) swept over `angle` degrees about z, its middle at the top.
        const float r = num(args, "radius", 1), inner = r - std::min(num(args, "thickness", 0.25f), r * 0.99f);
        const float depth = num(args, "depth", 0.5f), angle = num(args, "angle", 180, 1, 360) * kPi / 180.0f;
        const uint32_t segments = count(args, "segments", 8, 1, 256);
        std::vector<std::vector<float3>> sections;
        for (uint32_t i = 0; i <= segments; ++i) {
            const float a = kPi * 0.5f - angle * 0.5f + angle * float(i) / float(segments), c = std::cos(a), s = std::sin(a);
            sections.push_back({{c * inner, s * inner, -depth * 0.5f}, {c * r, s * r, -depth * 0.5f}, {c * r, s * r, depth * 0.5f},
                                {c * inner, s * inner, depth * 0.5f}});
        }
        loft(m, sections, true, angle >= 2.0f * kPi - 1e-4f, angle < 2.0f * kPi - 1e-4f, {0, 1, 0, 2});
    } else if (name == "stairs") {
        // The side profile (rising toward +z), extruded across x: steps on top, solid underneath.
        const float3 s = size3(args, {1, 1, 2});
        const uint32_t steps = count(args, "steps", 6, 1, 256);
        std::vector<float2> profile = {{0, 0}, {s.z, 0}, {s.z, s.y}};
        for (uint32_t i = steps; i-- > 0;) {
            profile.push_back({s.z * float(i) / float(steps), s.y * float(i + 1) / float(steps)});
            if (i > 0)
                profile.push_back({s.z * float(i) / float(steps), s.y * float(i) / float(steps)});
        }
        std::vector<float3> left, right;
        for (float2 p : profile) {
            left.push_back({-s.x * 0.5f, p.y, p.x});
            right.push_back({s.x * 0.5f, p.y, p.x});
        }
        loft(m, {left, right}, true, false, true);
    } else if (name == "curvedStairs") {
        // Steps as wedges around y (each a closed piece), from the floor up to their tread.
        const float inner = num(args, "radius", 0.5f, 0.0f), width = num(args, "width", 1), height = num(args, "height", 2);
        const float angle = num(args, "angle", 90, 1, 360) * kPi / 180.0f;
        const uint32_t steps = count(args, "steps", 8, 1, 256);
        for (uint32_t i = 0; i < steps; ++i) {
            const float y = height * float(i + 1) / float(steps);
            std::vector<std::vector<float3>> sides;
            for (uint32_t e = 0; e < 2; ++e) {
                const float a = angle * float(i + e) / float(steps), c = std::cos(a), s = std::sin(a);
                const float ro = inner + width;
                sides.push_back({{c * inner, 0, s * inner}, {c * ro, 0, s * ro}, {c * ro, y, s * ro}, {c * inner, y, s * inner}});
            }
            loft(m, sides, true, false, true);
        }
    } else if (name == "polygon") {
        // An outline ([[x, z], ...]) on the ground, raised by `height` (0: one face, up).
        const Json* points = args.get("points");
        if (!points || points->array.size() < 3) {
            if (error)
                *error = "polygon: three points or more";
            return {};
        }
        const float h = std::max(float(args.getNumber("height", 1)), 0.0f);
        std::vector<float3> ground;
        for (const Json& p : points->array)
            if (p.array.size() >= 2)
                ground.push_back({float(p.array[0].number), 0, float(p.array[1].number)});
        if (h <= 0.0f) {
            m.positions = ground;
            std::vector<uint32_t> loop(ground.size());
            std::iota(loop.begin(), loop.end(), 0u);
            m.addFace(loop);
            if (m.faceNormal(0).y < 0)
                std::reverse(m.corners.begin(), m.corners.end());
            closed = false;
        } else {
            std::vector<float3> top = ground;
            for (float3& p : top)
                p.y = h;
            loft(m, {ground, top}, true, false, true);
        }
    } else {
        if (error)
            *error = "no shape '" + name + "'";
        return {};
    }
    if (closed)
        orientOutward(m);
    centre(m);
    return m;
}

}  // namespace fx::level
