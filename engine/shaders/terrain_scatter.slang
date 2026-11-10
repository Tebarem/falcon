// Terrain scatter (render/scatter_renderer.h): one group per job, a cell of one asset of one
// layer. Mode 0 generates the cell, mode 1 clears it (hides its slots).
//
// Candidates: a k x k jittered grid over the cell, candidate i in the cell's slot i, so the
// result never depends on thread order. Each is kept with probability density-share x rules:
// slope and height ranges (soft edges), the material layers under it, a clumping noise, the
// painted mask, no holes, not deleted. A kept candidate stands on the terrain (heights from the
// tile atlas, or the coarse map further out), yawed, leaned toward the ground and tilted at
// random, scaled, sunk and darkened by its hash. The rows go into the scene's instance table
// (and last frame's copy: they don't move).

import scene_types;

struct ScatterUniforms
{
    float4 grid;     // origin x, origin z, spacing, 1 / spacing
    float4 heights;  // height min, height range, coarse side, tiles per side
    uint4 info;      // page table byte offset (in terrainData), jobs
};

struct SubLayer
{
    uint4 a;   // mesh, side k, seed, flags: 1 no shadows | paint mode << 8 | paint bit << 16
    float4 b;  // cell size, keep (density share per candidate), scale min, scale max
    float4 c;  // slope min, slope max (degrees), height min, height max
    float4 d;  // 1 / clump scale, clump amount, clump threshold, material min
    int4 m;    // material layers (-1: none)
    float4 e;  // align, tilt (radians), sink, tint variation
    float4 f;  // base offset (minus the mesh's lowest y), jitter (0 grid .. 1 random), yaw range (radians), unused
    float4 g;  // terrain shading: layer (-1: none), base blend (metres)
};

struct Job
{
    int2 cell;
    uint sub, slotBase, mode, exclusionOffset, exclusionCount, pad;  // mode 1 (clear): exclusionOffset = mesh, exclusionCount = slots
};

