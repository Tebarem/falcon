#include "level/wear.h"

#include "core/json.h"
#include "level/level_mask.h"
#include "level/mesh_ops.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fx::level {

namespace {

float smooth(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(z) * 2246822519u + seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

float segmentDistance(float3 p, float3 a, float3 b)
{
    const float3 ab = b - a;
    const float l2 = dot(ab, ab);
    const float t = l2 > 1e-12f ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

}  // namespace

float valueNoise(float3 p, float cell, uint32_t seed)
{
    const float fx = p.x / cell, fy = p.y / cell, fz = p.z / cell;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy)), z0 = int(std::floor(fz));
    const float tx = fx - float(x0), ty = fy - float(y0), tz = fz - float(z0);
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty), sz = tz * tz * (3 - 2 * tz);
    float v = 0.0f;
    for (int k = 0; k < 8; ++k) {
        const int dx = k & 1, dy = k >> 1 & 1, dz = k >> 2 & 1;
        v += hash3(x0 + dx, y0 + dy, z0 + dz, seed) * (dx ? sx : 1 - sx) * (dy ? sy : 1 - sy) * (dz ? sz : 1 - sz);
    }
    return v;
}

float3 closestOnTriangle(float3 p, float3 a, float3 b, float3 c)  // (Ericson, Real-Time Collision Detection 5.1.5)
{
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;
    const float3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));
    const float3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float sum = va + vb + vc;
    return sum > 1e-20f ? a + ab * (vb / sum) + ac * (vc / sum) : a;
}

