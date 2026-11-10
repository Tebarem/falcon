// M15: the Animator's preview pane (engine/render/preview_renderer.h). One character on a ground
// grid, drawn forward into its own 4x MSAA target and tonemapped there (no TAA, GI, SSR or local
// lights):
//   vs_sky + fs_sky        the sky gradient behind everything
//   vs_ground + fs_ground  a ground disc at y = 0 with a grid, the character's shadow on it
//   vs_shadow + fs_shadow  the character into the sun's shadow map (depth only)
//   vs_main + fs_main      the character: skinned vertices, its virtual-textured material (pages
//                          the CPU asked for: graphics shaders can't write the feedback list), a
//                          sun with a PCF shadow, sky ambient and the split-sum specular
//   vs_lines + fs_lines    bone lines over it (no depth test)
// The parts are InstanceGpu rows of the preview's own (world: model -> preview space; pad1: the
// floating skin record), drawn as (part, leaf cluster) items like naive_scene.

import scene_types;
import skinning;
import vt;
import lighting;
import sky;
import post_common;

struct PreviewUniforms
{
    float4x4 viewProj;
    float4x4 shadowViewProj;
    float4 cameraPos;     // w: exposure
    float4 sunDirection;  // xyz toward the sun
    float4 sunColor;      // rgb radiance, w: ambient strength
    float4 ground;        // x: grid cell (m), y: disc radius, zw: grid offset (root motion moving on)
    uint4 params;         // x: page table offset (bytes), y: shadow map size, z: flags (1 grid, 2 shadow)
};

// ---- vertex: set 0 resources, set 1 uniforms
[[vk::binding(0, 0)]] ByteAddressBuffer items : register(t0, space0);     // uint2 (part, cluster)
[[vk::binding(1, 0)]] ByteAddressBuffer parts : register(t1, space0);     // InstanceGpu[]
[[vk::binding(2, 0)]] ByteAddressBuffer scene : register(t2, space0);
[[vk::binding(3, 0)]] ByteAddressBuffer skin : register(t3, space0);
[[vk::binding(0, 1)]] ConstantBuffer<PreviewUniforms> uv : register(b0, space1);

// ---- fragment: set 2 resources (samplers, then buffers), set 3 uniforms
[[vk::binding(0, 2)]] Sampler2DArray<float4> vtAlbedo : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2DArray<float4> vtNormal : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] Sampler2DArray<float4> vtOrm : register(t2, space2) : register(s2, space2);
[[vk::binding(3, 2)]] Sampler2DShadow shadowMap : register(t3, space2) : register(s3, space2);
[[vk::binding(4, 2)]] ByteAddressBuffer materialData : register(t4, space2);  // MaterialGpu[] then page table
[[vk::binding(0, 3)]] ConstantBuffer<PreviewUniforms> uf : register(b0, space3);

InstanceGpu loadPart(uint i) { return parts.Load<InstanceGpu>(i * kInstanceStride); }

float3 toneMap(float3 radiance, float exposure) { return agx(radiance * exposure); }

// ---- sky

struct SkyOut
{
    float4 position : SV_Position;
    float3 dir : DIRECTION;
};

[shader("vertex")]
SkyOut vs_sky(uint id : SV_VertexID)
{
    SkyOut o;
    const float2 p = float2((id << 1) & 2, id & 2) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.position = float4(p, 0.0, 1.0);  // reverse-Z: the far plane
    // The world direction d through this corner: with rows r of viewProj, clip x / w = p.x and
    // y / w = p.y, so d is perpendicular to (r0 - p.x r3) and (r1 - p.y r3), and in front (w > 0).
    const float3 r0 = uv.viewProj[0].xyz, r1 = uv.viewProj[1].xyz, r3 = uv.viewProj[3].xyz;
    float3 d = cross(r0 - p.x * r3, r1 - p.y * r3);
    if (dot(d, r3) < 0.0)
        d = -d;
    o.dir = d;
    return o;
}

[shader("fragment")]
float4 fs_sky(SkyOut i) : SV_Target
{
    const float3 dir = normalize(i.dir);
    return float4(toneMap(openRadiance(dir), uf.cameraPos.w), 1.0);
}

// ---- shadow lookups (3x3 PCF on the hardware 2x2 compare)

