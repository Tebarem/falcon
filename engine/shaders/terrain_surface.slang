// Terrain surface (before the resolve): for every pixel the terrain covers, its material and
// normal, packed for the resolve to light (visbuffer_resolve's terrain branch).
//
//   position  from depth along the pixel's ray (jittered as the raster was)
//   normal    from the heightmap at the pixel (central differences over about a pixel's
//             footprint): full detail where the tile is in the atlas, the coarse map elsewhere
//   layers    the material map's two layers per sample, bilinear over the 4 samples around the
//             pixel, the three strongest kept, then height-blended against each other with
//             each layer's own texture height (rocks poke through sand instead of fading)
//   per layer hex tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling"): each cell of a hex
//             grid samples the texture at its own random rotation and offset, three cells blended
//             with weights sharpened toward the brighter (taller) texel, so the texture never
//             visibly repeats; a second sample at farScale x the size blended in with distance;
//             world-space noise varies brightness and hue at macroScale; steep ground takes a
//             biplanar projection (from the sides) so cliffs don't stretch
// Without layers: the default look (terrain_shading.slang) with the heightmap's normal.
//
// Terrain-shaded instances (kInstanceTerrainShaded: generated rocks): their own layer, projected
// from three sides, on the mesh's smooth normal (from the visibility buffer's triangle); near
// the ground the ground's own strongest layer is blended onto their up-facing parts, so rocks
// sit in the ground instead of on it.
//
// Output (RGBA32_UINT): x: sqrt(albedo) rgb8 + roughness8, y: octahedral normal snorm16x2,
// z: ao8, w: 1 = terrain written here.

import scene_types;
import terrain_shading;

struct SurfaceUniforms
{
    float4 cameraPos;    // xyz, w: zNear
    float4 cameraRight;  // xyz, w: 1 / P00
    float4 cameraUp;     // xyz, w: 1 / P11
    float4 cameraForward;
    float4 jitter;       // xy: NDC jitter of this frame's raster
    uint4 screen;        // width, height, frame, layer count
    float4 grid;         // origin x, origin z, spacing, 1 / spacing
    float4 heights;      // height min, height range (max - min), coarse side, tiles per side
    uint4 slots;         // first terrain instance, slot count, page table byte offset, mip bias (asfloat)
};

[[vk::binding(0, 0)]] Sampler2D<float> sceneDepth : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float> coarseHeight : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2DArray<float> tileHeight : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float4> coarseMaterial : register(t3, space0) : register(s3, space0);
[[vk::binding(4, 0)]] Sampler2DArray<float4> tileMaterial : register(t4, space0) : register(s4, space0);
[[vk::binding(5, 0)]] Sampler2DArray<float4> layerAlbedo : register(t5, space0) : register(s5, space0);
[[vk::binding(6, 0)]] Sampler2DArray<float4> layerNormal : register(t6, space0) : register(s6, space0);
[[vk::binding(7, 0)]] Sampler2DArray<float4> layerOrm : register(t7, space0) : register(s7, space0);
[[vk::binding(8, 0)]] Texture2D<uint> visBuffer : register(t8, space0);
[[vk::binding(9, 0)]] ByteAddressBuffer visible : register(t9, space0);
[[vk::binding(10, 0)]] ByteAddressBuffer terrainData : register(t10, space0);  // layers, then the tile page table
[[vk::binding(11, 0)]] ByteAddressBuffer scene : register(t11, space0);        // terrain-shaded instances' triangles
[[vk::binding(0, 1)]] [format("rgba32ui")] RWTexture2D<uint4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<SurfaceUniforms> su : register(b0, space2);

static const uint kTileQuadsS = 256;
static const float kTileSamplesS = 257.0;
static const uint kLayerStride = 80;
static const uint kLayersOffset = 16;

struct LayerParams
{
    float4 a;  // 1 / tile size, rotation (share of a turn), contrast, height blend
    float4 b;  // macro, 1 / macro scale, projection, normal strength
    float4 c;  // tint rgb, roughness multiplier
    float4 d;  // far scale, hex (0 / 1), loaded (0 / 1), unused
    float4 mean;
};

