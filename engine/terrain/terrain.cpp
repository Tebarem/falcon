#include "terrain/terrain.h"

#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"

#include <meshoptimizer.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fx {

using namespace terrain;

namespace {

constexpr uint32_t kRegionTiles = 8;  // region files hold 8 x 8 tiles
constexpr char kRegionMagic[4] = {'F', 'T', 'R', '1'};
constexpr char kCoarseMagic[4] = {'F', 'T', 'C', '1'};
constexpr uint32_t kFileVersion = 2;  // 2: material maps (version 1 files load with layer 0 everywhere)
constexpr size_t kMaxLog = 4096;

struct RegionEntry {
    uint64_t offset;
    uint32_t bytes;          // heights
    uint32_t materialBytes;  // then the material map (version 2)
};
struct RegionHeader {
    char magic[4];
    uint32_t version;
    uint32_t tiles;  // entries (kRegionTiles^2)
    uint32_t pad;
    RegionEntry entries[kRegionTiles * kRegionTiles];
};

// Tile heights <-> bytes: each sample's difference from its gradient prediction (left + up -
// up-left), zigzagged so small changes either way are small numbers, through meshoptimizer's
// vertex codec (two samples per 4-byte "vertex"). Smooth terrain packs to a few bits a sample.
std::vector<uint8_t> encodeTile(const std::vector<uint16_t>& h)
{
    const uint32_t n = kTileSampleCount + 1;  // even
    std::vector<uint16_t> r(n, 0);
    for (uint32_t z = 0; z < kTileSamples; ++z)
        for (uint32_t x = 0; x < kTileSamples; ++x) {
            const int a = x ? h[z * kTileSamples + x - 1] : (z ? h[(z - 1) * kTileSamples + x] : 0);
            const int b = z ? h[(z - 1) * kTileSamples + x] : a;
            const int c = x && z ? h[(z - 1) * kTileSamples + x - 1] : b;
            const int pred = std::clamp(a + b - c, 0, 65535);
            const int16_t d = int16_t(uint16_t(h[z * kTileSamples + x] - pred));
            r[z * kTileSamples + x] = uint16_t((uint16_t(d) << 1) ^ uint16_t(d >> 15));
        }
    std::vector<uint8_t> out(meshopt_encodeVertexBufferBound(n / 2, 4));
    out.resize(meshopt_encodeVertexBuffer(out.data(), out.size(), r.data(), n / 2, 4));
    return out;
}

bool decodeTile(const uint8_t* data, size_t bytes, std::vector<uint16_t>& h)
{
    const uint32_t n = kTileSampleCount + 1;
    std::vector<uint16_t> r(n);
    if (meshopt_decodeVertexBuffer(r.data(), n / 2, 4, data, bytes) != 0)
        return false;
    h.resize(kTileSampleCount);
    for (uint32_t z = 0; z < kTileSamples; ++z)
        for (uint32_t x = 0; x < kTileSamples; ++x) {
            const int a = x ? h[z * kTileSamples + x - 1] : (z ? h[(z - 1) * kTileSamples + x] : 0);
            const int b = z ? h[(z - 1) * kTileSamples + x] : a;
            const int c = x && z ? h[(z - 1) * kTileSamples + x - 1] : b;
            const int pred = std::clamp(a + b - c, 0, 65535);
            const uint16_t zz = r[z * kTileSamples + x];
            const int16_t d = int16_t((zz >> 1) ^ uint16_t(-int16_t(zz & 1)));
            h[z * kTileSamples + x] = uint16_t(pred + d);
        }
    return true;
}

// Material maps <-> bytes: the codec on one 4-byte sample per vertex (long runs of one material
// pack to almost nothing).
std::vector<uint8_t> encodeMaterials(const std::vector<uint32_t>& m)
{
    std::vector<uint8_t> out(meshopt_encodeVertexBufferBound(m.size(), 4));
    out.resize(meshopt_encodeVertexBuffer(out.data(), out.size(), m.data(), m.size(), 4));
    return out;
}

bool decodeMaterials(const uint8_t* data, size_t bytes, std::vector<uint32_t>& m)
{
    m.resize(kTileSampleCount);
    return meshopt_decodeVertexBuffer(m.data(), m.size(), 4, data, bytes) == 0;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    out.resize(size_t(in.tellg()));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

bool writeFileAtomic(const std::string& path, const void* data, size_t bytes)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), std::streamsize(bytes));
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// The surface of a patch level between its vertices, at a point `fx, fz` in vertex units from
// the patch's corner (h(i, j): the vertex heights). Quads split along alternating diagonals,
// as the renderer's grid: (i + j) even -> (i, j)-(i+1, j+1), odd -> (i+1, j)-(i, j+1).
template <typename H>
float surface(H&& h, float fx, float fz)
{
    const int i = std::clamp(int(std::floor(fx)), 0, int(kPatchQuads) - 1);
    const int j = std::clamp(int(std::floor(fz)), 0, int(kPatchQuads) - 1);
    const float u = fx - float(i), v = fz - float(j);
    const float h00 = h(i, j), h10 = h(i + 1, j), h01 = h(i, j + 1), h11 = h(i + 1, j + 1);
    if (((i + j) & 1) == 0)
        return u >= v ? h00 + (h10 - h00) * u + (h11 - h10) * v : h00 + (h01 - h00) * v + (h11 - h01) * u;
    return u + v <= 1.0f ? h00 + (h10 - h00) * u + (h01 - h00) * v : h11 + (h01 - h11) * (1.0f - u) + (h10 - h11) * (1.0f - v);
}

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

}  // namespace

float terrainNoise(float x, float z, uint32_t seed)
{
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = int(fx), iz = int(fz);
    const float u = x - fx, v = z - fz;
    auto grad = [&](int cx, int cz, float dx, float dz) {
        const uint32_t h = hash(uint32_t(cx) * 0x8da6b343u ^ uint32_t(cz) * 0xd8163841u ^ seed * 0xcb1ab31fu);
        const float a = float(h & 0xffff) * (6.28318531f / 65536.0f);
        return std::cos(a) * dx + std::sin(a) * dz;
    };
    const float su = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f), sv = v * v * v * (v * (v * 6.0f - 15.0f) + 10.0f);
    const float n00 = grad(ix, iz, u, v), n10 = grad(ix + 1, iz, u - 1.0f, v);
    const float n01 = grad(ix, iz + 1, u, v - 1.0f), n11 = grad(ix + 1, iz + 1, u - 1.0f, v - 1.0f);
    const float nx0 = n00 + (n10 - n00) * su, nx1 = n01 + (n11 - n01) * su;
    return (nx0 + (nx1 - nx0) * sv) * 1.41421356f;
}

