#include "terrain/terrain_layers.h"

#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"

// Implementations live in the cooker (tools/cook/build_textures.cpp), linked into falcon.exe.
#include <bc7enc.h>
#include <rgbcx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fx {

const char* const kGeneratedLayers[5] = {"grass", "dirt", "rock", "sand", "snow"};

namespace {

constexpr char kCacheMagic[4] = {'F', 'T', 'L', '1'};
constexpr uint32_t kCacheVersion = 1;

// ---- tileable noise (u, v in [0, 1), `cells` lattice cells across the texture)

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}

float periodicNoise(float u, float v, uint32_t cells, uint32_t seed)
{
    const float x = u * float(cells), y = v * float(cells);
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = int(fx), iy = int(fy);
    const float tx = x - fx, ty = y - fy;
    auto grad = [&](int cx, int cy, float dx, float dy) {
        const uint32_t h = hash3(uint32_t((cx % int(cells) + int(cells)) % int(cells)), uint32_t((cy % int(cells) + int(cells)) % int(cells)), seed);
        const float a = float(h & 0xffff) * (6.28318531f / 65536.0f);
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float sx = tx * tx * tx * (tx * (tx * 6 - 15) + 10), sy = ty * ty * ty * (ty * (ty * 6 - 15) + 10);
    const float a = grad(ix, iy, tx, ty), b = grad(ix + 1, iy, tx - 1, ty);
    const float c = grad(ix, iy + 1, tx, ty - 1), d = grad(ix + 1, iy + 1, tx - 1, ty - 1);
    return ((a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy) * 1.41421356f;  // [-1, 1]
}

float periodicFbm(float u, float v, uint32_t cells, uint32_t octaves, uint32_t seed)
{
    float sum = 0, amp = 0.5f, norm = 0;
    for (uint32_t o = 0; o < octaves; ++o, cells *= 2, amp *= 0.5f) {
        sum += periodicNoise(u, v, cells, seed + o * 7919u) * amp;
        norm += amp;
    }
    return sum / norm;  // [-1, 1]
}

// Distances to the nearest and second-nearest feature point, one point per cell (in cell units).
void worley(float u, float v, uint32_t cells, uint32_t seed, float& f1, float& f2)
{
    const float x = u * float(cells), y = v * float(cells);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    f1 = f2 = 1e9f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = ix + dx, cy = iy + dy;
            const uint32_t h = hash3(uint32_t((cx % int(cells) + int(cells)) % int(cells)), uint32_t((cy % int(cells) + int(cells)) % int(cells)), seed);
            const float px = float(cx) + float(h & 0xffff) / 65536.0f, py = float(cy) + float(h >> 16) / 65536.0f;
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
}

float smooth(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float mix(float a, float b, float t) { return a + (b - a) * t; }

struct Texel {
    float albedo[3];  // linear
    float height, roughness, ao;
};

// The generated looks; `bump`: how strongly height turns into the normal map.
Texel generate(const std::string& kind, float u, float v, uint32_t seed, float& bump)
{
    Texel t = {};
    if (kind == "dirt") {
        bump = 4.0f;
        const float base = periodicFbm(u, v, 6, 5, seed) * 0.5f + 0.5f;
        float f1, f2;
        worley(u, v, 22, seed + 3, f1, f2);
        const float pebble = 1.0f - smooth(0.18f, 0.42f, f1);
        const float grain = periodicFbm(u, v, 96, 2, seed + 9) * 0.5f + 0.5f;
        t.height = std::clamp(0.35f * base + 0.5f * pebble * (0.7f + 0.3f * grain) + 0.15f * grain, 0.0f, 1.0f);
        const float shade = 0.75f + 0.5f * base;
        const float c[3] = {mix(0.11f, 0.19f, pebble), mix(0.075f, 0.165f, pebble), mix(0.045f, 0.135f, pebble)};
        for (int k = 0; k < 3; ++k)
            t.albedo[k] = c[k] * shade * (0.85f + 0.3f * grain);
        t.roughness = mix(0.93f, 0.72f, pebble);
        t.ao = 0.55f + 0.45f * t.height;
    } else if (kind == "rock") {
        bump = 7.0f;
        float ridged = 0, amp = 0.5f, norm = 0;
        for (uint32_t o = 0, cells = 4; o < 6; ++o, cells *= 2, amp *= 0.5f) {
            ridged += (1.0f - std::fabs(periodicNoise(u, v, cells, seed + o * 31))) * amp;
            norm += amp;
        }
        ridged /= norm;
        float f1, f2;
        worley(u, v, 9, seed + 5, f1, f2);
        const float crack = 1.0f - smooth(0.0f, 0.08f, f2 - f1);
        const float lichen = smooth(0.35f, 0.6f, periodicFbm(u, v, 5, 4, seed + 11) * 0.5f + 0.5f);
        t.height = std::clamp(ridged * 0.9f - crack * 0.35f + 0.1f, 0.0f, 1.0f);
        const float shade = 0.6f + 0.6f * t.height;
        const float c[3] = {mix(0.19f, 0.17f, lichen), mix(0.18f, 0.18f, lichen), mix(0.17f, 0.11f, lichen)};
        for (int k = 0; k < 3; ++k)
            t.albedo[k] = c[k] * shade;
        t.roughness = 0.78f;
        t.ao = 0.4f + 0.6f * (1.0f - crack) * (0.5f + 0.5f * t.height);
    } else if (kind == "sand") {
        bump = 2.5f;
        const float warp = periodicFbm(u, v, 4, 4, seed);
        const float ripple = 0.5f + 0.5f * std::sin(6.28318531f * (14.0f * v + 0.9f * warp + 2.0f * u));
        const float grain = periodicFbm(u, v, 128, 2, seed + 4) * 0.5f + 0.5f;
        const float dunes = periodicFbm(u, v, 3, 3, seed + 8) * 0.5f + 0.5f;
        t.height = std::clamp(0.45f * ripple + 0.2f * grain + 0.35f * dunes, 0.0f, 1.0f);
        const float shade = 0.85f + 0.3f * grain;
        t.albedo[0] = 0.46f * shade;
        t.albedo[1] = 0.38f * shade;
        t.albedo[2] = 0.25f * shade;
        t.roughness = 0.95f;
        t.ao = 0.7f + 0.3f * t.height;
    } else if (kind == "snow") {
        bump = 2.0f;
        const float n = periodicFbm(u, v, 4, 5, seed) * 0.5f + 0.5f;
        const float crust = periodicFbm(u, v, 48, 3, seed + 2) * 0.5f + 0.5f;
        t.height = std::clamp(0.75f * n + 0.25f * crust, 0.0f, 1.0f);
        const float shade = 0.9f + 0.1f * t.height;
        t.albedo[0] = 0.80f * shade;
        t.albedo[1] = 0.84f * shade;
        t.albedo[2] = 0.90f * shade;
        t.roughness = mix(0.65f, 0.45f, crust);
        t.ao = 0.85f + 0.15f * t.height;
    } else {  // grass
        bump = 3.0f;
        const float patches = periodicFbm(u, v, 4, 4, seed) * 0.5f + 0.5f;
        const float blades = periodicFbm(u, v, 64, 3, seed + 1) * 0.5f + 0.5f;
        const float dry = smooth(0.55f, 0.75f, periodicFbm(u, v, 3, 3, seed + 2) * 0.5f + 0.5f);
        t.height = std::clamp(0.3f * patches + 0.7f * blades, 0.0f, 1.0f);
        const float c0[3] = {0.055f, 0.10f, 0.022f}, c1[3] = {0.12f, 0.16f, 0.035f}, cd[3] = {0.20f, 0.17f, 0.07f};
        for (int k = 0; k < 3; ++k)
            t.albedo[k] = mix(mix(c0[k], c1[k], patches), cd[k], dry * 0.7f) * (0.65f + 0.7f * blades);
        t.roughness = 0.9f;
        t.ao = 0.5f + 0.5f * blades;
    }
    return t;
}

float toLinear(float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
uint8_t toSrgb8(float l)
{
    l = std::clamp(l, 0.0f, 1.0f);
    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(s * 255.0f));
}
uint8_t unorm8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// RGBA8 image, wrapped resize to res x res.
bool loadImage(const std::string& path, uint32_t res, bool srgb, std::vector<uint8_t>& out)
{
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) {
        FX_WARN("terrain: cannot read layer image %s", path.c_str());
        return false;
    }
    out.resize(size_t(res) * res * 4);
    STBIR_RESIZE r;
    stbir_resize_init(&r, px, w, h, 0, out.data(), int(res), int(res), 0, srgb ? STBIR_RGBA : STBIR_4CHANNEL,
                      srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8);
    stbir_set_edgemodes(&r, STBIR_EDGE_WRAP, STBIR_EDGE_WRAP);
    stbir_resize_extended(&r);
    stbi_image_free(px);
    return true;
}

// Mip 0 of the three channels as RGBA8: albedo (sRGB) + height, normal xy, ORM.
void buildBase(const TerrainLayer& layer, const std::string& folder, uint32_t res, std::vector<uint8_t> base[3], float mean[3])
{
    const size_t n = size_t(res) * res;
    for (int c = 0; c < 3; ++c)
        base[c].assign(n * 4, 255);
    std::vector<float> height(n, 0.5f);
    float bump = 3.0f;
    bool haveNormal = false;
    if (layer.generated.empty()) {
        std::vector<uint8_t> img;
        auto path = [&](const std::string& p) { return folder + "/" + p; };
        if (!layer.albedo.empty() && loadImage(path(layer.albedo), res, true, img))
            for (size_t i = 0; i < n; ++i)
                std::memcpy(&base[0][i * 4], &img[i * 4], 3);
        else
            for (size_t i = 0; i < n; ++i)
                base[0][i * 4] = base[0][i * 4 + 1] = base[0][i * 4 + 2] = 128;
        // Height: its own image, else the albedo's brightness (a fair guess for most ground).
        if (!layer.height.empty() && loadImage(path(layer.height), res, false, img)) {
            for (size_t i = 0; i < n; ++i)
                height[i] = float(img[i * 4]) / 255.0f;
        } else {
            for (size_t i = 0; i < n; ++i)
                height[i] = (0.3f * base[0][i * 4] + 0.59f * base[0][i * 4 + 1] + 0.11f * base[0][i * 4 + 2]) / 255.0f;
        }
        if (!layer.normal.empty() && loadImage(path(layer.normal), res, false, img)) {
            for (size_t i = 0; i < n; ++i) {
                base[1][i * 4] = img[i * 4];
                base[1][i * 4 + 1] = img[i * 4 + 1];
            }
            haveNormal = true;
        }
        const bool rough = !layer.roughnessMap.empty() && loadImage(path(layer.roughnessMap), res, false, img);
        for (size_t i = 0; i < n; ++i)
            base[2][i * 4 + 1] = rough ? img[i * 4] : 217;  // 0.85
        const bool ao = !layer.ao.empty() && loadImage(path(layer.ao), res, false, img);
        for (size_t i = 0; i < n; ++i) {
            base[2][i * 4] = ao ? img[i * 4] : 255;
            base[2][i * 4 + 2] = 0;  // metallic
        }
    } else {
        for (uint32_t y = 0; y < res; ++y)
            for (uint32_t x = 0; x < res; ++x) {
                const size_t i = size_t(y) * res + x;
                const Texel t = generate(layer.generated, (float(x) + 0.5f) / float(res), (float(y) + 0.5f) / float(res), layer.seed, bump);
                for (int k = 0; k < 3; ++k)
                    base[0][i * 4 + k] = toSrgb8(t.albedo[k]);
                height[i] = t.height;
                base[2][i * 4] = unorm8(t.ao);
                base[2][i * 4 + 1] = unorm8(t.roughness);
                base[2][i * 4 + 2] = 0;
            }
    }
    for (size_t i = 0; i < n; ++i)
        base[0][i * 4 + 3] = unorm8(height[i]);
    if (!haveNormal) {  // from the height, wrapping around
        const float k = bump * float(res) / 256.0f;
        for (uint32_t y = 0; y < res; ++y)
            for (uint32_t x = 0; x < res; ++x) {
                const float dx = height[size_t(y) * res + (x + 1) % res] - height[size_t(y) * res + (x + res - 1) % res];
                const float dy = height[size_t((y + 1) % res) * res + x] - height[size_t((y + res - 1) % res) * res + x];
                float nx = -dx * k, ny = -dy * k;
                const float l = std::sqrt(nx * nx + ny * ny + 1.0f);
                nx /= l;
                ny /= l;
                const size_t i = size_t(y) * res + x;
                base[1][i * 4] = unorm8(nx * 0.5f + 0.5f);
                base[1][i * 4 + 1] = unorm8(ny * 0.5f + 0.5f);
            }
    }
    double sum[3] = {};
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            sum[c] += toLinear(float(base[0][i * 4 + c]) / 255.0f);
    for (int c = 0; c < 3; ++c)
        mean[c] = float(sum[c] / double(n));
}

// The next mip: 2x2 averages (albedo in linear light, normals renormalized).
void downsample(const std::vector<uint8_t>& src, uint32_t res, int channel, std::vector<uint8_t>& dst)
{
    const uint32_t r = res / 2;
    dst.resize(size_t(r) * r * 4);
    for (uint32_t y = 0; y < r; ++y)
        for (uint32_t x = 0; x < r; ++x) {
            const uint8_t* p[4] = {&src[(size_t(2 * y) * res + 2 * x) * 4], &src[(size_t(2 * y) * res + 2 * x + 1) * 4],
                                   &src[(size_t(2 * y + 1) * res + 2 * x) * 4], &src[(size_t(2 * y + 1) * res + 2 * x + 1) * 4]};
            uint8_t* o = &dst[(size_t(y) * r + x) * 4];
            if (channel == 0) {
                for (int c = 0; c < 3; ++c)
                    o[c] = toSrgb8((toLinear(p[0][c] / 255.0f) + toLinear(p[1][c] / 255.0f) + toLinear(p[2][c] / 255.0f) +
                                    toLinear(p[3][c] / 255.0f)) * 0.25f);
                o[3] = uint8_t((p[0][3] + p[1][3] + p[2][3] + p[3][3] + 2) / 4);
            } else if (channel == 1) {
                float nx = 0, ny = 0, nz = 0;
                for (const uint8_t* q : p) {
                    const float a = q[0] / 127.5f - 1.0f, b = q[1] / 127.5f - 1.0f;
                    nx += a;
                    ny += b;
                    nz += std::sqrt(std::max(1.0f - a * a - b * b, 0.0f));
                }
                const float l = std::max(std::sqrt(nx * nx + ny * ny + nz * nz), 1e-6f);
                o[0] = unorm8(nx / l * 0.5f + 0.5f);
                o[1] = unorm8(ny / l * 0.5f + 0.5f);
                o[2] = o[3] = 255;
            } else {
                for (int c = 0; c < 4; ++c)
                    o[c] = uint8_t((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
            }
        }
}

// BC blocks of one mip (16 bytes each), rows of blocks split over threads.
void encode(const std::vector<uint8_t>& img, uint32_t res, int channel, uint8_t* out)
{
    static const bool once = [] {
        bc7enc_compress_block_init();
        rgbcx::init();
        return true;
    }();
    (void)once;
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (channel == 2)
        bc7enc_compress_block_params_init_linear_weights(&params);
    const uint32_t blocks = res / 4;
    std::atomic<uint32_t> next = 0;
    auto work = [&] {
        uint8_t block[64];
        for (uint32_t by; (by = next++) < blocks;)
            for (uint32_t bx = 0; bx < blocks; ++bx) {
                for (uint32_t y = 0; y < 4; ++y)
                    std::memcpy(block + y * 16, &img[(size_t(by * 4 + y) * res + bx * 4) * 4], 16);
                uint8_t* dst = out + (size_t(by) * blocks + bx) * 16;
                if (channel == 1)
                    rgbcx::encode_bc5(dst, block, 0, 1, 4);
                else
                    bc7enc_compress_block(dst, block, &params);
            }
    };
    std::vector<std::thread> threads(std::clamp(std::thread::hardware_concurrency(), 1u, 16u) - 1);
    for (std::thread& t : threads)
        t = std::thread(work);
    work();
    for (std::thread& t : threads)
        t.join();
}

uint32_t mipCount(uint32_t res)  // down to 4 x 4 (one BC block)
{
    uint32_t m = 0;
    for (uint32_t r = res; r >= 4; r /= 2)
        ++m;
    return m;
}

bool readCache(const std::string& path, uint64_t key, uint32_t res, TerrainLayerTextures& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    char magic[4];
    uint32_t meta[3];
    uint64_t fileKey = 0;
    float mean[3];
    uint64_t sizes[3];
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(meta), sizeof(meta));
    in.read(reinterpret_cast<char*>(&fileKey), 8);
    in.read(reinterpret_cast<char*>(mean), sizeof(mean));
    in.read(reinterpret_cast<char*>(sizes), sizeof(sizes));
    if (!in || std::memcmp(magic, kCacheMagic, 4) || meta[0] != kCacheVersion || meta[1] != res || fileKey != key)
        return false;
    out.resolution = res;
    out.mips = meta[2];
    std::memcpy(out.meanAlbedo, mean, sizeof(mean));
    for (int c = 0; c < 3; ++c) {
        out.channel[c].resize(size_t(sizes[c]));
        in.read(reinterpret_cast<char*>(out.channel[c].data()), std::streamsize(sizes[c]));
    }
    return bool(in);
}

void writeCache(const std::string& path, uint64_t key, const TerrainLayerTextures& t)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const uint32_t meta[3] = {kCacheVersion, t.resolution, t.mips};
        const uint64_t sizes[3] = {t.channel[0].size(), t.channel[1].size(), t.channel[2].size()};
        out.write(kCacheMagic, 4);
        out.write(reinterpret_cast<const char*>(meta), sizeof(meta));
        out.write(reinterpret_cast<const char*>(&key), 8);
        out.write(reinterpret_cast<const char*>(t.meanAlbedo), sizeof(t.meanAlbedo));
        out.write(reinterpret_cast<const char*>(sizes), sizeof(sizes));
        for (const auto& c : t.channel)
            out.write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size()));
        if (!out)
            return;
    }
    std::filesystem::rename(tmp, path, ec);
}

}  // namespace

