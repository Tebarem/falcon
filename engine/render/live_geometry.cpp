#include "render/live_geometry.h"

#include "core/log.h"
#include "core/mesh_tangents.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/ray_tracing/scene_bvh.h"
#include "render/virtual_shadow_map.h"

#include <box3d/collision.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace fx {
namespace {

constexpr uint32_t kBlockVertices = ScenePak::kLiveBlockVertices;
constexpr uint32_t kBlockTriangles = ScenePak::kLiveBlockTriangles;

uint32_t snorm16(float v)
{
    const float c = std::clamp(v, -1.0f, 1.0f);
    return uint32_t(uint16_t(int16_t(std::lround(c * 32767.0f))));
}

// Octahedral snorm16 x2, as the cooker writes it (tools/cook/build_mesh.cpp).
uint32_t octEncode(float x, float y, float z)
{
    const float l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (l1 <= 0.0f)
        return snorm16(0) | snorm16(0) << 16;
    x /= l1;
    y /= l1;
    if (z < 0.0f) {
        const float ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    return snorm16(x) | snorm16(y) << 16;
}

struct Vertex {
    float p[3], n[3], uv[2], t[4], uv1[2];
    uint32_t color;
};

// A tangent perpendicular to n (meshes without UVs: normal maps don't apply, any frame will do).
void anyTangent(const float n[3], float t[4])
{
    const float ax = std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, ay = 1.0f - ax;
    const float d = ax * n[0] + ay * n[1];
    float3 v = normalize(float3{ax - n[0] * d, ay - n[1] * d, -n[2] * d});
    t[0] = v.x;
    t[1] = v.y;
    t[2] = v.z;
    t[3] = 1.0f;
}

// World bounding sphere of mesh-space bounds under a 3x4 transform.
void worldSphere(const float lo[3], const float hi[3], const float world[3][4], float out[4])
{
    const float c[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float r = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                     (hi[2] - lo[2]) * (hi[2] - lo[2]));
    float scale = 0.0f;
    for (int col = 0; col < 3; ++col)
        scale = std::max(scale, length(float3{world[0][col], world[1][col], world[2][col]}));
    for (int row = 0; row < 3; ++row)
        out[row] = world[row][0] * c[0] + world[row][1] * c[1] + world[row][2] * c[2] + world[row][3];
    out[3] = r * scale;
}

struct TriKey {
    uint32_t a, b, c;
    bool operator==(const TriKey& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct TriKeyHash {
    size_t operator()(const TriKey& k) const
    {
        return size_t(k.a) * 0x9E3779B97F4A7C15ull ^ size_t(k.b) * 0xC2B2AE3D27D4EB4Full ^ size_t(k.c) * 0x165667B19E3779F9ull;
    }
};
// Rotation-independent (a meshlet may start a triangle at another corner), winding kept.
TriKey triKey(uint32_t a, uint32_t b, uint32_t c)
{
    if (b < a && b < c)
        return {b, c, a};
    if (c < a && c < b)
        return {c, a, b};
    return {a, b, c};
}

}  // namespace

void LiveGeometry::init(SDL_GPUDevice* device, const ScenePak& pak, VirtualShadowMap* vsm)
{
    device_ = device;
    pak_ = &pak;
    vsm_ = vsm;
    slots_ = pak.liveSlots();
    verticesOffset_ = pak.gpuSectionOffset(fpak::Section::Vertices);
    meshes_.assign(slots_.meshes, Mesh{});
    free_.clear();
    if (slots_.blocks)
        free_.push_back({0u, slots_.blocks});
    stats_ = {};
    stats_.meshCapacity = slots_.meshes;
    stats_.blockCapacity = slots_.blocks;
}

void LiveGeometry::shutdown()
{
    if (upload_)
        gpumem::releaseTransferBuffer(device_, upload_);
    upload_ = nullptr;
    uploadBytes_ = 0;
    meshes_.clear();
    free_.clear();
    writes_.clear();
    staging_.clear();
    slots_ = {};
}

int32_t LiveGeometry::create()
{
    for (uint32_t i = 0; i < meshes_.size(); ++i)
        if (!meshes_[i].used) {
            meshes_[i] = Mesh{};
            meshes_[i].used = true;
            ++stats_.meshes;
            return int32_t(i);
        }
    return -1;
}

bool LiveGeometry::allocate(uint32_t count, uint32_t& first)
{
    for (size_t i = 0; i < free_.size(); ++i)
        if (free_[i].second >= count) {
            first = free_[i].first;
            free_[i].first += count;
            free_[i].second -= count;
            if (!free_[i].second)
                free_.erase(free_.begin() + ptrdiff_t(i));
            stats_.blocks += count;
            return true;
        }
    return false;
}

void LiveGeometry::release(uint32_t first, uint32_t count)
{
    if (!count)
        return;
    stats_.blocks -= count;
    auto it = std::lower_bound(free_.begin(), free_.end(), std::make_pair(first, 0u));
    it = free_.insert(it, {first, count});
    // Merge with the neighbours.
    if (it + 1 != free_.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        free_.erase(it + 1);
    }
    if (it != free_.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        free_.erase(it);
    }
}

void LiveGeometry::add(uint32_t section, uint32_t offset, const void* data, uint32_t bytes)
{
    writes_.push_back({section, offset, bytes, uint32_t(staging_.size())});
    const uint8_t* b = static_cast<const uint8_t*>(data);
    staging_.insert(staging_.end(), b, b + bytes);
}

void LiveGeometry::invalidate(const float sphere[4])
{
    if (sphere[3] <= 0.0f)
        return;
    invalidations_.push_back({sphere[0], sphere[1], sphere[2], sphere[3]});
    const float lo[3] = {sphere[0] - sphere[3], sphere[1] - sphere[3], sphere[2] - sphere[3]};
    const float hi[3] = {sphere[0] + sphere[3], sphere[1] + sphere[3], sphere[2] + sphere[3]};
    for (int a = 0; a < 3; ++a) {
        giBox_[a] = giChanged_ ? std::min(giBox_[a], lo[a]) : lo[a];
        giBox_[a + 3] = giChanged_ ? std::max(giBox_[a + 3], hi[a]) : hi[a];
    }
    giChanged_ = true;
}

void PreparedLiveMesh::ColliderDeleter::operator()(b3MeshData* mesh) const
{
    if (mesh)
        b3DestroyMesh(mesh);
}

std::vector<uint8_t> LiveGeometry::blendedMaterials() const
{
    const auto& materials = pak_->materials();
    std::vector<uint8_t> out(materials.size());
    for (size_t i = 0; i < materials.size(); ++i)
        out[i] = (materials[i].flags & fpak::kMaterialAlphaBlend) ? 1 : 0;
    return out;
}

bool LiveGeometry::set(uint32_t live, const LiveMeshData& data, const float world[3][4], bool collider)
{
    FX_ZONE_CAT("LiveGeometry::set", Rendering);
    if (live >= meshes_.size() || !meshes_[live].used)
        return false;
    PreparedLiveMesh prepared;
    if (!prepare(data, blendedMaterials(), traced(), collider && colliderPak_, prepared))
        return false;
    return commit(live, std::move(prepared), world);
}

bool LiveGeometry::prepare(const LiveMeshData& data, const std::vector<uint8_t>& blended, bool traced, bool collider,
                           PreparedLiveMesh& out)
{
    FX_ZONE_CAT("LiveGeometry::prepare", Rendering);
    const auto t0 = std::chrono::steady_clock::now();
    out = PreparedLiveMesh{};
    const size_t n = data.positions.size() / 3;
    const size_t triCount = data.indices.size() / 3;
    if (data.positions.size() != n * 3 || data.normals.size() != n * 3 || (!data.uvs.empty() && data.uvs.size() != n * 2) ||
        (!data.tangents.empty() && data.tangents.size() != n * 4) || (!data.materials.empty() && data.materials.size() != triCount) ||
        (!data.uv1.empty() && data.uv1.size() != n * 2) || (!data.colors.empty() && data.colors.size() != n))
        return false;
    for (uint32_t i : data.indices)
        if (i >= n)
            return false;

    // Vertices; without tangents, MikkTSpace per corner, then the identical corners welded again.
    std::vector<Vertex> verts(n);
    for (size_t v = 0; v < n; ++v) {
        Vertex& o = verts[v];
        std::memcpy(o.p, &data.positions[v * 3], 12);
        std::memcpy(o.n, &data.normals[v * 3], 12);
        o.uv[0] = data.uvs.empty() ? 0.0f : data.uvs[v * 2];
        o.uv[1] = data.uvs.empty() ? 0.0f : data.uvs[v * 2 + 1];
        if (!data.tangents.empty())
            std::memcpy(o.t, &data.tangents[v * 4], 16);
        else
            anyTangent(o.n, o.t);
        o.uv1[0] = data.uv1.empty() ? 0.0f : data.uv1[v * 2];
        o.uv1[1] = data.uv1.empty() ? 0.0f : data.uv1[v * 2 + 1];
        o.color = data.colors.empty() ? 0xffffffffu : data.colors[v];
    }
    std::vector<uint32_t> indices(data.indices.begin(), data.indices.begin() + ptrdiff_t(triCount * 3));
    if (data.tangents.empty() && !data.uvs.empty() && !indices.empty()) {
        std::vector<float> tangents(indices.size() * 4);
        if (generateTangents(indices.data(), indices.size(), {verts[0].p, sizeof(Vertex)}, {verts[0].n, sizeof(Vertex)},
                             {verts[0].uv, sizeof(Vertex)}, tangents.data())) {
            std::vector<Vertex> corners(indices.size());
            for (size_t c = 0; c < indices.size(); ++c) {
                corners[c] = verts[indices[c]];
                std::memcpy(corners[c].t, &tangents[c * 4], 16);
            }
            std::vector<uint32_t> remap(corners.size());
            const size_t unique = meshopt_generateVertexRemap(remap.data(), nullptr, corners.size(), corners.data(),
                                                              corners.size(), sizeof(Vertex));
            verts.resize(unique);
            meshopt_remapVertexBuffer(verts.data(), corners.data(), corners.size(), sizeof(Vertex), remap.data());
            indices = std::move(remap);  // (corner c of the unindexed list is vertex remap[c])
        }
    }

    // Bounds and quantization (to the mesh's own bounds, like cooked meshes).
    float* lo = out.lo;
    float* hi = out.hi;
    float* scale = out.scale;
    for (size_t v = 0; v < verts.size(); ++v)
        for (int a = 0; a < 3; ++a) {
            lo[a] = v ? std::min(lo[a], verts[v].p[a]) : verts[v].p[a];
            hi[a] = v ? std::max(hi[a], verts[v].p[a]) : verts[v].p[a];
        }
    for (int a = 0; a < 3; ++a)
        scale[a] = std::max(hi[a] - lo[a], 1e-6f) / 65535.0f;
    std::vector<float> quantized(verts.size() * 3);  // what the GPU will draw (cluster bounds)
    std::vector<fpak::VertexGpu> gpuVerts(verts.size());
    std::vector<uint64_t> gpuExtra(verts.size());  // the extra stream: uv1 (unorm16x2), colour (RGBA8)
    for (size_t v = 0; v < verts.size(); ++v) {
        uint32_t q[3];
        for (int a = 0; a < 3; ++a) {
            q[a] = uint32_t(std::clamp(std::lround((verts[v].p[a] - lo[a]) / scale[a]), 0l, 65535l));
            quantized[v * 3 + a] = lo[a] + float(q[a]) * scale[a];
        }
        fpak::VertexGpu& g = gpuVerts[v];
        g.posXY = q[0] | q[1] << 16;
        g.posZ = q[2] | (verts[v].t[3] < 0.0f ? 1u : 0u) << 16;
        g.normalOct = octEncode(verts[v].n[0], verts[v].n[1], verts[v].n[2]);
        g.tangentOct = octEncode(verts[v].t[0], verts[v].t[1], verts[v].t[2]);
        g.u = verts[v].uv[0];
        g.v = verts[v].uv[1];
        const auto unorm16 = [](float x) { return uint64_t(std::clamp(std::lround(x * 65535.0f), 0l, 65535l)); };
        gpuExtra[v] = unorm16(verts[v].uv1[0]) | unorm16(verts[v].uv1[1]) << 16 | uint64_t(verts[v].color) << 32;
    }

    // Clusters: meshlets of each material's triangles (a cluster has one material).
    std::vector<uint32_t> order(triCount);
    std::iota(order.begin(), order.end(), 0u);
    auto materialOf = [&](uint32_t t) { return data.materials.empty() ? 0u : data.materials[t]; };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return materialOf(a) < materialOf(b); });
    std::unordered_map<TriKey, uint32_t, TriKeyHash> sourceOf;  // welded triangle -> source triangle
    sourceOf.reserve(triCount);
    for (uint32_t t = 0; t < triCount; ++t)
        sourceOf.emplace(triKey(indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]), t);
    struct Cluster {
        meshopt_Meshlet meshlet;
        uint32_t material;
        size_t vertexBase, triangleBase;  // into the group's arrays below
        uint32_t group;
    };
    struct Group {
        std::vector<uint32_t> vertices;
        std::vector<uint8_t> triangles;
    };
    std::vector<Cluster> clusters;
    std::vector<Group> groups;
    for (size_t begin = 0; begin < order.size();) {
        const uint32_t material = materialOf(order[begin]);
        size_t end = begin;
        std::vector<uint32_t> groupIndices;
        while (end < order.size() && materialOf(order[end]) == material) {
            for (int k = 0; k < 3; ++k)
                groupIndices.push_back(indices[order[end] * 3 + k]);
            ++end;
        }
        const size_t bound = meshopt_buildMeshletsBound(groupIndices.size(), kBlockVertices, kBlockTriangles);
        std::vector<meshopt_Meshlet> meshlets(bound);
        Group g;
        g.vertices.resize(bound * kBlockVertices);
        g.triangles.resize(bound * kBlockTriangles * 3);
        const size_t count = meshopt_buildMeshlets(meshlets.data(), g.vertices.data(), g.triangles.data(), groupIndices.data(),
                                                   groupIndices.size(), quantized.data(), verts.size(), 12, kBlockVertices,
                                                   kBlockTriangles, 0.25f);
        for (size_t c = 0; c < count; ++c)
            clusters.push_back({meshlets[c], material, meshlets[c].vertex_offset, meshlets[c].triangle_offset, uint32_t(groups.size())});
        groups.push_back(std::move(g));
        begin = end;
    }

