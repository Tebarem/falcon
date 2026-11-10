#include "level/navmesh.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fx::level {

namespace {

constexpr uint32_t kMagic = 0x56414e46u;  // "FNAV"
constexpr uint32_t kVersion = 2;  // (2: the areas' costs)
constexpr uint16_t kWalkFlag = 1;
constexpr unsigned char kFirstCostArea = 2;  // (cost areas: 2.., one a volume; 63 the walkable ground)

struct FileHeader {
    uint32_t magic, version, tiles, reserved;
    dtNavMeshParams params;
    NavSettings settings;
    float areaCost[64];
};

// The filter every query uses: walkable polygons, each area at its cost.
void filterFor(dtQueryFilter& filter, const float costs[64])
{
    filter.setIncludeFlags(kWalkFlag);
    for (int a = 0; a < 64; ++a)
        filter.setAreaCost(a, costs[a] > 0.0f ? costs[a] : 1.0f);
}
struct TileHeader {
    int32_t x, y;
    uint32_t size, reserved;
};

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

uint32_t log2u(uint32_t v)
{
    uint32_t r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}

// The volumes into the walkable cells: blockers none, cost areas their own area (2.., in order).
bool markVolumes(rcContext& ctx, const NavInput& in, rcCompactHeightfield& chf)
{
    unsigned char next = kFirstCostArea;
    for (const NavVolume& v : in.volumes) {
        if (v.outline.size() < 3)
            continue;
        std::vector<float> verts;
        for (const float3& p : v.outline)
            verts.insert(verts.end(), {p.x, p.y, p.z});
        const unsigned char area = v.blocks ? RC_NULL_AREA : next;
        if (!v.blocks && next < 62)
            ++next;
        rcMarkConvexPolyArea(&ctx, verts.data(), int(v.outline.size()), v.bottom, v.top, area, chf);
    }
    return true;
}

// One tile's navigation data (dtAlloc'd: the mesh frees it), from the triangles over it.
unsigned char* buildTile(rcContext& ctx, const NavInput& in, const NavSettings& s, const float tileMin[3], const float tileMax[3], int tx, int ty,
                         int& size)
{
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.maxSlope;
    cfg.walkableHeight = int(std::ceil(s.agentHeight / s.cellHeight));
    cfg.walkableClimb = int(std::floor(s.agentClimb / s.cellHeight));
    cfg.walkableRadius = int(std::ceil(s.agentRadius / s.cellSize));
    cfg.maxEdgeLen = int(12.0f / s.cellSize);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = int(s.tileSize);
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.cellSize * 6.0f;
    cfg.detailSampleMaxError = s.cellHeight;
    rcVcopy(cfg.bmin, tileMin);
    rcVcopy(cfg.bmax, tileMax);
    cfg.bmin[0] -= float(cfg.borderSize) * cfg.cs;
    cfg.bmin[2] -= float(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] += float(cfg.borderSize) * cfg.cs;
    cfg.bmax[2] += float(cfg.borderSize) * cfg.cs;

    // The triangles that reach it (with its border), walkable by their slope where they may be.
    std::vector<int> tris;
    std::vector<unsigned char> areas;
    const size_t triCount = in.indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const float3 a = in.vertices[in.indices[t * 3]], b = in.vertices[in.indices[t * 3 + 1]], c = in.vertices[in.indices[t * 3 + 2]];
        if (std::max({a.x, b.x, c.x}) < cfg.bmin[0] || std::min({a.x, b.x, c.x}) > cfg.bmax[0] || std::max({a.z, b.z, c.z}) < cfg.bmin[2] ||
            std::min({a.z, b.z, c.z}) > cfg.bmax[2])
            continue;
        tris.insert(tris.end(), {int(in.indices[t * 3]), int(in.indices[t * 3 + 1]), int(in.indices[t * 3 + 2])});
        areas.push_back(in.walkable.empty() || in.walkable[t] ? 1 : 0);
    }
    if (tris.empty())
        return nullptr;
    const int nt = int(tris.size() / 3);
    std::vector<unsigned char> slopeOk(size_t(nt), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, &in.vertices[0].x, int(in.vertices.size()), tris.data(), nt, slopeOk.data());
    for (int t = 0; t < nt; ++t)
        areas[size_t(t)] = areas[size_t(t)] && slopeOk[size_t(t)] ? RC_WALKABLE_AREA : RC_NULL_AREA;

