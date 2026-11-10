// GPU scene data as stored in .fpak sections; mirrors engine/assets/fpak_format.h.
// All buffers are ByteAddressBuffers (SDL_GPU raw views), read with Load<T>.

module scene_types;

public struct InstanceGpu
{
    public float4 row0, row1, row2;  // world transform, rows of a 3x4 matrix
    public uint mesh;
    public uint flags;  // kInstanceHidden...; bits 16..31: material override + 1 (0: the clusters')
    public uint tint;   // RGBA8 base colour multiplier, 0 = none
    public uint pad1;   // skinned (kInstanceSkinned): skinned-instance record index
};

public static const uint kInstanceHidden = 1;  // unused dynamic-pool slot
public static const uint kInstanceTerrainShaded = 4;  // shaded with a terrain layer: pad1 = layer | base blend (dm) << 8
public static const uint kInstanceNoShadow = 8;  // casts no shadow
public static const uint kInstanceScatter = 16;  // grown by a scatter layer (thinned first when budgets run out)
public static const uint kInstanceDynamic = 32;  // moved lately: drawn over the shadow pages' static layer
public static const uint kInstanceLayered = 64;  // a layered level mesh: pad1 = its layer record (render/surface_layers.h)

public float3 instanceTint(InstanceGpu inst)
{
    return inst.tint == 0 ? float3(1.0, 1.0, 1.0)
                          : float3(inst.tint & 0xff, (inst.tint >> 8) & 0xff, (inst.tint >> 16) & 0xff) / 255.0;
}

public struct MeshGpu
{
    public float4 posOffset;  // w: 1 when one of its materials glows
    public float4 posScale;
    public uint clusterOffset, clusterCount;
    public uint lodNodeOffset, lodLevels;
};

public struct ClusterGpu
{
    public float4 cullSphere;
    public float4 selfLod;
    public float4 parentLod;
    public float selfError;
    public float parentError;
    public uint vertexOffset;
    public uint triangleOffset;
    public uint counts;  // vertexCount | triangleCount << 8 | material << 16
    public uint cone;
    public uint depth;
    public uint pad;
};

public struct MaterialGpu
{
    public float4 baseColor;
    public float metallic, roughness, alphaCutoff;
    public uint flags;
    public uint vtPageTable;  // first page-table entry, 0xffffffff = untextured
    public uint vtSize;       // log2(width) | log2(height) << 8 | mip count << 16 | roughness min (255ths) << 24
    public float normalScale, occlusionStrength;
    public float4 emissive;   // rgb, w = strength
};

public static const uint kInstanceStride = 64;
public static const uint kMeshStride = 48;
public static const uint kClusterStride = 80;
public static const uint kMaterialStride = 64;
public static const uint kVertexStride = 24;

// A material's roughness where its roughness map reads `map`: the map's black gives its roughness
// min (vtSize's top byte), its white its roughness (min 0: the map times the roughness).
public float materialRoughness(MaterialGpu m, float map)
{
    return lerp(float(m.vtSize >> 24) / 255.0, m.roughness, map);
}

public uint clusterVertexCount(ClusterGpu c) { return c.counts & 0xff; }
public uint clusterTriangleCount(ClusterGpu c) { return (c.counts >> 8) & 0xff; }
public uint clusterMaterial(ClusterGpu c) { return c.counts >> 16; }
// The material an instance draws a cluster with: its own (flags bits 16..31 = material + 1, set
// in the editor) or the cluster's.
public uint instanceMaterial(InstanceGpu inst, ClusterGpu c)
{
    const uint own = inst.flags >> 16;
    return own != 0 ? own - 1 : clusterMaterial(c);
}

