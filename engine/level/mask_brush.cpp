#include "level/mask_brush.h"

#include "level/level_mask.h"

#include <algorithm>
#include <cmath>

namespace fx::level {

namespace {

float3 rows(const float w[3][4], float3 p)
{
    return {w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
            w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
}

// A texel from what the stroke has given it (`cover`) over what it was before the stroke.
void shade(uint8_t* out, const uint8_t* before, float cover, uint32_t channel)
{
    if (channel >= 3) {
        for (int ch = 0; ch < 3; ++ch)
            out[ch] = uint8_t(std::lround(float(before[ch]) * (1.0f - cover)));
    } else {
        out[channel] = uint8_t(std::lround(float(before[channel]) + (255.0f - float(before[channel])) * cover));
    }
}

}  // namespace

float brushFalloff(float d, float radius)
{
    const float t = std::clamp((d / std::max(radius, 1e-6f) - 0.5f) * 2.0f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

bool paintMaskDab(const PolyMesh& mesh, const float world[3][4], const MaskDab& dab, std::vector<uint8_t>& mask, MaskStroke* stroke)
{
    if (mesh.cornerUv1.size() != mesh.corners.size() || mesh.faces.empty() || dab.radius <= 0.0f)
        return false;
    if (mask.size() != kMaskBytes)
        mask.assign(kMaskBytes, 0);
    MaskStroke own;
    MaskStroke& s = stroke ? *stroke : own;
    if (s.start.size() != kMaskBytes || s.cover.size() != size_t(kMaskSize) * kMaskSize) {
        s.start = mask;
        s.cover.assign(size_t(kMaskSize) * kMaskSize, 0.0f);
    }
    const float size = float(kMaskSize), r2 = dab.radius * dab.radius;
    // What this dab gave each texel it reached (for the two texels of padding round it: the charts'
    // edges sample past them), whether or not an earlier dab of the stroke gave it more.
    std::vector<float> got;
    int bx0 = int(kMaskSize), by0 = int(kMaskSize), bx1 = -1, by1 = -1;
    for (uint32_t f = 0; f < mesh.faces.size(); ++f) {
        const Face& face = mesh.faces[f];
        for (uint32_t k = 1; k + 1 < face.count; ++k) {
            const uint32_t c[3] = {face.first, face.first + k, face.first + k + 1};
            float3 p[3];
            float2 t[3];
            for (int i = 0; i < 3; ++i) {
                p[i] = rows(world, mesh.positions[mesh.corners[c[i]]]);
                t[i] = {mesh.cornerUv1[c[i]].x * size, mesh.cornerUv1[c[i]].y * size};
            }
            // Out of the ball's reach, or turned away from the surface under the mouse.
            const float3 lo = {std::min({p[0].x, p[1].x, p[2].x}), std::min({p[0].y, p[1].y, p[2].y}), std::min({p[0].z, p[1].z, p[2].z})};
            const float3 hi = {std::max({p[0].x, p[1].x, p[2].x}), std::max({p[0].y, p[1].y, p[2].y}), std::max({p[0].z, p[1].z, p[2].z})};
            const float3 q = {std::clamp(dab.center.x, lo.x, hi.x), std::clamp(dab.center.y, lo.y, hi.y), std::clamp(dab.center.z, lo.z, hi.z)};
            if (dot(q - dab.center, q - dab.center) >= r2)
                continue;
            const float3 n = cross(p[1] - p[0], p[2] - p[0]);
            const float nl = length(n);
            if (nl < 1e-12f || dot(n, dab.normal) < -0.5f * nl * length(dab.normal))
                continue;
            const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
            if (std::fabs(area) < 1e-9f)
                continue;
            const int x0 = std::max(int(std::floor(std::min({t[0].x, t[1].x, t[2].x}))), 0);
            const int x1 = std::min(int(std::ceil(std::max({t[0].x, t[1].x, t[2].x}))), int(kMaskSize) - 1);
            const int y0 = std::max(int(std::floor(std::min({t[0].y, t[1].y, t[2].y}))), 0);
            const int y1 = std::min(int(std::ceil(std::max({t[0].y, t[1].y, t[2].y}))), int(kMaskSize) - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                    const float w1 = ((px - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (py - t[0].y)) / area;
                    const float w2 = ((t[1].x - t[0].x) * (py - t[0].y) - (px - t[0].x) * (t[1].y - t[0].y)) / area;
                    const float w0 = 1.0f - w1 - w2;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                        continue;
                    const float3 at = p[0] * w0 + p[1] * w1 + p[2] * w2;
                    const float d2 = dot(at - dab.center, at - dab.center);
                    if (d2 >= r2)
                        continue;
                    const float amount = dab.strength * brushFalloff(std::sqrt(d2), dab.radius);
                    if (amount <= 0.0f)
                        continue;
                    const size_t i = size_t(y) * kMaskSize + size_t(x);
                    if (got.empty())
                        got.assign(size_t(kMaskSize) * kMaskSize, 0.0f);
                    got[i] = std::max(got[i], amount);
                    bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
                }
        }
    }
    if (got.empty())
        return false;
    // Two texels out from what it reached: the most of their neighbours' (as the wear pads).
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = got;
        for (int y = std::max(by0 - 1, 0); y <= std::min(by1 + 1, int(kMaskSize) - 1); ++y)
            for (int x = std::max(bx0 - 1, 0); x <= std::min(bx1 + 1, int(kMaskSize) - 1); ++x) {
                const size_t i = size_t(y) * kMaskSize + size_t(x);
                if (got[i] > 0.0f)
                    continue;
                float most = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < int(kMaskSize) && ny < int(kMaskSize))
                            most = std::max(most, got[size_t(ny) * kMaskSize + size_t(nx)]);
                    }
                next[i] = most;
            }
        got = std::move(next);
        bx0 = std::max(bx0 - 1, 0), by0 = std::max(by0 - 1, 0);
        bx1 = std::min(bx1 + 1, int(kMaskSize) - 1), by1 = std::min(by1 + 1, int(kMaskSize) - 1);
    }
    // Each texel: the most any one dab of the stroke gave it, over the mask before the stroke.
    for (int y = by0; y <= by1; ++y)
        for (int x = bx0; x <= bx1; ++x) {
            const size_t i = size_t(y) * kMaskSize + size_t(x);
            if (got[i] > s.cover[i]) {
                s.cover[i] = got[i];
                shade(&mask[i * 4], &s.start[i * 4], got[i], dab.channel);
            }
        }
    return true;
}

}  // namespace fx::level
