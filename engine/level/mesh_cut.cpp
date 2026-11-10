#include "level/mesh_cut.h"

#include "level/wear.h"

#include <manifold/manifold.h>
#include <manifold/polygon.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace fx::level {

namespace {

// Per vertex: position, the corner's manual UV, mask UV, colour (rgba 0..1).
constexpr uint32_t kProps = 3 + 2 + 2 + 4;

float3 apply(const float w[3][4], float3 p, float t = 1.0f)
{
    return {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3] * t, w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3] * t,
            w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3] * t};
}

bool invert(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20f)
        return false;
    const float s = 1.0f / det;
    const float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                           {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                           {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
    return true;
}

manifold::mat3x4 toMat(const float w[3][4])
{
    return manifold::mat3x4({w[0][0], w[1][0], w[2][0]}, {w[0][1], w[1][1], w[2][1]}, {w[0][2], w[1][2], w[2][2]},
                            {w[0][3], w[1][3], w[2][3]});
}

// A face's triangles (corner indices within the face) in its plane: Manifold's triangulator.
std::vector<manifold::ivec3> triangles(const PolyMesh& m, uint32_t f)
{
    const Face& face = m.faces[f];
    if (face.count == 3)
        return {{0, 1, 2}};
    const float3 n = m.faceNormal(f);
    const float3 u = std::fabs(n.x) < 0.9f ? normalize(cross(n, float3{1, 0, 0})) : normalize(cross(n, float3{0, 1, 0}));
    const float3 v = cross(n, u);
    manifold::SimplePolygonIdx poly;
    for (uint32_t k = 0; k < face.count; ++k) {
        const float3 p = m.positions[m.corners[face.first + k]];
        poly.push_back({manifold::vec2(dot(p, u), dot(p, v)), int(k)});
    }
    try {
        return manifold::TriangulateIdx({poly});
    } catch (...) {
        return {};
    }
}

// The target as a Manifold in world space: a vertex per face corner (its own UVs, colour), merged
// back by the mesh's vertices; each triangle's faceID its face.
manifold::Manifold targetManifold(const PolyMesh& m, const float world[3][4], uint32_t* originalId = nullptr)
{
    manifold::MeshGL gl;
    gl.numProp = kProps;
    // Its own original ID, given here rather than by AsOriginal() (which drops the triangles'
    // faceID): after the cuts its triangles are found by it, and their faces by their faceID.
    const uint32_t id = manifold::Manifold::ReserveIDs(1);
    gl.runOriginalID = {id};
    if (originalId)
        *originalId = id;
    std::vector<int32_t> firstCorner(m.positions.size(), -1);
    for (uint32_t f = 0; f < m.faces.size(); ++f) {
        const Face& face = m.faces[f];
        const uint32_t base = uint32_t(gl.vertProperties.size() / kProps);
        for (uint32_t k = 0; k < face.count; ++k) {
            const uint32_t c = face.first + k, vertex = m.corners[c];
            const float3 p = apply(world, m.positions[vertex]);
            const float2 uv = m.cornerUvs.empty() ? float2{0, 0} : m.cornerUvs[c];
            const float2 uv1 = m.cornerUv1.empty() ? float2{0, 0} : m.cornerUv1[c];
            const uint32_t color = m.vertexColors.empty() ? 0xffffffffu : m.vertexColors[vertex];
            gl.vertProperties.insert(gl.vertProperties.end(), {p.x, p.y, p.z, uv.x, uv.y, uv1.x, uv1.y, float(color & 0xff) / 255.0f,
                                                               float(color >> 8 & 0xff) / 255.0f, float(color >> 16 & 0xff) / 255.0f,
                                                               float(color >> 24) / 255.0f});
            if (firstCorner[vertex] < 0) {
                firstCorner[vertex] = int32_t(base + k);
            } else {
                gl.mergeFromVert.push_back(base + k);
                gl.mergeToVert.push_back(uint32_t(firstCorner[vertex]));
            }
        }
        for (const manifold::ivec3& t : triangles(m, f)) {
            gl.triVerts.insert(gl.triVerts.end(), {base + uint32_t(t[0]), base + uint32_t(t[1]), base + uint32_t(t[2])});
            gl.faceID.push_back(f);
        }
    }
    return manifold::Manifold(gl);
}

manifold::Manifold cutManifold(const Cut& cut)
{
    manifold::Manifold shape;
    switch (cut.shape) {
    case CutShape::Box: shape = manifold::Manifold::Cube({cut.size[0], cut.size[1], cut.size[2]}, true); break;
    case CutShape::Cylinder:  // (Manifold's runs along z: turned to run along y, centred)
        shape = manifold::Manifold::Cylinder(cut.size[1], cut.size[0] * 0.5, cut.size[0] * 0.5, 32, true)
                    .Rotate(-90.0, 0.0, 0.0)
                    .Scale({1.0, 1.0, double(cut.size[2]) / std::max(double(cut.size[0]), 1e-6)});
        break;
    case CutShape::Sphere: shape = manifold::Manifold::Sphere(cut.size[0] * 0.5, 32); break;
    case CutShape::Damage: {  // each point of a unit ball moved along its direction by two octaves of noise
        // (64 segments: the fine octave sampled finely enough not to come out as spikes)
        const float rough = std::clamp(cut.roughness, 0.0f, 0.9f);
        const uint32_t seed = cut.seed;
        shape = manifold::Manifold::Sphere(0.5, 64)
                    .Warp([rough, seed](manifold::vec3& p) {
                        const float3 d = {float(p.x), float(p.y), float(p.z)};
                        const float n = valueNoise(d, 0.22f, seed) * 0.7f + valueNoise(d, 0.08f, seed + 11) * 0.3f;
                        p *= double(1.0f + rough * (n * 2.0f - 1.0f));
                    })
                    .Scale({cut.size[0], cut.size[1], cut.size[2]});
        break;
    }
    case CutShape::Mesh:
        if (cut.mesh) {
            const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
            shape = targetManifold(*cut.mesh, identity);
        }
        break;
    }
    return shape.Transform(toMat(cut.world)).AsOriginal();
}

// Clipping an open target: polygons (a vertex: position and the corner's properties, kProps
// floats) split by the cutter's convex pieces (planes n.p <= d inside).
struct ClipVertex {
    float p[kProps];
};
struct ClipPolygon {
    std::vector<ClipVertex> v;
    uint32_t face;
};
struct Plane {
    double n[3], d;
};
struct Piece {
    std::vector<Plane> planes;
    float3 lo, hi;
};

double side(const Plane& pl, const float* p)
{
    return pl.n[0] * p[0] + pl.n[1] * p[1] + pl.n[2] * p[2] - pl.d;
}

void boundsOf(const std::vector<ClipVertex>& v, float3& lo, float3& hi)
{
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (const ClipVertex& c : v) {
        lo = {std::min(lo.x, c.p[0]), std::min(lo.y, c.p[1]), std::min(lo.z, c.p[2])};
        hi = {std::max(hi.x, c.p[0]), std::max(hi.y, c.p[1]), std::max(hi.z, c.p[2])};
    }
}

bool overlaps(float3 alo, float3 ahi, float3 blo, float3 bhi)
{
    return alo.x <= bhi.x && ahi.x >= blo.x && alo.y <= bhi.y && ahi.y >= blo.y && alo.z <= bhi.z && ahi.z >= blo.z;
}

double area(const std::vector<ClipVertex>& v)
{
    double x = 0, y = 0, z = 0;
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        const double a[3] = {v[i].p[0] - v[0].p[0], v[i].p[1] - v[0].p[1], v[i].p[2] - v[0].p[2]};
        const double b[3] = {v[i + 1].p[0] - v[0].p[0], v[i + 1].p[1] - v[0].p[1], v[i + 1].p[2] - v[0].p[2]};
        x += a[1] * b[2] - a[2] * b[1];
        y += a[2] * b[0] - a[0] * b[2];
        z += a[0] * b[1] - a[1] * b[0];
    }
    return 0.5 * std::sqrt(x * x + y * y + z * z);
}

