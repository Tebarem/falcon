// What screen-space reflections read (reflections.slang): this frame's image with mips 0..6, all
// built in one dispatch. A group of 32x32 threads takes a 64x64 tile of the image: each thread
// copies its 2x2 pixels (level 0) and averages them (level 1), and the group carries on in
// shared memory, a quarter of the threads per level, down to the tile's one texel at level 6.
// NaN becomes 0 and overflow is clamped on the way: a coarse texel covers up to 64x64 pixels,
// and one bad value in it turned every reflection that read it black, in patches that came and
// went as the camera moved (SDL's mip generation left such texels on some frames).
// (Levels from the image itself, a dispatch each, cost 2.3 ms: the coarse levels' few threads
// each read thousands of pixels; 4x4 pixels a thread, 0.36 ms.)

struct SourceUniforms
{
    uint4 image;  // xy: the image's size (level L: the size >> L, at least 1)
};

[[vk::binding(0, 0)]] Sampler2D<float4> image : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> level0 : register(u0, space1);
[[vk::binding(1, 1)]] [format("rgba16f")] RWTexture2D<float4> level1 : register(u1, space1);
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> level2 : register(u2, space1);
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> level3 : register(u3, space1);
[[vk::binding(4, 1)]] [format("rgba16f")] RWTexture2D<float4> level4 : register(u4, space1);
[[vk::binding(5, 1)]] [format("rgba16f")] RWTexture2D<float4> level5 : register(u5, space1);
[[vk::binding(6, 1)]] [format("rgba16f")] RWTexture2D<float4> level6 : register(u6, space1);
[[vk::binding(0, 2)]] ConstantBuffer<SourceUniforms> u : register(b0, space2);

// Finite, not negative, under half precision's limit.
float3 clean(float3 c)
{
    return clamp(select(isnan(c), float3(0.0), c), 0.0, 60000.0);
}

// A texel of level L, written only inside that level (the image's size >> L).
bool inside(uint2 p, uint L)
{
    return all(p < max(u.image.xy >> L, uint2(1, 1)));
}

void write(uint L, uint2 p, float3 c)
{
    if (!inside(p, L))
        return;
    const float4 v = float4(c, 1.0);
    switch (L)
    {
    case 2: level2[p] = v; break;
    case 3: level3[p] = v; break;
    case 4: level4[p] = v; break;
    case 5: level5[p] = v; break;
    default: level6[p] = v; break;
    }
}

groupshared float3 gLevel[32][32];

[shader("compute")]
[numthreads(32, 32, 1)]
void cs_source(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 t = thread.xy;
    const uint2 pixel = group.xy * 64 + t * 2;  // this thread's 2x2 pixels
    const float2 texel = 1.0 / float2(u.image.xy);
    float3 sum = 0.0;
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x)
        {
            const uint2 p = pixel + uint2(x, y);
            const float3 c = clean(image.SampleLevel((float2(min(p, u.image.xy - 1)) + 0.5) * texel, 0.0).rgb);
            if (inside(p, 0))
                level0[p] = float4(c, 1.0);
            sum += c * 0.25;
        }
    if (inside(pixel >> 1, 1))
        level1[pixel >> 1] = float4(sum, 1.0);
    gLevel[t.y][t.x] = sum;
    // Levels 2..6: each from the 2x2 below it, by a quarter of the threads that made those.
    for (uint L = 2, n = 16; L <= 6; ++L, n >>= 1)
    {
        GroupMemoryBarrierWithGroupSync();
        float3 c = 0.0;
        if (all(t < n))
            c = (gLevel[t.y * 2][t.x * 2] + gLevel[t.y * 2][t.x * 2 + 1] + gLevel[t.y * 2 + 1][t.x * 2] +
                 gLevel[t.y * 2 + 1][t.x * 2 + 1]) * 0.25;
        GroupMemoryBarrierWithGroupSync();
        if (all(t < n))
        {
            gLevel[t.y][t.x] = c;
            write(L, (group.xy << (6 - L)) + t, c);
        }
    }
}
