#include "level/roofs.h"

#include "core/json.h"

#include <algorithm>
#include <cmath>

namespace fx::level {

namespace {

constexpr float kPi = 3.14159265358979f;

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

uint32_t vertex(PolyMesh& m, float3 p)
{
    m.positions.push_back(p);
    return uint32_t(m.positions.size() - 1);
}

// A face over existing vertices, turned to face away from `inside` (the sloped kinds are convex).
void outward(PolyMesh& m, std::vector<uint32_t> loop, uint32_t material, float3 inside)
{
    float3 normal = {}, centre = {};
    for (size_t k = 0; k < loop.size(); ++k) {
        const float3 a = m.positions[loop[k]], b = m.positions[loop[(k + 1) % loop.size()]];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
        centre = centre + a;
    }
    centre = centre * (1.0f / float(loop.size()));
    if (dot(normal, centre - inside) < 0.0f)
        std::reverse(loop.begin(), loop.end());
    const uint32_t f = m.addFace(loop, material);
    m.faces[f].uv.mode = UvMode::World;
}

// The outline counter-clockwise from above (its top face's normal up), without repeated points.
std::vector<float2> tidy(const std::vector<float2>& in)
{
    std::vector<float2> out;
    for (const float2 p : in)
        if (out.empty() || len(sub(p, out.back())) > 1e-4f)
            out.push_back(p);
    while (out.size() > 1 && len(sub(out.front(), out.back())) <= 1e-4f)
        out.pop_back();
    double area = 0.0;
    for (size_t k = 0; k < out.size(); ++k) {
        const float2 a = out[k], b = out[(k + 1) % out.size()];
        area += double(a.x) * b.y - double(b.x) * a.y;
    }
    if (area > 0.0)  // ((x, z) seen from above: a negative area is counter-clockwise there)
        std::reverse(out.begin(), out.end());
    return out;
}

// The outline grown by `by` (each corner along its two edges' outward normals: a mitre, kept short
// at sharp corners).
std::vector<float2> grow(const std::vector<float2>& outline, float by)
{
    const size_t n = outline.size();
    std::vector<float2> out(n);
    for (size_t k = 0; k < n; ++k) {
        const float2 prev = outline[(k + n - 1) % n], at = outline[k], next = outline[(k + 1) % n];
        const float2 n0 = leftOf(unit(sub(at, prev))), n1 = leftOf(unit(sub(next, at)));
        const float2 mid = unit({n0.x + n1.x, n0.y + n1.y});
        const float c = std::max(mid.x * n0.x + mid.y * n0.y, 0.25f);
        out[k] = {at.x + mid.x * by / c, at.y + mid.y * by / c};
    }
    return out;
}

}  // namespace

bool Roof::operator==(const Roof& o) const
{
    if (outline.size() != o.outline.size())
        return false;
    for (size_t k = 0; k < outline.size(); ++k)
        if (outline[k].x != o.outline[k].x || outline[k].y != o.outline[k].y)
            return false;
    return style == o.style && base == o.base && pitch == o.pitch && overhang == o.overhang && thickness == o.thickness &&
           material == o.material && under == o.under;
}

PolyMesh buildRoof(const Roof& roof)
{
    PolyMesh m;
    const std::vector<float2> outline = tidy(roof.outline);
    if (outline.size() < 3)
        return m;
    const uint32_t top = slotOf(m, roof.material), under = slotOf(m, roof.under);
    const float overhang = std::max(roof.overhang, 0.0f), base = roof.base;

    if (roof.style == RoofStyle::Flat) {
        // A slab over the outline grown by the overhang.
        const std::vector<float2> o = grow(outline, overhang);
        const float y0 = base, y1 = base + std::max(roof.thickness, 0.01f);
        const uint32_t first = uint32_t(m.positions.size()), n = uint32_t(o.size());
        for (const float2 p : o)
            m.positions.push_back({p.x, y0, p.y});
        for (const float2 p : o)
            m.positions.push_back({p.x, y1, p.y});
        std::vector<uint32_t> bottom, upper;
        for (uint32_t k = 0; k < n; ++k) {
            bottom.push_back(first + (n - 1 - k));
            upper.push_back(first + n + k);
            const uint32_t f = m.addFace({first + k, first + (k + 1) % n, first + n + (k + 1) % n, first + n + k}, under);
            m.faces[f].uv.mode = UvMode::World;
        }
        m.faces[m.addFace(upper, top)].uv.mode = UvMode::World;
        m.faces[m.addFace(bottom, under)].uv.mode = UvMode::World;
        return m;
    }

    // The sloped kinds: over the outline's bounding rectangle along its longest edge (u along it,
    // v across), grown by the overhang; closed underneath (a flat ceiling: cut volumes cut it).
    float2 along = {1, 0};
    float longest = -1.0f;
    for (size_t k = 0; k < outline.size(); ++k)
        if (const float2 d = sub(outline[(k + 1) % outline.size()], outline[k]); len(d) > longest) {
            longest = len(d);
            along = unit(d);
        }
    const float2 across = leftOf(along);
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (const float2 p : outline) {
        const float u = p.x * along.x + p.y * along.y, v = p.x * across.x + p.y * across.y;
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    u0 -= overhang, u1 += overhang, v0 -= overhang, v1 += overhang;
    const float length = u1 - u0, width = v1 - v0, slope = std::tan(std::clamp(roof.pitch, 1.0f, 80.0f) * kPi / 180.0f);
    const auto at = [&](float u, float v, float y) {
        return float3{along.x * u + across.x * v, y, along.y * u + across.y * v};
    };
    const uint32_t c00 = vertex(m, at(u0, v0, base)), c10 = vertex(m, at(u1, v0, base)), c11 = vertex(m, at(u1, v1, base)),
                   c01 = vertex(m, at(u0, v1, base));
    RoofStyle style = roof.style;
    if (style == RoofStyle::Hip && length <= width + 1e-4f)
        style = RoofStyle::Pyramid;  // (no ridge left)
    float rise = 0.0f;
    float3 inside = {};

    switch (style) {
    case RoofStyle::Shed: {  // one slope, up towards v1 (a wall at the high side)
        rise = slope * width;
        inside = at((u0 + u1) * 0.5f, v0 + width * 0.66f, base + rise * 0.33f);
        const uint32_t t0 = vertex(m, at(u0, v1, base + rise)), t1 = vertex(m, at(u1, v1, base + rise));
        outward(m, {c00, c10, t1, t0}, top, inside);
        outward(m, {c11, c01, t0, t1}, under, inside);
        outward(m, {c01, c00, t0}, under, inside);
        outward(m, {c10, c11, t1}, under, inside);
        break;
    }
    case RoofStyle::Gable: {  // two slopes up to a ridge along u, gable ends
        rise = slope * width * 0.5f;
        const float vm = (v0 + v1) * 0.5f;
        inside = at((u0 + u1) * 0.5f, vm, base + rise * 0.33f);
        const uint32_t r0 = vertex(m, at(u0, vm, base + rise)), r1 = vertex(m, at(u1, vm, base + rise));
        outward(m, {c00, c10, r1, r0}, top, inside);
        outward(m, {c11, c01, r0, r1}, top, inside);
        outward(m, {c01, c00, r0}, under, inside);
        outward(m, {c10, c11, r1}, under, inside);
        break;
    }
    case RoofStyle::Hip: {  // four slopes: the ridge shortened by half the width at each end
        rise = slope * width * 0.5f;
        const float vm = (v0 + v1) * 0.5f;
        inside = at((u0 + u1) * 0.5f, vm, base + rise * 0.33f);
        const uint32_t r0 = vertex(m, at(u0 + width * 0.5f, vm, base + rise)), r1 = vertex(m, at(u1 - width * 0.5f, vm, base + rise));
        outward(m, {c00, c10, r1, r0}, top, inside);
        outward(m, {c11, c01, r0, r1}, top, inside);
        outward(m, {c01, c00, r0}, top, inside);
        outward(m, {c10, c11, r1}, top, inside);
        break;
    }
    default: {  // pyramid: every side up to a point over the middle
        rise = slope * std::min(length, width) * 0.5f;
        inside = at((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, base + rise * 0.25f);
        const uint32_t apex = vertex(m, at((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, base + rise));
        outward(m, {c00, c10, apex}, top, inside);
        outward(m, {c10, c11, apex}, top, inside);
        outward(m, {c11, c01, apex}, top, inside);
        outward(m, {c01, c00, apex}, top, inside);
        break;
    }
    }
    outward(m, {c00, c01, c11, c10}, under, inside);  // the underside
    return m;
}

void readRoof(const Json& j, Roof& out)
{
    if (const Json* pts = j.get("outline")) {
        out.outline.clear();
        for (const Json& p : pts->array)
            if (p.array.size() >= 2)
                out.outline.push_back({float(p.array[0].number), float(p.array[1].number)});
    }
    const std::string style = j.getString("style", kRoofStyleNames[uint32_t(out.style) % kRoofStyles]);
    for (uint32_t k = 0; k < kRoofStyles; ++k)
        if (style == kRoofStyleNames[k])
            out.style = RoofStyle(k);
    out.base = float(j.getNumber("base", out.base));
    out.pitch = std::clamp(float(j.getNumber("pitch", out.pitch)), 1.0f, 80.0f);
    out.overhang = std::max(float(j.getNumber("overhang", out.overhang)), 0.0f);
    out.thickness = std::max(float(j.getNumber("thickness", out.thickness)), 0.01f);
    out.material = j.getString("material", out.material.c_str());
    out.under = j.getString("under", out.under.c_str());
}

void writeRoof(JsonWriter& w, const char* key, const Roof& roof)
{
    w.beginObject(key).beginArray("outline");
    for (const float2 p : roof.outline)
        w.beginArray().value(nullptr, double(p.x)).value(nullptr, double(p.y)).endArray();
    w.endArray().value("style", kRoofStyleNames[uint32_t(roof.style) % kRoofStyles]).value("base", double(roof.base))
        .value("pitch", double(roof.pitch)).value("overhang", double(roof.overhang)).value("thickness", double(roof.thickness))
        .value("material", roof.material).value("under", roof.under).endObject();
}

}  // namespace fx::level
