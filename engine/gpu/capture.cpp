#include "gpu/capture.h"

#include "core/log.h"
#include "core/profile.h"
#include "gpu/gpu_memory.h"

#include <SDL3/SDL.h>
#include <stb_image_resize2.h>

#include <vector>

namespace fx {

bool captureTextureToPng(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t width,
                         uint32_t height, const char* path, bool bgra, uint32_t outWidth, uint32_t outHeight)
{
    FX_ZONE_CAT("captureTextureToPng", Wait);  // (waits for the GPU, then writes the PNG)
    const uint32_t size = width * height * 4;
    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tinfo.size = size;
    SDL_GPUTransferBuffer* download = gpumem::createTransferBuffer("Capture", device, &tinfo);
    if (!download) {
        FX_ERROR("capture: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = texture;
    src.w = width;
    src.h = height;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    bool ok = fence && SDL_WaitForGPUFences(device, true, &fence, 1);
    if (fence)
        SDL_ReleaseGPUFence(device, fence);

    if (ok) {
        void* pixels = SDL_MapGPUTransferBuffer(device, download, false);
        // Resampled (previews: rendered larger, so the downscale smooths the edges).
        std::vector<uint8_t> scaled;
        if (pixels && outWidth && outHeight && (outWidth != width || outHeight != height)) {
            scaled.resize(size_t(outWidth) * outHeight * 4);
            stbir_resize_uint8_srgb(static_cast<const uint8_t*>(pixels), int(width), int(height), int(width * 4), scaled.data(),
                                    int(outWidth), int(outHeight), int(outWidth * 4), bgra ? STBIR_BGRA : STBIR_RGBA);
            pixels = scaled.data();
            width = outWidth;
            height = outHeight;
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(int(width), int(height), bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32,
                                                     pixels, int(width * 4));
        ok = surface && SDL_SavePNG(surface, path);
        SDL_DestroySurface(surface);
        SDL_UnmapGPUTransferBuffer(device, download);
    }
    gpumem::releaseTransferBuffer(device, download);
    if (ok)
        FX_INFO("capture: wrote %s (%ux%u)", path, width, height);
    else
        FX_ERROR("capture failed: %s", SDL_GetError());
    return ok;
}

}  // namespace fx
