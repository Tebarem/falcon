// Probe GI (DDGI): irradiance probes on grids round the camera, updated by rays through the
// scene's BVH (probe_trace, probe_blend, probe_relocate) and sampled by whatever is lit by them
// (the resolve, transparent surfaces, water, the probe rays' own hits for further bounces).
//
// Cascades: kProbeCascades grids of kProbeDims probes, each twice the previous one's spacing
// (1 m, 2 m, 4 m by default). A grid follows the camera in whole probes; its probes sit at
// (world index + half) x spacing (plus a relocation offset: probePosition) and are stored at
// index mod dims, so a move only renews the probes it brings in.
//
// Atlases, one 2D texture each with the cascades stacked down it (a probe's tile at
// (x + y * dims.x, z + cascade * dims.z)):
//   irradiance  rgba16f, 8x8 texels a probe: 6x6 octahedral directions plus a border copied
//               from the opposite edges (so bilinear filtering wraps over the octahedron's seams);
//               the cosine-weighted mean radiance arriving from that direction's hemisphere
//               (irradiance / pi: diffuse light is albedo times it)
//   distance    rg16f, 16x16 texels a probe (14x14 plus a border): mean distance to the nearest
//               surface and its square, round the direction (for the Chebyshev visibility test)
//   data        rgba16f, a texel a probe: xyz its offset from its grid position (in spacings),
//               w its state (kProbeFresh, kProbeActive, kProbeInactive)

module probe_common;

import sky;

public static const uint3 kProbeDims = uint3(32, 16, 32);
public static const uint kProbeCascades = 3;
public static const uint kProbeRays = 64;       // a probe update's rays (a group)
public static const uint kProbeFixedRays = 16;  // the first ones, never turned: relocation and classification
public static const uint kIrradianceTexels = 6, kIrradianceTile = 8;
public static const uint kDistanceTexels = 14, kDistanceTile = 16;
public static const uint2 kProbeTiles = uint2(kProbeDims.x * kProbeDims.y, kProbeDims.z * kProbeCascades);

// Probe states (the data texture's w).
public static const float kProbeFresh = 0.0;     // no light yet (not sampled; its next update is taken whole)
public static const float kProbeActive = 1.0;
public static const float kProbeInactive = 2.0;  // no surface in its cells, or inside one (not sampled; traced lightly)

// Schedule entries (uint): slot index | cascade << 16 | fresh << 18 | age << 19. Fresh: just
// scrolled in (no offset yet, taken whole). Age: the probe's updates since it was fresh or since a
// change near it; its memory is the mean of them all (capped at the hysteresis), and its rays'
// turn is its age's, so a probe's light depends on what it saw, not on which frames it was
// updated in.
public struct ProbeEntry
{
    public uint index, cascade, age;
    public bool fresh;
};

public ProbeEntry probeEntry(uint e)
{
    ProbeEntry r;
    r.index = e & 0xffff;
    r.cascade = (e >> 16) & 3;
    r.fresh = ((e >> 18) & 1) != 0;
    r.age = e >> 19;
    return r;
}

// The grids, as the CPU sets them (render/probe_gi.h ProbeVolumeGpu).
public struct ProbeVolume
{
    public int4 base[3];      // xyz: the world index of each grid's lowest probe
    public float4 spacing;    // xyz: each grid's spacing; w: 1 when there are probes (0: GI off)
    public float4 bias;       // x: along the normal, y: toward the eye (in spacings); z: sky intensity
};

// What the update passes share (render/probe_gi.cpp ProbeUpdateUniforms).
public struct ProbeUpdate
{
    public ProbeVolume volume;
    public float4 sun;          // xyz: toward the sun; w: bounce intensity
    public float4 sunColor;     // rgb: its irradiance; w: sky intensity
    public uint4 params;        // x: entries, y: VT page-table byte offset, z: alpha-masked holes, w: entries to reset
    public uint4 batch;         // x: the first entry's index in the schedule, y: the first slot to reset's
    public float4 memory;       // x: hysteresis (the most a probe keeps), z: nearest front face (spacings)
};