// Where an edge crosses a plane, the same bits whichever way round the edge is taken (neighbouring
// triangles share it: welded back by position).
ClipVertex crossing(const ClipVertex& a, const ClipVertex& b, const Plane& pl)
{
    const bool swap = std::lexicographical_compare(b.p, b.p + 3, a.p, a.p + 3);
    const ClipVertex& u = swap ? b : a;
    const ClipVertex& w = swap ? a : b;
    const double du = side(pl, u.p), dw = side(pl, w.p);
    const double t = du / (du - dw);
    ClipVertex x;
    for (uint32_t k = 0; k < kProps; ++k)
        x.p[k] = float(double(u.p[k]) + (double(w.p[k]) - double(u.p[k])) * t);
    return x;
}

// A convex polygon's parts behind (inside) and in front of a plane.
void split(const std::vector<ClipVertex>& poly, const Plane& pl, std::vector<ClipVertex>& in, std::vector<ClipVertex>& out)
{
    constexpr double kOn = 1e-7;
    in.clear();
    out.clear();
    bool anyIn = false, anyOut = false;
    for (const ClipVertex& v : poly) {
        const double d = side(pl, v.p);
        anyIn |= d < -kOn;
        anyOut |= d > kOn;
    }
    if (!anyOut) {
        in = poly;
        return;
    }
    if (!anyIn) {
        out = poly;
        return;
    }
    for (size_t i = 0; i < poly.size(); ++i) {
        const ClipVertex& a = poly[i];
        const ClipVertex& b = poly[(i + 1) % poly.size()];
        const double da = side(pl, a.p), db = side(pl, b.p);
        if (da <= kOn)
            in.push_back(a);
        if (da >= -kOn)
            out.push_back(a);
        if ((da < -kOn && db > kOn) || (da > kOn && db < -kOn)) {
            const ClipVertex x = crossing(a, b, pl);
            in.push_back(x);
            out.push_back(x);
        }
    }
}

