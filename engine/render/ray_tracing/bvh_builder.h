#pragma once

// Bounding volume hierarchies for the compute-shader ray tracer (ray_trace.slang), built on the
// CPU: a binned surface-area-heuristic builder over boxes, used for each mesh's triangles (the
// bottom level) and for the scene's instances (the top level).
//
// GPU layout (mirrored in ray_trace.slang): two-wide nodes of 64 bytes that hold both children's
// boxes, so one step of the traversal tests two boxes and descends into what it hit. A child is
// another node (its index) or a leaf (kLeafBit | first << kLeafShift | count): `count` primitives
// from `first` in the primitive order the build returns. An empty child has a box far away (1e30 on every axis), which no slab test enters.
// Nodes are numbered depth first, so a node's children always come after it (refit walks
// backwards).

#include <cstdint>
#include <vector>

namespace fx::rt {

constexpr uint32_t kLeafBit = 0x80000000u;
constexpr uint32_t kLeafShift = 3;
constexpr uint32_t kLeafCountMask = (1u << kLeafShift) - 1;  // up to 7 primitives a leaf
constexpr uint32_t kMaxLeafFirst = (kLeafBit >> kLeafShift) - 1;

struct Aabb {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    void grow(const float p[3]);
    void grow(const Aabb& b);
    bool empty() const { return lo[0] > hi[0]; }
    float area() const;  // half the surface area (the heuristic only compares them)
};

struct BvhNodeGpu {
    float leftLo[3], leftHi[3];
    float rightLo[3], rightHi[3];
    uint32_t left, right;  // child: node index, or kLeafBit | first << kLeafShift | count
    uint32_t pad[2];
};
static_assert(sizeof(BvhNodeGpu) == 64);

// A triangle as the tracer reads it: positions in mesh space, and where it came from in the
// scene buffer (cluster << 7 | triangle in the cluster), for the hit's UVs, normal and material.
struct BvhTriangleGpu {
    float v0[3], v1[3], v2[3];
    uint32_t id;
};
static_assert(sizeof(BvhTriangleGpu) == 40);

inline uint32_t triangleId(uint32_t cluster, uint32_t triangle) { return cluster << 7 | triangle; }

struct Bvh {
    std::vector<BvhNodeGpu> nodes;  // [0] is the root (always a node, even for one leaf)
    std::vector<uint32_t> order;    // primitive order: leaves index into this
    float cost = 0.0f;              // surface-area cost when built (refits compare against it)
};

// Builds over `count` primitive boxes. `maxLeaf` primitives a leaf at most (1..7). Empty input:
// one node with two empty children.
void build(const Aabb* boxes, uint32_t count, uint32_t maxLeaf, Bvh& out);
// Recomputes every node's child boxes from new primitive boxes (same count, same topology).
void refit(const Aabb* boxes, Bvh& bvh);
// Surface-area cost of the tree as it is (traversal steps + primitive tests, per unit area of the
// root): a refit tree whose cost has grown well past its build's wants rebuilding.
float sahCost(const Bvh& bvh, uint32_t primitiveCost = 1);

// Builds a triangle BVH and reorders `triangles` into its leaf order (the order array dropped).
void buildTriangles(std::vector<BvhTriangleGpu>& triangles, Bvh& out);
Aabb rootBounds(const Bvh& bvh);

// CPU reference of the GPU traversal (tests): the closest hit along origin + t * dir, t in
// (0, tMax). Returns the triangle index (in `triangles`) or -1, with t and barycentrics.
struct Hit {
    float t = 0.0f, u = 0.0f, v = 0.0f;
    bool backface = false;
};
int32_t traceClosest(const Bvh& bvh, const BvhTriangleGpu* triangles, const float origin[3], const float dir[3],
                     float tMax, Hit& hit);
bool intersectTriangle(const BvhTriangleGpu& tri, const float origin[3], const float dir[3], float tMax, Hit& hit);

}  // namespace fx::rt
