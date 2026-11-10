// Tests of the ray tracing BVH builder (engine/render/ray_tracing/bvh_builder.*), on the CPU:
// the GPU traversal's logic (rt::traceClosest mirrors ray_trace.slang) against brute force.
//   falcon_bvh_test soup      random triangles, random rays: the same nearest hit
//   falcon_bvh_test stacked   thousands of triangles in one place: the tree stays shallow enough
//                             for the GPU's stack, and still finds every hit
//   falcon_bvh_test refit     triangles moved, the tree refit: the same hits as brute force
//   falcon_bvh_test edges     rays down shared edges of a grid always hit (no cracks)
//   falcon_bvh_test small     no triangles, one triangle

#include "render/ray_tracing/bvh_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace fx::rt;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

BvhTriangleGpu triangle(const float a[3], const float b[3], const float c[3], uint32_t id)
{
    BvhTriangleGpu t;
    std::memcpy(t.v0, a, 12);
    std::memcpy(t.v1, b, 12);
    std::memcpy(t.v2, c, 12);
    t.id = id;
    return t;
}

std::vector<BvhTriangleGpu> soup(std::mt19937& rng, uint32_t n, float extent, float size)
{
    std::uniform_real_distribution<float> at(-extent, extent), off(-size, size);
    std::vector<BvhTriangleGpu> out;
    for (uint32_t i = 0; i < n; ++i) {
        const float c[3] = {at(rng), at(rng), at(rng)};
        float v[3][3];
        for (auto& p : v)
            for (int a = 0; a < 3; ++a)
                p[a] = c[a] + off(rng);
        out.push_back(triangle(v[0], v[1], v[2], i));
    }
    return out;
}

// The nearest hit by testing every triangle: its t, or -1.
float bruteForce(const std::vector<BvhTriangleGpu>& tris, const float o[3], const float d[3], float tMax)
{
    float best = -1.0f;
    for (const BvhTriangleGpu& t : tris) {
        Hit h;
        if (intersectTriangle(t, o, d, best >= 0.0f ? best : tMax, h))
            best = h.t;
    }
    return best;
}

void randomRay(std::mt19937& rng, float extent, float o[3], float d[3])
{
    std::uniform_real_distribution<float> at(-extent, extent), dir(-1.0f, 1.0f);
    for (int a = 0; a < 3; ++a)
        o[a] = at(rng);
    float l = 0.0f;
    do {
        for (int a = 0; a < 3; ++a)
            d[a] = dir(rng);
        l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    } while (l < 0.1f);
    for (int a = 0; a < 3; ++a)
        d[a] /= l;
}

// Compares `rays` random rays traced through the tree with brute force; returns the mismatches.
int compare(const Bvh& bvh, const std::vector<BvhTriangleGpu>& tris, std::mt19937& rng, float extent, int rays)
{
    int bad = 0;
    for (int r = 0; r < rays; ++r) {
        float o[3], d[3];
        randomRay(rng, extent, o, d);
        Hit h;
        const int32_t found = traceClosest(bvh, tris.data(), o, d, 1e30f, h);
        const float expect = bruteForce(tris, o, d, 1e30f);
        const bool same = found < 0 ? expect < 0.0f : (expect >= 0.0f && std::fabs(h.t - expect) <= 1e-4f * std::max(1.0f, expect));
        if (!same && bad++ < 5)
            std::printf("  ray %d: bvh %s %g, brute force %g\n", r, found < 0 ? "miss" : "hit", double(found < 0 ? 0.0f : h.t),
                        double(expect));
    }
    return bad;
}

uint32_t depth(const Bvh& bvh, uint32_t node = 0)
{
    const BvhNodeGpu& n = bvh.nodes[node];
    uint32_t d = 1;
    for (uint32_t c : {n.left, n.right})
        if (!(c & kLeafBit))
            d = std::max(d, 1 + depth(bvh, c));
    return d;
}

void soupTest()
{
    std::mt19937 rng(1);
    for (uint32_t n : {7u, 100u, 5000u}) {
        std::vector<BvhTriangleGpu> tris = soup(rng, n, 10.0f, 1.0f);
        Bvh bvh;
        buildTriangles(tris, bvh);
        check(depth(bvh) < 48, "soup: tree shallower than the GPU stack");
        const int bad = compare(bvh, tris, rng, 12.0f, 2000);
        std::printf("soup %u: %zu nodes, depth %u, cost %.1f, %d mismatches\n", n, bvh.nodes.size(), depth(bvh), double(bvh.cost), bad);
        check(bad == 0, "soup: same hits as brute force");
    }
}

