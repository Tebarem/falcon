// Visibility-buffer resolve: per pixel, rebuild the triangle, intersect the camera ray with
// it (exact barycentrics, robust for triangles crossing the near plane), derive UV gradients
// analytically from the neighbouring pixels' rays, sample the virtual texture, and shade.
// Also writes virtual texture feedback for 1 in 16 pixels (rotating every frame).

import scene_types;
import skinning;
import culling;
import vt;
import lighting;
import vsm;
import probe_common;
import terrain_shading;
import shadows;
import sky;
import view_common;
import falcon_surface;
import project_surfaces;  // (R4: a project's build has its surface shaders here)

struct ResolveUniforms
{
    float4x4 prevViewProj;  // previous frame, unjittered: motion vectors
    float4 cameraPos;
    float4 cameraRight;    // w = 1 / P00
    float4 cameraUp;       // w = 1 / P11
    float4 cameraForward;
    float4 sunDirection;
    uint4 params;          // width, height, debug mode, frame
    uint4 vtParams;        // page-table byte offset, feedback capacity, page count, materials' params offset (R4)
    float4 vtParamsF;      // mip bias, seconds (R4)
    float4 sunColor;
    LightGrid lightGrid;
    VsmParams vsm;
    float4 jitter;         // xy: NDC jitter of this frame's raster (rays are un-jittered by it)
    ProbeVolume probes;    // probe GI (probe_common.slang)
    uint4 ambient;         // x: AO scale; y: probe GI on; zw: the AO's sample offset
    uint4 terrain;         // x: terrainSurface holds this frame's terrain pixels; y: the live vertices' extra
                           // stream's address, less the first live vertex's (+ vertex * 8); z: layers bound
    float4 reflect;        // x: the reflection pass adds the specular reflections (reflections.slang)
    uint4 decals;          // count, words per tile, tiles across, tile size (pixels)
};