    // Blocks, one a cluster, numbered from 0 (commit places them).
    const uint32_t blocks = uint32_t(clusters.size());
    out.blockVerts.assign(size_t(blocks) * kBlockVertices, fpak::VertexGpu{});
    out.blockExtra.assign(size_t(blocks) * kBlockVertices, uint64_t(0xffffffffu) << 32);
    out.blockTris.assign(size_t(blocks) * kBlockTriangles, 0u);
    out.rows.assign(blocks, fpak::ClusterGpu{});
    out.sourceTriangles.assign(size_t(blocks) * kBlockTriangles, ~0u);
    for (uint32_t c = 0; c < blocks; ++c) {
        const Cluster& cl = clusters[c];
        const Group& g = groups[cl.group];
        const uint32_t* local = &g.vertices[cl.vertexBase];
        const uint8_t* tris = &g.triangles[cl.triangleBase];
        for (uint32_t v = 0; v < cl.meshlet.vertex_count; ++v) {
            out.blockVerts[size_t(c) * kBlockVertices + v] = gpuVerts[local[v]];
            out.blockExtra[size_t(c) * kBlockVertices + v] = gpuExtra[local[v]];
        }
        for (uint32_t t = 0; t < cl.meshlet.triangle_count; ++t) {
            const uint8_t a = tris[t * 3], b = tris[t * 3 + 1], d = tris[t * 3 + 2];
            out.blockTris[size_t(c) * kBlockTriangles + t] = uint32_t(a) | uint32_t(b) << 8 | uint32_t(d) << 16;
            const auto found = sourceOf.find(triKey(local[a], local[b], local[d]));
            out.sourceTriangles[size_t(c) * kBlockTriangles + t] = found == sourceOf.end() ? ~0u : found->second;
        }
        const meshopt_Bounds b = meshopt_computeMeshletBounds(local, tris, cl.meshlet.triangle_count, quantized.data(),
                                                             verts.size(), 12);
        fpak::ClusterGpu& r = out.rows[c];
        for (float* sphere : {r.cullSphere, r.selfLod, r.parentLod}) {
            sphere[0] = b.center[0];
            sphere[1] = b.center[1];
            sphere[2] = b.center[2];
            sphere[3] = b.radius;
        }
        r.parentError = 3.0e38f;  // one level: always drawn
        r.vertexOffset = c * kBlockVertices;
        r.triangleOffset = c * kBlockTriangles;
        r.counts = cl.meshlet.vertex_count | cl.meshlet.triangle_count << 8 | (cl.material & 0xffff) << 16;
        r.cone = uint32_t(uint8_t(b.cone_axis_s8[0])) | uint32_t(uint8_t(b.cone_axis_s8[1])) << 8 |
                 uint32_t(uint8_t(b.cone_axis_s8[2])) << 16 | uint32_t(uint8_t(b.cone_cutoff_s8)) << 24;
    }
    // Ray tracing: the triangles as quantized here (alpha-blended clusters left out).
    if (traced) {
        std::vector<rt::BvhTriangleGpu> tracedTris;
        tracedTris.reserve(triCount);
        for (uint32_t c = 0; c < blocks; ++c) {
            const uint32_t material = clusters[c].material;
            if (material < blended.size() && blended[material])
                continue;
            for (uint32_t t = 0, nt = clusters[c].meshlet.triangle_count; t < nt; ++t) {
                const uint32_t packed = out.blockTris[size_t(c) * kBlockTriangles + t];
                rt::BvhTriangleGpu tri;
                float* corner[3] = {tri.v0, tri.v1, tri.v2};
                for (int k = 0; k < 3; ++k) {
                    const fpak::VertexGpu& v = out.blockVerts[size_t(c) * kBlockVertices + (packed >> (8 * k) & 0xff)];
                    corner[k][0] = lo[0] + float(v.posXY & 0xffff) * scale[0];
                    corner[k][1] = lo[1] + float(v.posXY >> 16) * scale[1];
                    corner[k][2] = lo[2] + float(v.posZ & 0xffff) * scale[2];
                }
                tri.id = rt::triangleId(c, t);
                tracedTris.push_back(tri);
            }
        }
        out.traced = true;
        out.bvh = SceneBvh::buildMesh(std::move(tracedTris));
    }

