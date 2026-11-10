// Debug visualization of a HiZ level (reverse-Z depth remapped for visibility).

struct HizViewUniforms
{
    uint4 level;   // word offset, width, height, level index
    uint4 screen;  // width, height
};

[[vk::binding(0, 0)]] ByteAddressBuffer hiz : register(t0, space0);
[[vk::binding(0, 1)]] [format("rgba8")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<HizViewUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_view_hiz(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= u.screen.x || tid.y >= u.screen.y)
        return;
    const uint texelSize = 2u << u.level.w;
    const uint2 t = min(tid.xy / texelSize, uint2(u.level.y - 1, u.level.z - 1));
    const float d = asfloat(hiz.Load((u.level.x + t.y * u.level.y + t.x) * 4));
    const float v = saturate(pow(d * 20.0, 0.35));  // reverse-Z: near = bright
    output[tid.xy] = float4(v, v * 0.9, v * 0.75, 1.0);
}
