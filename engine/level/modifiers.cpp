#include "level/modifiers.h"

#include "core/json.h"
#include "level/mesh_ops.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

namespace fx::level {

namespace {

constexpr float kRadians = 3.14159265f / 180.0f;
const char* const kAxisNames[] = {"X", "Y", "Z"};

float& at(float3& p, uint32_t axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }
float at(const float3& p, uint32_t axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

float3 rotate(float3 p, uint32_t axis, float radians)
{
    const float c = std::cos(radians), s = std::sin(radians);
    const uint32_t a = (axis + 1) % 3, b = (axis + 2) % 3;  // (right-handed: a towards b)
    const float pa = at(p, a), pb = at(p, b);
    at(p, a) = pa * c - pb * s;
    at(p, b) = pa * s + pb * c;
    return p;
}

// An empty mesh with `like`'s slots and layers (what a modifier's result starts from).
PolyMesh emptyLike(const PolyMesh& like)
{
    PolyMesh m;
    m.materials = like.materials;
    for (int k = 0; k < 3; ++k)
        m.layerMaterials[k] = like.layerMaterials[k];
    m.layerBlend = like.layerBlend;
    return m;
}

// Appends `from` (its vertices through `move`, each face wound the other way when `flip`) to `to`,
// each face's source from `fromSource`.
void append(PolyMesh& to, const PolyMesh& from, const std::function<float3(uint32_t, float3)>& move, bool flip,
            const std::vector<uint32_t>& fromSource, std::vector<uint32_t>& toSource)
{
    const uint32_t base = uint32_t(to.positions.size());
    for (uint32_t v = 0; v < from.positions.size(); ++v)
        to.positions.push_back(move(v, from.positions[v]));
    if (!from.vertexColors.empty() || !to.vertexColors.empty()) {
        to.vertexColors.resize(base, 0xffffffffu);
        for (uint32_t v = 0; v < from.positions.size(); ++v)
            to.vertexColors.push_back(from.vertexColors.empty() ? 0xffffffffu : from.vertexColors[v]);
    }
    const bool uvs = !from.cornerUvs.empty() || !to.cornerUvs.empty(), uv1 = !from.cornerUv1.empty() || !to.cornerUv1.empty();
    if (uvs)
        to.cornerUvs.resize(to.corners.size(), float2{0, 0});
    if (uv1)
        to.cornerUv1.resize(to.corners.size(), float2{0, 0});
    for (uint32_t f = 0; f < from.faces.size(); ++f) {
        Face face = from.faces[f];
        face.first = uint32_t(to.corners.size());
        for (uint32_t k = 0; k < face.count; ++k) {
            const uint32_t c = from.faces[f].first + (flip ? (face.count - k) % face.count : k);
            to.corners.push_back(base + from.corners[c]);
            if (uvs)
                to.cornerUvs.push_back(from.cornerUvs.empty() ? float2{0, 0} : from.cornerUvs[c]);
            if (uv1)
                to.cornerUv1.push_back(from.cornerUv1.empty() ? float2{0, 0} : from.cornerUv1[c]);
        }
        to.faces.push_back(face);
        toSource.push_back(f < fromSource.size() ? fromSource[f] : ~0u);
    }
}

// A face over `loop` (vertices of `m`) like face `like` (its material, flat), its corners' UVs none.
void addLike(PolyMesh& m, const std::vector<uint32_t>& loop, uint32_t material, std::vector<uint32_t>& source, uint32_t from)
{
    Face face;
    face.first = uint32_t(m.corners.size());
    face.count = uint32_t(loop.size());
    face.material = material;
    m.corners.insert(m.corners.end(), loop.begin(), loop.end());
    if (!m.cornerUvs.empty())
        m.cornerUvs.resize(m.corners.size(), float2{0, 0});
    if (!m.cornerUv1.empty())
        m.cornerUv1.resize(m.corners.size(), float2{0, 0});
    m.faces.push_back(face);
    source.push_back(from);
}

// Vertices closer than `eps` made one; faces left with fewer than three corners dropped; faces over
// the same vertices as another (back to back where copies meet) dropped, both; vertices unused gone.
void weld(PolyMesh& m, float eps, std::vector<uint32_t>& source)
{
    std::vector<uint32_t> into(m.positions.size());
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    const auto cellOf = [&](int64_t x, int64_t y, int64_t z) { return uint64_t(x & 0x1fffff) | uint64_t(y & 0x1fffff) << 21 | uint64_t(z & 0x1fffff) << 42; };
    for (uint32_t v = 0; v < m.positions.size(); ++v) {
        const float3 p = m.positions[v];
        const int64_t cx = int64_t(std::floor(p.x / eps)), cy = int64_t(std::floor(p.y / eps)), cz = int64_t(std::floor(p.z / eps));
        into[v] = v;
        bool found = false;
        for (int64_t dx = -1; dx <= 1 && !found; ++dx)
            for (int64_t dy = -1; dy <= 1 && !found; ++dy)
                for (int64_t dz = -1; dz <= 1 && !found; ++dz) {
                    const auto it = cells.find(cellOf(cx + dx, cy + dy, cz + dz));
                    if (it == cells.end())
                        continue;
                    for (const uint32_t w : it->second)
                        if (length(m.positions[w] - p) <= eps) {
                            into[v] = w;
                            found = true;
                            break;
                        }
                }
        if (!found)
            cells[cellOf(cx, cy, cz)].push_back(v);
    }
    // Each face's loop through the merge, repeats in a row gone.
    struct Kept {
        Face face;
        std::vector<uint32_t> loop, corners;  // (corners: where each came from, for its UVs)
        uint32_t source;
    };
    std::vector<Kept> kept;
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        Kept k{m.faces[f], {}, {}, source[f]};
        for (uint32_t i = 0; i < m.faces[f].count; ++i) {
            const uint32_t c = m.faces[f].first + i, v = into[m.corners[c]];
            if (!k.loop.empty() && k.loop.back() == v)
                continue;
            k.loop.push_back(v);
            k.corners.push_back(c);
        }
        while (k.loop.size() > 1 && k.loop.front() == k.loop.back()) {
            k.loop.pop_back();
            k.corners.pop_back();
        }
        if (k.loop.size() >= 3)
            kept.push_back(std::move(k));
    }
    // Faces over the same vertices: back to back (where copies meet) both gone; facing the same way
    // (a copy over the original) one kept.
    std::map<std::vector<uint32_t>, std::vector<size_t>> same;
    for (size_t i = 0; i < kept.size(); ++i) {
        std::vector<uint32_t> key = kept[i].loop;
        std::sort(key.begin(), key.end());
        same[key].push_back(i);
    }
    const auto normalOf = [&](const std::vector<uint32_t>& loop) {
        float3 n = {0, 0, 0};
        for (size_t c = 0; c < loop.size(); ++c) {
            const float3 p = m.positions[loop[c]], q = m.positions[loop[(c + 1) % loop.size()]];
            n = n + float3{(p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)};
        }
        return n;
    };
    std::vector<uint8_t> gone(kept.size(), 0);
    for (const auto& [key, list] : same)
        for (size_t a = 0; a < list.size(); ++a)
            for (size_t b = a + 1; b < list.size() && !gone[list[a]]; ++b) {
                if (gone[list[b]])
                    continue;
                if (dot(normalOf(kept[list[a]].loop), normalOf(kept[list[b]].loop)) < 0.0f)
                    gone[list[a]] = 1;
                gone[list[b]] = 1;
            }
    // Rebuilt, vertices packed.
    std::vector<uint32_t> packed(m.positions.size(), ~0u);
    PolyMesh out = emptyLike(m);
    std::vector<uint32_t> outSource;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (gone[i])
            continue;
        Face face = kept[i].face;
        face.first = uint32_t(out.corners.size());
        face.count = uint32_t(kept[i].loop.size());
        for (size_t c = 0; c < kept[i].loop.size(); ++c) {
            uint32_t& v = packed[kept[i].loop[c]];
            if (v == ~0u) {
                v = uint32_t(out.positions.size());
                out.positions.push_back(m.positions[kept[i].loop[c]]);
                if (!m.vertexColors.empty())
                    out.vertexColors.push_back(m.vertexColors[kept[i].loop[c]]);
            }
            out.corners.push_back(v);
            if (!m.cornerUvs.empty())
                out.cornerUvs.push_back(m.cornerUvs[kept[i].corners[c]]);
            if (!m.cornerUv1.empty())
                out.cornerUv1.push_back(m.cornerUv1[kept[i].corners[c]]);
        }
        out.faces.push_back(face);
        outSource.push_back(kept[i].source);
    }
    m = std::move(out);
    source = std::move(outSource);
}

std::vector<uint32_t> identity(size_t n)
{
    std::vector<uint32_t> s(n);
    for (size_t i = 0; i < n; ++i)
        s[i] = uint32_t(i);
    return s;
}

float weldDistance(const PolyMesh& m)
{
    float3 lo, hi;
    m.bounds(lo, hi);
    return std::max(length(hi - lo) * 1e-5f, 1e-5f);
}

void mirror(const PolyMesh& in, const Modifier& mod, PolyMesh& out, std::vector<uint32_t>& source)
{
    const std::vector<uint32_t> ids = identity(in.faces.size());
    const uint32_t axis = std::min(mod.axis, 2u);
    append(out, in, [](uint32_t, float3 p) { return p; }, false, ids, source);
    append(out, in, [&](uint32_t, float3 p) { at(p, axis) = 2.0f * mod.offset - at(p, axis); return p; }, true, ids, source);
    if (mod.weld)
        weld(out, weldDistance(in), source);
}

struct Random {
    uint32_t state;
    float next()  // -1..1
    {
        state = state * 1664525u + 1013904223u;
        uint32_t h = (state ^ (state >> 16)) * 2246822519u;
        return float((h ^ (h >> 13)) & 0xffffu) / 32767.5f - 1.0f;
    }
};

void array(const PolyMesh& in, const Modifier& mod, PolyMesh& out, std::vector<uint32_t>& source)
{
    const std::vector<uint32_t> ids = identity(in.faces.size());
    float3 lo, hi;
    in.bounds(lo, hi);
    const float3 size = hi - lo;
    const float3 step = mod.relative ? float3{mod.step.x * size.x, mod.step.y * size.y, mod.step.z * size.z} : mod.step;
    const uint32_t axis = std::min(mod.axis, 2u), out0 = axis == 1 ? 0u : axis == 0 ? 1u : 0u;
    const uint32_t nx = std::clamp(mod.count[0], 1u, 1000u);
    const uint32_t ny = mod.mode == ArrayMode::Grid ? std::clamp(mod.count[1], 1u, 1000u) : 1u;
    const uint32_t nz = mod.mode == ArrayMode::Grid ? std::clamp(mod.count[2], 1u, 1000u) : 1u;
    if (uint64_t(nx) * ny * nz * in.faces.size() > 2'000'000u)
        return append(out, in, [](uint32_t, float3 p) { return p; }, false, ids, source);  // (too many: as it is)
    const bool full = std::fabs(mod.angle) >= 359.99f;
    const float turnStep = mod.angle * kRadians / float(full ? nx : std::max(nx - 1, 1u));
    for (uint32_t k = 0; k < nz; ++k)
        for (uint32_t j = 0; j < ny; ++j)
            for (uint32_t i = 0; i < nx; ++i) {
                const uint32_t copy = (k * ny + j) * nx + i;
                // The copy's random move, turn and scale (none for the first: it stays where it is).
                Random r{mod.seed * 2654435761u + copy * 40503u + 1u};
                float3 move = {0, 0, 0};
                float turn = 0.0f, scale = 1.0f;
                if (copy > 0) {
                    move = {r.next() * mod.jitter.x, r.next() * mod.jitter.x, r.next() * mod.jitter.x};
                    turn = r.next() * mod.jitter.y * kRadians;
                    scale = std::max(1.0f + r.next() * mod.jitter.z, 0.01f);
                }
                append(
                    out, in,
                    [&](uint32_t, float3 p) {
                        p = rotate(p * scale, 1, turn);
                        if (mod.mode == ArrayMode::Radial) {
                            at(p, out0) += mod.radius;
                            p = rotate(p, axis, turnStep * float(i));
                        } else if (mod.mode == ArrayMode::Grid) {
                            p = p + float3{step.x * float(i), step.y * float(j), step.z * float(k)};
                        } else {
                            p = p + step * float(i);
                        }
                        return p + move;
                    },
                    false, ids, source);
            }
    if (mod.weld)
        weld(out, weldDistance(in), source);
}

// Each directed edge (a -> b): its face and the corner at a.
struct Edges {
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> of;
    explicit Edges(const PolyMesh& m)
    {
        for (uint32_t f = 0; f < m.faces.size(); ++f)
            for (uint32_t k = 0; k < m.faces[f].count; ++k)
                of[key(m.corners[m.faces[f].first + k], m.corners[m.faces[f].first + (k + 1) % m.faces[f].count])] = {f, k};
    }
    static uint64_t key(uint32_t a, uint32_t b) { return uint64_t(a) << 32 | b; }
    const std::pair<uint32_t, uint32_t>* find(uint32_t a, uint32_t b) const
    {
        const auto it = of.find(key(a, b));
        return it == of.end() ? nullptr : &it->second;
    }
};

void solidify(const PolyMesh& in, const Modifier& mod, PolyMesh& out, std::vector<uint32_t>& source)
{
    const std::vector<uint32_t> ids = identity(in.faces.size());
    // Each vertex's normal (its faces'), longer where they meet at an angle (the walls stay as thick).
    std::vector<float3> sum(in.positions.size(), float3{0, 0, 0});
    std::vector<std::vector<float3>> normals(in.positions.size());
    for (uint32_t f = 0; f < in.faces.size(); ++f) {
        const float3 n = in.faceNormal(f);
        for (uint32_t k = 0; k < in.faces[f].count; ++k) {
            const uint32_t v = in.corners[in.faces[f].first + k];
            sum[v] = sum[v] + n;
            normals[v].push_back(n);
        }
    }
    std::vector<float3> along(in.positions.size(), float3{0, 0, 0});
    for (uint32_t v = 0; v < in.positions.size(); ++v) {
        if (length(sum[v]) < 1e-6f)
            continue;
        const float3 n = normalize(sum[v]);
        float least = 1.0f;
        for (const float3& fn : normals[v])
            least = std::min(least, dot(n, fn));
        along[v] = n * (1.0f / std::max(least, 0.35f));
    }
    const float t = mod.amount, o = std::clamp(mod.offset, -1.0f, 1.0f);
    const float outside = t * (1.0f + o) * 0.5f, inside = t * (1.0f - o) * 0.5f;
    const uint32_t n = uint32_t(in.positions.size());
    append(out, in, [&](uint32_t v, float3 p) { return p + along[v] * outside; }, false, ids, source);
    append(out, in, [&](uint32_t v, float3 p) { return p - along[v] * inside; }, true, ids, source);
    // A rim along each open edge (a -> b with no b -> a): outer b, a, inner a, b.
    const Edges edges(in);
    for (uint32_t f = 0; f < in.faces.size(); ++f)
        for (uint32_t k = 0; k < in.faces[f].count; ++k) {
            const uint32_t a = in.corners[in.faces[f].first + k], b = in.corners[in.faces[f].first + (k + 1) % in.faces[f].count];
            if (!edges.find(b, a))
                addLike(out, {b, a, n + a, n + b}, in.faces[f].material, source, f);
        }
}

void bevel(const PolyMesh& in, const Modifier& mod, PolyMesh& out, std::vector<uint32_t>& source)
{
    const Edges edges(in);
    const float cosSharp = std::cos(mod.sharp * kRadians), width = std::max(mod.amount, 0.0f);
    std::vector<float3> normal(in.faces.size());
    for (uint32_t f = 0; f < in.faces.size(); ++f)
        normal[f] = in.faceNormal(f);
    const auto sharp = [&](uint32_t f, uint32_t a, uint32_t b) {  // (a -> b in f)
        const auto* o = edges.find(b, a);
        return o && dot(normal[f], normal[o->first]) < cosSharp;
    };
    const auto cornerOf = [&](uint32_t f, uint32_t k) { return in.corners[in.faces[f].first + (k % in.faces[f].count)]; };
    // How far each corner moves along a smooth edge from it: the same from the faces on both sides
    // (their mean), so the faces still meet there.
    std::unordered_map<uint64_t, std::pair<float, uint32_t>> smoothTravel;
    struct Move {
        float3 dir;      // the unit direction it moves along (0: stays; both edges sharp: the bisector)
        float travel;
        uint32_t smoothTo = ~0u;  // the vertex at the far end of the smooth edge it slides along
    };
    std::vector<std::vector<Move>> moves(in.faces.size());
    for (uint32_t f = 0; f < in.faces.size(); ++f) {
        const uint32_t count = in.faces[f].count;
        moves[f].resize(count);
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t c = cornerOf(f, k), p = cornerOf(f, k + count - 1), q = cornerOf(f, k + 1);
            const float3 d1 = normalize(in.positions[p] - in.positions[c]), d2 = normalize(in.positions[q] - in.positions[c]);
            const float sine = std::max(length(cross(d1, d2)), 0.1f);
            const bool before = sharp(f, p, c), after = sharp(f, c, q);
            Move& m = moves[f][k];
            if (before && after) {
                m.dir = d1 + d2;
                m.travel = width / sine;
            } else if (before || after) {
                m.dir = before ? d2 : d1;
                m.travel = width / sine;
                m.smoothTo = before ? q : p;
                auto& [total, n] = smoothTravel[edgeKey(c, m.smoothTo) ^ (uint64_t(c) * 0x9e3779b97f4a7c15ull)];
                total += m.travel;
                ++n;
            } else {
                m.dir = {0, 0, 0};
                m.travel = 0.0f;
            }
        }
    }
    // Each face's corners moved (a vertex each), the face over them.
    std::vector<std::vector<uint32_t>> corner(in.faces.size());
    const std::vector<uint32_t> none;
    out.positions.reserve(in.corners.size());
    for (uint32_t f = 0; f < in.faces.size(); ++f) {
        std::vector<uint32_t> loop;
        for (uint32_t k = 0; k < in.faces[f].count; ++k) {
            const uint32_t c = cornerOf(f, k);
            const Move& m = moves[f][k];
            float travel = m.travel;
            if (m.smoothTo != ~0u) {
                const auto& [total, n] = smoothTravel[edgeKey(c, m.smoothTo) ^ (uint64_t(c) * 0x9e3779b97f4a7c15ull)];
                travel = total / float(n);
            }
            // (never past most of the edges it moves along)
            const float3 pc = in.positions[c];
            const float reach = std::min(length(in.positions[cornerOf(f, k + 1)] - pc), length(in.positions[cornerOf(f, k + in.faces[f].count - 1)] - pc));
            travel = std::min(travel, reach * 0.45f);
            loop.push_back(uint32_t(out.positions.size()));
            out.positions.push_back(pc + m.dir * travel);
            if (!in.vertexColors.empty())
                out.vertexColors.push_back(in.vertexColors[c]);
        }
        corner[f] = loop;
        Face face = in.faces[f];
        face.first = uint32_t(out.corners.size());
        out.corners.insert(out.corners.end(), loop.begin(), loop.end());
        if (!in.cornerUvs.empty())
            out.cornerUvs.insert(out.cornerUvs.end(), in.cornerUvs.begin() + in.faces[f].first, in.cornerUvs.begin() + in.faces[f].first + face.count);
        if (!in.cornerUv1.empty())
            out.cornerUv1.insert(out.cornerUv1.end(), in.cornerUv1.begin() + in.faces[f].first, in.cornerUv1.begin() + in.faces[f].first + face.count);
        out.faces.push_back(face);
        source.push_back(f);
    }
    // Round ones: across each sharp edge at each end, the profile's points from the face with the
    // lower index's moved corner to the other's (level::bevelProfile), shared by the edge's faces and
    // the corner's; their faces in a smoothing group of their own.
    const uint32_t segments = std::clamp(mod.segments, 1u, 32u);
    const float curve = std::clamp(mod.curve, -1.0f, 1.0f);
    const bool round = segments > 1 && curve != 0.0f;
    uint32_t smooth = 0;
    if (round) {
        for (const Face& face : in.faces)
            if (face.smoothing < 0x10000000u)
                smooth = std::max(smooth, face.smoothing);
        ++smooth;
    }
    const auto smoothLast = [&] {
        if (round)
            out.faces.back().smoothing = smooth;
    };
    struct Profile {
        uint32_t from;                // the face its points run from (the lower index)
        std::vector<uint32_t> inner;  // the points between the two moved corners
    };
    std::unordered_map<uint64_t, Profile> profiles;  // by edge key and end (the key's low bit: its far vertex)
    const auto profileKey = [](uint32_t a, uint32_t b, uint32_t end) { return edgeKey(a, b) * 2 + (end == std::max(a, b) ? 1 : 0); };
    // A face along each sharp edge (once: from the face with the lower index).
    for (uint32_t f = 0; f < in.faces.size(); ++f)
        for (uint32_t k = 0; k < in.faces[f].count; ++k) {
            const uint32_t a = cornerOf(f, k), b = cornerOf(f, k + 1);
            const auto* o = edges.find(b, a);
            if (!o || o->first < f || (o->first == f) || !sharp(f, a, b))
                continue;
            const uint32_t g = o->first, m = o->second;  // (g's corner at b; a after it)
            const uint32_t fa = corner[f][k], fb = corner[f][(k + 1) % in.faces[f].count];
            const uint32_t ga = corner[g][(m + 1) % in.faces[g].count], gb = corner[g][m];
            std::vector<uint32_t> chainA = {fa}, chainB = {fb};
            for (const uint32_t end : {a, b}) {
                const uint32_t p = end == a ? fa : fb, q = end == a ? ga : gb;
                const float3 pp = out.positions[p], pq = out.positions[q];
                const float3 at = nearestOnLine((pp + pq) * 0.5f, in.positions[a], in.positions[b]);
                Profile& profile = profiles[profileKey(a, b, end)];
                profile.from = f;
                for (uint32_t i = 1; i < segments; ++i) {
                    profile.inner.push_back(uint32_t(out.positions.size()));
                    out.positions.push_back(bevelProfile(pp, at, pq, float(i) / float(segments), curve));
                    if (!in.vertexColors.empty())
                        out.vertexColors.push_back(in.vertexColors[end]);
                }
                std::vector<uint32_t>& chain = end == a ? chainA : chainB;
                chain.insert(chain.end(), profile.inner.begin(), profile.inner.end());
            }
            chainA.push_back(ga);
            chainB.push_back(gb);
            for (uint32_t i = 0; i < segments; ++i) {
                addLike(out, {chainB[i], chainA[i], chainA[i + 1], chainB[i + 1]}, in.faces[f].material, source, f);
                smoothLast();
            }
        }
    // A face at each corner with a sharp edge, over the moved corners of the faces around it and the
    // profiles between them (the fan walked face to face; corners on an open edge get none). Round:
    // a fan of triangles from a point nearer the old corner (on the ball a box's corner rounds to).
    std::vector<uint8_t> done(in.positions.size(), 0);
    for (uint32_t f = 0; f < in.faces.size(); ++f)
        for (uint32_t k = 0; k < in.faces[f].count; ++k) {
            const uint32_t v = cornerOf(f, k);
            if (done[v])
                continue;
            done[v] = 1;
            std::vector<uint32_t> ring;
            float3 around = {0, 0, 0}, moved = {0, 0, 0};
            uint32_t faces = 0;
            bool closed = false, anySharp = false;
            uint32_t g = f, j = k;
            for (size_t guard = 0; guard < 64; ++guard) {
                ring.push_back(corner[g][j]);
                moved = moved + out.positions[corner[g][j]];
                ++faces;
                around = around + normal[g];
                const uint32_t q = cornerOf(g, j + 1);
                const bool cut = sharp(g, v, q);
                anySharp |= cut;
                const auto* o = edges.find(q, v);
                if (!o)
                    break;
                if (cut)
                    if (const auto it = profiles.find(profileKey(v, q, v)); it != profiles.end()) {
                        if (it->second.from == g)
                            ring.insert(ring.end(), it->second.inner.begin(), it->second.inner.end());
                        else
                            ring.insert(ring.end(), it->second.inner.rbegin(), it->second.inner.rend());
                    }
                g = o->first;
                j = (o->second + 1) % in.faces[g].count;  // (v follows q in g)
                if (g == f && j == k) {
                    closed = true;
                    break;
                }
            }
            if (!closed || !anySharp || ring.size() < 3)
                continue;
            // Wound to face out (as the faces around it do).
            float3 newell = {0, 0, 0};
            for (size_t i = 0; i < ring.size(); ++i) {
                const float3 p = out.positions[ring[i]], q = out.positions[ring[(i + 1) % ring.size()]];
                newell = newell + float3{(p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)};
            }
            if (dot(newell, around) < 0.0f)
                std::reverse(ring.begin(), ring.end());
            if (!round || ring.size() == faces) {
                addLike(out, ring, in.faces[f].material, source, f);
                smoothLast();
                continue;
            }
            // (a box's corner: from the mean of the moved corners about 0.37 of the way to the old
            // one lands on the ball its three round edges lie on)
            const float3 mean = moved * (1.0f / float(faces));
            const uint32_t apex = uint32_t(out.positions.size());
            out.positions.push_back(mean + (in.positions[v] - mean) * (0.366f * curve));
            if (!in.vertexColors.empty())
                out.vertexColors.push_back(in.vertexColors[v]);
            for (size_t i = 0; i < ring.size(); ++i) {
                addLike(out, {apex, ring[i], ring[(i + 1) % ring.size()]}, in.faces[f].material, source, f);
                smoothLast();
            }
        }
    weld(out, weldDistance(in), source);
}

