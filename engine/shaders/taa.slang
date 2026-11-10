// Temporal anti-aliasing, and temporal upscaling (TAAU) when the display is larger than the
// render size: reproject history through depth (closest depth in 3x3 for edges), Catmull-Rom
// history fetch, variance clipping in YCoCg, luminance-weighted blend. The output and history
// are display-sized; the current frame, depth and velocity are render-sized.

import scene_types;
import post_common;

struct TaaUniforms
{
    ViewReconstruct view;     // render size, jitter
    float4x4 viewToPrevClip;  // current view space -> previous frame's unjittered clip space
    float4 params;            // x: blend of the current frame, y: 1 = history invalid (reset), z: how much the reactive mask counts
    uint4 display;            // xy: display (output, history) size
};

[[vk::binding(0, 0)]] Sampler2D<float4> current : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] Sampler2D<float4> history : register(t1, space0) : register(s1, space0);
[[vk::binding(2, 0)]] Sampler2D<float> depth : register(t2, space0) : register(s2, space0);
[[vk::binding(3, 0)]] Sampler2D<float2> velocity : register(t3, space0) : register(s3, space0);  // render pixels to last frame
// Where the frame has content that doesn't move with the surface behind it (particles, r 0..1):
// the current frame counts more there, so it doesn't smear.
[[vk::binding(4, 0)]] Sampler2D<float> reactive : register(t4, space0) : register(s4, space0);
[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<TaaUniforms> u : register(b0, space2);

float3 fetchCurrent(int2 p)
{
    p = clamp(p, int2(0, 0), int2(u.view.screen.xy) - 1);
    return current.SampleLevel((float2(p) + 0.5) / float2(u.view.screen.xy), 0.0).rgb;
}

// 5-tap Catmull-Rom (corners dropped), bilinear taps on the linear sampler. uv in the history
// (display-sized).
float3 sampleHistory(float2 uv)
{
    const float2 size = float2(u.display.xy);
    const float2 pos = uv * size;
    const float2 c = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (c - 1.0) / size, t3 = (c + 2.0) / size, t12 = (c + w2 / w12) / size;
    float3 r = history.SampleLevel(float2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y);
    r += history.SampleLevel(float2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y);
    r += history.SampleLevel(t12, 0.0).rgb * (w12.x * w12.y);
    r += history.SampleLevel(float2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y);
    r += history.SampleLevel(float2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_taa(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= u.display.x || tid.y >= u.display.y)
        return;
    const float2 renderSize = float2(u.view.screen.xy), displaySize = float2(u.display.xy);
    const bool upscaling = any(u.view.screen.xy != u.display.xy);
    const float2 toRender = renderSize / displaySize;  // render pixels per display pixel
    const float2 uv = (float2(tid.xy) + 0.5) / displaySize;
    const float2 at = uv * renderSize;                  // this pixel's centre, in render pixels
    const int2 p = int2(floor(at));                     // the render pixel it falls in
    const float2 inv = 1.0 / renderSize;

    // Neighbourhood statistics, the closest depth (reverse-Z: largest value), and the current
    // frame reconstructed at this pixel's centre: each render pixel sampled the scene `jitter`
    // away from its own centre, so a Gaussian of its sample's distance to our centre
    // unjitters it (else the current frame's share keeps shifting edges with the jitter phase)
    // and, when upscaling, interpolates between the sparser render samples.
    const float2 unjitter = float2(u.view.jitter.x, -u.view.jitter.y) * 0.5 * renderSize;
    float3 m1 = 0.0, m2 = 0.0, filtered = 0.0;
    float filterWeight = 0.0, nearest = 0.0;
    float closest = 0.0;
    int2 closestPixel = p;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 q = p + int2(x, y);
            const float3 rgb = fetchCurrent(q);
            const float2 offset = float2(q) + 0.5 - unjitter - at;  // its sample, from our centre (render px)
            const float w = exp(-2.29 * dot(offset, offset));       // ~Blackman-Harris of radius 1.5 px
            filtered += rgb * w;
            filterWeight += w;
            const float2 offsetDisplay = offset / toRender;          // ... in display pixels
            nearest = max(nearest, exp(-2.29 * dot(offsetDisplay, offsetDisplay)));
            const float3 c = rgbToYCoCg(rgb);
            m1 += c;
            m2 += c * c;
            const int2 qc = clamp(q, int2(0, 0), int2(u.view.screen.xy) - 1);
            const float d = fetchFloat(depth, uint2(qc), inv);
            if (d > closest)
            {
                closest = d;
                closestPixel = qc;
            }
        }
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 cur = filtered / max(filterWeight, 1e-6);

    if (u.params.y > 0.0)
    {
        output[tid.xy] = float4(cur, 1.0);
        return;
    }

    // Reproject (sky: depth 0 -> treat as infinitely far, camera rotation only).
    float2 prevUv;
    if (closest > 0.0)
    {
        // Per-pixel motion (camera and moving instances) of the closest surface nearby, so
        // edges of moving objects reproject with the object.
        const float2 motion = velocity.SampleLevel((float2(closestPixel) + 0.5) * inv, 0.0);
        prevUv = uv + motion * inv;
    }
    else
    {
        // The pixel's own direction, without the jitter viewPosition takes out (the history is
        // unjittered: see the resolve's motion vectors).
        const float3 vp = viewPosition(u.view, at + unjitter, 1e-7);
        const float4 clip = mul(u.viewToPrevClip, float4(vp, 0.0));  // direction only
        const float2 ndc = clip.xy / clip.w;
        prevUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    }
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        output[tid.xy] = float4(cur, 1.0);
        return;
    }

    // Variance clipping toward the neighbourhood mean.
    const float3 hist = rgbToYCoCg(sampleHistory(prevUv));
    const float3 lo = mean - 1.25 * sigma, hi = mean + 1.25 * sigma;
    const float3 dir = hist - mean;
    const float3 extent = max(hi - mean, 1e-5);
    const float3 unit = abs(dir / extent);
    const float scale = max(unit.x, max(unit.y, unit.z));
    const float3 clipped = scale > 1.0 ? mean + dir / scale : hist;

    // Luminance weighting (Karis) suppresses flicker from bright subpixel details. Upscaling:
    // the current frame counts by how close a jittered sample landed to this display pixel,
    // so detail builds up over the jitter period instead of being smeared in every frame.
    const float3 histRgb = yCoCgToRgb(clipped);
    float a = u.params.x * (upscaling ? saturate(2.0 * nearest) : 1.0);
    a = lerp(a, 1.0, saturate(reactive.SampleLevel(uv, 0.0) * u.params.z));
    const float wc = a / (1.0 + luminance(cur)), wh = (1.0 - a) / (1.0 + luminance(histRgb));
    output[tid.xy] = float4((cur * wc + histRgb * wh) / (wc + wh), 1.0);
}
