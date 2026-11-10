// Final composite: (scene + bloom) * auto exposure -> AgX (Sobotka; polynomial fit by
// Wrensch) -> look -> display. The AgX sigmoid output is already display-encoded, so it is
// written to the UNORM target as is. Debug views bypass everything (passthrough).

import post_common;

struct TonemapUniforms
{
    float4 params;  // x: bloom intensity, y: manual exposure (0 = auto), z: look power, w: look saturation
    uint4 screen;   // width, height, passthrough
};

[[vk::binding(0, 0)]] Sampler2D<float4> scene : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> bloom : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer exposureState : register(t2, space0);
[[vk::binding(0, 1)]] [format("rgba8")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<TonemapUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_tonemap(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= u.screen.x || tid.y >= u.screen.y)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(u.screen.xy);
    const float3 c = scene.SampleLevel(uv, 0.0).rgb;
    if (u.screen.z != 0)
    {
        output[tid.xy] = float4(saturate(c), 1.0);  // debug views are display-ready
        return;
    }
    const float exposure = u.params.y > 0.0 ? u.params.y : asfloat(exposureState.Load(256 * 4));
    const float3 hdr = (c + bloom.SampleLevel(uv, 0.0).rgb * u.params.x) * max(exposure, 1e-6);
    output[tid.xy] = float4(agxLook(agx(hdr), u.params.z, u.params.w), 1.0);
}
