// FXAA (Lottes' FXAA 3.11, "quality" variant) on the display-encoded frame, after the
// tonemap: per pixel, a local-contrast test, then the edge's direction and its two ends
// searched along it, and a blend toward the neighbour across the edge by how far the pixel is
// from the nearer end. A subpixel term softens single-pixel features (aliasing the edge search
// can't see).

struct FxaaUniforms
{
    float4 texel;  // 1 / width, 1 / height
    uint4 screen;  // width, height
};

[[vk::binding(0, 0)]] Sampler2D<float4> frame : register(t0, space0) : register(s0, space0);  // bilinear, clamped
[[vk::binding(0, 1)]] [format("rgba8")] RWTexture2D<float4> output : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<FxaaUniforms> u : register(b0, space2);

static const float kEdgeThreshold = 1.0 / 8.0;     // minimum contrast, relative to the local maximum
static const float kEdgeThresholdMin = 1.0 / 16.0; // ... and absolute (dark areas)
static const float kSubpixel = 0.75;               // strength of the subpixel blend
static const uint kSteps = 10;
static const float kStepScale[kSteps] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0 };

float luma(float2 uv) { return dot(frame.SampleLevel(uv, 0.0).rgb, float3(0.299, 0.587, 0.114)); }
float lumaAt(float2 uv, int2 offset) { return luma(uv + float2(offset) * u.texel.xy); }

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_fxaa(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= u.screen.x || tid.y >= u.screen.y)
        return;
    const float2 uv = (float2(tid.xy) + 0.5) * u.texel.xy;
    const float4 center = frame.SampleLevel(uv, 0.0);
    const float m = dot(center.rgb, float3(0.299, 0.587, 0.114));
    // +y is down the image: "n" is the row above.
    const float n = lumaAt(uv, int2(0, -1)), s = lumaAt(uv, int2(0, 1));
    const float w = lumaAt(uv, int2(-1, 0)), e = lumaAt(uv, int2(1, 0));
    const float lumaMax = max(m, max(max(n, s), max(w, e)));
    const float lumaMin = min(m, min(min(n, s), min(w, e)));
    const float range = lumaMax - lumaMin;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
    {
        output[tid.xy] = center;
        return;
    }
    const float nw = lumaAt(uv, int2(-1, -1)), ne = lumaAt(uv, int2(1, -1));
    const float sw = lumaAt(uv, int2(-1, 1)), se = lumaAt(uv, int2(1, 1));

    // A horizontal edge changes most from row to row (and a vertical one across columns).
    const float rowChange = abs(nw - 2.0 * w + sw) + 2.0 * abs(n - 2.0 * m + s) + abs(ne - 2.0 * e + se);
    const float columnChange = abs(nw - 2.0 * n + ne) + 2.0 * abs(w - 2.0 * m + e) + abs(sw - 2.0 * s + se);
    const bool horizontal = rowChange >= columnChange;

    // Which side of the pixel the edge is on: the neighbour across it with the steeper change.
    const float before = horizontal ? n : w, after = horizontal ? s : e;
    const float gradBefore = before - m, gradAfter = after - m;
    const bool beforeSteeper = abs(gradBefore) >= abs(gradAfter);
    const float gradient = 0.25 * max(abs(gradBefore), abs(gradAfter));
    float step = horizontal ? u.texel.y : u.texel.x;  // across the edge
    float edgeLuma;
    if (beforeSteeper)
    {
        step = -step;
        edgeLuma = 0.5 * (before + m);
    }
    else
        edgeLuma = 0.5 * (after + m);

    // Walk both ways along the edge (half a texel across, on the edge itself) until the luma
    // there leaves the edge's average.
    float2 onEdge = uv;
    if (horizontal)
        onEdge.y += step * 0.5;
    else
        onEdge.x += step * 0.5;
    const float2 along = horizontal ? float2(u.texel.x, 0.0) : float2(0.0, u.texel.y);
    float2 uvNeg = onEdge - along, uvPos = onEdge + along;
    float endNeg = luma(uvNeg) - edgeLuma, endPos = luma(uvPos) - edgeLuma;
    bool doneNeg = abs(endNeg) >= gradient, donePos = abs(endPos) >= gradient;
    for (uint i = 1; i < kSteps && !(doneNeg && donePos); ++i)
    {
        if (!doneNeg)
        {
            uvNeg -= along * kStepScale[i];
            endNeg = luma(uvNeg) - edgeLuma;
            doneNeg = abs(endNeg) >= gradient;
        }
        if (!donePos)
        {
            uvPos += along * kStepScale[i];
            endPos = luma(uvPos) - edgeLuma;
            donePos = abs(endPos) >= gradient;
        }
    }

    const float distNeg = horizontal ? uv.x - uvNeg.x : uv.y - uvNeg.y;
    const float distPos = horizontal ? uvPos.x - uv.x : uvPos.y - uv.y;
    const bool negCloser = distNeg < distPos;
    const float dist = min(distNeg, distPos);
    // Only blend when the nearer end turns the way that continues this pixel's side of the edge.
    const bool centerDarker = m < edgeLuma;
    const bool consistent = ((negCloser ? endNeg : endPos) < 0.0) != centerDarker;
    float offset = consistent ? 0.5 - dist / (distNeg + distPos) : 0.0;

    // Subpixel: how far the pixel stands out from its 3x3 average.
    const float average = (2.0 * (n + s + w + e) + (nw + ne + sw + se)) / 12.0;
    const float sub = saturate(abs(average - m) / range);
    const float subSmooth = (3.0 - 2.0 * sub) * sub * sub;
    offset = max(offset, subSmooth * subSmooth * kSubpixel);

    float2 result = uv;
    if (horizontal)
        result.y += offset * step;
    else
        result.x += offset * step;
    output[tid.xy] = float4(frame.SampleLevel(result, 0.0).rgb, 1.0);
}
