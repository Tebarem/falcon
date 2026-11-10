// Water bodies on the GPU (render/water_renderer.h): each body's parameter block (its shape and
// placement, its material resolved with the body's overrides, its waves), the surface's patches,
// and the wave functions the surface pass and the composite share.

module water_common;

public static const uint kWaterBodyStride = 464;
public static const uint kWaterPatchStride = 32;
public static const uint kWaterPatchQuads = 32;  // per patch side
public static const uint kMaxWaves = 8;

// Body shapes (WaterShape in water/water_system.h).
public static const uint kWaterRect = 0;
public static const uint kWaterCircle = 1;
public static const uint kWaterPolygon = 2;
public static const uint kWaterOcean = 3;
public static const uint kWaterRiver = 4;
public static const uint kWaterBox = 5;

public struct WaterBody
{
    public float4 posYaw;    // the surface's centre (world), yaw (radians)
    public float4 shape;     // x: shape, y: half width (circle: radius; polygon: its point count), z: depth below the surface, w: half length
    public float4 deep;      // rgb: deep colour, w: clarity (m)
    public float4 shallow;   // rgb: shallow tint, w: scattering
    public float4 optics;    // murk, refraction, roughness, reflection
    public float4 detail;    // ripple strength, ripple size (m), ripple speed, film coverage
    public float4 film;      // rgb, w: film patch size (m)
    public float4 foam;      // rgb, w: shore foam width (m)
    public float4 foam2;     // crest foam, foam size (m), edge softness (m), shore calm (m)
    public float4 caustic;   // caustics, size (m), depth (m), flow speed (m/s)
    public float4 fog;       // rgb, w: density (per m)
    public float4 flow;      // xy: flow direction (world), z: wave count (-1: the ocean's FFT waves), w: polygon: its first point
    public float4 wave[kMaxWaves];   // xy: direction, z: amplitude (m), w: wavenumber k (rad/m)
    public float4 wave2[kMaxWaves];  // x: angular frequency (rad/s), y: steepness Q, z: phase
    public float4 ripple;            // x: live ripples' strength (0: none), y: their fade
};

public WaterBody loadWaterBody(ByteAddressBuffer bodies, uint index)
{
    return bodies.Load<WaterBody>(index * kWaterBodyStride);
}

// A surface patch: kWaterPatchQuads^2 quads over a square of the body's local plane, at one
// level of the quadtree (render/water_renderer.cpp).
public struct WaterPatch
{
    public float2 origin;     // local x, z of its corner
    public float size;        // metres per side
    public float morphStart;  // camera distance where its odd vertices start moving onto its parent's grid
    public float morphEnd;    // ... and where they are there
    public uint body;
    public uint face;  // 0: the surface; a tank's sides 1..4 (-x, +x, -z, +z), 5 its bottom; 6 a river's stretch
    public uint pad;
};

public WaterPatch loadWaterPatch(ByteAddressBuffer patches, uint index)
{
    return patches.Load<WaterPatch>(index * kWaterPatchStride);
}

public float3 bodyToWorld(WaterBody b, float2 local)
{
    const float c = cos(b.posYaw.w), s = sin(b.posYaw.w);
    return float3(b.posYaw.x + local.x * c + local.y * s, b.posYaw.y, b.posYaw.z - local.x * s + local.y * c);
}

public float2 worldToBody(WaterBody b, float3 p)
{
    const float c = cos(b.posYaw.w), s = sin(b.posYaw.w);
    const float2 d = p.xz - b.posYaw.xz;
    return float2(d.x * c - d.y * s, d.x * s + d.y * c);
}

// Inside the body's outline (local coordinates). Polygons: an even-odd crossing test over their
// points (float2 each, from flow.w in `outlines`).
public bool oceanWaves(WaterBody b) { return b.flow.z < 0.0; }

public bool insideShape(WaterBody b, float2 local, ByteAddressBuffer outlines)
{
    const uint shape = uint(b.shape.x);
    if (shape == kWaterRect || shape == kWaterBox)
        return abs(local.x) <= b.shape.y && abs(local.y) <= b.shape.w;
    if (shape == kWaterCircle)
        return dot(local, local) <= b.shape.y * b.shape.y;
    if (shape == kWaterPolygon)
    {
        const uint first = uint(b.flow.w), count = uint(b.shape.y);
        bool inside = false;
        float2 a = outlines.Load<float2>((first + count - 1) * 8);
        for (uint i = 0; i < count; ++i)
        {
            const float2 c = outlines.Load<float2>((first + i) * 8);
            if ((c.y > local.y) != (a.y > local.y) && local.x < (a.x - c.x) * (local.y - c.y) / (a.y - c.y) + c.x)
                inside = !inside;
            a = c;
        }
        return inside;
    }
    return true;
}

public struct WaveSample
{
    public float3 offset;  // displacement of the still surface's point
    public float3 normal;
    public float jacobian; // < 1 where the surface is squeezed (crests): foam
};

// Gerstner waves (Tessendorf; GPU Gems 1, ch. 1) at a still-surface point. `scale` calms them
// (shores, distance). The CPU evaluates the same sum for physics (water/water_system.cpp).
public WaveSample gerstner(WaterBody b, float2 xz, float t, float scale)
{
    WaveSample w;
    w.offset = 0.0;
    float3 n = float3(0.0, 1.0, 0.0);
    float jxx = 1.0, jzz = 1.0, jxz = 0.0;
    const uint count = min(uint(b.flow.z), kMaxWaves);
    for (uint i = 0; i < count; ++i)
    {
        const float2 d = b.wave[i].xy;
        const float a = b.wave[i].z * scale, k = b.wave[i].w;
        const float q = b.wave2[i].y;
        const float theta = k * dot(d, xz) - b.wave2[i].x * t + b.wave2[i].z;
        const float c = cos(theta), s = sin(theta);
        w.offset += float3(q * a * d.x * c, a * s, q * a * d.y * c);
        const float ka = k * a;
        n.x -= d.x * ka * c;
        n.z -= d.y * ka * c;
        n.y -= q * ka * s;
        jxx -= q * ka * d.x * d.x * s;
        jzz -= q * ka * d.y * d.y * s;
        jxz -= q * ka * d.x * d.y * s;
    }
    w.normal = normalize(n);
    w.jacobian = jxx * jzz - jxz * jxz;
    return w;
}

// Schlick's Fresnel for water (F0 = 0.02).
public float waterFresnel(float NoV)
{
    const float f = 1.0 - saturate(NoV);
    const float f2 = f * f;
    return 0.02 + 0.98 * f2 * f2 * f;
}
