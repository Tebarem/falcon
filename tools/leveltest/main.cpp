// falcon_level_test: the level tools' geometry libraries and kernel, without the engine running.
//   falcon_level_test <test>
// Tests: tangents (MikkTSpace through core/mesh_tangents), boolean (Manifold), unwrap (xatlas),
// navmesh (Recast build + Detour path), polymesh (level/poly_mesh: build, concave faces, smoothing,
// world UVs, save/load), meshops (level/mesh_ops: every shape and operation keeps the mesh whole),
// uvops (fit, nudge, align, match, unwrap, manual and reset, world space), trims (faces onto a trim
// strip, runs carrying u on, the .ftrim text), layers (layer materials, the mask UVs, vertex colours,
// format 2, both kept through operations), cuts (level/mesh_cut: doors, windows, intersect, mesh
// cutters, open meshes left alone, UVs carried), walls (level/walls: straight, mitred corners,
// aligned, rooms with floors and ceilings, the path, a door cut through), wear (level/wear: edge wear
// and grime made into a mask; smart materials).
// Prints what it checked; exit code 0 when everything held.

#include "core/json.h"
#include "core/mesh_tangents.h"
#include "level/cut_bake.h"
#include "level/mesh_cut.h"
#include "level/mesh_ops.h"
#include "level/modifiers.h"
#include "level/navmesh.h"
#include "level/poly_mesh.h"
#include "level/rubble.h"
#include "level/trim_sheet.h"
#include "level/level_mask.h"
#include "level/mask_brush.h"
#include "level/roofs.h"
#include "level/walls.h"
#include "level/smart_material.h"
#include "level/wear.h"
#include "render/live_mesh_data.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>
#include <manifold/manifold.h>
#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// A unit cube as 24 vertices (4 per face, own normals and UVs) and 12 triangles.
struct Cube {
    std::vector<float> pos, nrm, uv;
    std::vector<uint32_t> idx;
};

Cube makeCube()
{
    Cube c;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        // Two axes spanning the face: u x v = n.
        const float* nf = n[f];
        float u[3] = {nf[1] + nf[2], 0, nf[0]};  // any axis not along n
        if (std::fabs(nf[1]) > 0.5f) {
            u[0] = 1;
            u[1] = 0;
            u[2] = 0;
        }
        const float v[3] = {nf[1] * u[2] - nf[2] * u[1], nf[2] * u[0] - nf[0] * u[2], nf[0] * u[1] - nf[1] * u[0]};
        const uint32_t base = uint32_t(c.pos.size() / 3);
        const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& k : corner) {
            for (int a = 0; a < 3; ++a) {
                c.pos.push_back(0.5f * (nf[a] + k[0] * u[a] + k[1] * v[a]));
                c.nrm.push_back(nf[a]);
            }
            c.uv.push_back(0.5f * (k[0] + 1));
            c.uv.push_back(0.5f * (k[1] + 1));
        }
        for (uint32_t t : {0u, 1u, 2u, 0u, 2u, 3u})
            c.idx.push_back(base + t);
    }
    return c;
}

int testTangents()
{
    const Cube c = makeCube();
    std::vector<float> t(c.idx.size() * 4);
    const bool ok = fx::generateTangents(c.idx.data(), c.idx.size(), {c.pos.data(), 12}, {c.nrm.data(), 12},
                                         {c.uv.data(), 8}, t.data());
    check(ok, "MikkTSpace ran");
    // Each corner's tangent must be unit length, perpendicular to its normal, and point along +U
    // of its face (the face's u axis, which the UVs follow).
    bool unit = true, perpendicular = true, alongU = true;
    for (size_t k = 0; k < c.idx.size(); ++k) {
        const float* tk = &t[k * 4];
        const float* nk = &c.nrm[c.idx[k] * 3];
        const float len = std::sqrt(tk[0] * tk[0] + tk[1] * tk[1] + tk[2] * tk[2]);
        unit &= std::fabs(len - 1.0f) < 1e-3f;
        perpendicular &= std::fabs(tk[0] * nk[0] + tk[1] * nk[1] + tk[2] * nk[2]) < 1e-3f;
        // +U direction of this face: position delta between corner 1 and corner 0 of the quad.
        const uint32_t base = c.idx[k] / 4 * 4;
        float du[3];
        for (int a = 0; a < 3; ++a)
            du[a] = c.pos[(base + 1) * 3 + a] - c.pos[base * 3 + a];
        alongU &= tk[0] * du[0] + tk[1] * du[1] + tk[2] * du[2] > 0.99f;
    }
    check(unit, "tangents are unit length");
    check(perpendicular, "tangents are perpendicular to the normals");
    check(alongU, "tangents follow +U of each face");
    return g_failures;
}

int testBoolean()
{
    using manifold::Manifold;
    using manifold::vec3;
    const Manifold wall = Manifold::Cube(vec3(4.0, 3.0, 0.2), true);
    // From below the wall's foot to 0.5: 2 m of it inside the wall.
    const Manifold door = Manifold::Cube(vec3(1.0, 2.2, 1.0), true).Translate(vec3(0.0, -0.6, 0.0));
    const Manifold cut = wall - door;
    const double expected = 4.0 * 3.0 * 0.2 - 1.0 * 2.0 * 0.2;
    std::printf("     wall %.4f - door -> %.4f m^3 (expected %.4f), %zu triangles\n", wall.Volume(), cut.Volume(),
                expected, cut.NumTri());
    check(cut.Status() == Manifold::Error::NoError, "the cut is a valid manifold");
    check(std::fabs(cut.Volume() - expected) < 1e-6, "the door's volume is removed exactly");
    check(cut.Genus() == 0, "a door from the floor leaves genus 0 (a notch, not a hole)");
    const Manifold window = Manifold::Cube(vec3(1.0, 1.0, 1.0), true).Translate(vec3(1.0, 0.5, 0.0));
    check((wall - window).Genus() == 1, "a window through the wall makes genus 1 (a hole)");
    // Determinism: the same inputs give the same triangles, bit for bit.
    const manifold::MeshGL a = (wall - door).GetMeshGL(), b = (wall - door).GetMeshGL();
    check(a.vertProperties == b.vertProperties && a.triVerts == b.triVerts, "two runs give identical meshes");
    return g_failures;
}

int testUnwrap()
{
    const Cube c = makeCube();
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl mesh;
    mesh.vertexCount = uint32_t(c.pos.size() / 3);
    mesh.vertexPositionData = c.pos.data();
    mesh.vertexPositionStride = 12;
    mesh.vertexNormalData = c.nrm.data();
    mesh.vertexNormalStride = 12;
    mesh.indexCount = uint32_t(c.idx.size());
    mesh.indexData = c.idx.data();
    mesh.indexFormat = xatlas::IndexFormat::UInt32;
    check(xatlas::AddMesh(atlas, mesh) == xatlas::AddMeshError::Success, "xatlas took the cube");
    xatlas::PackOptions pack;
    pack.resolution = 256;
    pack.padding = 2;
    xatlas::Generate(atlas, {}, pack);
    std::printf("     %u charts in %ux%u, %.0f%% used\n", atlas->chartCount, atlas->width, atlas->height,
                atlas->utilization ? atlas->utilization[0] * 100.0 : 0.0);
    check(atlas->meshCount == 1 && atlas->chartCount >= 1, "the cube got charts");
    bool inside = true;
    for (uint32_t v = 0; v < atlas->meshes[0].vertexCount; ++v) {
        const xatlas::Vertex& x = atlas->meshes[0].vertexArray[v];
        inside &= x.uv[0] >= 0 && x.uv[1] >= 0 && x.uv[0] <= float(atlas->width) && x.uv[1] <= float(atlas->height);
    }
    check(inside, "every UV is inside the atlas");
    xatlas::Destroy(atlas);
    return g_failures;
}

int testNavmesh()
{
    // A 20 x 20 m floor with a 2 m wall across the middle that leaves a 4 m gap at one end.
    std::vector<float> verts;
    std::vector<int> tris;
    auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        const int b = int(verts.size() / 3);
        for (int i = 0; i < 8; ++i) {
            verts.push_back(i & 1 ? x1 : x0);
            verts.push_back(i & 2 ? y1 : y0);
            verts.push_back(i & 4 ? z1 : z0);
        }
        // Outward-facing (counter-clockwise from outside, Recast's winding is irrelevant for slope:
        // it uses the normal's y, so tops must face up).
        const int f[12][3] = {{2, 6, 7}, {2, 7, 3}, {0, 1, 5}, {0, 5, 4}, {0, 4, 6}, {0, 6, 2},
                              {1, 3, 7}, {1, 7, 5}, {0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}};
        for (const auto& t : f)
            for (int k = 0; k < 3; ++k)
                tris.push_back(b + t[k]);
    };
    box(-10, -0.5f, -10, 10, 0, 10);
    box(-0.25f, 0, -10, 0.25f, 2, 6);
    const int nverts = int(verts.size() / 3), ntris = int(tris.size() / 3);

    rcConfig cfg{};
    cfg.cs = 0.2f;
    cfg.ch = 0.1f;
    cfg.walkableSlopeAngle = 45;
    cfg.walkableHeight = int(std::ceil(1.8f / cfg.ch));
    cfg.walkableClimb = int(std::floor(0.4f / cfg.ch));
    cfg.walkableRadius = int(std::ceil(0.3f / cfg.cs));
    cfg.maxEdgeLen = int(12 / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6;
    cfg.detailSampleMaxError = cfg.ch;
    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext ctx(false);
    rcHeightfield* hf = rcAllocHeightfield();
    std::vector<unsigned char> areas(size_t(ntris), 0);
    bool ok = rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
    ok = ok && rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *hf, cfg.walkableClimb);
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    ok = ok && rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
    ok = ok && rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
    ok = ok && rcBuildDistanceField(&ctx, *chf);
    ok = ok && rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);
    rcContourSet* cset = rcAllocContourSet();
    ok = ok && rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    ok = ok && rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);
    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    ok = ok && rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
    check(ok, "Recast built the poly mesh");
    std::printf("     %d polygons\n", pmesh->npolys);

    for (int i = 0; i < pmesh->npolys; ++i)
        pmesh->flags[i] = 1;
    dtNavMeshCreateParams p{};
    p.verts = pmesh->verts;
    p.vertCount = pmesh->nverts;
    p.polys = pmesh->polys;
    p.polyAreas = pmesh->areas;
    p.polyFlags = pmesh->flags;
    p.polyCount = pmesh->npolys;
    p.nvp = pmesh->nvp;
    p.detailMeshes = dmesh->meshes;
    p.detailVerts = dmesh->verts;
    p.detailVertsCount = dmesh->nverts;
    p.detailTris = dmesh->tris;
    p.detailTriCount = dmesh->ntris;
    p.walkableHeight = 1.8f;
    p.walkableRadius = 0.3f;
    p.walkableClimb = 0.4f;
    rcVcopy(p.bmin, pmesh->bmin);
    rcVcopy(p.bmax, pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int dataSize = 0;
    ok = ok && dtCreateNavMeshData(&p, &data, &dataSize);
    dtNavMesh* nav = dtAllocNavMesh();
    ok = ok && dtStatusSucceed(nav->init(data, dataSize, DT_TILE_FREE_DATA));
    check(ok, "Detour loaded the nav mesh");

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    query->init(nav, 2048);
    dtQueryFilter filter;
    const float ext[3] = {1, 2, 1};
    const float start[3] = {-5, 0, -5}, end[3] = {5, 0, -5};
    dtPolyRef sref = 0, eref = 0;
    float snear[3], enear[3];
    query->findNearestPoly(start, ext, &filter, &sref, snear);
    query->findNearestPoly(end, ext, &filter, &eref, enear);
    dtPolyRef path[256];
    int npath = 0;
    query->findPath(sref, eref, snear, enear, &filter, path, &npath, 256);
    float straight[256 * 3];
    int nstraight = 0;
    query->findStraightPath(snear, enear, path, npath, straight, nullptr, nullptr, &nstraight, 256);
    float length = 0;
    for (int i = 1; i < nstraight; ++i)
        length += std::sqrt(rcSqr(straight[i * 3] - straight[i * 3 - 3]) + rcSqr(straight[i * 3 + 2] - straight[i * 3 - 1]));
    std::printf("     path across the wall: %d corners, %.1f m (straight line 10 m)\n", nstraight, length);
    check(sref && eref && npath > 0, "a path exists between the two sides");
    check(length > 20.0f, "the path goes around the wall through the gap");

    dtFreeNavMeshQuery(query);
    dtFreeNavMesh(nav);
    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);
    rcFreeContourSet(cset);
    rcFreeCompactHeightfield(chf);
    rcFreeHeightField(hf);
    return g_failures;
}

// Triangle areas (counted along `up`: negative when wound the other way).
float signedArea(const fx::LiveMeshData& d, size_t t, fx::float3 up)
{
    auto at = [&](uint32_t i) { return fx::float3{d.positions[i * 3], d.positions[i * 3 + 1], d.positions[i * 3 + 2]}; };
    const fx::float3 a = at(d.indices[t * 3]), b = at(d.indices[t * 3 + 1]), c = at(d.indices[t * 3 + 2]);
    return 0.5f * fx::dot(fx::cross(b - a, c - a), up);
}