[[vk::binding(0, 0)]] Sampler2DArray<float4> vtAlbedo : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2DArray<float4> vtNormal : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2DArray<float4> vtOrm : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2DShadow vsmAtlas : register(t3, space0) : register(s3, space0);
// GTAO / SSAO (ao.slang): r = AO, g = view depth.
[[vk::binding(4, 0)]] Sampler2D<float2> aoTexture : register(t4, space0) : register(s4, space0);
// Probe GI (render/probe_gi.h): irradiance and distance atlases (bilinear), probe data (point).
[[vk::binding(5, 0)]] Sampler2D<float4> probeIrradianceAtlas : register(t5, space0) : register(s5, space0);
[[vk::binding(6, 0)]] Sampler2D<float4> probeDistanceAtlas : register(t6, space0) : register(s6, space0);
[[vk::binding(7, 0)]] Sampler2D<float4> probeDataAtlas : register(t7, space0) : register(s7, space0);
// Read-only storage texture: SDL binds it as an SRV (D3D12) / SAMPLED_IMAGE (Vulkan), so a
// plain Texture2D works on both, and integer formats need no sampler support.
// The layered level meshes' masks (render/surface_layers.h): a layer each, RGBA = layers 1..3, wear.
[[vk::binding(8, 0)]] Sampler2DArray<float4> layerMasks : register(t8, space0) : register(s8, space0);
[[vk::binding(9, 0)]] Texture2D<uint> visBuffer : register(t9, space0);
// The terrain's material and normal per pixel (terrain_surface.slang), while view.terrain.x.
[[vk::binding(10, 0)]] Texture2D<uint4> terrainSurface : register(t10, space0);
// Decals (render/decal_renderer.h): 9 texels each (the box's inverse, its axes and sizes, tint,
// fades, material / flags / steps), and per 32x32 tile a bitmask of those that may cover it (words
// per tile: view.decals.y), in their order.
[[vk::binding(11, 0)]] Texture2D<float4> decalData : register(t11, space0);
[[vk::binding(12, 0)]] Texture2D<uint> decalBins : register(t12, space0);
// Their records, one texel each: x materials 1 | 2 << 16, y material 3 | blend << 16 (0xffff: none),
// z the mask's layer.
[[vk::binding(13, 0)]] Texture2D<uint4> layerRecords : register(t13, space0);
[[vk::binding(14, 0)]] ByteAddressBuffer visible : register(t14, space0);
[[vk::binding(15, 0)]] ByteAddressBuffer scene : register(t15, space0);
[[vk::binding(16, 0)]] ByteAddressBuffer materialData : register(t16, space0);  // materials, then page table
[[vk::binding(17, 0)]] ByteAddressBuffer lights : register(t17, space0);
[[vk::binding(18, 0)]] ByteAddressBuffer lightGrid : register(t18, space0);
[[vk::binding(19, 0)]] ByteAddressBuffer vsmState : register(t19, space0);
[[vk::binding(20, 0)]] ByteAddressBuffer prevInstances : register(t20, space0);  // last frame's instance table
[[vk::binding(21, 0)]] ByteAddressBuffer skin : register(t21, space0);  // skinned instances
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);  // HDR
[[vk::binding(1, 1)]] [format("rg16f")] RWTexture2D<float2> velocity : register(u1, space1);  // pixels to last frame
// For the reflection pass: rgb the specular weight (split-sum BRDF x occlusion), a roughness
// (2: nothing to reflect: sky, unlit views); the shading and geometric normals (octahedral).
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> reflectWeight : register(u2, space1);
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> reflectNormal : register(u3, space1);
[[vk::binding(4, 1)]] RWByteAddressBuffer feedbackList : register(u4, space1);
[[vk::binding(5, 1)]] RWByteAddressBuffer feedbackMask : register(u5, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ResolveUniforms> view : register(b0, space2);

static const float kPi = 3.14159265;

float3 lodColor(uint depth)
{
    const float3 palette[8] = { float3(0.15, 0.45, 1.0), float3(0.1, 0.85, 0.85), float3(0.2, 0.9, 0.3),
                                float3(0.85, 0.95, 0.2), float3(1.0, 0.6, 0.1), float3(1.0, 0.25, 0.2),
                                float3(0.9, 0.2, 0.75), float3(0.6, 0.35, 1.0) };
    return palette[min(depth, 7u)];
}

// Where this pixel's (jittered) ray points without the TAA jitter: motion vectors start there,
// so a still camera has no motion. (Measured from the pixel centre, the jitter moved the
// history fetch by a different subpixel offset every frame: the image wobbled.)
float2 unjittered(float2 pixel)
{
    return pixel - float2(view.jitter.x, -view.jitter.y) * 0.5 * float2(view.params.xy);
}

// The sky's motion: its direction under last frame's camera rotation (infinitely far, so the
// camera's movement doesn't count). Upscalers (FSR) take the sky's motion like any surface's.
float2 skyMotion(float2 pixel, float3 dir)
{
    const float4 prevClip = mul(view.prevViewProj, float4(dir, 0.0));
    if (prevClip.w <= 1e-6)
        return float2(-16384.0, -16384.0);
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(view.params.xy);
    return clamp(prevPixel - unjittered(pixel), -16384.0, 16384.0);
}

// Share of sunlight a foliage card passes to its far side (diffuse, tinted by its albedo).
static const float kFoliageTransmission = 0.5;
// Foliage receivers look up the sun's shadow this many texels nearer the sun: a thin card at a
// grazing angle to the light otherwise shadows itself through the 5x5 filter. Other cards
// further than that toward the sun still shadow it.
static const float kFoliageLightBias = 4.0;

float3 rayDir(float2 pixel)
{
    return cameraRay(pixel, float2(view.params.xy), view.jitter.xy, view.cameraForward.xyz, view.cameraRight, view.cameraUp);
}

// Moller-Trumbore without culling or bounds: also valid for the neighbouring pixels' rays,
// which extend the triangle's plane (that is what makes the UV gradients analytic).
float3 barycentrics(float3 p0, float3 e1, float3 e2, float3 origin, float3 dir)
{
    const float3 pv = cross(dir, e2);
    const float det = dot(e1, pv);
    const float inv = abs(det) > 1e-20 ? 1.0 / det : 0.0;
    const float3 tv = origin - p0;
    const float u = dot(tv, pv) * inv;
    const float v = dot(dir, cross(tv, e1)) * inv;
    return float3(1.0 - u - v, u, v);
}

struct Surface
{
    float4 albedo;  // linear rgb, alpha
    float2 normalXY;
    float3 orm;
    float height;   // the ORM pages' alpha: a material's height map (decals' parallax), 1 without
    uint mipWanted, mipUsed;
};

Surface sampleSurface(MaterialGpu m, float2 uv, float2 dUVdx, float2 dUVdy, uint2 pixel)
{
    Surface s;
    s.albedo = 1.0;
    s.normalXY = 0.5;
    s.orm = 1.0;
    s.height = 1.0;
    s.mipWanted = s.mipUsed = 0;
    if (m.vtPageTable == 0xffffffffu)
        return s;

    const uint mips = vtMipCount(m);
    const float mip = vtMipLevel(m, dUVdx, dUVdy, view.vtParamsF.x);
    const uint m0 = min(uint(mip), mips - 1);
    const float blend = m0 + 1 < mips ? frac(mip) : 0.0;
    const float2 wrapped = frac(uv);  // repeat addressing (pages carry wrapped borders)

    // Trilinear: two levels, each falling back to the nearest coarser resident page.
    const VtPage a = vtFind(materialData, view.vtParams.x, m, wrapped, m0);
    const VtPage b = blend > 0.0 ? vtFind(materialData, view.vtParams.x, m, wrapped, m0 + 1) : a;
    const float3 ca = float3(a.uv, float(a.layer)), cb = float3(b.uv, float(b.layer));
    s.albedo = lerp(vtAlbedo.SampleLevel(ca, 0.0), vtAlbedo.SampleLevel(cb, 0.0), blend);
    s.normalXY = lerp(vtNormal.SampleLevel(ca, 0.0).xy, vtNormal.SampleLevel(cb, 0.0).xy, blend);
    const float4 orm = lerp(vtOrm.SampleLevel(ca, 0.0), vtOrm.SampleLevel(cb, 0.0), blend);
    s.orm = orm.xyz;
    s.height = orm.w;
    s.mipWanted = m0;
    s.mipUsed = a.mip;

    // Feedback: 1 pixel in each 4x4 tile per frame, deduplicated per page with a bitmask.
    const uint frame = view.params.w;
    if (((pixel.x + frame) & 3) == 0 && ((pixel.y + (frame >> 2)) & 3) == 0)
    {
        float2 unused;
        const uint e = vtEntry(m, m0, wrapped, unused);
        uint old;
        feedbackMask.InterlockedOr((e >> 5) * 4, 1u << (e & 31), old);
        if ((old & (1u << (e & 31))) == 0)
        {
            uint slot;
            feedbackList.InterlockedAdd(0, 1, slot);
            if (slot < view.vtParams.y)
                feedbackList.Store((1 + slot) * 4, e);
        }
    }
    return s;
}

// A layered level mesh's surface (kInstanceLayered): up to three materials over its own, blended by
// height where its mask says (terrain_surface.slang's blend), and its vertices' colours. The result
// is the base surface with the factors folded in (the caller's material factors then set to 1).
struct LayeredLook
{
    float3 tint;
    float occlusion;
};

void layerSurface(uint index, float2 uv, float2 dUVdx, float2 dUVdy, uint2 pixel, out float3 albedo, out float3 orm, out float2 nxy,
                  out float height)
{
    const MaterialGpu lm = materialData.Load<MaterialGpu>(index * kMaterialStride);
    const Surface ls = sampleSurface(lm, uv, dUVdx, dUVdy, pixel);
    albedo = ls.albedo.rgb * lm.baseColor.rgb;
    orm = float3(lerp(1.0, ls.orm.r, lm.occlusionStrength), materialRoughness(lm, ls.orm.g), ls.orm.b * lm.metallic);
    nxy = (ls.normalXY * 2.0 - 1.0) * lm.normalScale;
    height = ls.height;
}

LayeredLook blendLayers(InstanceGpu inst, ClusterGpu cl, uint tri, float3 bary, float3 baryX, float3 baryY, float2 uv, float2 uvX,
                        float2 uvY, uint2 pixel, inout Surface s, inout MaterialGpu m)
{
    LayeredLook look;
    float2 uv1 = 0.0, uv1X = 0.0, uv1Y = 0.0;
    float4 color = 0.0;
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const uint g = loadClusterVertex(scene, cl, (tri >> (8 * k)) & 0xff);
        const uint2 extra = scene.Load2(view.terrain.y + g * 8);
        const float2 e = float2(extra.x & 0xffff, extra.x >> 16) / 65535.0;
        const float4 c = float4(extra.y & 0xff, (extra.y >> 8) & 0xff, (extra.y >> 16) & 0xff, extra.y >> 24) / 255.0;
        uv1 += e * bary[k];
        uv1X += e * baryX[k];
        uv1Y += e * baryY[k];
        color += c * bary[k];
    }
    look.tint = color.rgb;
    look.occlusion = color.a;
    const uint4 record = layerRecords.Load(int3(inst.pad1, 0, 0));
    const float4 mask = layerMasks.SampleGrad(float3(uv1, float(record.z)), uv1X - uv1, uv1Y - uv1);
    const uint layerMaterial[3] = {record.x & 0xffff, record.x >> 16, record.y & 0xffff};
    const float blendWidth = max(float(record.y >> 16) / 65535.0, 1e-3);
    // The base: the instance's own material, weighted by what the layers leave.
    float3 albedo[4], orm[4];
    float2 nxy[4];
    float height[4], weight[4];
    albedo[0] = s.albedo.rgb * m.baseColor.rgb;
    orm[0] = float3(lerp(1.0, s.orm.r, m.occlusionStrength), materialRoughness(m, s.orm.g), s.orm.b * m.metallic);
    nxy[0] = (s.normalXY * 2.0 - 1.0) * m.normalScale;
    height[0] = s.height;
    weight[0] = 1.0 - max(max(mask.r, mask.g), mask.b);
    float best = height[0] + weight[0];
    [unroll]
    for (uint l = 0; l < 3; ++l)
    {
        weight[l + 1] = layerMaterial[l] != 0xffffu ? mask[l] : 0.0;
        albedo[l + 1] = albedo[0];
        orm[l + 1] = orm[0];
        nxy[l + 1] = nxy[0];
        height[l + 1] = 0.0;
        if (weight[l + 1] > 0.001)
        {
            layerSurface(layerMaterial[l], uv, uvX - uv, uvY - uv, pixel, albedo[l + 1], orm[l + 1], nxy[l + 1], height[l + 1]);
            best = max(best, height[l + 1] + weight[l + 1]);
        }
    }
    float3 a = 0.0, o = 0.0;
    float2 n = 0.0;
    float total = 0.0;
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const float w = max(height[i] + weight[i] - (best - blendWidth), 0.0) * weight[i];
        a += albedo[i] * w;
        o += orm[i] * w;
        n += nxy[i] * w;
        total += w;
    }
    if (total > 1e-5)
    {
        s.albedo.rgb = a / total;
        s.orm = o / total;
        s.normalXY = n / total * 0.5 + 0.5;
        m.baseColor.rgb = 1.0;
        m.roughness = 1.0;
        m.vtSize &= 0x00ffffffu;  // (each layer's roughness min is in its blend already)
        m.metallic = 1.0;
        m.occlusionStrength = 1.0;
        m.normalScale = 1.0;
    }
    return look;
}

