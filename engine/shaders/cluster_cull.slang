// GPU-driven culling and LOD selection. Each dependent step is its own compute pass
// (SDL_GPU only synchronizes at pass boundaries); see GpuDrivenRenderer::render.
//
//   cs_reset          clear counters
//   cs_instances      frustum-cull instances, emit 64-cluster chunks
//   cs_args_cull1     chunks -> dispatch args
//   cs_cull_first     DAG cut + frustum + cone + HiZ(prev frame); visible or re-test candidate
//   cs_args_draw1     -> expand/draw args for pass 1, dispatch args for the re-test
//   cs_expand         visible clusters -> index buffer (cluster_expand.slang)
//   cs_args_raster1   -> the pass-1 draws' index counts (the triangles cs_expand packed)
//   cs_cull_second    candidates vs HiZ of this frame's pass-1 depth
//   cs_args_draw2     -> expand/draw args for pass 2
//   cs_args_raster2   -> the pass-2 draws' index counts

import scene_types;
import culling;
import skinning;

// set 0: read-only storage buffers
[[vk::binding(0, 0)]] ByteAddressBuffer workList : register(t0, space0);  // chunks or candidates: uint2
[[vk::binding(1, 0)]] ByteAddressBuffer scene : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer materials : register(t2, space0);  // VT material data
[[vk::binding(3, 0)]] ByteAddressBuffer hiz : register(t3, space0);
[[vk::binding(4, 0)]] ByteAddressBuffer skin : register(t4, space0);  // skinned instances (skinning.slang)
// set 1: read-write storage buffers
[[vk::binding(0, 1)]] RWByteAddressBuffer counters : register(u0, space1);
// Slot 1 is bound to a different buffer per pass: chunks (cs_instances), visible clusters
// (cs_cull_*), or the indirect args buffer (cs_reset, cs_args_*).
[[vk::binding(1, 1)]] RWByteAddressBuffer rwOut : register(u1, space1);
[[vk::binding(2, 1)]] RWByteAddressBuffer outCandidates : register(u2, space1);
[[vk::binding(3, 1)]] RWByteAddressBuffer outTransparent : register(u3, space1);  // cs_cull_first: blend clusters
// set 2: uniforms
[[vk::binding(0, 2)]] ConstantBuffer<CullUniforms> cull : register(b0, space2);

// ---- reset

[shader("compute")]
[numthreads(16, 1, 1)]
void cs_reset(uint3 tid : SV_DispatchThreadID)
{
    counters.Store(tid.x * 4, 0);
    if (tid.x == 0)
    {
        [unroll]
        for (uint i = 0; i < kArgDraw1; i += 16)
            rwOut.Store4(i, uint4(0, 1, 1, 0));  // dispatch-shaped defaults
        for (uint d = kArgDraw1; d < kArgCullTransparent; d += 32)
        {
            rwOut.Store4(d, uint4(0, 1, 0, 0));  // indexed args: count, instances, first index, vertex offset
            rwOut.Store4(d + 16, uint4(0, 0, 0, 0));
        }
        rwOut.Store4(kArgCullTransparent, uint4(0, 1, 1, 0));
        rwOut.Store4(kArgExpandTransparent, uint4(0, 1, 1, 0));
        rwOut.Store4(kArgDrawTransparent, uint4(0, 1, 0, 0));
        rwOut.Store4(kArgDrawTransparent + 16, uint4(0, 0, 0, 0));
    }
}

