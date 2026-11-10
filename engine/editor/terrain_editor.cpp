#include "editor/terrain_editor.h"

#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/gpu_driven_renderer.h"
#include "terrain/terrain_generate.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fx {

namespace {

constexpr const char* kOpNames[uint32_t(BrushOp::Count)] = {"raise", "lower", "smooth", "flatten", "set", "ramp",
                                                           "terrace", "noise", "sharpen", "stamp", "paint", "hole",
                                                           "scatter", "erode"};
constexpr float kDabsPerSecond = 20.0f;  // while the mouse holds still

uint64_t splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
float unit(uint64_t h) { return float(h >> 40) / float(1ull << 24); }  // [0, 1)

// Falloff presets, value at distance / radius = i / 8.
struct Preset {
    const char* name;
    float v[BrushSettings::kCurvePoints];
};
constexpr Preset kFalloffs[] = {
    {"smooth", {1.0f, 0.99f, 0.95f, 0.86f, 0.72f, 0.53f, 0.31f, 0.12f, 0.0f}},
    {"linear", {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f, 0.0f}},
    {"sphere", {1.0f, 0.99f, 0.97f, 0.93f, 0.87f, 0.78f, 0.66f, 0.48f, 0.0f}},
    {"constant", {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}},
    {"sharp", {1.0f, 0.6f, 0.36f, 0.22f, 0.13f, 0.08f, 0.04f, 0.02f, 0.0f}},
};

bool isImage(const std::filesystem::path& p)
{
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

}  // namespace

void TerrainEditor::init(const Init& init)
{
    terrain_ = init.terrain;
    renderer_ = init.renderer;
    pak_ = init.pak;
    settings_ = init.settings;
    if (pak_)
        for (uint32_t m = 0; m < pak_->cookedMeshCount(); ++m)
            meshNames_.push_back(pak_->meshName(m));
    scenePath_ = init.scenePath;
    loadPresets();
}

std::string TerrainEditor::sceneFolder() const
{
    const std::filesystem::path dir = std::filesystem::path(scenePath_).parent_path();
    return dir.empty() ? std::string(".") : dir.string();
}

// ---- brush shape

BrushOp TerrainEditor::effectiveOp() const
{
    if (smooth_)
        return BrushOp::Smooth;
    if (invert_ && brush_.op == BrushOp::Raise)
        return BrushOp::Lower;
    if (invert_ && brush_.op == BrushOp::Lower)
        return BrushOp::Raise;
    return brush_.op;
}

float TerrainEditor::falloff(float t) const
{
    const float f = std::clamp(t, 0.0f, 1.0f) * float(BrushSettings::kCurvePoints - 1);
    const uint32_t i = std::min(uint32_t(f), BrushSettings::kCurvePoints - 2);
    const float u = f - float(i);
    return std::clamp(brush_.falloff[i] + (brush_.falloff[i + 1] - brush_.falloff[i]) * u, 0.0f, 1.0f);
}

float TerrainEditor::alphaAt(float lx, float lz) const
{
    if (alpha_.empty())
        return 1.0f;
    const float u = (lx * 0.5f + 0.5f) * float(alphaW_) - 0.5f, v = (lz * 0.5f + 0.5f) * float(alphaH_) - 0.5f;
    if (u < -0.5f || v < -0.5f || u > float(alphaW_) - 0.5f || v > float(alphaH_) - 0.5f)
        return 0.0f;
    const int32_t x0 = std::clamp(int32_t(std::floor(u)), 0, int32_t(alphaW_) - 1), z0 = std::clamp(int32_t(std::floor(v)), 0, int32_t(alphaH_) - 1);
    const int32_t x1 = std::min(x0 + 1, int32_t(alphaW_) - 1), z1 = std::min(z0 + 1, int32_t(alphaH_) - 1);
    const float fu = std::clamp(u - float(x0), 0.0f, 1.0f), fv = std::clamp(v - float(z0), 0.0f, 1.0f);
    const float a = alpha_[z0 * alphaW_ + x0] * (1 - fu) + alpha_[z0 * alphaW_ + x1] * fu;
    const float b = alpha_[z1 * alphaW_ + x0] * (1 - fu) + alpha_[z1 * alphaW_ + x1] * fu;
    return a * (1 - fv) + b * fv;
}

bool TerrainEditor::loadAlpha(const std::string& name)
{
    if (name == alphaLoaded_)
        return !alpha_.empty() || name.empty();
    alpha_.clear();
    alphaW_ = alphaH_ = 0;
    alphaLoaded_ = name;
    if (name.empty())
        return true;
    const std::string path = sceneFolder() + "/brushes/" + name;
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 1);
    if (!px) {
        FX_WARN("terrain: cannot read brush image %s", path.c_str());
        return false;
    }
    alpha_.resize(size_t(w) * h);
    for (size_t i = 0; i < alpha_.size(); ++i)
        alpha_[i] = float(px[i]) / 255.0f;
    stbi_image_free(px);
    alphaW_ = uint32_t(w);
    alphaH_ = uint32_t(h);
    return true;
}

void TerrainEditor::scaleRadius(float factor)
{
    brush_.radius = std::clamp(brush_.radius * factor, 0.5f, 2000.0f);
    changed_ = true;
}

// ---- strokes

bool TerrainEditor::hit(float3 origin, float3 dir, float3& p) const
{
    float t;
    if (!valid() || !terrain_->raycast(origin, dir, 1e6f, t))
        return false;
    p = origin + dir * t;
    return true;
}

bool TerrainEditor::generating() const { return renderer_ && renderer_->terrainGenerator().busy(); }

void TerrainEditor::beginStroke(float3 p, bool invert, bool smooth)
{
    if (!valid() || generating())
        return;
    erodeDabs_.clear();
    stroking_ = true;
    invert_ = invert;
    smooth_ = smooth;
    start_ = last_ = p;
    start_.y = terrain_->heightAt(p.x, p.z);
    flattenTarget_ = start_.y;
    travelled_ = still_ = 0.0f;
    before_.clear();
    pending_.clear();
    loadAlpha(brush_.alpha);
    if (effectiveOp() != BrushOp::Ramp)
        addDab(p.x, p.z);
}

void TerrainEditor::strokeTo(float3 p)
{
    if (!stroking_)
        return;
    const BrushOp op = effectiveOp();
    if (op == BrushOp::Ramp || op == BrushOp::Stamp) {
        last_ = p;
        return;
    }
    // Dabs every spacing x radius along the path.
    const float step = std::max(brush_.spacing, 0.02f) * brush_.radius;
    const float dx = p.x - last_.x, dz = p.z - last_.z;
    const float length = std::sqrt(dx * dx + dz * dz);
    float along = step - travelled_;
    while (along <= length) {
        const float t = along / std::max(length, 1e-6f);
        addDab(last_.x + dx * t, last_.z + dz * t);
        along += step;
    }
    travelled_ = length - (along - step);
    if (length > 0.0f)
        still_ = 0.0f;
    last_ = p;
}

void TerrainEditor::addDab(float x, float z)
{
    // Jitter: deterministic per dab of the stroke.
    const uint64_t h = splitmix(uint64_t(dabIndex_++) * 0x632be59bd9b4e019ull + 17);
    const float a = unit(h) * 6.2831853f, r = std::sqrt(unit(splitmix(h)));
    const float s = 1.0f + (unit(splitmix(h + 1)) * 2.0f - 1.0f) * brush_.jitterScale;
    const float radius = std::max(brush_.radius * s, 0.05f);
    pending_.push_back({x + std::cos(a) * r * brush_.jitterPosition * brush_.radius,
                        z + std::sin(a) * r * brush_.jitterPosition * brush_.radius, radius,
                        brush_.rotation + (unit(splitmix(h + 2)) * 2.0f - 1.0f) * brush_.jitterRotation});
}

