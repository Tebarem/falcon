// Shared helpers for the post-processing chain (M5).

module post_common;

public struct ViewReconstruct
{
    public float4 proj;    // P00, P11, zNear, unused
    public float4 jitter;  // xy: subpixel jitter in NDC applied this frame (subtract to unjitter)
    public uint4 screen;   // width, height, frame, unused
};

// View-space position (camera looks down -Z) of a pixel from reverse-Z infinite-far depth.
public float3 viewPosition(ViewReconstruct v, float2 pixel, float depth)
{
    const float2 ndc = float2(pixel.x / v.screen.x * 2.0 - 1.0, 1.0 - pixel.y / v.screen.y * 2.0) - v.jitter.xy;
    const float viewZ = v.proj.z / max(depth, 1e-12);
    return float3(ndc.x / v.proj.x * viewZ, ndc.y / v.proj.y * viewZ, -viewZ);
}

// View-space position of a full-resolution pixel center, from the depth buffer (clamped to the
// screen).
public float3 depthPosition(ViewReconstruct v, Sampler2D<float> depth, float2 pixel)
{
    const uint2 p = uint2(clamp(pixel, 0.0, float2(v.screen.xy) - 1.0));
    const float2 c = float2(p) + 0.5;
    return viewPosition(v, c, depth.SampleLevel(c / float2(v.screen.xy), 0.0));
}

// View-space normal at pixel (position p) from the depth neighbours with the smaller depth
// difference on each axis (keeps edges sharp).
public float3 depthNormal(ViewReconstruct v, Sampler2D<float> depth, float2 pixel, float3 p)
{
    const float3 l = depthPosition(v, depth, pixel - float2(1, 0)), r = depthPosition(v, depth, pixel + float2(1, 0));
    const float3 t = depthPosition(v, depth, pixel - float2(0, 1)), b = depthPosition(v, depth, pixel + float2(0, 1));
    // At the screen border the clamped neighbour is the pixel itself: use the other side.
    const float2 size = float2(v.screen.xy);
    const bool hasL = pixel.x > 1.0, hasR = pixel.x + 1.0 < size.x;
    const bool hasT = pixel.y > 1.0, hasB = pixel.y + 1.0 < size.y;
    const float3 dx = hasR && (!hasL || abs(r.z - p.z) < abs(p.z - l.z)) ? r - p : p - l;
    const float3 dy = hasB && (!hasT || abs(b.z - p.z) < abs(p.z - t.z)) ? b - p : p - t;
    return normalize(cross(dy, dx));
}

// Ambient terms at reduced resolution (AO, GI): texel i shades full-resolution pixel
// i * scale + offset (scale 1 or 2; with TAA the offset walks the 2x2 block). The resolve
// upsamples them. Sky texels store this view depth.
public static const float kSkyViewDepth = 65504.0;

public uint2 reducedSize(uint2 screen, uint scale) { return (screen + scale - 1) / scale; }

public uint2 reducedPixel(uint2 texel, uint scale, uint2 offset, uint2 screen)
{
    return min(texel * scale + offset, screen - 1);
}

// Rotates with the 8-frame TAA jitter period, so a converged frame depends only on the phase.
public float interleavedGradientNoise(float2 pixel, uint frame)
{
    pixel += float(frame % 8) * 5.588238;
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

public float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

public float3 rgbToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

public float3 yCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

public float3 agxContrast(float3 x)
{
    const float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// AgX (Sobotka; polynomial fit by Wrensch): linear HDR -> display-encoded.
public float3 agx(float3 c)
{
    // Rows given so mul(c, M) matches the reference GLSL M * c (GLSL matrices are column-major).
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(max(c, 1e-10), inset);
    c = (clamp(log2(c), minEv, maxEv) - minEv) / (maxEv - minEv);
    c = agxContrast(c);
    return saturate(mul(c, outset));
}

// The reference AgX look: an ASC CDL power and saturation on the encoded image. AgX base
// alone is deliberately flat (midtones high, shadows lifted); "Punchy" is power 1.35,
// saturation 1.4.
public float3 agxLook(float3 c, float power, float saturation)
{
    c = pow(c, power);
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    return saturate(luma + saturation * (c - luma));
}