void deform(const PolyMesh& in, const Modifier& mod, PolyMesh& out, std::vector<uint32_t>& source)
{
    out = in;
    source = identity(in.faces.size());
    float3 lo, hi;
    in.bounds(lo, hi);
    const uint32_t a = std::min(mod.axis, 2u), b = (a + 1) % 3;
    const float start = at(lo, a), length = at(hi, a) - at(lo, a);
    if (length < 1e-6f)
        return;
    const float total = mod.angle * kRadians;
    for (float3& p : out.positions) {
        const float t = (at(p, a) - start) / length;
        if (mod.deform == DeformType::Bend) {
            if (std::fabs(total) < 1e-5f)
                continue;
            const float r = length / total, theta = total * t, rb = r - at(p, b);
            at(p, a) = start + rb * std::sin(theta);
            at(p, b) = r - rb * std::cos(theta);
        } else if (mod.deform == DeformType::Twist) {
            p = rotate(p, a, total * t);
        } else {
            const float s = 1.0f + (mod.amount - 1.0f) * t;
            at(p, (a + 1) % 3) *= s;
            at(p, (a + 2) % 3) *= s;
        }
    }
}

// Along a spline: its length axis along the curve, y up (z for a mesh long in y), the third to the
// curve's right (signed so it isn't turned inside out).
void curve(const PolyMesh& in, const Modifier& mod, const Spline* spline, PolyMesh& out, std::vector<uint32_t>& source)
{
    const std::vector<uint32_t> ids = identity(in.faces.size());
    if (!spline || spline->points.size() < 2)
        return append(out, in, [](uint32_t, float3 p) { return p; }, false, ids, source);
    const float total = splineLength(*spline);
    const std::vector<SplineFrame> frames = splineFrames(*spline, std::clamp(total / 400.0f, 0.02f, 0.25f));
    float3 lo, hi;
    in.bounds(lo, hi);
    const uint32_t a = std::min(mod.axis, 2u), up = a == 1 ? 2u : 1u, side = 3u - a - up;
    const float sign = a == 2 ? -1.0f : 1.0f;
    const float own = std::max(at(hi, a) - at(lo, a), 1e-4f);
    if (!mod.copies) {
        const uint32_t n = mod.fit ? std::clamp(uint32_t(std::lround(total / own)), 1u, 10000u) : 1u;
        const float scale = mod.fit ? total / (float(n) * own) : 1.0f;
        for (uint32_t i = 0; i < n; ++i)
            append(
                out, in,
                [&](uint32_t, float3 p) {
                    const SplineFrame f = frameAt(frames, (at(p, a) - at(lo, a) + float(i) * own) * scale);
                    return f.position + f.up * at(p, up) + f.side * (sign * at(p, side));
                },
                false, ids, source);
        if (n > 1 || spline->closed)
            weld(out, weldDistance(in), source);
        return;
    }
    const float spacing = mod.spacing > 0.0f ? mod.spacing : own;
    const uint32_t n = std::clamp(uint32_t(std::floor(total / spacing + 1e-4f)) + (spline->closed ? 0u : 1u), 1u, 10000u);
    for (uint32_t i = 0; i < n; ++i) {
        const SplineFrame f = frameAt(frames, float(i) * spacing);
        append(
            out, in,
            [&](uint32_t, float3 p) {
                return mod.align ? f.position + f.tangent * at(p, a) + f.up * at(p, up) + f.side * (sign * at(p, side)) : f.position + p;
            },
            false, ids, source);
    }
}

}  // namespace

