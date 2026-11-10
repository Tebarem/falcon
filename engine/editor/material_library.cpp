#include "editor/material_library.h"
#include "project/project_shaders.h"

#include <algorithm>

#include "assets/scene_pak.h"
#include "core/log.h"
#include "project/asset_library.h"
#include "render/virtual_texture.h"

#include <cmath>
#include <filesystem>

#include <stb_image.h>

namespace fx {

namespace {
const float kNoParams[8] = {};  // (a material whose params were never written)
}  // namespace

void MaterialLibrary::init(ScenePak& pak, const std::string& projectDir, uint32_t spare, uint32_t runtime)
{
    pak_ = &pak;
    projectDir_ = projectDir;
    if (!projectDir.empty())
        findSurfaces();
    for (const auto& [source, index] : pak.materialSources()) {
        index_[source] = index;
        load(source, index);
    }
    spareCount_ = spare;
    spareFirst_ = spare ? pak.addMaterials(spare) : 0;
    runtimeCount_ = runtime;
    runtimeFirst_ = runtime ? pak.addMaterials(runtime) : 0;
}

void MaterialLibrary::findSurfaces()
{
    surfaces_ = ProjectShaders::surfaceShaders(projectDir_);
    vertexShaders_.clear();
    for (const std::string& s : surfaces_)
        if (ProjectShaders::hasVertexFunction(projectDir_, s))
            vertexShaders_.push_back(s);
}

const float* MaterialLibrary::engineParams(uint32_t index) const
{
    const float* p = params(index);
    return p ? p + 4 : nullptr;
}

void MaterialLibrary::write(uint32_t index, const fpak::MaterialGpu& m)
{
    if (!pak_->setMaterial(index, m))
        return;
    if (std::find(dirty_.begin(), dirty_.end(), index) == dirty_.end())
        dirty_.push_back(index);
}

void MaterialLibrary::flush(SDL_GPUCommandBuffer* cmd)
{
    if (dirty_.empty() || !device_ || !pak_->sceneBuffer())
        return;
    const uint32_t stride = uint32_t(sizeof(fpak::MaterialGpu)), bytes = uint32_t(dirty_.size()) * stride;
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!tb)
        return;
    auto* mapped = static_cast<fpak::MaterialGpu*>(SDL_MapGPUTransferBuffer(device_, tb, false));
    for (size_t i = 0; i < dirty_.size(); ++i)
        mapped[i] = pak_->materials()[dirty_[i]];
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    const uint32_t sceneMaterials = pak_->gpuSectionOffset(fpak::Section::Materials);
    for (size_t i = 0; i < dirty_.size(); ++i) {
        const SDL_GPUTransferBufferLocation src = {tb, uint32_t(i) * stride};
        const SDL_GPUBufferRegion scene = {pak_->sceneBuffer(), sceneMaterials + dirty_[i] * stride, stride};
        SDL_UploadToGPUBuffer(copy, &src, &scene, false);
        if (vt_ && vt_->materialData()) {
            const SDL_GPUBufferRegion shading = {vt_->materialData(), dirty_[i] * stride, stride};
            SDL_UploadToGPUBuffer(copy, &src, &shading, false);
        }
    }
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);  // (released once the frame is done with it)
    // (R4) Their params, after the VT's page table.
    if (vt_ && vt_->materialData() && !params_.empty()) {
        constexpr uint32_t bytes = kParamFloats * 4;
        SDL_GPUTransferBufferCreateInfo pi = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uint32_t(dirty_.size()) * bytes, 0};
        if (SDL_GPUTransferBuffer* pb = SDL_CreateGPUTransferBuffer(device_, &pi)) {
            auto* out = static_cast<float*>(SDL_MapGPUTransferBuffer(device_, pb, false));
            for (size_t i = 0; i < dirty_.size(); ++i) {
                const float* p = params(dirty_[i]);
                std::copy(p, p + kParamFloats, out + i * kParamFloats);
            }
            SDL_UnmapGPUTransferBuffer(device_, pb);
            SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
            for (size_t i = 0; i < dirty_.size(); ++i) {
                const SDL_GPUTransferBufferLocation src = {pb, uint32_t(i) * bytes};
                const SDL_GPUBufferRegion dst = {vt_->materialData(), vt_->paramsOffset() + dirty_[i] * bytes, bytes};
                SDL_UploadToGPUBuffer(pass, &src, &dst, false);
            }
            SDL_EndGPUCopyPass(pass);
            SDL_ReleaseGPUTransferBuffer(device_, pb);
        }
    }
    dirty_.clear();
}

void MaterialLibrary::writeParams(uint32_t index, const float values[4], uint32_t first)
{
    if (params_.size() < pak_->materials().size() * kParamFloats)
        params_.resize(pak_->materials().size() * kParamFloats, 0.0f);
    if ((index + 1) * kParamFloats > params_.size())
        return;
    std::copy(values, values + 4, params_.begin() + index * kParamFloats + first);
    if (std::find(dirty_.begin(), dirty_.end(), index) == dirty_.end())
        dirty_.push_back(index);
}

const float* MaterialLibrary::params(uint32_t index) const
{
    return (index + 1) * kParamFloats <= params_.size() ? params_.data() + index * kParamFloats : (gpu(index) ? kNoParams : nullptr);
}

bool MaterialLibrary::setParams(uint32_t index, const float values[4])
{
    const float* now = params(index);
    if (!now)
        return false;
    paramsBefore_.try_emplace(index, std::array<float, 4>{now[0], now[1], now[2], now[3]});
    writeParams(index, values);
    return true;
}