void TerrainEditor::tick(float dt)
{
    // Layer textures finishing: the panel shows it.
    if (const bool building = valid() && renderer_->terrainBuilding(); building != wasBuilding_) {
        wasBuilding_ = building;
        changed_ = true;
    }
    if (stroking_) {
        const BrushOp op = effectiveOp();
        if (op != BrushOp::Ramp && op != BrushOp::Stamp) {
            still_ += dt;
            while (still_ >= 1.0f / kDabsPerSecond) {
                still_ -= 1.0f / kDabsPerSecond;
                addDab(last_.x, last_.z);
            }
        }
    }
    if (!pending_.empty()) {
        applyDabs(pending_);
        pending_.clear();
    }
    clearMasks();
    // Generation: GPU work and finished chunks; its undo step when done.
    if (generating()) {
        TerrainGenerator& g = renderer_->terrainGenerator();
        const float before = g.progress();
        if (g.tick()) {
            finishedEdit_ = undoRun_ ? pushEdit() : ~0u;
            before_.clear();
            changed_ = true;
        } else if (int(g.progress() * 50.0f) != int(before * 50.0f)) {
            changed_ = true;  // the panel's progress bar
        }
    }
}

void TerrainEditor::clearMasks()
{
    if (!maskClear_ || !valid() || stroking_ || generating())
        return;
    const uint32_t tiles = terrain_->settings().tiles;
    const int32_t q = int32_t(terrain::kTileQuads);
    const int32_t tx = int32_t(clearTile_ % tiles), tz = int32_t(clearTile_ / tiles);
    const TerrainRect r = terrain_->clampRect({tx * q, tz * q, (tx + 1) * q, (tz + 1) * q});
    if (!clearPinned_) {
        if (!terrain_->pinned().empty())
            return;  // (someone else's region)
        terrain_->pin(r);
        clearPinned_ = true;
    }
    if (!terrain_->rectResident(r))
        return;
    terrain_->readMaterials(r, materials_);
    const uint32_t mask = maskClear_ << 25;  // (kScatterMask << bit)
    bool any = false;
    for (uint32_t& m : materials_)
        if (m & mask) {
            m &= ~mask;
            any = true;
        }
    if (any)
        terrain_->writeMaterials(r, materials_);
    terrain_->pin({0, 0, -1, -1});
    clearPinned_ = false;
    if (++clearTile_ >= tiles * tiles) {
        clearTile_ = 0;
        maskClear_ = 0;
    }
}

uint32_t TerrainEditor::pushEdit()
{
    if (before_.empty())
        return ~0u;
    Edit e;
    e.generation = generation_;
    const uint32_t T = terrain_->tilesPerSide();
    for (auto& [key, before] : before_) {
        TileState t = {key % T, key / T, std::move(before), {}};
        terrain_->copyTile(t.tx, t.tz, t.after);
        e.tiles.push_back(std::move(t));
    }
    before_.clear();
    edits_.push_back(std::move(e));
    return uint32_t(edits_.size() - 1);
}

void TerrainEditor::snapshot(TerrainRect r)
{
    // Every tile holding a sample of r (edge samples live in neighbours too), before its first change.
    const uint32_t T = terrain_->tilesPerSide();
    const uint32_t tx0 = r.x0 > 0 ? uint32_t(r.x0 - 1) / terrain::kTileQuads : 0;
    const uint32_t tz0 = r.z0 > 0 ? uint32_t(r.z0 - 1) / terrain::kTileQuads : 0;
    for (uint32_t tz = tz0; tz <= std::min(uint32_t(r.z1) / terrain::kTileQuads, T - 1); ++tz)
        for (uint32_t tx = tx0; tx <= std::min(uint32_t(r.x1) / terrain::kTileQuads, T - 1); ++tx) {
            const uint32_t key = tz * T + tx;
            if (before_.count(key))
                continue;
            TerrainTileData copy;
            if (terrain_->copyTile(tx, tz, copy))
                before_.emplace(key, std::move(copy));
        }
}

void TerrainEditor::applyDabs(const std::vector<Dab>& dabs)
{
    FX_ZONE_CAT("TerrainEditor::applyDabs", Editor);
    if (!valid() || dabs.empty())
        return;
    const TerrainSettings& s = terrain_->settings();
    const float margin = 3.0f * s.spacing;  // the blur's reach
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (const Dab& d : dabs) {
        lo[0] = std::min(lo[0], d.x - d.radius - margin);
        lo[1] = std::min(lo[1], d.z - d.radius - margin);
        hi[0] = std::max(hi[0], d.x + d.radius + margin);
        hi[1] = std::max(hi[1], d.z + d.radius + margin);
    }
    const TerrainRect r = terrain_->worldRect(lo[0], lo[1], hi[0], hi[1]);
    if (r.empty() || !terrain_->rectResident(r))
        return;
    snapshot(r);
    const BrushOp op = effectiveOp();
    if (op == BrushOp::Erode) {  // erodes when the stroke ends
        erodeDabs_.insert(erodeDabs_.end(), dabs.begin(), dabs.end());
        return;
    }
    if (op == BrushOp::Paint || op == BrushOp::Hole || op == BrushOp::Scatter)
        return applyPaint(dabs, r);
    terrain_->read(r, heights_);
    const int32_t w = r.x1 - r.x0 + 1, d = r.z1 - r.z0 + 1;
    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f);
    for (const Dab& dab : dabs) {
        const float cr = std::cos(dab.rotation * (kPi / 180.0f)), sr = std::sin(dab.rotation * (kPi / 180.0f));
        const int32_t x0 = std::max(int32_t(std::floor((dab.x - dab.radius - s.originX) / s.spacing)) - r.x0, 0);
        const int32_t x1 = std::min(int32_t(std::ceil((dab.x + dab.radius - s.originX) / s.spacing)) - r.x0, w - 1);
        const int32_t z0 = std::max(int32_t(std::floor((dab.z - dab.radius - s.originZ) / s.spacing)) - r.z0, 0);
        const int32_t z1 = std::min(int32_t(std::ceil((dab.z + dab.radius - s.originZ) / s.spacing)) - r.z0, d - 1);
        if (x0 > x1 || z0 > z1)
            continue;
        // Ops reading neighbours (smooth, sharpen) read this dab's input, not their own output.
        if (op == BrushOp::Smooth || op == BrushOp::Sharpen)
            scratch_ = heights_;
        auto blurred = [&](int32_t x, int32_t z) {  // 5 x 5 binomial
            static constexpr float k[5] = {1, 4, 6, 4, 1};
            float sum = 0.0f;
            for (int32_t j = -2; j <= 2; ++j)
                for (int32_t i = -2; i <= 2; ++i)
                    sum += k[i + 2] * k[j + 2] *
                           scratch_[size_t(std::clamp(z + j, 0, d - 1)) * w + std::clamp(x + i, 0, w - 1)];
            return sum / 256.0f;
        };
        for (int32_t z = z0; z <= z1; ++z)
            for (int32_t x = x0; x <= x1; ++x) {
                const float wx = s.originX + float(r.x0 + x) * s.spacing - dab.x;
                const float wz = s.originZ + float(r.z0 + z) * s.spacing - dab.z;
                const float dist = std::sqrt(wx * wx + wz * wz) / dab.radius;
                if (dist >= 1.0f)
                    continue;
                // Brush-local coordinates for the alpha image (rotated with the dab).
                const float lx = (wx * cr + wz * sr) / dab.radius, lz = (-wx * sr + wz * cr) / dab.radius;
                const float f = falloff(dist) * alphaAt(lx, lz);
                if (f <= 0.0f)
                    continue;
                float& h = heights_[size_t(z) * w + x];
                switch (op) {
                case BrushOp::Raise: h += strength * strength * dab.radius * 0.08f * f; break;
                case BrushOp::Lower: h -= strength * strength * dab.radius * 0.08f * f; break;
                case BrushOp::Smooth: h += (blurred(x, z) - h) * f * (0.25f + 0.75f * strength); break;
                case BrushOp::Sharpen: h += (h - blurred(x, z)) * f * strength * 0.5f; break;
                case BrushOp::Flatten: h += (flattenTarget_ - h) * f * strength; break;
                case BrushOp::SetHeight: h += (brush_.height - h) * f * strength; break;
                case BrushOp::Terrace: {
                    const float step = std::max(brush_.terraceStep, 0.05f);
                    const float base = std::floor(h / step) * step, t = (h - base) / step;
                    const float k = std::clamp(brush_.terraceSharpness, 0.0f, 0.98f);
                    const float u = std::clamp((t - k) / (1.0f - k), 0.0f, 1.0f);
                    const float target = base + step * u * u * (3.0f - 2.0f * u);
                    h += (target - h) * f * strength * 0.5f;
                    break;
                }
                case BrushOp::Noise: {
                    const float px = s.originX + float(r.x0 + x) * s.spacing, pz = s.originZ + float(r.z0 + z) * s.spacing;
                    const float n = terrainFbm(px / std::max(brush_.noiseScale, 0.1f), pz / std::max(brush_.noiseScale, 0.1f), 4, 7);
                    h += n * brush_.noiseAmplitude * strength * f * (invert_ ? -1.0f : 1.0f);
                    break;
                }
                case BrushOp::Stamp: h += brush_.stampHeight * strength * f * (invert_ ? -1.0f : 1.0f); break;
                default: break;
                }
            }
    }
    terrain_->write(r, heights_);
}

