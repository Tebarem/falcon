#pragma once

// A shader compiled by falcon_shaderc --blob (a project's: Shaders/*.slang, compiled by the editor
// into Cache/Shaders/; R2): its entry points as ShaderBlobs pointing into its bytes (GpuDevice's
// createShader / createComputePipeline take them as they take the engine's own), and each one's
// first uniform block as reflected, so parameters can be set by name.

#include "gpu/shader_blob.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fx {

struct ShaderFile {
    struct Uniform {
        std::string name, type;  // type: float, int, uint, other
        uint32_t offset = 0, size = 0, components = 0;
    };
    struct Entry {
        std::string name, mslEntry;
        ShaderBlob blob = {};
        std::vector<Uniform> uniforms;
        uint32_t uniformSize = 0;
        const Uniform* uniform(std::string_view name) const;
    };
    std::string source;  // in the project
    std::vector<uint8_t> bytes;
    std::vector<Entry> entries;
    const Entry* entry(std::string_view name = {}) const;  // by name ("": the first)
};

bool loadShaderFile(const std::string& path, ShaderFile& out, std::string& error);

}  // namespace fx