float terrainFbm(float x, float z, uint32_t octaves, uint32_t seed)
{
    float sum = 0.0f, amplitude = 0.5f, norm = 0.0f;
    for (uint32_t o = 0; o < octaves; ++o) {
        sum += terrainNoise(x, z, seed + o * 101u) * amplitude;
        norm += amplitude;
        // Rotated between octaves so the grids never line up.
        const float rx = x * 1.6f - z * 1.2f, rz = x * 1.2f + z * 1.6f;
        x = rx;
        z = rz;
        amplitude *= 0.5f;
    }
    return sum / std::max(norm, 1e-6f);
}

Terrain::~Terrain() { unload(); }

std::string Terrain::dirFor(const std::string& scenePath)
{
    std::string p = scenePath;
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + ".terrain";
}

void Terrain::unload()
{
    if (loader_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        loader_.join();
    }
    quit_ = false;
    requests_.clear();
    loaded_.clear();
    tiles_.clear();
    coarse_.clear();
    coarseMaterials_.clear();
    layersJson_ = scatterJson_ = "[]";
    generatorJson_ = "{}";
    pinned_ = {0, 0, -1, -1};
    levelNodes_.clear();
    log_.clear();
    dirtyTiles_ = residentCount_ = 0;
    settingsDirty_ = false;
    dir_.clear();
}

void Terrain::initLayout(const TerrainSettings& s)
{
    settings_ = s;
    settings_.tiles = std::clamp(std::bit_floor(std::max(s.tiles, 1u)), 1u, kMaxTiles);
    settings_.spacing = std::max(s.spacing, 0.01f);
    if (settings_.heightMax <= settings_.heightMin)
        settings_.heightMax = settings_.heightMin + 1.0f;
    heightScale_ = (settings_.heightMax - settings_.heightMin) / 65535.0f;
    levels_ = uint32_t(std::countr_zero(settings_.tiles)) + kTileLevels;
    tiles_.assign(size_t(settings_.tiles) * settings_.tiles, Tile{});
    coarseSide_ = settings_.tiles * (kTileQuads / kCoarseStep) + 1;
    coarse_.assign(size_t(coarseSide_) * coarseSide_, 0);
    coarseMaterials_.assign(size_t(coarseSide_) * coarseSide_, 0);
    levelNodes_.assign(levels_, {});
    for (uint32_t l = kCoarseLevel; l < levels_; ++l)
        levelNodes_[l].assign(size_t(nodesPerSide(l)) * nodesPerSide(l), TerrainNode{0.0f, 0.0f, 0.0f});
    revision_ = residencyRevision_ = 1;
    log_.clear();
}

uint16_t Terrain::fromWorld(float h) const
{
    return uint16_t(std::clamp(std::lround((h - settings_.heightMin) / heightScale_), 0l, 65535l));
}

float3 Terrain::samplePosition(int32_t gx, int32_t gz) const
{
    return {settings_.originX + float(gx) * settings_.spacing, toWorld(sample(gx, gz)),
            settings_.originZ + float(gz) * settings_.spacing};
}

void Terrain::bounds(float lo[3], float hi[3]) const
{
    const TerrainNode root = levelNodes_.empty() ? TerrainNode{} : levelNodes_[levels_ - 1][0];
    lo[0] = settings_.originX;
    lo[1] = root.lo;
    lo[2] = settings_.originZ;
    hi[0] = settings_.originX + worldSize();
    hi[1] = std::max(root.hi, root.lo + 0.01f);
    hi[2] = settings_.originZ + worldSize();
}

bool Terrain::create(const TerrainSettings& settings, float height, float hills, uint32_t seed)
{
    FX_ZONE("Terrain::create");
    unload();
    initLayout(settings);
    const uint32_t T = settings_.tiles;
    // Tiles in parallel (an 8 km terrain is 4096 tiles of noise).
    std::atomic<uint32_t> next = 0;
    auto work = [&] {
        for (uint32_t i; (i = next++) < T * T;) {
            const uint32_t tx = i % T, tz = i / T;
            Tile& t = tileAt(tx, tz);
            t.heights.resize(kTileSampleCount);
            for (uint32_t z = 0; z < kTileSamples; ++z)
                for (uint32_t x = 0; x < kTileSamples; ++x) {
                    const float wx = float(tx * kTileQuads + x) * settings_.spacing;
                    const float wz = float(tz * kTileQuads + z) * settings_.spacing;
                    float h = height;
                    if (hills > 0.0f) {  // broad hills with a little detail on top
                        h += hills * (terrainFbm(wx / 400.0f, wz / 400.0f, 5, seed) * 0.5f + 0.5f);
                        h += hills * 0.02f * terrainFbm(wx / 12.0f, wz / 12.0f, 3, seed + 7);
                    }
                    t.heights[z * kTileSamples + x] = fromWorld(h);
                }
            t.materials.assign(kTileSampleCount, packMaterial(0, 0, 0));
            t.dirty = true;
            buildTileNodes(tx, tz);  // (its own nodes, level-3 nodes and coarse samples: no overlap
                                     // between tiles but the shared coarse edge, written the same)
        }
    };
    std::vector<std::thread> threads(std::clamp(std::thread::hardware_concurrency(), 1u, 32u) - 1);
    for (std::thread& th : threads)
        th = std::thread(work);
    work();
    for (std::thread& th : threads)
        th.join();
    buildCoarseNodes(0, 0, nodesPerSide(kCoarseLevel) - 1, nodesPerSide(kCoarseLevel) - 1);
    dirtyTiles_ = T * T;
    residentCount_ = T * T;
    settingsDirty_ = true;
    FX_INFO("terrain: created %ux%u tiles (%.0f m, %.2f m samples)", T, T, double(worldSize()), double(settings_.spacing));
    return true;
}

