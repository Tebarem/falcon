// Virtual texture cooking: per material, decode -> resize to a power of two -> mip chain ->
// 128x128 pages with a 4-texel wrapped border -> BC7 (albedo, ORM) / BC5 (normal) -> .fvt.

#include "cook.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_FALLBACK
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

extern "C" {
#include <bc7enc.h>
}
#define RGBCX_IMPLEMENTATION
#include <rgbcx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fx::cook {

// fseek past 2 GB (virtual texture pages).
static int seek64(FILE* f, int64_t offset, int origin)
{
#if defined(_WIN32)
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, off_t(offset), origin);
#endif
}


namespace {

// The key table after the pages: "FVTK", a version, the count, then one entry per material.
constexpr char kKeysMagic[4] = {'F', 'V', 'T', 'K'};
constexpr uint32_t kKeysVersion = 1;  // bump when the same key would encode different pages

struct VtKeyEntry {
    uint64_t key;
    uint32_t firstPage, pageCount;
    uint32_t flags;  // the material's flags after encoding (alpha mask dropped or not)
    uint32_t pad;
};
static_assert(sizeof(VtKeyEntry) == 24);

uint64_t hashBytes(uint64_t h, const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * kMul;
        h ^= h >> 32;
    }
    for (; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// An image's bytes, embedded or in its file (0: unreadable).
uint64_t imageHash(const SrcImage& src)
{
    uint64_t h = 0xcbf29ce484222325ull;
    if (!src.bytes.empty())
        return hashBytes(h, src.bytes.data(), src.bytes.size());
    FILE* f = std::fopen(src.path.c_str(), "rb");
    if (!f)
        return 0;
    std::vector<uint8_t> buf(1 << 20);
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;)
        h = hashBytes(h, buf.data(), n);
    std::fclose(f);
    return h;
}

// Pages from an earlier cook, by material key.
struct ReuseSource {
    FILE* file = nullptr;
    uint64_t dataOffset = 0;
    std::mutex lock;
};
struct ReusedMaterial {
    ReuseSource* source;
    VtKeyEntry entry;
};

bool readKeys(const std::string& path, ReuseSource& source, std::vector<VtKeyEntry>& entries)
{
    source.file = std::fopen(path.c_str(), "rb");
    if (!source.file)
        return false;
    fpak::VtFileHeader header = {};
    char magic[4] = {};
    uint32_t version = 0, count = 0;
    if (std::fread(&header, sizeof(header), 1, source.file) != 1 || std::memcmp(header.magic, "FVT1", 4) ||
        header.pageBytes != fpak::kVtPageBytes ||
        seek64(source.file, int64_t(header.dataOffset + uint64_t(header.pageCount) * header.pageBytes), SEEK_SET) ||
        std::fread(magic, 4, 1, source.file) != 1 || std::memcmp(magic, kKeysMagic, 4) ||
        std::fread(&version, 4, 1, source.file) != 1 || version != kKeysVersion || std::fread(&count, 4, 1, source.file) != 1) {
        std::fclose(source.file);
        source.file = nullptr;
        return false;
    }
    entries.resize(count);
    if (count && std::fread(entries.data(), sizeof(VtKeyEntry), count, source.file) != count) {
        std::fclose(source.file);
        source.file = nullptr;
        return false;
    }
    source.dataOffset = header.dataOffset;
    for (const VtKeyEntry& e : entries)
        if (uint64_t(e.firstPage) + e.pageCount > header.pageCount)
            return false;  // (kept open: closed with the others)
    return true;
}

struct Image {
    std::vector<uint8_t> px;  // RGBA8
    int w = 0, h = 0;
};

uint32_t log2Pow2Ceil(uint32_t v)
{
    uint32_t l = 0;
    while ((1u << l) < v)
        ++l;
    return l;
}

bool imageInfo(const SrcImage& src, int& w, int& h)
{
    int comp = 0;
    if (!src.bytes.empty())
        return stbi_info_from_memory(src.bytes.data(), int(src.bytes.size()), &w, &h, &comp) != 0;
    return !src.path.empty() && stbi_info(src.path.c_str(), &w, &h, &comp) != 0;
}

Image decode(const SrcImage& src)
{
    Image img;
    int comp = 0;
    stbi_uc* p = !src.bytes.empty()
                     ? stbi_load_from_memory(src.bytes.data(), int(src.bytes.size()), &img.w, &img.h, &comp, 4)
                     : stbi_load(src.path.c_str(), &img.w, &img.h, &comp, 4);
    if (!p) {
        std::fprintf(stderr, "falcon cook: cannot decode image %s: %s\n", src.path.c_str(), stbi_failure_reason());
        img.w = img.h = 0;
        return img;
    }
    img.px.assign(p, p + size_t(img.w) * img.h * 4);
    stbi_image_free(p);
    return img;
}

Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < img.px.size(); i += 4) {
        img.px[i] = r;
        img.px[i + 1] = g;
        img.px[i + 2] = b;
        img.px[i + 3] = a;
    }
    return img;
}

