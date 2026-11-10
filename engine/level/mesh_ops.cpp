#include "level/mesh_ops.h"

#include "core/json.h"
#include "level/walls.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fx::level {

namespace {

// ---- an editable form: faces as loops of vertex indices (with their UVs and settings)

struct Editable {
    std::vector<float3> pos;
    std::vector<std::vector<uint32_t>> loops;
    std::vector<Face> attrs;                      // (first / count unused here)
    std::vector<std::vector<float2>> uvs;          // per face corner (Manual faces)
    std::vector<std::vector<float2>> uv1;          // per face corner, the mask's (a changed loop: none, (0, 0))
    std::vector<uint32_t> colors;                  // per vertex (empty: the mesh has none; new vertices white)
    std::vector<std::string> materials;
    std::string layerMaterials[3];
    float layerBlend = 0.15f;
    bool hasUvs = false, hasUv1 = false;

    uint32_t addVertex(float3 p)
    {
        pos.push_back(p);
        if (!colors.empty())
            colors.push_back(0xffffffffu);
        return uint32_t(pos.size() - 1);
    }
    uint32_t addFace(std::vector<uint32_t> loop, const Face& like, std::vector<float2> uv = {}, std::vector<float2> mask = {})
    {
        if (uv.size() != loop.size())
            uv.assign(loop.size(), float2{0, 0});
        loops.push_back(std::move(loop));
        attrs.push_back(like);
        uvs.push_back(std::move(uv));
        uv1.push_back(std::move(mask));
        return uint32_t(loops.size() - 1);
    }
    // The same mesh without faces (its vertices, slots and layers): to add some of these faces back.
    Editable shell() const
    {
        Editable e;
        e.pos = pos;
        e.colors = colors;
        e.materials = materials;
        for (int k = 0; k < 3; ++k)
            e.layerMaterials[k] = layerMaterials[k];
        e.layerBlend = layerBlend;
        e.hasUvs = hasUvs;
        e.hasUv1 = hasUv1;
        return e;
    }
    uint32_t copyFace(const Editable& from, uint32_t f) { return addFace(from.loops[f], from.attrs[f], from.uvs[f], from.uv1[f]); }
    float3 normal(uint32_t f) const
    {
        const auto& l = loops[f];
        float3 n = {0, 0, 0};
        for (size_t i = 0; i < l.size(); ++i) {
            const float3 a = pos[l[i]], b = pos[l[(i + 1) % l.size()]];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        return normalize(n);
    }
    float3 center(uint32_t f) const
    {
        float3 c = {0, 0, 0};
        for (uint32_t v : loops[f])
            c += pos[v];
        return loops[f].empty() ? c : c * (1.0f / float(loops[f].size()));
    }
    // edge -> (face, corner) of each face using it, corner k going to k + 1
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> edges() const
    {
        std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> e;
        for (uint32_t f = 0; f < loops.size(); ++f)
            for (uint32_t k = 0; k < loops[f].size(); ++k)
                e[edgeKey(loops[f][k], loops[f][(k + 1) % loops[f].size()])].push_back({f, k});
        return e;
    }
};

Editable unpack(const PolyMesh& m)
{
    Editable e;
    e.pos = m.positions;
    e.materials = m.materials;
    e.hasUvs = !m.cornerUvs.empty();
    e.hasUv1 = !m.cornerUv1.empty();
    e.colors = m.vertexColors;
    for (int k = 0; k < 3; ++k)
        e.layerMaterials[k] = m.layerMaterials[k];
    e.layerBlend = m.layerBlend;
    for (const Face& f : m.faces) {
        std::vector<uint32_t> loop(m.corners.begin() + f.first, m.corners.begin() + f.first + f.count);
        std::vector<float2> uv, mask;
        if (e.hasUvs)
            uv.assign(m.cornerUvs.begin() + f.first, m.cornerUvs.begin() + f.first + f.count);
        if (e.hasUv1)
            mask.assign(m.cornerUv1.begin() + f.first, m.cornerUv1.begin() + f.first + f.count);
        e.addFace(std::move(loop), f, std::move(uv), std::move(mask));
    }
    return e;
}

// Back to a PolyMesh: faces of fewer than 3 distinct corners dropped (with their repeats removed),
// unused vertices dropped. `vertexMap` (optional): old vertex -> new (~0u: gone); `faceMap`: likewise.
PolyMesh pack(const Editable& e, std::vector<uint32_t>* vertexMap = nullptr, std::vector<uint32_t>* faceMap = nullptr)
{
    PolyMesh m;
    m.materials = e.materials.empty() ? std::vector<std::string>{""} : e.materials;
    for (int k = 0; k < 3; ++k)
        m.layerMaterials[k] = e.layerMaterials[k];
    m.layerBlend = e.layerBlend;
    std::vector<uint32_t> remap(e.pos.size(), ~0u);
    if (faceMap)
        faceMap->assign(e.loops.size(), ~0u);
    for (uint32_t f = 0; f < e.loops.size(); ++f) {
        std::vector<uint32_t> loop;
        std::vector<float2> uv, mask;
        const bool keepsMask = f < e.uv1.size() && e.uv1[f].size() == e.loops[f].size();  // (a changed loop: (0, 0))
        for (size_t k = 0; k < e.loops[f].size(); ++k) {
            const uint32_t v = e.loops[f][k];
            if (!loop.empty() && loop.back() == v)
                continue;
            loop.push_back(v);
            uv.push_back(k < e.uvs[f].size() ? e.uvs[f][k] : float2{0, 0});
            mask.push_back(keepsMask ? e.uv1[f][k] : float2{0, 0});
        }
        while (loop.size() > 1 && loop.front() == loop.back()) {
            loop.pop_back();
            uv.pop_back();
            mask.pop_back();
        }
        if (std::set<uint32_t>(loop.begin(), loop.end()).size() < 3)
            continue;
        Face face = e.attrs[f];
        face.first = uint32_t(m.corners.size());
        face.count = uint32_t(loop.size());
        face.material = std::min<uint32_t>(face.material, uint32_t(m.materials.size() - 1));
        for (uint32_t& v : loop) {
            if (remap[v] == ~0u) {
                remap[v] = uint32_t(m.positions.size());
                m.positions.push_back(e.pos[v]);
                if (!e.colors.empty())
                    m.vertexColors.push_back(v < e.colors.size() ? e.colors[v] : 0xffffffffu);
            }
            v = remap[v];
        }
        m.corners.insert(m.corners.end(), loop.begin(), loop.end());
        if (e.hasUvs)
            m.cornerUvs.insert(m.cornerUvs.end(), uv.begin(), uv.end());
        if (e.hasUv1)
            m.cornerUv1.insert(m.cornerUv1.end(), mask.begin(), mask.end());
        if (faceMap)
            (*faceMap)[f] = uint32_t(m.faces.size());
        m.faces.push_back(face);
    }
    if (vertexMap)
        *vertexMap = std::move(remap);
    return m;
}

std::vector<uint32_t> listOf(const Json& args, const char* key)
{
    std::vector<uint32_t> out;
    if (const Json* a = args.get(key); a && a->type == Json::Type::Array)
        for (const Json& v : a->array)
            out.push_back(uint32_t(v.number));
    return out;
}

std::vector<uint64_t> keysOf(const Json& args, const char* key)
{
    std::vector<uint64_t> out;
    if (const Json* a = args.get(key); a && a->type == Json::Type::Array)
        for (const Json& v : a->array)
            out.push_back(uint64_t(v.number));
    return out;
}

uint32_t lo(uint64_t key) { return uint32_t(key >> 32); }
uint32_t hi(uint64_t key) { return uint32_t(key & 0xffffffffu); }

int32_t cornerOf(const std::vector<uint32_t>& loop, uint32_t v)
{
    for (size_t k = 0; k < loop.size(); ++k)
        if (loop[k] == v)
            return int32_t(k);
    return -1;
}

// A face's selection result, through the face map of pack().
void selectFaces(OpResult& out, const std::vector<uint32_t>& faces, const std::vector<uint32_t>& faceMap)
{
    out.selectionMode = 3;
    for (uint32_t f : faces)
        if (f < faceMap.size() && faceMap[f] != ~0u)
            out.selection.push_back(faceMap[f]);
}

void selectEdges(OpResult& out, const std::vector<std::pair<uint32_t, uint32_t>>& edges, const std::vector<uint32_t>& vertexMap)
{
    out.selectionMode = 2;
    for (const auto& [a, b] : edges)
        if (a < vertexMap.size() && b < vertexMap.size() && vertexMap[a] != ~0u && vertexMap[b] != ~0u)
            out.selection.push_back(edgeKey(vertexMap[a], vertexMap[b]));
}

// ---- operations

bool extrude(Editable& e, const std::vector<uint32_t>& faces, float distance, OpResult& out)
{
    std::set<uint32_t> chosen(faces.begin(), faces.end());
    std::erase_if(chosen, [&](uint32_t f) { return f >= e.loops.size(); });
    if (chosen.empty())
        return out.error = "no faces", false;
    // Each vertex of the region moves along the average of its chosen faces' normals.
    std::map<uint32_t, float3> along;
    for (uint32_t f : chosen)
        for (uint32_t v : e.loops[f])
            along[v] += e.normal(f);
    std::map<uint32_t, uint32_t> moved;
    for (auto& [v, n] : along) {
        const float3 d = normalize(n);
        moved[v] = e.addVertex(e.pos[v] + d * distance);
    }
    // Walls along the region's border: edges only one chosen face uses.
    const auto edges = e.edges();
    std::vector<std::pair<uint32_t, uint32_t>> border;
    for (uint32_t f : chosen) {
        const auto loop = e.loops[f];
        for (size_t k = 0; k < loop.size(); ++k) {
            const uint32_t a = loop[k], b = loop[(k + 1) % loop.size()];
            uint32_t users = 0;
            for (const auto& [g, c] : edges.at(edgeKey(a, b)))
                users += chosen.count(g) ? 1u : 0u;
            if (users == 1)
                border.push_back({a, b});
        }
    }
    for (uint32_t f : chosen)
        for (uint32_t& v : e.loops[f])
            v = moved[v];
    const Face like = e.attrs[*chosen.begin()];
    for (const auto& [a, b] : border)  // a -> b as the chosen face winds it: [a, b, b', a'] faces out
        e.addFace({a, b, moved[b], moved[a]}, like);
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, std::vector<uint32_t>(chosen.begin(), chosen.end()), faceMap);
    return true;
}

bool extrudeEdges(Editable& e, const std::vector<uint64_t>& keys, float distance, OpResult& out)
{
    const auto edges = e.edges();
    std::vector<std::pair<uint32_t, uint32_t>> made;
    for (uint64_t key : keys) {
        const auto it = edges.find(key);
        if (it == edges.end() || it->second.size() != 1)
            continue;  // (only open edges: one face)
        const auto [f, k] = it->second[0];
        const uint32_t a = e.loops[f][k], b = e.loops[f][(k + 1) % e.loops[f].size()];
        const float3 n = e.normal(f), dir = normalize(cross(e.pos[b] - e.pos[a], n));  // out of the face, in its plane
        const uint32_t a2 = e.addVertex(e.pos[a] + dir * distance), b2 = e.addVertex(e.pos[b] + dir * distance);
        e.addFace({b, a, a2, b2}, e.attrs[f]);  // (the edge runs b -> a in the new face: the same side as f)
        made.push_back({a2, b2});
    }
    if (made.empty())
        return out.error = "no open edges (an edge with one face)", false;
    std::vector<uint32_t> vertexMap;
    out.mesh = pack(e, &vertexMap);
    selectEdges(out, made, vertexMap);
    return true;
}

bool inset(Editable& e, const std::vector<uint32_t>& faces, float amount, OpResult& out)
{
    std::vector<uint32_t> done;
    for (uint32_t f : faces) {
        if (f >= e.loops.size())
            continue;
        const float3 c = e.center(f);
        const std::vector<uint32_t> outer = e.loops[f];
        std::vector<uint32_t> inner;
        for (uint32_t v : outer) {
            const float3 to = c - e.pos[v];
            const float l = length(to);
            inner.push_back(e.addVertex(e.pos[v] + (l > 1e-6f ? to * (std::min(amount, l * 0.98f) / l) : float3{0, 0, 0})));
        }
        for (size_t k = 0; k < outer.size(); ++k) {
            const size_t n = (k + 1) % outer.size();
            e.addFace({outer[k], outer[n], inner[n], inner[k]}, e.attrs[f]);
        }
        e.loops[f] = inner;
        done.push_back(f);
    }
    if (done.empty())
        return out.error = "no faces", false;
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, done, faceMap);
    return true;
}

