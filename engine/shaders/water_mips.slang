// Mips of the ocean's textures (render/water_ocean.cpp), computed: the same on every backend,
// unlike the drivers' blits.

// Four levels per dispatch: a group of 16 x 16 threads averages a 32 x 32 block of level s into its
// 16 x 16 at s + 1, then on in shared memory to s + 2, s + 3 and s + 4.
struct MipUniforms
{
    uint4 size;  // x: level s's size (texels per side)
};
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> mipSource : register(u0, space1);
[[vk::binding(1, 1)]] [format("rgba16f")] RWTexture2D<float4> mip1 : register(u1, space1);
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> mip2 : register(u2, space1);
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> mip3 : register(u3, space1);
[[vk::binding(4, 1)]] [format("rgba16f")] RWTexture2D<float4> mip4 : register(u4, space1);
[[vk::binding(0, 2)]] ConstantBuffer<MipUniforms> mips : register(b0, space2);

groupshared float4 gMip[16][16];

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_mips(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint size = mips.size.x;
    uint2 p = group.xy * 16 + thread.xy;  // at level s + 1
    float4 v = 0.0;
    if (all(p * 2 < size))
    {
        v = (mipSource[p * 2] + mipSource[p * 2 + uint2(1, 0)] + mipSource[p * 2 + uint2(0, 1)] + mipSource[p * 2 + uint2(1, 1)]) * 0.25;
        mip1[p] = v;
    }
    gMip[thread.y][thread.x] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint level = 2, half = 8; level <= 4; ++level, half /= 2)
    {
        float4 w = 0.0;
        if (all(thread.xy < half))
            w = (gMip[thread.y * 2][thread.x * 2] + gMip[thread.y * 2][thread.x * 2 + 1] + gMip[thread.y * 2 + 1][thread.x * 2] +
                 gMip[thread.y * 2 + 1][thread.x * 2 + 1]) * 0.25;
        GroupMemoryBarrierWithGroupSync();
        if (all(thread.xy < half))
        {
            gMip[thread.y][thread.x] = w;
            const uint2 q = group.xy * half + thread.xy;
            if (all(q < max(size >> level, 1u)))
            {
                if (level == 2)
                    mip2[q] = w;
                else if (level == 3)
                    mip3[q] = w;
                else
                    mip4[q] = w;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
