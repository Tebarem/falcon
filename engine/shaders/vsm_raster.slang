// Virtual shadow map rasterization into the physical depth atlas (or, with moving casters, its
// static layer; vsm_copy.slang then starts each page from it and they are drawn over):
//   vs_clear + fs_clear   instanced quad per page whose static layer is rendered, writes depth 0 (= no occluder)
//   vs_shadow + fs_depth  every (cluster, page) record's triangles that reach past their page's
//                         edge (vsm_pages.slang cs_expand): clip distances confine them to it
//   vs_inside + fs_inside the rest, inside their page (no clipping, which is slow)
//   vs_edge + fs_edge     small triangles crossing the edge: whole, fragments outside the page discarded
//   both: alpha-masked materials (foliage cards) alpha-test as in
//                         visbuffer's fs_masked, so they cast their cut-out's shadow, and
//                         foliage (double-sided too) casts a partial one (kFoliageShadow)

import scene_types;
import vsm;
import lighting;
import skinning;
import vt;

[[vk::binding(0, 0)]] ByteAddressBuffer records : register(t0, space0);  // (instance, cluster, entry, 0)
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer state : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer lights : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer skin : register(t4, space0);
[[vk::binding(0, 1)]] ConstantBuffer<VsmParams> vsm : register(b0, space1);

struct MaskUniforms
{
    uint pageTableOffset;
    float mipBias;
    uint pad0, pad1;
};

[[vk::binding(0, 2)]] Sampler2DArray<float4> vtAlbedo : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] ByteAddressBuffer materialData : register(t1, space2);
[[vk::binding(0, 3)]] ConstantBuffer<MaskUniforms> mask : register(b0, space3);

struct ShadowVsOut
{
    float4 position : SV_Position;
    float clip0 : SV_ClipDistance0;  // four page edges (scalar outputs pack on both backends)
    float clip1 : SV_ClipDistance1;
    float clip2 : SV_ClipDistance2;
    float clip3 : SV_ClipDistance3;
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;  // ~0: no alpha test
};

struct InsideVsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
};

struct EdgeVsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint2 page : PAGE;  // the page's first atlas texel
};

float4 atlasPosition(uint phys, float2 uvInPage, float depth)
{
    const float2 uv = (physicalOrigin(phys) / float(kVsmPageTexels) + uvInPage) / float(kAtlasPagesPerSide);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
}

[shader("vertex")]
ShadowVsOut vs_clear(uint vertexId : SV_VertexID, uint dirtyIndex : SV_InstanceID)
{
    const uint d = state.Load((vsm.layout2.z + dirtyIndex) * 4);
    const uint e = state.Load((vsm.layout.x + d) * 4);  // the page's entry
    const bool whole = (state.Load((vsm.layout2.w + (e >> 5)) * 4) & (1u << (e & 31))) != 0;
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    ShadowVsOut o;
    o.position = atlasPosition(d, corners[vertexId], 0.0);
    o.clip0 = o.clip1 = o.clip2 = o.clip3 = whole ? 1.0 : -1.0;  // (moving casters only: its static layer stays)
    o.uv = 0.0;
    o.material = 0xffffffffu;
    return o;
}

[shader("vertex")]
ShadowVsOut vs_shadow(uint index : SV_VertexID)
{
    return shadowVertex(index);
}

[shader("vertex")]
InsideVsOut vs_inside(uint index : SV_VertexID)
{
    const ShadowVsOut s = shadowVertex(index);
    InsideVsOut o;
    o.position = s.clip0 < 0.0 && s.clip1 < 0.0 ? float4(0.0, 0.0, 0.0, 1.0) : s.position;  // (a dropped page's record: degenerate)
    o.uv = s.uv;
    o.material = s.material;
    return o;
}

[shader("vertex")]
EdgeVsOut vs_edge(uint index : SV_VertexID)
{
    const ShadowVsOut s = shadowVertex(index);
    EdgeVsOut o;
    o.position = s.clip0 < 0.0 && s.clip1 < 0.0 ? float4(0.0, 0.0, 0.0, 1.0) : s.position;
    o.uv = s.uv;
    o.material = s.material;
    const uint phys = state.Load(records.Load((index >> 7) * 16 + 8) * 4) & 0xffff;
    o.page = uint2(physicalOrigin(phys));
    return o;
}

