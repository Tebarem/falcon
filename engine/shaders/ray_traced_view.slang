// Debug view 14 (Ray Traced): a camera ray per pixel through the scene's BVH (ray_trace.slang),
// each hit coloured by its cluster and triangle exactly as debug view 4 (Triangles) colours the
// rasterized ones, and the sky as the debug views draw it. With the finest LOD rasterized
// (--lod-pixels 0) the two views match: a check of the tracer against the raster.

import scene_types;
import ray_trace;
import sky;
import view_common;
import vt;

struct RayViewUniforms
{
    float4 cameraPos;
    float4 cameraRight;    // w: 1 / P00
    float4 cameraUp;       // w: 1 / P11
    float4 cameraForward;
    float4 jitter;         // xy: NDC jitter
    uint4 params;          // x, y: size; z: alpha-masked holes; w: VT page-table byte offset
};

[[vk::binding(0, 0)]] Sampler2DArray<float4> vtAlbedo : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer rt : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer scene : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer materialData : register(t3, space0);  // materials, then the page table
[[vk::binding(0, 1)]] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<RayViewUniforms> u : register(b0, space2);

// Alpha-masked materials' holes, at a coarse mip of their colour's alpha.
struct AlphaFilter : IRtFilter
{
    bool accept(uint instance, uint cluster, uint triangle, float2 bary)
    {
        if (u.params.z == 0)
            return true;
        const InstanceGpu inst = loadInstance(scene, instance);
        const ClusterGpu cl = loadCluster(scene, cluster);
        const MaterialGpu m = materialData.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
        if ((m.flags & 1u) == 0 || m.vtPageTable == 0xffffffffu)  // kMaterialAlphaMask
            return true;
        const MeshGpu mesh = loadMesh(scene, inst.mesh);
        const uint tri = loadClusterTriangle(scene, cl, triangle);
        float2 uv = 0.0;
        [unroll]
        for (uint k = 0; k < 3; ++k)
        {
            const DecodedVertex v = loadVertex(scene, mesh, loadClusterVertex(scene, cl, (tri >> (8 * k)) & 0xff));
            uv += v.uv * (k == 0 ? 1.0 - bary.x - bary.y : (k == 1 ? bary.x : bary.y));
        }
        const uint mips = vtMipCount(m);
        const VtPage page = vtFind(materialData, u.params.w, m, frac(uv), mips > 3 ? mips - 3 : 0);
        const float alpha = vtAlbedo.SampleLevel(float3(page.uv, float(page.layer)), 0.0).a * m.baseColor.a;
        return alpha >= m.alphaCutoff;
    }
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_ray_view(uint3 tid : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    const uint2 size = u.params.xy;
    const float2 pixel = float2(tid.xy) + 0.5;
    const float3 dir = cameraRay(pixel, float2(size), u.jitter.xy, u.cameraForward.xyz, u.cameraRight, u.cameraUp);
    AlphaFilter filter;
    const RtHit hit = rtTraceClosest(rt, u.cameraPos.xyz, dir, 1e30, lane, filter);
    if (tid.x >= size.x || tid.y >= size.y)
        return;
    output[tid.xy] = float4(hit.hit ? hashColor(hit.cluster * 131u + hit.triangle) : skyGradient(dir), 1.0);
}