namespace {
std::atomic<ScriptModifierHandler> g_scriptHandler{nullptr};
std::atomic<uint32_t> g_scriptGeneration{0};

// A handler call into a growing buffer (it says how much it needs when short).
bool callScript(int32_t op, const char* cls, const char* settings, const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    const ScriptModifierHandler h = g_scriptHandler.load();
    if (!h)
        return false;
    out.resize(std::max<size_t>(in.size() * 2 + 4096, 4096));
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int32_t n = h(op, cls, settings, in.empty() ? nullptr : in.data(), int32_t(in.size()), out.data(), int32_t(out.size()));
        if (n > 0) {
            out.resize(size_t(n));
            return true;
        }
        if (n == 0)
            return false;
        out.resize(size_t(-n));
    }
    return false;
}
}  // namespace

void setScriptModifierHandler(ScriptModifierHandler handler)
{
    g_scriptHandler = handler;
    ++g_scriptGeneration;
}

uint32_t scriptModifierGeneration() { return g_scriptGeneration.load(); }

bool runScriptModifier(const std::string& cls, const std::string& settings, const PolyMesh& in, PolyMesh& out, std::string* error)
{
    std::vector<uint8_t> result;
    if (!callScript(0, cls.c_str(), settings.c_str(), in.save(), result)) {
        if (error)
            *error = g_scriptHandler.load() ? cls + " failed (or isn't in the game's code)" : "no scripts loaded";
        return false;
    }
    if (!out.load(result.data(), result.size())) {
        if (error)
            *error = cls + " gave back no mesh";
        return false;
    }
    return true;
}