// Wrap-around edges match the repeat addressing the shader uses.
// Share of texels whose alpha passes an alpha test at `cutoff` (0-255 units).
float alphaCoverage(const Image& img, float cutoff)
{
    size_t pass = 0;
    for (size_t i = 3; i < img.px.size(); i += 4)
        pass += float(img.px[i]) >= cutoff;
    return float(pass) / float(std::max<size_t>(img.px.size() / 4, 1));
}

// Scales a mip's alpha so `coverage` of its texels pass the alpha test at `cutoff`, as many as in
// mip 0 (Castano, "Computing alpha mipmaps"). Plain averaging thins alpha toward the background
// with every level, so thin cut-outs (grass blades, leaves) vanish with distance. The alpha
// whose upper tail holds that share is found from a histogram, then moved to the cutoff.
void preserveCoverage(Image& img, float cutoff, float coverage)
{
    uint64_t histogram[256] = {};
    for (size_t i = 3; i < img.px.size(); i += 4)
        ++histogram[img.px[i]];
    const uint64_t target = uint64_t(double(coverage) * double(img.px.size() / 4) + 0.5);
    uint64_t above = 0;
    int t = 255;
    for (; t > 0; --t) {
        above += histogram[t];
        if (above >= target)
            break;
    }
    const float scale = std::min(cutoff / float(std::max(t, 1)), 16.0f);
    if (target == 0 || std::fabs(scale - 1.0f) < 1e-3f)
        return;
    for (size_t i = 3; i < img.px.size(); i += 4)
        img.px[i] = uint8_t(std::min(float(img.px[i]) * scale + 0.5f, 255.0f));
}

Image resize(const Image& src, int w, int h, bool srgbWithAlpha)
{
    if (src.w == w && src.h == h)
        return src;
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * h * 4);
    STBIR_RESIZE r;
    stbir_resize_init(&r, src.px.data(), src.w, src.h, 0, out.px.data(), w, h, 0,
                      srgbWithAlpha ? STBIR_RGBA : STBIR_4CHANNEL, srgbWithAlpha ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8);
    stbir_set_edgemodes(&r, STBIR_EDGE_WRAP, STBIR_EDGE_WRAP);
    stbir_resize_extended(&r);
    return out;
}

// One 136x136 page with a wrapped border, then BC-encoded into dst.
void encodePage(const Image& img, uint32_t px, uint32_t py, bool bc5, const bc7enc_compress_block_params& params,
                uint8_t* dst)
{
    constexpr int T = int(fpak::kVtPhysicalTexels), B = int(fpak::kVtBorder), P = int(fpak::kVtPageTexels);
    uint8_t block[16 * 4];
    for (int by = 0; by < T / 4; ++by)
        for (int bx = 0; bx < T / 4; ++bx) {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    int sx = (int(px) * P + bx * 4 + x - B) % img.w;
                    int sy = (int(py) * P + by * 4 + y - B) % img.h;
                    sx += sx < 0 ? img.w : 0;
                    sy += sy < 0 ? img.h : 0;
                    std::memcpy(block + (y * 4 + x) * 4, img.px.data() + (size_t(sy) * img.w + sx) * 4, 4);
                }
            uint8_t* out = dst + size_t(by * (T / 4) + bx) * 16;
            if (bc5)
                rgbcx::encode_bc5(out, block, 0, 1, 4);
            else
                bc7enc_compress_block(out, block, &params);
        }
}

}  // namespace