int testPolyMesh()
{
    using namespace fx::level;
    using fx::float3;
    // A box: 6 quads, 12 triangles, flat normals pointing out.
    const PolyMesh box = PolyMesh::box({2, 1, 4});
    fx::LiveMeshData d;
    std::vector<uint32_t> faceOf;
    check(box.valid() && build(box, {}, d, &faceOf), "a box builds");
    check(d.indices.size() == 36 && faceOf.size() == 12 && d.materials.size() == 12, "12 triangles, each with its face");
    bool outward = true;
    for (uint32_t f = 0; f < 6; ++f)
        outward &= fx::dot(box.faceNormal(f), box.faceCenter(f)) > 0.0f;
    bool wound = true;
    for (size_t t = 0; t < 12; ++t)
        wound &= signedArea(d, t, box.faceNormal(faceOf[t])) > 0.0f;
    check(outward && wound, "faces point out, triangles wound counter-clockwise from the front");

    // A concave L (6 corners) facing up: 4 triangles covering its 3 square metres.
    PolyMesh l;
    for (const auto& xz : std::vector<std::pair<float, float>>{{0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}, {2, 0}})
        l.positions.push_back({xz.first, 0.0f, xz.second});
    l.addFace({0, 1, 2, 3, 4, 5});
    l.materials.push_back("");
    check(fx::dot(l.faceNormal(0), {0, 1, 0}) > 0.99f, "the L faces up");
    check(build(l, {}, d, &faceOf) && d.indices.size() == 12, "the L makes 4 triangles");
    float area = 0.0f;
    bool up = true;
    for (size_t t = 0; t < 4; ++t) {
        const float a = signedArea(d, t, {0, 1, 0});
        area += a;
        up &= a > 0.0f;
    }
    std::printf("     L area %.4f (expected 3)\n", area);
    check(std::fabs(area - 3.0f) < 1e-4f && up, "the triangles cover it exactly, all facing up");

    // Smoothing: a box in one group has one normal per corner, the corner's diagonal.
    PolyMesh smooth = PolyMesh::box({1, 1, 1});
    for (Face& f : smooth.faces)
        f.smoothing = 1;
    check(build(smooth, {}, d), "a smoothed box builds");
    bool diagonal = true;
    for (size_t v = 0; v < d.positions.size() / 3; ++v) {
        const float3 p = fx::normalize({d.positions[v * 3], d.positions[v * 3 + 1], d.positions[v * 3 + 2]});
        const float3 n = {d.normals[v * 3], d.normals[v * 3 + 1], d.normals[v * 3 + 2]};
        diagonal &= fx::dot(p, n) > 0.999f;
    }
    check(diagonal, "smoothed corners share the diagonal normal");

    // World UVs: two 1 m boxes side by side (x 0 and 1) give the same u where they touch on top.
    PolyMesh a = PolyMesh::box({1, 1, 1});
    for (Face& f : a.faces)
        f.uv.mode = UvMode::World;
    BuildOptions left, right;
    right.world[0][3] = 1.0f;
    fx::LiveMeshData da, db;
    build(a, left, da);
    build(a, right, db);
    auto uOfTopCorner = [](const fx::LiveMeshData& m, float localX, float localZ) {
        for (size_t v = 0; v < m.positions.size() / 3; ++v)
            if (m.normals[v * 3 + 1] > 0.9f && std::fabs(m.positions[v * 3] - localX) < 1e-4f && std::fabs(m.positions[v * 3 + 2] - localZ) < 1e-4f)
                return m.uvs[v * 2];
        return -999.0f;
    };
    const float ua = uOfTopCorner(da, 0.5f, 0.5f), ub = uOfTopCorner(db, -0.5f, 0.5f);
    std::printf("     shared edge u: %.3f and %.3f\n", ua, ub);
    check(ua > -900.0f && std::fabs(ua - ub) < 1e-5f, "world UVs line up across neighbouring meshes");

    // Save and load: the same mesh back (the same hash); a cut-short blob is refused.
    PolyMesh copy;
    const std::vector<uint8_t> blob = smooth.save();
    check(copy.load(blob.data(), blob.size()) && copy.hash() == smooth.hash(), "save and load round trip");
    check(!copy.load(blob.data(), blob.size() - 5), "a truncated blob is refused");
    PolyMesh fromText;
    check(fromText.loadText(smooth.saveText()) && fromText.hash() == smooth.hash(), "and as base64 text");
    PolyMesh bad = box;
    bad.corners[3] = 99;
    check(!bad.valid(), "a corner past the vertices is invalid");
    return g_failures;
}

// How whole a mesh is: valid, every edge used by at most two faces, in opposite directions (wound
// alike), closed when every edge has two; and its volume (closed meshes: positive when facing out).
struct Topology {
    bool valid = false, wound = true, closed = true;
    double volume = 0.0;
};

Topology topologyOf(const fx::level::PolyMesh& m)
{
    Topology t;
    t.valid = m.valid();
    if (!t.valid)
        return t;
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
    for (const fx::level::Face& f : m.faces)
        for (uint32_t k = 0; k < f.count; ++k)
            ++directed[{m.corners[f.first + k], m.corners[f.first + (k + 1) % f.count]}];
    for (const auto& [e, n] : directed) {
        const auto back = directed.find({e.second, e.first});
        t.wound &= n == 1;
        t.closed &= back != directed.end();
    }
    for (const fx::level::Face& f : m.faces) {
        const fx::float3 p0 = m.positions[m.corners[f.first]];
        for (uint32_t k = 1; k + 1 < f.count; ++k)
            t.volume += fx::dot(p0, fx::cross(m.positions[m.corners[f.first + k]], m.positions[m.corners[f.first + k + 1]])) / 6.0;
    }
    return t;
}

fx::level::OpResult op(const char* name, const fx::level::PolyMesh& in, const char* args)
{
    fx::level::OpResult out;
    if (!fx::level::runOp(name, in, fx::Json::parse(args), out))
        std::printf("     %s: %s\n", name, out.error.c_str());
    return out;
}

// Closed, wound alike, facing out, the expected face count and (when given) volume.
void checkSolid(const fx::level::PolyMesh& m, size_t faces, double volume, const std::string& what)
{
    const Topology t = topologyOf(m);
    const bool ok = t.valid && t.wound && t.closed && t.volume > 0.0 && (faces == 0 || m.faces.size() == faces) &&
                    (volume <= 0.0 || std::fabs(t.volume - volume) < 1e-3);
    if (!ok)
        std::printf("     %s: valid %d wound %d closed %d volume %.4f faces %zu\n", what.c_str(), t.valid, t.wound, t.closed, t.volume,
                    m.faces.size());
    check(ok, what.c_str());
}

uint32_t topFace(const fx::level::PolyMesh& m)
{
    for (uint32_t f = 0; f < m.faces.size(); ++f)
        if (m.faceNormal(f).y > 0.99f)
            return f;
    return 0;
}

// The edge along x on top of a unit box (y and z at +0.5).
uint64_t topFrontEdge(const fx::level::PolyMesh& m)
{
    uint32_t a = ~0u, b = ~0u;
    for (uint32_t v = 0; v < m.positions.size(); ++v)
        if (m.positions[v].y > 0.4f && m.positions[v].z > 0.4f)
            (a == ~0u ? a : b) = v;
    return fx::level::edgeKey(a, b);
}

int testMeshOps()
{
    using namespace fx::level;
    using fx::Json;
    // Shapes: each closed and facing out (the plane and a flat polygon: one side up).
    for (const char* name : {"box", "cylinder", "cone", "sphere", "torus", "pipe", "prism", "arch", "stairs", "curvedStairs"}) {
        std::string error;
        const PolyMesh m = makeShape(name, Json::parse("{}"), &error);
        fx::LiveMeshData d;
        check(build(m, {}, d) && !d.indices.empty(), (std::string(name) + " builds").c_str());
        checkSolid(m, 0, 0.0, std::string(name) + " is closed and faces out");
    }
    checkSolid(makeShape("stairs", Json::parse(R"({"size":[1,1,2],"steps":6})")), 0, 2.0 * 7.0 / 12.0, "stairs hold their volume (1 x 1 x 2, 6 steps)");
    checkSolid(makeShape("polygon", Json::parse(R"({"points":[[0,0],[0,2],[1,2],[1,1],[2,1],[2,0]],"height":0.5})")), 8, 1.5,
               "a polygon outline raised");
    const PolyMesh flat = makeShape("polygon", Json::parse(R"({"points":[[0,0],[1,0],[1,1]],"height":0})"));
    check(flat.faces.size() == 1 && flat.faceNormal(0).y > 0.99f, "a flat polygon faces up");
    std::string error;
    check(makeShape("teapot", Json::parse("{}"), &error).empty() && !error.empty(), "an unknown shape is refused");

    const PolyMesh box = PolyMesh::box({1, 1, 1});
    const std::string top = std::to_string(topFace(box));
    // Extrude: the top up a metre (a 1 x 2 x 1 box), the moved face selected.
    OpResult r = op("extrude", box, (R"({"faces":[)" + top + R"(],"distance":1})").c_str());
    checkSolid(r.mesh, 10, 2.0, "extrude: walls around the moved face, twice the volume");
    check(r.selectionMode == 3 && r.selection.size() == 1 && r.mesh.faceNormal(uint32_t(r.selection[0])).y > 0.99f &&
              r.mesh.faceCenter(uint32_t(r.selection[0])).y > 1.4f,
          "the moved face stays selected");
    r = op("inset", box, (R"({"faces":[)" + top + R"(],"amount":0.2})").c_str());
    checkSolid(r.mesh, 10, 1.0, "inset: a ring of four faces, the same volume");
    // Bevel a top edge 0.1: a chamfer face, half of 0.1 x 0.1 x 1 cut off.
    r = op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.1})").c_str());
    checkSolid(r.mesh, 7, 1.0 - 0.005, "bevel: a chamfer face where the edge was");
    // Bevel inside a grid (vertices of four faces): triangles close the ends, the mesh stays whole.
    const PolyMesh grid = PolyMesh::plane(3, 3, 3, 3);
    uint32_t ga = ~0u, gb = ~0u;
    for (uint32_t v = 0; v < grid.positions.size(); ++v)
        if (std::fabs(grid.positions[v].z - 0.5f) < 1e-4f && std::fabs(grid.positions[v].x) < 0.6f)
            (ga == ~0u ? ga : gb) = v;
    r = op("bevel", grid, (R"({"edges":[)" + std::to_string(edgeKey(ga, gb)) + R"(],"amount":0.1})").c_str());
    Topology t = topologyOf(r.mesh);
    check(t.valid && t.wound && r.mesh.faces.size() == 9 + 1 + 2, "bevel in a grid: the chamfer, two end triangles, wound alike");
    // Round: 8 faces across the edge on a quarter circle (radius 0.1 round y = z = 0.4), smooth; the
    // box less the corner's square and plus the quarter disc. Curved in: the arc mirrored (less kept).
    const double kPi = 3.14159265358979;
    r = op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.1,"segments":8,"curve":1})").c_str());
    checkSolid(r.mesh, 6 + 8, 1.0 - 0.01 * (1.0 - kPi / 4.0), "bevel round: 8 faces across the edge, a quarter circle's volume");
    float worst = 0.0f;
    for (const fx::float3 p : r.mesh.positions)
        if (p.y > 0.4001f && p.z > 0.4001f)
            worst = std::max(worst, std::fabs(std::sqrt((p.y - 0.4f) * (p.y - 0.4f) + (p.z - 0.4f) * (p.z - 0.4f)) - 0.1f));
    uint32_t smoothFaces = 0;
    for (const fx::level::Face& f : r.mesh.faces)
        smoothFaces += f.smoothing != 0;
    check(worst < 1e-4f && smoothFaces == 8, "bevel round: on the circle touching both faces, its faces smooth");
    r = op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.1,"segments":8,"curve":-1})").c_str());
    checkSolid(r.mesh, 6 + 8, 1.0 - 0.01 * kPi / 4.0, "bevel curved in: a cove, the quarter disc taken");
    r = op("bevel", grid, (R"({"edges":[)" + std::to_string(edgeKey(ga, gb)) + R"(],"amount":0.1,"segments":4})").c_str());
    t = topologyOf(r.mesh);
    check(t.valid && t.wound && r.mesh.faces.size() == 9 + 4 + 2, "bevel round in a grid: 4 faces, the ends closed round, wound alike");

    // Loop cut across the four sides (the ring of a vertical edge).
    uint32_t va = ~0u, vb = ~0u;
    for (uint32_t v = 0; v < box.positions.size(); ++v)
        if (box.positions[v].x > 0.4f && box.positions[v].z > 0.4f)
            (va == ~0u ? va : vb) = v;
    r = op("ring", box, (R"({"edges":[)" + std::to_string(edgeKey(va, vb)) + "]}").c_str());
    check(r.selection.size() == 4, "ring: the four vertical edges");
    r = op("loopCut", box, (R"({"edges":[)" + std::to_string(edgeKey(va, vb)) + "]}").c_str());
    checkSolid(r.mesh, 10, 1.0, "loop cut: the sides split in two");
    check(r.selectionMode == 2 && r.selection.size() == 4, "the new loop selected");
    // Loop through a grid's middle edge: across the grid (stops at the border).
    const PolyMesh grid4 = PolyMesh::plane(4, 4, 4, 4);
    uint32_t la = ~0u, lb = ~0u;
    for (uint32_t v = 0; v < grid4.positions.size(); ++v)
        if (std::fabs(grid4.positions[v].z) < 1e-4f && (std::fabs(grid4.positions[v].x) < 1e-4f || std::fabs(grid4.positions[v].x - 1) < 1e-4f))
            (la == ~0u ? la : lb) = v;
    r = op("loop", grid4, (R"({"edges":[)" + std::to_string(edgeKey(la, lb)) + "]}").c_str());
    check(r.selection.size() == 4, "loop: the grid line, end to end");
    // Subdivide the whole box: 24 quads.
    r = op("subdivide", box, R"({"faces":[0,1,2,3,4,5]})");
    checkSolid(r.mesh, 24, 1.0, "subdivide: each face into four");
    // Connect two opposite corners of the top: two triangles.
    {
        const Face& f = box.faces[topFace(box)];
        const std::string a = std::to_string(box.corners[f.first]), c = std::to_string(box.corners[f.first + 2]);
        r = op("connect", box, (R"({"vertices":[)" + a + "," + c + "]}").c_str());
        checkSolid(r.mesh, 7, 1.0, "connect: the top split corner to corner");
    }
    // Delete the top, fill the hole back.
    r = op("delete", box, (R"({"faces":[)" + top + "]}").c_str());
    t = topologyOf(r.mesh);
    check(r.mesh.faces.size() == 5 && t.wound && !t.closed, "delete: an open box");
    const PolyMesh open = r.mesh;
    uint64_t rim = 0;
    for (uint32_t v = 0; v < open.positions.size() && !rim; ++v)
        for (uint32_t w = 0; w < open.positions.size() && !rim; ++w)
            if (v != w && open.positions[v].y > 0.4f && open.positions[w].y > 0.4f &&
                std::fabs(fx::length(open.positions[v] - open.positions[w]) - 1.0f) < 1e-4f)
                rim = edgeKey(v, w);
    r = op("fill", open, (R"({"edges":[)" + std::to_string(rim) + "]}").c_str());
    checkSolid(r.mesh, 6, 1.0, "fill: the hole closed, wound with its neighbours");
    // Detach the top: the rest open, the top a mesh of its own.
    r = op("detach", box, (R"({"faces":[)" + top + "]}").c_str());
    check(r.mesh.faces.size() == 5 && r.detached && r.detached->faces.size() == 1 && r.detached->positions.size() == 4,
          "detach: the face taken out as its own mesh");
    // Flip everything: inside out; flip one face, conform: whole again.
    r = op("flip", box, "{}");
    check(topologyOf(r.mesh).volume < 0.0, "flip: inside out (a room)");
    r = op("flip", box, (R"({"faces":[)" + top + "]}").c_str());
    check(!topologyOf(r.mesh).wound, "one face flipped disagrees with its neighbours");
    r = op("conform", r.mesh, "{}");
    check(topologyOf(r.mesh).wound, "conform: wound alike again");
    // Triangulate and back.
    r = op("triangulate", box, "{}");
    checkSolid(r.mesh, 12, 1.0, "triangulate: twelve triangles");
    r = op("quadrangulate", r.mesh, "{}");
    checkSolid(r.mesh, 6, 1.0, "quadrangulate: the six quads back");
    // A concave face triangulated: its area kept (the L from the polygon shape).
    const PolyMesh l = makeShape("polygon", Json::parse(R"({"points":[[0,0],[0,2],[1,2],[1,1],[2,1],[2,0]],"height":0})"));
    r = op("triangulate", l, "{}");
    double area = 0.0;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f) {
        const auto& c = r.mesh.corners;
        const Face& fc = r.mesh.faces[f];
        area += 0.5 * fx::dot(fx::cross(r.mesh.positions[c[fc.first + 1]] - r.mesh.positions[c[fc.first]],
                                        r.mesh.positions[c[fc.first + 2]] - r.mesh.positions[c[fc.first]]),
                              {0, 1, 0});
    }
    check(r.mesh.faces.size() == 4 && std::fabs(area - 3.0) < 1e-4, "a concave face into four triangles, all facing up");
    // Merge two faces of a grid: one face of six corners.
    r = op("mergeFaces", PolyMesh::plane(2, 1, 2, 1), R"({"faces":[0,1]})");
    check(r.mesh.faces.size() == 1 && r.mesh.faces[0].count == 6, "merge faces: one face around both");
    // Merge vertices, weld.
    r = op("merge", box, R"({"vertices":[0,1]})");
    check(r.mesh.valid() && r.mesh.positions.size() == 7 && r.selection.size() == 1, "merge: two vertices into one");
    PolyMesh twoQuads = PolyMesh::plane(1, 1);
    {
        PolyMesh other = twoQuads;
        for (fx::float3& p : other.positions)
            p.x += 1.0f;
        const uint32_t base = uint32_t(twoQuads.positions.size());
        twoQuads.positions.insert(twoQuads.positions.end(), other.positions.begin(), other.positions.end());
        std::vector<uint32_t> loop;
        for (uint32_t k = 0; k < other.faces[0].count; ++k)
            loop.push_back(base + other.corners[k]);
        twoQuads.addFace(loop);
    }
    r = op("weld", twoQuads, R"({"distance":0.001})");
    check(r.mesh.positions.size() == 6 && topologyOf(r.mesh).wound, "weld: the touching corners shared");
    // Selections: grow from the top (it and its four neighbours), by normal on a subdivided box.
    r = op("grow", box, (R"({"faces":[)" + top + "]}").c_str());
    check(r.selection.size() == 5, "grow: the face and its neighbours");
    const PolyMesh fine = op("subdivide", box, R"({"faces":[0,1,2,3,4,5]})").mesh;
    r = op("byNormal", fine, (R"({"faces":[)" + std::to_string(topFace(fine)) + "]}").c_str());
    check(r.selection.size() == 4, "by normal: the top's four quads");
    // Smoothing and materials.
    const PolyMesh cylinder = makeShape("cylinder", Json::parse(R"({"sides":12})"));
    r = op("autoSmooth", cylinder, R"({"angle":40})");
    uint32_t smoothSides = 0, flatCaps = 0;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f) {
        const bool cap = std::fabs(r.mesh.faceNormal(f).y) > 0.99f;
        if (cap && r.mesh.faces[f].smoothing == 0)
            ++flatCaps;
        if (!cap && r.mesh.faces[f].smoothing != 0)
            ++smoothSides;
    }
    check(smoothSides == 12 && flatCaps == 2, "auto smooth: the sides smooth, the caps flat");
    r = op("material", box, (R"({"faces":[)" + top + R"(],"path":"materials/brick.fmat"})").c_str());
    check(r.mesh.materials.size() == 2 && r.mesh.faces[topFace(r.mesh)].material == 1, "material: a new slot for the top");
    // The same op twice: the same mesh (replays and networked edits rely on it).
    check(op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.1})").c_str()).mesh.hash() ==
              op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.1})").c_str()).mesh.hash(),
          "operations are deterministic");
    fx::level::OpResult bad;
    check(!runOp("explode", box, Json::parse("{}"), bad) && !bad.error.empty(), "an unknown operation is refused");
    return g_failures;
}

