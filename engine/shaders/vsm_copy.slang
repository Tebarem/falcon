// Virtual shadow maps with moving casters (see vsm_pages.slang): each re-rendered page starts as a
// copy of its static layer (the casters that did not move lately, cached in their own atlas at
// the same physical page), then the moving casters are drawn over it. One instanced quad per
// re-rendered page, its depth read texel for texel.

import vsm;

[[vk::binding(0, 0)]] ByteAddressBuffer state : register(t0, space0);
[[vk::binding(0, 1)]] ConstantBuffer<VsmParams> vsm : register(b0, space1);
[[vk::binding(0, 2)]] Sampler2D<float> staticLayer : register(t0, space2) : register(s0, space2);  // point sampler

[shader("vertex")]
float4 vs_copy(uint vertexId : SV_VertexID, uint dirtyIndex : SV_InstanceID) : SV_Position
{
    const uint phys = state.Load((vsm.layout2.z + dirtyIndex) * 4);
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    const float2 uv = (physicalOrigin(phys) / float(kVsmPageTexels) + corners[vertexId]) / float(kAtlasPagesPerSide);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

[shader("fragment")]
float fs_copy(float4 position : SV_Position) : SV_Depth
{
    return staticLayer.SampleLevel(position.xy / float(kAtlasTexels), 0.0);
}
