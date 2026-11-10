#include "level/rubble.h"

#include <algorithm>
#include <cmath>

namespace fx::level {

namespace {

// An icosahedron (its faces counter-clockwise seen from outside).
constexpr float kPhi = 1.6180340f;
const float3 kIco[12] = {{-1, kPhi, 0}, {1, kPhi, 0}, {-1, -kPhi, 0}, {1, -kPhi, 0}, {0, -1, kPhi}, {0, 1, kPhi},
                         {0, -1, -kPhi}, {0, 1, -kPhi}, {kPhi, 0, -1}, {kPhi, 0, 1}, {-kPhi, 0, -1}, {-kPhi, 0, 1}};
const uint32_t kIcoFaces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                   {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                                   {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};

struct Random {
    uint32_t state;
    float next()  // 0..1
    {
        state = state * 1664525u + 1013904223u;
        uint32_t h = state ^ (state >> 16);
        h *= 2246822519u;
        return float((h ^ (h >> 13)) & 0xffffu) / 65535.0f;
    }
};

}  // namespace

PolyMesh makeRubble(const RubbleSettings& s)
{
    PolyMesh m;
    m.materials.push_back(s.material);
    Random r{s.seed * 2654435761u + 12345u};
    const float spread = std::max(s.spread, 0.0f), size = std::max(s.size, 0.005f);
    for (uint32_t c = 0; c < std::max(s.count, 1u); ++c) {
        // Where it lies (the middle ones bigger), how big and how stretched, how it is turned.
        const float radius = spread * std::sqrt(r.next()), around = r.next() * 6.2831853f;
        const float big = size * (1.15f - 0.5f * (spread > 0.0f ? radius / spread : 0.0f)) * (0.6f + 0.6f * r.next());
        const float3 stretch = {big * (0.7f + 0.6f * r.next()), big * (0.45f + 0.4f * r.next()), big * (0.7f + 0.6f * r.next())};
        const float yaw = r.next() * 6.2831853f, tilt = (r.next() - 0.5f) * 0.8f;
        const float cy = std::cos(yaw), sy = std::sin(yaw), ct = std::cos(tilt), st = std::sin(tilt);
        const uint32_t first = uint32_t(m.positions.size());
        float lowest = 1e30f;
        for (const float3& v : kIco) {
            // Lumpy (each corner pushed in or out), then sized, tilted (about x) and turned (about y).
            const float lump = (0.75f + 0.4f * r.next()) * 0.5f / std::sqrt(1.0f + kPhi * kPhi);
            const float3 p = {v.x * lump * stretch.x, v.y * lump * stretch.y, v.z * lump * stretch.z};
            const float3 q = {p.x, p.y * ct - p.z * st, p.y * st + p.z * ct};
            const float3 w = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
            m.positions.push_back(w);
            lowest = std::min(lowest, w.y);
        }
        // On the ground, a little sunk in.
        const float3 at = {radius * std::cos(around), -lowest - stretch.y * 0.08f, radius * std::sin(around)};
        for (uint32_t k = first; k < m.positions.size(); ++k)
            m.positions[k] = m.positions[k] + at;
        for (const auto& f : kIcoFaces)
            m.addFace({first + f[0], first + f[1], first + f[2]});
    }
    return m;
}

}  // namespace fx::level
