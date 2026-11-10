// Height displacement cooked into geometry: a primitive subdivided until its edges are short
// enough for the height map's detail, then pushed out along its normal by the map. The result
// is an ordinary primitive, so the cluster LOD DAG (build_mesh.cpp) simplifies it again with
// distance: the detail costs triangles only where it is seen up close.
//
// Watertight: positions shared by several vertices (UV seams, hard edges) are welded first; an
// edge is split (at its midpoint) on both sides when the welded edge is long, and every vertex
// of a welded position moves the same way (the average of their heights, along the average of
// their normals), so no crack opens along seams or hard edges.

#include "cook.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace fx::cook {

namespace {

struct Vec3 {
    float x, y, z;
};
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a, Vec3 fallback = {0, 1, 0})
{
    const float l = length(a);
    return l > 1e-20f ? Vec3{a.x / l, a.y / l, a.z / l} : fallback;
}
Vec3 pos(const SrcVertex& v) { return {v.px, v.py, v.pz}; }
Vec3 nrm(const SrcVertex& v) { return {v.nx, v.ny, v.nz}; }

uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a; }

SrcVertex midpoint(const SrcVertex& a, const SrcVertex& b)
{
    SrcVertex m = a;
    m.px = (a.px + b.px) * 0.5f;
    m.py = (a.py + b.py) * 0.5f;
    m.pz = (a.pz + b.pz) * 0.5f;
    const Vec3 n = normalize({a.nx + b.nx, a.ny + b.ny, a.nz + b.nz}, nrm(a));
    m.nx = n.x, m.ny = n.y, m.nz = n.z;
    m.u = (a.u + b.u) * 0.5f;
    m.v = (a.v + b.v) * 0.5f;
    const Vec3 t = normalize({a.tx + b.tx, a.ty + b.ty, a.tz + b.tz}, {a.tx, a.ty, a.tz});
    m.tx = t.x, m.ty = t.y, m.tz = t.z;
    return m;
}

