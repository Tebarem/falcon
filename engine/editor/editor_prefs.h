#pragma once

// The editor's own settings, kept across sessions in the user's app-data folder
// (SDL_GetPrefPath: %APPDATA%/Falcon/Editor/editor.json on Windows):
//   settings  render / post / lighting-panel values (EditorBridge::writeSettings keys), vsync
//   tools     scene-view tool state (tool, space, pivot, grid, snaps)
//   page      the page's own layout state (panel sizes, tabs, foldouts...)
//   window    x, y, width, height, maximized
//   scenes    per scene file name: sun and scene-camera pose
//   library   folder: My Library's folder (project/asset_library.h; the Project panel's ⚙)

#include "core/json.h"

#include <cstdint>
#include <string>

namespace fx {

class EditorPrefs {
public:
    bool load();  // false (and empty prefs) when there is no file yet
    // Writes when something changed: at most once a second, or right away with `force`.
    void save(bool force = false);

    Json& section(const char* name) { return root_.child(name); }
    Json& scene(const std::string& file) { return root_.child("scenes").child(file); }
    void touch() { dirty_ = true; }
    const std::string& path() const { return path_; }

private:
    Json root_ = Json::makeObject();
    std::string path_;
    bool dirty_ = false;
    uint64_t lastSaveNs_ = 0;
};

}  // namespace fx
