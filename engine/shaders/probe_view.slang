// Debug view 15 (Probes): the probe GI's probes as small spheres over what they light. Camera rays
// through the BVH (as debug view 14); a surface shows the probes' light there, each probe of the
// finest grid in front of it a sphere showing the light it holds toward the sphere's normal
// (inactive probes grey, fresh ones dark red), drawn where it sits (offset by relocation).

import scene_types;
import ray_trace;
import ray_surface;
import probe_common;
import sky;
import view_common;

struct ProbeViewUniforms
{
    float4 cameraPos;
    float4 cameraRight;    // w: 1 / P00
    float4 cameraUp;       // w: 1 / P11
    float4 cameraForward;
    float4 jitter;         // xy: NDC jitter
    uint4 params;          // x, y: size; z: alpha-masked holes; w: VT page-table byte offset
    ProbeVolume probes;
};

[[vk::binding(0, 0)]] Sampler2DArray<float4> vtAlbedo : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> irradianceAtlas : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float4> distanceAtlas : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float4> probeDataAtlas : register(t3, space0) : register(s3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer rt : register(t4, space0);
[[vk::binding(5, 0)]] ByteAddressBuffer scene : register(t5, space0);
[[vk::binding(6, 0)]] ByteAddressBuffer materialData : register(t6, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ProbeViewUniforms> u : register(b0, space2);

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
        return u.params.z == 0 || rayAlphaAccept(scene, materialData, vtAlbedo, u.params.w, 1, instance, cluster, triangle, bary);
    }
};

// Light as a display colour (the debug views show colours as they are).
float3 display(float3 light) { return light * 4.0 / (1.0 + light * 4.0); }

static const float kRadius = 0.12;  // spheres' radius, in spacings
static const uint kSteps = 64;      // cells walked along the ray

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_probe_view(uint3 tid : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    const uint2 size = u.params.xy;
    const float3 dir = cameraRay(float2(tid.xy) + 0.5, float2(size), u.jitter.xy, u.cameraForward.xyz, u.cameraRight, u.cameraUp);
    const float3 origin = u.cameraPos.xyz;
    AlphaFilter filter;
    const RtHit hit = rtTraceClosest(rt, origin, dir, 1e30, lane, filter);
    if (tid.x >= size.x || tid.y >= size.y)
        return;
    const Atlases atlases;
    float3 color = skyGradient(dir);
    float tEnd = 1e30;
    if (hit.hit)
    {
        const RaySurface s = raySurface(scene, materialData, vtAlbedo, u.params.w, hit, origin, dir);
        color = display(probeIrradiance(u.probes, atlases, s.position, s.normal, -dir)) * 0.7;
        tEnd = hit.t;
    }

    // The finest grid's cells along the ray (3D DDA), each cell's 8 corner probes.
    const float spacing = u.probes.spacing.x;
    const int3 base = u.probes.base[0].xyz, top = base + int3(kProbeDims) - 1;
    const float3 g = origin / spacing - kProbeGridOffset, d = dir;
    int3 cell = int3(floor(g));
    const int3 stepTo = int3(sign(d));
    const float3 delta = abs(1.0 / max(abs(d), 1e-8));
    float3 next = (select(d > 0.0, float3(cell) + 1.0 - g, g - float3(cell))) * delta;
    float bestT = tEnd;
    float3 sphere = 0.0;
    for (uint step = 0; step < kSteps; ++step)
    {
        if (min(min(next.x, next.y), next.z) * spacing > bestT + spacing * 2.0)
            break;
        [unroll]
        for (uint i = 0; i < 8; ++i)
        {
            const int3 world = cell + int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            if (any(world < base) || any(world > top))
                continue;
            const uint2 tile = probeTile(slotOf(world), 0);
            const float4 data = probeData(atlases, tile);
            const float3 c = probePosition(world, data.xyz, spacing);
            const float3 oc = origin - c;
            const float b = dot(oc, dir), cc = dot(oc, oc) - (kRadius * spacing) * (kRadius * spacing);
            const float disc = b * b - cc;
            if (disc < 0.0)
                continue;
            const float t = -b - sqrt(disc);
            if (t <= 0.0 || t >= bestT)
                continue;
            bestT = t;
            const float3 n = normalize(origin + dir * t - c);
            sphere = data.w == kProbeActive ? display(atlases.irradiance(atlasUv(tile, n, kIrradianceTexels, kIrradianceTile)))
                     : data.w == kProbeInactive ? float3(0.3, 0.3, 0.3) * (0.6 + 0.4 * n.y)
                                                : float3(0.35, 0.02, 0.02);
        }
        const uint a = next.x <= next.y && next.x <= next.z ? 0u : next.y <= next.z ? 1u : 2u;
        cell[a] += stepTo[a];
        next[a] += delta[a];
    }
    if (bestT < tEnd)
        color = sphere;
    output[tid.xy] = float4(color, 1.0);
}
