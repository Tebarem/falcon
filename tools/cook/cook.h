#pragma once

#include "anim/anim_format.h"
#include "anim/anim_graph_compiler.h"
#include "assets/fpak_format.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fx::cook {

// Full-precision vertex as imported. Positions first, then the attributes the
// simplifier weighs (normal) and protects (uv), see buildMesh().
struct SrcVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, tw;
    // Skinning (skinned primitives only; zero otherwise): skeleton bone indices + weights.
    // Part of the vertex so welding/remapping keeps them attached.
    uint16_t joints[4];
    float weights[4];
};

struct SrcPrimitive {
    std::vector<SrcVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t material = 0;
    int32_t skeleton = -1;  // skinned: index into SrcScene::skeletons
};

struct SrcInstance {
    float world[16];  // column-major; skinned: the skeleton's armature (model) space
    uint32_t primitive;
    int32_t node = -1;  // its glTF node
    std::string name;  // glTF node name ("Node 12" when unnamed), "/<n>" per extra primitive
};

// Local (parent-space) transform as glTF stores it.
struct SrcTransform {
    float t[3] = {0, 0, 0};
    float r[4] = {0, 0, 0, 1};  // x, y, z, w
    float s[3] = {1, 1, 1};
};

// One glTF skin. Bone 0 is a synthetic root standing for the armature (model) space; every
// other bone is a joint, ordered so that parents come before their children.
struct SrcSkeleton {
    std::string name;
    std::vector<std::string> boneNames;
    std::vector<int32_t> parents;              // -1 for bone 0
    std::vector<SrcTransform> reference;       // rest pose, parent space
    std::vector<std::array<float, 16>> inverseBind;  // column-major, model space -> bone space
    std::vector<int32_t> nodes;                // glTF node of each bone (-1: synthetic root)
};

// An animation sampled at a fixed rate into local transforms (frames x bones).
struct SrcClip {
    std::string name;
    uint32_t skeleton = 0;
    float duration = 0.0f;  // seconds; frame f is at f * duration / (frameCount - 1)
    uint32_t frameCount = 0;
    std::vector<SrcTransform> frames;
};

// An encoded image (PNG/JPEG): a file path, or bytes embedded in the glTF buffers.
struct SrcImage {
    std::string path;
    std::vector<uint8_t> bytes;
};

// Source images (indices into SrcScene::images, -1 = none) for one material.
struct SrcMaterialTextures {
    int baseColor = -1, normal = -1, metallicRoughness = -1, occlusion = -1;
    int height = -1;  // a material asset's height map: the ORM pages' alpha (decals' parallax); 1 without
    bool any() const { return baseColor >= 0 || normal >= 0 || metallicRoughness >= 0 || occlusion >= 0 || height >= 0; }
};

// A glTF camera (perspective): its node's world transform (column-major; it looks down -Z).
struct SrcCamera {
    std::string name;
    float yfov = 0.8f;  // radians
    float world[16];
    int32_t node = -1;
};

// A moving mesh node or camera (rigid node animation, sampled at SrcScene::nodeFps): its world
// transform at frames [first, first + count) of the loop, rows in SrcScene::nodeTrackData.
struct SrcNodeTrack {
    uint32_t target;  // instance (or camera) index
    bool camera = false;
    uint32_t first = 0, count = 0;
    uint32_t data = 0;  // index into nodeTrackData (shared by a node's primitives)
};

struct SrcScene {
    std::vector<SrcPrimitive> primitives;
    std::vector<SrcInstance> instances;
    std::vector<fpak::MaterialGpu> materials;
    std::vector<SrcMaterialTextures> materialTextures;  // parallel to materials
    std::vector<SrcImage> images;
    std::vector<fpak::LightGpu> lights;
    std::vector<SrcSkeleton> skeletons;
    std::vector<SrcClip> clips;
    std::vector<SrcCamera> cameras;
    std::vector<SrcNodeTrack> nodeTracks;
    std::vector<std::vector<float>> nodeTrackData;  // 12 floats (3x4 row-major world) per frame
    uint32_t nodeFrames = 0;  // the loop
    float nodeFps = 30.0f;
};

// An animation graph compiled from JSON (anim/anim_graph_compiler.h, against the scene: build_graph.cpp).
using CompiledGraph = anim::CompiledGraph;

bool compileGraph(const SrcScene& scene, const std::string& path, CompiledGraph& out);

// Decodes every clip frame with the runtime sampler and compares it with the source pose.
// Returns the number of violations (and logs them).
size_t verifyAnimation(const SrcScene& scene, const std::vector<uint8_t>& section);

// Virtual texture layout of one material, decided before any page is encoded.
struct VtLayout {
    uint32_t log2Width = 0, log2Height = 0, mipCount = 0;
    uint32_t firstPage = 0, pageCount = 0;
};

