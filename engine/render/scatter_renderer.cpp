#include "render/scatter_renderer.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "render/terrain_surface.h"
#include "render/virtual_shadow_map.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"

#include "terrain_scatter.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

using namespace terrain;

namespace {

struct SubLayerGpu {
    uint32_t a[4];
    float b[4], c[4], d[4];
    int32_t m[4];
    float e[4], f[4], g[4];
};
static_assert(sizeof(SubLayerGpu) == 128);

struct JobGpu {
    int32_t cell[2];
    uint32_t sub, slotBase, mode, exclusionOffset, exclusionCount, pad;  // mode 1: exclusionOffset = mesh, exclusionCount = slots
};
static_assert(sizeof(JobGpu) == 32);

struct ScatterUniforms {
    float4 grid, heights;
    uint32_t info[4];
};

constexpr uint32_t kMaxSubs = 256;

}  // namespace

bool ScatterRenderer::init(const GpuDevice& gpu, const ScenePak& pak, VirtualShadowMap& vsm)
{
    device_ = gpu.get();
    pak_ = &pak;
    vsm_ = &vsm;
    pass_ = gpu.createComputePipeline(shaders::terrain_scatter_cs_scatter);
    auto buffer = [&](uint32_t bytes, const char* name) {
        SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, bytes, 0};
        SDL_GPUBuffer* buf = gpumem::createBuffer("Scatter", device_, &b);
        if (buf)
            gpumem::setBufferName(device_, buf, name);
        return buf;
    };
    jobCapacity_ = kMaxJobsPerFrame * 2;
    jobs_ = buffer(jobCapacity_ * sizeof(JobGpu), "scatter jobs");
    params_ = buffer(kMaxSubs * sizeof(SubLayerGpu), "scatter layers");
    exclusionCapacity_ = 4096;
    exclusions_ = buffer(exclusionCapacity_ * 4, "scatter exclusions");
    return pass_ && jobs_ && params_ && exclusions_;
}

void ScatterRenderer::shutdown()
{
    if (!device_)
        return;
    for (Download& d : downloads_)
        gpumem::releaseTransferBuffer(device_, d.buffer);
    downloads_.clear();
    for (SDL_GPUBuffer* b : {jobs_, params_, exclusions_})
        gpumem::releaseBuffer(device_, b);
    gpumem::releaseTransferBuffer(device_, upload_);
    SDL_ReleaseGPUComputePipeline(device_, pass_);
    device_ = nullptr;
}

void ScatterRenderer::setTerrain(const Terrain* terrain)
{
    terrain_ = terrain && terrain->valid() ? terrain : nullptr;
    scatterJson_ = "\x01";  // parsed again on the next update, every cell cleared then
    reset_ = true;
}

bool ScatterRenderer::ownsSlot(uint32_t slot) const
{
    for (const ScatterPool& p : pools_)
        if (slot >= p.firstSlot && slot < p.firstSlot + p.count)
            return true;
    for (const auto& [first, count] : arena_)  // (spare slots: hidden, or a cell being cleared)
        if (slot >= first && slot < first + count)
            return true;
    return false;
}

bool ScatterRenderer::find(uint32_t slot, Hit& hit) const
{
    for (uint32_t s = 0; s < subs_.size(); ++s) {
        const Sub& sub = subs_[s];
        if (slot < sub.first || slot >= sub.first + sub.blocks * sub.k * sub.k)
            continue;
        const uint32_t block = (slot - sub.first) / (sub.k * sub.k);
        for (const auto& [key, c] : cells_)
            if (c.sub == s && c.block == block) {
                hit = {sub.layer, sub.asset, c.x, c.z, (slot - sub.first) % (sub.k * sub.k)};
                return true;
            }
    }
    return false;
}

void ScatterRenderer::setPools(std::vector<ScatterPool> pools, uint32_t spareFirst, uint32_t spareCount)
{
    // The slots the pools were reserved in, and the spare ones, as ranges: every layer list is
    // given slots from them again, in order (the reserved layers get the same ranges back).
    arena_.clear();
    for (const ScatterPool& p : pools) {
        if (!arena_.empty() && arena_.back().first + arena_.back().second == p.firstSlot)
            arena_.back().second += p.count;
        else
            arena_.push_back({p.firstSlot, p.count});
    }
    if (spareCount) {
        if (!arena_.empty() && arena_.back().first + arena_.back().second == spareFirst)
            arena_.back().second += spareCount;
        else
            arena_.push_back({spareFirst, spareCount});
    }
    pools_ = std::move(pools);
}