bool generateWear(const PolyMesh& mesh, const WearSettings& s, std::vector<uint8_t>& mask, const std::vector<float3>* nearby)
{
    if (mesh.cornerUv1.size() != mesh.corners.size() || mesh.faces.empty())
        return false;
    if (mask.size() != kMaskBytes)
        mask.assign(kMaskBytes, 0);
    const uint32_t channel = std::min(s.channel, 3u);
    const float width = std::max(s.width, 1e-4f);
    // The edges that wear (convex and sharp; open ones too), gather grime (concave and sharp) or
    // start streaks (convex and sharp, along a face looking up: a ledge's lip).
    std::unordered_map<uint64_t, std::vector<uint32_t>> facesOf;
    for (uint32_t f = 0; f < mesh.faces.size(); ++f)
        for (uint32_t k = 0; k < mesh.faces[f].count; ++k)
            facesOf[edgeKey(mesh.corners[mesh.faces[f].first + k], mesh.corners[mesh.faces[f].first + (k + 1) % mesh.faces[f].count])].push_back(f);
    const float cosSharp = std::cos(s.sharp * 3.14159265f / 180.0f);
    float minY = 1e30f;
    for (const float3& p : mesh.positions)
        minY = std::min(minY, p.y);
    struct Segment {
        float3 a, b, lo, hi;
    };
    std::vector<Segment> wanted;
    // Edges that turn less (a round bevel's, a curve's): their turn spread over the faces beside them
    // (half each), counted where the surface turns by about `sharp` within `width` (convex for
    // wear, concave for grime).
    struct Soft {
        float3 a, b, lo, hi, normal;
        float turn, half;  // (degrees; metres across)
    };
    std::vector<Soft> soft;
    const auto lineDistance = [](float3 p, float3 a, float3 b) {
        const float3 ab = b - a;
        const float l2 = dot(ab, ab);
        return length(p - (l2 > 1e-12f ? a + ab * (dot(p - a, ab) / l2) : a));
    };
    // (faces without mask UVs of their own, a cut's broken ones: nothing drawn on them)
    std::vector<uint8_t> drawn(mesh.faces.size(), 0);
    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        for (uint32_t k = 1; k < face.count && !drawn[f]; ++k)
            drawn[f] = mesh.cornerUv1[face.first + k].x != mesh.cornerUv1[face.first].x || mesh.cornerUv1[face.first + k].y != mesh.cornerUv1[face.first].y;
    }
    for (const auto& [key, users] : facesOf) {
        if (users.size() == 2 && s.kind != WearKind::Streaks && drawn[users[0]] && drawn[users[1]]) {
            const uint32_t f = users[0], g = users[1];
            const float3 nf = mesh.faceNormal(f), ng = mesh.faceNormal(g);
            const float c = std::clamp(dot(nf, ng), -1.0f, 1.0f);
            const float turn = std::acos(c) * 180.0f / 3.14159265f;
            const bool convex = dot(mesh.faceCenter(g) - mesh.faceCenter(f), nf) < 0.0f;
            if (c > cosSharp && turn > 1.0f && convex == (s.kind == WearKind::Edges)) {
                const float3 a = mesh.positions[uint32_t(key >> 32)], b = mesh.positions[uint32_t(key)];
                const float half = std::max(0.5f * (lineDistance(mesh.faceCenter(f), a, b) + lineDistance(mesh.faceCenter(g), a, b)), 1e-4f);
                const float3 sum = nf + ng;
                soft.push_back({a, b, {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
                                {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}, length(sum) > 1e-6f ? normalize(sum) : nf, turn, half});
            }
        }
        bool want = users.size() == 1 && s.kind == WearKind::Edges;  // (an open edge: worn)
        for (size_t i = 0; i < users.size() && !want; ++i)
            for (size_t j = 0; j < users.size() && !want; ++j) {
                const uint32_t f = users[i], g = users[j];
                const float3 nf = mesh.faceNormal(f), ng = mesh.faceNormal(g);
                if (f == g || dot(nf, ng) > cosSharp)
                    continue;
                const bool convex = dot(mesh.faceCenter(g) - mesh.faceCenter(f), nf) < 0.0f;
                want = s.kind == WearKind::Edges ? convex : s.kind == WearKind::Grime ? !convex : convex && ng.y > 0.5f && std::fabs(nf.y) < 0.75f;
            }
        if (!want)
            continue;
        const float3 a = mesh.positions[uint32_t(key >> 32)], b = mesh.positions[uint32_t(key)];
        wanted.push_back({a, b, {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}, {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}});
    }
    // Nearby surfaces' triangles' bounds (grime where they come near).
    const std::vector<float3> none;
    const std::vector<float3>& around = nearby && s.kind == WearKind::Grime ? *nearby : none;
    std::vector<float3> aroundLo(around.size() / 3), aroundHi(around.size() / 3);
    for (size_t t = 0; t < around.size() / 3; ++t) {
        const float3 *p = &around[t * 3];
        aroundLo[t] = {std::min({p[0].x, p[1].x, p[2].x}), std::min({p[0].y, p[1].y, p[2].y}), std::min({p[0].z, p[1].z, p[2].z})};
        aroundHi[t] = {std::max({p[0].x, p[1].x, p[2].x}), std::max({p[0].y, p[1].y, p[2].y}), std::max({p[0].z, p[1].z, p[2].z})};
    }
    // Each face's triangles drawn into the mask in its mask UVs; each texel's value from its point.
    const float size = float(kMaskSize);
    std::vector<uint8_t> written(size_t(kMaskSize) * kMaskSize, 0);
    std::vector<const Segment*> segments;
    std::vector<const Soft*> turning;
    std::vector<uint32_t> close;
    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        if (!drawn[f])
            continue;
        const Face& face = mesh.faces[f];
        const float3 nf = mesh.faceNormal(f), cf = mesh.faceCenter(f);
        const bool side = std::fabs(nf.y) < 0.75f;  // (streaks run down the sides only)
        float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (uint32_t k = 0; k < face.count; ++k) {
            const float3 p = mesh.positions[mesh.corners[face.first + k]];
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        // The wanted edges in its plane (its own, and those of the faces flat with it: a cut mesh's
        // triangles) that can reach it; the nearby triangles that can.
        segments.clear();
        for (const Segment& g : wanted) {
            if (s.kind == WearKind::Streaks && !side)
                break;
            if (std::fabs(dot(g.a - cf, nf)) > 2e-3f || std::fabs(dot(g.b - cf, nf)) > 2e-3f)
                continue;
            const float r = s.kind == WearKind::Streaks ? 0.03f : width;
            if (g.lo.x > hi.x + r || g.hi.x < lo.x - r || g.lo.z > hi.z + r || g.hi.z < lo.z - r)
                continue;
            if (s.kind == WearKind::Streaks ? g.hi.y < lo.y || g.lo.y > hi.y + width : g.lo.y > hi.y + r || g.hi.y < lo.y - r)
                continue;
            segments.push_back(&g);
        }
        turning.clear();
        for (const Soft& g : soft) {
            const float r = width + g.half;
            if (dot(g.normal, nf) <= 0.0f || g.lo.x > hi.x + r || g.hi.x < lo.x - r || g.lo.y > hi.y + r || g.hi.y < lo.y - r ||
                g.lo.z > hi.z + r || g.hi.z < lo.z - r)
                continue;
            turning.push_back(&g);
        }
        close.clear();
        for (uint32_t t = 0; t < aroundLo.size(); ++t)
            if (aroundLo[t].x <= hi.x + width && aroundHi[t].x >= lo.x - width && aroundLo[t].y <= hi.y + width &&
                aroundHi[t].y >= lo.y - width && aroundLo[t].z <= hi.z + width && aroundHi[t].z >= lo.z - width)
                close.push_back(t);
        for (uint32_t k = 1; k + 1 < face.count; ++k) {
            const uint32_t c[3] = {face.first, face.first + k, face.first + k + 1};
            float2 t[3];
            float3 p[3];
            for (int i = 0; i < 3; ++i) {
                t[i] = {mesh.cornerUv1[c[i]].x * size, mesh.cornerUv1[c[i]].y * size};
                p[i] = mesh.positions[mesh.corners[c[i]]];
            }
            const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
            if (std::fabs(area) < 1e-9f)
                continue;
            const int x0 = std::max(int(std::floor(std::min({t[0].x, t[1].x, t[2].x}))), 0);
            const int x1 = std::min(int(std::ceil(std::max({t[0].x, t[1].x, t[2].x}))), int(kMaskSize) - 1);
            const int y0 = std::max(int(std::floor(std::min({t[0].y, t[1].y, t[2].y}))), 0);
            const int y1 = std::min(int(std::ceil(std::max({t[0].y, t[1].y, t[2].y}))), int(kMaskSize) - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                    const float w1 = ((px - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (py - t[0].y)) / area;
                    const float w2 = ((t[1].x - t[0].x) * (py - t[0].y) - (px - t[0].x) * (t[1].y - t[0].y)) / area;
                    const float w0 = 1.0f - w1 - w2;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                        continue;
                    const float3 at = p[0] * w0 + p[1] * w1 + p[2] * w2;
                    float v = 0.0f;
                    if (s.kind == WearKind::Streaks) {
                        // Below a ledge's lip (straight down, within `width`): thin runs, longer near it.
                        float reach = 0.0f;
                        for (const Segment* g : segments) {
                            const float2 ab = {g->b.x - g->a.x, g->b.z - g->a.z};
                            const float l2 = ab.x * ab.x + ab.y * ab.y;
                            if (l2 < 1e-8f)
                                continue;  // (an upright edge)
                            const float len = std::sqrt(l2), along = ((at.x - g->a.x) * ab.x + (at.z - g->a.z) * ab.y) / len;
                            if (along < -0.02f || along > len + 0.02f)
                                continue;
                            const float3 q = g->a + (g->b - g->a) * std::clamp(along / len, 0.0f, 1.0f);
                            const float drop = q.y - at.y, off = std::hypot(at.x - q.x, at.z - q.z);
                            if (drop <= 0.0f || drop >= width || off > 0.03f)
                                continue;
                            reach = std::max(reach, (1.0f - drop / width) * smooth(-0.02f, 0.04f, std::min(along, len - along)));
                        }
                        if (reach > 0.0f) {
                            const float n = valueNoise({at.x, at.y * 0.08f, at.z}, 0.03f, s.seed) * 0.6f +
                                            valueNoise({at.x, at.y * 0.25f, at.z}, 0.1f, s.seed + 5) * 0.4f;
                            v = smooth(0.3f, 0.65f, n * (0.35f + 0.85f * reach)) * smooth(0.0f, 0.25f, reach);
                        }
                    } else {
                        float d = 1e30f;
                        for (const Segment* g : segments)
                            d = std::min(d, segmentDistance(at, g->a, g->b));
                        float base = d < width ? 1.0f - smooth(0.0f, width, d) : 0.0f;
                        if (!turning.empty()) {  // (how far the surface turns within `width` of here)
                            float turn = 0.0f;
                            for (const Soft* g : turning) {
                                const float e = segmentDistance(at, g->a, g->b);
                                const float overlap = std::min(g->half, e + width) - std::max(-g->half, e - width);
                                if (overlap > 0.0f)
                                    turn += g->turn * std::min(overlap / (2.0f * g->half), 1.0f);
                            }
                            base = std::max(base, smooth(0.5f * s.sharp, s.sharp, turn));
                        }
                        if (s.kind == WearKind::Grime && s.foot > 0.0f)
                            base = std::max(base, 1.0f - smooth(0.0f, s.foot, at.y - minY));
                        if (!close.empty()) {  // (where other surfaces come near)
                            float dn = 1e30f;
                            for (const uint32_t tri : close) {
                                const float3* q = &around[size_t(tri) * 3];
                                dn = std::min(dn, length(at - closestOnTriangle(at, q[0], q[1], q[2])));
                            }
                            base = std::max(base, 1.0f - smooth(0.0f, width, dn));
                        }
                        const float n = base > 0.0f ? valueNoise(at, 0.12f, s.seed) * 0.65f + valueNoise(at, 0.035f, s.seed + 7) * 0.35f : 0.0f;
                        v = s.kind == WearKind::Edges ? smooth(0.35f, 0.65f, base * (0.55f + 0.9f * n)) : base * (0.45f + 0.55f * n);
                    }
                    uint8_t& out = mask[(size_t(y) * kMaskSize + size_t(x)) * 4 + channel];
                    out = std::max(out, uint8_t(std::lround(std::clamp(v * s.strength, 0.0f, 1.0f) * 255.0f)));
                    written[size_t(y) * kMaskSize + size_t(x)] = 1;
                }
        }
    }
    // Two texels out from the charts (their edges sample a little past them).
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint8_t> next = written;
        for (int y = 0; y < int(kMaskSize); ++y)
            for (int x = 0; x < int(kMaskSize); ++x) {
                const size_t i = size_t(y) * kMaskSize + size_t(x);
                if (written[i])
                    continue;
                for (int dy = -1; dy <= 1 && !next[i]; ++dy)
                    for (int dx = -1; dx <= 1 && !next[i]; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= int(kMaskSize) || ny >= int(kMaskSize))
                            continue;
                        const size_t j = size_t(ny) * kMaskSize + size_t(nx);
                        if (written[j]) {
                            mask[i * 4 + channel] = std::max(mask[i * 4 + channel], mask[j * 4 + channel]);
                            next[i] = 1;
                        }
                    }
            }
        written = std::move(next);
    }
    return true;
}

