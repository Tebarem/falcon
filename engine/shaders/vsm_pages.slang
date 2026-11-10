// Virtual shadow map page management and caster culling (one compute pass per step):
//   cs_clear      reset request/dirty masks and counters
//   (vsm_mark  depth buffer -> requested pages)
//   cs_touch      requested pages that are still valid: mark used this frame
//   cs_invalidate requested valid pages under a moved caster's swept sphere: re-render in place
//   cs_free       physical pages not used this or last frame -> free list
//   cs_alloc      requested pages without a valid physical page -> allocate (budgeted), mark dirty
//   cs_args       dispatch/draw args for clearing and culling
//   cs_cull       per cluster and dirty level / local face mip: DAG cut at that texel size,
//                 emit (cluster, page) records for every dirty page it overlaps
//   cs_args2      expansion + draw args
//   cs_expand     records -> index buffer (128-triangle slots), without the triangles that miss
//                 the record's page or every texel centre in it; those crossing the page's edge
//                 go to a list of their own (discarded outside it per fragment, or clipped)
//   cs_args3      draw args of the crossing lists

import scene_types;
import culling;
import vsm;
import lighting;
import skinning;

[[vk::binding(0, 0)]] ByteAddressBuffer ro0 : register(t0, space0);   // cull: shadow chunks; expand: records; invalidate: spheres
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer lights : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer skin : register(t3, space0);  // cull: skinned instances
[[vk::binding(0, 1)]] RWByteAddressBuffer state : register(u0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer rw1 : register(u1, space1);  // args / records / indices
[[vk::binding(2, 1)]] RWByteAddressBuffer crossing : register(u2, space1);  // expand: triangles crossing their page's edge
[[vk::binding(0, 2)]] ConstantBuffer<VsmParams> vsm : register(b0, space2);

// args layout (bytes)
static const uint kArgClear = 0;    // non-indexed draw: 6 vertices x dirty pages
static const uint kArgCull = 16;    // dispatch
static const uint kArgExpand = 32;  // dispatch
static const uint kArgDraw = 48;    // indexed draw (static layer)
static const uint kArgDrawMoving = 80;  // indexed draw (moving casters)
static const uint kArgCrossing = 112;  // indexed draws of the triangles crossing their page's edge: static layer small,
                                       // clipped (+32), moving casters small (+64), clipped (+96)

uint counterAddr(uint c) { return (vsm.layout2.x + c) * 4; }
uint entries() { return vsm.config.y; }

// Records written this frame: static layer (from the front), moving casters (from the back).
void recordCounts(out uint s, out uint d)
{
    s = min(state.Load(counterAddr(kVsmStaticRecords)), vsm.config2.y);
    d = min(state.Load(counterAddr(kVsmDynamicRecords)), vsm.config2.y - s);
}

bool requested(uint e) { return (state.Load((vsm.layout.z + (e >> 5)) * 4) & (1u << (e & 31))) != 0; }
bool dirty(uint e) { return (state.Load((vsm.layout.w + (e >> 5)) * 4) & (1u << (e & 31))) != 0; }
bool staticDirty(uint e) { return (state.Load((vsm.layout2.w + (e >> 5)) * 4) & (1u << (e & 31))) != 0; }
bool splitLayers() { return vsm.config2.z != 0; }


bool validHit(uint e)
{
    const uint st = state.Load(e * 4);
    // config2.w: 1 the sun's pages are stale (it moved), 2 every page (uncached, or a full rebuild)
    if ((st & kAllocated) == 0 || vsm.config2.w > 1 || (vsm.config2.w == 1 && e < vsm.local.y))
        return false;
    const uint phys = st & 0xffff;
    return state.Load((entries() + e) * 4) == entryTag(vsm, e) && state.Load((vsm.layout.x + phys) * 4) == e;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_clear(uint3 tid : SV_DispatchThreadID)
{
    const uint maskWords = (entries() + 31) / 32;
    if (tid.x < maskWords)
    {
        state.Store((vsm.layout.z + tid.x) * 4, 0);
        state.Store((vsm.layout.w + tid.x) * 4, 0);
        state.Store((vsm.layout2.w + tid.x) * 4, 0);
    }
    if (tid.x < kVsmCounterCount)
        state.Store(counterAddr(tid.x), 0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_touch(uint3 tid : SV_DispatchThreadID)
{
    const uint e = tid.x;
    if (e >= entries() || !requested(e))
        return;
    state.InterlockedAdd(counterAddr(kVsmRequested), 1);
    if (e >= vsm.local.y)
        state.InterlockedAdd(counterAddr(kVsmSlotRequested + (e - vsm.local.y) / (6 * kEntriesPerFace)), 1);
    if (validHit(e))
        state.Store((vsm.layout.y + (state.Load(e * 4) & 0xffff)) * 4, vsm.config.z);
}

// Queue an allocated page for rendering: the moving casters only, over its static layer, or
// (whole) both layers. Its level / face goes into the masks the cull reads for each kind of caster.
void markDirty(uint e, bool whole)
{
    const uint bit = 1u << (e & 31);
    const uint level = e < vsm.local.y ? e / kEntriesPerLevel : 0;
    const uint face = e < vsm.local.y ? 0 : (e - vsm.local.y) / kEntriesPerFace;
    uint old;
    if (whole)
    {
        state.InterlockedOr((vsm.layout2.w + (e >> 5)) * 4, bit, old);
        if ((old & bit) == 0)
        {
            if (e < vsm.local.y)
                state.InterlockedOr(counterAddr(kVsmStaticLevelMask), 1u << level);
            else
                state.InterlockedOr(counterAddr(kVsmStaticFaceMask + (face >> 5)), 1u << (face & 31));
        }
    }
    state.InterlockedOr((vsm.layout.w + (e >> 5)) * 4, bit, old);
    if ((old & bit) != 0)
        return;  // already queued
    if (e < vsm.local.y)
        state.InterlockedOr(counterAddr(kVsmLevelMask), 1u << level);
    else
        state.InterlockedOr(counterAddr(kVsmFaceMask + (face >> 5)), 1u << (face & 31));
    uint d;
    state.InterlockedAdd(counterAddr(kVsmDirty), 1, d);
    state.Store((vsm.layout2.z + d) * 4, state.Load(e * 4) & 0xffff);  // the clear and copy passes only need the physical page
}

// Queue a cached page for re-rendering in its current physical page (no allocation, no budget).
// Cached pages not requested this frame are dropped instead: re-rendered when next requested.
void invalidateEntry(uint e, bool whole)
{
    if (!validHit(e))
        return;
    if (!requested(e))
    {
        state.Store((entries() + e) * 4, 0xffffffffu);  // tag mismatch: no longer a valid hit
        return;
    }
    markDirty(e, whole);
}

// One thread per local page table entry: entries of the slots flagged in local.w (their light
// moved, changed shape or now belongs to another light) are re-rendered or dropped.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_invalidate_slots(uint3 tid : SV_DispatchThreadID)
{
    const uint slot = tid.x / (6 * kEntriesPerFace);
    if (slot >= 32 || (vsm.local.w & (1u << slot)) == 0)
        return;
    const uint e = vsm.local.y + tid.x;
    if (e < entries())
        invalidateEntry(e, true);
}

// One thread per (sphere, view): a sun clipmap level or a local light face (all its mips).
// The sphere bounds a body's previous and new pose, so both its old and new shadow go. A
// negative radius: a moving caster that was already moving (its pages' static layer stays).
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_invalidate(uint3 tid : SV_DispatchThreadID)
{
    const uint levels = vsm.config.x;
    const uint views = levels + vsm.local.x * 6;
    const uint sphereIndex = tid.x / views, view = tid.x % views;
    if (sphereIndex >= vsm.local.z)
        return;
    const float4 sphere = ro0.Load<float4>(sphereIndex * 16);
    const float3 center = sphere.xyz;
    const float radius = abs(sphere.w);
    const bool whole = sphere.w >= 0.0 || !splitLayers();
    if (view < levels)
    {
        // Sun: orthographic, so the light-space footprint is the sphere's xy disc, at any depth.
        const uint k = view;
        const float2 xy = lightXY(vsm, center);
        const float ps = pageWorldSize(vsm, k);
        const int2 lo = max(int2(floor((xy - radius) / ps)), vsm.centers[k].xy - int(kPagesPerSide / 2));
        const int2 hi = min(int2(floor((xy + radius) / ps)), vsm.centers[k].xy + int(kPagesPerSide / 2) - 1);
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x)
                invalidateEntry(pageEntry(k, int2(x, y)), whole);
        return;
    }
    const uint s = (view - levels) / 6, f = (view - levels) % 6;
    const LightGpu l = loadLight(lights, slotLight(vsm, s));
    if (f >= faceCount(l))
        return;
    const float3 d = center - l.positionRange.xyz;
    if (length(d) - radius > l.positionRange.w)
        return;
    const float zNear = localNear(vsm, l);
    const ShadowFace face = shadowFace(l, f);
    const float z = dot(d, face.forward);
    if (z + radius <= zNear)
        return;
    float2 uvMin = 0.0, uvMax = 1.0;
    if (z - radius > zNear)
    {
        const float2 c = float2(dot(d, face.right), dot(d, face.up)) / (z * face.tanHalf);
        const float r = radius / ((z - radius) * face.tanHalf);
        const float2 lo = c - r, hi = c + r;
        if (any(hi < -1.0) || any(lo > 1.0))
            return;
        uvMin = saturate(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5));
        uvMax = saturate(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5));
    }
    for (uint m = 0; m < kFaceMips; ++m)
    {
        const uint pages = facePages(m);
        const uint2 p0 = min(uint2(uvMin * float(pages)), pages - 1);
        const uint2 p1 = min(uint2(uvMax * float(pages)), pages - 1);
        for (uint y = p0.y; y <= p1.y; ++y)
            for (uint x = p0.x; x <= p1.x; ++x)
                invalidateEntry(localEntry(vsm, s, f, m, uint2(x, y)), whole);
    }
}

// Free list: pages never used, or not used in the last frame (their cached page stays valid
// until an allocation takes the page). The list is in physical page order, compacted with a
// prefix sum in one group rather than appended atomically: allocations take its front, so the
// order decides which cached pages get evicted, and with an atomic append that depended on
// thread scheduling. Shading falls back on cached pages wherever a pixel's own page was not
// requested (the mark pass samples every other pixel), so a scheduling-dependent eviction
// made the image differ from run to run.
static const uint kFreeThreads = 1024;
static const uint kFreePagesPerThread = kPhysicalPages / kFreeThreads;
groupshared uint freePrefix[kFreeThreads];

[shader("compute")]
[numthreads(kFreeThreads, 1, 1)]
void cs_free(uint3 tid : SV_GroupThreadID)
{
    const uint t = tid.x;
    uint mask = 0, count = 0;
    for (uint i = 0; i < kFreePagesPerThread; ++i)
    {
        const uint p = t * kFreePagesPerThread + i;
        const uint owner = state.Load((vsm.layout.x + p) * 4);
        const uint used = state.Load((vsm.layout.y + p) * 4);
        if (owner == 0xffffffffu || used + 1 < vsm.config.z)
        {
            mask |= 1u << i;
            ++count;
        }
    }
    freePrefix[t] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < kFreeThreads; offset <<= 1)  // inclusive scan (Hillis-Steele)
    {
        const uint add = t >= offset ? freePrefix[t - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        freePrefix[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint slot = freePrefix[t] - count;
    for (uint i = 0; i < kFreePagesPerThread; ++i)
        if ((mask & (1u << i)) != 0)
            state.Store((vsm.layout2.y + slot++) * 4, t * kFreePagesPerThread + i);
    if (t == kFreeThreads - 1)
        state.Store(counterAddr(kVsmFree), freePrefix[t]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_alloc(uint3 tid : SV_DispatchThreadID)
{
    const uint e = tid.x;
    if (e >= entries() || !requested(e) || validHit(e))
        return;
    uint slot;
    state.InterlockedAdd(counterAddr(kVsmAllocs), 1, slot);
    const uint freeCount = state.Load(counterAddr(kVsmFree));
    if (slot >= freeCount || slot >= vsm.config.w)
    {
        state.Store(e * 4, 0);  // no page this frame: sampling falls back to a coarser level
        return;
    }
    const uint phys = state.Load((vsm.layout2.y + slot) * 4);
    // Unlink the previous owner, but only if it still points here (it may have been
    // reallocated elsewhere by another thread of this pass).
    const uint prev = state.Load((vsm.layout.x + phys) * 4);
    if (prev != 0xffffffffu && prev != e)
    {
        uint ignored;
        state.InterlockedCompareExchange(prev * 4, kAllocated | phys, 0, ignored);
    }
    state.Store((vsm.layout.x + phys) * 4, e);
    state.Store((vsm.layout.y + phys) * 4, vsm.config.z);
    state.Store(e * 4, kAllocated | phys);
    state.Store((entries() + e) * 4, entryTag(vsm, e));
    markDirty(e, true);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args(uint3 tid : SV_DispatchThreadID)
{
    const uint dirtyPages = state.Load(counterAddr(kVsmDirty));
    rw1.Store4(kArgClear, uint4(6, dirtyPages, 0, 0));
    rw1.Store3(kArgCull, uint3(dirtyPages > 0 ? vsm.config2.x : 0, 1, 1));
}

// Static layer records fill the buffer from the front, moving casters' from the back (each kind
// is one draw, into its own atlas); the total counter keeps them from meeting.
void emitRecord(uint instance, uint cluster, uint e, bool moving)
{
    uint n;
    state.InterlockedAdd(counterAddr(kVsmRecords), 1, n);
    if (n >= vsm.config2.y)
    {
        state.Store(e * 4, 0);  // incomplete page: drop it so it is re-rendered later
        state.InterlockedOr(counterAddr(kVsmOverflow), 1);
        return;
    }
    uint slot;
    state.InterlockedAdd(counterAddr(moving ? kVsmDynamicRecords : kVsmStaticRecords), 1, slot);
    rw1.Store4((moving ? vsm.config2.y - 1 - slot : slot) * 16, uint4(instance, cluster, e, 0));
}

// The pages a caster draws into: a moving one, every page being rendered; a static one, only
// those whose static layer is (without moving casters, both are every page being rendered).
bool wantsPage(uint e, bool moving) { return moving ? dirty(e) : staticDirty(e); }

// Local shadow casters: every shadowed light whose range reaches the cluster, every face with
// dirty pages that the cluster's sphere projects into, at each mip whose texel size matches
// the cluster's DAG error at that distance.
void emitLocalRecords(uint instance, uint clusterIndex, ClusterGpu cl, float3 center, float radius, float scale, bool scatter,
                      bool moving)
{
    const uint faceMask = moving ? kVsmFaceMask : kVsmStaticFaceMask;
    for (uint s = 0; s < vsm.local.x; ++s)
    {
        const LightGpu l = loadLight(lights, slotLight(vsm, s));
        const float3 d = center - l.positionRange.xyz;
        if (length(d) - radius > l.positionRange.w)
            continue;
        const float zNear = localNear(vsm, l);
        for (uint f = 0; f < faceCount(l); ++f)
        {
            const uint faceIndex = s * 6 + f;
            if ((state.Load(counterAddr(faceMask + (faceIndex >> 5))) & (1u << (faceIndex & 31))) == 0)
                continue;
            const ShadowFace face = shadowFace(l, f);
            const float z = dot(d, face.forward);
            if (z + radius <= zNear)
                continue;
            float2 uvMin = 0.0, uvMax = 1.0;
            if (z - radius > zNear)
            {
                const float2 c = float2(dot(d, face.right), dot(d, face.up)) / (z * face.tanHalf);
                const float r = radius / ((z - radius) * face.tanHalf);
                const float2 lo = c - r, hi = c + r;
                if (any(hi < -1.0) || any(lo > 1.0))
                    continue;
                uvMin = saturate(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5));
                uvMax = saturate(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5));
            }
            const float texel0 = 2.0 * max(z, zNear) * face.tanHalf / float(kLevelTexels) * vsm.lod.x;
            for (uint m = 0; m < kFaceMips; ++m)
            {
                const float texel = texel0 * float(1u << m);
                if (cl.selfError * scale > texel || cl.parentError * scale <= texel)
                    continue;
                if (scatter && radius < texel * vsm.lod.z)
                    continue;
                const uint pages = facePages(m);
                const uint2 p0 = min(uint2(uvMin * float(pages)), pages - 1);
                const uint2 p1 = min(uint2(uvMax * float(pages)), pages - 1);
                for (uint y = p0.y; y <= p1.y; ++y)
                    for (uint x = p0.x; x <= p1.x; ++x)
                    {
                        const uint e = localEntry(vsm, s, f, m, uint2(x, y));
                        if (wantsPage(e, moving))
                            emitRecord(instance, clusterIndex, e, moving);
                    }
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 chunk = ro0.Load2(group.x * 8);  // (instance, first cluster): every instance, no frustum
    const InstanceGpu inst = loadInstance(scene, chunk.x);
    if ((inst.flags & (kInstanceHidden | kInstanceNoShadow)) != 0)
        return;
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint clusterIndex = chunk.y + thread.x;
    if (clusterIndex >= mesh.clusterOffset + mesh.clusterCount)
        return;
    const ClusterGpu cl = loadCluster(scene, clusterIndex);
    const float scale = maxScale(inst);
    const float4 sphere = isSkinned(inst) ? skinnedClusterSphere(skin, inst, clusterIndex) : cl.cullSphere;
    const float3 center = transformPoint(inst, sphere.xyz);
    const float radius = sphere.w * scale;
    const float2 xy = lightXY(vsm, center);
    const bool scatter = (inst.flags & kInstanceScatter) != 0;
    const bool moving = splitLayers() && (inst.flags & kInstanceDynamic) != 0;
    emitLocalRecords(chunk.x, clusterIndex, cl, center, radius, scale, scatter, moving);
    uint levels = state.Load(counterAddr(moving ? kVsmLevelMask : kVsmStaticLevelMask));
    while (levels != 0)
    {
        const uint k = firstbitlow(levels);
        levels &= levels - 1;
        // Per-level DAG cut: the coarsest clusters whose world-space error fits one shadow texel.
        const float texel = levelWorldSize(vsm, k) / float(kAtlasTexels) * vsm.lod.x;
        if (cl.selfError * scale > texel || cl.parentError * scale <= texel)
            continue;
        if (scatter && radius < texel * vsm.lod.z)  // (a blade of grass on a coarse level: a texel or two)
            continue;
        const float ps = pageWorldSize(vsm, k);
        const int2 lo = max(int2(floor((xy - radius) / ps)), vsm.centers[k].xy - int(kPagesPerSide / 2));
        const int2 hi = min(int2(floor((xy + radius) / ps)), vsm.centers[k].xy + int(kPagesPerSide / 2) - 1);
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const uint e = pageEntry(k, int2(x, y));
                if (wantsPage(e, moving))
                    emitRecord(chunk.x, clusterIndex, e, moving);
            }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args2(uint3 tid : SV_DispatchThreadID)
{
    uint s, d;
    recordCounts(s, d);
    rw1.Store3(kArgExpand, uint3(s + d, 1, 1));
    rw1.Store4(kArgDraw, uint4(s * kIndicesPerCluster, 1, 0, 0));
    rw1.Store(kArgDraw + 16, 0);
    rw1.Store4(kArgDrawMoving, uint4(d * kIndicesPerCluster, 1, (vsm.config2.y - d) * kIndicesPerCluster, 0));
    rw1.Store(kArgDrawMoving + 16, 0);
}

// A world point in a page's clip space, as vs_shadow places it: xy the page's uv times w, z = w
// (the sun's is orthographic: w = 1). Inside the page: 0 <= xy <= w.
float3 pageClip(float3 wp, uint e)
{
    if (e >= vsm.local.y)
    {
        uint faceIndex, mip;
        uint2 page;
        decodeLocalEntry(vsm, e, faceIndex, mip, page);
        const LightGpu l = loadLight(lights, slotLight(vsm, faceIndex / 6));
        const ShadowFace face = shadowFace(l, faceIndex % 6);
        const float3 d = wp - l.positionRange.xyz;
        const float w = dot(d, face.forward);
        const float2 ndc = float2(dot(d, face.right), dot(d, face.up)) / face.tanHalf;
        return float3(float2(ndc.x * 0.5 + 0.5 * w, 0.5 * w - ndc.y * 0.5) * float(facePages(mip)) - float2(page) * w, w);
    }
    const uint level = e / kEntriesPerLevel;
    return float3(lightXY(vsm, wp) / pageWorldSize(vsm, level) - float2(slotWorldPage(vsm, e)), 1.0);
}

groupshared float3 expandClip[128];

// Each record draws its cluster into one page. Most of a cluster spread over several pages lies
// outside any one of them, and many of a detailed mesh's triangles fall between the texel centres
// of a coarse page: neither writes a texel, but each cost the vertex work and triangle setup of
// one that does (a played model's moving parts: about a million triangles a frame). Those are
// left degenerate here. The tests keep a margin (a hundredth of a texel, past the raster's
// subpixel snapping), so what is drawn is exactly what it was.
static const float kCullMargin = 0.01 / float(kVsmPageTexels);
// The rest must stay inside its page. Clip distances do that, but clipping is slow: about 50k
// triangles crossing page edges were most of the shadows' cost. So only triangles reaching past
// the edge by more than kInsideMargin need confining (less: they reach no texel centre of the
// next page, half a texel out); small ones (up to kSmallCrossing pages across) are drawn whole
// and their fragments outside the page discarded (at most a few pages of fill each); only the
// large ones are clipped.
static const float kInsideMargin = 0.25 / float(kVsmPageTexels);
static const float kSmallCrossing = 2.0;
static const uint kMaxCrossing = 1u << 20;  // triangles in the crossing list (half each kind)

[shader("compute")]
[numthreads(128, 1, 1)]
void cs_expand(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint s, d;
    recordCounts(s, d);
    const uint r = group.x < s ? group.x : vsm.config2.y - d + (group.x - s);  // the static records, then the moving ones
    const uint4 record = ro0.Load4(r * 16);
    const InstanceGpu inst = loadInstance(scene, record.x);
    const ClusterGpu cl = loadCluster(scene, record.y);
    if (thread.x < clusterVertexCount(cl))
    {
        const DecodedVertex v = loadInstanceVertex(scene, skin, inst, loadMesh(scene, inst.mesh), loadClusterVertex(scene, cl, thread.x));
        expandClip[thread.x] = pageClip(transformPoint(inst, v.position), record.z);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint key = r << 7;
    uint3 tri = uint3(key, key, key);
    if (thread.x < clusterTriangleCount(cl))
    {
        const uint packed = loadClusterTriangle(scene, cl, thread.x);
        const uint3 local = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
        const float3 a = expandClip[local.x], b = expandClip[local.y], c = expandClip[local.z];
        // Outside one of the page's four edges (all three corners past it).
        bool keep = !any(a.xy < -kCullMargin * abs(a.z) && b.xy < -kCullMargin * abs(b.z) && c.xy < -kCullMargin * abs(c.z)) &&
                    !any(a.xy > a.z * (1.0 + kCullMargin) && b.xy > b.z * (1.0 + kCullMargin) && c.xy > c.z * (1.0 + kCullMargin));
        if (keep && record.z < vsm.local.y)
        {
            // Sun (w = 1): no texel centre between its extremes, in x or in y.
            const float2 lo = (min(min(a.xy, b.xy), c.xy) - kCullMargin) * float(kVsmPageTexels) - 0.5;
            const float2 hi = (max(max(a.xy, b.xy), c.xy) + kCullMargin) * float(kVsmPageTexels) - 0.5;
            keep = all(ceil(lo) <= floor(hi));
        }
        const bool inside = a.z > 0.0 && b.z > 0.0 && c.z > 0.0 &&
                            all(a.xy >= -kInsideMargin * a.z && a.xy <= a.z * (1.0 + kInsideMargin) &&
                                b.xy >= -kInsideMargin * b.z && b.xy <= b.z * (1.0 + kInsideMargin) &&
                                c.xy >= -kInsideMargin * c.z && c.xy <= c.z * (1.0 + kInsideMargin));
        if (keep && inside)
            tri = key | local;
        else if (keep)
        {
            const uint half = group.x >= s ? 1 : 0;  // moving casters' half
            const float2 extent = max(max(a.xy / a.z, b.xy / b.z), c.xy / c.z) - min(min(a.xy / a.z, b.xy / b.z), c.xy / c.z);
            const bool small = a.z > 0.0 && b.z > 0.0 && c.z > 0.0 && all(extent <= kSmallCrossing);
            const uint h = kMaxCrossing / 2;
            uint n;
            state.InterlockedAdd(counterAddr(kVsmCrossing + half), 1, n);
            if (n < h)
            {
                uint slot;
                state.InterlockedAdd(counterAddr((small ? kVsmCrossingSmall : kVsmCrossingClipped) + half), 1, slot);
                crossing.Store3((half * h + (small ? slot : h - 1 - slot)) * 12, key | local);
            }
            else
            {
                state.Store(record.z * 4, 0);  // incomplete page: drop it so it is re-rendered later
                state.InterlockedOr(counterAddr(kVsmOverflow), 1);
            }
        }
    }
    rw1.Store3((r * kIndicesPerCluster + thread.x * 3) * 4, tri);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args3(uint3 tid : SV_DispatchThreadID)
{
    const uint h = kMaxCrossing / 2;
    for (uint half = 0; half < 2; ++half)
    {
        const uint small = min(state.Load(counterAddr(kVsmCrossingSmall + half)), h);
        const uint clipped = min(state.Load(counterAddr(kVsmCrossingClipped + half)), h - small);
        const uint at = kArgCrossing + half * 64;
        rw1.Store4(at, uint4(small * 3, 1, half * h * 3, 0));
        rw1.Store(at + 16, 0);
        rw1.Store4(at + 32, uint4(clipped * 3, 1, (half * h + h - clipped) * 3, 0));
        rw1.Store(at + 48, 0);
    }
}