public uint probeHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The turn of an update's rays past the fixed ones: uniformly random (Shoemake's quaternion), the
// same for every probe of the same age.
public float3x3 probeRotation(uint age)
{
    const uint h0 = probeHash(age * 3u + 0x5eedu), h1 = probeHash(h0 ^ 0x68e31da4u), h2 = probeHash(h1 ^ 0xb5297a4du);
    const float u1 = float(h0 >> 8) / 16777216.0, u2 = float(h1 >> 8) / 16777216.0 * 6.2831853, u3 = float(h2 >> 8) / 16777216.0 * 6.2831853;
    const float a = sqrt(1.0 - u1), b = sqrt(u1);
    const float x = a * sin(u2), y = a * cos(u2), z = b * sin(u3), w = b * cos(u3);
    return float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                    2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                    2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));
}

// Probes sit half a spacing off the whole multiples of it. Level geometry snaps to those (walls
// centred on whole metres, floors at 0, 3 m storeys), and probes on them sat inside walls, on
// wall tops and on slab edges: one on a closed room's wall top, pushed out above it, saw the
// wall's edge near and the ground far past it in one direction, and the spread let it light the
// room's top corners with the sky. Half a spacing off, a 0.25 m wall on a whole metre has
// 0.375 m clear on either side, and a storey's probes float between its floor and ceiling.
public static const float kProbeGridOffset = 0.5;

public float3 probePosition(int3 world, float3 offset, float spacing)
{
    return (float3(world) + kProbeGridOffset + offset) * spacing;
}

public uint3 probeSlot(uint index) { return uint3(index % kProbeDims.x, (index / kProbeDims.x) % kProbeDims.y, index / (kProbeDims.x * kProbeDims.y)); }

public uint3 slotOf(int3 world)
{
    const int3 d = int3(kProbeDims);
    return uint3(((world % d) + d) % d);
}

// The world index of the probe a grid keeps at `slot`.
public int3 worldOf(ProbeVolume v, uint cascade, uint3 slot)
{
    const int3 d = int3(kProbeDims), base = v.base[cascade].xyz;
    return base + ((int3(slot) - base) % d + d) % d;
}

public uint2 probeTile(uint3 slot, uint cascade)
{
    return uint2(slot.x + slot.y * kProbeDims.x, slot.z + cascade * kProbeDims.z);
}

// Octahedral mapping of a unit direction to [-1, 1]^2 and back.
public float2 probeOct(float3 d)
{
    float2 p = d.xz / max(abs(d.x) + abs(d.y) + abs(d.z), 1e-20);  // (a zero direction: the centre)
    if (d.y < 0.0)
        p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p;
}

public float3 probeOctDecode(float2 p)
{
    float3 d = float3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (d.y < 0.0)
        d.xz = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    return normalize(d);
}

// The direction a tile's interior texel stands for (texel in [0, texels)).
public float3 texelDirection(uint2 texel, uint texels)
{
    return probeOctDecode((float2(texel) + 0.5) / float(texels) * 2.0 - 1.0);
}

// Where a border texel copies from (DDGI's octahedral border): tile coordinates in [0, n + 1],
// the interior in [1, n].
public uint2 borderSource(uint2 t, uint n)
{
    const bool left = t.x == 0, right = t.x == n + 1, top = t.y == 0, bottom = t.y == n + 1;
    if ((left || right) && (top || bottom))
        return uint2(left ? n : 1, top ? n : 1);
    if (top || bottom)
        return uint2(n + 1 - t.x, top ? 1 : n);
    return uint2(left ? 1 : n, n + 1 - t.y);
}

// Spherical Fibonacci directions: i of n, spread evenly over the sphere.
public float3 fibonacciDirection(uint i, uint n)
{
    const float phi = 6.2831853 * frac(float(i) * 0.61803399);
    const float y = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float r = sqrt(saturate(1.0 - y * y));
    return float3(cos(phi) * r, y, sin(phi) * r);
}

// Ray `lane` of a probe update: the fixed set, then the rest turned by the frame's rotation.
public float3 probeRayDirection(uint lane, float3x3 rotation)
{
    if (lane < kProbeFixedRays)
        return fibonacciDirection(lane, kProbeFixedRays);
    return mul(rotation, fibonacciDirection(lane - kProbeFixedRays, kProbeRays - kProbeFixedRays));
}

// The probe atlases as a pass binds them: bilinear irradiance and distance, the data point
// sampled (each pass's struct reads its own globals).
public interface IProbeTextures
{
    float3 irradiance(float2 uv);
    float2 distance(float2 uv);
    float4 data(float2 uv);
};

public float4 probeData<T : IProbeTextures>(T t, uint2 tile)
{
    return t.data((float2(tile) + 0.5) / float2(kProbeTiles));
}

