// Vertex functions (V1): the instances whose material has one (near the camera) are deformed like
// skinned ones (render/skinning.h, deform records): once a frame, before anything is drawn, this
// writes their vertices into the skinned-vertex buffer (the generation of this frame; last frame's
// stays, for motion vectors), and the skinning's bounds pass then bounds their clusters. Every pass
// reads them through loadInstanceVertex, as it reads a character's.
//
// One group per cluster of a deform record (all its DAG levels): the cluster's material says
// whether its vertices move (its surface shader's vertex function) and with which params. A vertex
// in clusters of two levels is written by both, the same.
//
// The editor compiles this against the project's surface shaders (Cache/Shaders/deform.fshader);
// the engine's own build (no vertex functions) writes the vertices where they are.

import scene_types;
import skinning;
import falcon_surface;
import project_surfaces;

struct DeformUniforms
{
    uint4 params;  // first deform record, deform record count, materials' params offset (bytes)
    float4 time;   // seconds
};

[[vk::binding(0, 0)]] ByteAddressBuffer scene : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer materialData : register(t1, space0);  // materials, page table, params
[[vk::binding(0, 1)]] RWByteAddressBuffer skin : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<DeformUniforms> u : register(b0, space2);

uint snorm16(float v) { return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xffff; }

uint octEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-12);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return snorm16(e.x) | snorm16(e.y) << 16;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_deform(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    if (group.y >= u.params.y)
        return;
    const SkinInstanceGpu s = loadSkinInstance(skin, u.params.x + group.y);
    if (group.x >= s.clusterCount)
        return;
    const InstanceGpu inst = loadInstance(scene, s.pad1);  // (a deform record's instance slot)
    const MeshGpu mesh = loadMesh(scene, s.mesh);
    const ClusterGpu cl = loadCluster(scene, s.clusterOffset + group.x);
    const uint material = instanceMaterial(inst, cl);
    const MaterialGpu m = materialData.Load<MaterialGpu>(material * kMaterialStride);
    const uint shader = (m.flags >> 8) & 0xffu;
    const bool moves = kProjectHasVertex && shader != 0 && (m.flags & 32u) != 0;  // kMaterialVertex
    const float4 params = asfloat(materialData.Load4(u.params.z + material * 32));
    // World offsets back into the mesh's space: the instance's 3x3, inverted (adjugate / det).
    const float3 r0 = inst.row0.xyz, r1 = inst.row1.xyz, r2 = inst.row2.xyz;
    const float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
    const float det = dot(r0, c0);
    const float3x3 inverse = transpose(float3x3(c0, c1, c2)) / (abs(det) > 1e-20 ? det : 1.0);
    const float3 origin = float3(inst.row0.w, inst.row1.w, inst.row2.w);
    uint h = s.pad1 * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    const float random = float((h >> 22u) ^ h) / 4294967295.0;

    const uint vertices = skinSection(skin, kSkinHdrVertices);
    const uint n = clusterVertexCount(cl);
    for (uint i = thread.x; i < n; i += 64)
    {
        const uint g = loadClusterVertex(scene, cl, i);
        const DecodedVertex v = loadVertex(scene, mesh, g);
        float3 position = v.position, normal = v.normal;
        if (moves)
        {
            VertexIn vi;
            vi.position = v.position;
            vi.world = transformPoint(inst, v.position);
            vi.normal = normalize(transformDirection(inst, v.normal));
            vi.uv = v.uv;
            vi.params = params;
            vi.time = u.time.x;
            vi.origin = origin;
            vi.random = random;
            VertexOut vo;
            vo.offset = 0.0;
            vo.normal = vi.normal;
            projectVertex(shader, vi, vo);
            position += mul(inverse, vo.offset);
            // A world normal into the mesh's space: by the transform's transpose (normals go by the
            // inverse transpose; the mesh's normal through the instance is its inverse's transpose).
            normal = mul(transpose(float3x3(r0, r1, r2)), vo.normal);
            normal = dot(normal, normal) > 1e-12 ? normalize(normal) : v.normal;
        }
        const uint out = vertices + (s.vertexBase + g - s.firstVertex) * kSkinVertexStride;
        skin.Store3(out, asuint(position));
        skin.Store2(out + 12, uint2(octEncode(normal), octEncode(v.tangent.xyz)));
        skin.Store3(out + kSkinViewPosition, asuint(position));
    }
}
