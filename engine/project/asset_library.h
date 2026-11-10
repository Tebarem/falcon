#pragma once

// Asset libraries: folders of materials, textures and models every project's Project panel shows
// (its Library section), used in a project by copying them into it.
//
//   @falcon/...   the engine's own (bin/content/library: CC0 materials), read-only
//   @library/...  the user's, My Library (Documents/Falcon Library unless the editor prefs'
//                 "library": {"folder"} or --library say otherwise): made and changed like a
//                 project's Assets/ (folders, imports, New Material)
//
// A path with one of those prefixes names a library item; a library .fmat's texture paths carry
// them too. A project uses one through its copy: @falcon/X is Assets/Falcon/X, @library/X is
// Assets/Library/X (a material with its textures, its paths made the project's; a .gltf with its
// files). A copy made before is used as it is, so the project's own edits to it stay. Projects
// cook and ship only their copies; game builds leave bin/content/library out.

#include <string>
#include <vector>

namespace fx::asset_library {

constexpr const char* kFalcon = "@falcon";
constexpr const char* kUser = "@library";

// Where the two are (absolute, '/' separated; empty: none). The editor sets them as it starts.
void setRoots(const std::string& falcon, const std::string& user);
const std::string& falconRoot();
const std::string& userRoot();
// Documents/Falcon Library.
std::string defaultUserRoot();

bool isLibrary(const std::string& path);  // "@falcon", "@library" or below them
bool readOnly(const std::string& path);   // Falcon's
// The file it names (empty: not a library path, a library that isn't there, or a path out of it).
std::string absolute(const std::string& path);
// Where its copy goes in a project: "@falcon/Wood/Oak.fmat" -> "Assets/Falcon/Wood/Oak.fmat".
std::string projectPathOf(const std::string& path);

// The project path to use for `path`: a project path as it is; a library item copied into the
// project first unless it is there already (`copied`: something was copied). Empty on an error.
std::string useInProject(const std::string& projectDir, const std::string& path, bool& copied, std::string& error);
// A project's material (with its textures, into a folder of its own) put into My Library's
// folder `into` ("@library/..."): its @library path. Empty on an error.
std::string addMaterial(const std::string& projectDir, const std::string& path, const std::string& into, std::string& error);

// A glTF's own files (buffers, images), relative to it; data: URIs are inside it.
std::vector<std::string> gltfDependencies(const std::string& gltf);

}  // namespace fx::asset_library
