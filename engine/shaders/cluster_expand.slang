// Visible clusters -> index buffer. Each draw (pass 1/2, opaque/masked) has an index range of
// 128 triangles per cluster it draws, but only real triangles go in: each cluster takes the
// next run of its draw's range (one atomic per cluster) and records where the run starts in
// triangleBase, so the raster turns SV_PrimitiveID back into the cluster-local triangle.
// Padding every cluster to 128 triangles (degenerate) made the draws 40% longer in Zero-Day,
// and the raster's cost is mostly per primitive. Index value: visibleIndex << 7 | cluster-local
// vertex.
//
// Back faces of single-sided materials go in as degenerate triangles (the raster draws both
// sides, for double-sided materials). Drawn, they tied in depth with the front face of a
// back-to-back copy (curtains), and the draw order, which follows the culling's atomics, picked
// the winner: pixels changed from run to run. Degenerate keeps SV_PrimitiveID's numbering.

import scene_types;
import skinning;
import culling;

struct ExpandUniforms
{
    uint pass;  // 0: clusters [0, n1); 1: clusters [n1, n1 + n2)
    uint maxVisible;
    uint backfaces;  // 1: drop single-sided materials' back faces (backface culling on)
    uint pad0;
    float4 eye;      // the raster camera's position
};

[[vk::binding(0, 0)]] ByteAddressBuffer visible : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer materials : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer skin : register(t3, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer counters : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer indices : register(u1, space1);
[[vk::binding(2, 1)]] RWByteAddressBuffer triangleBase : register(u2, space1);  // per visible slot
[[vk::binding(0, 2)]] ConstantBuffer<ExpandUniforms> expand : register(b0, space2);

groupshared uint gsFirst;

// World position of a cluster-local vertex, as the raster's vertex stage places it.
float3 worldVertex(InstanceGpu inst, MeshGpu mesh, ClusterGpu cl, uint local)
{
    return transformPoint(inst, loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, local), false, true).position);
}

[shader("compute")]
[numthreads(128, 1, 1)]
void cs_expand(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    // Group -> visible slot: opaque clusters first, then the masked range (stored top-down).
    const VisibleCounts c = loadVisibleCounts(counters, expand.maxVisible);
    const uint opaque = expand.pass == 0 ? c.opaque1 : c.opaque2;
    const bool masked = group.x >= opaque;
    const uint vis = !masked ? (expand.pass == 0 ? 0 : c.opaque1) + group.x
                             : expand.maxVisible - 1 - (expand.pass == 0 ? 0 : c.masked1) - (group.x - opaque);
    // The draw's first index (as cs_args_draw1/2 give it), in clusters.
    const uint drawStart = !masked ? (expand.pass == 0 ? 0 : c.opaque1)
                                   : expand.maxVisible - c.masked1 - (expand.pass == 0 ? 0 : c.masked2);
    const uint2 item = visible.Load2(vis * 8);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const uint triCount = clusterTriangleCount(cl);
    const uint t = thread.x;
    if (t == 0)
    {
        uint first;
        counters.InterlockedAdd((kCtrPacked + expand.pass * 2 + (masked ? 1 : 0)) * 4, triCount, first);
        triangleBase.Store(vis * 4, first);
        gsFirst = first;
        counters.InterlockedAdd((expand.pass == 0 ? kCtrTriangles1 : kCtrTriangles2) * 4, triCount);
    }
    GroupMemoryBarrierWithGroupSync();
    if (t < triCount)
    {
        const uint key = vis << 7;
        const uint packed = loadClusterTriangle(scene, cl, t);
        uint3 local = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
        if (expand.backfaces != 0)
        {
            const InstanceGpu inst = loadInstance(scene, item.x & ~kVisibleSecondPassBit);
            const MaterialGpu m = materials.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
            if ((m.flags & 4u) == 0)  // kMaterialDoubleSided
            {
                const MeshGpu mesh = loadMesh(scene, inst.mesh);
                const float3 p0 = worldVertex(inst, mesh, cl, local.x);
                const float3 p1 = worldVertex(inst, mesh, cl, local.y);
                const float3 p2 = worldVertex(inst, mesh, cl, local.z);
                // Front: the side cross(p1 - p0, p2 - p0) faces in mesh space (the cones' convention);
                // a mirroring instance turns the world-space cross product around.
                const float mirror = dot(inst.row0.xyz, cross(inst.row1.xyz, inst.row2.xyz)) < 0.0 ? -1.0 : 1.0;
                if (dot(cross(p1 - p0, p2 - p0) * mirror, p0 - expand.eye.xyz) > 0.0)
                    local = local.xxx;
            }
        }
        indices.Store3((drawStart * kIndicesPerCluster + (gsFirst + t) * 3) * 4, key | local);
    }
}
