#include "render/decal_renderer.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "scene/fly_camera.h"
#include "vfx/vfx_components.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

SDL_GPUTexture* storageTexture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, uint32_t w, uint32_t h, const char* name)
{
    SDL_GPUTextureCreateInfo t = {};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = format;
    t.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    t.width = std::max(w, 1u);
    t.height = std::max(h, 1u);
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    SDL_GPUTexture* tex = gpumem::createTexture("Decals", device, &t);
    if (tex)
        gpumem::setTextureName(device, tex, name);
    return tex;
}

void inverse(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
    const float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                           {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                           {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
}

}  // namespace

bool DecalRenderer::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    data_ = storageTexture(device_, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, kRows, kMaxVisible, "decals");
    return data_ && ensureBins(1, 1);
}

void DecalRenderer::shutdown()
{
    if (!device_)
        return;
    gpumem::releaseTexture(device_, data_);
    gpumem::releaseTexture(device_, bins_);
    gpumem::releaseTransferBuffer(device_, upload_);
    data_ = bins_ = nullptr;
    upload_ = nullptr;
    device_ = nullptr;
}

bool DecalRenderer::ensureBins(uint32_t width, uint32_t height)
{
    const uint32_t tx = (width + kTile - 1) / kTile, ty = (height + kTile - 1) / kTile;
    if (bins_ && tx == tilesX_ && ty == tilesY_)
        return true;
    gpumem::releaseTexture(device_, bins_);
    tilesX_ = tx;
    tilesY_ = ty;
    bins_ = storageTexture(device_, SDL_GPU_TEXTUREFORMAT_R32_UINT, tilesX_ * (kMaxVisible / 32), tilesY_, "decal tiles");
    cleared_ = false;
    return bins_ != nullptr;
}

