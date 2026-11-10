// Transparent (alpha-blend) clusters, after the opaque visibility passes (render/transparent_renderer.h):
//
//   cs_cull_transparent    cs_cull_first's blend candidates vs this frame's final HiZ, with their
//                          sort key (view depth): the transparent list
//   cs_sort_transparent    one group: the list sorted farthest first (bitonic, in groupshared), each
//                          cluster's first triangle in the draw (a scan of their triangle counts),
//                          and the expand and draw arguments
//   cs_expand_transparent  sorted clusters -> index buffer: a single-sided material's back faces as
//                          degenerate triangles, a double-sided one's back faces first (the far side of
//                          a closed shape draws before its near side)
//
// Everything is in sort order, so the draw (transparent.slang) blends back to front with no
// atomics deciding the order: the frame is the same every run.

import scene_types;
import skinning;
import culling;

[[vk::binding(0, 0)]] ByteAddressBuffer workList : register(t0, space0);  // candidates, list or sorted draws
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer materials : register(t2, space0);  // VT material data
[[vk::binding(3, 0)]] ByteAddressBuffer hiz : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer skin : register(t4, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer counters : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer rwOut : register(u1, space1);   // the list, sorted draws or indices
[[vk::binding(2, 1)]] RWByteAddressBuffer rwArgs : register(u2, space1);  // cs_sort_transparent: indirect args
[[vk::binding(0, 2)]] ConstantBuffer<CullUniforms> cull : register(b0, space2);

// ---- cull: list entry = (instance, cluster, key.x, key.y). Key: farther first (the view depth's
// float bits inverted), then a hash of the cluster and instance (a tie in depth still sorts the same
// every run), the low 12 bits the entry's slot (what the sort carries).

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull_transparent(uint3 tid : SV_DispatchThreadID)
{
    const uint count = min(counters.Load(kCtrTransparentCandidates * 4), kMaxTransparentCandidates);
    if (tid.x >= count)
        return;
    const uint2 item = workList.Load2(tid.x * 8);
    const InstanceGpu inst = loadInstance(scene, item.x);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const float4 sphere = isSkinned(inst) ? skinnedClusterSphere(skin, inst, item.y) : cl.cullSphere;
    const float3 center = transformPoint(inst, sphere.xyz);
    const float radius = sphere.w * maxScale(inst);
    if ((cull.params.x & kFlagOcclusion) != 0 && occludedByHiz(cull, hiz, cull.view, center, radius))
        return;
    uint slot;
    counters.InterlockedAdd(kCtrTransparentVisible * 4, 1, slot);
    if (slot >= kMaxTransparent)
    {
        counters.InterlockedOr(kCtrOverflow * 4, kOverflowTransparent);
        return;
    }
    const float depth = max(-mul(cull.view, float4(center, 1.0)).z, 0.0);
    const uint hash = (item.y * 2654435761u) ^ (item.x * 40503u);
    rwOut.Store4(slot * 16, uint4(item.x, item.y, ~asuint(depth), (hash << 12) | slot));
}

// ---- sort: 4096 keys of 8 bytes is 32 KB of groupshared memory (the limit); the scan of the
// triangle counts reuses it once the sorted keys are in registers.

groupshared uint2 gsKeys[kMaxTransparent];

bool keyLess(uint2 a, uint2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); }