// One edge cut into a face (a chamfer): its two faces pulled back along their other edges at each
// end, the new face between them (or `segments` of them round the profile, level::bevelProfile). At
// each end, the faces around fill the gap: a corner where three faces meet (a box's) takes the new
// points in place of the old one; elsewhere the faces beside get them on their edges and a face
// closes the gap. A round strip's faces share smoothing group `smooth` (0: flat).
bool bevelOne(Editable& e, uint64_t key, float amount, uint32_t segments, float curve, uint32_t smooth, std::vector<uint32_t>& made)
{
    const auto edges = e.edges();
    const auto it = edges.find(key);
    if (it == edges.end() || it->second.size() != 2)
        return false;
    const uint32_t va = lo(key), vb = hi(key);
    const uint32_t f1 = it->second[0].first, f2 = it->second[1].first;
    // The face's other neighbour of v (not `other`) and the point `amount` toward it.
    const auto awayOf = [&](uint32_t f, uint32_t v, uint32_t other) {
        const auto& l = e.loops[f];
        const size_t k = size_t(cornerOf(l, v));
        const uint32_t prev = l[(k + l.size() - 1) % l.size()], next = l[(k + 1) % l.size()];
        return prev == other ? next : prev;
    };
    const auto slide = [&](uint32_t v, uint32_t away) {
        const float3 d = e.pos[away] - e.pos[v];
        const float len = length(d);
        return e.pos[v] + (len > 1e-6f ? d * (std::min(amount, len * 0.49f) / len) : float3{0, 0, 0});
    };
    struct End {
        uint32_t v, away1, away2, p1, p2;
        std::vector<uint32_t> chain;  // p1, the profile's points between, p2
    };
    End ends[2];
    const float3 edgeA = e.pos[va], edgeB = e.pos[vb];
    for (int s = 0; s < 2; ++s) {
        const uint32_t v = s == 0 ? va : vb, other = s == 0 ? vb : va;
        End& end = ends[s];
        end.v = v;
        end.away1 = awayOf(f1, v, other);
        end.away2 = awayOf(f2, v, other);
        end.p1 = e.addVertex(slide(v, end.away1));
        end.p2 = e.addVertex(slide(v, end.away2));
        const float3 p1 = e.pos[end.p1], p2 = e.pos[end.p2];
        const float3 corner = nearestOnLine((p1 + p2) * 0.5f, edgeA, edgeB);
        end.chain.push_back(end.p1);
        for (uint32_t i = 1; i < segments; ++i)
            end.chain.push_back(e.addVertex(bevelProfile(p1, corner, p2, float(i) / float(segments), curve)));
        end.chain.push_back(end.p2);
    }
    // The profile's points between p1 and p2, from `from` (p1 or p2) on.
    const auto between = [&](const End& end, uint32_t from) {
        std::vector<uint32_t> inner(end.chain.begin() + 1, end.chain.end() - 1);
        if (from != end.p1)
            std::reverse(inner.begin(), inner.end());
        return inner;
    };
    for (const End& end : ends) {
        uint32_t g1 = ~0u, g2 = ~0u;  // the faces beside f1 and f2 (across their other edges at v)
        for (uint32_t g = 0; g < e.loops.size(); ++g) {
            if (g == f1 || g == f2)
                continue;
            auto& l = e.loops[g];
            const int32_t k = cornerOf(l, end.v);
            if (k < 0)
                continue;
            const uint32_t prev = l[(size_t(k) + l.size() - 1) % l.size()], next = l[(size_t(k) + 1) % l.size()];
            std::vector<uint32_t> before, after;  // points onto its edges at v
            if (prev == end.away1)
                before.push_back(end.p1), g1 = g;
            if (prev == end.away2)
                before.push_back(end.p2), g2 = g;
            if (next == end.away1)
                after.push_back(end.p1), g1 = g;
            if (next == end.away2)
                after.push_back(end.p2), g2 = g;
            if (before.empty() && after.empty())
                continue;
            std::vector<uint32_t> with = before;
            if (before.empty() || after.empty()) {
                with.push_back(end.v);  // (v stays: the face closing the gap meets it)
            } else {
                const std::vector<uint32_t> inner = between(end, before[0]);
                with.insert(with.end(), inner.begin(), inner.end());
            }
            with.insert(with.end(), after.begin(), after.end());
            l.erase(l.begin() + k);
            l.insert(l.begin() + k, with.begin(), with.end());
            e.uvs[g].assign(l.size(), float2{0, 0});
            e.uv1[g].clear();
        }
        if (g1 == ~0u || g1 != g2) {
            // The gap p1, v, p2: wound against the face beside f1 (which runs p1 -> v or v -> p1).
            bool p1ThenV = false;
            if (g1 != ~0u) {
                const auto& l = e.loops[g1];
                const size_t k = size_t(cornerOf(l, end.p1));
                p1ThenV = l[(k + 1) % l.size()] == end.v;
            }
            // (v, p1, the profile, p2; or the other way round)
            std::vector<uint32_t> gap = {end.v, p1ThenV ? end.p1 : end.p2};
            const std::vector<uint32_t> inner = between(end, gap[1]);
            gap.insert(gap.end(), inner.begin(), inner.end());
            gap.push_back(p1ThenV ? end.p2 : end.p1);
            made.push_back(e.addFace(gap, e.attrs[f1]));
        }
    }
    for (uint32_t& v : e.loops[f1])
        v = v == va ? ends[0].p1 : v == vb ? ends[1].p1 : v;
    for (uint32_t& v : e.loops[f2])
        v = v == va ? ends[0].p2 : v == vb ? ends[1].p2 : v;
    // The new faces run against f1 along (a1, b1), across to f2.
    const auto& l1 = e.loops[f1];
    const size_t k1 = size_t(cornerOf(l1, ends[0].p1));
    const bool aThenB = l1[(k1 + 1) % l1.size()] == ends[1].p1;
    const auto& a = ends[0].chain;
    const auto& b = ends[1].chain;
    Face like = e.attrs[f1];
    like.smoothing = smooth;
    for (uint32_t i = 0; i < segments; ++i)
        made.push_back(e.addFace(aThenB ? std::vector<uint32_t>{b[i], a[i], a[i + 1], b[i + 1]} : std::vector<uint32_t>{a[i], b[i], b[i + 1], a[i + 1]},
                                 like));
    return true;
}

bool bevel(Editable& e, const std::vector<uint64_t>& keys, float amount, uint32_t segments, float curve, OpResult& out)
{
    segments = std::clamp(segments, 1u, 64u);
    curve = std::clamp(curve, -1.0f, 1.0f);
    uint32_t smooth = 0;  // a round one's faces: a smoothing group of their own
    if (segments > 1 && curve != 0.0f) {
        for (const Face& f : e.attrs)
            if (f.smoothing < 0x10000000u)
                smooth = std::max(smooth, f.smoothing);
        ++smooth;
    }
    // Edges one after another, each found again in the mesh as the last one left it (by position).
    std::vector<std::pair<float3, float3>> where;
    for (uint64_t key : keys)
        if (lo(key) < e.pos.size() && hi(key) < e.pos.size())
            where.push_back({e.pos[lo(key)], e.pos[hi(key)]});
    std::vector<uint32_t> made;
    for (const auto& [pa, pb] : where) {
        uint32_t a = ~0u, b = ~0u;
        for (uint32_t v = 0; v < e.pos.size(); ++v) {
            if (length(e.pos[v] - pa) < 1e-6f && a == ~0u)
                a = v;
            if (length(e.pos[v] - pb) < 1e-6f && b == ~0u)
                b = v;
        }
        if (a != ~0u && b != ~0u)
            bevelOne(e, edgeKey(a, b), amount, segments, curve, smooth, made);
    }
    if (made.empty())
        return out.error = "no edges between two faces", false;
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, made, faceMap);
    return true;
}

// Splits face f between two corners (indices into its loop): two faces.
void splitFace(Editable& e, uint32_t f, size_t i, size_t j)
{
    if (i > j)
        std::swap(i, j);
    const std::vector<uint32_t> l = e.loops[f];
    std::vector<uint32_t> a(l.begin() + ptrdiff_t(i), l.begin() + ptrdiff_t(j) + 1);
    std::vector<uint32_t> b(l.begin() + ptrdiff_t(j), l.end());
    b.insert(b.end(), l.begin(), l.begin() + ptrdiff_t(i) + 1);
    e.loops[f] = a;
    e.uvs[f].assign(a.size(), float2{0, 0});
    e.uv1[f].clear();
    e.addFace(b, e.attrs[f]);
}

// A vertex in the middle of edge (a, b) of every face using it (shared): its index.
uint32_t splitEdge(Editable& e, uint32_t a, uint32_t b, float t = 0.5f)
{
    const uint32_t m = e.addVertex(e.pos[a] + (e.pos[b] - e.pos[a]) * t);
    for (uint32_t f = 0; f < e.loops.size(); ++f) {
        auto& l = e.loops[f];
        for (size_t k = 0; k < l.size(); ++k) {
            const size_t n = (k + 1) % l.size();
            if ((l[k] == a && l[n] == b) || (l[k] == b && l[n] == a)) {
                l.insert(l.begin() + ptrdiff_t(n == 0 ? l.size() : n), m);
                e.uvs[f].assign(l.size(), float2{0, 0});
                e.uv1[f].clear();
                break;
            }
        }
    }
    return m;
}