// How much a sample takes paint, from the brush's filters: slope and height ranges (soft edges),
// and a noise mask that breaks a stroke's edge up the way nature would.
float TerrainEditor::filterAt(int32_t gx, int32_t gz) const
{
    const TerrainSettings& s = terrain_->settings();
    const BrushSettings& b = brush_;
    float mask = 1.0f;
    const float h = terrain_->toWorld(terrain_->sample(gx, gz));
    if (b.filterHeight[0] > -1e4f || b.filterHeight[1] < 1e4f)
        mask *= std::clamp((h - b.filterHeight[0]) / 0.5f + 0.5f, 0.0f, 1.0f) * std::clamp((b.filterHeight[1] - h) / 0.5f + 0.5f, 0.0f, 1.0f);
    if (b.filterSlope[0] > 0.0f || b.filterSlope[1] < 90.0f) {
        const float dx = terrain_->toWorld(terrain_->sample(gx + 1, gz)) - terrain_->toWorld(terrain_->sample(gx - 1, gz));
        const float dz = terrain_->toWorld(terrain_->sample(gx, gz + 1)) - terrain_->toWorld(terrain_->sample(gx, gz - 1));
        const float slope = std::atan(std::sqrt(dx * dx + dz * dz) / (2.0f * s.spacing)) * (180.0f / kPi);
        mask *= std::clamp((slope - b.filterSlope[0]) / 4.0f + 0.5f, 0.0f, 1.0f) * std::clamp((b.filterSlope[1] - slope) / 4.0f + 0.5f, 0.0f, 1.0f);
    }
    if (b.filterNoise > 0.0f) {
        const float x = s.originX + float(gx) * s.spacing, z = s.originZ + float(gz) * s.spacing;
        const float n = terrainFbm(x / std::max(b.filterNoiseScale, 0.1f), z / std::max(b.filterNoiseScale, 0.1f), 4, 31) * 0.5f + 0.5f;
        const float t = std::clamp((n - 0.35f) / 0.3f, 0.0f, 1.0f);
        mask *= 1.0f + (t * t * (3.0f - 2.0f * t) - 1.0f) * b.filterNoise;
    }
    return mask;
}

// Paint: moves each sample's pair of layers toward the brush's layer (the weakest of the three
// dropped). Hole: cuts cells out (Shift fills them). Scatter: marks samples in a scatter layer's
// mask (Shift clears them).
void TerrainEditor::applyPaint(const std::vector<Dab>& dabs, TerrainRect r)
{
    const TerrainSettings& s = terrain_->settings();
    terrain_->readMaterials(r, materials_);
    const int32_t w = r.x1 - r.x0 + 1, d = r.z1 - r.z0 + 1;
    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f);
    const bool hole = brush_.op == BrushOp::Hole, scatter = brush_.op == BrushOp::Scatter;
    int32_t paintBit = -1;
    if (scatter) {
        const std::vector<ScatterLayer> layers = parseScatterLayers(terrain_->scatterJson());
        paintBit = brush_.scatterLayer < layers.size() ? layers[brush_.scatterLayer].paintBit : -1;
        if (paintBit < 0)
            return;
    }
    const uint32_t scatterBit = terrain::kScatterMask << std::max(paintBit, 0);
    const uint32_t layer = std::min(brush_.layer, terrain::kMaxLayers - 1);
    for (const Dab& dab : dabs) {
        const float cr = std::cos(dab.rotation * (kPi / 180.0f)), sr = std::sin(dab.rotation * (kPi / 180.0f));
        const int32_t x0 = std::max(int32_t(std::floor((dab.x - dab.radius - s.originX) / s.spacing)) - r.x0, 0);
        const int32_t x1 = std::min(int32_t(std::ceil((dab.x + dab.radius - s.originX) / s.spacing)) - r.x0, w - 1);
        const int32_t z0 = std::max(int32_t(std::floor((dab.z - dab.radius - s.originZ) / s.spacing)) - r.z0, 0);
        const int32_t z1 = std::min(int32_t(std::ceil((dab.z + dab.radius - s.originZ) / s.spacing)) - r.z0, d - 1);
        for (int32_t z = z0; z <= z1; ++z)
            for (int32_t x = x0; x <= x1; ++x) {
                const float wx = s.originX + float(r.x0 + x) * s.spacing - dab.x;
                const float wz = s.originZ + float(r.z0 + z) * s.spacing - dab.z;
                const float dist = std::sqrt(wx * wx + wz * wz) / dab.radius;
                if (dist >= 1.0f)
                    continue;
                const float lx = (wx * cr + wz * sr) / dab.radius, lz = (-wx * sr + wz * cr) / dab.radius;
                const float f = falloff(dist) * alphaAt(lx, lz) * filterAt(r.x0 + x, r.z0 + z);
                if (f <= 0.0f)
                    continue;
                uint32_t& m = materials_[size_t(z) * w + x];
                if (hole || scatter) {
                    const uint32_t bit = hole ? terrain::kHole : scatterBit;
                    if (f > 0.5f)
                        m = invert_ ? (m & ~bit) : (m | bit);
                    continue;
                }
                const float t = std::min(f * strength, 1.0f);
                uint32_t ids[3] = {terrain::materialA(m), terrain::materialB(m), layer};
                const float wb = ids[0] == ids[1] ? 0.0f : float(terrain::materialWeightB(m)) / 255.0f;
                float ws[3] = {(1.0f - wb) * (1.0f - t), wb * (1.0f - t), t};
                for (int i = 0; i < 2; ++i)  // the brush's layer already in the pair: add to it
                    if (ids[i] == layer) {
                        ws[i] += ws[2];
                        ws[2] = 0.0f;
                    }
                // Keep the two strongest.
                int drop = ws[0] <= ws[1] && ws[0] <= ws[2] ? 0 : (ws[1] <= ws[2] ? 1 : 2);
                int a = drop == 0 ? 1 : 0, b = drop == 2 ? 1 : 2;
                if (ws[a] < ws[b])
                    std::swap(a, b);
                const float sum = std::max(ws[a] + ws[b], 1e-6f);
                const uint32_t weightB = ids[a] == ids[b] ? 0u : uint32_t(std::lround(ws[b] / sum * 255.0f));
                m = terrain::packMaterial(ids[a], ids[b], weightB, m & ~0xffffffu);  // (hole and scatter bits kept)
            }
    }
    terrain_->writeMaterials(r, materials_, hole ? (terrain::kChangeMaterials | terrain::kChangeHoles) : terrain::kChangeMaterials);
}

