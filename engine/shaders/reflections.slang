// Reflections (after the resolve, which wrote the lit image without them, and each pixel's
// specular weight, roughness and normal): the reflected ray marched against this frame's depth
// and looked up in a copy of this frame's image (screen space: sharp, near), else what
// reflection_far.slang traced through the scene's BVH (off screen, behind things; the probes'
// light round the reflection for rough and faint ones). Added to the image times the weight. Only
// this frame is read: no history, no ghosting, the same frame from the same scene however it got
// there.

import scene_types;
import probe_common;
import reflection_common;
import sky;
import view_common;

[[vk::binding(0, 0)]] Sampler2D<float> sceneDepth : register(t0, space0) : register(s0, space0);  // reverse-Z, infinite far
[[vk::binding(1, 0)]] Sampler2D<float4> sceneColor : register(t1, space0) : register(s1, space0);  // this frame, with mips
[[vk::binding(2, 0)]] Sampler2D<float4> reflectWeight : register(t2, space0) : register(s2, space0);  // rgb weight, a roughness (2: none)
[[vk::binding(3, 0)]] Sampler2D<float4> reflectNormal : register(t3, space0) : register(s3, space0);  // xy shading, zw geometric normal (octahedral)
// cs_reflect: the probe GI's atlases (where nothing far was traced).
[[vk::binding(4, 0)]] Sampler2D<float4> probeIrradianceAtlas : register(t4, space0) : register(s4, space0);
[[vk::binding(5, 0)]] Sampler2D<float4> probeDistanceAtlas : register(t5, space0) : register(s5, space0);
[[vk::binding(6, 0)]] Sampler2D<float4> probeDataAtlas : register(t6, space0) : register(s6, space0);
// cs_reflect: the screen-space hits, traced at half resolution by cs_trace (point sampled), and
// the far reflections reflection_far.slang traced for the same pixels.
[[vk::binding(7, 0)]] Sampler2D<float4> hits : register(t7, space0) : register(s7, space0);
[[vk::binding(8, 0)]] Sampler2D<float4> farHits : register(t8, space0) : register(s8, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);  // HDR, reflections added
// For FSR (its transparency-and-composition mask): how much of the pixel is a screen-space
// reflection, whose content moves unlike the surface's motion vectors (less history there).
[[vk::binding(1, 1)]] [format("rgba16f")] RWTexture2D<float4> composition : register(u1, space1);
// cs_trace: per half-resolution pixel, its ray's hit: uv, the mip to read there, how much to trust it.
[[vk::binding(2, 1)]] [format("rgba32f")] RWTexture2D<float4> hitsOut : register(u2, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ReflectUniforms> view : register(b0, space2);

struct ReflectProbes : IProbeTextures
{
    float3 irradiance(float2 uv) { return probeIrradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return probeDistanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

// The camera ray through a (jittered) pixel, as the resolve's.
float3 rayDir(float2 pixel)
{
    return cameraRay(pixel, float2(view.params.xy), view.jitter.xy, view.cameraForward.xyz, view.cameraRight, view.cameraUp);
}

// A world point in this frame's raster (jittered, like the depth buffer): pixel and view depth.
bool toScreen(float3 q, out float2 pixel, out float z)
{
    const float3 d = q - view.cameraPos.xyz;
    z = dot(d, view.cameraForward.xyz);
    pixel = 0.0;
    if (z <= view.reflect.z)
        return false;
    const float2 ndc = float2(dot(d, view.cameraRight.xyz) / (view.cameraRight.w * z), dot(d, view.cameraUp.xyz) / (view.cameraUp.w * z)) +
                       view.jitter.xy;
    pixel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(view.params.xy);
    return true;
}

float sceneZ(float2 pixel)
{
    return view.reflect.z / max(sceneDepth.SampleLevel(pixel / float2(view.params.xy), 0.0), 1e-12);
}

// Screen space: the reflected ray walked across the screen pixel by pixel (McGuire and Mara's
// DDA: its view depth is linear in 1/z along the screen, so each step knows the depth range the
// ray covers in that pixel), up to kScreenSteps steps (longer rays stride several pixels, the hit
// then refined to its pixel). A hit is a pixel whose depth lies within the ray's range there, or
// at most a thickness behind it. Steps in world units (the march before) crossed a surface at a
// different step in each pixel, some refined onto it and some past it: stripes along glossy
// surfaces, like z-fighting. Where it hit: uv, the mip its cone's footprint asks for (rough
// surfaces, far hits), and how much to trust it (0: missed or left the screen; fading near its
// edges, at roughness's limit, and where the ray only grazed behind a surface).

struct ScreenHit
{
    float2 uv;
    float lod;
    float fade;
};

// How far behind a surface the ray may pass and still hit it (view depth units).
float thickness(float z) { return max(0.03, z * 0.015) * view.ssr.x; }
uint screenSteps() { return clamp(uint(view.ssr.z), 8u, 128u); }
uint traceScale() { return uint(view.ssr.w); }

ScreenHit traceScreen(float3 wpos, float3 n, float3 r, float roughness, float2 pixel)
{
    ScreenHit miss = {};
    const float2 size = float2(view.params.xy);
    const float3 forward = view.cameraForward.xyz;
    const float z0 = dot(wpos - view.cameraPos.xyz, forward);
    const float3 start = wpos + n * (0.01 + z0 * 0.002);
    // The ray's end: far enough to cross the screen, kept in front of the near plane.
    const float zStart = dot(start - view.cameraPos.xyz, forward);
    const float along = dot(r, forward);
    float reach = zStart * 4.0 + 50.0;
    if (along < 0.0)
        reach = min(reach, (view.reflect.z * 2.0 - zStart) / along);
    float2 p0, p1;
    float zA, zB;
    if (reach <= 0.0 || !toScreen(start, p0, zA) || !toScreen(start + r * reach, p1, zB))
        return miss;
    // Clipped to the screen (no steps spent off it); 1/z is linear along the screen.
    float k0 = 1.0 / zA, k1 = 1.0 / zB;
    float2 delta = p1 - p0;
    float exit = 1.0;
    for (int a = 0; a < 2; ++a)
        if (abs(delta[a]) > 1e-6)
            exit = min(exit, ((delta[a] > 0.0 ? size[a] : 0.0) - p0[a]) / delta[a]);
    exit = max(exit, 0.0);
    p1 = p0 + delta * exit;
    k1 = k0 + (k1 - k0) * exit;
    delta = p1 - p0;
    if (dot(delta, delta) < 1.0)
        return miss;
    // Along the longer axis, a pixel (or a stride of them) per step.
    const bool swapped = abs(delta.x) < abs(delta.y);
    if (swapped)
    {
        p0 = p0.yx;
        p1 = p1.yx;
        delta = delta.yx;
    }
    const float dir = delta.x > 0.0 ? 1.0 : -1.0;
    const float stride = max(1.0, ceil(abs(delta.x) / float(screenSteps())));
    const float2 dP = float2(dir, delta.y / abs(delta.x)) * stride;
    const float dk = (k1 - k0) / abs(delta.x) * stride;
    float2 P = p0;
    float k = k0;
    float zPrev = 1.0 / k0;  // the ray's depth where the last step's pixel ends
    bool wasInFront = true;
    for (uint i = 0; i < screenSteps(); ++i)
    {
        P += dP;
        k += dk;
        if ((P.x - p1.x) * dir > 0.0)
            break;
        // The depth range the ray covers over this step: from where the last one ended to
        // half a step on.
        const float zNext = 1.0 / max(k + dk * 0.5, 1e-7);
        const float zMin = min(zPrev, zNext), zMax = max(zPrev, zNext);
        zPrev = zNext;
        float2 px = swapped ? P.yx : P;
        float scene = sceneZ(px);
        // A hit: the pixel's depth within the ray's range there (or a thickness behind it), or
        // the ray gone from in front of the depth buffer to behind it since the last step (a long
        // stride steps past a surface it crosses at a grazing angle by more than the thickness:
        // rows that sampled it a little behind hit it, the next ones did not, in stripes).
        const bool inFront = 1.0 / k < scene;
        const bool crossed = wasInFront && !inFront;
        wasInFront = inFront;
        if (!crossed && (zMax < scene || zMin > scene + thickness(scene)))
            continue;
        // Back to where it went behind: the step's first point behind the depth buffer.
        float lo = 0.0, hi = 1.0;  // fraction of the step back from P
        for (uint b = 0; b < 5; ++b)
        {
            const float m = (lo + hi) * 0.5;
            const float2 q = P - dP * m;
            if (1.0 / max(k - dk * m, 1e-7) >= sceneZ(swapped ? q.yx : q))
                lo = m;  // still behind at q: it went behind further back
            else
                hi = m;
        }
        const float2 Pr = P - dP * lo;
        const float kr = k - dk * lo;
        const float2 pr = swapped ? Pr.yx : Pr;
        const float sr = sceneZ(pr);
        // Far behind what it found: it passed behind something nearer (a depth jump); nothing
        // to see here, it goes on (and may meet what is behind). Counting those smeared what was
        // in front along glossy surfaces.
        const float tolerance = thickness(sr) + (zMax - zMin) / 16.0;  // (the refined step's own depth range)
        if (1.0 / kr - sr > tolerance)
            continue;
        P = Pr;
        k = kr;
        px = pr;
        scene = sr;
        const float2 uv = px / size;
        const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
        // Grazing past a surface (behind it by most of the thickness): faded out.
        const float gap = max(1.0 / k - scene, 0.0);
        const float grazed = saturate(1.0 - gap / tolerance * 0.5);
        const float fade = edge.x * edge.y * grazed * saturate((view.reflect.y - roughness) / (view.reflect.y * 0.3));
        // The reflected cone's width at the hit, in pixels there: the mip to read.
        const float2 ndc = float2(px.x / size.x * 2.0 - 1.0, 1.0 - px.y / size.y * 2.0) - view.jitter.xy;
        const float3 hit = view.cameraPos.xyz + (forward + view.cameraRight.xyz * (ndc.x * view.cameraRight.w) +
                                                 view.cameraUp.xyz * (ndc.y * view.cameraUp.w)) * scene;
        const float distance = length(hit - start);
        const float footprint = roughness * roughness * distance * 2.0 / (2.0 * scene * view.cameraUp.w / size.y);
        ScreenHit h = { uv, log2(max(footprint, 1.0)), fade };
        return h;
    }
    return miss;
}

// Each pixel's screen-space hit, for its neighbours (8x8 group).
groupshared float2 gHitUv[64];
groupshared uint gHit[64];

// How far apart neighbouring pixels' hits land in the image (pixels): the reflection there is
// that image minified, and read from one texel per pixel it aliased: thin, far detail (a floor's
// grilles in a glossy ceiling, squeezed into a few rows) became dashes that crawled as the camera
// moved. Read from the mip this spacing asks for instead, as a texture would be. Per axis, the
// nearer of the two neighbours' hits counts: minified, both are far; at an object's edge, only one.
float hitSpread(uint2 g, float2 uv, float2 size)
{
    float spread = 0.0;
    for (uint axis = 0; axis < 2; ++axis)
    {
        float nearest = 1e9;
        for (int s = -1; s <= 1; s += 2)
        {
            int2 q = int2(g);
            q[axis] += s;
            if (any(q < 0) || any(q > 7))
                continue;
            const uint slot = uint(q.y) * 8 + uint(q.x);
            if (gHit[slot] != 0)
                nearest = min(nearest, length((gHitUv[slot] - uv) * size));
        }
        if (nearest < 1e9)
            spread = max(spread, nearest);
    }
    return spread;
}

// The surface a pixel shows: world position, shading and geometric normals.
void surfaceAt(float2 pixel, out float3 wpos, out float3 n, out float3 geometric)
{
    reflectSurface(view, sceneDepth, reflectNormal, pixel, wpos, n, geometric);
}

// The screen-space trace, at half resolution: one ray per 2x2 pixels (the top left one's), a
// quarter of the work (it was most of the reflections' cost: 0.6 of 0.85 ms at 1600x900).
// cs_reflect puts each pixel's reflection together from the four hits around it.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_trace(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    const uint2 size = view.params.xy;
    const uint scale = traceScale();
    const uint2 traced = (size + scale - 1) / scale;
    // (Every thread of the group reaches the barrier below: no early return before it.)
    const bool inside = all(tid.xy < traced);
    const float2 pixel = float2(min(tid.xy * scale, size - 1)) + 0.5;
    ScreenHit near = {};
    if (inside)
    {
        const float4 w = reflectWeight.SampleLevel(pixel / float2(size), 0.0);
        const float roughness = w.a;  // (2: sky, unlit, no specular)
        if (roughness < view.reflect.y)
        {
            float3 wpos, n, geometric;
            surfaceAt(pixel, wpos, n, geometric);
            const float3 r = reflect(rayDir(pixel), n);
            if (dot(r, n) > 0.0)
                near = traceScreen(wpos, n, r, roughness, pixel);
            // Strongly bumped (normal-mapped) pixels reflect in all directions from one pixel to the
            // next: sharp, they sparkle as the jitter moves. They take the traced reflection,
            // aimed by the surface itself, instead.
            near.fade *= saturate((dot(n, geometric) - 0.9) / 0.08);
        }
    }
    const uint slot = gtid.y * 8 + gtid.x;
    gHitUv[slot] = near.uv;
    gHit[slot] = near.fade > 0.0 ? 1 : 0;
    GroupMemoryBarrierWithGroupSync();
    if (!inside)
        return;
    // (Neighbours here are `scale` pixels apart.)
    const float lod = near.fade > 0.0 ? min(max(near.lod, log2(max(hitSpread(gtid.xy, near.uv, float2(size)) / float(scale), 1.0))), 6.0) : 0.0;
    hitsOut[tid.xy] = float4(near.uv, lod, near.fade);
}

// This pixel's screen-space reflection from the four half-resolution hits around it: each read
// where it hit, weighted bilinearly and by how well the surface its ray left from matches this
// pixel's (its distance from this pixel's plane, its normal), so a reflection doesn't bleed
// across an object's edge. a: how much to trust it (the hits' own trust, misses counting 0).
// far: the far reflections reflection_far.slang traced for the same four, weighted the same; a: 0
// where none of them is on this pixel's surface.
float4 screenReflection(float2 pixel, float3 wpos, float3 geometric, float z, bool screen, out float4 far)
{
    const uint2 size = view.params.xy;
    const uint scale = traceScale();
    const uint2 traced = (size + scale - 1) / scale;
    const float2 h = (pixel - 0.5) / float(scale);  // traced texel i: full pixel i * scale
    const int2 b = int2(floor(h));
    const float2 f = h - float2(b);
    const float bil[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
    float3 color = 0.0, farColor = 0.0;
    float trust = 0.0, weights = 0.0, hitWeights = 0.0;
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        const uint2 q = uint2(clamp(b + int2(k & 1, k >> 1), int2(0, 0), int2(traced) - 1));
        const float4 hit = hits.SampleLevel((float2(q) + 0.5) / float2(size), 0.0);  // (full-size texture)
        float3 sp, sn, sg;
        surfaceAt(float2(min(q * scale, size - 1)) + 0.5, sp, sn, sg);
        const float rel = abs(dot(sp - wpos, geometric)) / (0.01 * z + 0.002);
        const float w = max(bil[k], 0.01) * exp2(-rel * rel) * saturate((dot(sg, geometric) - 0.8) * 5.0);
        weights += w;
        farColor += w * farHits.SampleLevel((float2(q) + 0.5) / float2(size), 0.0).rgb;
        trust += w * hit.a;
        if (screen && hit.a > 0.0)
        {
            color += w * hit.a * sceneColor.SampleLevel(hit.xy, hit.z).rgb;
            hitWeights += w * hit.a;
        }
    }
    far = weights < 1e-4 ? 0.0 : float4(farColor / weights, 1.0);
    if (weights < 1e-4 || hitWeights <= 0.0)
        return 0.0;
    return float4(color / hitWeights, trust / weights);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_reflect(uint3 tid : SV_DispatchThreadID)
{
    const uint2 size = view.params.xy;
    if (any(tid.xy >= size))
        return;
    const float2 pixel = float2(tid.xy) + 0.5;
    const float2 uv = pixel / float2(size);
    const float4 w = reflectWeight.SampleLevel(uv, 0.0);
    if (w.a > 1.5)  // sky, unlit debug views, no specular
    {
        composition[tid.xy] = 0.0;
        if (view.params.z == 13)
            output[tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const float3 dir = rayDir(pixel);
    float3 wpos, n, geometric;
    surfaceAt(pixel, wpos, n, geometric);
    const float3 r = reflect(dir, n);
    const float roughness = w.a;
    const float z = dot(wpos - view.cameraPos.xyz, view.cameraForward.xyz);
    float4 traced;
    float4 near = screenReflection(pixel, wpos, geometric, z, roughness < view.reflect.y, traced);
    // Strongly bumped pixels take the traced reflection (as in cs_trace), by their own normal: a
    // neighbour's ray, aimed by its own bump, changed with the jitter.
    near.a *= saturate((dot(n, geometric) - 0.9) / 0.08);
    float3 far = traced.rgb;
    if (traced.a == 0.0)  // (no traced pixel on its surface: the probes alone)
    {
        ReflectProbes probes;
        const float3 farR = reflectFarDirection(dir, n, geometric, roughness);
        far = view.reflect.x == 0.0 ? flatAmbient(farR) : probeIrradiance(view.probes, probes, wpos, geometric, farR, -dir);
    }
    const float3 reflection = lerp(far, near.rgb, near.a);
    const float4 lit = output[tid.xy];
    const float3 added = w.rgb * reflection * view.ssr.y;
    const float share = dot(added, float3(0.2126, 0.7152, 0.0722)) / max(dot(lit.rgb + added, float3(0.2126, 0.7152, 0.0722)), 1e-4);
    composition[tid.xy] = float4(saturate(share * near.a), 0.0, 0.0, 0.0);
    if (view.params.z == 13)
        output[tid.xy] = float4(reflection, 1.0);  // debug: the reflections alone
    else
        output[tid.xy] = lit + float4(added, 0.0);
}