// The cutter (in world space) as convex pieces: itself when convex, else a tetrahedron per face
// from a centre it is star-shaped about (its origin, or its vertices' middle). False: neither.
bool convexPieces(const manifold::Manifold& cutter, const float3 origin, std::vector<Piece>& out)
{
    const manifold::MeshGL gl = cutter.GetMeshGL();
    const uint32_t props = gl.numProp, n = uint32_t(gl.vertProperties.size() / std::max(props, 1u));
    std::vector<float3> v(n);
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f}, middle = {0, 0, 0};
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = {gl.vertProperties[size_t(i) * props], gl.vertProperties[size_t(i) * props + 1], gl.vertProperties[size_t(i) * props + 2]};
        lo = {std::min(lo.x, v[i].x), std::min(lo.y, v[i].y), std::min(lo.z, v[i].z)};
        hi = {std::max(hi.x, v[i].x), std::max(hi.y, v[i].y), std::max(hi.z, v[i].z)};
        middle = middle + v[i] * (1.0f / float(std::max(n, 1u)));
    }
    const double size = std::max({double(hi.x - lo.x), double(hi.y - lo.y), double(hi.z - lo.z), 1e-6});
    const double eps = 1e-6 * size;
    struct Tri {
        uint32_t a, b, c;
        Plane plane;
    };
    std::vector<Tri> tris;
    for (size_t t = 0; t + 2 < gl.triVerts.size(); t += 3) {
        const uint32_t a = gl.triVerts[t], b = gl.triVerts[t + 1], c = gl.triVerts[t + 2];
        const double e1[3] = {double(v[b].x) - v[a].x, double(v[b].y) - v[a].y, double(v[b].z) - v[a].z};
        const double e2[3] = {double(v[c].x) - v[a].x, double(v[c].y) - v[a].y, double(v[c].z) - v[a].z};
        double nn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len < 1e-12 * size * size)
            continue;  // (degenerate)
        Plane pl{{nn[0] / len, nn[1] / len, nn[2] / len}, 0};
        pl.d = pl.n[0] * v[a].x + pl.n[1] * v[a].y + pl.n[2] * v[a].z;
        tris.push_back({a, b, c, pl});
    }
    if (tris.empty())
        return false;
    // Convex: every vertex behind every face's plane. One piece, its planes (each once).
    bool convex = true;
    for (const Tri& t : tris) {
        for (const float3& p : v) {
            const float q[3] = {p.x, p.y, p.z};
            if (side(t.plane, q) > eps * 100.0) {
                convex = false;
                break;
            }
        }
        if (!convex)
            break;
    }
    if (convex) {
        Piece piece{{}, lo, hi};
        for (const Tri& t : tris) {
            bool known = false;
            for (const Plane& o : piece.planes)
                known |= t.plane.n[0] * o.n[0] + t.plane.n[1] * o.n[1] + t.plane.n[2] * o.n[2] > 1.0 - 1e-9 && std::fabs(t.plane.d - o.d) < eps;
            if (!known)
                piece.planes.push_back(t.plane);
        }
        out.push_back(std::move(piece));
        return true;
    }
    // Star-shaped about a centre: every face's plane has it strictly behind.
    for (const float3 c : {origin, middle}) {
        const float q[3] = {c.x, c.y, c.z};
        bool star = true;
        for (const Tri& t : tris)
            if (side(t.plane, q) > -eps) {
                star = false;
                break;
            }
        if (!star)
            continue;
        for (const Tri& t : tris) {
            Piece piece;
            piece.planes.push_back(t.plane);
            const uint32_t k[3] = {t.a, t.b, t.c};
            for (int e = 0; e < 3; ++e) {  // (through the centre and an edge, the third corner behind it)
                const float3 a = v[k[e]], b = v[k[(e + 1) % 3]], o = v[k[(e + 2) % 3]];
                const double u[3] = {double(a.x) - c.x, double(a.y) - c.y, double(a.z) - c.z};
                const double w[3] = {double(b.x) - c.x, double(b.y) - c.y, double(b.z) - c.z};
                double nn[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
                const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (len < 1e-30)
                    continue;
                Plane pl{{nn[0] / len, nn[1] / len, nn[2] / len}, 0};
                pl.d = pl.n[0] * c.x + pl.n[1] * c.y + pl.n[2] * c.z;
                const float oq[3] = {o.x, o.y, o.z};
                if (side(pl, oq) > 0)
                    pl = {{-pl.n[0], -pl.n[1], -pl.n[2]}, -pl.d};
                piece.planes.push_back(pl);
            }
            piece.lo = {std::min({c.x, v[t.a].x, v[t.b].x, v[t.c].x}), std::min({c.y, v[t.a].y, v[t.b].y, v[t.c].y}),
                        std::min({c.z, v[t.a].z, v[t.b].z, v[t.c].z})};
            piece.hi = {std::max({c.x, v[t.a].x, v[t.b].x, v[t.c].x}), std::max({c.y, v[t.a].y, v[t.b].y, v[t.c].y}),
                        std::max({c.z, v[t.a].z, v[t.b].z, v[t.c].z})};
            out.push_back(std::move(piece));
        }
        return true;
    }
    return false;
}

