// Visibility buffer raster: positions only. Output = (visibleIndex << 7 | triangle) + 1,
// 0 = empty (clear value), so the resolve pass can rebuild everything else. The triangle is
// SV_PrimitiveID less where its cluster's run starts in the draw (triangleBase, cluster_expand):
// the vertex stage passes id = (visibleIndex << 7) + 1 - that, the fragment adds the primitive.

import scene_types;
import skinning;
import vt;

struct VisUniforms
{
    float4x4 viewProj;
};

[[vk::binding(0, 0)]] ByteAddressBuffer visible : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer skin : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer triangleBase : register(t3, space0);
[[vk::binding(0, 1)]] ConstantBuffer<VisUniforms> vis : register(b0, space1);

struct VsOut
{
    float4 position : SV_Position;
    nointerpolation uint id : VISIBLE;  // the visibility id less SV_PrimitiveID
};

uint visibilityBase(uint visibleIndex) { return (visibleIndex << 7) + 1 - triangleBase.Load(visibleIndex * 4); }

[shader("vertex")]
VsOut vs_main(uint index : SV_VertexID)
{
    const uint visibleIndex = index >> 7;
    const uint2 item = visible.Load2(visibleIndex * 8);
    const InstanceGpu inst = loadInstance(scene, (item.x & 0x7fffffffu));
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const uint vertex = loadClusterVertex(scene, cl, (index & 127));
    const DecodedVertex v = loadInstanceVertex(scene, skin, inst, mesh, vertex, false, true);  // (as the eye sees it)
    VsOut o;
    o.position = mul(vis.viewProj, float4(transformPoint(inst, v.position), 1.0));
    o.id = visibilityBase(visibleIndex);
    return o;
}

[shader("fragment")]
uint fs_main(VsOut i, uint primitive : SV_PrimitiveID) : SV_Target0
{
    return i.id + primitive;
}

// ---- alpha-masked clusters: a separate pipeline, so the opaque draw keeps early depth.

struct MaskUniforms
{
    uint pageTableOffset;
    float mipBias;
    uint pad0, pad1;
};

[[vk::binding(0, 2)]] Sampler2DArray<float4> vtAlbedo : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] ByteAddressBuffer materialData : register(t1, space2);
[[vk::binding(0, 3)]] ConstantBuffer<MaskUniforms> mask : register(b0, space3);

struct MaskedVsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint id : VISIBLE;
    nointerpolation uint material : MATERIAL;
};

[shader("vertex")]
MaskedVsOut vs_masked(uint index : SV_VertexID)
{
    const uint visibleIndex = index >> 7;
    const uint2 item = visible.Load2(visibleIndex * 8);
    const InstanceGpu inst = loadInstance(scene, (item.x & 0x7fffffffu));
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const uint vertex = loadClusterVertex(scene, cl, (index & 127));
    const DecodedVertex v = loadInstanceVertex(scene, skin, inst, mesh, vertex, false, true);  // (as the eye sees it)
    MaskedVsOut o;
    o.position = mul(vis.viewProj, float4(transformPoint(inst, v.position), 1.0));
    o.uv = v.uv;
    o.id = visibilityBase(visibleIndex);
    o.material = instanceMaterial(inst, cl);
    return o;
}

[shader("fragment")]
uint fs_masked(MaskedVsOut i, uint primitive : SV_PrimitiveID) : SV_Target0
{
    const MaterialGpu m = materialData.Load<MaterialGpu>(i.material * kMaterialStride);
    float alpha = m.baseColor.a;
    if (m.vtPageTable != 0xffffffffu)
    {
        const float mip = vtMipLevel(m, ddx(i.uv), ddy(i.uv), mask.mipBias);
        const VtPage p = vtFind(materialData, mask.pageTableOffset, m, frac(i.uv), uint(mip));
        alpha *= vtAlbedo.SampleLevel(float3(p.uv, float(p.layer)), 0.0).a;
    }
    if (alpha < m.alphaCutoff)
        discard;
    return i.id + primitive;
}
