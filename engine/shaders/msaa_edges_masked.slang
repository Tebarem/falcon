// MSAA edges (msaa_edges.slang) for alpha-masked clusters: the same reconstruction after
// visbuffer's alpha test, so cut-out texels leave their samples to what is behind.

import vt;

[[vk::binding(0, 2)]] Sampler2D<float4> shaded : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2D<float> depth1x : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] Sampler2DArray<float4> vtAlbedo : register(t2, space2) : register(s2, space2);
[[vk::binding(3, 2)]] Texture2D<uint> visBuffer : register(t3, space2);
[[vk::binding(4, 2)]] ByteAddressBuffer materialData : register(t4, space2);

#include "msaa_common.slangh"

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
    const VisVertex v = visVertex(index);
    MaskedVsOut o;
    o.position = v.position;
    o.uv = v.uv;
    o.id = v.id;
    o.material = v.material;
    return o;
}

[shader("fragment")]
float4 fs_masked(MaskedVsOut i, uint primitive : SV_PrimitiveID) : SV_Target0
{
    const MaterialGpu m = materialData.Load<MaterialGpu>(i.material * kMaterialStride);
    float alpha = m.baseColor.a;
    if (m.vtPageTable != 0xffffffffu)
    {
        const float mip = vtMipLevel(m, ddx(i.uv), ddy(i.uv), u.mipBias);
        const VtPage p = vtFind(materialData, u.pageTableOffset, m, frac(i.uv), uint(mip));
        alpha *= vtAlbedo.SampleLevel(float3(p.uv, float(p.layer)), 0.0).a;
    }
    if (alpha < m.alphaCutoff)
        discard;
    return float4(reconstruct(int2(i.position.xy), visibilityId(i.id, primitive), i.position.z), 1.0);
}
