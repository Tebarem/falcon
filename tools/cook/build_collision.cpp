// Collision data and built-in primitives.
//
// Static collision is a simplified copy of each mesh's render geometry, cooked into Box3D's
// relocatable b3MeshData blob (BVH included), so the runtime creates static bodies without
// building anything. The built-in primitives also get a convex hull. (General hulls wait for
// a less fragile Box3D hull builder: the alpha asserts on some Intel Sponza props.)

#include "cook.h"

#include <box3d/collision.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx::cook {

namespace {

constexpr float kCollisionError = 0.02f;         // absolute simplification error (scene units, ~metres)
constexpr int kMaxHullVertices = 32;  // Box3D caps hulls at 255 half-edges

void addQuad(SrcPrimitive& p, const float c[3], const float u[3], const float v[3], const float n[3])
{
    const uint32_t base = uint32_t(p.vertices.size());
    for (int k = 0; k < 4; ++k) {
        const float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = k >= 2 ? 1.0f : -1.0f;
        SrcVertex vx = {};
        vx.px = c[0] + u[0] * su + v[0] * sv;
        vx.py = c[1] + u[1] * su + v[1] * sv;
        vx.pz = c[2] + u[2] * su + v[2] * sv;
        vx.nx = n[0], vx.ny = n[1], vx.nz = n[2];
        vx.u = su * 0.5f + 0.5f;
        vx.v = 0.5f - sv * 0.5f;
        vx.tx = u[0] * 2.0f, vx.ty = u[1] * 2.0f, vx.tz = u[2] * 2.0f, vx.tw = 1.0f;
        p.vertices.push_back(vx);
    }
    // Counter-clockwise seen from outside (n = u x v).
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
        p.indices.push_back(base + i);
}

SrcPrimitive makeCube()
{
    SrcPrimitive p;
    const float h = 0.5f;
    for (int axis = 0; axis < 3; ++axis)
        for (float s : {1.0f, -1.0f}) {
            float n[3] = {}, u[3] = {}, v[3] = {}, c[3] = {};
            n[axis] = s;
            c[axis] = s * h;
            // u, v span the face so that u x v = n.
            u[(axis + 1) % 3] = h * s;
            v[(axis + 2) % 3] = h;
            addQuad(p, c, u, v, n);
        }
    return p;
}

SrcPrimitive makeSphere(uint32_t segments, uint32_t rings)
{
    SrcPrimitive p;
    const float kPi = 3.14159265358979f;
    for (uint32_t r = 0; r <= rings; ++r) {
        const float theta = kPi * float(r) / float(rings);  // from +Y down
        for (uint32_t s = 0; s <= segments; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(segments);
            SrcVertex v = {};
            v.nx = std::sin(theta) * std::cos(phi);
            v.ny = std::cos(theta);
            v.nz = -std::sin(theta) * std::sin(phi);
            v.px = v.nx * 0.5f, v.py = v.ny * 0.5f, v.pz = v.nz * 0.5f;
            v.u = float(s) / float(segments);
            v.v = float(r) / float(rings);
            v.tx = -std::sin(phi), v.ty = 0.0f, v.tz = -std::cos(phi), v.tw = 1.0f;  // d(position)/du
            p.vertices.push_back(v);
        }
    }
    const uint32_t row = segments + 1;
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = r * row + s, b = a + row;
            if (r != 0)
                p.indices.insert(p.indices.end(), {a, b, a + 1});
            if (r + 1 != rings)
                p.indices.insert(p.indices.end(), {a + 1, b, b + 1});
        }
    return p;
}

constexpr float kPi = 3.14159265358979f;

SrcVertex vertex(float px, float py, float pz, float nx, float ny, float nz, float u, float v, float tx, float ty, float tz)
{
    SrcVertex x = {};
    x.px = px, x.py = py, x.pz = pz;
    x.nx = nx, x.ny = ny, x.nz = nz;
    x.u = u, x.v = v;
    x.tx = tx, x.ty = ty, x.tz = tz, x.tw = 1.0f;
    return x;
}

// One triangle, wound counter-clockwise seen from outside (its face normal agrees with the
// vertex normals); degenerate ones (collapsed poles) are dropped.
void addTriangle(SrcPrimitive& p, uint32_t a, uint32_t b, uint32_t c)
{
    const SrcVertex &va = p.vertices[a], &vb = p.vertices[b], &vc = p.vertices[c];
    const float e1[3] = {vb.px - va.px, vb.py - va.py, vb.pz - va.pz};
    const float e2[3] = {vc.px - va.px, vc.py - va.py, vc.pz - va.pz};
    const float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float n[3] = {va.nx + vb.nx + vc.nx, va.ny + vb.ny + vc.ny, va.nz + vb.nz + vc.nz};
    if (f[0] * f[0] + f[1] * f[1] + f[2] * f[2] < 1e-14f)
        return;
    if (f[0] * n[0] + f[1] * n[1] + f[2] * n[2] >= 0.0f)
        p.indices.insert(p.indices.end(), {a, b, c});
    else
        p.indices.insert(p.indices.end(), {a, c, b});
}

