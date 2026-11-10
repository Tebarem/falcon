// Editor picking of transparent surfaces (transparent.slang draws them; they are not in the
// visibility buffer that editor_pick reads). After cs_pick: the pixel's camera ray against the
// triangles of this frame's transparent clusters, one group; the nearest hit replaces cs_pick's
// instance when it is in front of the opaque depth there.

import scene_types;
import skinning;
import culling;
import view_common;

struct PickUniforms
{
    float4 cameraPos;      // w: near plane
    float4 cameraRight;    // w: 1 / P00
    float4 cameraUp;       // w: 1 / P11
    float4 cameraForward;
    uint4 pixel;           // x, y, width, height
};

[[vk::binding(0, 0)]] Sampler2D<float> depth : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer draws : register(t1, space0);     // sorted: instance, cluster, first, count
[[vk::binding(2, 0)]] ByteAddressBuffer scene : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer skin : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer counters : register(t4, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer result : register(u0, space1);  // cs_pick's instance slot
[[vk::binding(0, 2)]] ConstantBuffer<PickUniforms> u : register(b0, space2);

groupshared uint gsBest;

float3 worldVertex(InstanceGpu inst, MeshGpu mesh, ClusterGpu cl, uint local)
{
    return transformPoint(inst, loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, local), false, true).position);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_pick_transparent(uint3 thread : SV_GroupThreadID)
{
    const uint t = thread.x;
    if (t == 0)
        gsBest = 0xffffffffu;
    GroupMemoryBarrierWithGroupSync();
    const float2 size = float2(u.pixel.zw);
    const float3 origin = u.cameraPos.xyz;
    const float3 dir = cameraRay(float2(u.pixel.xy) + 0.5, size, float2(0.0, 0.0), u.cameraForward.xyz, u.cameraRight, u.cameraUp);
    const uint n = min(counters.Load(kCtrTransparentVisible * 4), kMaxTransparent);
    for (uint r = 0; r < n; ++r)
    {
        const uint4 d = draws.Load4(r * 16);
        if (t >= d.w)
            continue;
        const InstanceGpu inst = loadInstance(scene, d.x);
        const MeshGpu mesh = loadMesh(scene, inst.mesh);
        const ClusterGpu cl = loadCluster(scene, d.y);
        for (uint i = t; i < d.w; i += 256)
        {
            const uint packed = loadClusterTriangle(scene, cl, i);
            const float3 p0 = worldVertex(inst, mesh, cl, packed & 0xff);
            const float3 e1 = worldVertex(inst, mesh, cl, (packed >> 8) & 0xff) - p0;
            const float3 e2 = worldVertex(inst, mesh, cl, (packed >> 16) & 0xff) - p0;
            // Moller-Trumbore, both sides.
            const float3 pv = cross(dir, e2);
            const float det = dot(e1, pv);
            if (abs(det) < 1e-12)
                continue;
            const float3 tv = origin - p0;
            const float a = dot(tv, pv) / det;
            const float3 qv = cross(tv, e1);
            const float b = dot(dir, qv) / det;
            const float hit = dot(e2, qv) / det;
            if (a < 0.0 || b < 0.0 || a + b > 1.0 || hit <= 0.0)
                continue;
            uint unused;
            InterlockedMin(gsBest, (asuint(hit) & 0xfffff000u) | r, unused);  // (the draw in the low 12 bits)
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (t == 0 && gsBest != 0xffffffffu)
    {
        const float hit = asfloat(gsBest & 0xfffff000u);
        const float raw = depth.SampleLevel((float2(u.pixel.xy) + 0.5) / size, 0.0);
        const float opaque = raw > 0.0 ? (u.cameraPos.w / raw) / max(dot(dir, u.cameraForward.xyz), 1e-6) : 1e30;
        if (hit < opaque)
            result.Store3(0, uint3(draws.Load((gsBest & 0xfffu) * 16), 0xffffffffu, 0xffffffffu));
    }
}
