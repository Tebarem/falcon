// Auto exposure: 256-bin log2-luminance histogram of the HDR image, then one group averages
// the 50th..95th percentile range and eases the exposure toward key / average.
// State buffer: [256 bins] [exposure, average luminance, unused, unused]

import scene_types;
import post_common;

struct ExposureUniforms
{
    float4 range;   // x: min log2 luminance, y: 1 / (max - min), z: adaptation blend (0..1), w: exposure compensation (EV)
    uint4 screen;   // width, height
    float4 limit;   // x: past this exposure it rises at half the rate (0: no knee): PostSettings::exposureLiftKnee
};

[[vk::binding(0, 0)]] Sampler2D<float4> hdr : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer state : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ExposureUniforms> u : register(b0, space2);

static const uint kBins = 256;
groupshared uint bins[kBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_histogram(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    bins[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (tid.x < u.screen.x && tid.y < u.screen.y)
    {
        const float l = luminance(hdr.SampleLevel((float2(tid.xy) + 0.5) / float2(u.screen.xy), 0.0).rgb);
        if (l > 1e-5)
        {
            const uint bin = uint(saturate((log2(l) - u.range.x) * u.range.y) * 254.0) + 1;  // bin 0: black
            InterlockedAdd(bins[bin], 1);
        }
        else
            InterlockedAdd(bins[0], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (bins[gi] != 0)
        state.InterlockedAdd(gi * 4, bins[gi]);
}

groupshared uint prefix[kBins];

[shader("compute")]
[numthreads(256, 1, 1)]
void cs_exposure(uint gi : SV_GroupIndex)
{
    const uint count = state.Load(gi * 4);
    prefix[gi] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < kBins; offset <<= 1)  // inclusive prefix sum
    {
        const uint add = gi >= offset ? prefix[gi - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        prefix[gi] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        // Average log luminance over the [50%, 95%] percentile window (bin 0 = black, excluded).
        const float total = float(prefix[kBins - 1]);
        const float lo = total * 0.5, hi = total * 0.95;
        float wsum = 0.0, lsum = 0.0;
        for (uint b = 1; b < kBins; ++b)
        {
            const float end = float(prefix[b]), start = end - float(prefix[b] - prefix[b - 1]);
            const float w = max(min(end, hi) - max(start, lo), 0.0);
            wsum += w;
            lsum += w * (u.range.x + (float(b) - 0.5) / 254.0 / u.range.y);
        }
        const float avgLog = wsum > 0.0 ? lsum / wsum : -2.0;
        float target = 0.18 / exp2(avgLog) * exp2(u.range.w);
        if (u.limit.x > 0.0 && target > u.limit.x)
            target = u.limit.x * sqrt(target / u.limit.x);  // half the EV past the knee
        const float previous = asfloat(state.Load(kBins * 4));
        const float exposure = previous > 0.0 ? lerp(previous, target, u.range.z) : target;
        state.Store(kBins * 4, asuint(exposure));
        state.Store(kBins * 4 + 4, asuint(exp2(avgLog)));
    }
    AllMemoryBarrierWithGroupSync();
    state.Store(gi * 4, 0);  // clear the histogram for the next frame
}
