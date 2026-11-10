// Water composite (render/water_renderer.cpp): after the resolve, for each pixel where the
// surface pass drew water, the water's colour over what the resolve lit:
//   what is under it, seen bent through the surface (the reflection pass's mip pyramid of the
//   frame before water: blurrier with depth in murky water), dimmed by the water it crosses
//   (Beer-Lambert, per colour), plus the light the water itself scatters back (sun and sky);
//   faded into the shore over the last centimetres of depth; foam (shores, crests) and film on
//   top; the sun's highlight.
// The reflection of the scene comes after, from the reflection pass: this writes the surface's
// Fresnel weight and normals for it (and the reflection pass starts from the water's depth).
// Without the reflection pass the sky is reflected here.
// Caustics: light focused by the waves onto what is under the water (a pattern traced once on the
// CPU, two layers drifting apart, found where the sunlight entered the water).
// Under water (the camera in a body): every pixel whose near point is below the surface (a
// waterline across the lens when the camera is at it) sees through the water's fog; the surface
// from below shows the world above inside Snell's window and mirrors the water outside it.
// Mirrors (one body's still surface): the planar reflection of what is on screen; what is not
// (above the frame, behind something nearer) is found in the terrain's heightmap and lit here,
// the mirror fading into it toward the frame's top edge instead of stopping there.

import view_common;
import water_common;
import shadows;
import lighting;
import vsm;
import probe_common;
import ray_trace;
import ray_surface;
import ray_shade;
import sky;
import terrain_shading;

struct CompositeUniforms
{
    float4x4 prevViewProj;  // previous frame, unjittered: motion vectors
    float4 cameraPos;       // w: zNear
    float4 cameraRight;     // w = 1 / P00
    float4 cameraUp;        // w = 1 / P11
    float4 cameraForward;
    float4 jitter;          // xy: NDC jitter of this frame's raster
    uint4 params;           // width, height, rect origin x, y
    float4 sunDirection;
    float4 sunColor;
    VsmParams vsm;
    ProbeVolume probes;     // probe GI (probe_common.slang)
    float4 giParams;        // x: bounce intensity, y: sky intensity, z: GI on, w: the reflection pass follows
    float4 time;            // x: water time (s), y: the refraction source's last mip
    float4 underwater;      // x: the body the camera is in, + 1 (0: none)
    float4 mirror;          // x: the body the planar reflections are for, + 1 (0: none)
    float4 terrainGrid;     // the terrain's coarse map: world x, z of its first sample, its spacing (0: no terrain), 1 / spacing
    float4 terrainHeights;  // height min, range, the map's side, layers with textures (0: the default look)
    uint4 trace;            // x: VT page-table byte offset, y: the BVH is there, z: alpha-masked holes
};

