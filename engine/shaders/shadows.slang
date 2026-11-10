// Shadow lookups in the virtual shadow map (vsm.slang): the sun's clipmap levels and the local
// lights' faces, filtered. Shared by every pass that lights a surface (the resolve, water). The
// pass gives access to its atlas and page state through an IShadowMaps (its own bindings).

module shadows;

import lighting;
import vsm;

public interface IShadowMaps
{
    VsmParams vsm();
    float3 sunDirection();               // toward the sun
    uint resident(uint entry);           // residentPage(vsm, state, entry): physical page, or ~0
    float compare(float2 uv, float ref); // the atlas's depth compare (bilinear), uv over the atlas
}

// One bilinear depth-compare tap of sun level k at virtual texel coordinate c (world-anchored:
// texel i spans [i, i + 1)). The tap reads its own page, so filters cross page borders; a
// neighbour page that is not resident falls back to the edge of the centre page.
uint sunPage<S : IShadowMaps>(S sm, uint k, int2 page)
{
    return inWindow(sm.vsm(), k, page) ? sm.resident(pageEntry(k, page)) : 0xffffffffu;
}

float sunTap<S : IShadowMaps>(S sm, uint k, float2 c, int2 centerPage, uint centerPhys, float ref)
{
    int2 page = int2(floor(c / float(kVsmPageTexels)));
    uint phys = centerPhys;
    if (any(page != centerPage))
    {
        phys = sunPage(sm, k, page);
        if (phys == 0xffffffffu)
        {
            phys = centerPhys;
            page = centerPage;
        }
    }
    const float2 inPage = clamp(c - float2(page) * float(kVsmPageTexels), 0.5, float(kVsmPageTexels) - 0.5);
    return sm.compare((physicalOrigin(phys) + inPage) / float(kAtlasTexels), ref);
}

// Sun visibility at level k: normal-offset receiver, 5x5-texel tent filter from 9 bilinear
// compare taps (Castano's optimized PCF). False if the level has no current page here.
// lightBias: also move the receiver this many texels toward the sun (foliage: see the resolve).
bool sunLevelShadow<S : IShadowMaps>(S sm, uint k, float3 p, float3 n, float lightBias, out float visibility)
{
    visibility = 1.0;
    const VsmParams v = sm.vsm();
    const float texel = levelWorldSize(v, k) / float(kLevelTexels);
    const float3 q = p + n * (texel * 2.0) + sm.sunDirection() * (texel * lightBias);
    const float2 uv = lightXY(v, q) / texel;
    const int2 centerPage = int2(floor(uv / float(kVsmPageTexels)));
    const uint centerPhys = sunPage(sm, k, centerPage);
    if (centerPhys == 0xffffffffu)
        return false;
    const float ref = lightDepth(v, q);

    const float2 base = floor(uv + 0.5);
    const float2 s = uv + 0.5 - base;
    const float2 w0 = 4.0 - 3.0 * s, w2 = 1.0 + 3.0 * s;  // the middle weight is 7
    const float2 o0 = (3.0 - 2.0 * s) / w0 - 2.0, o1 = (3.0 + s) / 7.0, o2 = s / w2 + 2.0;
    const float2 b = base - 0.5;
    float sum = 0.0;
    sum += w0.x * w0.y * sunTap(sm, k, b + float2(o0.x, o0.y), centerPage, centerPhys, ref);
    sum += 7.0 * w0.y * sunTap(sm, k, b + float2(o1.x, o0.y), centerPage, centerPhys, ref);
    sum += w2.x * w0.y * sunTap(sm, k, b + float2(o2.x, o0.y), centerPage, centerPhys, ref);
    sum += w0.x * 7.0 * sunTap(sm, k, b + float2(o0.x, o1.y), centerPage, centerPhys, ref);
    sum += 49.0 * sunTap(sm, k, b + float2(o1.x, o1.y), centerPage, centerPhys, ref);
    sum += w2.x * 7.0 * sunTap(sm, k, b + float2(o2.x, o1.y), centerPage, centerPhys, ref);
    sum += w0.x * w2.y * sunTap(sm, k, b + float2(o0.x, o2.y), centerPage, centerPhys, ref);
    sum += 7.0 * w2.y * sunTap(sm, k, b + float2(o1.x, o2.y), centerPage, centerPhys, ref);
    sum += w2.x * w2.y * sunTap(sm, k, b + float2(o2.x, o2.y), centerPage, centerPhys, ref);
    visibility = sum / 144.0;
    return true;
}

