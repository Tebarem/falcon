// Virtual shadow map page requests: reconstruct each pixel's world position from the depth
// buffer (half resolution) and request the page of the finest clipmap level its footprint needs.

import scene_types;
import vsm;
import lighting;

struct MarkUniforms
{
    VsmParams vsm;
    float4 cameraPos;
    float4 cameraRight;    // w = 1 / P00
    float4 cameraUp;       // w = 1 / P11
    float4 cameraForward;  // w = zNear
    uint4 screen;          // width, height
};

[[vk::binding(0, 0)]] Sampler2D<float> depth : register(t0, space0) : register(s0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer lights : register(t1, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer state : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<MarkUniforms> u : register(b0, space2);

void request(uint e)
{
    const uint addr = (u.vsm.layout.z + (e >> 5)) * 4, bit = 1u << (e & 31);
    if ((state.Load(addr) & bit) == 0)  // most neighbours want the same page: skip the atomic
        state.InterlockedOr(addr, bit);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_mark(uint3 tid : SV_DispatchThreadID)
{
    const uint2 pixel = tid.xy * 2;
    if (pixel.x >= u.screen.x || pixel.y >= u.screen.y)
        return;
    const float d = fetchFloat(depth, pixel, 1.0 / float2(u.screen.xy));
    if (d <= 0.0)
        return;  // sky
    const float2 ndc = float2((pixel.x + 0.5) / u.screen.x * 2.0 - 1.0, 1.0 - (pixel.y + 0.5) / u.screen.y * 2.0);
    const float3 dir = normalize(u.cameraForward.xyz + u.cameraRight.xyz * (ndc.x * u.cameraRight.w) +
                                 u.cameraUp.xyz * (ndc.y * u.cameraUp.w));
    const float viewZ = u.cameraForward.w / d;  // reverse-Z, infinite far: depth = zNear / viewZ
    const float dist = viewZ / dot(dir, u.cameraForward.xyz);
    const float3 p = u.cameraPos.xyz + dir * dist;
    const float2 xy = lightXY(u.vsm, p);
    const float kf = levelCoord(u.vsm, dist);
    for (uint k = desiredLevel(u.vsm, dist); k < u.vsm.config.x; ++k)
    {
        const int2 page = worldPage(u.vsm, k, xy);
        if (!inWindow(u.vsm, k, page))
            continue;
        request(pageEntry(k, page));
        // Where the resolve blends toward the next level, it needs that level's pages too (with
        // slack: see levelBlend).
        if (levelBlend(u.vsm, k, kf, xy, kBlendSlack) > 0.0)
        {
            const float margin = kBlendMarginTexels * levelWorldSize(u.vsm, k + 1) / float(kLevelTexels);
            [unroll]
            for (uint c = 0; c < 4; ++c)
            {
                const float2 o = float2(c & 1 ? margin : -margin, c & 2 ? margin : -margin);
                const int2 next = worldPage(u.vsm, k + 1, xy + o);
                if (inWindow(u.vsm, k + 1, next))
                    request(pageEntry(k + 1, next));
            }
        }
        break;
    }

    // Shadowed local lights reaching this point: the page of the face it projects into.
    for (uint s = 0; s < u.vsm.local.x; ++s)
    {
        const LightGpu l = loadLight(lights, slotLight(u.vsm, s));
        const float3 dl = p - l.positionRange.xyz;
        if (dot(dl, dl) >= l.positionRange.w * l.positionRange.w || spotAttenuation(l, -normalize(dl)) <= 0.0)
            continue;
        const uint f = pickFace(l, dl);
        const ShadowFace face = shadowFace(l, f);
        const float3 fp = faceProject(face, l.positionRange.xyz, p);
        if (fp.z <= localNear(u.vsm, l) || any(fp.xy < 0.0) || any(fp.xy >= 1.0))
            continue;
        const uint m = desiredFaceMip(u.vsm, face, fp.z, dist);
        request(localEntry(u.vsm, s, f, m, uint2(fp.xy * float(facePages(m)))));
        if (faceMipBlend(m, faceMipCoord(u.vsm, face, fp.z, dist), kBlendSlack) > 0.0)
        {
            const float pages = float(facePages(m + 1));
            const float margin = kBlendMarginTexels / (pages * float(kVsmPageTexels));  // in face uv
            [unroll]
            for (uint c = 0; c < 4; ++c)
            {
                const float2 uv = saturate(fp.xy + float2(c & 1 ? margin : -margin, c & 2 ? margin : -margin));
                request(localEntry(u.vsm, s, f, m + 1, min(uint2(uv * pages), uint(pages) - 1)));
            }
        }
    }
}
