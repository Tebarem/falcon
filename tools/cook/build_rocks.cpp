// Procedural rock meshes (falcon cook --rocks): boulders, slabs, spires and an arch, for terrain
// scatter. Each is a signed distance field (a base shape, cut by a few random planes into facets,
// roughened by fractal noise, a spire banded by strata), turned into triangles with surface nets
// (a vertex per grid cell the surface crosses, at the average of its edge crossings pulled onto
// the surface; a quad per crossed grid edge), normals from the field's gradient. The meshes then
// go through the cooker like any other (cluster LOD, collision). Metres, +Y up, resting on y = 0
// (bases reach a little below it, to sink into the ground).

#include "cook.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace fx::cook {

namespace {

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a)
{
    const float l = length(a);
    return l > 1e-12f ? a * (1.0f / l) : V3{0, 1, 0};
}

uint32_t hash(uint32_t a, uint32_t b, uint32_t c, uint32_t seed)
{
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu ^ seed * 0x165667b1u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
float unit(uint32_t h) { return float(h & 0xffffffu) / 16777216.0f; }

// Value noise, -1..1, and its fractal sum.
float noise3(V3 p, uint32_t seed)
{
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float u = s(p.x - fx), v = s(p.y - fy), w = s(p.z - fz);
    auto at = [&](int dx, int dy, int dz) { return unit(hash(uint32_t(ix + dx), uint32_t(iy + dy), uint32_t(iz + dz), seed)) * 2.0f - 1.0f; };
    const float x00 = at(0, 0, 0) + (at(1, 0, 0) - at(0, 0, 0)) * u, x10 = at(0, 1, 0) + (at(1, 1, 0) - at(0, 1, 0)) * u;
    const float x01 = at(0, 0, 1) + (at(1, 0, 1) - at(0, 0, 1)) * u, x11 = at(0, 1, 1) + (at(1, 1, 1) - at(0, 1, 1)) * u;
    const float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}
float fbm3(V3 p, uint32_t octaves, uint32_t seed)
{
    float sum = 0.0f, amplitude = 0.5f;
    for (uint32_t o = 0; o < octaves; ++o) {
        sum += noise3(p, seed + o * 31u) * amplitude;
        p = V3{p.x * 1.6f + p.z * 1.2f, p.y * 2.0f, -p.x * 1.2f + p.z * 1.6f} + V3{5.3f, 1.7f, 9.1f};
        amplitude *= 0.5f;
    }
    return sum;
}

using Field = std::function<float(V3)>;

// Facets: the shape cut by `count` random planes `inset` inside its bounding radius.
Field faceted(Field base, uint32_t count, float radius, float inset, V3 centre, uint32_t seed)
{
    std::vector<std::pair<V3, float>> planes;
    for (uint32_t i = 0; i < count; ++i) {
        // Random directions, mostly sideways and up (the base stays flat-ish under the ground).
        const float a = unit(hash(i, 1, 0, seed)) * 6.2831853f, b = 0.15f + unit(hash(i, 2, 0, seed)) * 1.1f;
        const V3 n = normalize({std::cos(a) * std::cos(b - 0.4f), std::sin(b), std::sin(a) * std::cos(b - 0.4f)});
        planes.push_back({n, radius * (inset + 0.12f * unit(hash(i, 3, 0, seed)))});
    }
    return [base, planes, centre](V3 p) {
        float d = base(p);
        for (const auto& [n, o] : planes)
            d = std::max(d, dot(p - centre, n) - o);
        return d;
    };
}

Field roughened(Field base, float amplitude, float frequency, uint32_t seed)
{
    return [base, amplitude, frequency, seed](V3 p) { return base(p) + fbm3(p * frequency, 4, seed) * amplitude; };
}

// Surface nets over [lo, hi] with cells of `cell` metres.
SrcPrimitive surfaceNets(const Field& field, V3 lo, V3 hi, float cell)
{
    const int nx = int(std::ceil((hi.x - lo.x) / cell)), ny = int(std::ceil((hi.y - lo.y) / cell)), nz = int(std::ceil((hi.z - lo.z) / cell));
    auto pointAt = [&](int i, int j, int k) { return V3{lo.x + float(i) * cell, lo.y + float(j) * cell, lo.z + float(k) * cell}; };
    const size_t sx = size_t(nx + 1), sy = size_t(ny + 1);
    std::vector<float> d(sx * sy * size_t(nz + 1));
    for (int k = 0; k <= nz; ++k)
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i <= nx; ++i)
                d[(size_t(k) * sy + size_t(j)) * sx + size_t(i)] = field(pointAt(i, j, k));
    auto D = [&](int i, int j, int k) { return d[(size_t(k) * sy + size_t(j)) * sx + size_t(i)]; };
    auto gradient = [&](V3 p) {
        const float e = cell * 0.5f;
        return normalize({field(p + V3{e, 0, 0}) - field(p - V3{e, 0, 0}), field(p + V3{0, e, 0}) - field(p - V3{0, e, 0}),
                          field(p + V3{0, 0, e}) - field(p - V3{0, 0, e})});
    };

    SrcPrimitive prim;
    std::vector<int32_t> vertexOf(size_t(nx) * size_t(ny) * size_t(nz), -1);
    auto cellIndex = [&](int i, int j, int k) { return (size_t(k) * size_t(ny) + size_t(j)) * size_t(nx) + size_t(i); };
    static const int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                float c[8];
                bool in = false, out = false;
                for (int q = 0; q < 8; ++q) {
                    c[q] = D(i + kCorner[q][0], j + kCorner[q][1], k + kCorner[q][2]);
                    (c[q] < 0.0f ? in : out) = true;
                }
                if (!in || !out)
                    continue;
                V3 sum = {0, 0, 0};
                int crossings = 0;
                for (const auto& e : kEdge) {
                    const float a = c[e[0]], b = c[e[1]];
                    if ((a < 0.0f) == (b < 0.0f))
                        continue;
                    const float t = a / (a - b);
                    const V3 pa = pointAt(i + kCorner[e[0]][0], j + kCorner[e[0]][1], k + kCorner[e[0]][2]);
                    const V3 pb = pointAt(i + kCorner[e[1]][0], j + kCorner[e[1]][1], k + kCorner[e[1]][2]);
                    sum = sum + pa + (pb - pa) * t;
                    ++crossings;
                }
                V3 p = sum * (1.0f / float(crossings));
                // Onto the surface (two Newton steps along the gradient), staying in the cell.
                for (int it = 0; it < 2; ++it)
                    p = p - gradient(p) * field(p);
                const V3 cmin = pointAt(i, j, k);
                p = {std::clamp(p.x, cmin.x, cmin.x + cell), std::clamp(p.y, cmin.y, cmin.y + cell), std::clamp(p.z, cmin.z, cmin.z + cell)};
                const V3 n = gradient(p);
                SrcVertex v = {};
                v.px = p.x, v.py = p.y, v.pz = p.z;
                v.nx = n.x, v.ny = n.y, v.nz = n.z;
                v.u = (p.x + p.z) * 0.5f, v.v = p.y * 0.5f;  // (terrain-shaded rocks project their own)
                const V3 t = normalize(cross({0, 1, 0}, n).x == 0.0f && cross({0, 1, 0}, n).z == 0.0f ? V3{1, 0, 0} : cross({0, 1, 0}, n));
                v.tx = t.x, v.ty = t.y, v.tz = t.z, v.tw = 1.0f;
                vertexOf[cellIndex(i, j, k)] = int32_t(prim.vertices.size());
                prim.vertices.push_back(v);
            }
    // A quad per crossed grid edge, between the four cells around it; wound to face outward.
    auto quad = [&](int a, int b, int c, int dd) {
        if (a < 0 || b < 0 || c < 0 || dd < 0)
            return;
        for (const auto& t : {std::array<int, 3>{a, b, c}, std::array<int, 3>{a, c, dd}}) {
            const SrcVertex &v0 = prim.vertices[size_t(t[0])], &v1 = prim.vertices[size_t(t[1])], &v2 = prim.vertices[size_t(t[2])];
            const V3 p0 = {v0.px, v0.py, v0.pz}, p1 = {v1.px, v1.py, v1.pz}, p2 = {v2.px, v2.py, v2.pz};
            const V3 fn = cross(p1 - p0, p2 - p0);
            const V3 sn = V3{v0.nx + v1.nx + v2.nx, v0.ny + v1.ny + v2.ny, v0.nz + v1.nz + v2.nz};
            if (dot(fn, fn) < 1e-14f)
                continue;
            const bool flip = dot(fn, sn) < 0.0f;
            prim.indices.push_back(uint32_t(t[0]));
            prim.indices.push_back(uint32_t(flip ? t[2] : t[1]));
            prim.indices.push_back(uint32_t(flip ? t[1] : t[2]));
        }
    };
    auto V = [&](int i, int j, int k) { return (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) ? -1 : vertexOf[cellIndex(i, j, k)]; };
    for (int k = 0; k <= nz; ++k)
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i <= nx; ++i) {
                const bool inside = D(i, j, k) < 0.0f;
                if (i < nx && inside != (D(i + 1, j, k) < 0.0f))
                    quad(V(i, j - 1, k - 1), V(i, j, k - 1), V(i, j, k), V(i, j - 1, k));
                if (j < ny && inside != (D(i, j + 1, k) < 0.0f))
                    quad(V(i - 1, j, k - 1), V(i, j, k - 1), V(i, j, k), V(i - 1, j, k));
                if (k < nz && inside != (D(i, j, k + 1) < 0.0f))
                    quad(V(i - 1, j - 1, k), V(i, j - 1, k), V(i, j, k), V(i - 1, j, k));
            }
    return prim;
}