void stackedTest()
{
    // 4000 copies of almost the same triangle plus a few elsewhere: the centres can't be split by
    // area, so the builder halves by count.
    std::mt19937 rng(2);
    std::vector<BvhTriangleGpu> tris;
    const float a[3] = {0, 0, 0}, b[3] = {1, 0, 0}, c[3] = {0, 1, 0};
    for (uint32_t i = 0; i < 4000; ++i) {
        BvhTriangleGpu t = triangle(a, b, c, i);
        t.v0[2] = t.v1[2] = t.v2[2] = float(i) * 1e-6f;
        tris.push_back(t);
    }
    std::vector<BvhTriangleGpu> more = soup(rng, 50, 5.0f, 0.5f);
    tris.insert(tris.end(), more.begin(), more.end());
    Bvh bvh;
    buildTriangles(tris, bvh);
    std::printf("stacked: %zu nodes, depth %u\n", bvh.nodes.size(), depth(bvh));
    check(depth(bvh) < 48, "stacked: tree shallower than the GPU stack");
    check(compare(bvh, tris, rng, 6.0f, 2000) == 0, "stacked: same hits as brute force");
}

void refitTest()
{
    std::mt19937 rng(3);
    std::vector<BvhTriangleGpu> tris = soup(rng, 2000, 10.0f, 0.8f);
    std::vector<Aabb> boxes(tris.size());
    auto boxesOf = [&] {
        for (size_t i = 0; i < tris.size(); ++i) {
            boxes[i] = {};
            boxes[i].grow(tris[i].v0);
            boxes[i].grow(tris[i].v1);
            boxes[i].grow(tris[i].v2);
        }
    };
    boxesOf();
    Bvh bvh;
    build(boxes.data(), uint32_t(boxes.size()), 4, bvh);
    // Move every triangle (a different shift each), refit, trace the leaf-ordered copy.
    std::uniform_real_distribution<float> shift(-3.0f, 3.0f);
    for (BvhTriangleGpu& t : tris) {
        const float s[3] = {shift(rng), shift(rng), shift(rng)};
        for (float* p : {t.v0, t.v1, t.v2})
            for (int a = 0; a < 3; ++a)
                p[a] += s[a];
    }
    boxesOf();
    refit(boxes.data(), bvh);
    std::vector<BvhTriangleGpu> ordered(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        ordered[i] = tris[bvh.order[i]];
    const float before = bvh.cost, after = sahCost(bvh);
    const int bad = compare(bvh, ordered, rng, 13.0f, 2000);
    std::printf("refit: cost %.1f built, %.1f refit, %d mismatches\n", double(before), double(after), bad);
    check(bad == 0, "refit: same hits as brute force");
    check(after > before, "refit: scattered triangles cost more than the tree built for them");
}

void edgesTest()
{
    // A 64 x 64 grid of quads (two triangles each) on z = 0: rays straight down through every
    // vertex and edge midpoint must hit.
    std::vector<BvhTriangleGpu> tris;
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) {
            const float x0 = float(i) * 0.25f, y0 = float(j) * 0.25f, x1 = x0 + 0.25f, y1 = y0 + 0.25f;
            const float a[3] = {x0, y0, 0}, b[3] = {x1, y0, 0}, c[3] = {x1, y1, 0}, d[3] = {x0, y1, 0};
            tris.push_back(triangle(a, b, c, uint32_t(tris.size())));
            tris.push_back(triangle(a, c, d, uint32_t(tris.size())));
        }
    Bvh bvh;
    buildTriangles(tris, bvh);
    int misses = 0;
    for (int j = 1; j < 128; ++j)
        for (int i = 1; i < 128; ++i) {
            const float o[3] = {float(i) * 0.125f, float(j) * 0.125f, 1.0f}, d[3] = {0, 0, -1};
            Hit h;
            if (traceClosest(bvh, tris.data(), o, d, 10.0f, h) < 0)
                ++misses;
        }
    std::printf("edges: %d misses\n", misses);
    check(misses == 0, "edges: rays down shared edges and vertices hit");
}

void smallTest()
{
    std::vector<BvhTriangleGpu> none;
    Bvh bvh;
    buildTriangles(none, bvh);
    check(bvh.nodes.size() == 1, "small: an empty tree has its root");
    const float o[3] = {0, 0, 5}, d[3] = {0, 0, -1};
    Hit h;
    check(traceClosest(bvh, none.data(), o, d, 1e30f, h) < 0, "small: an empty tree hits nothing");
    const float a[3] = {-1, -1, 0}, b[3] = {1, -1, 0}, c[3] = {0, 1, 0};
    std::vector<BvhTriangleGpu> one = {triangle(a, b, c, 0)};
    buildTriangles(one, bvh);
    check(traceClosest(bvh, one.data(), o, d, 1e30f, h) == 0 && std::fabs(h.t - 5.0f) < 1e-5f, "small: one triangle is hit at 5");
    check(!h.backface, "small: seen from +z, a counter-clockwise triangle shows its front");
    const float o2[3] = {3, 3, 5};
    check(traceClosest(bvh, one.data(), o2, d, 1e30f, h) < 0, "small: a ray beside it misses");
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string which = argc > 1 ? argv[1] : "all";
    bool ran = false;
    auto run = [&](const char* name, void (*test)()) {
        if (which == "all" || which == name) {
            test();
            ran = true;
        }
    };
    run("soup", soupTest);
    run("stacked", stackedTest);
    run("refit", refitTest);
    run("edges", edgesTest);
    run("small", smallTest);
    if (!ran) {
        std::printf("unknown test: %s\n", which.c_str());
        return 2;
    }
    std::printf(failures ? "%d failures\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