float sunShadow(float3 world, float3 n)
{
    if ((uf.params.z & 2u) == 0)
        return 1.0;
    const float4 c = mul(uf.shadowViewProj, float4(world + n * 0.01, 1.0));
    const float3 s = c.xyz / c.w;
    const float2 st = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(st <= 0.0) || any(st >= 1.0))
        return 1.0;
    const float texel = 1.0 / float(uf.params.y);
    float lit = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            lit += shadowMap.SampleCmpLevelZero(st + float2(x, y) * texel, saturate(s.z + 0.0015));  // reverse-Z
    return lit / 9.0;
}

// ---- ground

struct GroundOut
{
    float4 position : SV_Position;
    float3 world : WORLD;
};

[shader("vertex")]
GroundOut vs_ground(uint id : SV_VertexID)
{
    // A square two triangles wide covering the disc; the fragment cuts it round and fades it.
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    GroundOut o;
    const float3 p = float3(corners[id].x, 0.0, corners[id].y) * uv.ground.y;
    o.world = p;
    o.position = mul(uv.viewProj, float4(p, 1.0));
    return o;
}

float gridLine(float2 p, float2 width, float cell)
{
    const float2 c = p / cell;
    const float2 d = abs(frac(c - 0.5) - 0.5) / max(width / cell, 1e-5);
    return 1.0 - saturate(min(d.x, d.y));
}

[shader("fragment")]
float4 fs_ground(GroundOut i) : SV_Target
{
    const float r = length(i.world.xz) / uf.ground.y;
    if (r > 1.0)
        discard;
    const float3 n = float3(0.0, 1.0, 0.0);
    const float3 v = normalize(uf.cameraPos.xyz - i.world);
    float3 albedo = float3(0.30, 0.30, 0.31);
    if ((uf.params.z & 1u) != 0)
    {
        const float2 p = i.world.xz + uf.ground.zw;
        const float2 width = fwidth(p);
        const float cell = uf.ground.x;
        const float lines = max(gridLine(p, width, cell) * 0.55, gridLine(p, width, cell * 10.0) * 0.9);
        albedo = lerp(albedo, float3(0.55, 0.55, 0.57), lines);
    }
    const float shadow = sunShadow(i.world, n);
    const float3 color = brdf(albedo, 0.0, 0.9, n, v, uf.sunDirection.xyz) * uf.sunColor.rgb * shadow +
                         albedo * flatAmbient(n) * uf.sunColor.w;
    // Fades into the sky towards the disc's edge.
    const float fade = saturate((1.0 - r) * 4.0);
    const float3 sky = openRadiance(normalize(i.world - uf.cameraPos.xyz));
    return float4(toneMap(lerp(sky, color, fade), uf.cameraPos.w), 1.0);
}

// ---- the character

struct MainOut
{
    float4 position : SV_Position;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
};

// The vertex of an item's triangle corner (degenerate past the cluster's triangles).
bool itemVertex(uint vertexId, uint itemId, out InstanceGpu inst, out DecodedVertex v, out uint material)
{
    const uint2 item = items.Load2(itemId * 8);
    inst = loadPart(item.x);
    const ClusterGpu cluster = loadCluster(scene, item.y);
    v = (DecodedVertex)0;
    material = 0;
    const uint tri = vertexId / 3;
    if (tri >= clusterTriangleCount(cluster))
        return false;
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint packed = loadClusterTriangle(scene, cluster, tri);
    const uint local = (packed >> (8 * (vertexId % 3))) & 0xff;
    const uint index = loadClusterVertex(scene, cluster, local);
    v = loadInstanceVertex(scene, skin, inst, mesh, index);
    material = instanceMaterial(inst, cluster);
    return true;
}

[shader("vertex")]
float4 vs_shadow(uint vertexId : SV_VertexID, uint itemId : SV_InstanceID) : SV_Position
{
    InstanceGpu inst;
    DecodedVertex v;
    uint material;
    if (!itemVertex(vertexId, itemId, inst, v, material))
        return float4(0.0, 0.0, 0.0, 1.0);
    return mul(uv.shadowViewProj, float4(transformPoint(inst, v.position), 1.0));
}

[shader("fragment")]
void fs_shadow() {}  // (depth only; SDL_GPU pipelines need a fragment shader)