ShadowVsOut shadowVertex(uint index)
{
    const uint4 record = records.Load4((index >> 7) * 16);
    const InstanceGpu inst = loadInstance(scene, record.x);
    const ClusterGpu cl = loadCluster(scene, record.y);
    const uint e = record.z;
    const uint st = state.Load(e * 4);
    ShadowVsOut o;
    if ((st & kAllocated) == 0)  // page dropped after culling overflowed: clip the whole record
    {
        o.position = float4(0.0, 0.0, 0.0, 1.0);
        o.clip0 = o.clip1 = o.clip2 = o.clip3 = -1.0;
        o.uv = 0.0;
        o.material = 0xffffffffu;
        return o;
    }
    const uint phys = st & 0xffff;
    const DecodedVertex v = loadInstanceVertex(scene, skin, inst, loadMesh(scene, inst.mesh), loadClusterVertex(scene, cl, index & 127));
    const float3 wp = transformPoint(inst, v.position);
    o.uv = v.uv;
    o.material = instanceMaterial(inst, cl);
    if (e >= vsm.local.y)
    {
        // Local face: perspective, written homogeneously so the page clip planes and the
        // page -> atlas mapping stay linear (and interpolation perspective-correct).
        uint faceIndex, mip;
        uint2 page;
        decodeLocalEntry(vsm, e, faceIndex, mip, page);
        const LightGpu l = loadLight(lights, slotLight(vsm, faceIndex / 6));
        const ShadowFace face = shadowFace(l, faceIndex % 6);
        const float3 d = wp - l.positionRange.xyz;
        const float w = dot(d, face.forward);
        const float2 ndc = float2(dot(d, face.right), dot(d, face.up)) / face.tanHalf;  // times w
        const float n = float(facePages(mip));
        const float2 uvw = float2(ndc.x * 0.5 + 0.5 * w, 0.5 * w - ndc.y * 0.5) * n - float2(page) * w;  // page uv * w
        const float2 atlas = (physicalOrigin(phys) / float(kVsmPageTexels) * w + uvw) / float(kAtlasPagesPerSide);
        o.position = float4(atlas.x * 2.0 - w, w - atlas.y * 2.0, localNear(vsm, l), w);  // depth = near / z
        o.clip0 = uvw.x;
        o.clip1 = w - uvw.x;
        o.clip2 = uvw.y;
        o.clip3 = w - uvw.y;
        return o;
    }
    const uint level = e / kEntriesPerLevel;
    const int2 page = slotWorldPage(vsm, e);
    const float2 uvInPage = lightXY(vsm, wp) / pageWorldSize(vsm, level) - float2(page);
    o.position = atlasPosition(phys, uvInPage, lightDepth(vsm, wp));
    o.clip0 = uvInPage.x;
    o.clip1 = 1.0 - uvInPage.x;
    o.clip2 = uvInPage.y;
    o.clip3 = 1.0 - uvInPage.y;
    return o;
}

[shader("fragment")]
void fs_clear()
{
}

// Foliage casts a partial shadow: its cards write depth into this share of the atlas texels (a
// 4x4 ordered dither), which the receivers' 5x5 filter averages. Blades are thinner than a shadow
// texel and light passes between and through them; covering every texel they touch, a field
// of grass shadowed itself and the ground completely. The pattern is the same for every card,
// so layers of grass don't stack past it, and it is fixed in the atlas: cached pages agree with
// fresh ones.
static const float kFoliageShadow = 0.5;
static const float kBayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

[shader("fragment")]
void fs_depth(ShadowVsOut i)
{
    alphaTest(i.position, i.uv, i.material);
}

[shader("fragment")]
void fs_inside(InsideVsOut i)
{
    alphaTest(i.position, i.uv, i.material);
}

[shader("fragment")]
void fs_edge(EdgeVsOut i)
{
    const int2 texel = int2(i.position.xy) - int2(i.page);  // (exact: an interpolated page uv is not, in perspective)
    if (any(texel < 0 || texel >= int(kVsmPageTexels)))
        discard;
    alphaTest(i.position, i.uv, i.material);
}

void alphaTest(float4 position, float2 uv, uint material)
{
    if (material == 0xffffffffu)
        return;
    const MaterialGpu m = materialData.Load<MaterialGpu>(material * kMaterialStride);
    if ((m.flags & 3u) == 0)
        return;  // opaque (alpha-blend materials cast where their alpha reaches the cutoff: dense parts
                 // of a window cast, clear glass does not)
    float alpha = m.baseColor.a;
    if (m.vtPageTable != 0xffffffffu)
    {
        // The material's last mip: always resident (pinned), so a cached shadow page doesn't
        // depend on how far texture streaming had got when it was rendered.
        const VtPage p = vtFind(materialData, mask.pageTableOffset, m, frac(uv), vtMipCount(m) - 1);
        alpha *= vtAlbedo.SampleLevel(float3(p.uv, float(p.layer)), 0.0).a;
    }
    if (alpha < m.alphaCutoff)
        discard;
    if ((m.flags & 5u) == 5u)  // kMaterialAlphaMask | kMaterialDoubleSided: foliage
    {
        const uint2 t = uint2(position.xy) & 3u;
        if ((kBayer4[t.y * 4 + t.x] + 0.5) / 16.0 >= kFoliageShadow)
            discard;
    }
}
