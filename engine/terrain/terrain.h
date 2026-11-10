#pragma once

// Heightfield terrain data: a square grid of tiles of 16-bit heights and material maps (which
// layers show where, and holes), streamed from <scene>.terrain/ around a focus point, plus what
// the renderer needs to pick patch levels.
//
// Samples: (tiles * 256 + 1)^2 on a regular grid, `spacing` apart, height = heightMin +
// value / 65535 * (heightMax - heightMin). Tile (tx, tz) holds samples [256 tx, 256 tx + 256] on
// each axis, so neighbouring tiles share their edge samples (writes update every copy).
//
// Patches (what the renderer draws): a node of level L is 8 x 8 quads of 2^L samples each, its
// 9 x 9 vertices taken from the grid every 2^L samples. Level 0 is full detail; levels 0..5 lie
// inside one tile (level 5 = the whole tile); the root covers the terrain. Every node knows its
// height bounds and its error: the largest height difference between its surface and full detail.
// Levels below kCoarseLevel need their tile resident; coarser levels read the coarse map (every
// 8th sample of the grid, always resident) and their nodes are always known.

#include "core/math.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <span>
#include <vector>

namespace fx {

namespace terrain {
inline constexpr uint32_t kTileQuads = 256;
inline constexpr uint32_t kTileSamples = kTileQuads + 1;
inline constexpr uint32_t kTileSampleCount = kTileSamples * kTileSamples;
inline constexpr uint32_t kPatchQuads = 8;
inline constexpr uint32_t kPatchSamples = kPatchQuads + 1;
inline constexpr uint32_t kTileLevels = 6;   // levels 0..5 fit in one tile
inline constexpr uint32_t kCoarseLevel = 3;  // this level and coarser sample the coarse map
inline constexpr uint32_t kCoarseStep = 1u << kCoarseLevel;
inline constexpr uint32_t kMaxTiles = 64;    // per side: 16384 quads (8 km at 0.5 m)
inline constexpr uint32_t kMaxLayers = 32;   // material layers

// Material map, one uint32 per sample: two layer ids, how much of the second shows (0..255),
// and flags. kHole on a sample cuts out the grid cell it is the minimum corner of; kScatterMask
// << n marks it in scatter layer n's painted mask (n < 7).
inline constexpr uint32_t kHole = 1u << 24;
inline constexpr uint32_t kScatterMask = 1u << 25;
inline uint32_t packMaterial(uint32_t a, uint32_t b, uint32_t weightB, uint32_t flags = 0)
{
    return (a & 0xff) | (b & 0xff) << 8 | (weightB & 0xff) << 16 | flags;
}
inline uint32_t materialA(uint32_t m) { return m & 0xff; }
inline uint32_t materialB(uint32_t m) { return (m >> 8) & 0xff; }
inline uint32_t materialWeightB(uint32_t m) { return (m >> 16) & 0xff; }

// What a change touched (TerrainChange::kinds).
inline constexpr uint32_t kChangeHeights = 1, kChangeMaterials = 2, kChangeHoles = 4;
}  // namespace terrain

struct TerrainSettings {
    float originX = 0.0f, originZ = 0.0f;  // world position of sample (0, 0), the terrain's min corner
    uint32_t tiles = 4;                     // per side, a power of two up to terrain::kMaxTiles
    float spacing = 0.5f;                   // between samples, metres
    float heightMin = -200.0f, heightMax = 800.0f;  // range of the 16-bit heights
};

struct TerrainNode {
    float error;   // largest difference between this node's surface and full detail (m)
    float lo, hi;  // height bounds of the full-detail surface under it (world)
};

// A rectangle of global sample coordinates, inclusive.
struct TerrainRect {
    int32_t x0, z0, x1, z1;
    bool empty() const { return x1 < x0 || z1 < z0; }
};

struct TerrainChange {
    TerrainRect rect;
    uint32_t kinds;  // terrain::kChange*
};

// One tile's raw data (undo snapshots, GPU uploads).
struct TerrainTileData {
    std::vector<uint16_t> heights;    // kTileSamples^2
    std::vector<uint32_t> materials;  // kTileSamples^2
};

class Terrain {
public:
    Terrain() = default;
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;
    ~Terrain();

