// Probe GI, step 3: each updated probe moved and classified from its fixed rays (DDGI), a thread
// per probe:
//   relocation  more than a quarter of back faces: the probe is inside something, out through
//               the nearest back face (or, that too far, away from it: a probe on a slab's
//               underside); a front face nearer than u.memory.z spacings: away from it; else back
//               toward its grid position. Never more than 0.45 spacings from it.
//   state       inside something: fresh (moved, it is traced fully again before it is sampled),
//               or inactive when it can't get out; no surface hit within its cells (a spacing each
//               way, and as far past as sampling's bias moves a point): inactive (not sampled, traced lightly); else active, or fresh when it was
//               traced lightly (no light yet)
// cs_reset: the probes a grid scrolled in, fresh until their first update.

import probe_common;

[[vk::binding(0, 0)]] ByteAddressBuffer rays : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer schedule : register(t1, space0);  // entries, then the slots to reset
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> dataAtlas : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ProbeUpdate> u : register(b0, space2);

static const float kMissed = 1e26;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_probe_relocate(uint3 tid : SV_DispatchThreadID)
{
    const uint e = tid.x;
    if (e >= u.params.x)
        return;
    const ProbeEntry entry = probeEntry(schedule.Load((u.batch.x + e) * 4));
    const uint cascade = entry.cascade;
    const uint3 slot = probeSlot(entry.index);
    const uint2 tile = probeTile(slot, cascade);
    const float4 data = entry.fresh ? float4(0.0, 0.0, 0.0, kProbeFresh) : dataAtlas[tile];
    const bool light = data.w == kProbeInactive;  // traced the fixed rays only
    const float spacing = u.volume.spacing[cascade];
    const float3x3 rotation = probeRotation(entry.age);
    // The surfaces it lights: in its cells, or as far past them as the sampling's bias moves a point
    // (a ceiling exactly a spacing above it: lit by it, and float error made it miss).
    const float reach = spacing * (1.0 + u.volume.bias.x + u.volume.bias.y);

    uint backfaces = 0;
    float closestBack = 1e30, closestFront = 1e30, farthestFront = 0.0;
    float3 backDir = 0.0, frontDir = 0.0;
    bool near = false;
    const uint count = light ? kProbeFixedRays : kProbeRays;
    for (uint r = 0; r < count; ++r)
    {
        const float d = asfloat(rays.Load((e * kProbeRays + r) * 16 + 12));
        const float3 dir = probeRayDirection(r, rotation);
        if (r < kProbeFixedRays)
        {
            if (d < 0.0)
            {
                ++backfaces;
                if (-d * 5.0 < closestBack)
                {
                    closestBack = -d * 5.0;
                    backDir = dir;
                }
            }
            else if (d < kMissed)
            {
                if (d < closestFront)
                {
                    closestFront = d;
                    frontDir = dir;
                }
                farthestFront = max(farthestFront, d);
            }
        }
        if (d >= 0.0 && d < kMissed && all(abs(dir * d) <= reach))
            near = true;
    }

    const float3 offset = data.xyz * spacing;
    const float minFront = u.memory.z * spacing;
    const bool inside = float(backfaces) > 0.25 * float(kProbeFixedRays);
    const float kMaxOffset = 0.45;  // spacings
    float3 kept = data.xyz;
    float state = !near ? kProbeInactive : light ? kProbeFresh : kProbeActive;
    if (inside)
    {
        // Out through the nearest back face; when that is too far (a probe on a slab's underside
        // sees its top from within), away from it: back to the side the probe sits on. Neither:
        // stuck inside (not sampled, traced lightly until something changes).
        const float3 through = (offset + backDir * (closestBack + minFront * 0.5)) / spacing;
        const float3 away = (offset - backDir * minFront) / spacing;
        if (dot(through, through) < kMaxOffset * kMaxOffset)
            kept = through, state = kProbeFresh;
        else if (dot(away, away) < kMaxOffset * kMaxOffset)
            kept = away, state = kProbeFresh;
        else
            state = kProbeInactive;
    }
    else
    {
        float3 moved = offset;
        if (closestFront < minFront)
            moved = offset - frontDir * (minFront - closestFront);
        else if (dot(offset, offset) > 0.0)
            moved = offset - normalize(offset) * min(closestFront - minFront, length(offset));
        const float3 scaled = moved / spacing;
        if (dot(scaled, scaled) < kMaxOffset * kMaxOffset)
            kept = scaled;
    }
    dataAtlas[tile] = float4(kept, state);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_reset(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= u.params.w)
        return;
    const ProbeEntry entry = probeEntry(schedule.Load((u.batch.y + tid.x) * 4));
    dataAtlas[probeTile(probeSlot(entry.index), entry.cascade)] = float4(0.0, 0.0, 0.0, kProbeFresh);
}