bool connect(Editable& e, const std::vector<uint64_t>& keys, const std::vector<uint32_t>& vertices, OpResult& out)
{
    std::vector<std::pair<uint32_t, uint32_t>> made;
    if (!keys.empty()) {
        // Each edge's middle; then each face with two of those middles split between them.
        std::map<uint64_t, uint32_t> middle;
        for (uint64_t key : keys)
            if (lo(key) < e.pos.size() && hi(key) < e.pos.size() && !middle.count(key))
                middle[key] = splitEdge(e, lo(key), hi(key));
        std::set<uint32_t> mids;
        for (const auto& [k, m] : middle)
            mids.insert(m);
        for (uint32_t f = 0, n = uint32_t(e.loops.size()); f < n; ++f) {
            std::vector<size_t> at;
            for (size_t k = 0; k < e.loops[f].size(); ++k)
                if (mids.count(e.loops[f][k]))
                    at.push_back(k);
            if (at.size() >= 2) {
                made.push_back({e.loops[f][at[0]], e.loops[f][at[1]]});
                splitFace(e, f, at[0], at[1]);
            }
        }
    } else {
        const std::set<uint32_t> chosen(vertices.begin(), vertices.end());
        for (uint32_t f = 0, n = uint32_t(e.loops.size()); f < n; ++f) {
            std::vector<size_t> at;
            for (size_t k = 0; k < e.loops[f].size(); ++k)
                if (chosen.count(e.loops[f][k]))
                    at.push_back(k);
            if (at.size() >= 2 && (at[1] - at[0]) % e.loops[f].size() > 1 && at[0] + e.loops[f].size() - at[1] > 1) {
                made.push_back({e.loops[f][at[0]], e.loops[f][at[1]]});
                splitFace(e, f, at[0], at[1]);
            }
        }
    }
    if (made.empty())
        return out.error = "nothing to connect (two edges or two vertices of one face)", false;
    std::vector<uint32_t> vertexMap;
    out.mesh = pack(e, &vertexMap);
    selectEdges(out, made, vertexMap);
    return true;
}

// The ring through an edge: across each quad to its opposite edge, through each of its faces.
std::vector<uint64_t> ringOf(const Editable& e, uint64_t start)
{
    const auto edges = e.edges();
    std::vector<uint64_t> ring = {start};
    std::set<uint64_t> seen = {start};
    const auto first = edges.find(start);
    if (first == edges.end())
        return ring;
    for (const auto& [firstFace, unused] : first->second) {
        uint64_t key = start;
        uint32_t f = firstFace;
        for (size_t step = 0; step < e.loops.size(); ++step) {
            const auto& l = e.loops[f];
            if (l.size() != 4)
                break;
            int32_t k = -1;
            for (int32_t c = 0; c < 4; ++c)
                if (edgeKey(l[size_t(c)], l[size_t((c + 1) % 4)]) == key)
                    k = c;
            if (k < 0)
                break;
            const uint64_t opposite = edgeKey(l[size_t((k + 2) % 4)], l[size_t((k + 3) % 4)]);
            if (!seen.insert(opposite).second)
                break;
            ring.push_back(opposite);
            uint32_t next = ~0u;
            for (const auto& [g, c] : edges.at(opposite))
                if (g != f)
                    next = g;
            if (next == ~0u)
                break;
            key = opposite;
            f = next;
        }
    }
    return ring;
}

// The loop through an edge: at each end, the edge straight across a vertex with four edges.
std::vector<uint64_t> loopOf(const Editable& e, uint64_t start)
{
    std::vector<uint64_t> loop = {start};
    std::set<uint64_t> seen = {start};
    for (int side = 0; side < 2; ++side) {
        uint32_t from = side == 0 ? lo(start) : hi(start), at = side == 0 ? hi(start) : lo(start);
        for (int step = 0; step < 10000; ++step) {
            // Faces around `at` and its edges; with four of each, the one sharing no face with (from, at).
            std::set<uint32_t> around;
            std::set<uint32_t> faces;
            for (uint32_t f = 0; f < e.loops.size(); ++f) {
                const int32_t k = cornerOf(e.loops[f], at);
                if (k < 0)
                    continue;
                faces.insert(f);
                around.insert(e.loops[f][(size_t(k) + 1) % e.loops[f].size()]);
                around.insert(e.loops[f][(size_t(k) + e.loops[f].size() - 1) % e.loops[f].size()]);
            }
            if (around.size() != 4 || faces.size() != 4)
                break;
            const auto inFace = [&](uint32_t f, uint32_t a, uint32_t b) { return cornerOf(e.loops[f], a) >= 0 && cornerOf(e.loops[f], b) >= 0; };
            uint32_t next = ~0u;
            for (uint32_t v : around) {
                if (v == from)
                    continue;
                bool shares = false;
                for (uint32_t f : faces)
                    shares |= inFace(f, at, v) && inFace(f, at, from);
                if (!shares)
                    next = v;
            }
            if (next == ~0u || !seen.insert(edgeKey(at, next)).second)
                break;
            loop.push_back(edgeKey(at, next));
            from = at;
            at = next;
        }
    }
    return loop;
}

bool subdivide(Editable& e, const std::vector<uint32_t>& faces, OpResult& out)
{
    std::map<uint64_t, uint32_t> middle;
    const auto mid = [&](uint32_t a, uint32_t b) {
        const uint64_t key = edgeKey(a, b);
        if (const auto it = middle.find(key); it != middle.end())
            return it->second;
        return middle[key] = splitEdge(e, a, b);
    };
    std::vector<std::vector<uint32_t>> originals;
    std::vector<uint32_t> chosen;
    for (uint32_t f : faces)
        if (f < e.loops.size()) {
            chosen.push_back(f);
            originals.push_back(e.loops[f]);
        }
    if (chosen.empty())
        return out.error = "no faces", false;
    // Edge middles first (neighbours get them too: no cracks), then each face as quads around its centre.
    for (const auto& l : originals)
        for (size_t k = 0; k < l.size(); ++k)
            mid(l[k], l[(k + 1) % l.size()]);
    std::vector<uint32_t> made;
    for (size_t i = 0; i < chosen.size(); ++i) {
        const uint32_t f = chosen[i];
        const auto& l = originals[i];
        float3 c = {0, 0, 0};
        for (uint32_t v : l)
            c += e.pos[v];
        const uint32_t centre = e.addVertex(c * (1.0f / float(l.size())));
        const Face like = e.attrs[f];
        for (size_t k = 0; k < l.size(); ++k) {
            const uint32_t m0 = middle[edgeKey(l[(k + l.size() - 1) % l.size()], l[k])], m1 = middle[edgeKey(l[k], l[(k + 1) % l.size()])];
            const std::vector<uint32_t> quad = {l[k], m1, centre, m0};
            if (k == 0) {
                e.loops[f] = quad;
                e.uvs[f].assign(4, float2{0, 0});
                e.uv1[f].clear();
                made.push_back(f);
            } else {
                made.push_back(e.addFace(quad, like));
            }
        }
    }
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, made, faceMap);
    return true;
}

bool mergeVertices(Editable& e, const std::vector<uint32_t>& vertices, OpResult& out)
{
    std::set<uint32_t> chosen;
    for (uint32_t v : vertices)
        if (v < e.pos.size())
            chosen.insert(v);
    if (chosen.size() < 2)
        return out.error = "two vertices or more", false;
    float3 c = {0, 0, 0};
    for (uint32_t v : chosen)
        c += e.pos[v];
    const uint32_t into = *chosen.begin();
    e.pos[into] = c * (1.0f / float(chosen.size()));
    for (auto& l : e.loops)
        for (uint32_t& v : l)
            if (chosen.count(v))
                v = into;
    std::vector<uint32_t> vertexMap;
    out.mesh = pack(e, &vertexMap);
    out.selectionMode = 1;
    if (vertexMap[into] != ~0u)
        out.selection.push_back(vertexMap[into]);
    return true;
}

bool weld(Editable& e, const std::vector<uint32_t>& vertices, float distance, OpResult& out)
{
    std::vector<uint32_t> list;
    for (uint32_t v : vertices)
        if (v < e.pos.size())
            list.push_back(v);
    if (vertices.empty())
        for (uint32_t v = 0; v < e.pos.size(); ++v)
            list.push_back(v);
    // Each vertex into the first one before it within the distance (cells of that size: the 27 around).
    const float cell = std::max(distance, 1e-5f);
    const auto cellOf = [&](float3 p, int dx, int dy, int dz) {
        const int64_t x = int64_t(std::floor(p.x / cell)) + dx, y = int64_t(std::floor(p.y / cell)) + dy, z = int64_t(std::floor(p.z / cell)) + dz;
        return uint64_t(x * 73856093) ^ uint64_t(y * 19349663) ^ uint64_t(z * 83492791);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    std::vector<uint32_t> into(e.pos.size());
    for (uint32_t v = 0; v < into.size(); ++v)
        into[v] = v;
    uint32_t welded = 0;
    for (uint32_t v : list) {
        uint32_t found = ~0u;
        for (int dx = -1; dx <= 1 && found == ~0u; ++dx)
            for (int dy = -1; dy <= 1 && found == ~0u; ++dy)
                for (int dz = -1; dz <= 1 && found == ~0u; ++dz)
                    if (const auto it = grid.find(cellOf(e.pos[v], dx, dy, dz)); it != grid.end())
                        for (uint32_t o : it->second)
                            if (length(e.pos[v] - e.pos[o]) <= distance) {
                                found = o;
                                break;
                            }
        if (found != ~0u) {
            into[v] = found;
            ++welded;
        } else {
            grid[cellOf(e.pos[v], 0, 0, 0)].push_back(v);
        }
    }
    for (auto& l : e.loops)
        for (uint32_t& v : l)
            v = into[v];
    out.mesh = pack(e);
    if (!welded)
        out.error = "nothing within the distance";
    return welded > 0;
}

bool fill(Editable& e, const std::vector<uint64_t>& keys, const std::vector<uint32_t>& vertices, OpResult& out)
{
    const auto edges = e.edges();
    std::vector<uint32_t> loop;
    Face like = e.attrs.empty() ? Face{} : e.attrs[0];
    if (keys.size() == 2) {
        // A bridge: the two edges as one face, wound opposite to the faces beside them.
        uint32_t a = lo(keys[0]), b = hi(keys[0]), c = lo(keys[1]), d = hi(keys[1]);
        if (const auto it = edges.find(keys[0]); it != edges.end() && !it->second.empty()) {
            const auto [f, k] = it->second[0];
            like = e.attrs[f];
            a = e.loops[f][(k + 1) % e.loops[f].size()];  // the face's b -> a, reversed: a -> b for us
            b = e.loops[f][k];
        }
        if (length(e.pos[b] - e.pos[c]) > length(e.pos[b] - e.pos[d]))
            std::swap(c, d);
        loop = {a, b, c, d};
    } else if (keys.size() == 1) {
        // An open edge's hole: follow the open edges around it.
        std::unordered_map<uint32_t, uint32_t> next;  // open edge a -> b (as their faces wind it), reversed for the hole
        for (const auto& [key, users] : edges)
            if (users.size() == 1) {
                const auto [f, k] = users[0];
                next[e.loops[f][(k + 1) % e.loops[f].size()]] = e.loops[f][k];
                like = e.attrs[f];
            }
        uint32_t v = lo(keys[0]);
        if (!next.count(v))
            v = hi(keys[0]);
        for (size_t step = 0; step < next.size() + 1 && next.count(v); ++step) {
            loop.push_back(v);
            v = next[v];
            if (v == loop.front())
                break;
        }
        if (loop.empty() || v != loop.front())
            return out.error = "the edge is on no hole (an edge with one face)", false;
    } else if (vertices.size() >= 3) {
        // A face over the vertices, around their centre (on their plane).
        float3 c = {0, 0, 0};
        for (uint32_t v : vertices)
            c += e.pos[v];
        c = c * (1.0f / float(vertices.size()));
        float3 n = cross(e.pos[vertices[1]] - e.pos[vertices[0]], e.pos[vertices[2]] - e.pos[vertices[0]]);
        n = normalize(n);
        const float3 u = normalize(e.pos[vertices[0]] - c), w = cross(n, u);
        loop = vertices;
        std::sort(loop.begin(), loop.end(), [&](uint32_t a, uint32_t b) {
            const float3 da = e.pos[a] - c, db = e.pos[b] - c;
            return std::atan2(dot(da, w), dot(da, u)) < std::atan2(dot(db, w), dot(db, u));
        });
    } else {
        return out.error = "two edges (a bridge), an open edge (its hole) or three vertices or more", false;
    }
    const uint32_t made = e.addFace(loop, like);
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, {made}, faceMap);
    return true;
}