    // A new terrain at `height` (world), with rolling hills `hills` metres high when > 0. Every
    // tile is resident and unsaved.
    bool create(const TerrainSettings& settings, float height, float hills, uint32_t seed);
    // <scene>.terrain/: settings and the coarse map now, tiles streamed by stream().
    bool load(const std::string& dir);
    // Writes the unsaved tiles (their region files), the coarse map and the settings.
    bool save(const std::string& dir);
    void unload();
    static std::string dirFor(const std::string& scenePath);

    bool valid() const { return !tiles_.empty(); }
    bool dirty() const { return dirtyTiles_ > 0 || settingsDirty_; }
    const TerrainSettings& settings() const { return settings_; }
    uint32_t tilesPerSide() const { return settings_.tiles; }
    uint32_t samplesPerSide() const { return settings_.tiles * terrain::kTileQuads + 1; }
    uint32_t levels() const { return levels_; }  // patch levels 0..levels() - 1
    float worldSize() const { return float(settings_.tiles * terrain::kTileQuads) * settings_.spacing; }
    // World AABB: the terrain's extent and its height bounds.
    void bounds(float lo[3], float hi[3]) const;

    float toWorld(uint16_t v) const { return settings_.heightMin + float(v) * heightScale_; }
    uint16_t fromWorld(float h) const;
    float3 samplePosition(int32_t gx, int32_t gz) const;

    // Samples: full detail where the tile is resident, the coarse map's (bilinear) otherwise.
    bool tileResident(uint32_t tx, uint32_t tz) const;
    uint16_t sample(int32_t gx, int32_t gz) const;  // clamped to the grid
    float heightAt(float x, float z) const;         // world, bilinear on the triangles' grid
    // Nearest hit of a ray with the surface (t along `dir`, which needs not be unit).
    bool raycast(float3 origin, float3 dir, float maxT, float& t) const;

    // Nodes. node() of a level below kCoarseLevel needs nodeAvailable().
    uint32_t nodesPerSide(uint32_t level) const { return (settings_.tiles * terrain::kTileQuads / terrain::kPatchQuads) >> level; }
    bool nodeAvailable(uint32_t level, uint32_t x, uint32_t z) const;
    TerrainNode node(uint32_t level, uint32_t x, uint32_t z) const;
    // A node's 9 x 9 heights (u16), row-major (z then x).
    void patchSamples(uint32_t level, uint32_t x, uint32_t z, uint16_t out[terrain::kPatchSamples * terrain::kPatchSamples]) const;

    // Streaming (main thread, once per frame): tiles within `radius` of `focus` are loaded on a
    // background thread; saved tiles beyond 1.5 x radius are dropped. Returns true when tiles
    // came or went.
    bool stream(float3 focus, float radius);
    // Around several points (a server's players): a tile is wanted near any of them.
    bool stream(std::span<const float3> foci, float radius);
    uint64_t residencyRevision() const { return residencyRevision_; }
    uint32_t residentTiles() const { return residentCount_; }
    bool loading() const;  // tiles queued or being read
    // Tiles of a rectangle loaded and kept (however far from the focus) until unpinned (an
    // empty rectangle). For work over a region (generation).
    void pin(TerrainRect r);
    TerrainRect pinned() const { return pinned_; }

    // Editing (main thread). Global sample rectangles, clamped to the grid; their tiles must be
    // resident (rectResident). Heights are world units.
    TerrainRect clampRect(TerrainRect r) const;
    TerrainRect worldRect(float x0, float z0, float x1, float z1) const;
    bool rectResident(TerrainRect r) const;
    void read(TerrainRect r, std::vector<float>& heights) const;
    void write(TerrainRect r, const std::vector<float>& heights);
    void readMaterials(TerrainRect r, std::vector<uint32_t>& materials) const;
    void writeMaterials(TerrainRect r, const std::vector<uint32_t>& materials, uint32_t kinds = terrain::kChangeMaterials);
    // The material at a sample: full detail where resident, the coarse map's nearest otherwise.
    uint32_t material(int32_t gx, int32_t gz) const;
    // Raw tiles (undo snapshots).
    bool copyTile(uint32_t tx, uint32_t tz, TerrainTileData& out) const;
    void restoreTile(uint32_t tx, uint32_t tz, const TerrainTileData& data);
    // Resident tiles' data in place (nullptr when not resident), and the coarse maps.
    const uint16_t* tileHeights(uint32_t tx, uint32_t tz) const;
    const uint32_t* tileMaterials(uint32_t tx, uint32_t tz) const;
    const std::vector<uint16_t>& coarseHeights() const { return coarse_; }
    const std::vector<uint32_t>& coarseMaterials() const { return coarseMaterials_; }
    uint32_t coarseSide() const { return coarseSide_; }