// The UVs a face shows, its corners' bounds.
void uvBounds(const fx::level::PolyMesh& m, uint32_t f, fx::float2& lo, fx::float2& hi)
{
    lo = {1e30f, 1e30f};
    hi = {-1e30f, -1e30f};
    for (fx::float2 p : fx::level::faceUvs(m, f)) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
}

bool near2(fx::float2 a, fx::float2 b, float e = 1e-4f) { return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e; }

int testUvOps()
{
    using namespace fx::level;
    using fx::Json;
    const PolyMesh wall = makeShape("box", Json::parse(R"({"size":[4,3,0.2]})"));
    uint32_t front = 0;
    for (uint32_t f = 0; f < wall.faces.size(); ++f)
        if (wall.faceNormal(f).z > 0.99f)
            front = f;
    const std::string faces = R"({"faces":[)" + std::to_string(front) + "]";
    // Fit: the 4 x 3 m face shows the texture once (0..1 both ways).
    OpResult r = op("uvFit", wall, (faces + "}").c_str());
    fx::float2 lo, hi;
    uvBounds(r.mesh, front, lo, hi);
    std::printf("     fit: %.3f %.3f .. %.3f %.3f\n", lo.x, lo.y, hi.x, hi.y);
    check(near2(lo, {0, 0}) && near2(hi, {1, 1}), "fit: the face's UVs span 0..1");
    // Nudge: half a tile across; turned a quarter: the bounds swap their sizes (Local: 1 m tiles).
    r = op("uvNudge", wall, (faces + R"(,"offset":[0.5,0]})").c_str());
    fx::float2 lo0, hi0;
    uvBounds(wall, front, lo0, hi0);
    uvBounds(r.mesh, front, lo, hi);
    check(near2(lo, {lo0.x + 0.5f, lo0.y}), "nudge: moved half a tile");
    r = op("uvNudge", wall, (faces + R"(,"rotation":90})").c_str());
    uvBounds(r.mesh, front, lo, hi);
    check(std::fabs((hi.x - lo.x) - 3.0f) < 1e-3f && std::fabs((hi.y - lo.y) - 4.0f) < 1e-3f, "nudge: turned a quarter");
    // Align: turned 30 degrees, aligned again to its bottom edge: back to level (u along x).
    PolyMesh turned = op("uvNudge", wall, (faces + R"(,"rotation":30})").c_str()).mesh;
    uint32_t a = ~0u, b = ~0u;
    for (uint32_t v = 0; v < wall.positions.size(); ++v)
        if (wall.positions[v].y < -1.4f && wall.positions[v].z > 0.09f)
            (a == ~0u ? a : b) = v;
    r = op("uvAlign", turned, (faces + R"(,"edges":[)" + std::to_string(edgeKey(a, b)) + "]}").c_str());
    uvBounds(r.mesh, front, lo, hi);
    check(std::fabs((hi.y - lo.y) - 3.0f) < 1e-3f || std::fabs((hi.x - lo.x) - 3.0f) < 1e-3f, "align: square to its edge again");
    {
        const std::vector<fx::float2> uvs = faceUvs(r.mesh, front);
        const Face& f = r.mesh.faces[front];
        float dv = 1e9f;
        for (uint32_t k = 0; k < f.count; ++k)
            if ((r.mesh.corners[f.first + k] == a && r.mesh.corners[f.first + (k + 1) % f.count] == b) ||
                (r.mesh.corners[f.first + k] == b && r.mesh.corners[f.first + (k + 1) % f.count] == a))
                dv = std::fabs(uvs[k].y - uvs[(k + 1) % f.count].y);
        check(dv < 1e-4f, "align: the edge runs along u");
    }
    // Match: a box's four sides, each turned and moved at random, carried on from the first:
    // every shared edge has the same UVs on both sides.
    PolyMesh box = makeShape("box", Json::parse(R"({"size":[2,2,2]})"));
    std::vector<uint32_t> sides;
    for (uint32_t f = 0; f < box.faces.size(); ++f)
        if (std::fabs(box.faceNormal(f).y) < 0.5f) {
            sides.push_back(f);
            box.faces[f].uv.rotation = float(f * 37 % 360);
            box.faces[f].uv.offset[0] = 0.13f * float(f);
        }
    std::string list;
    for (uint32_t f : sides)
        list += (list.empty() ? "" : ",") + std::to_string(f);
    r = op("uvMatch", box, (R"({"faces":[)" + list + "]}").c_str());
    uint32_t seams = 0, matched = 0;
    for (uint32_t f : sides)
        for (uint32_t g : sides) {
            if (f >= g)
                continue;
            const auto uf = faceUvs(r.mesh, f), ug = faceUvs(r.mesh, g);
            const Face &ff = r.mesh.faces[f], &fg = r.mesh.faces[g];
            uint32_t shared = 0, same = 0;
            for (uint32_t i = 0; i < ff.count; ++i)
                for (uint32_t j = 0; j < fg.count; ++j)
                    if (r.mesh.corners[ff.first + i] == r.mesh.corners[fg.first + j]) {
                        ++shared;
                        same += near2(uf[i], ug[j], 1e-3f);
                    }
            if (shared == 2) {
                ++seams;
                matched += same == 2;
            }
        }
    std::printf("     match: %u of %u seams continuous\n", matched, seams);
    check(seams == 4 && matched >= 3, "match: the texture carries on around the sides (the last seam closes the loop)");
    // Unwrap: Manual UVs, the area in UV space the area in metres (a metre a tile), charts apart.
    r = op("uvUnwrap", box, "{}");
    double area = 0.0;
    bool manual = true;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f) {
        manual &= r.mesh.faces[f].uv.mode == UvMode::Manual;
        const auto uvs = faceUvs(r.mesh, f);
        for (size_t k = 1; k + 1 < uvs.size(); ++k)
            area += 0.5 * std::fabs((uvs[k].x - uvs[0].x) * (uvs[k + 1].y - uvs[0].y) - (uvs[k + 1].x - uvs[0].x) * (uvs[k].y - uvs[0].y));
    }
    std::printf("     unwrap: UV area %.3f (24 m2 of faces)\n", area);
    check(manual && std::fabs(area - 24.0) < 0.5, "unwrap: Manual UVs at a metre a tile");
    // To Manual: the same UVs, now the corners' own; reset: a Local projection again.
    r = op("uv", wall, (faces + R"(,"mode":"manual"})").c_str());
    const auto before = faceUvs(wall, front), after = faceUvs(r.mesh, front);
    bool same = r.mesh.faces[front].uv.mode == UvMode::Manual && !r.mesh.cornerUvs.empty();
    for (size_t k = 0; k < before.size(); ++k)
        same &= near2(before[k], after[k]);
    check(same, "to manual: the face looks the same");
    r = op("uvReset", r.mesh, (faces + "}").c_str());
    check(r.mesh.faces[front].uv.mode == UvMode::Local && r.mesh.cornerUvs.empty(), "reset: a Local projection again");
    // World faces read the entity's transform: moved a metre in x, half... their UVs move with the world.
    PolyMesh worldWall = op("uv", wall, (faces + R"(,"mode":"world"})").c_str()).mesh;
    r = op("uvFit", worldWall, (faces + R"(,"world":[1,0,0,10, 0,1,0,0, 0,0,1,0]})").c_str());
    fx::float2 wlo, whi;
    const float shifted[3][4] = {{1, 0, 0, 10}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    wlo = {1e30f, 1e30f};
    whi = {-1e30f, -1e30f};
    for (fx::float2 p : faceUvs(r.mesh, front, shifted)) {
        wlo = {std::min(wlo.x, p.x), std::min(wlo.y, p.y)};
        whi = {std::max(whi.x, p.x), std::max(whi.y, p.y)};
    }
    check(near2(wlo, {0, 0}) && near2(whi, {1, 1}), "fit in world space: through the entity's transform");
    return g_failures;
}

int testTrims()
{
    using namespace fx::level;
    using fx::Json;
    // A run of three 1 x 0.5 m quads (a bevel's strip): onto the strip 0.25..0.5.
    PolyMesh strip = PolyMesh::plane(3, 0.5f, 3, 1);
    OpResult r = op("trim", strip, R"({"faces":[0,1,2],"v0":0.25,"v1":0.5})");
    bool inStrip = r.mesh.valid(), full = true;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f) {
        fx::float2 lo, hi;
        uvBounds(r.mesh, f, lo, hi);
        inStrip &= lo.y > 0.25f - 1e-4f && hi.y < 0.5f + 1e-4f && r.mesh.faces[f].uv.mode == UvMode::Manual;
        full &= std::fabs(lo.y - 0.25f) < 1e-4f && std::fabs(hi.y - 0.5f) < 1e-4f;
        // Square texels: 0.5 m across is 0.25 of v, so 1 m along is 0.5 of u.
        full &= std::fabs((hi.x - lo.x) - 0.5f) < 1e-3f;
    }
    check(inStrip && full, "trim: each face fills the strip's rows, u at square texels");
    // The run: shared vertices have the same UVs on both faces (u carries on, 0 .. 1.5).
    bool continuous = true;
    float umin = 1e30f, umax = -1e30f;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f)
        for (uint32_t g = f + 1; g < r.mesh.faces.size(); ++g) {
            const auto uf = faceUvs(r.mesh, f), ug = faceUvs(r.mesh, g);
            for (uint32_t i = 0; i < r.mesh.faces[f].count; ++i)
                for (uint32_t j = 0; j < r.mesh.faces[g].count; ++j)
                    if (r.mesh.corners[r.mesh.faces[f].first + i] == r.mesh.corners[r.mesh.faces[g].first + j])
                        continuous &= near2(uf[i], ug[j], 1e-3f);
        }
    for (fx::float2 c : r.mesh.cornerUvs)
        umin = std::min(umin, c.x), umax = std::max(umax, c.x);
    std::printf("     run: u %.3f .. %.3f\n", umin, umax);
    check(continuous && std::fabs((umax - umin) - 1.5f) < 1e-3f, "trim: the run carries u on across the shared edges");
    // A band round a pillar (sides wider than tall): three seams continuous, the fourth closes it.
    PolyMesh pillar = makeShape("box", Json::parse(R"({"size":[2,0.5,2]})"));
    std::string sides;
    for (uint32_t f = 0; f < pillar.faces.size(); ++f)
        if (std::fabs(pillar.faceNormal(f).y) < 0.5f)
            sides += (sides.empty() ? "" : ",") + std::to_string(f);
    r = op("trim", pillar, (R"({"faces":[)" + sides + R"(],"v0":0,"v1":0.125,"aspect":2})").c_str());
    uint32_t seams = 0;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f)
        for (uint32_t g = f + 1; g < r.mesh.faces.size(); ++g) {
            if (r.mesh.faces[f].uv.mode != UvMode::Manual || r.mesh.faces[g].uv.mode != UvMode::Manual)
                continue;
            const auto uf = faceUvs(r.mesh, f), ug = faceUvs(r.mesh, g);
            uint32_t shared = 0, same = 0;
            for (uint32_t i = 0; i < r.mesh.faces[f].count; ++i)
                for (uint32_t j = 0; j < r.mesh.faces[g].count; ++j)
                    if (r.mesh.corners[r.mesh.faces[f].first + i] == r.mesh.corners[r.mesh.faces[g].first + j]) {
                        ++shared;
                        same += near2(uf[i], ug[j], 1e-3f);
                    }
            seams += shared == 2 && same == 2;
        }
    std::printf("     pillar: %u continuous seams\n", seams);
    check(seams >= 3, "trim: a ring round a pillar carries on (the last seam closes it)");
    OpResult bad;
    check(!runOp("trim", strip, Json::parse(R"({"faces":[0],"v0":0.5,"v1":0.25})"), bad), "trim: an upside-down strip is refused");
    // The .ftrim text: written and read back.
    TrimSheet sheet;
    sheet.material = "Assets/Trims/Metal.fmat";
    sheet.aspect = 2.0f;
    sheet.strips = {{"Edge", 0.0f, 0.125f}, {"Bolts", 0.25f, 0.375f}};
    TrimSheet back;
    std::string error;
    check(parseTrimSheet(trimSheetJson(sheet), back, error) && back.material == sheet.material && back.aspect == 2.0f &&
              back.strips.size() == 2 && back.strips[1].name == "Bolts" && back.strips[1].v1 == 0.375f,
          "trim sheet: the .ftrim text round trip");
    check(!parseTrimSheet("[1,2]", back, error), "trim sheet: not an object, refused");
    return g_failures;
}