float ellipsoid(V3 p, V3 r)
{
    const float k0 = length({p.x / r.x, p.y / r.y, p.z / r.z});
    return (k0 - 1.0f) * std::min({r.x, r.y, r.z});
}

float roundBox(V3 p, V3 half, float round)
{
    const V3 q = {std::fabs(p.x) - half.x + round, std::fabs(p.y) - half.y + round, std::fabs(p.z) - half.z + round};
    const V3 m = {std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    return length(m) + std::min(std::max({q.x, q.y, q.z}), 0.0f) - round;
}

struct Rock {
    const char* name;
    Field field;
    V3 lo, hi;
    float cell;
};

std::vector<Rock> rockSet()
{
    std::vector<Rock> rocks;
    // Boulders: squashed ellipsoids, faceted and rough; three sizes and shapes.
    const V3 boulderRadii[3] = {{1.0f, 0.75f, 0.85f}, {1.3f, 0.6f, 1.0f}, {0.8f, 0.9f, 0.7f}};
    const char* boulderNames[3] = {"rock_boulder_a", "rock_boulder_b", "rock_boulder_c"};
    for (uint32_t b = 0; b < 3; ++b) {
        const V3 r = boulderRadii[b], c = {0.0f, r.y * 0.65f, 0.0f};
        Field f = [r, c](V3 p) { return ellipsoid(p - c, r); };
        f = faceted(f, 9, std::max({r.x, r.y, r.z}), 0.62f, c, 11 + b);
        f = roughened(f, 0.07f, 2.2f, 21 + b);
        rocks.push_back({boulderNames[b], f, {-r.x - 0.2f, -0.3f, -r.z - 0.2f}, {r.x + 0.2f, c.y + r.y + 0.2f, r.z + 0.2f}, 0.045f});
    }
    // Slabs: flat rounded boxes with layered edges (strata), faceted.
    const V3 slabHalf[2] = {{1.6f, 0.38f, 1.1f}, {2.2f, 0.3f, 0.9f}};
    const char* slabNames[2] = {"rock_slab_a", "rock_slab_b"};
    for (uint32_t s = 0; s < 2; ++s) {
        const V3 h = slabHalf[s], c = {0.0f, h.y * 0.8f, 0.0f};
        Field f = [h, c](V3 p) {
            const V3 q = p - c;
            return roundBox(q, h, 0.12f) + 0.025f * std::fabs(std::sin(q.y * 26.0f));
        };
        f = faceted(f, 7, std::max(h.x, h.z), 0.78f, c, 31 + s);
        f = roughened(f, 0.05f, 1.8f, 41 + s);
        rocks.push_back({slabNames[s], f, {-h.x - 0.2f, -0.3f, -h.z - 0.2f}, {h.x + 0.2f, c.y + h.y + 0.2f, h.z + 0.2f}, 0.05f});
    }
    // Spires: tapering columns banded by strata, leaning a little.
    const float spireHeight[2] = {7.0f, 5.0f};
    const char* spireNames[2] = {"rock_spire_a", "rock_spire_b"};
    for (uint32_t s = 0; s < 2; ++s) {
        const float H = spireHeight[s];
        const float lean = 0.06f * (s == 0 ? 1.0f : -1.0f);
        Field f = [H, lean, s](V3 p) {
            const float t = std::clamp(p.y / H, 0.0f, 1.0f);
            const float r = 1.15f + (0.3f - 1.15f) * std::pow(t, 0.8f) + 0.12f * fbm3({p.x * 0.4f, p.y * 0.35f, p.z * 0.4f}, 3, 51 + s);
            const V3 axis = {p.x - lean * p.y, 0.0f, p.z};
            // Strata: shallow grooves whose height wanders around the column.
            const float strata = std::fabs(std::sin(p.y * 3.1f + 1.7f * fbm3({p.x * 0.6f, p.y * 0.15f, p.z * 0.6f}, 2, 57 + s)));
            const float side = length(axis) - r + 0.035f * strata;
            const float top = p.y - H, bottom = -p.y - 0.4f;
            return std::max({side, top, bottom});
        };
        f = faceted(f, 8, 1.2f, 0.85f, {0.0f, H * 0.4f, 0.0f}, 61 + s);
        f = roughened(f, 0.06f, 1.5f, 71 + s);
        rocks.push_back({spireNames[s], f, {-1.8f, -0.5f, -1.8f}, {1.8f + std::fabs(lean) * H, H + 0.3f, 1.8f}, 0.07f});
    }
    // Arch: half a torus standing on its legs, thicker at the feet.
    {
        const float R = 3.4f;
        Field f = [R](V3 p) {
            const float q = std::sqrt(p.x * p.x + p.y * p.y) - R;
            const float height = std::clamp(p.y / R, 0.0f, 1.0f);
            const float minor = 0.95f + 0.45f * (1.0f - height);
            const float torus = std::sqrt(q * q + p.z * p.z * 1.4f) - minor;
            return std::max(torus, -p.y - 0.4f);
        };
        f = faceted(f, 10, R + 1.0f, 0.92f, {0.0f, R * 0.5f, 0.0f}, 81);
        f = roughened(f, 0.09f, 1.3f, 91);
        rocks.push_back({"rock_arch", f, {-R - 1.7f, -0.6f, -1.6f}, {R + 1.7f, R + 1.3f, 1.6f}, 0.08f});
    }
    return rocks;
}

}  // namespace

void appendRockPrimitives(SrcScene& scene, std::vector<std::string>& names)
{
    // One material for all: plain and rough, for terrain-shaded instances to texture.
    fpak::MaterialGpu m = {};
    m.baseColor[0] = m.baseColor[1] = m.baseColor[2] = 0.45f;
    m.baseColor[3] = 1.0f;
    m.roughness = 0.9f;
    m.alphaCutoff = 0.5f;
    m.vtPageTable = fpak::kVtNoTexture;
    m.normalScale = 1.0f;
    m.occlusionStrength = 1.0f;
    const uint32_t material = uint32_t(scene.materials.size());
    scene.materials.push_back(m);
    scene.materialTextures.push_back({});
    for (const Rock& r : rockSet()) {
        SrcPrimitive p = surfaceNets(r.field, r.lo, r.hi, r.cell);
        p.material = material;
        names.resize(scene.primitives.size());
        names.push_back(r.name);
        scene.primitives.push_back(std::move(p));
    }
}

}  // namespace fx::cook
