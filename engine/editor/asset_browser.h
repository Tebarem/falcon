#pragma once

// The asset browser's engine side (the Project panel, ui/editor/assets.js): lists the project's
// files and folders (a scene's .fpak, .fvt, edits and terrain are one item), does the panel's
// file operations, and reads images for it. Without a project it lists the scene's folder,
// read-only.
//
// Page -> engine:
//   editor.assets.watch {on}            the Project tab shows: rescan about once a second, send changes
//   editor.assets.refresh               send the list now
//   editor.assets.newFolder {parent, name?}
//   editor.assets.rename {path, name}   a scene's name has no extension (its companions follow)
//   editor.assets.move {paths, to}      into folder `to` ("" = the project's root)
//   editor.assets.duplicate {paths}
//   editor.assets.delete {paths}        to the Recycle Bin (tests: deleted outright)
//   editor.assets.reveal {path}         Explorer, the item selected
//   editor.assets.copyPath {path}       its absolute path, to the clipboard
//   editor.assets.read {path}           an image, at most 256 px: editor.assets.image {path, url, width, height}
//   editor.assets.previews {keys}       rendered previews ("mesh:<name>", "model:<path>",
//                                       "material:<path>", "scene:<path>"): each
//                                       answered with editor.assets.preview {key, url} once there is one
//   editor.assets.regenerate {keys}     render those again
//   editor.assets.folder {path}         the folder the page shows (files dropped on the window go there)
//   editor.assets.newMaterial {folder}  a material asset ("New Material.fmat") -> editor.assets.done
//   editor.assets.readMaterial {path}   -> editor.assets.material {path, material: {...the .fmat's values}}
//   editor.assets.import {folder}       a file picker; what is picked is imported
//   editor.assets.importFiles {paths, folder}  files (or folders) copied into the project: into
//                                       `folder` when it is in Assets/ (or My Library), else
//                                       Assets/; a .gltf with files of its own (buffers, images)
//                                       gets a folder for them
//   editor.assets.addToLibrary {paths, folder?}  project items copied into My Library (its folder
//                                       `folder`): a material with its textures (a folder of its
//                                       own), images, models and clips
//   editor.assets.use {path, id}        a library item's copy in the project (copied now unless it
//                                       is there): editor.assets.used {id, path, project?, copied}
//   editor.assets.libraryFolder {path?} My Library's folder (no path: a folder picker), kept in
//                                       the editor prefs
// Engine -> page:
//   editor.assets {root, project, truncated, stale, items: [[path, kind, size, modified, flags]],
//                  meshes: [[name, group, source]], libraries: [{prefix, name, folder, readOnly}]}
//     stale: models were added, removed or changed since the open scene was cooked (reload it);
//     path: relative to the root, '/' separated, or a library's ("@falcon/Wood/Oak.fmat",
//     project/asset_library.h; each library's own folder is an item too); kind: folder, scene,
//     model, material, prefab, image, script, data, json, project, file; size in bytes (a scene:
//     its .fpak + .fvt); modified: seconds since 1970; flags: kHidden | kStartup | kCurrent |
//     kInProject | kLibrary | kReadOnly (Falcon's); meshes: the open scene's meshes by group
//     (shape, rock, model, scene) and the model each came from
//   editor.assets.done {op, paths}      what an operation made (the page selects it)
//   editor.assets.preview {key, url}    a preview (a PNG data URL)
//
// Previews are rendered once and cached (Cache/Thumbnails, index.json: key -> {png, hash}): a
// background `falcon --preview` (app/preview.h) renders what has no preview yet, the meshes on
// the open scene, a scene on its own. A mesh's hash is its geometry's, so it renders again only
// when the mesh changes; a scene's preview is retaken from the editor's view when it is saved.
// Nothing else (opening the project, importing other assets) renders anything again.
// Paths are refused outside the project and the libraries; the open scene (and folders holding
// it) can't be renamed, moved or deleted; a scene's companions and project.falcon follow every
// operation. My Library takes the same operations as the project (without one too); Falcon's is
// read-only; items move within one of them. A library material's picture (editor.assets.read)
// is its colour texture: previews render only what the open scene has cooked.