// Points of other polygons lying on a polygon's edge put into that edge (splitting one polygon by
// plane after plane leaves T-junctions: hairline cracks when drawn). Their properties interpolated
// along the edge, their positions the very point's (welded).
void closeTJunctions(std::vector<ClipPolygon>& polys)
{
    constexpr double kOn = 2e-5;
    std::vector<float3> points;
    std::unordered_map<uint64_t, std::vector<uint32_t>> seen;
    for (const ClipPolygon& poly : polys)
        for (const ClipVertex& c : poly.v) {
            uint32_t bits[3];
            std::memcpy(bits, c.p, 12);
            const uint64_t key = (uint64_t(bits[0]) * 73856093u) ^ (uint64_t(bits[1]) * 19349663u) ^ (uint64_t(bits[2]) * 83492791u);
            bool known = false;
            for (const uint32_t o : seen[key])
                known |= std::memcmp(&points[o], c.p, 12) == 0;
            if (!known) {
                seen[key].push_back(uint32_t(points.size()));
                points.push_back({c.p[0], c.p[1], c.p[2]});
            }
        }
    std::sort(points.begin(), points.end(), [](float3 a, float3 b) { return a.x < b.x; });
    std::vector<std::pair<double, uint32_t>> on;
    for (ClipPolygon& poly : polys) {
        std::vector<ClipVertex> v;
        for (size_t i = 0; i < poly.v.size(); ++i) {
            const ClipVertex& a = poly.v[i];
            const ClipVertex& b = poly.v[(i + 1) % poly.v.size()];
            v.push_back(a);
            const double e[3] = {double(b.p[0]) - a.p[0], double(b.p[1]) - a.p[1], double(b.p[2]) - a.p[2]};
            const double len2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            if (len2 < kOn * kOn)
                continue;
            on.clear();
            const float x0 = float(std::min(a.p[0], b.p[0]) - kOn), x1 = float(std::max(a.p[0], b.p[0]) + kOn);
            auto it = std::lower_bound(points.begin(), points.end(), x0, [](float3 p, float x) { return p.x < x; });
            for (; it != points.end() && it->x <= x1; ++it) {
                const double d[3] = {double(it->x) - a.p[0], double(it->y) - a.p[1], double(it->z) - a.p[2]};
                const double t = (d[0] * e[0] + d[1] * e[1] + d[2] * e[2]) / len2;
                if (t <= 1e-6 || t >= 1.0 - 1e-6)
                    continue;
                const double off[3] = {d[0] - e[0] * t, d[1] - e[1] * t, d[2] - e[2] * t};
                if (off[0] * off[0] + off[1] * off[1] + off[2] * off[2] < kOn * kOn)
                    on.push_back({t, uint32_t(it - points.begin())});
            }
            std::sort(on.begin(), on.end());
            for (const auto& [t, p] : on) {
                ClipVertex x;
                for (uint32_t k = 3; k < kProps; ++k)
                    x.p[k] = float(double(a.p[k]) + (double(b.p[k]) - double(a.p[k])) * t);
                x.p[0] = points[p].x, x.p[1] = points[p].y, x.p[2] = points[p].z;
                v.push_back(x);
            }
        }
        poly.v = std::move(v);
    }
}

