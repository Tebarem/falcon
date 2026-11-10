#include "render/terrain_renderer.h"
#include "render/ray_tracing/scene_bvh.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/terrain_surface.h"
#include "render/virtual_shadow_map.h"
#include "terrain/terrain.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace fx {

using namespace terrain;

namespace {

constexpr uint32_t kSample = kPatchSamples;  // 9 vertices per side
constexpr uint32_t kPrevInstances = ~0u;     // Write::section for the previous-instance table

uint32_t octEncode(float3 n)
{
    const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float x = n.x / l1, y = n.y / l1;
    if (n.z < 0.0f) {
        const float ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    const int16_t qx = int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
    const int16_t qy = int16_t(std::lround(std::clamp(y, -1.0f, 1.0f) * 32767.0f));
    return uint32_t(uint16_t(qx)) | uint32_t(uint16_t(qy)) << 16;
}

}  // namespace

bool TerrainRenderer::init(SDL_GPUDevice* device, const ScenePak& pak, VirtualShadowMap& vsm)
{
    device_ = device;
    pak_ = &pak;
    vsm_ = &vsm;
    slots_ = pak.terrainSlots();
    stats_.capacity = slots_.patches;
    freeSlots_.clear();
    for (uint32_t s = slots_.patches; s-- > 0;)
        freeSlots_.push_back(s);
    return true;
}

void TerrainRenderer::shutdown()
{
    if (device_)
        gpumem::releaseTransferBuffer(device_, upload_);
    upload_ = nullptr;
    uploadBytes_ = 0;
    device_ = nullptr;
}

void TerrainRenderer::setTerrain(const Terrain* terrain)
{
    terrain_ = terrain;
    rewriteAll_ = true;
    selectedFrom_ = {1e30f, 1e30f, 1e30f};
}

void TerrainRenderer::Selection::setSplit(uint32_t l, uint32_t x, uint32_t z)
{
    const uint64_t i = uint64_t(z) * side[l] + x;
    uint64_t& word = bits[l][i >> 6];
    if (!((word >> (i & 63)) & 1)) {
        word |= 1ull << (i & 63);
        splits[l].push_back(key(l, x, z));
    }
}

bool TerrainRenderer::Selection::isLeaf(uint64_t k) const
{
    const uint32_t l = uint32_t(k >> 48), x = uint32_t(k & 0xffffff), z = uint32_t((k >> 24) & 0xffffff);
    if (l >= side.size() || x >= side[l] || z >= side[l] || isSplit(l, x, z))
        return false;
    return l + 1 == side.size() || isSplit(l + 1, x >> 1, z >> 1);
}

void TerrainRenderer::select(const CameraState& camera, uint32_t displayHeight, float pixels, float detailDistance,
                             Selection& sel) const
{
    const Terrain& t = *terrain_;
    const TerrainSettings& s = t.settings();
    // A node splits while error / distance * this > pixels (off screen: 8x the pixels), or while
    // nearer than the full-detail distance.
    const float scale = camera.p11 * 0.5f * float(displayHeight);
    constexpr float kOffscreen = 8.0f;
    // World frustum planes of the view (left, right, bottom, top, near), Gribb-Hartmann.
    const float4x4& m = camera.viewProj;
    float4 planes[5];
    const int rows[5] = {0, 0, 1, 1, 2};
    const float signs[5] = {1.0f, -1.0f, 1.0f, -1.0f, -1.0f};
    for (int i = 0; i < 5; ++i) {
        const float4 p = {m.m[0][3] + signs[i] * m.m[0][rows[i]], m.m[1][3] + signs[i] * m.m[1][rows[i]],
                          m.m[2][3] + signs[i] * m.m[2][rows[i]], m.m[3][3] + signs[i] * m.m[3][rows[i]]};
        const float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        planes[i] = {p.x / l, p.y / l, p.z / l, p.w / l};
    }
    sel.side.resize(t.levels());
    sel.bits.resize(t.levels());
    sel.splits.resize(t.levels());
    for (uint32_t l = 0; l < t.levels(); ++l) {
        sel.side[l] = t.nodesPerSide(l);
        sel.bits[l].assign((uint64_t(sel.side[l]) * sel.side[l] + 63) / 64, 0);
        sel.splits[l].clear();
    }
    struct Item {
        uint32_t level, x, z;
    };
    std::vector<Item> stack = {{t.levels() - 1, 0, 0}};
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        if (it.level == 0 || !t.nodeAvailable(it.level - 1, it.x * 2, it.z * 2))
            continue;
        const TerrainNode n = t.node(it.level, it.x, it.z);
        const float size = float(kPatchQuads << it.level) * s.spacing;
        const float x0 = s.originX + float(it.x) * size, z0 = s.originZ + float(it.z) * size;
        const float dx = std::max({x0 - camera.position.x, 0.0f, camera.position.x - (x0 + size)});
        const float dy = std::max({n.lo - camera.position.y, 0.0f, camera.position.y - n.hi});
        const float dz = std::max({z0 - camera.position.z, 0.0f, camera.position.z - (z0 + size)});
        const float d = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), camera.zNear);
        bool visible = true;
        for (const float4& p : planes)  // the box's corner furthest along the plane normal
            if (p.x * (p.x > 0 ? x0 + size : x0) + p.y * (p.y > 0 ? n.hi : n.lo) + p.z * (p.z > 0 ? z0 + size : z0) + p.w < 0.0f)
                visible = false;
        if (n.error * scale <= pixels * d * (visible ? 1.0f : kOffscreen) && (d >= detailDistance || n.error <= 0.0f))
            continue;
        sel.setSplit(it.level, it.x, it.z);
        for (uint32_t c = 0; c < 4; ++c)
            stack.push_back({it.level - 1, it.x * 2 + (c & 1), it.z * 2 + (c >> 1)});
    }
}

