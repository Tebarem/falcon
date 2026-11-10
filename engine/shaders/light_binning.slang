// Light tile binning: each light's screen-space bounds are rasterized into per-tile bitmasks
// (bit = index in the depth-sorted light list). See lighting.slang for the grid layout.

import lighting;

struct BinUniforms
{
    float4x4 view;
    float4 proj;     // P00, P11, zNear
    uint4 screen;    // width, height, tilesX, tilesY
    uint4 grid;      // order offset, tile offset, words per tile, light count
};

[[vk::binding(0, 0)]] ByteAddressBuffer lights : register(t0, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer grid : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<BinUniforms> u : register(b0, space2);

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_clear_tiles(uint3 tid : SV_DispatchThreadID)
{
    const uint total = u.screen.z * u.screen.w * u.grid.z;
    if (tid.x < total)
        grid.Store((u.grid.y + tid.x) * 4, 0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_bin_lights(uint3 tid : SV_DispatchThreadID)
{
    const uint sorted = tid.x;
    if (sorted >= u.grid.w)
        return;
    const LightGpu l = loadLight(lights, grid.Load((u.grid.x + sorted) * 4));
    const float3 c = mul(u.view, float4(l.positionRange.xyz, 1.0)).xyz;
    const float r = l.positionRange.w;
    const float dz = -c.z;
    const float zNear = u.proj.z;
    if (dz + r <= zNear)
        return;  // entirely behind the camera

    int2 t0 = 0, t1 = int2(u.screen.zw) - 1;
    if (dz - r > zNear)
    {
        float2 lo = 1e30, hi = -1e30;
        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            const float3 q = c + float3((i & 1) ? r : -r, (i & 2) ? r : -r, (i & 4) ? r : -r);
            const float2 ndc = float2(u.proj.x * q.x, u.proj.y * q.y) / -q.z;
            lo = min(lo, ndc);
            hi = max(hi, ndc);
        }
        if (hi.x < -1.0 || lo.x > 1.0 || hi.y < -1.0 || lo.y > 1.0)
            return;
        const float2 size = float2(u.screen.xy);
        const float2 pmin = clamp(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * size, 0.0, size - 1.0);
        const float2 pmax = clamp(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * size, 0.0, size - 1.0);
        t0 = int2(pmin) / int(kTileSize);
        t1 = int2(pmax) / int(kTileSize);
    }
    const uint word = sorted >> 5, bit = 1u << (sorted & 31);
    for (int y = t0.y; y <= t1.y; ++y)
        for (int x = t0.x; x <= t1.x; ++x)
            grid.InterlockedOr((u.grid.y + (uint(y) * u.screen.z + uint(x)) * u.grid.z + word) * 4, bit);
}
