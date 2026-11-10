// Many-light shading: binned clustered lighting (Activision style).
//   lights     LightGpu[] (scene order, static)
//   grid       [zbins: kZBins uints (min | max << 16 sorted light index)]
//              [order: sorted light index -> scene light index]
//              [tiles: tilesX * tilesY * words bitmask words over sorted indices]
// Lights are sorted by view depth each frame, so a z-bin covers a contiguous index range and
// only the bitmask words inside that range are visited.

module lighting;

public static const uint kZBins = 1024;
public static const uint kTileSize = 16;
public static const uint kLightStride = 64;
public static const uint kLightPoint = 0;
public static const uint kLightSpot = 1;
public static const float kPi = 3.14159265;

public struct LightGpu
{
    public float4 positionRange;  // xyz, w range
    public float4 color;          // rgb * intensity, w type
    public float4 direction;      // xyz (direction the light points to), w cos outer
    public float4 params;         // x cos inner, y shadow slot (-1: no shadows)
};

public struct LightGrid
{
    public uint orderOffset;  // word offsets into the grid buffer
    public uint tileOffset;
    public uint tilesX;
    public uint words;        // bitmask words per tile
    public float binScale;    // z-bin = view depth * binScale
    public uint count;
    public uint pad0, pad1;
};

public LightGpu loadLight(ByteAddressBuffer lights, uint i)
{
    return lights.Load<LightGpu>(i * kLightStride);
}

// Windowed inverse-square falloff (smoothly reaches 0 at the range).
public float distanceAttenuation(float d2, float range)
{
    const float x = d2 / (range * range);
    const float w = saturate(1.0 - x * x);
    return w * w / max(d2, 1e-4);
}

public float spotAttenuation(LightGpu l, float3 toLight)
{
    if (uint(l.color.w) != kLightSpot)
        return 1.0;
    const float cd = dot(-toLight, l.direction.xyz);
    return saturate((cd - l.direction.w) / max(l.params.x - l.direction.w, 1e-4));
}

// GGX specular + Lambert diffuse for one light, times N.L (radiance in, radiance out).
public float3 brdf(float3 albedo, float metallic, float roughness, float3 n, float3 v, float3 l)
{
    const float3 h = normalize(v + l);
    const float ndl = saturate(dot(n, l)), ndv = max(dot(n, v), 1e-4), ndh = saturate(dot(n, h)), vdh = saturate(dot(v, h));
    if (ndl <= 0.0)
        return 0.0;
    const float a = roughness * roughness, a2 = a * a;
    const float d = a2 / (kPi * pow(ndh * ndh * (a2 - 1.0) + 1.0, 2.0));
    const float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    const float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
    const float3 f0 = lerp(0.04, albedo, metallic);
    const float3 f = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    const float3 spec = d * g * f / max(4.0 * ndl * ndv, 1e-4);
    const float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / kPi;
    return (diffuse + spec) * ndl;
}

// Sum of all punctual lights affecting a pixel (tile bitmask AND z-bin index range).
public float3 shadeLights(ByteAddressBuffer lights, ByteAddressBuffer grid, LightGrid g, uint2 pixel, float viewDepth,
                          float3 p, float3 albedo, float metallic, float roughness, float3 n, float3 v, out uint visited)
{
    visited = 0;
    float3 sum = 0.0;
    if (g.count == 0)
        return sum;
    const uint bin = min(uint(max(viewDepth, 0.0) * g.binScale), kZBins - 1);
    const uint range = grid.Load(bin * 4);
    const uint first = range & 0xffff, last = range >> 16;
    if (first > last)
        return sum;
    const uint tile = (pixel.y / kTileSize) * g.tilesX + pixel.x / kTileSize;
    const uint tileBase = (g.tileOffset + tile * g.words) * 4;
    for (uint w = first >> 5; w <= (last >> 5); ++w)
    {
        uint bits = grid.Load(tileBase + w * 4);
        const uint lo = w * 32, hi = lo + 31;
        if (first > lo)
            bits &= ~0u << (first - lo);
        if (last < hi)
            bits &= ~0u >> (hi - last);
        while (bits != 0)
        {
            const uint b = firstbitlow(bits);
            bits &= bits - 1;
            const uint sorted = w * 32 + b;
            const LightGpu l = loadLight(lights, grid.Load((g.orderOffset + sorted) * 4));
            if (l.params.y >= 0.0)
                continue;  // shadow-casting lights are shaded separately, with their shadow maps
            const float3 d = l.positionRange.xyz - p;
            const float d2 = dot(d, d);
            if (d2 >= l.positionRange.w * l.positionRange.w)
                continue;
            const float3 toLight = d * rsqrt(max(d2, 1e-8));
            const float atten = distanceAttenuation(d2, l.positionRange.w) * spotAttenuation(l, toLight);
            sum += l.color.rgb * atten * brdf(albedo, metallic, roughness, n, v, toLight);
            ++visited;
        }
    }
    return sum;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile"): what
// share of the light from the reflection direction reaches the eye.
public float3 envSpecular(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022), c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}
