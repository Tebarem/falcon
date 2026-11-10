// Shared definitions for the GPU-driven pipeline (mirrored in render/gpu_driven_renderer.h).

module culling;

import scene_types;

// counters buffer (uint indices): atomics written during culling
public static const uint kCtrChunks = 0;
public static const uint kCtrVisible1 = 1;
public static const uint kCtrVisible2 = 2;
public static const uint kCtrCandidates = 3;
public static const uint kCtrTriangles1 = 4;
public static const uint kCtrTriangles2 = 5;
public static const uint kCtrOverflow = 6;
public static const uint kCtrInstances = 7;
public static const uint kCtrMasked1 = 8;  // alpha-masked clusters, filled top-down from maxVisible
public static const uint kCtrMasked2 = 9;
// Triangles packed into each draw's index range so far (cs_expand), in draw order: pass 1
// opaque, pass 1 masked, pass 2 opaque, pass 2 masked.
public static const uint kCtrPacked = 10;
// Transparent (alpha-blend) clusters: candidates from the first cull (frustum, cone), and those
// still visible against this frame's HiZ (transparent_cull.slang).
public static const uint kCtrTransparentCandidates = 14;
public static const uint kCtrTransparentVisible = 15;
public static const uint kCounterCount = 16;
// kCtrOverflow bits: 1, 2 the visible clusters of pass 1 / 2; 4 the transparent ones.
public static const uint kOverflowTransparent = 4;
public static const uint kMaxTransparentCandidates = 16384;
public static const uint kMaxTransparent = 4096;  // sorted in one group (transparent_cull.slang)

// args buffer (byte offsets): indirect dispatch / draw arguments, never written by atomics
// in the same pass that consumes them (SDL cannot bind one buffer as UAV and indirect at once).
public static const uint kArgCull1 = 0;
public static const uint kArgCull2 = 16;
public static const uint kArgExpand1 = 32;
public static const uint kArgExpand2 = 48;
public static const uint kArgDraw1 = 64;
public static const uint kArgDraw1Masked = 96;
public static const uint kArgDraw2 = 128;
public static const uint kArgDraw2Masked = 160;
public static const uint kArgCullTransparent = 192;    // dispatch
public static const uint kArgExpandTransparent = 208;  // dispatch
public static const uint kArgDrawTransparent = 224;    // indexed draw
public static const uint kArgBytes = 256;

public static const uint kClustersPerChunk = 64;
public static const uint kIndicesPerCluster = 128 * 3;
public static const uint kVisibleSecondPassBit = 0x80000000u;  // set on VisibleCluster.instance

public static const uint kFlagOcclusion = 1u << 0;  // HiZ occlusion culling enabled
public static const uint kFlagCone = 1u << 1;       // backface cone culling enabled
public static const uint kFlagForceRetest = 1u << 2;  // debug: treat every pass-1 cluster as occluded
public static const uint kFlagNoAlphaMask = 1u << 3;  // debug: draw alpha-masked clusters as opaque

// Visible-cluster slots: opaque clusters grow up from 0 (pass 1, then pass 2), alpha-masked
// clusters grow down from maxVisible - 1, so each draw covers one contiguous index range.
public struct VisibleCounts
{
    public uint opaque1, opaque2, masked1, masked2;
};

public VisibleCounts loadVisibleCounts(RWByteAddressBuffer counters, uint maxVisible)
{
    VisibleCounts c;
    c.opaque1 = min(counters.Load(kCtrVisible1 * 4), maxVisible);
    c.masked1 = min(counters.Load(kCtrMasked1 * 4), maxVisible - c.opaque1);
    c.opaque2 = min(counters.Load(kCtrVisible2 * 4), maxVisible - c.opaque1 - c.masked1);
    c.masked2 = min(counters.Load(kCtrMasked2 * 4), maxVisible - c.opaque1 - c.masked1 - c.opaque2);
    return c;
}

public struct CullUniforms
{
    public float4x4 view;      // cull camera, current frame
    public float4x4 prevView;  // cull camera, previous frame (HiZ of pass 1 was rendered from it)
    public float4 planes[5];   // world-space frustum planes (left, right, bottom, top, near)
    public float4 cameraPos;   // xyz; w = LOD threshold as a fraction of screen height
    public float4 proj;        // P00, P11, zNear, w = smallest instance drawn: projected radius as a fraction of screen height
    public uint4 hizLevels[16];  // per level: word offset, width, height; .w of [0], [1]: terrain patch instances (first, count)
    public uint4 limits;       // instanceCount, maxChunks, maxVisible, maxCandidates
    public uint4 params;       // flags, hizLevelCount, screenWidth, screenHeight
    public float4 scatter;     // x: smallest scatter instance drawn (as proj.w), raised while the visible clusters overflow;
                               // y: full-detail distance (world units): nearer, full detail and nothing culled for its size
};

