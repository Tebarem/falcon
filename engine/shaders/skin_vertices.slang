// GPU skinning (M8), two passes per frame:
//   cs_skin         every vertex of every skinned instance: linear blend skinning with up to
//                   4 bones -> model-space position, normal, tangent
//   cs_skin_bounds  every cluster of every skinned instance: bounding sphere of its skinned
//                   vertices (culling uses it instead of the bind-pose sphere)
// Dispatch: x = ceil(max items / 64), y = skinned instance record.

import scene_types;
import skinning;

struct SkinUniforms
{
    uint4 params;  // record count
};

[[vk::binding(0, 0)]] ByteAddressBuffer scene : register(t0, space0);
[[vk::binding(0, 1)]] RWByteAddressBuffer skin : register(u0, space1);
[[vk::binding(0, 2)]] ConstantBuffer<SkinUniforms> u : register(b0, space2);

uint snorm16(float v) { return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xffff; }

uint octEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-12);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return snorm16(e.x) | snorm16(e.y) << 16;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_skin(uint3 tid : SV_DispatchThreadID)
{
    if (tid.y >= u.params.x)
        return;
    const SkinInstanceGpu s = loadSkinInstance(skin, tid.y);
    if (tid.x >= s.vertexCount || s.firstSkinVertex == 0xffffffffu)  // (a deform record: vertex_deform writes it)
        return;
    const DecodedVertex v = loadVertex(scene, loadMesh(scene, s.mesh), s.firstVertex + tid.x);
    const uint2 jw = scene.Load2(sceneSection(scene, kSecSkinVertices) + (s.firstSkinVertex + tid.x) * 8);

    // Blended skinning matrix (rows of a 3x4). To the eye, a hidden bone's share of the vertex
    // goes where the bone collapses to instead (the hidden bone's joint).
    const uint bones = skinSection(skin, kSkinHdrBones), collapse = skinSection(skin, kSkinHdrCollapse);
    const float4 p = float4(v.position, 1.0);
    float4 r0 = 0.0, r1 = 0.0, r2 = 0.0;
    float3 hiddenShare = 0.0, hiddenTo = 0.0;  // the hidden bones' part of the position, and where it goes
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        const float w = float((jw.y >> (8 * k)) & 0xff) / 255.0;
        if (w == 0.0)
            continue;
        const uint bone = s.firstBone + ((jw.x >> (8 * k)) & 0xff);
        const uint a = bones + bone * 48;
        const float4 b0 = asfloat(skin.Load4(a)), b1 = asfloat(skin.Load4(a + 16)), b2 = asfloat(skin.Load4(a + 32));
        r0 += b0 * w;
        r1 += b1 * w;
        r2 += b2 * w;
        if (s.hidden != 0)
        {
            const float4 c = asfloat(skin.Load4(collapse + bone * 16));
            if (c.w != 0.0)
            {
                hiddenShare += float3(dot(b0, p), dot(b1, p), dot(b2, p)) * w;
                hiddenTo += c.xyz * w;
            }
        }
    }
    const float3 position = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    const float3 eyePosition = position - hiddenShare + hiddenTo;
    const float3 normal = float3(dot(r0.xyz, v.normal), dot(r1.xyz, v.normal), dot(r2.xyz, v.normal));
    const float3 tangent = float3(dot(r0.xyz, v.tangent.xyz), dot(r1.xyz, v.tangent.xyz), dot(r2.xyz, v.tangent.xyz));

    const uint out = skinSection(skin, kSkinHdrVertices) + (s.vertexBase + tid.x) * kSkinVertexStride;
    skin.Store3(out, asuint(position));
    skin.Store2(out + 12, uint2(octEncode(normal), octEncode(tangent)));
    skin.Store3(out + kSkinViewPosition, asuint(eyePosition));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_skin_bounds(uint3 tid : SV_DispatchThreadID)
{
    if (tid.y >= u.params.x)
        return;
    const SkinInstanceGpu s = loadSkinInstance(skin, tid.y);
    if (tid.x >= s.clusterCount)
        return;
    const ClusterGpu cl = loadCluster(scene, s.clusterOffset + tid.x);
    const uint vertices = skinSection(skin, kSkinHdrVertices);
    float3 lo = 1e30, hi = -1e30;
    const uint n = clusterVertexCount(cl);
    for (uint i = 0; i < n; ++i)
    {
        const uint g = loadClusterVertex(scene, cl, i);
        const float3 p = asfloat(skin.Load3(vertices + (s.vertexBase + g - s.firstVertex) * kSkinVertexStride));
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const float3 c = (lo + hi) * 0.5;
    float r = 0.0;
    for (uint i = 0; i < n; ++i)
    {
        const uint g = loadClusterVertex(scene, cl, i);
        const float3 p = asfloat(skin.Load3(vertices + (s.vertexBase + g - s.firstVertex) * kSkinVertexStride));
        r = max(r, length(p - c));
    }
    skin.Store4(skinSection(skin, kSkinHdrClusterBounds) + (s.clusterBoundsBase + tid.x) * 16, asuint(float4(c, r)));
}