void TerrainRenderer::balance(Selection& sel) const
{
    // Restricted quadtree: no leaf may touch one two or more levels finer. Equivalently, a split
    // node's edge neighbours (at its level) must exist, so their parents are split too. Splitting
    // a parent adds a node one level up, checked when that level's turn comes (fine to coarse).
    const Terrain& t = *terrain_;
    for (uint32_t l = 1; l + 1 < t.levels(); ++l) {
        const uint32_t n = t.nodesPerSide(l);
        for (size_t i = 0; i < sel.splits[l].size(); ++i) {
            const uint64_t k = sel.splits[l][i];
            const uint32_t x = uint32_t(k & 0xffffff), z = uint32_t((k >> 24) & 0xffffff);
            const int32_t around[4][2] = {{int32_t(x) - 1, int32_t(z)}, {int32_t(x) + 1, int32_t(z)},
                                          {int32_t(x), int32_t(z) - 1}, {int32_t(x), int32_t(z) + 1}};
            for (const auto& a : around) {
                if (a[0] < 0 || a[1] < 0 || a[0] >= int32_t(n) || a[1] >= int32_t(n))
                    continue;
                const uint32_t px = uint32_t(a[0]) >> 1, pz = uint32_t(a[1]) >> 1;
                if (t.nodeAvailable(l, px * 2, pz * 2))
                    sel.setSplit(l + 1, px, pz);
            }
        }
    }
}

void TerrainRenderer::collectLeaves(Selection& sel) const
{
    const Terrain& t = *terrain_;
    sel.leaves.clear();
    std::vector<uint64_t> stack = {key(t.levels() - 1, 0, 0)};
    while (!stack.empty()) {
        const uint64_t k = stack.back();
        stack.pop_back();
        const uint32_t l = uint32_t(k >> 48);
        const uint32_t x = uint32_t(k & 0xffffff), z = uint32_t((k >> 24) & 0xffffff);
        if (!sel.isSplit(l, x, z)) {
            sel.leaves.push_back(k);
            continue;
        }
        for (uint32_t c = 0; c < 4; ++c)
            stack.push_back(key(l - 1, x * 2 + (c & 1), z * 2 + (c >> 1)));
    }
}