std::vector<TerrainLayer> parseTerrainLayers(const std::string& json)
{
    std::vector<TerrainLayer> layers;
    try {
        const Json root = Json::parse(json);
        for (const Json& j : root.array) {
            TerrainLayer l;
            l.name = j.getString("name", "Layer");
            l.generated = j.getString("generated", "");
            l.seed = uint32_t(j.getNumber("seed", 1));
            l.albedo = j.getString("albedo");
            l.normal = j.getString("normal");
            l.roughnessMap = j.getString("roughnessMap");
            l.ao = j.getString("ao");
            l.height = j.getString("height");
            l.tileSize = float(std::clamp(j.getNumber("tileSize", 4.0), 0.05, 1000.0));
            l.hex = j.getBool("hex", true);
            l.rotation = float(std::clamp(j.getNumber("rotation", 1.0), 0.0, 1.0));
            l.contrast = float(std::clamp(j.getNumber("contrast", 0.5), 0.0, 1.0));
            l.heightBlend = float(std::clamp(j.getNumber("heightBlend", 0.25), 0.01, 1.0));
            l.macro = float(std::clamp(j.getNumber("macro", 0.25), 0.0, 1.0));
            l.macroScale = float(std::clamp(j.getNumber("macroScale", 60.0), 1.0, 10000.0));
            l.projection = uint32_t(std::clamp(j.getNumber("projection", 1.0), 0.0, 2.0));
            if (const Json* t = j.get("tint"); t && t->array.size() == 3)
                for (int k = 0; k < 3; ++k)
                    l.tint[k] = float(std::clamp(t->array[k].number, 0.0, 4.0));
            l.roughness = float(std::clamp(j.getNumber("roughness", 1.0), 0.0, 2.0));
            l.normalStrength = float(std::clamp(j.getNumber("normalStrength", 1.0), 0.0, 4.0));
            l.farScale = float(std::clamp(j.getNumber("farScale", 4.0), 1.0, 32.0));
            l.acoustic = j.getString("acoustic");
            l.surface = j.getString("surface");
            layers.push_back(std::move(l));
            if (layers.size() == 32)
                break;
        }
    } catch (const std::exception& e) {
        FX_WARN("terrain: bad layer list (%s)", e.what());
    }
    return layers;
}

