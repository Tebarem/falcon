#include "render/ray_tracing/bvh_builder.h"

#include <algorithm>
#include <cmath>

namespace fx::rt {

void Aabb::grow(const float p[3])
{
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], p[a]);
        hi[a] = std::max(hi[a], p[a]);
    }
}

void Aabb::grow(const Aabb& b)
{
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], b.lo[a]);
        hi[a] = std::max(hi[a], b.hi[a]);
    }
}

float Aabb::area() const
{
    if (empty())
        return 0.0f;
    const float x = hi[0] - lo[0], y = hi[1] - lo[1], z = hi[2] - lo[2];
    return x * y + y * z + z * x;
}

namespace {

constexpr uint32_t kBins = 16;
// Past this depth the builder halves by count instead of by area, so no tree gets deeper than
// about this plus log2 of what is left: the GPU traversal's stack is a fixed size.
constexpr uint32_t kSahDepth = 36;
constexpr uint32_t kEmptyChild = kLeafBit;  // a leaf of nothing (its box inverted)

// An empty child's box is a point far away (1e30 on every axis): a slab test never enters it,
// whichever way the ray points (an inverted box would pass the test).
constexpr float kFar = 1e30f;

void setBox(float lo[3], float hi[3], const Aabb& b)
{
    for (int a = 0; a < 3; ++a) {
        lo[a] = b.empty() ? kFar : b.lo[a];
        hi[a] = b.empty() ? kFar : b.hi[a];
    }
}

Aabb boxOf(const float lo[3], const float hi[3])
{
    Aabb b;
    if (lo[0] >= 0.5f * kFar)
        return b;
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = lo[a];
        b.hi[a] = hi[a];
    }
    return b;
}

struct Builder {
    const Aabb* boxes;
    std::vector<float> centers;  // 3 per primitive
    std::vector<uint32_t>& order;
    std::vector<BvhNodeGpu>& nodes;
    uint32_t maxLeaf;

    float center(uint32_t p, int a) const { return centers[size_t(p) * 3 + a]; }

    uint32_t leaf(uint32_t begin, uint32_t end) const { return kLeafBit | begin << kLeafShift | (end - begin); }

    Aabb bounds(uint32_t begin, uint32_t end) const
    {
        Aabb b;
        for (uint32_t i = begin; i < end; ++i)
            b.grow(boxes[order[i]]);
        return b;
    }

    // Where to split [begin, end): the index the right half starts at (the order partitioned).
    uint32_t split(uint32_t begin, uint32_t end, uint32_t depth)
    {
        Aabb cb;
        for (uint32_t i = begin; i < end; ++i) {
            const float c[3] = {center(order[i], 0), center(order[i], 1), center(order[i], 2)};
            cb.grow(c);
        }
        int bestAxis = -1;
        uint32_t bestBin = 0;
        float bestCost = 1e38f;
        if (depth < kSahDepth) {
            for (int a = 0; a < 3; ++a) {
                const float extent = cb.hi[a] - cb.lo[a];
                if (!(extent > 1e-12f))
                    continue;
                Aabb bin[kBins];
                uint32_t count[kBins] = {};
                const float k = float(kBins) / extent;
                for (uint32_t i = begin; i < end; ++i) {
                    const uint32_t b = std::min(uint32_t((center(order[i], a) - cb.lo[a]) * k), kBins - 1);
                    bin[b].grow(boxes[order[i]]);
                    ++count[b];
                }
                float rightArea[kBins];
                uint32_t rightCount[kBins];
                Aabb acc;
                uint32_t n = 0;
                for (uint32_t b = kBins - 1; b > 0; --b) {
                    acc.grow(bin[b]);
                    n += count[b];
                    rightArea[b] = acc.area();
                    rightCount[b] = n;
                }
                acc = Aabb{};
                n = 0;
                for (uint32_t b = 0; b + 1 < kBins; ++b) {
                    acc.grow(bin[b]);
                    n += count[b];
                    if (n == 0 || rightCount[b + 1] == 0)
                        continue;
                    const float cost = acc.area() * float(n) + rightArea[b + 1] * float(rightCount[b + 1]);
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestAxis = a;
                        bestBin = b + 1;
                    }
                }
            }
        }
        if (bestAxis < 0) {
            // All centres in one place, or too deep: halve by count along the widest axis.
            int a = 0;
            for (int i = 1; i < 3; ++i)
                if (cb.hi[i] - cb.lo[i] > cb.hi[a] - cb.lo[a])
                    a = i;
            const uint32_t mid = begin + (end - begin) / 2;
            std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                             [&](uint32_t x, uint32_t y) { return center(x, a) < center(y, a); });
            return mid;
        }
        const float k = float(kBins) / (cb.hi[bestAxis] - cb.lo[bestAxis]);
        const auto mid = std::partition(order.begin() + begin, order.begin() + end, [&](uint32_t p) {
            return std::min(uint32_t((center(p, bestAxis) - cb.lo[bestAxis]) * k), kBins - 1) < bestBin;
        });
        return uint32_t(mid - order.begin());
    }

    // A child for [begin, end): a leaf, or a new node (numbered before its own children).
    uint32_t child(uint32_t begin, uint32_t end, uint32_t depth, Aabb& box)
    {
        if (end - begin <= maxLeaf) {
            box = bounds(begin, end);
            return leaf(begin, end);
        }
        const uint32_t index = uint32_t(nodes.size());
        nodes.push_back({});
        const uint32_t mid = split(begin, end, depth);
        Aabb l, r;
        const uint32_t left = child(begin, mid, depth + 1, l);
        const uint32_t right = child(mid, end, depth + 1, r);
        BvhNodeGpu& n = nodes[index];
        setBox(n.leftLo, n.leftHi, l);
        setBox(n.rightLo, n.rightHi, r);
        n.left = left;
        n.right = right;
        box = l;
        box.grow(r);
        return index;
    }
};