LayerParams loadLayer(uint i) { return terrainData.Load<LayerParams>(kLayersOffset + i * kLayerStride); }

// Atlas layer of a tile (-1: not resident on the GPU).
int atlasLayer(uint tx, uint tz)
{
    const uint tiles = uint(su.heights.w);
    return int(terrainData.Load(su.slots.z + (min(tz, tiles - 1) * tiles + min(tx, tiles - 1)) * 4)) - 1;
}

// World height at grid position g (samples); `coarse`: from the coarse map only.
float heightAt(float2 g, bool coarse)
{
    const float last = su.heights.w * float(kTileQuadsS);
    g = clamp(g, 0.0, last);
    if (!coarse)
    {
        const uint2 t = min(uint2(g / float(kTileQuadsS)), uint2(uint(su.heights.w) - 1));
        const int layer = atlasLayer(t.x, t.y);
        if (layer >= 0)
        {
            const float2 uv = (g - float2(t) * float(kTileQuadsS) + 0.5) / kTileSamplesS;
            return su.heights.x + tileHeight.SampleLevel(float3(uv, float(layer)), 0.0) * su.heights.y;
        }
    }
    const float2 uv = (g / 8.0 + 0.5) / su.heights.z;
    return su.heights.x + coarseHeight.SampleLevel(uv, 0.0) * su.heights.y;
}

// The packed material of the sample at integer grid position s.
uint materialAt(int2 s)
{
    const int last = int(su.heights.w) * int(kTileQuadsS);
    s = clamp(s, int2(0, 0), int2(last, last));
    const uint2 t = min(uint2(s) / kTileQuadsS, uint2(uint(su.heights.w) - 1));
    const int layer = atlasLayer(t.x, t.y);
    float4 m;
    if (layer >= 0)
        m = tileMaterial.SampleLevel(float3((float2(s - int2(t) * int(kTileQuadsS)) + 0.5) / kTileSamplesS, float(layer)), 0.0);
    else
        m = coarseMaterial.SampleLevel((float2((s + 4) / 8) + 0.5) / su.heights.z, 0.0);
    const uint4 b = uint4(round(m * 255.0));
    return b.x | b.y << 8 | b.z << 16 | b.w << 24;
}

float hash12(float2 p)
{
    uint2 q = uint2(int2(p) + 65536);
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return float(h & 0xffffffu) / 16777216.0;
}

float noise2(float2 p)
{
    const float2 i = floor(p), f = p - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), u.x), lerp(hash12(i + float2(0, 1)), hash12(i + float2(1, 1)), u.x), u.y);
}

struct Sample
{
    float3 albedo;
    float height;
    float2 normal;  // tangent space xy
    float rough, ao;
};

Sample fetch(uint layer, float2 uv, float2 dx, float2 dy)
{
    Sample s;
    const float4 a = layerAlbedo.SampleGrad(float3(uv, float(layer)), dx, dy);
    const float2 n = layerNormal.SampleGrad(float3(uv, float(layer)), dx, dy).xy * 2.0 - 1.0;
    const float4 o = layerOrm.SampleGrad(float3(uv, float(layer)), dx, dy);
    s.albedo = a.rgb;
    s.height = a.a;
    s.normal = n;
    s.ao = o.r;
    s.rough = o.g;
    return s;
}

float2 rotate(float2 v, float c, float s) { return float2(c * v.x - s * v.y, s * v.x + c * v.y); }