int testLayers()
{
    using namespace fx::level;
    using fx::Json;
    const PolyMesh box = PolyMesh::box({2, 1, 2});
    // Layers on: three materials, the mask UVs made (every corner in 0..1, the charts apart).
    OpResult r = op("layers", box, R"({"materials":["Assets/Moss.fmat","Assets/Dirt.fmat",""],"blend":0.3})");
    const PolyMesh layered = r.mesh;
    bool inside = layered.cornerUv1.size() == layered.corners.size();
    double area = 0.0;
    for (const fx::float2 c : layered.cornerUv1)
        inside &= c.x >= 0.0f && c.x <= 1.0f && c.y >= 0.0f && c.y <= 1.0f;
    for (const Face& f : layered.faces)
        for (uint32_t k = 1; k + 1 < f.count; ++k) {
            const fx::float2 a = layered.cornerUv1[f.first], b = layered.cornerUv1[f.first + k], c = layered.cornerUv1[f.first + k + 1];
            area += 0.5 * std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
        }
    std::printf("     mask charts cover %.3f of the mask\n", area);
    check(layered.layered() && layered.layerMaterials[1] == "Assets/Dirt.fmat" && std::fabs(layered.layerBlend - 0.3f) < 1e-6f,
          "layers: the materials and blend set");
    check(inside && area > 0.3 && area < 1.0, "layers: the mask UVs made, inside 0..1, charts apart");
    // Format 2: saved and loaded the same; a format 1 blob (an older mesh) still loads.
    PolyMesh back;
    const std::vector<uint8_t> blob = layered.save();
    check(back.load(blob.data(), blob.size()) && back.hash() == layered.hash() && back.cornerUv1.size() == layered.cornerUv1.size(),
          "format 2: layers and mask UVs round trip");
    std::vector<uint8_t> old = box.save();
    old[4] = 1;  // (a plain box's blob is format 1's but for its number)
    check(back.load(old.data(), old.size()) && !back.layered(), "format 1 blobs still load");
    // Vertex colours: the top's four vertices red; ops keep them, new vertices white.
    const std::string top = std::to_string(topFace(layered));
    r = op("vertexColor", layered, (R"({"faces":[)" + top + R"(],"color":[1,0,0,1]})").c_str());
    const PolyMesh tinted = r.mesh;
    uint32_t red = 0;
    for (uint32_t c : tinted.vertexColors)
        red += c == 0xff0000ffu;
    check(tinted.vertexColors.size() == tinted.positions.size() && red == 4, "vertex colours: the top's four vertices red");
    r = op("extrude", tinted, (R"({"faces":[)" + top + R"(],"distance":1})").c_str());
    red = 0;
    for (uint32_t c : r.mesh.vertexColors)
        red += c == 0xff0000ffu;
    check(r.mesh.vertexColors.size() == r.mesh.positions.size() && red == 4 && r.mesh.layered(),
          "extrude: the colours and layers kept (the moved top keeps its red, the walls' new vertices white)");
    // Mask UVs through ops: the extruded face keeps its own; the new walls have none ((0, 0)).
    const uint32_t movedTop = uint32_t(r.selection[0]);
    const Face& before = layered.faces[topFace(layered)];
    const Face& after = r.mesh.faces[movedTop];
    bool kept = after.count == before.count;
    for (uint32_t k = 0; kept && k < after.count; ++k)
        kept &= near2(r.mesh.cornerUv1[after.first + k], layered.cornerUv1[before.first + k]);
    check(kept && r.mesh.cornerUv1.size() == r.mesh.corners.size(), "extrude: the moved face keeps its mask UVs");
    // Flip: the mask UVs turn with the corners (each vertex keeps its mask UV).
    r = op("flip", layered, "{}");
    bool follows = true;
    for (uint32_t f = 0; f < r.mesh.faces.size(); ++f)
        for (uint32_t k = 0; k < r.mesh.faces[f].count; ++k) {
            const fx::float3 p = r.mesh.positions[r.mesh.corners[r.mesh.faces[f].first + k]];  // (pack renumbers vertices)
            const Face& o = layered.faces[f];
            for (uint32_t j = 0; j < o.count; ++j)
                if (fx::length(layered.positions[layered.corners[o.first + j]] - p) < 1e-6f)
                    follows &= near2(layered.cornerUv1[o.first + j], r.mesh.cornerUv1[r.mesh.faces[f].first + k]);
        }
    check(follows, "flip: the mask UVs stay with their corners");
    // All white again: no colours kept.
    r = op("vertexColor", tinted, R"({"color":[1,1,1,1]})");
    check(r.mesh.vertexColors.empty(), "vertex colours: all white is none");
    // The paint brush's dab (mask_brush.h): a ball, painting round an edge onto both faces, not the
    // faces out of its reach, not a thin wall's far side; erasing takes it off again.
    const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // The mask's value (channel 0) at a point on a face: found in its triangles, read at its mask UV.
    const auto maskAt = [](const PolyMesh& m, const std::vector<uint8_t>& mask, fx::float3 p, fx::float3 normal) {
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            if (fx::dot(m.faceNormal(f), normal) < 0.9f)
                continue;
            const Face& fc = m.faces[f];
            for (uint32_t k = 1; k + 1 < fc.count; ++k) {
                const uint32_t c[3] = {fc.first, fc.first + k, fc.first + k + 1};
                const fx::float3 a = m.positions[m.corners[c[0]]], e1 = m.positions[m.corners[c[1]]] - a, e2 = m.positions[m.corners[c[2]]] - a;
                const float d00 = fx::dot(e1, e1), d01 = fx::dot(e1, e2), d11 = fx::dot(e2, e2), d20 = fx::dot(p - a, e1), d21 = fx::dot(p - a, e2);
                const float den = d00 * d11 - d01 * d01;
                const float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1.0f - v - w;
                if (u < -1e-4f || v < -1e-4f || w < -1e-4f || std::fabs(fx::dot(p - a, fx::cross(e1, e2))) > 1e-4f)
                    continue;
                const fx::float2 uv = {m.cornerUv1[c[0]].x * u + m.cornerUv1[c[1]].x * v + m.cornerUv1[c[2]].x * w,
                                       m.cornerUv1[c[0]].y * u + m.cornerUv1[c[1]].y * v + m.cornerUv1[c[2]].y * w};
                const uint32_t x = std::min(uint32_t(uv.x * kMaskSize), kMaskSize - 1), y = std::min(uint32_t(uv.y * kMaskSize), kMaskSize - 1);
                return int(mask[(size_t(y) * kMaskSize + x) * 4]);
            }
        }
        return -1;
    };
    fx::float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (const fx::float3 p : layered.positions)
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    const float cz = (lo.z + hi.z) * 0.5f, cx = (lo.x + hi.x) * 0.5f;
    MaskDab dab;
    dab.center = {hi.x, hi.y, cz};  // (on the edge between the top and the +x side)
    dab.normal = {0, 1, 0};
    dab.radius = 0.6f;
    dab.strength = 1.0f;
    std::vector<uint8_t> mask;
    check(paintMaskDab(layered, identity, dab, mask), "dab: painted");
    const int onTop = maskAt(layered, mask, {hi.x - 0.2f, hi.y, cz}, {0, 1, 0}), onSide = maskAt(layered, mask, {hi.x, hi.y - 0.2f, cz}, {1, 0, 0});
    const int below = maskAt(layered, mask, {cx, lo.y, cz}, {0, -1, 0}), across = maskAt(layered, mask, {lo.x, (lo.y + hi.y) * 0.5f, cz}, {-1, 0, 0});
    std::printf("     dab on the top edge: top %d, side %d, bottom %d, far side %d\n", onTop, onSide, below, across);
    check(onTop > 200 && onSide > 200, "dab: a ball round the point, onto both faces of the edge");
    check(below == 0 && across == 0, "dab: nothing out of its reach");
    dab.channel = 3;
    paintMaskDab(layered, identity, dab, mask);
    check(maskAt(layered, mask, {hi.x - 0.2f, hi.y, cz}, {0, 1, 0}) < 20, "dab: erasing takes it off");
    // A thin wall: a dab on its front doesn't come through to its back 10 cm behind.
    const PolyMesh wall = op("layers", PolyMesh::box({2, 1, 0.1f}), R"({"materials":["Assets/Moss.fmat","",""]})").mesh;
    fx::float3 wl = {1e30f, 1e30f, 1e30f}, wh = {-1e30f, -1e30f, -1e30f};
    for (const fx::float3 p : wall.positions)
        wl = {std::min(wl.x, p.x), std::min(wl.y, p.y), std::min(wl.z, p.z)}, wh = {std::max(wh.x, p.x), std::max(wh.y, p.y), std::max(wh.z, p.z)};
    const fx::float3 front = {(wl.x + wh.x) * 0.5f, (wl.y + wh.y) * 0.5f, wh.z};
    dab.center = front;
    dab.normal = {0, 0, 1};
    dab.channel = 0;
    dab.radius = 0.4f;
    mask.clear();
    paintMaskDab(wall, identity, dab, mask);
    const int frontValue = maskAt(wall, mask, front, {0, 0, 1}), backValue = maskAt(wall, mask, {front.x, front.y, wl.z}, {0, 0, -1});
    std::printf("     thin wall: front %d, back %d\n", frontValue, backValue);
    check(frontValue > 200 && backValue == 0, "dab: a thin wall's far side left alone");
    // A stroke (41 dabs 2 cm apart across the wall): each texel the most one dab gave it (each dab
    // painted alone, the brightest kept), not their sum, which ridges the edge with every dab; half
    // strength staying half however many dabs pass over.
    for (const float strength : {1.0f, 0.5f}) {
        mask.clear();
        MaskStroke stroke;
        std::vector<uint8_t> brightest(kMaskBytes, 0);
        dab.radius = 0.15f;
        dab.strength = strength;
        for (int i = 0; i <= 40; ++i) {
            dab.center = {front.x - 0.4f + 0.02f * float(i), front.y, front.z};
            paintMaskDab(wall, identity, dab, mask, &stroke);
            std::vector<uint8_t> alone;
            paintMaskDab(wall, identity, dab, alone);
            for (size_t k = 0; k < kMaskBytes; ++k)
                brightest[k] = std::max(brightest[k], alone[k]);
        }
        int worst = 0;
        for (size_t k = 0; k < kMaskBytes; ++k)
            worst = std::max(worst, std::abs(int(mask[k]) - int(brightest[k])));
        const int middle = maskAt(wall, mask, front, {0, 0, 1});
        std::printf("     stroke at %.1f: off the brightest single dab by %d at most, its middle %d\n", strength, worst, middle);
        check(worst <= 1, "stroke: each texel the most one dab gave it (an even edge)");
        check(std::abs(middle - int(std::lround(strength * 255.0f))) <= 2, "stroke: its strength, however many dabs");
    }
    return g_failures;
}