// Sun visibility from the virtual shadow map: the finest resident level at or above the one
// this pixel's footprint asks for, cross-faded into the next level near the switch (vsm.slang
// levelBlend) so level changes don't show as lines that move with the camera.
public float sunShadow<S : IShadowMaps>(S sm, float3 p, float3 n, float distance, float lightBias, out uint levelUsed)
{
    levelUsed = 0xffffffffu;
    const VsmParams v = sm.vsm();
    const float kf = levelCoord(v, distance);
    const float2 xy = lightXY(v, p);
    for (uint k = desiredLevel(v, distance); k < v.config.x; ++k)
    {
        float a;
        if (!sunLevelShadow(sm, k, p, n, lightBias, a))
            continue;
        levelUsed = k;
        const float w = levelBlend(v, k, kf, xy);
        float b;
        if (w > 0.0 && sunLevelShadow(sm, k + 1, p, n, lightBias, b))
            a = lerp(a, b, w);
        return a;
    }
    return 1.0;
}

// One bilinear depth-compare tap of a local face mip at texel coordinate c (as sunTap; taps
// past the face's edge stay on the centre page).
uint facePage<S : IShadowMaps>(S sm, uint slot, uint f, uint m, int2 page)
{
    if (any(page < 0) || any(page >= int(facePages(m))))
        return 0xffffffffu;
    return sm.resident(localEntry(sm.vsm(), slot, f, m, uint2(page)));
}

float faceTap<S : IShadowMaps>(S sm, uint slot, uint f, uint m, float2 c, int2 centerPage, uint centerPhys, float ref)
{
    int2 page = int2(floor(c / float(kVsmPageTexels)));
    uint phys = centerPhys;
    if (any(page != centerPage))
    {
        phys = facePage(sm, slot, f, m, page);
        if (phys == 0xffffffffu)
        {
            phys = centerPhys;
            page = centerPage;
        }
    }
    const float2 inPage = clamp(c - float2(page) * float(kVsmPageTexels), 0.5, float(kVsmPageTexels) - 0.5);
    return sm.compare((physicalOrigin(phys) + inPage) / float(kAtlasTexels), ref);
}

// Visibility of face f at mip m: normal-offset receiver, the same 5x5 tent filter as the sun.
// False if the mip has no current page here; outside the face counts as lit.
bool localFaceShadow<S : IShadowMaps>(S sm, LightGpu l, uint slot, uint f, ShadowFace face, uint m, float3 p, float3 n, float z,
                                      out float visibility)
{
    visibility = 1.0;
    const float3 lp = l.positionRange.xyz;
    const float zNear = localNear(sm.vsm(), l);
    const uint pages = facePages(m);
    const float texel = 2.0 * z * face.tanHalf / float(pages * kVsmPageTexels);
    const float3 fq = faceProject(face, lp, p + n * (texel * 2.0));
    if (any(fq.xy < 0.0) || any(fq.xy >= 1.0) || fq.z <= zNear)
        return true;
    const float2 uv = fq.xy * float(pages * kVsmPageTexels);
    const int2 centerPage = min(int2(floor(uv / float(kVsmPageTexels))), int(pages) - 1);
    const uint centerPhys = facePage(sm, slot, f, m, centerPage);
    if (centerPhys == 0xffffffffu)
        return false;
    const float ref = zNear / fq.z;  // reverse-Z perspective depth, as rasterized

    const float2 base = floor(uv + 0.5);
    const float2 s = uv + 0.5 - base;
    const float2 w0 = 4.0 - 3.0 * s, w2 = 1.0 + 3.0 * s;
    const float2 o0 = (3.0 - 2.0 * s) / w0 - 2.0, o1 = (3.0 + s) / 7.0, o2 = s / w2 + 2.0;
    const float2 b = base - 0.5;
    float sum = 0.0;
    sum += w0.x * w0.y * faceTap(sm, slot, f, m, b + float2(o0.x, o0.y), centerPage, centerPhys, ref);
    sum += 7.0 * w0.y * faceTap(sm, slot, f, m, b + float2(o1.x, o0.y), centerPage, centerPhys, ref);
    sum += w2.x * w0.y * faceTap(sm, slot, f, m, b + float2(o2.x, o0.y), centerPage, centerPhys, ref);
    sum += w0.x * 7.0 * faceTap(sm, slot, f, m, b + float2(o0.x, o1.y), centerPage, centerPhys, ref);
    sum += 49.0 * faceTap(sm, slot, f, m, b + float2(o1.x, o1.y), centerPage, centerPhys, ref);
    sum += w2.x * 7.0 * faceTap(sm, slot, f, m, b + float2(o2.x, o1.y), centerPage, centerPhys, ref);
    sum += w0.x * w2.y * faceTap(sm, slot, f, m, b + float2(o0.x, o2.y), centerPage, centerPhys, ref);
    sum += 7.0 * w2.y * faceTap(sm, slot, f, m, b + float2(o1.x, o2.y), centerPage, centerPhys, ref);
    sum += w2.x * w2.y * faceTap(sm, slot, f, m, b + float2(o2.x, o2.y), centerPage, centerPhys, ref);
    visibility = sum / 144.0;
    return true;
}