bool remove(Editable& e, const std::vector<uint32_t>& faces, const std::vector<uint64_t>& keys, const std::vector<uint32_t>& vertices,
            OpResult& out, bool keepTaken = false)
{
    std::set<uint32_t> gone(faces.begin(), faces.end());
    const std::set<uint32_t> verts(vertices.begin(), vertices.end());
    const std::set<uint64_t> edgeSet(keys.begin(), keys.end());
    for (uint32_t f = 0; f < e.loops.size(); ++f) {
        const auto& l = e.loops[f];
        for (size_t k = 0; k < l.size(); ++k)
            if (verts.count(l[k]) || edgeSet.count(edgeKey(l[k], l[(k + 1) % l.size()])))
                gone.insert(f);
    }
    if (gone.empty())
        return out.error = "nothing to delete", false;
    if (keepTaken) {
        Editable taken = e.shell();
        for (uint32_t f : gone)
            if (f < e.loops.size())
                taken.copyFace(e, f);
        out.detached = pack(taken);
    }
    Editable kept = e.shell();
    for (uint32_t f = 0; f < e.loops.size(); ++f)
        if (!gone.count(f))
            kept.copyFace(e, f);
    out.mesh = pack(kept);
    out.selectionMode = 3;  // (nothing chosen after)
    return true;
}

void flipFace(Editable& e, uint32_t f)
{
    std::reverse(e.loops[f].begin(), e.loops[f].end());
    std::reverse(e.uvs[f].begin(), e.uvs[f].end());
    std::reverse(e.uv1[f].begin(), e.uv1[f].end());
}

bool conform(Editable& e, OpResult& out)
{
    // From each face not yet seen, its neighbours wound as it is: an edge used once each way.
    const auto edges = e.edges();
    std::vector<int8_t> seen(e.loops.size(), 0);
    uint32_t flipped = 0;
    for (uint32_t start = 0; start < e.loops.size(); ++start) {
        if (seen[start])
            continue;
        std::vector<uint32_t> stack = {start};
        seen[start] = 1;
        while (!stack.empty()) {
            const uint32_t f = stack.back();
            stack.pop_back();
            const auto l = e.loops[f];
            for (size_t k = 0; k < l.size(); ++k) {
                const uint32_t a = l[k], b = l[(k + 1) % l.size()];
                for (const auto& [g, c] : edges.at(edgeKey(a, b))) {
                    if (g == f || seen[g])
                        continue;
                    seen[g] = 1;
                    // Same direction there (a -> b in both): g is wound the other way.
                    const auto& gl = e.loops[g];
                    const int32_t ga = cornerOf(gl, a);
                    if (ga >= 0 && gl[(size_t(ga) + 1) % gl.size()] == b) {
                        flipFace(e, g);
                        ++flipped;
                    }
                    stack.push_back(g);
                }
            }
        }
    }
    out.mesh = pack(e);
    if (!flipped)
        out.error = "every face already agrees";
    return true;
}

// A loop's triangles by ear clipping in its plane (concave faces too): corners of the loop, 3 each.
std::vector<uint32_t> earClip(const Editable& e, const std::vector<uint32_t>& loop, float3 n)
{
    const float3 u = std::fabs(n.x) < 0.9f ? normalize(cross(n, float3{1, 0, 0})) : normalize(cross(n, float3{0, 1, 0}));
    const float3 w = cross(n, u);
    std::vector<float2> p;
    for (uint32_t v : loop)
        p.push_back({dot(e.pos[v], u), dot(e.pos[v], w)});
    const auto area = [&](size_t a, size_t b, size_t c) {
        return (p[b].x - p[a].x) * (p[c].y - p[a].y) - (p[c].x - p[a].x) * (p[b].y - p[a].y);
    };
    std::vector<size_t> left(loop.size());
    for (size_t i = 0; i < left.size(); ++i)
        left[i] = i;
    std::vector<uint32_t> out;
    while (left.size() > 3) {
        bool clipped = false;
        for (size_t i = 0; i < left.size() && !clipped; ++i) {
            const size_t a = left[(i + left.size() - 1) % left.size()], b = left[i], c = left[(i + 1) % left.size()];
            if (area(a, b, c) <= 1e-12f)
                continue;  // (reflex or flat)
            bool inside = false;
            for (size_t q : left)
                if (q != a && q != b && q != c && area(a, b, q) >= 0 && area(b, c, q) >= 0 && area(c, a, q) >= 0)
                    inside = true;
            if (inside)
                continue;
            out.insert(out.end(), {uint32_t(a), uint32_t(b), uint32_t(c)});
            left.erase(left.begin() + ptrdiff_t(i));
            clipped = true;
        }
        if (!clipped) {  // (degenerate: a fan of what is left)
            for (size_t i = 1; i + 1 < left.size(); ++i)
                out.insert(out.end(), {uint32_t(left[0]), uint32_t(left[i]), uint32_t(left[i + 1])});
            return out;
        }
    }
    out.insert(out.end(), {uint32_t(left[0]), uint32_t(left[1]), uint32_t(left[2])});
    return out;
}

bool triangulate(Editable& e, const std::vector<uint32_t>& faces, OpResult& out)
{
    std::vector<uint32_t> list = faces;
    if (list.empty())
        for (uint32_t f = 0; f < e.loops.size(); ++f)
            list.push_back(f);
    std::vector<uint32_t> made;
    for (uint32_t f : list) {
        if (f >= e.loops.size() || e.loops[f].size() <= 3)
            continue;
        const std::vector<uint32_t> l = e.loops[f];
        const std::vector<float2> uv = e.uvs[f], mask = e.uv1[f];
        const std::vector<uint32_t> tris = earClip(e, l, e.normal(f));
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const std::vector<uint32_t> tri = {l[tris[t]], l[tris[t + 1]], l[tris[t + 2]]};
            std::vector<float2> triUv = {uv[tris[t]], uv[tris[t + 1]], uv[tris[t + 2]]}, triMask;
            if (mask.size() == l.size())
                triMask = {mask[tris[t]], mask[tris[t + 1]], mask[tris[t + 2]]};
            if (t == 0) {
                e.loops[f] = tri;
                e.uvs[f] = triUv;
                e.uv1[f] = triMask;
                made.push_back(f);
            } else {
                made.push_back(e.addFace(tri, e.attrs[f], triUv, triMask));
            }
        }
    }
    std::vector<uint32_t> faceMap;
    out.mesh = pack(e, nullptr, &faceMap);
    selectFaces(out, made, faceMap);
    return !made.empty() || (out.error = "no face of more than three corners", false);
}

// Adjacent faces into one: the region's outline, when it is one loop.
bool mergeFaces(Editable& e, const std::vector<uint32_t>& faces, OpResult& out, bool quadsOnly = false)
{
    std::set<uint32_t> chosen;
    for (uint32_t f : faces)
        if (f < e.loops.size())
            chosen.insert(f);
    if (chosen.size() < 2)
        return out.error = "two faces or more", false;
    std::map<uint64_t, int> uses;
    for (uint32_t f : chosen)
        for (size_t k = 0; k < e.loops[f].size(); ++k)
            ++uses[edgeKey(e.loops[f][k], e.loops[f][(k + 1) % e.loops[f].size()])];
    std::unordered_map<uint32_t, uint32_t> next;
    for (uint32_t f : chosen)
        for (size_t k = 0; k < e.loops[f].size(); ++k) {
            const uint32_t a = e.loops[f][k], b = e.loops[f][(k + 1) % e.loops[f].size()];
            if (uses[edgeKey(a, b)] == 1)
                next[a] = b;
        }
    if (next.empty())
        return out.error = "the faces have no outline", false;
    std::vector<uint32_t> loop;
    uint32_t v = next.begin()->first;
    for (size_t step = 0; step <= next.size(); ++step) {
        loop.push_back(v);
        v = next[v];
        if (v == loop.front())
            break;
    }
    if (v != loop.front() || loop.size() != next.size() || (quadsOnly && loop.size() != 4))
        return out.error = "the faces' outline is not one loop", false;
    const uint32_t keep = *chosen.begin();
    e.loops[keep] = loop;
    e.uvs[keep].assign(loop.size(), float2{0, 0});
    e.uv1[keep].clear();
    Editable kept = e.shell();
    uint32_t made = ~0u;
    for (uint32_t f = 0; f < e.loops.size(); ++f)
        if (f == keep)
            made = kept.copyFace(e, f);
        else if (!chosen.count(f))
            kept.copyFace(e, f);
    std::vector<uint32_t> faceMap;
    out.mesh = pack(kept, nullptr, &faceMap);
    selectFaces(out, {made}, faceMap);
    return true;
}