int testCuts()
{
    using namespace fx::level;
    using fx::Json;
    const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // A wall 4 x 3 x 0.2 with a material; a door cut from the floor: its volume gone exactly, the
    // mesh closed and facing out, the wall's faces keep their material, the cut's faces its cap.
    PolyMesh wall = PolyMesh::box({4, 3, 0.2f});
    wall.materials = {"Assets/Plaster.fmat"};
    Cut door;
    door.size[0] = 1.0f, door.size[1] = 2.2f, door.size[2] = 1.0f;
    door.world[1][3] = -0.6f;
    door.capMaterial = "Assets/Frame.fmat";
    CutResult r;
    check(cutMesh(wall, identity, {door}, r) && r.applied == 1, "a door cut from the wall");
    checkSolid(r.mesh, 0, 4.0 * 3.0 * 0.2 - 1.0 * 2.0 * 0.2, "the door's volume removed exactly, closed, facing out");
    uint32_t caps = 0, plaster = 0;
    for (const Face& f : r.mesh.faces)
        (r.mesh.materials[f.material] == "Assets/Frame.fmat" ? caps : plaster) += 1;
    std::printf("     %zu faces: %u plaster, %u cap\n", r.mesh.faces.size(), plaster, caps);
    check(caps >= 6 && plaster >= 10, "the cut's faces (the door's sides, head) take its cap material");
    // Each side its own material: every face left keeps its own side's (not another's, nor the cap).
    PolyMesh sided = wall;
    sided.materials.clear();
    for (uint32_t f = 0; f < sided.faces.size(); ++f) {
        sided.materials.push_back("Assets/Side" + std::to_string(f) + ".fmat");
        sided.faces[f].material = f;
    }
    CutResult rs;
    uint32_t kept = 0, wrong = 0;
    check(cutMesh(sided, identity, {door}, rs) && rs.applied == 1 && rs.sourceFace.size() == rs.mesh.faces.size(), "a door cut from a wall of six materials");
    for (uint32_t f = 0; f < rs.mesh.faces.size(); ++f)
        if (rs.sourceFace[f] != ~0u)
            (rs.mesh.materials[rs.mesh.faces[f].material] == sided.materials[sided.faces[rs.sourceFace[f]].material] ? kept : wrong) += 1;
    std::printf("     %u faces kept their side's material, %u did not\n", kept, wrong);
    check(kept >= 10 && wrong == 0, "the faces left keep their own side's material");
    // Moved with its entity: the same door, the wall's transform taken into account.
    float moved[3][4] = {{1, 0, 0, 10}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    Cut door2 = door;
    door2.world[0][3] = 10.0f;
    CutResult rm;
    check(cutMesh(wall, moved, {door2}, rm) && rm.applied == 1 && std::fabs(topologyOf(rm.mesh).volume - topologyOf(r.mesh).volume) < 1e-3,
          "the cut in world space, the result in the mesh's own (a moved wall, a moved door)");
    // A cut beside it does nothing; a window through it (a cylinder) makes a hole.
    Cut far = door;
    far.world[0][3] = 20.0f;
    CutResult rf;
    check(cutMesh(wall, identity, {far}, rf) && rf.applied == 0 && rf.mesh.hash() == wall.hash(), "a cut that misses changes nothing");
    Cut window;
    window.shape = CutShape::Cylinder;
    window.size[0] = 1.0f, window.size[1] = 1.0f, window.size[2] = 1.0f;
    window.world[1][1] = 0.0f, window.world[1][2] = -1.0f, window.world[2][1] = 1.0f, window.world[2][2] = 0.0f;  // (along z)
    window.world[1][3] = 0.6f;
    CutResult rw;
    check(cutMesh(wall, identity, {window}, rw) && rw.applied == 1, "a round window cut through");
    const double windowVolume = topologyOf(rw.mesh).volume;
    std::printf("     round window: %.4f m3 left (%.4f without it)\n", windowVolume, 4.0 * 3.0 * 0.2);
    check(topologyOf(rw.mesh).closed && windowVolume < 2.4 - 0.15 && windowVolume > 2.4 - 0.16, "its volume (a 1 m disc through 0.2 m) gone");
    // Intersect: only what is inside the cut.
    Cut keep = door;
    keep.intersect = true;
    CutResult ri;
    check(cutMesh(wall, identity, {keep}, ri) && std::fabs(topologyOf(ri.mesh).volume - 1.0 * 2.0 * 0.2) < 1e-4, "intersect keeps what is inside");
    // A level mesh as the cutter (a prism), and two cuts in order.
    Cut prism;
    prism.shape = CutShape::Mesh;
    prism.mesh = std::make_shared<PolyMesh>(makeShape("prism", Json::parse(R"({"size":[0.6,0.6,1]})")));
    prism.world[0][3] = 1.5f;
    prism.world[1][3] = 1.0f;
    CutResult r2;
    check(cutMesh(wall, identity, {door, prism}, r2) && r2.applied == 2 && topologyOf(r2.mesh).closed, "two cuts, one of them a level mesh");
    // An open mesh (the wall without its +x end) is clipped: no caps, the door's area gone from its
    // front, back and bottom; intersecting keeps just that; neighbouring pieces welded.
    const PolyMesh open = op("delete", wall, R"({"faces":[0]})").mesh;
    const auto surface = [](const PolyMesh& m) {
        double a = 0.0;
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            const Face& face = m.faces[f];
            const fx::float3 p0 = m.positions[m.corners[face.first]];
            for (uint32_t k = 1; k + 1 < face.count; ++k) {
                const fx::float3 c = fx::cross(m.positions[m.corners[face.first + k]] - p0, m.positions[m.corners[face.first + k + 1]] - p0);
                a += 0.5 * std::sqrt(double(c.x) * c.x + double(c.y) * c.y + double(c.z) * c.z);
            }
        }
        return a;
    };
    CutResult ro;
    check(cutMesh(open, identity, {door}, ro) && ro.applied == 1 && ro.clipped && ro.error.empty(), "an open mesh is clipped");
    uint32_t capFaces = 0;
    for (const uint32_t s : ro.sourceFace)
        capFaces += s == ~0u ? 1u : 0u;
    std::printf("     clipped wall: %.4f m2 (%.4f before), %zu faces, %zu vertices\n", surface(ro.mesh), surface(open), ro.mesh.faces.size(),
                ro.mesh.positions.size());
    check(capFaces == 0 && ro.sourceFace.size() == ro.mesh.faces.size() && std::fabs(surface(open) - 26.2) < 1e-4 &&
              std::fabs(surface(ro.mesh) - (26.2 - 4.2)) < 1e-3,
          "no caps; the door's 2 x 2 m front and back and its 1 x 0.2 m of the bottom gone");
    CutResult roi;
    check(cutMesh(open, identity, {keep}, roi) && roi.clipped && std::fabs(surface(roi.mesh) - 4.2) < 1e-3, "intersect keeps just what is inside");
    // A plane (open) with a sphere through it (convex: one piece), a damage hole (star-shaped: a
    // piece per face from its centre), a torus mesh (neither: can't clip, left as it is).
    const PolyMesh ground = PolyMesh::plane(4.0f, 4.0f, 4, 4);
    Cut ball;
    ball.shape = CutShape::Sphere;
    CutResult rp;
    check(cutMesh(ground, identity, {ball}, rp) && rp.clipped && rp.applied == 1, "a sphere clips a hole in a plane");
    const double holeArea = 16.0 - surface(rp.mesh);
    std::printf("     sphere hole: %.4f m2 (a 1 m disc: %.4f), %zu faces\n", holeArea, 3.14159265 * 0.25, rp.mesh.faces.size());
    check(holeArea > 0.77 && holeArea < 0.786, "its hole about a 1 m disc");
    Cut blob;
    blob.shape = CutShape::Damage;
    blob.seed = 3;
    CutResult rpd;
    check(cutMesh(ground, identity, {blob}, rpd) && rpd.clipped && rpd.applied == 1 && 16.0 - surface(rpd.mesh) > 0.3, "a damage hole clipped in it");
    std::printf("     damage hole: %.4f m2, %zu faces\n", 16.0 - surface(rpd.mesh), rpd.mesh.faces.size());
    Cut ring;
    ring.shape = CutShape::Mesh;
    ring.mesh = std::make_shared<PolyMesh>(makeShape("torus", Json::parse(R"({"radius":0.8,"tube":0.3})")));
    CutResult rt;
    check(!cutMesh(ground, identity, {ring}, rt) && !rt.error.empty() && rt.mesh.hash() == ground.hash() && !rt.clipped,
          "a cutter that isn't star-shaped can't clip: left as it is");
    std::printf("     (%s)\n", rt.error.c_str());
    // Neighbouring triangles' edge points welded: the clipped plane's boundary is only its outline and the hole.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const Face& f : rp.mesh.faces)
        for (uint32_t k = 0; k < f.count; ++k) {
            const uint32_t a = rp.mesh.corners[f.first + k], b = rp.mesh.corners[f.first + (k + 1) % f.count];
            edges[{std::min(a, b), std::max(a, b)}] += 1;
        }
    double boundary = 0.0;
    for (const auto& [e, n] : edges)
        if (n == 1)
            boundary += double(fx::length(rp.mesh.positions[e.first] - rp.mesh.positions[e.second]));
    std::printf("     boundary: %.4f m (16 + a 1 m circle: %.4f)\n", boundary, 16.0 + 3.14159265);
    check(boundary > 16.0 + 3.0 && boundary < 16.0 + 3.15, "welded: the boundary is the plane's outline and the hole's");
    // Manual UVs and layers' mask UVs carried onto the cut faces (parallel to the corners).
    PolyMesh layered = op("layers", wall, R"({"materials":["Assets/Moss.fmat","",""]})").mesh;
    layered = op("uvUnwrap", layered, "{}").mesh;
    CutResult rl;
    check(cutMesh(layered, identity, {door}, rl) && rl.mesh.cornerUv1.size() == rl.mesh.corners.size() &&
              rl.mesh.cornerUvs.size() == rl.mesh.corners.size() && rl.mesh.layered(),
          "UVs, mask UVs and layers carried through the cut");
    // Damage: a lumpy hole through the wall, the same for the same seed, another for another; a
    // corner of a box broken off; rougher reaching further.
    Cut hole;
    hole.shape = CutShape::Damage;
    hole.size[0] = 1.0f, hole.size[1] = 1.0f, hole.size[2] = 1.0f;
    hole.world[1][3] = 0.5f;
    hole.seed = 3;
    CutResult rd, rd2, rd3;
    check(cutMesh(wall, identity, {hole}, rd) && rd.applied == 1 && topologyOf(rd.mesh).closed, "a damage hole through the wall");
    const double holeGone = 2.4 - topologyOf(rd.mesh).volume;
    std::printf("     damage hole: %.4f m3 gone (a 1 m ball: %.4f)\n", holeGone, 3.14159 * 0.5 * 0.5 * 0.2);
    check(holeGone > 0.05 && holeGone < 0.3, "about a 1 m ball's worth through 0.2 m");
    check(cutMesh(wall, identity, {hole}, rd2) && rd2.mesh.hash() == rd.mesh.hash(), "the same seed: the same hole");
    Cut other = hole;
    other.seed = 4;
    check(cutMesh(wall, identity, {other}, rd3) && rd3.mesh.hash() != rd.mesh.hash(), "another seed: another hole");
    fx::float3 lo, hi;
    cutBounds(hole, lo, hi);
    check(hi.x - lo.x > 1.0f && hi.x - lo.x < 1.8f, "its bounds hold its lumps");
    const PolyMesh box = PolyMesh::box({1, 1, 1});
    Cut corner = hole;
    corner.size[0] = corner.size[1] = corner.size[2] = 0.4f;
    corner.world[0][3] = corner.world[1][3] = corner.world[2][3] = 0.5f;
    CutResult rc;
    check(cutMesh(box, identity, {corner}, rc) && rc.applied == 1 && topologyOf(rc.mesh).closed && topologyOf(rc.mesh).volume < 1.0 &&
              topologyOf(rc.mesh).volume > 0.98,
          "a box's corner broken off");
    // Where the damage brush's dabs go on the box's top: on its edge, at its corner, or where it is.
    uint32_t top = 0;
    for (uint32_t f = 0; f < box.faces.size(); ++f)
        if (box.faceNormal(f).y > 0.9f)
            top = f;
    fx::float3 spot;
    check(damageSpot(box, top, {0.1f, 0.5f, 0.3f}, DamageSnap::Chip, spot) && std::fabs(spot.z - 0.5f) < 1e-5f && std::fabs(spot.x - 0.1f) < 1e-5f,
          "a chip: on the nearest sharp edge");
    check(damageSpot(box, top, {0.3f, 0.5f, 0.4f}, DamageSnap::Corner, spot) && std::fabs(spot.x - 0.5f) < 1e-5f && std::fabs(spot.z - 0.5f) < 1e-5f,
          "a corner break: at the nearest corner");
    check(damageSpot(box, top, {0.1f, 0.5f, 0.3f}, DamageSnap::Hole, spot) && std::fabs(spot.x - 0.1f) < 1e-5f, "a hole: where it is");
    // Damage over the selection: chips along every sharp edge (a 1 m box, 25 cm apart: 4 an edge,
    // out between its faces), along one chosen edge, a corner at a vertex, holes over a face.
    {
        const auto all = damageSpots(box, identity, 0, {}, 0.25f, 1);
        bool onEdges = all.size() == 48;
        for (const DamageSpotAt& s : all) {
            const int edgeAxes = (std::fabs(std::fabs(s.at.x) - 0.5f) < 1e-4f) + (std::fabs(std::fabs(s.at.y) - 0.5f) < 1e-4f) +
                                 (std::fabs(std::fabs(s.at.z) - 0.5f) < 1e-4f);
            onEdges = onEdges && s.kind == DamageSnap::Chip && edgeAxes >= 2 && dot(s.normal, s.at) > 0.6f;
        }
        std::printf("     every edge: %zu chips\n", all.size());
        check(onEdges, "damage: chips along every sharp edge, out between its faces");
        float scaled[3][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}};
        check(damageSpots(box, scaled, 0, {}, 0.25f, 1).size() == 96, "damage: spaced in the world (a box twice the size, twice the chips)");
        const uint32_t a = box.corners[box.faces[top].first], b = box.corners[box.faces[top].first + 1];
        const auto one = damageSpots(box, identity, 2, {uint64_t(std::min(a, b)) << 32 | std::max(a, b)}, 0.25f, 1);
        check(one.size() == 4 && one[0].normal.y > 0.6f, "damage: along one chosen edge");
        const auto corner = damageSpots(box, identity, 1, {a}, 0.25f, 1);
        check(corner.size() == 1 && corner[0].kind == DamageSnap::Corner && length(corner[0].at - box.positions[a]) < 1e-5f &&
                  dot(corner[0].normal, corner[0].at) > 0.8f,
              "damage: a broken corner at a chosen vertex, out from it");
        const auto holes = damageSpots(box, identity, 3, {top}, 0.25f, 1);
        bool onTop = holes.size() == 16;
        for (const DamageSpotAt& s : holes)
            onTop = onTop && s.kind == DamageSnap::Hole && std::fabs(s.at.y - 0.5f) < 1e-4f && std::fabs(s.at.x) <= 0.5f &&
                    std::fabs(s.at.z) <= 0.5f && s.normal.y > 0.99f;
        std::printf("     a face: %zu holes\n", holes.size());
        check(onTop, "damage: holes over a chosen face, about one per spacing squared");
        const auto again = damageSpots(box, identity, 3, {top}, 0.25f, 1), other = damageSpots(box, identity, 3, {top}, 0.25f, 2);
        check(again.size() == holes.size() && length(again[0].at - holes[0].at) < 1e-6f && length(other[0].at - holes[0].at) > 1e-4f,
              "damage: the same holes for one seed, others for another");
        check(damageSpots(box, identity, 3, {top}, 5.0f, 1).size() == 1, "damage: a face smaller than the spacing still gets one");
        // Randomness: the chips stray along their edges (still on them) and vary in size; none at 0.
        // Density: about its share of the places, the same for one seed.
        const auto even = damageSpots(box, identity, 0, {}, 0.25f, 1, 0.0f, 1.0f), strayed = damageSpots(box, identity, 0, {}, 0.25f, 1, 1.0f, 1.0f);
        bool sameEven = even.size() == all.size(), onEdge = strayed.size() == all.size(), strayedAny = false, sized = false, evenSize = true;
        for (size_t i = 0; i < even.size() && i < all.size(); ++i)
            sameEven = sameEven && length(even[i].at - all[i].at) < 1e-6f, evenSize = evenSize && even[i].scale == 1.0f;
        for (size_t i = 0; i < strayed.size() && i < all.size(); ++i) {
            const fx::float3 p = strayed[i].at;
            const int edgeAxes = (std::fabs(std::fabs(p.x) - 0.5f) < 1e-4f) + (std::fabs(std::fabs(p.y) - 0.5f) < 1e-4f) + (std::fabs(std::fabs(p.z) - 0.5f) < 1e-4f);
            onEdge = onEdge && edgeAxes >= 2 && strayed[i].scale >= 0.5f && strayed[i].scale <= 1.5f;
            strayedAny = strayedAny || length(p - all[i].at) > 0.02f;
            sized = sized || std::fabs(strayed[i].scale - 1.0f) > 0.05f;
        }
        check(sameEven && evenSize, "damage: no randomness, evenly spaced and one size");
        check(onEdge && strayedAny && sized, "damage: randomness strays the chips along their edges and varies their size");
        const auto half = damageSpots(box, identity, 0, {}, 0.25f, 1, 0.0f, 0.5f), halfAgain = damageSpots(box, identity, 0, {}, 0.25f, 1, 0.0f, 0.5f);
        std::printf("     density 50%%: %zu of %zu chips\n", half.size(), all.size());
        check(half.size() >= 14 && half.size() <= 34 && halfAgain.size() == half.size(), "damage: density keeps about its share of the places, the same for one seed");
        check(damageSpots(box, identity, 0, {}, 0.25f, 1, 0.0f, 0.0f).empty(), "damage: density 0, none");
        check(damageSpots(box, identity, 0, {}, 0.5f, 1).size() == 24, "damage: the spacing sets how many (50 cm apart, 2 an edge)");
    }
    // Rubble: closed chunks facing out, resting on y = 0 within their spread; the same seed the same
    // heap, another seed another.
    RubbleSettings rubble;
    rubble.seed = 7;
    rubble.count = 6;
    rubble.size = 0.2f;
    rubble.spread = 0.5f;
    rubble.material = "Assets/Concrete.fmat";
    const PolyMesh heap = makeRubble(rubble);
    checkSolid(heap, 6 * 20, 0.0, "rubble: six closed chunks");
    fx::float3 heapLo, heapHi;
    heap.bounds(heapLo, heapHi);
    std::printf("     rubble: y %.3f..%.3f, x %.2f..%.2f, z %.2f..%.2f\n", heapLo.y, heapHi.y, heapLo.x, heapHi.x, heapLo.z, heapHi.z);
    check(heapLo.y < 0.0f && heapLo.y > -0.03f && heapHi.y > 0.03f && heapHi.y < 0.3f, "rubble rests on the ground, a little sunk in");
    check(std::max({-heapLo.x, heapHi.x, -heapLo.z, heapHi.z}) < 0.5f + 0.2f * 1.2f && heap.materials.size() == 1 && heap.materials[0] == rubble.material,
          "rubble within its spread, its material");
    const PolyMesh again = makeRubble(rubble);
    rubble.seed = 8;
    const PolyMesh another = makeRubble(rubble);
    const auto same = [](const PolyMesh& a, const PolyMesh& b) {
        return a.positions.size() == b.positions.size() && std::memcmp(a.positions.data(), b.positions.data(), a.positions.size() * sizeof(fx::float3)) == 0;
    };
    check(same(again, heap) && !same(another, heap), "the same seed the same heap, another another");
    // Baked cuts: a key from what was cut (the same for the wall saved and read back, another for a
    // moved door, another cap, another wall), the bake's blob read back to the very meshes.
    PolyMesh wallAgain;
    const std::vector<uint8_t> wallBlob = wall.save();
    check(wallAgain.load(wallBlob.data(), wallBlob.size()) && meshKey(wallAgain) == meshKey(wall), "a mesh's key: its content");
    const uint64_t key = cutKey(meshKey(wall), identity, {door, prism});
    Cut capped = door;
    capped.capMaterial = "Assets/Other.fmat";
    PolyMesh taller = PolyMesh::box({4, 3.1f, 0.2f});
    taller.materials = wall.materials;
    check(key == cutKey(meshKey(wallAgain), identity, {door, prism}) && key != cutKey(meshKey(wall), moved, {door, prism}) &&
              key != cutKey(meshKey(wall), identity, {door2, prism}) && key != cutKey(meshKey(wall), identity, {capped, prism}) &&
              key != cutKey(meshKey(wall), identity, {prism, door}) && key != cutKey(meshKey(taller), identity, {door, prism}),
          "a cut's key: the same inputs the same key; moved, another cap, another order or another wall another");
    CutBake bake;
    auto baked = std::make_shared<BakedCut>();
    baked->mesh = std::make_shared<PolyMesh>(r2.mesh);
    baked->sourceFace = r2.sourceFace;
    bake.results[key] = baked;
    auto bakedHole = std::make_shared<BakedCut>();
    bakedHole->mesh = std::make_shared<PolyMesh>(rd.mesh);
    bakedHole->sourceFace = rd.sourceFace;
    bake.results[cutKey(meshKey(wall), identity, {hole})] = bakedHole;
    const std::vector<uint8_t> bakeBlob = bake.save();
    CutBake read;
    check(read.load(bakeBlob.data(), bakeBlob.size()) && read.results.size() == 2 && read.results.count(key) &&
              read.results[key]->mesh->hash() == r2.mesh.hash() && read.results[key]->sourceFace == r2.sourceFace &&
              read.results[cutKey(meshKey(wall), identity, {hole})]->mesh->hash() == rd.mesh.hash(),
          "the bake read back: its meshes and faces as cut");
    std::printf("     bake of 2 cuts: %zu bytes\n", bakeBlob.size());
    CutBake bad;
    check(!bad.load(bakeBlob.data(), bakeBlob.size() - 3) && bad.results.empty() && !bad.load(wallBlob.data(), wallBlob.size()),
          "a short or foreign blob refused");
    check(cutBakePathFor("Scenes/Main.fpak") == "Scenes/Main.cuts" && cutBakePathFor("a.b/Main") == "a.b/Main.cuts", "<scene>.cuts");
    return g_failures;
}