WearSettings wearFromJson(const Json& j, WearKind kind)
{
    const std::string name = j.getString("kind");
    for (uint32_t k = 0; k < kWearKinds; ++k)
        if (name == kWearKindNames[k])
            kind = WearKind(k);
    WearSettings w;
    w.kind = kind;
    w.channel = uint32_t(std::clamp(j.getNumber("layer", kind == WearKind::Edges ? 0 : 1), 0.0, 2.0));
    w.width = float(j.getNumber("width", kind == WearKind::Edges ? 0.08 : kind == WearKind::Grime ? 0.15 : 1.2));
    w.strength = float(j.getNumber("strength", 1.0));
    w.foot = float(j.getNumber("foot", kind == WearKind::Grime ? 0.3 : 0.0));
    w.seed = uint32_t(j.getNumber("seed", 1));
    return w;
}

void writeWearJson(JsonWriter& w, const char* key, const WearSettings& s)
{
    w.beginObject(key).value("kind", kWearKindNames[uint32_t(s.kind) % kWearKinds]).value("layer", s.channel)
        .value("width", double(s.width)).value("strength", double(s.strength));
    if (s.kind == WearKind::Grime)
        w.value("foot", double(s.foot));
    if (s.seed != 1)
        w.value("seed", s.seed);
    w.endObject();
}