void TerrainEditor::applyRamp()
{
    const TerrainSettings& s = terrain_->settings();
    const float3 a = start_;
    float3 b = last_;
    b.y = terrain_->heightAt(b.x, b.z);
    const float R = brush_.radius;
    const TerrainRect r = terrain_->worldRect(std::min(a.x, b.x) - R, std::min(a.z, b.z) - R, std::max(a.x, b.x) + R,
                                              std::max(a.z, b.z) + R);
    if (r.empty() || !terrain_->rectResident(r))
        return;
    snapshot(r);
    terrain_->read(r, heights_);
    const int32_t w = r.x1 - r.x0 + 1, d = r.z1 - r.z0 + 1;
    const float ex = b.x - a.x, ez = b.z - a.z, len2 = std::max(ex * ex + ez * ez, 1e-6f);
    const float strength = std::clamp(brush_.strength, 0.0f, 1.0f);
    for (int32_t z = 0; z < d; ++z)
        for (int32_t x = 0; x < w; ++x) {
            const float px = s.originX + float(r.x0 + x) * s.spacing, pz = s.originZ + float(r.z0 + z) * s.spacing;
            const float t = std::clamp(((px - a.x) * ex + (pz - a.z) * ez) / len2, 0.0f, 1.0f);
            const float qx = a.x + ex * t - px, qz = a.z + ez * t - pz;
            const float dist = std::sqrt(qx * qx + qz * qz) / R;
            if (dist >= 1.0f)
                continue;
            float& h = heights_[size_t(z) * w + x];
            h += (a.y + (b.y - a.y) * t - h) * falloff(dist) * strength;
        }
    terrain_->write(r, heights_);
}

uint32_t TerrainEditor::endStroke()
{
    if (!stroking_)
        return ~0u;
    tick(0.0f);  // the last dabs
    if (effectiveOp() == BrushOp::Ramp)
        applyRamp();
    const bool erode = effectiveOp() == BrushOp::Erode;
    stroking_ = invert_ = smooth_ = false;
    changed_ = true;
    if (erode) {
        startErosionBrush();  // its undo step comes when it finishes
        return ~0u;
    }
    return pushEdit();
}

void TerrainEditor::stopGenerating()
{
    if (!generating())
        return;
    renderer_->terrainGenerator().cancel();
    finishedEdit_ = undoRun_ ? pushEdit() : ~0u;  // what it wrote so far
    changed_ = true;
}

void TerrainEditor::undo(uint32_t id)
{
    if (id >= edits_.size() || edits_[id].generation != generation_ || !valid())
        return;
    for (const TileState& t : edits_[id].tiles)
        terrain_->restoreTile(t.tx, t.tz, t.before);
}

void TerrainEditor::redo(uint32_t id)
{
    if (id >= edits_.size() || edits_[id].generation != generation_ || !valid())
        return;
    for (const TileState& t : edits_[id].tiles)
        terrain_->restoreTile(t.tx, t.tz, t.after);
}

// ---- page

void TerrainEditor::setPreset(const std::string& name)
{
    for (const Preset& p : kFalloffs)
        if (name == p.name) {
            std::memcpy(brush_.falloff, p.v, sizeof(p.v));
            brush_.falloffPreset = name;
            return;
        }
}

void TerrainEditor::readBrush(const Json& m)
{
    const std::string op = m.getString("op");
    for (uint32_t i = 0; i < uint32_t(BrushOp::Count); ++i)
        if (op == kOpNames[i])
            brush_.op = BrushOp(i);
    auto num = [&](const char* key, float& v, float lo, float hi) {
        if (const Json* j = m.get(key); j && j->type == Json::Type::Number)
            v = std::clamp(float(j->number), lo, hi);
    };
    num("radius", brush_.radius, 0.5f, 2000.0f);
    num("strength", brush_.strength, 0.0f, 1.0f);
    num("spacing", brush_.spacing, 0.02f, 2.0f);
    num("rotation", brush_.rotation, -360.0f, 360.0f);
    num("jitterPosition", brush_.jitterPosition, 0.0f, 2.0f);
    num("jitterRotation", brush_.jitterRotation, 0.0f, 180.0f);
    num("jitterScale", brush_.jitterScale, 0.0f, 0.95f);
    num("height", brush_.height, -1e5f, 1e5f);
    num("terraceStep", brush_.terraceStep, 0.05f, 1000.0f);
    num("terraceSharpness", brush_.terraceSharpness, 0.0f, 1.0f);
    num("noiseScale", brush_.noiseScale, 0.1f, 10000.0f);
    num("noiseAmplitude", brush_.noiseAmplitude, 0.0f, 1000.0f);
    num("stampHeight", brush_.stampHeight, 0.0f, 10000.0f);
    if (const Json* l = m.get("layer"); l && l->type == Json::Type::Number)
        brush_.layer = uint32_t(std::clamp(l->number, 0.0, double(terrain::kMaxLayers - 1)));
    if (const Json* l = m.get("scatterLayer"); l && l->type == Json::Type::Number)
        brush_.scatterLayer = uint32_t(std::clamp(l->number, 0.0, 255.0));
    num("slopeMin", brush_.filterSlope[0], 0.0f, 90.0f);
    num("slopeMax", brush_.filterSlope[1], 0.0f, 90.0f);
    num("heightMin", brush_.filterHeight[0], -1e5f, 1e5f);
    num("heightMax", brush_.filterHeight[1], -1e5f, 1e5f);
    num("noiseMask", brush_.filterNoise, 0.0f, 1.0f);
    num("noiseMaskScale", brush_.filterNoiseScale, 0.1f, 1000.0f);
    if (const Json* f = m.get("falloff"); f && f->array.size() == BrushSettings::kCurvePoints) {
        for (uint32_t i = 0; i < BrushSettings::kCurvePoints; ++i)
            brush_.falloff[i] = std::clamp(float(f->array[i].number), 0.0f, 1.0f);
        brush_.falloffPreset = "custom";
    }
    if (const Json* p = m.get("falloffPreset"); p && p->type == Json::Type::String)
        setPreset(p->str);
    if (const Json* a = m.get("alpha"); a && a->type == Json::Type::String)
        brush_.alpha = a->str;
}

void TerrainEditor::writeBrush(JsonWriter& w, const BrushSettings& b) const
{
    w.value("op", kOpNames[uint32_t(b.op)])
        .value("radius", double(b.radius))
        .value("strength", double(b.strength))
        .value("spacing", double(b.spacing))
        .value("rotation", double(b.rotation))
        .value("jitterPosition", double(b.jitterPosition))
        .value("jitterRotation", double(b.jitterRotation))
        .value("jitterScale", double(b.jitterScale))
        .value("falloffPreset", b.falloffPreset)
        .value("alpha", b.alpha)
        .value("height", double(b.height))
        .value("terraceStep", double(b.terraceStep))
        .value("terraceSharpness", double(b.terraceSharpness))
        .value("noiseScale", double(b.noiseScale))
        .value("noiseAmplitude", double(b.noiseAmplitude))
        .value("stampHeight", double(b.stampHeight))
        .value("layer", b.layer)
        .value("scatterLayer", b.scatterLayer)
        .value("slopeMin", double(b.filterSlope[0]))
        .value("slopeMax", double(b.filterSlope[1]))
        .value("heightMin", double(b.filterHeight[0]))
        .value("heightMax", double(b.filterHeight[1]))
        .value("noiseMask", double(b.filterNoise))
        .value("noiseMaskScale", double(b.filterNoiseScale));
    w.beginArray("falloff");
    for (float v : b.falloff)
        w.value(nullptr, double(v));
    w.endArray();
}