int testModifiers()
{
    using namespace fx::level;
    using fx::Json;
    std::string error;
    std::vector<uint32_t> source;
    const auto run = [&](const PolyMesh& in, std::vector<Modifier> stack) {
        PolyMesh out;
        error.clear();
        if (!applyModifiers(in, stack, out, &source, &error))
            std::printf("     (%s)\n", error.c_str());
        return out;
    };
    // A unit box from x 0 to 1.
    PolyMesh half = PolyMesh::box({1, 1, 1});
    for (fx::float3& p : half.positions)
        p.x += 0.5f;
    // Mirror: welded, the seam's faces gone: one 2 x 1 x 1 box; not welded: two boxes.
    Modifier mirror = makeModifier(ModifierKind::Mirror);
    checkSolid(run(half, {mirror}), 10, 2.0, "mirror x, welded: one box twice as long");
    bool sourced = source.size() == 10;
    for (const uint32_t s : source)
        sourced &= s < half.faces.size();
    check(sourced, "mirror: each face's source face");
    checkSolid(run(PolyMesh::box({1, 1, 1}), {mirror}), 6, 1.0, "mirror of a symmetric box, welded: the box (its copy over it gone)");
    mirror.weld = false;
    checkSolid(run(half, {mirror}), 12, 2.0, "mirror x, not welded: two boxes");
    // Arrays: three in a row (touching faces gone), a ring of eight, a 3 x 3 grid.
    const PolyMesh box = PolyMesh::box({1, 1, 1});
    checkSolid(run(box, {makeModifier(ModifierKind::Array)}), 14, 3.0, "array: three in a row, welded");
    Modifier ring = makeModifier(ModifierKind::Array, ArrayMode::Radial);
    const PolyMesh ringed = run(box, {ring});
    checkSolid(ringed, 48, 8.0, "radial array: eight around y");
    fx::float3 lo, hi;
    ringed.bounds(lo, hi);
    check(hi.x > 2.4f && lo.x < -2.4f && hi.z > 2.4f && lo.z < -2.4f, "radial array: two metres out, all the way around");
    checkSolid(run(box, {makeModifier(ModifierKind::Array, ArrayMode::Grid)}), 30, 9.0, "grid array: 3 x 3, welded");
    Modifier jitter = makeModifier(ModifierKind::Array);
    jitter.step = {2, 0, 0};
    jitter.jitter = {0.2f, 30.0f, 0.2f};
    jitter.weld = false;  // (apart anyway; its vertices kept in order)
    const PolyMesh j1 = run(box, {jitter});
    jitter.seed = 2;
    const PolyMesh j2 = run(box, {jitter});
    bool firstStays = true;
    for (size_t v = 0; v < 8; ++v)
        firstStays &= j1.positions[v].x == box.positions[v].x && j1.positions[v].y == box.positions[v].y;
    check(firstStays && std::memcmp(j1.positions.data(), j2.positions.data(), j1.positions.size() * sizeof(fx::float3)) != 0,
          "array jitter: the first copy where it is, another seed other places");
    // Solidify: a 2 x 2 plane 10 cm thick, closed by its rim.
    Modifier solid = makeModifier(ModifierKind::Solidify);
    checkSolid(run(PolyMesh::plane(2, 2), {solid}), 6, 0.4, "solidify: a plane made a slab");
    // Bevel: a unit box's 12 edges and 8 corners cut 10 cm in (its volume less the edges' and corners' pieces).
    Modifier bevel = makeModifier(ModifierKind::Bevel);
    bevel.amount = 0.1f;
    const double w = 0.1, chamfered = 1.0 - 12.0 * (w * w / 2.0) * (1.0 - 2.0 * w) - 8.0 * (5.0 / 6.0) * w * w * w;
    checkSolid(run(box, {bevel}), 26, chamfered, "bevel: a box's edges and corners chamfered");
    // A cylinder's sharp rims bevelled, its smooth sides left (and still closed).
    const PolyMesh cylinder = makeShape("cylinder", Json::parse(R"({"radius":0.5,"height":1,"sides":16})"));
    const PolyMesh rounded = run(cylinder, {bevel});
    checkSolid(rounded, 0, 0.0, "bevel: a cylinder's rims, still closed");
    std::printf("     bevelled cylinder: %zu faces (from %zu)\n", rounded.faces.size(), cylinder.faces.size());
    check(rounded.faces.size() == cylinder.faces.size() + 32, "bevel: a face along each rim edge only");
    // Round (16 segments): the box's edges quarter circles, its corners eighths of a ball.
    Modifier roundBevel = bevel;
    roundBevel.segments = 16;
    const double pi = 3.14159265358979;
    const double roundBox = 1.0 - 12.0 * (w * w * (1.0 - pi / 4.0)) * (1.0 - 2.0 * w) - 8.0 * (w * w * w * (1.0 - pi / 6.0));
    const PolyMesh roundedBox = run(box, {roundBevel});
    checkSolid(roundedBox, 0, roundBox, "bevel round: a rounded box");
    std::printf("     rounded box: %zu faces\n", roundedBox.faces.size());
    roundBevel.curve = 0.0f;
    checkSolid(run(box, {roundBevel}), 0, chamfered, "bevel 16 segments flat: the chamfer's volume");
    // Deform: tapered to half at its top, twisted a quarter turn.
    Modifier taper = makeModifier(ModifierKind::Deform, ArrayMode::Linear, DeformType::Taper);
    const PolyMesh tapered = run(box, {taper});
    tapered.bounds(lo, hi);
    bool halved = true;
    for (const fx::float3& p : tapered.positions)
        if (p.y > 0.4f)
            halved &= std::fabs(std::fabs(p.x) - 0.25f) < 1e-4f && std::fabs(std::fabs(p.z) - 0.25f) < 1e-4f;
    check(halved && hi.x > 0.49f, "taper: its top half as wide, its bottom as it was");
    Modifier twist = makeModifier(ModifierKind::Deform, ArrayMode::Linear, DeformType::Twist);
    const PolyMesh twisted = run(box, {twist});
    bool turned = true;
    for (size_t v = 0; v < box.positions.size(); ++v)
        if (box.positions[v].y > 0.4f)
            turned &= std::fabs(twisted.positions[v].x + box.positions[v].z) < 1e-4f || std::fabs(twisted.positions[v].x - box.positions[v].z) < 1e-4f;
    check(turned, "twist: its top turned a quarter");
    Modifier bend = makeModifier(ModifierKind::Deform, ArrayMode::Linear, DeformType::Bend);
    bend.axis = 0;
    bend.angle = 90.0f;
    Modifier pieces = makeModifier(ModifierKind::Array);  // (a 4 m bar in 20 pieces: something to bend)
    pieces.count[0] = 20;
    const PolyMesh bar = run(PolyMesh::box({0.2f, 0.2f, 0.2f}), {pieces});
    const PolyMesh bent = run(bar, {bend});
    bent.bounds(lo, hi);
    std::printf("     bent bar: x %.2f..%.2f, y %.2f..%.2f\n", lo.x, hi.x, lo.y, hi.y);
    check(hi.y > 2.0f && hi.x < 3.0f, "bend: a bar curled a quarter turn towards y");
    // A stack: mirrored, then three in a row; JSON back and forth; a disabled one skipped.
    checkSolid(run(half, {makeModifier(ModifierKind::Mirror), makeModifier(ModifierKind::Array)}), 26, 6.0, "mirror then array");
    for (const Modifier& m : {mirror, ring, jitter, solid, bevel, taper, twist, bend}) {
        fx::JsonWriter jw;
        writeModifierJson(jw, nullptr, m);
        check(modifierFromJson(Json::parse(jw.str())) == m, ("JSON back and forth: " + modifierTitle(m)).c_str());
    }
    Modifier off = makeModifier(ModifierKind::Array);
    off.enabled = false;
    check(run(box, {off}).faces.size() == 6, "a disabled modifier skipped");
    return g_failures;
}

int testSplines()
{
    using namespace fx::level;
    using fx::Json;
    // A straight one: its length, frames running along x with y up and z to its right.
    Spline straight;
    straight.points = {{{0, 0, 0}}, {{10, 0, 0}}};
    const std::vector<SplineFrame> frames = splineFrames(straight, 0.5f);
    check(std::fabs(splineLength(straight) - 10.0f) < 1e-3f && frames.size() == 21 && std::fabs(frames.back().distance - 10.0f) < 1e-3f,
          "a straight spline: 10 m, a frame every half metre");
    check(frames[5].tangent.x > 0.999f && frames[5].up.y > 0.999f && frames[5].side.z > 0.999f, "its frames: along x, y up, z to its right");
    const SplineFrame past = frameAt(frames, 12.0f);
    check(std::fabs(past.position.x - 12.0f) < 1e-3f, "past its end: on along its tangent");
    // Smooth through its points; corners make straight sides (a 10 m square: 40 m round).
    Spline bend;
    bend.points = {{{0, 0, 0}}, {{10, 0, 0}}, {{10, 0, 10}}};
    const std::vector<fx::float3> curve = splineCurve(bend, 0.25f);
    bool through = false;
    for (const fx::float3& p : curve)
        through |= std::fabs(p.x - 10.0f) < 1e-4f && std::fabs(p.z) < 1e-4f;
    check(through && splineLength(bend) > 20.0f, "a smooth spline goes through its points");
    Spline square;
    square.closed = true;
    for (const fx::float3 p : {fx::float3{0, 0, 0}, fx::float3{10, 0, 0}, fx::float3{10, 0, 10}, fx::float3{0, 0, 10}})
        square.points.push_back({p, true});
    std::printf("     square: %.3f m round\n", splineLength(square));
    check(std::fabs(splineLength(square) - 40.0f) < 1e-2f, "corners: straight sides, a closed square 40 m round");
    // Lofts: a road along the straight one (4 x 0.2 m: 8 m^3), a kerb round the square, a pipe.
    LoftSettings road = loftPreset("road");
    road.material = "Assets/Asphalt.fmat";
    const PolyMesh slab = loftSpline(straight, road);
    checkSolid(slab, 0, 8.0, "loft: a road 10 m long, 4 m wide, 20 cm thick");
    fx::float3 lo, hi;
    slab.bounds(lo, hi);
    check(std::fabs(hi.y) < 1e-4f && std::fabs(lo.y + 0.2f) < 1e-4f && std::fabs(hi.z - 2.0f) < 1e-4f, "the road's top on the curve");
    check(slab.materials.size() == 1 && slab.materials[0] == road.material && slab.cornerUvs.size() == slab.corners.size(), "its material, its UVs");
    checkSolid(loftSpline(square, loftPreset("kerb")), 0, 0.0, "loft: a kerb round a closed square (no caps, closed)");
    checkSolid(loftSpline(bend, loftPreset("pipe")), 0, 0.0, "loft: a pipe along a curve");
    check(loftSpline(straight, LoftSettings{}).faces.empty(), "no profile: nothing");
    // The curve modifier: a unit box bent along the straight one (ten end to end, welded), along the
    // bend (closed still); copies every 2 m (six).
    const PolyMesh box = PolyMesh::box({1, 1, 1});
    Modifier follow = makeModifier(ModifierKind::Curve);
    ModifierContext context{&straight};
    PolyMesh out;
    applyModifiers(box, {follow}, out, nullptr, nullptr, &context);
    checkSolid(out, 42, 10.0, "follow spline: ten boxes end to end along 10 m");
    context.spline = &bend;
    applyModifiers(box, {follow}, out, nullptr, nullptr, &context);
    checkSolid(out, 0, 0.0, "follow spline: bent round a curve, still closed");
    out.bounds(lo, hi);
    check(hi.z > 9.0f && hi.x > 9.0f, "follow spline: all the way round the bend");
    Modifier copies = makeModifier(ModifierKind::Curve);
    copies.copies = true;
    copies.spacing = 2.0f;
    context.spline = &straight;
    applyModifiers(box, {copies}, out, nullptr, nullptr, &context);
    checkSolid(out, 36, 6.0, "copies along a spline: six, 2 m apart");
    applyModifiers(box, {follow}, out);
    check(out.faces.size() == 6, "no spline: the mesh as it is");
    // JSON back and forth.
    fx::JsonWriter jw;
    writeSpline(jw, nullptr, bend, road);
    Spline back;
    LoftSettings backLoft;
    readSpline(Json::parse(jw.str()), back, backLoft);
    check(back == bend && backLoft == road, "a spline and its loft read back from JSON");
    fx::JsonWriter jm;
    writeModifierJson(jm, nullptr, copies);
    check(modifierFromJson(Json::parse(jm.str())) == copies, "the curve modifier read back from JSON");
    return g_failures;
}