// ---- Decals

struct DecalShade
{
    float3 albedo;     // premultiplied by coverage
    float roughness;   // the same
    float3 normal;     // the same (a world normal's share)
    float cavity;      // the same: how much darker (1 - its height's darkening)
    float coverage;
};

float4 decalRow(uint i, uint row) { return decalData.Load(int3(row, i, 0)); }

// The height (0 deepest .. 1 the surface) of a decal's material at uv, one mip.
float decalHeight(MaterialGpu m, float2 uv, uint mip)
{
    const VtPage pg = vtFind(materialData, view.vtParams.x, m, saturate(uv) * 0.99999, mip);
    return vtOrm.SampleLevel(float3(pg.uv, float(pg.layer)), 0.0).w;
}

// ---- Parallax occlusion: a material's height map (the ORM pages' alpha, 1 the surface) `parallax`
// metres deep under it (flags bits 16..31, mm), drawn by stepping the view ray down through it from
// the triangle's plane; the surface is sampled where the ray meets it. No geometry changes.

float surfaceHeight(MaterialGpu m, float2 uv, uint mip)
{
    const VtPage pg = vtFind(materialData, view.vtParams.x, m, frac(uv), mip);
    return vtOrm.SampleLevel(float3(pg.uv, float(pg.layer)), 0.0).w;
}

// The UV change across a world step `w` in a triangle's plane (e1, e2: its edges; d1, d2: theirs in UV).
float2 uvAcross(float3 e1, float3 e2, float2 d1, float2 d2, float3 w)
{
    const float a = dot(e1, e1), b = dot(e1, e2), c = dot(e2, e2);
    const float det = a * c - b * b;
    if (abs(det) < 1e-20)
        return 0.0;
    const float x = dot(w, e1), y = dot(w, e2);
    return d1 * ((x * c - y * b) / det) + d2 * ((a * y - b * x) / det);
}