void Terrain::buildTileNodes(uint32_t tx, uint32_t tz)
{
    Tile& t = tileAt(tx, tz);
    const uint16_t* h = t.heights.data();
    auto at = [&](uint32_t x, uint32_t z) { return toWorld(h[z * kTileSamples + x]); };
    // Level 0: exact; bounds over its 9 x 9 samples.
    const uint32_t n0 = kTileQuads / kPatchQuads;  // 32
    t.nodes[0].resize(n0 * n0);
    for (uint32_t nz = 0; nz < n0; ++nz)
        for (uint32_t nx = 0; nx < n0; ++nx) {
            float lo = 1e30f, hi = -1e30f;
            for (uint32_t z = 0; z <= kPatchQuads; ++z)
                for (uint32_t x = 0; x <= kPatchQuads; ++x) {
                    const float v = at(nx * kPatchQuads + x, nz * kPatchQuads + z);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            t.nodes[0][nz * n0 + nx] = {0.0f, lo, hi};
        }
    // Levels 1..3: bounds of the children; error = the larger of the children's and how far this
    // level's surface is from the children's vertices. Close to the distance to full detail (the
    // sum would bound it, but grows with every level and made distant patches needlessly fine),
    // and never below a child's, so a parent never splits less readily than its children.
    for (uint32_t l = 1; l <= kCoarseLevel; ++l) {
        const uint32_t n = n0 >> l, step = 1u << l;
        if (l < kCoarseLevel)
            t.nodes[l].resize(n * n);
        const TerrainNode* child = t.nodes[l - 1].data();
        for (uint32_t nz = 0; nz < n; ++nz)
            for (uint32_t nx = 0; nx < n; ++nx) {
                TerrainNode node = {0.0f, 1e30f, -1e30f};
                for (uint32_t c = 0; c < 4; ++c) {
                    const TerrainNode& k = child[(nz * 2 + (c >> 1)) * (n * 2) + nx * 2 + (c & 1)];
                    node.error = std::max(node.error, k.error);
                    node.lo = std::min(node.lo, k.lo);
                    node.hi = std::max(node.hi, k.hi);
                }
                const uint32_t x0 = nx * kPatchQuads * step, z0 = nz * kPatchQuads * step;
                auto vertex = [&](int i, int j) { return at(x0 + uint32_t(i) * step, z0 + uint32_t(j) * step); };
                float dev = 0.0f;
                for (uint32_t j = 0; j <= 2 * kPatchQuads; ++j)
                    for (uint32_t i = 0; i <= 2 * kPatchQuads; ++i)
                        if ((i | j) & 1)
                            dev = std::max(dev, std::fabs(at(x0 + i * step / 2, z0 + j * step / 2) -
                                                          surface(vertex, float(i) * 0.5f, float(j) * 0.5f)));
                node.error = std::max(node.error, dev);
                if (l < kCoarseLevel)
                    t.nodes[l][nz * n + nx] = node;
                else
                    globalNode(l, tx * n + nx, tz * n + nz) = node;
            }
    }
    // Its coarse-map samples.
    const uint32_t per = kTileQuads / kCoarseStep;
    for (uint32_t z = 0; z <= per; ++z)
        for (uint32_t x = 0; x <= per; ++x) {
            const uint32_t c = (tz * per + z) * coarseSide_ + tx * per + x, s = z * kCoarseStep * kTileSamples + x * kCoarseStep;
            coarse_[c] = h[s];
            if (!t.materials.empty())
                coarseMaterials_[c] = t.materials[s];
        }
}

void Terrain::buildCoarseNodes(uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1)
{
    for (uint32_t l = kCoarseLevel + 1; l < levels_; ++l) {
        x0 >>= 1, z0 >>= 1, x1 >>= 1, z1 >>= 1;
        const uint32_t n = nodesPerSide(l), step = 1u << l, cstep = step / kCoarseStep;
        for (uint32_t nz = z0; nz <= z1 && nz < n; ++nz)
            for (uint32_t nx = x0; nx <= x1 && nx < n; ++nx) {
                TerrainNode node = {0.0f, 1e30f, -1e30f};
                for (uint32_t c = 0; c < 4; ++c) {
                    const TerrainNode& k = globalNode(l - 1, nx * 2 + (c & 1), nz * 2 + (c >> 1));
                    node.error = std::max(node.error, k.error);
                    node.lo = std::min(node.lo, k.lo);
                    node.hi = std::max(node.hi, k.hi);
                }
                const uint32_t cx0 = nx * kPatchQuads * cstep, cz0 = nz * kPatchQuads * cstep;
                auto at = [&](uint32_t cx, uint32_t cz) { return toWorld(coarseSample(cx, cz)); };
                auto vertex = [&](int i, int j) { return at(cx0 + uint32_t(i) * cstep, cz0 + uint32_t(j) * cstep); };
                float dev = 0.0f;
                for (uint32_t j = 0; j <= 2 * kPatchQuads; ++j)
                    for (uint32_t i = 0; i <= 2 * kPatchQuads; ++i)
                        if ((i | j) & 1)
                            dev = std::max(dev, std::fabs(at(cx0 + i * cstep / 2, cz0 + j * cstep / 2) -
                                                          surface(vertex, float(i) * 0.5f, float(j) * 0.5f)));
                node.error = std::max(node.error, dev);
                globalNode(l, nx, nz) = node;
            }
    }
}

bool Terrain::tileResident(uint32_t tx, uint32_t tz) const
{
    return tx < settings_.tiles && tz < settings_.tiles && !tileAt(tx, tz).heights.empty();
}

uint16_t Terrain::sample(int32_t gx, int32_t gz) const
{
    const int32_t last = int32_t(samplesPerSide()) - 1;
    gx = std::clamp(gx, 0, last);
    gz = std::clamp(gz, 0, last);
    const uint32_t tx = std::min(uint32_t(gx) / kTileQuads, settings_.tiles - 1);
    const uint32_t tz = std::min(uint32_t(gz) / kTileQuads, settings_.tiles - 1);
    const Tile& t = tileAt(tx, tz);
    if (!t.heights.empty())
        return t.heights[(uint32_t(gz) - tz * kTileQuads) * kTileSamples + uint32_t(gx) - tx * kTileQuads];
    // Not resident: the coarse map, bilinear.
    const float fx = float(gx) / float(kCoarseStep), fz = float(gz) / float(kCoarseStep);
    const uint32_t cx = std::min(uint32_t(fx), coarseSide_ - 2), cz = std::min(uint32_t(fz), coarseSide_ - 2);
    const float u = fx - float(cx), v = fz - float(cz);
    const float a = float(coarseSample(cx, cz)) * (1 - u) + float(coarseSample(cx + 1, cz)) * u;
    const float b = float(coarseSample(cx, cz + 1)) * (1 - u) + float(coarseSample(cx + 1, cz + 1)) * u;
    return uint16_t(std::lround(a * (1 - v) + b * v));
}

float Terrain::heightAt(float x, float z) const
{
    const float fx = (x - settings_.originX) / settings_.spacing, fz = (z - settings_.originZ) / settings_.spacing;
    const float last = float(samplesPerSide() - 1);
    const float cx = std::clamp(fx, 0.0f, last), cz = std::clamp(fz, 0.0f, last);
    const int32_t i = std::min(int32_t(cx), int32_t(last) - 1), j = std::min(int32_t(cz), int32_t(last) - 1);
    // The quad's own diagonal (the grid's parity is the same as a patch's: 8 is even).
    const int32_t pi = i & ~7, pj = j & ~7;
    auto h = [&](int a, int b) { return toWorld(sample(pi + a, pj + b)); };
    return surface(h, cx - float(pi), cz - float(pj));
}

bool Terrain::raycast(float3 origin, float3 dir, float maxT, float& tHit) const
{
    if (!valid())
        return false;
    float lo[3], hi[3];
    bounds(lo, hi);
    // Clip to the terrain's box.
    float t0 = 0.0f, t1 = maxT;
    for (int a = 0; a < 3; ++a) {
        const float o = (&origin.x)[a], d = (&dir.x)[a];
        if (std::fabs(d) < 1e-12f) {
            if (o < lo[a] || o > hi[a])
                return false;
            continue;
        }
        float ta = (lo[a] - o) / d, tb = (hi[a] - o) / d;
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t0 > t1)
        return false;
    // March in half-sample steps, skipping level-3 cells whose height bounds the ray passes over.
    const float speed = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    const float step = settings_.spacing * 0.5f / std::max(speed, 1e-6f);
    const float cell = settings_.spacing * float(kPatchQuads << kCoarseLevel);
    auto above = [&](float t) {
        const float3 p = origin + dir * t;
        return p.y - heightAt(p.x, p.z);
    };
    float t = t0, prev = above(t0);
    if (prev < 0.0f) {
        tHit = t0;
        return true;
    }
    const uint32_t n3 = nodesPerSide(kCoarseLevel);
    while (t < t1) {
        const float3 p = origin + dir * t;
        const uint32_t nx = std::min(uint32_t(std::max((p.x - settings_.originX) / cell, 0.0f)), n3 - 1);
        const uint32_t nz = std::min(uint32_t(std::max((p.z - settings_.originZ) / cell, 0.0f)), n3 - 1);
        const TerrainNode& node = levelNodes_[kCoarseLevel][nz * n3 + nx];
        // Exit of this cell along xz.
        float exit = t1;
        for (int a = 0; a < 3; a += 2) {
            const float d = (&dir.x)[a];
            if (std::fabs(d) < 1e-12f)
                continue;
            const float base = a == 0 ? settings_.originX + float(nx) * cell : settings_.originZ + float(nz) * cell;
            exit = std::min(exit, ((d > 0 ? base + cell : base) - (&origin.x)[a]) / d);
        }
        exit = std::max(exit, t + step * 0.01f);
        const float yEnter = p.y, yExit = origin.y + dir.y * std::min(exit, t1);
        if (std::min(yEnter, yExit) > node.hi + 0.01f) {  // over this cell: skip it
            t = exit;
            prev = above(std::min(t, t1));
            continue;
        }
        for (; t < exit && t < t1;) {
            const float tn = std::min(t + step, t1);
            const float a = above(tn);
            if (a <= 0.0f) {  // crossed between t and tn: bisect
                float l = t, r = tn;
                for (int k = 0; k < 16; ++k) {
                    const float m = (l + r) * 0.5f;
                    (above(m) > 0.0f ? l : r) = m;
                }
                tHit = r;
                return true;
            }
            prev = a;
            t = tn;
        }
    }
    return false;
}

bool Terrain::nodeAvailable(uint32_t level, uint32_t x, uint32_t z) const
{
    if (level >= kCoarseLevel)
        return true;
    const uint32_t per = (kTileQuads / kPatchQuads) >> level;
    return tileResident(x / per, z / per);
}

TerrainNode Terrain::node(uint32_t level, uint32_t x, uint32_t z) const
{
    if (level >= kCoarseLevel)
        return levelNodes_[level][z * nodesPerSide(level) + x];
    const uint32_t per = (kTileQuads / kPatchQuads) >> level;
    const Tile& t = tileAt(x / per, z / per);
    return t.nodes[level][(z % per) * per + x % per];
}

void Terrain::patchSamples(uint32_t level, uint32_t x, uint32_t z, uint16_t out[kPatchSamples * kPatchSamples]) const
{
    const uint32_t step = 1u << level;
    if (level >= kCoarseLevel) {
        const uint32_t cstep = step / kCoarseStep, cx0 = x * kPatchQuads * cstep, cz0 = z * kPatchQuads * cstep;
        for (uint32_t j = 0; j < kPatchSamples; ++j)
            for (uint32_t i = 0; i < kPatchSamples; ++i)
                out[j * kPatchSamples + i] = coarseSample(cx0 + i * cstep, cz0 + j * cstep);
        return;
    }
    const uint32_t gx0 = x * kPatchQuads * step, gz0 = z * kPatchQuads * step;
    const uint32_t tx = gx0 / kTileQuads, tz = gz0 / kTileQuads;
    const Tile& t = tileAt(tx, tz);
    const uint32_t lx = gx0 - tx * kTileQuads, lz = gz0 - tz * kTileQuads;
    for (uint32_t j = 0; j < kPatchSamples; ++j)
        for (uint32_t i = 0; i < kPatchSamples; ++i)
            out[j * kPatchSamples + i] = t.heights[(lz + j * step) * kTileSamples + lx + i * step];
}

TerrainRect Terrain::clampRect(TerrainRect r) const
{
    const int32_t last = int32_t(samplesPerSide()) - 1;
    return {std::max(r.x0, 0), std::max(r.z0, 0), std::min(r.x1, last), std::min(r.z1, last)};
}

TerrainRect Terrain::worldRect(float x0, float z0, float x1, float z1) const
{
    const float s = settings_.spacing;
    return clampRect({int32_t(std::floor((x0 - settings_.originX) / s)), int32_t(std::floor((z0 - settings_.originZ) / s)),
                      int32_t(std::ceil((x1 - settings_.originX) / s)), int32_t(std::ceil((z1 - settings_.originZ) / s))});
}

bool Terrain::rectResident(TerrainRect r) const
{
    if (r.empty())
        return false;
    const uint32_t T = settings_.tiles;
    const uint32_t tx0 = std::min(uint32_t(r.x0) / kTileQuads, T - 1), tx1 = std::min(uint32_t(r.x1) / kTileQuads, T - 1);
    const uint32_t tz0 = std::min(uint32_t(r.z0) / kTileQuads, T - 1), tz1 = std::min(uint32_t(r.z1) / kTileQuads, T - 1);
    for (uint32_t tz = tz0; tz <= tz1; ++tz)
        for (uint32_t tx = tx0; tx <= tx1; ++tx)
            if (!tileResident(tx, tz))
                return false;
    return true;
}

void Terrain::read(TerrainRect r, std::vector<float>& heights) const
{
    const uint32_t w = uint32_t(r.x1 - r.x0 + 1), d = uint32_t(r.z1 - r.z0 + 1);
    heights.resize(size_t(w) * d);
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t x = 0; x < w; ++x)
            heights[z * w + x] = toWorld(sample(r.x0 + int32_t(x), r.z0 + int32_t(z)));
}