bool quadrangulate(Editable& e, const std::vector<uint32_t>& faces, OpResult& out)
{
    // Triangle pairs sharing their longest edge, nearly flat together, into quads.
    std::vector<uint32_t> list = faces;
    if (list.empty())
        for (uint32_t f = 0; f < e.loops.size(); ++f)
            list.push_back(f);
    std::set<uint32_t> tris;
    for (uint32_t f : list)
        if (f < e.loops.size() && e.loops[f].size() == 3)
            tris.insert(f);
    const auto edges = e.edges();
    std::set<uint32_t> used, removed;
    uint32_t merged = 0;
    for (uint32_t f : tris) {
        if (used.count(f))
            continue;
        const auto& l = e.loops[f];
        size_t longest = 0;
        for (size_t k = 1; k < 3; ++k)
            if (length(e.pos[l[(k + 1) % 3]] - e.pos[l[k]]) > length(e.pos[l[(longest + 1) % 3]] - e.pos[l[longest]]))
                longest = k;
        const uint32_t a = l[longest], b = l[(longest + 1) % 3], c = l[(longest + 2) % 3];
        for (const auto& [g, k] : edges.at(edgeKey(a, b))) {
            if (g == f || !tris.count(g) || used.count(g) || dot(e.normal(f), e.normal(g)) < 0.99f)
                continue;
            uint32_t d = ~0u;
            for (uint32_t v : e.loops[g])
                if (v != a && v != b)
                    d = v;
            e.loops[f] = {a, d, b, c};  // (f had a -> b; g has b -> a: d goes between them)
            e.uvs[f].assign(4, float2{0, 0});
            e.uv1[f].clear();
            used.insert(f);
            used.insert(g);
            removed.insert(g);
            ++merged;
            break;
        }
    }
    Editable kept = e.shell();
    for (uint32_t f = 0; f < e.loops.size(); ++f)
        if (!removed.count(f))
            kept.copyFace(e, f);
    out.mesh = pack(kept);
    return merged > 0 || (out.error = "no pair of triangles to join", false);
}

void selectionFrom(const Editable& e, const Json& args, uint32_t& mode, std::set<uint32_t>& faces)
{
    // The given elements as faces (vertices and edges: the faces using them).
    const auto vs = listOf(args, "vertices");
    const auto ks = keysOf(args, "edges");
    const auto fs = listOf(args, "faces");
    mode = !fs.empty() ? 3u : !ks.empty() ? 2u : 1u;
    faces.insert(fs.begin(), fs.end());
    const std::set<uint32_t> vset(vs.begin(), vs.end());
    const std::set<uint64_t> kset(ks.begin(), ks.end());
    if (vset.empty() && kset.empty())
        return;
    for (uint32_t f = 0; f < e.loops.size(); ++f)
        for (size_t k = 0; k < e.loops[f].size(); ++k)
            if (vset.count(e.loops[f][k]) || kset.count(edgeKey(e.loops[f][k], e.loops[f][(k + 1) % e.loops[f].size()])))
                faces.insert(f);
}

// ---- UVs: the faces' settings and corner UVs (the shape stays)

constexpr float kDegrees = 180.0f / 3.14159265358979f;

// The entity's transform from "world" (12 numbers, 3x4 rows), for World faces; null without one.
const float (*worldOf(const Json& args, float (&rows)[3][4]))[4]
{
    const Json* w = args.get("world");
    if (!w || w->array.size() != 12)
        return nullptr;
    for (size_t k = 0; k < 12; ++k)
        rows[k / 4][k % 4] = float(w->array[k].number);
    return rows;
}

