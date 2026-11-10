// Snap points of modular kit pieces (level_meshes.h SnapPoint): a piece moved near another's snap
// point clicks onto it. Its own snap point (one in its subtree) nearest another's (outside it, of a
// matching tag, within kReach) is put on that one, turned to face it (its +z against the other's,
// +y with +y): the whole piece moved and turned with it.

#include "editor/editor_bridge.h"

#include "editor/scene_view.h"
#include "level/level_meshes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

constexpr float kReach = 0.4f;  // metres

void multiply(const float a[3][4], const float b[3][4], float out[3][4])
{
    float r[3][4];
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 4; ++c)
            r[row][c] = a[row][0] * b[0][c] + a[row][1] * b[1][c] + a[row][2] * b[2][c] + (c == 3 ? a[row][3] : 0.0f);
    std::memcpy(out, r, sizeof(r));
}

bool invert(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20f)
        return false;
    const float s = 1.0f / det;
    const float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                           {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                           {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
    return true;
}

// A frame's rotation without its scale (its columns made unit length).
void unitAxes(const float m[3][4], float3 axes[3])
{
    for (int c = 0; c < 3; ++c) {
        const float3 v = {m[0][c], m[1][c], m[2][c]};
        axes[c] = length(v) > 1e-8f ? normalize(v) : float3{c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f};
    }
}

}  // namespace

bool EditorBridge::snapPiece(uint32_t root, float world[3][4]) const
{
    float now[3][4], inverse[3][4];
    if (root >= records_.size() || !worldOf(root, now) || !invert(now, inverse))
        return false;
    // Its own snap points where `world` puts them; the others'.
    const std::vector<uint32_t> own = subtree(root);
    struct Point {
        float frame[3][4];
        std::string tag;
    };
    std::vector<Point> mine, theirs;
    for (uint32_t k = 0; k < records_.size(); ++k) {
        const flecs::entity e = entityOf(k);
        const SnapPoint* sp = e ? e.try_get<SnapPoint>() : nullptr;
        float w[3][4];
        if (!sp || !capture(k).alive || hiddenBelow(k) || !worldOf(k, w))
            continue;
        Point p;
        p.tag = sp->tag;
        if (std::find(own.begin(), own.end(), k) != own.end()) {
            float relative[3][4];
            multiply(inverse, w, relative);  // (relative to the piece, then where it is going)
            multiply(world, relative, p.frame);
            mine.push_back(p);
        } else {
            std::memcpy(p.frame, w, sizeof(w));
            theirs.push_back(p);
        }
    }
    // The nearest pair that fits.
    const Point* a = nullptr;
    const Point* b = nullptr;
    float best = kReach;
    for (const Point& m : mine)
        for (const Point& t : theirs) {
            if (!m.tag.empty() && !t.tag.empty() && m.tag != t.tag)
                continue;
            const float d = length(float3{m.frame[0][3] - t.frame[0][3], m.frame[1][3] - t.frame[1][3], m.frame[2][3] - t.frame[2][3]});
            if (d < best) {
                best = d;
                a = &m;
                b = &t;
            }
        }
    if (!a)
        return false;
    // The turn that puts its point's axes on the other's turned half round its up (x and z
    // reversed, y kept), then the move that puts the two points together.
    float3 sa[3], sb[3];
    unitAxes(a->frame, sa);
    unitAxes(b->frame, sb);
    const float3 want[3] = {sb[0] * -1.0f, sb[1], sb[2] * -1.0f};
    float turn[3][4] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            // turn = want * saᵀ (sa is orthonormal: its inverse is its transpose)
            const float* wr[3] = {&want[0].x, &want[1].x, &want[2].x};
            const float* sr[3] = {&sa[0].x, &sa[1].x, &sa[2].x};
            turn[r][c] = wr[0][r] * sr[0][c] + wr[1][r] * sr[1][c] + wr[2][r] * sr[2][c];
        }
    const float3 from = {a->frame[0][3], a->frame[1][3], a->frame[2][3]}, to = {b->frame[0][3], b->frame[1][3], b->frame[2][3]};
    for (int r = 0; r < 3; ++r)
        turn[r][3] = (&to.x)[r] - (turn[r][0] * from.x + turn[r][1] * from.y + turn[r][2] * from.z);
    multiply(turn, world, world);
    return true;
}

}  // namespace fx