void ScatterRenderer::assignPools(const std::vector<ScatterLayer>& layers)
{
    pools_.clear();
    stats_.unplaced = 0;
    size_t range = 0;
    uint32_t used = 0;  // of arena_[range]
    for (uint32_t l = 0; l < layers.size(); ++l)
        for (uint32_t a = 0; a < layers[l].assets.size(); ++a) {
            const int32_t mesh = pak_->findMesh(layers[l].assets[a].mesh);
            if (mesh < 0)
                continue;
            const uint32_t want = layers[l].assets[a].budget;
            while (range < arena_.size() && arena_[range].second - used < want && arena_[range].second - used < 4096) {
                ++range;  // (what is left of a range too small for it: the next range)
                used = 0;
            }
            if (range >= arena_.size()) {
                ++stats_.unplaced;
                continue;
            }
            const uint32_t count = std::min(want, arena_[range].second - used);
            pools_.push_back({l, a, uint32_t(mesh), arena_[range].first + used, count});
            used += count;
        }
}

void ScatterRenderer::buildSubs(const std::vector<ScatterLayer>& layers, std::vector<Sub>& subs, std::vector<uint8_t>& params) const
{
    subs.clear();
    params.assign(kMaxSubs * sizeof(SubLayerGpu), 0);
    std::vector<std::string> looks;  // the terrain's layers' generated looks
    if (terrain_)
        for (const TerrainLayer& t : parseTerrainLayers(terrain_->layersJson()))
            looks.push_back(t.generated);
    for (uint32_t pi = 0; pi < pools_.size() && subs.size() < kMaxSubs; ++pi) {
        const ScatterPool& pool = pools_[pi];
        if (pool.layer >= layers.size())
            continue;
        const ScatterLayer& l = layers[pool.layer];
        if (!l.enabled || pool.asset >= l.assets.size() || pak_->findMesh(l.assets[pool.asset].mesh) != int32_t(pool.mesh))
            continue;
        float total = 0.0f;
        for (const ScatterAsset& a : l.assets)
            total += a.weight;
        const ScatterAsset& asset = l.assets[pool.asset];
        const float cell = scatterCellSize(l);
        const float expected = l.density * (total > 0.0f ? asset.weight / total : 0.0f) * cell * cell;
        if (expected <= 0.0f)
            continue;
        Sub s;
        s.layer = pool.layer;
        s.asset = pool.asset;
        s.pool = pi;
        s.first = pool.firstSlot;
        s.mesh = pool.mesh;
        s.k = std::clamp(uint32_t(std::ceil(std::sqrt(expected))), 1u, 64u);
        s.cellSize = cell;
        s.drawDistance = l.drawDistance;
        s.blocks = pool.count / (s.k * s.k);
        for (uint32_t b = s.blocks; b-- > 0;)
            s.freeBlocks.push_back(b);
        s.collision = l.collision;
        s.paintOnly = l.paintBit >= 0 && l.paint == 2 ? l.paintBit : -1;
        const fpak::MeshGpu& mesh = pak_->meshes()[pool.mesh];
        SubLayerGpu g = {};
        g.a[0] = pool.mesh;
        g.a[1] = s.k;
        g.a[2] = l.seed * 0x9e3779b9u + pool.asset * 0x85ebca6bu;
        const uint32_t paint = l.paintBit >= 0 ? l.paint : 0u;
        g.a[3] = (l.castShadows ? 0u : 1u) | paint << 8 | uint32_t(std::max(l.paintBit, 0)) << 16;
        g.b[0] = cell;
        g.b[1] = std::min(expected / float(s.k * s.k), 1.0f);
        g.b[2] = asset.scaleMin;
        g.b[3] = std::max(asset.scaleMax, asset.scaleMin);
        g.c[0] = l.slope[0];
        g.c[1] = l.slope[1];
        g.c[2] = l.height[0];
        g.c[3] = l.height[1];
        g.d[0] = 1.0f / l.clumpScale;
        g.d[1] = l.clumpAmount;
        g.d[2] = l.clumpThreshold;
        g.d[3] = l.materialMin;
        std::memcpy(g.m, l.materialLayers, sizeof(g.m));
        // (Listed layers first: the shader stops at the first -1 in m.x.)
        std::stable_partition(g.m, g.m + 4, [](int32_t v) { return v >= 0; });
        g.e[0] = l.align;
        g.e[1] = l.tilt * (kPi / 180.0f);
        g.e[2] = l.sink;
        g.e[3] = l.tintVariation;
        g.f[0] = -mesh.posOffset[1];  // the mesh's lowest point stands on the ground
        g.f[1] = l.jitter;
        g.f[2] = l.yawRange * (kPi / 180.0f);
        // Terrain shading: the layer (by number, or the first with the look), the base blend.
        int32_t shade = l.terrainLayer;
        if (!l.terrainLook.empty())
            for (size_t i = 0; i < looks.size(); ++i)
                if (looks[i] == l.terrainLook) {
                    shade = int32_t(i);
                    break;
                }
        g.g[0] = float(shade);
        g.g[1] = l.baseBlend;
        std::memcpy(params.data() + subs.size() * sizeof(SubLayerGpu), &g, sizeof(g));
        subs.push_back(std::move(s));
    }
}

