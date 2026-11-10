#include "render/shader_file.h"

#include "core/json.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace fx {

const ShaderFile::Uniform* ShaderFile::Entry::uniform(std::string_view name) const
{
    for (const Uniform& u : uniforms)
        if (u.name == name)
            return &u;
    return nullptr;
}

const ShaderFile::Entry* ShaderFile::entry(std::string_view name) const
{
    for (const Entry& e : entries)
        if (name.empty() || e.name == name)
            return &e;
    return nullptr;
}

bool loadShaderFile(const std::string& path, ShaderFile& out, std::string& error)
{
    out = {};
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        error = path + ": " + SDL_GetError();
        return false;
    }
    out.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    SDL_free(data);
    uint32_t header[3] = {};
    if (size < 12 || std::memcmp(out.bytes.data(), "FXSH", 4) != 0) {
        error = path + ": not a compiled shader";
        return false;
    }
    std::memcpy(header, out.bytes.data(), 12);
    const uint32_t jsonSize = header[2];
    if (header[1] != 1 || 12ull + jsonSize > size) {
        error = path + ": a compiled shader of another version (compile it again)";
        return false;
    }
    const size_t code = 12 + jsonSize;
    try {
        const Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(out.bytes.data()) + 12, jsonSize));
        out.source = root.getString("source");
        if (const Json* list = root.get("entries"))
            for (const Json& e : list->array) {
                ShaderFile::Entry entry;
                entry.name = e.getString("name");
                const std::string stage = e.getString("stage");
                ShaderBlob& b = entry.blob;
                b.stage = stage == "vertex" ? ShaderStage::Vertex : stage == "fragment" ? ShaderStage::Fragment : ShaderStage::Compute;
                const auto at = [&](const char* key, size_t i) {
                    const Json* a = e.get(key);
                    return a && i < a->array.size() ? uint32_t(a->array[i].number) : 0u;
                };
                // (counts in falcon_shaderc's order: samplers, storage textures, storage buffers, rw
                // storage textures, rw storage buffers, uniform buffers)
                b.numSamplers = at("counts", 0);
                b.numStorageTextures = at("counts", 1);
                b.numStorageBuffers = at("counts", 2);
                b.numRwStorageTextures = at("counts", 3);
                b.numRwStorageBuffers = at("counts", 4);
                b.numUniformBuffers = at("counts", 5);
                b.threadsX = at("threads", 0);
                b.threadsY = at("threads", 1);
                b.threadsZ = at("threads", 2);
                if (code + at("spirv", 0) + at("spirv", 1) > size || code + at("dxil", 0) + at("dxil", 1) > size) {
                    error = path + ": truncated";
                    return false;
                }
                b.spirv = out.bytes.data() + code + at("spirv", 0);
                b.spirvSize = at("spirv", 1);
                b.dxil = out.bytes.data() + code + at("dxil", 0);
                b.dxilSize = at("dxil", 1);
                // (no "msl" in shaders compiled before Metal: mslSize 0, refused on Metal)
                if (code + at("msl", 0) + at("msl", 1) <= size) {
                    b.msl = out.bytes.data() + code + at("msl", 0);
                    b.mslSize = at("msl", 1);
                }
                entry.mslEntry = e.getString("mslEntry");
                entry.uniformSize = uint32_t(e.getNumber("uniformSize", 0));
                if (const Json* us = e.get("uniforms"))
                    for (const Json& u : us->array)
                        entry.uniforms.push_back({u.getString("name"), u.getString("type"), uint32_t(u.getNumber("offset")),
                                                  uint32_t(u.getNumber("size")), uint32_t(u.getNumber("components"))});
                out.entries.push_back(std::move(entry));
            }
    } catch (const std::exception& e) {
        error = path + ": " + e.what();
        return false;
    }
    for (ShaderFile::Entry& e : out.entries) {  // (the names: the entries' own strings, now they stay put)
        e.blob.name = e.name.c_str();
        e.blob.dxilEntry = e.name.c_str();
        e.blob.mslEntry = e.mslEntry.c_str();
    }
    if (out.entries.empty()) {
        error = path + ": no entry points";
        return false;
    }
    return true;
}

}  // namespace fx