[[vk::binding(0, 0)]] Sampler2D<float> coarseHeight : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2DArray<float> tileHeight : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float4> coarseMaterial : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2DArray<float4> tileMaterial : register(t3, space0) : register(s3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer jobs : register(t4, space0);
[[vk::binding(5, 0)]] ByteAddressBuffer params : register(t5, space0);
[[vk::binding(6, 0)]] ByteAddressBuffer terrainData : register(t6, space0);
[[vk::binding(7, 0)]] ByteAddressBuffer exclusions : register(t7, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer sceneRW : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer prevInstances : register(u1, space1);
[[vk::binding(0, 2)]] ConstantBuffer<ScatterUniforms> su : register(b0, space2);

static const uint kScatterTileQuads = 256;
static const float kScatterTileSamples = 257.0;

int scatterAtlasLayer(uint tx, uint tz)
{
    const uint tiles = uint(su.heights.w);
    return int(terrainData.Load(su.info.x + (min(tz, tiles - 1) * tiles + min(tx, tiles - 1)) * 4)) - 1;
}

float scatterHeight(float2 g)
{
    g = clamp(g, 0.0, su.heights.w * float(kScatterTileQuads));
    const uint2 t = min(uint2(g / float(kScatterTileQuads)), uint2(uint(su.heights.w) - 1));
    const int layer = scatterAtlasLayer(t.x, t.y);
    if (layer >= 0)
        return su.heights.x + tileHeight.SampleLevel(float3((g - float2(t) * float(kScatterTileQuads) + 0.5) / kScatterTileSamples, float(layer)), 0.0) * su.heights.y;
    return su.heights.x + coarseHeight.SampleLevel((g / 8.0 + 0.5) / su.heights.z, 0.0) * su.heights.y;
}

uint scatterMaterial(int2 s)
{
    const int last = int(su.heights.w) * int(kScatterTileQuads);
    s = clamp(s, int2(0, 0), int2(last, last));
    const uint2 t = min(uint2(s) / kScatterTileQuads, uint2(uint(su.heights.w) - 1));
    const int layer = scatterAtlasLayer(t.x, t.y);
    float4 m;
    if (layer >= 0)
        m = tileMaterial.SampleLevel(float3((float2(s - int2(t) * int(kScatterTileQuads)) + 0.5) / kScatterTileSamples, float(layer)), 0.0);
    else
        m = coarseMaterial.SampleLevel((float2((s + 4) / 8) + 0.5) / su.heights.z, 0.0);
    const uint4 b = uint4(round(m * 255.0));
    return b.x | b.y << 8 | b.z << 16 | b.w << 24;
}

uint scatterHash(uint a, uint b, uint c, uint d)
{
    uint h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu ^ d * 0x165667b1u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
float unitOf(uint h) { return float(h & 0xffffffu) / 16777216.0; }

float valueNoise2(float2 p, uint seed)
{
    const float2 i = floor(p), f = p - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(i);
    const float a = unitOf(scatterHash(uint(c.x), uint(c.y), seed, 7u)), b = unitOf(scatterHash(uint(c.x + 1), uint(c.y), seed, 7u));
    const float d = unitOf(scatterHash(uint(c.x), uint(c.y + 1), seed, 7u)), e = unitOf(scatterHash(uint(c.x + 1), uint(c.y + 1), seed, 7u));
    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
}

// A range over a quantity that runs from `first` to `last`: an end at its limit is open (no fade).
float rangeMask(float x, float lo, float hi, float soft, float first, float last)
{
    return (lo > first ? saturate((x - lo) / soft + 0.5) : 1.0) * (hi < last ? saturate((hi - x) / soft + 0.5) : 1.0);
}

void storeRow(uint slot, float4 r0, float4 r1, float4 r2, uint mesh, uint flags, uint tint, uint extra = 0)
{
    const uint base = sceneRW.Load(kSecInstances * 4) + slot * kInstanceStride;
    const uint4 tail = uint4(mesh, flags, tint, extra);
    sceneRW.Store4(base, asuint(r0));
    sceneRW.Store4(base + 16, asuint(r1));
    sceneRW.Store4(base + 32, asuint(r2));
    sceneRW.Store4(base + 48, tail);
    const uint prev = slot * kInstanceStride;
    prevInstances.Store4(prev, asuint(r0));
    prevInstances.Store4(prev + 16, asuint(r1));
    prevInstances.Store4(prev + 32, asuint(r2));
    prevInstances.Store4(prev + 48, tail);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_scatter(uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    if (gid.x >= su.info.y)
        return;
    const Job job = jobs.Load<Job>(gid.x * 32);
    // Clearing: the job carries the mesh and the slot count (its layer may be gone from params).
    if (job.mode != 0)
    {
        for (uint i = lid.x; i < job.exclusionCount; i += 64)
            storeRow(job.slotBase + i, float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0), job.exclusionOffset, kInstanceHidden, 0);
        return;
    }
    const SubLayer p = params.Load<SubLayer>(job.sub * 128);
    const uint k = p.a.y, n = k * k, seed = p.a.z;
    const float cell = p.b.x;
    const float last = su.heights.w * float(kScatterTileQuads);
    for (uint i = lid.x; i < n; i += 64)
    {
        const uint slot = job.slotBase + i;
        const uint h0 = scatterHash(uint(job.cell.x), uint(job.cell.y), i, seed);
        const uint h1 = scatterHash(h0, 1u, seed, 3u), h2 = scatterHash(h0, 2u, seed, 5u), h3 = scatterHash(h0, 3u, seed, 9u);
        const float2 local = (float2(i % k, i / k) + 0.5 + (float2(unitOf(h0), unitOf(h1)) - 0.5) * p.f.y) / float(k);
        const float2 xz = (float2(job.cell) + local) * cell;
        const float2 g = (xz - su.grid.xy) * su.grid.w;
        float mask = (g.x >= 0.0 && g.y >= 0.0 && g.x <= last && g.y <= last) ? 1.0 : 0.0;
        // Excluded (deleted in the editor).
        for (uint e = 0; e < job.exclusionCount && mask > 0.0; ++e)
            if (exclusions.Load((job.exclusionOffset + e) * 4) == i)
                mask = 0.0;
        float3 nrm = float3(0, 1, 0);
        float y = 0.0;
        if (mask > 0.0)
        {
            y = scatterHeight(g);
            const float hl = scatterHeight(g - float2(1, 0)), hr = scatterHeight(g + float2(1, 0));
            const float hd = scatterHeight(g - float2(0, 1)), hu = scatterHeight(g + float2(0, 1));
            nrm = normalize(float3(-(hr - hl), 2.0 * su.grid.z, -(hu - hd)));
            const float slope = degrees(acos(saturate(nrm.y)));
            mask *= rangeMask(slope, p.c.x, p.c.y, 3.0, 0.0, 90.0) * rangeMask(y, p.c.z, p.c.w, 1.0, -1e4, 1e4);
            const uint m = scatterMaterial(int2(round(g)));
            if ((m & (1u << 24)) != 0)
                mask = 0.0;  // a hole
            const uint paintMode = (p.a.w >> 8) & 0xff, bit = (p.a.w >> 16) & 0xff;
            if (paintMode != 0)
            {
                const bool painted = (m & (1u << (25 + bit))) != 0;
                if ((paintMode == 1 && painted) || (paintMode == 2 && !painted))
                    mask = 0.0;
            }
            if (p.m.x >= 0)  // on these material layers only
            {
                const uint a = m & 0xff, b = (m >> 8) & 0xff;
                const float wb = float((m >> 16) & 0xff) / 255.0;
                float w = 0.0;
                [unroll]
                for (uint q = 0; q < 4; ++q)
                {
                    const int l = p.m[q];
                    if (l < 0)
                        continue;
                    w = max(w, (uint(l) == a ? 1.0 - wb : 0.0) + (uint(l) == b ? wb : 0.0));
                }
                mask *= smoothstep(p.d.w - 0.1, p.d.w + 0.1, w);
            }
            if (p.d.y > 0.0)
            {
                const float c = valueNoise2(xz * p.d.x, seed) * 0.65 + valueNoise2(xz * p.d.x * 2.3 + 17.0, seed + 1) * 0.35;
                mask *= lerp(1.0, smoothstep(p.d.z - 0.12, p.d.z + 0.12, c), p.d.y);
            }
        }
        if (unitOf(h2) >= p.b.y * mask)
        {
            storeRow(slot, float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0), p.a.x, kInstanceHidden, 0);
            continue;
        }
        // Stand it: up between the world's and the ground's, a random lean, a yaw, a scale.
        const uint h4 = scatterHash(h0, 4u, seed, 11u), h5 = scatterHash(h0, 5u, seed, 13u), h6 = scatterHash(h0, 6u, seed, 17u);
        float3 up = normalize(lerp(float3(0, 1, 0), nrm, p.e.x));
        if (p.e.y > 0.0)
        {
            const float lean = unitOf(h4) * p.e.y, dir = unitOf(h5) * 6.28318531;
            up = normalize(up + float3(cos(dir), 0.0, sin(dir)) * tan(lean));
        }
        const float yaw = unitOf(h3) * p.f.z;
        const float3 heading = float3(sin(yaw), 0.0, cos(yaw));
        float3 right = cross(up, heading);
        right = dot(right, right) > 1e-6 ? normalize(right) : float3(1, 0, 0);
        const float3 forward = cross(right, up);
        const float s = lerp(p.b.z, p.b.w, unitOf(h6));
        const float3 pos = float3(xz.x, y, xz.y) + up * ((p.f.x - p.e.z) * s);
        const float3 cx = right * s, cy = up * s, cz = forward * s;
        const float shade = 1.0 - p.e.w * unitOf(scatterHash(h0, 7u, seed, 19u));
        const uint c8 = uint(round(saturate(shade) * 255.0));
        const uint tint = p.e.w > 0.0 ? (c8 | c8 << 8 | c8 << 16 | 255u << 24) : 0u;
        // Terrain-shaded (generated rocks): the layer, and the base blend in decimetres.
        const bool shaded = p.g.x >= 0.0;
        const uint extra = shaded ? uint(p.g.x) | min(uint(round(p.g.y * 10.0)), 255u) << 8 : 0u;
        storeRow(slot, float4(cx.x, cy.x, cz.x, pos.x), float4(cx.y, cy.y, cz.y, pos.y), float4(cx.z, cy.z, cz.z, pos.z), p.a.x,
                 kInstanceScatter | (shaded ? kInstanceTerrainShaded : 0u) | ((p.a.w & 1u) != 0 ? kInstanceNoShadow : 0u), tint, extra);
    }
}