void MaterialLibrary::reloadAll()
{
    if (!projectDir_.empty())
        findSurfaces();
    for (const auto& [source, index] : index_)
        load(source, index);
}

const fpak::MaterialGpu* MaterialLibrary::gpu(uint32_t index) const
{
    return pak_ && index < pak_->materials().size() ? &pak_->materials()[index] : nullptr;
}

bool MaterialLibrary::set(uint32_t index, const fpak::MaterialGpu& m)
{
    if (!gpu(index))
        return false;
    scriptedBefore_.try_emplace(index, *gpu(index));
    write(index, m);
    return true;
}

int32_t MaterialLibrary::clone(uint32_t index)
{
    if (!gpu(index) || runtimeUsed_ >= runtimeCount_)
        return -1;
    const uint32_t copy = runtimeFirst_ + runtimeUsed_++;
    set(copy, *gpu(index));
    setParams(copy, params(index));
    writeParams(copy, params(index) + 4, 4);  // (the engine's: refraction, ior, vertex bound and distance)
    return int32_t(copy);
}

void MaterialLibrary::restoreScripted()
{
    for (const auto& [index, before] : scriptedBefore_)
        write(index, before);
    scriptedBefore_.clear();
    for (const auto& [index, before] : paramsBefore_)
        writeParams(index, before.data());
    paramsBefore_.clear();
    runtimeUsed_ = 0;
}

bool MaterialLibrary::load(const std::string& source, uint32_t index)
{
    if (projectDir_.empty() || index >= pak_->materials().size())
        return false;
    MaterialAsset asset;
    std::string error;
    if (!readMaterialAsset(projectDir_ + "/" + source, asset, error)) {
        FX_WARN("materials: %s", error.c_str());
        return false;
    }
    fpak::MaterialGpu m = pak_->materials()[index];
    applyMaterialFactors(asset, m);
    // Not cooked yet (no textures in the scene's virtual texture): its colour texture's average
    // tints it meanwhile, so it reads as itself rather than plain white until Reload Scene.
    if (m.vtPageTable == fpak::kVtNoTexture && !asset.textures[0].empty()) {
        const float* a = averageColour(asset.textures[0]);
        for (int k = 0; k < 3; ++k)
            m.baseColor[k] *= a[k];
    }
    // (R4) Its surface shader's id (its place among the project's + 1; 0: none) and params.
    const auto shader = std::find(surfaces_.begin(), surfaces_.end(), asset.shader);
    const uint32_t id = asset.shader.empty() || shader == surfaces_.end() ? 0u : uint32_t(shader - surfaces_.begin() + 1);
    if (!asset.shader.empty() && !id)
        FX_WARN("materials: %s: no surface shader %s in the project", source.c_str(), asset.shader.c_str());
    const bool vertex = id && std::find(vertexShaders_.begin(), vertexShaders_.end(), asset.shader) != vertexShaders_.end();
    m.flags = (m.flags & ~(0xff00u | fpak::kMaterialVertex)) | (std::min(id, 255u) << 8) | (vertex ? fpak::kMaterialVertex : 0u);
    write(index, m);
    writeParams(index, asset.params);
    const float engine[4] = {asset.refraction, asset.ior, asset.vertexBound, asset.vertexDistance};
    writeParams(index, engine, 4);
    return true;
}

int32_t MaterialLibrary::indexOf(const std::string& source)
{
    if (source.empty() || !pak_)
        return -1;
    if (auto it = index_.find(source); it != index_.end())
        return int32_t(it->second);
    if (spareUsed_ >= spareCount_) {
        if (spareCount_)
            FX_WARN("materials: no spare material slot for %s (reload the scene)", source.c_str());
        return -1;
    }
    const uint32_t index = spareFirst_ + spareUsed_++;
    index_[source] = index;
    load(source, index);
    return int32_t(index);
}

const float* MaterialLibrary::averageColour(const std::string& image)
{
    static const float kWhite[3] = {1.0f, 1.0f, 1.0f};
    const std::string file = asset_library::isLibrary(image) ? asset_library::absolute(image) : projectDir_ + "/" + image;
    std::error_code ec;
    const auto u8 = std::filesystem::path(std::u8string(file.begin(), file.end()));
    const auto time = std::filesystem::last_write_time(u8, ec);
    if (ec)
        return kWhite;
    const uint64_t stamp = uint64_t(time.time_since_epoch().count());
    auto& entry = averages_[file];
    if (entry.stamp == stamp)
        return entry.rgb.data();
    entry.stamp = stamp;
    entry.rgb = {1.0f, 1.0f, 1.0f};
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(file.c_str(), &w, &h, &n, 3);
    if (!px)
        return entry.rgb.data();
    // (sRGB texels to linear, averaged on a grid of at most 64x64 of them)
    double sum[3] = {};
    const int step = std::max(1, std::max(w, h) / 64);
    uint32_t count = 0;
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step, ++count)
            for (int k = 0; k < 3; ++k) {
                const double c = px[(size_t(y) * w + x) * 3 + k] / 255.0;
                sum[k] += c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            }
    stbi_image_free(px);
    for (int k = 0; k < 3; ++k)
        entry.rgb[k] = count ? float(sum[k] / count) : 1.0f;
    return entry.rgb.data();
}

bool MaterialLibrary::reload(const std::string& source)
{
    const auto it = index_.find(source);
    return it != index_.end() && load(source, it->second);
}

bool MaterialLibrary::cooked(const std::string& source) const { return pak_ && pak_->findMaterial(source) >= 0; }

std::string MaterialLibrary::sourceOf(uint32_t index) const
{
    for (const auto& [source, i] : index_)
        if (i == index)
            return source;
    return {};
}

}  // namespace fx
