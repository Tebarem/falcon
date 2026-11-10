#pragma once

#include <cstdint>

namespace fx {

enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };

// Precompiled shader embedded by falcon_shaderc. Resource counts follow
// SDL_GPU's per-stage binding model and are validated at build time.
struct ShaderBlob {
    const char* name;
    ShaderStage stage;
    const uint8_t* spirv;
    uint32_t spirvSize;
    const uint8_t* dxil;
    uint32_t dxilSize;
    const char* dxilEntry;
    const uint8_t* msl;  // Metal Shading Language source (not NUL-terminated)
    uint32_t mslSize;
    const char* mslEntry;
    uint32_t numSamplers;
    uint32_t numStorageTextures;  // read-only
    uint32_t numStorageBuffers;   // read-only
    uint32_t numUniformBuffers;
    uint32_t numRwStorageTextures;  // compute only
    uint32_t numRwStorageBuffers;   // compute only
    uint32_t threadsX, threadsY, threadsZ;  // compute only
};

}  // namespace fx