bool damageSpot(const PolyMesh& mesh, uint32_t f, float3 p, DamageSnap snap, float3& out)
{
    out = p;
    if (snap == DamageSnap::Hole)
        return true;
    if (f >= mesh.faces.size())
        return false;
    // The face's convex sharp edges (an open edge counts): the nearest point on one, or its nearer end.
    // A face with none (on a round bevel): its convex edges that turn at all.
    const Face& face = mesh.faces[f];
    const float3 nf = mesh.faceNormal(f), cf = mesh.faceCenter(f);
    const float cosSharp = std::cos(30.0f * 3.14159265f / 180.0f), cosTurn = std::cos(1.0f * 3.14159265f / 180.0f);
    float best = 1e30f;
    for (int pass = 0; pass < 2 && best == 1e30f; ++pass)
    for (uint32_t k = 0; k < face.count; ++k) {
        const uint32_t a = mesh.corners[face.first + k], b = mesh.corners[face.first + (k + 1) % face.count];
        bool sharp = pass == 0;
        for (uint32_t g = 0; g < mesh.faces.size(); ++g) {
            if (g == f)
                continue;
            const Face& o = mesh.faces[g];
            for (uint32_t i = 0; i < o.count; ++i)
                if (mesh.corners[o.first + i] == b && mesh.corners[o.first + (i + 1) % o.count] == a)
                    sharp = dot(nf, mesh.faceNormal(g)) <= (pass == 0 ? cosSharp : cosTurn) && dot(mesh.faceCenter(g) - cf, nf) < 0.0f;
        }
        if (!sharp)
            continue;
        const float3 pa = mesh.positions[a], pb = mesh.positions[b], ab = pb - pa;
        const float l2 = dot(ab, ab);
        const float t = l2 > 1e-12f ? std::clamp(dot(p - pa, ab) / l2, 0.0f, 1.0f) : 0.0f;
        const float3 at = snap == DamageSnap::Chip ? pa + ab * t : t < 0.5f ? pa : pb;
        if (const float d = length(p - at); d < best)
            best = d, out = at;
    }
    return best < 1e30f;
}

