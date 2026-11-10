// Screen-space ambient occlusion from depth alone (normals reconstructed from depth).
//   mode 0: GTAO (horizon-based, cosine-weighted, Jimenez et al. 2016), 2 slices x 6 steps
//   mode 1: SSAO (normal-oriented hemisphere, 12 samples)
// Noise rotates every frame; TAA integrates it.
// Runs at full or half resolution (mode.y = scale, mode.zw = sample offset; see reducedPixel).
// Output: r = AO, g = view depth of the shaded pixel (the denoise and the resolve's upsample
// weigh by it).

import scene_types;
import post_common;

struct AoUniforms
{
    ViewReconstruct view;
    float4 params;  // x: world radius, y: intensity (power), z: max radius in pixels
    uint4 mode;     // x: 0 GTAO, 1 SSAO; y: scale (1 or 2); zw: sample offset
};

[[vk::binding(0, 0)]] Sampler2D<float> depth : register(t0, space0) : register(s0, space0);
[[vk::binding(0, 1)]] [format("rg16f")] RWTexture2D<float2> ao : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<AoUniforms> u : register(b0, space2);

static const float kPi = 3.14159265;

float3 positionAt(float2 pixel) { return depthPosition(u.view, depth, pixel); }

float gtao(float2 pixel, float3 p, float3 n, float radiusPixels, float worldRadius, float noise, float noise2)
{
    const float3 v = normalize(-p);
    const uint slices = 2, steps = 6;
    float visibility = 0.0;
    for (uint s = 0; s < slices; ++s)
    {
        const float phi = (float(s) + noise) * kPi / float(slices);
        const float2 dir = float2(cos(phi), sin(phi));
        const float3 dir3 = float3(dir.x, -dir.y, 0.0);  // screen y down -> view y up
        const float3 axis = normalize(cross(dir3, v));
        const float3 projN = n - axis * dot(n, axis);
        const float projLen = length(projN);
        const float cosN = clamp(dot(projN / max(projLen, 1e-5), v), -1.0, 1.0);
        // cross(v, axis) is the slice direction orthogonalized against v: positive angles lean toward +dir.
        const float nAngle = sign(dot(projN, cross(v, axis))) * acos(cosN);
        float h[2];
        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            float maxCos = -1.0;
            const float sgn = side == 0 ? -1.0 : 1.0;
            for (uint i = 0; i < steps; ++i)
            {
                const float t = (float(i) + noise2) / float(steps);
                const float2 offset = dir * sgn * max(t * t * radiusPixels, float(i + 1));
                const float3 sp = positionAt(pixel + offset);
                const float3 d = sp - p;
                const float dist = length(d);
                const float falloff = saturate(1.0 - dist / worldRadius);
                maxCos = max(maxCos, lerp(-1.0, dot(d / max(dist, 1e-5), v), falloff));
            }
            h[side] = sgn * acos(clamp(maxCos, -1.0, 1.0));
        }
        const float h0 = nAngle + max(h[0] - nAngle, -kPi * 0.5);
        const float h1 = nAngle + min(h[1] - nAngle, kPi * 0.5);
        const float sinN = sin(nAngle);
        visibility += projLen * 0.25 * ((-cos(2.0 * h0 - nAngle) + cosN + 2.0 * h0 * sinN) +
                                        (-cos(2.0 * h1 - nAngle) + cosN + 2.0 * h1 * sinN));
    }
    return saturate(visibility / float(slices));
}

float ssao(float3 p, float3 n, float worldRadius, float noise)
{
    const float3 helper = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(helper, n));
    float3 b = cross(n, t);
    const float rot = noise * 2.0 * kPi;
    t = t * cos(rot) + b * sin(rot);
    b = cross(n, t);
    const uint count = 12;
    float occlusion = 0.0;
    for (uint i = 0; i < count; ++i)
    {
        // Fibonacci hemisphere, denser near the surface point.
        const float fi = (float(i) + 0.5) / float(count);
        const float r = sqrt(1.0 - fi), a = float(i) * 2.39996323;
        const float3 h = float3(cos(a) * r, sin(a) * r, sqrt(fi));
        const float scale = lerp(0.15, 1.0, fi * fi);
        const float3 sp = p + (t * h.x + b * h.y + n * h.z) * (worldRadius * scale);
        // project back to the screen
        const float2 ndc = float2(u.view.proj.x * sp.x, u.view.proj.y * sp.y) / -sp.z + u.view.jitter.xy;
        const float2 pixel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * float2(u.view.screen.xy);
        const float sceneZ = positionAt(pixel).z;
        const float range = saturate(worldRadius / max(abs(p.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= sp.z + 0.02 * worldRadius ? 1.0 : 0.0) * range;
    }
    return 1.0 - occlusion / float(count);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_ao(uint3 tid : SV_DispatchThreadID)
{
    const uint scale = u.mode.y;
    if (any(tid.xy >= reducedSize(u.view.screen.xy, scale)))
        return;
    const uint2 ip = reducedPixel(tid.xy, scale, u.mode.zw, u.view.screen.xy);
    const float2 pixel = float2(ip) + 0.5;
    const float d = fetchFloat(depth, ip, 1.0 / float2(u.view.screen.xy));
    if (d <= 0.0)
    {
        ao[tid.xy] = float2(1.0, kSkyViewDepth);
        return;
    }
    const float3 p = viewPosition(u.view, pixel, d);
    if (u.params.y <= 0.0)  // AO off (power 0)
    {
        ao[tid.xy] = float2(1.0, -p.z);
        return;
    }
    const float3 n = depthNormal(u.view, depth, pixel, p);
    const float worldRadius = u.params.x;
    const float radiusPixels = min(worldRadius * u.view.proj.y * 0.5 * float(u.view.screen.y) / -p.z, u.params.z);
    // Noise over the texels actually shaded (a stride-2 walk would alias the pattern).
    const float2 noisePixel = float2(tid.xy) + 0.5;
    const float noise = interleavedGradientNoise(noisePixel, u.view.screen.z);
    const float noise2 = interleavedGradientNoise(noisePixel.yx + 17.0, u.view.screen.z);
    float visibility = 1.0;
    if (radiusPixels >= 1.0)
        visibility = u.mode.x == 0 ? gtao(pixel, p, n, radiusPixels, worldRadius, noise, noise2) : ssao(p, n, worldRadius, noise);
    ao[tid.xy] = float2(pow(saturate(visibility), u.params.y), -p.z);
}
