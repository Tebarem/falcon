// Cooks one primitive: dedup -> quantize -> cluster LOD DAG (meshoptimizer clusterlod)
// -> BVH over cluster groups -> GPU-ready arrays. Foliage made of many separate pieces (grass
// blades, leaves) gets pruned LOD levels instead of simplified ones (Pruning below).

#include "cook.h"

#include <meshoptimizer.h>
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace fx::cook {

namespace {

constexpr size_t kLodNodeWidth = 8;

uint32_t snorm16(float v)
{
    const float c = std::clamp(v, -1.0f, 1.0f);
    return uint32_t(uint16_t(int16_t(std::lround(c * 32767.0f))));
}

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

void setSphere(float out[4], const clodBounds& b)
{
    out[0] = b.center[0];
    out[1] = b.center[1];
    out[2] = b.center[2];
    out[3] = b.radius;
}

// Stochastic pruning for foliage (Cook, Halstead, Planck, Ryu 2007: "Stochastic Simplification
// of Aggregate Detail"). Simplification measures geometric error, and collapsing a blade a
// centimetre wide into a line costs almost none, so simplified levels lost whole blades and grass
// thinned to bare ground with distance. Here level L keeps a fixed 1/2^L of the mesh's separate
// pieces instead (chosen by a hash, so the same pieces at every level and every cook) and widens
// each survivor 2^L times across its width axis, which keeps the area the foliage covers. Pieces
// are disconnected, so dropping one opens no crack. The widened survivors are extra vertices,
// appended before quantization; clusterlod picks them through its simplifier override.
constexpr uint32_t kPruneMinPieces = 8;  // fewer: every instance would drop the same few parts
constexpr uint32_t kPruneLevels = 4;     // down to 1/16 of the pieces, 16x as wide

struct Pruning {
    std::vector<uint32_t> piece;     // per vertex (copies too)
    std::vector<uint32_t> original;  // per vertex: the original it copies (itself for originals)
    std::vector<float> rank;         // per piece, [0, 1): survives level L while rank < 1 / 2^L
    std::vector<float> width;        // per piece: extent across its width axis (mesh units)
    std::vector<uint32_t> copies[kPruneLevels + 1];  // [L][original]: its widened copy (~0u: pruned)
};

uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Unit eigenvector of the largest eigenvalue of symmetric `c` (power iteration from `seed`).
void majorAxis(const double c[3][3], double seed[3], double out[3])
{
    double v[3] = {seed[0], seed[1], seed[2]};
    for (int it = 0; it < 64; ++it) {
        double w[3];
        for (int r = 0; r < 3; ++r)
            w[r] = c[r][0] * v[0] + c[r][1] * v[1] + c[r][2] * v[2];
        const double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (l < 1e-30)
            break;
        for (int r = 0; r < 3; ++r)
            v[r] = w[r] / l;
    }
    for (int r = 0; r < 3; ++r)
        out[r] = v[r];
}

// Splits the mesh into pieces and appends the widened survivors of every level to `verts`.
// False (no pruning) for meshes of too few pieces.
bool buildPruning(std::vector<SrcVertex>& verts, const std::vector<uint32_t>& indices, Pruning& p)
{
    const size_t n = verts.size();
    std::vector<unsigned int> same(n);  // vertices at one position count as connected
    meshopt_generatePositionRemap(same.data(), &verts[0].px, n, sizeof(SrcVertex));
    std::vector<uint32_t> parent(n);
    for (size_t v = 0; v < n; ++v)
        parent[v] = uint32_t(v);
    auto find = [&](uint32_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t a = find(same[indices[t]]);
        parent[find(same[indices[t + 1]])] = a;
        parent[find(same[indices[t + 2]])] = a;
    }
    // Pieces numbered in index order (deterministic).
    std::vector<uint32_t> pieceOfRoot(n, ~0u);
    p.piece.assign(n, 0);
    uint32_t pieces = 0;
    for (const uint32_t i : indices) {
        uint32_t& id = pieceOfRoot[find(same[i])];
        if (id == ~0u)
            id = pieces++;
    }
    if (pieces < kPruneMinPieces)
        return false;
    for (size_t v = 0; v < n; ++v)
        p.piece[v] = pieceOfRoot[find(same[v])] == ~0u ? 0 : pieceOfRoot[find(same[v])];

    // Each piece's width axis: across its length (the major principal axis) and in its surface
    // (across its average normal). Not the middle principal axis: a bent blade curves out of its
    // plane further than it is wide.
    std::vector<double> sum(size_t(pieces) * 3, 0.0), sq(size_t(pieces) * 9, 0.0), normal(size_t(pieces) * 3, 0.0);
    std::vector<uint32_t> count(pieces, 0);
    for (size_t v = 0; v < n; ++v) {
        const double q[3] = {verts[v].px, verts[v].py, verts[v].pz};
        const uint32_t k = p.piece[v];
        ++count[k];
        normal[k * 3] += verts[v].nx;
        normal[k * 3 + 1] += verts[v].ny;
        normal[k * 3 + 2] += verts[v].nz;
        for (int a = 0; a < 3; ++a) {
            sum[k * 3 + a] += q[a];
            for (int b = 0; b < 3; ++b)
                sq[k * 9 + a * 3 + b] += q[a] * q[b];
        }
    }
    std::vector<float> center(size_t(pieces) * 3), axis(size_t(pieces) * 3);
    p.rank.resize(pieces);
    p.width.assign(pieces, 0.0f);
    for (uint32_t k = 0; k < pieces; ++k) {
        p.rank[k] = float(splitmix64(k) >> 40) / float(1ull << 24);
        const double inv = 1.0 / std::max(count[k], 1u);
        double c[3][3], m[3];
        for (int a = 0; a < 3; ++a)
            m[a] = sum[k * 3 + a] * inv;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                c[a][b] = sq[k * 9 + a * 3 + b] * inv - m[a] * m[b];
        double seed[3] = {0.57735, 0.57735, 0.57735}, major[3];
        majorAxis(c, seed, major);
        const double* nk = &normal[k * 3];
        double w[3] = {nk[1] * major[2] - nk[2] * major[1], nk[2] * major[0] - nk[0] * major[2], nk[0] * major[1] - nk[1] * major[0]};
        double wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (wl < 1e-9) {  // no usable normal (it averages out): any axis across the length
            w[0] = major[1] - major[2];
            w[1] = major[2] - major[0];
            w[2] = major[0] - major[1];
            wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (wl < 1e-9) {
                w[0] = 1.0;
                w[1] = w[2] = 0.0;
                wl = 1.0;
            }
        }
        for (int a = 0; a < 3; ++a) {
            center[k * 3 + a] = float(m[a]);
            axis[k * 3 + a] = float(w[a] / wl);
        }
    }
    for (size_t v = 0; v < n; ++v) {
        const uint32_t k = p.piece[v];
        const float d = (verts[v].px - center[k * 3]) * axis[k * 3] + (verts[v].py - center[k * 3 + 1]) * axis[k * 3 + 1] +
                        (verts[v].pz - center[k * 3 + 2]) * axis[k * 3 + 2];
        p.width[k] = std::max(p.width[k], 2.0f * std::fabs(d));
    }

    // The widened survivors of each level.
    p.original.resize(n);
    for (size_t v = 0; v < n; ++v)
        p.original[v] = uint32_t(v);
    for (uint32_t level = 1; level <= kPruneLevels; ++level) {
        const float keep = std::ldexp(1.0f, -int(level)), widen = std::ldexp(1.0f, int(level)) - 1.0f;
        p.copies[level].assign(n, ~0u);
        for (size_t v = 0; v < n; ++v) {
            const uint32_t k = p.piece[v];
            if (p.rank[k] >= keep)
                continue;
            SrcVertex c = verts[v];
            const float d = (c.px - center[k * 3]) * axis[k * 3] + (c.py - center[k * 3 + 1]) * axis[k * 3 + 1] +
                            (c.pz - center[k * 3 + 2]) * axis[k * 3 + 2];
            c.px += axis[k * 3] * d * widen;
            c.py += axis[k * 3 + 1] * d * widen;
            c.pz += axis[k * 3 + 2] * d * widen;
            p.copies[level][v] = uint32_t(verts.size());
            verts.push_back(c);
            p.piece.push_back(k);
            p.original.push_back(uint32_t(v));
        }
    }
    return true;
}

// clusterlod's simplifier override: a merged group of level `depth` clusters -> level depth + 1,
// the pieces that survive it, widened. Error, like the simplifier's (how far the surface moved):
// how far widening moved the survivors' edges, half their width times 2^L - 1, averaged over the
// group's triangles.
void prune(void* context, const unsigned int* indices, size_t indexCount, size_t, int depth, unsigned int* lod,
           size_t* lodCount, float* error)
{
    const Pruning& p = *static_cast<const Pruning*>(context);
    const uint32_t level = uint32_t(depth) + 1;
    *lodCount = 0;
    *error = 0.0f;
    if (level > kPruneLevels) {  // the last level: stuck (no further pruning)
        std::copy(indices, indices + indexCount, lod);
        *lodCount = indexCount;
        return;
    }
    const float keep = std::ldexp(1.0f, -int(level)), moved = 0.5f * (std::ldexp(1.0f, int(level)) - 1.0f);
    double width = 0.0;
    for (size_t t = 0; t + 2 < indexCount; t += 3) {
        const uint32_t k = p.piece[indices[t]];
        if (p.rank[k] >= keep)
            continue;
        for (int c = 0; c < 3; ++c)
            lod[(*lodCount)++] = p.copies[level][p.original[indices[t + c]]];
        width += p.width[k];
    }
    if (*lodCount)
        *error = float(width / double(*lodCount / 3)) * moved;
    if (*lodCount == 0 && indexCount >= 3) {  // keep the group non-empty (a DAG invariant)
        std::copy(indices, indices + 3, lod);
        *lodCount = 3;
    }
}

}  // namespace

BuiltMesh buildMesh(const SrcPrimitive& src, bool foliage)
{
    BuiltMesh out;

    // Exact-duplicate vertex removal keeps the simplifier's topology intact.
    std::vector<unsigned int> remap(src.vertices.size());
    const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), src.indices.data(), src.indices.size(),
                                                           src.vertices.data(), src.vertices.size(), sizeof(SrcVertex));
    std::vector<SrcVertex> verts(uniqueCount);
    std::vector<uint32_t> indices(src.indices.size());
    meshopt_remapVertexBuffer(verts.data(), src.vertices.data(), src.vertices.size(), sizeof(SrcVertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), src.indices.data(), src.indices.size(), remap.data());
    for (int a = 0; a < 3; ++a) {
        out.surfaceMin[a] = FLT_MAX;
        out.surfaceMax[a] = -FLT_MAX;
    }
    for (const SrcVertex& v : verts) {
        const float p[3] = {v.px, v.py, v.pz};
        for (int a = 0; a < 3; ++a) {
            out.surfaceMin[a] = std::min(out.surfaceMin[a], p[a]);
            out.surfaceMax[a] = std::max(out.surfaceMax[a], p[a]);
        }
    }
    Pruning pruning;
    const bool pruned = foliage && buildPruning(verts, indices, pruning);

    // Quantize positions to the mesh AABB (16 bits per axis).
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const SrcVertex& v : verts) {
        const float p[3] = {v.px, v.py, v.pz};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    float scale[3];
    for (int a = 0; a < 3; ++a)
        scale[a] = std::max(hi[a] - lo[a], 1e-6f) / 65535.0f;

    out.vertices.resize(verts.size());
    std::vector<SrcVertex> quantized = verts;  // dequantized copy: what the GPU will actually render
    for (size_t i = 0; i < verts.size(); ++i) {
        const SrcVertex& v = verts[i];
        const float p[3] = {v.px, v.py, v.pz};
        uint32_t q[3];
        float* dq[3] = {&quantized[i].px, &quantized[i].py, &quantized[i].pz};
        for (int a = 0; a < 3; ++a) {
            q[a] = uint32_t(std::clamp(std::lround((p[a] - lo[a]) / scale[a]), 0l, 65535l));
            *dq[a] = lo[a] + float(q[a]) * scale[a];
            out.maxQuantError = std::max(out.maxQuantError, std::fabs(*dq[a] - p[a]));
        }
        fpak::VertexGpu& g = out.vertices[i];
        g.posXY = q[0] | q[1] << 16;
        g.posZ = q[2] | (v.tw < 0.0f ? 1u : 0u) << 16;
        g.normalOct = octEncode(v.nx, v.ny, v.nz);
        g.tangentOct = octEncode(v.tx, v.ty, v.tz);
        g.u = v.u;
        g.v = v.v;
        if (src.skeleton >= 0) {
            // Weights to 8 bits summing to exactly 255 (largest remainders get the leftovers).
            fpak::SkinVertexGpu s = {};
            float rem[4];
            int sum = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = std::clamp(v.weights[k], 0.0f, 1.0f) * 255.0f;
                s.weights[k] = uint8_t(std::floor(w));
                rem[k] = w - std::floor(w);
                sum += s.weights[k];
                s.joints[k] = uint8_t(v.joints[k]);
            }
            for (int left = 255 - sum; left > 0; --left) {
                const int k = int(std::max_element(rem, rem + 4) - rem);
                ++s.weights[k];
                rem[k] = -1.0f;
            }
            out.skin.push_back(s);
        }
    }
    for (int a = 0; a < 3; ++a) {
        out.mesh.posOffset[a] = lo[a];
        out.mesh.posScale[a] = scale[a];
    }

    // Cluster LOD DAG. Normals are weighted; UV discontinuities are protected so the
    // permissive simplifier never collapses across seams.
    const float attributeWeights[3] = {0.5f, 0.5f, 0.5f};
    clodConfig config = clodDefaultConfig(fpak::kMaxClusterTriangles);
    config.max_vertices = fpak::kMaxClusterVertices;
    if (pruned) {
        config.simplify_override = prune;
        config.simplify_override_context = &pruning;
    } else if (foliage) {
        config.simplify_ratio = 1.0f;  // too few pieces to prune: every cluster is "stuck", full detail only
    }
    clodMesh mesh = {};
    mesh.indices = indices.data();
    mesh.index_count = indices.size();
    mesh.vertex_count = quantized.size();
    mesh.vertex_positions = &quantized[0].px;
    mesh.vertex_positions_stride = sizeof(SrcVertex);
    mesh.vertex_attributes = &quantized[0].nx;
    mesh.vertex_attributes_stride = sizeof(SrcVertex);
    mesh.attribute_weights = attributeWeights;
    mesh.attribute_count = 3;
    mesh.attribute_protect_mask = (1u << 3) | (1u << 4);  // u, v

    std::vector<clodGroup> groups;
    std::vector<uint32_t> groupFirstCluster, groupClusterCount;
    std::vector<int> clusterRefined;
    unsigned int localVerts[fpak::kMaxClusterVertices];
    unsigned char localTris[fpak::kMaxClusterTriangles * 3];

    clodBuild(config, mesh, [&](clodGroup group, const clodCluster* clusters, size_t count) -> int {
        const int groupId = int(groups.size());
        groups.push_back(group);
        groupFirstCluster.push_back(uint32_t(out.clusters.size()));
        groupClusterCount.push_back(uint32_t(count));
        out.levels = std::max(out.levels, uint32_t(group.depth) + 1);

        for (size_t c = 0; c < count; ++c) {
            const clodCluster& cl = clusters[c];
            const size_t triCount = cl.index_count / 3;
            const size_t vertCount = clodLocalIndices(localVerts, localTris, cl.indices, cl.index_count);

            fpak::ClusterGpu g = {};
            const meshopt_Bounds b = meshopt_computeClusterBounds(cl.indices, cl.index_count, &quantized[0].px,
                                                                  quantized.size(), sizeof(SrcVertex));
            g.cullSphere[0] = b.center[0];
            g.cullSphere[1] = b.center[1];
            g.cullSphere[2] = b.center[2];
            g.cullSphere[3] = b.radius;
            if (cl.refined >= 0) {
                setSphere(g.selfLod, groups[cl.refined].simplified);
                g.selfError = groups[cl.refined].simplified.error;
            } else {
                setSphere(g.selfLod, cl.bounds);
                g.selfError = 0.0f;
            }
            setSphere(g.parentLod, group.simplified);
            g.parentError = group.simplified.error;
            g.vertexOffset = uint32_t(out.clusterVertices.size());
            g.triangleOffset = uint32_t(out.clusterTriangles.size());
            g.counts = uint32_t(vertCount) | uint32_t(triCount) << 8 | (src.material & 0xffff) << 16;
            g.cone = uint32_t(uint8_t(b.cone_axis_s8[0])) | uint32_t(uint8_t(b.cone_axis_s8[1])) << 8 |
                     uint32_t(uint8_t(b.cone_axis_s8[2])) << 16 | uint32_t(uint8_t(b.cone_cutoff_s8)) << 24;
            g.depth = uint32_t(group.depth);
            out.clusters.push_back(g);
            clusterRefined.push_back(cl.refined);

            out.clusterVertices.insert(out.clusterVertices.end(), localVerts, localVerts + vertCount);
            for (size_t t = 0; t < triCount; ++t)
                out.clusterTriangles.push_back(uint32_t(localTris[t * 3]) | uint32_t(localTris[t * 3 + 1]) << 8 |
                                               uint32_t(localTris[t * 3 + 2]) << 16);
            out.totalTriangles += uint32_t(triCount);
            if (cl.refined < 0)
                out.leafTriangles += uint32_t(triCount);
        }
        return groupId;
    });

    // BVH over groups: nodes [0, levels) are the per-level roots.
    std::vector<clodNode> nodes(clodBuildHierarchyBound(groups.size(), kLodNodeWidth, out.levels));
    nodes.resize(clodBuildHierarchy(nodes.data(), groups.data(), groups.size(), kLodNodeWidth, out.levels));
    out.lodNodes.reserve(nodes.size());
    for (const clodNode& n : nodes) {
        fpak::LodNodeGpu g = {};
        setSphere(g.sphere, n.bounds);
        g.error = n.bounds.error;
        g.group = n.group;
        if (n.group >= 0) {
            g.childOffset = groupFirstCluster[n.group];
            g.childCount = groupClusterCount[n.group];
        } else {
            g.childOffset = n.child_offset;
            g.childCount = n.child_count;
        }
        out.lodNodes.push_back(g);
    }

    out.mesh.clusterCount = uint32_t(out.clusters.size());
    out.mesh.lodLevels = out.levels;
    return out;
}