// Quads between consecutive rows of `columns` vertices starting at `first`.
void addGrid(SrcPrimitive& p, uint32_t first, uint32_t rows, uint32_t columns)
{
    for (uint32_t r = 0; r + 1 < rows; ++r)
        for (uint32_t c = 0; c + 1 < columns; ++c) {
            const uint32_t a = first + r * columns + c, b = a + columns;
            addTriangle(p, a, b, a + 1);
            addTriangle(p, a + 1, b, b + 1);
        }
}

// A ring of the surfaces of revolution below: angle phi around +Y, as on the sphere
// (x = cos phi, z = -sin phi; tangent = d(position)/du).
void addRing(SrcPrimitive& p, uint32_t segments, float y, float radius, float ny, float v)
{
    const float nr = std::sqrt(std::max(1.0f - ny * ny, 0.0f));
    for (uint32_t s = 0; s <= segments; ++s) {
        const float phi = 2.0f * kPi * float(s) / float(segments), c = std::cos(phi), sn = std::sin(phi);
        p.vertices.push_back(vertex(c * radius, y, -sn * radius, c * nr, ny, -sn * nr, float(s) / float(segments), v, -sn, 0.0f, -c));
    }
}

// A flat cap (disc) facing +Y or -Y at height y: planar UVs, tangent +X.
void addCap(SrcPrimitive& p, uint32_t segments, float y, float radius, float ny)
{
    const uint32_t center = uint32_t(p.vertices.size());
    p.vertices.push_back(vertex(0.0f, y, 0.0f, 0.0f, ny, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f));
    for (uint32_t s = 0; s <= segments; ++s) {
        const float phi = 2.0f * kPi * float(s) / float(segments), x = std::cos(phi) * radius, z = -std::sin(phi) * radius;
        // cross(n, t) * w points toward decreasing v (glTF): v follows +z on top, -z below.
        p.vertices.push_back(vertex(x, y, z, 0.0f, ny, 0.0f, 0.5f + x, 0.5f + z * ny, 1.0f, 0.0f, 0.0f));
    }
    for (uint32_t s = 0; s < segments; ++s)
        addTriangle(p, center, center + 1 + s, center + 2 + s);
}

SrcPrimitive makeCylinder(uint32_t segments)
{
    SrcPrimitive p;
    addRing(p, segments, 1.0f, 0.5f, 0.0f, 0.0f);
    addRing(p, segments, -1.0f, 0.5f, 0.0f, 1.0f);
    addGrid(p, 0, 2, segments + 1);
    addCap(p, segments, 1.0f, 0.5f, 1.0f);
    addCap(p, segments, -1.0f, 0.5f, -1.0f);
    return p;
}

// Two hemispheres 1 apart joined by a cylinder: rows from the top pole down, v by height.
SrcPrimitive makeCapsule(uint32_t segments, uint32_t hemisphereRings)
{
    SrcPrimitive p;
    uint32_t rows = 0;
    for (uint32_t half = 0; half < 2; ++half)
        for (uint32_t r = 0; r <= hemisphereRings; ++r) {
            const float theta = 0.5f * kPi * (float(half) + float(r) / float(hemisphereRings));  // from +Y down
            const float y = std::cos(theta) * 0.5f + (half == 0 ? 0.5f : -0.5f);
            addRing(p, segments, y, std::sin(theta) * 0.5f, std::cos(theta), 0.5f - y * 0.5f);
            ++rows;
        }
    addGrid(p, 0, rows, segments + 1);
    return p;
}

SrcPrimitive makeCone(uint32_t segments)
{
    SrcPrimitive p;
    // Side normals lean up by the slope (radius 0.5 over height 1); the apex is one vertex per
    // segment so the shading stays smooth around it.
    const float ny = 0.5f / std::sqrt(1.25f);
    addRing(p, segments, 0.5f, 0.0f, ny, 0.0f);
    addRing(p, segments, -0.5f, 0.5f, ny, 1.0f);
    addGrid(p, 0, 2, segments + 1);
    addCap(p, segments, -0.5f, 0.5f, -1.0f);
    return p;
}

// Unity's plane: 10 x 10 units on y = 0, facing +Y, 10 x 10 quads.
SrcPrimitive makePlane()
{
    SrcPrimitive p;
    const uint32_t n = 10;
    for (uint32_t r = 0; r <= n; ++r)
        for (uint32_t c = 0; c <= n; ++c) {
            const float u = float(c) / float(n), v = float(r) / float(n);
            p.vertices.push_back(vertex(u * 10.0f - 5.0f, 0.0f, v * 10.0f - 5.0f, 0.0f, 1.0f, 0.0f, u, v, 1.0f, 0.0f, 0.0f));
        }
    addGrid(p, 0, n + 1, n + 1);
    return p;
}