uint8_t TerrainRenderer::stitchOf(uint64_t k, const Selection& sel) const
{
    // An edge neighbour at this level that does not exist (its parent is a leaf) means a coarser
    // patch over there.
    const uint32_t l = uint32_t(k >> 48);
    if (l + 1 >= terrain_->levels())
        return 0;
    const int32_t x = int32_t(k & 0xffffff), z = int32_t((k >> 24) & 0xffffff), n = int32_t(terrain_->nodesPerSide(l));
    const int32_t around[4][2] = {{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}};
    uint8_t stitch = 0;
    for (uint32_t e = 0; e < 4; ++e) {
        const int32_t ax = around[e][0], az = around[e][1];
        if (ax < 0 || az < 0 || ax >= n || az >= n)
            continue;
        if (!sel.isSplit(l + 1, uint32_t(ax) >> 1, uint32_t(az) >> 1))
            stitch |= uint8_t(1u << e);
    }
    return stitch;
}

float4 TerrainRenderer::writePatch(uint64_t k, uint8_t stitch, uint32_t slot)
{
    const Terrain& t = *terrain_;
    const TerrainSettings& s = t.settings();
    const uint32_t level = uint32_t(k >> 48), nx = uint32_t(k & 0xffffff), nz = uint32_t((k >> 24) & 0xffffff);
    const int32_t step = 1 << level;
    const int32_t gx0 = int32_t(nx * kPatchQuads) * step, gz0 = int32_t(nz * kPatchQuads) * step;

    // Heights with a one-vertex border (for normals): the patch's own samples inside, the grid
    // around it (at the same spacing) outside.
    constexpr int32_t B = kSample + 2;
    float h[B * B];
    uint16_t own[kSample * kSample];
    t.patchSamples(level, nx, nz, own);
    for (int32_t j = -1; j <= int32_t(kSample); ++j)
        for (int32_t i = -1; i <= int32_t(kSample); ++i) {
            const bool inside = i >= 0 && j >= 0 && i < int32_t(kSample) && j < int32_t(kSample);
            h[(j + 1) * B + i + 1] = t.toWorld(inside ? own[j * kSample + i] : t.sample(gx0 + i * step, gz0 + j * step));
        }
    // Stitched edges: odd vertices on the line between their even neighbours (the coarser
    // neighbour's edge).
    auto at = [&](int32_t i, int32_t j) -> float& { return h[(j + 1) * B + i + 1]; };
    for (int32_t v = 1; v < int32_t(kPatchQuads); v += 2) {
        if (stitch & 1)
            at(0, v) = 0.5f * (at(0, v - 1) + at(0, v + 1));
        if (stitch & 2)
            at(kPatchQuads, v) = 0.5f * (at(kPatchQuads, v - 1) + at(kPatchQuads, v + 1));
        if (stitch & 4)
            at(v, 0) = 0.5f * (at(v - 1, 0) + at(v + 1, 0));
        if (stitch & 8)
            at(v, kPatchQuads) = 0.5f * (at(v - 1, kPatchQuads) + at(v + 1, kPatchQuads));
    }

    const float d = float(step) * s.spacing;
    float3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    float3 pos[kSample * kSample];
    for (int32_t j = 0; j < int32_t(kSample); ++j)
        for (int32_t i = 0; i < int32_t(kSample); ++i) {
            const float3 p = {s.originX + float(gx0 + i * step) * s.spacing, at(i, j), s.originZ + float(gz0 + j * step) * s.spacing};
            pos[j * kSample + i] = p;
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    const float3 scale = {std::max((hi.x - lo.x) / 65535.0f, 1e-7f), std::max((hi.y - lo.y) / 65535.0f, 1e-7f),
                          std::max((hi.z - lo.z) / 65535.0f, 1e-7f)};

    // Mesh row: the quantization box.
    fpak::MeshGpu mesh = {};
    mesh.posOffset[0] = lo.x;
    mesh.posOffset[1] = lo.y;
    mesh.posOffset[2] = lo.z;
    mesh.posScale[0] = scale.x;
    mesh.posScale[1] = scale.y;
    mesh.posScale[2] = scale.z;
    mesh.clusterOffset = slots_.firstCluster + slot;
    mesh.clusterCount = 1;
    // Cluster row: bounds; always its own LOD.
    const float3 center = (lo + hi) * 0.5f;
    const float radius = length(hi - lo) * 0.5f;
    fpak::ClusterGpu cl = {};
    for (float* sphere : {cl.cullSphere, cl.selfLod, cl.parentLod}) {
        sphere[0] = center.x;
        sphere[1] = center.y;
        sphere[2] = center.z;
        sphere[3] = radius;
    }
    cl.parentError = 3.0e38f;
    cl.vertexOffset = slots_.firstClusterVertex + slot * ScenePak::kPatchVertices;
    cl.triangleOffset = slots_.firstTriangle + slot * ScenePak::kPatchTriangles;
    cl.counts = ScenePak::kPatchVertices | ScenePak::kPatchTriangles << 8 | slots_.material << 16;
    cl.cone = 127u << 24;
    cl.depth = level;

    fpak::VertexGpu verts[kSample * kSample];
    for (int32_t j = 0; j < int32_t(kSample); ++j)
        for (int32_t i = 0; i < int32_t(kSample); ++i) {
            const float3 p = pos[j * kSample + i];
            const float dhdx = (at(i + 1, j) - at(i - 1, j)) / (2.0f * d);
            const float dhdz = (at(i, j + 1) - at(i, j - 1)) / (2.0f * d);
            const float3 n = normalize(float3{-dhdx, 1.0f, -dhdz});
            const float3 tangent = normalize(float3{1.0f, dhdx, 0.0f});
            const uint32_t qx = uint32_t(std::lround((p.x - lo.x) / scale.x));
            const uint32_t qy = uint32_t(std::lround((p.y - lo.y) / scale.y));
            const uint32_t qz = uint32_t(std::lround((p.z - lo.z) / scale.z));
            fpak::VertexGpu& v = verts[j * kSample + i];
            v.posXY = std::min(qx, 65535u) | std::min(qy, 65535u) << 16;
            v.posZ = std::min(qz, 65535u);
            v.normalOct = octEncode(n);
            v.tangentOct = octEncode(tangent);
            v.u = p.x;
            v.v = p.z;
        }

    // Triangles: the grid (alternating diagonals, as ScenePak lays it out), holes degenerate. A
    // quad is a hole where the cell at its middle is (coarse patches thin out small holes).
    uint32_t tris[ScenePak::kPatchTriangles];
    uint32_t nt = 0;
    for (uint32_t j = 0; j < kPatchQuads; ++j)
        for (uint32_t i = 0; i < kPatchQuads; ++i) {
            const bool hole = (t.material(gx0 + int32_t(i) * step + step / 2, gz0 + int32_t(j) * step + step / 2) & kHole) != 0;
            const uint32_t v00 = j * kSample + i, v10 = v00 + 1, v01 = v00 + kSample, v11 = v00 + kSample + 1;
            auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { tris[nt++] = hole ? 0u : (a | b << 8 | c << 16); };
            if (((i + j) & 1) == 0) {
                tri(v00, v01, v11);
                tri(v00, v11, v10);
            } else {
                tri(v00, v01, v10);
                tri(v10, v01, v11);
            }
        }
    // Ray tracing: the patch's triangles as quantized (holes left out).
    if (bvh_) {
        std::vector<rt::BvhTriangleGpu> traced;
        traced.reserve(nt);
        for (uint32_t i = 0; i < nt; ++i) {
            if (tris[i] == 0)
                continue;
            rt::BvhTriangleGpu tri;
            float* corner[3] = {tri.v0, tri.v1, tri.v2};
            for (int c = 0; c < 3; ++c) {
                const fpak::VertexGpu& v = verts[tris[i] >> (8 * c) & 0xff];
                corner[c][0] = lo.x + float(v.posXY & 0xffff) * scale.x;
                corner[c][1] = lo.y + float(v.posXY >> 16) * scale.y;
                corner[c][2] = lo.z + float(v.posZ & 0xffff) * scale.z;
            }
            tri.id = rt::triangleId(slots_.firstCluster + slot, i);
            traced.push_back(tri);
        }
        bvh_->setMesh(slots_.firstMesh + slot, std::move(traced));
    }
    // GI's albedo for the patch: the layers' average colours under its vertices, as the instance
    // tint (none until the layers' textures are built: the default look then).
    uint32_t tint = 0;
    if (surface_ && surface_->layerCount()) {
        float3 sum = {0, 0, 0};
        float total = 0.0f;
        bool ok = true;
        for (uint32_t j = 0; j < kSample && ok; j += 2)
            for (uint32_t i = 0; i < kSample && ok; i += 2) {
                const uint32_t m = t.material(gx0 + int32_t(i) * step, gz0 + int32_t(j) * step);
                const float wb = float(materialWeightB(m)) / 255.0f;
                float3 a, b;
                ok = surface_->layerMean(materialA(m), a) && (wb <= 0.0f || surface_->layerMean(materialB(m), b));
                if (ok) {
                    sum += a * (1.0f - wb);
                    if (wb > 0.0f)
                        sum += b * wb;
                    total += 1.0f;
                }
            }
        if (ok && total > 0.0f) {
            auto c8 = [](float v) { return uint32_t(std::clamp(std::lround(v * 255.0f), 1l, 255l)); };
            tint = c8(sum.x / total) | c8(sum.y / total) << 8 | c8(sum.z / total) << 16 | 255u << 24;
        }
    }
    fpak::InstanceGpu row = {};
    row.world[0][0] = row.world[1][1] = row.world[2][2] = 1.0f;
    row.mesh = slots_.firstMesh + slot;
    row.tint = tint;

    auto add = [&](uint32_t section, uint32_t offset, const void* data, uint32_t bytes) {
        writes_.push_back({section, offset, bytes, uint32_t(staging_.size())});
        const uint8_t* b = static_cast<const uint8_t*>(data);
        staging_.insert(staging_.end(), b, b + bytes);
    };
    for (uint32_t section : {uint32_t(fpak::Section::Instances), kPrevInstances})
        add(section, (slots_.firstInstance + slot) * uint32_t(sizeof(row)), &row, sizeof(row));
    add(uint32_t(fpak::Section::ClusterTriangles), (slots_.firstTriangle + slot * ScenePak::kPatchTriangles) * 4, tris, sizeof(tris));
    add(uint32_t(fpak::Section::Meshes), (slots_.firstMesh + slot) * uint32_t(sizeof(mesh)), &mesh, sizeof(mesh));
    add(uint32_t(fpak::Section::Clusters), (slots_.firstCluster + slot) * uint32_t(sizeof(cl)), &cl, sizeof(cl));
    add(uint32_t(fpak::Section::Vertices), (slots_.firstVertex + slot * ScenePak::kPatchVertices) * uint32_t(sizeof(fpak::VertexGpu)),
        verts, sizeof(verts));
    return {center.x, center.y, center.z, radius};
}

bool TerrainRenderer::takeGiChange(bool& all, float region[6])
{
    all = giAll_;
    if (giRegion_)
        std::memcpy(region, giBox_, sizeof(giBox_));
    const bool any = giAll_ || giRegion_;
    giAll_ = giRegion_ = false;
    return any;
}

void TerrainRenderer::hideAll()
{
    for (auto& [k, p] : patches_) {
        freeSlots_.push_back(p.slot);
        invalidations_.push_back(p.sphere);
        fpak::InstanceGpu row = {};
        row.world[0][0] = row.world[1][1] = row.world[2][2] = 1.0f;
        row.mesh = slots_.firstMesh + p.slot;
        row.flags = fpak::kInstanceHidden;
        const uint32_t offset = (slots_.firstInstance + p.slot) * uint32_t(sizeof(row));
        for (uint32_t section : {uint32_t(fpak::Section::Instances), kPrevInstances}) {
            writes_.push_back({section, offset, uint32_t(sizeof(row)), uint32_t(staging_.size())});
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&row);
            staging_.insert(staging_.end(), b, b + sizeof(row));
        }
    }
    patches_.clear();
    patchItems_.clear();
}