float2 parallaxUv(MaterialGpu m, float2 uv, float2 dUVdx, float2 dUVdy, float3 e1, float3 e2, float2 d1, float2 d2, float3 dir)
{
    const float depth = f16tof32(m.flags >> 16);
    if (depth <= 0.0 || m.vtPageTable == 0xffffffffu)
        return uv;
    float3 g = normalize(cross(e1, e2));
    if (dot(g, dir) > 0.0)
        g = -g;  // (the side the camera sees)
    const float facing = max(-dot(dir, g), 0.12);
    // How far along the surface (in UV) the ray has gone at the full depth.
    const float2 maxShift = uvAcross(e1, e2, d1, d2, (dir - g * dot(dir, g)) * (depth / facing));
    // (less than half a pixel of relief, far off: none)
    if (dot(maxShift, maxShift) < 0.25 * max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy)))
        return uv;
    const uint mip = min(uint(vtMipLevel(m, dUVdx, dUVdy, view.vtParamsF.x)), vtMipCount(m) - 1);
    // (a step every two pixels or so the ray crosses, 8..96)
    const float pixels = sqrt(dot(maxShift, maxShift) / max(max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy)), 1e-20));
    const float steps = clamp(max(lerp(32.0, 8.0, facing), pixels * 0.5), 8.0, 96.0);
    const float layer = 1.0 / steps;
    float d = 0.0, prevD = 0.0;
    float2 cur = uv, prev = uv;
    float h = surfaceHeight(m, cur, mip), prevH = h;
    for (uint k = 0; k < 96 && float(k) < steps && d < 1.0 - h; ++k)
    {
        prev = cur;
        prevD = d;
        prevH = h;
        cur += maxShift * layer;
        d += layer;
        h = surfaceHeight(m, cur, mip);
    }
    // Between the last two steps, where the ray crosses the height.
    const float after = (1.0 - h) - d, before = (1.0 - prevH) - prevD;
    const float t = abs(after - before) > 1e-5 ? saturate(before / (before - after)) : 1.0;
    return lerp(prev, cur, t);
}

// How much sun reaches the point the view ray met in the height map: the ray toward the sun, up
// through the height map from there to the surface; soft by how far the relief rises over it.
float parallaxShadow(MaterialGpu m, float2 uv, float2 dUVdx, float2 dUVdy, float3 e1, float3 e2, float2 d1, float2 d2, float3 dir,
                     float3 sun)
{
    const float depth = f16tof32(m.flags >> 16);
    if (depth <= 0.0 || m.vtPageTable == 0xffffffffu)
        return 1.0;
    float3 g = normalize(cross(e1, e2));
    if (dot(g, dir) > 0.0)
        g = -g;
    const float up = dot(sun, g);
    if (up <= 0.0)
        return 1.0;  // (the sun behind the face: the face's own shadow test says so)
    const float2 toSun = uvAcross(e1, e2, d1, d2, (sun - g * up) * (depth / max(up, 0.12)));
    if (dot(toSun, toSun) < 0.25 * max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy)))
        return 1.0;
    const uint mip = min(uint(vtMipLevel(m, dUVdx, dUVdy, view.vtParamsF.x)), vtMipCount(m) - 1);
    const float h0 = surfaceHeight(m, uv, mip);
    const float pixels = sqrt(dot(toSun, toSun) / max(max(dot(dUVdx, dUVdx), dot(dUVdy, dUVdy)), 1e-20));
    const uint steps = uint(clamp(pixels * 0.25, 12.0, 48.0));
    float lit = 1.0;
    [loop]
    for (uint k = 1; k <= steps && h0 < 0.999; ++k)
    {
        const float t = float(k) / float(steps);
        const float rayH = h0 + (1.0 - h0) * t;  // (at the surface after the depth it starts at)
        const float h = surfaceHeight(m, uv + toSun * ((1.0 - h0) * t), mip);
        lit = min(lit, 1.0 - saturate((h - rayH) * 10.0));
    }
    return lit;
}