// Visibility of a shadow-casting local light: cube face (or spot face) the point projects
// into, finest resident mip at or above the one its footprint asks for, cross-faded into the
// next mip near the switch.
public float localShadow<S : IShadowMaps>(S sm, LightGpu l, uint slot, float3 p, float3 n, float cameraDistance)
{
    const VsmParams v = sm.vsm();
    const float3 lp = l.positionRange.xyz;
    const uint f = pickFace(l, p - lp);
    const ShadowFace face = shadowFace(l, f);
    const float3 fp = faceProject(face, lp, p);
    if (fp.z <= localNear(v, l))
        return 1.0;
    const float mf = faceMipCoord(v, face, fp.z, cameraDistance);
    for (uint m = desiredFaceMip(v, face, fp.z, cameraDistance); m < kFaceMips; ++m)
    {
        float a;
        if (!localFaceShadow(sm, l, slot, f, face, m, p, n, fp.z, a))
            continue;
        const float w = faceMipBlend(m, mf);
        float b;
        if (w > 0.0 && localFaceShadow(sm, l, slot, f, face, m + 1, p, n, fp.z, b))
            a = lerp(a, b, w);
        return a;
    }
    return 1.0;
}

// The shadow-casting local lights' light at a surface, added to hdr light by light (their
// shadows looked up per light). visited: counts the lights that reached it (the light-count
// debug view).
public void shadeShadowedLights<S : IShadowMaps>(S sm, ByteAddressBuffer lights, float3 wpos, float3 n, float3 shadingN, float3 vdir,
                                                 float3 albedo, float metallic, float roughness, float cameraDistance, inout float3 hdr,
                                                 inout uint visited)
{
    const VsmParams v = sm.vsm();
    for (uint s = 0; s < v.local.x; ++s)
    {
        const LightGpu l = loadLight(lights, slotLiveLight(v, s));
        const float3 dl = l.positionRange.xyz - wpos;
        const float d2 = dot(dl, dl);
        if (d2 >= l.positionRange.w * l.positionRange.w)
            continue;
        const float3 toLight = dl * rsqrt(max(d2, 1e-8));
        const float atten = distanceAttenuation(d2, l.positionRange.w) * spotAttenuation(l, toLight);
        if (atten <= 0.0 || dot(n, toLight) <= 0.0)
            continue;
        const LightGpu shadow = loadLight(lights, slotLight(v, s));  // (as its pages were rendered)
        hdr += l.color.rgb * atten * localShadow(sm, shadow, s, wpos, n, cameraDistance) * brdf(albedo, metallic, roughness, shadingN, vdir, toLight);
        ++visited;
    }
}
