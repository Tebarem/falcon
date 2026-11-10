#include "render/terrain_surface.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/device.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "terrain/terrain.h"

#include "terrain_surface.shader.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fx {

using namespace terrain;

namespace {

constexpr uint32_t kLayerStride = 80, kLayersOffset = 16;
constexpr uint32_t kPageTableOffset = kLayersOffset + kMaxLayers * kLayerStride;
constexpr uint32_t kAtlasUploadsPerFrame = 12;

struct SurfaceUniforms {
    float4 cameraPos, cameraRight, cameraUp, cameraForward, jitter;
    uint32_t screen[4];
    float4 grid, heights;
    uint32_t slots[4];
};

struct LayerGpu {
    float a[4], b[4], c[4], d[4], mean[4];
};
static_assert(sizeof(LayerGpu) == kLayerStride);

const SDL_GPUTextureFormat kLayerFormats[3] = {SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB, SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM,
                                               SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM};

uint32_t mipCount(uint32_t res)
{
    uint32_t m = 0;
    for (uint32_t r = res; r >= 4; r /= 2)
        ++m;
    return m;
}

SDL_GPUTexture* makeTexture(SDL_GPUDevice* device, SDL_GPUTextureType type, SDL_GPUTextureFormat format, uint32_t w, uint32_t h,
                            uint32_t layers, uint32_t mips, SDL_GPUTextureUsageFlags usage, const char* name)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = type;
    info.format = format;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = layers;
    info.num_levels = mips;
    SDL_GPUTexture* t = gpumem::createTexture("Terrain", device, &info);
    if (t)
        gpumem::setTextureName(device, t, name);
    else
        FX_ERROR("terrain: %s (%ux%u x%u): %s", name, w, h, layers, SDL_GetError());
    return t;
}

}  // namespace

uint32_t TerrainSurface::pageTableOffset() { return kPageTableOffset; }

bool TerrainSurface::init(const GpuDevice& gpu)
{
    device_ = gpu.get();
    pass_ = gpu.createComputePipeline(shaders::terrain_surface_cs_surface);
    SDL_GPUSamplerCreateInfo s = {};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linearClamp_ = SDL_CreateGPUSampler(device_, &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    pointClamp_ = SDL_CreateGPUSampler(device_, &s);
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    s.enable_anisotropy = true;
    s.max_anisotropy = 8.0f;
    s.max_lod = 32.0f;
    layerSampler_ = SDL_CreateGPUSampler(device_, &s);
    output_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT, 1, 1, 1, 1,
                          SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ, "terrain surface");
    outputW_ = outputH_ = 1;
    dataBytes_ = kPageTableOffset + 4;
    SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, dataBytes_, 0};
    data_ = gpumem::createBuffer("Terrain", device_, &b);
    gpumem::setBufferName(device_, data_, "terrain layers + page table");
    return pass_ && linearClamp_ && pointClamp_ && layerSampler_ && output_ && data_;
}

void TerrainSurface::releaseTerrainTextures()
{
    for (SDL_GPUTexture** t : {&tileHeights_, &tileMaterials_, &coarseHeights_, &coarseMaterials_}) {
        gpumem::releaseTexture(device_, *t);
        *t = nullptr;
    }
}

void TerrainSurface::shutdown()
{
    builder_.stop();
    if (!device_)
        return;
    releaseTerrainTextures();
    for (SDL_GPUTexture*& t : layerTex_) {
        gpumem::releaseTexture(device_, t);
        t = nullptr;
    }
    gpumem::releaseTexture(device_, output_);
    gpumem::releaseBuffer(device_, data_);
    gpumem::releaseTransferBuffer(device_, upload_);
    for (SDL_GPUSampler* s : {linearClamp_, pointClamp_, layerSampler_})
        SDL_ReleaseGPUSampler(device_, s);
    SDL_ReleaseGPUComputePipeline(device_, pass_);
    device_ = nullptr;
}