public float2 atlasUv(uint2 tile, float3 dir, uint texels, uint size)
{
    const float2 inTile = 1.0 + (probeOct(dir) * 0.5 + 0.5) * float(texels);
    return (float2(tile * size) + inTile) / float2(kProbeTiles * size);
}

// One grid's light at p (8 probes, DDGI's weights), and how much of it to use (0 near and past
// its edges, where the next grid takes over). The point is moved off the surface first: along the
// normal and toward the eye, so a probe just behind the surface doesn't take it.
float3 cascadeIrradiance<T : IProbeTextures>(ProbeVolume v, T t, uint cascade, float3 p, float3 n, float3 lookup, float3 toEye,
                         out float use)
{
    use = 0.0;
    const float spacing = v.spacing[cascade];
    const float3 biased = p + (n * v.bias.x + toEye * v.bias.y) * spacing;
    const float3 g = biased / spacing - kProbeGridOffset;
    const int3 lowest = int3(floor(g));
    const float3 alpha = g - float3(lowest);
    const int3 base = v.base[cascade].xyz, top = base + int3(kProbeDims) - 1;
    // In probes from the grid's edges: none at the outermost cell, all two probes in.
    const float3 inside = min(g - float3(base), float3(top) - g);
    const float edge = saturate(min(min(inside.x, inside.y), inside.z) - 1.0);
    if (edge <= 0.0)
        return 0.0;
    float3 sum = 0.0;
    float total = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 world = lowest + corner;
        const uint2 tile = probeTile(slotOf(world), cascade);
        const float4 d = probeData(t, tile);
        if (d.w != kProbeActive)
            continue;
        const float3 probe = probePosition(world, d.xyz, spacing);
        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(corner));
        // Probes behind the surface count less: smooth in the angle, never quite nothing.
        const float3 toProbe = (probe - p) * rsqrt(max(dot(probe - p, probe - p), 1e-12));
        const float facing = (dot(toProbe, n) + 1.0) * 0.5;
        float weight = facing * facing + 0.2;
        // Chebyshev: whether the probe saw a surface nearer than the point along this direction.
        const float3 fromProbe = biased - probe;
        const float dist = length(fromProbe);
        const float2 moments = t.distance(atlasUv(tile, fromProbe / max(dist, 1e-4), kDistanceTexels, kDistanceTile));
        if (dist > moments.x)
        {
            const float variance = abs(moments.y - moments.x * moments.x);
            const float over = dist - moments.x;
            const float chebyshev = variance / (variance + over * over);
            weight *= max(chebyshev * chebyshev * chebyshev, 0.05);
        }
        weight = max(weight, 1e-6);
        if (weight < 0.2)  // crush weak weights (DDGI): what a wall hides mostly goes
            weight *= weight * weight * 25.0;
        weight *= trilinear.x * trilinear.y * trilinear.z;
        sum += t.irradiance(atlasUv(tile, lookup, kIrradianceTexels, kIrradianceTile)) * weight;
        total += weight;
    }
    if (total < 1e-5)
        return 0.0;
    use = edge;
    return sum / total;
}

// The probes' light at p for a surface facing n (the cosine-weighted mean radiance: diffuse light
// is albedo times it). toEye: unit direction back toward the eye (or the ray's origin). Each grid
// from the finest, the next taking over at its edges; past them all, the sky.
public float3 probeIrradiance<T : IProbeTextures>(ProbeVolume v, T t, float3 p, float3 n, float3 toEye)
{
    return probeIrradiance(v, t, p, n, n, toEye);
}

// The same with the light looked up for another normal than the surface's (a normal map's): the
// surface's normal moves the point and weighs the probes.
public float3 probeIrradiance<T : IProbeTextures>(ProbeVolume v, T t, float3 p, float3 n, float3 lookup, float3 toEye)
{
    float3 light = 0.0;
    float left = 1.0;
    [unroll]
    for (uint c = 0; c < kProbeCascades; ++c)
    {
        float use;
        const float3 e = cascadeIrradiance(v, t, c, p, n, lookup, toEye, use);
        light += e * (use * left);
        left *= 1.0 - use;
        if (left <= 0.001)
            break;
    }
    return light + probeSkyFallback(lookup) * (v.bias.z * left);
}

// Past every grid: the sky over the normal's hemisphere, roughly.
public float3 probeSkyFallback(float3 n)
{
    return lerp(float3(0.035, 0.032, 0.03), float3(0.16, 0.2, 0.27), saturate(n.y * 0.5 + 0.5));
}