void TerrainEditor::loadPresets()
{
    presets_.clear();
    size_t size = 0;
    void* text = SDL_LoadFile((sceneFolder() + "/brushes.json").c_str(), &size);
    if (!text)
        return;
    try {
        const Json root = Json::parse(std::string_view(static_cast<const char*>(text), size));
        if (const Json* list = root.get("presets"))
            for (const Json& p : list->array) {
                const BrushSettings keep = brush_;
                brush_ = BrushSettings{};
                readBrush(p);
                presets_.emplace_back(p.getString("name", "Brush"), brush_);
                brush_ = keep;
            }
    } catch (const std::exception& e) {
        FX_WARN("terrain: %s/brushes.json: %s (ignored)", sceneFolder().c_str(), e.what());
    }
    SDL_free(text);
}

bool TerrainEditor::savePresets() const
{
    JsonWriter w;
    w.beginObject().beginArray("presets");
    for (const auto& [name, b] : presets_) {
        w.beginObject().value("name", name);
        writeBrush(w, b);
        w.endObject();
    }
    w.endArray().endObject();
    const std::string path = sceneFolder() + "/brushes.json";
    std::ofstream out(path + ".tmp", std::ios::binary | std::ios::trunc);
    out << w.str() << '\n';
    out.close();
    if (!out)
        return false;
    std::error_code ec;
    std::filesystem::rename(path + ".tmp", path, ec);
    return !ec;
}

void TerrainEditor::readLayer(const Json& m, TerrainLayer& l) const
{
    auto num = [&](const char* key, float& v, float lo, float hi) {
        if (const Json* j = m.get(key); j && j->type == Json::Type::Number)
            v = std::clamp(float(j->number), lo, hi);
    };
    auto str = [&](const char* key, std::string& v) {
        if (const Json* j = m.get(key); j && j->type == Json::Type::String)
            v = j->str;
    };
    str("name", l.name);
    str("generated", l.generated);
    str("albedo", l.albedo);
    str("normal", l.normal);
    str("roughnessMap", l.roughnessMap);
    str("ao", l.ao);
    str("height", l.height);
    str("acoustic", l.acoustic);  // (an acoustic preset; "" from its look)
    str("surface", l.surface);    // (a surface sound preset; "" as its acoustics)
    if (const Json* j = m.get("seed"); j && j->type == Json::Type::Number)
        l.seed = uint32_t(std::max(j->number, 0.0));
    num("tileSize", l.tileSize, 0.05f, 1000.0f);
    if (const Json* j = m.get("hex"); j && j->type == Json::Type::Bool)
        l.hex = j->b;
    num("rotation", l.rotation, 0.0f, 1.0f);
    num("contrast", l.contrast, 0.0f, 1.0f);
    num("heightBlend", l.heightBlend, 0.01f, 1.0f);
    num("macro", l.macro, 0.0f, 1.0f);
    num("macroScale", l.macroScale, 1.0f, 10000.0f);
    if (const Json* j = m.get("projection"); j && j->type == Json::Type::Number)
        l.projection = uint32_t(std::clamp(j->number, 0.0, 2.0));
    num("tintR", l.tint[0], 0.0f, 4.0f);
    num("tintG", l.tint[1], 0.0f, 4.0f);
    num("tintB", l.tint[2], 0.0f, 4.0f);
    num("roughness", l.roughness, 0.0f, 2.0f);
    num("normalStrength", l.normalStrength, 0.0f, 4.0f);
    num("farScale", l.farScale, 1.0f, 32.0f);
}

void TerrainEditor::writeLayer(JsonWriter& w, const TerrainLayer& l) const
{
    w.beginObject()
        .value("name", l.name)
        .value("acoustic", l.acoustic)
        .value("surface", l.surface)
        .value("generated", l.generated)
        .value("seed", l.seed)
        .value("albedo", l.albedo)
        .value("normal", l.normal)
        .value("roughnessMap", l.roughnessMap)
        .value("ao", l.ao)
        .value("height", l.height)
        .value("tileSize", double(l.tileSize))
        .value("hex", l.hex)
        .value("rotation", double(l.rotation))
        .value("contrast", double(l.contrast))
        .value("heightBlend", double(l.heightBlend))
        .value("macro", double(l.macro))
        .value("macroScale", double(l.macroScale))
        .value("projection", l.projection)
        .value("tintR", double(l.tint[0]))
        .value("tintG", double(l.tint[1]))
        .value("tintB", double(l.tint[2]))
        .value("roughness", double(l.roughness))
        .value("normalStrength", double(l.normalStrength))
        .value("farScale", double(l.farScale))
        .endObject();
}

