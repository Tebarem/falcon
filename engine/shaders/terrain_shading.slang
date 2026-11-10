// The terrain's default look until it has material layers: grass on gentle ground, earth on
// slopes, rock on cliffs, varied by world-space noise at a few scales so nothing tiles.
// Shared by the resolve and the GI's rays (which must agree on its colour).

module terrain_shading;

public static const uint kMaterialTerrain = 8u;  // MaterialGpu.flags (fpak::kMaterialTerrain)

float hash21(float2 p)
{
    uint2 q = uint2(int2(floor(p)) + 32768);
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return float(h & 0xffffu) / 65535.0;
}

// Value noise in [0, 1], smooth (quintic).
float valueNoise(float2 p)
{
    const float2 i = floor(p), f = p - i;
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float a = hash21(i), b = hash21(i + float2(1, 0)), c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm3(float2 p)
{
    return valueNoise(p) * 0.5 + valueNoise(p * 2.03 + 17.1) * 0.3 + valueNoise(p * 4.11 + 41.7) * 0.2;
}

public struct TerrainLook
{
    public float3 albedo;  // linear
    public float roughness;
};

public TerrainLook terrainLook(float3 p, float3 n)
{
    const float slope = 1.0 - saturate(n.y);  // 0 flat, 1 vertical
    const float macro = fbm3(p.xz / 160.0);
    const float mid = fbm3(p.xz / 9.0);
    const float fine = valueNoise(p.xz * 1.7);
    const float3 grass = lerp(float3(0.045, 0.085, 0.022), float3(0.11, 0.13, 0.04), macro);
    const float3 earth = lerp(float3(0.10, 0.075, 0.05), float3(0.16, 0.13, 0.09), mid);
    const float3 rock = lerp(float3(0.17, 0.16, 0.15), float3(0.27, 0.25, 0.23), fbm3(p.xz / 3.0 + p.y / 5.0));
    // Slope thresholds wander with the noise, so the borders are ragged rather than contour lines.
    const float toEarth = smoothstep(0.10, 0.22, slope + (mid - 0.5) * 0.12);
    const float toRock = smoothstep(0.33, 0.48, slope + (macro - 0.5) * 0.15);
    TerrainLook t;
    t.albedo = lerp(lerp(grass, earth, toEarth), rock, toRock) * (0.85 + 0.3 * fine);
    t.roughness = lerp(0.95, 0.8, toRock);
    return t;
}
