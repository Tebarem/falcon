// Ray tracing through the scene's BVH (render/ray_tracing/scene_bvh.h, bvh_builder.h): a top
// level over the instances, a bottom level per mesh, two-wide nodes holding both children's
// boxes. Rays are traced in compute shaders; there is no hardware ray tracing under SDL_GPU.
//
// Callers run groups of kRtGroupSize threads and pass each thread's SV_GroupIndex (`lane`): the
// traversal stack lives in groupshared memory, interleaved (entry i of every thread together, so
// the threads of a wave hit different banks; a local array indexed at run time compiled
// differently on D3D12 and Vulkan once). The bottom level's stack continues above
// the top level's.

module ray_trace;

public static const uint kRtGroupSize = 64;
public static const uint kRtStackSize = 48;
public static const uint kRtLeafBit = 0x80000000u;
public static const uint kRtLeafShift = 3;
public static const uint kRtMirrored = 0x80000000u;  // instance record's instance bit: the world matrix flips handedness

groupshared uint rtStack[kRtGroupSize * kRtStackSize];

public struct RtHit
{
    public bool hit;
    public float t;          // along the ray as given (world units when the direction is a unit vector)
    public float2 bary;      // weights of the triangle's second and third vertices
    public bool backface;    // the triangle's clockwise side (front faces wind counter-clockwise)
    public uint record;      // the TLAS instance record
    public uint instance;    // the scene instance (scene_types loadInstance)
    public uint cluster;     // the scene cluster and its triangle (loadCluster, loadClusterTriangle)
    public uint triangle;
};

// Which hits count: alpha-masked surfaces' holes say no. Called for every triangle the ray meets
// nearer than the closest hit so far.
public interface IRtFilter
{
    bool accept(uint instance, uint cluster, uint triangle, float2 bary);
}

public struct RtOpaque : IRtFilter
{
    public bool accept(uint instance, uint cluster, uint triangle, float2 bary) { return true; }
};

struct RtNode
{
    float3 leftLo, leftHi, rightLo, rightHi;
    uint left, right;
};

RtNode loadNode(ByteAddressBuffer rt, uint at)
{
    const float4 a = asfloat(rt.Load4(at));
    const float4 b = asfloat(rt.Load4(at + 16));
    const float4 c = asfloat(rt.Load4(at + 32));
    const uint2 d = rt.Load2(at + 48);
    RtNode n;
    n.leftLo = a.xyz;
    n.leftHi = float3(a.w, b.x, b.y);
    n.rightLo = float3(b.z, b.w, c.x);
    n.rightHi = c.yzw;
    n.left = d.x;
    n.right = d.y;
    return n;
}

bool isLeaf(uint c) { return (c & kRtLeafBit) != 0; }
uint leafFirst(uint c) { return (c & ~kRtLeafBit) >> kRtLeafShift; }
uint leafCount(uint c) { return c & ((1u << kRtLeafShift) - 1u); }

// Slab test: whether the ray enters the box before tMax, and where.
bool slab(float3 lo, float3 hi, float3 origin, float3 inv, float tMax, out float tNear)
{
    const float3 t0 = (lo - origin) * inv, t1 = (hi - origin) * inv;
    const float3 n = min(t0, t1), f = max(t0, t1);
    tNear = max(max(n.x, n.y), max(n.z, 0.0));
    const float tFar = min(min(f.x, f.y), min(f.z, tMax));
    return tNear <= tFar;
}

float3 safeInverse(float3 d)
{
    return 1.0 / float3(abs(d.x) > 1e-20 ? d.x : (d.x < 0.0 ? -1e-20 : 1e-20),
                        abs(d.y) > 1e-20 ? d.y : (d.y < 0.0 ? -1e-20 : 1e-20),
                        abs(d.z) > 1e-20 ? d.z : (d.z < 0.0 ? -1e-20 : 1e-20));
}

struct RtHeader
{
    uint tlasAt, instancesAt, nodesAt, trianglesAt, instanceCount;
};

RtHeader loadHeader(ByteAddressBuffer rt)
{
    const uint4 a = rt.Load4(0);
    RtHeader h;
    h.tlasAt = a.x;
    h.instancesAt = a.y;
    h.nodesAt = a.z;
    h.trianglesAt = a.w;
    h.instanceCount = rt.Load(16);
    return h;
}

// Moller-Trumbore, edges inclusive (a ray down a shared edge hits one of its two triangles).
bool intersect(ByteAddressBuffer rt, uint at, float3 o, float3 d, float tMax, out float t, out float2 bary, out bool backface,
               out uint id)
{
    const float4 a = asfloat(rt.Load4(at));
    const float4 b = asfloat(rt.Load4(at + 16));
    const uint2 c = rt.Load2(at + 32);
    const float3 v0 = a.xyz, v1 = float3(a.w, b.x, b.y), v2 = float3(b.z, b.w, asfloat(c.x));
    id = c.y;
    const float3 e1 = v1 - v0, e2 = v2 - v0;
    const float3 p = cross(d, e2);
    const float det = dot(e1, p);
    t = 0.0;
    bary = 0.0;
    backface = det < 0.0;
    if (abs(det) < 1e-20)
        return false;
    const float inv = 1.0 / det;
    const float3 s = o - v0;
    const float u = dot(s, p) * inv;
    const float3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    t = dot(e2, q) * inv;
    bary = float2(u, v);
    return u >= 0.0 && v >= 0.0 && u + v <= 1.0 && t > 0.0 && t < tMax;
}

