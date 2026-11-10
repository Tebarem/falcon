import scene_types;

// HiZ pyramid in a flat float buffer: level 0 is half resolution, each texel the minimum
// (farthest, reverse-Z) of the 2x2 below it. Edge texels clamp, so odd sizes stay conservative.

struct HizUniforms
{
    uint4 src;  // word offset, width, height (for level 0: depth texture width, height)
    uint4 dst;  // word offset, width, height
};

[[vk::binding(0, 0)]] Sampler2D<float> depth : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer hiz : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<HizUniforms> level : register(b0, space2);

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hiz_depth(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= level.dst.y || tid.y >= level.dst.z)
        return;
    const uint2 maxPixel = uint2(level.src.y - 1, level.src.z - 1);
    const float2 inv = 1.0 / float2(level.src.yz);
    const uint2 p = tid.xy * 2;
    const float d = min(min(fetchFloat(depth, min(p, maxPixel), inv), fetchFloat(depth, min(p + uint2(1, 0), maxPixel), inv)),
                        min(fetchFloat(depth, min(p + uint2(0, 1), maxPixel), inv), fetchFloat(depth, min(p + uint2(1, 1), maxPixel), inv)));
    hiz.Store((level.dst.x + tid.y * level.dst.y + tid.x) * 4, asuint(d));
}

float srcTexel(uint2 p)
{
    p = min(p, uint2(level.src.y - 1, level.src.z - 1));
    return asfloat(hiz.Load((level.src.x + p.y * level.src.y + p.x) * 4));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hiz_down(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= level.dst.y || tid.y >= level.dst.z)
        return;
    const uint2 p = tid.xy * 2;
    const float d = min(min(srcTexel(p), srcTexel(p + uint2(1, 0))), min(srcTexel(p + uint2(0, 1)), srcTexel(p + uint2(1, 1))));
    hiz.Store((level.dst.x + tid.y * level.dst.y + tid.x) * 4, asuint(d));
}