void Terrain::tilesOf(TerrainRect r, uint32_t& tx0, uint32_t& tz0, uint32_t& tx1, uint32_t& tz1) const
{
    const uint32_t T = settings_.tiles;
    tx0 = r.x0 > 0 ? uint32_t(r.x0 - 1) / kTileQuads : 0;
    tz0 = r.z0 > 0 ? uint32_t(r.z0 - 1) / kTileQuads : 0;
    tx1 = std::min(uint32_t(r.x1) / kTileQuads, T - 1);
    tz1 = std::min(uint32_t(r.z1) / kTileQuads, T - 1);
}

void Terrain::write(TerrainRect r, const std::vector<float>& heights)
{
    FX_ZONE("Terrain::write");
    const uint32_t w = uint32_t(r.x1 - r.x0 + 1);
    uint32_t tx0, tz0, tx1, tz1;
    tilesOf(r, tx0, tz0, tx1, tz1);
    for (uint32_t tz = tz0; tz <= tz1; ++tz)
        for (uint32_t tx = tx0; tx <= tx1; ++tx) {
            Tile& t = tileAt(tx, tz);
            if (t.heights.empty())
                continue;
            const int32_t bx = int32_t(tx * kTileQuads), bz = int32_t(tz * kTileQuads);
            const int32_t x0 = std::max(r.x0, bx), x1 = std::min(r.x1, bx + int32_t(kTileQuads));
            const int32_t z0 = std::max(r.z0, bz), z1 = std::min(r.z1, bz + int32_t(kTileQuads));
            if (x0 > x1 || z0 > z1)
                continue;
            for (int32_t z = z0; z <= z1; ++z)
                for (int32_t x = x0; x <= x1; ++x)
                    t.heights[uint32_t(z - bz) * kTileSamples + uint32_t(x - bx)] =
                        fromWorld(heights[uint32_t(z - r.z0) * w + uint32_t(x - r.x0)]);
            if (!t.dirty)
                ++dirtyTiles_;
            t.dirty = true;
            buildTileNodes(tx, tz);
        }
    const uint32_t per3 = kTileQuads / (kPatchQuads << kCoarseLevel);
    buildCoarseNodes(tx0 * per3, tz0 * per3, (tx1 + 1) * per3 - 1, (tz1 + 1) * per3 - 1);
    logChange(r, kChangeHeights);
}