    struct Owned {
        rcHeightfield* hf = rcAllocHeightfield();
        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        rcContourSet* cset = rcAllocContourSet();
        rcPolyMesh* pmesh = rcAllocPolyMesh();
        rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
        ~Owned()
        {
            rcFreeHeightField(hf);
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
        }
    } o;
    if (!rcCreateHeightfield(&ctx, *o.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch) ||
        !rcRasterizeTriangles(&ctx, &in.vertices[0].x, int(in.vertices.size()), tris.data(), areas.data(), nt, *o.hf, cfg.walkableClimb))
        return nullptr;
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *o.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *o.hf);
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.hf, *o.chf) || !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *o.chf) ||
        !markVolumes(ctx, in, *o.chf) || !rcBuildDistanceField(&ctx, *o.chf) ||
        !rcBuildRegions(&ctx, *o.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea) ||
        !rcBuildContours(&ctx, *o.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *o.cset) ||
        !rcBuildPolyMesh(&ctx, *o.cset, cfg.maxVertsPerPoly, *o.pmesh) ||
        !rcBuildPolyMeshDetail(&ctx, *o.pmesh, *o.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *o.dmesh))
        return nullptr;
    if (o.pmesh->npolys == 0)
        return nullptr;
    for (int i = 0; i < o.pmesh->npolys; ++i)
        o.pmesh->flags[i] = o.pmesh->areas[i] != RC_NULL_AREA ? kWalkFlag : 0;

    dtNavMeshCreateParams p{};
    p.verts = o.pmesh->verts;
    p.vertCount = o.pmesh->nverts;
    p.polys = o.pmesh->polys;
    p.polyAreas = o.pmesh->areas;
    p.polyFlags = o.pmesh->flags;
    p.polyCount = o.pmesh->npolys;
    p.nvp = o.pmesh->nvp;
    p.detailMeshes = o.dmesh->meshes;
    p.detailVerts = o.dmesh->verts;
    p.detailVertsCount = o.dmesh->nverts;
    p.detailTris = o.dmesh->tris;
    p.detailTriCount = o.dmesh->ntris;
    p.walkableHeight = s.agentHeight;
    p.walkableRadius = s.agentRadius;
    p.walkableClimb = s.agentClimb;
    p.tileX = tx;
    p.tileY = ty;
    p.tileLayer = 0;
    rcVcopy(p.bmin, o.pmesh->bmin);
    rcVcopy(p.bmax, o.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    if (!dtCreateNavMeshData(&p, &data, &size))
        return nullptr;
    return data;
}

}  // namespace

NavMesh::NavMesh() = default;

NavMesh::~NavMesh() { clear(); }

void NavMesh::clear()
{
    dtFreeNavMeshQuery(query_);
    dtFreeNavMesh(nav_);
    query_ = nullptr;
    nav_ = nullptr;
}

bool NavMesh::build(const NavInput& in, const NavSettings& s, std::string* error)
{
    clear();
    settings_ = s;
    for (float& c : areaCost_)
        c = 1.0f;
    unsigned char next = kFirstCostArea;
    for (const NavVolume& v : in.volumes)
        if (!v.blocks && v.outline.size() >= 3 && next < 62)
            areaCost_[next++] = std::max(v.cost, 0.01f);
    const auto fail = [&](const char* why) {
        clear();
        if (error)
            *error = why;
        return false;
    };
    if (in.indices.size() < 3 || in.vertices.empty())
        return fail("nothing to walk on");
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const float3& v : in.vertices)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], (&v.x)[a]);
            hi[a] = std::max(hi[a], (&v.x)[a]);
        }
    const float tileWorld = float(s.tileSize) * s.cellSize;
    const int tw = std::max(1, int(std::ceil((hi[0] - lo[0]) / tileWorld))), th = std::max(1, int(std::ceil((hi[2] - lo[2]) / tileWorld)));
    if (int64_t(tw) * th > 16384)
        return fail("too big for its tiles (raise the tile size or the cell size)");
    const uint32_t tileBits = std::min(log2u(nextPow2(uint32_t(tw * th))), 14u), polyBits = 22 - tileBits;
    dtNavMeshParams params{};
    rcVcopy(params.orig, lo);
    params.tileWidth = tileWorld;
    params.tileHeight = tileWorld;
    params.maxTiles = int(1u << tileBits);
    params.maxPolys = int(1u << polyBits);
    nav_ = dtAllocNavMesh();
    if (!nav_ || dtStatusFailed(nav_->init(&params)))
        return fail("navigation mesh not made");
    rcContext ctx(false);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float tileMin[3] = {lo[0] + float(x) * tileWorld, lo[1], lo[2] + float(y) * tileWorld};
            const float tileMax[3] = {lo[0] + float(x + 1) * tileWorld, hi[1], lo[2] + float(y + 1) * tileWorld};
            int size = 0;
            if (unsigned char* data = buildTile(ctx, in, s, tileMin, tileMax, x, y, size))
                if (dtStatusFailed(nav_->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr)))
                    dtFree(data);
        }
    if (polygonCount() == 0)
        return fail("nothing walkable found");
    query_ = dtAllocNavMeshQuery();
    if (!query_ || dtStatusFailed(query_->init(nav_, 4096)))
        return fail("navigation queries not made");
    return true;
}

