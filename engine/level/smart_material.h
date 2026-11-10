#pragma once
// Smart material presets (.fsmart): one click makes a level mesh look like "old concrete" or
// "chipped paint over metal": its faces' material, its three layers, and the wear and grime
// generated into them (level/wear.h).
//
//   {"name": "Chipped Paint", "base": "Assets/Smart/Paint.fmat",
//    "layers": ["Assets/Smart/Bare Metal.fmat", "Assets/Smart/Dirt.fmat", ""], "blend": 0.15,
//    "wear": {"layer": 0, "width": 0.08, "strength": 1}, "grime": {"layer": 1, "width": 0.15, "foot": 0.3, "strength": 0.8}}

#include "level/wear.h"

#include <optional>
#include <string>

namespace fx::level {

struct SmartMaterial {
    std::string name, base;  // base: every face's material ("": leave them)
    std::string layers[3];
    float blend = 0.15f;
    std::optional<WearSettings> wear, grime;  // (their channel: the layer they go into)
};

bool parseSmartMaterial(const std::string& json, SmartMaterial& out, std::string& error);
bool readSmartMaterial(const std::string& path, SmartMaterial& out, std::string& error);
bool writeSmartMaterial(const std::string& path, const SmartMaterial& smart, std::string& error);

// The mesh and its mask with the preset applied (the mesh gets mask UVs if it has none). `live`:
// its wear and grime as live wear rules instead (level_meshes.h LevelWear; `mask` left empty).
bool applySmartMaterial(const SmartMaterial& smart, PolyMesh& mesh, std::vector<uint8_t>& mask, std::string& error,
                        std::vector<WearSettings>* live = nullptr);

// Starter presets in `folder` (project-relative, under Assets/): their materials and .fsmart files.
bool makeStarterSmartMaterials(const std::string& projectDir, const std::string& folder, std::string& error);

}  // namespace fx::level
