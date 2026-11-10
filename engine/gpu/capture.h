#pragma once

#include <SDL3/SDL_gpu.h>

namespace fx {

// Records a download of an RGBA8 (or BGRA8) texture into `cmd`, submits it, waits for the GPU
// and writes a PNG, resampled to outWidth x outHeight when given (previews). Stalls the GPU;
// meant for tests, golden images and previews.
bool captureTextureToPng(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t width,
                         uint32_t height, const char* path, bool bgra = false, uint32_t outWidth = 0, uint32_t outHeight = 0);

}  // namespace fx