std::vector<uint8_t> NavMesh::save() const
{
    std::vector<uint8_t> out;
    if (!nav_)
        return out;
    FileHeader h{kMagic, kVersion, 0, 0, *nav_->getParams(), settings_, {}};
    std::memcpy(h.areaCost, areaCost_, sizeof(areaCost_));
    const dtNavMesh* nav = nav_;
    for (int i = 0; i < nav->getMaxTiles(); ++i)
        if (const dtMeshTile* t = nav->getTile(i); t && t->header && t->dataSize)
            ++h.tiles;
    out.resize(sizeof(h));
    std::memcpy(out.data(), &h, sizeof(h));
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* t = nav->getTile(i);
        if (!t || !t->header || !t->dataSize)
            continue;
        const TileHeader th{t->header->x, t->header->y, uint32_t(t->dataSize), 0};
        const size_t at = out.size();
        out.resize(at + sizeof(th) + size_t(t->dataSize));
        std::memcpy(out.data() + at, &th, sizeof(th));
        std::memcpy(out.data() + at + sizeof(th), t->data, size_t(t->dataSize));
    }
    return out;
}

bool NavMesh::load(const uint8_t* data, size_t size)
{
    clear();
    FileHeader h;
    if (size < sizeof(h))
        return false;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion)
        return false;
    settings_ = h.settings;
    std::memcpy(areaCost_, h.areaCost, sizeof(areaCost_));
    nav_ = dtAllocNavMesh();
    if (!nav_ || dtStatusFailed(nav_->init(&h.params))) {
        clear();
        return false;
    }
    size_t at = sizeof(h);
    for (uint32_t i = 0; i < h.tiles; ++i) {
        TileHeader th;
        if (at + sizeof(th) > size)
            break;
        std::memcpy(&th, data + at, sizeof(th));
        at += sizeof(th);
        if (at + th.size > size)
            break;
        auto* tile = static_cast<unsigned char*>(dtAlloc(int(th.size), DT_ALLOC_PERM));
        std::memcpy(tile, data + at, th.size);
        at += th.size;
        if (dtStatusFailed(nav_->addTile(tile, int(th.size), DT_TILE_FREE_DATA, 0, nullptr)))
            dtFree(tile);
    }
    query_ = dtAllocNavMeshQuery();
    if (!query_ || dtStatusFailed(query_->init(nav_, 4096))) {
        clear();
        return false;
    }
    return true;
}

bool NavMesh::nearest(float3 p, float3& out, float3 extents) const
{
    if (!query_)
        return false;
    dtQueryFilter filter;
    filterFor(filter, areaCost_);
    dtPolyRef ref = 0;
    float at[3];
    if (dtStatusFailed(query_->findNearestPoly(&p.x, &extents.x, &filter, &ref, at)) || !ref)
        return false;
    out = {at[0], at[1], at[2]};
    return true;
}