bool TerrainRenderer::update(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* prevInstances, const CameraState& camera,
                             uint32_t displayHeight, float pixels, float detailDistance)
{
    FX_ZONE_CAT("TerrainRenderer::update", Rendering);
    if (!slots_.patches)
        return false;
    staging_.clear();
    writes_.clear();
    invalidations_.clear();
    const bool have = terrain_ && terrain_->valid();
    if (!have) {
        if (patches_.empty())
            return false;
        hideAll();
        giAll_ = true;
    } else {
        // Terrain edits since the last update: rewrite the patches over them.
        std::vector<TerrainChange> changed;
        if (!terrain_->changesSince(terrainRevision_, changed))
            rewriteAll_ = true;
        terrainRevision_ = terrain_->revision();
        // New layer colours: every patch's GI tint.
        if (surface_ && surface_->meanRevision() != meanRevision_) {
            meanRevision_ = surface_->meanRevision();
            rewriteAll_ = true;
        }
        const float moved = length(camera.position - selectedFrom_);
        const bool reselect = rewriteAll_ || moved > 0.5f || pixels != selectedPixels_ || detailDistance != selectedDetail_ ||
                              displayHeight != selectedHeight_ ||
                              residencyRevision_ != terrain_->residencyRevision() || !changed.empty();
        if (!reselect)
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        if (rewriteAll_) {
            giAll_ = true;
        } else if (!changed.empty()) {
            float lo[3], hi[3];
            terrain_->bounds(lo, hi);
            const TerrainSettings& s = terrain_->settings();
            for (const TerrainChange& c : changed) {
                const TerrainRect& r = c.rect;
                const float box[6] = {s.originX + float(r.x0 - 1) * s.spacing, lo[1], s.originZ + float(r.z0 - 1) * s.spacing,
                                      s.originX + float(r.x1 + 1) * s.spacing, hi[1], s.originZ + float(r.z1 + 1) * s.spacing};
                for (int a = 0; a < 3; ++a) {
                    giBox_[a] = giRegion_ ? std::min(giBox_[a], box[a]) : box[a];
                    giBox_[3 + a] = giRegion_ ? std::max(giBox_[3 + a], box[3 + a]) : box[3 + a];
                }
                giRegion_ = true;
            }
        }
        Selection& sel = selection_;  // (kept: its bit arrays are reused)
        float used = std::max(pixels, lastPixels_ > pixels ? lastPixels_ / 1.25f : pixels);
        float detail = detailDistance;
        for (uint32_t attempt = 0; attempt < 16; ++attempt) {
            select(camera, displayHeight, used, detail, sel);
            balance(sel);
            collectLeaves(sel);
            if (sel.leaves.size() <= slots_.patches)
                break;
            used *= 1.25f;  // more patches than slots: coarser everywhere until they fit
            detail *= 0.8f;
        }
        lastPixels_ = used;
        std::vector<uint64_t>& leaves = sel.leaves;
        if (leaves.size() > slots_.patches)
            leaves.resize(slots_.patches);  // (a hole somewhere rather than no terrain)
        // Gone: hide their slots.
        uint32_t rewritten = 0;
        for (auto it = patches_.begin(); it != patches_.end();) {
            if (sel.isLeaf(it->first)) {
                ++it;
                continue;
            }
            freeSlots_.push_back(it->second.slot);
            invalidations_.push_back(it->second.sphere);
            fpak::InstanceGpu row = {};
            row.world[0][0] = row.world[1][1] = row.world[2][2] = 1.0f;
            row.mesh = slots_.firstMesh + it->second.slot;
            row.flags = fpak::kInstanceHidden;
            const uint32_t offset = (slots_.firstInstance + it->second.slot) * uint32_t(sizeof(row));
            for (uint32_t section : {uint32_t(fpak::Section::Instances), kPrevInstances}) {
                writes_.push_back({section, offset, uint32_t(sizeof(row)), uint32_t(staging_.size())});
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&row);
                staging_.insert(staging_.end(), b, b + sizeof(row));
            }
            it = patches_.erase(it);
        }
        // New, re-stitched or edited: (re)write them.
        // What changed under a patch (terrain::kChange*; normals reach one vertex beyond it).
        auto touched = [&](uint64_t k) {
            const uint32_t l = uint32_t(k >> 48);
            const int32_t step = 1 << l, span = int32_t(kPatchQuads) * step;
            const int32_t x0 = int32_t(k & 0xffffff) * span, z0 = int32_t((k >> 24) & 0xffffff) * span;
            uint32_t kinds = 0;
            for (const TerrainChange& c : changed) {
                const TerrainRect& r = c.rect;
                if (r.x0 <= x0 + span + step && r.x1 >= x0 - step && r.z0 <= z0 + span + step && r.z1 >= z0 - step)
                    kinds |= c.kinds;
            }
            return kinds;
        };
        for (const uint64_t k : leaves) {
            const uint8_t stitch = stitchOf(k, sel);
            auto it = patches_.find(k);
            const bool fresh = it == patches_.end();
            const uint32_t kinds = fresh ? 0u : touched(k);
            if (!fresh && !rewriteAll_ && it->second.stitch == stitch && !kinds)
                continue;
            // Shadows change with the shape only (not with paint, nor with GI tints).
            const bool shape = fresh || it->second.stitch != stitch || (kinds & (kChangeHeights | kChangeHoles));
            if (fresh) {
                it = patches_.emplace(k, Patch{freeSlots_.back(), stitch, {}}).first;
                freeSlots_.pop_back();
            } else if (shape) {
                invalidations_.push_back(it->second.sphere);
            }
            it->second.stitch = stitch;
            it->second.sphere = writePatch(k, stitch, it->second.slot);
            if (shape || rewriteAll_)
                invalidations_.push_back(it->second.sphere);
            ++rewritten;
        }
        patchItems_.clear();
        for (const auto& [k, p] : patches_)
            patchItems_.push_back({slots_.firstInstance + p.slot, slots_.firstCluster + p.slot, p.sphere});
        selectedFrom_ = camera.position;
        selectedPixels_ = pixels;
        selectedDetail_ = detailDistance;
        selectedHeight_ = displayHeight;
        residencyRevision_ = terrain_->residencyRevision();
        rewriteAll_ = false;
        stats_.patches = uint32_t(patches_.size());
        if (rewritten)
            stats_.rewritten = rewritten;
        stats_.pixels = used;
        stats_.selectMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    if (writes_.empty())
        return false;

    // Upload: one transfer buffer (grown as needed), one copy pass.
    if (staging_.size() > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = uint32_t(std::max<size_t>(staging_.size() + staging_.size() / 2, 1u << 20));
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Terrain", device_, &info, "terrain patch upload");
        if (!upload_) {
            uploadBytes_ = 0;
            return false;
        }
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
    std::memcpy(mapped, staging_.data(), staging_.size());
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "terrain patches");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const Write& w : writes_) {
        const SDL_GPUTransferBufferLocation src = {upload_, w.src};
        const SDL_GPUBufferRegion dst = w.section == kPrevInstances
                                            ? SDL_GPUBufferRegion{prevInstances, w.offset, w.bytes}
                                            : SDL_GPUBufferRegion{pak_->sceneBuffer(),
                                                                  pak_->gpuSectionOffset(fpak::Section(w.section)) + w.offset, w.bytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    vsm_->uploadInvalidations(copy, invalidations_, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

}  // namespace fx
