#pragma once

// GPU timing with timestamp queries (SDL_falcon_timestamps.h, our SDL patch).
//
// Marks, not begin/end pairs: one timestamp just before each pass starts. A mark lasts until
// the next mark of the same or a higher level, so the passes partition the frame's GPU time
// and nothing is left untracked between them. Sections (level 0: "Shadows", "Post") group the
// passes after them (level 1: "vsm mark", "bloom down 3").
//
// A small ring of query sets: one per frame in flight. A frame's set is read when the renderer's
// stats readback for a later or equal frame has signaled (GpuDrivenRenderer::pollStats), and
// its spans go to the profiler's frame record (profiler::attachGpu).

#include "core/profiler.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace fx::gpuprof {

bool init(SDL_GPUDevice* device);  // false (and inactive) when the backend has no timestamps
void shutdown();
bool available();
// Marks are written only while active (the Profiler is open, or --profile).
void setActive(bool on);
bool active();
// Sections (level-0 marks) are timed every frame even while inactive, for what budgets itself
// by its GPU time (the shadow map): sectionMs() is a section's time in the frame of `ticket`
// (false: not timed, or no longer kept).
bool sectionMs(uint64_t ticket, uint32_t name, float& ms);

// Frame: begin right after the command buffer is acquired, end right before it is submitted
// (outside passes). end() returns a ticket for the frame (0 = nothing recorded).
void beginFrame(SDL_GPUCommandBuffer* cmd);
uint64_t endFrame(SDL_GPUCommandBuffer* cmd);
// Every frame with a ticket up to `ticket` has finished on the GPU.
void completed(uint64_t ticket);
uint64_t lastTicket();

// Outside passes (just before SDL_Begin*Pass / SDL_BlitGPUTexture).
void mark(SDL_GPUCommandBuffer* cmd, uint32_t name, uint32_t level);
void pass(SDL_GPUCommandBuffer* cmd, const char* name);  // level 1, interned per call

struct Stats {
    uint32_t marks = 0;       // written last frame
    uint32_t dropped = 0;     // frames skipped because every set was in flight
};
Stats stats();

}  // namespace fx::gpuprof

#define FX_GPU_MARK(cmd, name, level)                                                                         \
    do {                                                                                                     \
        static const uint32_t fxGpuMarkName = ::fx::profiler::intern(name, ::fx::profiler::Category::Rendering); \
        ::fx::gpuprof::mark(cmd, fxGpuMarkName, level);                                                      \
    } while (0)
#define FX_GPU_SECTION(cmd, name) FX_GPU_MARK(cmd, name, 0)
#define FX_GPU_PASS(cmd, name) FX_GPU_MARK(cmd, name, 1)