bool NavMesh::findPath(float3 from, float3 to, std::vector<float3>& path) const
{
    path.clear();
    if (!query_)
        return false;
    dtQueryFilter filter;
    filterFor(filter, areaCost_);
    const float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef a = 0, b = 0;
    float pa[3], pb[3];
    if (dtStatusFailed(query_->findNearestPoly(&from.x, extents, &filter, &a, pa)) || !a ||
        dtStatusFailed(query_->findNearestPoly(&to.x, extents, &filter, &b, pb)) || !b)
        return false;
    dtPolyRef polys[512];
    int count = 0;
    const dtStatus status = query_->findPath(a, b, pa, pb, &filter, polys, &count, 512);
    if (dtStatusFailed(status) || count == 0)
        return false;
    // (a partial path: as near as it gets; the end on its last polygon)
    float end[3] = {pb[0], pb[1], pb[2]};
    if (polys[count - 1] != b)
        query_->closestPointOnPoly(polys[count - 1], pb, end, nullptr);
    float corners[256 * 3];
    int n = 0;
    query_->findStraightPath(pa, end, polys, count, corners, nullptr, nullptr, &n, 256);
    for (int i = 0; i < n; ++i)
        path.push_back({corners[i * 3], corners[i * 3 + 1], corners[i * 3 + 2]});
    return polys[count - 1] == b && !dtStatusDetail(status, DT_PARTIAL_RESULT);
}

bool NavMesh::raycast(float3 from, float3 to, float& t, float3& hit) const
{
    t = 1.0f;
    hit = to;
    if (!query_)
        return false;
    dtQueryFilter filter;
    filterFor(filter, areaCost_);
    const float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef a = 0;
    float pa[3];
    if (dtStatusFailed(query_->findNearestPoly(&from.x, extents, &filter, &a, pa)) || !a)
        return false;
    float normal[3];
    dtPolyRef polys[256];
    int count = 0;
    if (dtStatusFailed(query_->raycast(a, pa, &to.x, &filter, &t, normal, polys, &count, 256)))
        return false;
    if (t > 1.0f) {  // (FLT_MAX: it got there)
        t = 1.0f;
        return false;
    }
    hit = {pa[0] + (to.x - pa[0]) * t, pa[1] + (to.y - pa[1]) * t, pa[2] + (to.z - pa[2]) * t};
    return true;
}

uint32_t NavMesh::tileCount() const
{
    uint32_t n = 0;
    if (const dtNavMesh* nav = nav_)
        for (int i = 0; i < nav->getMaxTiles(); ++i)
            if (const dtMeshTile* t = nav->getTile(i); t && t->header)
                ++n;
    return n;
}

uint32_t NavMesh::polygonCount() const
{
    uint32_t n = 0;
    if (const dtNavMesh* nav = nav_)
        for (int i = 0; i < nav->getMaxTiles(); ++i)
            if (const dtMeshTile* t = nav->getTile(i); t && t->header)
                n += uint32_t(t->header->polyCount);
    return n;
}

namespace {
std::shared_ptr<const NavMesh> g_navigation;
std::unique_ptr<NavCrowd> g_crowd;
uint32_t g_crowdGeneration = 0;
constexpr int32_t kMaxAgents = 512;
}  // namespace

std::shared_ptr<const NavMesh> navigation() { return g_navigation; }

void setNavigation(std::shared_ptr<const NavMesh> nav)
{
    g_navigation = std::move(nav);
    g_crowd.reset();  // (its agents were on the old mesh: they join the new one's crowd again)
}

NavCrowd* navCrowd()
{
    if (!g_navigation)
        return nullptr;
    if (!g_crowd || g_crowd->mesh() != g_navigation.get())
        g_crowd = std::make_unique<NavCrowd>(g_navigation, ++g_crowdGeneration & 0x7fffu);
    return g_crowd.get();
}

NavCrowd::NavCrowd(std::shared_ptr<const NavMesh> nav, uint32_t generation) : nav_(std::move(nav)), generation_(generation)
{
    crowd_ = dtAllocCrowd();
    if (!crowd_ || !nav_ || !nav_->detour() || !crowd_->init(kMaxAgents, 2.0f, nav_->detour())) {
        dtFreeCrowd(crowd_);
        crowd_ = nullptr;
        return;
    }
    dtQueryFilter* filter = crowd_->getEditableFilter(0);
    filter->setIncludeFlags(kWalkFlag);
    for (int a = 0; a < 64; ++a)
        filter->setAreaCost(a, nav_->areaCosts()[a] > 0.0f ? nav_->areaCosts()[a] : 1.0f);
    dtObstacleAvoidanceParams avoid = *crowd_->getObstacleAvoidanceParams(0);
    avoid.adaptiveDivs = 7;
    avoid.adaptiveRings = 2;
    avoid.adaptiveDepth = 3;
    crowd_->setObstacleAvoidanceParams(3, &avoid);
}

