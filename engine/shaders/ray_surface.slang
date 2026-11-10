// What a ray hit (ray_trace.slang's RtHit) looks like, for shading it away from the screen: its
// point, normal, colour and glow from the scene buffer and the virtual texture's coarse mips, as
// the resolve reads them. And alpha-masked surfaces' holes, for filters.

module ray_surface;

import scene_types;
import ray_trace;
import terrain_shading;
import vt;

public static const uint kMaterialAlphaMask = 1u;
public static const uint kMaterialDoubleSided = 4u;

public struct RaySurface
{
    public float3 position;  // world
    public float3 normal;    // world, toward the ray (a double-sided surface's back turned round)
    public float3 albedo;
    public float3 emissive;
    public bool backface;    // the ray hit a one-sided surface from behind (inside a wall)
};

// Whether a hit on an alpha-masked material lands in its holes (false) or on it, from its colour's
// alpha at its mip count - mipsUp (resident or the nearest coarser one; 1: the coarsest mip, always
// resident, so the answer doesn't depend on what has streamed in).
public bool rayAlphaAccept(ByteAddressBuffer scene, ByteAddressBuffer materialData, Sampler2DArray<float4> albedo, uint pageTable,
                           uint mipsUp, uint instance, uint cluster, uint triangle, float2 bary)
{
    const InstanceGpu inst = loadInstance(scene, instance);
    const ClusterGpu cl = loadCluster(scene, cluster);
    const MaterialGpu m = materialData.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
    if ((m.flags & kMaterialAlphaMask) == 0 || m.vtPageTable == 0xffffffffu)
        return true;
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint tri = loadClusterTriangle(scene, cl, triangle);
    float2 uv = 0.0;
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const DecodedVertex v = loadVertex(scene, mesh, loadClusterVertex(scene, cl, (tri >> (8 * k)) & 0xff));
        uv += v.uv * (k == 0 ? 1.0 - bary.x - bary.y : (k == 1 ? bary.x : bary.y));
    }
    const uint mips = vtMipCount(m);
    const VtPage page = vtFind(materialData, pageTable, m, frac(uv), mips > mipsUp ? mips - mipsUp : 0);
    return albedo.SampleLevel(float3(page.uv, float(page.layer)), 0.0).a * m.baseColor.a >= m.alphaCutoff;
}

// The surface at a hit of the ray origin + t * dir. Colour from the material's coarsest mip
// (always resident), the terrain's tint or default look.
public RaySurface raySurface(ByteAddressBuffer scene, ByteAddressBuffer materialData, Sampler2DArray<float4> albedo, uint pageTable,
                             RtHit hit, float3 origin, float3 dir)
{
    const InstanceGpu inst = loadInstance(scene, hit.instance);
    const ClusterGpu cl = loadCluster(scene, hit.cluster);
    const MaterialGpu m = materialData.Load<MaterialGpu>(instanceMaterial(inst, cl) * kMaterialStride);
    const MeshGpu mesh = loadMesh(scene, inst.mesh);
    const uint tri = loadClusterTriangle(scene, cl, hit.triangle);
    float2 uv = 0.0;
    float3 n = 0.0;
    float3 p[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const DecodedVertex v = loadVertex(scene, mesh, loadClusterVertex(scene, cl, (tri >> (8 * k)) & 0xff));
        const float w = k == 0 ? 1.0 - hit.bary.x - hit.bary.y : (k == 1 ? hit.bary.x : hit.bary.y);
        uv += v.uv * w;
        n += v.normal * w;
        p[k] = transformPoint(inst, v.position);
    }
    RaySurface s;
    s.position = origin + dir * hit.t;
    // The vertex normal (mesh space to world: the inverse transpose; uniform scales are most of
    // them, so the transform and a renormalize), on the side the ray came from.
    float3 geometric = cross(p[1] - p[0], p[2] - p[0]);
    geometric = dot(geometric, geometric) > 1e-20 ? normalize(geometric) : -dir;
    float3 wn = transformDirection(inst, n);
    wn = dot(wn, wn) > 1e-12 ? normalize(wn) : geometric;
    const bool doubleSided = (m.flags & kMaterialDoubleSided) != 0;
    s.backface = hit.backface && !doubleSided;
    if (dot(wn, dir) > 0.0)
        wn = -wn;  // (a double-sided surface's back, or a smooth normal past the silhouette)
    s.normal = wn;
    s.albedo = m.baseColor.rgb * instanceTint(inst);
    if (m.vtPageTable != 0xffffffffu)
    {
        const VtPage page = vtFind(materialData, pageTable, m, frac(uv), vtMipCount(m) - 1);
        s.albedo *= albedo.SampleLevel(float3(page.uv, float(page.layer)), 0.0).rgb;
    }
    if ((m.flags & kMaterialTerrain) != 0)
        s.albedo = inst.tint != 0 ? instanceTint(inst) : terrainLook(s.position, s.normal).albedo;
    s.emissive = m.emissive.rgb * m.emissive.w;
    return s;
}