    // Collider: the triangles as they are (welded by position in Box3D).
    if (collider && triCount) {
        std::vector<b3Vec3> positions(verts.size());
        for (size_t v = 0; v < verts.size(); ++v)
            positions[v] = {verts[v].p[0], verts[v].p[1], verts[v].p[2]};
        std::vector<int> tri(indices.begin(), indices.end());
        b3MeshDef def = {};
        def.vertices = positions.data();
        def.indices = tri.data();
        def.vertexCount = int(positions.size());
        def.triangleCount = int(triCount);
        def.weldVertices = true;
        def.identifyEdges = true;
        out.collider.reset(b3CreateMesh(&def, nullptr, 0));
    }
    out.triangles = uint32_t(triCount);
    out.valid = true;
    out.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool LiveGeometry::commit(uint32_t live, PreparedLiveMesh&& p, const float world[3][4])
{
    FX_ZONE_CAT("LiveGeometry::commit", Rendering);
    const auto t0 = std::chrono::steady_clock::now();
    if (live >= meshes_.size() || !meshes_[live].used || !p.valid)
        return false;
    Mesh& m = meshes_[live];

    // Room: reuse the mesh's run if it is big enough, else a new run (the old one kept on failure).
    const uint32_t blocks = uint32_t(p.rows.size());
    uint32_t first = m.firstBlock;
    if (blocks > m.blockCount) {
        if (!allocate(blocks, first)) {
            FX_WARN("live meshes: no room for %u clusters (%u of %u blocks used)", blocks, stats_.blocks, stats_.blockCapacity);
            return false;
        }
        release(m.firstBlock, m.blockCount);
        m.firstBlock = first;
        m.blockCount = blocks;
    }

    // Rows: blocks [first, first + blocks) of every geometry section, written whole.
    std::vector<uint32_t> blockClusterVerts(size_t(blocks) * kBlockVertices);
    for (size_t v = 0; v < blockClusterVerts.size(); ++v)
        blockClusterVerts[v] = slots_.firstVertex + first * kBlockVertices + uint32_t(v);
    for (fpak::ClusterGpu& r : p.rows) {
        r.vertexOffset += slots_.firstClusterVertex + first * kBlockVertices;
        r.triangleOffset += slots_.firstTriangle + first * kBlockTriangles;
    }
    m.sourceTriangles = std::move(p.sourceTriangles);
    if (bvh_) {
        // (rt::triangleId is cluster << 7 | triangle: the clusters move up by where the blocks went)
        const uint32_t base = rt::triangleId(slots_.firstCluster + first, 0);
        for (rt::BvhTriangleGpu& tri : p.bvh.triangles)
            tri.id += base;
        const uint32_t count = uint32_t(p.bvh.triangles.size());
        stats_.bvhTriangles = stats_.bvhTriangles - m.bvhTriangles + count;
        m.bvhTriangles = count;
        bvh_->setBuilt(meshOf(live), std::move(p.bvh));
    }
    using S = fpak::Section;
    if (blocks) {
        add(uint32_t(S::Vertices), (slots_.firstVertex + first * kBlockVertices) * uint32_t(sizeof(fpak::VertexGpu)),
            p.blockVerts.data(), uint32_t(p.blockVerts.size() * sizeof(fpak::VertexGpu)));
        add(uint32_t(S::Vertices), extraOffset() + first * kBlockVertices * 8u, p.blockExtra.data(), uint32_t(p.blockExtra.size() * 8));
        add(uint32_t(S::ClusterVertices), (slots_.firstClusterVertex + first * kBlockVertices) * 4u, blockClusterVerts.data(),
            uint32_t(blockClusterVerts.size() * 4));
        add(uint32_t(S::ClusterTriangles), (slots_.firstTriangle + first * kBlockTriangles) * 4u, p.blockTris.data(),
            uint32_t(p.blockTris.size() * 4));
        add(uint32_t(S::Clusters), (slots_.firstCluster + first) * uint32_t(sizeof(fpak::ClusterGpu)), p.rows.data(),
            uint32_t(p.rows.size() * sizeof(fpak::ClusterGpu)));
    }
    fpak::MeshGpu row = {};
    for (int a = 0; a < 3; ++a) {
        row.posOffset[a] = p.lo[a];
        row.posScale[a] = p.scale[a];
    }
    row.clusterOffset = slots_.firstCluster + first;
    row.clusterCount = blocks;
    chunksChanged_ = true;
    add(uint32_t(S::Meshes), meshOf(live) * uint32_t(sizeof(fpak::MeshGpu)), &row, sizeof(row));
    if (colliderPak_)
        colliderPak_->setLiveMeshRow(meshOf(live), row);

    // Shadows and GI where it was and where it is now.
    invalidate(m.sphere);
    std::memcpy(m.lo, p.lo, sizeof(m.lo));
    std::memcpy(m.hi, p.hi, sizeof(m.hi));
    worldSphere(p.lo, p.hi, world, m.sphere);
    invalidate(m.sphere);
    stats_.triangles = stats_.triangles - m.triangles + p.triangles;
    m.triangles = p.triangles;
    m.clusters = blocks;
    ++stats_.uploads;

    if (colliderPak_)
        colliderPak_->setLiveCollider(meshOf(live), p.collider.release());
    stats_.lastBuildMs = p.ms + std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void LiveGeometry::destroy(uint32_t live, const float world[3][4])
{
    if (live >= meshes_.size() || !meshes_[live].used)
        return;
    Mesh& m = meshes_[live];
    invalidate(m.sphere);
    (void)world;
    release(m.firstBlock, m.blockCount);
    if (bvh_)
        bvh_->setMesh(meshOf(live), {});
    stats_.bvhTriangles -= m.bvhTriangles;
    m.bvhTriangles = 0;
    chunksChanged_ = true;
    fpak::MeshGpu row = {};
    row.posScale[0] = row.posScale[1] = row.posScale[2] = 1.0f / 65535.0f;
    add(uint32_t(fpak::Section::Meshes), meshOf(live) * uint32_t(sizeof(fpak::MeshGpu)), &row, sizeof(row));
    if (colliderPak_) {
        colliderPak_->setLiveMeshRow(meshOf(live), row);
        colliderPak_->setLiveCollider(meshOf(live), nullptr);
    }
    stats_.triangles -= m.triangles;
    --stats_.meshes;
    m = Mesh{};
}

void LiveGeometry::bounds(uint32_t live, float lo[3], float hi[3]) const
{
    for (int a = 0; a < 3; ++a)
        lo[a] = hi[a] = 0.0f;
    if (live < meshes_.size())
        for (int a = 0; a < 3; ++a) {
            lo[a] = meshes_[live].lo[a];
            hi[a] = meshes_[live].hi[a];
        }
}

int32_t LiveGeometry::sourceTriangle(uint32_t live, uint32_t cluster, uint32_t triangle) const
{
    if (live >= meshes_.size() || triangle >= kBlockTriangles)
        return -1;
    const Mesh& m = meshes_[live];
    const uint32_t c = cluster - (slots_.firstCluster + m.firstBlock);
    if (cluster < slots_.firstCluster + m.firstBlock || c >= m.clusters)
        return -1;
    const uint32_t s = m.sourceTriangles[size_t(c) * kBlockTriangles + triangle];
    return s == ~0u ? -1 : int32_t(s);
}

bool LiveGeometry::flush(SDL_GPUCommandBuffer* cmd)
{
    if (writes_.empty() && invalidations_.empty() && !chunksChanged_)
        return false;
    FX_ZONE_CAT("LiveGeometry::flush", Rendering);
    if (staging_.size() > uploadBytes_) {
        if (upload_)
            gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = uint32_t(std::max<size_t>(staging_.size() + staging_.size() / 2, 1u << 20));
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Renderer", device_, &info, "live mesh upload");
        if (!upload_) {
            uploadBytes_ = 0;
            return false;
        }
    }
    if (!staging_.empty()) {
        void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
        std::memcpy(mapped, staging_.data(), staging_.size());
        SDL_UnmapGPUTransferBuffer(device_, upload_);
    }
    FX_GPU_PASS(cmd, "live meshes");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const Write& w : writes_) {
        const SDL_GPUTransferBufferLocation src = {upload_, w.src};
        const SDL_GPUBufferRegion dst = {pak_->sceneBuffer(), pak_->gpuSectionOffset(fpak::Section(w.section)) + w.offset, w.bytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    if (vsm_ && !invalidations_.empty())
        vsm_->uploadInvalidations(copy, invalidations_, true);
    // Shadow casters: every live mesh's clusters, 64 a chunk (the shadow map's cull reads a fixed
    // list; without these, live meshes cast no shadows).
    if (vsm_ && chunksChanged_) {
        std::vector<uint32_t> chunks;
        for (uint32_t live = 0; live < meshes_.size(); ++live)
            if (const Mesh& m = meshes_[live]; m.used)
                for (uint32_t c = 0; c < m.clusters; c += 64) {
                    chunks.push_back(instanceOf(live));
                    chunks.push_back(slots_.firstCluster + m.firstBlock + c);
                }
        vsm_->setLiveChunks(copy, chunks);
    }
    chunksChanged_ = false;
    SDL_EndGPUCopyPass(copy);
    writes_.clear();
    staging_.clear();
    invalidations_.clear();
    return true;
}

bool LiveGeometry::takeGiChange(float region[6])
{
    if (!giChanged_)
        return false;
    std::memcpy(region, giBox_, sizeof(giBox_));
    giChanged_ = false;
    return true;
}

}  // namespace fx