// Pictures under the scene folder (two levels deep, the first 400), looked up again at most
// every two seconds.
const std::vector<std::string>& TerrainEditor::images()
{
    const uint64_t now = SDL_GetTicksNS();
    if (imagesNs_ && now - imagesNs_ < 2'000'000'000ull)
        return images_;
    imagesNs_ = now;
    images_.clear();
    std::error_code ec;
    const std::filesystem::path root = sceneFolder();
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && images_.size() < 400; it.increment(ec)) {
        if (it.depth() > 2) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isImage(it->path()))
            images_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
    }
    std::sort(images_.begin(), images_.end());
    return images_;
}
void TerrainEditor::handle(const std::string& type, const Json& m, std::string& notice, bool& error)
{
    error = false;
    if (type == "editor.terrain.create") {
        if (stroking_)
            endStroke();
        renderer_->terrainGenerator().cancel();  // (its undo step belongs to the old terrain)
        TerrainSettings s;
        s.tiles = uint32_t(std::clamp(m.getNumber("tiles", 4), 1.0, double(terrain::kMaxTiles)));
        s.spacing = float(std::clamp(m.getNumber("spacing", 0.5), 0.1, 8.0));
        s.heightMin = float(m.getNumber("heightMin", -200.0));
        s.heightMax = float(m.getNumber("heightMax", 800.0));
        const float size = float(std::bit_floor(s.tiles) * terrain::kTileQuads) * s.spacing;
        s.originX = s.originZ = -0.5f * size;
        terrain_->create(s, float(m.getNumber("height", 0.0)), float(m.getNumber("hills", 0.0)), uint32_t(m.getNumber("seed", 1)));
        ++generation_;
        deleted_ = false;
        renderer_->setTerrain(terrain_, scenePath_);
        char text[160];
        SDL_snprintf(text, sizeof(text), "Terrain created: %.0f x %.0f m, %.2f m samples (save to keep it)", double(size),
                     double(size), double(s.spacing));
        notice = text;
    } else if (type == "editor.terrain.delete") {
        if (!valid())
            return;
        if (stroking_)
            endStroke();
        renderer_->terrainGenerator().cancel();
        terrain_->unload();
        renderer_->setTerrain(nullptr, scenePath_);
        ++generation_;
        deleted_ = true;
        notice = "Terrain deleted (saving moves its folder aside)";
    } else if (type == "editor.terrain.brush") {
        readBrush(m);
        if (!loadAlpha(brush_.alpha)) {
            notice = "Cannot read brush image " + brush_.alpha;
            error = true;
        }
    } else if (type == "editor.terrain.preset.save") {
        const std::string name = m.getString("name");
        if (name.empty())
            return;
        auto it = std::find_if(presets_.begin(), presets_.end(), [&](const auto& p) { return p.first == name; });
        if (it != presets_.end())
            it->second = brush_;
        else
            presets_.emplace_back(name, brush_);
        if (!savePresets()) {
            notice = "Could not write brushes.json";
            error = true;
        } else {
            notice = "Brush preset saved: " + name;
        }
    } else if (type == "editor.terrain.preset.load") {
        const std::string name = m.getString("name");
        for (const auto& [n, b] : presets_)
            if (n == name) {
                brush_ = b;
                loadAlpha(brush_.alpha);
            }
    } else if (type == "editor.terrain.preset.delete") {
        const std::string name = m.getString("name");
        presets_.erase(std::remove_if(presets_.begin(), presets_.end(), [&](const auto& p) { return p.first == name; }),
                       presets_.end());
        savePresets();
    } else if (type == "editor.terrain.layer.add" || type == "editor.terrain.layer.set" || type == "editor.terrain.layer.remove") {
        if (!valid())
            return;
        std::vector<TerrainLayer> layers = parseTerrainLayers(terrain_->layersJson());
        if (type == "editor.terrain.layer.add") {
            if (layers.size() >= terrain::kMaxLayers) {
                notice = "A terrain has at most 32 layers";
                error = true;
                return;
            }
            TerrainLayer l;
            l.generated = m.getString("generated", m.get("albedo") ? "" : "grass");
            readLayer(m, l);
            if (!m.get("name"))  // named after its look or its colour image
                l.name = !l.generated.empty() ? std::string(1, char(std::toupper(uint8_t(l.generated[0])))) + l.generated.substr(1)
                                              : std::filesystem::path(l.albedo).stem().string();
            layers.push_back(l);
            brush_.layer = uint32_t(layers.size() - 1);
            notice = "Layer added: " + l.name + (layers.size() == 1 ? " (the whole terrain shows it until you paint others)" : "");
        } else if (type == "editor.terrain.layer.set") {
            const uint32_t i = uint32_t(m.getNumber("index", -1));
            if (i >= layers.size())
                return;
            readLayer(m, layers[i]);
        } else if (!layers.empty()) {  // only the last: painted samples keep their layer numbers
            layers.pop_back();
            brush_.layer = std::min(brush_.layer, uint32_t(std::max<size_t>(layers.size(), 1) - 1));
        }
        terrain_->setLayersJson(terrainLayersJson(layers));
        changed_ = true;
    } else if (type.starts_with("editor.terrain.scatter.")) {
        if (!valid())
            return;
        std::vector<ScatterLayer> layers = parseScatterLayers(terrain_->scatterJson());
        const uint32_t i = uint32_t(m.getNumber("index", -1));
        const size_t pools = renderer_->scatter().pools().size();
        if (type == "editor.terrain.scatter.add") {
            ScatterLayer l;
            ScatterAsset a;
            a.mesh = m.getString("mesh", meshNames_.empty() ? "" : meshNames_[0].c_str());
            l.assets.push_back(a);
            l.name = a.mesh;
            l.seed = uint32_t(layers.size() + 1);
            if (const char* preset = scatterPreset(m.getString("preset")))  // a built-in layer (rocks)
                readScatterLayer(Json::parse(preset), l);
            uint32_t used = maskClear_;
            for (const ScatterLayer& o : layers)
                if (o.paintBit >= 0)
                    used |= 1u << o.paintBit;
            l.paintBit = -1;
            for (int32_t b = 0; b < int32_t(ScatterLayer::kPaintable) && l.paintBit < 0; ++b)
                if (!(used & (1u << b)))
                    l.paintBit = b;
            l.paint = l.paintBit >= 0 ? 2u : 0u;  // painted by hand; without a bit, generated
            readScatterLayer(m, l);
            layers.push_back(l);
            brush_.scatterLayer = uint32_t(layers.size() - 1);
            if (l.paintBit >= 0 && l.paint != 0)
                brush_.op = BrushOp::Scatter;
            notice = "Scatter layer added: " + l.name +
                     (l.paintBit < 0 ? " (generated: the 7 paintable layers are taken)"
                      : l.paint == 2 ? ": paint it onto the terrain (Shift erases), or set Placement to Generated" : "");
        } else if (type == "editor.terrain.scatter.set") {
            if (i >= layers.size())
                return;
            readScatterLayer(m, layers[i]);  // (meshes added or changed get slots at once, from the spare ones)
        } else if (type == "editor.terrain.scatter.remove") {
            if (i >= layers.size())
                return;
            if (layers[i].paintBit >= 0)
                maskClear_ |= 1u << layers[i].paintBit;  // (cleared over the terrain before the bit is used again)
            layers.erase(layers.begin() + i);
            if (brush_.scatterLayer > i)
                --brush_.scatterLayer;
            picked_.valid = false;
            (void)pools;
        } else if (type == "editor.terrain.scatter.restore") {
            if (i >= layers.size())
                return;
            layers[i].exclusions.clear();
        } else if (type == "editor.terrain.scatter.exclude") {
            if (!picked_.valid || picked_.layer >= layers.size())
                return;
            layers[picked_.layer].exclusions.push_back(
                {picked_.cellX, picked_.cellZ, int32_t(picked_.asset), int32_t(picked_.candidate)});
            notice = "Scatter instance deleted (Restore on its layer brings deleted ones back)";
            picked_.valid = false;
        } else {
            return;
        }
        terrain_->setScatterJson(scatterLayersJson(layers));
        addScatterLooks(*terrain_);  // (a preset's terrain look added to the material layers)
        changed_ = true;
    } else if (type.starts_with("editor.terrain.generate.")) {
        if (!valid())
            return;
        TerrainGenStack stack = parseGenStack(terrain_->generatorJson());
        const uint32_t i = uint32_t(m.getNumber("index", -1));
        if (type == "editor.terrain.generate.preset") {
            const char* json = terrainGeneratorPreset(m.getString("name"));
            if (!json)
                return;
            const GenRegion region = stack.region;  // the preset's steps over the chosen region
            stack = parseGenStack(json);
            stack.region = region;
            notice = "Generator: " + m.getString("name") + " (Generate to apply it)";
        } else if (type == "editor.terrain.generate.region") {
            readGenRegion(m, stack.region);
            if (m.getBool("atView")) {  // where the camera looks at the ground (or below it)
                float3 p;
                if (!hit(viewPosition_, viewForward_, p))
                    p = viewPosition_;
                stack.region.all = false;
                stack.region.x = p.x;
                stack.region.z = p.z;
            }
        } else if (type == "editor.terrain.generate.step.add") {
            GenStep s;
            readGenStep(m, s);
            s.seed = uint32_t(stack.steps.size() + 1);
            if (s.op == GenOp::Paint)
                s.layer = std::min(brush_.layer, terrain::kMaxLayers - 1);
            stack.steps.push_back(s);
        } else if (type == "editor.terrain.generate.step.set") {
            if (i >= stack.steps.size())
                return;
            readGenStep(m, stack.steps[i]);
        } else if (type == "editor.terrain.generate.step.remove") {
            if (i >= stack.steps.size())
                return;
            stack.steps.erase(stack.steps.begin() + i);
        } else if (type == "editor.terrain.generate.step.move") {
            const uint32_t to = uint32_t(m.getNumber("to", -1));
            if (i >= stack.steps.size() || to >= stack.steps.size())
                return;
            const GenStep s = stack.steps[i];
            stack.steps.erase(stack.steps.begin() + i);
            stack.steps.insert(stack.steps.begin() + to, s);
        } else if (type == "editor.terrain.generate.run") {
            if (stroking_ || generating())
                return;
            resolveGenLayers(stack, *terrain_);
            // Large runs are not undoable (their snapshots would not fit in memory).
            const GenRegion& g = stack.region;
            const int32_t side = int32_t(terrain_->samplesPerSide());
            const TerrainRect r = g.all ? terrain_->clampRect({0, 0, side, side})
                                        : terrain_->worldRect(g.x - g.size * 0.5f, g.z - g.size * 0.5f, g.x + g.size * 0.5f, g.z + g.size * 0.5f);
            const size_t tiles = r.empty() ? 0
                                           : size_t(r.x1 - r.x0 + int32_t(terrain::kTileQuads)) / terrain::kTileQuads *
                                                 (size_t(r.z1 - r.z0 + int32_t(terrain::kTileQuads)) / terrain::kTileQuads);
            undoRun_ = tiles <= kUndoTiles;
            before_.clear();
            if (!renderer_->terrainGenerator().start(*terrain_, stack, [this](TerrainRect c) {
                    if (undoRun_)
                        snapshot(c);
                })) {
                notice = "Nothing to generate";
                error = true;
                return;
            }
            if (!undoRun_)
                notice = "Generating (too large to undo: save first to keep a way back)";
        } else if (type == "editor.terrain.generate.cancel") {
            if (generating()) {
                stopGenerating();
                notice = "Generation stopped (finished chunks stay)";
            }
            return;
        } else {
            return;
        }
        terrain_->setGeneratorJson(genStackJson(stack));
        changed_ = true;
    } else if (type == "editor.terrain.import") {
        if (!valid() || stroking_ || generating())
            return;
        importHeights(m, notice, error);
    } else if (type == "editor.terrain.settings") {
        if (const Json* p = m.get("pixels"); p && settings_)
            settings_->terrainPixels = std::clamp(float(p->number), 0.25f, 16.0f);
    }
}