void Terrain::readMaterials(TerrainRect r, std::vector<uint32_t>& materials) const
{
    const uint32_t w = uint32_t(r.x1 - r.x0 + 1), d = uint32_t(r.z1 - r.z0 + 1);
    materials.resize(size_t(w) * d);
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t x = 0; x < w; ++x)
            materials[z * w + x] = material(r.x0 + int32_t(x), r.z0 + int32_t(z));
}

void Terrain::writeMaterials(TerrainRect r, const std::vector<uint32_t>& materials, uint32_t kinds)
{
    const uint32_t w = uint32_t(r.x1 - r.x0 + 1);
    uint32_t tx0, tz0, tx1, tz1;
    tilesOf(r, tx0, tz0, tx1, tz1);
    const uint32_t per = kTileQuads / kCoarseStep;
    for (uint32_t tz = tz0; tz <= tz1; ++tz)
        for (uint32_t tx = tx0; tx <= tx1; ++tx) {
            Tile& t = tileAt(tx, tz);
            if (t.materials.empty())
                continue;
            const int32_t bx = int32_t(tx * kTileQuads), bz = int32_t(tz * kTileQuads);
            const int32_t x0 = std::max(r.x0, bx), x1 = std::min(r.x1, bx + int32_t(kTileQuads));
            const int32_t z0 = std::max(r.z0, bz), z1 = std::min(r.z1, bz + int32_t(kTileQuads));
            if (x0 > x1 || z0 > z1)
                continue;
            for (int32_t z = z0; z <= z1; ++z)
                for (int32_t x = x0; x <= x1; ++x) {
                    const uint32_t m = materials[uint32_t(z - r.z0) * w + uint32_t(x - r.x0)];
                    t.materials[uint32_t(z - bz) * kTileSamples + uint32_t(x - bx)] = m;
                    if (x % int32_t(kCoarseStep) == 0 && z % int32_t(kCoarseStep) == 0)
                        coarseMaterials_[(tz * per + uint32_t(z - bz) / kCoarseStep) * coarseSide_ + tx * per +
                                         uint32_t(x - bx) / kCoarseStep] = m;
                }
            if (!t.dirty)
                ++dirtyTiles_;
            t.dirty = true;
        }
    logChange(r, kinds);
}

