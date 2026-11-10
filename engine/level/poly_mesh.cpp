#include "level/poly_mesh.h"

#include "render/live_mesh_data.h"

#include <manifold/polygon.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace fx::level {
namespace {

constexpr char kMagic[4] = {'F', 'X', 'L', 'M'};
constexpr uint32_t kFormat = 2;  // (1: no mask UVs, vertex colours or layers)
constexpr float kPi = 3.14159265358979f;

float3 rows(const float m[3][4], float3 p, float w)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3] * w, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3] * w,
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] * w};
}

// Writes and reads plain values in order (little-endian, as the machine is).
struct Writer {
    std::vector<uint8_t> out;
    template <typename T>
    void put(const T& v)
    {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }
    void bytes(const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
};
struct Reader {
    const uint8_t* p;
    size_t left;
    template <typename T>
    bool get(T& v)
    {
        if (left < sizeof(T))
            return false;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return true;
    }
    bool bytes(void* d, size_t n)
    {
        if (left < n)
            return false;
        std::memcpy(d, p, n);
        p += n;
        left -= n;
        return true;
    }
};

}  // namespace

uint32_t PolyMesh::triangleCount() const
{
    uint32_t n = 0;
    for (const Face& f : faces)
        n += f.count >= 3 ? f.count - 2 : 0;
    return n;
}

bool PolyMesh::valid(std::string* why) const
{
    auto fail = [&](const std::string& w) {
        if (why)
            *why = w;
        return false;
    };
    if (!cornerUvs.empty() && cornerUvs.size() != corners.size())
        return fail("cornerUvs is not parallel to corners");
    if (!cornerUv1.empty() && cornerUv1.size() != corners.size())
        return fail("cornerUv1 is not parallel to corners");
    if (!vertexColors.empty() && vertexColors.size() != positions.size())
        return fail("vertexColors is not parallel to positions");
    for (uint32_t c : corners)
        if (c >= positions.size())
            return fail("a corner past the vertices");
    for (size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.count < 3 || uint64_t(face.first) + face.count > corners.size())
            return fail("face " + std::to_string(f) + ": fewer than 3 corners, or past the corners");
        if (face.material >= std::max<size_t>(materials.size(), 1))
            return fail("face " + std::to_string(f) + ": a material slot past the slots");
        for (uint32_t i = 0; i < face.count; ++i)
            if (corners[face.first + i] == corners[face.first + (i + 1) % face.count])
                return fail("face " + std::to_string(f) + ": the same vertex twice in a row");
    }
    return true;
}

uint32_t PolyMesh::addFace(const std::vector<uint32_t>& loop, uint32_t material, uint32_t smoothing)
{
    Face f;
    f.first = uint32_t(corners.size());
    f.count = uint32_t(loop.size());
    f.material = material;
    f.smoothing = smoothing;
    corners.insert(corners.end(), loop.begin(), loop.end());
    if (!cornerUvs.empty())
        cornerUvs.resize(corners.size(), float2{0.0f, 0.0f});
    faces.push_back(f);
    return uint32_t(faces.size() - 1);
}

