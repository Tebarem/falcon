// Transparent (alpha-blend) scene materials: one indexed indirect draw of the sorted clusters
// (transparent_cull.slang), farthest first, blended (premultiplied) over the lit frame after the
// resolve. The opaque depth is the draw's depth target, tested and never written: what the scene
// hides is rejected before shading. Shaded like the resolve: the material's textures and factors,
// its surface shader (R4), the sun and its shadow, the local lights (and their shadows), ambient
// from the probe GI or the sky, specular from the ambient. Writes TAA's and FSR's
// reactive mask (its coverage), as particles do: transparent surfaces have no motion vectors.
//
// Refraction (T2): a material with "refraction" draws what is behind it itself, from the frame
// before transparents with mips (the reflection source: reflection_source), its lookup bent by the
// view-space normal and blurred by the roughness; it then writes alpha 1 (over what was drawn behind
// it: transparents behind a refracting one are not seen through it). Where the bent lookup lands on
// something in front of the surface (the HiZ says), the straight one is used.

import scene_types;
import skinning;
import vt;
import lighting;
import vsm;
import shadows;
import sky;
import probe_common;
import falcon_surface;
import project_surfaces;  // (a project's build has its surface shaders here)

struct TransparentVertexUniforms
{
    float4x4 viewProj;  // the raster's (jittered)
};

struct TransparentUniforms
{
    float4 cameraPos;      // w: near plane
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    uint4 screen;          // width, height, scene colour's mips, 1: the HiZ is this frame's
    float4 sunDirection;
    float4 sunColor;
    uint4 vtParams;        // page-table byte offset, materials' params offset (R4), GI (1: the probes)
    float4 vtParamsF;      // mip bias, seconds
    LightGrid lightGrid;
    VsmParams vsm;
    ProbeVolume probes;
};

// ---- vertex: set 0 resources, set 1 uniforms
[[vk::binding(0, 0)]] ByteAddressBuffer draws : register(t0, space0);  // sorted: instance, cluster, first, count
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer skin : register(t2, space0);
[[vk::binding(0, 1)]] ConstantBuffer<TransparentVertexUniforms> tv : register(b0, space1);

// ---- fragment: set 2 resources, set 3 uniforms
[[vk::binding(0, 2)]] Sampler2DArray<float4> vtAlbedo : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2DArray<float4> vtNormal : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] Sampler2DArray<float4> vtOrm : register(t2, space2) : register(s2, space2);
[[vk::binding(3, 2)]] Sampler2DShadow vsmAtlas : register(t3, space2) : register(s3, space2);
// Probe GI (render/probe_gi.h): irradiance and distance atlases (bilinear), probe data (point).
[[vk::binding(4, 2)]] Sampler2D<float4> probeIrradianceAtlas : register(t4, space2) : register(s4, space2);
[[vk::binding(5, 2)]] Sampler2D<float4> probeDistanceAtlas : register(t5, space2) : register(s5, space2);
[[vk::binding(6, 2)]] Sampler2D<float4> probeDataAtlas : register(t6, space2) : register(s6, space2);
[[vk::binding(7, 2)]] Sampler2D<float4> sceneColor : register(t7, space2) : register(s7, space2);  // with mips
[[vk::binding(8, 2)]] ByteAddressBuffer materialData : register(t8, space2);  // materials, page table, params
[[vk::binding(9, 2)]] ByteAddressBuffer lights : register(t9, space2);
[[vk::binding(10, 2)]] ByteAddressBuffer lightGrid : register(t10, space2);
[[vk::binding(11, 2)]] ByteAddressBuffer vsmState : register(t11, space2);
[[vk::binding(12, 2)]] ByteAddressBuffer hiz : register(t12, space2);  // farthest depth per 2x2 (level 0)
[[vk::binding(0, 3)]] ConstantBuffer<TransparentUniforms> tf : register(b0, space3);

struct TransparentShadows : IShadowMaps
{
    VsmParams vsm() { return tf.vsm; }
    float3 sunDirection() { return tf.sunDirection.xyz; }
    uint resident(uint entry) { return residentPage(tf.vsm, vsmState, entry); }
    float compare(float2 uv, float ref) { return vsmAtlas.SampleCmpLevelZero(uv, ref); }
};

struct TransparentProbes : IProbeTextures
{
    float3 irradiance(float2 uv) { return probeIrradianceAtlas.SampleLevel(uv, 0.0).rgb; }
    float2 distance(float2 uv) { return probeDistanceAtlas.SampleLevel(uv, 0.0).rg; }
    float4 data(float2 uv) { return probeDataAtlas.SampleLevel(uv, 0.0); }
};

struct VsOut
{
    float4 position : SV_Position;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;  // w: bitangent sign (mirroring included)
    float2 uv : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint tint : TINT;
};

