#include "assets/material_asset.h"

#include "core/json.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fx {

namespace {

void readFloats(const Json& j, const char* key, float* out, size_t count)
{
    if (const Json* v = j.get(key))
        for (size_t i = 0; i < count && i < v->array.size(); ++i)
            if (v->array[i].type == Json::Type::Number)
                out[i] = float(v->array[i].number);
}

float finiteOr(double v, float otherwise)
{
    return std::isfinite(v) ? float(v) : otherwise;
}

}  // namespace

bool readMaterialAsset(const std::string& path, MaterialAsset& out, std::string& error)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text) {
        error = "Cannot read " + path;
        return false;
    }
    try {
        const Json j = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        out = {};
        out.name = j.getString("name", std::filesystem::path(path).stem().string().c_str());
        readFloats(j, "baseColor", out.baseColor, 4);
        out.metallic = float(j.getNumber("metallic", out.metallic));
        out.roughness = float(j.getNumber("roughness", out.roughness));
        out.roughnessMin = std::clamp(finiteOr(j.getNumber("roughnessMin", 0.0), 0.0f), 0.0f, 1.0f);
        readFloats(j, "emissive", out.emissive, 3);
        out.emissiveStrength = float(j.getNumber("emissiveStrength", out.emissiveStrength));
        out.normalScale = float(j.getNumber("normalScale", out.normalScale));
        out.occlusionStrength = float(j.getNumber("occlusionStrength", out.occlusionStrength));
        out.alpha = j.getString("alpha", "opaque");
        if (out.alpha != "mask" && out.alpha != "blend")
            out.alpha = "opaque";
        out.alphaCutoff = float(j.getNumber("alphaCutoff", out.alphaCutoff));
        out.doubleSided = j.getBool("doubleSided");
        readFloats(j, "size", out.size, 2);
        out.size[0] = std::max(out.size[0], 0.0f), out.size[1] = std::max(out.size[1], 0.0f);
        out.shader = j.getString("shader");
        out.refraction = float(std::clamp(j.getNumber("refraction", 0.0), 0.0, 1.0));
        out.ior = float(std::clamp(j.getNumber("ior", 1.5), 1.0, 3.0));
        out.vertexBound = float(std::clamp(j.getNumber("vertexBound", 1.0), 0.0, 1000.0));
        out.vertexDistance = float(std::clamp(j.getNumber("vertexDistance", 0.0), 0.0, 100000.0));
        if (const Json* p = j.get("params"))
            for (size_t i = 0; i < 4 && i < p->array.size(); ++i)
                out.params[i] = float(p->array[i].number);
        if (const Json* t = j.get("textures")) {
            for (int s = 0; s < 4; ++s)
                out.textures[s] = t->getString(MaterialAsset::kTextureSlots[s]);
            out.height = t->getString("height");
        }
        out.displacement = finiteOr(j.getNumber("displacement", 0.0), 0.0f);
        out.heightCenter = float(std::clamp(j.getNumber("heightCenter", 0.0), 0.0, 1.0));
        out.displacementDetail = float(std::clamp(j.getNumber("displacementDetail", 0.0), 0.0, 10.0));
        out.parallax = std::max(finiteOr(j.getNumber("parallax", 0.0), 0.0f), 0.0f);
        if (const Json* a = j.get("acoustic"))
            readAcousticJson(*a, out.acousticPreset, out.acoustic);
        if (const Json* s = j.get("surface"))
            readSurfaceJson(*s, out.surfacePreset, out.surface);
    } catch (const std::exception& e) {
        error = path + " is malformed (" + e.what() + ")";
        return false;
    }
    return true;
}