[[vk::binding(0, 0)]] Sampler2D<float> sceneDepth : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float> waterDepth : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float4> waterNormals : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float4> waterExtra : register(t3, space0) : register(s3, space0);
[[vk::binding(4, 0)]] Sampler2D<float4> refractSource : register(t4, space0) : register(s4, space0);  // with mips
[[vk::binding(5, 0)]] Sampler2DShadow vsmAtlas : register(t5, space0) : register(s5, space0);
// Probe GI (render/probe_gi.h): irradiance and distance atlases (bilinear), probe data (point).
[[vk::binding(6, 0)]] Sampler2D<float4> probeIrradianceAtlas : register(t6, space0) : register(s6, space0);
[[vk::binding(7, 0)]] Sampler2D<float4> probeDistanceAtlas : register(t7, space0) : register(s7, space0);
[[vk::binding(8, 0)]] Sampler2D<float4> probeDataAtlas : register(t8, space0) : register(s8, space0);
// Tileable detail (water_surface.slang): b foam pattern, a film pattern.
[[vk::binding(9, 0)]] Sampler2D<float4> detailMap : register(t9, space0) : register(s9, space0);
// Caustics (render/water_renderer.cpp): r the focused light, 0.25 = as much as fell on the surface.
[[vk::binding(10, 0)]] Sampler2D<float> causticMap : register(t10, space0) : register(s10, space0);
// The terrain's coarse maps (render/terrain_surface.h): every 8th sample's height (R16) and material.
[[vk::binding(11, 0)]] Sampler2D<float> terrainHeight : register(t11, space0) : register(s11, space0);
[[vk::binding(12, 0)]] Sampler2D<float4> terrainMaterial : register(t12, space0) : register(s12, space0);
// The virtual texture's colour (what reflections off screen hit).
[[vk::binding(13, 0)]] Sampler2DArray<float4> vtAlbedo : register(t13, space0) : register(s13, space0);
[[vk::binding(14, 0)]] ByteAddressBuffer bodies : register(t14, space0);
[[vk::binding(15, 0)]] ByteAddressBuffer vsmState : register(t15, space0);
// Planar reflections (water_sspr.slang): per pixel, the pixel of the frame it reflects (x | y << 16), or ~0.
[[vk::binding(16, 0)]] ByteAddressBuffer mirrorSources : register(t16, space0);
// The terrain's layers (TerrainSurface: 80 bytes each from byte 16; tint at 32, loaded at 56, mean colour at 64).
[[vk::binding(17, 0)]] ByteAddressBuffer terrainData : register(t17, space0);
// The BVH (render/ray_tracing/scene_bvh.h), the scene and its materials: reflections off screen.
[[vk::binding(18, 0)]] ByteAddressBuffer rt : register(t18, space0);
[[vk::binding(19, 0)]] ByteAddressBuffer scene : register(t19, space0);
[[vk::binding(20, 0)]] ByteAddressBuffer materialData : register(t20, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);  // HDR
[[vk::binding(1, 1)]] [format("rg16f")] RWTexture2D<float2> velocity : register(u1, space1);
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> reflectWeight : register(u2, space1);
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> reflectNormal : register(u3, space1);
[[vk::binding(0, 2)]] ConstantBuffer<CompositeUniforms> view : register(b0, space2);

struct WaterShadows : IShadowMaps
{
    VsmParams vsm() { return view.vsm; }
    float3 sunDirection() { return view.sunDirection.xyz; }
    uint resident(uint entry) { return residentPage(view.vsm, vsmState, entry); }
    float compare(float2 uv, float ref) { return vsmAtlas.SampleCmpLevelZero(uv, ref); }
};