public float3 octDecode(uint packed)
{
    const float2 e = float2(float(int(packed << 16) >> 16), float(int(packed) >> 16)) / 32767.0;
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

public struct DecodedVertex
{
    public float3 position;  // mesh space
    public float3 normal;
    public float4 tangent;   // xyz, w = bitangent sign
    public float2 uv;
};

// The scene buffer holds every .fpak section: a 64-byte header with the byte offset of each
// section (uint[16], in fpak::Section order), then the sections themselves. One binding instead
// of eight keeps passes under SDL_GPU's 8-storage-buffer-per-stage limit.
public static const uint kSecVertices = 0;
public static const uint kSecClusterVertices = 1;
public static const uint kSecClusterTriangles = 2;
public static const uint kSecClusters = 3;
public static const uint kSecLodNodes = 4;
public static const uint kSecMeshes = 5;
public static const uint kSecInstances = 6;
public static const uint kSecMaterials = 7;
public static const uint kSecLights = 8;
public static const uint kSecSkinVertices = 9;   // SkinVertexGpu (u8 joints x4, u8 weights x4)
public static const uint kSecMeshSkins = 10;

public uint sceneSection(ByteAddressBuffer scene, uint section) { return scene.Load(section * 4); }

public InstanceGpu loadInstance(ByteAddressBuffer scene, uint i)
{
    return scene.Load<InstanceGpu>(sceneSection(scene, kSecInstances) + i * kInstanceStride);
}
public MeshGpu loadMesh(ByteAddressBuffer scene, uint i)
{
    return scene.Load<MeshGpu>(sceneSection(scene, kSecMeshes) + i * kMeshStride);
}
public ClusterGpu loadCluster(ByteAddressBuffer scene, uint i)
{
    return scene.Load<ClusterGpu>(sceneSection(scene, kSecClusters) + i * kClusterStride);
}
public MaterialGpu loadSceneMaterial(ByteAddressBuffer scene, uint i)
{
    return scene.Load<MaterialGpu>(sceneSection(scene, kSecMaterials) + i * kMaterialStride);
}
// Packed triangle t of a cluster: 3 x u8 cluster-local vertex indices.
public uint loadClusterTriangle(ByteAddressBuffer scene, ClusterGpu c, uint t)
{
    return scene.Load(sceneSection(scene, kSecClusterTriangles) + (c.triangleOffset + t) * 4);
}
// Global vertex index of a cluster-local vertex.
public uint loadClusterVertex(ByteAddressBuffer scene, ClusterGpu c, uint local)
{
    return scene.Load(sceneSection(scene, kSecClusterVertices) + (c.vertexOffset + local) * 4);
}

public DecodedVertex loadVertex(ByteAddressBuffer scene, MeshGpu mesh, uint index)
{
    const uint base = sceneSection(scene, kSecVertices) + index * kVertexStride;
    const uint4 a = scene.Load4(base);
    const uint2 b = scene.Load2(base + 16);
    DecodedVertex v;
    const float3 q = float3(float(a.x & 0xffff), float(a.x >> 16), float(a.y & 0xffff));
    v.position = mesh.posOffset.xyz + q * mesh.posScale.xyz;
    v.normal = octDecode(a.z);
    v.tangent = float4(octDecode(a.w), ((a.y >> 16) & 1) != 0 ? -1.0 : 1.0);
    v.uv = asfloat(b);
    return v;
}

public float3 transformPoint(InstanceGpu inst, float3 p)
{
    const float4 h = float4(p, 1.0);
    return float3(dot(inst.row0, h), dot(inst.row1, h), dot(inst.row2, h));
}

public float3 transformDirection(InstanceGpu inst, float3 d)
{
    return float3(dot(inst.row0.xyz, d), dot(inst.row1.xyz, d), dot(inst.row2.xyz, d));
}

public float3 hashColor(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return float3(float(x & 0xff), float((x >> 8) & 0xff), float((x >> 16) & 0xff)) / 255.0;
}

// Texel fetch through a combined sampler. Slang's HLSL output for Sampler2D.Load/operator[]
// passes the sampler into Texture2D.Load (rejected by DXC), so use a point-sampled SampleLevel
// at the texel center. (Integer textures are read as plain Texture2D storage textures instead.)
public float fetchFloat(Sampler2D<float> t, uint2 p, float2 invSize)
{
    return t.SampleLevel((float2(p) + 0.5) * invSize, 0.0);
}