int testNavigation()
{
    using namespace fx::level;
    using fx::float3;
    // A 30 x 30 m floor (several tiles) with a wall across it, a 2 m gap at its far end.
    NavInput in;
    const auto box = [&](float3 lo, float3 hi, bool walkable) {
        const uint32_t base = uint32_t(in.vertices.size());
        for (int k = 0; k < 8; ++k)
            in.vertices.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        const uint32_t faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (const auto& f : faces) {
            in.indices.insert(in.indices.end(), {base + f[0], base + f[1], base + f[2], base + f[0], base + f[2], base + f[3]});
            in.walkable.insert(in.walkable.end(), {uint8_t(walkable), uint8_t(walkable)});
        }
    };
    box({-15, -0.5f, -15}, {15, 0, 15}, true);
    box({-0.2f, 0, -15}, {0.2f, 3, 11}, true);  // (its top too high to climb, too narrow to stand on)
    NavMesh nav;
    std::string error;
    check(nav.build(in, NavSettings{}, &error), ("built: " + error).c_str());
    std::printf("     navmesh: %u tiles, %u polygons\n", nav.tileCount(), nav.polygonCount());
    check(nav.tileCount() > 4 && nav.polygonCount() > 4, "several tiles, some polygons");
    // Across the wall: round its end (through the gap), longer than straight across.
    std::vector<float3> path;
    check(nav.findPath({-5, 0, 0}, {5, 0, 0}, path) && path.size() > 2, "a path across the wall");
    float length = 0.0f, farthest = -1e30f;
    for (size_t i = 1; i < path.size(); ++i)
        length += fx::length(path[i] - path[i - 1]);
    for (const float3& p : path)
        farthest = std::max(farthest, p.z);
    std::printf("     path: %zu corners, %.1f m, out to z %.1f\n", path.size(), length, farthest);
    check(length > 25.0f && farthest > 10.5f, "round the wall's end");
    // A straight walk into the wall stops at it; one along it gets there.
    float t;
    float3 hit;
    check(nav.raycast({-5, 0, 0}, {5, 0, 0}, t, hit) && hit.x < 0.0f && hit.x > -1.0f, "a walk into the wall stops at it");
    check(!nav.raycast({-5, 0, 0}, {-5, 0, 8}, t, hit), "a walk along it gets there");
    float3 on;
    check(nav.nearest({3, 1.5f, 3}, on) && std::fabs(on.y) < 0.3f, "the nearest point: on the floor");
    check(!nav.nearest({3, 20, 3}, on), "nothing near high above it");
    // Saved and read back: the same path.
    const std::vector<uint8_t> blob = nav.save();
    NavMesh back;
    std::vector<float3> again;
    check(back.load(blob.data(), blob.size()) && back.polygonCount() == nav.polygonCount() && back.findPath({-5, 0, 0}, {5, 0, 0}, again) &&
              again.size() == path.size(),
          "saved and read back");
    // A blocker volume in the gap: no way round any more; a costly area across the open floor: the
    // path goes round it.
    NavInput gapped = in;
    NavVolume blocker;
    blocker.outline = {{-2, 0, 10.5f}, {2, 0, 10.5f}, {2, 0, 15.5f}, {-2, 0, 15.5f}};
    blocker.top = 3.0f;
    blocker.bottom = -1.0f;
    gapped.volumes.push_back(blocker);
    NavMesh shut;
    check(shut.build(gapped, NavSettings{}, &error) && !shut.findPath({-5, 0, 0}, {5, 0, 0}, again), "a blocker in the gap: no way across");
    NavInput open;
    open.vertices = {in.vertices.begin(), in.vertices.begin() + 8};
    open.indices = {in.indices.begin(), in.indices.begin() + 36};
    NavVolume mud;
    mud.outline = {{-2, 0, -15}, {2, 0, -15}, {2, 0, 5}, {-2, 0, 5}};
    mud.blocks = false;
    mud.cost = 20.0f;
    mud.bottom = -1.0f;
    mud.top = 2.0f;
    open.volumes.push_back(mud);
    NavMesh costly;
    std::vector<float3> round;
    check(costly.build(open, NavSettings{}, &error) && costly.findPath({-8, 0, 0}, {8, 0, 0}, round), "a path over a floor with a costly area");
    float out = -1e30f;
    for (const float3& p : round)
        out = std::max(out, p.z);
    std::printf("     round the costly area: out to z %.1f\n", out);
    check(out > 4.5f, "round the costly area");
    const std::vector<uint8_t> costBlob = costly.save();
    NavMesh costBack;
    std::vector<float3> roundBack;
    check(costBack.load(costBlob.data(), costBlob.size()) && costBack.findPath({-8, 0, 0}, {8, 0, 0}, roundBack) && roundBack.size() == round.size(),
          "its costs read back");
    // A crowd: an agent walked round the wall to its target (a step every 50 ms); a stale id.
    auto shared = std::make_shared<NavMesh>();
    check(shared->build(in, NavSettings{}, &error), "built again (shared)");
    NavCrowd crowd(shared, 1);
    const int32_t walker = crowd.add({-5, 0, 0}, 0.4f, 1.8f, 4.0f, 10.0f);
    check(walker >= 0 && crowd.target(walker, {5, 0, 0}), "an agent with a target");
    float3 at, vel;
    float left = -1.0f;
    uint32_t steps = 0;
    for (; steps < 600; ++steps) {
        crowd.update(0.05f);
        left = crowd.state(walker, at, vel);
        if (steps > 10 && left < 0.3f)
            break;
    }
    std::printf("     walker: at %.2f %.2f %.2f after %.1f s (%.2f m left)\n", at.x, at.y, at.z, float(steps) * 0.05f, left);
    check(left >= 0.0f && left < 0.5f && std::fabs(at.x - 5.0f) < 0.6f && std::fabs(at.z) < 0.6f && steps > 100, "the agent walked round the wall to its target");
    check(crowd.state(int32_t(2u << 16 | uint32_t(walker & 0xffff)), at, vel) < 0.0f, "another crowd's id: none");
    // Not walkable (an obstacle's top): nothing on it.
    NavInput blocked = in;
    std::fill(blocked.walkable.begin(), blocked.walkable.end(), uint8_t(0));
    NavMesh none;
    check(!none.build(blocked, NavSettings{}, &error), "nothing walkable: no mesh");
    return g_failures;
}

int testWalls()
{
    using namespace fx::level;
    // A straight wall 4 m long, 3 high, 0.2 thick: closed, its volume, its materials per side.
    Wall straight;
    straight.points = {{0, 0}, {4, 0}};
    straight.sideA = "Assets/Brick.fmat";
    straight.sideB = "Assets/Plaster.fmat";
    const PolyMesh s = buildWall(straight);
    checkSolid(s, 6, 4.0 * 3.0 * 0.2, "a straight wall: six faces, closed, its volume");
    bool sides = false;
    for (uint32_t f = 0; f < s.faces.size(); ++f)
        sides |= s.materials[s.faces[f].material] == "Assets/Plaster.fmat" && std::fabs(s.faceNormal(f).z) > 0.99f;
    check(sides, "each side its own material");
    // An L (4 m then 3 m, centred): mitred, the centre line's length times height and thickness.
    Wall corner;
    corner.points = {{0, 0}, {4, 0}, {4, 3}};
    checkSolid(buildWall(corner), 10, 7.0 * 3.0 * 0.2, "an L of two walls, mitred where they meet");
    // Aligned to one face: the same volume, its path on that face.
    corner.align = 1;
    const PolyMesh aligned = buildWall(corner);
    checkSolid(aligned, 10, 0.0, "aligned to its left face");
    // A room: four walls round a 4 m square, then with a floor and a ceiling.
    Wall room;
    room.points = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    room.closed = true;
    checkSolid(buildWall(room), 16, 16.0 * 3.0 * 0.2, "a room's walls: closed round, mitred at every corner");
    room.floor = room.ceiling = true;
    room.floorMaterial = "Assets/Tiles.fmat";
    const PolyMesh full = buildWall(room);
    // (the floor's top 5 mm above the base: off the plane of a ground the room stands on)
    checkSolid(full, 16 + 6 + 6, 16.0 * 3.0 * 0.2 + 3.8 * 3.8 * (0.2 + 0.005) + 3.8 * 3.8 * 0.2,
               "with a floor and a ceiling slab inside");
    bool tiles = false;
    for (uint32_t f = 0; f < full.faces.size(); ++f)
        tiles |= full.materials[full.faces[f].material] == "Assets/Tiles.fmat" && full.faceNormal(f).y > 0.99f;
    check(tiles, "the floor's top its material");
    // The path: a point and direction along it, the nearest distance to a point.
    Wall path;
    path.points = {{0, 0}, {4, 0}, {4, 3}};
    fx::float2 at, dir;
    check(wallPathAt(path, 5.0f, at, dir) && near2(at, {4, 1}) && near2(dir, {0, 1}), "a point 5 m along the L");
    check(std::fabs(wallLength(path) - 7.0f) < 1e-5f && std::fabs(wallNearest(path, {4.5f, 2}) - 6.0f) < 1e-4f,
          "its length, and the distance along it nearest a point");
    // A door cut through the straight wall (it is a solid): its volume gone.
    Cut door;
    door.size[0] = 1.0f, door.size[1] = 2.2f, door.size[2] = 1.0f;
    door.world[0][3] = 2.0f, door.world[1][3] = 0.5f;
    CutResult r;
    const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    check(cutMesh(s, identity, {door}, r) && r.applied == 1 &&
              std::fabs(topologyOf(r.mesh).volume - (2.4 - 1.0 * 1.6 * 0.2)) < 1e-4,
          "a door cut through it (from below its foot: 1.6 m of it in the wall)");
    // Roofs over a 4 m square (or 6 x 4 m), pitched 45 degrees, no overhang: closed, their volumes.
    fx::level::Roof roof;
    roof.outline = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    roof.pitch = 45.0f;
    roof.overhang = 0.0f;
    roof.base = 3.0f;
    roof.material = "Assets/Tiles.fmat";
    roof.style = fx::level::RoofStyle::Flat;
    checkSolid(fx::level::buildRoof(roof), 6, 4.0 * 4.0 * 0.2, "a flat roof: a slab");
    roof.style = fx::level::RoofStyle::Shed;
    checkSolid(fx::level::buildRoof(roof), 5, 0.5 * 4.0 * 4.0 * 4.0, "a shed roof: a wedge, 4 m up across 4 m");
    roof.style = fx::level::RoofStyle::Gable;
    const PolyMesh gable = fx::level::buildRoof(roof);
    checkSolid(gable, 5, 0.5 * 4.0 * 2.0 * 4.0, "a gable roof: a ridge 2 m up, gable ends");
    fx::float3 lo, hi;
    gable.bounds(lo, hi);
    check(std::fabs(lo.y - 3.0f) < 1e-5f && std::fabs(hi.y - 5.0f) < 1e-5f, "its eaves at the base, its ridge 2 m above");
    bool slopes = false;
    for (uint32_t f = 0; f < gable.faces.size(); ++f)
        slopes |= gable.materials[gable.faces[f].material] == "Assets/Tiles.fmat" && gable.faceNormal(f).y > 0.6f && gable.faceNormal(f).y < 0.8f;
    check(slopes, "its slopes have its material, at 45 degrees");
    roof.style = fx::level::RoofStyle::Pyramid;
    checkSolid(fx::level::buildRoof(roof), 5, 4.0 * 4.0 * 2.0 / 3.0, "a pyramid roof");
    roof.style = fx::level::RoofStyle::Hip;
    checkSolid(fx::level::buildRoof(roof), 5, 4.0 * 4.0 * 2.0 / 3.0, "a hip roof over a square: a pyramid");
    roof.outline = {{0, 0}, {6, 0}, {6, 4}, {0, 4}};
    checkSolid(fx::level::buildRoof(roof), 5, 4.0 * 2.0 / 6.0 * (3.0 * 6.0 - 4.0), "a hip roof over 6 x 4 m: its ridge 2 m long");
    // Turned 30 degrees (its ridge along the longest side still), and the other way round.
    const float c = std::cos(0.5236f), s30 = std::sin(0.5236f);
    std::vector<fx::float2> turned;
    for (const fx::float2 p : roof.outline)
        turned.push_back({p.x * c - p.y * s30, p.x * s30 + p.y * c});
    std::reverse(turned.begin(), turned.end());
    roof.outline = turned;
    checkSolid(fx::level::buildRoof(roof), 5, 4.0 * 2.0 / 6.0 * (3.0 * 6.0 - 4.0), "turned and wound the other way: the same");
    // A flat roof over an L (concave), and grown by an overhang.
    roof.style = fx::level::RoofStyle::Flat;
    roof.outline = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    checkSolid(fx::level::buildRoof(roof), 8, 3.0 * 0.2, "a flat roof over an L");
    roof.overhang = 0.5f;
    checkSolid(fx::level::buildRoof(roof), 8, (3.0 * 3.0 - 1.0 * 1.0) * 0.2, "grown by its overhang (mitred corners)");
    roof.outline.resize(2);
    check(fx::level::buildRoof(roof).empty(), "two points: no roof");
    return g_failures;
}