// Instance scale bound: lengths of the 3x3 columns.
public float maxScale(InstanceGpu inst)
{
    const float3 c0 = float3(inst.row0.x, inst.row1.x, inst.row2.x);
    const float3 c1 = float3(inst.row0.y, inst.row1.y, inst.row2.y);
    const float3 c2 = float3(inst.row0.z, inst.row1.z, inst.row2.z);
    return sqrt(max(dot(c0, c0), max(dot(c1, c1), dot(c2, c2))));
}

public bool sphereInFrustum(CullUniforms u, float3 c, float r)
{
    [unroll]
    for (uint i = 0; i < 5; ++i)
        if (dot(u.planes[i].xyz, c) + u.planes[i].w < -r)
            return false;
    return true;
}

// Projected simplification error (fraction of screen height) of a mesh-space LOD sphere.
// Same formula for a cluster and its parent keeps the DAG cut consistent. Within the full-detail
// distance any simplification is too much (a parent's sphere holds its children's, so it is
// nearer: the cut stays consistent).
public float lodError(CullUniforms u, InstanceGpu inst, float scale, float4 sphere, float error)
{
    const float3 c = transformPoint(inst, sphere.xyz);
    const float d = length(c - u.cameraPos.xyz) - sphere.w * scale;
    if (d < u.scatter.y && u.scatter.y > 0.0 && error > 0.0)
        return 1e30;
    return error * scale / max(d, u.proj.z) * (u.proj.y * 0.5);
}

// HiZ (min = farthest depth, reverse-Z) stored as a flat float buffer with a mip chain.
// Level k texel covers 2^(k+1) x 2^(k+1) pixels.
public bool occludedByHiz(CullUniforms u, ByteAddressBuffer hiz, float4x4 view, float3 centerWorld, float radius)
{
    const float3 c = mul(view, float4(centerWorld, 1.0)).xyz;
    const float dz = -c.z;
    const float zNear = u.proj.z;
    if (dz - radius <= zNear)
        return false;  // touches the near plane: assume visible

    // Screen rect of the sphere's view-space bounding box.
    float2 lo = float2(1e30, 1e30), hi = float2(-1e30, -1e30);
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        const float3 p = c + float3((i & 1) ? radius : -radius, (i & 2) ? radius : -radius, (i & 4) ? radius : -radius);
        const float2 ndc = float2(u.proj.x * p.x, u.proj.y * p.y) / -p.z;
        lo = min(lo, ndc);
        hi = max(hi, ndc);
    }
    const float2 screen = float2(u.params.z, u.params.w);
    // NDC y up -> pixel y down
    float2 pmin = float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * screen;
    float2 pmax = float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * screen;
    // One pixel of margin: the depth that built the HiZ was rasterized with subpixel TAA jitter.
    pmin = clamp(pmin - 1.0, 0.0, screen - 1.0);
    pmax = clamp(pmax + 1.0, 0.0, screen - 1.0);
    const float extent = max(pmax.x - pmin.x, pmax.y - pmin.y);

    const uint levelCount = u.params.y;
    uint level = uint(clamp(ceil(log2(max(extent, 1.0))) - 1.0, 0.0, float(levelCount - 1)));
    int2 t0 = int2(pmin) >> (level + 1);
    int2 t1 = int2(pmax) >> (level + 1);
    if ((t1.x - t0.x > 1 || t1.y - t0.y > 1) && level + 1 < levelCount)
    {
        level += 1;
        t0 = int2(pmin) >> (level + 1);
        t1 = int2(pmax) >> (level + 1);
    }
    const uint4 L = u.hizLevels[level];
    t1 = min(t1, int2(L.y - 1, L.z - 1));
    const float d00 = asfloat(hiz.Load((L.x + uint(t0.y) * L.y + uint(t0.x)) * 4));
    const float d10 = asfloat(hiz.Load((L.x + uint(t0.y) * L.y + uint(t1.x)) * 4));
    const float d01 = asfloat(hiz.Load((L.x + uint(t1.y) * L.y + uint(t0.x)) * 4));
    const float d11 = asfloat(hiz.Load((L.x + uint(t1.y) * L.y + uint(t1.x)) * 4));
    const float farthest = min(min(d00, d10), min(d01, d11));
    const float nearest = zNear / (dz - radius);  // reverse-Z depth of the sphere's closest point
    return nearest < farthest;
}

// Backface cone test (meshoptimizer convention), world space.
public bool coneBackfacing(CullUniforms u, InstanceGpu inst, float3 centerWorld, float radiusWorld, uint cone)
{
    const int4 s = int4(int(cone << 24) >> 24, int(cone << 16) >> 24, int(cone << 8) >> 24, int(cone) >> 24);
    const float cutoff = float(s.w) / 127.0;
    if (cutoff >= 1.0)
        return false;
    const float3 axis = normalize(transformDirection(inst, float3(s.xyz) / 127.0));
    const float3 v = centerWorld - u.cameraPos.xyz;
    return dot(v, axis) >= cutoff * length(v) + radiusWorld;
}
