#pragma once

// Projects: a folder with a project file and the scenes made in it.
//
//   MyWorld/
//     project.falcon   {"format": 1, "name", "startupScene": "Scenes/Main.fpak", "scenes": [...],
//                       "rendering": {"lodPixels", "cullPixels", "terrainPixels", "detailDistance"}}
//     Assets/          source files (glTF, images) imported into the project
//     Scripts/         the game's C# sources (project/game_scripts.h), built into Cache/Scripts/
//     Scenes/          each scene's cooked .fpak (+ .fvt), its edits (.edits.json) and its
//                      terrain (.terrain/), as for any scene
//     Cache/           rebuildable data
//
// A scene is cooked empty (`falcon cook --empty --rocks`: the built-in shapes and the procedural
// rocks, nothing placed) with every model of the project as a library (--library: their meshes
// and materials, placeable, nothing placed); what is placed lives in its edits. The launcher (editor/launcher.h) creates and opens projects; the
// editor opens one with --project. Recently opened projects are kept in the user's app-data
// folder (projects.json beside the editor's settings).

#include "level/navmesh.h"
#include "audio/audio_settings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fx {

// A game's network ("network" in project.falcon; Project Settings > Network): how its sessions
// run, given to the game at its start (Falcon.Network). A preset sets the rest; changing one of
// them makes it "custom".
struct NetworkSettings {
    std::string preset = "competitive";  // competitive, large, mmo, rollback, custom
    std::string mode = "authoritative";   // authoritative, rollback
    uint32_t sendRate = 30;               // the host's snapshots a second
    uint32_t bandwidthKbps = 512;         // each client's budget (kbit/s)
    uint32_t maxPlayers = 64;             // 0: no limit
    float interpolationMs = 100.0f;       // proxies shown this far in the past
    uint32_t inputDelay = 2;              // rollback mode: ticks between an input and its tick
    uint32_t rollbackWindow = 32;         // rollback mode: how far back a late input can go
    uint32_t port = 27015;                // a host's or dedicated server's default port
};
// A preset's values ("competitive": 64 players at 30 snapshots a second; "large": 1,000 at 20;
// "mmo": 2,000 at 10; "rollback": 2-8 players, the whole world from inputs). False: no such preset.
bool applyNetworkPreset(NetworkSettings& s, const std::string& preset);
// The settings as the game's launch JSON has them ({"mode", "sendRate", ...}).
std::string networkJson(const NetworkSettings& s);

struct Project {
    std::string dir;           // absolute, '/' separators, no trailing one
    std::string name;
    std::string startupScene;  // relative to dir
    std::vector<std::string> scenes;
    // Rendering settings saved with the project ("rendering" in project.falcon; File > Project
    // Settings in the editor), for the editor's view and the game alike.
    // geometry detail: the largest simplification error on screen, pixels (less: more detail).
    // 0.5 by default, half the engine's own (Zero-Day: ~28% more triangles, ~3% more GPU time).
    float lodPixels = 0.5f;
    float cullPixels = 4.0f;  // objects whose projected radius is under this many pixels are not drawn (0: all are)
    float terrainPixels = 1.0f;  // terrain detail: the largest height error on screen, pixels
    // Nearer than this (metres), meshes and terrain keep their full detail and nothing is left
    // out for being small on screen (the pixel limits above apply beyond it).
    float detailDistance = 10.0f;
    AudioSettings audio;      // "audio": volumes and the HRTF (File > Project Settings)
    // Steam ("steam" in project.falcon): the game's App ID (0: the game doesn't use Steam). 480 is
    // Valve's Spacewar, for development.
    uint32_t steamAppId = 0;
    uint32_t steamDepotId = 0;          // its Windows depot (0: the App ID + 1, Steamworks' default)
    std::string steamBranch;            // the branch an upload is set live on ("": none; never "default" by itself)
    bool steamRestartIfNecessary = true;  // a shipped game started outside Steam starts again through it
    NetworkSettings network;  // "network": how the game's sessions run
    // "navigation": the agent the navigation mesh is baked for (level/navmesh.h), and whether the
    // editor bakes it again as the level changes.
    level::NavSettings navigation;
    bool navAutoBake = true;
    // "level": the material level meshes' faces without one draw with ("": the scene's first), and
    // whether the editor has put the prototype textures into the project (level/prototype_materials.h:
    // once, so deleting them keeps them gone).
    std::string levelDefaultMaterial = "Assets/Prototype/Light Grid.fmat";
    bool prototypeInstalled = false;
    std::string file() const { return dir + "/project.falcon"; }
    std::string path(const std::string& relative) const { return dir + "/" + relative; }
};