uint64_t ScatterRenderer::tileRevisions(const Cell& c, const TerrainSurface& surface) const
{
    const TerrainSettings& s = terrain_->settings();
    const float size = subs_[c.sub].cellSize, tile = float(kTileQuads) * s.spacing;
    const uint32_t T = terrain_->tilesPerSide();
    auto tileOf = [&](float v, float origin) { return uint32_t(std::clamp((v - origin) / tile, 0.0f, float(T - 1))); };
    uint64_t sum = 0;
    for (uint32_t tz = tileOf(float(c.z) * size, s.originZ); tz <= tileOf(float(c.z + 1) * size, s.originZ); ++tz)
        for (uint32_t tx = tileOf(float(c.x) * size, s.originX); tx <= tileOf(float(c.x + 1) * size, s.originX); ++tx)
            sum = sum * 31 + surface.tileRevision(tz * T + tx);
    return sum;
}

uint8_t ScatterRenderer::tilePaintBits(uint32_t tx, uint32_t tz)
{
    const uint32_t T = terrain_->tilesPerSide();
    if (tilePaint_.size() != size_t(T) * T)
        tilePaint_.assign(size_t(T) * T, {});
    TilePaint& p = tilePaint_[tz * T + tx];
    const uint32_t* full = terrain_->tileMaterials(tx, tz);
    if (p.valid && p.resident == (full != nullptr))
        return p.bits;
    uint32_t bits = 0;
    if (full) {
        for (uint32_t i = 0; i < kTileSampleCount; ++i)
            bits |= full[i];
    } else {
        const std::vector<uint32_t>& coarse = terrain_->coarseMaterials();
        const uint32_t side = terrain_->coarseSide(), per = kTileQuads / kCoarseStep;
        for (uint32_t z = tz * per; z <= std::min((tz + 1) * per, side - 1); ++z)
            for (uint32_t x = tx * per; x <= std::min((tx + 1) * per, side - 1); ++x)
                bits |= coarse[size_t(z) * side + x];
    }
    p = {true, full != nullptr, uint8_t((bits / terrain::kScatterMask) & 0x7f)};
    return p.bits;
}

bool ScatterRenderer::cellPainted(const Sub& sub, int32_t x, int32_t z)
{
    const TerrainSettings& s = terrain_->settings();
    const float size = sub.cellSize, tile = float(kTileQuads) * s.spacing;
    const uint32_t T = terrain_->tilesPerSide();
    auto tileOf = [&](float v, float origin) { return uint32_t(std::clamp((v - origin) / tile, 0.0f, float(T - 1))); };
    for (uint32_t tz = tileOf(float(z) * size, s.originZ); tz <= tileOf(float(z + 1) * size, s.originZ); ++tz)
        for (uint32_t tx = tileOf(float(x) * size, s.originX); tx <= tileOf(float(x + 1) * size, s.originX); ++tx)
            if (tilePaintBits(tx, tz) & (1u << sub.paintOnly))
                return true;
    return false;
}