[shader("compute")]
[numthreads(1024, 1, 1)]
void cs_sort_transparent(uint3 thread : SV_GroupThreadID)
{
    const uint t = thread.x;
    const uint n = min(counters.Load(kCtrTransparentVisible * 4), kMaxTransparent);
    uint size = 1;
    while (size < n)
        size <<= 1;
    for (uint i = t; i < size; i += 1024)
        gsKeys[i] = i < n ? workList.Load2(i * 16 + 8) : uint2(0xffffffffu, 0xffffffffu);
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2; k <= size; k <<= 1)
    {
        for (uint j = k >> 1; j > 0; j >>= 1)
        {
            for (uint i = t; i < size; i += 1024)
            {
                const uint l = i ^ j;
                if (l > i)
                {
                    const uint2 a = gsKeys[i], b = gsKeys[l];
                    if (keyLess(b, a) == ((i & k) == 0))
                    {
                        gsKeys[i] = b;
                        gsKeys[l] = a;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // This thread's four sorted entries (ranks 4t .. 4t + 3) and their triangle counts.
    uint4 entries[4];
    uint sum = 0;
    for (uint q = 0; q < 4; ++q)
    {
        const uint r = t * 4 + q;
        entries[q] = uint4(0, 0, 0, 0);
        if (r < n)
        {
            const uint4 e = workList.Load4((gsKeys[r].y & 0xfffu) * 16);
            entries[q] = uint4(e.x, e.y, 0, clusterTriangleCount(loadCluster(scene, e.y)));
            sum += entries[q].w;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    gsKeys[t].x = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < 1024; offset <<= 1)  // inclusive scan (Hillis-Steele)
    {
        const uint add = t >= offset ? gsKeys[t - offset].x : 0;
        GroupMemoryBarrierWithGroupSync();
        gsKeys[t].x += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint first = gsKeys[t].x - sum;
    for (uint q = 0; q < 4; ++q)
    {
        const uint r = t * 4 + q;
        if (r < n)
        {
            rwOut.Store4(r * 16, uint4(entries[q].x, entries[q].y, first, entries[q].w));
            first += entries[q].w;
        }
    }
    if (t == 1023)
    {
        rwArgs.Store3(kArgExpandTransparent, uint3(n, 1, 1));
        rwArgs.Store4(kArgDrawTransparent, uint4(gsKeys[1023].x * 3, 1, 0, 0));
        rwArgs.Store(kArgDrawTransparent + 16, 0);
    }
}

// ---- expand: sorted draw r = (instance, cluster, first triangle, triangle count). Index value:
// r << 7 | cluster-local vertex.

groupshared uint gsBack[128];

// World position of a cluster-local vertex, as the draw's vertex stage places it.
float3 worldVertex(InstanceGpu inst, MeshGpu mesh, ClusterGpu cl, uint local)
{
    return transformPoint(inst, loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, local), false, true).position);
}

[shader("compute")]
[numthreads(128, 1, 1)]
void cs_expand_transparent(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint4 d = workList.Load4(group.x * 16);
    const InstanceGpu inst = loadInstance(scene, d.x);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const ClusterGpu cl = loadCluster(scene, d.y);
    const MaterialGpu m = materials.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
    const bool doubleSided = (m.flags & 4u) != 0;  // kMaterialDoubleSided
    const uint t = thread.x;
    uint3 local = uint3(0, 0, 0);
    bool back = false;
    if (t < d.w)
    {
        const uint packed = loadClusterTriangle(scene, cl, t);
        local = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
        const float3 p0 = worldVertex(inst, mesh, cl, local.x);
        const float3 p1 = worldVertex(inst, mesh, cl, local.y);
        const float3 p2 = worldVertex(inst, mesh, cl, local.z);
        // Front: the side cross(p1 - p0, p2 - p0) faces in mesh space (cluster_expand.slang).
        const float mirror = dot(inst.row0.xyz, cross(inst.row1.xyz, inst.row2.xyz)) < 0.0 ? -1.0 : 1.0;
        back = dot(cross(p1 - p0, p2 - p0) * mirror, p0 - cull.cameraPos.xyz) > 0.0;
    }
    uint position = t;
    if (doubleSided)  // (the same for the whole group: one material per cluster)
    {
        gsBack[t] = back ? 1u : 0u;
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 1; offset < 128; offset <<= 1)  // inclusive scan
        {
            const uint add = t >= offset ? gsBack[t - offset] : 0;
            GroupMemoryBarrierWithGroupSync();
            gsBack[t] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        const uint backsBefore = gsBack[t] - (back ? 1u : 0u);
        position = back ? backsBefore : gsBack[127] + t - backsBefore;
    }
    else if (back)
    {
        local = local.xxx;
    }
    if (t < d.w)
        rwOut.Store3((d.z + position) * 12, (group.x << 7) | local);
}