uint32_t Terrain::material(int32_t gx, int32_t gz) const
{
    const int32_t last = int32_t(samplesPerSide()) - 1;
    gx = std::clamp(gx, 0, last);
    gz = std::clamp(gz, 0, last);
    const uint32_t tx = std::min(uint32_t(gx) / kTileQuads, settings_.tiles - 1);
    const uint32_t tz = std::min(uint32_t(gz) / kTileQuads, settings_.tiles - 1);
    const Tile& t = tileAt(tx, tz);
    if (!t.materials.empty())
        return t.materials[(uint32_t(gz) - tz * kTileQuads) * kTileSamples + uint32_t(gx) - tx * kTileQuads];
    const uint32_t cx = std::min((uint32_t(gx) + kCoarseStep / 2) / kCoarseStep, coarseSide_ - 1);
    const uint32_t cz = std::min((uint32_t(gz) + kCoarseStep / 2) / kCoarseStep, coarseSide_ - 1);
    return coarseMaterials_[cz * coarseSide_ + cx];
}

const uint16_t* Terrain::tileHeights(uint32_t tx, uint32_t tz) const
{
    return tileResident(tx, tz) ? tileAt(tx, tz).heights.data() : nullptr;
}

const uint32_t* Terrain::tileMaterials(uint32_t tx, uint32_t tz) const
{
    return tileResident(tx, tz) && !tileAt(tx, tz).materials.empty() ? tileAt(tx, tz).materials.data() : nullptr;
}

void Terrain::setScatterJson(std::string json)
{
    if (json != scatterJson_) {
        scatterJson_ = std::move(json);
        settingsDirty_ = true;
    }
}

void Terrain::setGeneratorJson(std::string json)
{
    if (json != generatorJson_) {
        generatorJson_ = std::move(json);
        settingsDirty_ = true;
    }
}

void Terrain::pin(TerrainRect r)
{
    pinned_ = r.empty() ? TerrainRect{0, 0, -1, -1} : clampRect(r);
}

void Terrain::setLayersJson(std::string json)
{
    if (json != layersJson_) {
        layersJson_ = std::move(json);
        settingsDirty_ = true;
    }
}

bool Terrain::copyTile(uint32_t tx, uint32_t tz, TerrainTileData& out) const
{
    if (!tileResident(tx, tz))
        return false;
    out.heights = tileAt(tx, tz).heights;
    out.materials = tileAt(tx, tz).materials;
    return true;
}

void Terrain::restoreTile(uint32_t tx, uint32_t tz, const TerrainTileData& data)
{
    Tile& t = tileAt(tx, tz);
    if (data.heights.size() != kTileSampleCount || data.materials.size() != kTileSampleCount)
        return;
    if (t.heights.empty())
        ++residentCount_;
    t.heights = data.heights;
    t.materials = data.materials;
    if (!t.dirty)
        ++dirtyTiles_;
    t.dirty = true;
    buildTileNodes(tx, tz);
    const uint32_t per3 = kTileQuads / (kPatchQuads << kCoarseLevel);
    buildCoarseNodes(tx * per3, tz * per3, (tx + 1) * per3 - 1, (tz + 1) * per3 - 1);
    logChange({int32_t(tx * kTileQuads), int32_t(tz * kTileQuads), int32_t((tx + 1) * kTileQuads), int32_t((tz + 1) * kTileQuads)},
              kChangeHeights | kChangeMaterials | kChangeHoles);
}

void Terrain::logChange(TerrainRect r, uint32_t kinds)
{
    log_.push_back({++revision_, {r, kinds}});
    while (log_.size() > kMaxLog)
        log_.pop_front();
}

bool Terrain::changesSince(uint64_t revision, std::vector<TerrainChange>& out) const
{
    out.clear();
    if (revision >= revision_)
        return true;
    if (log_.empty() || log_.front().first > revision + 1)
        return false;
    for (const auto& [rev, change] : log_)
        if (rev > revision)
            out.push_back(change);
    return true;
}
std::string Terrain::regionPath(uint32_t rx, uint32_t rz) const
{
    return dir_ + "/regions/r_" + std::to_string(rx) + "_" + std::to_string(rz) + ".bin";
}

