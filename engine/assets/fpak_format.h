#pragma once

// .fpak: cooked scene package. Every section is page-aligned and already in the exact
// layout of its GPU buffer, so loading is read -> copy into a transfer buffer -> upload,
// with no parsing or fix-up. Offsets only, no pointers.
//
// GPU structs here are mirrored in engine/shaders/shared/scene_types.slang; keep
// both in sync (sizes are static_asserted below and fields are 16-byte friendly so
// std430, scalar and D3D layouts agree).

#include <cstdint>

namespace fx::fpak {

inline constexpr char kMagic[4] = {'F', 'P', 'A', 'K'};
inline constexpr uint32_t kVersion = 8;
inline constexpr uint32_t kSectionAlign = 4096;

// Cluster limits shared with the visibility-buffer encoding (triangle:7 bits).
inline constexpr uint32_t kMaxClusterVertices = 128;
inline constexpr uint32_t kMaxClusterTriangles = 128;

enum class Section : uint32_t {
    Vertices,          // VertexGpu[]
    ClusterVertices,   // uint32 global vertex index per cluster-local vertex
    ClusterTriangles,  // uint32 per triangle: 3 x u8 local vertex indices
    Clusters,          // ClusterGpu[]
    LodNodes,          // LodNodeGpu[] (BVH over cluster groups, per mesh)
    Meshes,            // MeshGpu[]
    Instances,         // InstanceGpu[]
    Materials,         // MaterialGpu[]
    Lights,            // LightGpu[] (KHR_lights_punctual, world space)
    SkinVertices,      // SkinVertexGpu[] (skinned meshes, parallel to their vertices)
    MeshSkins,         // MeshSkinGpu[] per mesh
    Colliders,         // CPU only: Box3D b3MeshData / b3HullData blobs (relocatable, 16-byte aligned)
    MeshColliders,     // CPU only: MeshCollider[] per mesh
    Animation,         // CPU only: skeletons and compressed clips (anim/anim_format.h)
    Names,             // CPU only: uint32 offset per cooked instance, then NUL-terminated UTF-8 names
    NodeAnimation,     // CPU only: NodeAnimHeader, NodeTrackEntry[], then float rows (rigid node animation)
    Count
};

// Built-in meshes every pak carries (Unity's sizes, centered on the origin):
//   Cube      1 x 1 x 1                 Sphere    radius 0.5
//   Capsule   radius 0.5, height 2 (Y)  Cylinder  radius 0.5, height 2 (Y)
//   Plane     10 x 10 on y = 0, facing +Y (10 x 10 quads)
//   Quad      1 x 1 on z = 0, facing -Z  Cone      radius 0.5, height 1, apex up
enum class Builtin : uint32_t { Cube, Sphere, Capsule, Cylinder, Plane, Quad, Cone, Count };
inline constexpr const char* kBuiltinNames[uint32_t(Builtin::Count)] = {"cube", "sphere", "capsule", "cylinder",
                                                                       "plane", "quad", "cone"};

// Sections the runtime keeps in host memory only (not uploaded to the scene buffer).
inline constexpr bool sectionOnGpu(Section s) { return uint32_t(s) < uint32_t(Section::Colliders); }

struct SectionEntry {
    uint64_t offset;
    uint64_t size;
    uint32_t count;
    uint32_t stride;
};

struct Header {
    char magic[4];
    uint32_t version;
    uint64_t sourceHash;  // hash of the source files + cooker version, for cook caching
    float boundsMin[3];
    float boundsMax[3];
    uint32_t leafTriangles;  // triangles at full detail across all instances
    uint32_t totalTriangles;  // all DAG levels, unique geometry
    uint32_t vtPageCount;     // virtual texture pages in the companion .fvt file
    uint32_t vtPageBytes;
    uint32_t builtinMeshes[uint32_t(Builtin::Count)];  // mesh index of each built-in, by Builtin
    uint32_t builtinMaterial;
    SectionEntry sections[uint32_t(Section::Count)];
};

// Rigid node animation (glTF node TRS channels, baked by the cooker): per moving mesh node or
// camera, its world transform (3x4, row-major; for a library: in the library's own space) at
// each frame of [first, first + count), held before and after; the whole loops every `loop`
// frames. Library tracks belong to library `library` (ScenePak::libraries order), node or camera
// `target`; the scene's own (library kNodeAnimScene) to cooked instance `target`.
inline constexpr uint32_t kNodeAnimScene = 0xffffffffu;
inline constexpr uint32_t kNodeTrackCamera = 1u;
struct NodeAnimHeader {
    uint32_t trackCount;
    float fps;
    uint32_t floatCount;
    uint32_t pad;
};
struct NodeTrackEntry {
    uint32_t library, target, flags;  // flags: kNodeTrackCamera
    uint32_t first, count, loop;      // frames
    uint32_t offset;                  // first float of its rows (12 per frame)
    uint32_t pad;
};
static_assert(sizeof(NodeTrackEntry) == 32);

// Collision data of one mesh, as byte ranges in the Colliders section (0 bytes = none).
// meshBytes: static triangle mesh (a simplified copy of the render geometry; alpha-masked and
// blended materials are left out). hullBytes: convex hull (built-in primitives for now).
// Both are in mesh space and used in place (Box3D keeps pointers to them).
struct MeshCollider {
    uint32_t meshOffset, meshBytes;
    uint32_t hullOffset, hullBytes;
};
static_assert(sizeof(MeshCollider) == 16);

// Virtual texturing (.fvt companion file). Every textured material owns a run of page-table
// entries: mip 0 first, row-major pages of 128x128 texels. Level m is max(w >> m, 128) x
// max(h >> m, 128) texels; the last level is a single page and stays resident. Each page is
// stored with a 4-texel wrapped border (136x136) in three BC channels sharing one physical slot.
inline constexpr uint32_t kVtPageTexels = 128;
inline constexpr uint32_t kVtBorder = 4;
inline constexpr uint32_t kVtPhysicalTexels = kVtPageTexels + 2 * kVtBorder;  // 136
inline constexpr uint32_t kVtChannelBytes = (kVtPhysicalTexels / 4) * (kVtPhysicalTexels / 4) * 16;
inline constexpr uint32_t kVtChannels = 3;  // albedo+alpha (BC7 sRGB), normal xy (BC5), occlusion/roughness/metallic (BC7)
inline constexpr uint32_t kVtPageBytes = kVtChannelBytes * kVtChannels;
inline constexpr uint32_t kVtNoTexture = 0xffffffffu;

struct VtFileHeader {
    char magic[4];  // "FVT1"
    uint32_t pageCount;
    uint32_t pageBytes;
    uint32_t pad;
    uint64_t dataOffset;  // page e is at dataOffset + e * pageBytes
    uint64_t sourceHash;  // matches the .fpak header
};

// 24 bytes. Positions are quantized to the owning mesh's AABB (MeshGpu::posOffset/Scale).
struct VertexGpu {
    uint32_t posXY;       // u16 x | u16 y << 16
    uint32_t posZ;        // u16 z | u16 flags << 16 (bit 0: tangent w < 0)
    uint32_t normalOct;   // snorm16 x2 octahedral
    uint32_t tangentOct;  // snorm16 x2 octahedral
    float u, v;
};
static_assert(sizeof(VertexGpu) == 24);

// 80 bytes. Spheres are in mesh space.
// DAG cut: render iff lodError(self) <= threshold < lodError(parent).
struct ClusterGpu {
    float cullSphere[4];    // xyz center, w radius (tight, for culling)
    float selfLod[4];       // bounds of the group this cluster was simplified from (leaf: own bounds)
    float parentLod[4];     // bounds of the group this cluster belongs to (its simplified version)
    float selfError;        // 0 for full-detail clusters
    float parentError;      // FLT_MAX when no coarser version exists
    uint32_t vertexOffset;  // into ClusterVertices
    uint32_t triangleOffset;  // into ClusterTriangles
    uint32_t counts;        // vertexCount | triangleCount << 8 | material << 16
    uint32_t cone;          // backface cone: axis xyz s8, cutoff s8
    uint32_t depth;         // DAG level (0 = full detail)
    uint32_t pad;
};
static_assert(sizeof(ClusterGpu) == 80);

// 32 bytes. BVH node over cluster groups (meshoptimizer clodBuildHierarchy).
struct LodNodeGpu {
    float sphere[4];  // xyz center, w radius
    float error;
    int32_t group;          // >= 0: leaf; clusters [clusterOffset, +clusterCount)
    uint32_t childOffset;   // internal: children (absolute index); leaf: first cluster (absolute)
    uint32_t childCount;    // internal: child count; leaf: cluster count
};
static_assert(sizeof(LodNodeGpu) == 32);

// 48 bytes.
struct MeshGpu {
    float posOffset[4];  // xyz; w: 1 when one of its materials glows (set at load: ScenePak::open)
    float posScale[4];   // xyz; w unused
    uint32_t clusterOffset, clusterCount;
    uint32_t lodNodeOffset;  // first node; nodes [offset, offset + lodLevels) are per-level roots
    uint32_t lodLevels;
};
static_assert(sizeof(MeshGpu) == 48);

// 8 bytes. Bone indices into the mesh's skeleton; weights sum to 255.
struct SkinVertexGpu {
    uint8_t joints[4];
    uint8_t weights[4];
};
static_assert(sizeof(SkinVertexGpu) == 8);

inline constexpr uint32_t kNotSkinned = 0xffffffffu;

// 16 bytes. Vertex range of a mesh (all DAG levels share it) and, when skinned, its skin data.
struct MeshSkinGpu {
    uint32_t firstVertex, vertexCount;
    uint32_t firstSkinVertex;  // kNotSkinned for rigid meshes
    uint32_t skeleton;         // index into the animation section's skeletons
};
static_assert(sizeof(MeshSkinGpu) == 16);

enum InstanceFlags : uint32_t {
    kInstanceHidden = 1u << 0,   // unused slot of a dynamic instance pool: culled everywhere
    kInstanceSkinned = 1u << 1,  // InstanceGpu::skin indexes the frame's skinned-instance records
    // Shaded like the terrain (render/terrain_surface.h): with the terrain material layer in
    // InstanceGpu::skin bits 0..7, the ground's own material blended onto it up to bits 8..15
    // decimetres above the ground (rocks that match the cliffs around them).
    kInstanceTerrainShaded = 1u << 2,
    kInstanceNoShadow = 1u << 3,  // casts no shadow (scatter layers with shadows off)
    kInstanceScatter = 1u << 4,   // grown by a scatter layer, or all alpha-masked (set at load): thinned first when the GPU's budgets run out
    // Moved lately (set by the renderer while it streams instances): drawn over the shadow
    // pages' cached static layer instead of into it (render/virtual_shadow_map.h).
    kInstanceDynamic = 1u << 5,
    // Runtime only: a layered level mesh (render/surface_layers.h): InstanceGpu::skin is its layer
    // record; its vertices have the live meshes' extra stream (mask UVs, colours).
    kInstanceLayered = 1u << 6,
};
// Bits 16..31 of InstanceGpu::flags: a material the instance draws with instead of its clusters'
// (the material's index + 1; 0: the clusters'). Set by the editor (a material assigned).
inline constexpr uint32_t kInstanceMaterialShift = 16;

// 64 bytes. World transform as three rows of a 3x4 matrix.
struct InstanceGpu {
    float world[3][4];
    uint32_t mesh;
    uint32_t flags;  // InstanceFlags
    uint32_t tint;   // RGBA8 multiplier on the base colour; 0 = none (white)
    uint32_t skin;   // kInstanceSkinned: skinned-instance record (render/skinning.h); kInstanceTerrainShaded: layer, base blend
};
static_assert(sizeof(InstanceGpu) == 64);

enum MaterialFlags : uint32_t {
    kMaterialAlphaMask = 1u << 0,
    kMaterialAlphaBlend = 1u << 1,
    kMaterialDoubleSided = 1u << 2,
    kMaterialTerrain = 1u << 3,  // runtime only: the terrain's patches (shaded from their world position)
    kMaterialRefraction = 1u << 4,  // runtime only (T2): a blend material bending what is seen through it
    kMaterialVertex = 1u << 5,      // runtime only (V1): its surface shader has a vertex function
    // bits 8..15: its surface shader (runtime); bits 16..31: its parallax depth in metres, a half (MaterialAsset::parallax)
    // Bits 8..15: the project's surface shader (R4), set at runtime.
};

// 64 bytes. Texture channels multiply the factors (absent channels are cooked as white /
// flat normal, so shading never branches on them).
struct MaterialGpu {
    float baseColor[4];
    float metallic, roughness, alphaCutoff;
    uint32_t flags;
    uint32_t vtPageTable;  // first page-table entry, or kVtNoTexture
    uint32_t vtSize;       // log2(width) | log2(height) << 8 | mip count << 16 | roughness min (255ths) << 24
    float normalScale, occlusionStrength;
    float emissive[4];
};
static_assert(sizeof(MaterialGpu) == 64);

// MaterialGpu.vtSize's top byte: the roughness the roughness map's black gives (the map's white
// gives MaterialGpu.roughness), in 255ths. 0: the map times the roughness, as before it was there.
inline constexpr uint32_t kVtRoughnessMinMask = 0xff000000u;
inline uint32_t roughnessMinBits(float roughnessMin)
{
    const float r = roughnessMin > 0.0f ? (roughnessMin < 1.0f ? roughnessMin : 1.0f) : 0.0f;
    return uint32_t(r * 255.0f + 0.5f) << 24;
}
inline float roughnessMinOf(const MaterialGpu& m) { return float(m.vtSize >> 24) / 255.0f; }

inline uint32_t vtLevelPages(uint32_t log2Size, uint32_t mip)
{
    const uint32_t size = 1u << log2Size;
    const uint32_t s = (size >> mip) > kVtPageTexels ? (size >> mip) : kVtPageTexels;
    return s / kVtPageTexels;
}

enum LightType : uint32_t { kLightPoint = 0, kLightSpot = 1, kLightDirectional = 2 };

// 64 bytes. Intensity is folded into color (glTF units: candela for point/spot, lux for
// directional). Range 0 = derived from intensity at runtime.
struct LightGpu {
    float positionRange[4];  // xyz world position, w range
    float color[4];          // rgb * intensity, w type
    float direction[4];      // xyz direction the light points to, w cos(outer cone)
    float params[4];         // x cos(inner cone)
};
static_assert(sizeof(LightGpu) == 64);

}  // namespace fx::fpak