std::string describeScriptModifiers()
{
    std::vector<uint8_t> text;
    return callScript(1, nullptr, nullptr, {}, text) ? std::string(text.begin(), text.end()) : std::string("[]");
}

bool hasScript(const std::vector<Modifier>& stack)
{
    for (const Modifier& m : stack)
        if (m.kind == ModifierKind::Script && m.enabled)
            return true;
    return false;
}

bool needsSpline(const std::vector<Modifier>& stack)
{
    for (const Modifier& m : stack)
        if (m.kind == ModifierKind::Curve && m.enabled)
            return true;
    return false;
}

bool Modifier::operator==(const Modifier& o) const
{
    const auto same = [](float3 a, float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    return kind == o.kind && enabled == o.enabled && axis == o.axis && offset == o.offset && weld == o.weld && mode == o.mode &&
           count[0] == o.count[0] && count[1] == o.count[1] && count[2] == o.count[2] && same(step, o.step) && relative == o.relative &&
           angle == o.angle && radius == o.radius && same(jitter, o.jitter) && seed == o.seed && amount == o.amount && sharp == o.sharp &&
           segments == o.segments && curve == o.curve &&
           deform == o.deform && copies == o.copies && fit == o.fit && spacing == o.spacing && align == o.align && script == o.script &&
           settings == o.settings;
}

Modifier makeModifier(ModifierKind kind, ArrayMode mode, DeformType type)
{
    Modifier m;
    m.kind = kind;
    m.mode = mode;
    m.deform = type;
    switch (kind) {
    case ModifierKind::Array:
        m.count[0] = mode == ArrayMode::Radial ? 8 : 3;
        m.count[2] = mode == ArrayMode::Grid ? 3 : 1;  // (a grid: across x and z, one high)
        m.step = mode == ArrayMode::Grid ? float3{1.0f, 1.0f, 1.0f} : float3{1.0f, 0.0f, 0.0f};
        m.axis = 1;
        m.radius = mode == ArrayMode::Radial ? 2.0f : 0.0f;
        break;
    case ModifierKind::Solidify:
        m.amount = 0.1f;
        break;
    case ModifierKind::Bevel:
        m.amount = 0.05f;
        break;
    case ModifierKind::Deform:
        m.axis = 1;
        m.angle = type == DeformType::Twist ? 90.0f : 45.0f;
        m.amount = 0.5f;
        break;
    case ModifierKind::Curve:
        m.axis = 0;
        break;
    default:
        break;
    }
    return m;
}

Modifier modifierFromJson(const Json& j)
{
    ModifierKind kind = ModifierKind::Mirror;
    const std::string name = j.getString("kind");
    for (uint32_t k = 0; k < kModifierKinds; ++k)
        if (name == kModifierNames[k])
            kind = ModifierKind(k);
    ArrayMode mode = ArrayMode::Linear;
    for (uint32_t k = 0; k < 3; ++k)
        if (j.getString("mode") == kArrayModeNames[k])
            mode = ArrayMode(k);
    DeformType type = DeformType::Bend;
    for (uint32_t k = 0; k < 3; ++k)
        if (j.getString("type") == kDeformNames[k])
            type = DeformType(k);
    Modifier m = makeModifier(kind, mode, type);
    m.enabled = j.getBool("enabled", true);
    m.axis = uint32_t(std::clamp(j.getNumber("axis", m.axis), 0.0, 2.0));
    m.offset = float(j.getNumber("offset", m.offset));
    m.weld = j.getBool("weld", m.weld);
    if (const Json* c = j.get("count"))
        for (size_t k = 0; k < 3 && k < c->array.size(); ++k)
            m.count[k] = uint32_t(std::clamp(c->array[k].number, 1.0, 1000.0));
    const auto vec = [&](const char* key, float3& v) {
        if (const Json* a = j.get(key); a && a->array.size() == 3)
            v = {float(a->array[0].number), float(a->array[1].number), float(a->array[2].number)};
    };
    vec("step", m.step);
    vec("jitter", m.jitter);
    m.relative = j.getBool("relative", m.relative);
    m.angle = float(j.getNumber("angle", m.angle));
    m.radius = float(j.getNumber("radius", m.radius));
    m.seed = uint32_t(j.getNumber("seed", m.seed));
    m.amount = float(j.getNumber(kind == ModifierKind::Solidify ? "thickness" : kind == ModifierKind::Bevel ? "width" : "factor", m.amount));
    m.sharp = float(j.getNumber("sharp", m.sharp));
    m.segments = uint32_t(std::clamp(j.getNumber("segments", m.segments), 1.0, 32.0));
    m.curve = float(std::clamp(j.getNumber("curve", m.curve), -1.0, 1.0));
    m.copies = j.getString("follow") == "copies";
    m.fit = j.getBool("fit", m.fit);
    m.spacing = std::max(float(j.getNumber("spacing", m.spacing)), 0.0f);
    m.align = j.getBool("align", m.align);
    m.script = j.getString("class");
    if (const Json* s = j.get("settings"); s && s->isObject())
        m.settings = s->dump(0);
    return m;
}

void writeModifierJson(JsonWriter& w, const char* key, const Modifier& m)
{
    w.beginObject(key).value("kind", kModifierNames[uint32_t(m.kind) % kModifierKinds]).value("enabled", m.enabled);
    const auto vec = [&](const char* k, float3 v) { w.beginArray(k).value(nullptr, double(v.x)).value(nullptr, double(v.y)).value(nullptr, double(v.z)).endArray(); };
    switch (m.kind) {
    case ModifierKind::Mirror:
        w.value("axis", m.axis).value("offset", double(m.offset)).value("weld", m.weld);
        break;
    case ModifierKind::Array:
        w.value("mode", kArrayModeNames[uint32_t(m.mode) % 3]).beginArray("count").value(nullptr, m.count[0]).value(nullptr, m.count[1])
            .value(nullptr, m.count[2]).endArray();
        vec("step", m.step);
        w.value("relative", m.relative).value("axis", m.axis).value("angle", double(m.angle)).value("radius", double(m.radius));
        vec("jitter", m.jitter);
        w.value("seed", m.seed).value("weld", m.weld);
        break;
    case ModifierKind::Solidify:
        w.value("thickness", double(m.amount)).value("offset", double(m.offset));
        break;
    case ModifierKind::Bevel:
        w.value("width", double(m.amount)).value("sharp", double(m.sharp)).value("segments", m.segments).value("curve", double(m.curve));
        break;
    case ModifierKind::Deform:
        w.value("type", kDeformNames[uint32_t(m.deform) % 3]).value("axis", m.axis).value("angle", double(m.angle)).value("factor", double(m.amount));
        break;
    case ModifierKind::Curve:
        w.value("follow", m.copies ? "copies" : "bend").value("axis", m.axis).value("fit", m.fit).value("spacing", double(m.spacing))
            .value("align", m.align);
        break;
    case ModifierKind::Script:
        w.value("class", m.script).raw("settings", m.settings.empty() ? std::string_view("{}") : std::string_view(m.settings));
        break;
    }
    w.endObject();
}

std::string modifierTitle(const Modifier& m)
{
    switch (m.kind) {
    case ModifierKind::Mirror:
        return std::string("Mirror ") + kAxisNames[std::min(m.axis, 2u)];
    case ModifierKind::Array:
        return m.mode == ArrayMode::Radial ? "Radial Array" : m.mode == ArrayMode::Grid ? "Grid Array" : "Array";
    case ModifierKind::Solidify:
        return "Solidify";
    case ModifierKind::Bevel:
        return "Bevel";
    case ModifierKind::Deform:
        return m.deform == DeformType::Twist ? "Twist" : m.deform == DeformType::Taper ? "Taper" : "Bend";
    case ModifierKind::Curve:
        return m.copies ? "Copies Along Spline" : "Follow Spline";
    case ModifierKind::Script:
        return m.script.substr(m.script.find_last_of('.') == std::string::npos ? 0 : m.script.find_last_of('.') + 1);
    }
    return "Modifier";
}

bool applyModifiers(const PolyMesh& in, const std::vector<Modifier>& stack, PolyMesh& out, std::vector<uint32_t>* sourceFace, std::string* error,
                    const ModifierContext* context)
{
    PolyMesh mesh = in;
    std::vector<uint32_t> total = identity(in.faces.size());
    for (const Modifier& mod : stack) {
        if (!mod.enabled || mesh.faces.empty())
            continue;
        PolyMesh next = emptyLike(mesh);
        std::vector<uint32_t> source;
        switch (mod.kind) {
        case ModifierKind::Mirror:
            mirror(mesh, mod, next, source);
            break;
        case ModifierKind::Array:
            array(mesh, mod, next, source);
            break;
        case ModifierKind::Solidify:
            solidify(mesh, mod, next, source);
            break;
        case ModifierKind::Bevel:
            bevel(mesh, mod, next, source);
            break;
        case ModifierKind::Deform:
            deform(mesh, mod, next, source);
            break;
        case ModifierKind::Curve:
            curve(mesh, mod, context ? context->spline : nullptr, next, source);
            break;
        case ModifierKind::Script: {
            // C#: its result (each face its source when it kept the faces' count; else none).
            std::string why;
            if (runScriptModifier(mod.script, mod.settings, mesh, next, &why)) {
                source = next.faces.size() == mesh.faces.size() ? identity(mesh.faces.size()) : std::vector<uint32_t>(next.faces.size(), ~0u);
            } else {
                next = mesh;
                source = identity(mesh.faces.size());
            }
            break;
        }
        }
        std::string why;
        if (!next.valid(&why)) {
            if (error)
                *error = modifierTitle(mod) + ": " + why;
            out = in;
            if (sourceFace)
                *sourceFace = identity(in.faces.size());
            return false;
        }
        for (uint32_t& s : source)
            s = s < total.size() ? total[s] : ~0u;
        total = std::move(source);
        mesh = std::move(next);
    }
    // The faces the stack made, mask UVs in the free space (wear and paint show on them).
    maskFill(mesh, &in);
    out = std::move(mesh);
    if (sourceFace)
        *sourceFace = std::move(total);
    return true;
}

}  // namespace fx::level