struct WaterProbes : IProbeTextures
{
    float3 irradiance(float2 uv) { return probeIrradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return probeDistanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

float3 rayDir(float2 pixel)
{
    return cameraRay(pixel, float2(view.params.xy), view.jitter.xy, view.cameraForward.xyz, view.cameraRight, view.cameraUp);
}

// The point at reverse-Z depth d along this pixel's ray (d = 0: the sky).
float3 pointAt(float3 dir, float d)
{
    return view.cameraPos.xyz + dir * (view.cameraPos.w / max(d, 1e-12) / dot(dir, view.cameraForward.xyz));
}

// Unit direction from p back to the eye (n at the eye itself: the fog's light is looked up there).
float3 toEye(float3 p, float3 n)
{
    const float3 e = view.cameraPos.xyz - p;
    return dot(e, e) > 1e-8 ? normalize(e) : n;
}

// The probe GI's light falling on a surface at p facing n (irradiance).
float3 ambientAround(float3 p, float3 n)
{
    if (view.giParams.z == 0.0)
        return flatAmbient(n);
    const WaterProbes probes;
    return probeIrradiance(view.probes, probes, p, n, toEye(p, n)) * kPi;
}

// The light falling on the water.
float3 ambientAbove(float3 p) { return ambientAround(p, float3(0.0, 1.0, 0.0)); }

// GGX highlight (the sun's), with water's Fresnel left to the caller.
float sunHighlight(float3 n, float3 v, float3 l, float roughness)
{
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), NoL = saturate(dot(n, l)), NoV = max(dot(n, v), 1e-3);
    const float a = max(roughness * roughness, 0.002), a2 = a * a;
    const float d = NoH * NoH * (a2 - 1.0) + 1.0;
    const float D = a2 / (kPi * d * d);
    const float k = a * 0.5;
    const float G = NoL / (NoL * (1.0 - k) + k) * NoV / (NoV * (1.0 - k) + k);
    return D * G / (4.0 * NoV) ;  // times NoL / NoL: G above carries it
}

// The planar reflection at a pixel, bent by how far the surface's normal leans from the plane's:
// the frame's colour where the reflected point is, and how much to trust it: 0 where nothing
// landed (the sky, off screen, behind the camera, hidden), fading to 0 as the point nears the
// frame's top edge (past it the mirror has nothing: a hard line across the reflection). Gaps the
// mirror's stretching leaves between rows are filled from the nearest rows above and below.
float mirrorReflection(uint2 pix, float3 n, float roughness, out float3 color)
{
    color = 0.0;
    const int2 size = int2(view.params.xy);
    const float3 lean = n - float3(0.0, 1.0, 0.0);
    const float2 bend = float2(dot(lean, view.cameraRight.xyz), -dot(lean, view.cameraUp.xyz)) * (float(size.y) * 0.08);
    const int2 q0 = clamp(int2(float2(pix) + bend), int2(0, 0), size - 1);
    const int taps[7] = { 0, 1, -1, 2, -2, 3, -3 };
    for (uint t = 0; t < 7; ++t)
    {
        const int2 q = int2(q0.x, clamp(q0.y + taps[t], 0, size.y - 1));
        const uint s = mirrorSources.Load(uint(q.y * size.x + q.x) * 4);
        if (s == 0xffffffffu)
            continue;
        const float2 uv = (float2(s & 0xffffu, s >> 16) + 0.5) / float2(size);
        color = refractSource.SampleLevel(uv, min(roughness * roughness * 60.0, view.time.y)).rgb;
        return saturate(float(s >> 16) / (float(size.y) * 0.08));
    }
    return 0.0;
}

// The terrain's height at world (x, z), from its coarse map (bilinear; clamped at its edges).
float terrainHeightAt(float2 xz)
{
    const float2 c = (xz - view.terrainGrid.xy) * view.terrainGrid.w;
    return view.terrainHeights.x + terrainHeight.SampleLevel((c + 0.5) / view.terrainHeights.z, 0.0) * view.terrainHeights.y;
}

// A layer's colour from afar: its texture's mean times its tint (the default look until its
// textures are built).
float3 layerColor(uint layer, float3 p, float3 n)
{
    const uint at = 16 + layer * 80;
    if (layer >= uint(view.terrainHeights.w) || asfloat(terrainData.Load(at + 56)) == 0.0)
        return terrainLook(p, n).albedo;
    return asfloat(terrainData.Load3(at + 64)) * asfloat(terrainData.Load3(at + 32));
}

// Where a ray from p first meets the terrain (its distance, < 0: never): marched over the coarse
// map in steps of most of a sample (5% of the distance further out, at most 64), then bisected.
float traceTerrain(float3 p, float3 r)
{
    if (view.terrainGrid.z == 0.0 || r.y <= 1e-3)
        return -1.0;
    const float extent = (view.terrainHeights.z - 1.0) * view.terrainGrid.z;
    const float3 lo = float3(view.terrainGrid.x, view.terrainHeights.x, view.terrainGrid.y);
    const float3 hi = lo + float3(extent, view.terrainHeights.y, extent);
    const float3 inv = 1.0 / (abs(r) > 1e-6 ? r : 1e-6);
    const float3 a = (lo - p) * inv, b = (hi - p) * inv;
    const float3 near = min(a, b), far = max(a, b);
    const float t0 = max(max(near.x, near.z), 0.0);
    const float t1 = min(min(far.x, far.z), far.y);
    if (t0 >= t1)
        return -1.0;
    float prev = t0, t = t0 + 0.05;
    if (p.y + r.y * t - terrainHeightAt(p.xz + r.xz * t) < 0.0)
        return t;  // starting in the bank
    for (uint i = 0; i < 64 && t < t1; ++i)
    {
        prev = t;
        t = min(t + max(view.terrainGrid.z * 0.75, t * 0.05), t1);
        const float3 q = p + r * t;
        if (q.y < terrainHeightAt(q.xz))
        {
            float inside = t, outside = prev;
            for (uint k = 0; k < 5; ++k)
            {
                const float m = (inside + outside) * 0.5;
                const float3 s = p + r * m;
                if (s.y < terrainHeightAt(s.xz))
                    inside = m;
                else
                    outside = m;
            }
            return (inside + outside) * 0.5;
        }
    }
    return -1.0;
}

struct AlphaFilter : IRtFilter
{
    bool accept(uint instance, uint cluster, uint triangle, float2 bary)
    {
        return view.trace.z == 0 || rayAlphaAccept(scene, materialData, vtAlbedo, view.trace.x, 1, instance, cluster, triangle, bary);
    }
};

static const float kTraceMax = 32.0;  // (as the reflection pass's)

// What a mirror reflects where the frame holds nothing: what the ray hits through the scene's BVH,
// lit as the probe GI's rays light it (reflections.slang's traceFar: past kTraceMax by the probes'
// light there alone); without the BVH the terrain (its layers' colours, lit by the sun through the
// shadow map and by the probes); else the sky.
float3 farReflection(float3 p, float3 r, float roughness, uint lane)
{
    if (view.trace.y != 0 && view.giParams.z != 0.0)
    {
        AlphaFilter filter;
        const RtHit hit = rtTraceClosest(rt, p, r, 1e4, lane, filter);
        if (!hit.hit)
            return openRadiance(r) * view.giParams.y;
        const RaySurface s = raySurface(scene, materialData, vtAlbedo, view.trace.x, hit, p, r);
        const WaterProbes probes;
        if (s.backface)
            return 0.0;
        if (hit.t > kTraceMax)
            return s.albedo * probeIrradiance(view.probes, probes, s.position, s.normal, -r) * view.giParams.x + s.emissive;
        return rayHitLight(rt, s, r, view.sunDirection.xyz, view.sunColor.rgb, view.giParams.x, view.probes, probes, lane, filter);
    }
    const float t = traceTerrain(p, r);
    if (t >= 0.0)
    {
        const float3 q = p + r * t;
        const float e = view.terrainGrid.z;
        const float3 n = normalize(float3(terrainHeightAt(q.xz - float2(e, 0.0)) - terrainHeightAt(q.xz + float2(e, 0.0)), 2.0 * e,
                                          terrainHeightAt(q.xz - float2(0.0, e)) - terrainHeightAt(q.xz + float2(0.0, e))));
        float3 albedo = terrainLook(q, n).albedo;
        if (view.terrainHeights.w > 0.0)
        {
            const float2 c = (q.xz - view.terrainGrid.xy) * view.terrainGrid.w;
            const uint4 m = uint4(round(terrainMaterial.SampleLevel((round(c) + 0.5) / view.terrainHeights.z, 0.0) * 255.0));
            albedo = lerp(layerColor(m.x, q, n), layerColor(m.y, q, n), float(m.z) / 255.0);
        }
        const float3 sunL = view.sunDirection.xyz;
        float3 light = ambientAround(q, n);
        const float NoL = dot(n, sunL);
        if (NoL > 0.0)
        {
            const WaterShadows sm = {};
            uint level;
            light += view.sunColor.rgb * (NoL * sunShadow(sm, q, n, length(q - view.cameraPos.xyz), 0.0, level));
        }
        return albedo * light * (1.0 / kPi);
    }
    return view.giParams.z == 0.0 ? openRadiance(r) : openRadiance(r) * view.giParams.y;
}

// How far a ray from p (in the tank) goes before it leaves the box.
float tankExit(WaterBody b, float3 p, float3 dir)
{
    const float2 lp = worldToBody(b, p);
    const float c = cos(b.posYaw.w), s = sin(b.posYaw.w);
    const float2 ld = float2(dir.x * c - dir.z * s, dir.x * s + dir.z * c);
    const float3 o = float3(lp.x, p.y - b.posYaw.y, lp.y), d = float3(ld.x, dir.y, ld.y);
    const float3 lo = float3(-b.shape.y, -b.shape.z, -b.shape.w), hi = float3(b.shape.y, 0.05, b.shape.w);
    const float3 inv = 1.0 / (abs(d) > 1e-6 ? d : float3(1e-6, 1e-6, 1e-6));
    const float3 t1 = (lo - o) * inv, t2 = (hi - o) * inv;
    return max(min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z)), 0.0);
}