    // The material layers' description (terrain/terrain_layers.h), kept in terrain.json.
    const std::string& layersJson() const { return layersJson_; }
    void setLayersJson(std::string json);
    // The scatter layers (terrain/terrain_scatter.h), kept in terrain.json too.
    const std::string& scatterJson() const { return scatterJson_; }
    void setScatterJson(std::string json);
    // The generator stack (terrain/terrain_generate.h), kept in terrain.json too ("{}": none).
    const std::string& generatorJson() const { return generatorJson_; }
    void setGeneratorJson(std::string json);

    // What changed: every write and restore appends its rectangle under a new revision.
    // Consumers keep the revision they last saw; false means the log no longer reaches back that
    // far (redo everything).
    uint64_t revision() const { return revision_; }
    bool changesSince(uint64_t revision, std::vector<TerrainChange>& out) const;

private:
    struct Tile {
        std::vector<uint16_t> heights;  // kTileSamples^2, empty when not resident
        std::vector<uint32_t> materials;  // kTileSamples^2 while resident
        // Nodes of levels 0..2 (32^2, 16^2, 8^2), valid while resident.
        std::vector<TerrainNode> nodes[terrain::kCoarseLevel];
        bool dirty = false;    // differs from the file
        bool loading = false;  // queued on the loader thread
        uint64_t used = 0;     // last stream() that wanted it
    };
    struct Loaded {
        uint32_t tile;
        std::vector<uint16_t> heights;
        std::vector<uint32_t> materials;
    };

    void initLayout(const TerrainSettings& s);
    Tile& tileAt(uint32_t tx, uint32_t tz) { return tiles_[tz * settings_.tiles + tx]; }
    const Tile& tileAt(uint32_t tx, uint32_t tz) const { return tiles_[tz * settings_.tiles + tx]; }
    uint16_t coarseSample(uint32_t cx, uint32_t cz) const { return coarse_[cz * coarseSide_ + cx]; }
    // Nodes of levels 0..3 of a resident tile, and its coarse-map samples, from its heights.
    void buildTileNodes(uint32_t tx, uint32_t tz);
    // Levels 4.. over a rectangle of level-3 nodes (inclusive), from the coarse map.
    void buildCoarseNodes(uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1);
    TerrainNode& globalNode(uint32_t level, uint32_t x, uint32_t z)
    {
        return levelNodes_[level][z * nodesPerSide(level) + x];
    }
    void logChange(TerrainRect r, uint32_t kinds);
    // Every tile holding a sample of r (edge samples live in two or four tiles).
    void tilesOf(TerrainRect r, uint32_t& tx0, uint32_t& tz0, uint32_t& tx1, uint32_t& tz1) const;
    void loaderMain();
    std::string regionPath(uint32_t rx, uint32_t rz) const;

    TerrainSettings settings_;
    float heightScale_ = 1.0f;
    uint32_t levels_ = 0;
    std::vector<Tile> tiles_;
    std::vector<uint16_t> coarse_;  // every kCoarseStep-th sample, coarseSide_^2
    std::vector<uint32_t> coarseMaterials_;  // the same samples' materials
    std::string layersJson_ = "[]", scatterJson_ = "[]", generatorJson_ = "{}";
    uint32_t coarseSide_ = 0;
    std::vector<std::vector<TerrainNode>> levelNodes_;  // levels kCoarseLevel.. (lower levels: per tile)
    uint32_t dirtyTiles_ = 0, residentCount_ = 0;
    TerrainRect pinned_ = {0, 0, -1, -1};
    bool settingsDirty_ = false;
    uint64_t revision_ = 0, residencyRevision_ = 0, streamFrame_ = 0;
    std::deque<std::pair<uint64_t, TerrainChange>> log_;

    // Background tile loads from the region files.
    std::string dir_;
    std::thread loader_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<uint32_t> requests_;
    std::vector<Loaded> loaded_;
    bool quit_ = false;
};

// Seeded 2D gradient noise in [-1, 1] and its fractal sum (fBm). Deterministic across platforms.
float terrainNoise(float x, float z, uint32_t seed);
float terrainFbm(float x, float z, uint32_t octaves, uint32_t seed);

}  // namespace fx