// The open target clipped by the cuts (world space), back in the mesh's own space.
bool clipMesh(const PolyMesh& target, const float world[3][4], const float back[3][4], const std::vector<Cut>& cuts, CutResult& out)
{
    constexpr double kSliver = 1e-10;
    std::vector<ClipPolygon> polys;
    for (uint32_t f = 0; f < target.faces.size(); ++f) {
        const Face& face = target.faces[f];
        for (const manifold::ivec3& t : triangles(target, f)) {
            ClipPolygon poly{{}, f};
            for (int k = 0; k < 3; ++k) {
                const uint32_t c = face.first + uint32_t(t[k]), vertex = target.corners[c];
                const float3 p = apply(world, target.positions[vertex]);
                const float2 uv = target.cornerUvs.empty() ? float2{0, 0} : target.cornerUvs[c];
                const float2 uv1 = target.cornerUv1.empty() ? float2{0, 0} : target.cornerUv1[c];
                const uint32_t color = target.vertexColors.empty() ? 0xffffffffu : target.vertexColors[vertex];
                poly.v.push_back({{p.x, p.y, p.z, uv.x, uv.y, uv1.x, uv1.y, float(color & 0xff) / 255.0f, float(color >> 8 & 0xff) / 255.0f,
                                   float(color >> 16 & 0xff) / 255.0f, float(color >> 24) / 255.0f}});
            }
            polys.push_back(std::move(poly));
        }
    }
    std::string why;
    std::vector<ClipVertex> in, outside;
    for (const Cut& cut : cuts) {
        std::vector<Piece> pieces;
        const manifold::Manifold cutter = cutManifold(cut);
        if (cutter.Status() != manifold::Manifold::Error::NoError || cutter.IsEmpty())
            continue;
        if (!convexPieces(cutter, {cut.world[0][3], cut.world[1][3], cut.world[2][3]}, pieces)) {
            why = "the mesh is not closed, and a cutter that isn't star-shaped can't clip it";
            continue;
        }
        float3 clo, chi;
        cutBounds(cut, clo, chi);
        double dropped = 0.0;
        std::vector<ClipPolygon> next;
        for (ClipPolygon& poly : polys) {
            float3 plo, phi;
            boundsOf(poly.v, plo, phi);
            if (!overlaps(plo, phi, clo, chi)) {
                if (cut.intersect)
                    dropped += area(poly.v);
                else
                    next.push_back(std::move(poly));
                continue;
            }
            const double before = area(poly.v);
            double kept = 0.0;
            if (cut.intersect) {  // what is inside any piece (they don't overlap)
                for (const Piece& piece : pieces) {
                    if (!overlaps(plo, phi, piece.lo, piece.hi))
                        continue;
                    std::vector<ClipVertex> rem = poly.v;
                    for (const Plane& pl : piece.planes) {
                        split(rem, pl, in, outside);
                        rem.swap(in);
                        if (rem.size() < 3)
                            break;
                    }
                    if (rem.size() >= 3 && area(rem) > kSliver) {
                        kept += area(rem);
                        next.push_back({std::move(rem), poly.face});
                    }
                }
            } else {  // what is outside every piece
                std::vector<std::vector<ClipVertex>> parts = {poly.v};
                for (const Piece& piece : pieces) {
                    std::vector<std::vector<ClipVertex>> after;
                    for (std::vector<ClipVertex>& part : parts) {
                        float3 alo, ahi;
                        boundsOf(part, alo, ahi);
                        if (!overlaps(alo, ahi, piece.lo, piece.hi)) {
                            after.push_back(std::move(part));
                            continue;
                        }
                        std::vector<ClipVertex> rem = std::move(part);
                        for (const Plane& pl : piece.planes) {
                            split(rem, pl, in, outside);
                            if (outside.size() >= 3 && area(outside) > kSliver)
                                after.push_back(outside);
                            rem.swap(in);
                            if (rem.size() < 3)
                                break;
                        }
                    }
                    parts.swap(after);
                }
                for (std::vector<ClipVertex>& part : parts) {
                    kept += area(part);
                    next.push_back({std::move(part), poly.face});
                }
            }
            dropped += std::max(before - kept, 0.0);
        }
        polys.swap(next);
        if (dropped > 1e-8)
            ++out.applied;
    }
    if (!out.applied) {
        out.error = why;
        return why.empty();
    }
    closeTJunctions(polys);
    // Back to a PolyMesh: a face per polygon, its source face's settings; welded by position.
    PolyMesh m;
    m.materials = target.materials.empty() ? std::vector<std::string>{""} : target.materials;
    for (int k = 0; k < 3; ++k)
        m.layerMaterials[k] = target.layerMaterials[k];
    m.layerBlend = target.layerBlend;
    const bool manual = !target.cornerUvs.empty(), mask = !target.cornerUv1.empty(), colors = !target.vertexColors.empty();
    std::unordered_map<uint64_t, std::vector<uint32_t>> at;
    std::vector<float3> worldOf;
    for (const ClipPolygon& poly : polys) {
        Face face = target.faces[poly.face];
        face.first = uint32_t(m.corners.size());
        for (const ClipVertex& c : poly.v) {
            uint32_t bits[3];
            std::memcpy(bits, c.p, 12);
            const uint64_t key = (uint64_t(bits[0]) * 73856093u) ^ (uint64_t(bits[1]) * 19349663u) ^ (uint64_t(bits[2]) * 83492791u);
            uint32_t found = ~0u;
            for (const uint32_t o : at[key])
                if (std::memcmp(&worldOf[o], c.p, 12) == 0)
                    found = o;
            if (found == ~0u) {
                found = uint32_t(m.positions.size());
                at[key].push_back(found);
                worldOf.push_back({c.p[0], c.p[1], c.p[2]});
                m.positions.push_back(apply(back, {c.p[0], c.p[1], c.p[2]}));
                if (colors) {
                    uint32_t rgba = 0;
                    for (int ch = 0; ch < 4; ++ch)
                        rgba |= uint32_t(std::lround(std::clamp(c.p[7 + ch], 0.0f, 1.0f) * 255.0f)) << (8 * ch);
                    m.vertexColors.push_back(rgba);
                }
            }
            if (m.corners.size() > face.first && (m.corners.back() == found || m.corners[face.first] == found))
                continue;  // (the same point twice: one corner)
            m.corners.push_back(found);
            if (manual)
                m.cornerUvs.push_back({c.p[3], c.p[4]});
            if (mask)
                m.cornerUv1.push_back({c.p[5], c.p[6]});
        }
        face.count = uint32_t(m.corners.size()) - face.first;
        if (face.count < 3) {  // (a sliver gone to a line)
            m.corners.resize(face.first);
            if (manual)
                m.cornerUvs.resize(face.first);
            if (mask)
                m.cornerUv1.resize(face.first);
            continue;
        }
        m.faces.push_back(face);
        out.sourceFace.push_back(poly.face);
    }
    out.clipped = true;
    if (!m.valid(&out.error)) {
        out.sourceFace.clear();
        out.applied = 0;
        out.clipped = false;
        return out.mesh = target, false;
    }
    out.mesh = std::move(m);
    return true;
}

}  // namespace