[shader("vertex")]
MainOut vs_main(uint vertexId : SV_VertexID, uint itemId : SV_InstanceID)
{
    MainOut o = (MainOut)0;
    InstanceGpu inst;
    DecodedVertex v;
    uint material;
    if (!itemVertex(vertexId, itemId, inst, v, material))
    {
        o.position = float4(0.0, 0.0, 0.0, 1.0);
        return o;
    }
    o.world = transformPoint(inst, v.position);
    o.position = mul(uv.viewProj, float4(o.world, 1.0));
    o.normal = transformDirection(inst, v.normal);
    o.tangent = float4(transformDirection(inst, v.tangent.xyz), v.tangent.w);
    o.uv = v.uv;
    o.material = material;
    return o;
}

[shader("fragment")]
float4 fs_main(MainOut i, bool front : SV_IsFrontFace) : SV_Target
{
    const MaterialGpu m = materialData.Load<MaterialGpu>(i.material * kMaterialStride);
    float4 albedo = m.baseColor;
    float3 orm = float3(1.0, m.roughness, m.metallic);
    float2 normalXY = 0.5;
    if (m.vtPageTable != 0xffffffffu)
    {
        const uint mips = vtMipCount(m);
        const float mip = vtMipLevel(m, ddx(i.uv), ddy(i.uv), 0.0);
        const uint m0 = min(uint(mip), mips - 1);
        const float blend = m0 + 1 < mips ? frac(mip) : 0.0;
        const float2 wrapped = frac(i.uv);
        const VtPage a = vtFind(materialData, uf.params.x, m, wrapped, m0);
        const VtPage b = blend > 0.0 ? vtFind(materialData, uf.params.x, m, wrapped, m0 + 1) : a;
        const float3 ca = float3(a.uv, float(a.layer)), cb = float3(b.uv, float(b.layer));
        albedo *= lerp(vtAlbedo.SampleLevel(ca, 0.0), vtAlbedo.SampleLevel(cb, 0.0), blend);
        normalXY = lerp(vtNormal.SampleLevel(ca, 0.0).xy, vtNormal.SampleLevel(cb, 0.0).xy, blend);
        const float3 t = lerp(vtOrm.SampleLevel(ca, 0.0).xyz, vtOrm.SampleLevel(cb, 0.0).xyz, blend);
        orm = float3(lerp(1.0, t.x, m.occlusionStrength), materialRoughness(m, t.y), t.z * m.metallic);
    }
    if (albedo.a < m.alphaCutoff && (m.flags & 1u) != 0)  // alpha-masked
        discard;
    float3 n = normalize(front ? i.normal : -i.normal);
    if (m.vtPageTable != 0xffffffffu && dot(i.tangent.xyz, i.tangent.xyz) > 1e-8)
    {
        const float3 t = normalize(i.tangent.xyz - n * dot(n, i.tangent.xyz));
        const float3 b = cross(n, t) * (i.tangent.w < 0.0 ? -1.0 : 1.0);
        const float2 xy = (normalXY * 2.0 - 1.0) * m.normalScale;
        const float z = sqrt(saturate(1.0 - dot(xy, xy)));
        n = normalize(t * xy.x + b * xy.y + n * z);
    }
    const float3 v = normalize(uf.cameraPos.xyz - i.world);
    const float roughness = clamp(orm.y, 0.04, 1.0), metallic = saturate(orm.z);
    const float shadow = sunShadow(i.world, n);
    const float3 f0 = lerp(0.04, albedo.rgb, metallic);
    const float ndv = max(dot(n, v), 1e-4);
    const float3 ambient = flatAmbient(n) * uf.sunColor.w;
    float3 color = brdf(albedo.rgb, metallic, roughness, n, v, uf.sunDirection.xyz) * uf.sunColor.rgb * shadow;
    color += (albedo.rgb * (1.0 - metallic) * ambient + envSpecular(f0, roughness, ndv) * openRadiance(reflect(-v, n)) * uf.sunColor.w) * orm.x;
    color += m.emissive.rgb * m.emissive.w;
    return float4(toneMap(color, uf.cameraPos.w), 1.0);
}

// ---- bone lines (preview space, two points per line): `items` holds float4 (xyz, RGBA8 colour)

struct LineOut
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
LineOut vs_lines(uint id : SV_VertexID)
{
    const float4 p = asfloat(items.Load4(id * 16));
    const uint c = asuint(p.w);
    LineOut o;
    o.position = mul(uv.viewProj, float4(p.xyz, 1.0));
    o.color = float4(float(c & 0xff), float((c >> 8) & 0xff), float((c >> 16) & 0xff), float(c >> 24)) / 255.0;
    return o;
}

[shader("fragment")]
float4 fs_lines(LineOut i) : SV_Target { return i.color; }
