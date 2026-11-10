#pragma once

// GPU memory accounting for the profiler: SDL_GPU resource creation and release go through these
// wrappers, which record each resource's size, module ("Shadows", "Post") and debug name. Sizes
// are what the resources need (texels x format size over every mip, buffer sizes), not what the
// driver allocates (alignment, padding, compression metadata).

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fx::gpumem {

SDL_GPUBuffer* createBuffer(const char* module, SDL_GPUDevice* device, const SDL_GPUBufferCreateInfo* info);
SDL_GPUTexture* createTexture(const char* module, SDL_GPUDevice* device, const SDL_GPUTextureCreateInfo* info);
// Transfer buffers have no SDL debug name; `name` is only for the profiler's memory view.
SDL_GPUTransferBuffer* createTransferBuffer(const char* module, SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* info,
                                           const char* name = nullptr);
void releaseBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer);
void releaseTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture);
void releaseTransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* buffer);
void setBufferName(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, const char* name);
void setTextureName(SDL_GPUDevice* device, SDL_GPUTexture* texture, const char* name);

struct Resource {
    std::string module, name, kind;  // kind: "buffer", "texture", "transfer"
    uint64_t bytes;
};
std::vector<Resource> resources();
// Per-module totals, sorted by size, and the overall total.
std::vector<std::pair<std::string, uint64_t>> modules(uint64_t* total = nullptr);

}  // namespace fx::gpumem