[shader("vertex")]
VsOut vs_transparent(uint index : SV_VertexID)
{
    const uint4 d = draws.Load4((index >> 7) * 16);
    const InstanceGpu inst = loadInstance(scene, d.x);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const ClusterGpu cl = loadCluster(scene, d.y);
    const DecodedVertex v = loadInstanceVertex(scene, skin, inst, mesh, loadClusterVertex(scene, cl, index & 127), false, true);
    const float mirror = dot(inst.row0.xyz, cross(inst.row1.xyz, inst.row2.xyz)) < 0.0 ? -1.0 : 1.0;
    VsOut o;
    o.world = transformPoint(inst, v.position);
    o.position = mul(tv.viewProj, float4(o.world, 1.0));
    o.normal = transformDirection(inst, v.normal);
    o.tangent = float4(transformDirection(inst, v.tangent.xyz), v.tangent.w * mirror);
    o.uv = v.uv;
    o.material = instanceMaterial(inst, cl);
    o.tint = inst.tint;
    return o;
}

struct FsOut
{
    float4 color : SV_Target0;    // premultiplied
    float reactive : SV_Target1;  // coverage (MAX)
};

[shader("fragment")]
FsOut fs_transparent(VsOut i)
{
    const MaterialGpu m = materialData.Load<MaterialGpu>(i.material * kMaterialStride);

    // The material's pages (trilinear, each level falling back to the nearest coarser resident
    // page, as the resolve samples them). Graphics stages write no VT feedback: the renderer asks
    // for these pages itself (TransparentRenderer::requestPages).
    float4 albedoTex = 1.0;
    float2 normalXY = 0.5;
    float3 orm = 1.0;
    if (m.vtPageTable != 0xffffffffu)
    {
        const uint mips = vtMipCount(m);
        const float mip = vtMipLevel(m, ddx(i.uv), ddy(i.uv), tf.vtParamsF.x);
        const uint m0 = min(uint(mip), mips - 1);
        const float blend = m0 + 1 < mips ? frac(mip) : 0.0;
        const float2 wrapped = frac(i.uv);
        const VtPage a = vtFind(materialData, tf.vtParams.x, m, wrapped, m0);
        const VtPage b = blend > 0.0 ? vtFind(materialData, tf.vtParams.x, m, wrapped, m0 + 1) : a;
        const float3 ca = float3(a.uv, float(a.layer)), cb = float3(b.uv, float(b.layer));
        albedoTex = lerp(vtAlbedo.SampleLevel(ca, 0.0), vtAlbedo.SampleLevel(cb, 0.0), blend);
        normalXY = lerp(vtNormal.SampleLevel(ca, 0.0).xy, vtNormal.SampleLevel(cb, 0.0).xy, blend);
        orm = lerp(vtOrm.SampleLevel(ca, 0.0), vtOrm.SampleLevel(cb, 0.0), blend).xyz;
    }
    const float3 tint = i.tint == 0 ? float3(1.0, 1.0, 1.0)
                                    : float3(i.tint & 0xff, (i.tint >> 8) & 0xff, (i.tint >> 16) & 0xff) / 255.0;
    float alpha = saturate(albedoTex.a * m.baseColor.a);
    float3 albedo = albedoTex.rgb * m.baseColor.rgb * tint;
    float roughness = clamp(materialRoughness(m, orm.g), 0.045, 1.0);
    float metallic = saturate(orm.b * m.metallic);
    float ao = lerp(1.0, orm.r, m.occlusionStrength);
    float3 emissive = m.emissive.rgb * m.emissive.w;

    const float3 vdir = normalize(tf.cameraPos.xyz - i.world);
    float3 n = dot(i.normal, i.normal) > 1e-12 ? normalize(i.normal) : vdir;
    const bool backFace = dot(n, vdir) < 0.0;  // (double-sided: the back is lit as its own front)
    if (backFace)
        n = -n;
    float3 t = i.tangent.xyz - n * dot(n, i.tangent.xyz);
    if (dot(t, t) < 1e-12)
        t = abs(n.x) < 0.9 ? cross(n, float3(1, 0, 0)) : cross(n, float3(0, 1, 0));
    t = normalize(t);
    const float3 bt = cross(n, t) * i.tangent.w * (backFace ? -1.0 : 1.0);
    const float2 nxy = (normalXY * 2.0 - 1.0) * m.normalScale;
    float3 shadingN = normalize(t * nxy.x + bt * nxy.y + n * sqrt(saturate(1.0 - dot(nxy, nxy))));

    // The material's params: its surface shader's, then refraction and ior (editor/material_library.h).
    const float4 engineParams = asfloat(materialData.Load4(tf.vtParams.y + i.material * 32 + 16));
    const bool refracts = (m.flags & 16u) != 0;  // kMaterialRefraction
    const float2 screenUv = i.position.xy / float2(tf.screen.xy);
    float2 refraction = 0.0;
    float refractionBlur = 0.0;
    if (refracts)
    {
        // The shading normal's sideways part on screen bends the lookup (a flat pane facing the
        // camera: none); rough surfaces blur what is behind them.
        const float2 nv = float2(dot(shadingN, tf.cameraRight.xyz), -dot(shadingN, tf.cameraUp.xyz));
        refraction = -nv * engineParams.x * (1.0 - 1.0 / max(engineParams.y, 1.0));
        refractionBlur = roughness * roughness * float(max(tf.screen.z, 1u) - 1);
    }
    const uint surfaceShader = (m.flags >> 8) & 0xffu;
    float4 surfaceParams = float4(0.0, 0.0, 0.0, 0.0);
    if (surfaceShader != 0)
    {
        surfaceParams = asfloat(materialData.Load4(tf.vtParams.y + i.material * 32));
        SurfaceIn si;
        si.position = i.world;
        si.normal = n;
        si.view = vdir;
        si.uv = i.uv;
        si.params = surfaceParams;
        si.baseColor = m.baseColor;
        si.time = tf.vtParamsF.y;
        si.screenUv = screenUv;
        si.transparent = true;
        SurfaceOut so;
        so.alpha = alpha;
        so.refraction = refraction;
        so.refractionBlur = refractionBlur;
        so.albedo = albedo;
        so.roughness = roughness;
        so.metallic = metallic;
        so.ao = ao;
        so.emissive = emissive;
        so.normal = shadingN;
        projectSurface(surfaceShader, si, so);
        albedo = so.albedo;
        roughness = clamp(so.roughness, 0.045, 1.0);
        metallic = saturate(so.metallic);
        ao = saturate(so.ao);
        emissive = so.emissive;
        shadingN = dot(so.normal, so.normal) > 1e-12 ? normalize(so.normal) : shadingN;
        alpha = saturate(so.alpha);
        refraction = so.refraction;
        refractionBlur = max(so.refractionBlur, 0.0);
    }

    const float3 sunL = tf.sunDirection.xyz;
    const float cameraDistance = length(i.world - tf.cameraPos.xyz);
    const TransparentShadows sm = {};
    uint level;
    const float shadow = dot(n, sunL) > 0.0 ? sunShadow(sm, i.world, n, cameraDistance, 0.0, level) : 0.0;
    float3 direct = brdf(albedo, metallic, roughness, shadingN, vdir, sunL) * tf.sunColor.rgb * shadow;
    uint visited;
    const float viewDepth = dot(i.world - tf.cameraPos.xyz, tf.cameraForward.xyz);
    direct += shadeLights(lights, lightGrid, tf.lightGrid, uint2(i.position.xy), viewDepth, i.world, albedo, metallic, roughness,
                          shadingN, vdir, visited);
    shadeShadowedLights(sm, lights, i.world, n, shadingN, vdir, albedo, metallic, roughness, cameraDistance, direct, visited);
    // Ambient: the probe GI (as the resolve samples it), or the flat sky without GI.
    float3 ambient;
    if (tf.vtParams.z != 0)
    {
        const TransparentProbes probes;
        ambient = probeIrradiance(tf.probes, probes, i.world, n, shadingN, vdir);
    }
    else
    {
        ambient = flatAmbient(shadingN);
    }
    const float3 specular = envSpecular(lerp(0.04, albedo, metallic), roughness, saturate(dot(shadingN, vdir))) * ambient * ao;
    float3 hdr = direct + albedo * (1.0 - metallic) * ambient * ao;
    if (surfaceShader != 0)
    {
        LightIn li;
        li.albedo = albedo;
        li.normal = shadingN;
        li.view = vdir;
        li.sunDirection = sunL;
        li.sunColor = tf.sunColor.rgb;
        li.sunDot = dot(shadingN, sunL);
        li.shadow = shadow;
        li.ambient = ambient;
        li.params = surfaceParams;
        li.time = tf.vtParamsF.y;
        projectLight(surfaceShader, li, hdr);
    }
    // Premultiplied: the surface's own light fades with its coverage; what it reflects (the
    // ambient's specular) and what it emits do not (clear glass still reflects).
    FsOut o;
    o.reactive = alpha;
    if (refracts)
    {
        // What is behind it, bent: unless the bent lookup lands on something in front of the
        // surface (the farthest depth of that 2x2 nearer than the surface, reverse-Z).
        float2 uv = saturate(screenUv + refraction);
        if (tf.screen.w != 0)
        {
            const uint2 level = (tf.screen.xy + 1) / 2;  // (level 0: half resolution, rounded up)
            const uint2 t = min(uint2(uv * float2(tf.screen.xy)) / 2, level - 1);
            if (asfloat(hiz.Load((t.y * level.x + t.x) * 4)) > i.position.z)
                uv = screenUv;
        }
        const float3 behind = sceneColor.SampleLevel(uv, refractionBlur).rgb * albedo;  // (tinted by the surface's colour)
        o.color = float4(hdr * alpha + behind * (1.0 - alpha) + specular + emissive, 1.0);
        return o;
    }
    o.color = float4(hdr * alpha + specular + emissive, alpha);
    return o;
}