float4 ScatterRenderer::cellSphere(const Cell& c) const
{
    const Sub& sub = subs_[c.sub];
    const float x0 = float(c.x) * sub.cellSize, z0 = float(c.z) * sub.cellSize, h = sub.cellSize * 0.5f;
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 9; ++i) {
        const float y = terrain_->heightAt(x0 + float(i % 3) * h, z0 + float(i / 3) * h);
        lo = std::min(lo, y);
        hi = std::max(hi, y);
    }
    const fpak::MeshGpu& m = pak_->meshes()[sub.mesh];
    const float tall = std::max({m.posScale[0], m.posScale[1], m.posScale[2]}) * 65535.0f *
                       layers_[sub.layer].assets[sub.asset].scaleMax;
    const float halfY = (hi - lo) * 0.5f + tall;
    return {x0 + h, (lo + hi) * 0.5f, z0 + h, std::sqrt(2.0f * h * h + halfY * halfY)};
}

void ScatterRenderer::update(SDL_GPUCommandBuffer* cmd, SDL_GPUBuffer* prevInstances, const TerrainSurface& surface, float3 camera)
{
    FX_ZONE_CAT("ScatterRenderer::update", Rendering);
    if (!device_ || (pools_.empty() && subs_.empty() && arena_.empty()))
        return;
    std::vector<JobGpu> jobs;
    std::vector<uint32_t> exclusions;
    std::vector<float4> spheres;
    auto clear = [&](const Cell& c) {
        const Sub& sub = subs_[c.sub];
        const uint32_t slots = sub.k * sub.k;
        jobs.push_back({{c.x, c.z}, 0, sub.first + c.block * slots, 1, sub.mesh, slots, 0});
        if (c.generated)
            spheres.push_back(cellSphere(c));
    };

    // A changed layer list. With the same cells and slots (rules, deletions, draw distance
    // changed): the cells of changed layers are generated again in place. Otherwise (layers or
    // assets added, removed, enabled, a new density or terrain): every cell cleared, a new layout.
    const bool have = terrain_ && terrain_->valid();
    const std::string json = have ? terrain_->scatterJson() : std::string("[]");
    // The terrain's layer list: which layer a terrain look names (every cell again when it moves).
    const std::string looks = have ? terrain_->layersJson() : std::string("[]");
    const bool looksChanged = looks != layersJson_;
    layersJson_ = looks;
    if (json != scatterJson_ || looksChanged) {
        scatterJson_ = json;
        std::vector<ScatterLayer> layers = have ? parseScatterLayers(json) : std::vector<ScatterLayer>();
        std::vector<Sub> subs;
        std::vector<uint8_t> params;
        if (!arena_.empty())
            assignPools(layers);  // (layers and meshes added since the scene loaded get slots too)
        buildSubs(layers, subs, params);
        bool same = !reset_ && subs.size() == subs_.size();
        for (size_t i = 0; same && i < subs.size(); ++i)
            same = subs[i].pool == subs_[i].pool && subs[i].first == subs_[i].first && subs[i].blocks == subs_[i].blocks &&
                   subs[i].k == subs_[i].k && subs[i].cellSize == subs_[i].cellSize;
        if (same) {
            auto rules = [](ScatterLayer l) {
                l.exclusions.clear();
                return scatterLayersJson({l});
            };
            auto excluded = [](const ScatterLayer& l, const Cell& c, uint32_t asset) {
                std::vector<int32_t> out;
                for (const auto& x : l.exclusions)
                    if (x[0] == c.x && x[1] == c.z && uint32_t(x[2]) == asset)
                        out.push_back(x[3]);
                std::sort(out.begin(), out.end());
                return out;
            };
            std::vector<uint8_t> changed(layers.size());
            for (size_t l = 0; l < layers.size(); ++l)
                changed[l] = looksChanged || l >= layers_.size() || rules(layers[l]) != rules(layers_[l]);  // (a layer added: no pool yet)
            for (auto& [key, c] : cells_) {
                const Sub& old = subs_[c.sub];
                if (changed[old.layer] || excluded(layers[old.layer], c, old.asset) != excluded(layers_[old.layer], c, old.asset)) {
                    c.stale = true;
                    if (c.generated)
                        spheres.push_back(cellSphere(c));  // the shadow pages of the old instances (old scales)
                }
                if (old.collision && !subs[c.sub].collision && c.downloaded) {
                    collisionRemoved_.push_back(key);
                    c.downloaded = false;
                }
            }
            for (size_t i = 0; i < subs.size(); ++i)
                subs[i].freeBlocks = std::move(subs_[i].freeBlocks);
        } else {
            for (const auto& [key, c] : cells_) {
                clear(c);
                if (subs_[c.sub].collision && c.downloaded)
                    collisionRemoved_.push_back(key);
            }
            cells_.clear();
            if (have)
                terrainRevision_ = terrain_->revision();
        }
        layers_ = std::move(layers);
        subs_ = std::move(subs);
        stats_.subs = uint32_t(subs_.size());
        tilePaint_.clear();
        paramsCpu_ = std::move(params);
        paramsDirty_ = true;
        reset_ = false;
    }

    if (have && !subs_.empty()) {
        // What the terrain changed: cells over it are generated again.
        std::vector<TerrainChange> changes;
        const bool all = !terrain_->changesSince(terrainRevision_, changes);
        terrainRevision_ = terrain_->revision();
        const TerrainSettings& ts = terrain_->settings();
        if (all) {
            tilePaint_.clear();
        } else {
            const int32_t T = int32_t(terrain_->tilesPerSide()), q = int32_t(kTileQuads);
            for (const TerrainChange& ch : changes)
                if ((ch.kinds & terrain::kChangeMaterials) && !tilePaint_.empty())
                    for (int32_t tz = std::max(ch.rect.z0 / q - 1, 0); tz <= std::min(ch.rect.z1 / q, T - 1); ++tz)
                        for (int32_t tx = std::max(ch.rect.x0 / q - 1, 0); tx <= std::min(ch.rect.x1 / q, T - 1); ++tx)
                            tilePaint_[size_t(tz) * T + tx].valid = false;
        }
        for (auto& [key, c] : cells_) {
            const float size = subs_[c.sub].cellSize;
            const float x0 = float(c.x) * size, z0 = float(c.z) * size;
            bool touched = all;
            for (const TerrainChange& ch : changes) {
                const float rx0 = ts.originX + float(ch.rect.x0 - 1) * ts.spacing, rx1 = ts.originX + float(ch.rect.x1 + 1) * ts.spacing;
                const float rz0 = ts.originZ + float(ch.rect.z0 - 1) * ts.spacing, rz1 = ts.originZ + float(ch.rect.z1 + 1) * ts.spacing;
                touched = touched || (rx0 <= x0 + size && rx1 >= x0 && rz0 <= z0 + size && rz1 >= z0);
            }
            if (touched || (c.generated && tileRevisions(c, surface) != c.revision))
                c.stale = true;
        }

        // Wanted cells (within each layer's draw distance) and cells to drop (beyond 1.15 x).
        struct Want {
            float distance;
            uint32_t sub;
            int32_t x, z;
        };
        std::vector<Want> wanted;
        const float tx0 = ts.originX, tz0 = ts.originZ, tx1 = ts.originX + terrain_->worldSize(), tz1 = ts.originZ + terrain_->worldSize();
        auto distance = [&](float size, int32_t x, int32_t z) {
            const float x0 = float(x) * size, z0 = float(z) * size;
            const float dx = std::max({x0 - camera.x, 0.0f, camera.x - (x0 + size)});
            const float dz = std::max({z0 - camera.z, 0.0f, camera.z - (z0 + size)});
            return std::sqrt(dx * dx + dz * dz);
        };
        for (uint32_t s = 0; s < subs_.size(); ++s) {
            const Sub& sub = subs_[s];
            const float C = sub.cellSize, dd = sub.drawDistance;
            const int32_t cx0 = int32_t(std::floor(std::max(camera.x - dd, tx0) / C)), cx1 = int32_t(std::floor(std::min(camera.x + dd, tx1) / C));
            const int32_t cz0 = int32_t(std::floor(std::max(camera.z - dd, tz0) / C)), cz1 = int32_t(std::floor(std::min(camera.z + dd, tz1) / C));
            for (int32_t z = cz0; z <= cz1; ++z)
                for (int32_t x = cx0; x <= cx1; ++x)
                    if (const float d = distance(C, x, z); d <= dd && !cells_.count(cellKey(s, x, z)) &&
                                                           (sub.paintOnly < 0 || cellPainted(sub, x, z)))
                        wanted.push_back({d, s, x, z});
        }
        for (auto it = cells_.begin(); it != cells_.end();) {
            Cell& c = it->second;
            Sub& sub = subs_[c.sub];
            if (distance(sub.cellSize, c.x, c.z) <= sub.drawDistance * 1.15f && (sub.paintOnly < 0 || cellPainted(sub, c.x, c.z))) {
                ++it;
                continue;
            }
            clear(c);
            sub.freeBlocks.push_back(c.block);
            if (sub.collision && c.downloaded)
                collisionRemoved_.push_back(it->first);
            it = cells_.erase(it);
        }
        std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.distance < b.distance; });
        stats_.starved = 0;
        // Out of blocks: a nearer cell takes the block of the farthest one (the camera moved on and
        // the budget is short; otherwise the far cells keep theirs and the near ground stays bare).
        std::vector<std::vector<std::pair<float, uint64_t>>> farthest;  // per sub, nearest first (popped from the back)
        uint32_t evicted = 0;
        for (const Want& w : wanted) {
            Sub& sub = subs_[w.sub];
            if (sub.freeBlocks.empty() && evicted < kMaxJobsPerFrame) {
                if (farthest.empty()) {
                    farthest.resize(subs_.size());
                    for (const auto& [key, c] : cells_)
                        farthest[c.sub].push_back({distance(subs_[c.sub].cellSize, c.x, c.z), key});
                    for (auto& f : farthest)
                        std::sort(f.begin(), f.end());
                }
                auto& f = farthest[w.sub];
                if (!f.empty() && f.back().first > w.distance * 1.25f + sub.cellSize) {
                    const auto it = cells_.find(f.back().second);
                    f.pop_back();
                    clear(it->second);
                    sub.freeBlocks.push_back(it->second.block);
                    if (sub.collision && it->second.downloaded)
                        collisionRemoved_.push_back(it->first);
                    cells_.erase(it);
                    ++evicted;
                }
            }
            if (sub.freeBlocks.empty()) {
                ++stats_.starved;
                continue;
            }
            Cell c = {w.sub, w.x, w.z, sub.freeBlocks.back(), 0, 0, false, true, false};
            sub.freeBlocks.pop_back();
            cells_.emplace(cellKey(w.sub, w.x, w.z), c);
        }
        // Generate the stale cells, nearest first, as many as the frame allows.
        std::vector<std::pair<float, uint64_t>> stale;
        for (const auto& [key, c] : cells_)
            if (c.stale)
                stale.push_back({distance(subs_[c.sub].cellSize, c.x, c.z), key});
        std::sort(stale.begin(), stale.end());
        uint32_t generated = 0;
        for (const auto& [d, key] : stale) {
            if (generated++ == kMaxJobsPerFrame)
                break;
            Cell& c = cells_[key];
            const Sub& sub = subs_[c.sub];
            const uint32_t slots = sub.k * sub.k;
            const uint32_t first = uint32_t(exclusions.size());
            for (const auto& x : layers_[sub.layer].exclusions)
                if (x[0] == c.x && x[1] == c.z && uint32_t(x[2]) == sub.asset)
                    exclusions.push_back(uint32_t(x[3]));
            jobs.push_back({{c.x, c.z}, c.sub, sub.first + c.block * slots, 0, first, uint32_t(exclusions.size()) - first, 0});
            if (sub.collision && c.downloaded)
                collisionRemoved_.push_back(key);  // its new rows are read back again
            c.generated = true;
            c.stale = false;
            c.downloaded = false;
            c.generation = ++generations_;
            c.revision = tileRevisions(c, surface);
            spheres.push_back(cellSphere(c));
        }
        // Collision: generated cells near the focus whose rows were not read back yet.
        for (auto& [key, c] : cells_) {
            const Sub& sub = subs_[c.sub];
            if (!sub.collision || !c.generated || c.stale)
                continue;
            const float x0 = float(c.x) * sub.cellSize, z0 = float(c.z) * sub.cellSize;
            const float dx = std::max({x0 - collisionFocus_.x, 0.0f, collisionFocus_.x - (x0 + sub.cellSize)});
            const float dz = std::max({z0 - collisionFocus_.z, 0.0f, collisionFocus_.z - (z0 + sub.cellSize)});
            const bool near = dx * dx + dz * dz <= collisionRadius_ * collisionRadius_;
            const bool pending = std::any_of(downloads_.begin(), downloads_.end(),
                                             [&](const Download& d) { return d.cell == key && d.generation == c.generation; });
            if (near && !c.downloaded && !pending) {
                downloads_.push_back({key, c.sub, sub.k * sub.k, c.generation, nullptr, 0, false});
                c.downloaded = true;
            } else if (!near && c.downloaded) {
                collisionRemoved_.push_back(key);
                c.downloaded = false;
            }
        }
    }
    stats_.jobs = uint32_t(jobs.size());
    stats_.cells = uint32_t(cells_.size());
    stats_.slots = 0;
    for (const auto& [key, c] : cells_)
        stats_.slots += subs_[c.sub].k * subs_[c.sub].k;
    if (jobs.empty() && !paramsDirty_)
        return;

    // Upload: layer parameters (when changed), jobs, exclusions; the shadow pages over the cells.
    if (jobs.size() > jobCapacity_) {  // (clearing every cell of a new layout)
        gpumem::releaseBuffer(device_, jobs_);
        jobCapacity_ = uint32_t(jobs.size() * 2);
        SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, jobCapacity_ * uint32_t(sizeof(JobGpu)), 0};
        jobs_ = gpumem::createBuffer("Scatter", device_, &b);
        gpumem::setBufferName(device_, jobs_, "scatter jobs");
    }
    if (exclusions.size() > exclusionCapacity_) {
        gpumem::releaseBuffer(device_, exclusions_);
        exclusionCapacity_ = uint32_t(exclusions.size() * 2);
        SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, exclusionCapacity_ * 4, 0};
        exclusions_ = gpumem::createBuffer("Scatter", device_, &b);
        gpumem::setBufferName(device_, exclusions_, "scatter exclusions");
    }
    const uint32_t paramsBytes = paramsDirty_ ? uint32_t(paramsCpu_.size()) : 0u;
    const uint32_t jobBytes = uint32_t(jobs.size() * sizeof(JobGpu)), exclusionBytes = uint32_t(exclusions.size() * 4);
    const uint32_t total = paramsBytes + jobBytes + exclusionBytes + 16;
    if (total > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = std::max(total * 2, 1u << 16);
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Scatter", device_, &info, "scatter upload");
    }
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    if (paramsBytes)
        std::memcpy(mapped, paramsCpu_.data(), paramsBytes);
    std::memcpy(mapped + paramsBytes, jobs.data(), jobBytes);
    std::memcpy(mapped + paramsBytes + jobBytes, exclusions.data(), exclusionBytes);
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_SECTION(cmd, "Scatter");
    FX_GPU_PASS(cmd, "scatter upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    if (paramsBytes) {
        const SDL_GPUTransferBufferLocation src = {upload_, 0};
        const SDL_GPUBufferRegion dst = {params_, 0, paramsBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        paramsDirty_ = false;
    }
    if (jobBytes) {
        const SDL_GPUTransferBufferLocation src = {upload_, paramsBytes};
        const SDL_GPUBufferRegion dst = {jobs_, 0, jobBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    if (exclusionBytes) {
        const SDL_GPUTransferBufferLocation src = {upload_, paramsBytes + jobBytes};
        const SDL_GPUBufferRegion dst = {exclusions_, 0, exclusionBytes};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    vsm_->uploadInvalidations(copy, spheres, true);
    SDL_EndGPUCopyPass(copy);
    if (jobs.empty() || !surface.coarseHeights())
        return;

    const TerrainSettings& s = terrain_->settings();
    ScatterUniforms u = {};
    u.grid = {s.originX, s.originZ, s.spacing, 1.0f / s.spacing};
    u.heights = {s.heightMin, s.heightMax - s.heightMin, float(terrain_->coarseSide()), float(s.tiles)};
    u.info[0] = TerrainSurface::pageTableOffset();
    u.info[1] = uint32_t(jobs.size());
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    FX_GPU_PASS(cmd, "scatter generate");
    const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{pak_->sceneBuffer(), false}, {prevInstances, false}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, 2);
    SDL_BindGPUComputePipeline(pass, pass_);
    const SDL_GPUTextureSamplerBinding samplers[4] = {{surface.coarseHeights(), surface.linearClamp()},
                                                      {surface.tileHeights(), surface.linearClamp()},
                                                      {surface.coarseMaterials(), surface.pointClamp()},
                                                      {surface.tileMaterials(), surface.pointClamp()}};
    SDL_BindGPUComputeSamplers(pass, 0, samplers, 4);
    SDL_GPUBuffer* ro[4] = {jobs_, params_, surface.data(), exclusions_};
    SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 4);
    SDL_DispatchGPUCompute(pass, uint32_t(jobs.size()), 1, 1);
    SDL_EndGPUComputePass(pass);
}

void ScatterRenderer::recordDownloads(SDL_GPUCopyPass* copy, uint32_t ring)
{
    for (Download& d : downloads_) {
        // Recorded into this slot by a frame never submitted with a fence (a capture): again.
        if (d.recorded && d.ring == ring) {
            gpumem::releaseTransferBuffer(device_, d.buffer);
            d.buffer = nullptr;
            d.recorded = false;
        }
        if (d.recorded)
            continue;
        const auto it = cells_.find(d.cell);
        if (it == cells_.end() || it->second.generation != d.generation)
            continue;
        const Sub& sub = subs_[it->second.sub];
        const uint32_t bytes = d.slots * uint32_t(sizeof(fpak::InstanceGpu));
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0};
        d.buffer = gpumem::createTransferBuffer("Scatter", device_, &info, "scatter collision readback");
        if (!d.buffer)
            continue;
        const SDL_GPUBufferRegion src = {pak_->sceneBuffer(),
                                         pak_->gpuSectionOffset(fpak::Section::Instances) +
                                             (sub.first + it->second.block * d.slots) * uint32_t(sizeof(fpak::InstanceGpu)),
                                         bytes};
        const SDL_GPUTransferBufferLocation dst = {d.buffer, 0};
        SDL_DownloadFromGPUBuffer(copy, &src, &dst);
        d.recorded = true;
        d.ring = ring;
    }
    // Cells that went away before their download was recorded.
    downloads_.erase(std::remove_if(downloads_.begin(), downloads_.end(), [&](const Download& d) {
                                        const auto it = cells_.find(d.cell);
                                        return !d.recorded && (it == cells_.end() || it->second.generation != d.generation);
                                    }),
                     downloads_.end());
}