// How much more (or less) sunlight reaches a point `depth` metres under the surface for the
// waves focusing it: 1 without caustics. Looked up where the light entered the water.
float causticLight(WaterBody b, float3 bed, float depth, float footprint)
{
    if (b.caustic.x <= 0.0 || depth <= 0.0)
        return 1.0;
    const float3 sunL = view.sunDirection.xyz;
    const float2 entry = bed.xz + sunL.xz / max(sunL.y, 0.2) * depth;
    const float scale = b.caustic.y, t = view.time.x;
    const float lod = log2(max(footprint * 256.0 / scale, 1.0));
    const float2 drift = b.flow.xy * b.caustic.w * t;
    const float c1 = causticMap.SampleLevel((entry - drift) / scale + float2(0.013, 0.021) * t, lod) * 4.0;
    const float c2 = causticMap.SampleLevel(float2(entry.y, -entry.x) / (scale * 1.37) - float2(0.017, -0.011) * t, lod) * 4.0;
    const float c = min(c1, c2) * 1.6;  // (where both layers focus: the bright network)
    const float fade = saturate(1.0 - depth / max(b.caustic.z, 0.01)) * saturate(depth * 4.0) * saturate(sunL.y * 3.0);
    // Between the lines the floor still gets the scattered light: darkened less than the lines brighten.
    return max(1.0 + b.caustic.x * fade * (c > 1.0 ? c - 1.0 : (c - 1.0) * 0.35), 0.0);
}