// The decals over a surface point: each one whose box holds it, in order, faded by how much the
// surface turns away from it and toward its box's depth, its material stepped through by the view
// ray (parallax occlusion: its height map), blended over the ones before it.
DecalShade shadeDecals(uint2 pixel, float3 wpos, float3 wposX, float3 wposY, float3 geoN, float3 dir, bool skinned)
{
    DecalShade o;
    o.albedo = 0.0;
    o.roughness = 0.0;
    o.normal = 0.0;
    o.cavity = 0.0;
    o.coverage = 0.0;
    const uint count = view.decals.x;
    if (count == 0)
        return o;
    const uint words = view.decals.y;
    const uint2 tile = pixel / view.decals.w;
    for (uint w = 0; w < words; ++w)
    {
        uint bits = decalBins.Load(int3(int(tile.x * words + w), int(tile.y), 0));
        while (bits != 0)
        {
            const uint i = w * 32 + firstbitlow(bits);
            bits &= bits - 1;
            if (i >= count)
                break;
            const float4 r0 = decalRow(i, 0), r1 = decalRow(i, 1), r2 = decalRow(i, 2);
            const float3 local = float3(dot(r0.xyz, wpos) + r0.w, dot(r1.xyz, wpos) + r1.w, dot(r2.xyz, wpos) + r2.w);
            if (any(abs(local) > 0.5))
                continue;
            const float4 ax = decalRow(i, 3), ay = decalRow(i, 4), az = decalRow(i, 5);
            const float4 tint = decalRow(i, 6), fades = decalRow(i, 7), info = decalRow(i, 8);
            const uint flags = uint(info.y);
            if (skinned && (flags & 1u) == 0u)
                continue;  // (characters: only decals that allow them)
            if (info.x < 0.0)
                continue;
            // Facing: full while the surface faces it within the angle, gone edge-on.
            const float facing = dot(geoN, ay.xyz);
            const float full = cos((1.0 - saturate(fades.x)) * 1.5707963);
            const float fade = saturate(facing / max(full, 1e-3)) * saturate((0.5 - abs(local.y)) / max(fades.y * 0.5, 1e-3));
            if (fade <= 0.0)
                continue;
            const MaterialGpu m = materialData.Load<MaterialGpu>(uint(info.x) * kMaterialStride);
            float2 uv = float2(local.x + 0.5, local.z + 0.5);
            // UV footprints of the neighbouring pixels (on the same triangle's plane).
            const float2 uvX = float2(dot(r0.xyz, wposX) + r0.w, dot(r2.xyz, wposX) + r2.w) + 0.5;
            const float2 uvY = float2(dot(r0.xyz, wposY) + r0.w, dot(r2.xyz, wposY) + r2.w) + 0.5;
            // Parallax: the view ray, in the decal's plane (x, z) and up (y), stepped down through
            // its height map, `parallaxDepth` metres at its deepest.
            float height = 1.0;
            if ((flags & 2u) != 0u && fades.w > 0.0 && m.vtPageTable != 0xffffffffu)
            {
                const float3 toEye = -dir;
                const float3 v = float3(dot(toEye, ax.xyz), dot(toEye, az.xyz), max(dot(toEye, ay.xyz), 0.08));
                const float2 maxShift = -(v.xy / v.z) * fades.w / float2(ax.w, az.w);
                const float steps = lerp(info.z * 2.0, info.z, saturate(v.z));
                const uint mip = min(uint(vtMipLevel(m, uvX - uv, uvY - uv, view.vtParamsF.x)), vtMipCount(m) - 1);
                const float layer = 1.0 / steps;
                float depth = 0.0, prevDepth = 0.0;
                float2 cur = uv, prev = uv;
                float h = decalHeight(m, cur, mip), prevH = h;
                for (uint k = 0; k < 64 && float(k) < steps && depth < 1.0 - h; ++k)
                {
                    prev = cur;
                    prevDepth = depth;
                    prevH = h;
                    cur += maxShift * layer;
                    depth += layer;
                    h = decalHeight(m, cur, mip);
                }
                // Between the last two steps, where the ray crosses the height.
                const float after = (1.0 - h) - depth, before = (1.0 - prevH) - prevDepth;
                const float t = abs(after - before) > 1e-5 ? saturate(before / (before - after)) : 1.0;
                uv = lerp(prev, cur, t);
                if (any(uv < 0.0) || any(uv > 1.0))
                    continue;  // (stepped off its edge)
            }
            const Surface ds = sampleSurface(m, uv, uvX - uv, uvY - uv, pixel);
            height = ds.height;
            const float a = saturate(ds.albedo.a * m.baseColor.a * tint.a) * fade;
            if (a <= 0.0)
                continue;
            // Its normal map, in its axes, then turned onto the surface (the decal's up becomes the
            // surface's normal).
            const float2 nxy = (ds.normalXY * 2.0 - 1.0) * m.normalScale;
            const float3 nts = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
            float3 t = ax.xyz - geoN * dot(geoN, ax.xyz);
            t = dot(t, t) > 1e-8 ? normalize(t) : (abs(geoN.x) < 0.9 ? normalize(cross(geoN, float3(1, 0, 0))) : normalize(cross(geoN, float3(0, 1, 0))));
            const float3 b = cross(t, geoN) * -1.0;
            const float3 dn = normalize(t * nts.x + b * nts.y + geoN * nts.z);
            const float3 color = ds.albedo.rgb * m.baseColor.rgb * tint.rgb;
            const float rough = clamp(materialRoughness(m, ds.orm.g), 0.045, 1.0);
            const float darken = lerp(1.0, height, saturate(fades.z));
            o.albedo = o.albedo * (1.0 - a) + color * darken * a;
            o.roughness = o.roughness * (1.0 - a) + rough * a;
            o.normal = o.normal * (1.0 - a) + dn * a;
            o.cavity = o.cavity * (1.0 - a) + darken * a;
            o.coverage = o.coverage * (1.0 - a) + a;
        }
    }
    return o;
}

// The virtual shadow map, for the shadow lookups (shadows.slang).
struct ResolveShadows : IShadowMaps
{
    VsmParams vsm() { return view.vsm; }
    float3 sunDirection() { return view.sunDirection.xyz; }
    uint resident(uint entry) { return residentPage(view.vsm, vsmState, entry); }
    float compare(float2 uv, float ref) { return vsmAtlas.SampleCmpLevelZero(uv, ref); }
};

