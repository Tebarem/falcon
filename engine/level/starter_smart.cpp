// The starter smart materials (smart_material.h makeStarterSmartMaterials): their materials (by
// their factors) and three presets.

#include "level/smart_material.h"

#include "assets/material_asset.h"

#include <filesystem>

namespace fx::level {

bool makeStarterSmartMaterials(const std::string& projectDir, const std::string& folder, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(projectDir) / folder;
    fs::create_directories(dir, ec);
    if (ec)
        return error = "Can't make " + dir.generic_string() + ": " + ec.message(), false;
    // Materials by their factors (no textures: they read at once).
    struct Mat {
        const char* name;
        float color[3], metallic, roughness;
    };
    const Mat mats[] = {
        {"Concrete", {0.55f, 0.54f, 0.5f}, 0.0f, 0.9f},  {"Concrete Stain", {0.3f, 0.28f, 0.25f}, 0.0f, 0.95f},
        {"Moss", {0.2f, 0.32f, 0.1f}, 0.0f, 0.9f},        {"Paint", {0.2f, 0.36f, 0.45f}, 0.0f, 0.45f},
        {"Bare Metal", {0.8f, 0.8f, 0.82f}, 1.0f, 0.3f}, {"Dirt", {0.25f, 0.17f, 0.1f}, 0.0f, 0.95f},
        {"Rust", {0.45f, 0.2f, 0.08f}, 0.2f, 0.85f},     {"Steel", {0.55f, 0.56f, 0.58f}, 1.0f, 0.45f},
    };
    for (const Mat& m : mats) {
        MaterialAsset a;
        a.name = m.name;
        for (int k = 0; k < 3; ++k)
            a.baseColor[k] = m.color[k];
        a.metallic = m.metallic;
        a.roughness = m.roughness;
        if (!writeMaterialAsset((dir / (std::string(m.name) + ".fmat")).string(), a, error))
            return false;
    }
    const auto path = [&](const char* name) { return folder + "/" + name + ".fmat"; };
    const auto wear = [](WearKind kind, uint32_t layer, float width, float strength, float foot = 0.0f) {
        WearSettings w;
        w.kind = kind;
        w.channel = layer;
        w.width = width;
        w.strength = strength;
        w.foot = foot;
        return w;
    };
    SmartMaterial presets[3];
    presets[0] = {"Old Concrete", path("Concrete"), {path("Concrete Stain"), path("Moss"), ""}, 0.2f,
                  wear(WearKind::Edges, 0, 0.06f, 0.8f), wear(WearKind::Grime, 1, 0.2f, 0.9f, 0.4f)};
    presets[1] = {"Chipped Paint", path("Paint"), {path("Bare Metal"), path("Dirt"), ""}, 0.15f, wear(WearKind::Edges, 0, 0.08f, 1.0f),
                  wear(WearKind::Grime, 1, 0.15f, 0.7f, 0.25f)};
    presets[2] = {"Rusty Steel", path("Steel"), {path("Rust"), path("Dirt"), ""}, 0.25f, wear(WearKind::Edges, 0, 0.15f, 1.0f),
                  wear(WearKind::Grime, 1, 0.12f, 0.6f, 0.3f)};
    for (const SmartMaterial& p : presets)
        if (!writeSmartMaterial((dir / (p.name + ".fsmart")).string(), p, error))
            return false;
    return true;
}

}  // namespace fx::level
