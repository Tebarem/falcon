// Spatial AO denoise for frames without TAA: a 4x4 average of the raw AO (a static noise
// pattern then), weighted down across depth discontinuities so it does not bleed over edges.
// Works at the AO's own resolution: r = AO, g = view depth (ao.slang).

[[vk::binding(0, 0)]] Sampler2D<float2> rawAo : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] [format("rg16f")] RWTexture2D<float2> ao : register(u0, space1);
struct AoBlurUniforms
{
    uint4 size;  // xy: AO texture size
};

[[vk::binding(0, 2)]] ConstantBuffer<AoBlurUniforms> u : register(b0, space2);

float2 fetchAo(uint2 p, float2 inv) { return rawAo.SampleLevel((float2(p) + 0.5) * inv, 0.0); }

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_ao_blur(uint3 tid : SV_DispatchThreadID)
{
    const uint2 dims = u.size.xy;
    if (tid.x >= dims.x || tid.y >= dims.y)
        return;
    const float2 inv = 1.0 / float2(dims);
    const float2 c = fetchAo(tid.xy, inv);
    const float z0 = c.y;
    if (z0 >= 65504.0)
    {
        ao[tid.xy] = c;  // sky
        return;
    }
    float sum = 0.0, weight = 0.0;
    [unroll]
    for (int y = -2; y < 2; ++y)
    {
        [unroll]
        for (int x = -2; x < 2; ++x)
        {
            const uint2 p = uint2(clamp(int2(tid.xy) + int2(x, y), int2(0, 0), int2(dims) - 1));
            const float2 s = fetchAo(p, inv);
            const float w = saturate(1.0 - abs(s.y - z0) / (0.05 * z0 + 1e-4));
            sum += s.x * w;
            weight += w;
        }
    }
    ao[tid.xy] = float2(weight > 0.0 ? sum / weight : c.x, z0);
}