void DecalRenderer::prepare(SDL_GPUCommandBuffer* cmd, const CameraState& camera, uint32_t width, uint32_t height)
{
    FX_ZONE_CAT("DecalRenderer::prepare", Rendering);
    const uint32_t before = count_;
    count_ = 0;
    if (!data_ || !ensureBins(width, height))
        return;
    all_.clear();
    if (decals_ && scene_)
        decals_->gather(*scene_, all_);
    // Visible: a box with a corner in front of the camera, its corners' screen rectangle on screen.
    struct Seen {
        uint32_t index;
        float distance;
        uint32_t x0, y0, x1, y1;  // tiles
    };
    std::vector<Seen> seen;
    for (uint32_t i = 0; i < all_.size(); ++i) {
        const DecalInstance& d = all_[i];
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        bool behind = false, front = false;
        for (int k = 0; k < 8; ++k) {
            const float sx = (k & 1) ? 0.5f : -0.5f, sy = (k & 2) ? 0.5f : -0.5f, sz = (k & 4) ? 0.5f : -0.5f;
            float p[3];
            for (int r = 0; r < 3; ++r)
                p[r] = d.world[r][0] * sx + d.world[r][1] * sy + d.world[r][2] * sz + d.world[r][3];
            const float4x4& m = camera.viewProj;
            float clip[4];
            for (int r = 0; r < 4; ++r)
                clip[r] = m.m[0][r] * p[0] + m.m[1][r] * p[1] + m.m[2][r] * p[2] + m.m[3][r];
            if (clip[3] <= 1e-3f) {
                behind = true;
                continue;
            }
            front = true;
            const float nx = clip[0] / clip[3], ny = clip[1] / clip[3];
            minX = std::min(minX, nx), maxX = std::max(maxX, nx);
            minY = std::min(minY, ny), maxY = std::max(maxY, ny);
        }
        if (!front)
            continue;
        if (behind)  // (it reaches behind the camera: anywhere on screen)
            minX = minY = -1.0f, maxX = maxY = 1.0f;
        if (maxX < -1.0f || minX > 1.0f || maxY < -1.0f || minY > 1.0f)
            continue;
        const auto tileX = [&](float ndc) { return uint32_t(std::clamp((ndc * 0.5f + 0.5f) * float(width), 0.0f, float(width - 1))) / kTile; };
        const auto tileY = [&](float ndc) { return uint32_t(std::clamp((0.5f - ndc * 0.5f) * float(height), 0.0f, float(height - 1))) / kTile; };
        const float3 centre = {d.world[0][3], d.world[1][3], d.world[2][3]};
        seen.push_back({i, length(centre - camera.position), tileX(minX), tileY(maxY), tileX(maxX), tileY(minY)});
    }
    // The nearest when there are too many, then in their order (higher on top; newer placed ones
    // over older): the bitmask's order is the resolve's.
    if (seen.size() > kMaxVisible) {
        std::nth_element(seen.begin(), seen.begin() + kMaxVisible, seen.end(), [](const Seen& a, const Seen& b) { return a.distance < b.distance; });
        seen.resize(kMaxVisible);
    }
    std::stable_sort(seen.begin(), seen.end(), [&](const Seen& a, const Seen& b) {
        const DecalInstance &x = all_[a.index], &y = all_[b.index];
        return x.order != y.order ? x.order < y.order : x.sequence < y.sequence;
    });
    count_ = uint32_t(seen.size());
    if (count_ == 0 && before == 0 && cleared_)
        return;  // (nothing then, nothing now: the textures stay as they are, unread)

    // Rows: the box's inverse (world -> unit box), its axes and sizes, tint, fades, then the
    // material, flags and parallax steps (as floats: exact for these).
    words_ = std::max(1u, (count_ + 31) / 32);
    materials_.clear();
    rows_.assign(size_t(std::max(count_, 1u)) * kRows * 4, 0.0f);
    for (uint32_t i = 0; i < count_; ++i) {
        const DecalInstance& d = all_[seen[i].index];
        float* r = &rows_[size_t(i) * kRows * 4];
        float inv[3][4];
        inverse(d.world, inv);
        std::memcpy(r, inv, sizeof(inv));
        for (int axis = 0; axis < 3; ++axis) {
            const float3 col = {d.world[0][axis], d.world[1][axis], d.world[2][axis]};
            const float len = length(col);
            const float3 unit = len > 1e-8f ? col * (1.0f / len) : float3{0, 0, 0};
            r[12 + axis * 4 + 0] = unit.x, r[12 + axis * 4 + 1] = unit.y, r[12 + axis * 4 + 2] = unit.z, r[12 + axis * 4 + 3] = len;
        }
        std::memcpy(r + 24, d.tint, sizeof(d.tint));
        r[28] = d.angleFade, r[29] = d.edgeFade, r[30] = d.cavity, r[31] = d.parallaxDepth;
        int32_t material = -1;
        if (auto it = materials_.find(d.material); it != materials_.end()) {
            material = it->second;
        } else {
            const std::string& path = vfxAssetPath(d.material);
            material = path.empty() || !resolve_ ? -1 : resolve_(path);
            materials_[d.material] = material;
        }
        r[32] = float(material);
        r[33] = float(d.flags);
        r[34] = float(std::clamp(d.steps, 1u, 64u));
    }
    // Tiles: bit i of a tile's words when decal i may cover it.
    binWords_.assign(size_t(tilesX_) * words_ * tilesY_, 0u);
    for (uint32_t i = 0; i < count_; ++i)
        for (uint32_t ty = seen[i].y0; ty <= seen[i].y1; ++ty)
            for (uint32_t tx = seen[i].x0; tx <= seen[i].x1; ++tx)
                binWords_[(size_t(ty) * tilesX_ + tx) * words_ + i / 32] |= 1u << (i % 32);

    const uint32_t dataBytes = uint32_t(rows_.size() * 4), binBytes = uint32_t(binWords_.size() * 4);
    if (dataBytes + binBytes > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = (dataBytes + binBytes) * 3 / 2;
        const SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Decals", device_, &ti, "decal upload");
        if (!upload_) {
            uploadBytes_ = 0;
            count_ = 0;
            return;
        }
    }
    uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    std::memcpy(mapped, rows_.data(), dataBytes);
    std::memcpy(mapped + dataBytes, binWords_.data(), binBytes);
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "decal upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {upload_, 0, kRows, std::max(count_, 1u)};
    SDL_GPUTextureRegion dst = {};
    dst.texture = data_;
    dst.w = kRows;
    dst.h = std::max(count_, 1u);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    src = {upload_, dataBytes, tilesX_ * words_, tilesY_};
    dst = {};
    dst.texture = bins_;
    dst.w = tilesX_ * words_;
    dst.h = tilesY_;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    cleared_ = true;
}

}  // namespace fx