// ---- instances

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_instances(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= cull.limits.x)
        return;
    const InstanceGpu inst = loadInstance(scene, i);
    if ((inst.flags & kInstanceHidden) != 0)
        return;
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    if (mesh.clusterCount == 0)
        return;
    const float3 halfExtent = mesh.posScale.xyz * (65535.0 * 0.5);
    // Skinned: the animated bounds (the bind-pose box does not contain the moving mesh).
    const float4 bounds = isSkinned(inst) ? loadSkinInstance(skin, inst.pad1).bounds
                                          : float4(mesh.posOffset.xyz + halfExtent, length(halfExtent));
    const float3 center = transformPoint(inst, bounds.xyz);
    const float radius = bounds.w * maxScale(inst);
    if (!sphereInFrustum(cull, center, radius))
        return;
    // Contribution culling: an instance smaller than a few pixels adds nothing visible (an open
    // world's distant grass patches and flowers would otherwise fill the visible-cluster budget).
    // Not the terrain's patches: small far ones are still ground (cliffs keep fine patches far).
    // Scatter (grass, flowers) has its own, larger smallest size while the visible-cluster budget
    // is short: the far small ones go first, the same ones every frame (an overflow drops
    // clusters of any mesh, different ones each frame).
    // Not glowing meshes either (or ones drawn with another material, which may glow): a light
    // strip a few pixels wide, far off, still shows and blooms; culled, distant lights' glow
    // stopped at a distance. Nor anything within the full-detail distance.
    const float distance = max(length(center - cull.cameraPos.xyz) - radius, cull.proj.z);
    const bool near = distance < cull.scatter.y;
    const bool terrainPatch = i - cull.hizLevels[0].w < cull.hizLevels[1].w;
    const bool glows = mesh.posOffset.w > 0.5 || (inst.flags >> 16) != 0;  // (bits 16..31: material override + 1)
    const float smallest = (inst.flags & kInstanceScatter) != 0 ? max(cull.proj.w, cull.scatter.x) : cull.proj.w;
    if (!terrainPatch && !glows && !near && radius * cull.proj.y < 2.0 * smallest * distance)
        return;

    const uint chunks = (mesh.clusterCount + kClustersPerChunk - 1) / kClustersPerChunk;
    uint base;
    counters.InterlockedAdd(kCtrChunks * 4, chunks, base);
    counters.InterlockedAdd(kCtrInstances * 4, 1);
    for (uint c = 0; c < chunks && base + c < cull.limits.y; ++c)
        rwOut.Store2((base + c) * 8, uint2(i, mesh.clusterOffset + c * kClustersPerChunk));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args_cull1(uint3 tid : SV_DispatchThreadID)
{
    const uint chunks = min(counters.Load(kCtrChunks * 4), cull.limits.y);
    rwOut.Store3(kArgCull1, uint3(chunks, 1, 1));
}

// ---- clusters