// Unity's quad: 1 x 1 on z = 0, facing -Z (seen from a camera looking along +Z).
SrcPrimitive makeQuad()
{
    SrcPrimitive p;
    const float c[3] = {0.0f, 0.0f, 0.0f}, u[3] = {-0.5f, 0.0f, 0.0f}, v[3] = {0.0f, 0.5f, 0.0f}, n[3] = {0.0f, 0.0f, -1.0f};
    addQuad(p, c, u, v, n);
    return p;
}

}  // namespace

void appendBuiltinPrimitives(SrcScene& scene, uint32_t meshes[uint32_t(fpak::Builtin::Count)], uint32_t& material)
{
    fpak::MaterialGpu m = {};
    m.baseColor[0] = m.baseColor[1] = m.baseColor[2] = 0.8f;
    m.baseColor[3] = 1.0f;
    m.roughness = 0.45f;
    m.alphaCutoff = 0.5f;
    m.vtPageTable = fpak::kVtNoTexture;
    m.normalScale = 1.0f;
    m.occlusionStrength = 1.0f;
    material = uint32_t(scene.materials.size());
    scene.materials.push_back(m);
    scene.materialTextures.push_back({});

    SrcPrimitive shapes[uint32_t(fpak::Builtin::Count)] = {makeCube(), makeSphere(32, 16), makeCapsule(32, 8), makeCylinder(32),
                                                          makePlane(), makeQuad(), makeCone(32)};
    for (uint32_t k = 0; k < uint32_t(fpak::Builtin::Count); ++k) {
        shapes[k].material = material;
        meshes[k] = uint32_t(scene.primitives.size());
        scene.primitives.push_back(std::move(shapes[k]));
    }
}

BuiltCollider buildCollider(const SrcPrimitive& prim, const fpak::MaterialGpu& material, bool builtin)
{
    BuiltCollider out;
    if (prim.indices.empty())
        return out;
    std::vector<float> positions(prim.vertices.size() * 3);
    for (size_t v = 0; v < prim.vertices.size(); ++v) {
        positions[v * 3 + 0] = prim.vertices[v].px;
        positions[v * 3 + 1] = prim.vertices[v].py;
        positions[v * 3 + 2] = prim.vertices[v].pz;
    }

    // Cutouts (foliage, chains) and transparent surfaces do not block bodies.
    if (!builtin && !(material.flags & (fpak::kMaterialAlphaMask | fpak::kMaterialAlphaBlend))) {
        // Position-only welding first, so UV seams do not stop the simplifier.
        std::vector<uint32_t> remap(prim.vertices.size());
        const size_t unique = meshopt_generateVertexRemap(remap.data(), prim.indices.data(), prim.indices.size(),
                                                          positions.data(), prim.vertices.size(), sizeof(float) * 3);
        std::vector<uint32_t> indices(prim.indices.size());
        meshopt_remapIndexBuffer(indices.data(), prim.indices.data(), prim.indices.size(), remap.data());
        std::vector<float> welded(unique * 3);
        meshopt_remapVertexBuffer(welded.data(), positions.data(), prim.vertices.size(), sizeof(float) * 3, remap.data());

        std::vector<uint32_t> simplified(indices.size());
        float error = 0.0f;
        simplified.resize(meshopt_simplify(simplified.data(), indices.data(), indices.size(), welded.data(), unique,
                                           sizeof(float) * 3, 0, kCollisionError, meshopt_SimplifyErrorAbsolute, &error));
        if (simplified.size() >= 3) {
            std::vector<uint32_t> compact(unique);
            const size_t count = meshopt_optimizeVertexFetchRemap(compact.data(), simplified.data(), simplified.size(), unique);
            std::vector<b3Vec3> verts(count);
            for (size_t v = 0; v < unique; ++v)
                if (compact[v] != ~0u)
                    verts[compact[v]] = {welded[v * 3], welded[v * 3 + 1], welded[v * 3 + 2]};
            std::vector<int32_t> tris(simplified.size());
            for (size_t i = 0; i < simplified.size(); ++i)
                tris[i] = int32_t(compact[simplified[i]]);

            b3MeshDef def = {};
            def.vertices = verts.data();
            def.indices = tris.data();
            def.vertexCount = int(verts.size());
            def.triangleCount = int(tris.size() / 3);
            def.weldVertices = false;
            def.identifyEdges = true;
            if (def.vertexCount >= 3 && def.triangleCount >= 1) {
                b3MeshData* mesh = b3CreateMesh(&def, nullptr, 0);
                if (mesh) {
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(mesh);
                    out.mesh.assign(bytes, bytes + mesh->byteCount);
                    out.meshTriangles = uint32_t(def.triangleCount);
                    b3DestroyMesh(mesh);
                }
            }
        }
    }

    if (builtin) {
        std::vector<b3Vec3> points(prim.vertices.size());
        for (size_t v = 0; v < points.size(); ++v)
            points[v] = {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
        b3HullData* hull = b3CreateHull(points.data(), int(points.size()), kMaxHullVertices);
        if (hull) {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(hull);
            out.hull.assign(bytes, bytes + hull->byteCount);
            b3DestroyHull(hull);
        }
    }
    return out;
}

}  // namespace fx::cook