// The mask's value at a face's point (its mask UVs at barycentrics of its first fan triangle).
float maskAt(const fx::level::PolyMesh& m, const std::vector<uint8_t>& mask, uint32_t face, fx::float3 bary, uint32_t channel)
{
    const fx::level::Face& f = m.faces[face];
    const fx::float2 a = m.cornerUv1[f.first], b = m.cornerUv1[f.first + 1], c = m.cornerUv1[f.first + 2];
    const float u = a.x * bary.x + b.x * bary.y + c.x * bary.z, v = a.y * bary.x + b.y * bary.y + c.y * bary.z;
    const uint32_t x = std::min(uint32_t(u * fx::level::kMaskSize), fx::level::kMaskSize - 1), y = std::min(uint32_t(v * fx::level::kMaskSize), fx::level::kMaskSize - 1);
    return float(mask[(size_t(y) * fx::level::kMaskSize + x) * 4 + channel]) / 255.0f;
}

double maskMean(const std::vector<uint8_t>& mask, uint32_t channel)
{
    double sum = 0.0;
    for (size_t i = channel; i < mask.size(); i += 4)
        sum += mask[i];
    return sum / 255.0 / double(mask.size() / 4);
}

int testWear()
{
    using namespace fx::level;
    using fx::Json;
    // A 2 m box with layers (its mask UVs): edge wear into layer 1 along all twelve edges.
    const PolyMesh box = op("layers", PolyMesh::box({2, 2, 2}), R"({"materials":["Assets/Metal.fmat","",""]})").mesh;
    std::vector<uint8_t> mask;
    WearSettings edges;
    edges.width = 0.15f;
    check(generateWear(box, edges, mask) && mask.size() == kMaskBytes, "edge wear made into the mask");
    const uint32_t top = topFace(box);
    // (the first fan triangle of a quad: corners 0, 1, 2; near corner 0, and its middle)
    const float corner = maskAt(box, mask, top, {0.97f, 0.015f, 0.015f}, 0), middle = maskAt(box, mask, top, {0.25f, 0.5f, 0.25f}, 0);
    std::printf("     edge wear: %.2f at a corner, %.2f mid-face, mean %.3f\n", corner, middle, maskMean(mask, 0));
    check(corner > 0.5f && middle < 0.05f, "worn at its edges, clean in the middle of its faces");
    check(maskMean(mask, 0) > 0.02 && maskMean(mask, 0) < 0.5 && maskMean(mask, 1) == 0.0, "only along the edges, only its channel");
    // Painted values kept where they are higher (wear is the higher of the two).
    std::vector<uint8_t> painted(kMaskBytes, 0);
    for (size_t i = 0; i < painted.size(); i += 4)
        painted[i] = 200;
    generateWear(box, edges, painted);
    check(maskMean(painted, 0) > 200.0 / 255.0 - 1e-6, "painting kept where it is higher");
    // Grime: its foot (the bottom face, the sides' lowest 0.3 m), none at the top.
    WearSettings grime;
    grime.kind = WearKind::Grime;
    grime.channel = 1;
    grime.foot = 0.3f;
    std::vector<uint8_t> dirt;
    check(generateWear(box, grime, dirt), "grime made");
    uint32_t bottom = 0;
    for (uint32_t f = 0; f < box.faces.size(); ++f)
        if (box.faceNormal(f).y < -0.99f)
            bottom = f;
    const float underneath = maskAt(box, dirt, bottom, {0.34f, 0.33f, 0.33f}, 1), above = maskAt(box, dirt, top, {0.34f, 0.33f, 0.33f}, 1);
    std::printf("     grime: %.2f underneath, %.2f on top\n", underneath, above);
    check(underneath > 0.4f && above < 0.05f, "grime at its foot, none on top");
    // An L of walls: grime in its inside corner (a concave edge).
    Wall l;
    l.points = {{0, 0}, {4, 0}, {4, 4}};
    PolyMesh wall = buildWall(l);
    wall = op("layers", wall, R"({"materials":["Assets/Dirt.fmat","",""]})").mesh;
    WearSettings corners;
    corners.kind = WearKind::Grime;
    corners.foot = 0.0f;
    corners.width = 0.2f;
    std::vector<uint8_t> inside;
    check(generateWear(wall, corners, inside) && maskMean(inside, 0) > 0.0, "grime in a wall's inside corner");
    // Grime where another surface comes near: a floor under the box (none at its foot, no concave
    // edges of its own), the sides' lowest `width` dirty, the top clean.
    WearSettings contact;
    contact.kind = WearKind::Grime;
    contact.foot = 0.0f;
    contact.width = 0.3f;
    std::vector<uint8_t> alone, onFloor;
    const std::vector<fx::float3> floor = {{-5, -1, -5}, {-5, -1, 5}, {5, -1, 5}, {-5, -1, -5}, {5, -1, 5}, {5, -1, -5}};
    generateWear(box, contact, alone);
    check(generateWear(box, contact, onFloor, &floor), "grime made with a floor near");
    std::printf("     grime by a floor: mean %.3f (alone %.3f), %.2f underneath, %.2f on top\n", maskMean(onFloor, 0), maskMean(alone, 0),
                maskAt(box, onFloor, bottom, {0.34f, 0.33f, 0.33f}, 0), maskAt(box, onFloor, top, {0.34f, 0.33f, 0.33f}, 0));
    check(maskMean(alone, 0) == 0.0 && maskMean(onFloor, 0) > 0.02 && maskAt(box, onFloor, top, {0.34f, 0.33f, 0.33f}, 0) < 0.01f,
          "grime where the floor meets it, none on top");
    // Streaks: down the sides from the top's edges (a ledge's lip), within `width`; none on the top
    // or the bottom, none farther down; another seed, other streaks.
    WearSettings streaks;
    streaks.kind = WearKind::Streaks;
    streaks.channel = 2;
    streaks.width = 1.0f;
    std::vector<uint8_t> rain, rain2;
    check(generateWear(box, streaks, rain), "streaks made");
    streaks.seed = 9;
    generateWear(box, streaks, rain2);
    uint32_t sideFace = 0;
    for (uint32_t f = 0; f < box.faces.size(); ++f)
        if (std::fabs(box.faceNormal(f).y) < 0.1f)
            sideFace = f;
    // (the side's lowest metre: below where they reach)
    float lowest = 0.0f;
    for (float a = 0.02f; a < 0.98f; a += 0.04f)
        for (float b = 0.02f; a + b < 0.98f; b += 0.04f) {
            const Face& face = box.faces[sideFace];
            const fx::float3 p0 = box.positions[box.corners[face.first]], p1 = box.positions[box.corners[face.first + 1]],
                         p2 = box.positions[box.corners[face.first + 2]];
            if ((p0 * (1 - a - b) + p1 * a + p2 * b).y < -0.05f)
                lowest = std::max(lowest, maskAt(box, rain, sideFace, {1 - a - b, a, b}, 2));
        }
    std::printf("     streaks: mean %.3f (seed 9: %.3f), %.2f at most in the lowest metre\n", maskMean(rain, 2), maskMean(rain2, 2), lowest);
    check(maskMean(rain, 2) > 0.005 && maskMean(rain, 0) == 0.0 && maskAt(box, rain, top, {0.34f, 0.33f, 0.33f}, 2) == 0.0f &&
              maskAt(box, rain, bottom, {0.34f, 0.33f, 0.33f}, 2) == 0.0f && lowest == 0.0f,
          "streaks down the sides from the top, within their length");
    check(rain != rain2, "another seed: other streaks");
    // Rules as JSON and back.
    WearSettings back = wearFromJson(Json::parse(R"({"kind":"streaks","layer":2,"width":1,"seed":9})"), WearKind::Edges);
    streaks.foot = 0.0f;  // (grime's only: none read for the others)
    check(back == streaks, "a rule read back from JSON");
    // Without mask UVs: refused.
    std::vector<uint8_t> none;
    check(!generateWear(PolyMesh::box({1, 1, 1}), edges, none), "no mask UVs: nothing made");
    // A round bevel (8 faces across the box's top front edge, 15 cm): its new faces get mask UVs in
    // the free space (the faces it left alone keep theirs, the new charts on none of the old ones),
    // wear all over the round, chips along its middle; the Bevel modifier's faces get theirs too.
    {
        const PolyMesh round = op("bevel", box, (R"({"edges":[)" + std::to_string(topFrontEdge(box)) + R"(],"amount":0.15,"segments":8,"curve":1})").c_str()).mesh;
        const auto uvArea = [](const PolyMesh& m, uint32_t f) {
            const Face& face = m.faces[f];
            float a = 0.0f;
            for (uint32_t k = 1; k + 1 < face.count; ++k) {
                const fx::float2 p = m.cornerUv1[face.first], q = m.cornerUv1[face.first + k], r = m.cornerUv1[face.first + k + 1];
                a += std::fabs((q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y)) * 0.5f;
            }
            return a;
        };
        const auto uvInside = [](fx::float2 p, fx::float2 a, fx::float2 b, fx::float2 c) {
            const float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y), d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y),
                        d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
            return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
        };
        bool all = round.cornerUv1.size() == round.corners.size(), apart = true;
        uint32_t middle = ~0u, made = 0;
        for (uint32_t f = 0; f < round.faces.size(); ++f) {
            all &= uvArea(round, f) > 1e-7f;
            if (round.faces[f].smoothing == 0)
                continue;
            ++made;
            const fx::float3 n = round.faceNormal(f);
            if (middle == ~0u || n.y * 0.7071f + n.z * 0.7071f > round.faceNormal(middle).y * 0.7071f + round.faceNormal(middle).z * 0.7071f)
                middle = f;
            const Face& face = round.faces[f];
            fx::float2 c = {0, 0};
            for (uint32_t k = 0; k < face.count; ++k)
                c = {c.x + round.cornerUv1[face.first + k].x / float(face.count), c.y + round.cornerUv1[face.first + k].y / float(face.count)};
            for (uint32_t g = 0; g < box.faces.size(); ++g)
                for (uint32_t k = 1; k + 1 < box.faces[g].count; ++k)
                    apart &= !uvInside(c, box.cornerUv1[box.faces[g].first], box.cornerUv1[box.faces[g].first + k], box.cornerUv1[box.faces[g].first + k + 1]);
        }
        uint32_t bottomRound = 0;
        for (uint32_t f = 0; f < round.faces.size(); ++f)
            if (round.faceNormal(f).y < -0.99f)
                bottomRound = f;
        bool kept = true;
        for (uint32_t k = 0; k < 4; ++k) {
            const fx::float2 a = round.cornerUv1[round.faces[bottomRound].first + k];
            bool found = false;
            for (uint32_t i = 0; i < 4; ++i) {
                const fx::float2 b = box.cornerUv1[box.faces[bottom].first + i];
                found |= a.x == b.x && a.y == b.y;
            }
            kept &= found;
        }
        check(all && made == 8 && apart && kept, "a round bevel's faces: mask UVs of their own, apart from the others' (kept)");
        std::vector<uint8_t> worn;
        WearSettings rounded = edges;
        check(generateWear(round, rounded, worn), "edge wear on a round bevel");
        float lowest = 1.0f;
        for (uint32_t f = 0; f < round.faces.size(); ++f)
            if (round.faces[f].smoothing != 0)
                lowest = std::min(lowest, maskAt(round, worn, f, {0.34f, 0.33f, 0.33f}, 0));
        const float onTop = maskAt(round, worn, topFace(round), {0.34f, 0.33f, 0.33f}, 0);
        std::printf("     round bevel wear: %.2f at least across it, %.2f mid-top\n", lowest, onTop);
        check(lowest > 0.3f, "worn all across the round (its faces turn less than 30 degrees each)");
        // Chips: along its middle (the arc's at y = z = 0.85 + 0.15 / sqrt 2), 25 cm apart.
        const float identityRows[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        const auto spots = damageSpots(round, identityRows, 0, {}, 0.25f, 1);
        uint32_t along = 0;
        for (const DamageSpotAt& d : spots)
            along += std::fabs(d.at.y - 0.956f) < 0.04f && std::fabs(d.at.z - 0.956f) < 0.04f && d.normal.y > 0.5f && d.normal.z > 0.5f;
        std::printf("     round bevel chips: %u along it (%zu in all)\n", along, spots.size());
        check(along >= 6 && along <= 8, "damage over the mesh: chips along the round's middle");
        fx::float3 spot;
        check(damageSpot(round, middle, round.faceCenter(middle), DamageSnap::Chip, spot), "a chip on a round bevel's face: onto one of its edges");
        Modifier bevelRound = makeModifier(ModifierKind::Bevel);
        bevelRound.segments = 4;
        PolyMesh modified;
        check(applyModifiers(box, {bevelRound}, modified), "the Bevel modifier on a box with layers");
        bool every = modified.cornerUv1.size() == modified.corners.size();
        for (uint32_t f = 0; f < modified.faces.size(); ++f)
            every &= uvArea(modified, f) > 1e-8f;
        check(every, "the Bevel modifier's faces: mask UVs of their own");
    }
    // A smart material: its base on every face, its layers, its wear and grime made.
    SmartMaterial smart;
    std::string error;
    check(parseSmartMaterial(R"({"name":"Chipped","base":"Assets/Paint.fmat","layers":["Assets/Metal.fmat","Assets/Dirt.fmat",""],
                                 "wear":{"layer":0,"width":0.1},"grime":{"layer":1,"foot":0.3}})",
                             smart, error) && smart.wear && smart.grime && smart.grime->channel == 1,
          "a smart material read");
    PolyMesh plain = PolyMesh::box({2, 2, 2});
    std::vector<uint8_t> smartMask;
    check(applySmartMaterial(smart, plain, smartMask, error) && plain.layered() && plain.layerMaterials[1] == "Assets/Dirt.fmat",
          "applied: its layers");
    bool based = true;
    for (const Face& f : plain.faces)
        based &= plain.materials[f.material] == "Assets/Paint.fmat";
    check(based && maskMean(smartMask, 0) > 0.02 && maskMean(smartMask, 1) > 0.02, "its base on every face, its wear and grime made");
    return g_failures;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: falcon_level_test <tangents|boolean|unwrap|navmesh|polymesh|meshops|uvops|trims|layers|cuts|walls|wear|modifiers|splines|navigation>\n");
        return 2;
    }
    const char* t = argv[1];
    if (!std::strcmp(t, "tangents"))
        return testTangents();
    if (!std::strcmp(t, "boolean"))
        return testBoolean();
    if (!std::strcmp(t, "unwrap"))
        return testUnwrap();
    if (!std::strcmp(t, "navmesh"))
        return testNavmesh();
    if (!std::strcmp(t, "polymesh"))
        return testPolyMesh();
    if (!std::strcmp(t, "meshops"))
        return testMeshOps();
    if (!std::strcmp(t, "uvops"))
        return testUvOps();
    if (!std::strcmp(t, "trims"))
        return testTrims();
    if (!std::strcmp(t, "layers"))
        return testLayers();
    if (!std::strcmp(t, "cuts"))
        return testCuts();
    if (!std::strcmp(t, "walls"))
        return testWalls();
    if (!std::strcmp(t, "wear"))
        return testWear();
    if (!std::strcmp(t, "modifiers"))
        return testModifiers();
    if (!std::strcmp(t, "splines"))
        return testSplines();
    if (!std::strcmp(t, "navigation"))
        return testNavigation();
    std::fprintf(stderr, "falcon_level_test: no test '%s'\n", t);
    return 2;
}