bool writeMaterialAsset(const std::string& path, const MaterialAsset& m, std::string& error)
{
    JsonWriter w;
    w.beginObject().value("format", 1u).value("name", m.name).beginArray("baseColor");
    for (const float c : m.baseColor)
        w.value(nullptr, double(c));
    w.endArray().value("metallic", double(m.metallic)).value("roughness", double(m.roughness)).beginArray("emissive");
    for (const float c : m.emissive)
        w.value(nullptr, double(c));
    w.endArray().value("emissiveStrength", double(m.emissiveStrength)).value("normalScale", double(m.normalScale))
        .value("occlusionStrength", double(m.occlusionStrength)).value("alpha", m.alpha).value("alphaCutoff", double(m.alphaCutoff))
        .value("doubleSided", m.doubleSided).value("displacement", double(m.displacement))
        .value("heightCenter", double(m.heightCenter)).value("displacementDetail", double(m.displacementDetail));
    if (m.refraction > 0.0f)
        w.value("refraction", double(m.refraction)).value("ior", double(m.ior));
    if (m.parallax > 0.0f)
        w.value("parallax", double(m.parallax));
    if (m.roughnessMin > 0.0f)
        w.value("roughnessMin", double(m.roughnessMin));
    if (m.size[0] > 0.0f && m.size[1] > 0.0f)
        w.beginArray("size").value(nullptr, double(m.size[0])).value(nullptr, double(m.size[1])).endArray();
    if (!m.shader.empty()) {
        w.value("vertexBound", double(m.vertexBound)).value("vertexDistance", double(m.vertexDistance));
        w.value("shader", m.shader).beginArray("params");
        for (const float p : m.params)
            w.value(nullptr, double(p));
        w.endArray();
    }
    w.beginObject("textures");
    for (int s = 0; s < 4; ++s)
        if (!m.textures[s].empty())
            w.value(MaterialAsset::kTextureSlots[s], m.textures[s]);
    if (!m.height.empty())
        w.value("height", m.height);
    w.endObject();
    writeAcousticJson(w, "acoustic", m.acousticPreset, m.acoustic);
    w.beginObject("surface");
    writeSurfaceFields(w, m.surfacePreset, m.surface);
    w.endObject().endObject();
    const std::string text = Json::parse(w.str()).dump(2) + "\n";
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    if (!SDL_SaveFile(tmp.c_str(), text.data(), text.size())) {
        error = "Cannot write " + path;
        return false;
    }
    const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
    std::filesystem::rename(u8(tmp), u8(path), ec);
    if (ec) {
        error = "Cannot write " + path + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

// A float's IEEE half bits (round to nearest; past 65504 the largest half, under 2^-24 zero), for
// values >= 0.
static uint32_t halfBits(float f)
{
    if (!(f > 0.0f))
        return 0;
    if (f >= 65504.0f)
        return 0x7bffu;
    if (f < 6.103515625e-5f)  // (subnormal: steps of 2^-24)
        return uint32_t(std::lround(f * 16777216.0f));
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t e = ((x >> 23) & 0xffu) - 127u + 15u;
    return std::min((e << 10) + (((x & 0x7fffffu) + 0x1000u) >> 13), 0x7bffu);
}

void applyMaterialFactors(const MaterialAsset& m, fpak::MaterialGpu& gpu)
{
    std::copy(std::begin(m.baseColor), std::end(m.baseColor), gpu.baseColor);
    gpu.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
    gpu.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
    gpu.vtSize = (gpu.vtSize & ~fpak::kVtRoughnessMinMask) | fpak::roughnessMinBits(m.roughnessMin);
    gpu.alphaCutoff = m.alphaCutoff;
    const uint32_t parallax = halfBits(m.parallax);  // (metres, as a half)
    gpu.flags = (gpu.flags & ~uint32_t(fpak::kMaterialAlphaMask | fpak::kMaterialAlphaBlend | fpak::kMaterialDoubleSided |
                                       fpak::kMaterialRefraction | 0xffff0000u)) |
                (m.height.empty() ? 0u : parallax << 16) |
                (m.alpha == "mask" ? fpak::kMaterialAlphaMask : 0u) | (m.alpha == "blend" ? fpak::kMaterialAlphaBlend : 0u) |
                (m.doubleSided ? fpak::kMaterialDoubleSided : 0u) |
                (m.alpha == "blend" && m.refraction > 0.0f ? fpak::kMaterialRefraction : 0u);
    gpu.normalScale = m.normalScale;
    gpu.occlusionStrength = m.occlusionStrength;
    std::copy(std::begin(m.emissive), std::end(m.emissive), gpu.emissive);
    gpu.emissive[3] = m.emissiveStrength;
}

std::string materialCookKey(const MaterialAsset& m)
{
    std::string key;
    for (const std::string& t : m.textures)
        key += t + ";";
    if (!m.height.empty() && !m.displaced())  // (the height map: the ORM pages' alpha)
        key += "h:" + m.height + ";";
    if (m.displaced())  // (the displaced meshes)
        key += m.height + ";" + std::to_string(m.displacement) + ";" + std::to_string(m.heightCenter) + ";" +
               std::to_string(m.displacementDetail) + ";";
    return key;
}

}  // namespace fx
