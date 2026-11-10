// Probe GI, step 2: each updated probe's rays (probe_trace.slang) into its atlas tiles. A group
// per probe: every irradiance texel the cosine-weighted mean of the light along its hemisphere's
// rays, every distance texel the mean distance and its square round its direction (sharply
// weighted, DDGI's power 50), each blended with what the texel held (the hysteresis: the
// probe's memory: the mean of its updates since it was fresh or since a change near it, then
// at most the hysteresis); then the tiles'
// borders from the opposite edges.
// Back faces' rays bring no light, and their distances (shortened) count. Misses count as far as
// the probe's cells reach (1.5 x the cell's diagonal). Only the turned rays count: the fixed ones
// (relocation's and classification's) look the same way every update, so something bright one of
// them happened to hit stayed in the probe's light however many updates it took (a blotch).

import probe_common;

[[vk::binding(0, 0)]] Sampler2D<float4> probeDataAtlas : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer rays : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer schedule : register(t2, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> irradiance : register(u0, space1);
[[vk::binding(1, 1)]] [format("rg16f")] RWTexture2D<float2> distance : register(u1, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ProbeUpdate> u : register(b0, space2);

groupshared float4 rayLight[kProbeRays];  // rgb, distance
groupshared float3 rayDirection[kProbeRays];
groupshared float3 irradianceTile[kIrradianceTile * kIrradianceTile];
groupshared float2 distanceTile[kDistanceTile * kDistanceTile];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_probe_blend(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint e = gid.x;
    if (e >= u.params.x)
        return;
    const ProbeEntry entry = probeEntry(schedule.Load((u.batch.x + e) * 4));
    const uint cascade = entry.cascade;
    const uint3 slot = probeSlot(entry.index);
    const uint2 tile = probeTile(slot, cascade);
    const float state = entry.fresh ? kProbeFresh : probeDataAtlas.SampleLevel((float2(tile) + 0.5) / float2(kProbeTiles), 0.0).w;
    if (state == kProbeInactive)
        return;  // (traced lightly: only its classification changes)
    const float spacing = u.volume.spacing[cascade];
    const float reach = spacing * 1.7320508 * 1.5;
    rayLight[lane] = asfloat(rays.Load4((e * kProbeRays + lane) * 16));
    rayDirection[lane] = probeRayDirection(lane, probeRotation(entry.age));
    GroupMemoryBarrierWithGroupSync();
    // The mean of its updates since it was fresh (or something changed near it), then at most the hysteresis.
    const float memory = state == kProbeFresh ? 0.0 : min(float(entry.age) / float(entry.age + 1), u.memory.x);

    const uint2 irradianceAt = tile * kIrradianceTile;
    for (uint t = lane; t < kIrradianceTexels * kIrradianceTexels; t += 64)
    {
        const uint2 texel = uint2(t % kIrradianceTexels, t / kIrradianceTexels);
        const float3 dir = texelDirection(texel, kIrradianceTexels);
        float3 sum = 0.0;
        float total = 0.0;
        for (uint r = kProbeFixedRays; r < kProbeRays; ++r)
        {
            const float4 ray = rayLight[r];
            if (ray.w < 0.0)
                continue;
            const float w = max(dot(dir, rayDirection[r]), 0.0);
            sum += ray.rgb * w;
            total += w;
        }
        const uint2 at = irradianceAt + 1 + texel;
        float3 value = total > 1e-5 ? sum / total : 0.0;
        if (memory > 0.0)
            value = lerp(value, irradiance[at].rgb, memory);
        irradiance[at] = float4(value, 1.0);
        irradianceTile[(texel.y + 1) * kIrradianceTile + texel.x + 1] = value;
    }

    const uint2 distanceAt = tile * kDistanceTile;
    for (uint t = lane; t < kDistanceTexels * kDistanceTexels; t += 64)
    {
        const uint2 texel = uint2(t % kDistanceTexels, t / kDistanceTexels);
        const float3 dir = texelDirection(texel, kDistanceTexels);
        float2 sum = 0.0;
        float total = 0.0;
        for (uint r = kProbeFixedRays; r < kProbeRays; ++r)
        {
            const float w = pow(max(dot(dir, rayDirection[r]), 0.0), 50.0);
            const float d = min(abs(rayLight[r].w), reach);
            sum += float2(d, d * d) * w;
            total += w;
        }
        const uint2 at = distanceAt + 1 + texel;
        float2 value = total > 1e-6 ? sum / total : float2(reach, reach * reach);
        if (memory > 0.0)
            value = lerp(value, distance[at], memory);
        distance[at] = value;
        distanceTile[(texel.y + 1) * kDistanceTile + texel.x + 1] = value;
    }
    GroupMemoryBarrierWithGroupSync();

    // Borders (the interior's opposite edges, so bilinear filtering wraps over the seams).
    for (uint t = lane; t < kIrradianceTile * kIrradianceTile; t += 64)
    {
        const uint2 p = uint2(t % kIrradianceTile, t / kIrradianceTile);
        if (all(p >= 1) && all(p <= kIrradianceTexels))
            continue;
        const uint2 s = borderSource(p, kIrradianceTexels);
        irradiance[irradianceAt + p] = float4(irradianceTile[s.y * kIrradianceTile + s.x], 1.0);
    }
    for (uint t = lane; t < kDistanceTile * kDistanceTile; t += 64)
    {
        const uint2 p = uint2(t % kDistanceTile, t / kDistanceTile);
        if (all(p >= 1) && all(p <= kDistanceTexels))
            continue;
        const uint2 s = borderSource(p, kDistanceTexels);
        distance[distanceAt + p] = distanceTile[s.y * kDistanceTile + s.x];
    }
}
