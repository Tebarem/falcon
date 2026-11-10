// Noise for terrain generation: integer hashes (no tables, the same on every GPU), gradient
// noise, fractal sums (fbm, ridged, billow, and "eroded": fbm whose detail fades where the
// slope of the octaves before is steep), and Voronoi (nearest and second-nearest feature
// points, the nearest cell's id). Positions are in feature units (world metres / feature size).

module noise;

public uint noiseHash(uint a, uint b, uint c)
{
    uint h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
public float noiseUnit(uint h) { return float(h & 0xffffffu) / 16777216.0; }

float cornerGradient(int2 q, float2 d, uint seed)
{
    const float a = noiseUnit(noiseHash(uint(q.x), uint(q.y), seed)) * 6.28318531;
    return cos(a) * d.x + sin(a) * d.y;
}

// Gradient noise, about -1..1.
public float gradientNoise(float2 p, uint seed)
{
    const float2 i = floor(p), f = p - i;
    const int2 c = int2(i);
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float n00 = cornerGradient(c, f, seed), n10 = cornerGradient(c + int2(1, 0), f - float2(1, 0), seed);
    const float n01 = cornerGradient(c + int2(0, 1), f - float2(0, 1), seed), n11 = cornerGradient(c + int2(1, 1), f - float2(1, 1), seed);
    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y) * 1.41421356;
}

// Value noise (-1..1) and its derivative.
public float3 valueNoiseD(float2 p, uint seed)
{
    const float2 i = floor(p), f = p - i;
    const int2 c = int2(i);
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    const float a = noiseUnit(noiseHash(uint(c.x), uint(c.y), seed)) * 2.0 - 1.0;
    const float b = noiseUnit(noiseHash(uint(c.x + 1), uint(c.y), seed)) * 2.0 - 1.0;
    const float d = noiseUnit(noiseHash(uint(c.x), uint(c.y + 1), seed)) * 2.0 - 1.0;
    const float e = noiseUnit(noiseHash(uint(c.x + 1), uint(c.y + 1), seed)) * 2.0 - 1.0;
    const float k = a - b - d + e;
    return float3(a + (b - a) * u.x + (d - a) * u.y + k * u.x * u.y, du * (float2(b - a, d - a) + k * u.yx));
}

// Each octave turned (so the grids never line up) and scaled by the lacunarity.
float2 nextOctave(float2 p, float lacunarity)
{
    return float2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8) * lacunarity + float2(17.3, -9.1);
}

// 0..1.
public float fbm(float2 p, uint octaves, float lacunarity, float gain, uint seed)
{
    float sum = 0.0, amplitude = 1.0, norm = 0.0;
    for (uint o = 0; o < octaves; ++o)
    {
        sum += gradientNoise(p, seed + o * 101u) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        p = nextOctave(p, lacunarity);
    }
    return saturate(sum / max(norm, 1e-6) * 0.5 + 0.5);
}

// Ridges where the noise crosses zero, each octave weighted by the one before (Musgrave). 0..1.
public float ridged(float2 p, uint octaves, float lacunarity, float gain, float sharpness, uint seed)
{
    float sum = 0.0, amplitude = 1.0, norm = 0.0, weight = 1.0;
    for (uint o = 0; o < octaves; ++o)
    {
        float s = 1.0 - abs(gradientNoise(p, seed + o * 101u));
        s = pow(saturate(s), 1.0 + sharpness);
        s *= weight;
        weight = saturate(s * 2.0);
        sum += s * amplitude;
        norm += amplitude;
        amplitude *= gain;
        p = nextOctave(p, lacunarity);
    }
    return saturate(sum / max(norm, 1e-6));
}

// Rounded bumps (|noise|). 0..1.
public float billow(float2 p, uint octaves, float lacunarity, float gain, uint seed)
{
    float sum = 0.0, amplitude = 1.0, norm = 0.0;
    for (uint o = 0; o < octaves; ++o)
    {
        sum += abs(gradientNoise(p, seed + o * 101u)) * amplitude;
        norm += amplitude;
        amplitude *= gain;
        p = nextOctave(p, lacunarity);
    }
    return saturate(sum / max(norm, 1e-6) * 1.4);
}

// fbm whose octaves fade where the ones before are steep: smooth slopes, detailed flats and
// crests, a look of erosion without simulating it (Quilez). 0..1.
public float erodedFbm(float2 p, uint octaves, float lacunarity, float gain, uint seed)
{
    float sum = 0.0, amplitude = 1.0, norm = 0.0;
    float2 d = 0.0;
    for (uint o = 0; o < octaves; ++o)
    {
        const float3 n = valueNoiseD(p, seed + o * 101u);
        d += n.yz * amplitude;
        sum += amplitude * n.x / (1.0 + dot(d, d));
        norm += amplitude;
        amplitude *= gain;
        p = nextOctave(p, lacunarity);
    }
    return saturate(sum / max(norm, 1e-6) * 0.5 + 0.5);
}

// Voronoi: x = distance to the nearest feature point, y = to the second nearest, z = the
// nearest one's cell hash (as a float 0..1).
public float3 voronoi(float2 p, uint seed)
{
    const float2 i = floor(p);
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
        {
            const int2 c = int2(i) + int2(x, z);
            const uint h = noiseHash(uint(c.x), uint(c.y), seed);
            const float2 point = float2(c) + 0.1 + 0.8 * float2(noiseUnit(h), noiseUnit(noiseHash(h, 7u, seed)));
            const float dist = length(point - p);
            if (dist < f1)
            {
                f2 = f1;
                f1 = dist;
                id = noiseUnit(noiseHash(h, 13u, seed));
            }
            else if (dist < f2)
                f2 = dist;
        }
    return float3(f1, f2, id);
}