void TerrainSurface::setTerrain(const Terrain* terrain, const std::string& scenePath)
{
    releaseTerrainTextures();
    terrain_ = terrain && terrain->valid() ? terrain : nullptr;
    scenePath_ = scenePath;
    const std::filesystem::path folder = std::filesystem::path(scenePath).parent_path();
    sceneFolder_ = folder.empty() ? std::string(".") : folder.string();
    layersJson_ = "\x01";  // never a valid list: parsed again
    active_ = false;
    if (!terrain_)
        return;
    const uint32_t T = terrain_->tilesPerSide();
    coarseSide_ = terrain_->coarseSide();
    coarseHeights_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R16_UNORM, coarseSide_, coarseSide_, 1, 1,
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER, "terrain coarse heights");
    coarseMaterials_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, coarseSide_, coarseSide_,
                                   1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER, "terrain coarse materials");
    tileHeights_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R16_UNORM, kTileSamples, kTileSamples,
                               kAtlasTiles, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER, "terrain tile heights");
    tileMaterials_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kTileSamples,
                                 kTileSamples, kAtlasTiles, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER, "terrain tile materials");
    atlasOfTile_.assign(size_t(T) * T, -1);
    tileOfAtlas_.assign(kAtlasTiles, -1);
    atlasStale_.assign(kAtlasTiles, 0);
    pageTable_.assign(size_t(T) * T, 0);
    tileRevision_.assign(size_t(T) * T, 1);
    dataBytes_ = std::max(dataBytes_, kPageTableOffset + T * T * 4);
    gpumem::releaseBuffer(device_, data_);
    SDL_GPUBufferCreateInfo b = {SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, dataBytes_, 0};
    data_ = gpumem::createBuffer("Terrain", device_, &b);
    gpumem::setBufferName(device_, data_, "terrain layers + page table");
    fullCoarse_ = true;
    dataDirty_ = true;
    terrainRevision_ = terrain_->revision();
    if (!layerTex_[0])
        ensureLayerArrays(4, mipCount(kLayerResolution));
}

bool TerrainSurface::ensureLayerArrays(uint32_t capacity, uint32_t mips)
{
    if (capacity <= layerCapacity_ && mips == layerMips_ && layerTex_[0])
        return true;
    capacity = std::clamp((std::max(capacity, layerCapacity_) + 3) / 4 * 4, 4u, kMaxLayers);
    for (SDL_GPUTexture*& t : layerTex_) {
        gpumem::releaseTexture(device_, t);
        t = nullptr;
    }
    static const char* const kNames[3] = {"terrain layer albedo", "terrain layer normal", "terrain layer orm"};
    for (uint32_t c = 0; c < 3; ++c) {
        layerTex_[c] = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D_ARRAY, kLayerFormats[c], kLayerResolution, kLayerResolution,
                                   capacity, mips, SDL_GPU_TEXTUREUSAGE_SAMPLER, kNames[c]);
        if (!layerTex_[c])
            return false;
    }
    layerCapacity_ = capacity;
    layerMips_ = mips;
    // What the old arrays held is gone: built again (from the cache).
    const std::string cacheDir = Terrain::dirFor(scenePath_) + "/layers";
    for (uint32_t i = 0; i < layers_.size(); ++i)
        if (loaded_[i]) {
            loaded_[i] = 0;
            builder_.request(i, layers_[i], sceneFolder_, cacheDir, kLayerResolution);
        }
    dataDirty_ = true;
    return true;
}

bool TerrainSurface::coarseLayout(float4& grid, float4& heights) const
{
    if (!terrain_ || !terrain_->valid() || !coarseHeights_ || !coarseMaterials_ || !data_)
        return false;
    const TerrainSettings& s = terrain_->settings();
    uint32_t loaded = 0;
    for (const uint64_t k : loaded_)
        loaded += k != 0;
    const float spacing = s.spacing * float(terrain::kCoarseStep);
    grid = {s.originX, s.originZ, spacing, 1.0f / spacing};
    heights = {s.heightMin, s.heightMax - s.heightMin, float(coarseSide_), loaded ? float(layers_.size()) : 0.0f};
    return true;
}

bool TerrainSurface::layerMean(uint32_t layer, float3& mean) const
{
    if (layer >= layers_.size() || !loaded_[layer])
        return false;
    mean = means_[layer];
    return true;
}

void TerrainSurface::writeData()
{
    dataCpu_.assign(dataBytes_, 0);
    const uint32_t header[4] = {uint32_t(layers_.size()), terrain_ ? terrain_->tilesPerSide() : 0, kPageTableOffset, 0};
    std::memcpy(dataCpu_.data(), header, sizeof(header));
    for (uint32_t i = 0; i < layers_.size() && i < kMaxLayers; ++i) {
        const TerrainLayer& l = layers_[i];
        const LayerGpu g = {{1.0f / l.tileSize, l.rotation, l.contrast, l.heightBlend},
                            {l.macro, 1.0f / l.macroScale, float(l.projection), l.normalStrength},
                            {l.tint[0], l.tint[1], l.tint[2], l.roughness},
                            {l.farScale, l.hex ? 1.0f : 0.0f, loaded_[i] ? 1.0f : 0.0f, 0.0f},
                            {means_[i].x, means_[i].y, means_[i].z, 0.0f}};
        std::memcpy(dataCpu_.data() + kLayersOffset + i * kLayerStride, &g, sizeof(g));
    }
    std::memcpy(dataCpu_.data() + kPageTableOffset, pageTable_.data(), pageTable_.size() * 4);
}

