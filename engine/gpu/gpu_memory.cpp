#include "gpu/gpu_memory.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace fx::gpumem {

namespace {

std::mutex g_mutex;
std::unordered_map<const void*, Resource> g_resources;

void track(const void* key, const char* module, const char* kind, uint64_t bytes)
{
    if (!key)
        return;
    std::lock_guard lock(g_mutex);
    g_resources[key] = {module, "", kind, bytes};
}

void untrack(const void* key)
{
    std::lock_guard lock(g_mutex);
    g_resources.erase(key);
}

void name(const void* key, const char* text)
{
    std::lock_guard lock(g_mutex);
    if (auto it = g_resources.find(key); it != g_resources.end() && text)
        it->second.name = text;
}

}  // namespace

SDL_GPUBuffer* createBuffer(const char* module, SDL_GPUDevice* device, const SDL_GPUBufferCreateInfo* info)
{
    SDL_GPUBuffer* b = SDL_CreateGPUBuffer(device, info);
    track(b, module, "buffer", info->size);
    return b;
}

SDL_GPUTexture* createTexture(const char* module, SDL_GPUDevice* device, const SDL_GPUTextureCreateInfo* info)
{
    SDL_GPUTexture* t = SDL_CreateGPUTexture(device, info);
    const bool volume = info->type == SDL_GPU_TEXTURETYPE_3D;
    const uint32_t layers = volume ? 1 : info->layer_count_or_depth;
    uint64_t bytes = 0;
    for (uint32_t m = 0; m < std::max(info->num_levels, 1u); ++m) {
        const uint32_t w = std::max(info->width >> m, 1u), h = std::max(info->height >> m, 1u);
        const uint32_t d = volume ? std::max(info->layer_count_or_depth >> m, 1u) : 1u;
        bytes += uint64_t(SDL_CalculateGPUTextureFormatSize(info->format, w, h, d)) * layers;
    }
    if (info->sample_count != SDL_GPU_SAMPLECOUNT_1)
        bytes <<= uint32_t(info->sample_count);
    track(t, module, "texture", bytes);
    return t;
}

SDL_GPUTransferBuffer* createTransferBuffer(const char* module, SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* info,
                                           const char* text)
{
    SDL_GPUTransferBuffer* b = SDL_CreateGPUTransferBuffer(device, info);
    track(b, module, "transfer", info->size);
    name(b, text);
    return b;
}

void releaseBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer)
{
    untrack(buffer);
    SDL_ReleaseGPUBuffer(device, buffer);
}

void releaseTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture)
{
    untrack(texture);
    SDL_ReleaseGPUTexture(device, texture);
}

void releaseTransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* buffer)
{
    untrack(buffer);
    SDL_ReleaseGPUTransferBuffer(device, buffer);
}

void setBufferName(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, const char* text)
{
    name(buffer, text);
    SDL_SetGPUBufferName(device, buffer, text);
}

void setTextureName(SDL_GPUDevice* device, SDL_GPUTexture* texture, const char* text)
{
    name(texture, text);
    SDL_SetGPUTextureName(device, texture, text);
}

std::vector<Resource> resources()
{
    std::lock_guard lock(g_mutex);
    std::vector<Resource> out;
    out.reserve(g_resources.size());
    for (const auto& [key, r] : g_resources)
        out.push_back(r);
    std::sort(out.begin(), out.end(), [](const Resource& a, const Resource& b) { return a.bytes > b.bytes; });
    return out;
}

std::vector<std::pair<std::string, uint64_t>> modules(uint64_t* total)
{
    std::lock_guard lock(g_mutex);
    std::vector<std::pair<std::string, uint64_t>> out;
    uint64_t sum = 0;
    for (const auto& [key, r] : g_resources) {
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& m) { return m.first == r.module; });
        if (it == out.end())
            out.push_back({r.module, r.bytes});
        else
            it->second += r.bytes;
        sum += r.bytes;
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (total)
        *total = sum;
    return out;
}

}  // namespace fx::gpumem
