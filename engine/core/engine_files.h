#pragma once

// The engine's own files: the UI's page bundles and the engine's content (the prototype textures,
// the material library). A build with FALCON_EMBED_DATA carries them in falcon.exe (core/bundle.h);
// other builds have them beside it in falcon-ui/ and content/.
//
//   UI pages      read from falcon.exe as they load: the page's URL is falcon://ui/<bundle>/...
//                 (UiLayer serves the falcon scheme), else file: URLs of <exe>/falcon-ui/<bundle>/.
//   Content       copied into projects by plain file code (asset_library, prototype_materials), so
//                 falcon.exe's is unpacked once (per version of it) into the user's cache
//                 (platform::localCacheDir()/Content/<hash of the content>/), on a thread from the start.

#include "core/bundle.h"

#include <filesystem>
#include <string>

namespace fx::engine_files {

constexpr const char* kUiScheme = "falcon";
constexpr const char* kUiPrefix = "falcon://ui/";

// What falcon.exe carries (empty: nothing, the folders are used).
const bundle::Bundle& packed();

// A page bundle by name ("editor", "launcher", "hud"): falcon://ui/<name> when falcon.exe carries
// it, else <exe>/falcon-ui/<name> (returned whether it is there or not when `orFolder`; empty when
// neither is there otherwise).
std::string uiBundle(const std::string& name, bool orFolder = true);
// Whether `bundle` is one falcon.exe carries (falcon://ui/...), and the path of a file of it.
bool isPackedUi(const std::string& bundle);
// A file of the engine's page bundles ("editor/falcon-panel.js") copied to `to`.
bool copyUiFile(const std::string& path, const std::filesystem::path& to);

// Starts unpacking falcon.exe's content, if it carries any and it isn't unpacked already.
void prepareContent();
// A folder of the engine's content ("library", "prototype"), absolute and '/' separated: waits for
// the unpacking. Empty when falcon.exe's content couldn't be unpacked.
std::string contentDir(const std::string& name);

}  // namespace fx::engine_files