// Appends a visible cluster to the opaque (bottom-up) or alpha-masked (top-down) range.
void emitVisible(uint2 item, bool masked, uint pass)
{
    const uint maxVisible = cull.limits.z;
    uint slot;
    if (masked)
    {
        const uint before = pass == 0 ? 0 : min(counters.Load(kCtrMasked1 * 4), maxVisible);
        counters.InterlockedAdd((pass == 0 ? kCtrMasked1 : kCtrMasked2) * 4, 1, slot);
        slot = before + slot;
        if (slot < maxVisible)
            rwOut.Store2((maxVisible - 1 - slot) * 8, item);
    }
    else
    {
        const uint before = pass == 0 ? 0 : min(counters.Load(kCtrVisible1 * 4), maxVisible);
        counters.InterlockedAdd((pass == 0 ? kCtrVisible1 : kCtrVisible2) * 4, 1, slot);
        slot = before + slot;
        if (slot < maxVisible)
            rwOut.Store2(slot * 8, item);
    }
    if (slot >= maxVisible)
        counters.InterlockedOr(kCtrOverflow * 4, pass + 1);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull_first(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 chunk = workList.Load2(group.x * 8);  // (instance, first cluster)
    const InstanceGpu inst = loadInstance(scene, chunk.x);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint clusterIndex = chunk.y + thread.x;
    if (clusterIndex >= mesh.clusterOffset + mesh.clusterCount)
        return;
    const ClusterGpu cl = loadCluster(scene, clusterIndex);
    const float scale = maxScale(inst);

    // DAG cut: the coarsest version whose error is still below the threshold. Skinned meshes
    // measure every cluster from the instance's animated bounds: one distance for the whole
    // mesh keeps the cut consistent (crack-free) while the bind-pose group spheres move.
    const bool skinned = isSkinned(inst);
    const float4 animated = skinned ? loadSkinInstance(skin, inst.pad1).bounds : float4(0.0);
    const float threshold = cull.cameraPos.w;
    if (lodError(cull, inst, scale, skinned ? animated : cl.selfLod, cl.selfError) > threshold)
        return;
    if (lodError(cull, inst, scale, skinned ? animated : cl.parentLod, cl.parentError) <= threshold)
        return;

    const float4 sphere = skinned ? skinnedClusterSphere(skin, inst, clusterIndex) : cl.cullSphere;
    const float3 center = transformPoint(inst, sphere.xyz);
    const float radius = sphere.w * scale;
    if (!sphereInFrustum(cull, center, radius))
        return;
    const MaterialGpu m = materials.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
    // Normal cones do not survive deformation: no backface culling for skinned clusters.
    if ((cull.params.x & kFlagCone) != 0 && (m.flags & 4u) == 0 && !skinned &&
        coneBackfacing(cull, inst, center, radius, cl.cone))
        return;

    const uint2 item = uint2(chunk.x, clusterIndex);
    // Alpha-blend materials: not in the visibility buffer. They go to the transparent pass
    // (transparent_cull.slang), which tests them against this frame's final HiZ.
    if ((m.flags & 2u) != 0)  // kMaterialAlphaBlend
    {
        uint slot;
        counters.InterlockedAdd(kCtrTransparentCandidates * 4, 1, slot);
        if (slot < kMaxTransparentCandidates)
            outTransparent.Store2(slot * 8, item);
        else
            counters.InterlockedOr(kCtrOverflow * 4, kOverflowTransparent);
        return;
    }
    if ((cull.params.x & kFlagForceRetest) != 0 ||
        ((cull.params.x & kFlagOcclusion) != 0 && occludedByHiz(cull, hiz, cull.prevView, center, radius)))
    {
        uint slot;
        counters.InterlockedAdd(kCtrCandidates * 4, 1, slot);
        if (slot < cull.limits.w)
            outCandidates.Store2(slot * 8, item);
        return;
    }
    emitVisible(item, (m.flags & 1u) != 0 && (cull.params.x & kFlagNoAlphaMask) == 0, 0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args_draw1(uint3 tid : SV_DispatchThreadID)
{
    const VisibleCounts c = loadVisibleCounts(counters, cull.limits.z);
    const uint candidates = min(counters.Load(kCtrCandidates * 4), cull.limits.w);
    rwOut.Store3(kArgExpand1, uint3(c.opaque1 + c.masked1, 1, 1));
    rwOut.Store4(kArgDraw1, uint4(c.opaque1 * kIndicesPerCluster, 1, 0, 0));
    rwOut.Store(kArgDraw1 + 16, 0);
    rwOut.Store4(kArgDraw1Masked, uint4(c.masked1 * kIndicesPerCluster, 1, (cull.limits.z - c.masked1) * kIndicesPerCluster, 0));
    rwOut.Store(kArgDraw1Masked + 16, 0);
    rwOut.Store3(kArgCull2, uint3((candidates + 63) / 64, 1, 1));
    const uint transparent = min(counters.Load(kCtrTransparentCandidates * 4), kMaxTransparentCandidates);
    rwOut.Store3(kArgCullTransparent, uint3((transparent + 63) / 64, 1, 1));
}

// The draws' index counts once cs_expand has packed their triangles (cs_args_draw* gave each
// draw its first index). Scalar stores (see cs_args_draw2).
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args_raster1(uint3 tid : SV_DispatchThreadID)
{
    rwOut.Store(kArgDraw1, counters.Load((kCtrPacked + 0) * 4) * 3);
    rwOut.Store(kArgDraw1Masked, counters.Load((kCtrPacked + 1) * 4) * 3);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args_raster2(uint3 tid : SV_DispatchThreadID)
{
    rwOut.Store(kArgDraw2, counters.Load((kCtrPacked + 2) * 4) * 3);
    rwOut.Store(kArgDraw2Masked, counters.Load((kCtrPacked + 3) * 4) * 3);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_cull_second(uint3 tid : SV_DispatchThreadID)
{
    const uint candidates = min(counters.Load(kCtrCandidates * 4), cull.limits.w);
    if (tid.x >= candidates)
        return;
    const uint2 item = workList.Load2(tid.x * 8);
    const InstanceGpu inst = loadInstance(scene, item.x);
    const ClusterGpu cl = loadCluster(scene, item.y);
    const float scale = maxScale(inst);
    const float4 sphere = isSkinned(inst) ? skinnedClusterSphere(skin, inst, item.y) : cl.cullSphere;
    const float3 center = transformPoint(inst, sphere.xyz);
    if (occludedByHiz(cull, hiz, cull.view, center, sphere.w * scale))
        return;
    const MaterialGpu m = materials.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
    emitVisible(uint2(item.x | kVisibleSecondPassBit, item.y), (m.flags & 1u) != 0 && (cull.params.x & kFlagNoAlphaMask) == 0, 1);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_args_draw2(uint3 tid : SV_DispatchThreadID)
{
    const VisibleCounts c = loadVisibleCounts(counters, cull.limits.z);
    const uint maskedBase = cull.limits.z - c.masked1 - c.masked2;
    // Scalar stores: compiled to SPIR-V, this Store3 also wrote (count, 1, 1) over kArgDraw1,
    // which the MSAA pass reads after pass 2 (D3D12 was fine).
    rwOut.Store(kArgExpand2, c.opaque2 + c.masked2);
    rwOut.Store(kArgExpand2 + 4, 1);
    rwOut.Store(kArgExpand2 + 8, 1);
    rwOut.Store4(kArgDraw2, uint4(c.opaque2 * kIndicesPerCluster, 1, c.opaque1 * kIndicesPerCluster, 0));
    rwOut.Store(kArgDraw2 + 16, 0);
    rwOut.Store4(kArgDraw2Masked, uint4(c.masked2 * kIndicesPerCluster, 1, maskedBase * kIndicesPerCluster, 0));
    rwOut.Store(kArgDraw2Masked + 16, 0);
}