std::string terrainLayersJson(const std::vector<TerrainLayer>& layers)
{
    JsonWriter w;
    w.beginArray();
    for (const TerrainLayer& l : layers) {
        w.beginObject()
            .value("name", l.name)
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
            .value("projection", l.projection);
        w.beginArray("tint");
        for (float t : l.tint)
            w.value(nullptr, double(t));
        w.endArray()
            .value("roughness", double(l.roughness))
            .value("normalStrength", double(l.normalStrength))
            .value("farScale", double(l.farScale))
            .value("acoustic", l.acoustic)
            .value("surface", l.surface)
            .endObject();
    }
    w.endArray();
    return w.str();
}

uint64_t terrainLayerKey(const TerrainLayer& l, const std::string& folder, uint32_t res)
{
    std::string text = std::to_string(kCacheVersion) + "|" + std::to_string(res) + "|" + l.generated + "|" + std::to_string(l.seed);
    for (const std::string* p : {&l.albedo, &l.normal, &l.roughnessMap, &l.ao, &l.height}) {
        text += "|" + *p;
        if (!p->empty()) {  // the file's size and time: a re-saved image rebuilds
            std::error_code ec;
            const std::filesystem::path path = std::filesystem::path(folder) / *p;
            text += ":" + std::to_string(std::filesystem::file_size(path, ec));
            text += ":" + std::to_string(int64_t(std::filesystem::last_write_time(path, ec).time_since_epoch().count()));
        }
    }
    uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a
    for (const char c : text)
        h = (h ^ uint8_t(c)) * 0x100000001b3ull;
    return h;
}