// Bilinear, wrapping like the textures' repeat addressing. glTF's UV origin is the image's top left.
float sampleHeight(const HeightMap& map, float u, float v)
{
    const float x = u * float(map.width) - 0.5f, y = v * float(map.height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto at = [&](int ix, int iy) {
        ix = ((ix % map.width) + map.width) % map.width;
        iy = ((iy % map.height) + map.height) % map.height;
        return map.values[size_t(iy) * map.width + ix];
    };
    const int ix = int(fx), iy = int(fy);
    const float top = at(ix, iy) * (1.0f - tx) + at(ix + 1, iy) * tx;
    const float bottom = at(ix, iy + 1) * (1.0f - tx) + at(ix + 1, iy + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

}  // namespace

bool loadHeightMap(const std::string& path, HeightMap& out)
{
    int w = 0, h = 0, comp = 0;
    out = {};
    if (stbi_is_16_bit(path.c_str())) {
        stbi_us* p = stbi_load_16(path.c_str(), &w, &h, &comp, 1);
        if (!p)
            return false;
        out.values.resize(size_t(w) * h);
        for (size_t i = 0; i < out.values.size(); ++i)
            out.values[i] = float(p[i]) / 65535.0f;
        stbi_image_free(p);
    } else {
        stbi_uc* p = stbi_load(path.c_str(), &w, &h, &comp, 1);
        if (!p)
            return false;
        out.values.resize(size_t(w) * h);
        for (size_t i = 0; i < out.values.size(); ++i)
            out.values[i] = float(p[i]) / 255.0f;
        stbi_image_free(p);
    }
    out.width = w;
    out.height = h;
    return w > 0 && h > 0;
}

SrcPrimitive displacePrimitive(const SrcPrimitive& src, const HeightMap& map, const DisplaceParams& params, std::string& note)
{
    SrcPrimitive out = src;
    std::vector<SrcVertex>& verts = out.vertices;
    std::vector<uint32_t>& tris = out.indices;
    if (tris.empty() || map.values.empty())
        return out;

    // Welded positions (exactly equal ones: the importer splits vertices without moving them).
    std::vector<uint32_t> weld(verts.size());
    uint32_t welds = 0;
    {
        struct Key {
            float x, y, z;
            bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct Hash {
            size_t operator()(const Key& k) const
            {
                uint32_t b[3];
                std::memcpy(b, &k, sizeof(b));
                return size_t(b[0] * 73856093u ^ b[1] * 19349663u ^ b[2] * 83492791u);
            }
        };
        std::unordered_map<Key, uint32_t, Hash> ids;
        for (size_t i = 0; i < verts.size(); ++i) {
            const auto [it, added] = ids.emplace(Key{verts[i].px, verts[i].py, verts[i].pz}, welds);
            weld[i] = it->second;
            welds += added;
        }
    }

    // The target edge length: given, or about one texel of the map (the median over triangles
    // of world size per UV unit); then no more than the triangle budget allows.
    double area = 0.0;
    std::vector<float> texel;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const SrcVertex &a = verts[tris[t]], &b = verts[tris[t + 1]], &c = verts[tris[t + 2]];
        const float worldArea = 0.5f * length(cross(sub(pos(b), pos(a)), sub(pos(c), pos(a))));
        const float uvArea = 0.5f * std::fabs((b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v));
        area += worldArea;
        if (uvArea > 1e-12f && worldArea > 0.0f)
            texel.push_back(std::sqrt(worldArea / uvArea) / float(std::max(map.width, map.height)));
    }
    float target = params.detail;
    if (target <= 0.0f) {
        if (texel.empty()) {
            note = "no UVs";
            return out;
        }
        std::nth_element(texel.begin(), texel.begin() + texel.size() / 2, texel.end());
        target = texel[texel.size() / 2];
    }
    // Splitting at midpoints until every edge fits: equilateral-ish triangles of side ~target/1.5.
    const double perTriangle = 0.433 * double(target) * double(target) / 2.25;
    const double estimate = area / std::max(perTriangle, 1e-12);
    if (estimate > double(params.maxTriangles)) {
        target *= float(std::sqrt(estimate / double(params.maxTriangles)));
        note = "detail coarsened to " + std::to_string(target) + " m (triangle budget)";
    }

    // Conforming refinement: each pass splits every welded edge longer than the target, in all
    // the triangles that share it (1 -> 2, 3 or 4 triangles by how many of its edges split).
    for (int pass = 0; pass < 24; ++pass) {
        std::unordered_map<uint64_t, uint8_t> longEdges;  // welded edge -> split
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = tris[t + e], b = tris[t + (e + 1) % 3];
                if (length(sub(pos(verts[a]), pos(verts[b]))) > target)
                    longEdges[edgeKey(weld[a], weld[b])] = 1;
            }
        if (longEdges.empty() || tris.size() / 3 > size_t(params.maxTriangles) * 2)
            break;
        std::unordered_map<uint64_t, uint32_t> mids;      // vertex pair -> its midpoint vertex
        std::unordered_map<uint64_t, uint32_t> midWelds;  // welded edge -> the midpoints' weld
        const auto mid = [&](uint32_t a, uint32_t b) {
            const auto [it, added] = mids.emplace(edgeKey(a, b), uint32_t(verts.size()));
            if (added) {
                verts.push_back(midpoint(verts[a], verts[b]));
                const auto [w, newWeld] = midWelds.emplace(edgeKey(weld[a], weld[b]), welds);
                weld.push_back(w->second);
                welds += newWeld;
            }
            return it->second;
        };
        std::vector<uint32_t> next;
        next.reserve(tris.size() * 2);
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const uint32_t v[3] = {tris[t], tris[t + 1], tris[t + 2]};
            bool split[3];
            int count = 0;
            for (int e = 0; e < 3; ++e)
                count += split[e] = longEdges.contains(edgeKey(weld[v[e]], weld[v[(e + 1) % 3]]));
            if (count == 0) {
                next.insert(next.end(), v, v + 3);
            } else if (count == 3) {
                const uint32_t m0 = mid(v[0], v[1]), m1 = mid(v[1], v[2]), m2 = mid(v[2], v[0]);
                const uint32_t add[12] = {v[0], m0, m2, m0, v[1], m1, m2, m1, v[2], m0, m1, m2};
                next.insert(next.end(), add, add + 12);
            } else if (count == 1) {
                const int e = split[0] ? 0 : split[1] ? 1 : 2;
                const uint32_t a = v[e], b = v[(e + 1) % 3], c = v[(e + 2) % 3], m = mid(a, b);
                const uint32_t add[6] = {a, m, c, m, b, c};
                next.insert(next.end(), add, add + 6);
            } else {
                const int keep = !split[0] ? 0 : !split[1] ? 1 : 2;  // the edge not split: (v[keep], v[keep+1])
                const uint32_t a = v[keep], b = v[(keep + 1) % 3], c = v[(keep + 2) % 3];
                const uint32_t mbc = mid(b, c), mca = mid(c, a);
                const uint32_t add[9] = {a, b, mbc, a, mbc, mca, mca, mbc, c};
                next.insert(next.end(), add, add + 9);
            }
        }
        tris.swap(next);
    }

    // Each welded position moves once: the average height of its vertices (their UVs may differ
    // across a seam), along the average of their normals (a hard edge's corner moves diagonally).
    std::vector<Vec3> dir(welds, {0, 0, 0});
    std::vector<float> height(welds, 0.0f);
    std::vector<uint32_t> members(welds, 0);
    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t w = weld[i];
        const Vec3 n = nrm(verts[i]);
        dir[w] = {dir[w].x + n.x, dir[w].y + n.y, dir[w].z + n.z};
        height[w] += sampleHeight(map, verts[i].u, verts[i].v);
        ++members[w];
    }
    const std::vector<SrcVertex> before = verts;  // (the original normals: which faces smooth together)
    for (size_t i = 0; i < verts.size(); ++i) {
        const uint32_t w = weld[i];
        const Vec3 d = normalize(dir[w], nrm(verts[i]));
        const float offset = (height[w] / float(members[w]) - params.center) * params.amount;
        verts[i].px += d.x * offset;
        verts[i].py += d.y * offset;
        verts[i].pz += d.z * offset;
    }

    // Normals and tangents of the new surface: area-weighted face normals and UV tangents, summed
    // per vertex, then shared by the vertices of a welded position whose original normals match
    // (seams smooth, hard edges stay hard).
    std::vector<Vec3> faceN(verts.size(), {0, 0, 0}), faceT(verts.size(), {0, 0, 0});
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const SrcVertex &a = verts[tris[t]], &b = verts[tris[t + 1]], &c = verts[tris[t + 2]];
        const Vec3 e1 = sub(pos(b), pos(a)), e2 = sub(pos(c), pos(a));
        const Vec3 n = cross(e1, e2);  // (length: twice the area)
        const float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
        const float det = du1 * dv2 - du2 * dv1;
        const float r = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
        const float l = length(n);
        const Vec3 tg = {(e1.x * dv2 - e2.x * dv1) * r * l, (e1.y * dv2 - e2.y * dv1) * r * l, (e1.z * dv2 - e2.z * dv1) * r * l};
        for (int k = 0; k < 3; ++k) {
            const uint32_t i = tris[t + k];
            faceN[i] = {faceN[i].x + n.x, faceN[i].y + n.y, faceN[i].z + n.z};
            const Vec3 tn = normalize(tg, {0, 0, 0});
            faceT[i] = {faceT[i].x + tn.x * l, faceT[i].y + tn.y * l, faceT[i].z + tn.z * l};
        }
    }
    struct Group {
        uint32_t weld;
        int32_t nx, ny, nz;
        bool operator==(const Group& o) const { return weld == o.weld && nx == o.nx && ny == o.ny && nz == o.nz; }
    };
    struct GroupHash {
        size_t operator()(const Group& g) const { return size_t(g.weld) * 2654435761u ^ size_t(g.nx * 73 + g.ny * 151 + g.nz * 199); }
    };
    std::unordered_map<Group, std::pair<Vec3, Vec3>, GroupHash> groups;
    const auto groupOf = [&](size_t i) {
        const Vec3 n = nrm(before[i]);
        return Group{weld[i], int32_t(std::lround(n.x * 20.0f)), int32_t(std::lround(n.y * 20.0f)), int32_t(std::lround(n.z * 20.0f))};
    };
    for (size_t i = 0; i < verts.size(); ++i) {
        auto& g = groups[groupOf(i)];
        g.first = {g.first.x + faceN[i].x, g.first.y + faceN[i].y, g.first.z + faceN[i].z};
        g.second = {g.second.x + faceT[i].x, g.second.y + faceT[i].y, g.second.z + faceT[i].z};
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        const auto& g = groups[groupOf(i)];
        const Vec3 n = normalize(g.first, nrm(before[i]));
        Vec3 t = g.second;
        t = sub(t, {n.x * dot(n, t), n.y * dot(n, t), n.z * dot(n, t)});  // Gram-Schmidt
        t = normalize(t, normalize(cross(n, std::fabs(n.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0})));
        SrcVertex& v = verts[i];
        v.nx = n.x, v.ny = n.y, v.nz = n.z;
        v.tx = t.x, v.ty = t.y, v.tz = t.z;
        if (v.tw == 0.0f)
            v.tw = 1.0f;
    }
    return out;
}

}  // namespace fx::cook