// Hex tiling: three cells of a hex grid around uv, each its own rotation and offset.
Sample hexSample(uint layer, LayerParams p, float2 uv, float2 dx, float2 dy)
{
    if (p.d.y < 0.5)
        return fetch(layer, uv, dx, dy);
    // Triangle grid (skewed), its three vertices and barycentric weights.
    const float2 st = uv * 3.46410162;  // 2 sqrt(3): cells about one texture repeat across
    const float2 skewed = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    const float2 base = floor(skewed);
    float3 t = float3(frac(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    const float s = t.z < 0.0 ? 1.0 : 0.0, s2 = 2.0 * s - 1.0;
    const float3 w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    const float2 v[3] = { base + float2(s, s), base + float2(s, 1.0 - s), base + float2(1.0 - s, s) };
    Sample r[3];
    float3 lum;
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const float h = hash12(v[k] * 1.731 + float(layer) * 17.0);
        const float angle = (h - 0.5) * 6.28318531 * p.a.y;
        const float c = cos(angle), sn = sin(angle);
        // The vertex's centre back in uv: rotate around it, then a random offset.
        const float2 centre = float2(v[k].x + 0.5 * v[k].y, 0.8660254 * v[k].y) / 3.46410162;
        const float2 offset = float2(hash12(v[k] + 91.7), hash12(v[k] + 47.3));
        const float2 uvk = rotate(uv - centre, c, sn) + centre + offset;
        r[k] = fetch(layer, uvk, rotate(dx, c, sn), rotate(dy, c, sn));
        r[k].normal = rotate(r[k].normal, c, -sn);  // back into the uv frame
        lum[k] = dot(r[k].albedo, float3(0.299, 0.587, 0.114)) * 0.5 + r[k].height * 0.5;
    }
    // Sharpened weights, favouring the brighter / taller texel where cells meet.
    float3 wk = pow(max(w, 0.0), 1.0 + 7.0 * p.a.z) * pow(lum + 0.05, 2.0 * p.a.z);
    wk /= max(wk.x + wk.y + wk.z, 1e-6);
    Sample o;
    o.albedo = r[0].albedo * wk.x + r[1].albedo * wk.y + r[2].albedo * wk.z;
    o.height = r[0].height * wk.x + r[1].height * wk.y + r[2].height * wk.z;
    o.normal = r[0].normal * wk.x + r[1].normal * wk.y + r[2].normal * wk.z;
    o.ao = r[0].ao * wk.x + r[1].ao * wk.y + r[2].ao * wk.z;
    o.rough = r[0].rough * wk.x + r[1].rough * wk.y + r[2].rough * wk.z;
    return o;
}

struct Shaded
{
    float3 albedo, normal;
    float height, rough, ao;
};

// One layer at a world point: one or three projections (top, and the sides on steep ground or
// when `sides`).
Shaded shadeLayer(uint layer, float3 wp, float3 n, float3 dpx, float3 dpy, float footprint, bool sides = false)
{
    const LayerParams p = loadLayer(layer);
    const float inv = p.a.x;
    float3 pw = float3(0, 1, 0);
    if (sides || p.b.z > 1.5 || (p.b.z > 0.5 && abs(n.y) < 0.85))
    {
        pw = pow(abs(n), 4.0);
        const float3 share = pw / (pw.x + pw.y + pw.z);
        pw = float3(share.x < 0.03 ? 0.0 : pw.x, share.y < 0.03 ? 0.0 : pw.y, share.z < 0.03 ? 0.0 : pw.z);
        pw /= max(pw.x + pw.y + pw.z, 1e-6);
    }
    // Far away, a larger sample of the same texture blends in (kills repetition at a distance).
    const float far = smoothstep(0.03, 0.25, footprint * inv);
    Shaded o;
    o.albedo = 0.0;
    o.height = 0.0;
    o.rough = 0.0;
    o.ao = 0.0;
    float3 nsum = 0.0;
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const float wk = k == 0 ? pw.y : (k == 1 ? pw.x : pw.z);
        if (wk <= 0.0)
            continue;
        // Plane coordinates: top xz, side x: zy, side z: xy.
        const float2 uv = (k == 0 ? wp.xz : (k == 1 ? wp.zy : wp.xy)) * inv;
        const float2 dx = (k == 0 ? dpx.xz : (k == 1 ? dpx.zy : dpx.xy)) * inv;
        const float2 dy = (k == 0 ? dpy.xz : (k == 1 ? dpy.zy : dpy.xy)) * inv;
        Sample s = hexSample(layer, p, uv, dx, dy);
        if (far > 0.0)
        {
            const Sample f = fetch(layer, uv / p.d.x + 0.37, dx / p.d.x, dy / p.d.x);
            s.albedo = lerp(s.albedo, f.albedo, far * 0.5);
            s.height = lerp(s.height, f.height, far * 0.5);
        }
        // Whiteout-style blend of the texture's normal onto the surface's, per plane.
        const float2 tn = s.normal * p.b.w;
        float3 wn;
        if (k == 0)
            wn = float3(tn.x + n.x, abs(n.y) * sqrt(saturate(1.0 - dot(tn, tn))), tn.y + n.z) * float3(1, sign(n.y), 1);
        else if (k == 1)
            wn = float3(abs(n.x) * sqrt(saturate(1.0 - dot(tn, tn))) * sign(n.x), tn.y + n.y, tn.x + n.z);
        else
            wn = float3(tn.x + n.x, tn.y + n.y, abs(n.z) * sqrt(saturate(1.0 - dot(tn, tn))) * sign(n.z));
        nsum += normalize(wn) * wk;
        o.albedo += s.albedo * wk;
        o.height += s.height * wk;
        o.rough += s.rough * wk;
        o.ao += s.ao * wk;
    }
    o.normal = normalize(nsum);
    // Large-scale variation: brightness and a little hue, from world-space noise.
    const float m = noise2(wp.xz * p.b.y) * 0.65 + noise2(wp.xz * p.b.y * 2.7 + 13.1) * 0.35;
    o.albedo *= lerp(1.0, 0.7 + 0.6 * m, p.b.x) * p.c.rgb;
    o.albedo = lerp(o.albedo, o.albedo * float3(1.06, 1.0, 0.9), p.b.x * (noise2(wp.xz * p.b.y * 0.6 + 71.0) - 0.5));
    o.rough = saturate(o.rough * p.c.w);
    return o;
}