// `path`: the project's folder or its project.falcon.
bool loadProject(const std::string& path, Project& out, std::string& error);
bool saveProject(const Project& project, std::string& error);
// A new project in `dir` (which must not exist, or be empty), with one empty scene, Main.
bool createProject(const std::string& dir, const std::string& name, Project& out, std::string& error);
// A new empty scene `name` (<folder>/<name>.fpak, the folder relative to the project), added to
// the project (and saved). `relative`: its path in the project. Without `cook`, it is cooked
// when it is first opened.
bool addScene(Project& project, const std::string& name, const std::string& folder, std::string& relative, std::string& error,
              bool cook = true);
// Cooks an empty scene (with the rocks) to `fpakPath`, running the cooker as a child process.
bool cookEmptyScene(const std::string& fpakPath, std::string& error);
// The project's models: .gltf / .glb files under Assets/ (relative to the project, sorted).
std::vector<std::string> projectModels(const std::string& projectDir);
// Its material assets: .fmat files under Assets/.
std::vector<std::string> projectMaterials(const std::string& projectDir);
// What a project scene's cook is doing (the cooker's --progress), for a progress bar.
struct CookProgress {
    // waiting (another process is cooking this scene), import, meshes, textures, writing,
    // installing (moving the cook into Scenes/)
    std::string stage;
    uint32_t done = 0, total = 0;
    uint32_t reused = 0;  // textures whose pages came from an earlier cook
    size_t models = 0;
};
// Called on the cooking thread a few times a second; false cancels the cook.
using CookProgressFn = std::function<bool(const CookProgress&)>;

// Cooks scene `relative` (empty, the rocks, every model as a library, every material asset)
// unless it was cooked with exactly the models and material textures there are now
// (Cache/Stamps/<scene>.models: path, size and date of each).
// - One cook per scene at a time: Cache/Stamps/<scene>.cooking holds the cooking process's id,
//   and another process waits for it (then finds the scene cooked). A lock whose process is
//   gone is ignored.
// - The cooker reuses the texture pages of the project's earlier cooks (this scene's and the
//   other scenes' .fvt), so only new or changed models' textures are encoded.
// - The cook is written to Cache/Cook/ and then moved into place. With `install` false (the
//   editor, which has the scene open) it stays there, and the next process that opens the
//   scene moves it in.
// - `error`: why it failed, from the cooker's own messages (or "Cancelled").
bool cookProjectScene(const Project& project, const std::string& relative, std::string& error,
                      const CookProgressFn& progress = {}, bool install = true);
// Does scene `relative` need a cook before it opens (no .fpak, or cooked with other models)?
// One already cooked and waiting to be moved in doesn't.
bool sceneNeedsCook(const Project& project, const std::string& relative);
// Were models (or material assets, or their textures) added, removed or changed since scene
// `relative` was cooked? (The editor offers to reload it.)
bool modelsChanged(const std::string& projectDir, const std::string& relative);
// A path made absolute and normal ('/' separators, no trailing one): how project folders compare.
std::string absolutePath(const std::string& path);
// A name as a file or folder name: letters, digits, spaces, '-' and '_' kept, the rest dropped
// (empty when nothing is left).
std::string fileSafeName(const std::string& name);

struct RecentProject {
    std::string dir, name;
    int64_t opened = 0;  // seconds since 1970
};
// The recently opened projects, newest first. `persistent` false: kept in memory only (tests).
class RecentProjects {
public:
    explicit RecentProjects(bool persistent = true);
    const std::vector<RecentProject>& list() const { return list_; }
    void touch(const Project& project);  // moved to the front, saved
    void forget(const std::string& dir);
    bool contains(const std::string& dir) const;

private:
    void save() const;
    std::vector<RecentProject> list_;
    std::string path_;
};

}  // namespace fx
