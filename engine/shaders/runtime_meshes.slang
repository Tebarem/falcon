// Meshes made at runtime (R5: scripts' Mesh), drawn after the resolve into the lit frame: lit like
// the scene (the sun and its shadow, the local lights, the sky), tested against the scene's depth
// here (they write none), opaque ones as they are, transparent ones blended (premultiplied) back
// to front. Vertices: position, normal, uv, colour.

import scene_types;
import lighting;
import vsm;
import shadows;
import sky;

struct MeshUniforms
{
    float4x4 viewProj;     // the raster's (jittered)
    float4x4 world;        // the mesh's (System.Numerics' layout: points as mul(world, p))
    float4 cameraPos;      // w: near plane
    float4 cameraForward;
    float4 sunDirection;
    float4 sunColor;
    float4 color;          // linear rgb, alpha (under 1: blended)
    float4 emissive;       // rgb, strength
    float4 surface;        // roughness, metallic, ambient scale
    uint4 screen;          // width, height
    LightGrid lightGrid;
    VsmParams vsm;
};

// ---- vertex: set 1 uniforms
[[vk::binding(0, 1)]] ConstantBuffer<MeshUniforms> uv : register(b0, space1);

// ---- fragment: set 2 resources, set 3 uniforms
[[vk::binding(0, 2)]] Sampler2DShadow vsmAtlas : register(t0, space2) : register(s0, space2);
[[vk::binding(1, 2)]] Sampler2D<float> sceneDepth : register(t1, space2) : register(s1, space2);
[[vk::binding(2, 2)]] ByteAddressBuffer lights : register(t2, space2);
[[vk::binding(3, 2)]] ByteAddressBuffer lightGrid : register(t3, space2);
[[vk::binding(4, 2)]] ByteAddressBuffer vsmState : register(t4, space2);
[[vk::binding(0, 3)]] ConstantBuffer<MeshUniforms> uf : register(b0, space3);

struct MeshShadows : IShadowMaps
{
    VsmParams vsm() { return uf.vsm; }
    float3 sunDirection() { return uf.sunDirection.xyz; }
    uint resident(uint entry) { return residentPage(uf.vsm, vsmState, entry); }
    float compare(float2 c, float ref) { return vsmAtlas.SampleCmpLevelZero(c, ref); }
};

struct VertexIn
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOut vs_mesh(VertexIn i)
{
    VertexOut o;
    o.world = mul(uv.world, float4(i.position, 1.0)).xyz;
    o.normal = mul((float3x3)uv.world, i.normal);
    o.color = i.color * uv.color;
    o.position = mul(uv.viewProj, float4(o.world, 1.0));
    return o;
}

[shader("fragment")]
float4 fs_mesh(VertexOut i) : SV_Target0
{
    // Behind the scene's surface there: hidden (reverse-Z, infinite far).
    const float viewDepth = dot(i.world - uf.cameraPos.xyz, uf.cameraForward.xyz);
    const float raw = sceneDepth.SampleLevel(i.position.xy / float2(uf.screen.xy), 0.0);
    const float sceneView = raw > 0.0 ? uf.cameraPos.w / raw : 1e9;
    if (viewDepth > sceneView * 1.0005)
        discard;
    const float3 v = normalize(uf.cameraPos.xyz - i.world);
    float3 n = dot(i.normal, i.normal) > 1e-12 ? normalize(i.normal) : v;
    if (dot(n, v) < 0.0)
        n = -n;  // (both sides lit)
    const float3 albedo = i.color.rgb;
    const float roughness = clamp(uf.surface.x, 0.045, 1.0), metallic = saturate(uf.surface.y);
    const MeshShadows sm = {};
    uint level;
    const float3 sun = uf.sunDirection.xyz;
    const float shadow = dot(n, sun) > 0.0 ? sunShadow(sm, i.world, n, length(i.world - uf.cameraPos.xyz), 0.0, level) : 0.0;
    float3 color = brdf(albedo, metallic, roughness, n, v, sun) * uf.sunColor.rgb * shadow;
    uint visited;
    color += shadeLights(lights, lightGrid, uf.lightGrid, uint2(i.position.xy), viewDepth, i.world, albedo, metallic, roughness, n, v,
                         visited);
    color += albedo * (1.0 - metallic) * flatAmbient(n) * uf.surface.z;
    color += uf.emissive.rgb * uf.emissive.w;
    const float a = saturate(i.color.a);
    return float4(color * a, a);  // (premultiplied)
}