#include "project/project.h"

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fx {

class ScenePak;
class UiLayer;
struct Json;

class AssetBrowser {
public:
    enum Flags : uint32_t { kHidden = 1, kStartup = 2, kCurrent = 4, kInProject = 8, kLibrary = 16, kReadOnly = 32 };
    using Notice = std::function<void(const std::string& message, bool error)>;

    struct Init {
        UiLayer* ui = nullptr;
        const ScenePak* pak = nullptr;
        Project* project = nullptr;     // null: the scene's folder, read-only
        std::string sceneFolder;        // without a project: the folder listed
        std::string currentScene;       // the open scene, relative to the project
        std::string scenePath;          // the open scene's .fpak (mesh previews render from it)
        // A scene's last editor camera ({"position", "yaw", "pitch", ...} as JSON; empty: none).
        std::function<std::string(const std::string& scene)> cameraOf;
        SDL_Window* window = nullptr;   // the file picker's parent
        bool recycle = true;            // delete to the Recycle Bin (false: tests delete outright)
        Notice notice;
        std::function<void(const std::string& folder)> libraryFolderChanged;  // (the prefs keep it)
    };
    void init(const Init& init);
    ~AssetBrowser();  // a preview run still going is stopped (what it wrote stays)
    // editor.assets.* messages. True when the project's scenes changed (the bridge resends them).
    bool handle(const std::string& type, const Json& m);
    // Once a frame: the watch's rescans (on a worker thread), sent when something changed.
    void update();

    // Files dropped on the window (SDL_EVENT_DROP_FILE): imported into the folder the page shows.
    void dropped(const std::vector<std::string>& paths);

    // Tests: does `path` (relative to the root) exist?
    bool exists(const std::string& path) const;
    // The editor saved the open scene: where its new preview goes (the view, captured at the end
    // of this frame; empty without a project). The page gets it next frame.
    std::string sceneSaved();
    uint32_t previewRuns() const { return previewRuns_; }  // preview processes started
    bool previewsBusy() const { return previewProcess_ != nullptr || !queue_.empty(); }
    const std::string& root() const { return root_; }

    void rescanNow();  // the list, now (a file written outside the panel)

private:
    struct Item {
        std::string path;
        const char* kind = "file";
        uint64_t size = 0;
        int64_t modified = 0;
        uint32_t flags = 0;
    };
    struct Listing {
        std::vector<Item> items;
        uint64_t signature = 0;
        bool truncated = false, stale = false;
    };
    // What a scan needs from the project, copied: scans run on a worker thread.
    struct ScanInput {
        std::string root, startup, current;
        bool project = false;
        std::vector<std::string> scenes;
        std::vector<std::pair<std::string, std::string>> libraries;  // prefix, folder
    };
    ScanInput scanInput() const;
    static Listing scan(const ScanInput& in);
    void send(const Listing& listing);
    void done(const char* op, const std::vector<std::string>& paths);
    void fail(const std::string& message);

    // Path checks: an absolute path inside the root, or empty (and a notice).
    std::string resolve(const std::string& relative);
    bool editable(const std::string& relative);    // inside the project, not the open scene or a folder holding it
    bool canWriteIn(const std::string& folder);    // the project's (with one) or My Library's
    bool isScene(const std::string& relative) const;
    void setLibraryFolder(const std::string& folder);
    std::vector<std::string> companions(const std::string& fpakAbsolute) const;  // existing ones, the .fpak excluded
    // Moves (or renames) one item with its companions and updates the project's scenes.
    bool relocate(const std::string& from, const std::string& to, bool& projectChanged);
    void projectRenamed(const std::string& from, const std::string& to, bool folder, bool& projectChanged);
    std::string uniqueName(const std::string& folderAbsolute, const std::string& stem, const std::string& ext) const;
    bool remove(const std::vector<std::string>& absolutePaths);
    void readImage(const std::string& relative);
    std::vector<std::string> importFiles(const std::vector<std::string>& sources, const std::string& folder);
    std::string folder_;  // the page's folder
    std::mutex pickedMutex_;
    std::vector<std::string> picked_;  // the file picker's answer (on SDL's thread)
    bool pickedDone_ = false;
    std::string pickedLibrary_;        // the library folder picker's
    SDL_Window* window_ = nullptr;
    std::function<void(const std::string&)> libraryFolderChanged_;

    // Previews.
    struct PreviewEntry {
        std::string png, hash;  // the file in Cache/Thumbnails; what it was rendered from
    };
    struct PreviewRequest {
        std::string key, hash;
    };
    std::string thumbnails() const;  // Cache/Thumbnails (absolute)
    std::string previewFile(const std::string& key) const;
    std::string expectedHash(const std::string& key) const;  // empty: any (scenes)
    void loadIndex();
    void saveIndex() const;
    void requestPreviews(const std::vector<std::string>& keys);
    void sendPreview(const std::string& key, const std::string& png);
    void startPreviews();
    void finishPreviews(bool ok);
    std::unordered_map<std::string, PreviewEntry> index_;
    bool indexLoaded_ = false;
    std::vector<PreviewRequest> queue_, running_;
    std::unordered_set<std::string> queued_, failed_;
    void* previewProcess_ = nullptr;
    std::string jobsFile_;
    uint32_t previewRuns_ = 0;
    std::string scenePath_;
    std::function<std::string(const std::string&)> cameraOf_;
    std::string sceneCapture_;  // a saved scene's preview, being written this frame

    UiLayer* ui_ = nullptr;
    const ScenePak* pak_ = nullptr;
    Project* project_ = nullptr;
    std::string root_, currentScene_;
    bool recycle_ = true;
    Notice notice_;
    bool watching_ = false;
    uint64_t lastScanNs_ = 0, sentSignature_ = 0;
    std::future<Listing> scanning_;
};

}  // namespace fx
