// Virtual shadow maps, all sharing one page table and one physical depth atlas.
//
// Directional (sun): a clipmap of square levels around the camera, each a toroidal 32x32
// window of 128x128-texel pages (4096^2 virtual texels per level). Level k covers
// worldSize0 * 2^k. Entries [0, levels * 1024).
//
// Local lights: up to kMaxShadowSlots shadow-casting lights, each with 6 faces (point: cube
// faces; spot: face 0 only). A face is a 4096^2 perspective virtual texture with 6 mips of
// 128^2 pages (32^2 .. 1 pages), 1365 entries per face, appended after the directional ones.
//
// Pages are allocated on demand in an 8192^2 physical atlas (64x64 pages) and stay cached
// while the scene and lights are static.
//
// Moving casters (kInstanceDynamic, once anything moves: config2.z): each page also keeps a
// static layer, the casters that did not move lately, in a second atlas at the same physical
// page. A page the movers alone made stale is restored from it and only they are drawn again;
// a new page, or one a static change made stale ("static dirty"), renders both layers.
//
// State buffer (uint words):
//   [state E] [tag E] [owner P] [lastUsed P] [request E/32] [dirty E/32] [counters 32] [free P] [dirty list P]
//   [static dirty E/32]
// state: bit 31 allocated | physical page;  tag: directional world page x | y << 16 (local: 0)

module vsm;

import lighting;

public static const uint kPagesPerSide = 32;  // per directional level window / per local face mip 0
public static const uint kVsmPageTexels = 128;
public static const uint kLevelTexels = kPagesPerSide * kVsmPageTexels;  // 4096 virtual texels per level / face
public static const uint kEntriesPerLevel = kPagesPerSide * kPagesPerSide;
public static const uint kAtlasPagesPerSide = 64;
public static const uint kAtlasTexels = kAtlasPagesPerSide * kVsmPageTexels;  // physical: 8192
public static const uint kPhysicalPages = kAtlasPagesPerSide * kAtlasPagesPerSide;
public static const uint kAllocated = 0x80000000u;

public static const uint kMaxShadowSlots = 32;
public static const uint kFaceMips = 6;
public static const uint kEntriesPerFace = 1365;  // 32^2 + 16^2 + 8^2 + 4^2 + 2^2 + 1

// counters (offsets into the counter block)
public static const uint kVsmAllocs = 0;
public static const uint kVsmFree = 1;
public static const uint kVsmDirty = 2;
public static const uint kVsmRecords = 3;
public static const uint kVsmLevelMask = 4;
public static const uint kVsmOverflow = 5;
public static const uint kVsmRequested = 6;
public static const uint kVsmStaticLevelMask = 7;  // levels with static dirty pages
public static const uint kVsmStaticRecords = 8;    // records of the static layer (from the front)
public static const uint kVsmDynamicRecords = 9;   // records of moving casters (from the back)
// Triangles crossing their page's edge, in the crossing list: its first half the static layer's,
// its second the moving casters'; in each, the small ones (discarded outside the page by the
// fragment shader) from the front, the others (clipped) from the back.
public static const uint kVsmCrossing = 10;         // per half (2): triangles in it
public static const uint kVsmCrossingSmall = 12;    // per half (2): small ones
public static const uint kVsmCrossingClipped = 14;  // per half (2): clipped ones
public static const uint kVsmFaceMask = 16;  // 6 words: bit per (slot * 6 + face) with dirty pages
public static const uint kVsmStaticFaceMask = 24;  // 6 words: faces with static dirty pages
public static const uint kVsmSlotRequested = 32;   // 32 words: each shadow slot's requested pages (the budget's costs)
public static const uint kVsmCounterCount = 64;

public struct VsmParams
{
    public float4 lightX;      // light-space basis (world): x, y on the shadow plane, z away from the sun
    public float4 lightY;
    public float4 lightZ;      // w: depth range start (zMin)
    public float4 world;       // x: level 0 world size, y: 1 / depth range, z: pixel footprint per unit distance, w: level bias
    public int4 centers[16];   // per level: window center in world pages (xy)
    public uint4 config;       // levels, entries, frame, allocation budget
    public uint4 layout;       // word offsets: owner, lastUsed, request, dirty
    public uint4 layout2;      // word offsets: counters, free list, dirty list, static dirty
    public uint4 config2;      // shadow chunk count, max records, moving casters on their own layer, invalidate all
    public float4 lod;         // x: DAG error threshold in shadow texels, y: local near plane (fraction of range), z: smallest scatter caster (texels)
    public uint4 local;        // shadowed light count, first local entry, invalidation spheres, slots to invalidate (bits)
    public uint4 slotLights[8];  // light index of each shadow slot (4 per uint4)
};

// ---- directional