std::vector<VtLayout> layoutVirtualTextures(const SrcScene& scene, uint32_t maxSize, uint32_t& totalPages)
{
    std::vector<VtLayout> layouts(scene.materials.size());
    totalPages = 0;
    const uint32_t maxLog2 = log2Pow2Ceil(std::max(maxSize, fpak::kVtPageTexels));
    const uint32_t minLog2 = log2Pow2Ceil(fpak::kVtPageTexels);
    for (size_t m = 0; m < scene.materials.size(); ++m) {
        const SrcMaterialTextures& t = scene.materialTextures[m];
        uint32_t w = 0, h = 0;
        for (int img : {t.baseColor, t.normal, t.metallicRoughness, t.occlusion, t.height}) {
            int iw = 0, ih = 0;
            if (img >= 0 && imageInfo(scene.images[img], iw, ih)) {
                w = std::max(w, uint32_t(iw));
                h = std::max(h, uint32_t(ih));
            }
        }
        if (!w || !h)
            continue;
        VtLayout& l = layouts[m];
        l.log2Width = std::clamp(log2Pow2Ceil(w), minLog2, maxLog2);
        l.log2Height = std::clamp(log2Pow2Ceil(h), minLog2, maxLog2);
        l.firstPage = totalPages;
        for (uint32_t mip = 0;; ++mip) {
            const uint32_t pw = fpak::vtLevelPages(l.log2Width, mip), ph = fpak::vtLevelPages(l.log2Height, mip);
            l.pageCount += pw * ph;
            l.mipCount = mip + 1;
            if (pw == 1 && ph == 1)
                break;
        }
        totalPages += l.pageCount;
    }
    return layouts;
}

