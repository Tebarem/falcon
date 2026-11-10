// Reflections off the screen (between reflections.slang's screen-space trace and its reflect
// pass), at the trace's resolution (half by default: one ray per 2x2 pixels): each pixel's far
// reflection, traced through the scene's BVH and lit where it hits as the probe GI's rays are (the
// sun with a shadow ray, the probes' light, its glow), past kTraceMax by the probes' light there
// alone; a miss, the sky the diffuse light has (sky.slang's giSky: the background's openRadiance
// is 3 to 4 times brighter than the sky that lights the scene, and reflected, every surface took
// a sheen of it out of proportion to its diffuse light).
// Rough surfaces (from kRoughTrace up, all of it at kRoughProbes) take the probes' light round the
// reflection instead: what a single ray hits is sharp, a rough reflection is a blur. So do faint
// ones: how much of the far reflection the pixel shows (its weight's brightness, less what its
// screen-space hit covers) below kTraceShown (all of it under half that): the probes' blur is as
// good there, and most of a frame's pixels reflect a few percent. Without the BVH, only the
// probes; without GI, the resolve's flat ambient for that direction.
// cs_reflect puts each pixel's far reflection together from the four around it.

import scene_types;
import ray_trace;
import ray_surface;
import ray_shade;
import probe_common;
import reflection_common;
import sky;
import view_common;

[[vk::binding(0, 0)]] Sampler2D<float> sceneDepth : register(t0, space0) : register(s0, space0);  // reverse-Z, infinite far
[[vk::binding(1, 0)]] Sampler2D<float4> reflectWeight : register(t1, space0) : register(s1, space0);  // rgb weight, a roughness (2: none)
[[vk::binding(2, 0)]] Sampler2D<float4> reflectNormal : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float4> hits : register(t3, space0) : register(s3, space0);  // the screen-space trace's (a: trust)
[[vk::binding(4, 0)]] Sampler2DArray<float4> vtAlbedo : register(t4, space0) : register(s4, space0);
[[vk::binding(5, 0)]] Sampler2D<float4> probeIrradianceAtlas : register(t5, space0) : register(s5, space0);
[[vk::binding(6, 0)]] Sampler2D<float4> probeDistanceAtlas : register(t6, space0) : register(s6, space0);
[[vk::binding(7, 0)]] Sampler2D<float4> probeDataAtlas : register(t7, space0) : register(s7, space0);
[[vk::binding(8, 0)]] ByteAddressBuffer rt : register(t8, space0);
[[vk::binding(9, 0)]] ByteAddressBuffer scene : register(t9, space0);
[[vk::binding(10, 0)]] ByteAddressBuffer materialData : register(t10, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> farOut : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ReflectUniforms> view : register(b0, space2);

static const float kTraceMax = 32.0;
static const float kRoughTrace = 0.3, kRoughProbes = 0.6;
static const float kTraceShown = 0.12;

struct Probes : IProbeTextures
{
    float3 irradiance(float2 uv) { return probeIrradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return probeDistanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

struct AlphaFilter : IRtFilter
{
    bool accept(uint instance, uint cluster, uint triangle, float2 bary)
    {
        return view.trace.z == 0 || rayAlphaAccept(scene, materialData, vtAlbedo, view.trace.x, 1, instance, cluster, triangle, bary);
    }
};

float3 traceFar(float3 wpos, float3 n, float3 r, float roughness, float shown, uint lane)
{
    if (view.reflect.x == 0.0)
        return flatAmbient(r);
    const Probes probes;
    const float3 toEye = normalize(view.cameraPos.xyz - wpos);
    const float rough = max(saturate((roughness - kRoughTrace) / (kRoughProbes - kRoughTrace)),
                            1.0 - saturate(shown / kTraceShown * 2.0 - 1.0));
    const float3 blurred = rough > 0.0 || view.trace.y == 0 ? probeIrradiance(view.probes, probes, wpos, n, r, toEye) : 0.0;
    if (rough >= 1.0 || view.trace.y == 0)
        return blurred;
    AlphaFilter filter;
    const float3 origin = wpos + n * 0.02;
    const RtHit hit = rtTraceClosest(rt, origin, r, 1e4, lane, filter);
    float3 sharp = giSky(r) * view.sunColor.w;
    if (hit.hit)
    {
        const RaySurface s = raySurface(scene, materialData, vtAlbedo, view.trace.x, hit, origin, r);
        if (s.backface)
            sharp = 0.0;
        else if (hit.t > kTraceMax)
            sharp = s.albedo * probeIrradiance(view.probes, probes, s.position, s.normal, -r) * view.sun.w + s.emissive;
        else
            sharp = rayHitLight(rt, s, r, view.sun.xyz, view.sunColor.rgb, view.sun.w, view.probes, probes, lane, filter);
    }
    return lerp(sharp, blurred, rough);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_reflect_far(uint3 tid : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    const uint2 size = view.params.xy;
    const uint scale = uint(view.ssr.w);
    const uint2 traced = (size + scale - 1) / scale;
    if (any(tid.xy >= traced))
        return;
    const float2 pixel = float2(min(tid.xy * scale, size - 1)) + 0.5;  // (the screen-space trace's)
    const float4 w = reflectWeight.SampleLevel(pixel / float2(size), 0.0);
    if (w.a > 1.5)  // sky, unlit debug views, no specular
    {
        farOut[tid.xy] = 0.0;
        return;
    }
    float3 wpos, n, geometric;
    reflectSurface(view, sceneDepth, reflectNormal, pixel, wpos, n, geometric);
    const float3 dir = cameraRay(pixel, float2(size), view.jitter.xy, view.cameraForward.xyz, view.cameraRight, view.cameraUp);
    const float trust = hits.SampleLevel((float2(tid.xy) + 0.5) / float2(size), 0.0).a;  // (full-size texture)
    const float shown = dot(w.rgb, float3(0.2126, 0.7152, 0.0722)) * view.ssr.y * (1.0 - trust);
    farOut[tid.xy] = float4(traceFar(wpos, geometric, reflectFarDirection(dir, n, geometric, w.a), w.a, shown, lane), 1.0);
}
