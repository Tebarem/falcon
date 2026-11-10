// The starter trim set (trim_sheet.h makeStarterTrims): a 1024 x 1024 texture of eight 128-pixel
// strips drawn here, its material and its .ftrim.

#include "level/trim_sheet.h"

#include "assets/material_asset.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fx::level {

namespace {

constexpr int kSize = 1024, kStrip = 128;

float hash(int x, int y, int seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

// Smooth value noise at a scale (pixels per cell), tiling across the texture's width.
float noise(float x, float y, float cell, int seed)
{
    const int period = std::max(1, int(kSize / cell));
    const float fx = x / cell, fy = y / cell;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const auto at = [&](int i, int j) { return hash(((i % period) + period) % period, j, seed); };
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

float fbm(float x, float y, float cell, int seed)
{
    return noise(x, y, cell, seed) * 0.55f + noise(x, y, cell * 0.5f, seed + 1) * 0.3f + noise(x, y, cell * 0.25f, seed + 2) * 0.15f;
}

struct Rgb {
    float r, g, b;
};
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scale(Rgb a, float s) { return {a.r * s, a.g * s, a.b * s}; }
float smooth(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Pixel x, y of strip `strip` (local row y in 0..kStrip).
Rgb shade(int strip, float x, float y)
{
    const float v = y / kStrip;
    const float bevel = 1.0f - 0.35f * (smooth(0.0f, 0.08f, 0.08f - v) + smooth(0.92f, 1.0f, v));  // dark lips top and bottom
    switch (strip) {
    case 0: {  // brushed metal edge
        const float brush = noise(x, y * 40.0f, 6.0f, 1) * 0.12f;
        const float hi = smooth(0.06f, 0.12f, v) * (1 - smooth(0.85f, 0.94f, v));
        return scale(mix({0.42f, 0.43f, 0.45f}, {0.62f, 0.63f, 0.65f}, hi), (0.9f + brush) * bevel);
    }
    case 1: {  // painted metal, worn through along its middle
        const float wear = smooth(0.42f, 0.55f, fbm(x, y, 18.0f, 2) + 0.35f * (1 - std::fabs(v - 0.5f) * 2));
        return scale(mix({0.20f, 0.33f, 0.42f}, {0.66f, 0.66f, 0.64f}, wear), bevel);
    }
    case 2: {  // dark plate, a bolt every 128 pixels
        const float dx = std::fmod(x, 128.0f) - 64.0f, dy = y - 64.0f, d = std::sqrt(dx * dx + dy * dy);
        const float head = 1 - smooth(16.0f, 19.0f, d), rim = smooth(13.0f, 16.0f, d) * head;
        Rgb plate = scale({0.26f, 0.27f, 0.28f}, 0.85f + 0.3f * fbm(x, y, 32.0f, 3));
        return scale(mix(plate, mix({0.55f, 0.55f, 0.56f}, {0.35f, 0.35f, 0.36f}, rim), head), bevel);
    }
    case 3: {  // panel with a seam groove
        const float groove = 1 - smooth(2.0f, 5.0f, std::fabs(y - 64.0f));
        return scale(scale({0.55f, 0.56f, 0.55f}, 0.9f + 0.15f * fbm(x, y, 40.0f, 4)), (1 - 0.65f * groove) * bevel);
    }
    case 4: {  // concrete, chipped along its top edge
        const float edge = 10.0f + 24.0f * fbm(x, 0.0f, 22.0f, 5);
        const float broken = smooth(edge - 2.0f, edge + 2.0f, y);
        Rgb concrete = scale({0.58f, 0.57f, 0.54f}, 0.8f + 0.35f * fbm(x, y, 12.0f, 6));
        return mix(scale(concrete, 0.45f), concrete, broken);
    }
    case 5: {  // planks: grain along u, a gap every 32 rows
        const float grain = noise(x * 0.05f, y * 3.0f, 2.0f, 7);
        const float gap = 1 - smooth(0.0f, 2.0f, std::fabs(std::fmod(y, 32.0f) - 16.0f) - 13.0f);
        return scale(mix({0.42f, 0.27f, 0.15f}, {0.58f, 0.39f, 0.22f}, grain), 1 - 0.6f * gap);
    }
    case 6: {  // hazard stripes
        const float band = std::fmod(x + y + 1024.0f, 64.0f) < 32.0f ? 1.0f : 0.0f;
        const float dirt = 0.85f + 0.15f * fbm(x, y, 20.0f, 8);
        return scale(mix({0.08f, 0.08f, 0.07f}, {0.85f, 0.65f, 0.08f}, band), dirt * bevel);
    }
    default: {  // grime running down: light at the top, dark at the bottom, streaked
        const float streak = noise(x, y * 0.1f, 8.0f, 9);
        const float dark = std::clamp(v * 0.9f + (streak - 0.5f) * 0.4f, 0.0f, 1.0f);
        return mix({0.62f, 0.60f, 0.56f}, {0.18f, 0.16f, 0.13f}, dark);
    }
    }
}

const char* const kNames[8] = {"Metal edge", "Worn bevel", "Bolts", "Panel seam", "Chipped concrete", "Planks", "Hazard stripes", "Grime"};

}  // namespace

bool makeStarterTrims(const std::string& projectDir, const std::string& folder, std::string& sheetPath, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(projectDir) / folder;
    fs::create_directories(dir, ec);
    if (ec)
        return error = "Can't make " + dir.generic_string() + ": " + ec.message(), false;
    std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const Rgb c = shade(y / kStrip, float(x), float(y % kStrip));
            uint8_t* p = &pixels[(size_t(y) * kSize + x) * 4];
            p[0] = uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = 255;
        }
    const std::string base = "Starter Trims";
    const std::string image = folder + "/" + base + ".png", material = folder + "/" + base + ".fmat";
    sheetPath = folder + "/" + base + ".ftrim";
    SDL_Surface* s = SDL_CreateSurfaceFrom(kSize, kSize, SDL_PIXELFORMAT_RGBA32, pixels.data(), kSize * 4);
    const bool saved = s && SDL_SavePNG(s, (fs::path(projectDir) / image).string().c_str());
    SDL_DestroySurface(s);
    if (!saved)
        return error = "Can't write " + image + ": " + SDL_GetError(), false;
    MaterialAsset mat;
    mat.name = base;
    mat.roughness = 0.6f;
    mat.textures[0] = image;
    if (!writeMaterialAsset((fs::path(projectDir) / material).string(), mat, error))
        return false;
    TrimSheet sheet;
    sheet.material = material;
    for (int k = 0; k < 8; ++k)
        sheet.strips.push_back({kNames[k], float(k) / 8.0f, float(k + 1) / 8.0f});
    return writeTrimSheet((fs::path(projectDir) / sheetPath).string(), sheet, error);
}

}  // namespace fx::level