public float levelWorldSize(VsmParams v, uint level) { return v.world.x * float(1u << level); }
public float pageWorldSize(VsmParams v, uint level) { return levelWorldSize(v, level) / float(kPagesPerSide); }

public float2 lightXY(VsmParams v, float3 p) { return float2(dot(p, v.lightX.xyz), dot(p, v.lightY.xyz)); }

// Reverse depth in [0, 1]: 1 nearest the sun; the atlas clears to 0.
public float lightDepth(VsmParams v, float3 p) { return 1.0 - (dot(p, v.lightZ.xyz) - v.lightZ.w) * v.world.y; }

public uint pageEntry(uint level, int2 page) { return level * kEntriesPerLevel + uint(page.y & 31) * kPagesPerSide + uint(page.x & 31); }
public uint pageTag(int2 page) { return (uint(page.x) & 0xffff) | (uint(page.y) & 0xffff) << 16; }

public bool inWindow(VsmParams v, uint level, int2 page)
{
    const int2 d = page - v.centers[level].xy;
    return all(d >= -int(kPagesPerSide / 2)) && all(d < int(kPagesPerSide / 2));
}

// Continuous clipmap level at this distance: log2 of the pixel footprint in level-0 texels.
public float levelCoord(VsmParams v, float distance)
{
    const float footprint = distance * v.world.z;
    const float texel0 = v.world.x / float(kLevelTexels);
    return log2(max(footprint / texel0, 1e-6)) + v.world.w;
}

// Finest level whose texels are no larger than the pixel footprint at this distance.
public uint desiredLevel(VsmParams v, float distance)
{
    return uint(clamp(ceil(levelCoord(v, distance)), 0.0, float(v.config.x - 1)));
}

// How much of the next level to blend into level k (0..1) at continuous level coord and
// light-space position xy: over the last 30% of an octave before the next level takes over,
// and over the outer pages of the level's window. Without it the switch between levels is a hard line that slides along with
// the camera, and shadow edges change sharpness across it.
// The mark pass requests the next level's pages with slack, so they stay resident for every
// pixel in the band: a wider band (kBlendSlack levels, 4x that in window pages) and the pages
// around the point (kBlendMarginTexels of the next level, for the normal offset and the filter
// footprint).
public static const float kLevelBlendBand = 0.3;
public static const float kBlendSlack = 0.05;
public static const float kBlendMarginTexels = 6.0;

public float levelBlend(VsmParams v, uint k, float coord, float2 xy, float slack = 0.0)
{
    if (k + 1 >= v.config.x)
        return 0.0;
    const float byDistance = saturate((coord + slack - float(k) + kLevelBlendBand) / kLevelBlendBand);
    const float2 pos = xy / pageWorldSize(v, k) - float2(v.centers[k].xy);  // the window spans [-16, 16)
    const float2 edge = min(pos + float(kPagesPerSide / 2), float(kPagesPerSide / 2) - pos);
    const float byWindow = saturate(1.0 - (min(edge.x, edge.y) - slack * 4.0 - 1.0) * 0.5);  // 3 pages from the edge to 1
    return max(byDistance, byWindow);
}

public int2 worldPage(VsmParams v, uint level, float2 xy) { return int2(floor(xy / pageWorldSize(v, level))); }

// The world page a directional page-table slot represents this frame (its window position).
public int2 slotWorldPage(VsmParams v, uint e)
{
    const uint level = e / kEntriesPerLevel, slot = e % kEntriesPerLevel;
    const int2 s = int2(slot % kPagesPerSide, slot / kPagesPerSide);
    const int2 lo = v.centers[level].xy - int(kPagesPerSide / 2);
    return lo + ((s - lo) % int(kPagesPerSide) + int(kPagesPerSide)) % int(kPagesPerSide);
}

// Expected tag of an entry this frame (local faces never scroll).
public uint entryTag(VsmParams v, uint e) { return e < v.local.y ? pageTag(slotWorldPage(v, e)) : 0u; }

// Physical page holding a page-table entry, or ~0 if it is not resident and current.
public uint residentPage(VsmParams v, ByteAddressBuffer st, uint e)
{
    const uint s = st.Load(e * 4);
    if ((s & kAllocated) == 0)
        return 0xffffffffu;
    const uint phys = s & 0xffff;
    if (st.Load((v.config.y + e) * 4) != entryTag(v, e) || st.Load((v.layout.x + phys) * 4) != e)
        return 0xffffffffu;
    return phys;
}

public float2 physicalOrigin(uint phys) { return float2(phys % kAtlasPagesPerSide, phys / kAtlasPagesPerSide) * float(kVsmPageTexels); }

// ---- local lights

