#pragma once

// Asset previews: `falcon --hidden --preview jobs.json` renders one asset after another from a
// cooked scene with the real renderer and writes each as a PNG (the editor's asset browser runs
// it in the background and caches what it writes; editor/asset_browser.h).
//
// jobs.json (one scene per file; every job uses it):
//   {"scene": "<path>.fpak", "edits": false, "terrain": false, "root": "<project folder>",
//    "jobs": [{"kind": "mesh", "mesh": "<mesh name>", "out": "<path>.png", "size": 256},
//             {"kind": "model", "model": "<library source>", "out": ..., "size": 256},
//             {"kind": "material", "material": "<.fmat, relative to root>", "out": ..., "size": 256},
//             {"kind": "prefab", "prefab": "<.fprefab, relative to root>", "out": ..., "size": 256},
//             {"kind": "scene", "out": "<path>.png", "size": 256,
//              "pose": {"position": [x, y, z], "yaw": 0, "pitch": 0}}]}
// A mesh (or a model: its nodes, as cooked from a --library) stands on a grey ground at the
// origin, seen from the front-right and a little above, framed to its bounds. A material is a
// sphere drawn with it (cooked into the scene: with its textures; else its factors alone). A
// prefab is its meshes (with their materials), placed as in the prefab. A scene is seen from `pose` (the editor's last camera there) or, without
// one, from where the game would start. Each job waits for streaming and shadows to settle; the
// image is rendered larger than `size` and resampled down. PNGs are written beside and renamed
// into place, so a reader never sees half a file.

#include "scene/fly_camera.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

class MaterialLibrary;
class ScenePak;
struct InstanceUpdate;

struct PreviewJob {
    std::string kind;   // mesh | model | scene
    std::string mesh;   // kind mesh: its name in the scene
    std::string model;  // kind model: its library's source (ScenePak::library)
    std::string material;  // kind material: its source (.fmat, relative to the root)
    std::string prefab;    // kind prefab: its source (.fprefab, relative to the root)
    int32_t materialIndex = -1;
    // Kind prefab: its meshes (resolved at reserve()), each with its transform and material.
    struct Part {
        uint32_t mesh;
        float world[3][4];
        int32_t material = -1;
    };
    std::vector<Part> parts;
    std::string out;
    uint32_t size = 256;
    bool hasPose = false;
    FlyCamera::Pose pose = {};
};

class PreviewSession {
public:
    // The jobs file; false (logged) when it can't be read.
    bool load(const char* path);
    const std::string& scene() const { return scene_; }
    bool edits() const { return edits_; }
    bool terrain() const { return terrain_; }
    const std::string& root() const { return root_; }  // the project (material jobs' sources)
    uint32_t spareMaterials() const;  // material slots a run may need (material jobs, prefabs' materials)
    static constexpr uint32_t kRenderSize = 640;  // the window (and render) size; previews are scaled down

    // Before ScenePak::upload(): an instance slot per mesh to preview, and the ground disc's.
    void reserve(ScenePak& pak, MaterialLibrary& materials);
    // Once a frame, before rendering: places the current job (instance rows, the camera) and
    // decides when to capture it. `idleFrames`: frames that streaming and shadows were idle.
    void update(const ScenePak& pak, FlyCamera& camera, std::vector<InstanceUpdate>& rows, uint32_t idleFrames);
    // This frame's capture: where (empty: none) and how big. Then captured(ok).
    std::string captureTarget() const;
    uint32_t captureSize() const;
    void captured(bool ok);
    bool done() const { return next_ >= jobs_.size() && current_ < 0; }
    uint32_t failures() const { return failures_; }

private:
    bool place(const ScenePak& pak, FlyCamera& camera, std::vector<InstanceUpdate>& rows);
    void hide(const ScenePak& pak, std::vector<InstanceUpdate>& rows);

    std::string scene_, root_;
    bool edits_ = false, terrain_ = false;
    std::vector<PreviewJob> jobs_;
    // The meshes a job shows, each with its transform (model space) and material.
    using Part = PreviewJob::Part;
    std::vector<Part> partsOf(const ScenePak& pak, const PreviewJob& job) const;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> slots_;  // mesh -> its reserved slots
    uint32_t groundSlot_ = ~0u;
    std::vector<uint32_t> shown_;  // slots the current job shows
    FlyCamera::Pose startPose_ = {};
    bool hasStartPose_ = false;
    int current_ = -1;  // the job being rendered (-1: none)
    size_t next_ = 0;
    uint32_t framesPlaced_ = 0, failures_ = 0;
    bool capture_ = false;
};

}  // namespace fx
