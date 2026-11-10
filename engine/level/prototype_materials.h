#pragma once
// Blockout materials: Kenney's prototype textures (CC0, the engine's content/prototype: six colours
// of thirteen 1 m patterns: grids, checkers, stairs, door and window guides). The editor puts them
// into a project once, as Assets/Prototype (every texture, and materials for the ones blockouts use
// most), and level meshes' faces without a material draw with Light Grid.

#include <string>

namespace fx::level {

inline constexpr const char* kPrototypeFolder = "Assets/Prototype";
inline constexpr const char* kPrototypeDefault = "Assets/Prototype/Light Grid.fmat";

// `contentDir`: the engine's content/prototype; `projectDir`: the project's folder. The textures go
// to Assets/Prototype/Textures/<Colour>/<pattern>.png, materials to Assets/Prototype/<Colour>
// <Pattern>.fmat (what is there already is written again). `materials`: how many were written.
bool installPrototype(const std::string& contentDir, const std::string& projectDir, std::string& error, int* materials = nullptr);

}  // namespace fx::level