NavCrowd::~NavCrowd() { dtFreeCrowd(crowd_); }

int32_t NavCrowd::index(int32_t id) const
{
    if (!crowd_ || id < 0 || uint32_t(id) >> 16 != generation_)
        return -1;
    const int32_t i = id & 0xffff;
    const dtCrowdAgent* a = crowd_->getAgent(i);
    return a && a->active ? i : -1;
}

int32_t NavCrowd::add(float3 position, float radius, float height, float speed, float acceleration)
{
    if (!crowd_)
        return -1;
    dtCrowdAgentParams p{};
    p.radius = std::clamp(radius, 0.05f, 2.0f);
    p.height = std::max(height, 0.1f);
    p.maxAcceleration = std::max(acceleration, 0.1f);
    p.maxSpeed = std::max(speed, 0.0f);
    p.collisionQueryRange = p.radius * 12.0f;
    p.pathOptimizationRange = p.radius * 30.0f;
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    p.obstacleAvoidanceType = 3;
    p.separationWeight = 2.0f;
    p.queryFilterType = 0;
    const int i = crowd_->addAgent(&position.x, &p);
    return i < 0 ? -1 : int32_t(generation_ << 16 | uint32_t(i));
}

void NavCrowd::remove(int32_t id)
{
    if (const int32_t i = index(id); i >= 0)
        crowd_->removeAgent(i);
}

bool NavCrowd::target(int32_t id, float3 to)
{
    const int32_t i = index(id);
    if (i < 0)
        return false;
    dtPolyRef ref = 0;
    float at[3];
    if (dtStatusFailed(crowd_->getNavMeshQuery()->findNearestPoly(&to.x, crowd_->getQueryExtents(), crowd_->getFilter(0), &ref, at)) || !ref)
        return false;
    return crowd_->requestMoveTarget(i, ref, at);
}

void NavCrowd::stop(int32_t id)
{
    if (const int32_t i = index(id); i >= 0)
        crowd_->resetMoveTarget(i);
}

void NavCrowd::update(float dt)
{
    if (crowd_ && dt > 0.0f)
        crowd_->update(std::min(dt, 0.1f), nullptr);
}

float NavCrowd::state(int32_t id, float3& position, float3& velocity) const
{
    const int32_t i = index(id);
    if (i < 0)
        return -1.0f;
    const dtCrowdAgent* a = crowd_->getAgent(i);
    position = {a->npos[0], a->npos[1], a->npos[2]};
    velocity = {a->vel[0], a->vel[1], a->vel[2]};
    if (a->targetState == DT_CROWDAGENT_TARGET_NONE || a->targetState == DT_CROWDAGENT_TARGET_FAILED)
        return 0.0f;
    const float dx = a->targetPos[0] - a->npos[0], dz = a->targetPos[2] - a->npos[2];
    return std::sqrt(dx * dx + dz * dz);
}

std::string navMeshPathFor(const std::string& scenePath)
{
    std::string p = scenePath;
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + ".nav";
}

bool loadNavigation(const std::string& scenePath)
{
    g_navigation.reset();
    if (scenePath.empty())
        return false;
    std::ifstream f(navMeshPathFor(scenePath), std::ios::binary);
    if (!f)
        return false;
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto nav = std::make_shared<NavMesh>();
    if (!nav->load(blob.data(), blob.size()))
        return false;
    g_navigation = std::move(nav);
    return true;
}

void NavMesh::edges(std::vector<float3>& lines) const
{
    lines.clear();
    const dtNavMesh* nav = nav_;
    if (!nav)
        return;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* t = nav->getTile(i);
        if (!t || !t->header)
            continue;
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;
            for (int k = 0; k < poly.vertCount; ++k) {
                const float* a = &t->verts[poly.verts[k] * 3];
                const float* b = &t->verts[poly.verts[(k + 1) % poly.vertCount] * 3];
                lines.push_back({a[0], a[1] + 0.05f, a[2]});
                lines.push_back({b[0], b[1] + 0.05f, b[2]});
            }
        }
    }
}

}  // namespace fx::level