void ScatterRenderer::finishDownloads(uint32_t ring)
{
    for (auto it = downloads_.begin(); it != downloads_.end();) {
        if (!it->recorded || it->ring != ring) {
            ++it;
            continue;
        }
        const auto owner = cells_.find(it->cell);
        if (owner != cells_.end() && owner->second.generation == it->generation && owner->second.downloaded)
            if (const auto* rows = static_cast<const fpak::InstanceGpu*>(SDL_MapGPUTransferBuffer(device_, it->buffer, false))) {
                ScatterCellBodies cell = {it->cell, {}};
                for (uint32_t i = 0; i < it->slots; ++i)
                    if (!(rows[i].flags & fpak::kInstanceHidden)) {
                        ScatterBody b;
                        b.mesh = rows[i].mesh;
                        std::memcpy(b.world, rows[i].world, sizeof(b.world));
                        cell.bodies.push_back(b);
                    }
                SDL_UnmapGPUTransferBuffer(device_, it->buffer);
                collisionAdded_.push_back(std::move(cell));
            }
        gpumem::releaseTransferBuffer(device_, it->buffer);
        it = downloads_.erase(it);
    }
}

void ScatterRenderer::takeCollision(std::vector<ScatterCellBodies>& added, std::vector<uint64_t>& removed)
{
    added = std::move(collisionAdded_);
    removed = std::move(collisionRemoved_);
    collisionAdded_.clear();
    collisionRemoved_.clear();
}

}  // namespace fx