// One primitive cooked into GPU-ready arrays. Offsets are local; the writer rebases them.
struct BuiltMesh {
    std::vector<fpak::VertexGpu> vertices;
    std::vector<uint32_t> clusterVertices;
    std::vector<uint32_t> clusterTriangles;
    std::vector<fpak::ClusterGpu> clusters;
    std::vector<fpak::LodNodeGpu> lodNodes;
    std::vector<fpak::SkinVertexGpu> skin;  // skinned meshes: one per vertex
    fpak::MeshGpu mesh = {};
    uint32_t leafTriangles = 0;
    uint32_t totalTriangles = 0;
    uint32_t levels = 0;
    float maxQuantError = 0.0f;  // largest |dequantized - original| position delta
    // The mesh's own extent. Its quantization range can be larger: pruned foliage levels add
    // widened copies of its pieces (buildMesh), which should not grow the scene's bounds.
    float surfaceMin[3] = {}, surfaceMax[3] = {};
};

bool importGltf(const std::string& path, SrcScene& scene, std::vector<std::string>& sourceFiles);

// Picks each textured material's virtual texture size (power of two, clamped to
// [128, maxSize]) from its source image dimensions. Untextured materials get no pages.
std::vector<VtLayout> layoutVirtualTextures(const SrcScene& scene, uint32_t maxSize, uint32_t& totalPages);

// Materials encoded so far (progress), and how many of them were copied from older pages.
struct VtProgress {
    std::atomic<uint32_t> done{0}, reused{0};
    uint32_t total = 0;
};

// Decodes, resizes, mips, borders and BC-encodes every page of every textured material,
// writing them into `out` at fixed offsets (deterministic, threads in any order). Alpha-masked
// materials whose texture never drops below the cutoff are downgraded to opaque.
//
// The .fvt ends with each material's content key (a hash of its images' bytes, its layout and
// its alpha settings) and where its pages are. A material whose key is in one of the `reuse`
// files (earlier cooks: this scene's, the project's other scenes') has its pages copied from
// there instead of encoded again, so a re-cook only encodes new or changed textures.
bool encodeVirtualTextures(SrcScene& scene, const std::vector<VtLayout>& layouts, uint32_t totalPages,
                           uint64_t sourceHash, const std::string& outPath, unsigned threads,
                           const std::vector<std::string>& reuse, VtProgress& progress);
// foliage: the primitive's material is alpha-masked and double-sided (grass, leaf cards): no
// simplified LOD levels. Simplification measures geometric error, and collapsing a blade a
// centimetre wide into a line costs almost none, so the coarser levels lost whole blades and
// grass thinned out to bare ground with distance.
BuiltMesh buildMesh(const SrcPrimitive& prim, bool foliage = false);

// Box3D collision blobs of one primitive (empty when not applicable).
struct BuiltCollider {
    std::vector<uint8_t> mesh;  // b3MeshData: simplified static triangle mesh
    std::vector<uint8_t> hull;  // b3HullData: convex hull (built-in primitives only, for now)
    uint32_t meshTriangles = 0;
};

// Skeletons + clips -> the .fpak Animation section (Esoterica-style compressed clips).
std::vector<uint8_t> buildAnimation(const SrcScene& scene, const std::vector<CompiledGraph>& graphs);

// Adds the built-in meshes (fpak::Builtin order; the editor creates them, dynamic bodies use the
// cube and sphere) and their shared material. `meshes` receives each one's primitive index.
void appendBuiltinPrimitives(SrcScene& scene, uint32_t meshes[uint32_t(fpak::Builtin::Count)], uint32_t& material);
// Procedural rocks (build_rocks.cpp): boulders, slabs, spires, an arch; `names` gets theirs.
void appendRockPrimitives(SrcScene& scene, std::vector<std::string>& names);
BuiltCollider buildCollider(const SrcPrimitive& prim, const fpak::MaterialGpu& material, bool builtin);

// Height displacement (build_displace.cpp): a grey height map (0..1) and what a material asset
// says to do with it.
struct HeightMap {
    std::vector<float> values;  // row-major, the first channel (16-bit maps keep their precision)
    int width = 0, height = 0;
};
struct DisplaceParams {
    float amount = 0.0f;        // metres at height 1 (minus center)
    float center = 0.0f;        // the height that stays on the surface
    float detail = 0.0f;        // target edge length, metres (0: about one texel of the map)
    uint32_t maxTriangles = 1u << 20;
};
bool loadHeightMap(const std::string& path, HeightMap& out);
// The primitive subdivided and pushed out along its normal (watertight across seams and hard
// edges), with new normals and tangents. `note`: what was changed to fit (the triangle budget).
SrcPrimitive displacePrimitive(const SrcPrimitive& src, const HeightMap& map, const DisplaceParams& params, std::string& note);

// Structural checks on a cooked mesh: bounds containment, monotonic DAG error,
// quantization error bound. Returns the number of violations (and logs them).
size_t verifyMesh(const SrcPrimitive& prim, const BuiltMesh& mesh, uint32_t meshIndex);

}  // namespace fx::cook