struct ResolveProbes : IProbeTextures
{
    float3 irradiance(float2 uv) { return probeIrradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return probeDistanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

// Upsample of a reduced-resolution ambient term (the AO; texel i shaded full-resolution pixel
// i * scale + offset). The 2x2 nearest texels, weighted bilinearly and by how close the point
// each one shaded lies to this pixel's surface plane, so nothing bleeds across depth edges.
// When none fits (thin geometry between samples) the best-fitting texel wins alone.
struct AmbientTaps
{
    float weight[4];
};

AmbientTaps ambientTaps(float4 depths, uint2 texel[4], uint scale, float2 f, float3 wpos, float3 n, float z)
{
    AmbientTaps t;
    const uint2 size = view.params.xy;
    const float bil[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
    float best = 1e30, sum = 0.0;
    uint bestTap = 0;
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        const uint2 q = min(texel[k] * scale + view.ambient.zw, size - 1);
        const float3 d = rayDir(float2(q) + 0.5);
        const float3 sp = view.cameraPos.xyz + d * (depths[k] / dot(d, view.cameraForward.xyz));
        const float rel = abs(dot(sp - wpos, n)) / (0.01 * z + 0.002);
        const float w = depths[k] >= 65504.0 ? 0.0 : max(bil[k], 0.01) * exp2(-rel * rel);
        t.weight[k] = w;
        sum += w;
        if (rel < best)
        {
            best = rel;
            bestTap = k;
        }
    }
    [unroll]
    for (uint k = 0; k < 4; ++k)
        t.weight[k] = sum > 1e-3 ? t.weight[k] / sum : (k == bestTap ? 1.0 : 0.0);
    return t;
}

void ambientTexels(uint2 pixel, uint scale, out uint2 texel[4], out float2 f)
{
    const uint2 reduced = (view.params.xy + scale - 1) / scale;
    const float2 x = (float2(pixel) - float2(view.ambient.zw)) / float(scale);
    const float2 b = floor(x);
    f = x - b;
    const int2 lo = int2(b), hi = int2(reduced) - 1;
    texel[0] = uint2(clamp(lo, int2(0, 0), hi));
    texel[1] = uint2(clamp(lo + int2(1, 0), int2(0, 0), hi));
    texel[2] = uint2(clamp(lo + int2(0, 1), int2(0, 0), hi));
    texel[3] = uint2(clamp(lo + int2(1, 1), int2(0, 0), hi));
}

float upsampleAo(uint2 pixel, float3 wpos, float3 n, float z)
{
    const uint scale = view.ambient.x;
    uint2 texel[4];
    float2 f;
    ambientTexels(pixel, scale, texel, f);
    const uint2 reduced = (view.params.xy + scale - 1) / scale;
    float2 s[4];
    [unroll]
    for (uint k = 0; k < 4; ++k)
        s[k] = aoTexture.SampleLevel((float2(texel[k]) + 0.5) / float2(reduced), 0.0);
    const AmbientTaps t = ambientTaps(float4(s[0].y, s[1].y, s[2].y, s[3].y), texel, scale, f, wpos, n, z);
    return s[0].x * t.weight[0] + s[1].x * t.weight[1] + s[2].x * t.weight[2] + s[3].x * t.weight[3];
}

float3 acesFilm(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_resolve(uint3 tid : SV_DispatchThreadID)
{
    const uint2 size = view.params.xy;
    if (tid.x >= size.x || tid.y >= size.y)
        return;
    const float2 pixel = float2(tid.xy) + 0.5;
    const float3 dir = rayDir(pixel);
    reflectWeight[tid.xy] = float4(0.0, 0.0, 0.0, 2.0);  // (the lit path below writes its own)

    const uint packed = visBuffer.Load(int3(tid.xy, 0));
    if (packed == 0)
    {
        // Lit view: linear HDR sky radiance for the post chain; debug views: display colour.
        output[tid.xy] = float4(view.params.z == 0 ? skyRadiance(dir) : skyGradient(dir), 1.0);
        velocity[tid.xy] = skyMotion(pixel, dir);
        return;
    }
    const uint visibleIndex = (packed - 1) >> 7;
    const uint triangle = (packed - 1) & 127;
    const uint2 item = visible.Load2(visibleIndex * 8);
    const uint instanceIndex = item.x & 0x7fffffffu;
    const InstanceGpu inst = loadInstance(scene, instanceIndex);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const uint tri = loadClusterTriangle(scene, cl, triangle);

    DecodedVertex v[3];
    float3 p[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const uint local = (tri >> (8 * k)) & 0xff;
        v[k] = loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, local), false, true);
        p[k] = transformPoint(inst, v[k].position);
    }
    const float3 e1 = p[1] - p[0], e2 = p[2] - p[0];
    const float3 origin = view.cameraPos.xyz;
    const float3 bary = barycentrics(p[0], e1, e2, origin, dir);
    const float3 baryX = barycentrics(p[0], e1, e2, origin, rayDir(pixel + float2(1.0, 0.0)));
    const float3 baryY = barycentrics(p[0], e1, e2, origin, rayDir(pixel + float2(0.0, 1.0)));
    const float2 uv = v[0].uv * bary.x + v[1].uv * bary.y + v[2].uv * bary.z;
    const float2 uvX = v[0].uv * baryX.x + v[1].uv * baryX.y + v[2].uv * baryX.z;
    const float2 uvY = v[0].uv * baryY.x + v[1].uv * baryY.y + v[2].uv * baryY.z;

    // Motion vector: the same mesh-space point under last frame's instance transform and camera.
    {
        float3 objPos = v[0].position * bary.x + v[1].position * bary.y + v[2].position * bary.z;
        if (isSkinned(inst))  // the same surface point in last frame's deformed mesh
        {
            objPos = 0.0;
            [unroll]
            for (uint k = 0; k < 3; ++k)
            {
                const uint local = (tri >> (8 * k)) & 0xff;
                objPos += loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, local), true, true).position *
                          bary[k];
            }
        }
        const InstanceGpu prevInst = prevInstances.Load<InstanceGpu>(instanceIndex * kInstanceStride);
        const float4 prevClip = mul(view.prevViewProj, float4(transformPoint(prevInst, objPos), 1.0));
        const float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-6);
        const float2 prevPixel = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * float2(size);
        // Behind the camera last frame: a vector far off screen (no history; finite, for FSR).
        velocity[tid.xy] = prevClip.w > 0.0 ? clamp(prevPixel - unjittered(pixel), -16384.0, 16384.0) : float2(-16384.0, -16384.0);
    }

    // Back face (what the rasterizer's SV_IsFrontFace would report): decided by the triangle's
    // winding, not the smoothed normal. Mirrored instances flip winding.
    const float mirror = determinant(float3x3(inst.row0.xyz, inst.row1.xyz, inst.row2.xyz)) < 0.0 ? -1.0 : 1.0;
    const bool backFace = dot(cross(e1, e2), dir) * mirror > 0.0;
    float3 n = transformDirection(inst, v[0].normal * bary.x + v[1].normal * bary.y + v[2].normal * bary.z);
    // Degenerate vertex normals (opposing normals interpolating to zero): fall back to the face.
    n = dot(n, n) > 1e-12 ? normalize(n) : normalize(cross(e1, e2)) * mirror;
    if (backFace)
        n = -n;

    const uint materialIndex = instanceMaterial(inst, cl);
    MaterialGpu m = materialData.Load<MaterialGpu>(materialIndex * kMaterialStride);
    // (parallax: the surface, and its layers, sampled where the view ray meets its height map)
    const float2 shift = parallaxUv(m, uv, uvX - uv, uvY - uv, e1, e2, v[1].uv - v[0].uv, v[2].uv - v[0].uv, dir) - uv;
    Surface s = sampleSurface(m, uv + shift, uvX - uv, uvY - uv, tid.xy);
    const float reliefSun =
        parallaxShadow(m, uv + shift, uvX - uv, uvY - uv, e1, e2, v[1].uv - v[0].uv, v[2].uv - v[0].uv, dir, view.sunDirection.xyz);
    LayeredLook layered;
    layered.tint = 1.0;
    layered.occlusion = 1.0;
    if ((inst.flags & kInstanceLayered) != 0 && view.terrain.z != 0)
        layered = blendLayers(inst, cl, tri, bary, baryX, baryY, uv + shift, uvX + shift, uvY + shift, tid.xy, s, m);
    // The terrain: its surface pass's material and per-pixel normal (or, without it, the
    // default look from where the pixel is). Its patches' tint is GI's colour, not a multiplier.
    const bool terrain = (m.flags & kMaterialTerrain) != 0;
    bool terrainShaded = false;
    float3 terrainN = n;
    // Terrain-shaded instances (generated rocks) too: the layer's look replaces their material's.
    if ((terrain || (inst.flags & kInstanceTerrainShaded) != 0) && view.terrain.x != 0)
    {
        const uint4 ts = terrainSurface.Load(int3(tid.xy, 0));
        if (ts.w != 0)
        {
            if (!terrain)
            {
                m.baseColor = float4(1.0, 1.0, 1.0, 1.0);
                m.roughness = 1.0;
                m.vtSize &= 0x00ffffffu;  // (the layer's roughness, as it is)
                m.metallic = 0.0;
                m.occlusionStrength = 1.0;
            }
            const float3 a = float3(ts.x & 0xff, (ts.x >> 8) & 0xff, (ts.x >> 16) & 0xff) / 255.0;
            s.albedo.rgb = a * a;
            s.orm = float3(float(ts.z & 0xff) / 255.0, float(ts.x >> 24) / 255.0, 0.0);
            terrainN = octDecode(ts.y);
            terrainShaded = true;
        }
    }
    if (terrain && !terrainShaded)
    {
        const float3 hit = origin + dir * dot(p[0] - origin, cross(e1, e2)) / dot(dir, cross(e1, e2));
        const TerrainLook look = terrainLook(hit, n);
        s.albedo.rgb = look.albedo;
        s.orm.g = look.roughness;
    }
    const float3 tint = terrain ? float3(1.0, 1.0, 1.0) : instanceTint(inst) * layered.tint;

    // Tangent-space normal map.
    const float4 tan4 = v[0].tangent * bary.x + v[1].tangent * bary.y + v[2].tangent * bary.z;
    float3 t = transformDirection(inst, tan4.xyz);
    t -= n * dot(n, t);
    // Missing (cooker default +X) or degenerate tangents can be parallel to n: any orthogonal
    // direction is as good as none, and avoids a NaN frame.
    if (dot(t, t) < 1e-12)
        t = abs(n.x) < 0.9 ? cross(n, float3(1, 0, 0)) : cross(n, float3(0, 1, 0));
    t = normalize(t);
    const float3 bt = cross(n, t) * (v[0].tangent.w * mirror) * (backFace ? -1.0 : 1.0);
    const float2 nxy = (s.normalXY * 2.0 - 1.0) * m.normalScale;
    const float3 nts = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
    float3 shadingN = terrainShaded ? (backFace ? -terrainN : terrainN) : normalize(t * nts.x + bt * nts.y + n * nts.z);

    // Decals over it (their albedo, roughness and normal over the surface's).
    const float3 hitPos = origin + dir * dot(p[0] - origin, cross(e1, e2)) / dot(dir, cross(e1, e2));
    const float3 hitX = p[0] + e1 * baryX.y + e2 * baryX.z, hitY = p[0] + e1 * baryY.y + e2 * baryY.z;
    const DecalShade decal = shadeDecals(tid.xy, hitPos, hitX, hitY, n, dir, isSkinned(inst));
    if (decal.coverage > 0.0)
        shadingN = normalize(shadingN * (1.0 - decal.coverage) + decal.normal);

    float3 color;
    switch (view.params.z)
    {
    case 1: color = hashColor(item.y) * (0.35 + 0.65 * saturate(n.y * 0.5 + 0.5)); break;
    case 2: color = shadingN * 0.5 + 0.5; break;
    case 3: color = hashColor(instanceIndex * 7919u); break;
    case 4: color = hashColor(item.y * 131u + triangle); break;
    case 5: color = lodColor(cl.depth) * (0.4 + 0.6 * saturate(n.y * 0.5 + 0.5)); break;
    case 6: color = ((item.x & kVisibleSecondPassBit) != 0 ? float3(1.0, 0.45, 0.1) : float3(0.25, 0.8, 0.35)) *
                    (0.4 + 0.6 * saturate(n.y * 0.5 + 0.5)); break;
    case 8: color = lodColor(s.mipUsed) * (s.mipUsed > s.mipWanted ? 0.35 : 1.0); break;  // dim = still streaming
    case 9: color = pow(s.albedo.rgb * m.baseColor.rgb * tint, 1.0 / 2.2); break;
    default:
    {
        float3 albedo = s.albedo.rgb * m.baseColor.rgb * tint * (1.0 - decal.coverage) + decal.albedo;
        float roughness = clamp(materialRoughness(m, s.orm.g), 0.045, 1.0) * (1.0 - decal.coverage) + decal.roughness;
        float metallic = saturate(s.orm.b * m.metallic) * (1.0 - decal.coverage);
        float ao = lerp(1.0, s.orm.r, m.occlusionStrength) * layered.occlusion;
        const float3 wpos = origin + dir * dot(p[0] - origin, cross(e1, e2)) / dot(dir, cross(e1, e2));
        const float3 vdir = -dir;
        float3 emissive = m.emissive.rgb * m.emissive.w;
        // (R4) A project's surface shader (its id in the material's flags): the surface after the
        // material's own, and (below) the light after the engine's.
        const uint surfaceShader = (m.flags >> 8) & 0xffu;
        float4 surfaceParams = float4(0.0, 0.0, 0.0, 0.0);
        if (surfaceShader != 0)
        {
            surfaceParams = asfloat(materialData.Load4(view.vtParams.w + materialIndex * 32));
            SurfaceIn si;
            si.position = wpos;
            si.normal = n;
            si.view = vdir;
            si.uv = uv;
            si.params = surfaceParams;
            si.baseColor = m.baseColor;
            si.time = view.vtParamsF.y;
            si.screenUv = (float2(tid.xy) + 0.5) / float2(view.params.xy);
            si.transparent = false;
            SurfaceOut so;
            so.alpha = 1.0;
            so.refraction = 0.0;
            so.refractionBlur = 0.0;
            so.albedo = albedo;
            so.roughness = roughness;
            so.metallic = metallic;
            so.ao = ao;
            so.emissive = emissive;
            so.normal = shadingN;
            projectSurface(surfaceShader, si, so);
            albedo = so.albedo;
            roughness = clamp(so.roughness, 0.045, 1.0);
            metallic = saturate(so.metallic);
            ao = saturate(so.ao);
            emissive = so.emissive;
            shadingN = dot(so.normal, so.normal) > 1e-12 ? normalize(so.normal) : shadingN;
        }
        uint shadowLevel = 0xffffffffu;
        // Foliage (alpha-masked and double-sided: leaf and grass cards) is thin: lit from either
        // side. The card's sun side takes the shadow test (its normal offset moves toward the
        // sun, not into the card's own shadow), and sun behind the card shines through it
        // (Blender's Translucent BSDF). Otherwise a card seen from its back got no sun at all.
        const float3 sunL = view.sunDirection.xyz;
        const bool foliage = (m.flags & 5u) == 5u;  // kMaterialAlphaMask | kMaterialDoubleSided
        const bool sunThrough = foliage && dot(n, sunL) < 0.0;
        const float3 sunN = sunThrough ? -n : n;
        const ResolveShadows sm = {};
        const float shadow =
            (dot(sunN, sunL) > 0.0 ? sunShadow(sm, wpos, sunN, length(wpos - origin), foliage ? kFoliageLightBias : 0.0, shadowLevel) : 0.0) *
            reliefSun;
        if (view.params.z == 11)
        {
            color = (shadowLevel == 0xffffffffu ? float3(0.3, 0.0, 0.3) : lodColor(shadowLevel % 8)) * (0.25 + 0.75 * shadow);
            break;
        }
        float3 hdr = (sunThrough ? albedo * (kFoliageTransmission * 0.31830989) * saturate(dot(-shadingN, sunL))  // (1 / pi)
                                 : brdf(albedo, metallic, roughness, shadingN, vdir, sunL)) *
                     view.sunColor.rgb * shadow;
        uint visited;
        const float viewDepth = dot(wpos - origin, view.cameraForward.xyz);
        hdr += shadeLights(lights, lightGrid, view.lightGrid, tid.xy, viewDepth, wpos, albedo, metallic, roughness,
                           shadingN, vdir, visited);
        shadeShadowedLights(sm, lights, wpos, n, shadingN, vdir, albedo, metallic, roughness, length(wpos - origin), hdr, visited);
        // Ambient: the probe GI (bounced light and the sky, per pixel with the normal-mapped
        // normal) or, with GI off, a flat sky term; occluded by GTAO/SSAO and the material AO
        // either way. (At half resolution the AO is upsampled against the geometric normal's plane.)
        const float ssao = upsampleAo(tid.xy, wpos, n, viewDepth);
        float3 ambient;
        if (view.ambient.y != 0)
        {
            const ResolveProbes probes;
            ambient = probeIrradiance(view.probes, probes, wpos, n, shadingN, vdir);
            if (view.params.z == 12)
            {
                color = ambient;  // debug: the indirect light alone
                break;
            }
        }
        else
        {
            ambient = flatAmbient(shadingN);
        }
        // Specular: the split-sum BRDF times what the reflection direction sees. The reflection
        // pass traces that (screen space, the BVH, the sky); without it, the ambient.
        const float3 specular = envSpecular(lerp(0.04, albedo, metallic), roughness, saturate(dot(shadingN, vdir))) * ao * ssao;
        hdr += albedo * (1.0 - metallic) * ambient * ao * ssao;
        if (view.reflect.x != 0.0)
        {
            reflectWeight[tid.xy] = float4(specular, roughness);
            reflectNormal[tid.xy] = float4(octEncode(shadingN), octEncode(n));
        }
        else
        {
            hdr += specular * ambient;
        }
        if (surfaceShader != 0)
        {
            LightIn li;
            li.albedo = albedo;
            li.normal = shadingN;
            li.view = vdir;
            li.sunDirection = sunL;
            li.sunColor = view.sunColor.rgb;
            li.sunDot = dot(shadingN, sunL);
            li.shadow = shadow;
            li.ambient = ambient;
            li.params = surfaceParams;
            li.time = view.vtParamsF.y;
            projectLight(surfaceShader, li, hdr);
        }
        hdr += emissive;
        if (view.params.z == 10)
        {
            const float t = saturate(float(visited) / 32.0);  // light count heatmap: 0 .. 32+
            color = lerp(lerp(float3(0.05, 0.05, 0.2), float3(0.1, 0.8, 0.3), saturate(t * 2.0)), float3(1.0, 0.2, 0.1),
                         saturate(t * 2.0 - 1.0));
            break;
        }
        color = hdr;  // linear HDR: exposure, bloom and AgX happen in the post chain
        break;
    }
    }
    output[tid.xy] = float4(view.params.z == 0 ? color : saturate(color), 1.0);
}