bool isLeaf(uint32_t c) { return (c & kLeafBit) != 0; }
uint32_t leafFirst(uint32_t c) { return (c & ~kLeafBit) >> kLeafShift; }
uint32_t leafCount(uint32_t c) { return c & kLeafCountMask; }

// Slab test: entry distance, or a miss (false).
bool hitBox(const float lo[3], const float hi[3], const float origin[3], const float inv[3], float tMax, float& tNear)
{
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float n = (lo[a] - origin[a]) * inv[a], f = (hi[a] - origin[a]) * inv[a];
        if (n > f)
            std::swap(n, f);
        t0 = std::max(t0, n);
        t1 = std::min(t1, f);
    }
    tNear = t0;
    return t0 <= t1;
}

}  // namespace

void build(const Aabb* boxes, uint32_t count, uint32_t maxLeaf, Bvh& out)
{
    maxLeaf = std::clamp(maxLeaf, 1u, kLeafCountMask);
    out.nodes.clear();
    out.order.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        out.order[i] = i;
    Builder b{boxes, {}, out.order, out.nodes, maxLeaf};
    b.centers.resize(size_t(count) * 3);
    for (uint32_t i = 0; i < count; ++i)
        for (int a = 0; a < 3; ++a)
            b.centers[size_t(i) * 3 + a] = 0.5f * (boxes[i].lo[a] + boxes[i].hi[a]);
    out.nodes.reserve(count ? size_t(count) * 2 / std::max(1u, maxLeaf) + 1 : 1);
    if (count <= maxLeaf) {
        BvhNodeGpu root = {};
        const Aabb box = b.bounds(0, count);
        setBox(root.leftLo, root.leftHi, box);
        setBox(root.rightLo, root.rightHi, Aabb{});
        root.left = count ? b.leaf(0, count) : kEmptyChild;
        root.right = kEmptyChild;
        out.nodes.push_back(root);
    } else {
        Aabb box;
        b.child(0, count, 0, box);
    }
    out.cost = sahCost(out);
}

void refit(const Aabb* boxes, Bvh& bvh)
{
    auto childBox = [&](uint32_t c) {
        Aabb box;
        if (isLeaf(c)) {
            for (uint32_t i = leafFirst(c), e = i + leafCount(c); i < e; ++i)
                box.grow(boxes[bvh.order[i]]);
        } else {
            const BvhNodeGpu& n = bvh.nodes[c];
            box = boxOf(n.leftLo, n.leftHi);
            box.grow(boxOf(n.rightLo, n.rightHi));
        }
        return box;
    };
    for (size_t i = bvh.nodes.size(); i-- > 0;) {
        BvhNodeGpu& n = bvh.nodes[i];
        setBox(n.leftLo, n.leftHi, childBox(n.left));
        setBox(n.rightLo, n.rightHi, childBox(n.right));
    }
}