bool encodeVirtualTextures(SrcScene& scene, const std::vector<VtLayout>& layouts, uint32_t totalPages,
                           uint64_t sourceHash, const std::string& outPath, unsigned threads,
                           const std::vector<std::string>& reuse, VtProgress& progress)
{
    bc7enc_compress_block_init();
    rgbcx::init();

    FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "falcon cook: cannot write %s\n", outPath.c_str());
        return false;
    }
    fpak::VtFileHeader header = {};
    std::memcpy(header.magic, "FVT1", 4);
    header.pageCount = totalPages;
    header.pageBytes = fpak::kVtPageBytes;
    header.dataOffset = fpak::kSectionAlign;
    header.sourceHash = sourceHash;
    std::vector<uint8_t> pad(fpak::kSectionAlign, 0);
    std::memcpy(pad.data(), &header, sizeof(header));
    std::fwrite(pad.data(), 1, pad.size(), f);
    std::mutex fileLock;
    std::atomic<bool> ok{true};

    bc7enc_compress_block_params color, data;
    bc7enc_compress_block_params_init(&color);  // perceptual weights for albedo
    bc7enc_compress_block_params_init(&data);
    bc7enc_compress_block_params_init_linear_weights(&data);  // occlusion/roughness/metallic are data

    std::vector<uint32_t> jobs;
    for (uint32_t m = 0; m < layouts.size(); ++m)
        if (layouts[m].pageCount)
            jobs.push_back(m);
    // Largest first for load balance.
    std::stable_sort(jobs.begin(), jobs.end(), [&](uint32_t a, uint32_t b) { return layouts[a].pageCount > layouts[b].pageCount; });
    progress.total = uint32_t(jobs.size());
    const unsigned workers = std::max(1u, std::min<unsigned>(threads, unsigned(jobs.size())));
    const auto parallel = [&](size_t count, const auto& body) {
        std::atomic<size_t> at{0};
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < workers; ++i)
            pool.emplace_back([&] {
                for (size_t k; (k = at.fetch_add(1)) < count;)
                    body(k);
            });
        for (auto& th : pool)
            th.join();
    };

    // Content keys: the images' bytes (each image hashed once), the layout and what decides
    // the alpha mask.
    std::vector<uint64_t> imageHashes(scene.images.size(), 0);
    std::vector<int> used;
    for (uint32_t m : jobs) {
        const SrcMaterialTextures& t = scene.materialTextures[m];
        for (int img : {t.baseColor, t.normal, t.metallicRoughness, t.occlusion, t.height})
            if (img >= 0 && std::find(used.begin(), used.end(), img) == used.end())
                used.push_back(img);
    }
    parallel(used.size(), [&](size_t k) { imageHashes[used[k]] = imageHash(scene.images[used[k]]); });
    std::vector<uint64_t> keys(layouts.size(), 0);
    for (uint32_t m : jobs) {
        const SrcMaterialTextures& t = scene.materialTextures[m];
        const VtLayout& l = layouts[m];
        const fpak::MaterialGpu& mat = scene.materials[m];
        uint64_t h = hashBytes(0xcbf29ce484222325ull, &kKeysVersion, sizeof(kKeysVersion));
        bool readable = true;
        for (int img : {t.baseColor, t.normal, t.metallicRoughness, t.occlusion}) {
            const uint64_t ih = img >= 0 ? imageHashes[img] : ~0ull;
            readable = readable && ih != 0;
            h = hashBytes(h, &ih, sizeof(ih));
        }
        if (t.height >= 0) {  // (only then: earlier cooks' keys stay as they were)
            const uint64_t ih = imageHashes[t.height];
            readable = readable && ih != 0;
            h = hashBytes(h, &ih, sizeof(ih));
        }
        const uint32_t shape[6] = {l.log2Width, l.log2Height, l.mipCount, l.pageCount, fpak::kVtPageBytes,
                                   mat.flags & (fpak::kMaterialAlphaMask | fpak::kMaterialDoubleSided)};
        const float alpha[2] = {mat.alphaCutoff, mat.baseColor[3]};
        h = hashBytes(h, shape, sizeof(shape));
        h = hashBytes(h, alpha, sizeof(alpha));
        keys[m] = readable && h ? h : 0;
    }

    // Earlier cooks' pages.
    std::vector<std::unique_ptr<ReuseSource>> sources;
    std::unordered_map<uint64_t, ReusedMaterial> reusable;
    for (const std::string& path : reuse) {
        auto source = std::make_unique<ReuseSource>();
        std::vector<VtKeyEntry> entries;
        if (readKeys(path, *source, entries))
            for (const VtKeyEntry& e : entries)
                reusable.emplace(e.key, ReusedMaterial{source.get(), e});
        if (source->file)
            sources.push_back(std::move(source));
    }
    std::vector<uint32_t> finalFlags(layouts.size(), 0);
    std::atomic<size_t> next{0};

    auto work = [&] {
        std::vector<uint8_t> page(fpak::kVtPageBytes);
        for (size_t j; (j = next.fetch_add(1)) < jobs.size();) {
            const uint32_t m = jobs[j];
            const VtLayout& l = layouts[m];
            const SrcMaterialTextures& t = scene.materialTextures[m];
            const int W = 1 << l.log2Width, H = 1 << l.log2Height;
            fpak::MaterialGpu& mat = scene.materials[m];

            // Encoded by an earlier cook: its pages copied, a few at a time.
            auto found = keys[m] ? reusable.find(keys[m]) : reusable.end();
            if (found != reusable.end() && found->second.entry.pageCount == l.pageCount) {
                const ReusedMaterial& r = found->second;
                std::vector<uint8_t> chunk;
                bool copied = true;
                for (uint32_t at = 0; at < l.pageCount && copied;) {
                    const uint32_t n = std::min(64u, l.pageCount - at);
                    chunk.resize(size_t(n) * fpak::kVtPageBytes);
                    {
                        std::lock_guard<std::mutex> lock(r.source->lock);
                        copied = !seek64(r.source->file, int64_t(r.source->dataOffset + uint64_t(r.entry.firstPage + at) * fpak::kVtPageBytes), SEEK_SET) &&
                                 std::fread(chunk.data(), 1, chunk.size(), r.source->file) == chunk.size();
                    }
                    if (!copied)
                        break;
                    std::lock_guard<std::mutex> lock(fileLock);
                    seek64(f, int64_t(header.dataOffset + uint64_t(l.firstPage + at) * fpak::kVtPageBytes), SEEK_SET);
                    if (std::fwrite(chunk.data(), 1, chunk.size(), f) != chunk.size())
                        ok = false;
                    at += n;
                }
                if (copied) {
                    mat.flags = (mat.flags & ~fpak::kMaterialAlphaMask) | (r.entry.flags & fpak::kMaterialAlphaMask);
                    finalFlags[m] = mat.flags;
                    progress.reused.fetch_add(1);
                    progress.done.fetch_add(1);
                    continue;
                }
                // (unreadable: encoded below)
            }

            std::map<int, Image> decoded;  // ORM textures often share one image
            auto source = [&](int index) -> const Image* {
                if (index < 0)
                    return nullptr;
                auto it = decoded.find(index);
                if (it == decoded.end())
                    it = decoded.emplace(index, decode(scene.images[index])).first;
                return it->second.w ? &it->second : nullptr;
            };

            Image albedo = source(t.baseColor) ? resize(*source(t.baseColor), W, H, true) : solid(W, H, 255, 255, 255, 255);
            Image normal = source(t.normal) ? resize(*source(t.normal), W, H, false) : solid(W, H, 128, 128, 255, 255);
            Image orm = solid(W, H, 255, 255, 255, 255);
            if (const Image* occ = source(t.occlusion)) {
                const Image r = resize(*occ, W, H, false);
                for (size_t i = 0; i < orm.px.size(); i += 4)
                    orm.px[i] = r.px[i];
            }
            if (const Image* mr = source(t.metallicRoughness)) {
                const Image r = resize(*mr, W, H, false);
                for (size_t i = 0; i < orm.px.size(); i += 4) {
                    orm.px[i + 1] = r.px[i + 1];  // roughness (glTF: G)
                    orm.px[i + 2] = r.px[i + 2];  // metallic (glTF: B)
                }
            }
            if (const Image* hm = source(t.height)) {  // alpha: the height (its grey), for decals' parallax
                const Image r = resize(*hm, W, H, false);
                for (size_t i = 0; i < orm.px.size(); i += 4)
                    orm.px[i + 3] = r.px[i];
            }
            decoded.clear();

            // "MASK" materials whose alpha never crosses the cutoff are opaque in practice; they
            // keep early depth testing in the visibility pass.
            if ((mat.flags & fpak::kMaterialAlphaMask) && mat.baseColor[3] >= mat.alphaCutoff) {
                const float cutoff = mat.alphaCutoff / std::max(mat.baseColor[3], 1e-6f) * 255.0f;
                bool cutout = false;
                for (size_t i = 3; i < albedo.px.size() && !cutout; i += 4)
                    cutout = float(albedo.px[i]) < cutoff;
                if (!cutout)
                    mat.flags &= ~fpak::kMaterialAlphaMask;
            }
            // Alpha-tested: every mip keeps mip 0's coverage at the cutoff (preserveCoverage).
            const float maskCutoff = mat.alphaCutoff / std::max(mat.baseColor[3], 1e-6f) * 255.0f;
            const bool masked = (mat.flags & fpak::kMaterialAlphaMask) != 0;
            const float maskCoverage = masked ? alphaCoverage(albedo, maskCutoff) : 0.0f;

            uint32_t entry = l.firstPage;
            for (uint32_t mip = 0; mip < l.mipCount; ++mip) {
                const int w = std::max(W >> mip, int(fpak::kVtPageTexels));
                const int h = std::max(H >> mip, int(fpak::kVtPageTexels));
                if (mip > 0) {  // each level from the previous one
                    albedo = resize(albedo, w, h, true);
                    normal = resize(normal, w, h, false);
                    orm = resize(orm, w, h, false);
                }
                const uint32_t pw = fpak::vtLevelPages(l.log2Width, mip), ph = fpak::vtLevelPages(l.log2Height, mip);
                // The chain stays unscaled (each level from the previous one); the written level
                // gets its coverage back.
                Image covered;
                if (masked && mip > 0) {
                    covered = albedo;
                    preserveCoverage(covered, maskCutoff, maskCoverage);
                }
                const Image& albedoOut = masked && mip > 0 ? covered : albedo;
                for (uint32_t py = 0; py < ph; ++py)
                    for (uint32_t px = 0; px < pw; ++px, ++entry) {
                        encodePage(albedoOut, px, py, false, color, page.data());
                        encodePage(normal, px, py, true, color, page.data() + fpak::kVtChannelBytes);
                        encodePage(orm, px, py, false, data, page.data() + 2 * fpak::kVtChannelBytes);
                        std::lock_guard<std::mutex> lock(fileLock);
                        seek64(f, int64_t(header.dataOffset + uint64_t(entry) * fpak::kVtPageBytes), SEEK_SET);
                        if (std::fwrite(page.data(), 1, page.size(), f) != page.size())
                            ok = false;
                    }
            }
            finalFlags[m] = mat.flags;
            progress.done.fetch_add(1);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < workers; ++i)
        pool.emplace_back(work);
    for (auto& th : pool)
        th.join();
    for (auto& s : sources)
        std::fclose(s->file);

    // The keys, after the pages (readers stop at the last page).
    std::vector<VtKeyEntry> entries;
    for (uint32_t m : jobs)
        if (keys[m])
            entries.push_back({keys[m], layouts[m].firstPage, layouts[m].pageCount, finalFlags[m], 0});
    const uint32_t count = uint32_t(entries.size());
    ok = ok && !seek64(f, int64_t(header.dataOffset + uint64_t(totalPages) * fpak::kVtPageBytes), SEEK_SET) &&
         std::fwrite(kKeysMagic, 4, 1, f) == 1 && std::fwrite(&kKeysVersion, 4, 1, f) == 1 && std::fwrite(&count, 4, 1, f) == 1 &&
         (!count || std::fwrite(entries.data(), sizeof(VtKeyEntry), count, f) == count);
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace fx::cook