void TerrainSurface::update(SDL_GPUCommandBuffer* cmd, float3 focus)
{
    FX_ZONE_CAT("TerrainSurface::update", Rendering);
    if (!terrain_ || !terrain_->valid() || !device_)
        return;
    staging_.clear();
    writes_.clear();
    auto add = [&](SDL_GPUTexture* t, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void* data, size_t bytes) {
        // (D3D12 places texture copies at 512-byte offsets.)
        staging_.resize((staging_.size() + 511) / 512 * 512);
        writes_.push_back({t, mip, layer, x, y, w, h, uint32_t(staging_.size())});
        const uint8_t* b = static_cast<const uint8_t*>(data);
        staging_.insert(staging_.end(), b, b + bytes);
    };
    const std::string cacheDir = Terrain::dirFor(scenePath_) + "/layers";

    // Layers: what changed in the list, and textures that finished building.
    if (terrain_->layersJson() != layersJson_) {
        layersJson_ = terrain_->layersJson();
        layers_ = parseTerrainLayers(layersJson_);
        requested_.resize(layers_.size(), 0);
        loaded_.resize(layers_.size(), 0);
        means_.resize(layers_.size(), {0.5f, 0.5f, 0.5f});
        for (uint32_t i = 0; i < layers_.size(); ++i) {
            const uint64_t key = terrainLayerKey(layers_[i], sceneFolder_, kLayerResolution);
            if (key != requested_[i]) {
                requested_[i] = key;
                if (loaded_[i] != key)
                    builder_.request(i, layers_[i], sceneFolder_, cacheDir, kLayerResolution);
            }
        }
        dataDirty_ = true;
        ++meanRevision_;
    }
    uint32_t index;
    uint64_t key;
    TerrainLayerTextures tex;
    while (builder_.poll(index, key, tex)) {
        if (index >= layers_.size() || key != requested_[index] || tex.resolution != kLayerResolution)
            continue;
        if (!ensureLayerArrays(index + 1, tex.mips))
            continue;
        for (uint32_t c = 0; c < 3; ++c) {
            size_t offset = 0;
            for (uint32_t m = 0; m < tex.mips; ++m) {
                const uint32_t r = kLayerResolution >> m;
                add(layerTex_[c], m, index, 0, 0, r, r, tex.channel[c].data() + offset, size_t(r) * r);
                offset += size_t(r) * r;
            }
        }
        loaded_[index] = key;
        means_[index] = {tex.meanAlbedo[0], tex.meanAlbedo[1], tex.meanAlbedo[2]};
        dataDirty_ = true;
        ++meanRevision_;
    }

    // Coarse maps: all of them at first, then the rectangles that changed.
    const uint32_t T = terrain_->tilesPerSide();
    std::vector<TerrainChange> changes;
    const bool all = !terrain_->changesSince(terrainRevision_, changes);
    terrainRevision_ = terrain_->revision();
    std::vector<uint8_t> rows;
    auto coarseRect = [&](uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1) {
        const uint32_t w = x1 - x0 + 1, h = z1 - z0 + 1;
        rows.resize(size_t(w) * h * 4);
        for (uint32_t z = 0; z < h; ++z)
            std::memcpy(&rows[size_t(z) * w * 2], &terrain_->coarseHeights()[size_t(z0 + z) * coarseSide_ + x0], w * 2);
        add(coarseHeights_, 0, 0, x0, z0, w, h, rows.data(), size_t(w) * h * 2);
        for (uint32_t z = 0; z < h; ++z)
            std::memcpy(&rows[size_t(z) * w * 4], &terrain_->coarseMaterials()[size_t(z0 + z) * coarseSide_ + x0], w * 4);
        add(coarseMaterials_, 0, 0, x0, z0, w, h, rows.data(), size_t(w) * h * 4);
    };
    if (fullCoarse_ || all) {
        coarseRect(0, 0, coarseSide_ - 1, coarseSide_ - 1);
        fullCoarse_ = false;
        std::fill(atlasStale_.begin(), atlasStale_.end(), uint8_t(1));
    } else {
        for (const TerrainChange& c : changes) {
            const TerrainRect r = c.rect;
            coarseRect(uint32_t(std::max(r.x0, 0)) / kCoarseStep, uint32_t(std::max(r.z0, 0)) / kCoarseStep,
                       std::min((uint32_t(r.x1) + kCoarseStep - 1) / kCoarseStep, coarseSide_ - 1),
                       std::min((uint32_t(r.z1) + kCoarseStep - 1) / kCoarseStep, coarseSide_ - 1));
            for (uint32_t tz = uint32_t(std::max(r.z0 - 1, 0)) / kTileQuads; tz <= std::min(uint32_t(r.z1) / kTileQuads, T - 1); ++tz)
                for (uint32_t tx = uint32_t(std::max(r.x0 - 1, 0)) / kTileQuads; tx <= std::min(uint32_t(r.x1) / kTileQuads, T - 1); ++tx)
                    if (atlasOfTile_[tz * T + tx] >= 0)
                        atlasStale_[uint32_t(atlasOfTile_[tz * T + tx])] = 1;
        }
    }

    // Atlas: the resident tiles nearest the focus.
    const float tileSize = float(kTileQuads) * terrain_->settings().spacing;
    std::vector<std::pair<float, uint32_t>> resident;
    for (uint32_t tz = 0; tz < T; ++tz)
        for (uint32_t tx = 0; tx < T; ++tx)
            if (terrain_->tileHeights(tx, tz)) {
                const float cx = terrain_->settings().originX + (float(tx) + 0.5f) * tileSize - focus.x;
                const float cz = terrain_->settings().originZ + (float(tz) + 0.5f) * tileSize - focus.z;
                resident.push_back({cx * cx + cz * cz, tz * T + tx});
            }
    std::sort(resident.begin(), resident.end());
    if (resident.size() > kAtlasTiles)
        resident.resize(kAtlasTiles);
    std::vector<uint8_t> wanted(size_t(T) * T, 0);
    for (const auto& r : resident)
        wanted[r.second] = 1;
    std::vector<uint32_t> freeLayers;
    for (uint32_t a = 0; a < kAtlasTiles; ++a) {
        const int32_t tile = tileOfAtlas_[a];
        if (tile >= 0 && !wanted[uint32_t(tile)]) {
            atlasOfTile_[uint32_t(tile)] = -1;
            pageTable_[uint32_t(tile)] = 0;
            ++tileRevision_[uint32_t(tile)];
            tileOfAtlas_[a] = -1;
            dataDirty_ = true;
        }
        if (tileOfAtlas_[a] < 0)
            freeLayers.push_back(a);
    }
    for (const auto& r : resident)
        if (atlasOfTile_[r.second] < 0 && !freeLayers.empty()) {
            const uint32_t a = freeLayers.back();
            freeLayers.pop_back();
            atlasOfTile_[r.second] = int32_t(a);
            tileOfAtlas_[a] = int32_t(r.second);
            atlasStale_[a] = 1;
        }
    uint32_t uploads = 0;
    for (const auto& r : resident) {  // nearest first
        const int32_t a = atlasOfTile_[r.second];
        if (a < 0 || !atlasStale_[uint32_t(a)] || uploads >= kAtlasUploadsPerFrame)
            continue;
        const uint32_t tx = r.second % T, tz = r.second / T;
        add(tileHeights_, 0, uint32_t(a), 0, 0, kTileSamples, kTileSamples, terrain_->tileHeights(tx, tz), kTileSampleCount * 2);
        add(tileMaterials_, 0, uint32_t(a), 0, 0, kTileSamples, kTileSamples, terrain_->tileMaterials(tx, tz), kTileSampleCount * 4);
        atlasStale_[uint32_t(a)] = 0;
        if (pageTable_[r.second] != uint32_t(a) + 1) {
            pageTable_[r.second] = uint32_t(a) + 1;
            dataDirty_ = true;
            ++tileRevision_[r.second];
        }
        ++uploads;
    }

    if (dataDirty_)
        writeData();
    if (writes_.empty() && !dataDirty_)
        return;
    const size_t dataAt = (staging_.size() + 15) / 16 * 16;
    if (dataDirty_) {
        staging_.resize(dataAt);
        staging_.insert(staging_.end(), dataCpu_.begin(), dataCpu_.end());
    }
    if (staging_.size() > uploadBytes_) {
        gpumem::releaseTransferBuffer(device_, upload_);
        uploadBytes_ = uint32_t(std::max<size_t>(staging_.size() + staging_.size() / 2, 1u << 20));
        const SDL_GPUTransferBufferCreateInfo info = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadBytes_, 0};
        upload_ = gpumem::createTransferBuffer("Terrain", device_, &info, "terrain texture upload");
        if (!upload_) {
            uploadBytes_ = 0;
            return;
        }
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, upload_, true);
    std::memcpy(mapped, staging_.data(), staging_.size());
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    FX_GPU_PASS(cmd, "terrain texture upload");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const TextureWrite& w : writes_) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = upload_;
        src.offset = w.src;
        src.pixels_per_row = w.w;
        src.rows_per_layer = w.h;
        SDL_GPUTextureRegion dst = {};
        dst.texture = w.texture;
        dst.mip_level = w.mip;
        dst.layer = w.layer;
        dst.x = w.x;
        dst.y = w.y;
        dst.w = w.w;
        dst.h = w.h;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
    }
    if (dataDirty_) {
        const SDL_GPUTransferBufferLocation src = {upload_, uint32_t(dataAt)};
        const SDL_GPUBufferRegion dst = {data_, 0, uint32_t(dataCpu_.size())};
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        dataDirty_ = false;
    }
    SDL_EndGPUCopyPass(copy);
}

