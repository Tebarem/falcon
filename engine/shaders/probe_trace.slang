// Probe GI, step 1: the rays of this frame's probe updates through the scene's BVH. A group per
// scheduled probe, a ray per thread (probeRayDirection): each stores the light coming back along
// it and how far it went.
//   hit   the surface's colour times what lights it: the sun (with a shadow ray), the point and
//         spot lights round it (probe_lights.slang: one of them tested with a shadow ray), the
//         probes' light there from earlier updates (so light bounces on, update after update),
//         and its glow; times the bounce intensity
//   miss  the sky (times the sky intensity), at "infinity"
//   back  the inside of a one-sided surface (the probe is in a wall): no light, and the distance
//         kept negative and shortened (DDGI): the blend's visibility sees it as near, relocation
//         as a reason to move
// An inactive probe (no surface near it) traces only the fixed rays, enough to tell when one
// comes near.

import scene_types;
import ray_trace;
import ray_surface;
import probe_common;
import ray_shade;
import probe_lights;
import sky;
import vt;

[[vk::binding(0, 0)]] Sampler2DArray<float4> vtAlbedo : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> irradianceAtlas : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float4> distanceAtlas : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float4> probeDataAtlas : register(t3, space0) : register(s3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer rt : register(t4, space0);
[[vk::binding(5, 0)]] ByteAddressBuffer scene : register(t5, space0);
[[vk::binding(6, 0)]] ByteAddressBuffer materialData : register(t6, space0);
[[vk::binding(7, 0)]] ByteAddressBuffer schedule : register(t7, space0);  // entries, then the slots to reset
[[vk::binding(8, 0)]] ByteAddressBuffer localLights : register(t8, space0);  // render/probe_lights.h
[[vk::binding(0, 1)]] RWByteAddressBuffer rays : register(u0, space1);    // float4 per ray: light, distance
[[vk::binding(0, 2)]] ConstantBuffer<ProbeUpdate> u : register(b0, space2);

static const float kRayMax = 1e4;
static const float kMissDistance = 1e27;

struct Atlases : IProbeTextures
{
    float3 irradiance(float2 uv) { return irradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return distanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

struct AlphaFilter : IRtFilter
{
    bool accept(uint instance, uint cluster, uint triangle, float2 bary)
    {
        return u.params.z == 0 || rayAlphaAccept(scene, materialData, vtAlbedo, u.params.y, 1, instance, cluster, triangle, bary);
    }
};

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_probe_trace(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint e = gid.x;
    if (e >= u.params.x)
        return;
    const ProbeEntry entry = probeEntry(schedule.Load((u.batch.x + e) * 4));
    const uint cascade = entry.cascade;
    const uint3 slot = probeSlot(entry.index);
    const Atlases atlases;
    const float4 data = entry.fresh ? float4(0.0, 0.0, 0.0, kProbeFresh) : probeData(atlases, probeTile(slot, cascade));
    const float spacing = u.volume.spacing[cascade];
    const float3 origin = probePosition(worldOf(u.volume, cascade, slot), data.xyz, spacing);
    const float3 dir = probeRayDirection(lane, probeRotation(entry.age));
    float4 result = float4(0.0, 0.0, 0.0, kMissDistance);
    if (data.w != kProbeInactive || lane < kProbeFixedRays)
    {
        AlphaFilter filter;
        const RtHit hit = rtTraceClosest(rt, origin, dir, kRayMax, lane, filter);
        if (!hit.hit)
        {
            result = float4(giSky(dir) * u.sunColor.w, kMissDistance);
        }
        else
        {
            const RaySurface s = raySurface(scene, materialData, vtAlbedo, u.params.y, hit, origin, dir);
            if (s.backface)
            {
                result = float4(0.0, 0.0, 0.0, -0.2 * hit.t);
            }
            else
            {
                // (Which light gets the shadow ray: by probe, ray and age, so runs match.)
                const uint h = probeHash(entry.index * 0x9e3779b9u ^ (cascade << 28) ^ probeHash(entry.age * kProbeRays + lane));
                const float3 local = probeLocalLight(localLights, rt, s, float(h >> 8) / 16777216.0, lane, filter);
                result = float4(rayHitLight(rt, s, dir, u.sun.xyz, u.sunColor.rgb, u.sun.w, u.volume, atlases, lane, filter) +
                                    s.albedo * local * u.sun.w,
                                hit.t);
            }
        }
    }
    rays.Store4((e * kProbeRays + lane) * 16, asuint(result));
}