float3 rayDirection(float2 pixel)
{
    const float2 size = float2(su.screen.xy);
    const float2 ndc = float2(pixel.x / size.x * 2.0 - 1.0, 1.0 - pixel.y / size.y * 2.0) - su.jitter.xy;
    return su.cameraForward.xyz + su.cameraRight.xyz * (ndc.x * su.cameraRight.w) + su.cameraUp.xyz * (ndc.y * su.cameraUp.w);
}

// Moller-Trumbore without bounds (also for the neighbouring pixels' rays, on the triangle's plane).
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

// A terrain-shaded instance's pixel: its layer on the mesh, the ground's blended onto its base.
void shadeInstance(uint2 tid, InstanceGpu inst, uint packed)
{
    const uint layer = inst.pad1 & 0xff;
    const uint layerCount = su.screen.w;
    if (layer >= layerCount || loadLayer(layer).d.z < 0.5)
        return;  // (no such layer, or its textures not built: the mesh's own material)
    const uint2 item = visible.Load2(((packed - 1) >> 7) * 8);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint tri = loadClusterTriangle(scene, cl, (packed - 1) & 127);
    DecodedVertex v[3];
    float3 p[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        v[k] = loadVertex(scene, mesh, loadClusterVertex(scene, cl, (tri >> (8 * k)) & 0xff));
        p[k] = transformPoint(inst, v[k].position);
    }
    const float2 pixel = float2(tid) + 0.5;
    const float3 dir = rayDirection(pixel), origin = su.cameraPos.xyz;
    const float3 e1 = p[1] - p[0], e2 = p[2] - p[0];
    const float3 b = barycentrics(p[0], e1, e2, origin, dir);
    const float3 wp = p[0] + e1 * b.y + e2 * b.z;
    // (Smooth normals near the outline may face a little away: kept, as the resolve's are.)
    const float3 n = normalize(transformDirection(inst, v[0].normal * b.x + v[1].normal * b.y + v[2].normal * b.z));
    const float3 bx = barycentrics(p[0], e1, e2, origin, rayDirection(pixel + float2(1, 0)));
    const float3 by = barycentrics(p[0], e1, e2, origin, rayDirection(pixel + float2(0, 1)));
    const float3 dpx = e1 * (bx.y - b.y) + e2 * (bx.z - b.z), dpy = e1 * (by.y - b.y) + e2 * (by.z - b.z);
    const float pixelWorld = max(length(dpx), length(dpy));
    Shaded s = shadeLayer(layer, wp, n, dpx, dpy, pixelWorld, true);

    // The ground near its base: the ground's strongest layer on its up-facing parts, fading over
    // the blend height (its edge broken by noise).
    const float blend = float((inst.pad1 >> 8) & 0xff) * 0.1;
    if (blend > 0.0)
    {
        const float2 g = (wp.xz - su.grid.xy) * su.grid.w;
        const float above = wp.y - heightAt(g, false);
        float t = 1.0 - smoothstep(0.0, blend, above + (noise2(wp.xz * 1.7) - 0.5) * blend * 0.6);
        t *= saturate(n.y * 1.2 + 0.6);  // (up-facing parts hold more of it)
        const uint m = materialAt(int2(round(g)));
        const uint ground = ((m >> 16) & 0xff) > 127 ? (m >> 8) & 0xff : m & 0xff;
        if (t > 0.0 && ground < layerCount && ground != layer && loadLayer(ground).d.z > 0.5)
        {
            const Shaded gs = shadeLayer(ground, wp, n, dpx, dpy, pixelWorld, true);
            // Height-blended like terrain layers: the ground fills the rock's hollows first.
            const float w = saturate(t * 1.5 - 0.25 + (gs.height - s.height) * 0.8);
            s.albedo = lerp(s.albedo, gs.albedo, w);
            s.normal = normalize(lerp(s.normal, gs.normal, w));
            s.rough = lerp(s.rough, gs.rough, w);
            s.ao = lerp(s.ao, gs.ao, w);
        }
    }
    writeSurface(tid, s.albedo, s.normal, s.rough, s.ao);
}