// A shadow slot's light as its pages were last rendered (LightSystem keeps these after its
// kMaxLights lights): what the shadow passes and lookups project with. A moving light whose pages
// wait for the shadow budget keeps its last pose here, so its pages and lookups still agree.
public static const uint kShadowLightBase = 16384;  // LightSystem::kMaxLights
public uint slotLight(VsmParams v, uint slot) { return kShadowLightBase + slot; }
// The slot's light as it is now (its light and colour; shading).
public uint slotLiveLight(VsmParams v, uint slot) { return v.slotLights[slot >> 2][slot & 3]; }
public uint facePages(uint mip) { return kPagesPerSide >> mip; }
public uint faceMipOffset(uint mip)
{
    uint o = 0;
    for (uint m = 0; m < mip; ++m)
        o += facePages(m) * facePages(m);
    return o;
}
public uint localEntry(VsmParams v, uint slot, uint face, uint mip, uint2 page)
{
    return v.local.y + (slot * 6 + face) * kEntriesPerFace + faceMipOffset(mip) + page.y * facePages(mip) + page.x;
}

// Decodes a local entry into (slot * 6 + face, mip, page).
public void decodeLocalEntry(VsmParams v, uint e, out uint faceIndex, out uint mip, out uint2 page)
{
    const uint li = e - v.local.y;
    faceIndex = li / kEntriesPerFace;
    uint r = li % kEntriesPerFace;
    mip = 0;
    while (r >= facePages(mip) * facePages(mip))
    {
        r -= facePages(mip) * facePages(mip);
        ++mip;
    }
    page = uint2(r % facePages(mip), r / facePages(mip));
}

public struct ShadowFace
{
    public float3 right, up, forward;
    public float tanHalf;
};

public ShadowFace shadowFace(LightGpu l, uint face)
{
    ShadowFace f;
    if (uint(l.color.w) == kLightSpot)
    {
        f.forward = normalize(l.direction.xyz);
        const float3 helper = abs(f.forward.y) > 0.99 ? float3(1, 0, 0) : float3(0, 1, 0);
        f.right = normalize(cross(helper, f.forward));
        f.up = cross(f.forward, f.right);
        const float c = clamp(l.direction.w, 0.17, 0.9999);  // cos(outer), clamped to < 80 degrees
        f.tanHalf = sqrt(1.0 - c * c) / c * 1.05;
        return f;
    }
    const float3 axes[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
    const float3 ups[6] = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0) };
    f.forward = axes[face];
    f.up = ups[face];
    f.right = cross(f.up, f.forward);
    f.tanHalf = 1.0;
    return f;
}

public uint faceCount(LightGpu l) { return uint(l.color.w) == kLightSpot ? 1 : 6; }

// Cube face whose frustum contains direction v (spot lights: 0).
public uint pickFace(LightGpu l, float3 v)
{
    if (uint(l.color.w) == kLightSpot)
        return 0;
    const float3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z)
        return v.x >= 0.0 ? 0 : 1;
    if (a.y >= a.z)
        return v.y >= 0.0 ? 2 : 3;
    return v.z >= 0.0 ? 4 : 5;
}

// (At most 0.1: a light without a range reaches hundreds of units, and casters within its
// near plane had no shadow.)
public float localNear(VsmParams v, LightGpu l) { return clamp(l.positionRange.w * v.lod.y, 1e-3, 0.1); }

// Face-space projection of a world point: uv in [0,1]^2 (y down), view depth z.
public float3 faceProject(ShadowFace f, float3 lightPos, float3 p)
{
    const float3 d = p - lightPos;
    const float z = dot(d, f.forward);
    const float2 ndc = float2(dot(d, f.right), dot(d, f.up)) / (max(z, 1e-6) * f.tanHalf);
    return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, z);
}

// Continuous face mip: log2 of the pixel footprint in mip-0 texels (face texels grow with z).
public float faceMipCoord(VsmParams v, ShadowFace f, float z, float cameraDistance)
{
    const float texel0 = 2.0 * z * f.tanHalf / float(kLevelTexels);
    const float footprint = cameraDistance * v.world.z;
    return log2(max(footprint / texel0, 1e-6)) + v.world.w;
}

// Finest face mip whose texels are no larger than the pixel footprint.
public uint desiredFaceMip(VsmParams v, ShadowFace f, float z, float cameraDistance)
{
    return uint(clamp(ceil(faceMipCoord(v, f, z, cameraDistance)), 0.0, float(kFaceMips - 1)));
}

// How much of mip m + 1 to blend into mip m (as levelBlend, by distance only).
public float faceMipBlend(uint m, float mipCoord, float slack = 0.0)
{
    return m + 1 < kFaceMips ? saturate((mipCoord + slack - float(m) + kLevelBlendBand) / kLevelBlendBand) : 0.0;
}