// Under water: what this pixel sees through the water the camera is in.
void underwaterPixel(uint2 pix, float2 uv, float3 dir, WaterBody cb, float surfaceY)
{
    const uint2 size = view.params.xy;
    const float4 extra = waterExtra.SampleLevel(uv, 0.0);
    const float wd = waterDepth.SampleLevel(uv, 0.0), sd = sceneDepth.SampleLevel(uv, 0.0);
    const float3 sunL = view.sunDirection.xyz;
    const float footprint = 2.0 * view.cameraUp.w / float(size.y);
    const float3 extinction = cb.fog.w * lerp(1.0, 3.0, 1.0 - cb.shallow.rgb);
    float3 color = refractSource.SampleLevel(uv, 0.0).rgb;  // the frame before water
    float dist;
    const bool surface = extra.y < 0.0 && wd > sd;  // the water's surface from below, nearer than the scene
    const bool tank = uint(cb.shape.x) == kWaterBox;
    if (surface && extra.w > 0.5)
    {
        // A tank's wall from inside: a window onto the scene past it; the fog stops there.
        dist = length(pointAt(dir, wd) - view.cameraPos.xyz);
    }
    else if (surface)
    {
        const float3 p = pointAt(dir, wd);
        const float3 n = octDecode(waterNormals.SampleLevel(uv, 0.0).xy);  // (facing the camera: down)
        // Snell's window: leaving water (index 1.33) for air; past the critical angle, total
        // internal reflection mirrors the water's own colour.
        const float cosI = saturate(dot(-dir, n));
        const float sin2T = 1.33 * 1.33 * (1.0 - cosI * cosI);
        const float3 fogLight = cb.fog.rgb * (view.sunColor.rgb * saturate(sunL.y) * 0.3 + ambientAbove(p));
        if (sin2T >= 1.0)
        {
            color = fogLight * (0.7 + 0.6 * saturate(dot(n, -sunL) * 0.5 + 0.5));  // (the waves' shading)
        }
        else
        {
            const float transmit = 1.0 - waterFresnel(sqrt(1.0 - sin2T));
            const float2 bend = float2(dot(n, view.cameraRight.xyz), -dot(n, view.cameraUp.xyz)) * 0.04;
            color = refractSource.SampleLevel(saturate(uv + bend), 0.0).rgb * transmit + fogLight * (1.0 - transmit);
        }
        dist = length(p - view.cameraPos.xyz);
        const float4 prevClip = mul(view.prevViewProj, float4(p, 1.0));
        const float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-6);
        const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(size);
        const float2 unjittered = float2(pix) + 0.5 - float2(view.jitter.x, -view.jitter.y) * 0.5 * float2(size);
        velocity[pix] = prevClip.w > 0.0 ? clamp(prevPixel - unjittered, -16384.0, 16384.0) : float2(-16384.0, -16384.0);
    }
    else
    {
        const float3 end = sd > 0.0 ? pointAt(dir, sd) : view.cameraPos.xyz + dir * 1e3;
        dist = length(end - view.cameraPos.xyz);
        const float inside = tank ? tankExit(cb, view.cameraPos.xyz, dir) : 1e30;
        if (sd > 0.0 && end.y < surfaceY && dist <= inside)  // (in a tank: only what is in it)
            color *= causticLight(cb, end, surfaceY - end.y, footprint * dist);
        dist = min(dist, inside);
    }
    // The water between: its fog, lit by what reaches the camera's depth.
    const float camDepth = max(surfaceY - view.cameraPos.y, 0.0);
    const float3 light = (view.sunColor.rgb * saturate(sunL.y) * 0.3 + ambientAbove(view.cameraPos.xyz)) * exp(-extinction * camDepth);
    const float3 transmit = exp(-extinction * min(dist, 1e3));
    color = color * transmit + cb.fog.rgb * light * (1.0 - transmit);
    output[pix] = float4(color, 1.0);
    reflectWeight[pix] = float4(0.0, 0.0, 0.0, 2.0);  // (no reflections under water)
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_composite(uint3 tid : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    const uint2 size = view.params.xy;
    const uint2 pix = view.params.zw + tid.xy;
    if (pix.x >= size.x || pix.y >= size.y)
        return;
    const float2 invSize = 1.0 / float2(size);
    const float2 uv = (float2(pix) + 0.5) * invSize;
    // The camera in a body: pixels whose near point is under its surface are seen through it.
    if (view.underwater.x > 0.0)
    {
        const WaterBody cb = loadWaterBody(bodies, uint(view.underwater.x) - 1);
        const float3 d = rayDir(float2(pix) + 0.5);
        const float3 near = view.cameraPos.xyz + d * (view.cameraPos.w / dot(d, view.cameraForward.xyz));
        const float surfaceY = cb.posYaw.y + (oceanWaves(cb) ? 0.0 : gerstner(cb, near.xz, view.time.x, 1.0).offset.y);
        if (near.y < surfaceY)
        {
            underwaterPixel(pix, uv, d, cb, surfaceY);
            return;
        }
    }
    const float4 extra = waterExtra.SampleLevel(uv, 0.0);
    if (extra.y <= 0.0)
        return;  // no water here (or its underside, seen from above the water: a tank from below)
    const WaterBody b = loadWaterBody(bodies, uint(abs(extra.y)) - 1);
    const float wd = waterDepth.SampleLevel(uv, 0.0);
    const float sd = sceneDepth.SampleLevel(uv, 0.0);
    const float2 pixel = float2(pix) + 0.5;
    const float3 dir = rayDir(pixel);
    const float3 p = pointAt(dir, wd);
    const float4 packedN = waterNormals.SampleLevel(uv, 0.0);
    const float3 n = octDecode(packedN.xy), macro = octDecode(packedN.zw);
    const float3 v = -dir;
    const float NoV = saturate(dot(n, v));
    const float distance = length(p - view.cameraPos.xyz);

    // What is under the surface, bent by it: the offset follows the surface's slope on screen,
    // growing with the water's thickness there (thin water bends little).
    const float3 under = sd > 0.0 ? pointAt(dir, sd) : p + dir * 1e4;
    // Nothing under the water within its depth (a pool floating in an empty scene, the sky past an
    // edge): its depth is where its bottom is.
    float bottom = b.shape.z > 0.0 ? b.shape.z / max(abs(dir.y), 0.02) : 1e4;
    if (uint(b.shape.x) == kWaterBox)  // a tank: where the ray leaves its box
        bottom = tankExit(b, p, dir);
    float thickness = min(length(under - p), bottom);
    const float2 slope = float2(dot(n, view.cameraRight.xyz), -dot(n, view.cameraUp.xyz));
    float2 uvR = uv + slope * (b.optics.y * 0.06 * saturate(thickness * 0.5) / max(distance * 0.1, 1.0));
    float sdR = sceneDepth.SampleLevel(uvR, 0.0);
    if (sdR > wd || any(uvR < 0.0) || any(uvR > 1.0))  // something in front of the water there: no bend
    {
        uvR = uv;
        sdR = sd;
    }
    else
    {
        thickness = min(sdR > 0.0 ? length(pointAt(dir, sdR) - p) : 1e4, bottom);  // (along the unbent ray: close enough)
    }
    const float mip = min(log2(1.0 + b.optics.x * thickness * 4.0), view.time.y);
    float3 behind = refractSource.SampleLevel(uvR, mip).rgb;

    // Light through the water: absorbed per colour (the shallow tint), over `clarity` metres to a tenth.
    const float sigma = 2.302585 / max(b.deep.w, 0.01);
    const float3 extinction = sigma * lerp(1.0, 3.0, 1.0 - b.shallow.rgb);
    const float3 transmit = exp(-extinction * min(thickness, 1e4));

    // Light reaching the water: the sun (shadowed at the surface) and the sky / GI above.
    const float3 sunL = view.sunDirection.xyz;
    const WaterShadows sm = {};
    uint level;
    const float shadow = sunShadow(sm, p, macro, distance, 0.0, level);
    const float3 sunIn = view.sunColor.rgb * shadow;
    const float3 ambient = ambientAbove(p);
    const float3 scattered = b.deep.rgb * b.shallow.w * (sunIn * (saturate(sunL.y) * 0.3183099) + ambient);
    // The edge: over the last edgeFade metres of depth, the water fades into the shore under it.
    const float depthBelow = sd > 0.0 ? b.posYaw.y - under.y : 1e4;
    // Caustics on what is under the water, where the sun reaches it.
    if (sd > 0.0 && b.caustic.x > 0.0 && depthBelow > 0.0 && depthBelow < b.caustic.z)
    {
        const float bedDistance = length(under - view.cameraPos.xyz);
        uint bedLevel;
        const float bedSun = sunShadow(sm, under, float3(0.0, 1.0, 0.0), bedDistance, 0.0, bedLevel);
        behind *= lerp(1.0, causticLight(b, under, depthBelow, 2.0 * view.cameraUp.w / float(size.y) * bedDistance), bedSun);
    }
    float3 water = behind * transmit + scattered * (1.0 - transmit);

    const bool tankFace = extra.w > 0.5 && extra.w < 2.0;  // (a tank's side or bottom: no shore there)
    const float edge = b.foam2.z > 0.0 && !tankFace ? saturate(depthBelow / b.foam2.z) : 1.0;
    water = lerp(refractSource.SampleLevel(uv, 0.0).rgb, water, edge);

    // Foam along shores and on crests, and film, as a diffuse layer on top.
    const float shore = b.foam.w > 0.0 && !tankFace ? saturate(1.0 - depthBelow / b.foam.w) : 0.0;
    float foamWant = max(shore * shore * shore, extra.x);
    // The patterns at this point, drifting with the flow (mip from the distance: no derivatives here).
    const float2 drift = b.flow.xy * b.caustic.w * view.time.x;
    const float lod = log2(max(distance * 2.0 * view.cameraUp.w / float(size.y) * 256.0, 1.0));
    float foamPattern = detailMap.SampleLevel((p.xz - drift) / b.foam2.y, lod - log2(b.foam2.y)).b;
    if (extra.w >= 2.0)
    {
        // Rivers: foam in streaks along the flow, drifting downstream (two phases, as the ripples).
        const float a = (extra.w - 2.0) * 2.0 * 3.14159265;
        const float2 down = float2(cos(a), sin(a)), across = float2(-down.y, down.x);
        float streaks = 0.0;
        for (uint phase = 0; phase < 2; ++phase)
        {
            const float cycle = frac(view.time.x / 4.0 + float(phase) * 0.5);
            const float2 q = float2(dot(p.xz, down) - b.caustic.w * cycle * 4.0, dot(p.xz, across)) + float2(0.37, 0.71) * float(phase);
            streaks += detailMap.SampleLevel(q / float2(b.foam2.y * 4.0, b.foam2.y * 0.7), lod - log2(b.foam2.y)).b * (1.0 - abs(cycle * 2.0 - 1.0));
        }
        foamPattern = streaks;
        foamWant = max(foamWant, saturate(abs(b.caustic.w) * 0.25) * b.foam2.x);
    }
    const float filmPattern = detailMap.SampleLevel((p.xz - drift) / b.film.w, lod - log2(b.film.w)).a;
    const float foam = saturate((foamWant - (1.0 - foamPattern)) * 3.0) * edge;
    const float film = b.detail.w > 0.0 ? saturate((filmPattern + b.detail.w - 1.0) * 5.0) * edge : 0.0;
    const float cover = max(foam, film);
    const float3 coverAlbedo = foam >= film ? b.foam.rgb : b.film.rgb;
    const float3 coverLit = coverAlbedo * (sunIn * saturate(dot(macro, sunL)) * 0.3183099 + ambient * 0.3183099);

    // Reflection weight (the reflection pass adds the scene's), and the sun's highlight.
    const float roughness = clamp(sqrt(b.optics.z * b.optics.z + extra.z * extra.z) + cover * 0.6, 0.02, 1.0);
    const float fresnel = min(waterFresnel(NoV) * b.optics.w, 1.0) * edge * (1.0 - cover);  // (strength 2 at a grazing angle: past 1)
    float3 color = lerp(water * (1.0 - fresnel), coverLit, cover);
    if (dot(macro, sunL) > 0.0)
        color += sunIn * (sunHighlight(n, v, sunL, roughness) * fresnel);
    // A mirror: the planar reflection (this body's plane), the terrain and sky where it has
    // nothing; else the reflection pass's trace.
    if (view.mirror.x == abs(extra.y) && extra.w < 0.5)
    {
        float3 mirrored;
        const float trust = mirrorReflection(pix, n, roughness, mirrored);
        if (trust < 1.0)
        {
            float3 r = reflect(dir, n);
            r.y = max(r.y, 0.0);
            mirrored = lerp(farReflection(p, normalize(r), roughness, lane), mirrored, trust);
        }
        color += mirrored * fresnel;
        reflectWeight[pix] = float4(0.0, 0.0, 0.0, 2.0);
    }
    else if (view.giParams.w != 0.0)
    {
        reflectWeight[pix] = float4(fresnel, fresnel, fresnel, roughness);
        reflectNormal[pix] = float4(octEncode(n), octEncode(macro));
    }
    else
    {
        color += openRadiance(reflect(dir, n)) * fresnel;
    }
    output[pix] = float4(color, 1.0);

    // Motion: the surface point under last frame's camera (the water moving is not counted).
    const float4 prevClip = mul(view.prevViewProj, float4(p, 1.0));
    const float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-6);
    const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(size);
    const float2 unjittered = pixel - float2(view.jitter.x, -view.jitter.y) * 0.5 * float2(size);
    velocity[pix] = prevClip.w > 0.0 ? clamp(prevPixel - unjittered, -16384.0, 16384.0) : float2(-16384.0, -16384.0);
}