size_t verifyMesh(const SrcPrimitive& src, const BuiltMesh& m, uint32_t meshIndex)
{
    size_t violations = 0;
    auto fail = [&](const char* what, size_t cluster) {
        if (violations < 8)
            std::fprintf(stderr, "verify: mesh %u cluster %zu: %s\n", meshIndex, cluster, what);
        ++violations;
    };
    (void)src;
    // Half a quantization step, plus the float rounding of offset + q * scale at this magnitude.
    float magnitude = 0.0f, step = 0.0f;
    for (int a = 0; a < 3; ++a) {
        magnitude = std::max({magnitude, std::fabs(m.mesh.posOffset[a]),
                              std::fabs(m.mesh.posOffset[a] + 65535.0f * m.mesh.posScale[a])});
        step = std::max(step, m.mesh.posScale[a]);
    }
    if (m.maxQuantError > 0.5f * step + 4.0f * FLT_EPSILON * magnitude)
        fail("quantization error exceeds half a step", 0);

    for (size_t c = 0; c < m.clusters.size(); ++c) {
        const fpak::ClusterGpu& cl = m.clusters[c];
        const uint32_t vc = cl.counts & 0xff, tc = (cl.counts >> 8) & 0xff;
        if (vc == 0 || vc > fpak::kMaxClusterVertices || tc == 0 || tc > fpak::kMaxClusterTriangles)
            fail("cluster vertex/triangle count out of range", c);
        if (!(cl.selfError <= cl.parentError))
            fail("DAG error not monotonic (self > parent)", c);
        for (uint32_t t = 0; t < tc; ++t) {
            const uint32_t tri = m.clusterTriangles[cl.triangleOffset + t];
            if ((tri & 0xff) >= vc || ((tri >> 8) & 0xff) >= vc || ((tri >> 16) & 0xff) >= vc)
                fail("triangle references a vertex outside the cluster", c);
        }
        const float r = cl.cullSphere[3] * 1.0001f + 1e-5f;
        for (uint32_t v = 0; v < vc; ++v) {
            const fpak::VertexGpu& g = m.vertices[m.clusterVertices[cl.vertexOffset + v]];
            const float p[3] = {m.mesh.posOffset[0] + float(g.posXY & 0xffff) * m.mesh.posScale[0],
                                m.mesh.posOffset[1] + float(g.posXY >> 16) * m.mesh.posScale[1],
                                m.mesh.posOffset[2] + float(g.posZ & 0xffff) * m.mesh.posScale[2]};
            const float d[3] = {p[0] - cl.cullSphere[0], p[1] - cl.cullSphere[1], p[2] - cl.cullSphere[2]};
            if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > r) {
                fail("vertex outside cluster cull sphere", c);
                break;
            }
        }
    }
    return violations;
}

}  // namespace fx::cook