bool Terrain::save(const std::string& dir)
{
    FX_ZONE("Terrain::save");
    if (!valid())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(dir + "/regions", ec);
    if (ec) {
        FX_ERROR("terrain: cannot create %s: %s", dir.c_str(), ec.message().c_str());
        return false;
    }
    const std::string oldDir = dir_;
    if (dir_ != dir)
        dir_ = dir;  // (the loader thread only runs once a directory exists)
    // Regions with unsaved tiles: resident tiles encoded, the others copied from the old file.
    const uint32_t T = settings_.tiles, R = (T + kRegionTiles - 1) / kRegionTiles;
    uint32_t written = 0;
    for (uint32_t rz = 0; rz < R; ++rz)
        for (uint32_t rx = 0; rx < R; ++rx) {
            bool any = false;
            for (uint32_t z = 0; z < kRegionTiles && !any; ++z)
                for (uint32_t x = 0; x < kRegionTiles && !any; ++x) {
                    const uint32_t tx = rx * kRegionTiles + x, tz = rz * kRegionTiles + z;
                    any = tx < T && tz < T && tileAt(tx, tz).dirty;
                }
            const std::string path = regionPath(rx, rz);
            const bool sameDir = oldDir == dir;
            if (!any && sameDir)
                continue;
            std::vector<uint8_t> old;
            const std::string oldPath = oldDir.empty() ? std::string() : oldDir + "/regions/r_" + std::to_string(rx) + "_" + std::to_string(rz) + ".bin";
            const bool haveOld = !oldPath.empty() && readFile(oldPath, old) && old.size() >= sizeof(RegionHeader) &&
                                 !std::memcmp(old.data(), kRegionMagic, 4);
            RegionHeader hdr = {};
            std::memcpy(hdr.magic, kRegionMagic, 4);
            hdr.version = kFileVersion;
            hdr.tiles = kRegionTiles * kRegionTiles;
            std::vector<uint8_t> body;
            for (uint32_t e = 0; e < kRegionTiles * kRegionTiles; ++e) {
                const uint32_t tx = rx * kRegionTiles + e % kRegionTiles, tz = rz * kRegionTiles + e / kRegionTiles;
                if (tx >= T || tz >= T)
                    continue;
                const Tile& t = tileAt(tx, tz);
                std::vector<uint8_t> blob, materials;
                if (!t.heights.empty()) {
                    blob = encodeTile(t.heights);
                    materials = encodeMaterials(t.materials);
                } else if (haveOld) {
                    const RegionHeader& oh = *reinterpret_cast<const RegionHeader*>(old.data());
                    const RegionEntry& oe = oh.entries[e];
                    const uint32_t matBytes = oh.version >= 2 ? oe.materialBytes : 0;
                    if (oe.offset + oe.bytes + matBytes <= old.size()) {
                        blob.assign(old.begin() + std::ptrdiff_t(oe.offset), old.begin() + std::ptrdiff_t(oe.offset + oe.bytes));
                        materials.assign(old.begin() + std::ptrdiff_t(oe.offset + oe.bytes),
                                         old.begin() + std::ptrdiff_t(oe.offset + oe.bytes + matBytes));
                    }
                    if (materials.empty())  // a version 1 tile: layer 0 everywhere
                        materials = encodeMaterials(std::vector<uint32_t>(kTileSampleCount, 0));
                }
                hdr.entries[e] = {sizeof(RegionHeader) + body.size(), uint32_t(blob.size()), uint32_t(materials.size())};
                body.insert(body.end(), blob.begin(), blob.end());
                body.insert(body.end(), materials.begin(), materials.end());
            }
            std::vector<uint8_t> file(sizeof(hdr) + body.size());
            std::memcpy(file.data(), &hdr, sizeof(hdr));
            std::memcpy(file.data() + sizeof(hdr), body.data(), body.size());
            if (!writeFileAtomic(path, file.data(), file.size())) {
                FX_ERROR("terrain: cannot write %s", path.c_str());
                return false;
            }
            ++written;
        }
    // The coarse map, the level-3 nodes and the coarse material map (what non-resident tiles are
    // known by).
    {
        const auto& n3 = levelNodes_[kCoarseLevel];
        std::vector<uint8_t> file(16 + coarse_.size() * 2 + n3.size() * sizeof(TerrainNode) + coarseMaterials_.size() * 4);
        std::memcpy(file.data(), kCoarseMagic, 4);
        const uint32_t meta[3] = {kFileVersion, coarseSide_, uint32_t(n3.size())};
        std::memcpy(file.data() + 4, meta, sizeof(meta));
        std::memcpy(file.data() + 16, coarse_.data(), coarse_.size() * 2);
        std::memcpy(file.data() + 16 + coarse_.size() * 2, n3.data(), n3.size() * sizeof(TerrainNode));
        std::memcpy(file.data() + 16 + coarse_.size() * 2 + n3.size() * sizeof(TerrainNode), coarseMaterials_.data(),
                    coarseMaterials_.size() * 4);
        if (!writeFileAtomic(dir + "/coarse.bin", file.data(), file.size())) {
            FX_ERROR("terrain: cannot write %s/coarse.bin", dir.c_str());
            return false;
        }
    }
    JsonWriter w;
    w.beginObject()
        .value("version", kFileVersion)
        .value("originX", double(settings_.originX))
        .value("originZ", double(settings_.originZ))
        .value("tiles", settings_.tiles)
        .value("spacing", double(settings_.spacing))
        .value("heightMin", double(settings_.heightMin))
        .value("heightMax", double(settings_.heightMax))
        .endObject();
    // The layers' description as it was handed over (JSON text).
    std::string json = w.str();
    json.insert(json.size() - 1, ",\"layers\":" + layersJson_ + ",\"scatter\":" + scatterJson_ + ",\"generator\":" + generatorJson_);
    json += "\n";
    if (!writeFileAtomic(dir + "/terrain.json", json.data(), json.size()))
        return false;
    for (Tile& t : tiles_)
        t.dirty = false;
    dirtyTiles_ = 0;
    settingsDirty_ = false;
    if (!loader_.joinable())
        loader_ = std::thread([this] { loaderMain(); });
    FX_INFO("terrain: saved %s (%u region files)", dir.c_str(), written);
    return true;
}

bool Terrain::load(const std::string& dir)
{
    FX_ZONE("Terrain::load");
    unload();
    size_t size = 0;
    void* text = SDL_LoadFile((dir + "/terrain.json").c_str(), &size);
    if (!text)
        return false;
    TerrainSettings s;
    std::string layersText = "[]", scatterText = "[]", generatorText = "{}";
    try {
        const Json root = Json::parse(std::string_view(static_cast<const char*>(text), size));
        s.originX = float(root.getNumber("originX", 0.0));
        s.originZ = float(root.getNumber("originZ", 0.0));
        s.tiles = uint32_t(root.getNumber("tiles", 4));
        s.spacing = float(root.getNumber("spacing", 0.5));
        s.heightMin = float(root.getNumber("heightMin", -200.0));
        s.heightMax = float(root.getNumber("heightMax", 800.0));
        if (const Json* layers = root.get("layers"))
            layersText = layers->dump(0);
        if (const Json* scatter = root.get("scatter"))
            scatterText = scatter->dump(0);
        if (const Json* generator = root.get("generator"))
            generatorText = generator->dump(0);
    } catch (const std::exception& e) {
        SDL_free(text);
        FX_ERROR("terrain: %s/terrain.json: %s", dir.c_str(), e.what());
        return false;
    }
    SDL_free(text);
    initLayout(s);
    layersJson_ = layersText;
    scatterJson_ = scatterText;
    generatorJson_ = generatorText;
    std::vector<uint8_t> coarse;
    const auto& n3 = levelNodes_[kCoarseLevel];
    const size_t expect = 16 + coarse_.size() * 2 + n3.size() * sizeof(TerrainNode);  // version 1
    if (!readFile(dir + "/coarse.bin", coarse) || (coarse.size() != expect && coarse.size() != expect + coarseMaterials_.size() * 4) ||
        std::memcmp(coarse.data(), kCoarseMagic, 4)) {
        FX_ERROR("terrain: %s/coarse.bin is missing or does not match terrain.json", dir.c_str());
        tiles_.clear();
        return false;
    }
    std::memcpy(coarse_.data(), coarse.data() + 16, coarse_.size() * 2);
    std::memcpy(levelNodes_[kCoarseLevel].data(), coarse.data() + 16 + coarse_.size() * 2, n3.size() * sizeof(TerrainNode));
    if (coarse.size() > expect)
        std::memcpy(coarseMaterials_.data(), coarse.data() + expect, coarseMaterials_.size() * 4);
    buildCoarseNodes(0, 0, nodesPerSide(kCoarseLevel) - 1, nodesPerSide(kCoarseLevel) - 1);
    dir_ = dir;
    loader_ = std::thread([this] { loaderMain(); });
    FX_INFO("terrain: %s, %ux%u tiles (%.0f m, %.2f m samples), heights %.0f..%.0f", dir.c_str(), settings_.tiles,
            settings_.tiles, double(worldSize()), double(settings_.spacing), double(settings_.heightMin),
            double(settings_.heightMax));
    return true;
}