float3 PolyMesh::faceNormal(uint32_t face) const
{
    const Face& f = faces[face];
    float3 n = {0, 0, 0};
    for (uint32_t i = 0; i < f.count; ++i) {
        const float3 a = positions[corners[f.first + i]];
        const float3 b = positions[corners[f.first + (i + 1) % f.count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return normalize(n);
}

float3 PolyMesh::faceCenter(uint32_t face) const
{
    const Face& f = faces[face];
    float3 c = {0, 0, 0};
    for (uint32_t i = 0; i < f.count; ++i)
        c += positions[corners[f.first + i]];
    return f.count ? c * (1.0f / float(f.count)) : c;
}

void PolyMesh::bounds(float3& lo, float3& hi) const
{
    lo = hi = positions.empty() ? float3{0, 0, 0} : positions[0];
    for (const float3& p : positions) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
}

uint64_t PolyMesh::hash() const
{
    const std::vector<uint8_t> bytes = save();
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes)
        h = (h ^ b) * 0x100000001b3ull;
    return h;
}

std::vector<uint8_t> PolyMesh::save() const
{
    Writer w;
    w.bytes(kMagic, 4);
    w.put(kFormat);
    w.put(uint32_t(positions.size()));
    w.put(uint32_t(corners.size()));
    w.put(uint32_t(faces.size()));
    w.put(uint32_t(materials.size()));
    const bool layers = layered() || layerBlend != 0.15f;
    // flags: 1 corner UVs, 2 mask UVs, 4 vertex colours, 8 layers
    w.put(uint32_t((cornerUvs.empty() ? 0u : 1u) | (cornerUv1.empty() ? 0u : 2u) | (vertexColors.empty() ? 0u : 4u) | (layers ? 8u : 0u)));
    w.bytes(positions.data(), positions.size() * sizeof(float3));
    w.bytes(corners.data(), corners.size() * sizeof(uint32_t));
    if (!cornerUvs.empty())
        w.bytes(cornerUvs.data(), cornerUvs.size() * sizeof(float2));
    for (const Face& f : faces) {
        w.put(f.first);
        w.put(f.count);
        w.put(f.material);
        w.put(f.smoothing);
        w.put(uint32_t(f.uv.mode));
        w.put(f.uv.offset[0]);
        w.put(f.uv.offset[1]);
        w.put(f.uv.scale[0]);
        w.put(f.uv.scale[1]);
        w.put(f.uv.rotation);
    }
    for (const std::string& m : materials) {
        w.put(uint32_t(m.size()));
        w.bytes(m.data(), m.size());
    }
    if (!cornerUv1.empty())
        w.bytes(cornerUv1.data(), cornerUv1.size() * sizeof(float2));
    if (!vertexColors.empty())
        w.bytes(vertexColors.data(), vertexColors.size() * sizeof(uint32_t));
    if (layers) {
        for (const std::string& m : layerMaterials) {
            w.put(uint32_t(m.size()));
            w.bytes(m.data(), m.size());
        }
        w.put(layerBlend);
    }
    return std::move(w.out);
}

bool PolyMesh::load(const uint8_t* data, size_t size)
{
    Reader r{data, size};
    char magic[4];
    uint32_t format = 0, np = 0, nc = 0, nf = 0, nm = 0, flags = 0;
    if (!r.bytes(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !r.get(format) || format < 1 || format > kFormat || !r.get(np) ||
        !r.get(nc) || !r.get(nf) || !r.get(nm) || !r.get(flags))
        return false;
    // (each count bounded by what is left, so a bad header can't ask for gigabytes)
    if (uint64_t(np) * sizeof(float3) > r.left || uint64_t(nc) * 4 > r.left || uint64_t(nf) * 40 > r.left || nm > r.left)
        return false;
    PolyMesh m;
    m.positions.resize(np);
    m.corners.resize(nc);
    if (!r.bytes(m.positions.data(), np * sizeof(float3)) || !r.bytes(m.corners.data(), nc * sizeof(uint32_t)))
        return false;
    if (flags & 1u) {
        m.cornerUvs.resize(nc);
        if (!r.bytes(m.cornerUvs.data(), nc * sizeof(float2)))
            return false;
    }
    m.faces.resize(nf);
    for (Face& f : m.faces) {
        uint32_t mode = 0;
        if (!r.get(f.first) || !r.get(f.count) || !r.get(f.material) || !r.get(f.smoothing) || !r.get(mode) ||
            !r.get(f.uv.offset[0]) || !r.get(f.uv.offset[1]) || !r.get(f.uv.scale[0]) || !r.get(f.uv.scale[1]) ||
            !r.get(f.uv.rotation) || mode > uint32_t(UvMode::Manual))
            return false;
        f.uv.mode = UvMode(mode);
    }
    m.materials.resize(nm);
    const auto string = [&](std::string& s) {
        uint32_t len = 0;
        if (!r.get(len) || len > r.left)
            return false;
        s.resize(len);
        return r.bytes(s.data(), len);
    };
    for (std::string& s : m.materials)
        if (!string(s))
            return false;
    if (flags & 2u) {
        m.cornerUv1.resize(nc);
        if (!r.bytes(m.cornerUv1.data(), nc * sizeof(float2)))
            return false;
    }
    if (flags & 4u) {
        m.vertexColors.resize(np);
        if (!r.bytes(m.vertexColors.data(), np * sizeof(uint32_t)))
            return false;
    }
    if (flags & 8u) {
        for (std::string& s : m.layerMaterials)
            if (!string(s))
                return false;
        if (!r.get(m.layerBlend))
            return false;
    }
    if (!m.valid())
        return false;
    *this = std::move(m);
    return true;
}

std::string PolyMesh::saveText() const
{
    static const char* const kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::vector<uint8_t> b = save();
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4);
    for (size_t i = 0; i < b.size(); i += 3) {
        const uint32_t n = uint32_t(b[i]) << 16 | (i + 1 < b.size() ? uint32_t(b[i + 1]) << 8 : 0u) | (i + 2 < b.size() ? b[i + 2] : 0u);
        out += kDigits[n >> 18 & 63];
        out += kDigits[n >> 12 & 63];
        out += i + 1 < b.size() ? kDigits[n >> 6 & 63] : '=';
        out += i + 2 < b.size() ? kDigits[n & 63] : '=';
    }
    return out;
}

bool PolyMesh::loadText(std::string_view text)
{
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<uint8_t> b;
    b.reserve(text.size() / 4 * 3);
    uint32_t n = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=')
            break;
        const int d = digit(c);
        if (d < 0)
            return false;
        n = n << 6 | uint32_t(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b.push_back(uint8_t(n >> bits));
        }
    }
    return load(b.data(), b.size());
}

PolyMesh PolyMesh::box(float3 size)
{
    PolyMesh m;
    const float3 h = size * 0.5f;
    for (int i = 0; i < 8; ++i)
        m.positions.push_back({i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z});
    // Counter-clockwise seen from outside.
    m.addFace({1, 3, 7, 5});  // +x
    m.addFace({0, 4, 6, 2});  // -x
    m.addFace({2, 6, 7, 3});  // +y
    m.addFace({0, 1, 5, 4});  // -y
    m.addFace({4, 5, 7, 6});  // +z
    m.addFace({0, 2, 3, 1});  // -z
    m.materials.push_back("");
    return m;
}

PolyMesh PolyMesh::plane(float width, float depth, uint32_t xSegments, uint32_t zSegments)
{
    PolyMesh m;
    xSegments = std::max(xSegments, 1u);
    zSegments = std::max(zSegments, 1u);
    for (uint32_t z = 0; z <= zSegments; ++z)
        for (uint32_t x = 0; x <= xSegments; ++x)
            m.positions.push_back({(float(x) / float(xSegments) - 0.5f) * width, 0.0f, (float(z) / float(zSegments) - 0.5f) * depth});
    const uint32_t row = xSegments + 1;
    for (uint32_t z = 0; z < zSegments; ++z)
        for (uint32_t x = 0; x < xSegments; ++x) {
            const uint32_t a = z * row + x;
            m.addFace({a, a + row, a + row + 1, a + 1});  // facing +y
        }
    m.materials.push_back("");
    return m;
}

float2 projectUv(const FaceUv& uv, float3 n, float3 p)
{
    // The axis the face mostly faces picks the projection (box projection: a box's sides line up
    // across their edges). u x v = the axis, so the texture reads the right way round from the front.
    const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    float3 u, v;
    if (ay >= ax && ay >= az) {
        u = {1, 0, 0};
        v = {0, 0, n.y >= 0.0f ? -1.0f : 1.0f};
    } else if (ax >= az) {
        u = {0, 0, n.x >= 0.0f ? -1.0f : 1.0f};
        v = {0, 1, 0};
    } else {
        u = {n.z >= 0.0f ? 1.0f : -1.0f, 0, 0};
        v = {0, 1, 0};
    }
    float s = dot(p, u), t = -dot(p, v);  // (image v runs down)
    const float r = uv.rotation * (kPi / 180.0f), c = std::cos(r), sn = std::sin(r);
    const float rs = c * s - sn * t, rt = sn * s + c * t;
    return {rs / (uv.scale[0] != 0.0f ? uv.scale[0] : 1.0f) + uv.offset[0], rt / (uv.scale[1] != 0.0f ? uv.scale[1] : 1.0f) + uv.offset[1]};
}

std::vector<float2> faceUvs(const PolyMesh& mesh, uint32_t f, const FaceUv& uv, const float (*world)[4])
{
    std::vector<float2> out;
    if (f >= mesh.faces.size())
        return out;
    const Face& face = mesh.faces[f];
    const bool inWorld = uv.mode == UvMode::World && world;
    const float3 n = mesh.faceNormal(f);
    const float3 projNormal = inWorld ? normalize(rows(world, n, 0.0f)) : n;
    const float r = uv.rotation * (kPi / 180.0f), c = std::cos(r), sn = std::sin(r);
    for (uint32_t i = 0; i < face.count; ++i) {
        if (uv.mode == UvMode::Manual) {
            const float2 m = mesh.cornerUvs.empty() ? float2{0, 0} : mesh.cornerUvs[face.first + i];
            const float s = m.x * uv.scale[0], t = m.y * uv.scale[1];
            out.push_back({c * s - sn * t + uv.offset[0], sn * s + c * t + uv.offset[1]});
        } else {
            const float3 p = mesh.positions[mesh.corners[face.first + i]];
            out.push_back(projectUv(uv, projNormal, inWorld ? rows(world, p, 1.0f) : p));
        }
    }
    return out;
}

std::vector<float2> faceUvs(const PolyMesh& mesh, uint32_t f, const float (*world)[4])
{
    return f < mesh.faces.size() ? faceUvs(mesh, f, mesh.faces[f].uv, world) : std::vector<float2>{};
}

bool build(const PolyMesh& mesh, const BuildOptions& options, LiveMeshData& out, std::vector<uint32_t>* faceOfTriangle, uint32_t* skipped)
{
    out = {};
    if (faceOfTriangle)
        faceOfTriangle->clear();
    if (skipped)
        *skipped = 0;
    if (!mesh.valid())
        return false;

    // Smoothed normals: per (vertex, group), the area-weighted face normals around it.
    std::vector<float3> faceNormals(mesh.faces.size());
    std::unordered_map<uint64_t, float3> smooth;
    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        faceNormals[f] = mesh.faceNormal(f);
        const Face& face = mesh.faces[f];
        if (!face.smoothing)
            continue;
        // (Newell's sum unnormalized is twice the area times the normal: the weight)
        float3 area = {0, 0, 0};
        for (uint32_t i = 0; i < face.count; ++i) {
            const float3 a = mesh.positions[mesh.corners[face.first + i]];
            const float3 b = mesh.positions[mesh.corners[face.first + (i + 1) % face.count]];
            area += cross(a, b);
        }
        for (uint32_t i = 0; i < face.count; ++i)
            smooth[uint64_t(mesh.corners[face.first + i]) << 32 | face.smoothing] += area;
    }

    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        const float3 n = faceNormals[f];
        if (length(n) < 0.5f) {  // degenerate (no area)
            if (skipped)
                ++*skipped;
            continue;
        }
        // Triangulated in the face's plane (any loop: concave too), counter-clockwise from the front.
        std::vector<manifold::ivec3> tris;
        if (face.count == 3) {
            tris.push_back({0, 1, 2});
        } else {
            const float3 any = std::fabs(n.x) < 0.9f ? float3{1, 0, 0} : float3{0, 1, 0};
            const float3 u = normalize(cross(any, n)), v = cross(n, u);
            manifold::SimplePolygonIdx poly;
            for (uint32_t i = 0; i < face.count; ++i) {
                const float3 p = mesh.positions[mesh.corners[face.first + i]];
                poly.push_back({manifold::vec2(dot(p, u), dot(p, v)), int(i)});
            }
            try {
                tris = manifold::TriangulateIdx({poly});
            } catch (...) {
                tris.clear();
            }
            if (tris.empty()) {
                if (skipped)
                    ++*skipped;
                continue;
            }
        }
        // Its corners as vertices of their own (each face its own normals and UVs).
        const uint32_t base = uint32_t(out.positions.size() / 3);
        const std::vector<float2> uvs = faceUvs(mesh, f, options.world);
        for (uint32_t i = 0; i < face.count; ++i) {
            const uint32_t vtx = mesh.corners[face.first + i];
            const float3 p = mesh.positions[vtx];
            float3 cn = n;
            if (face.smoothing)
                if (const auto it = smooth.find(uint64_t(vtx) << 32 | face.smoothing); it != smooth.end() && length(it->second) > 0.0f)
                    cn = normalize(it->second);
            out.positions.insert(out.positions.end(), {p.x, p.y, p.z});
            out.normals.insert(out.normals.end(), {cn.x, cn.y, cn.z});
            out.uvs.insert(out.uvs.end(), {uvs[i].x, uvs[i].y});
            if (!mesh.cornerUv1.empty() || !mesh.vertexColors.empty()) {  // (the extra stream: both or neither)
                const float2 m = mesh.cornerUv1.empty() ? float2{0, 0} : mesh.cornerUv1[face.first + i];
                out.uv1.insert(out.uv1.end(), {m.x, m.y});
                out.colors.push_back(mesh.vertexColors.empty() ? 0xffffffffu : mesh.vertexColors[vtx]);
            }
        }
        const uint32_t material = options.material ? options.material(face.material) : face.material;
        for (const manifold::ivec3& t : tris) {
            out.indices.insert(out.indices.end(), {base + uint32_t(t[0]), base + uint32_t(t[1]), base + uint32_t(t[2])});
            out.materials.push_back(material);
            if (faceOfTriangle)
                faceOfTriangle->push_back(f);
        }
    }
    return true;
}

}  // namespace fx::level
