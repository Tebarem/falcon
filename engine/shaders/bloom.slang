// Bloom (Jimenez, "Next Generation Post Processing in Call of Duty: AW"): 13-tap downsample
// chain (Karis average on the first level against fireflies), then a 3x3 tent upsample that
// accumulates each level onto the next finer one.

import post_common;

struct BloomUniforms
{
    float4 texel;  // xy: source texel size, zw: destination size in pixels
    uint4 mode;    // x: 1 = first downsample (Karis average)
};

[[vk::binding(0, 0)]] Sampler2D<float4> src : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> base : register(t1, space0) : register(s1, space0);  // upsample: same-level downsample
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<BloomUniforms> u : register(b0, space2);

float3 tap(float2 uv) { return src.SampleLevel(uv, 0.0).rgb; }
float karis(float3 c) { return 1.0 / (1.0 + luminance(c)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_down(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= uint(u.texel.z) || tid.y >= uint(u.texel.w))
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / u.texel.zw;
    const float2 t = u.texel.xy;
    const float3 a = tap(uv + t * float2(-2, -2)), b = tap(uv + t * float2(0, -2)), c = tap(uv + t * float2(2, -2));
    const float3 d = tap(uv + t * float2(-2, 0)), e = tap(uv), f = tap(uv + t * float2(2, 0));
    const float3 g = tap(uv + t * float2(-2, 2)), h = tap(uv + t * float2(0, 2)), i = tap(uv + t * float2(2, 2));
    const float3 j = tap(uv + t * float2(-1, -1)), k = tap(uv + t * float2(1, -1));
    const float3 l = tap(uv + t * float2(-1, 1)), m = tap(uv + t * float2(1, 1));
    float3 r;
    if (u.mode.x == 1)
    {
        // Karis average per 2x2 box keeps single bright pixels from blowing up the chain.
        const float3 b0 = (a + b + d + e) * 0.25, b1 = (b + c + e + f) * 0.25, b2 = (d + e + g + h) * 0.25;
        const float3 b3 = (e + f + h + i) * 0.25, b4 = (j + k + l + m) * 0.25;
        const float w0 = karis(b0) * 0.125, w1 = karis(b1) * 0.125, w2 = karis(b2) * 0.125, w3 = karis(b3) * 0.125,
                    w4 = karis(b4) * 0.5;
        r = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4);
    }
    else
        r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    output[tid.xy] = float4(max(r, 0.0), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_up(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= uint(u.texel.z) || tid.y >= uint(u.texel.w))
        return;
    const float2 uv = (float2(tid.xy) + 0.5) / u.texel.zw;
    const float2 t = u.texel.xy;  // coarser level's texel
    float3 up = tap(uv) * 4.0;
    up += (tap(uv + t * float2(-1, 0)) + tap(uv + t * float2(1, 0)) + tap(uv + t * float2(0, -1)) + tap(uv + t * float2(0, 1))) * 2.0;
    up += tap(uv + t * float2(-1, -1)) + tap(uv + t * float2(1, -1)) + tap(uv + t * float2(-1, 1)) + tap(uv + t * float2(1, 1));
    output[tid.xy] = float4(up / 16.0 + base.SampleLevel(uv, 0.0).rgb, 1.0);
}