void Terrain::loaderMain()
{
    std::vector<uint8_t> region;
    std::string regionLoaded;
    for (;;) {
        uint32_t tile;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !requests_.empty(); });
            if (quit_)
                return;
            tile = requests_.front();
            requests_.pop_front();
        }
        const uint32_t tx = tile % settings_.tiles, tz = tile / settings_.tiles;
        const std::string path = regionPath(tx / kRegionTiles, tz / kRegionTiles);
        Loaded result = {tile, {}, {}};
        if (path != regionLoaded) {  // nearby tiles come from the same region: keep the last one read
            if (!readFile(path, region) || region.size() < sizeof(RegionHeader) || std::memcmp(region.data(), kRegionMagic, 4))
                region.clear();
            regionLoaded = path;
        }
        if (!region.empty()) {
            const RegionEntry& e =
                reinterpret_cast<const RegionHeader*>(region.data())->entries[(tz % kRegionTiles) * kRegionTiles + tx % kRegionTiles];
            const uint32_t version = reinterpret_cast<const RegionHeader*>(region.data())->version;
            const uint32_t matBytes = version >= 2 ? e.materialBytes : 0;
            if (e.bytes == 0 || e.offset + e.bytes + matBytes > region.size() || !decodeTile(region.data() + e.offset, e.bytes, result.heights))
                result.heights.clear();
            else if (!matBytes || !decodeMaterials(region.data() + e.offset + e.bytes, matBytes, result.materials))
                result.materials.assign(kTileSampleCount, 0);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        loaded_.push_back(std::move(result));
    }
}

bool Terrain::loading() const
{
    for (const Tile& t : tiles_)
        if (t.loading)
            return true;
    return false;
}

bool Terrain::stream(float3 focus, float radius) { return stream(std::span<const float3>(&focus, 1), radius); }

bool Terrain::stream(std::span<const float3> foci, float radius)
{
    if (!valid() || foci.empty())
        return false;
    ++streamFrame_;
    bool changed = false;
    std::vector<Loaded> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(loaded_);
    }
    for (Loaded& l : done) {
        const uint32_t tx = l.tile % settings_.tiles, tz = l.tile / settings_.tiles;
        Tile& t = tileAt(tx, tz);
        t.loading = false;
        if (!t.heights.empty())
            continue;
        if (l.heights.size() != kTileSampleCount) {
            // Missing from the files: rebuilt from the coarse map (and saved with the next save).
            FX_WARN("terrain: tile %u,%u is missing from %s; rebuilt from the coarse map", tx, tz, dir_.c_str());
            l.heights.resize(kTileSampleCount);  // (sample() reads the coarse map while the tile is empty)
            for (uint32_t z = 0; z < kTileSamples; ++z)
                for (uint32_t x = 0; x < kTileSamples; ++x)
                    l.heights[z * kTileSamples + x] = sample(int32_t(tx * kTileQuads + x), int32_t(tz * kTileQuads + z));
        }
        if (l.materials.size() != kTileSampleCount) {
            l.materials.resize(kTileSampleCount);
            for (uint32_t z = 0; z < kTileSamples; ++z)
                for (uint32_t x = 0; x < kTileSamples; ++x)
                    l.materials[z * kTileSamples + x] = material(int32_t(tx * kTileQuads + x), int32_t(tz * kTileQuads + z));
        }
        t.heights = std::move(l.heights);
        t.materials = std::move(l.materials);
        buildTileNodes(tx, tz);
        ++residentCount_;
        changed = true;
    }
    if (dir_.empty()) {  // never saved: everything is resident already
        if (changed)
            ++residencyRevision_;
        return changed;
    }
    // Wanted tiles, nearest first; far, saved tiles dropped.
    const float tileSize = float(kTileQuads) * settings_.spacing;
    std::vector<std::pair<float, uint32_t>> wanted;
    for (uint32_t tz = 0; tz < settings_.tiles; ++tz)
        for (uint32_t tx = 0; tx < settings_.tiles; ++tx) {
            const float x0 = settings_.originX + float(tx) * tileSize, z0 = settings_.originZ + float(tz) * tileSize;
            float d = FLT_MAX;
            for (const float3& focus : foci) {
                const float dx = std::max({x0 - focus.x, 0.0f, focus.x - (x0 + tileSize)});
                const float dz = std::max({z0 - focus.z, 0.0f, focus.z - (z0 + tileSize)});
                d = std::min(d, std::sqrt(dx * dx + dz * dz));
            }
            Tile& t = tileAt(tx, tz);
            // Pinned tiles hold a sample of the pinned rectangle (they come first).
            const bool pinned = !pinned_.empty() && int32_t((tx + 1) * kTileQuads) >= pinned_.x0 &&
                                int32_t(tx * kTileQuads) <= pinned_.x1 && int32_t((tz + 1) * kTileQuads) >= pinned_.z0 &&
                                int32_t(tz * kTileQuads) <= pinned_.z1;
            if (d <= radius || pinned) {
                t.used = streamFrame_;
                if (t.heights.empty() && !t.loading)
                    wanted.push_back({pinned ? -1.0f : d, tz * settings_.tiles + tx});
            } else if (d > radius * 1.5f && !t.heights.empty() && !t.dirty) {
                t.heights.clear();
                t.heights.shrink_to_fit();
                t.materials.clear();
                t.materials.shrink_to_fit();
                for (auto& n : t.nodes)
                    n.clear();
                --residentCount_;
                changed = true;
            }
        }
    if (!wanted.empty()) {
        std::sort(wanted.begin(), wanted.end());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& w : wanted) {
                requests_.push_back(w.second);
                tileAt(w.second % settings_.tiles, w.second / settings_.tiles).loading = true;
            }
        }
        wake_.notify_one();
    }
    if (changed)
        ++residencyRevision_;
    return changed;
}

}  // namespace fx