float2 sub(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
float angleOf(float2 d) { return std::atan2(d.y, d.x) * kDegrees; }

std::vector<uint32_t> facesOr(const PolyMesh& m, const std::vector<uint32_t>& faces)
{
    std::vector<uint32_t> list;
    for (uint32_t f : faces)
        if (f < m.faces.size())
            list.push_back(f);
    if (faces.empty())
        for (uint32_t f = 0; f < m.faces.size(); ++f)
            list.push_back(f);
    return list;
}

// A face's UVs made its corners' own (Manual, no offset, scale or rotation): it looks the same.
void bake(PolyMesh& m, uint32_t f, const float (*world)[4], bool always = false)
{
    if (m.faces[f].uv.mode == UvMode::Manual && !always)
        return;
    const std::vector<float2> uvs = faceUvs(m, f, world);
    if (m.cornerUvs.empty())
        m.cornerUvs.assign(m.corners.size(), float2{0, 0});
    Face& face = m.faces[f];
    for (uint32_t k = 0; k < face.count; ++k)
        m.cornerUvs[face.first + k] = uvs[k];
    face.uv = FaceUv{};
    face.uv.mode = UvMode::Manual;
}

// The face's UVs before its offset and rotation (projected ones: before its scale too).
std::vector<float2> rawUvs(const PolyMesh& m, uint32_t f, const float (*world)[4])
{
    FaceUv raw;
    raw.mode = m.faces[f].uv.mode;
    if (raw.mode == UvMode::Manual)
        raw.scale[0] = m.faces[f].uv.scale[0], raw.scale[1] = m.faces[f].uv.scale[1];
    return faceUvs(m, f, raw, world);
}

// Each face's texture once across it: its UVs' bounds become 0..1.
void uvFit(PolyMesh& m, const std::vector<uint32_t>& faces, const float (*world)[4])
{
    for (uint32_t f : faces) {
        if (m.faces[f].uv.mode == UvMode::Manual)
            bake(m, f, world, true);  // (rotation and scale into the corners: fitting scales the axes)
        FaceUv& uv = m.faces[f].uv;
        uv.offset[0] = uv.offset[1] = 0.0f;
        const std::vector<float2> uvs = faceUvs(m, f, world);
        float2 lo = {1e30f, 1e30f}, hi = {-1e30f, -1e30f};
        for (float2 p : uvs) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
        const float size[2] = {hi.x - lo.x, hi.y - lo.y}, low[2] = {lo.x, lo.y};
        for (int a = 0; a < 2; ++a) {
            if (size[a] < 1e-6f)
                continue;
            uv.scale[a] = uv.mode == UvMode::Manual ? uv.scale[a] / size[a] : uv.scale[a] * size[a];
            uv.offset[a] = -low[a] / size[a];
        }
    }
}

// Each face's texture turned so u runs along an edge: the given edge where the face has it, else
// its longest.
void uvAlign(PolyMesh& m, const std::vector<uint32_t>& faces, const std::vector<uint64_t>& edges, const float (*world)[4])
{
    const std::set<uint64_t> wanted(edges.begin(), edges.end());
    for (uint32_t f : faces) {
        const Face& face = m.faces[f];
        uint32_t best = 0;
        float bestLength = -1.0f;
        for (uint32_t k = 0; k < face.count; ++k) {
            const uint32_t a = m.corners[face.first + k], b = m.corners[face.first + (k + 1) % face.count];
            const float l = length(m.positions[b] - m.positions[a]) + (wanted.count(edgeKey(a, b)) ? 1e6f : 0.0f);
            if (l > bestLength)
                bestLength = l, best = k;
        }
        const std::vector<float2> raw = rawUvs(m, f, world);
        m.faces[f].uv.rotation = -angleOf(sub(raw[(best + 1) % face.count], raw[best]));
    }
}

// The faces' textures carried on from the first one across their shared edges (a strip, a room's
// walls): each face reached turned and moved so its UVs meet its neighbour's along the edge.
void uvMatch(PolyMesh& m, const std::vector<uint32_t>& faces, const float (*world)[4])
{
    if (faces.size() < 2)
        return;
    std::set<uint32_t> waiting(faces.begin() + 1, faces.end());
    std::vector<uint32_t> queue = {faces[0]};
    const auto cornerOfVertex = [&](uint32_t f, uint32_t v) {
        for (uint32_t k = 0; k < m.faces[f].count; ++k)
            if (m.corners[m.faces[f].first + k] == v)
                return int32_t(k);
        return int32_t(-1);
    };
    while (!queue.empty() && !waiting.empty()) {
        const uint32_t f = queue.front();
        queue.erase(queue.begin());
        const std::vector<float2> from = faceUvs(m, f, world);
        const Face& ff = m.faces[f];
        for (uint32_t k = 0; k < ff.count; ++k) {
            const uint32_t a = m.corners[ff.first + k], b = m.corners[ff.first + (k + 1) % ff.count];
            for (auto it = waiting.begin(); it != waiting.end();) {
                const uint32_t g = *it;
                const int32_t ga = cornerOfVertex(g, a), gb = cornerOfVertex(g, b);
                if (ga < 0 || gb < 0) {
                    ++it;
                    continue;
                }
                FaceUv& uv = m.faces[g].uv;
                const bool manual = uv.mode == UvMode::Manual;
                if (!manual && ff.uv.mode != UvMode::Manual)
                    uv.scale[0] = ff.uv.scale[0], uv.scale[1] = ff.uv.scale[1];
                const std::vector<float2> raw = rawUvs(m, g, world);
                float2 want = sub(from[(k + 1) % ff.count], from[k]);
                if (!manual)
                    want = {want.x * uv.scale[0], want.y * uv.scale[1]};
                uv.rotation = angleOf(want) - angleOf(sub(raw[size_t(gb)], raw[size_t(ga)]));
                uv.offset[0] = uv.offset[1] = 0.0f;
                const float2 at = faceUvs(m, g, world)[size_t(ga)];
                uv.offset[0] = from[k].x - at.x;
                uv.offset[1] = from[k].y - at.y;
                queue.push_back(g);
                it = waiting.erase(it);
            }
        }
    }
}

// The faces unfolded flat into charts (xatlas), as Manual UVs at the faces' texel density (a
// metre: one tile at their scale), packed apart.
bool uvUnwrap(PolyMesh& m, const std::vector<uint32_t>& faces, OpResult& out)
{
    const Editable e = unpack(m);
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> cornerOf;  // per triangle corner: face, corner
    for (uint32_t f : faces) {
        const std::vector<uint32_t>& loop = e.loops[f];
        const std::vector<uint32_t> tris = loop.size() == 3 ? std::vector<uint32_t>{0, 1, 2} : earClip(e, loop, e.normal(f));
        for (uint32_t c : tris) {
            indices.push_back(loop[c]);
            cornerOf.push_back({f, c});
        }
    }
    if (indices.empty())
        return out.error = "no faces", false;
    constexpr float kTexelsPerMetre = 64.0f;
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexPositionData = m.positions.data();
    decl.vertexCount = uint32_t(m.positions.size());
    decl.vertexPositionStride = sizeof(float3);
    decl.indexData = indices.data();
    decl.indexCount = uint32_t(indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    if (xatlas::AddMesh(atlas, decl, 1) != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return out.error = "xatlas refused the faces", false;
    }
    xatlas::PackOptions pack;
    pack.texelsPerUnit = kTexelsPerMetre;
    pack.padding = 2;
    xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
    const xatlas::Mesh& charts = atlas->meshes[0];
    if (m.cornerUvs.empty())
        m.cornerUvs.assign(m.corners.size(), float2{0, 0});
    for (size_t i = 0; i < cornerOf.size() && i < charts.indexCount; ++i) {
        const xatlas::Vertex& v = charts.vertexArray[charts.indexArray[i]];
        const auto [f, c] = cornerOf[i];
        m.cornerUvs[m.faces[f].first + c] = {v.uv[0] / kTexelsPerMetre, v.uv[1] / kTexelsPerMetre};
    }
    xatlas::Destroy(atlas);
    for (uint32_t f : faces) {
        FaceUv& uv = m.faces[f].uv;
        const float density[2] = {uv.mode == UvMode::Manual ? 1.0f : 1.0f / (uv.scale[0] != 0.0f ? uv.scale[0] : 1.0f),
                                  uv.mode == UvMode::Manual ? 1.0f : 1.0f / (uv.scale[1] != 0.0f ? uv.scale[1] : 1.0f)};
        uv = FaceUv{};
        uv.mode = UvMode::Manual;
        uv.scale[0] = std::fabs(density[0]);
        uv.scale[1] = std::fabs(density[1]);
    }
    return true;
}

// Faces onto a trim strip (v0..v1, the texture's full width repeating along u): each face's u along
// its longest edge (or a given one), its extent across that edge filling the strip, u at square
// texels (aspect: the texture's width / height). Faces in a run (a bevel's strip, a ring round a
// pillar) carry u on across their shared edges, turned and mirrored to meet.
void uvTrim(PolyMesh& m, const std::vector<uint32_t>& faces, const std::vector<uint64_t>& edges, float v0, float v1, float aspect)
{
    const std::set<uint64_t> wanted(edges.begin(), edges.end());
    const float height = v1 - v0;
    if (m.cornerUvs.empty())
        m.cornerUvs.assign(m.corners.size(), float2{0, 0});
    for (uint32_t f : faces) {
        Face& face = m.faces[f];
        uint32_t best = 0;
        float bestLength = -1.0f;
        for (uint32_t k = 0; k < face.count; ++k) {
            const uint32_t a = m.corners[face.first + k], b = m.corners[face.first + (k + 1) % face.count];
            const float l = length(m.positions[b] - m.positions[a]) + (wanted.count(edgeKey(a, b)) ? 1e6f : 0.0f);
            if (l > bestLength)
                bestLength = l, best = k;
        }
        const float3 a = m.positions[m.corners[face.first + best]], e = normalize(m.positions[m.corners[face.first + (best + 1) % face.count]] - a);
        const float3 w = normalize(cross(m.faceNormal(f), e));  // (into the face from the edge)
        float smin = 1e30f, tmin = 1e30f, tmax = -1e30f;
        for (uint32_t k = 0; k < face.count; ++k) {
            const float3 d = m.positions[m.corners[face.first + k]] - a;
            smin = std::min(smin, dot(d, e));
            tmin = std::min(tmin, dot(d, w));
            tmax = std::max(tmax, dot(d, w));
        }
        const float across = std::max(tmax - tmin, 1e-6f), perMetre = height / (across * aspect);
        for (uint32_t k = 0; k < face.count; ++k) {
            const float3 d = m.positions[m.corners[face.first + k]] - a;
            m.cornerUvs[face.first + k] = {(dot(d, e) - smin) * perMetre, v0 + (dot(d, w) - tmin) / across * height};
        }
        face.uv = FaceUv{};
        face.uv.mode = UvMode::Manual;
    }
    // The run: from the first face, each neighbour (sharing an edge across the strip, where u is the
    // same at both ends) continues from its end of the strip.
    std::set<uint32_t> waiting(faces.begin(), faces.end());
    if (waiting.empty())
        return;
    std::vector<uint32_t> queue = {faces[0]};
    waiting.erase(faces[0]);
    const auto cornerOfVertex = [&](uint32_t f, uint32_t v) {
        for (uint32_t k = 0; k < m.faces[f].count; ++k)
            if (m.corners[m.faces[f].first + k] == v)
                return int32_t(k);
        return int32_t(-1);
    };
    const auto uRange = [&](uint32_t f, float& lo, float& hi) {
        lo = 1e30f, hi = -1e30f;
        for (uint32_t k = 0; k < m.faces[f].count; ++k)
            lo = std::min(lo, m.cornerUvs[m.faces[f].first + k].x), hi = std::max(hi, m.cornerUvs[m.faces[f].first + k].x);
    };
    while (!queue.empty()) {
        const uint32_t f = queue.front();
        queue.erase(queue.begin());
        const Face& ff = m.faces[f];
        float flo, fhi;
        uRange(f, flo, fhi);
        for (uint32_t k = 0; k < ff.count; ++k) {
            const uint32_t va = m.corners[ff.first + k], vb = m.corners[ff.first + (k + 1) % ff.count];
            const float2 fa = m.cornerUvs[ff.first + k], fb = m.cornerUvs[ff.first + (k + 1) % ff.count];
            if (std::fabs(fa.x - fb.x) > 1e-4f)
                continue;  // (along the strip: a neighbouring strip, not the run)
            for (auto it = waiting.begin(); it != waiting.end();) {
                const uint32_t g = *it;
                const int32_t ga = cornerOfVertex(g, va), gb = cornerOfVertex(g, vb);
                if (ga < 0 || gb < 0) {
                    ++it;
                    continue;
                }
                const Face& fg = m.faces[g];
                float2* uv = &m.cornerUvs[fg.first];
                // The run goes on from f's end: g starts where f ends (or ends where f starts).
                float glo, ghi;
                uRange(g, glo, ghi);
                const bool fEnds = std::fabs(fa.x - fhi) < std::fabs(fa.x - flo);
                const bool gStarts = std::fabs(uv[ga].x - glo) < std::fabs(uv[ga].x - ghi);
                if (fEnds != gStarts)
                    for (uint32_t c = 0; c < fg.count; ++c)
                        uv[c].x = -uv[c].x;
                const float shift = fa.x - uv[ga].x;
                for (uint32_t c = 0; c < fg.count; ++c)
                    uv[c].x += shift;
                if (std::fabs(uv[ga].y - fa.y) > 1e-4f)  // (upside down to f: v flipped within the strip)
                    for (uint32_t c = 0; c < fg.count; ++c)
                        uv[c].y = v0 + v1 - uv[c].y;
                queue.push_back(g);
                it = waiting.erase(it);
            }
        }
    }
}

// The mask's UVs (cornerUv1): every face unfolded into charts (xatlas), packed into 0..1 apart.
bool maskUnwrap(PolyMesh& m, OpResult& out)
{
    const Editable e = unpack(m);
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> cornerOf;
    for (uint32_t f = 0; f < e.loops.size(); ++f) {
        const std::vector<uint32_t>& loop = e.loops[f];
        const std::vector<uint32_t> tris = loop.size() == 3 ? std::vector<uint32_t>{0, 1, 2} : earClip(e, loop, e.normal(f));
        for (uint32_t c : tris) {
            indices.push_back(loop[c]);
            cornerOf.push_back({f, c});
        }
    }
    if (indices.empty())
        return out.error = "no faces", false;
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexPositionData = m.positions.data();
    decl.vertexCount = uint32_t(m.positions.size());
    decl.vertexPositionStride = sizeof(float3);
    decl.indexData = indices.data();
    decl.indexCount = uint32_t(indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    if (xatlas::AddMesh(atlas, decl, 1) != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return out.error = "xatlas refused the faces", false;
    }
    xatlas::PackOptions pack;
    pack.resolution = 512;  // (a 512² mask: charts kept 4 texels apart)
    pack.padding = 4;
    pack.bilinear = true;
    xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
    const xatlas::Mesh& charts = atlas->meshes[0];
    // (into the lower 80% each way: room left for the charts of faces made later, maskFill)
    const float w = float(std::max({atlas->width, atlas->height, 1u})) / 0.8f, h = w;
    m.cornerUv1.assign(m.corners.size(), float2{0, 0});
    for (size_t i = 0; i < cornerOf.size() && i < charts.indexCount; ++i) {
        const xatlas::Vertex& v = charts.vertexArray[charts.indexArray[i]];
        const auto [f, c] = cornerOf[i];
        m.cornerUv1[m.faces[f].first + c] = {v.uv[0] / w, v.uv[1] / h};
    }
    const bool spilled = atlas->atlasCount > 1;
    xatlas::Destroy(atlas);
    if (spilled)
        out.error = "the mask's charts did not fit one 512 texture: some overlap";
    return true;
}

// Colours of the given vertices (faces: theirs; none: all), RGBA8 from "color": [r, g, b, a] 0..1.
void vertexColor(PolyMesh& m, const std::vector<uint32_t>& vertices, const std::vector<uint32_t>& faces, const Json& args)
{
    float rgba[4] = {1, 1, 1, 1};
    if (const Json* c = args.get("color"))
        for (size_t k = 0; k < 4 && k < c->array.size(); ++k)
            rgba[k] = std::clamp(float(c->array[k].number), 0.0f, 1.0f);
    uint32_t packed = 0;
    for (int k = 0; k < 4; ++k)
        packed |= uint32_t(rgba[k] * 255.0f + 0.5f) << (8 * k);
    if (m.vertexColors.empty())
        m.vertexColors.assign(m.positions.size(), 0xffffffffu);
    std::set<uint32_t> chosen(vertices.begin(), vertices.end());
    for (uint32_t f : faces)
        if (f < m.faces.size())
            for (uint32_t k = 0; k < m.faces[f].count; ++k)
                chosen.insert(m.corners[m.faces[f].first + k]);
    for (uint32_t v = 0; v < m.positions.size(); ++v)
        if ((vertices.empty() && faces.empty()) || chosen.count(v))
            m.vertexColors[v] = packed;
    if (std::all_of(m.vertexColors.begin(), m.vertexColors.end(), [](uint32_t c) { return c == 0xffffffffu; }))
        m.vertexColors.clear();  // (all white: none)
}

bool uvOp(const std::string& op, const PolyMesh& in, const Json& args, const std::vector<uint32_t>& given, OpResult& out)
{
    PolyMesh m = in;
    float rows[3][4];
    const float (*world)[4] = worldOf(args, rows);
    const std::vector<uint32_t> faces = facesOr(m, given);
    const auto pair = [&](const char* key, float (&v)[2]) {
        const Json* a = args.get(key);
        if (!a || a->array.size() != 2)
            return false;
        v[0] = float(a->array[0].number), v[1] = float(a->array[1].number);
        return true;
    };
    if (op == "uv") {
        for (uint32_t f : faces) {
            if (const std::string mode = args.getString("mode"); !mode.empty()) {
                const UvMode to = mode == "world" ? UvMode::World : mode == "manual" ? UvMode::Manual : UvMode::Local;
                if (to == UvMode::Manual)
                    bake(m, f, world);  // (from a projection: the same UVs, now its own)
                m.faces[f].uv.mode = to;
            }
            FaceUv& uv = m.faces[f].uv;
            pair("offset", uv.offset);
            pair("scale", uv.scale);
            if (args.get("rotation"))
                uv.rotation = float(args.getNumber("rotation"));
        }
    } else if (op == "uvNudge") {
        float offset[2] = {0, 0}, scale[2] = {1, 1};
        pair("offset", offset);
        pair("scale", scale);
        const float turn = float(args.getNumber("rotation", 0.0));
        for (uint32_t f : faces) {
            FaceUv& uv = m.faces[f].uv;
            uv.offset[0] += offset[0], uv.offset[1] += offset[1];
            uv.scale[0] *= scale[0], uv.scale[1] *= scale[1];
            uv.rotation = std::remainder(uv.rotation + turn, 360.0f);
        }
    } else if (op == "uvReset") {
        for (uint32_t f : faces)
            m.faces[f].uv = FaceUv{};
    } else if (op == "uvFit") {
        uvFit(m, faces, world);
    } else if (op == "uvAlign") {
        uvAlign(m, faces, keysOf(args, "edges"), world);
    } else if (op == "uvMatch") {
        if (faces.size() < 2)
            return out.error = "two faces or more (the first one's texture carries on)", false;
        uvMatch(m, faces, world);
    } else if (op == "uvUnwrap") {
        if (!uvUnwrap(m, faces, out))
            return false;
    } else if (op == "layers") {
        // {"materials": [a, b, c] ("" none), "blend"}: the mesh's layers; its mask UVs made when it has none.
        if (const Json* mats = args.get("materials"))
            for (size_t k = 0; k < 3; ++k)
                m.layerMaterials[k] = k < mats->array.size() ? mats->array[k].str : std::string();
        if (args.get("blend"))
            m.layerBlend = std::clamp(float(args.getNumber("blend")), 0.0f, 1.0f);
        if (m.layered() && m.cornerUv1.empty() && !maskUnwrap(m, out))
            return false;
    } else if (op == "maskUnwrap") {
        if (!maskUnwrap(m, out))
            return false;
    } else if (op == "vertexColor") {
        vertexColor(m, listOf(args, "vertices"), given.empty() && args.get("vertices") ? std::vector<uint32_t>{} : faces, args);
    } else if (op == "trim") {
        const float v0 = float(args.getNumber("v0", 0.0)), v1 = float(args.getNumber("v1", 1.0));
        if (!(v1 > v0))
            return out.error = "a trim strip runs from v0 down to a larger v1", false;
        uvTrim(m, faces, keysOf(args, "edges"), v0, v1, std::max(float(args.getNumber("aspect", 1.0)), 1e-3f));
        if (const Json* material = args.get("material"); material && material->type == Json::Type::String) {  // (the sheet's)
            const auto it = std::find(m.materials.begin(), m.materials.end(), material->str);
            const uint32_t slot = uint32_t(it - m.materials.begin());
            if (it == m.materials.end())
                m.materials.push_back(material->str);
            for (uint32_t f : faces)
                m.faces[f].material = slot;
        }
    } else {
        return false;
    }
    if (std::none_of(m.faces.begin(), m.faces.end(), [](const Face& f) { return f.uv.mode == UvMode::Manual; }))
        m.cornerUvs.clear();  // (only Manual faces read them)
    out.mesh = std::move(m);
    return true;
}

}  // namespace

float3 bevelProfile(float3 p, float3 corner, float3 q, float t, float curve)
{
    const float3 flat = p + (q - p) * t;
    const float3 a = p - corner, b = q - corner;
    const float la = length(a), lb = length(b);
    if (la < 1e-7f || lb < 1e-7f || curve == 0.0f)
        return flat;
    // A rational quadratic Bezier over p, the corner and q, weighted by the sine of half the angle at
    // the corner: a circle's arc touching both lines.
    const float cosine = std::clamp(dot(a, b) / (la * lb), -1.0f, 1.0f);
    const float w = std::sqrt(std::max(0.0f, (1.0f - cosine) * 0.5f));
    const float u = 1.0f - t, wp = u * u, wc = 2.0f * t * u * w, wq = t * t;
    const float3 round = (p * wp + corner * wc + q * wq) * (1.0f / (wp + wc + wq));
    // Toward its place on the flat cut (curve 0) and past it (-1: the arc mirrored, a cove).
    const float3 onCut = nearestOnLine(round, p, q);
    return onCut + (round - onCut) * curve;
}

float3 nearestOnLine(float3 p, float3 a, float3 b)
{
    const float3 d = b - a;
    const float l2 = dot(d, d);
    return l2 < 1e-12f ? a : a + d * (dot(p - a, d) / l2);
}

// (runOp's work, before the new faces get mask UVs)
bool runOpOn(const std::string& op, const PolyMesh& in, const Json& args, OpResult& out);

bool runOp(const std::string& op, const PolyMesh& in, const Json& args, OpResult& out)
{
    if (!runOpOn(op, in, args, out))
        return false;
    if (op != "maskUnwrap")
        maskFill(out.mesh, &in);
    return true;
}

namespace {

bool maskMissing(const PolyMesh& m, uint32_t f)
{
    const Face& face = m.faces[f];
    if (face.count < 3)
        return false;
    const float2 first = m.cornerUv1[face.first];
    for (uint32_t k = 1; k < face.count; ++k) {
        const float2 c = m.cornerUv1[face.first + k];
        if (c.x != first.x || c.y != first.y)
            return false;
    }
    return true;
}

// The mask texels a mesh's charts cover (theirs, and `pad` around), into `taken` (512 x 512).
void markCharts(const PolyMesh& m, std::vector<uint8_t>& taken, int pad)
{
    const int size = 512;
    if (m.cornerUv1.size() != m.corners.size())
        return;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        if (maskMissing(m, f))
            continue;
        const Face& face = m.faces[f];
        for (uint32_t k = 1; k + 1 < face.count; ++k) {
            const auto texels = [&](uint32_t c) { return float2{m.cornerUv1[c].x * float(size), m.cornerUv1[c].y * float(size)}; };
            const float2 t[3] = {texels(face.first), texels(face.first + k), texels(face.first + k + 1)};
            const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
            const int x0 = std::max(int(std::floor(std::min({t[0].x, t[1].x, t[2].x}))) - pad, 0);
            const int x1 = std::min(int(std::ceil(std::max({t[0].x, t[1].x, t[2].x}))) + pad, size - 1);
            const int y0 = std::max(int(std::floor(std::min({t[0].y, t[1].y, t[2].y}))) - pad, 0);
            const int y1 = std::min(int(std::ceil(std::max({t[0].y, t[1].y, t[2].y}))) + pad, size - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    // (inside, or within `pad` texels of an edge: the distance to each side)
                    bool near = std::fabs(area) < 1e-9f;
                    if (!near) {
                        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                        float worst = 1e30f;
                        for (int i = 0; i < 3; ++i) {
                            const float2 a = t[i], b = t[(i + 1) % 3];
                            const float2 ab = {b.x - a.x, b.y - a.y};
                            const float len = std::sqrt(ab.x * ab.x + ab.y * ab.y);
                            if (len < 1e-6f)
                                continue;
                            // signed: positive inside (for either winding)
                            const float side = ((px - a.x) * ab.y - (py - a.y) * ab.x) / len * (area > 0.0f ? -1.0f : 1.0f);
                            worst = std::min(worst, side);
                        }
                        near = worst >= -float(pad) - 0.75f;
                    }
                    if (near)
                        taken[size_t(y) * size + size_t(x)] = 1;
                }
        }
    }
}

}  // namespace

bool maskFill(PolyMesh& m, const PolyMesh* before)
{
    if (m.cornerUv1.size() != m.corners.size() || m.corners.empty())
        return false;
    std::vector<uint32_t> missing;
    for (uint32_t f = 0; f < m.faces.size(); ++f)
        if (maskMissing(m, f))
            missing.push_back(f);
    if (missing.empty())
        return false;
    const int size = 512, pad = 3;
    // What is taken, and the density there (texels per metre).
    std::vector<uint8_t> taken(size_t(size) * size, 0);
    markCharts(m, taken, pad);
    if (before)
        markCharts(*before, taken, pad);
    double uvArea = 0.0, area = 0.0;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        if (maskMissing(m, f))
            continue;
        const Face& face = m.faces[f];
        for (uint32_t k = 1; k + 1 < face.count; ++k) {
            const float2 a = m.cornerUv1[face.first], b = m.cornerUv1[face.first + k], c = m.cornerUv1[face.first + k + 1];
            uvArea += 0.5 * std::fabs(double((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y))) * size * size;
            const float3 p = m.positions[m.corners[face.first]], q = m.positions[m.corners[face.first + k]],
                         r = m.positions[m.corners[face.first + k + 1]];
            area += 0.5 * double(length(cross(q - p, r - p)));
        }
    }
    const float density = area > 1e-9 && uvArea > 1e-9 ? float(std::sqrt(uvArea / area)) : 64.0f;
    // Charts for the faces without (xatlas, at that density; their own layout, placed below).
    const Editable e = unpack(m);
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> cornerOf;
    for (const uint32_t f : missing) {
        const std::vector<uint32_t>& loop = e.loops[f];
        const std::vector<uint32_t> tris = loop.size() == 3 ? std::vector<uint32_t>{0, 1, 2} : earClip(e, loop, e.normal(f));
        for (uint32_t c : tris) {
            indices.push_back(loop[c]);
            cornerOf.push_back({f, c});
        }
    }
    if (indices.empty())
        return false;
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexPositionData = m.positions.data();
    decl.vertexCount = uint32_t(m.positions.size());
    decl.vertexPositionStride = sizeof(float3);
    decl.indexData = indices.data();
    decl.indexCount = uint32_t(indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    if (xatlas::AddMesh(atlas, decl, 1) != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return false;
    }
    xatlas::PackOptions options;
    options.texelsPerUnit = density;
    options.padding = 0;
    xatlas::Generate(atlas, xatlas::ChartOptions(), options);
    const xatlas::Mesh& made = atlas->meshes[0];
    // Each chart's box (texels), biggest first.
    struct Chart {
        float2 lo = {1e30f, 1e30f}, hi = {-1e30f, -1e30f};
        int x = 0, y = 0;
    };
    std::vector<Chart> charts(made.chartCount);
    for (uint32_t i = 0; i < made.vertexCount; ++i) {
        const xatlas::Vertex& v = made.vertexArray[i];
        if (v.chartIndex < 0 || uint32_t(v.chartIndex) >= charts.size())
            continue;
        Chart& c = charts[size_t(v.chartIndex)];
        c.lo = {std::min(c.lo.x, v.uv[0]), std::min(c.lo.y, v.uv[1])};
        c.hi = {std::max(c.hi.x, v.uv[0]), std::max(c.hi.y, v.uv[1])};
    }
    std::vector<uint32_t> order(charts.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return charts[a].hi.y - charts[a].lo.y > charts[b].hi.y - charts[b].lo.y; });
    // First fit into the free texels (a summed table of what's taken), at a half, then a quarter of
    // the density when they don't.
    float scale = 1.0f;
    bool placed = false;
    std::vector<uint32_t> sum(size_t(size + 1) * (size + 1), 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            sum[size_t(y + 1) * (size + 1) + size_t(x + 1)] = taken[size_t(y) * size + size_t(x)] + sum[size_t(y) * (size + 1) + size_t(x + 1)] +
                                                            sum[size_t(y + 1) * (size + 1) + size_t(x)] - sum[size_t(y) * (size + 1) + size_t(x)];
    struct Rect {
        int x, y, w, h;
    };
    std::vector<Rect> used;
    for (int attempt = 0; attempt < 5 && !placed; ++attempt, scale *= 0.5f) {
        used.clear();
        placed = true;
        for (const uint32_t i : order) {
            Chart& c = charts[i];
            if (c.lo.x > c.hi.x)
                continue;
            const int w = int(std::ceil((c.hi.x - c.lo.x) * scale)) + 1 + 2 * pad, h = int(std::ceil((c.hi.y - c.lo.y) * scale)) + 1 + 2 * pad;
            const auto busy = [&](int x, int y) {
                if (sum[size_t(y + h) * (size + 1) + size_t(x + w)] - sum[size_t(y) * (size + 1) + size_t(x + w)] -
                    sum[size_t(y + h) * (size + 1) + size_t(x)] + sum[size_t(y) * (size + 1) + size_t(x)])
                    return true;
                for (const Rect& r : used)
                    if (x < r.x + r.w && r.x < x + w && y < r.y + r.h && r.y < y + h)
                        return true;
                return false;
            };
            bool found = false;
            for (int y = 0; y + h <= size && !found; ++y)
                for (int x = 0; x + w <= size && !found; ++x)
                    if (!busy(x, y)) {
                        c.x = x, c.y = y, found = true;
                        used.push_back({x, y, w, h});
                    }
            if (!found) {
                placed = false;
                break;
            }
        }
        if (placed)
            break;  // (scale: the one they fit at)
    }
    if (!placed) {
        xatlas::Destroy(atlas);
        OpResult ignored;
        return maskUnwrap(m, ignored);
    }
    for (size_t i = 0; i < cornerOf.size() && i < made.indexCount; ++i) {
        const xatlas::Vertex& v = made.vertexArray[made.indexArray[i]];
        if (v.chartIndex < 0 || uint32_t(v.chartIndex) >= charts.size())
            continue;
        const Chart& c = charts[size_t(v.chartIndex)];
        const auto [f, k] = cornerOf[i];
        m.cornerUv1[m.faces[f].first + k] = {(float(c.x + pad) + (v.uv[0] - c.lo.x) * scale + 0.5f) / float(size),
                                             (float(c.y + pad) + (v.uv[1] - c.lo.y) * scale + 0.5f) / float(size)};
    }
    xatlas::Destroy(atlas);
    return true;
}

bool runOpOn(const std::string& op, const PolyMesh& in, const Json& args, OpResult& out)
{
    out = {};
    if (op == "shape") {
        out.mesh = makeShape(args.getString("name", "box"), args, &out.error);
        return out.error.empty();
    }
    if (op == "wall") {  // a wall's mesh from its parameters (level/walls.h; "points": [[x, z], ...])
        Wall w;
        if (const Json* pts = args.get("points"))
            for (const Json& p : pts->array)
                if (p.array.size() >= 2)
                    w.points.push_back({float(p.array[0].number), float(p.array[1].number)});
        w.closed = args.getBool("closed");
        w.height = float(args.getNumber("height", w.height));
        w.thickness = float(args.getNumber("thickness", w.thickness));
        w.align = uint8_t(args.getNumber("align", 0));
        w.floor = args.getBool("floor");
        w.ceiling = args.getBool("ceiling");
        out.mesh = buildWall(w);
        return !out.mesh.faces.empty() || (out.error = "a wall needs two points or more", false);
    }
    if (!in.valid(&out.error))
        return false;
    Editable e = unpack(in);
    const auto faces = listOf(args, "faces");
    const auto vertices = listOf(args, "vertices");
    const auto edgeKeys = keysOf(args, "edges");
    if (op == "extrude")
        return extrude(e, faces, float(args.getNumber("distance", 0.5)), out);
    if (op == "extrudeEdges")
        return extrudeEdges(e, edgeKeys, float(args.getNumber("distance", 0.5)), out);
    if (op == "inset")
        return inset(e, faces, float(args.getNumber("amount", 0.1)), out);
    if (op == "bevel")
        return bevel(e, edgeKeys, float(args.getNumber("amount", 0.1)), uint32_t(std::max(1.0, args.getNumber("segments", 1))),
                     float(args.getNumber("curve", 1)), out);
    if (op == "connect")
        return connect(e, edgeKeys, vertices, out);
    if (op == "loop" || op == "ring") {
        std::set<uint64_t> all;
        for (uint64_t key : edgeKeys)
            for (uint64_t k : op == "loop" ? loopOf(e, key) : ringOf(e, key))
                all.insert(k);
        out.mesh = in;
        out.selectionMode = 2;
        out.selection.assign(all.begin(), all.end());
        return !all.empty();
    }
    if (op == "loopCut") {
        std::set<uint64_t> all;
        for (uint64_t key : edgeKeys)
            for (uint64_t k : ringOf(e, key))
                all.insert(k);
        return connect(e, std::vector<uint64_t>(all.begin(), all.end()), {}, out);
    }
    if (op == "subdivide")
        return subdivide(e, faces, out);
    if (op == "merge")
        return mergeVertices(e, vertices, out);
    if (op == "weld")
        return weld(e, vertices, float(args.getNumber("distance", 0.01)), out);
    if (op == "fill")
        return fill(e, edgeKeys, vertices, out);
    if (op == "delete")
        return remove(e, faces, edgeKeys, vertices, out);
    if (op == "detach")
        return remove(e, faces, {}, {}, out, true);
    if (op == "flip") {
        std::vector<uint32_t> list = faces;
        if (list.empty())
            for (uint32_t f = 0; f < e.loops.size(); ++f)
                list.push_back(f);
        for (uint32_t f : list)
            if (f < e.loops.size())
                flipFace(e, f);
        out.mesh = pack(e);
        out.selectionMode = faces.empty() ? 0u : 3u;
        out.selection.assign(faces.begin(), faces.end());
        return true;
    }
    if (op == "conform")
        return conform(e, out);
    if (op == "triangulate")
        return triangulate(e, faces, out);
    if (op == "quadrangulate")
        return quadrangulate(e, faces, out);
    if (op == "mergeFaces")
        return mergeFaces(e, faces, out);
    if (op == "smooth" || op == "autoSmooth") {
        if (op == "smooth") {
            for (uint32_t f : faces)
                if (f < e.attrs.size())
                    e.attrs[f].smoothing = uint32_t(args.getNumber("group", 1));
        } else {
            // Faces meeting at under `angle` share a group (flood fill); the rest flat.
            const float limit = std::cos(float(args.getNumber("angle", 40.0)) * 3.14159265f / 180.0f);
            const auto edges = e.edges();
            std::vector<uint32_t> group(e.loops.size(), 0);
            uint32_t next = 1;
            for (uint32_t s = 0; s < e.loops.size(); ++s) {
                if (group[s])
                    continue;
                group[s] = next;
                std::vector<uint32_t> stack = {s};
                while (!stack.empty()) {
                    const uint32_t f = stack.back();
                    stack.pop_back();
                    for (size_t k = 0; k < e.loops[f].size(); ++k)
                        for (const auto& [g, c] : edges.at(edgeKey(e.loops[f][k], e.loops[f][(k + 1) % e.loops[f].size()])))
                            if (!group[g] && dot(e.normal(f), e.normal(g)) >= limit) {
                                group[g] = next;
                                stack.push_back(g);
                            }
                }
                ++next;
            }
            std::map<uint32_t, uint32_t> sizes;
            for (uint32_t g : group)
                ++sizes[g];
            for (uint32_t f = 0; f < e.attrs.size(); ++f)
                e.attrs[f].smoothing = sizes[group[f]] > 1 ? group[f] : 0;
        }
        out.mesh = pack(e);
        return true;
    }
    if (op == "material") {
        uint32_t slot = uint32_t(args.getNumber("slot", 0));
        if (const std::string path = args.getString("path"); args.get("path")) {
            const auto it = std::find(e.materials.begin(), e.materials.end(), path);
            slot = it != e.materials.end() ? uint32_t(it - e.materials.begin()) : uint32_t(e.materials.size());
            if (it == e.materials.end())
                e.materials.push_back(path);
        }
        if (slot >= std::max<size_t>(e.materials.size(), 1))
            return out.error = "no such material slot", false;
        float size[2] = {0.0f, 0.0f};  // (the material's real-world size: projected faces' metres per tile)
        if (const Json* s = args.get("size"); s && s->array.size() == 2)
            size[0] = float(s->array[0].number), size[1] = float(s->array[1].number);
        for (uint32_t f = 0; f < e.attrs.size(); ++f)  // (without faces: all of them)
            if (faces.empty() || std::find(faces.begin(), faces.end(), f) != faces.end()) {
                e.attrs[f].material = slot;
                if (size[0] > 0.0f && size[1] > 0.0f && e.attrs[f].uv.mode != UvMode::Manual)
                    e.attrs[f].uv.scale[0] = size[0], e.attrs[f].uv.scale[1] = size[1];
            }
        out.mesh = pack(e);
        return true;
    }
    if (op == "uv" || op == "uvNudge" || op == "uvReset" || op == "uvFit" || op == "uvAlign" || op == "uvMatch" || op == "uvUnwrap" || op == "trim" ||
        op == "layers" || op == "maskUnwrap" || op == "vertexColor")
        return uvOp(op, in, args, faces, out);
    if (op == "grow" || op == "shrink" || op == "byMaterial" || op == "byNormal") {
        uint32_t mode;
        std::set<uint32_t> region;
        selectionFrom(e, args, mode, region);
        std::set<uint32_t> result = region;
        const auto edges = e.edges();
        if (op == "grow" || op == "shrink") {
            for (uint32_t f : region) {
                for (size_t k = 0; k < e.loops[f].size(); ++k)
                    for (const auto& [g, c] : edges.at(edgeKey(e.loops[f][k], e.loops[f][(k + 1) % e.loops[f].size()]))) {
                        if (op == "grow")
                            result.insert(g);
                        else if (!region.count(g))
                            result.erase(f);
                    }
            }
        } else {
            const float limit = std::cos(float(args.getNumber("angle", 10.0)) * 3.14159265f / 180.0f);
            for (uint32_t g = 0; g < e.loops.size(); ++g)
                for (uint32_t f : region)
                    if (op == "byMaterial" ? e.attrs[g].material == e.attrs[f].material : dot(e.normal(g), e.normal(f)) >= limit)
                        result.insert(g);
        }
        out.mesh = in;
        out.selectionMode = 3;
        if (mode == 3) {
            out.selection.assign(result.begin(), result.end());
        } else {  // back to vertices or edges: those of the faces
            std::set<uint64_t> items;
            for (uint32_t f : result)
                for (size_t k = 0; k < e.loops[f].size(); ++k)
                    items.insert(mode == 1 ? uint64_t(e.loops[f][k]) : edgeKey(e.loops[f][k], e.loops[f][(k + 1) % e.loops[f].size()]));
            out.selectionMode = mode;
            out.selection.assign(items.begin(), items.end());
        }
        return true;
    }
    out.error = "no operation '" + op + "'";
    return false;
}

}  // namespace fx::level