void writeSurface(uint2 pixel, float3 albedo, float3 normal, float rough, float ao)
{
    const uint3 a8 = uint3(round(sqrt(saturate(albedo)) * 255.0));
    output[pixel] = uint4(a8.x | a8.y << 8 | a8.z << 16 | uint(round(saturate(rough) * 255.0)) << 24, octPack(normal),
                          uint(round(saturate(ao) * 255.0)), 1);
}

uint octPack(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 sgn = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    const float2 e = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sgn;
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xffffu) | (uint(q.y) << 16);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_surface(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= su.screen.x || tid.y >= su.screen.y)
        return;
    const uint packed = visBuffer.Load(int3(tid.xy, 0));
    if (packed == 0)
        return;
    const uint instance = visible.Load(((packed - 1) >> 7) * 8) & 0x7fffffffu;
    if (instance < su.slots.x || instance >= su.slots.x + su.slots.y)
    {
        const InstanceGpu inst = loadInstance(scene, instance);
        if ((inst.flags & kInstanceTerrainShaded) != 0)
            shadeInstance(tid.xy, inst, packed);
        return;
    }

    // Position: the depth along this pixel's ray (whose forward component is 1: t = view depth).
    const float2 pixel = float2(tid.xy) + 0.5;
    const float depth = sceneDepth.SampleLevel(pixel / float2(su.screen.xy), 0.0);
    const float z = su.cameraPos.w / max(depth, 1e-9);
    const float3 dir = rayDirection(pixel);
    const float3 wp = su.cameraPos.xyz + dir * z;
    const float pixelWorld = z * 2.0 * su.cameraUp.w / float(su.screen.y);

    // Normal from the heightmap, over about a pixel (never finer than a sample).
    const float2 g = (wp.xz - su.grid.xy) * su.grid.w;
    const float stepSamples = max(1.0, pixelWorld * su.grid.w);
    const bool coarse = stepSamples >= 8.0;
    const float hl = heightAt(g - float2(stepSamples, 0), coarse), hr = heightAt(g + float2(stepSamples, 0), coarse);
    const float hd = heightAt(g - float2(0, stepSamples), coarse), hu = heightAt(g + float2(0, stepSamples), coarse);
    const float span = 2.0 * stepSamples * su.grid.z;
    const float3 n = normalize(float3(-(hr - hl) / span, 1.0, -(hu - hd) / span));

    float3 albedo, normal = n;
    float rough, ao = 1.0;
    const uint layerCount = su.screen.w;
    if (layerCount == 0)
    {
        const TerrainLook look = terrainLook(wp, n);
        albedo = look.albedo;
        rough = look.roughness;
    }
    else
    {
        // The neighbouring pixels' points on the surface's tangent plane: texture gradients.
        const float3 dirX = rayDirection(pixel + float2(1, 0)), dirY = rayDirection(pixel + float2(0, 1));
        const float3 dpx = su.cameraPos.xyz + dirX * (dot(wp - su.cameraPos.xyz, n) / dot(dirX, n)) - wp;
        const float3 dpy = su.cameraPos.xyz + dirY * (dot(wp - su.cameraPos.xyz, n) / dot(dirY, n)) - wp;

        // Layer weights: bilinear over the four samples around the point, two layers each.
        const int2 s0 = int2(floor(g));
        const float2 f = g - float2(s0);
        uint ids[8];
        float ws[8];
        uint count = 0;
        [unroll]
        for (uint c = 0; c < 4; ++c)
        {
            const int2 o = int2(c & 1, c >> 1);
            const uint m = materialAt(s0 + o);
            const float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
            const float wb = float((m >> 16) & 0xff) / 255.0;
            const uint pair[2] = { m & 0xff, (m >> 8) & 0xff };
            const float pw[2] = { bw * (1.0 - wb), bw * wb };
            [unroll]
            for (uint q = 0; q < 2; ++q)
            {
                if (pw[q] <= 0.0)
                    continue;
                uint at = count;
                for (uint e = 0; e < count; ++e)
                    if (ids[e] == pair[q])
                        at = e;
                if (at == count)
                {
                    ids[count] = pair[q];
                    ws[count] = 0.0;
                    ++count;
                }
                ws[at] += pw[q];
            }
        }
        // The three strongest.
        uint top[3] = { 0, 0, 0 };
        float tw[3] = { 0.0, 0.0, 0.0 };
        for (uint e = 0; e < count; ++e)
        {
            const float w = ws[e];
            if (w > tw[0]) { tw[2] = tw[1]; top[2] = top[1]; tw[1] = tw[0]; top[1] = top[0]; tw[0] = w; top[0] = ids[e]; }
            else if (w > tw[1]) { tw[2] = tw[1]; top[2] = top[1]; tw[1] = w; top[1] = ids[e]; }
            else if (w > tw[2]) { tw[2] = w; top[2] = ids[e]; }
        }
        // Shade them, then blend by height: a layer shows where its texture stands taller.
        Shaded sh[3];
        float hb[3];
        float best = -1e9;
        [unroll]
        for (uint k = 0; k < 3; ++k)
        {
            if (tw[k] <= 0.0 || top[k] >= layerCount || loadLayer(top[k]).d.z < 0.5)  // (textures not built yet)
            {
                hb[k] = -1e9;
                continue;
            }
            sh[k] = shadeLayer(top[k], wp, n, dpx, dpy, pixelWorld);
            hb[k] = sh[k].height + tw[k];
            best = max(best, hb[k]);
        }
        float wsum = 0.0;
        albedo = 0.0;
        normal = 0.0;
        rough = 0.0;
        ao = 0.0;
        [unroll]
        for (uint k = 0; k < 3; ++k)
        {
            if (hb[k] < -1e8)
                continue;
            const float blendWidth = loadLayer(top[k]).a.w;
            const float w = max(hb[k] - (best - blendWidth), 0.0) * tw[k];
            albedo += sh[k].albedo * w;
            normal += sh[k].normal * w;
            rough += sh[k].rough * w;
            ao += sh[k].ao * w;
            wsum += w;
        }
        if (wsum > 0.0)
        {
            albedo /= wsum;
            normal = normalize(normal);
            rough /= wsum;
            ao /= wsum;
        }
        else
        {
            const TerrainLook look = terrainLook(wp, n);
            albedo = look.albedo;
            rough = look.roughness;
            normal = n;
            ao = 1.0;
        }
    }
    writeSurface(tid.xy, albedo, normal, rough, ao);
}
