#include "level/walls.h"

#include <algorithm>
#include <cmath>

namespace fx::level {

namespace {

constexpr float kFloorLift = 0.005f;  // a room's floor top above its base (metres; see its slab below)

float2 sub(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
float len(float2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
float2 unit(float2 a)
{
    const float l = len(a);
    return l > 1e-9f ? float2{a.x / l, a.y / l} : float2{1, 0};
}
float2 leftOf(float2 d) { return {-d.y, d.x}; }  // (x, z): a quarter turn

uint32_t slotOf(PolyMesh& m, const std::string& material)
{
    const auto it = std::find(m.materials.begin(), m.materials.end(), material);
    if (it != m.materials.end())
        return uint32_t(it - m.materials.begin());
    m.materials.push_back(material);
    return uint32_t(m.materials.size() - 1);
}

uint32_t face(PolyMesh& m, std::vector<uint32_t> loop, uint32_t material)
{
    const uint32_t f = m.addFace(loop, material);
    m.faces[f].uv.mode = UvMode::World;
    return f;
}

double volumeOf(const PolyMesh& m)
{
    double v = 0.0;
    for (const Face& f : m.faces) {
        const float3 p0 = m.positions[m.corners[f.first]];
        for (uint32_t k = 1; k + 1 < f.count; ++k)
            v += double(dot(p0, cross(m.positions[m.corners[f.first + k]], m.positions[m.corners[f.first + k + 1]])));
    }
    return v;
}

// A slab: the outline (x, z) between heights y0 and y1, closed, its top `topMaterial`.
void slab(PolyMesh& m, const std::vector<float2>& outline, float y0, float y1, uint32_t topMaterial, uint32_t sideMaterial)
{
    const uint32_t base = uint32_t(m.positions.size()), n = uint32_t(outline.size());
    for (const float2 p : outline)
        m.positions.push_back({p.x, y0, p.y});
    for (const float2 p : outline)
        m.positions.push_back({p.x, y1, p.y});
    std::vector<uint32_t> bottom, top;
    for (uint32_t k = 0; k < n; ++k) {
        bottom.push_back(base + (n - 1 - k));
        top.push_back(base + n + k);
        const uint32_t a = k, b = (k + 1) % n;
        face(m, {base + a, base + b, base + n + b, base + n + a}, sideMaterial);
    }
    face(m, bottom, sideMaterial);
    face(m, top, topMaterial);
    // (wound alike; outward checked by the caller per slab)
}

}  // namespace

bool Wall::operator==(const Wall& o) const
{
    if (points.size() != o.points.size())
        return false;
    for (size_t k = 0; k < points.size(); ++k)
        if (points[k].x != o.points[k].x || points[k].y != o.points[k].y)
            return false;
    return closed == o.closed && height == o.height && thickness == o.thickness && align == o.align && floor == o.floor &&
           ceiling == o.ceiling && slab == o.slab && sideA == o.sideA && sideB == o.sideB && top == o.top && floorMaterial == o.floorMaterial &&
           ceilingMaterial == o.ceilingMaterial;
}

PolyMesh buildWall(const Wall& wall)
{
    PolyMesh m;
    m.materials = {wall.sideA};
    std::vector<float2> pts;
    for (const float2 p : wall.points)  // (repeats dropped)
        if (pts.empty() || len(sub(p, pts.back())) > 1e-4f)
            pts.push_back(p);
    const bool closed = wall.closed && pts.size() >= 3 && len(sub(pts.front(), pts.back())) > 1e-4f;
    if (pts.size() < 2)
        return m;
    const uint32_t n = uint32_t(pts.size()), segments = closed ? n : n - 1;
    const float t = std::max(wall.thickness, 1e-3f), h = std::max(wall.height, 1e-3f);
    const float dl = wall.align == 1 ? 0.0f : wall.align == 2 ? t : t * 0.5f, dr = t - dl;
    const uint32_t matA = slotOf(m, wall.sideA), matB = slotOf(m, wall.sideB), matTop = slotOf(m, wall.top);
    // Each point's left and right (mitred where segments meet), at the bottom and the top.
    std::vector<float2> left(n), right(n);
    for (uint32_t i = 0; i < n; ++i) {
        const bool first = i == 0 && !closed, last = i == n - 1 && !closed;
        const float2 dIn = first ? unit(sub(pts[1], pts[0])) : unit(sub(pts[i], pts[(i + n - 1) % n]));
        const float2 dOut = last ? dIn : unit(sub(pts[(i + 1) % n], pts[i]));
        const float2 nIn = leftOf(dIn), nOut = leftOf(dOut);
        float2 miter = unit({nIn.x + nOut.x, nIn.y + nOut.y});
        const float c = miter.x * nOut.x + miter.y * nOut.y;
        const float scale = 1.0f / std::max(c, 0.25f);  // (a sharp corner: the miter clipped at 4x)
        left[i] = {pts[i].x + miter.x * dl * scale, pts[i].y + miter.y * dl * scale};
        right[i] = {pts[i].x - miter.x * dr * scale, pts[i].y - miter.y * dr * scale};
    }
    // Vertices: per point left bottom, left top, right bottom, right top.
    for (uint32_t i = 0; i < n; ++i) {
        m.positions.push_back({left[i].x, 0.0f, left[i].y});
        m.positions.push_back({left[i].x, h, left[i].y});
        m.positions.push_back({right[i].x, 0.0f, right[i].y});
        m.positions.push_back({right[i].x, h, right[i].y});
    }
    const auto Lb = [](uint32_t i) { return i * 4; };
    const auto Lt = [](uint32_t i) { return i * 4 + 1; };
    const auto Rb = [](uint32_t i) { return i * 4 + 2; };
    const auto Rt = [](uint32_t i) { return i * 4 + 3; };
    for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t i = s, j = (s + 1) % n;
        face(m, {Lb(i), Lb(j), Lt(j), Lt(i)}, matA);
        face(m, {Rb(j), Rb(i), Rt(i), Rt(j)}, matB);
        face(m, {Lt(i), Lt(j), Rt(j), Rt(i)}, matTop);
        face(m, {Lb(i), Rb(i), Rb(j), Lb(j)}, matTop);
    }
    if (!closed) {
        face(m, {Lb(0), Lt(0), Rt(0), Rb(0)}, matTop);
        face(m, {Rb(n - 1), Rt(n - 1), Lt(n - 1), Lb(n - 1)}, matTop);
    }
    // Wound alike; facing out (the path's turn decides which way that is).
    if (volumeOf(m) < 0.0)
        for (const Face& f : m.faces)
            std::reverse(m.corners.begin() + f.first, m.corners.begin() + f.first + f.count);
    // A room's floor and ceiling: slabs over the inside (the smaller of its two outlines).
    if (closed && (wall.floor || wall.ceiling)) {
        const auto area = [](const std::vector<float2>& o) {
            double a = 0.0;
            for (size_t k = 0; k < o.size(); ++k)
                a += double(o[k].x) * o[(k + 1) % o.size()].y - double(o[(k + 1) % o.size()].x) * o[k].y;
            return std::fabs(a) * 0.5;
        };
        const std::vector<float2>& inside = area(left) < area(right) ? left : right;
        for (int which = 0; which < 2; ++which) {
            if (which == 0 ? !wall.floor : !wall.ceiling)
                continue;
            PolyMesh s;
            s.materials = m.materials;
            const uint32_t topSlot = slotOf(s, which == 0 ? wall.floorMaterial : wall.ceilingMaterial);
            // The floor's top a little above the room's base: at 0 it lay in the plane of the
            // ground the room stands on, and the two fought for the depth test, the floor
            // flickering between them as the camera moved (the walls still start at 0: no gap).
            const float y0 = which == 0 ? -wall.slab : h, y1 = which == 0 ? kFloorLift : h + wall.slab;
            slab(s, inside, y0, y1, which == 0 ? topSlot : matTop, which == 0 ? matTop : topSlot);
            if (volumeOf(s) < 0.0)
                for (const Face& f : s.faces)
                    std::reverse(s.corners.begin() + f.first, s.corners.begin() + f.first + f.count);
            if (which == 1)  // (a ceiling's material faces down, into the room)
                for (Face& f : s.faces)
                    if (s.faceNormal(uint32_t(&f - s.faces.data())).y < -0.9f)
                        f.material = topSlot;
                    else if (f.material == topSlot)
                        f.material = matTop;
            // Appended to the wall's mesh (its own piece).
            const uint32_t base = uint32_t(m.positions.size());
            m.positions.insert(m.positions.end(), s.positions.begin(), s.positions.end());
            m.materials = s.materials;
            for (const Face& f : s.faces) {
                std::vector<uint32_t> loop;
                for (uint32_t k = 0; k < f.count; ++k)
                    loop.push_back(base + s.corners[f.first + k]);
                face(m, loop, f.material);
            }
        }
    }
    return m;
}

float wallLength(const Wall& wall)
{
    const size_t n = wall.points.size();
    float total = 0.0f;
    for (size_t k = 0; k + 1 < n; ++k)
        total += len(sub(wall.points[k + 1], wall.points[k]));
    if (wall.closed && n >= 3)
        total += len(sub(wall.points.front(), wall.points.back()));
    return total;
}

bool wallPathAt(const Wall& wall, float distance, float2& point, float2& direction)
{
    const float total = wallLength(wall);
    const size_t n = wall.points.size();
    if (total <= 1e-6f || n < 2)
        return false;
    float d = wall.closed ? std::fmod(std::fmod(distance, total) + total, total) : std::clamp(distance, 0.0f, total);
    const size_t segments = wall.closed && n >= 3 ? n : n - 1;
    for (size_t s = 0; s < segments; ++s) {
        const float2 a = wall.points[s], b = wall.points[(s + 1) % n];
        const float l = len(sub(b, a));
        if (d <= l || s + 1 == segments) {
            const float k = l > 1e-9f ? std::clamp(d / l, 0.0f, 1.0f) : 0.0f;
            point = {a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k};
            direction = unit(sub(b, a));
            return true;
        }
        d -= l;
    }
    return false;
}

float wallNearest(const Wall& wall, float2 p)
{
    const size_t n = wall.points.size();
    const size_t segments = wall.closed && n >= 3 ? n : (n >= 1 ? n - 1 : 0);
    float best = 1e30f, at = 0.0f, walked = 0.0f;
    for (size_t s = 0; s < segments; ++s) {
        const float2 a = wall.points[s], b = wall.points[(s + 1) % n], ab = sub(b, a);
        const float l2 = ab.x * ab.x + ab.y * ab.y, l = std::sqrt(l2);
        const float k = l2 > 1e-12f ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
        const float2 q = {a.x + ab.x * k, a.y + ab.y * k};
        const float d = len(sub(p, q));
        if (d < best) {
            best = d;
            at = walked + l * k;
        }
        walked += l;
    }
    return at;
}

}  // namespace fx::level