void cutBounds(const Cut& cut, float3& lo, float3& hi)
{
    float3 a = {-cut.size[0] * 0.5f, -cut.size[1] * 0.5f, -cut.size[2] * 0.5f}, b = a * -1.0f;
    if (cut.shape == CutShape::Sphere)
        a = float3{-1, -1, -1} * (cut.size[0] * 0.5f), b = a * -1.0f;
    if (cut.shape == CutShape::Damage)
        a = a * (1.0f + std::clamp(cut.roughness, 0.0f, 0.9f)), b = a * -1.0f;
    if (cut.shape == CutShape::Mesh && cut.mesh)
        cut.mesh->bounds(a, b);
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        const float3 p = apply(cut.world, {k & 1 ? b.x : a.x, k & 2 ? b.y : a.y, k & 4 ? b.z : a.z});
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
}

bool cutMesh(const PolyMesh& target, const float world[3][4], const std::vector<Cut>& cuts, CutResult& out)
{
    out = {};
    out.mesh = target;
    if (cuts.empty())
        return true;
    float back[3][4];
    if (!invert(world, back))
        return out.error = "the entity's transform is flat (no inverse)", false;
    uint32_t targetId = 0;
    manifold::Manifold solid = targetManifold(target, world, &targetId);
    if (solid.Status() != manifold::Manifold::Error::NoError || solid.IsEmpty())  // (not a solid: clipped, no caps)
        return clipMesh(target, world, back, cuts, out);
    std::unordered_map<uint32_t, size_t> cutOfId;  // a cutter's original ID -> its cut
    for (size_t i = 0; i < cuts.size(); ++i) {
        const manifold::Manifold cutter = cutManifold(cuts[i]);
        if (cutter.Status() != manifold::Manifold::Error::NoError || cutter.IsEmpty())
            continue;
        cutOfId[uint32_t(cutter.OriginalID())] = i;
        const manifold::Manifold next = cuts[i].intersect ? solid ^ cutter : solid - cutter;
        if (next.Status() != manifold::Manifold::Error::NoError)
            continue;
        if (next.NumTri() != solid.NumTri() || std::fabs(next.Volume() - solid.Volume()) > 1e-9)
            ++out.applied;
        solid = next;
    }
    if (!out.applied)
        return true;
    // Back to a PolyMesh: a face per triangle, its source face's settings (or the cut's cap).
    const manifold::MeshGL gl = solid.GetMeshGL();
    PolyMesh m;
    m.materials = target.materials.empty() ? std::vector<std::string>{""} : target.materials;
    for (int k = 0; k < 3; ++k)
        m.layerMaterials[k] = target.layerMaterials[k];
    m.layerBlend = target.layerBlend;
    const uint32_t props = gl.numProp, vertexCount = uint32_t(gl.vertProperties.size() / std::max(props, 1u));
    const bool manual = !target.cornerUvs.empty(), mask = !target.cornerUv1.empty(), colors = !target.vertexColors.empty();
    // Manifold keeps a vertex apart wherever the corners' properties differ (UV seams): welded back
    // by position (the mesh closed again, smoothing across them), the properties kept per corner.
    std::vector<uint32_t> weld(vertexCount);
    std::unordered_map<uint64_t, std::vector<uint32_t>> at;
    for (uint32_t v = 0; v < vertexCount; ++v) {
        const float* p = &gl.vertProperties[size_t(v) * props];
        uint32_t bits[3];
        std::memcpy(bits, p, 12);
        const uint64_t key = (uint64_t(bits[0]) * 73856093u) ^ (uint64_t(bits[1]) * 19349663u) ^ (uint64_t(bits[2]) * 83492791u);
        uint32_t found = ~0u;
        for (const uint32_t o : at[key])
            if (std::memcmp(&gl.vertProperties[size_t(o) * props], p, 12) == 0)
                found = weld[o];
        if (found != ~0u) {
            weld[v] = found;
            continue;
        }
        at[key].push_back(v);
        weld[v] = uint32_t(m.positions.size());
        m.positions.push_back(apply(back, {p[0], p[1], p[2]}));
        if (colors) {
            uint32_t c = 0;
            for (int ch = 0; ch < 4; ++ch)
                c |= uint32_t(std::lround(std::clamp(props > 7u + ch ? p[7 + ch] : 1.0f, 0.0f, 1.0f) * 255.0f)) << (8 * ch);
            m.vertexColors.push_back(c);
        }
    }
    std::unordered_map<std::string, uint32_t> capSlot;
    const uint32_t tris = uint32_t(gl.triVerts.size() / 3);
    for (uint32_t run = 0; run + 1 < gl.runIndex.size(); ++run) {
        const uint32_t id = run < gl.runOriginalID.size() ? gl.runOriginalID[run] : 0u;
        for (uint32_t t = gl.runIndex[run] / 3; t < gl.runIndex[run + 1] / 3 && t < tris; ++t) {
            // (a sliver whose corners weld into one another, across a UV seam: none)
            const uint32_t w0 = weld[gl.triVerts[size_t(t) * 3]], w1 = weld[gl.triVerts[size_t(t) * 3 + 1]], w2 = weld[gl.triVerts[size_t(t) * 3 + 2]];
            if (w0 == w1 || w1 == w2 || w2 == w0)
                continue;
            Face face;
            if (id == targetId && t < gl.faceID.size() && gl.faceID[t] < target.faces.size()) {
                face = target.faces[gl.faceID[t]];
                out.sourceFace.push_back(gl.faceID[t]);
            } else {
                out.sourceFace.push_back(~0u);  // a cap: the cut's material, projected
                const auto cut = cutOfId.find(id);
                const std::string material = cut != cutOfId.end() ? cuts[cut->second].capMaterial : std::string();
                uint32_t slot = 0;
                if (!material.empty()) {
                    const auto known = capSlot.find(material);
                    if (known != capSlot.end()) {
                        slot = known->second;
                    } else {
                        const auto it = std::find(m.materials.begin(), m.materials.end(), material);
                        slot = uint32_t(it - m.materials.begin());
                        if (it == m.materials.end())
                            m.materials.push_back(material);
                        capSlot[material] = slot;
                    }
                }
                face = Face{};
                face.material = slot;
                // A rounded cutter's faces shaded smooth together (one group a cut, apart from the
                // target's), not as facets.
                if (cut != cutOfId.end() && (cuts[cut->second].shape == CutShape::Damage || cuts[cut->second].shape == CutShape::Sphere))
                    face.smoothing = 0x40000000u + uint32_t(cut->second);
            }
            face.first = uint32_t(m.corners.size());
            face.count = 3;
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = gl.triVerts[size_t(t) * 3 + k];
                const float* p = &gl.vertProperties[size_t(v) * props];
                m.corners.push_back(weld[v]);
                if (manual)
                    m.cornerUvs.push_back({props > 3 ? p[3] : 0.0f, props > 4 ? p[4] : 0.0f});
                if (mask)
                    m.cornerUv1.push_back({props > 5 ? p[5] : 0.0f, props > 6 ? p[6] : 0.0f});
            }
            m.faces.push_back(face);
        }
    }
    if (!m.valid(&out.error)) {
        out.sourceFace.clear();
        return out.mesh = target, false;
    }
    out.mesh = std::move(m);
    return true;
}

}  // namespace fx::level
