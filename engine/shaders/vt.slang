// Virtual texture sampling (layout: engine/assets/fpak_format.h, "Virtual texturing").
// A material's pages live in a page table (one uint per page: bit 31 resident, low 16 bits
// the physical array layer). Lookups walk toward coarser mips until a resident page is found;
// the last mip of every material is pinned, so the walk always ends.

module vt;

import scene_types;

public static const uint kPageTexels = 128;
public static const float kBorder = 4.0;
public static const float kPhysicalTexels = 136.0;
public static const uint kResident = 0x80000000u;

public uint vtLevelPages(uint log2Size, uint mip)
{
    return max((1u << log2Size) >> mip, kPageTexels) / kPageTexels;
}

public uint vtMipCount(MaterialGpu m) { return (m.vtSize >> 16) & 0xff; }
public float2 vtSizeTexels(MaterialGpu m) { return float2(float(1u << (m.vtSize & 0xff)), float(1u << ((m.vtSize >> 8) & 0xff))); }

// Page-table entry of the page covering uv (already wrapped to [0,1)) at a mip level.
public uint vtEntry(MaterialGpu m, uint mip, float2 uv, out float2 inPage)
{
    const uint lw = m.vtSize & 0xff, lh = (m.vtSize >> 8) & 0xff;
    uint offset = 0;
    for (uint k = 0; k < mip; ++k)
        offset += vtLevelPages(lw, k) * vtLevelPages(lh, k);
    const uint2 pages = uint2(vtLevelPages(lw, mip), vtLevelPages(lh, mip));
    const float2 p = uv * float2(pages);
    const uint2 page = min(uint2(p), pages - 1);
    inPage = p - float2(page);
    return m.vtPageTable + offset + page.y * pages.x + page.x;
}

public struct VtPage
{
    public uint layer;
    public float2 uv;  // in the physical page, border included
    public uint mip;   // level actually found (>= requested when falling back)
};

public VtPage vtFind(ByteAddressBuffer pageTable, uint pageTableOffset, MaterialGpu m, float2 uv, uint mip)
{
    VtPage r;
    const uint mips = vtMipCount(m);
    for (uint k = min(mip, mips - 1); k < mips; ++k)
    {
        float2 inPage;
        const uint e = vtEntry(m, k, uv, inPage);
        const uint pt = pageTable.Load(pageTableOffset + e * 4);
        if ((pt & kResident) != 0 || k == mips - 1)
        {
            r.layer = pt & 0xffff;
            r.uv = (inPage * float(kPageTexels) + kBorder) / kPhysicalTexels;
            r.mip = k;
            return r;
        }
    }
    r.layer = 0;
    r.uv = 0.5;
    r.mip = mips - 1;
    return r;
}

// Isotropic mip from texture-space footprints of one pixel step in x and y.
public float vtMipLevel(MaterialGpu m, float2 dUVdx, float2 dUVdy, float bias)
{
    const float2 size = vtSizeTexels(m);
    const float2 dx = dUVdx * size, dy = dUVdy * size;
    return max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)) + bias, 0.0);
}
