// Skinned instances (M8): one GPU buffer per frame holding, after a 64-byte header of byte
// offsets, the skinned-instance records, the skinning matrices, the skinned vertices (two
// generations: this frame and last frame, for motion vectors) and per-cluster bounds.
// Mirrors engine/render/skinning.h.

module skinning;

import scene_types;

public static const uint kSkinHdrRecords = 0;
public static const uint kSkinHdrBones = 1;
public static const uint kSkinHdrVertices = 2;
public static const uint kSkinHdrClusterBounds = 3;
public static const uint kSkinHdrCollapse = 4;    // per bone: float4 (where its vertices go to the eye, 1) or 0: shown
public static const uint kSkinRecordStride = 64;
public static const uint kSkinVertexStride = 32;  // float3 position, oct normal, oct tangent, float3 position to the eye
public static const uint kSkinViewPosition = 20;  // (the eye's: on hidden bones, pulled into their joint)

public static const uint kInstanceSkinned = 2;    // InstanceGpu.flags; InstanceGpu.skin = record index

public struct SkinInstanceGpu
{
    public float4 bounds;           // animated bounding sphere, model (instance) space
    public uint firstBone;          // into the matrix array (3 float4 rows each)
    public uint vertexBase;         // skinned vertex of the mesh's first vertex, this frame
    public uint prevVertexBase;     // same, last frame
    public uint clusterBoundsBase;  // dynamic sphere of the mesh's first cluster
    public uint firstVertex;        // mesh vertex range in the scene's Vertices section
    public uint vertexCount;
    public uint firstSkinVertex;    // into the scene's SkinVertices section
    public uint mesh;
    public uint clusterOffset;      // mesh clusters (all DAG levels)
    public uint clusterCount;
    public uint hidden;             // 1: bones hidden to the eye (kSkinHdrCollapse)
    public uint pad1;
};

public uint skinSection(ByteAddressBuffer skin, uint s) { return skin.Load(s * 4); }
public uint skinSection(RWByteAddressBuffer skin, uint s) { return skin.Load(s * 4); }

public SkinInstanceGpu loadSkinInstance(ByteAddressBuffer skin, uint i)
{
    return skin.Load<SkinInstanceGpu>(skinSection(skin, kSkinHdrRecords) + i * kSkinRecordStride);
}
public SkinInstanceGpu loadSkinInstance(RWByteAddressBuffer skin, uint i)
{
    return skin.Load<SkinInstanceGpu>(skinSection(skin, kSkinHdrRecords) + i * kSkinRecordStride);
}

public bool isSkinned(InstanceGpu inst) { return (inst.flags & kInstanceSkinned) != 0; }

// Skinned position/normal/tangent of mesh vertex `g` (UVs and the tangent sign stay those of
// the cooked vertex). `previous`: last frame's skinning. `eye`: the position the eye sees (hidden
// bones' vertices pulled into their joint: a first-person head); else the whole character's (shadows).
public DecodedVertex loadInstanceVertex(ByteAddressBuffer scene, ByteAddressBuffer skin, InstanceGpu inst,
                                        MeshGpu mesh, uint g, bool previous = false, bool eye = false)
{
    DecodedVertex v = loadVertex(scene, mesh, g);
    if (isSkinned(inst))
    {
        const SkinInstanceGpu s = loadSkinInstance(skin, inst.pad1);
        const uint base = previous ? s.prevVertexBase : s.vertexBase;
        const uint a = skinSection(skin, kSkinHdrVertices) + (base + g - s.firstVertex) * kSkinVertexStride;
        const uint3 p = skin.Load3(a + (eye ? kSkinViewPosition : 0));
        const uint2 nt = skin.Load2(a + 12);
        v.position = asfloat(p);
        v.normal = octDecode(nt.x);
        v.tangent.xyz = octDecode(nt.y);
    }
    return v;
}

// Model-space bounding sphere of a skinned cluster (recomputed every frame).
public float4 skinnedClusterSphere(ByteAddressBuffer skin, InstanceGpu inst, uint clusterIndex)
{
    const SkinInstanceGpu s = loadSkinInstance(skin, inst.pad1);
    return asfloat(skin.Load4(skinSection(skin, kSkinHdrClusterBounds) +
                              (s.clusterBoundsBase + clusterIndex - s.clusterOffset) * 16));
}