void TerrainEditor::importHeights(const Json& m, std::string& notice, bool& error)
{
    const std::string name = m.getString("image");
    const std::filesystem::path path = std::filesystem::path(name).is_absolute() ? std::filesystem::path(name)
                                                                                 : std::filesystem::path(sceneFolder()) / name;
    int w = 0, h = 0, n = 0;
    const bool deep = stbi_is_16_bit(path.string().c_str()) != 0;
    stbi_us* px = stbi_load_16(path.string().c_str(), &w, &h, &n, 1);  // (8-bit images scaled to 16)
    if (!px || w < 2 || h < 2) {
        if (px)
            stbi_image_free(px);
        notice = "Cannot read heightmap " + name;
        error = true;
        return;
    }
    const int32_t side = int32_t(terrain_->samplesPerSide());
    const TerrainRect all = terrain_->clampRect({0, 0, side - 1, side - 1});
    if (!terrain_->rectResident(all)) {
        terrain_->pin(all);  // (kept until an import goes through)
        stbi_image_free(px);
        notice = "Loading the terrain's tiles: import again in a moment";
        error = true;
        return;
    }
    const float low = float(m.getNumber("low", 0.0)), high = float(m.getNumber("high", 100.0));
    const size_t tiles = size_t(terrain_->tilesPerSide()) * terrain_->tilesPerSide();
    undoRun_ = tiles <= kUndoTiles;
    before_.clear();
    if (undoRun_)
        snapshot(all);
    // The image covers the whole terrain: its top row along the -z edge, left column along -x.
    // Bilinear between pixels; a strip of tile rows at a time.
    const float sx = float(w - 1) / float(side - 1), sz = float(h - 1) / float(side - 1);
    const float scale = (high - low) / 65535.0f;
    for (int32_t z0 = 0; z0 < side; z0 += int32_t(terrain::kTileQuads)) {
        const TerrainRect r = terrain_->clampRect({0, z0, side - 1, z0 + int32_t(terrain::kTileQuads) - 1});
        heights_.resize(size_t(side) * size_t(r.z1 - r.z0 + 1));
        for (int32_t gz = r.z0; gz <= r.z1; ++gz) {
            const float v = float(gz) * sz;
            const int32_t y0 = std::min(int32_t(v), h - 2);
            const float fy = v - float(y0);
            const stbi_us* row0 = px + size_t(y0) * w;
            const stbi_us* row1 = row0 + w;
            for (int32_t gx = 0; gx < side; ++gx) {
                const float u = float(gx) * sx;
                const int32_t x0 = std::min(int32_t(u), w - 2);
                const float fx = u - float(x0);
                const float a = float(row0[x0]) * (1 - fx) + float(row0[x0 + 1]) * fx;
                const float b = float(row1[x0]) * (1 - fx) + float(row1[x0 + 1]) * fx;
                heights_[size_t(gz - r.z0) * side + gx] = low + (a * (1 - fy) + b * fy) * scale;
            }
        }
        terrain_->write(r, heights_);
    }
    stbi_image_free(px);
    terrain_->pin({0, 0, -1, -1});
    finishedEdit_ = undoRun_ ? pushEdit() : ~0u;
    changed_ = true;
    const TerrainSettings& s = terrain_->settings();
    char text[200];
    SDL_snprintf(text, sizeof(text), "Heightmap imported: %dx%d, %.1f to %.1f m%s%s", w, h, double(low), double(high),
                 deep ? "" : " (8-bit: steps of 1/255 of the range; a 16-bit PNG is smoother)",
                 std::min(low, high) < s.heightMin || std::max(low, high) > s.heightMax ? " (clipped to the terrain's height range)" : "");
    notice = text;
    if (!undoRun_)
        notice += " (too large to undo)";
}

void TerrainEditor::startErosionBrush()
{
    if (erodeDabs_.empty())
        return;
    const TerrainSettings& s = terrain_->settings();
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (const Dab& d : erodeDabs_) {
        lo[0] = std::min(lo[0], d.x - d.radius);
        lo[1] = std::min(lo[1], d.z - d.radius);
        hi[0] = std::max(hi[0], d.x + d.radius);
        hi[1] = std::max(hi[1], d.z + d.radius);
    }
    const TerrainRect r = terrain_->worldRect(lo[0], lo[1], hi[0], hi[1]);
    if (r.empty())
        return;
    // How much of the erosion each sample takes: the strongest dab over it (falloff x shape).
    const int32_t w = r.x1 - r.x0 + 1, d = r.z1 - r.z0 + 1;
    std::vector<float> weights(size_t(w) * d, 0.0f);
    for (const Dab& dab : erodeDabs_) {
        const float cr = std::cos(dab.rotation * (kPi / 180.0f)), sr = std::sin(dab.rotation * (kPi / 180.0f));
        const int32_t x0 = std::max(int32_t(std::floor((dab.x - dab.radius - s.originX) / s.spacing)) - r.x0, 0);
        const int32_t x1 = std::min(int32_t(std::ceil((dab.x + dab.radius - s.originX) / s.spacing)) - r.x0, w - 1);
        const int32_t z0 = std::max(int32_t(std::floor((dab.z - dab.radius - s.originZ) / s.spacing)) - r.z0, 0);
        const int32_t z1 = std::min(int32_t(std::ceil((dab.z + dab.radius - s.originZ) / s.spacing)) - r.z0, d - 1);
        for (int32_t z = z0; z <= z1; ++z)
            for (int32_t x = x0; x <= x1; ++x) {
                const float wx = s.originX + float(r.x0 + x) * s.spacing - dab.x, wz = s.originZ + float(r.z0 + z) * s.spacing - dab.z;
                const float dist = std::sqrt(wx * wx + wz * wz) / dab.radius;
                if (dist >= 1.0f)
                    continue;
                const float lx = (wx * cr + wz * sr) / dab.radius, lz = (-wx * sr + wz * cr) / dab.radius;
                float& v = weights[size_t(z) * w + x];
                v = std::max(v, falloff(dist) * alphaAt(lx, lz) * filterAt(r.x0 + x, r.z0 + z));
            }
    }
    erodeDabs_.clear();
    // Erosion at 1 m cells (or the terrain's spacing); the strength sets how long it runs.
    TerrainGenStack stack;
    stack.region.all = false;
    GenStep e;
    e.op = GenOp::Erode;
    e.name = "Erosion brush";
    e.cellSize = std::max(1.0f, s.spacing);
    e.iterations = uint32_t(40.0f + 360.0f * std::clamp(brush_.strength, 0.0f, 1.0f));
    stack.steps.push_back(e);
    undoRun_ = true;
    renderer_->terrainGenerator().start(*terrain_, stack, [this](TerrainRect c) { snapshot(c); }, &weights, r);
}

