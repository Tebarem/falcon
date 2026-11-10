#include "level/prototype_materials.h"

#include "assets/material_asset.h"

#include <filesystem>

namespace fx::level {

namespace {

constexpr const char* kColours[] = {"Dark", "Light", "Green", "Orange", "Purple", "Red"};

// The materials made: a grid and a checker in every colour (colour-coding a blockout), and the
// stairs, door and window guides in the dark and light ones.
struct Made {
    const char* pattern;  // its texture's name
    const char* name;     // in the material's file name
    bool everyColour;
};
constexpr Made kMade[] = {
    {"grid", "Grid", true},     {"checker", "Checker", true}, {"stairs", "Stairs", false},
    {"door", "Door", false},    {"window", "Window", false},
};

}  // namespace

bool installPrototype(const std::string& contentDir, const std::string& projectDir, std::string& error, int* materials)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path from = fs::path(contentDir), to = fs::path(projectDir) / kPrototypeFolder;
    if (!fs::exists(from / "Dark" / "grid.png", ec))
        return error = "No prototype textures in " + from.generic_string(), false;
    fs::create_directories(to / "Textures", ec);
    fs::copy(from, to / "Textures", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec)
        return error = "Can't copy the prototype textures to " + (to / "Textures").generic_string() + ": " + ec.message(), false;
    int written = 0;
    for (const char* colour : kColours)
        for (const Made& m : kMade) {
            if (!m.everyColour && std::string(colour) != "Dark" && std::string(colour) != "Light")
                continue;
            MaterialAsset mat;
            mat.name = std::string(colour) + " " + m.name;
            mat.roughness = 0.85f;
            mat.textures[0] = std::string(kPrototypeFolder) + "/Textures/" + colour + "/" + m.pattern + ".png";
            if (!writeMaterialAsset((to / (mat.name + ".fmat")).string(), mat, error))
                return false;
            ++written;
        }
    if (materials)
        *materials = written;
    return true;
}

}  // namespace fx::level
