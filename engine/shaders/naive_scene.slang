// M1 naive scene draw: one instanced draw, one instance per (scene instance, leaf cluster),
// 128 * 3 vertices each; unused triangle slots collapse to a degenerate point.
// Replaced in M2 by GPU culling + DAG LOD selection + the visibility buffer.

import scene_types;

struct ViewUniforms
{
    float4x4 viewProj;
    float4 cameraPos;
};

struct ShadeUniforms
{
    float4 sunDirection;  // xyz toward the sun
    uint debugMode;       // 0 lit, 1 clusters, 2 normals, 3 instances
    uint pad0, pad1, pad2;
};

// ---- vertex (set 0 resources, set 1 uniforms)
[[vk::binding(0, 0)]] ByteAddressBuffer drawItems : register(t0, space0);  // uint2 (instance, cluster)
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(0, 1)]] ConstantBuffer<ViewUniforms> view : register(b0, space1);

// ---- fragment (set 2 resources, set 3 uniforms)
[[vk::binding(0, 2)]] ByteAddressBuffer sceneFs : register(t0, space2);
[[vk::binding(0, 3)]] ConstantBuffer<ShadeUniforms> shade : register(b0, space3);

struct VsOut
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    nointerpolation uint cluster : CLUSTER;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint instance : INSTANCE;
};

[shader("vertex")]
VsOut vs_main(uint vertexId : SV_VertexID, uint itemId : SV_InstanceID)
{
    const uint2 item = drawItems.Load2(itemId * 8);
    const InstanceGpu inst = loadInstance(scene, item.x);
    const ClusterGpu cluster = loadCluster(scene, item.y);

    VsOut o = (VsOut)0;
    const uint tri = vertexId / 3;
    if (tri >= clusterTriangleCount(cluster))
    {
        o.position = float4(0.0, 0.0, 0.0, 1.0);  // all three corners equal: zero-area, rasterizes nothing
        return o;
    }
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint packed = loadClusterTriangle(scene, cluster, tri);
    const uint local = (packed >> (8 * (vertexId % 3))) & 0xff;
    const uint index = loadClusterVertex(scene, cluster, local);
    const DecodedVertex v = loadVertex(scene, mesh, index);

    const float3 world = transformPoint(inst, v.position);
    o.position = mul(view.viewProj, float4(world, 1.0));
    o.normal = transformDirection(inst, v.normal);
    o.uv = v.uv;
    o.cluster = item.y;
    o.material = instanceMaterial(inst, cluster);
    o.instance = item.x;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut i, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    float3 n = normalize(i.normal);
    if (!frontFace)
        n = -n;
    switch (shade.debugMode)
    {
    case 1: return float4(hashColor(i.cluster) * (0.35 + 0.65 * saturate(n.y * 0.5 + 0.5)), 1.0);
    case 2: return float4(n * 0.5 + 0.5, 1.0);
    case 3: return float4(hashColor(i.instance * 7919u), 1.0);
    default: break;
    }
    const MaterialGpu m = loadSceneMaterial(sceneFs, i.material);
    const float ndl = saturate(dot(n, shade.sunDirection.xyz));
    const float3 ambient = lerp(float3(0.08, 0.07, 0.06), float3(0.16, 0.19, 0.24), n.y * 0.5 + 0.5);
    const float3 color = m.baseColor.rgb * (ambient + ndl * float3(1.0, 0.95, 0.85));
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);  // temporary gamma until the HDR post chain (M5)
}