bool TerrainEditor::pickScatter(uint32_t slot)
{
    const bool was = picked_.valid;
    ScatterRenderer::Hit hit = {};
    picked_.valid = valid() && renderer_->scatter().find(slot, hit);
    if (picked_.valid)
        picked_ = {true, hit.layer, hit.asset, hit.candidate, hit.cellX, hit.cellZ};
    if (picked_.valid || was)
        changed_ = true;
    return picked_.valid;
}

uint32_t TerrainEditor::scatterExclusions() const
{
    uint32_t n = 0;
    if (valid())
        for (const ScatterLayer& l : parseScatterLayers(terrain_->scatterJson()))
            n += uint32_t(l.exclusions.size());
    return n;
}

float TerrainEditor::scatterGround(const ScatterLayer& l) const
{
    const TerrainSettings& s = terrain_->settings();
    const float size = terrain_->worldSize(), d = s.spacing;
    constexpr int kGrid = 64;
    int inside = 0;
    for (int i = 0; i < kGrid; ++i)
        for (int k = 0; k < kGrid; ++k) {
            const float x = s.originX + (float(i) + 0.5f) * size / kGrid, z = s.originZ + (float(k) + 0.5f) * size / kGrid;
            const float h = terrain_->heightAt(x, z);
            const float dx = terrain_->heightAt(x + d, z) - terrain_->heightAt(x - d, z);
            const float dz = terrain_->heightAt(x, z + d) - terrain_->heightAt(x, z - d);
            const float slope = std::atan(std::sqrt(dx * dx + dz * dz) / (2.0f * d)) * (180.0f / kPi);
            inside += slope >= l.slope[0] && slope <= l.slope[1] && h >= l.height[0] && h <= l.height[1];
        }
    return float(inside) / float(kGrid * kGrid);
}

void TerrainEditor::write(JsonWriter& w)
{
    w.beginObject("terrain").value("exists", valid());
    if (valid()) {
        const TerrainSettings& s = terrain_->settings();
        const TerrainRenderStats& rs = renderer_->terrainStats();
        w.value("tiles", s.tiles)
            .value("spacing", double(s.spacing))
            .value("size", double(terrain_->worldSize()))
            .value("heightMin", double(s.heightMin))
            .value("heightMax", double(s.heightMax))
            .value("resident", terrain_->residentTiles())
            .value("patches", rs.patches)
            .value("patchCapacity", rs.capacity)
            .value("dirty", terrain_->dirty());
    }
    if (settings_)
        w.value("pixels", double(settings_->terrainPixels));
    w.beginObject("brush");
    writeBrush(w, brush_);
    w.endObject();
    w.beginArray("layers");
    if (valid())
        for (const TerrainLayer& l : parseTerrainLayers(terrain_->layersJson()))
            writeLayer(w, l);
    w.endArray();
    w.value("building", valid() && renderer_->terrainBuilding());
    // Generation: the stack, the presets, the run in progress.
    {
        const TerrainGenerator& g = renderer_->terrainGenerator();
        w.beginObject("generate").raw("stack", valid() ? genStackJson(parseGenStack(terrain_->generatorJson())) : std::string("{}"));
        w.beginArray("presets");
        for (const GenPreset& p : terrainGeneratorPresets())
            w.value(nullptr, p.name);
        w.endArray().value("running", g.busy()).value("progress", double(g.progress())).value("status", g.status());
        w.value("lastMs", g.lastMs()).endObject();
    }
    // Scatter: the layers (deleted instances as a count), which have instance slots, the meshes
    // assets can use, the picked instance, the renderer's counts.
    {
        std::vector<ScatterLayer> layers = valid() ? parseScatterLayers(terrain_->scatterJson()) : std::vector<ScatterLayer>();
        std::vector<uint32_t> deleted;
        for (ScatterLayer& l : layers) {
            deleted.push_back(uint32_t(l.exclusions.size()));
            l.exclusions.clear();
        }
        const ScatterRenderer& sr = renderer_->scatter();
        w.beginObject("scatter").raw("layers", scatterLayersJson(layers));
        w.beginArray("deleted");
        for (uint32_t d : deleted)
            w.value(nullptr, d);
        w.endArray().beginArray("grows");  // per layer: the share of the terrain its slope and height rules allow
        for (const ScatterLayer& l : layers)
            w.value(nullptr, valid() ? scatterGround(l) : 1.0f);
        w.endArray().beginArray("live");  // per layer and asset: slots reserved this session
        for (uint32_t l = 0; l < layers.size(); ++l) {
            w.beginArray();
            for (uint32_t a = 0; a < layers[l].assets.size(); ++a) {
                bool live = false;
                for (const ScatterPool& p : sr.pools())
                    live = live || (p.layer == l && p.asset == a && pak_ && pak_->findMesh(layers[l].assets[a].mesh) == int32_t(p.mesh));
                w.value(nullptr, live);
            }
            w.endArray();
        }
        w.endArray().beginArray("meshes");
        for (const std::string& n : meshNames_)
            w.value(nullptr, n);
        w.endArray().beginArray("presets");
        for (const ScatterPreset& p : scatterPresets())
            w.value(nullptr, p.name);
        w.endArray();
        if (picked_.valid && picked_.layer < layers.size() && picked_.asset < layers[picked_.layer].assets.size())
            w.beginObject("picked")
                .value("layer", picked_.layer)
                .value("name", layers[picked_.layer].name)
                .value("mesh", layers[picked_.layer].assets[picked_.asset].mesh)
                .endObject();
        const ScatterStats& st = sr.stats();
        w.value("cells", st.cells).value("slots", st.slots).value("starved", st.starved).value("unplaced", st.unplaced);
        // Thinned to fit the GPU's budgets (dense grass): scatter's smallest size drawn, cast shadows.
        w.value("thinPixels", std::round(renderer_->scatterPixels())).value("thinShadow", std::round(renderer_->scatterShadowTexels() * 10.0f) / 10.0f);
        w.endObject();
    }
    w.beginArray("images");
    for (const std::string& i : images())
        w.value(nullptr, i);
    w.endArray();
    w.beginArray("presets");
    for (const auto& p : presets_)
        w.value(nullptr, p.first);
    w.endArray();
    w.beginArray("alphas");
    std::error_code ec;
    std::vector<std::string> images;
    for (const auto& f : std::filesystem::directory_iterator(sceneFolder() + "/brushes", ec))
        if (f.is_regular_file() && isImage(f.path()))
            images.push_back(f.path().filename().string());
    std::sort(images.begin(), images.end());
    for (const std::string& i : images)
        w.value(nullptr, i);
    w.endArray();
    w.endObject();
}

bool TerrainEditor::save()
{
    const std::string dir = Terrain::dirFor(scenePath_);
    if (deleted_) {
        std::error_code ec;
        if (std::filesystem::exists(dir, ec)) {
            const std::string aside = dir + ".deleted";
            std::filesystem::remove_all(aside, ec);
            std::filesystem::rename(dir, aside, ec);
            if (ec) {
                FX_ERROR("terrain: cannot move %s aside: %s", dir.c_str(), ec.message().c_str());
                return false;
            }
        }
        deleted_ = false;
        return true;
    }
    if (!valid() || !terrain_->dirty())
        return true;
    return terrain_->save(dir);
}

}  // namespace fx