float sahCost(const Bvh& bvh, uint32_t primitiveCost)
{
    if (bvh.nodes.empty())
        return 0.0f;
    const float root = rootBounds(bvh).area();
    if (!(root > 0.0f))
        return 0.0f;
    double cost = 0.0;
    for (const BvhNodeGpu& n : bvh.nodes) {
        const Aabb l = boxOf(n.leftLo, n.leftHi), r = boxOf(n.rightLo, n.rightHi);
        Aabb u = l;
        u.grow(r);
        cost += u.area();
        if (isLeaf(n.left))
            cost += l.area() * leafCount(n.left) * primitiveCost;
        if (isLeaf(n.right))
            cost += r.area() * leafCount(n.right) * primitiveCost;
    }
    return float(cost / root);
}

Aabb rootBounds(const Bvh& bvh)
{
    if (bvh.nodes.empty())
        return {};
    const BvhNodeGpu& n = bvh.nodes[0];
    Aabb b = boxOf(n.leftLo, n.leftHi);
    b.grow(boxOf(n.rightLo, n.rightHi));
    return b;
}

void buildTriangles(std::vector<BvhTriangleGpu>& triangles, Bvh& out)
{
    std::vector<Aabb> boxes(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i) {
        boxes[i].grow(triangles[i].v0);
        boxes[i].grow(triangles[i].v1);
        boxes[i].grow(triangles[i].v2);
    }
    build(boxes.data(), uint32_t(triangles.size()), 4, out);
    std::vector<BvhTriangleGpu> sorted(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i)
        sorted[i] = triangles[out.order[i]];
    triangles.swap(sorted);
    out.order.clear();
    out.order.shrink_to_fit();
}

bool intersectTriangle(const BvhTriangleGpu& tri, const float origin[3], const float dir[3], float tMax, Hit& hit)
{
    // Moller-Trumbore, edges inclusive (a ray down a shared edge hits one of its triangles).
    float e1[3], e2[3], s[3];
    for (int a = 0; a < 3; ++a) {
        e1[a] = tri.v1[a] - tri.v0[a];
        e2[a] = tri.v2[a] - tri.v0[a];
        s[a] = origin[a] - tri.v0[a];
    }
    const float p[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-20f)
        return false;
    const float inv = 1.0f / det;
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    if (!(t > 0.0f && t < tMax))
        return false;
    hit.t = t;
    hit.u = u;
    hit.v = v;
    hit.backface = det < 0.0f;  // (counter-clockwise front faces, as the rasterizer culls)
    return true;
}

int32_t traceClosest(const Bvh& bvh, const BvhTriangleGpu* triangles, const float origin[3], const float dir[3],
                     float tMax, Hit& hit)
{
    if (bvh.nodes.empty())
        return -1;
    float inv[3];
    for (int a = 0; a < 3; ++a)
        inv[a] = 1.0f / (std::fabs(dir[a]) > 1e-20f ? dir[a] : std::copysign(1e-20f, dir[a]));
    int32_t best = -1;
    uint32_t stack[96];
    uint32_t sp = 0, node = 0;
    for (;;) {
        const BvhNodeGpu& n = bvh.nodes[node];
        float tl = 0.0f, tr = 0.0f;
        const bool hl = hitBox(n.leftLo, n.leftHi, origin, inv, tMax, tl);
        const bool hr = hitBox(n.rightLo, n.rightHi, origin, inv, tMax, tr);
        uint32_t next[2];
        uint32_t nNext = 0;
        // Leaves at once, nodes nearest first.
        const uint32_t kids[2] = {tl <= tr ? n.left : n.right, tl <= tr ? n.right : n.left};
        const bool hits[2] = {tl <= tr ? hl : hr, tl <= tr ? hr : hl};
        for (int k = 0; k < 2; ++k) {
            if (!hits[k])
                continue;
            const uint32_t c = kids[k];
            if (isLeaf(c)) {
                for (uint32_t i = leafFirst(c), e = i + leafCount(c); i < e; ++i)
                    if (intersectTriangle(triangles[i], origin, dir, tMax, hit)) {
                        tMax = hit.t;
                        best = int32_t(i);
                    }
            } else {
                next[nNext++] = c;
            }
        }
        if (nNext == 2) {
            stack[sp++] = next[1];
            node = next[0];
        } else if (nNext == 1) {
            node = next[0];
        } else {
            if (sp == 0)
                break;
            node = stack[--sp];
        }
    }
    return best;  // (hit holds the last, nearest success)
}

}  // namespace fx::rt