// The triangles of one leaf (child reference c) of an instance's BLAS: updates `best`, true when
// one of them is the new nearest.
bool leafTriangles<F : IRtFilter>(ByteAddressBuffer rt, RtHeader h, uint record, uint4 m, uint c, float3 o, float3 d, F filter,
                                  inout RtHit best)
{
    const uint first = leafFirst(c), count = leafCount(c);
    bool found = false;
    for (uint i = 0; i < count; ++i)
    {
        float t;
        float2 bary;
        bool back;
        uint id;
        const uint instance = m.z & 0x7fffffffu;
        if (intersect(rt, h.trianglesAt + (m.y + first + i) * 40, o, d, best.t, t, bary, back, id) &&
            filter.accept(instance, id >> 7, id & 127u, bary))
        {
            best.hit = true;
            best.t = t;
            best.bary = bary;
            best.backface = back != ((m.z & kRtMirrored) != 0);
            best.record = record;
            best.instance = instance;
            best.cluster = id >> 7;
            best.triangle = id & 127u;
            found = true;
        }
    }
    return found;
}

// One instance record: a subtree of its mesh's BLAS (or a leaf), the stack from `spBase`.
// Updates `best`; true once anything was hit when `any` (shadow rays stop there).
bool traceInstance<F : IRtFilter>(ByteAddressBuffer rt, RtHeader h, uint record, float3 origin, float3 dir, uint lane, uint spBase,
                                  bool any, F filter, inout RtHit best)
{
    const uint at = h.instancesAt + record * 64;
    const float4 r0 = asfloat(rt.Load4(at)), r1 = asfloat(rt.Load4(at + 16)), r2 = asfloat(rt.Load4(at + 32));
    const uint4 m = rt.Load4(at + 48);  // node base, triangle base, instance (bit 31: mirrored), start
    const float4 oh = float4(origin, 1.0);
    const float3 o = float3(dot(r0, oh), dot(r1, oh), dot(r2, oh));
    const float3 d = float3(dot(r0.xyz, dir), dot(r1.xyz, dir), dot(r2.xyz, dir));
    if (isLeaf(m.w))
        return leafTriangles(rt, h, record, m, m.w, o, d, filter, best);
    const float3 inv = safeInverse(d);
    uint sp = spBase;
    uint node = m.w;
    bool found = false;
    for (uint step = 0; step < 65536u; ++step)
    {
        const RtNode n = loadNode(rt, h.nodesAt + (m.x + node) * 64);
        float tl, tr;
        bool hl = slab(n.leftLo, n.leftHi, o, inv, best.t, tl);
        bool hr = slab(n.rightLo, n.rightHi, o, inv, best.t, tr);
        // Leaves at once.
        if (hl && isLeaf(n.left))
        {
            found = leafTriangles(rt, h, record, m, n.left, o, d, filter, best) || found;
            hl = false;
        }
        if (hr && isLeaf(n.right))
        {
            found = leafTriangles(rt, h, record, m, n.right, o, d, filter, best) || found;
            hr = false;
        }
        if (any && found)
            return true;
        if (hl && hr)
        {
            const bool leftFirst = tl <= tr;
            if (sp < kRtStackSize)
                rtStack[sp++ * kRtGroupSize + lane] = leftFirst ? n.right : n.left;
            node = leftFirst ? n.left : n.right;
        }
        else if (hl)
            node = n.left;
        else if (hr)
            node = n.right;
        else
        {
            if (sp == spBase)
                break;
            node = rtStack[--sp * kRtGroupSize + lane];
        }
    }
    return found;
}

RtHit traceScene<F : IRtFilter>(ByteAddressBuffer rt, float3 origin, float3 dir, float tMax, uint lane, bool any, F filter)
{
    RtHit best;
    best.hit = false;
    best.t = tMax;
    best.bary = 0.0;
    best.backface = false;
    best.record = 0;
    best.instance = 0;
    best.cluster = 0;
    best.triangle = 0;
    const RtHeader h = loadHeader(rt);
    if (h.instanceCount == 0)
        return best;
    const float3 inv = safeInverse(dir);
    uint sp = 0;
    uint node = 0;
    for (uint step = 0; step < 65536u; ++step)
    {
        const RtNode n = loadNode(rt, h.tlasAt + node * 64);
        float tl, tr;
        bool hl = slab(n.leftLo, n.leftHi, origin, inv, best.t, tl);
        bool hr = slab(n.rightLo, n.rightHi, origin, inv, best.t, tr);
        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            const uint c = side == 0 ? n.left : n.right;
            if (!(side == 0 ? hl : hr) || !isLeaf(c))
                continue;
            const uint first = leafFirst(c), count = leafCount(c);
            for (uint i = 0; i < count; ++i)
                if (traceInstance(rt, h, first + i, origin, dir, lane, sp, any, filter, best) && any)
                    return best;
            if (side == 0)
                hl = false;
            else
                hr = false;
        }
        if (hl && hr)
        {
            const bool leftFirst = tl <= tr;
            if (sp < kRtStackSize)
                rtStack[sp++ * kRtGroupSize + lane] = leftFirst ? n.right : n.left;
            node = leftFirst ? n.left : n.right;
        }
        else if (hl)
            node = n.left;
        else if (hr)
            node = n.right;
        else
        {
            if (sp == 0)
                break;
            node = rtStack[--sp * kRtGroupSize + lane];
        }
    }
    return best;
}

// The nearest surface along origin + t * dir, t in (0, tMax).
public RtHit rtTraceClosest<F : IRtFilter>(ByteAddressBuffer rt, float3 origin, float3 dir, float tMax, uint lane, F filter)
{
    return traceScene(rt, origin, dir, tMax, lane, false, filter);
}

// Whether anything is in the way before tMax (shadow rays).
public bool rtOccluded<F : IRtFilter>(ByteAddressBuffer rt, float3 origin, float3 dir, float tMax, uint lane, F filter)
{
    return traceScene(rt, origin, dir, tMax, lane, true, filter).hit;
}
