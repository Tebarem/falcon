#include "level/pak_mesh.h"

#include "assets/scene_pak.h"
#include "core/json.h"
#include "level/mesh_ops.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace fx::level {

bool meshFromPak(const ScenePak& pak, uint32_t mesh, PolyMesh& out)
{
    out = {};
    if (mesh >= pak.meshes().size())
        return false;
    using S = fpak::Section;
    uint32_t clusterCount = 0, vertexCount = 0, clusterVertexCount = 0, triangleCount = 0;
    const auto* clusters = pak.view<fpak::ClusterGpu>(S::Clusters, &clusterCount);
    const auto* vertices = pak.view<fpak::VertexGpu>(S::Vertices, &vertexCount);
    const auto* clusterVertices = pak.view<uint32_t>(S::ClusterVertices, &clusterVertexCount);
    const auto* triangles = pak.view<uint32_t>(S::ClusterTriangles, &triangleCount);
    const fpak::MeshGpu& m = pak.meshes()[mesh];
    if (uint64_t(m.clusterOffset) + m.clusterCount > clusterCount)
        return false;
    std::unordered_map<uint32_t, uint32_t> slotOf;  // scene material -> slot
    std::unordered_map<uint64_t, uint32_t> welded;  // quantized position -> vertex
    for (uint32_t c = m.clusterOffset; c < m.clusterOffset + m.clusterCount; ++c) {
        const fpak::ClusterGpu& cl = clusters[c];
        if (cl.depth != 0)
            continue;  // (coarser levels of the DAG)
        const uint32_t nv = cl.counts & 0xff, nt = cl.counts >> 8 & 0xff, material = cl.counts >> 16;
        if (uint64_t(cl.vertexOffset) + nv > clusterVertexCount || uint64_t(cl.triangleOffset) + nt > triangleCount)
            return false;
        const auto [slot, added] = slotOf.try_emplace(material, uint32_t(out.materials.size()));
        if (added)
            out.materials.push_back("scene:" + std::to_string(material));
        for (uint32_t t = 0; t < nt; ++t) {
            const uint32_t packed = triangles[cl.triangleOffset + t];
            uint32_t loop[3];
            float2 uv[3];
            for (int k = 0; k < 3; ++k) {
                const uint32_t g = clusterVertices[cl.vertexOffset + (packed >> (8 * k) & 0xff)];
                if (g >= vertexCount)
                    return false;
                const fpak::VertexGpu& v = vertices[g];
                const uint64_t q = uint64_t(v.posXY) | uint64_t(v.posZ & 0xffff) << 32;
                const auto [at, fresh] = welded.try_emplace(q, uint32_t(out.positions.size()));
                if (fresh)
                    out.positions.push_back({m.posOffset[0] + float(v.posXY & 0xffff) * m.posScale[0],
                                             m.posOffset[1] + float(v.posXY >> 16) * m.posScale[1],
                                             m.posOffset[2] + float(v.posZ & 0xffff) * m.posScale[2]});
                loop[k] = at->second;
                uv[k] = {v.u, v.v};
            }
            if (loop[0] == loop[1] || loop[1] == loop[2] || loop[0] == loop[2])
                continue;  // (welded to nothing)
            const uint32_t f = out.addFace({loop[0], loop[1], loop[2]}, slot->second);
            out.faces[f].uv.mode = UvMode::Manual;
            out.cornerUvs.resize(out.corners.size());
            for (int k = 0; k < 3; ++k)
                out.cornerUvs[out.faces[f].first + k] = uv[k];
        }
    }
    if (out.faces.empty())
        return false;
    OpResult smooth;
    if (runOp("autoSmooth", out, Json::parse(R"({"angle":40})"), smooth))
        out = std::move(smooth.mesh);
    return out.valid();
}

}  // namespace fx::level