void TerrainSurface::shade(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* depth, SDL_GPUTexture* visBuffer, SDL_GPUBuffer* visible,
                           SDL_GPUSampler* pointSampler, uint32_t width, uint32_t height, const CameraState& camera,
                           float2 jitter, uint32_t firstInstance, uint32_t instanceCount, SDL_GPUBuffer* scene)
{
    active_ = terrain_ && terrain_->valid() && pass_ && coarseHeights_ && tileHeights_ && layerTex_[0];
    if (!active_)
        return;
    FX_ZONE_CAT("TerrainSurface::shade", Rendering);
    if (outputW_ != width || outputH_ != height) {
        gpumem::releaseTexture(device_, output_);
        output_ = makeTexture(device_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT, width, height, 1, 1,
                              SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ, "terrain surface");
        outputW_ = width;
        outputH_ = height;
        if (!output_) {
            active_ = false;
            return;
        }
    }
    const TerrainSettings& s = terrain_->settings();
    SurfaceUniforms u = {};
    u.cameraPos = {camera.position.x, camera.position.y, camera.position.z, camera.zNear};
    u.cameraRight = {camera.right.x, camera.right.y, camera.right.z, 1.0f / camera.p00};
    u.cameraUp = {camera.up.x, camera.up.y, camera.up.z, 1.0f / camera.p11};
    u.cameraForward = {camera.forward.x, camera.forward.y, camera.forward.z, 0.0f};
    u.jitter = {jitter.x, jitter.y, 0.0f, 0.0f};
    uint32_t loaded = 0;
    for (const uint64_t k : loaded_)
        loaded += k != 0;
    u.screen[0] = width;
    u.screen[1] = height;
    u.screen[3] = loaded ? uint32_t(layers_.size()) : 0u;  // nothing to show yet: the default look
    u.grid = {s.originX, s.originZ, s.spacing, 1.0f / s.spacing};
    u.heights = {s.heightMin, s.heightMax - s.heightMin, float(coarseSide_), float(s.tiles)};
    u.slots[0] = firstInstance;
    u.slots[1] = instanceCount;
    u.slots[2] = kPageTableOffset;
    SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof(u));
    FX_GPU_SECTION(cmd, "Terrain surface");
    FX_GPU_PASS(cmd, "terrain surface");
    const SDL_GPUStorageTextureReadWriteBinding out = {output_, 0, 0, false};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, &out, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, pass_);
    const SDL_GPUTextureSamplerBinding samplers[8] = {
        {depth, pointSampler},         {coarseHeights_, linearClamp_}, {tileHeights_, linearClamp_},
        {coarseMaterials_, pointClamp_}, {tileMaterials_, pointClamp_},  {layerTex_[0], layerSampler_},
        {layerTex_[1], layerSampler_}, {layerTex_[2], layerSampler_}};
    SDL_BindGPUComputeSamplers(pass, 0, samplers, 8);
    SDL_BindGPUComputeStorageTextures(pass, 0, &visBuffer, 1);
    SDL_GPUBuffer* buffers[3] = {visible, data_, scene};
    SDL_BindGPUComputeStorageBuffers(pass, 0, buffers, 3);
    SDL_DispatchGPUCompute(pass, (width + 7) / 8, (height + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
}

}  // namespace fx