std::vector<DamageSpotAt> damageSpots(const PolyMesh& source, const float w[3][4], uint32_t mode, const std::vector<uint64_t>& items,
                                      float spacing, uint32_t seed, float randomness, float density, uint32_t limit)
{
    std::vector<DamageSpotAt> out;
    if (source.faces.empty() || mode > 3)
        return out;
    spacing = std::max(spacing, 0.01f);
    randomness = std::clamp(randomness, 0.0f, 1.0f);
    density = std::clamp(density, 0.0f, 1.0f);
    // (each place in turn: whether it gets one, its size; how far a chip strays along its edge)
    uint32_t places = 0, strays = 0;
    const auto stray = [&]() { return randomness > 0.0f ? randomness * (hash3(int(strays++), 103, 7, seed) - 0.5f) : 0.0f; };
    // In the world (its normals and spacing as they are drawn, scaled or not).
    PolyMesh mesh = source;
    for (float3& p : mesh.positions)
        p = {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
             w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
    const auto key = [](uint32_t a, uint32_t b) { return uint64_t(std::min(a, b)) << 32 | std::max(a, b); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> facesOf;
    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        for (uint32_t k = 0; k < face.count; ++k)
            facesOf[key(mesh.corners[face.first + k], mesh.corners[face.first + (k + 1) % face.count])].push_back(f);
    }
    const auto add = [&](float3 at, float3 normal, DamageSnap kind, uint32_t face) {
        const uint32_t place = places++;
        if (density < 1.0f && hash3(int(place), 101, 7, seed) >= density)
            return;
        const float scale = 1.0f + randomness * (hash3(int(place), 102, 7, seed) - 0.5f);
        if (out.size() < limit && length(normal) > 1e-6f)
            out.push_back({at, normalize(normal), kind, face, scale});
    };
    // An edge: chips along it, out between its faces.
    const float cosSharp = std::cos(30.0f * 3.14159265f / 180.0f);
    const auto edge = [&](uint64_t k, bool onlySharp) {
        const auto found = facesOf.find(k);
        if (found == facesOf.end())
            return;
        const std::vector<uint32_t>& fs = found->second;
        const float3 n0 = mesh.faceNormal(fs[0]);
        if (onlySharp && fs.size() == 2 &&
            !(dot(n0, mesh.faceNormal(fs[1])) <= cosSharp && dot(mesh.faceCenter(fs[1]) - mesh.faceCenter(fs[0]), n0) < 0.0f))
            return;  // (flat, or an inside corner)
        const float3 normal = fs.size() == 2 ? n0 + mesh.faceNormal(fs[1]) : n0;
        const float3 a = mesh.positions[uint32_t(k >> 32)], b = mesh.positions[uint32_t(k)];
        const uint32_t n = std::max(1u, uint32_t(std::lround(length(b - a) / spacing)));
        for (uint32_t i = 0; i < n; ++i)
            add(a + (b - a) * std::clamp((float(i) + 0.5f + stray()) / float(n), 0.0f, 1.0f), normal, DamageSnap::Chip, fs[0]);
    };
    if (mode == 0) {  // every convex sharp edge (open ones too), in a fixed order
        std::vector<uint64_t> keys;
        for (const auto& [k, fs] : facesOf)
            keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const uint64_t k : keys)
            edge(k, true);
        // Round edges (a round bevel: convex edges that turn less, but by 30 degrees or more within a
        // chip's reach of each other): chips along the middle of each, `spacing` apart. Their edges
        // most turned around them first (the middle), the others' spots where none is near yet.
        const float reach = spacing / 1.5f;
        struct Soft {
            uint64_t key;
            float3 a, b, normal;
            float turn, half, around = 0.0f, priority = 0.0f;
        };
        std::vector<Soft> soft;
        for (const uint64_t k : keys) {
            const std::vector<uint32_t>& fs = facesOf[k];
            if (fs.size() != 2)
                continue;
            const float3 n0 = mesh.faceNormal(fs[0]), n1 = mesh.faceNormal(fs[1]);
            const float c = std::clamp(dot(n0, n1), -1.0f, 1.0f);
            if (c <= cosSharp || c > std::cos(1.0f * 3.14159265f / 180.0f) || dot(mesh.faceCenter(fs[1]) - mesh.faceCenter(fs[0]), n0) >= 0.0f)
                continue;
            const float3 a = mesh.positions[uint32_t(k >> 32)], b = mesh.positions[uint32_t(k)];
            const auto lineDistance = [&](float3 p) {
                const float3 ab = b - a;
                const float l2 = dot(ab, ab);
                return length(p - (l2 > 1e-12f ? a + ab * (dot(p - a, ab) / l2) : a));
            };
            soft.push_back({k, a, b, n0 + n1, std::acos(c) * 180.0f / 3.14159265f,
                            std::max(0.5f * (lineDistance(mesh.faceCenter(fs[0])) + lineDistance(mesh.faceCenter(fs[1]))), 1e-4f)});
        }
        const auto distance = [](float3 p, float3 a, float3 b) {
            const float3 ab = b - a;
            const float l2 = dot(ab, ab);
            return length(p - (l2 > 1e-12f ? a + ab * std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : a));
        };
        for (Soft& s : soft) {
            // (how far the surface turns within `reach`, and how evenly either side: the middle's even)
            const float3 mid = (s.a + s.b) * 0.5f, along = s.b - s.a;
            const float3 across = length(cross(along, s.normal)) > 1e-9f ? normalize(cross(along, s.normal)) : float3{0, 0, 0};
            float plus = 0.0f, minus = 0.0f;
            for (const Soft& o : soft) {
                const float d = distance(mid, o.a, o.b);
                const float overlap = std::min(o.half, d + reach) - std::max(-o.half, d - reach);
                if (overlap <= 0.0f)
                    continue;
                const float turn = o.turn * std::min(overlap / (2.0f * o.half), 1.0f);
                s.around += turn;
                const float side = dot((o.a + o.b) * 0.5f - mid, across);
                if (side > 1e-5f)
                    plus += turn;
                else if (side < -1e-5f)
                    minus += turn;
            }
            s.priority = s.around - std::fabs(plus - minus);
        }
        std::stable_sort(soft.begin(), soft.end(), [](const Soft& x, const Soft& y) { return x.priority > y.priority; });
        const size_t sharpSpots = out.size();
        for (const Soft& s : soft) {
            if (s.around < 30.0f)
                continue;
            const uint32_t n = std::max(1u, uint32_t(std::lround(length(s.b - s.a) / spacing)));
            for (uint32_t i = 0; i < n; ++i) {
                const float3 at = s.a + (s.b - s.a) * std::clamp((float(i) + 0.5f + stray()) / float(n), 0.0f, 1.0f);
                bool near = false;
                for (size_t j = sharpSpots; j < out.size() && !near; ++j)
                    near = length(out[j].at - at) < 0.9f * spacing;
                if (!near)
                    add(at, s.normal, DamageSnap::Chip, facesOf[s.key][0]);
            }
        }
    } else if (mode == 1) {  // a corner at each vertex, out along its faces' normals
        for (const uint64_t item : items) {
            const uint32_t v = uint32_t(item);
            if (v >= mesh.positions.size())
                continue;
            float3 normal = {};
            uint32_t on = ~0u;
            for (uint32_t f = 0; f < mesh.faces.size(); ++f)
                for (uint32_t k = 0; k < mesh.faces[f].count; ++k)
                    if (mesh.corners[mesh.faces[f].first + k] == v) {
                        normal = normal + mesh.faceNormal(f);
                        on = on == ~0u ? f : on;
                    }
            if (on != ~0u)
                add(mesh.positions[v], normal, DamageSnap::Corner, on);
        }
    } else if (mode == 2) {
        for (const uint64_t item : items)
            edge(key(uint32_t(item >> 32), uint32_t(item)), false);
    } else {  // holes over each face: about one per spacing² of its area, at random in its triangles
        for (const uint64_t item : items) {
            const uint32_t f = uint32_t(item);
            if (f >= mesh.faces.size())
                continue;
            const Face& face = mesh.faces[f];
            const float3 normal = mesh.faceNormal(f);
            const uint32_t before = places;
            for (uint32_t k = 1; k + 1 < face.count; ++k) {
                const float3 a = mesh.positions[mesh.corners[face.first]], b = mesh.positions[mesh.corners[face.first + k]],
                             c = mesh.positions[mesh.corners[face.first + k + 1]];
                const float area = 0.5f * length(cross(b - a, c - a));
                const float want = area / (spacing * spacing);
                // (a small triangle: one hole by the chance its area gives)
                uint32_t n = uint32_t(want);
                if (hash3(int(f), int(k), -1, seed) < want - float(n))
                    ++n;
                for (uint32_t i = 0; i < n; ++i) {
                    float u = hash3(int(f), int(k), int(i) * 2, seed), v = hash3(int(f), int(k), int(i) * 2 + 1, seed);
                    if (u + v > 1.0f)
                        u = 1.0f - u, v = 1.0f - v;
                    add(a + (b - a) * u + (c - a) * v, normal, DamageSnap::Hole, f);
                }
            }
            if (places == before)  // (smaller than one: a hole in its middle)
                add(mesh.faceCenter(f), normal, DamageSnap::Hole, f);
        }
    }
    return out;
}

}  // namespace fx::level