void TerrainLayerBuilder::request(uint32_t index, const TerrainLayer& layer, const std::string& sceneFolder,
                                  const std::string& cacheDir, uint32_t resolution)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The newest request per layer replaces a waiting one.
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.index == index; }), jobs_.end());
        jobs_.push_back({index, terrainLayerKey(layer, sceneFolder, resolution), layer, sceneFolder, cacheDir, resolution});
        if (!thread_.joinable())
            thread_ = std::thread([this] { main(); });
    }
    wake_.notify_one();
}

bool TerrainLayerBuilder::poll(uint32_t& index, uint64_t& key, TerrainLayerTextures& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty())
        return false;
    index = done_.front().index;
    key = done_.front().key;
    out = std::move(done_.front().textures);
    done_.erase(done_.begin());
    return true;
}

bool TerrainLayerBuilder::busy()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return working_ || !jobs_.empty() || !done_.empty();
}

void TerrainLayerBuilder::stop()
{
    if (!thread_.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    thread_.join();
    quit_ = false;
}

void TerrainLayerBuilder::main()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            working_ = true;
        }
        FX_ZONE("TerrainLayerBuilder::build");
        char name[32];
        SDL_snprintf(name, sizeof(name), "%016llx.ftl", (unsigned long long)job.key);
        const std::string cachePath = job.cacheDir + "/" + name;
        Done done = {job.index, job.key, {}};
        if (!readCache(cachePath, job.key, job.resolution, done.textures)) {
            const uint64_t t0 = SDL_GetTicksNS();
            TerrainLayerTextures& t = done.textures;
            t.resolution = job.resolution;
            t.mips = mipCount(job.resolution);
            std::vector<uint8_t> level[3], next;
            buildBase(job.layer, job.sceneFolder, job.resolution, level, t.meanAlbedo);
            for (int c = 0; c < 3; ++c) {
                size_t total = 0;
                for (uint32_t m = 0; m < t.mips; ++m)
                    total += size_t(job.resolution >> m) * (job.resolution >> m);  // 1 byte per texel (16 per 4x4 block)
                t.channel[c].resize(total);
                size_t offset = 0;
                std::vector<uint8_t> img = level[c];
                for (uint32_t m = 0; m < t.mips; ++m) {
                    const uint32_t r = job.resolution >> m;
                    encode(img, r, c, t.channel[c].data() + offset);
                    offset += size_t(r) * r;
                    if (m + 1 < t.mips) {
                        downsample(img, r, c, next);
                        img.swap(next);
                    }
                }
            }
            writeCache(cachePath, job.key, t);
            FX_INFO("terrain: layer '%s' textures built in %.1f s", job.layer.name.c_str(), double(SDL_GetTicksNS() - t0) * 1e-9);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(done));
        working_ = false;
    }
}

}  // namespace fx
