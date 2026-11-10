#include "editor/editor_bridge.h"

#include "anim/animation_system.h"
#include "core/platform.h"
#include "anim/graph_library.h"
#include "assets/scene_pak.h"
#include "assets/acoustic_material.h"
#include "assets/sound_asset.h"
#include "audio/audio_system.h"
#include "core/boot.h"
#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/editor_prefs.h"
#include "editor/material_library.h"
#include "level/level_mask.h"
#include "level/level_meshes.h"
#include "level/mesh_ops.h"
#include "level/prototype_materials.h"
#include "level/smart_material.h"
#include "level/wear.h"
#include "level/trim_sheet.h"
#include "project/asset_library.h"
#include "assets/material_asset.h"
#include "editor/scene_view.h"
#include "editor/terrain_editor.h"
#include "app/process.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "terrain/terrain_scatter.h"
#include "gpu/device.h"
#include "render/gpu_driven_renderer.h"
#include "render/light_system.h"
#include "render/shader_file.h"
#include "render/skinning.h"
#include "render/virtual_shadow_map.h"
#include "scene/fly_camera.h"
#include "ui/ui_layer.h"
#include "vfx/particle_system.h"
#include "vfx/runtime_meshes.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace fx {

namespace {

constexpr float kDeg = 57.29577951308232f;
constexpr size_t kMaxListedEntities = 8192;       // keeps editor.state well under cef-lite's 1 MB

// World rows (3x4) <-> position, XYZ euler degrees (R = Rz * Ry * Rx), per-axis scale.
void decompose(const float m[3][4], float pos[3], float rot[3], float scale[3])
{
    float r[3][3];
    for (int c = 0; c < 3; ++c) {
        scale[c] = std::sqrt(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
        for (int row = 0; row < 3; ++row)
            r[row][c] = scale[c] > 0.0f ? m[row][c] / scale[c] : (row == c ? 1.0f : 0.0f);
    }
    // A mirrored matrix: put the reflection into the x scale.
    const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                      r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.0f) {
        scale[0] = -scale[0];
        for (int row = 0; row < 3; ++row)
            r[row][0] = -r[row][0];
    }
    for (int a = 0; a < 3; ++a)
        pos[a] = m[a][3];
    const float sy = std::clamp(-r[2][0], -1.0f, 1.0f);
    rot[1] = std::asin(sy);
    if (std::fabs(sy) < 0.9999f) {
        rot[0] = std::atan2(r[2][1], r[2][2]);
        rot[2] = std::atan2(r[1][0], r[0][0]);
    } else {  // gimbal lock: fold z into x
        rot[0] = std::atan2(-r[1][2], r[1][1]);
        rot[2] = 0.0f;
    }
    for (int a = 0; a < 3; ++a)
        rot[a] *= kDeg;
}

void compose(const float pos[3], const float rot[3], const float scale[3], float m[3][4])
{
    const float cx = std::cos(rot[0] / kDeg), sx = std::sin(rot[0] / kDeg);
    const float cy = std::cos(rot[1] / kDeg), sy = std::sin(rot[1] / kDeg);
    const float cz = std::cos(rot[2] / kDeg), sz = std::sin(rot[2] / kDeg);
    const float r[3][3] = {
        {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
        {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
        {-sy, cy * sx, cy * cx},
    };
    for (int row = 0; row < 3; ++row) {
        for (int c = 0; c < 3; ++c)
            m[row][c] = r[row][c] * scale[c];
        m[row][3] = pos[row];
    }
}

// Two JSON values alike, numbers within 1e-4 (as floats round trip through doubles).
bool jsonNear(const Json& a, const Json& b)
{
    if (a.type == Json::Type::Number && b.type == Json::Type::Number)
        return std::fabs(a.number - b.number) <= 1e-4 * std::max(1.0, std::fabs(b.number));
    if (a.type != b.type)
        return false;
    switch (a.type) {
    case Json::Type::Bool: return a.b == b.b;
    case Json::Type::String: return a.str == b.str;
    case Json::Type::Array:
        if (a.array.size() != b.array.size())
            return false;
        for (size_t i = 0; i < a.array.size(); ++i)
            if (!jsonNear(a.array[i], b.array[i]))
                return false;
        return true;
    case Json::Type::Object:
        for (const auto& [key, v] : b.object)
            if (const Json* o = a.get(key.c_str()); !o || !jsonNear(*o, v))
                return false;
        return true;
    default: return true;
    }
}

bool readVec3(const Json& m, const char* key, float out[3])
{
    const Json* v = m.get(key);
    if (!v || v->array.size() != 3)
        return false;
    for (int a = 0; a < 3; ++a) {
        if (v->array[a].type != Json::Type::Number || !std::isfinite(v->array[a].number))
            return false;
        out[a] = float(v->array[a].number);
    }
    return true;
}

void writeVec3(JsonWriter& w, const char* key, const float v[3])
{
    w.beginArray(key);
    for (int a = 0; a < 3; ++a)
        w.value(nullptr, double(v[a]));
    w.endArray();
}

// The upscaling preset nearest a render scale (the page's Quality select).
uint32_t upscalePreset(float scale)
{
    uint32_t best = 0;
    for (uint32_t k = 1; k < 5; ++k)
        if (std::fabs(kUpscalePresets[k] - scale) < std::fabs(kUpscalePresets[best] - scale))
            best = k;
    return best;
}

// Light types as the page and the edits file name them.
const char* lightTypeName(uint32_t type) { return type == 2 ? "directional" : type == 1 ? "spot" : "point"; }
uint32_t lightTypeOf(const std::string& name) { return name == "directional" ? 2u : name == "spot" ? 1u : 0u; }

// Unity-like defaults: white, a 30 degree spot that casts shadows; no range (0: it reaches as
// far as its light shows, from its intensity).
Light defaultLight(uint32_t type)
{
    Light l = {};
    l.type = type;
    l.color[0] = l.color[1] = l.color[2] = 1.0f;
    l.intensity = l.type == 2 ? 3.2f : 10.0f;  // a sun: about the default sun's irradiance
    if (l.type == 2) {
        l.color[1] = 0.95f;
        l.color[2] = 0.85f;
    }
    l.range = 0.0f;
    l.innerAngle = 20.0f;
    l.outerAngle = 30.0f;
    l.flags = 1u;
    return l;
}

// "Point Light 2", "Spot Light 1"; the sun is "Directional Light".
std::string cookedLightName(uint32_t type, uint32_t number)
{
    if (type == 2)
        return number <= 1 ? "Directional Light" : "Directional Light " + std::to_string(number);
    return (type == 1 ? "Spot Light " : "Point Light ") + std::to_string(number);
}

// (X2) A data component field's kind: its lanes and each lane's bytes (0: not one the editor shows).
void dataKind(const std::string& kind, uint32_t& lanes, uint32_t& bytes)
{
    lanes = kind == "vec2" ? 2 : kind == "vec3" ? 3 : kind == "vec4" || kind == "quat" ? 4 : 1;
    bytes = kind == "double" || kind == "long" || kind == "ulong" || kind == "entity" ? 8
          : kind == "byte" || kind == "bool" ? 1
          : kind == "float" || kind == "int" || kind == "uint" || lanes > 1 ? 4 : 0;
}

double readDataLane(const std::string& kind, const uint8_t* at)
{
    const auto get = [&]<typename T>(T) { T v; std::memcpy(&v, at, sizeof(T)); return double(v); };
    if (kind == "double") return get(double());
    if (kind == "int") return get(int32_t());
    if (kind == "uint") return get(uint32_t());
    if (kind == "long") return get(int64_t());
    if (kind == "ulong" || kind == "entity") return get(uint64_t());
    if (kind == "byte") return get(uint8_t());
    if (kind == "bool") return *at ? 1.0 : 0.0;
    return get(float());
}

void writeDataLane(const std::string& kind, uint8_t* at, double v)
{
    const auto put = [&]<typename T>(T x) { std::memcpy(at, &x, sizeof(T)); };
    if (kind == "double") put(v);
    else if (kind == "int") put(int32_t(std::llround(v)));
    else if (kind == "uint") put(uint32_t(std::llround(std::max(v, 0.0))));
    else if (kind == "long") put(int64_t(std::llround(v)));
    else if (kind == "ulong") put(uint64_t(std::llround(std::max(v, 0.0))));
    else if (kind == "byte") put(uint8_t(std::clamp(std::llround(v), 0ll, 255ll)));
    else if (kind == "bool") put(uint8_t(v != 0.0));
    else put(float(v));
}

// A level "material" op's material's real-world size (its .fmat's "size"): its faces' UV scale.
void addMaterialSize(const Project& project, Json& args)
{
    const std::string path = args.getString("path");
    MaterialAsset asset;
    std::string error;
    if (path.empty() || !readMaterialAsset(project.path(path), asset, error) || asset.size[0] <= 0.0f || asset.size[1] <= 0.0f)
        return;
    Json size = Json::makeObject();
    size.type = Json::Type::Array;
    size.array = {Json::makeNumber(asset.size[0]), Json::makeNumber(asset.size[1])};
    args.set("size", std::move(size));
}

}  // namespace

void EditorBridge::init(UiLayer& ui, const Init& init)
{
    ui_ = &ui;
    backend_ = init.backend;
    settings_ = init.settings;
    editorApi_.version = kFxScriptApiVersion;
    editorApi_.size = sizeof(FxEditorApi);
    editorApi_.context = this;
    editorApi_.message = [](void* c, const char* json, int32_t length) {
        auto* self = static_cast<EditorBridge*>(c);
        try {
            self->handle(Json::parse(std::string_view(json, size_t(std::max(length, 0)))), self->lastCamera_);
        } catch (const std::exception& e) {
            self->notice((std::string("Editor code: ") + e.what()).c_str(), "error");
        }
    };
    editorApi_.query = [](void* c, const char* what, char* buffer, int32_t capacity) -> int32_t {
        const std::string text = static_cast<EditorBridge*>(c)->editorQuery(what ? what : "");
        const int32_t n = int32_t(text.size());
        if (!buffer || capacity < n)
            return -n;
        std::memcpy(buffer, text.data(), size_t(n));
        return n;
    };
    editorApi_.undoBegin = [](void* c, const char*) { static_cast<EditorBridge*>(c)->undoBegin(); };
    editorApi_.undoEnd = [](void* c) { static_cast<EditorBridge*>(c)->undoEnd(); };
    editorApi_.entityOf = [](void* c, uint32_t id) -> uint64_t {
        auto* self = static_cast<EditorBridge*>(c);
        const flecs::entity e = id && id <= self->records_.size() ? self->entityOf(id - 1) : flecs::entity();
        return e ? e.id() : 0;
    };
    editorApi_.idOf = [](void* c, uint64_t entity) -> uint32_t {
        auto* self = static_cast<EditorBridge*>(c);
        for (uint32_t i = 0; i < self->records_.size(); ++i)
            if (const flecs::entity e = self->entityOf(i); e && e.id() == entity)
                return i + 1;
        return 0;
    };
    gpu_ = init.gpu;
    prefs_ = init.prefs;
    audio_ = init.audio;
    relaunch_ = init.relaunch;
    if (init.projectDir) {
        std::string error;
        hasProject_ = loadProject(init.projectDir, project_, error);
        if (!hasProject_)
            FX_WARN("editor: %s", error.c_str());
        if (hasProject_)
            shaders_.setProject(project_.dir);
    }
    // Engine and script log lines also go to the page's console.
    SDL_GetLogOutputFunction(&previousLog_, &previousLogData_);
    SDL_SetLogOutputFunction(&EditorBridge::logOutput, this);
    logHooked_ = true;
    attachScene(init);
}

// What a game's Scene.Load left while playing: the scene as the editor had it.
struct EditorBridge::Kept {
    std::string scene;  // in the project
    std::string edits;  // its edits (as before Play)
    bool dirty = false;
    uint32_t selected = ~0u;
    std::vector<Command> undo, redo;
};

void EditorBridge::attachScene(const Init& init)
{
    scene_ = init.scene;
    pak_ = init.pak;
    scenePath_ = init.scenePath ? init.scenePath : "";
    editsPath_ = SceneEdits::pathFor(scenePath_.c_str());
    pools_ = init.pools;
    lights_ = init.lights;
    vsm_ = init.vsm;
    camera_ = init.camera;
    renderer_ = init.renderer;
    terrain_ = init.terrain;
    materials_ = init.materials;
    water_ = init.water;
    particles_ = init.particles;
    runtimeMeshes_ = init.runtimeMeshes;
    levelMeshes_ = init.levelMeshes;
    animation_ = init.animation;
    graphs_ = init.graphs;
    skinning_ = init.skinning;
    bones_ = init.bones;
    animEditor_.init(init.animPreview, init.graphs, init.animation, init.pak, init.projectDir ? init.projectDir : "");
    sceneKey_ = std::filesystem::path(scenePath_).filename().string();
    if (hasProject_) {
        sceneInProject_ = std::filesystem::path(scenePath_).lexically_relative(project_.dir).generic_string();
        sceneKey_ = project_.dir + "|" + sceneInProject_;  // (two projects' Scenes/Main.fpak apart)
    }
    // The Project panel: the project's files (or, without one, the scene's folder, read-only).
    AssetBrowser::Init ai;
    ai.ui = ui_;
    ai.pak = pak_;
    ai.project = hasProject_ ? &project_ : nullptr;
    ai.sceneFolder = std::filesystem::path(scenePath_).parent_path().generic_string();
    if (ai.sceneFolder.empty())
        ai.sceneFolder = ".";
    ai.currentScene = sceneInProject_;
    ai.recycle = prefs_ != nullptr;  // --no-prefs (tests): deleted outright, in the test's own folder
    ai.notice = [this](const std::string& message, bool error) { notice(message.c_str(), error ? "error" : "info"); };
    ai.scenePath = std::filesystem::absolute(scenePath_).lexically_normal().generic_string();
    ai.window = ui_->window();
    ai.cameraOf = [this](const std::string& scene) -> std::string {  // the editor's last view of it
        if (!prefs_ || !hasProject_)
            return {};
        const Json* cam = prefs_->scene(project_.dir + "|" + scene).get("camera");
        return cam && cam->isObject() ? cam->dump(0) : std::string();
    };
    ai.libraryFolderChanged = [this](const std::string& folder) {
        if (!prefs_)
            return;
        prefs_->section("library").set("folder", Json::makeString(folder));
        prefs_->touch();
    };
    assets_.init(ai);
    // Entities: cooked instances first (id = slot + 1), then the created ones as applied.
    const uint32_t cooked = pak_->cookedInstanceCount();
    for (uint32_t slot = 0; slot < cooked; ++slot) {
        Record r;
        r.entity = scene_->entity(slot).id();
        r.slot = slot;
        addRecord(std::move(r));
    }
    // Cooked point / spot lights, named per type in pak order.
    uint32_t lightNumber[3] = {0, 0, 0};
    for (uint32_t i = 0; i < scene_->cookedLightCount(); ++i) {
        const flecs::entity e = scene_->lightEntity(i);
        if (!e)
            continue;
        Light cooked;
        float world[3][4];
        scene_->cookedLight(i, cooked, world);
        Record r;
        r.entity = e.id();
        r.kind = EntityKind::Light;
        r.light = i;
        r.primitive = lightTypeName(e.try_get<Light>()->type);
        r.name = cookedLightName(cooked.type, ++lightNumber[std::min(cooked.type, 2u)]);  // by the cooked type
        addRecord(std::move(r));
    }
    if (init.edits) {
        for (const auto* list : {&init.edits->instances, &init.edits->lights, &init.edits->created})
            for (const EntityEdit& e : *list)
                for (const DataComponent& d : e.data)
                    dataLayouts_.try_emplace(d.name, d.size, d.align);
        for (const EntityEdit& e : init.edits->instances)
            if (e.slot < cooked) {
                records_[e.slot].name = e.name;
                records_[e.slot].hidden = e.hidden;
                records_[e.slot].material = e.material;
                if (e.hasAnimator)
                    records_[e.slot].animator = e.animator;
                records_[e.slot].scripts = e.scripts;
            }
        for (const EntityEdit& e : init.edits->lights)
            if (auto it = indexOf_.find(e.entity); e.entity && it != indexOf_.end()) {
                if (!e.name.empty())
                    records_[it->second].name = e.name;
                records_[it->second].hidden = e.hidden;
            }
        for (const EntityEdit& e : init.edits->created) {
            if (!e.entity)
                continue;
            Record r;
            r.entity = e.entity;
            r.kind = entityKindFromName(e.kind);
            r.slot = e.slot;
            r.uid = e.uid;
            r.primitive = r.kind == EntityKind::Light ? lightTypeName(e.light.type) : e.primitive;
            r.name = e.name;
            r.hidden = e.hidden;
            r.material = e.material;
            r.prefab = e.prefab;
            r.prefabChild = e.prefabChild;
            r.model = e.model;
            r.modelNode = e.modelNode;
            r.modelCamera = e.modelCamera;
            if (e.hasAnimator)
                r.animator = e.animator;
            r.scripts = e.scripts;
            addRecord(std::move(r));
        }
        // Prefab instances: each entity's root, by the root's uid.
        std::unordered_map<uint32_t, uint32_t> byUid;
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (records_[i].uid)
                byUid[records_[i].uid] = i;
        for (const EntityEdit& e : init.edits->created)
            if (e.prefabRoot && e.entity)
                if (const auto me = indexOf_.find(e.entity); me != indexOf_.end())
                    if (const auto root = byUid.find(e.prefabRoot); root != byUid.end())
                        records_[me->second].prefabRoot = root->second;
        nextUid_ = init.edits->nextUid;
    }
    // Skeletons expanded at load: their bones' records, after their characters'.
    if (bones_)
        for (const BoneEntities::Expanded& x : bones_->all())
            if (const auto root = indexOf_.find(x.root); root != indexOf_.end())
                addBoneRecords(root->second, x);
    applyPrefs();
    // Coming back to the scene a game's Scene.Load left: as the editor had it.
    if (kept_ && std::exchange(returning_, false) && kept_->scene == sceneInProject_) {
        dirty_ = kept_->dirty;
        selected_ = kept_->selected < records_.size() ? kept_->selected : ~0u;
        undo_ = std::move(kept_->undo);
        redo_ = std::move(kept_->redo);
        kept_.reset();
    }
    if (connected_) {  // (a scene after another: the page is there already)
        assets_.rescanNow();
        sendState(true);
    }
}

void EditorBridge::detachScene()
{
    persist();  // (this scene's camera and sun)
    view_ = nullptr;
    scene_ = nullptr;
    pak_ = nullptr;
    pools_ = nullptr;
    lights_ = nullptr;
    vsm_ = nullptr;
    camera_ = nullptr;
    renderer_ = nullptr;
    terrain_ = nullptr;
    materials_ = nullptr;
    water_ = nullptr;
    particles_ = nullptr;
    runtimeMeshes_ = nullptr;
    levelMeshes_ = nullptr;
    animation_ = nullptr;
    graphs_ = nullptr;
    skinning_ = nullptr;
    bones_ = nullptr;
    animEditor_.init(nullptr, nullptr, nullptr, nullptr, hasProject_ ? project_.dir : "");
    records_.clear();
    indexOf_.clear();
    indexOfSlot_.clear();
    selected_ = eventSelected_ = ~0u;
    undo_.clear();
    redo_.clear();
    undoGroupDepth_ = 0;
    undoGroupStart_ = 0;
    streamed_.clear();
    playSnapshot_.clear();
    previewBase_.reset();
    previewIndex_ = ~0u;
    liveDirty_ = liveMoved_ = false;
    dirty_ = false;
    nextUid_ = 1;
    debugLineCount_ = 0;
    bakeSent_.clear();
    ++sceneSwitches_;
}

bool EditorBridge::keepForPlay()
{
    if (kept_)  // (a second Scene.Load: Stop still comes back to the scene being edited)
        return true;
    if (terrain_ && terrain_->dirty()) {
        stopPlaying();
        notice("The game loaded another scene: save the terrain first (its changes can't be kept meanwhile); Play stopped", "error");
        return false;
    }
    restorePlay();  // (the scene as before Play: what its edits keep)
    kept_ = std::make_unique<Kept>();
    kept_->scene = sceneInProject_;
    kept_->edits = currentEdits().text();
    kept_->dirty = dirty_;
    kept_->selected = selected_;
    kept_->undo = std::move(undo_);
    kept_->redo = std::move(redo_);
    // (Terrain strokes are the terrain editor's: they don't come back.)
    for (auto* list : {&kept_->undo, &kept_->redo})
        list->erase(std::remove_if(list->begin(), list->end(),
                                   [](const Command& c) {
                                       return std::any_of(c.begin(), c.end(), [](const Change& x) { return x.terrainEdit != ~0u; });
                                   }),
                    list->end());
    return true;
}

std::string EditorBridge::keptScene() const { return kept_ ? kept_->scene : std::string(); }

std::string EditorBridge::playConditions() const
{
    // (the Play bar's Conditions: none, a LAN, the internet, a bad connection; each way)
    static const char* const kConditions[] = {"", "5,1,0", "50,10,1", "125,30,5"};
    return kConditions[std::min<uint32_t>(playConditions_, 3)];
}

std::string EditorBridge::comeBack()
{
    returning_ = kept_ != nullptr;
    return kept_ ? kept_->edits : std::string();
}

void EditorBridge::applyPrefs()
{
    if (!prefs_)
        return;
    for (const auto& [key, value] : prefs_->section("settings").object)
        if (!(hasProject_ && projectSetting(key)))  // (the project's, below)
            setSetting(key, value);
    if (hasProject_) {
        setSetting("lodPixels", Json::makeNumber(project_.lodPixels));
        setSetting("cullPixels", Json::makeNumber(project_.cullPixels));
        setSetting("terrainPixels", Json::makeNumber(project_.terrainPixels));
        setSetting("detailDistance", Json::makeNumber(project_.detailDistance));
        projectSettingsDirty_ = false;
    }
    const Json& scene = prefs_->scene(sceneKey_);
    if (const Json* cam = scene.get("camera"); cam && cam->isObject() && camera_) {
        const Json* p = cam->get("position");
        if (p && p->array.size() == 3) {
            FlyCamera::Pose pose = camera_->pose();
            pose.position = {float(p->array[0].number), float(p->array[1].number), float(p->array[2].number)};
            pose.yaw = float(cam->getNumber("yaw", pose.yaw));
            pose.pitch = float(cam->getNumber("pitch", pose.pitch));
            pose.pivotDistance = float(cam->getNumber("pivotDistance", pose.pivotDistance));
            pose.speed = float(cam->getNumber("speed", pose.speed));
            camera_->setPose(pose);
        }
    }
}

void EditorBridge::setSceneView(SceneView* view)
{
    view_ = view;
    if (!view_ || !prefs_)
        return;
    SceneToolState& t = view_->tools();
    const Json& p = prefs_->section("tools");
    const std::string tool = p.getString("tool");
    if (tool == "view") t.tool = SceneTool::View;
    else if (tool == "move") t.tool = SceneTool::Move;
    else if (tool == "rotate") t.tool = SceneTool::Rotate;
    else if (tool == "scale") t.tool = SceneTool::Scale;
    else if (tool == "brush") t.tool = SceneTool::Brush;
    t.local = p.getBool("local", t.local);
    t.pivotCenter = p.getBool("pivotCenter", t.pivotCenter);
    t.grid = p.getBool("grid", t.grid);
    t.snapMove = float(p.getNumber("snapMove", t.snapMove));
    t.snapRotate = float(p.getNumber("snapRotate", t.snapRotate));
    t.snapScale = float(p.getNumber("snapScale", t.snapScale));
    t.snapAbsolute = p.getBool("snapAbsolute", t.snapAbsolute);
}

void EditorBridge::persist()
{
    if (!prefs_)
        return;
    // Rebuild the sections from the live state; only a real change marks the file dirty.
    const std::string before = prefs_->section("settings").dump(0) + prefs_->section("tools").dump(0) +
                               prefs_->scene(sceneKey_).dump(0);
    JsonWriter w;
    w.beginObject();
    writeSettings(w);
    w.endObject();
    Json settings = *Json::parse(w.str()).get("settings");
    settings.object.erase(std::remove_if(settings.object.begin(), settings.object.end(),
                                         [&](const auto& kv) {
                                             return kv.first.starts_with("sun") || kv.first == "upscalerName" ||
                                                    kv.first == "projectSettings" || (hasProject_ && projectSetting(kv.first));
                                         }),
                          settings.object.end());
    prefs_->section("settings") = settings;
    if (view_) {
        JsonWriter t;
        t.beginObject();
        writeTools(t);
        t.endObject();
        Json tools = *Json::parse(t.str()).get("tools");
        tools.object.erase(std::remove_if(tools.object.begin(), tools.object.end(),
                                          [](const auto& kv) { return kv.first == "gameView"; }),
                           tools.object.end());
        prefs_->section("tools") = tools;
    }
    Json& scene = prefs_->scene(sceneKey_);
    scene.object.erase(std::remove_if(scene.object.begin(), scene.object.end(), [](const auto& kv) { return kv.first == "sun"; }),
                       scene.object.end());  // the sun is a scene entity now (saved with the edits)
    if (camera_) {
        const FlyCamera::Pose p = camera_->pose();
        Json& cam = scene.child("camera");
        Json pos;
        pos.type = Json::Type::Array;
        pos.array = {Json::makeNumber(p.position.x), Json::makeNumber(p.position.y), Json::makeNumber(p.position.z)};
        cam.set("position", pos);
        cam.set("yaw", Json::makeNumber(p.yaw));
        cam.set("pitch", Json::makeNumber(p.pitch));
        cam.set("pivotDistance", Json::makeNumber(p.pivotDistance));
        cam.set("speed", Json::makeNumber(p.speed));
    }
    const std::string after = prefs_->section("settings").dump(0) + prefs_->section("tools").dump(0) +
                              prefs_->scene(sceneKey_).dump(0);
    if (after != before)
        prefs_->touch();
}

EditorBridge::EditorBridge() = default;  // (here: Kept is complete)

EditorBridge::~EditorBridge()
{
    if (cook_) {  // (the cooker is stopped)
        cook_->cancel = true;
        cook_->done.wait();
    }
    if (logHooked_)
        SDL_SetLogOutputFunction(previousLog_, previousLogData_);
}

void SDLCALL EditorBridge::logOutput(void* user, int category, SDL_LogPriority priority, const char* message)
{
    auto* self = static_cast<EditorBridge*>(user);
    if (self->previousLog_)
        self->previousLog_(self->previousLogData_, category, priority, message);
    // The periodic stats lines are the profiler's; the console keeps events.
    static const char* const kStats[] = {"frame:", "vsm:", "vt:", "gpu:", "physics:", "animation:", "scripts:", "ui:"};
    for (const char* p : kStats)
        if (std::strncmp(message, p, std::strlen(p)) == 0)
            return;
    std::lock_guard lock(self->logMutex_);
    if (self->log_.size() < 512)
        self->log_.emplace_back(int(priority), message);
}

void EditorBridge::flushLog()
{
    std::vector<std::pair<int, std::string>> lines;
    {
        std::lock_guard lock(logMutex_);
        lines.swap(log_);
    }
    if (lines.empty() || !connected_)
        return;
    JsonWriter w;
    w.beginObject().value("type", "editor.log").beginArray("entries");
    for (const auto& [priority, text] : lines) {
        const char* level = priority >= SDL_LOG_PRIORITY_ERROR ? "error" : priority == SDL_LOG_PRIORITY_WARN ? "warning" : "info";
        w.beginObject().value("level", level).value("message", text).endObject();
    }
    w.endArray().endObject();
    ui_->send(w.str());
}

uint32_t EditorBridge::addRecord(Record r)
{
    const uint32_t index = uint32_t(records_.size());
    if (r.entity)
        indexOf_[r.entity] = index;
    if (r.slot != ~0u) {
        if (r.slot >= indexOfSlot_.size())
            indexOfSlot_.resize(r.slot + 1, ~0u);
        indexOfSlot_[r.slot] = index;
    }
    records_.push_back(std::move(r));
    return index;
}

flecs::entity EditorBridge::entityOf(uint32_t index) const
{
    if (index >= records_.size() || !records_[index].entity || !scene_->world().is_alive(records_[index].entity))
        return flecs::entity();
    return flecs::entity(scene_->world(), records_[index].entity);
}

uint32_t EditorBridge::parentOf(uint32_t index) const
{
    const flecs::entity p = scene_->parentOf(entityOf(index));
    if (!p)
        return ~0u;
    auto it = indexOf_.find(p.id());
    return it == indexOf_.end() ? ~0u : it->second;
}

std::vector<uint32_t> EditorBridge::subtree(uint32_t index) const
{
    std::vector<uint32_t> out = {index};
    // Breadth first over the records (parents come before children).
    for (size_t k = 0; k < out.size(); ++k)
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (parentOf(i) == out[k] && std::find(out.begin(), out.end(), i) == out.end())
                out.push_back(i);
    return out;
}

bool EditorBridge::hiddenBelow(uint32_t index) const
{
    for (uint32_t i = index, depth = 0; i != ~0u && depth < 256; i = parentOf(i), ++depth)
        if (records_[i].hidden)
            return true;
    return false;
}

void EditorBridge::refreshRows(uint32_t index)
{
    for (const uint32_t i : subtree(index)) {
        const Record& r = records_[i];
        const flecs::entity e = entityOf(i);
        if (const Light* l = e ? e.try_get<Light>() : nullptr) {
            Light flagged = *l;
            flagged.flags = (flagged.flags & ~2u) | (hiddenBelow(i) ? 2u : 0u);
            if (flagged.flags != l->flags)
                e.set<Light>(flagged);
        }
        if (r.slot == ~0u || !e)
            continue;
        // The CPU instance table is the source of extracted rows (flags, tint); the GPU copy
        // follows through the stream below.
        fpak::InstanceGpu& row = pak_->mutableInstances()[r.slot];
        if (const WorldTransform* t = e.try_get<WorldTransform>())
            std::memcpy(row.world, t->rows, sizeof(row.world));
        const bool hide = !e.enabled() || hiddenBelow(i) || e.has<LevelVolume>();  // (a gameplay volume: its outline only)
        row.flags = (row.flags & ~uint32_t(fpak::kInstanceHidden)) | (hide ? fpak::kInstanceHidden : 0u);
        streamed_.push_back(r.slot);
    }
}

const std::vector<PrimitivePool>& EditorBridge::pools() const
{
    static const std::vector<PrimitivePool> kNone;
    return pools_ ? *pools_ : kNone;
}

bool EditorBridge::editable(uint32_t index) const
{
    const flecs::entity e = entityOf(index);
    return e && !e.has<RigidBody>() && records_[index].kind != EntityKind::Bone;  // (bones follow the animation)
}

std::vector<uint32_t> EditorBridge::boneRecords(uint32_t root) const
{
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (records_[i].kind == EntityKind::Bone && records_[i].boneRoot == root)
            out.push_back(i);
    return out;
}

bool EditorBridge::canHold(uint32_t index) const
{
    return editable(index) || (index < records_.size() && records_[index].kind == EntityKind::Bone && entityOf(index) &&
                               entityOf(index).enabled());
}

bool EditorBridge::bonesExpanded(uint32_t root) const
{
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (records_[i].kind == EntityKind::Bone && records_[i].boneRoot == root && records_[i].bone < 0)
            return entityOf(i) && entityOf(i).enabled();
    return false;
}

std::vector<uint32_t> EditorBridge::addBoneRecords(uint32_t root, const BoneEntities::Expanded& x)
{
    std::vector<uint32_t> out;
    Record g;
    g.entity = x.group;
    g.kind = EntityKind::Bone;
    g.name = "Skeleton";
    g.boneRoot = root;
    out.push_back(addRecord(std::move(g)));
    for (uint32_t b = 0; b < x.bones.size(); ++b) {
        Record r;
        r.entity = x.bones[b];
        r.kind = EntityKind::Bone;
        r.name = x.skeleton->name(b);
        r.boneRoot = root;
        r.bone = int32_t(b);
        out.push_back(addRecord(std::move(r)));
    }
    return out;
}

flecs::entity_t EditorBridge::entityNamed(std::string_view name) const
{
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (const flecs::entity e = entityOf(i); e && e.is_alive() && e.enabled() && defaultName(i) == name)
            return e.id();
    return 0;
}

flecs::entity_t EditorBridge::entityNamedUnder(flecs::entity_t root, std::string_view name) const
{
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const flecs::entity e = entityOf(i);
        if (!e || !e.enabled() || defaultName(i) != name)
            continue;
        uint32_t depth = 0;
        for (flecs::entity p = scene_->parentOf(e); p && depth < 256; p = scene_->parentOf(p), ++depth)
            if (p.id() == root)
                return e.id();
    }
    return 0;
}

void EditorBridge::setScriptTypes(const std::string& json)
{
    try {
        scriptTypes_ = Json::parse(json.empty() ? "[]" : json);
        // (X3) Inspector pages as file URLs: the project's UI/ folder, or the library's.
        bool pages = false;
        for (Json& t : scriptTypes_.array)
            if (const std::string page = t.getString("page"); !page.empty() && hasProject_) {
                const std::string library = t.getString("library");
                const std::string path = project_.dir + (library.empty() ? "" : "/Libraries/" + library) + "/UI/" + page;
                std::string url = "file:///";
                for (const char c : path)
                    if (c == ' ' || c == '#' || c == '%' || c == '?')
                        url += "%" + std::string(1, "0123456789ABCDEF"[uint8_t(c) >> 4]) + std::string(1, "0123456789ABCDEF"[uint8_t(c) & 15]);
                    else
                        url += c == '\\' ? '/' : c;
                t.set("pageUrl", Json::makeString(url));
                pages = true;
            }
        scriptTypesJson_ = pages ? scriptTypes_.dump(0) : json.empty() ? "[]" : json;
    } catch (const std::exception&) {
        scriptTypes_ = Json::parse("[]");
        scriptTypesJson_ = "[]";
    }
    if (connected_)
        sendState(true);
}

void EditorBridge::setDataTypes(const std::string& json)
{
    try {
        dataTypes_ = Json::parse(json.empty() ? "[]" : json);
        dataTypesJson_ = json.empty() ? "[]" : json;
    } catch (const std::exception&) {
        dataTypes_ = Json::parse("[]");
        dataTypesJson_ = "[]";
    }
    for (const Json& t : dataTypes_.array)
        if (const std::string name = t.getString("name"); !name.empty())
            dataLayouts_[name] = {uint32_t(t.getNumber("size")), uint32_t(t.getNumber("align"))};
    if (connected_)
        sendState(true);
}

uint64_t EditorBridge::selectedEntity() const
{
    const flecs::entity e = selected_ != ~0u ? entityOf(selected_) : flecs::entity();
    return e && e.is_alive() ? e.id() : 0;
}

void EditorBridge::buttonDone(const ButtonRun& run, const std::string& properties)
{
    // Its properties as the button left them, but entity properties: those are kept as keys.
    if (playing_ || run.index >= records_.size() || !editable(run.index))
        return;
    Json after;
    try {
        after = Json::parse(properties);
    } catch (const std::exception&) {
        return;
    }
    State s = capture(run.index);
    const auto it = std::find_if(s.scripts.begin(), s.scripts.end(), [&](const ScriptEntry& e) { return e.className == run.className; });
    if (it == s.scripts.end() || !after.isObject())
        return;
    Json props;
    try {
        props = Json::parse(it->properties);
    } catch (const std::exception&) {
    }
    if (!props.isObject())
        props = Json::makeObject();
    const Json* list = scriptType(run.className) ? scriptType(run.className)->get("properties") : nullptr;
    for (const auto& [key, value] : after.object) {
        bool entity = false;
        for (const Json& p : list ? list->array : std::vector<Json>{})
            entity |= p.getString("name") == key && p.getString("kind") == "entity";
        if (!entity)
            props.set(key, value);
    }
    if (const std::string text = props.dump(0); text != it->properties) {
        it->properties = text;
        execute({{run.index, s}});
        sendState(true);
    }
}

const Json* EditorBridge::dataType(const std::string& name) const
{
    for (const Json& t : dataTypes_.array)
        if (t.getString("name") == name)
            return &t;
    return nullptr;
}

std::vector<DataComponent> EditorBridge::captureData(flecs::entity e) const
{
    std::vector<DataComponent> out;
    if (!e || !e.is_alive())
        return out;
    flecs::world world = e.world();
    for (const auto& [name, layout] : dataLayouts_) {
        DataComponent d{name, layout.first, layout.second, {}};
        const uint64_t id = dataComponentId(world, d, false);
        const void* at = id ? ecs_get_id(world.c_ptr(), e.id(), id) : nullptr;
        if (!at)
            continue;
        d.bytes.assign(static_cast<const uint8_t*>(at), static_cast<const uint8_t*>(at) + d.size);
        out.push_back(std::move(d));
    }
    return out;
}

void EditorBridge::applyData(flecs::entity e, const std::vector<DataComponent>& data)
{
    flecs::world world = e.world();
    for (const auto& [name, layout] : dataLayouts_)
        if (std::none_of(data.begin(), data.end(), [&](const DataComponent& d) { return d.name == name; }))
            if (const uint64_t id = dataComponentId(world, DataComponent{name, layout.first, layout.second, {}}, false);
                id && ecs_has_id(world.c_ptr(), e.id(), id))
                ecs_remove_id(world.c_ptr(), e.id(), id);
    setDataComponents(e, data);
}

const Json* EditorBridge::scriptType(const std::string& className) const
{
    for (const Json& t : scriptTypes_.array)
        if (t.getString("name") == className)
            return &t;
    return nullptr;
}

std::string EditorBridge::scriptPropsForUi(const ScriptEntry& e) const
{
    const Json* type = scriptType(e.className);
    if (!type)
        return e.properties;
    Json props;
    try {
        props = Json::parse(e.properties);
    } catch (const std::exception&) {
        return "{}";
    }
    if (const Json* list = type->get("properties"))
        for (const Json& p : list->array)
            if (p.getString("kind") == "entity")
                if (const Json* v = props.get(p.getString("name").c_str()); v && v->type == Json::Type::String) {
                    const uint32_t i = v->str.empty() ? ~0u : indexOfKey(v->str);
                    props.set(p.getString("name"), Json::makeNumber(i == ~0u ? 0.0 : double(i + 1)));
                }
    return props.dump(0);
}

std::vector<std::pair<uint64_t, std::vector<ScriptEntry>>> EditorBridge::sceneScripts() const
{
    std::vector<std::pair<uint64_t, std::vector<ScriptEntry>>> out;
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (const flecs::entity e = entityOf(i); e && e.is_alive() && !records_[i].scripts.empty())
            out.emplace_back(e.id(), records_[i].scripts);
    return out;
}

flecs::entity_t EditorBridge::entityByKey(const std::string& key) const
{
    const uint32_t i = key.empty() ? ~0u : indexOfKey(key);
    const flecs::entity e = i == ~0u ? flecs::entity() : entityOf(i);
    return e ? e.id() : 0;
}

std::string EditorBridge::nameOfEntity(flecs::entity_t entity) const
{
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (const flecs::entity e = entityOf(i); e && e.id() == entity)
            return defaultName(i);
    return "";
}

std::string EditorBridge::defaultName(uint32_t index) const
{
    if (index >= records_.size())
        return "";
    const Record& r = records_[index];
    if (!r.name.empty())
        return r.name;
    if (r.slot != ~0u)
        if (const char* n = pak_->instanceName(r.slot))
            return n;
    return "Instance " + std::to_string(r.slot);
}

std::string EditorBridge::createdName(EntityKind kind, const std::string& primitive, uint32_t slot) const
{
    if (kind == EntityKind::Primitive)
        for (const PrimitivePool& p : pools())
            if ((p.name == primitive || p.name.starts_with(primitive + "~")) && slot >= p.first && slot < p.first + p.size) {
                std::string n = primitive + " " + std::to_string(slot - p.first + 1);
                n[0] = char(std::toupper(uint8_t(n[0])));
                return n;
            }
    uint32_t n = 1;
    for (const Record& r : records_)
        n += r.kind == kind && (kind != EntityKind::Light || r.primitive == primitive);
    if (kind == EntityKind::Light)
        return cookedLightName(lightTypeOf(primitive), n);
    return (kind == EntityKind::Camera ? "Camera " : kind == EntityKind::Water ? "Water " : kind == EntityKind::Level ? "Level Mesh " : "Empty ") +
           std::to_string(n);
}

// The name init() gave a cooked light ("Point Light 2"), to tell renamed ones apart.
std::string EditorBridge::defaultCookedLightName(uint32_t lightIndex) const
{
    uint32_t number[3] = {0, 0, 0};
    for (uint32_t i = 0; i <= lightIndex && i < scene_->cookedLightCount(); ++i) {
        Light l;
        float world[3][4];
        if (!scene_->cookedLight(i, l, world))
            continue;
        const uint32_t type = std::min(l.type, 2u);
        ++number[type];
        if (i == lightIndex)
            return cookedLightName(type, number[type]);
    }
    return "";
}

std::string EditorBridge::keyOf(uint32_t index) const
{
    const Record& r = records_[index];
    if (r.kind == EntityKind::Bone)  // (by name under its character: bones are not saved)
        return SceneEdits::boneKey(keyOf(r.boneRoot), r.bone < 0 ? "" : r.name);
    if (linkedChild(index))  // (its uid is not saved: the prefab's id is)
        return SceneEdits::prefabKey(records_[r.prefabRoot].uid, r.prefabChild);
    return r.uid ? SceneEdits::createdKey(r.uid) : r.light != ~0u ? SceneEdits::lightKey(r.light) : SceneEdits::instanceKey(r.slot);
}

void EditorBridge::writeVfxInspector(JsonWriter& w, uint32_t index, const State& s) const
{
    if (s.vfx.empty())
        return;
    uint32_t count = 0;
    const VfxComponentInfo* infos = vfxComponents(count);
    static const char* kTypes[] = {"float", "uint", "int", "bool", "enum", "vec3", "color", "pfloat", "pvec3", "pgradient", "material", "prefab", "name", "mesh"};
    static const char* kKinds[] = {"effect", "emitter", "renderer", "controller", "decal"};
    w.beginObject("vfx").beginArray("components");
    for (const VfxBlob& b : s.vfx.items) {
        const VfxComponentInfo& info = infos[b.component];
        w.beginObject().value("name", info.name).value("json", info.json).value("label", info.label).value("icon", info.icon)
            .value("kind", kKinds[uint32_t(info.kind)]).beginArray("schema");
        for (uint32_t i = 0; i < info.fieldCount; ++i) {
            const VfxField& f = info.fields[i];
            w.beginObject().value("key", f.key).value("label", f.label).value("group", f.group).value("type", kTypes[uint32_t(f.type)]);
            if (f.options)
                w.value("options", f.options);
            if (f.max > f.min)
                w.value("min", double(f.min)).value("max", double(f.max));
            if (f.feature)
                w.value("feature", true);
            if (*f.tip)
                w.value("tip", f.tip);
            w.endObject();
        }
        w.endArray().beginObject("values");
        writeVfxComponent(w, info, b.data.data(), true);
        w.endObject();
        if (b.component == VfxIndexOf<ParticleModelRenderer>::value && runtimeMeshes_) {  // (a mesh it can't draw this session)
            ParticleModelRenderer r;
            std::memcpy(&r, b.data.data(), sizeof(r));
            if (const std::string why = runtimeMeshes_->missing(*pak_, r.mesh); !why.empty())
                w.value("warning", why);
        }
        w.endObject();
    }
    w.endArray();
    // The effect it works for (its nearest up the tree), and its particles now.
    flecs::entity effect;
    for (flecs::entity at = entityOf(index); at && !effect; at = scene_->parentOf(at))
        if (at.has<ParticleEffect>())
            effect = at;
    if (effect) {
        const ParticleSystem::Effect* fx = particles_ ? particles_->effectOf(effect) : nullptr;
        w.beginObject("effect").value("own", effect == entityOf(index)).value("name", indexOf_.count(effect.id()) ? defaultName(indexOf_.at(effect.id())) : std::string())
            .value("particles", fx ? uint32_t(fx->particles.size()) : 0u).value("playing", fx && fx->playing).endObject();
    }
    w.endObject();
}

EditorBridge::State EditorBridge::capture(uint32_t index) const
{
    State s;
    const flecs::entity e = entityOf(index);
    s.world[0][0] = s.world[1][1] = s.world[2][2] = 1.0f;
    if (const WorldTransform* t = e ? e.try_get<WorldTransform>() : nullptr)
        std::memcpy(s.world, t->rows, sizeof(s.world));
    else if (index < records_.size() && records_[index].slot != ~0u)
        std::memcpy(s.world, pak_->instances()[records_[index].slot].world, sizeof(s.world));
    s.parent = parentOf(index);
    if (const LocalTransform* l = e && s.parent != ~0u ? e.try_get<LocalTransform>() : nullptr) {
        std::memcpy(s.local, l->rows, sizeof(s.local));
        s.hasLocal = true;
    }
    s.alive = e && e.enabled();
    s.hidden = index < records_.size() && records_[index].hidden;
    s.name = defaultName(index);
    if (const Light* l = e ? e.try_get<Light>() : nullptr) {
        s.light = *l;
        s.light->flags &= ~2u;
    }
    if (const CameraComponent* c = e ? e.try_get<CameraComponent>() : nullptr)
        s.camera = *c;
    if (const int32_t body = waterBody(index); body >= 0)
        s.water = water_->body(uint32_t(body))->desc;
    if (const PhysicsBody* b = e ? e.try_get<PhysicsBody>() : nullptr) {
        s.body = *b;
        std::memset(s.body->bodyId, 0, sizeof(s.body->bodyId));  // (Play's body: not part of it)
    }
    if (const Collider* c = e ? e.try_get<Collider>() : nullptr)
        s.collider = *c;
    if (const AudioSource* a = e ? e.try_get<AudioSource>() : nullptr) {
        s.audioSource = *a;
        s.audioSource->voice = 0;  // (Play's voice: not part of it)
    }
    if (const AudioListener* l = e ? e.try_get<AudioListener>() : nullptr)
        s.audioListener = *l;
    if (const AcousticSurface* a = e ? e.try_get<AcousticSurface>() : nullptr)
        s.acoustics = *a;
    if (e)
        s.vfx = captureVfx(e);
    if (index < records_.size()) {
        s.material = records_[index].material;
        s.animator = records_[index].animator;
        s.scripts = records_[index].scripts;
    }
    s.data = captureData(e);
    if (const LevelMesh* lm = e ? e.try_get<LevelMesh>() : nullptr)
        s.levelMesh = lm->mesh;
    if (const LevelMask* mask = e ? e.try_get<LevelMask>() : nullptr)
        s.levelMask = mask->rgba;
    if (const LevelWear* wear = e ? e.try_get<LevelWear>() : nullptr)
        s.wear = *wear;
    if (const LevelModifiers* mods = e ? e.try_get<LevelModifiers>() : nullptr)
        s.modifiers = *mods;
    if (const LevelSpline* spline = e ? e.try_get<LevelSpline>() : nullptr)
        s.spline = *spline;
    if (const ScatterSet* scatter = e ? e.try_get<ScatterSet>() : nullptr)
        s.scatter = *scatter;
    if (const SnapPoint* snap = e ? e.try_get<SnapPoint>() : nullptr)
        s.snap = *snap;
    if (const LevelVolume* volume = e ? e.try_get<LevelVolume>() : nullptr)
        s.volume = *volume;
    if (const CutVolume* cut = e ? e.try_get<CutVolume>() : nullptr)
        s.cut = *cut;
    if (const LevelWall* wall = e ? e.try_get<LevelWall>() : nullptr)
        s.wall = wall->wall;
    if (const LevelRoof* roof = e ? e.try_get<LevelRoof>() : nullptr)
        s.roof = roof->roof;
    if (const WallOpening* opening = e ? e.try_get<WallOpening>() : nullptr)
        s.opening = *opening;
    return s;
}

const std::vector<std::string>& EditorBridge::graphAssets()
{
    const uint64_t now = SDL_GetTicksNS();
    if (graphAssetsNs_ && now - graphAssetsNs_ < 2000000000ull)
        return graphAssets_;
    graphAssetsNs_ = now;
    graphAssets_.clear();
    if (project_.dir.empty())
        return graphAssets_;
    std::error_code ec;
    const std::filesystem::path root(project_.dir);
    for (auto it = std::filesystem::recursive_directory_iterator(root / "Assets", ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && graphAssets_.size() < 200; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".fgraph")
            graphAssets_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
    std::sort(graphAssets_.begin(), graphAssets_.end());
    return graphAssets_;
}

void EditorBridge::bindAnimator(uint32_t index)
{
    if (!animation_ || !graphs_ || index >= records_.size())
        return;
    const std::optional<AnimatorEdit>& a = records_[index].animator;
    const std::vector<uint32_t> records = animation_->recordsUnder(scene_->world(), records_[index].entity);
    animation_->bind(records, a && !a->graph.empty() ? graphs_->get(a->graph) : nullptr, a ? a->params : AnimationSystem::Params{});
    if (!records.empty()) {
        // Its entity parameters' bindings (by key) and its own IK (an older scene's).
        std::vector<std::pair<std::string, uint64_t>> bound;
        for (const auto& [name, key] : a ? a->entities : decltype(a->entities){}) {
            const uint32_t i = indexOfKey(key);
            if (i != ~0u)
                bound.push_back({name, uint64_t(records_[i].entity)});
        }
        animation_->bindEntities(records[0], bound);
        animation_->setOwnIk(records[0], a ? ownIkOf(*a) : nullptr);
        animation_->setEditorPreview(records[0], a && a->editorPreview);
    }
    animation_->update(0.0f);  // posed now, also in edit mode
}

bool EditorBridge::moveIkToGraph(AnimatorEdit& a, std::string& error)
{
    const std::shared_ptr<const anim::IkSettings> own = ownIkOf(a);
    if (!own) {
        error = "it has no IK of its own";
        return false;
    }
    if (a.graph.empty()) {
        error = "choose a graph for its IK first";
        return false;
    }
    std::vector<std::string> targets;
    const auto use = [&](const std::string& name) {
        if (!name.empty() && std::find(targets.begin(), targets.end(), name) == targets.end())
            targets.push_back(name);
    };
    if (own->look.on)
        use(own->look.target);
    if (own->aim.on)
        use(own->aim.target), use(own->aim.hold);
    for (const anim::IkSettings::Reach& r : own->reaches)
        use(r.target);
    if (!animEditor_.addIk(a.graph, a.ik, targets, error, *ui_))
        return false;
    a.ik.clear();
    FX_INFO("animator: IK moved into %s", a.graph.c_str());
    return true;
}

uint32_t EditorBridge::indexOfKey(const std::string& key) const
{
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (records_[i].entity && keyOf(i) == key)
            return i;
    return ~0u;
}

const std::vector<std::string>& EditorBridge::waterAssets()
{
    const uint64_t now = SDL_GetTicksNS();
    if (waterAssetsNs_ && now - waterAssetsNs_ < 2'000'000'000ull)
        return waterAssets_;
    waterAssetsNs_ = now;
    waterAssets_.clear();
    if (project_.dir.empty())
        return waterAssets_;
    std::error_code ec;
    const std::filesystem::path root(project_.dir);
    for (auto it = std::filesystem::recursive_directory_iterator(root / "Assets", ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && waterAssets_.size() < 200; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".fwater")
            waterAssets_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
    std::sort(waterAssets_.begin(), waterAssets_.end());
    return waterAssets_;
}

int32_t EditorBridge::waterBody(uint32_t index) const
{
    if (!water_ || index >= records_.size() || records_[index].kind != EntityKind::Water)
        return -1;
    return water_->find(records_[index].entity);
}

void EditorBridge::setMaterialRow(uint32_t index, const std::string& material)
{
    Record& r = records_[index];
    r.material = material;
    if (r.slot == ~0u)
        return;
    const int32_t m = material.empty() || !materials_ ? -1 : materials_->indexOf(material);
    if (!material.empty() && m < 0)
        notice(("No room for the material " + material + " this session (Reload the scene)").c_str(), "error");
    fpak::InstanceGpu& row = pak_->mutableInstances()[r.slot];
    row.flags = (row.flags & 0xffffu) | (m >= 0 ? (uint32_t(m) + 1) << fpak::kInstanceMaterialShift : 0u);
    streamed_.push_back(r.slot);
}

bool EditorBridge::canAddComponent(uint32_t index, const std::string& component) const
{
    // Both are found by query (LightSystem, Scene::mainCamera), so any entity can carry them.
    const flecs::entity e = entityOf(index);
    if (!editable(index) || !e.enabled())
        return false;
    if (component.starts_with("Script:")) {  // a script component class, once per entity
        const std::string name = component.substr(7);
        return scriptType(name) && std::none_of(records_[index].scripts.begin(), records_[index].scripts.end(),
                                                [&](const ScriptEntry& s) { return s.className == name; });
    }
    if (component.starts_with("Data:")) {  // (X2) a data component of the game's, once per entity
        const std::string name = component.substr(5);
        const std::vector<DataComponent> data = captureData(e);
        return dataType(name) && std::none_of(data.begin(), data.end(), [&](const DataComponent& d) { return d.name == name; });
    }
    if (component == "Light")
        return !e.has<Light>();
    if (component == "CameraComponent")
        return !e.has<CameraComponent>();
    if (component == "PhysicsBody")
        return !e.has<PhysicsBody>();
    // One collider per entity; a mesh collider needs a mesh.
    if (component == "BoxCollider" || component == "SphereCollider" || component == "CapsuleCollider" || component == "Collider")
        return !e.has<Collider>();
    if (component == "MeshCollider")
        return !e.has<Collider>() && e.has<MeshInstance>();
    if (component == "AudioSource")
        return !e.has<AudioSource>();
    if (component == "AudioListener")
        return !e.has<AudioListener>();
    if (component == "Animator")  // on an entity with skinned meshes in it or below
        return !records_[index].animator && animation_ && !animation_->recordsUnder(scene_->world(), e.id()).empty();
    if (const int32_t c = vfxComponentIndex(component); c >= 0)  // particles and decals: any entity, one of each
        return !vfxGet(e, uint32_t(c));
    return false;
}

Collider EditorBridge::defaultCollider(uint32_t index, uint32_t shape) const
{
    // Unity's: around the mesh (its local bounds), or a 1 m box / ball; friction 0.6.
    Collider c = {};
    c.shape = shape;
    c.size[0] = c.size[1] = c.size[2] = shape == 1 ? 0.5f : 1.0f;
    if (shape == 3)  // a 2 m capsule, 0.5 m across (Unity's)
        c.size[0] = c.size[2] = 0.5f, c.size[1] = 2.0f;
    c.friction = 0.6f;
    float3 center, half;
    if (const MeshInstance* mi = entityOf(index).try_get<MeshInstance>();
        mi && shape != 2 && meshBoundsOf(pak_->instances()[mi->gpuSlot].mesh, center, half)) {
        c.center[0] = center.x, c.center[1] = center.y, c.center[2] = center.z;
        if (shape == 1)
            c.size[0] = c.size[1] = c.size[2] = std::max({half.x, half.y, half.z});
        else if (shape == 3)  // (radius, height)
            c.size[0] = std::max(half.x, half.z), c.size[1] = half.y * 2.0f, c.size[2] = c.size[0];
        else
            c.size[0] = half.x * 2.0f, c.size[1] = half.y * 2.0f, c.size[2] = half.z * 2.0f;
    }
    return c;
}

std::string EditorBridge::materialAcoustics(uint32_t index, std::string& preset, AcousticParams& params) const
{
    const std::string& material = index < records_.size() ? records_[index].material : std::string();
    if (!material.empty() && audio_ && audio_->materialAcoustics(material, preset, params)) {
        std::string label = std::filesystem::path(material).stem().string() + " · ";
        for (const AcousticPreset& p : acousticPresets())
            if (preset == p.name)
                return label + p.label;
        return label + "its own values";
    }
    preset = "generic";
    acousticPreset(preset, params);
    return material.empty() ? "Generic (the mesh's own material has none)" : "Generic";
}

std::string EditorBridge::materialSurface(uint32_t index, std::string& preset, SurfaceSound& sound) const
{
    const std::string& material = index < records_.size() ? records_[index].material : std::string();
    if (!material.empty() && audio_ && audio_->materialSurface(material, preset, sound)) {
        std::string label = std::filesystem::path(material).stem().string() + " · ";
        for (const SurfacePreset& p : surfacePresets())
            if (preset == p.name)
                return label + p.label;
        return label + "its own sound";
    }
    preset = "generic";
    sound = surfacePresets()[0].sound;
    return "Generic";
}

bool EditorBridge::canRemoveComponent(uint32_t index, const std::string& component)
{
    if (!editable(index))
        return false;
    if (component.starts_with("Script:")) {
        const std::string name = component.substr(7);
        return std::any_of(records_[index].scripts.begin(), records_[index].scripts.end(),
                           [&](const ScriptEntry& s) { return s.className == name; });
    }
    if (component.starts_with("Data:")) {
        const std::string name = component.substr(5);
        const std::vector<DataComponent> data = captureData(entityOf(index));
        return std::any_of(data.begin(), data.end(), [&](const DataComponent& d) { return d.name == name; });
    }
    const flecs::entity e = entityOf(index);
    const Record& r = records_[index];
    // A prefab entity's own component stays: an instance's override can add one, not take one off.
    const PrefabEntity* p = nullptr;
    if (r.prefabRoot != ~0u)
        if (const Prefab* def = prefabDef(records_[r.prefabRoot].prefab))
            p = def->find(r.prefabChild);
    if (component == "Light")
        return e.has<Light>() && r.kind != EntityKind::Light && !(p && p->light);
    if (component == "CameraComponent")
        return e.has<CameraComponent>() && r.kind != EntityKind::Camera && !(p && p->camera);
    if (component == "PhysicsBody")
        return e.has<PhysicsBody>() && !(p && p->body);
    if (component == "Collider")
        return e.has<Collider>() && !(p && p->collider);
    if (component == "AudioSource")
        return e.has<AudioSource>() && !(p && p->audioSource);
    if (component == "AudioListener")
        return e.has<AudioListener>() && !(p && p->audioListener);
    if (component == "Animator")
        return r.animator.has_value();
    if (const int32_t c = vfxComponentIndex(component); c >= 0)
        return vfxGet(e, uint32_t(c)) && !(p && p->vfx.has(uint32_t(c)));
    return false;
}

void EditorBridge::apply(uint32_t index, const State& s, bool batch)
{
    flecs::entity e = entityOf(index);
    if (!e)
        return;
    if (s.parent != parentOf(index))
        scene_->setParent(e, s.parent == ~0u ? flecs::entity() : entityOf(s.parent));
    // Under a bone (at any depth: a pistol on the hand, its mesh and muzzle under it): where it
    // hangs, wherever the bone is now. Its world was the pose's when captured, and the pose moves
    // (Play, Stop starting the animation again, an Animator previewing in the editor).
    bool onBone = false;
    for (uint32_t p = s.parent, depth = 0; s.hasLocal && p != ~0u && depth < 256 && !onBone; p = parentOf(p), ++depth)
        onBone = records_[p].kind == EntityKind::Bone;
    if (onBone)
        scene_->setLocal(e, entityOf(s.parent), s.local);
    else
        scene_->setWorld(e, s.world);
    // A Light or camera added in the Inspector goes again when the state has none (undo, Remove
    // Component); a light's own Light and a camera's own camera always stay.
    const EntityKind kind = records_[index].kind;
    if (s.camera)
        e.set<CameraComponent>(*s.camera);
    else if (kind != EntityKind::Camera && e.has<CameraComponent>())
        e.remove<CameraComponent>();
    if (s.light) {
        e.set<Light>(*s.light);  // refreshRows puts the hidden flag back
        if (kind == EntityKind::Light)
            records_[index].primitive = lightTypeName(s.light->type);
    } else if (kind != EntityKind::Light && e.has<Light>()) {
        e.remove<Light>();
    }
    if (s.body) {  // (a body Play built keeps its id)
        PhysicsBody b = *s.body;
        if (const PhysicsBody* now = e.try_get<PhysicsBody>())
            std::memcpy(b.bodyId, now->bodyId, sizeof(b.bodyId));
        e.set<PhysicsBody>(b);
    } else if (e.has<PhysicsBody>()) {
        e.remove<PhysicsBody>();
    }
    if (s.collider)
        e.set<Collider>(*s.collider);
    else if (e.has<Collider>())
        e.remove<Collider>();
    if (s.audioSource) {  // (the voice Play started keeps playing)
        AudioSource a = *s.audioSource;
        if (const AudioSource* now = e.try_get<AudioSource>())
            a.voice = now->voice;
        e.set<AudioSource>(a);
    } else if (e.has<AudioSource>()) {
        e.remove<AudioSource>();
    }
    if (s.audioListener)
        e.set<AudioListener>(*s.audioListener);
    else if (e.has<AudioListener>())
        e.remove<AudioListener>();
    if (s.acoustics)
        e.set<AcousticSurface>(*s.acoustics);
    else if (e.has<AcousticSurface>())
        e.remove<AcousticSurface>();
    if (!(s.vfx == captureVfx(e)))
        setVfx(e, s.vfx);
    if (s.data != captureData(e))
        applyData(e, s.data);
    if (s.levelMesh) {  // (LevelMeshes builds it again when the mesh is another one)
        const LevelMesh* now = e.try_get<LevelMesh>();
        if (!now || now->mesh != s.levelMesh)
            e.set<LevelMesh>({s.levelMesh});
    } else if (e.has<LevelMesh>()) {
        e.remove<LevelMesh>();
    }
    if (s.wall) {
        const LevelWall* now = e.try_get<LevelWall>();
        if (!now || !(now->wall == *s.wall))
            e.set<LevelWall>({*s.wall});
    } else if (e.has<LevelWall>()) {
        e.remove<LevelWall>();
    }
    if (s.roof) {
        const LevelRoof* now = e.try_get<LevelRoof>();
        if (!now || !(now->roof == *s.roof))
            e.set<LevelRoof>({*s.roof});
    } else if (e.has<LevelRoof>()) {
        e.remove<LevelRoof>();
    }
    if (s.opening) {
        const WallOpening* now = e.try_get<WallOpening>();
        if (!now || !(*now == *s.opening))
            e.set<WallOpening>(*s.opening);
    } else if (e.has<WallOpening>()) {
        e.remove<WallOpening>();
    }
    if (s.cut) {
        const CutVolume* now = e.try_get<CutVolume>();
        if (!now || !(*now == *s.cut))
            e.set<CutVolume>(*s.cut);
    } else if (e.has<CutVolume>()) {
        e.remove<CutVolume>();
    }
    if (s.levelMask) {
        const LevelMask* now = e.try_get<LevelMask>();
        if (!now || now->rgba != s.levelMask)
            e.set<LevelMask>({s.levelMask});
    } else if (e.has<LevelMask>()) {
        e.remove<LevelMask>();
    }
    if (s.volume) {
        const LevelVolume* now = e.try_get<LevelVolume>();
        if (!now || !(*now == *s.volume))
            e.set<LevelVolume>(*s.volume);
    } else if (e.has<LevelVolume>()) {
        e.remove<LevelVolume>();
    }
    if (s.snap) {
        const SnapPoint* now = e.try_get<SnapPoint>();
        if (!now || !(*now == *s.snap))
            e.set<SnapPoint>(*s.snap);
    } else if (e.has<SnapPoint>()) {
        e.remove<SnapPoint>();
    }
    if (s.scatter) {
        const ScatterSet* now = e.try_get<ScatterSet>();
        if (!now || !(*now == *s.scatter))
            e.set<ScatterSet>(*s.scatter);
    } else if (e.has<ScatterSet>()) {
        e.remove<ScatterSet>();
    }
    if (s.spline) {
        const LevelSpline* now = e.try_get<LevelSpline>();
        if (!now || !(*now == *s.spline))
            e.set<LevelSpline>(*s.spline);
    } else if (e.has<LevelSpline>()) {
        e.remove<LevelSpline>();
    }
    if (s.modifiers) {
        const LevelModifiers* now = e.try_get<LevelModifiers>();
        if (!now || !(*now == *s.modifiers))
            e.set<LevelModifiers>(*s.modifiers);
    } else if (e.has<LevelModifiers>()) {
        e.remove<LevelModifiers>();
    }
    if (s.wear) {
        const LevelWear* now = e.try_get<LevelWear>();
        if (!now || !(*now == *s.wear))
            e.set<LevelWear>(*s.wear);
    } else if (e.has<LevelWear>()) {
        e.remove<LevelWear>();
    }
    if (const int32_t body = s.water ? waterBody(index) : -1; body >= 0) {
        water_->set(uint32_t(body), *s.water);
        records_[index].primitive = waterShapeName(s.water->shape);
    }
    if (s.alive)
        e.enable();
    else
        e.disable();
    records_[index].hidden = s.hidden;
    records_[index].name = s.name;
    if (records_[index].material != s.material)
        setMaterialRow(index, s.material);
    if (records_[index].animator != s.animator) {
        records_[index].animator = s.animator;
        bindAnimator(index);
    }
    records_[index].scripts = s.scripts;
    dirty_ = true;
    if (batch)
        return;  // (the caller propagates once and refreshes the rows)
    scene_->propagate();  // children follow now: gizmos, captures and rows see it this frame
    refreshRows(index);
}

void EditorBridge::sceneChanged()
{
    ++sceneChanges_;
    if (settings_)
        ++settings_->gi.sceneRevision;
}

void EditorBridge::execute(const std::vector<std::pair<uint32_t, State>>& after)
{
    Command c;
    for (const auto& [index, state] : after)
        c.push_back({index, capture(index), state});
    for (const Change& ch : c)
        apply(ch.index, ch.after);
    undo_.push_back(std::move(c));
    redo_.clear();
    ++commands_;
    sceneChanged();
}

void EditorBridge::select(uint32_t index)
{
    selected_ = index != ~0u && editable(index) ? index : ~0u;
    selection_.clear();
    if (selected_ != ~0u)
        selection_.push_back(selected_);
}

void EditorBridge::selectMode(uint32_t index, SelectMode mode)
{
    if (mode == SelectMode::Replace || index == ~0u || !editable(index)) {
        if (mode == SelectMode::Replace)
            select(index);
        return;
    }
    const auto it = std::find(selection_.begin(), selection_.end(), index);
    if (mode == SelectMode::Toggle && it != selection_.end()) {
        selection_.erase(it);
        selected_ = selection_.empty() ? ~0u : selection_.back();
        return;
    }
    if (it != selection_.end())
        selection_.erase(it);
    selection_.push_back(index);  // (the primary: last)
    selected_ = index;
}

void EditorBridge::selectMany(const std::vector<uint32_t>& indices, SelectMode mode)
{
    if (mode == SelectMode::Replace)
        select(~0u);
    for (const uint32_t i : indices)
        selectMode(i, mode == SelectMode::Toggle ? SelectMode::Toggle : SelectMode::Add);
}

std::vector<uint32_t> EditorBridge::selectionRoots() const
{
    std::vector<uint32_t> out;
    for (const uint32_t i : selection_) {
        bool under = false;
        for (uint32_t p = parentOf(i), depth = 0; p != ~0u && depth < 256 && !under; p = parentOf(p), ++depth)
            under = std::find(selection_.begin(), selection_.end(), p) != selection_.end();
        if (!under)
            out.push_back(i);
    }
    return out;
}

void EditorBridge::selectableCenters(std::vector<std::pair<uint32_t, float3>>& out) const
{
    out.clear();
    for (uint32_t i = 0; i < records_.size(); ++i) {
        if (!editable(i) || !capture(i).alive || hiddenBelow(i) || records_[i].kind == EntityKind::Bone)
            continue;
        if (const flecs::entity e = entityOf(i); e && e.has<CutVolume>() && e.get<CutVolume>().shape == level::CutShape::Damage)
            continue;  // (damage cuts: not drawn unless their mesh is selected; a box takes the mesh)
        float w[3][4];
        if (!worldOf(i, w))
            continue;
        float3 c = {0, 0, 0}, half;
        if (!meshBounds(i, c, half))
            c = {0, 0, 0};
        out.push_back({i, {w[0][0] * c.x + w[0][1] * c.y + w[0][2] * c.z + w[0][3], w[1][0] * c.x + w[1][1] * c.y + w[1][2] * c.z + w[1][3],
                           w[2][0] * c.x + w[2][1] * c.y + w[2][2] * c.z + w[2][3]}});
    }
}

std::shared_ptr<const level::PolyMesh> EditorBridge::levelMeshOf(uint32_t index) const
{
    const flecs::entity e = entityOf(index);
    const LevelMesh* m = e ? e.try_get<LevelMesh>() : nullptr;
    return m ? m->mesh : nullptr;
}

void EditorBridge::setLevelMeshLive(uint32_t index, std::shared_ptr<const level::PolyMesh> mesh)
{
    if (const flecs::entity e = entityOf(index); e && mesh && editable(index))
        e.set<LevelMesh>({std::move(mesh)});
}

void EditorBridge::commitLevelMesh(uint32_t index, std::shared_ptr<const level::PolyMesh> before)
{
    if (!editable(index) || !before)
        return;
    const State after = capture(index);
    if (after.levelMesh == before)
        return;
    State b = after;
    b.levelMesh = std::move(before);
    undo_.push_back({{index, b, after}});
    redo_.clear();
    ++commands_;
    sceneChanged();
    sendState(false);
}

void EditorBridge::snapPoints(const std::vector<uint32_t>& skip, std::vector<float3>& out) const
{
    out.clear();
    for (uint32_t i = 0; i < records_.size() && out.size() < 65536; ++i) {
        if (std::find(skip.begin(), skip.end(), i) != skip.end() || !entityOf(i) || !capture(i).alive || hiddenBelow(i) ||
            records_[i].kind == EntityKind::Bone)
            continue;
        float w[3][4];
        if (!worldOf(i, w))
            continue;
        const auto at = [&](float3 p) {
            return float3{w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                          w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
        };
        if (const auto mesh = levelMeshOf(i)) {
            for (const float3& p : mesh->positions)
                out.push_back(at(p));
            for (const level::Face& f : mesh->faces)  // (edges' middles; shared edges twice, harmless)
                for (uint32_t k = 0; k < f.count; ++k)
                    out.push_back(at((mesh->positions[mesh->corners[f.first + k]] + mesh->positions[mesh->corners[f.first + (k + 1) % f.count]]) * 0.5f));
        } else if (float3 c, h; meshBounds(i, c, h)) {
            for (int k = 0; k < 8; ++k)
                out.push_back(at({k & 1 ? c.x + h.x : c.x - h.x, k & 2 ? c.y + h.y : c.y - h.y, k & 4 ? c.z + h.z : c.z - h.z}));
        } else {
            out.push_back({w[0][3], w[1][3], w[2][3]});
        }
    }
}

bool EditorBridge::surfaceHit(float3 origin, float3 dir, const std::vector<uint32_t>& skip, float3& point, float3& normal, uint32_t* record,
                              uint32_t* face) const
{
    float best = 1e30f;
    if (record)
        *record = ~0u;
    if (face)
        *face = ~0u;
    dir = normalize(dir);
    for (uint32_t i = 0; i < records_.size(); ++i) {
        if (std::find(skip.begin(), skip.end(), i) != skip.end() || !entityOf(i) || !capture(i).alive || hiddenBelow(i))
            continue;
        const auto mesh = levelMeshOf(i);
        float w[3][4];
        if (!mesh || !worldOf(i, w))
            continue;
        const auto at = [&](float3 p) {
            return float3{w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                          w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
        };
        for (uint32_t fi = 0; fi < mesh->faces.size(); ++fi)  // (a fan per face: faces are flat enough for snapping)
            for (uint32_t k = 1, n = mesh->faces[fi].count; k + 1 < n; ++k) {
                const level::Face& f = mesh->faces[fi];
                const float3 a = at(mesh->positions[mesh->corners[f.first]]), b = at(mesh->positions[mesh->corners[f.first + k]]),
                             c = at(mesh->positions[mesh->corners[f.first + k + 1]]);
                const float3 e1 = b - a, e2 = c - a, pv = cross(dir, e2);
                const float det = dot(e1, pv);
                if (std::fabs(det) < 1e-9f)
                    continue;
                const float3 tv = origin - a;
                const float u = dot(tv, pv) / det;
                const float3 qv = cross(tv, e1);
                const float v = dot(dir, qv) / det, t = dot(e2, qv) / det;
                if (u < 0.0f || v < 0.0f || u + v > 1.0f || t <= 1e-4f || t >= best)
                    continue;
                best = t;
                point = origin + dir * t;
                normal = normalize(cross(e1, e2));
                if (dot(normal, dir) > 0.0f)
                    normal = normal * -1.0f;
                if (record)
                    *record = i;
                if (face)
                    *face = fi;
            }
    }
    if (best < 1e30f)
        return true;
    // Else the terrain, else the ground (y = 0).
    normal = {0, 1, 0};
    if (float3 p; terrain_ && terrain_->hit(origin, dir, p)) {
        point = p;
        return true;
    }
    if (dir.y < -1e-4f && origin.y > 0.0f) {
        point = origin + dir * (-origin.y / dir.y);
        return true;
    }
    return false;
}

void EditorBridge::commitWorlds(const std::vector<std::pair<uint32_t, std::array<float, 12>>>& before)
{
    Command c;
    for (const auto& [index, world] : before) {
        if (!editable(index))
            continue;
        const State after = capture(index);
        if (std::memcmp(after.world, world.data(), sizeof(after.world)) == 0)
            continue;
        State b = after;
        std::memcpy(b.world, world.data(), sizeof(b.world));
        b.hasLocal = false;
        if (b.opening)
            frameOpening(b, true);
        c.push_back({index, b, after});
    }
    if (c.empty())
        return;
    undo_.push_back(std::move(c));
    redo_.clear();
    ++commands_;
    liveDirty_ = false;
    sceneChanged();
    sendState(false);
}

void EditorBridge::notePick(uint32_t slot, uint32_t cluster, uint32_t triangle)
{
    lastPick_ = {slot, cluster, triangle, -1, -1};
    if (renderer_)
        if (const int32_t live = renderer_->live().liveOfInstance(slot); live >= 0) {
            lastPick_.live = live;
            lastPick_.liveTriangle = renderer_->live().sourceTriangle(uint32_t(live), cluster, triangle);
        }
    if (levelMeshes_ && scene_)
        if (const flecs::entity e = scene_->entity(slot))
            lastPick_.face = levelMeshes_->faceOf(e.id(), cluster, triangle);
}

bool EditorBridge::setLevelMesh(flecs::entity_t entity, std::shared_ptr<const level::PolyMesh> mesh)
{
    const auto it = indexOf_.find(entity);
    if (playing_ || !mesh || it == indexOf_.end())
        return false;
    State s = capture(it->second);
    s.levelMesh = std::move(mesh);
    execute({{it->second, s}});
    sendState(false);
    return true;
}

flecs::entity_t EditorBridge::createLevelMesh(const float* rows, flecs::entity_t parent, std::shared_ptr<const level::PolyMesh> mesh)
{
    if (playing_ || !mesh || !rows)
        return 0;
    float world[3][4];
    std::memcpy(world, rows, sizeof(world));
    const uint32_t created = spawn(EntityKind::Level, "box", world, "");
    if (created == ~0u)
        return 0;
    State s;
    std::memcpy(s.world, world, sizeof(s.world));
    const auto p = parent ? indexOf_.find(parent) : indexOf_.end();
    s.parent = p != indexOf_.end() && canHold(p->second) ? p->second : ~0u;
    s.alive = true;
    s.name = defaultName(created);
    s.levelMesh = std::move(mesh);
    execute({{created, s}});
    select(created);
    sendState(true);
    return records_[created].entity;
}

void EditorBridge::selectSlot(uint32_t slot, SelectMode mode)
{
    // A cut copy of imported art (LevelMeshes): the instance it stands for.
    if (levelMeshes_)
        if (const int32_t original = levelMeshes_->originalOf(slot); original >= 0)
            slot = uint32_t(original);
    // A scatter instance is the terrain's selection (Delete removes it), not an entity.
    if (terrain_ && terrain_->pickScatter(slot)) {
        select(~0u);
        sendState(false);
        return;
    }
    const uint32_t index = slot < indexOfSlot_.size() ? indexOfSlot_[slot] : ~0u;
    if (mode == SelectMode::Replace)
        return selectEntity(index);
    if (index == ~0u)
        return;  // (Shift or Ctrl on empty space keeps the selection)
    selectMode(index, mode);
    sendState(false);
}

void EditorBridge::selectEntity(uint32_t index)
{
    const uint32_t before = selected_;
    select(index);
    if (selected_ != before)
        sendState(false);
}

uint32_t EditorBridge::selectedSlot() const
{
    const uint32_t slot = selected_ < records_.size() ? records_[selected_].slot : ~0u;
    if (levelMeshes_ && slot != ~0u)  // (imported art drawn cut: the copy outlined)
        if (const int32_t copy = levelMeshes_->copyOf(slot); copy >= 0)
            return uint32_t(copy);
    return slot;
}

bool EditorBridge::worldOf(uint32_t index, float world[3][4]) const
{
    if (!editable(index))
        return false;
    std::memcpy(world, capture(index).world, sizeof(float) * 12);
    return true;
}

bool EditorBridge::meshBounds(uint32_t index, float3& center, float3& half) const
{
    if (index >= records_.size())
        return false;
    // A level mesh: its own bounds as drawn (its live mesh has none packed).
    std::shared_ptr<const level::PolyMesh> level = levelMeshes_ && entityOf(index) ? levelMeshes_->drawnOf(entityOf(index).id()) : nullptr;
    if (!level)
        level = levelMeshOf(index);
    if (level && !level->empty()) {
        float3 lo, hi;
        level->bounds(lo, hi);
        center = (lo + hi) * 0.5f;
        half = (hi - lo) * 0.5f;
        return true;
    }
    if (records_[index].slot == ~0u)
        return false;
    uint32_t meshCount = 0;
    const fpak::MeshGpu* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const uint32_t mesh = pak_->instances()[records_[index].slot].mesh;
    if (mesh >= meshCount)
        return false;
    for (int a = 0; a < 3; ++a) {
        (&half.x)[a] = meshes[mesh].posScale[a] * 65535.0f * 0.5f;
        (&center.x)[a] = meshes[mesh].posOffset[a] + (&half.x)[a];
    }
    return true;
}

bool EditorBridge::openingPlace(const Json& m, uint32_t& wallIndex, float& distance, bool onWall)
{
    uint32_t record = ~0u, face = ~0u;
    wallIndex = selected_;
    distance = float(m.getNumber("distance", 1.0));
    if (const Json* at = m.get("at"); at && levelFaceAt(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), record, face))
        wallIndex = record;
    else if (onWall)
        return false;  // (the point not on a level mesh)
    const flecs::entity wallEntity = wallIndex != ~0u ? entityOf(wallIndex) : flecs::entity();
    const LevelWall* wall = wallEntity ? wallEntity.try_get<LevelWall>() : nullptr;
    if (!wall)
        return false;
    if (const Json* at = m.get("at"); at && view_) {  // the distance along it nearest the point
        float3 o, d, point, normal;
        view_->rayAt(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), o, d);
        float w[3][4], inv[3][4];
        if (surfaceHit(o, d, {}, point, normal) && worldOf(wallIndex, w)) {
            affineInverse(w, inv);
            const float2 p = {inv[0][0] * point.x + inv[0][1] * point.y + inv[0][2] * point.z + inv[0][3],
                              inv[2][0] * point.x + inv[2][1] * point.y + inv[2][2] * point.z + inv[2][3]};
            distance = level::wallNearest(wall->wall, p);
        }
    }
    return true;
}

bool EditorBridge::frameOpening(State& s, bool slide, bool lift)
{
    if (!s.opening || s.parent == ~0u)
        return false;
    const State wall = capture(s.parent);
    float ww[3][4];
    if (!wall.wall || !worldOf(s.parent, ww))
        return false;
    if (slide) {  // the nearest point of the wall's path to where it was put, at its height there
        float inv[3][4];
        affineInverse(ww, inv);
        const float x = s.world[0][3], y = s.world[1][3], z = s.world[2][3];
        const float2 p = {inv[0][0] * x + inv[0][1] * y + inv[0][2] * z + inv[0][3], inv[2][0] * x + inv[2][1] * y + inv[2][2] * z + inv[2][3]};
        s.opening->distance = std::clamp(level::wallNearest(*wall.wall, p), 0.0f, level::wallLength(*wall.wall));
        const float up = inv[1][0] * x + inv[1][1] * y + inv[1][2] * z + inv[1][3];  // (its sill: inside the wall)
        if (lift)
            s.opening->sill = std::clamp(up, 0.0f, std::max(wall.wall->height - s.opening->height, 0.0f));
    }
    s.hasLocal = false;
    return LevelMeshes::openingFrame(*wall.wall, ww, *s.opening, s.world);
}

// ---- prefabs

const Prefab* EditorBridge::prefabDef(const std::string& path)
{
    if (path.empty() || !hasProject_)
        return nullptr;
    if (auto it = prefabs_.find(path); it != prefabs_.end())
        return &it->second;
    Prefab def;
    std::string error;
    if (!readPrefab(project_.path(path), def, error)) {
        FX_WARN("editor: %s", error.c_str());
        return nullptr;
    }
    return &(prefabs_[path] = std::move(def));
}

bool EditorBridge::linkedChild(uint32_t index) const
{
    return index < records_.size() && records_[index].prefabRoot != ~0u && records_[index].prefabRoot != index;
}

std::vector<uint32_t> EditorBridge::instanceMembers(uint32_t root) const
{
    std::vector<uint32_t> out;
    for (const uint32_t i : subtree(root))
        if (records_[i].prefabRoot == root)
            out.push_back(i);
    return out;
}

void EditorBridge::localOf(uint32_t index, float out[3][4]) const
{
    const State s = capture(index);
    if (s.parent == ~0u) {
        std::memcpy(out, s.world, sizeof(s.world));
        return;
    }
    float inverse[3][4];
    affineInverse(capture(s.parent).world, inverse);
    affineMultiply(inverse, s.world, out);
}

std::vector<PrefabOverride> EditorBridge::overridesOf(uint32_t root, const Prefab& def) const
{
    std::vector<PrefabOverride> out;
    for (const uint32_t i : instanceMembers(root)) {
        const PrefabEntity* p = def.find(records_[i].prefabChild);
        if (!p)
            continue;
        if (PrefabOverride o = overrideOf(i, *p, i != root); !o.empty())
            out.push_back(std::move(o));
    }
    return out;
}

PrefabOverride EditorBridge::overrideOf(uint32_t i, const PrefabEntity& def, bool place) const
{
    const auto sameLight = [](const std::optional<Light>& a, const std::optional<Light>& b) {
        return a.has_value() == b.has_value() && (!a || std::memcmp(&*a, &*b, sizeof(Light)) == 0);
    };
    const auto sameCamera = [](const std::optional<CameraComponent>& a, const std::optional<CameraComponent>& b) {
        return a.has_value() == b.has_value() && (!a || (a->fov == b->fov && a->nearClip == b->nearClip && a->flags == b->flags));
    };
    {
        const PrefabEntity* p = &def;
        const State s = capture(i);
        PrefabOverride o;
        o.id = p->id;
        if (place && !s.alive) {  // deleted in the instance
            o.removed = true;
            return o;
        }
        if (s.material != p->material)
            o.material = s.material;
        if (!sameLight(s.light, p->light))
            o.light = s.light;
        if (!sameCamera(s.camera, p->camera))
            o.camera = s.camera;
        if (s.body.has_value() != p->body.has_value() || (s.body && std::memcmp(&*s.body, &*p->body, sizeof(PhysicsBody)) != 0))
            o.body = s.body;
        if (s.collider.has_value() != p->collider.has_value() ||
            (s.collider && std::memcmp(&*s.collider, &*p->collider, sizeof(Collider)) != 0))
            o.collider = s.collider;
        if (s.audioSource.has_value() != p->audioSource.has_value() ||
            (s.audioSource && std::memcmp(&*s.audioSource, &*p->audioSource, sizeof(AudioSource)) != 0))
            o.audioSource = s.audioSource;
        if (s.audioListener.has_value() != p->audioListener.has_value() ||
            (s.audioListener && std::memcmp(&*s.audioListener, &*p->audioListener, sizeof(AudioListener)) != 0))
            o.audioListener = s.audioListener;
        if (s.acoustics.has_value() != p->acoustics.has_value() ||
            (s.acoustics && std::memcmp(&*s.acoustics, &*p->acoustics, sizeof(AcousticSurface)) != 0))
            o.acoustics = s.acoustics;
        if (!(s.vfx == p->vfx))
            o.vfx = s.vfx;
        if (s.scripts != p->scripts)
            o.scripts = s.scripts;
        if (place) {  // (the root's transform, name and visibility are the instance's own)
            float local[3][4];
            localOf(i, local);
            for (int r = 0; r < 3 && !o.hasLocal; ++r)
                for (int c = 0; c < 4; ++c)
                    if (std::fabs(local[r][c] - p->local[r][c]) > 1e-4f) {
                        o.hasLocal = true;
                        break;
                    }
            if (o.hasLocal)
                std::memcpy(o.local, local, sizeof(local));
            if (s.hidden != p->hidden)
                o.hidden = s.hidden;
            if (s.name != p->name)
                o.name = s.name;
        }
        return o;
    }
}

PrefabEntity EditorBridge::prefabEntityOf(uint32_t i, bool nest) const
{
    const State s = capture(i);
    const Record& r = records_[i];
    PrefabEntity p;
    p.name = s.name;
    p.hidden = s.hidden;
    if (nest && r.prefabRoot == i && !r.prefab.empty()) {  // another prefab's instance: nested
        p.kind = "prefab";
        p.prefab = r.prefab;
        if (const Prefab* inner = const_cast<EditorBridge*>(this)->prefabDef(r.prefab))
            p.overrides = overridesOf(i, *inner);
        return p;
    }
    p.kind = r.kind == EntityKind::Primitive ? "mesh" : r.kind == EntityKind::Mesh ? "empty" : entityKindName(r.kind);
    p.mesh = r.kind == EntityKind::Primitive ? r.primitive : "";
    p.material = s.material;
    p.light = s.light;
    p.camera = s.camera;
    p.body = s.body;
    p.collider = s.collider;
    p.audioSource = s.audioSource;
    p.audioListener = s.audioListener;
    p.acoustics = s.acoustics;
    p.vfx = s.vfx;
    p.scripts = s.scripts;
    p.levelMesh = s.levelMesh;
    p.level = levelPartsOfState(s);
    return p;
}

EditorBridge::Links EditorBridge::linksOf(const std::vector<uint32_t>& indices) const
{
    Links out;
    for (const uint32_t i : indices)
        if (i < records_.size())
            out.push_back({i, {records_[i].prefab, records_[i].prefabRoot, records_[i].prefabChild}});
    return out;
}

void EditorBridge::setLinks(const Links& links)
{
    for (const auto& [i, l] : links)
        if (i < records_.size()) {
            records_[i].prefab = l.prefab;
            records_[i].prefabRoot = l.root;
            records_[i].prefabChild = l.child;
        }
    dirty_ = true;
}

void EditorBridge::changeLinks(const Links& after)
{
    std::vector<uint32_t> indices;
    for (const auto& [i, l] : after)
        indices.push_back(i);
    Change c;
    c.index = ~0u;
    c.links = true;
    c.linksBefore = linksOf(indices);
    c.linksAfter = after;
    setLinks(after);
    undo_.push_back({std::move(c)});
    redo_.clear();
    ++commands_;
}

void EditorBridge::reshapeInstance(uint32_t root, const Prefab& def, std::vector<std::pair<uint32_t, State>>& after)
{
    // Gone from the prefab: deleted (undo brings them back, linked as they were).
    std::unordered_map<std::string, uint32_t> member;
    std::unordered_set<std::string> known;  // (members deleted in the instance: removals, not made again)
    for (const uint32_t i : instanceMembers(root)) {
        known.insert(records_[i].prefabChild);
        if (!def.find(records_[i].prefabChild)) {
            State s = capture(i);
            s.alive = false;
            after.emplace_back(i, std::move(s));
        } else if (capture(i).alive) {
            member[records_[i].prefabChild] = i;
        }
    }
    // New in the prefab: made under their parent (in the prefab's order, parents first).
    Links links;
    for (const PrefabEntity& p : def.entities) {
        if (member.count(p.id) || known.count(p.id))
            continue;
        const auto up = member.find(p.parent);
        if (up == member.end())
            continue;
        const EntityKind named = entityKindFromName(p.kind);
        const EntityKind kind = p.kind == "mesh" ? EntityKind::Primitive : named == EntityKind::Primitive ? EntityKind::Empty : named;
        float parentWorld[3][4], world[3][4];
        std::memcpy(parentWorld, capture(up->second).world, sizeof(parentWorld));
        for (const auto& [index, state] : after)  // (its parent moved by this change: where it goes)
            if (index == up->second)
                std::memcpy(parentWorld, state.world, sizeof(parentWorld));
        affineMultiply(parentWorld, p.local, world);
        const uint32_t made = spawn(kind, kind == EntityKind::Light ? (p.light && p.light->type == 2 ? "directional" : p.light && p.light->type == 1 ? "spot" : "point")
                                          : kind == EntityKind::Level ? "box" : p.mesh,
                                    world, p.name, p.material);
        if (made == ~0u)
            continue;
        State s;
        std::memcpy(s.world, world, sizeof(s.world));
        s.parent = up->second;
        s.alive = true;
        s.name = p.name;
        s.hidden = p.hidden;
        s.material = p.material;
        s.light = p.light;
        s.camera = p.camera;
        s.body = p.body;
        s.collider = p.collider;
        s.audioSource = p.audioSource;
        s.audioListener = p.audioListener;
        s.acoustics = p.acoustics;
        s.vfx = p.vfx;
        s.scripts = p.scripts;
        s.levelMesh = p.levelMesh;
        setLevelPartsOfState(s, p.level);
        after.emplace_back(made, std::move(s));
        records_[made].prefabRoot = root;
        records_[made].prefabChild = p.id;
        member[p.id] = made;
    }
}

void EditorBridge::followPrefab(uint32_t root, const Prefab& def, const std::vector<PrefabOverride>& keep,
                                std::vector<std::pair<uint32_t, State>>& after) const
{
    std::unordered_map<uint32_t, std::array<float, 12>> worldOf;  // this change's new world transforms
    for (const uint32_t i : instanceMembers(root)) {
        const PrefabEntity* p = def.find(records_[i].prefabChild);
        if (!p)
            continue;
        const PrefabOverride* o = nullptr;
        for (const PrefabOverride& k : keep)
            if (k.id == p->id)
                o = &k;
        State s = capture(i);
        s.material = o && o->material ? *o->material : p->material;
        if (s.light || p->light)
            s.light = o && o->light ? o->light : p->light;
        if (s.camera || p->camera)
            s.camera = o && o->camera ? o->camera : p->camera;
        s.body = o && o->body ? o->body : p->body;
        s.collider = o && o->collider ? o->collider : p->collider;
        s.audioSource = o && o->audioSource ? o->audioSource : p->audioSource;
        s.audioListener = o && o->audioListener ? o->audioListener : p->audioListener;
        s.acoustics = o && o->acoustics ? o->acoustics : p->acoustics;
        s.vfx = o && o->vfx ? *o->vfx : p->vfx;
        s.scripts = o && o->scripts ? *o->scripts : p->scripts;
        if (p->levelMesh)
            s.levelMesh = p->levelMesh;
        setLevelPartsOfState(s, p->level);
        if (i != root) {
            s.hidden = o && o->hidden ? *o->hidden : p->hidden;
            s.name = o && o->name ? *o->name : p->name;
            float parentWorld[3][4];
            if (auto it = worldOf.find(s.parent); it != worldOf.end())
                std::memcpy(parentWorld, it->second.data(), sizeof(parentWorld));
            else
                std::memcpy(parentWorld, capture(s.parent).world, sizeof(parentWorld));
            affineMultiply(parentWorld, o && o->hasLocal ? o->local : p->local, s.world);
        }
        std::memcpy(worldOf[i].data(), s.world, sizeof(s.world));
        after.emplace_back(i, std::move(s));
    }
}

uint32_t EditorBridge::idByPath(const std::string& name) const
{
    // "Crate (1)/Lid": a root of that name, then a child of each name in turn.
    uint32_t at = ~0u;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        uint32_t found = ~0u;
        for (uint32_t i = 0; i < records_.size() && found == ~0u; ++i)
            if (entityOf(i) && capture(i).alive && parentOf(i) == at && defaultName(i) == part)
                found = i;
        for (uint32_t i = 0; i < records_.size() && found == ~0u; ++i)  // (else a deleted one: tests expect it gone)
            if (entityOf(i) && parentOf(i) == at && defaultName(i) == part)
                found = i;
        if (found == ~0u)
            return 0;
        at = found;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return at == ~0u ? 0 : at + 1;
}

bool EditorBridge::meshBoundsOf(uint32_t mesh, float3& center, float3& half) const
{
    uint32_t meshCount = 0;
    const fpak::MeshGpu* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    if (mesh >= meshCount)
        return false;
    for (int a = 0; a < 3; ++a) {
        (&half.x)[a] = meshes[mesh].posScale[a] * 65535.0f * 0.5f;
        (&center.x)[a] = meshes[mesh].posOffset[a] + (&half.x)[a];
    }
    return true;
}

float3 EditorBridge::placementPoint(float fx, float fy, const CameraState& camera, uint32_t* entity) const
{
    float3 o = camera.position, d = camera.forward;
    if (view_)
        view_->rayAt(fx, fy, o, d);
    float best = 1e30f;
    float3 hit = o + d * 10.0f;
    uint32_t nearest = ~0u;
    if (float3 p; terrain_ && terrain_->hit(o, d, p) && length(p - o) < best) {
        best = length(p - o);
        hit = p;
    }
    // Meshes: the ray against each visible one's box, in the mesh's own space.
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        if (r.slot == ~0u || hiddenBelow(i) || !entityOf(i))
            continue;
        const State s = capture(i);
        float3 c, h;
        if (!s.alive || !meshBoundsOf(pak_->instances()[r.slot].mesh, c, h))
            continue;
        // World -> mesh space: the inverse of the 3x3 part (scaled, rotated), then the box's slabs.
        const float(*w)[4] = s.world;
        const float det = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) - w[0][1] * (w[1][0] * w[2][2] - w[1][2] * w[2][0]) +
                          w[0][2] * (w[1][0] * w[2][1] - w[1][1] * w[2][0]);
        if (std::fabs(det) < 1e-12f)
            continue;
        float inv[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                const int a1 = (a + 1) % 3, a2 = (a + 2) % 3, b1 = (b + 1) % 3, b2 = (b + 2) % 3;
                inv[b][a] = (w[a1][b1] * w[a2][b2] - w[a1][b2] * w[a2][b1]) / det;
            }
        const float3 rel = {o.x - w[0][3], o.y - w[1][3], o.z - w[2][3]};
        float lo = 0.0f, hi = best;
        bool missed = false;
        for (int a = 0; a < 3 && !missed; ++a) {
            const float lo_ = inv[a][0] * rel.x + inv[a][1] * rel.y + inv[a][2] * rel.z;
            const float dir = inv[a][0] * d.x + inv[a][1] * d.y + inv[a][2] * d.z;
            const float mn = (&c.x)[a] - (&h.x)[a], mx = (&c.x)[a] + (&h.x)[a];
            if (std::fabs(dir) < 1e-9f) {
                missed = lo_ < mn || lo_ > mx;
                continue;
            }
            float t0 = (mn - lo_) / dir, t1 = (mx - lo_) / dir;
            if (t0 > t1)
                std::swap(t0, t1);
            lo = std::max(lo, t0);
            hi = std::min(hi, t1);
            missed = lo > hi;
        }
        if (!missed && lo > 0.0f && lo < best) {
            best = lo;
            hit = o + d * lo;
            nearest = i;
        }
    }
    if (d.y < -1e-4f && o.y > 0.0f) {  // the ground plane
        const float t = -o.y / d.y;
        if (t < best) {
            best = t;
            hit = o + d * t;
            nearest = ~0u;
        }
    }
    if (entity)
        *entity = nearest;
    return hit;
}

void EditorBridge::markers(std::vector<EntityMarker>& out) const
{
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        const flecs::entity e = entityOf(i);
        if (!e || !e.enabled() || hiddenBelow(i))
            continue;
        // A Light or camera added to a mesh or an empty: its icon (a mesh has none otherwise).
        const bool added = (r.kind == EntityKind::Mesh || r.kind == EntityKind::Primitive || r.kind == EntityKind::Empty) &&
                           (e.has<Light>() || e.has<CameraComponent>());
        const AudioSource* audio = e.try_get<AudioSource>();
        const bool vfx = e.has<ParticleEffect>() || e.has<Decal>() || e.has<ParticleSphereEmitter>() || e.has<ParticleBoxEmitter>() ||
                         e.has<ParticleConeEmitter>() || e.has<ParticleRingEmitter>();
        const bool spline = e.has<LevelSpline>() || e.has<LevelVolume>();  // (a volume's mesh isn't drawn: its outline is)
        if (r.slot != ~0u && !added && !audio && !vfx && !spline)
            continue;
        if (r.kind == EntityKind::Bone && i != selected_ && !added && !audio)
            continue;  // (a skeleton's bones: the selected one)
        EntityMarker m;
        m.index = i;
        // Particles: the effect's icon, the emitter's shape (by its constant or largest value), a decal's box.
        const auto most = [](const ParticleFloat& f) { return f.mode == ParticleFloat::Range ? std::max(f.a, f.b) : f.a; };
        m.effect = e.has<ParticleEffect>();
        if (const Decal* d = e.try_get<Decal>()) {
            m.decal = true;
            std::memcpy(m.decalSize, d->size, sizeof(m.decalSize));
        }
        if (const CutVolume* c = e.try_get<CutVolume>()) {
            m.cutShape = uint32_t(c->shape) + 1;
            m.opening = LevelMeshes::openingWorld(e, m.openingWorld);
            m.parent = parentOf(i);
        }
        if (spline)
            splineWorld(i, &m.splinePoints, &m.splineCurve);
        m.snapPoint = e.has<SnapPoint>();
        if (const LevelVolume* vol = e.try_get<LevelVolume>()) {  // (a volume: its mesh's edges, world)
            m.volumeKind = uint32_t(vol->kind) + 1;
            float w[3][4];
            if (const auto mesh = levelMeshOf(i); mesh && worldOf(i, w)) {
                const auto at = [&](float3 p) {
                    return float3{w[0][0] * p.x + w[0][1] * p.y + w[0][2] * p.z + w[0][3], w[1][0] * p.x + w[1][1] * p.y + w[1][2] * p.z + w[1][3],
                                  w[2][0] * p.x + w[2][1] * p.y + w[2][2] * p.z + w[2][3]};
                };
                for (const level::Face& f : mesh->faces)
                    for (uint32_t k = 0; k < f.count; ++k) {
                        const uint32_t a = mesh->corners[f.first + k], b = mesh->corners[f.first + (k + 1) % f.count];
                        if (a < b) {  // (each edge once)
                            m.volumeEdges.push_back(at(mesh->positions[a]));
                            m.volumeEdges.push_back(at(mesh->positions[b]));
                        }
                    }
            }
        }
        if (const auto* s = e.try_get<ParticleSphereEmitter>()) {
            m.emitterShape = 1;
            m.shape[0] = most(s->radius);
        } else if (const auto* b = e.try_get<ParticleBoxEmitter>()) {
            m.emitterShape = 2;
            std::memcpy(m.shape, b->size, 12);
        } else if (const auto* c = e.try_get<ParticleConeEmitter>()) {
            m.emitterShape = 3;
            m.shape[0] = most(c->angle), m.shape[1] = most(c->radius), m.shape[2] = std::max(most(c->length), 0.5f);
        } else if (const auto* g = e.try_get<ParticleRingEmitter>()) {
            m.emitterShape = 4;
            m.shape[0] = most(g->radius), m.shape[1] = most(g->arc);
        }
        m.kind = added ? (e.has<Light>() ? EntityKind::Light : EntityKind::Camera) : r.kind == EntityKind::Bone ? EntityKind::Empty : r.kind;
        std::memcpy(m.world, capture(i).world, sizeof(m.world));
        if (const Light* l = e.try_get<Light>()) {
            m.lightType = l->type;
            float peak = 0.0f;
            for (int c = 0; c < 3; ++c) {
                m.color[c] = l->color[c];
                peak = std::max(peak, l->color[c] * l->intensity);
            }
            m.range = l->range > 0.0f ? l->range : std::sqrt(std::max(peak, 1e-6f) / 0.01f);  // LightSystem's rule
            m.outerAngle = l->outerAngle;
        }
        if (const CameraComponent* c = e.try_get<CameraComponent>()) {
            m.fov = c->fov;
            m.mainCamera = (c->flags & 1u) != 0;
        }
        if (const int32_t body = waterBody(i); body >= 0) {
            const WaterBodyDesc& d = water_->body(uint32_t(body))->desc;
            m.waterShape = uint32_t(d.shape);
            m.waterSize[0] = d.width;
            m.waterSize[1] = d.length;
            m.waterSize[2] = d.depth;
            m.waterPoints = d.points.data();
            m.waterPointCount = uint32_t(d.points.size() / 2);
        }
        if (audio) {  // (a .fsound's distances unless the source's own are used)
            m.audio = true;
            m.audioSpatial = (audio->flags & 4u) != 0;
            m.audioMin = audio->minDistance;
            m.audioMax = audio->maxDistance;
            if (SoundAsset def; !(audio->flags & 8u) && audio_ && audio_->soundSettings(soundPath(audio->sound), def)) {
                m.audioSpatial = def.spatial;
                m.audioMin = def.minDistance;
                m.audioMax = def.maxDistance;
            }
            float transmission[3];
            if (audio_ && !audio_->directOf(e.id(), m.audioOcclusion, transmission))
                m.audioOcclusion = -1.0f;
        }
        out.push_back(m);
    }
}

// The world bounds of every mesh in the entity's subtree, added to this frame's live region.
void EditorBridge::growLiveRegion(uint32_t index)
{
    if (!liveMoved_)
        for (int a = 0; a < 3; ++a) {
            liveRegion_[a] = 1e30f;
            liveRegion_[3 + a] = -1e30f;
        }
    liveMoved_ = true;
    for (const uint32_t i : subtree(index)) {
        float3 c, half;
        float w[3][4];
        if (!meshBounds(i, c, half) || !worldOf(i, w))
            continue;
        for (int a = 0; a < 3; ++a) {
            const float center = w[a][0] * c.x + w[a][1] * c.y + w[a][2] * c.z + w[a][3];
            const float extent = std::fabs(w[a][0]) * half.x + std::fabs(w[a][1]) * half.y + std::fabs(w[a][2]) * half.z;
            liveRegion_[a] = std::min(liveRegion_[a], center - extent);
            liveRegion_[3 + a] = std::max(liveRegion_[3 + a], center + extent);
        }
    }
}

void EditorBridge::setWorldLive(uint32_t index, const float world[3][4])
{
    if (!editable(index))
        return;
    State s = capture(index);
    std::memcpy(s.world, world, sizeof(s.world));
    s.hasLocal = false;
    if (s.opening)
        frameOpening(s, true);  // (an opening slides along its wall)
    growLiveRegion(index);  // where it was
    apply(index, s);
    growLiveRegion(index);  // and where it is now
    liveDirty_ = true;
}

void EditorBridge::commitWorld(uint32_t index, const float before[3][4])
{
    if (!editable(index))
        return;
    const State after = capture(index);
    if (std::memcmp(after.world, before, sizeof(after.world)) == 0)
        return;
    State b = after;
    std::memcpy(b.world, before, sizeof(b.world));
    b.hasLocal = false;
    if (b.opening)
        frameOpening(b, true);  // (where it was along its wall)
    undo_.push_back({{index, b, after}});
    redo_.clear();
    ++commands_;
    liveDirty_ = false;
    sceneChanged();
    sendState(false);
}

void EditorBridge::toolsChanged()
{
    sendState(false);
}

bool EditorBridge::simulateFrame()
{
    if (!playing_)
        return false;
    if (!paused_)
        return true;
    if (steps_ == 0)
        return false;
    --steps_;
    return true;
}

uint32_t EditorBridge::spawn(EntityKind kind, const std::string& primitive, const float world[3][4], std::string name,
                            const std::string& material)
{
    Record r;
    r.kind = kind;
    flecs::entity e;
    if (kind == EntityKind::Primitive) {
        if (!pools_)
            return ~0u;
        auto pool = pools_->end();
        if (!material.empty()) {  // the material's displaced copy of the mesh, while it has slots
            const std::string displaced = displacedMeshName(primitive, material);
            pool = std::find_if(pools_->begin(), pools_->end(), [&](const PrimitivePool& p) { return p.name == displaced; });
            if (pool != pools_->end() && pool->used >= pool->size)
                pool = pools_->end();
        }
        if (pool == pools_->end())
            pool = std::find_if(pools_->begin(), pools_->end(), [&](const PrimitivePool& p) { return p.name == primitive; });
        if (pool == pools_->end()) {
            notice(("No mesh " + primitive + " in this scene (after an import: Reload the scene)").c_str(), "error");
            return ~0u;
        }
        if (pool->used >= pool->size) {
            notice(("No free " + primitive + " slots left this session (save and Reload the scene to place more)").c_str(), "error");
            return ~0u;
        }
        r.slot = pool->first + pool->used++;
        r.primitive = primitive;
        fpak::InstanceGpu& row = pak_->mutableInstances()[r.slot];
        std::memcpy(row.world, world, sizeof(row.world));
        e = scene_->spawnInstance(r.slot, row);
    } else {
        e = scene_->spawnTransform(world);
        if (kind == EntityKind::Light) {
            const Light l = defaultLight(lightTypeOf(primitive));
            e.set<Light>(l);
            r.primitive = lightTypeName(l.type);
        } else if (kind == EntityKind::Water) {
            // A kind of water to start from: lake, pond, pool, ocean, volume.
            if (!water_)
                return ~0u;
            WaterBodyDesc d;
            if (primitive == "pond") {
                d.shape = WaterShape::Circle;
                d.width = d.length = 12.0f;
                d.depth = 2.0f;
                d.material = "builtin:oasis";
            } else if (primitive == "pool") {
                d.width = 10.0f;
                d.length = 5.0f;
                d.depth = 1.6f;
                d.material = "builtin:pool";
            } else if (primitive == "ocean") {
                d.shape = WaterShape::Ocean;
                d.material = "builtin:ocean";
                d.depth = 50.0f;
            } else if (primitive == "river") {
                d.shape = WaterShape::River;
                d.width = 6.0f;
                d.depth = 2.0f;
                d.points = {-24.0f, 0.0f, -9.0f, 5.0f, 6.0f, -4.0f, 22.0f, 2.0f};
                d.heights = {0.6f, 0.4f, 0.15f, 0.0f};
                d.material = "builtin:river";
            } else if (primitive == "volume") {
                d.shape = WaterShape::Box;
                d.width = d.length = 4.0f;
                d.depth = 3.0f;
                d.material = "builtin:pool";
            }
            if (water_->add(d, e.id()) == ~0u) {
                e.destruct();
                notice("No room for more water bodies", "error");
                return ~0u;
            }
            r.primitive = waterShapeName(d.shape);
        } else if (kind == EntityKind::Level) {
            // A shape to start from (LevelMeshes gives it its live mesh and slot; update() records the slot).
            auto mesh = std::make_shared<level::PolyMesh>(primitive == "plane" ? level::PolyMesh::plane(4.0f, 4.0f)
                                                                               : level::PolyMesh::box({1.0f, 1.0f, 1.0f}));
            e.set<LevelMesh>({std::move(mesh)});
            r.primitive = primitive.empty() ? "box" : primitive;
        } else if (kind == EntityKind::Camera) {
            // Unity's defaults; the first camera is the main one.
            bool haveMain = false;
            scene_->world().each([&](flecs::entity, const CameraComponent& c) { haveMain |= (c.flags & 1u) != 0; });
            e.set<CameraComponent>({60.0f, 0.05f, haveMain ? 0u : 1u});
        }
    }
    e.disable();  // comes alive through its create command (and goes again on undo)
    r.entity = e.id();
    r.uid = nextUid_++;
    r.name = name.empty() ? createdName(kind, primitive, r.slot) : std::move(name);
    return addRecord(std::move(r));
}

// Settings the page can change live. Keys and value types match writeSettings().
void EditorBridge::setSetting(const std::string& key, const Json& v)
{
    if (!settings_)
        return;
    FrameSettings& s = *settings_;
    PostSettings& p = s.post;
    const float f = float(v.number);
    const bool b = v.type == Json::Type::Bool ? v.b : v.number != 0.0;
    // Steam: the project's App ID, depot, branch to set live, and relaunching through Steam.
    if (key.starts_with("steam")) {
        if (key == "steamAppId")
            project_.steamAppId = uint32_t(std::clamp(v.number, 0.0, 4294967295.0));
        else if (key == "steamDepotId")
            project_.steamDepotId = uint32_t(std::clamp(v.number, 0.0, 4294967295.0));
        else if (key == "steamBranch")
            project_.steamBranch = v.str;
        else if (key == "steamRestart")
            project_.steamRestartIfNecessary = b;
        else
            return;
        if (hasProject_)
            projectSettingsDirty_ = true;
        return;
    }
    // Network: the project's (saved with it); a preset sets the others, changing one makes it custom.
    if (key.starts_with("netPlay")) {  // (the Play bar's: the editor's prefs)
        if (key == "netPlayClients")
            playClients_ = uint32_t(std::clamp(v.number, 0.0, 8.0));
        else if (key == "netPlayConditions")
            playConditions_ = uint32_t(std::clamp(v.number, 0.0, 3.0));
        return;
    }
    if (key.starts_with("net")) {
        NetworkSettings& n = project_.network;
        if (key == "netPreset") {
            if (!applyNetworkPreset(n, v.str))
                n.preset = "custom";
        } else {
            if (key == "netMode")
                n.mode = v.str == "rollback" ? "rollback" : "authoritative";
            else if (key == "netSendRate")
                n.sendRate = uint32_t(std::clamp(v.number, 1.0, 60.0));
            else if (key == "netBandwidth")
                n.bandwidthKbps = uint32_t(std::clamp(v.number, 16.0, 100000.0));
            else if (key == "netMaxPlayers")
                n.maxPlayers = uint32_t(std::clamp(v.number, 0.0, 100000.0));
            else if (key == "netInterpolation")
                n.interpolationMs = float(std::clamp(v.number, 0.0, 1000.0));
            else if (key == "netInputDelay")
                n.inputDelay = uint32_t(std::clamp(v.number, 0.0, 8.0));
            else if (key == "netRollbackWindow")
                n.rollbackWindow = uint32_t(std::clamp(v.number, 4.0, 120.0));
            else if (key == "netPort")
                n.port = uint32_t(std::clamp(v.number, 1.0, 65535.0));
            else
                return;
            n.preset = "custom";
        }
        if (hasProject_)
            projectSettingsDirty_ = true;
        return;
    }
    // Audio: the project's (saved with it) volumes and HRTF switch, heard at once.
    static const char* const kAudioKeys[] = {"audioSfx", "audioPhysics", "audioAmbience", "audioMusic", "audioUi"};
    if (key.starts_with("audio")) {
        AudioSettings& a = project_.audio;
        if (key == "audioMaster")
            a.master = std::clamp(f, 0.0f, 4.0f);
        else if (key == "audioHrtf")
            a.hrtf = b;
        else if (key == "audioReflections")
            a.reflections = uint32_t(std::clamp(f, 0.0f, 3.0f));
        else if (key == "audioReverbType")
            a.reverbType = uint32_t(std::clamp(f, 0.0f, 2.0f));
        else if (key == "audioReverb")
            a.reverb = std::clamp(f, 0.0f, 4.0f);
        else if (key == "navAgentHeight")
            project_.navigation.agentHeight = std::clamp(f, 0.2f, 10.0f), navStale_ = true;
        else if (key == "navAgentRadius")
            project_.navigation.agentRadius = std::clamp(f, 0.05f, 5.0f), navStale_ = true;
        else if (key == "navAgentClimb")
            project_.navigation.agentClimb = std::clamp(f, 0.0f, 5.0f), navStale_ = true;
        else if (key == "navMaxSlope")
            project_.navigation.maxSlope = std::clamp(f, 0.0f, 89.0f), navStale_ = true;
        else if (key == "navCellSize")
            project_.navigation.cellSize = std::clamp(f, 0.05f, 2.0f), navStale_ = true;
        else if (key == "navAutoBake")
            project_.navAutoBake = b;
        else if (key == "audioPhysicsSounds")
            a.physicsSounds = b;
        else if (key == "audioWaterSounds")
            a.waterSounds = b;
        else if (key == "audioVariation")
            a.variation = b;
        for (int k = 0; k < 5; ++k)
            if (key == kAudioKeys[k])
                a.buses[k] = std::clamp(f, 0.0f, 4.0f);
        if (audio_)
            audio_->applySettings(a);
        projectSettingsDirty_ = hasProject_;
        return;
    }
    if (key == "debugView")
        s.debugView = DebugView(std::min(uint32_t(std::max(f, 0.0f)), uint32_t(DebugView::Count) - 1));
    else if (key == "lodPixels")
        s.lodPixels = std::clamp(f, 0.0f, 256.0f);
    else if (key == "cullPixels")
        s.cullPixels = std::clamp(f, 0.0f, 64.0f);
    else if (key == "terrainPixels")
        s.terrainPixels = std::clamp(f, 0.1f, 64.0f);
    else if (key == "detailDistance")
        s.detailDistance = std::clamp(f, 0.0f, 1000.0f);
    else if (key == "occlusion")
        s.occlusion = b;
    else if (key == "coneCulling")
        s.coneCulling = b;
    else if (key == "freezeCulling")
        s.freezeCulling = b;
    else if (key == "shadowCache")
        s.shadowCache = b;
    else if (key == "shadowBudgetMs")
        s.shadowBudgetMs = std::clamp(f, 0.0f, 16.0f);
    else if (key == "taa")
        p.taa = b;
    else if (key == "fxaa")
        p.fxaa = b;
    else if (key == "msaa")
        p.msaa = std::bit_floor(std::clamp(uint32_t(std::max(f, 1.0f)), 1u, 8u));  // 1, 2, 4 or 8
    else if (key == "upscaler")
        p.upscaler = Upscaler(std::min(uint32_t(std::max(f, 0.0f)), 3u));
    else if (key == "upscaleQuality")
        p.renderScale = kUpscalePresets[std::min(uint32_t(std::max(f, 0.0f)), 4u)];
    else if (key == "renderScale")
        p.renderScale = std::clamp(f, 0.25f, 1.0f);
    else if (key == "sharpness")
        p.sharpness = std::clamp(f, 0.0f, 1.0f);
    else if (key == "gi")
        s.gi.enabled = b;
    else if (key == "reflections")
        s.reflections = b;
    else if (key == "reflectScreenSpace")
        s.reflect.screenSpace = b;
    else if (key == "reflectHalfRes")
        s.reflect.halfRes = b;
    else if (key == "reflectIntensity")
        s.reflect.intensity = std::clamp(f, 0.0f, 4.0f);
    else if (key == "reflectRoughness")
        s.reflect.maxRoughness = std::clamp(f, 0.0f, 1.0f);
    else if (key == "reflectThickness")
        s.reflect.thickness = std::clamp(f, 0.1f, 8.0f);
    else if (key == "reflectSteps")
        s.reflect.steps = uint32_t(std::clamp(f, 8.0f, 128.0f));
    else if (key == "giIntensity")
        s.gi.intensity = std::max(f, 0.0f);
    else if (key == "skyIntensity")
        s.gi.skyIntensity = std::max(f, 0.0f);
    else if (key == "giHalfRes")
        ;  // (GI is sampled per pixel; older editors still send it)
    else if (key == "giProbeSpacing")
        s.gi.probeSpacing = std::clamp(f, 0.5f, 2.0f);
    else if (key == "giProbeRays")
        s.gi.probeRays = uint32_t(std::clamp(f, 32768.0f, 524288.0f));
    else if (key == "aoHalfRes")
        p.aoHalfRes = b;
    else if (key == "ao")
        p.ao = AoMode(std::min(uint32_t(std::max(f, 0.0f)), 2u));
    else if (key == "aoRadius")
        p.aoRadius = std::max(f, 0.01f);
    else if (key == "aoPower")
        p.aoPower = std::max(f, 0.0f);
    else if (key == "bloom")
        p.bloomIntensity = std::max(f, 0.0f);
    else if (key == "exposure")
        p.exposureCompensation = f;
    else if (key == "manualExposure")
        p.manualExposure = std::max(f, 0.0f);
    else if (key == "exposureLiftKnee")
        p.exposureLiftKnee = f;
    else if (key == "lookPower")
        p.lookPower = std::max(f, 0.05f);
    else if (key == "lookSaturation")
        p.lookSaturation = std::max(f, 0.0f);
    else if (key == "cameraSpeed" && camera_) {
        camera_->setSpeed(f);
    } else if (key == "vsync" && gpu_) {
        gpu_->setPresentMode(b ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
    }
    if (hasProject_ && projectSetting(key)) {
        project_.lodPixels = s.lodPixels;
        project_.cullPixels = s.cullPixels;
        project_.terrainPixels = s.terrainPixels;
        project_.detailDistance = s.detailDistance;
        projectSettingsDirty_ = true;
    }
}

void EditorBridge::writeSettings(JsonWriter& w) const
{
    if (!settings_)
        return;
    const FrameSettings& s = *settings_;
    const PostSettings& p = s.post;
    w.beginObject("settings")
        .value("debugView", uint32_t(s.debugView)).value("lodPixels", double(s.lodPixels))
        .value("cullPixels", double(s.cullPixels)).value("terrainPixels", double(s.terrainPixels))
        .value("detailDistance", double(s.detailDistance))
        .value("projectSettings", hasProject_)  // (these four are the project's, as the audio* are)
        .value("audioMaster", double(project_.audio.master)).value("audioSfx", double(project_.audio.buses[0]))
        .value("audioPhysics", double(project_.audio.buses[1])).value("audioAmbience", double(project_.audio.buses[2]))
        .value("audioMusic", double(project_.audio.buses[3])).value("audioUi", double(project_.audio.buses[4]))
        .value("audioHrtf", project_.audio.hrtf).value("audioReflections", project_.audio.reflections)
        .value("audioReverbType", project_.audio.reverbType).value("audioReverb", double(project_.audio.reverb))
        .value("audioPhysicsSounds", project_.audio.physicsSounds).value("audioWaterSounds", project_.audio.waterSounds)
        .value("audioVariation", project_.audio.variation)
        .value("navAgentHeight", double(project_.navigation.agentHeight)).value("navAgentRadius", double(project_.navigation.agentRadius))
        .value("navAgentClimb", double(project_.navigation.agentClimb)).value("navMaxSlope", double(project_.navigation.maxSlope))
        .value("navCellSize", double(project_.navigation.cellSize)).value("navAutoBake", project_.navAutoBake)
        .value("netPreset", project_.network.preset).value("netMode", project_.network.mode)
        .value("netSendRate", project_.network.sendRate).value("netBandwidth", project_.network.bandwidthKbps)
        .value("netMaxPlayers", project_.network.maxPlayers).value("netInterpolation", double(project_.network.interpolationMs))
        .value("netInputDelay", project_.network.inputDelay).value("netRollbackWindow", project_.network.rollbackWindow)
        .value("steamAppId", project_.steamAppId).value("steamDepotId", project_.steamDepotId)
        .value("steamBranch", project_.steamBranch).value("steamRestart", project_.steamRestartIfNecessary)
        .value("netPort", project_.network.port).value("netPlayClients", playClients_).value("netPlayConditions", playConditions_)
        .value("audioSpatializer", audio_ && audio_->active() ? audio_->spatializerName() : "off")
        .value("occlusion", s.occlusion).value("coneCulling", s.coneCulling).value("freezeCulling", s.freezeCulling)
        .value("shadowCache", s.shadowCache).value("shadowBudgetMs", double(s.shadowBudgetMs)).value("taa", p.taa).value("ao", uint32_t(p.ao))
        .value("aoRadius", double(p.aoRadius)).value("aoPower", double(p.aoPower)).value("bloom", double(p.bloomIntensity))
        .value("exposure", double(p.exposureCompensation)).value("manualExposure", double(p.manualExposure)).value("exposureLiftKnee", double(p.exposureLiftKnee))
        .value("lookPower", double(p.lookPower)).value("lookSaturation", double(p.lookSaturation))
        .value("cameraSpeed", camera_ ? double(camera_->speed()) : 0.0)
        .value("vsync", gpu_ && gpu_->presentMode() == SDL_GPU_PRESENTMODE_VSYNC)
        .value("gi", s.gi.enabled).value("reflections", s.reflections)
        .value("reflectScreenSpace", s.reflect.screenSpace).value("reflectHalfRes", s.reflect.halfRes)
        .value("reflectIntensity", double(s.reflect.intensity)).value("reflectRoughness", double(s.reflect.maxRoughness))
        .value("reflectThickness", double(s.reflect.thickness)).value("reflectSteps", s.reflect.steps).value("giIntensity", double(s.gi.intensity))
        .value("skyIntensity", double(s.gi.skyIntensity)).value("giProbeSpacing", double(s.gi.probeSpacing))
        .value("giProbeRays", s.gi.probeRays)
        .value("aoHalfRes", p.aoHalfRes).value("fxaa", p.fxaa).value("msaa", p.msaa)
        .value("upscaler", uint32_t(p.upscaler)).value("upscaleQuality", upscalePreset(p.renderScale))
        .value("renderScale", double(p.renderScale)).value("sharpness", double(p.sharpness))
        .value("upscalerName", renderer_ ? renderer_->upscalerStatus() : std::string())
        .endObject();
}

void EditorBridge::writeTools(JsonWriter& w) const
{
    if (!view_)
        return;
    const SceneToolState& t = view_->tools();
    static const char* const kTools[] = {"view", "move", "rotate", "scale", "brush", "paint", "spline", "measure"};
    w.beginObject("tools").value("tool", kTools[uint32_t(t.tool)]).value("local", t.local)
        .value("pivotCenter", t.pivotCenter).value("grid", t.grid).value("gameView", t.gameView)
        .value("snapMove", double(t.snapMove)).value("snapRotate", double(t.snapRotate))
        .value("snapScale", double(t.snapScale)).value("snapAbsolute", t.snapAbsolute)
        .value("paintMode", t.paintMode).value("paintRadius", double(t.paintRadius)).value("paintStrength", double(t.paintStrength))
        .value("paintColor", t.paintColor).value("paintDecal", t.paintDecal)
        .value("paintDamage", t.paintDamage).value("paintRubble", t.paintRubble).value("paintCap", t.paintCap).value("splineProfile", t.splineProfile).value("painting", paint_.active)
        .value("damageSpacing", double(t.damageSpacing)).value("damageRandomness", double(t.damageRandomness)).value("damageDensity", double(t.damageDensity));
    w.value("scatterDensity", double(t.scatterDensity)).value("scatterSpacing", double(t.scatterSpacing))
        .value("scatterSlope", double(t.scatterSlope)).value("scatterAlign", t.scatterAlign).value("showNav", t.showNav)
        .value("orthographic", camera_ && camera_->orthographic())
        .value("navPolys", level::navigation() ? level::navigation()->polygonCount() : 0u).value("navStale", navStale_).beginArray("scatterScale")
        .value(nullptr, double(t.scatterScale[0])).value(nullptr, double(t.scatterScale[1])).endArray().beginArray("scatterPrefabs");
    for (const std::string& p : t.scatterPrefabs)
        w.value(nullptr, p);
    w.endArray();
    // Element modes (the level tools): the mode, whether the selection is a level mesh (the modes
    // apply), and the elements chosen.
    static const char* const kElements[] = {"object", "vertex", "edge", "face"};
    const bool level = selected_ != ~0u && levelMeshOf(selected_) != nullptr;
    w.value("element", kElements[uint32_t(t.element)]).value("levelSelected", level)
        .value("elementCount", uint32_t(level && elements_.entity == selected_ && elements_.mode == uint32_t(t.element) ? elements_.items.size() : 0))
        .value("hint", hint_).value("captured", view_->captured()).value("lastCommand", lastCommand_)
        .endObject();
}

bool EditorBridge::clippedOf(uint32_t index) const
{
    if (!levelMeshes_ || index >= records_.size())
        return false;
    const Record& r = records_[index];
    if (r.kind == EntityKind::Level)
        return r.entity && levelMeshes_->clipped(r.entity);
    return r.slot != ~0u && levelMeshes_->copyClipped(r.slot);
}

bool EditorBridge::saveScene()
{
    if (terrain_ && !terrain_->save()) {
        notice(("Could not write " + Terrain::dirFor(scenePath_)).c_str(), "error");
        return false;
    }
    if (!save()) {
        notice(("Could not write " + editsPath_).c_str(), "error");
        return false;
    }
    // Its cuts baked as they are drawn now (level/cut_bake.h): the game, and the editor opening it
    // again, don't cut them again.
    if (levelMeshes_ && !levelMeshes_->saveBake(scenePath_))
        notice(("Could not write " + level::cutBakePathFor(scenePath_)).c_str(), "error");
    notice(("Saved " + editsPath_).c_str());
    // The scene's preview: the view as it is now (captured at the end of this frame).
    if (const std::string preview = assets_.sceneSaved(); !preview.empty() && captureRequest_.empty()) {
        captureRequest_ = preview;
        captureViewport_ = true;
        captureView_.clear();
        captureWidth_ = 320;
    }
    editorEvents_.push_back("{\"type\":\"saved\"}");
    return true;
}

void EditorBridge::switchTo(const std::vector<std::string>& args, bool saveFirst)
{
    if (saveFirst && !saveScene())
        return;
    std::string command;
    for (const std::string& a : args)
        command += (command.empty() ? "" : " ") + a;
    if (!relaunch_) {
        FX_INFO("editor: would run falcon %s (--no-relaunch)", command.c_str());
        launched_ = command;
        return;
    }
    if (prefs_)
        persist();
    if (!startFalcon(args))
        return notice("Could not start the editor again", "error");
    exitRequested_ = true;
}

void EditorBridge::switchScene(const std::string& scene, bool saveFirst)
{
    if (saveFirst && !saveScene())
        return;
    FX_INFO("editor: opening %s", scene.c_str());
    sceneSwitch_ = scene;  // (the engine takes it at the end of the frame)
}

void EditorBridge::openScene(const std::string& scene, bool saveFirst)
{
    if (!sceneNeedsCook(project_, scene))
        return switchScene(scene, saveFirst);
    if (cook_)
        return notice(("Cooking " + cook_->scene + " already").c_str(), "error");
    if (saveFirst && !saveScene())
        return;
    auto cook = std::make_unique<SceneCook>();
    cook->scene = scene;
    SceneCook* c = cook.get();
    // This scene's pages are open here: its cook is moved into place by the next editor.
    const bool install = scene != sceneInProject_;
    cook->done = std::async(std::launch::async, [c, project = project_, scene, install] {
        const auto progress = [c](const CookProgress& p) {
            std::lock_guard lock(c->lock);
            c->progress = p;
            return !c->cancel.load();
        };
        return cookProjectScene(project, scene, c->error, progress, install);
    });
    cook_ = std::move(cook);
    cookStages_.clear();
    cookReused_ = 0;
    FX_INFO("editor: cooking %s before opening it", scene.c_str());
}

void EditorBridge::updateCook()
{
    if (!cook_)
        return;
    const bool finished = cook_->done.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    CookProgress p;
    {
        std::lock_guard lock(cook_->lock);
        p = cook_->progress;  // (after `finished`: its last progress when it is)
    }
    if (!p.stage.empty() && std::find(cookStages_.begin(), cookStages_.end(), p.stage) == cookStages_.end())
        cookStages_.push_back(p.stage);
    cookReused_ = std::max(cookReused_, p.reused);
    const bool ok = finished && cook_->done.get();
    JsonWriter w;
    w.beginObject().value("type", "editor.cook").value("scene", cook_->scene).value("stage", p.stage.empty() ? "import" : p.stage);
    w.value("done", p.done).value("total", p.total).value("reused", p.reused).value("models", uint64_t(p.models));
    w.value("running", !finished);
    if (finished && !ok)
        w.value("error", cook_->error);
    w.endObject();
    if (w.str() != cook_->sent) {
        cook_->sent = w.str();
        ui_->send(w.str());
    }
    if (!finished)
        return;
    const std::string scene = cook_->scene, error = cook_->error;
    cook_.reset();
    if (!ok)
        return notice(error == "Cancelled" ? ("Cooking " + scene + " cancelled").c_str() : error.c_str(),
                      error == "Cancelled" ? "info" : "error");
    switchScene(scene, false);  // (the open scene's cook waits in Cache/Cook: moved in once it is let go)
}

const AudioBakedData* EditorBridge::audioBaked() const { return audio_ ? audio_->bakedData() : nullptr; }

bool EditorBridge::audioBakeRegion(float3& lo, float3& hi) const
{
    if (!audio_)
        return false;
    const AudioBakeSettings& s = audio_->bakeSettings();
    if (!s.autoRegion) {
        lo = s.center - s.size * 0.5f;
        hi = s.center + s.size * 0.5f;
        return true;
    }
    if (const AudioBakedData* b = audio_->bakedData()) {  // (auto: as the last bake found it)
        lo = b->info.lo;
        hi = b->info.hi;
        return true;
    }
    return false;
}

void EditorBridge::updateAudioBake()
{
    if (!audio_ || !ui_)
        return;
    const AudioBakeStatus st = audio_->bakeStatus();
    const AudioBakeSettings& s = audio_->bakeSettings();
    // Notices: a bake done or failed, the baked data gone stale.
    if (st.state != bakeState_ || st.error != bakeError_) {
        if (bakeState_ == "baking" && st.state != "baking") {
            if (!st.error.empty())
                notice(st.error == "Cancelled" ? "Bake cancelled" : ("Bake failed: " + st.error).c_str(), st.error == "Cancelled" ? "info" : "error");
            else
                notice(("Baked acoustics: " + std::to_string(st.info.probes) + " probes in " + std::to_string(int(std::lround(st.info.seconds))) +
                        " s")
                           .c_str(),
                       "info");
        } else if (st.state == "stale" && bakeState_ == "baked") {
            notice(("Baked acoustics out of date: " + st.staleWhy + ". Bake again (Audio tab)").c_str(), "warning");
        }
        bakeState_ = st.state;
        bakeError_ = st.error;
    }
    JsonWriter w;
    w.beginObject().value("type", "editor.audioBake").value("state", st.state).value("stage", st.stage);
    w.value("fraction", double(std::floor(st.fraction * 100.0f) / 100.0f)).value("probes", st.probes);
    w.value("staleWhy", st.staleWhy).value("error", st.error);
    const auto vec = [&](const char* key, float3 v) {
        w.beginArray(key).value(nullptr, double(v.x)).value(nullptr, double(v.y)).value(nullptr, double(v.z)).endArray();
    };
    w.beginObject("info").value("probes", st.info.probes).value("reverb", st.info.reverb).value("pathing", st.info.pathing);
    w.value("bytes", st.info.bytes).value("seconds", st.info.seconds).value("bakedAt", st.info.bakedAt);
    w.value("spacing", double(st.info.spacing)).value("quality", st.info.quality);
    vec("lo", st.info.lo);
    vec("hi", st.info.hi);
    w.endObject();
    w.beginObject("settings").value("spacing", double(s.spacing)).value("height", double(s.height)).value("quality", s.quality);
    w.value("pathing", s.pathing).value("autoRegion", s.autoRegion);
    vec("center", s.center);
    vec("size", s.size);
    w.endObject();
    w.value("showProbes", showAudioProbes_).endObject();
    if (w.str() != bakeSent_) {
        bakeSent_ = w.str();
        ui_->send(w.str());
    }
}

void EditorBridge::terrainEdited(uint32_t edit)
{
    if (edit != ~0u) {
        Change c;
        c.index = ~0u;
        c.terrainEdit = edit;
        undo_.push_back({std::move(c)});
        redo_.clear();
        dirty_ = true;
        ++commands_;
    }
    sendState(false);
}

void EditorBridge::extract(std::vector<InstanceUpdate>& rows)
{
    for (uint32_t i = 0; i < records_.size(); ++i)  // the eye hides water too
        if (const int32_t body = waterBody(i); body >= 0)
            water_->setHidden(uint32_t(body), hiddenBelow(i));
    std::sort(streamed_.begin(), streamed_.end());
    streamed_.erase(std::unique(streamed_.begin(), streamed_.end()), streamed_.end());
    for (const uint32_t slot : streamed_)
        rows.push_back({slot, pak_->instances()[slot]});
    streamed_.clear();
}

std::vector<NodeAnimator::Link> EditorBridge::animationLinks() const
{
    std::vector<NodeAnimator::Link> links;
    if (!pak_ || pak_->nodeTracks().empty())
        return links;
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        if (r.model.empty() || (r.modelNode < 0 && r.modelCamera < 0) || !entityOf(i))
            continue;
        const int32_t library = pak_->libraryIndex(r.model);
        if (library < 0)
            continue;
        const bool camera = r.modelCamera >= 0;
        if (const ScenePak::NodeTrack* t = pak_->nodeTrack(uint32_t(library), uint32_t(camera ? r.modelCamera : r.modelNode), camera))
            links.push_back({r.entity, t});
    }
    // The scene's own moving instances (a glTF scene opened directly).
    for (const ScenePak::NodeTrack& t : pak_->nodeTracks())
        if (t.library == fpak::kNodeAnimScene && !t.camera)
            if (const flecs::entity e = scene_->entity(t.target))
                links.push_back({e.id(), &t});
    return links;
}

void EditorBridge::noteDisplacement(uint32_t index, const std::string& material)
{
    if (!hasProject_ || index >= records_.size() || records_[index].kind != EntityKind::Primitive || records_[index].slot == ~0u)
        return;
    const Record& r = records_[index];
    MaterialAsset asset;
    std::string error;
    const bool displaced = !material.empty() && readMaterialAsset(project_.path(material), asset, error) && asset.displaced();
    const std::string drawn = pak_->meshName(pak_->mutableInstances()[r.slot].mesh);
    if (drawn != (displaced ? displacedMeshName(r.primitive, material) : r.primitive))
        notice(displaced ? "Height displacement is cooked into the mesh: save and Reload the scene to see it"
                         : "Save and Reload the scene to draw this mesh without the height displacement");
}

void EditorBridge::notice(const char* message, const char* level)
{
    notices_.push_back(message);
    if (notices_.size() > 8)
        notices_.erase(notices_.begin());
    JsonWriter w;
    w.beginObject().value("type", "editor.notice").value("message", message).value("level", level).endObject();
    ui_->send(w.str());
}

void EditorBridge::sendState(bool entities)
{
    FX_ZONE_CAT("EditorBridge::sendState", Editor);
    if (uvPanel_)
        sendLevelUvs();
    const char* slash = std::max(std::strrchr(scenePath_.c_str(), '/'), std::strrchr(scenePath_.c_str(), '\\'));
    const std::string sceneName = slash ? slash + 1 : scenePath_;
    JsonWriter w;
    w.beginObject().value("type", "editor.state").value("project", hasProject_ ? project_.name : std::string("Falcon")).value("scene", sceneName);
    if (hasProject_) {
        w.beginObject("projectInfo")
            .value("name", project_.name)
            .value("dir", project_.dir)
            .value("startupScene", project_.startupScene)
            .value("current", sceneInProject_);
        w.beginArray("scenes");
        for (const std::string& s : project_.scenes)
            w.value(nullptr, s);
        w.endArray().endObject();
    }
    w.beginArray("scenes").beginObject().value("id", sceneName).value("name", sceneName).endObject().endArray();
    if (entities) {
        uint32_t count = 0, bodies = 0;
        // Prefab instances' entities that differ from their prefab (a dot in the hierarchy).
        std::unordered_set<uint32_t> overridden;
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (records_[i].prefabRoot == i && entityOf(i) && entityOf(i).enabled())
                if (const Prefab* def = prefabDef(records_[i].prefab))
                    for (const PrefabOverride& o : overridesOf(i, *def))
                        for (const uint32_t m : instanceMembers(i))
                            if (records_[m].prefabChild == o.id)
                                overridden.insert(m);
        std::unordered_set<uint32_t> expanded;  // characters whose bones are shown
        for (const Record& r : records_)
            if (r.kind == EntityKind::Bone && r.bone < 0 && r.entity && scene_->world().is_alive(r.entity) &&
                flecs::entity(scene_->world(), r.entity).enabled())
                expanded.insert(r.boneRoot);
        w.beginArray("entities");
        for (uint32_t i = 0; i < records_.size(); ++i) {
            const flecs::entity e = entityOf(i);
            if (!e || !e.enabled())
                continue;
            ++count;
            if (count > kMaxListedEntities)
                continue;
            const Record& r = records_[i];
            const uint32_t flags = r.slot != ~0u ? pak_->instances()[r.slot].flags : 0u;
            const uint32_t parent = parentOf(i);
            w.beginObject().value("id", i + 1).value("name", defaultName(i)).value("parent", parent == ~0u ? 0u : parent + 1)
                .value("kind", entityKindName(r.kind)).value("instance", (flags & fpak::kInstanceSkinned) != 0)
                .value("hidden", r.hidden).value("primitive", r.primitive);
            if (clippedOf(i))
                w.value("clipped", true);
            if (expanded.contains(i))
                w.value("bones", true);
            if (r.kind == EntityKind::Bone)
                w.value("bone", r.bone);  // (-1: the Skeleton group)
            if (r.prefabRoot != ~0u) {
                w.value("prefab", r.prefabRoot == i ? "root" : "child").value("prefabPath", records_[r.prefabRoot].prefab)
                    .value("overridden", overridden.contains(i));
            }
            w.endObject();
        }
        w.endArray();
        scene_->world().each([&](flecs::entity, const RigidBody&) { ++bodies; });
        w.value("entityCount", count + bodies);
    }
    w.value("selectedEntity", selected_ == ~0u ? 0u : selected_ + 1);
    w.beginArray("selection");
    for (const uint32_t i : selection_)
        w.value(nullptr, i + 1);
    w.endArray();
    if (selected_ != ~0u) {
        const State s = capture(selected_);
        const Record& rec = records_[selected_];
        // Children show (and edit) their transform relative to the parent, as in Unity.
        float shown[3][4], pos[3], rot[3], scale[3];
        std::memcpy(shown, s.world, sizeof(shown));
        if (s.parent != ~0u) {
            float inverse[3][4];
            affineInverse(capture(s.parent).world, inverse);
            affineMultiply(inverse, s.world, shown);
        }
        decompose(shown, pos, rot, scale);
        w.beginObject("inspector").value("id", selected_ + 1).value("name", s.name).value("scene", sceneName)
            .value("prefab", rec.prefabRoot != ~0u ? records_[rec.prefabRoot].prefab : std::string())
            .value("prefabRoot", rec.prefabRoot != ~0u ? rec.prefabRoot + 1 : 0u)
            .value("kind", entityKindName(rec.kind)).value("primitive", rec.primitive)
            .value("parent", s.parent == ~0u ? 0u : s.parent + 1);
        if (rec.kind == EntityKind::Bone)  // (driven by the animation: shown, not edited)
            w.value("bone", rec.bone).value("boneOf", defaultName(rec.boneRoot));
        w.beginObject("transform");
        writeVec3(w, "position", pos);
        writeVec3(w, "rotation", rot);
        writeVec3(w, "scale", scale);
        w.value("local", s.parent != ~0u);
        w.endObject();
        if (rec.slot != ~0u) {
            const fpak::InstanceGpu& row = pak_->instances()[rec.slot];
            char mesh[96];
            uint32_t meshCount = 0;
            const fpak::MeshGpu* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
            std::snprintf(mesh, sizeof(mesh), "Mesh %u · %u clusters · %u LOD levels", row.mesh,
                          row.mesh < meshCount ? meshes[row.mesh].clusterCount : 0, row.mesh < meshCount ? meshes[row.mesh].lodLevels : 0);
            w.beginObject("render").value("path", mesh).value("visible", !s.hidden).value("mesh", pak_->meshName(row.mesh))
                .value("material", rec.material).endObject();
        }
        if (s.light) {
            const Light& l = *s.light;
            w.beginObject("light").value("type", lightTypeName(l.type)).beginArray("color");
            for (int c = 0; c < 3; ++c)
                w.value(nullptr, double(l.color[c]));
            w.endArray().value("intensity", double(l.intensity)).value("range", double(l.range))
                .value("inner", double(l.innerAngle)).value("outer", double(l.outerAngle))
                .value("shadows", (l.flags & 1u) != 0).endObject();
        }
        if (s.camera)
            w.beginObject("camera").value("fov", double(s.camera->fov)).value("near", double(s.camera->nearClip))
                .value("main", (s.camera->flags & 1u) != 0).endObject();
        if (clippedOf(selected_))
            w.value("clipped", true);
        if (s.cut)
            w.beginObject("cut").value("shape", level::kCutShapeNames[uint32_t(s.cut->shape) % level::kCutShapes])
                .value("intersect", s.cut->intersect).value("cap", s.cut->capMaterial).value("seed", s.cut->seed)
                .value("roughness", double(s.cut->roughness)).endObject();
        if (s.wall)
            writeWall(w, "wall", *s.wall);
        if (s.roof)
            level::writeRoof(w, "roof", *s.roof);
        if (s.spline)
            level::writeSpline(w, "spline", s.spline->spline, s.spline->loft);
        if (s.snap)
            w.beginObject("snapPoint").value("tag", s.snap->tag).endObject();
        if (s.volume)
            w.beginObject("volume").value("kind", kVolumeKindNames[uint32_t(s.volume->kind) % kVolumeKinds]).value("tag", s.volume->tag)
                .value("cost", double(s.volume->cost)).endObject();
        if (s.levelMesh) {  // its layer materials (painted over the faces' own) and their blend
            w.beginObject("surface").beginArray("layers");
            for (const std::string& layer : s.levelMesh->layerMaterials)
                w.value(nullptr, layer);
            w.endArray().value("blend", double(s.levelMesh->layerBlend)).endObject();
        }
        if (s.modifiers || s.levelMesh) {  // (a level mesh: its stack, empty or not: Add Modifier there)
            w.beginArray("modifiers");
            for (const level::Modifier& mod : s.modifiers ? s.modifiers->stack : std::vector<level::Modifier>{})
                level::writeModifierJson(w, nullptr, mod);
            w.endArray();
            // The game's C# modifiers (their properties for the Inspector; listed again after a reload).
            static uint32_t listed = ~0u;
            static std::string list = "[]";
            if (const uint32_t g = level::scriptModifierGeneration(); g != listed) {
                listed = g;
                list = level::describeScriptModifiers();
            }
            w.raw("scriptModifiers", list);
        }
        if (s.wear) {
            w.beginArray("wear");
            for (const level::WearSettings& r : s.wear->rules)
                level::writeWearJson(w, nullptr, r);
            w.endArray();
        }
        if (s.opening)
            w.beginObject("opening").value("distance", double(s.opening->distance)).value("sill", double(s.opening->sill))
                .value("width", double(s.opening->width)).value("height", double(s.opening->height)).endObject();
        if (const int32_t body = s.water ? waterBody(selected_) : -1; body >= 0) {
            // The body, its material as resolved (with its overrides) and before them, and what
            // the editor needs to show every field.
            const WaterSystem::Body& b = *water_->body(uint32_t(body));
            writeWaterBodyJson(w, "water", b.desc);
            w.beginObject("waterMaterial");
            writeWaterMaterial(w, b.material);
            w.endObject().beginObject("waterBase");
            writeWaterMaterial(w, water_->baseMaterial(b.desc));
            w.endObject();
            writeWaterSchema(w, "waterSchema");
            w.beginArray("waterMaterials");
            for (const auto& [name, label] : waterPresets())
                w.beginObject().value("value", "builtin:" + name).value("label", label).endObject();
            for (const std::string& f : waterAssets())
                w.beginObject().value("value", f).value("label", std::filesystem::path(f).stem().string()).endObject();
            w.endArray();
        }
        if (s.body)
            w.beginObject("body").value("mass", double(s.body->mass)).value("linearDamping", double(s.body->linearDamping))
                .value("angularDamping", double(s.body->angularDamping)).value("gravityScale", double(s.body->gravityScale))
                .value("kinematic", (s.body->flags & 1u) != 0).endObject();
        if (s.collider) {
            w.beginObject("collider").value("shape", colliderShapeName(s.collider->shape));
            writeVec3(w, "center", s.collider->center);
            writeVec3(w, "size", s.collider->size);
            w.value("friction", double(s.collider->friction)).value("bounciness", double(s.collider->bounciness));
            w.value("layer", double(s.collider->layer)).value("trigger", (s.collider->flags & 1u) != 0);
            w.beginArray("ignore");
            for (uint32_t l = 0; l < 32; ++l)
                if (s.collider->ignore & (1u << l))
                    w.value(nullptr, double(l));
            w.endArray().endObject();
        }
        if (s.audioSource) {
            const AudioSource& a = *s.audioSource;
            const std::string& sound = soundPath(a.sound);
            const AudioSource* live = entityOf(selected_).try_get<AudioSource>();
            w.beginObject("audioSource").value("sound", sound).value("playOnStart", (a.flags & 1u) != 0)
                .value("loop", (a.flags & 2u) != 0).value("spatial", (a.flags & 4u) != 0).value("ownSettings", (a.flags & 8u) != 0)
                .value("airAbsorption", (a.flags & 16u) != 0).value("rolloff", (a.flags & 32u) ? "linear" : "inverse")
                .value("occlusion", (a.flags & 64u) != 0).value("transmission", (a.flags & 128u) != 0)
                .value("reflections", (a.flags & 256u) != 0).value("vary", (a.flags & 512u) == 0)
                .value("volume", double(a.volume)).value("pitch", double(a.pitch)).value("minDistance", double(a.minDistance))
                .value("maxDistance", double(a.maxDistance)).value("bus", kSoundBuses[std::min(a.bus, 4u)])
                .value("playing", live && live->voice && audio_ && audio_->playing(live->voice));
            // A .fsound's own settings: what plays unless the source's are used.
            if (SoundAsset def; audio_ && audio_->soundSettings(sound, def))
                w.beginObject("soundSettings").value("loop", def.loop).value("spatial", def.spatial)
                    .value("airAbsorption", def.airAbsorption).value("rolloff", def.rolloff ? "linear" : "inverse")
                    .value("occlusion", def.occlusion).value("transmission", def.transmission).value("reflections", def.reflections)
                    .value("minDistance", double(def.minDistance)).value("maxDistance", double(def.maxDistance))
                    .value("bus", kSoundBuses[std::min(def.bus, 4u)]).value("clips", uint32_t(def.clips.size())).endObject();
            w.endObject();
        }
        if (s.audioListener)
            w.beginObject("audioListener").value("gain", double(s.audioListener->gain)).endObject();
        // An Animator: its graph (and the project's graphs), the graph's parameters with their
        // defaults and this entity's values, how many skinned meshes it drives, and what's wrong.
        if (s.animator && animation_ && graphs_) {
            const AnimatorEdit& a = *s.animator;
            const std::vector<uint32_t> records = animation_->recordsUnder(scene_->world(), records_[selected_].entity);
            const auto def = a.graph.empty() ? nullptr : graphs_->get(a.graph);
            w.beginObject("animator").value("graph", a.graph).value("meshes", uint32_t(records.size()));
            // Entity parameters: the graph's, then other names bound here (its own IK's, or ones the
            // graph no longer has), each with the entity bound (an id; 0: none).
            std::vector<std::string> names = def ? def->entityParameters : std::vector<std::string>{};
            const size_t ofGraph = names.size();
            for (const auto& [name, key] : a.entities)
                if (std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
            const std::shared_ptr<const anim::IkSettings> ownIk = ownIkOf(a);
            const bool graphIk = def && def->ik.any();
            const auto usedBy = [](const anim::IkSettings& ik, const std::string& name) {
                std::vector<std::string> uses;
                if (ik.look.on && ik.look.target == name)
                    uses.push_back("look");
                if (ik.aim.on && ik.aim.target == name)
                    uses.push_back("aim");
                if (ik.aim.on && ik.aim.hold == name)
                    uses.push_back("hold");
                for (const anim::IkSettings::Reach& r : ik.reaches)
                    if (r.target == name)
                        uses.push_back("reach " + r.bone);
                return uses;
            };
            w.beginArray("entities");
            for (size_t n = 0; n < names.size(); ++n) {
                uint32_t id = 0;
                for (const auto& [name, key] : a.entities)
                    if (name == names[n])
                        if (const uint32_t i = indexOfKey(key); i != ~0u)
                            id = i + 1;
                std::string use;
                for (const std::string& u : graphIk ? usedBy(def->ik, names[n]) : ownIk ? usedBy(*ownIk, names[n]) : std::vector<std::string>{})
                    use += (use.empty() ? "" : ", ") + u;
                w.beginObject().value("name", names[n]).value("id", id).value("graph", n < ofGraph).value("use", use).endObject();
            }
            w.endArray();
            w.value("ownIk", ownIk != nullptr).value("graphIk", graphIk).value("editorPreview", a.editorPreview);
            std::string problem;
            if (records.empty())
                problem = "No skinned mesh in this entity or below it";
            else if (!a.graph.empty() && !def)
                problem = graphs_->errors(a.graph).empty() ? "The graph can't be read"
                                                           : "The graph has errors: " + graphs_->errors(a.graph)[0].text;
            else if (def && def->skeleton != skinning_->skeletonOfRecord(records[0]))
                problem = "The graph is for another skeleton";
            w.value("problem", problem);
            w.beginArray("graphs");
            for (const std::string& g : graphAssets())
                w.value(nullptr, g);
            w.endArray().beginArray("params");
            for (uint32_t i = 0; def && i < def->parameters.size(); ++i) {
                const anim::fmt::GraphNode& n = def->nodes[i];
                const bool isBool = n.type == anim::fmt::NodeType::ParamBool;
                if (n.type == anim::fmt::NodeType::ParamId)
                    continue;  // (IDs: set by scripts)
                const float defaultValue = isBool ? float(n.flags & 1u) : n.f[0];
                float value = defaultValue;
                bool own = false;
                for (const auto& [name, v] : a.params)
                    if (name == def->parameters[i])
                        value = v, own = true;
                w.beginObject().value("name", def->parameters[i]).value("type", isBool ? "bool" : "float")
                    .value("default", double(defaultValue)).value("value", double(value)).value("own", own).endObject();
            }
            w.endArray().endObject();
        }
        // A mesh's acoustics: its own (an AcousticSurface) or its material's, which show beside them.
        if (const MeshInstance* mi = entityOf(selected_).try_get<MeshInstance>()) {
            std::string preset;
            AcousticParams p;
            const std::string label = materialAcoustics(selected_, preset, p);
            w.beginObject("acoustics").value("mode", !s.acoustics ? "material" : (s.acoustics->flags & 1u) ? "excluded"
                                                        : (s.acoustics->flags & 2u)                         ? "own"
                                                                                                            : "material")
                .value("moving", s.acoustics && (s.acoustics->flags & 4u) != 0)
                .value("dynamic", entityOf(selected_).has<PhysicsBody>() || entityOf(selected_).has<RigidBody>());
            w.beginObject("material").value("label", label);
            writeAcousticFields(w, preset, p);
            w.endObject();
            if (s.acoustics && (s.acoustics->flags & 2u)) {
                AcousticParams own;
                std::memcpy(own.absorption, s.acoustics->absorption, sizeof(own.absorption));
                own.scattering = s.acoustics->scattering;
                std::memcpy(own.transmission, s.acoustics->transmission, sizeof(own.transmission));
                w.beginObject("own");
                writeAcousticFields(w, acousticPresetName(s.acoustics->preset), own);
                w.endObject();
            }
            writeAcousticPresets(w, "presets");
            // Surface sounds: its material's, or a preset of its own at a level and pitch.
            {
                std::string spreset;
                SurfaceSound ss;
                const std::string slabel = materialSurface(selected_, spreset, ss);
                const bool own = s.acoustics && (s.acoustics->flags & 8u);
                w.value("surface", own ? surfacePresetName(s.acoustics->surface) : "").value("surfaceMaterial", slabel)
                    .value("impactGain", own ? double(s.acoustics->impactGain) : 1.0).value("impactPitch", own ? double(s.acoustics->impactPitch) : 1.0);
                w.beginArray("surfacePresets");
                for (const SurfacePreset& p : surfacePresets())
                    w.beginObject().value("value", p.name).value("label", p.label).endObject();
                w.endArray();
            }
            w.endObject();
            (void)mi;
        }
        // Add Component's list, and the components Remove Component can take off.
        w.beginArray("addable");
        uint32_t vfxCount = 0;
        const VfxComponentInfo* vfxInfos = vfxComponents(vfxCount);
        for (const char* c : {"Light", "CameraComponent", "PhysicsBody", "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "AudioSource",
                              "AudioListener", "Animator"})
            if (canAddComponent(selected_, c))
                w.value(nullptr, c);
        for (uint32_t c = 0; c < vfxCount; ++c)
            if (canAddComponent(selected_, vfxInfos[c].name))
                w.value(nullptr, vfxInfos[c].name);
        for (const Json& t : scriptTypes_.array)
            if (const std::string c = "Script:" + t.getString("name"); canAddComponent(selected_, c))
                w.value(nullptr, c);
        for (const Json& t : dataTypes_.array)
            if (const std::string c = "Data:" + t.getString("name"); canAddComponent(selected_, c))
                w.value(nullptr, c);
        w.endArray().beginArray("removable");
        for (const char* c : {"Light", "CameraComponent", "PhysicsBody", "Collider", "AudioSource", "AudioListener", "Animator"})
            if (canRemoveComponent(selected_, c))
                w.value(nullptr, c);
        for (uint32_t c = 0; c < vfxCount; ++c)
            if (canRemoveComponent(selected_, vfxInfos[c].name))
                w.value(nullptr, vfxInfos[c].name);
        for (const ScriptEntry& e : records_[selected_].scripts)
            w.value(nullptr, "Script:" + e.className);
        if (editable(selected_))
            for (const DataComponent& d : s.data)
                w.value(nullptr, "Data:" + d.name);
        w.endArray();
        writeVfxInspector(w, selected_, s);
        // Script components: each with its class's description (missing: renamed or gone; its
        // values kept), and while playing what the running ones hold.
        w.raw("scriptTypes", scriptTypesJson_).beginArray("scripts");
        for (const ScriptEntry& e : records_[selected_].scripts)
            w.beginObject().value("class", e.className).value("enabled", e.enabled).value("known", scriptType(e.className) != nullptr)
                .raw("properties", scriptPropsForUi(e)).endObject();
        w.endArray();
        // (X2) Its data components: each field's values as its type describes it (unknown: the
        // game has no such struct now; its bytes kept).
        w.raw("dataTypes", dataTypesJson_).beginArray("dataComponents");
        for (const DataComponent& d : s.data) {
            const Json* t = dataType(d.name);
            w.beginObject().value("name", d.name).value("known", t != nullptr).beginArray("fields");
            if (const Json* fields = t ? t->get("fields") : nullptr)
                for (const Json& f : fields->array) {
                    const std::string kind = f.getString("kind");
                    const uint32_t offset = uint32_t(f.getNumber("offset"));
                    uint32_t lanes, bytes;
                    dataKind(kind, lanes, bytes);
                    w.beginObject().value("name", f.getString("name")).value("kind", kind).beginArray("values");
                    for (uint32_t k = 0; bytes && k < lanes && offset + (k + 1) * bytes <= d.bytes.size(); ++k)
                        w.value(nullptr, readDataLane(kind, d.bytes.data() + offset + k * bytes));
                    w.endArray().endObject();
                }
            w.endArray().endObject();
        }
        w.endArray();
        if (playing_ && scriptValues_ && !records_[selected_].scripts.empty())
            if (const flecs::entity se = entityOf(selected_); se && se.is_alive())
                if (const std::string live = scriptValues_(se.id()); !live.empty())
                    w.raw("scriptLive", live);
        // Every flecs component on the entity, as the inspector shows it: schema components with
        // their fields (components.fschema), others (script-defined) with their raw bytes.
        w.beginArray("components");
        const flecs::entity e = entityOf(selected_);
        e.each([&](flecs::id id) {
            if (id.is_pair() || !id.is_entity())
                return;
            const flecs::entity c = id.entity();
            const ecs_type_info_t* ti = ecs_get_type_info(scene_->world(), id);
            // (tags, Disabled among them, have no data: ecs_get_id is for components only)
            const auto* data = ti ? static_cast<const uint8_t*>(ecs_get_id(scene_->world(), e, id)) : nullptr;
            const std::string name = c.name().c_str();
            w.beginObject().value("name", name).value("id", uint64_t(id.raw_id()) & 0xffffffffull)
                .value("size", ti ? uint32_t(ti->size) : 0u);
            const ComponentInfo* info = nullptr;
            for (const ComponentInfo& ci : kComponentInfos)
                if (name == ci.name)
                    info = &ci;
            if (info) {
                w.value("doc", info->doc).beginArray("fields");
                for (uint32_t f = 0; f < info->fieldCount && data; ++f) {
                    const ComponentFieldInfo& fi = info->fields[f];
                    const bool isFloat = fi.type[0] == 'f';
                    const uint32_t lanes = std::strcmp(fi.type, "f32x3") == 0 ? 3 : std::strcmp(fi.type, "f32x4") == 0 ? 4 : 1;
                    const uint32_t stride = std::strcmp(fi.type, "u64") == 0 ? 8 : 4;
                    w.beginObject().value("name", fi.name).value("type", fi.type).beginArray("values");
                    for (uint32_t k = 0; k < fi.count * lanes; ++k) {
                        const uint8_t* at = data + fi.offset + k * stride;
                        if (isFloat) {
                            float v;
                            std::memcpy(&v, at, 4);
                            w.value(nullptr, double(v));
                        } else if (stride == 8) {
                            uint64_t v;
                            std::memcpy(&v, at, 8);
                            w.value(nullptr, v);
                        } else if (fi.type[0] == 'i') {
                            int32_t v;
                            std::memcpy(&v, at, 4);
                            w.value(nullptr, v);
                        } else {
                            uint32_t v;
                            std::memcpy(&v, at, 4);
                            w.value(nullptr, v);
                        }
                    }
                    w.endArray().endObject();
                }
                w.endArray();
            } else if (data && ti && ti->size > 0) {
                char hex[3 * 32 + 4] = "";
                const uint32_t n = std::min<uint32_t>(uint32_t(ti->size), 32);
                for (uint32_t i = 0; i < n; ++i)
                    std::snprintf(hex + i * 3, 4, "%02x ", data[i]);
                if (n < uint32_t(ti->size))
                    std::strcat(hex, "…");
                w.value("bytes", hex);
            }
            w.endObject();
        });
        w.endArray();
        w.endObject();
    } else {
        w.null("inspector");
    }
    w.beginObject("scripts")
        .value("state", scriptsState())
        .value("error", scriptsError_)
        .value("running", game_.running)
        .value("playPending", pendingPlay_)
        .endObject();
    w.value("playing", playing_).value("paused", paused_).value("canUndo", !undo_.empty()).value("canRedo", !redo_.empty())
        .value("dirty", dirty_);
    writeTools(w);
    writeSettings(w);
    if (terrain_)
        terrain_->write(w);
    w.endObject();
    ui_->send(w.str());
}

bool EditorBridge::save()
{
    const SceneEdits edits = currentEdits();
    if (!edits.save(editsPath_))
        return false;
    dirty_ = false;
    FX_INFO("editor: saved %zu instance edits, %zu light edits and %zu created entities to %s", edits.instances.size(),
            edits.lights.size(), edits.created.size(), editsPath_.c_str());
    return true;
}

SceneEdits EditorBridge::currentEdits()
{
    SceneEdits edits;
    edits.nextUid = nextUid_;
    uint32_t cookedCount = 0;
    const fpak::InstanceGpu* cooked = pak_->view<fpak::InstanceGpu>(fpak::Section::Instances, &cookedCount);
    // Children store their local transform exactly as LocalTransform holds it (a world matrix
    // would come back with rounding differences through parent x local).
    const auto fill = [&](uint32_t i, const State& s, EntityEdit& e) {
        const uint32_t parent = s.parent != ~0u && capture(s.parent).alive ? s.parent : ~0u;
        const LocalTransform* local = parent != ~0u ? entityOf(i).try_get<LocalTransform>() : nullptr;
        e.parent = local ? keyOf(parent) : "";
        std::memcpy(e.world, local ? local->rows : &s.world[0][0], sizeof(e.world));
        e.hidden = s.hidden;
        e.material = s.material;
        e.hasBody = s.body.has_value();  // (added in the Inspector, on any kind)
        if (s.body)
            e.body = *s.body;
        e.hasCollider = s.collider.has_value();
        if (s.collider)
            e.collider = *s.collider;
        e.hasAudioSource = s.audioSource.has_value();
        if (s.audioSource)
            e.audioSource = *s.audioSource;
        e.hasAudioListener = s.audioListener.has_value();
        if (s.audioListener)
            e.audioListener = *s.audioListener;
        e.hasAcoustics = s.acoustics.has_value();
        if (s.acoustics)
            e.acoustics = *s.acoustics;
        e.hasAnimator = s.animator.has_value();
        if (s.animator)
            e.animator = *s.animator;
        e.vfx = s.vfx;
        e.scripts = s.scripts;
        e.data = s.data;
        e.bones = bonesExpanded(i);
    };
    for (uint32_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        if (!entityOf(i) || r.kind == EntityKind::Bone)  // (bones come back from their root's "bones")
            continue;
        const State s = capture(i);
        if (r.uid == 0 && r.light != ~0u) {  // cooked light: saved when it differs from the pak
            Light base;
            float world[3][4];
            if (!scene_->cookedLight(r.light, base, world))
                continue;
            const Light now = s.light.value_or(base);
            const bool renamed = !r.name.empty() && r.name != defaultCookedLightName(r.light);
            if (s.alive && !s.hidden && !renamed && s.parent == ~0u && std::memcmp(s.world, world, sizeof(world)) == 0 &&
                std::memcmp(&now, &base, sizeof(Light)) == 0 && !s.camera && !s.body && !s.collider && !s.audioSource && !s.audioListener &&
                !s.acoustics && !s.animator && s.vfx.empty() && s.scripts.empty() && s.data.empty())
                continue;
            EntityEdit e;
            e.slot = r.light;
            e.name = renamed ? r.name : "";
            e.deleted = !s.alive;
            e.hasLight = true;
            e.light = now;
            if (s.camera) {  // (added in the Inspector)
                e.hasCamera = true;
                e.camera = *s.camera;
            }
            fill(i, s, e);
            edits.lights.push_back(std::move(e));
        } else if (r.uid == 0) {  // cooked instance
            if (r.slot >= cookedCount)
                continue;
            const char* cookedName = pak_->instanceName(r.slot);
            const bool renamed = !r.name.empty() && r.name != (cookedName ? cookedName : "");
            if (s.alive && !s.hidden && !renamed && s.parent == ~0u && s.material.empty() && !s.light && !s.camera && !s.body &&
                !s.collider && !s.audioSource && !s.audioListener && !s.acoustics && !s.animator && !bonesExpanded(i) && s.vfx.empty() && s.scripts.empty() && s.data.empty() &&
                std::memcmp(s.world, cooked[r.slot].world, sizeof(s.world)) == 0)
                continue;
            EntityEdit e;
            e.slot = r.slot;
            e.name = renamed ? r.name : "";
            e.deleted = !s.alive;
            if (s.light) {  // components added in the Inspector
                e.hasLight = true;
                e.light = *s.light;
            }
            if (s.camera) {
                e.hasCamera = true;
                e.camera = *s.camera;
            }
            fill(i, s, e);
            edits.instances.push_back(std::move(e));
        } else if (s.alive && linkedChild(i)) {
            continue;  // written as its instance's overrides
        } else if (s.alive && r.prefabRoot == i) {
            EntityEdit e;  // a prefab instance: the root's transform, and what it changes
            e.uid = r.uid;
            e.kind = "prefab";
            e.prefab = r.prefab;
            e.name = s.name;
            if (const Prefab* def = prefabDef(r.prefab))
                e.overrides = overridesOf(i, *def);
            fill(i, s, e);
            e.material.clear();  // (an override of entity "0", as its body and collider are)
            e.hasBody = e.hasCollider = e.hasAudioSource = e.hasAudioListener = e.hasAcoustics = e.hasAnimator = false;
            e.vfx = {};
            e.scripts.clear();
            e.data.clear();  // (not in prefabs yet)
            edits.created.push_back(std::move(e));
        } else if (s.alive) {
            EntityEdit e;
            e.uid = r.uid;
            e.kind = entityKindName(r.kind == EntityKind::Mesh ? EntityKind::Primitive : r.kind);
            e.primitive = r.kind == EntityKind::Primitive ? r.primitive : "";
            e.name = s.name;
            e.model = r.model;
            e.modelNode = r.modelNode;
            e.modelCamera = r.modelCamera;
            if (s.light) {
                e.hasLight = true;
                e.light = *s.light;
            }
            if (s.camera) {
                e.hasCamera = true;
                e.camera = *s.camera;
            }
            if (s.water) {
                e.hasWater = true;
                e.water = *s.water;
            }
            e.levelMesh = s.levelMesh;
            e.levelMask = s.levelMask;
            if (s.wear)
                e.levelWear = s.wear->rules;
            if (s.modifiers)
                e.modifiers = s.modifiers->stack;
            e.spline = s.spline;
            e.scatterSet = s.scatter;
            e.snapPoint = s.snap;
            e.volume = s.volume;
            e.cut = s.cut;
            e.wall = s.wall;
            e.opening = s.opening;
            e.roof = s.roof;
            fill(i, s, e);
            edits.created.push_back(std::move(e));
        }
    }
    return edits;
}

void EditorBridge::handle(const Json& m, const CameraState& camera)
{
    const std::string type = m.getString("type");
    if (type.starts_with("editor.anim.") && animEditor_.handle(type, m, *ui_)) {
        if (animEditor_.takeCreated())
            assets_.rescanNow();
        return;
    }
    if (type == "net.subscribe") {
        netSubscribed_ = m.getBool("on", false);
        netSentNs_ = 0;
        return;
    }
    if (type.starts_with("profiler.") && profiler_.handle(type, m, *ui_))
        return;
    // Test scripts may name entities instead of giving ids ("id": "Cube 1"): created entities'
    // ids depend on what was created before.
    const auto idByName = [&](const std::string& name) {
        if (name.find('/') != std::string::npos)
            return idByPath(name);
        uint32_t found = 0;  // a live entity of that name wins over a deleted one
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (entityOf(i) && defaultName(i) == name && (found == 0 || capture(i).alive))
                found = i + 1;
        return found;
    };
    const auto idOf = [&](const char* key) -> uint32_t {
        const Json* v = m.get(key);
        return v && v->type == Json::Type::String ? idByName(v->str) : uint32_t(m.getNumber(key));
    };
    const uint32_t id = idOf("id");
    // Commands on "the" entity (duplicate, delete, frame) default to the selection.
    const bool onSelection = !m.get("id") && (type == "editor.entity.duplicate" || type == "editor.entity.delete" || type == "editor.frame");
    const uint32_t index = onSelection ? selected_ : id == 0 ? ~0u : id - 1;
    // Several selected: delete or duplicate each of them (without what is selected above it), one
    // undo step; the copies end up selected.
    if (onSelection && type != "editor.frame" && selection_.size() > 1) {
        const std::vector<uint32_t> roots = selectionRoots();
        std::vector<uint32_t> copies;
        undoBegin();
        for (const uint32_t r : roots) {
            Json one = Json::parse(std::string("{\"type\":\"") + type + "\",\"id\":" + std::to_string(r + 1) + "}");
            handle(one, camera);
            if (type == "editor.entity.duplicate" && selected_ != ~0u && selected_ != r)
                copies.push_back(selected_);
        }
        undoEnd();
        selectMany(copies, SelectMode::Replace);
        return sendState(true);
    }
    if (type == "editor.ready" || type == "editor.scene.reload") {
        if (type == "editor.ready" && prefs_) {  // the page's remembered layout, before the state
            ui_->send(R"({"type":"editor.prefs","page":)" + prefs_->section("page").dump(0) + "}");
        }
        // The code's commands and windows again: code loaded at startup (a build that's up to date)
        // describes them before the page is listening, and that message is lost.
        if (type == "editor.ready" && toolsJson_ != "{}")
            ui_->send("{\"type\":\"editor.tools\",\"tools\":" + toolsJson_ + "}");
        if (type == "editor.ready" && !toolState_.empty())
            ui_->send("{\"type\":\"editor.tools.state\",\"state\":" + toolState_ + "}");
        if (type == "editor.ready")
            ++pageReadies_;
        if (type == "editor.ready" && connected_) {  // the page reloaded (live reload: its files changed)
            JsonWriter w;
            w.beginObject().value("type", "engine.connected").value("backend", backend_).endObject();
            ui_->send(w.str());
            if (!gameUiMessage_.empty())
                ui_->send(gameUiMessage_);
        }
        if (!connected_) {
            boot::mark("editor page ready");
            JsonWriter w;
            w.beginObject().value("type", "engine.connected").value("backend", backend_).endObject();
            ui_->send(w.str());
            connected_ = true;
            bakeSent_.clear();  // (the bake's status again)
            connectedNs_ = SDL_GetTicksNS();
            sendState(true);
            boot::mark("editor state sent");
        } else {
            sendState(true);
        }
    } else if (type == "editor.select") {
        // The page's Esc (Select None) while a modal operation holds the view or has just ended (Esc
        // ends it, and the page hears the same key): the operation's, not a deselect.
        if (m.getBool("escape") && ((view_ && view_->captured()) || SDL_GetTicksNS() - captureEndedNs_ < 500000000ull))
            return;
        // "mode": "add" (Shift) / "toggle" (Ctrl) / replace; "ids": several at once (a box).
        const std::string mode = m.getString("mode");
        const SelectMode sm = mode == "add" ? SelectMode::Add : mode == "toggle" ? SelectMode::Toggle : SelectMode::Replace;
        if (const Json* ids = m.get("ids"); ids && ids->type == Json::Type::Array) {
            std::vector<uint32_t> list;
            for (const Json& v : ids->array)
                if (const uint32_t n = v.type == Json::Type::String ? idByName(v.str) : uint32_t(v.number); n)
                    list.push_back(n - 1);
            selectMany(list, sm);
        } else {
            selectMode(index, sm);
        }
        sendState(false);
    } else if (type == "editor.transform.set" || type == "editor.transform.preview") {
        float pos[3], rot[3], scale[3];
        if (!editable(index) || !readVec3(m, "position", pos) || !readVec3(m, "rotation", rot) || !readVec3(m, "scale", scale))
            return notice("Transform: not an editable entity or invalid values", "error");
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        compose(pos, rot, scale, s.world);
        s.hasLocal = false;
        if (s.parent != ~0u) {  // the page edits children relative to their parent
            const State parent = capture(s.parent);
            affineMultiply(parent.world, s.world, s.world);
        }
        if (s.opening)
            frameOpening(s, true);  // (an opening slides along its wall)
        if (type == "editor.transform.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            growLiveRegion(index);
            apply(index, s);
            growLiveRegion(index);
            return;
        }
        // One undo step from before the previews (if any) to the final value.
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        execute({{index, s}});
        sendState(false);
    } else if (type == "editor.render.set") {
        if (!editable(index))
            return;
        State s = capture(index);
        s.hidden = !m.getBool("visible", true);
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.entity.parent" && index < records_.size() && linkedChild(index)) {
        return notice("This entity is part of a prefab instance: Unpack the prefab first", "error");
    } else if (type == "editor.entity.delete") {
        if (!editable(index))
            return;
        // With everything below it (one undo step brings the whole subtree back).
        std::vector<std::pair<uint32_t, State>> after;
        for (const uint32_t i : subtree(index)) {
            State s = capture(i);
            if (!s.alive)
                continue;
            s.alive = false;
            after.emplace_back(i, std::move(s));
            if (i == selected_)
                select(~0u);
        }
        // Animators elsewhere let go of what goes: their IK target slots on it cleared in the same
        // step (Undo brings them back with it).
        std::unordered_set<std::string> gone;
        for (const auto& [i, s] : after)
            gone.insert(keyOf(i));
        const size_t deleted = after.size();
        for (uint32_t i = 0; i < records_.size(); ++i) {
            if (!records_[i].animator || records_[i].animator->entities.empty() ||
                std::any_of(after.begin(), after.begin() + ptrdiff_t(deleted), [&](const auto& d) { return d.first == i; }))
                continue;
            State s = capture(i);
            if (!s.alive || !s.animator)
                continue;
            bool changed = false;
            AnimatorEdit& a = *s.animator;
            changed = std::erase_if(a.entities, [&](const auto& e) { return gone.contains(e.second); }) > 0;
            if (changed)
                after.emplace_back(i, std::move(s));
        }
        execute(after);
        sendState(true);
    } else if (type == "editor.entity.rename") {
        const std::string name = m.getString("name");
        if (!editable(index) || name.empty())
            return;
        State s = capture(index);
        s.name = name;
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.vfx.set" || type == "editor.vfx.preview") {
        // A particle or decal component's fields: {id, component, fields: {key: value, ...}} (keys and
        // values as in the edits). Set: one undo step; preview: shown at once, undone into the next set.
        const int32_t c = vfxComponentIndex(m.getString("component"));
        if (!editable(index) || c < 0 || !vfxGet(entityOf(index), uint32_t(c)))
            return notice("Particles: the entity has no such component", "error");
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        uint32_t count = 0;
        const VfxComponentInfo& info = vfxComponents(count)[c];
        void* data = s.vfx.find(uint32_t(c));
        if (const Json* fields = m.get("fields"); fields && fields->isObject())
            for (const auto& [key, value] : fields->object) {
                const VfxField* field = nullptr;
                for (uint32_t i = 0; i < info.fieldCount && !field; ++i)
                    if (key == info.fields[i].key)
                        field = &info.fields[i];
                if (!field || !readVfxField(*field, data, value))
                    return notice((std::string(info.label) + ": can't set " + key).c_str(), "error");
            }
        if (type == "editor.vfx.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            apply(index, s);
            return;
        }
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.data.set") {
        // (X2) A data component's field: {id, component, field, value: a number, bool or [numbers]};
        // one undo step.
        if (!editable(index))
            return;
        State s = capture(index);
        const std::string name = m.getString("component"), field = m.getString("field");
        const auto it = std::find_if(s.data.begin(), s.data.end(), [&](const DataComponent& d) { return d.name == name; });
        const Json* t = dataType(name);
        const Json* fields = t ? t->get("fields") : nullptr;
        const Json* value = m.get("value");
        if (it == s.data.end() || !fields || !value)
            return notice(("Data: the entity has no " + name).c_str(), "error");
        for (const Json& f : fields->array) {
            if (f.getString("name") != field)
                continue;
            const std::string kind = f.getString("kind");
            const uint32_t offset = uint32_t(f.getNumber("offset"));
            uint32_t lanes, bytes;
            dataKind(kind, lanes, bytes);
            if (!bytes || kind == "entity" || offset + lanes * bytes > it->bytes.size())
                return notice(("Data: " + name + "." + field + " can't be set in the editor").c_str(), "error");
            for (uint32_t k = 0; k < lanes; ++k) {
                const Json* v = value->type == Json::Type::Array ? (k < value->array.size() ? &value->array[k] : nullptr) : value;
                if (v)
                    writeDataLane(kind, it->bytes.data() + offset + k * bytes, v->type == Json::Type::Bool ? (v->b ? 1.0 : 0.0) : v->number);
            }
            execute({{index, s}});
            sendState(true);
            return;
        }
        return notice(("Data: " + name + " has no field " + field).c_str(), "error");
    } else if (type == "editor.script.button") {
        // (X3) A component's [Button] method: {id, class, method}; the engine runs it (takeButtonRuns).
        const flecs::entity e = editable(index) ? entityOf(index) : flecs::entity();
        if (!e)
            return notice("Button: not an editable entity", "error");
        const std::string name = m.getString("class");
        const auto it = std::find_if(records_[index].scripts.begin(), records_[index].scripts.end(),
                                     [&](const ScriptEntry& s) { return s.className == name; });
        if (it == records_[index].scripts.end())
            return notice(("Button: the entity has no " + name).c_str(), "error");
        buttonRuns_.push_back({index, e.id(), name, m.getString("method"), it->properties});
    } else if (type == "editor.script.set" || type == "editor.script.preview") {
        // A script component's properties and checkbox: {id, class, values?: {name: value}, enabled?}.
        // Entity properties come as entity ids (0: none) and are kept as the edits' keys. Set: one
        // undo step; preview: shown at once, undone into the next set.
        if (!editable(index))
            return;
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        const std::string name = m.getString("class");
        const auto it = std::find_if(s.scripts.begin(), s.scripts.end(), [&](const ScriptEntry& e) { return e.className == name; });
        if (it == s.scripts.end())
            return notice(("Script: the entity has no " + name).c_str(), "error");
        if (const Json* v = m.get("enabled"))
            it->enabled = v->b;
        if (const Json* values = m.get("values"); values && values->isObject()) {
            Json props;
            try {
                props = Json::parse(it->properties);
            } catch (const std::exception&) {
                props = Json::makeObject();
            }
            if (!props.isObject())
                props = Json::makeObject();
            const Json* typeDesc = scriptType(name);
            for (const auto& [key, value] : values->object) {
                bool entity = false;
                if (const Json* list = typeDesc ? typeDesc->get("properties") : nullptr)
                    for (const Json& p : list->array)
                        entity |= p.getString("name") == key && p.getString("kind") == "entity";
                if (entity) {  // (the referenced entity's key: it survives saving and reloading)
                    const uint32_t target = value.type == Json::Type::Number ? uint32_t(value.number)
                                            : value.type == Json::Type::String ? idByName(value.str)  // (by name: tests)
                                                                               : 0u;
                    props.set(key, Json::makeString(target && target <= records_.size() ? keyOf(target - 1) : ""));
                } else {
                    props.set(key, value);
                }
            }
            it->properties = props.dump(0);
        }
        if (type == "editor.script.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            apply(index, s);
            return;
        }
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.vfx.restart" || type == "editor.vfx.pause") {
        // The effect's preview: from its start, or held (pause again: goes on).
        if (!particles_ || !editable(index))
            return;
        if (type == "editor.vfx.restart")
            particles_->restart(entityOf(index));
        else if (ParticleSystem::Effect* fx = particles_->effectOf(entityOf(index)))
            fx->playing ? (void)particles_->stop(entityOf(index), false) : (void)particles_->play(entityOf(index));
        sendState(false);
    } else if (type == "editor.light.set" || type == "editor.light.preview") {
        if (!editable(index) || !capture(index).light)
            return notice("Light: not a light", "error");
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        Light& l = *s.light;
        l.type = lightTypeOf(m.getString("lightType", lightTypeName(l.type)));  // ("type" is the message's)
        if (float c[3]; readVec3(m, "color", c))
            for (int k = 0; k < 3; ++k)
                l.color[k] = std::max(c[k], 0.0f);
        l.intensity = std::max(float(m.getNumber("intensity", l.intensity)), 0.0f);
        l.range = std::max(float(m.getNumber("range", l.range)), 0.0f);
        l.outerAngle = std::clamp(float(m.getNumber("outer", l.outerAngle)), 1.0f, 89.0f);
        l.innerAngle = std::clamp(float(m.getNumber("inner", l.innerAngle)), 0.0f, l.outerAngle);
        if (const Json* v = m.get("shadows"))
            l.flags = (l.flags & ~1u) | (v->b ? 1u : 0u);
        if (type == "editor.light.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            apply(index, s);
            return;
        }
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.water.set" || type == "editor.water.preview" || type == "editor.water.saveAsset" ||
               type == "editor.water.applyAsset") {
        const int32_t body = waterBody(index);
        if (!editable(index) || body < 0)
            return notice("Water: not a water body", "error");
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        WaterBodyDesc& d = *s.water;
        readWaterBodyJson(m, d);  // (any of shape, width, length, depth, points, material, overrides, buoyancy)
        if (d.shape == WaterShape::River && d.points.size() < 4) {  // a river: a gentle S to start from
            d.points = {-d.width * 2.0f, 0.0f, -d.width * 0.7f, d.width * 0.8f, d.width * 0.7f, -d.width * 0.8f, d.width * 2.0f, 0.0f};
            d.heights.clear();
        }
        if (d.shape == WaterShape::Polygon && d.points.size() < 6) {
            d.points.clear();
            for (int k = 0; k < 12; ++k) {
                const float a = float(k) / 12.0f * 2.0f * kPi, r = 0.5f * (0.82f + 0.18f * std::sin(a * 3.0f + 1.0f));
                d.points.push_back(std::cos(a) * r * d.width);
                d.points.push_back(std::sin(a) * r * d.length);
            }
        }
        // One field of the material as this body's override ("value" missing: back to the material's).
        if (const std::string field = m.getString("field"); !field.empty()) {
            Json o = Json::parse(d.overrides.empty() ? "{}" : d.overrides);
            Json kept = Json::makeObject();
            for (auto& [key, value] : o.object)
                if (key != field)
                    kept.set(key, value);
            if (const Json* v = m.get("value"))
                kept.set(field, *v);
            d.overrides = kept.dump(0);
        }
        if (m.getBool("resetAll"))
            d.overrides = "{}";
        if (type == "editor.water.saveAsset" || type == "editor.water.applyAsset") {
            // The material as this body shows it into a .fwater (a new one, or the body's own),
            // which the body then uses without overrides.
            const std::string path = type == "editor.water.saveAsset" ? m.getString("path") : d.material;
            if (path.empty() || path.starts_with("builtin:") || project_.dir.empty())
                return notice("Water: no material file to write (save it as an asset first)", "error");
            WaterMaterial mat = water_->body(uint32_t(body))->material;
            if (type == "editor.water.saveAsset")
                mat.name = std::filesystem::path(path).stem().string();
            std::string error;
            const std::string full = (std::filesystem::path(project_.dir) / path).generic_string();
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(full).parent_path(), ec);
            if (!writeWaterAsset(full, mat, error))
                return notice(error.c_str(), "error");
            d.material = path;
            d.overrides = "{}";
            water_->reloadMaterial(path);
            waterAssetsNs_ = 0;
            notice(("Water material saved: " + path).c_str(), "info");
        }
        if (type == "editor.water.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            apply(index, s);
            return;
        }
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.camera.set" || type == "editor.camera.preview") {
        if (!editable(index) || !capture(index).camera)
            return notice("Camera: not a camera", "error");
        if (previewBase_ && previewIndex_ != index)
            previewBase_.reset();
        State s = capture(index);
        CameraComponent& c = *s.camera;
        c.fov = std::clamp(float(m.getNumber("fov", c.fov)), 1.0f, 179.0f);
        c.nearClip = std::clamp(float(m.getNumber("near", c.nearClip)), 0.001f, 100.0f);
        if (const Json* v = m.get("main"))
            c.flags = (c.flags & ~1u) | (v->b ? 1u : 0u);
        if (type == "editor.camera.preview") {
            if (!previewBase_) {
                previewBase_ = capture(index);
                previewIndex_ = index;
            }
            apply(index, s);
            return;
        }
        if (previewBase_) {
            apply(index, *previewBase_);
            previewBase_.reset();
        }
        // One main camera: making this one main clears the others (in the same undo step).
        std::vector<std::pair<uint32_t, State>> after = {{index, s}};
        if (c.flags & 1u)
            for (uint32_t i = 0; i < records_.size(); ++i)
                if (State o = capture(i); i != index && o.camera && (o.camera->flags & 1u)) {
                    o.camera->flags &= ~1u;
                    after.emplace_back(i, std::move(o));
                }
        execute(after);
        sendState(true);
    } else if (type == "editor.component.add" || type == "editor.component.remove") {
        // The Inspector's Add Component / Remove Component: one undo step, saved with the entity.
        const bool add = type == "editor.component.add";
        std::string component = m.getString("component");
        if (component == "Camera")
            component = "CameraComponent";
        else if (component == "Rigidbody")
            component = "PhysicsBody";
        else if (component == "Audio Source")
            component = "AudioSource";
        else if (component == "Audio Listener")
            component = "AudioListener";
        if (add ? !canAddComponent(index, component) : !canRemoveComponent(index, component))
            return notice((std::string(add ? "Add Component: " : "Remove Component: ") + component +
                           (add ? " can't be added to this entity" : " can't be removed from this entity")).c_str(), "error");
        State s = capture(index);
        if (component.starts_with("Script:")) {
            const std::string name = component.substr(7);
            std::erase_if(s.scripts, [&](const ScriptEntry& e) { return e.className == name; });
            if (add)
                s.scripts.push_back({name, "{}", true});
        } else if (component.starts_with("Data:")) {  // (X2) with its struct's defaults (new T())
            const std::string name = component.substr(5);
            std::erase_if(s.data, [&](const DataComponent& d) { return d.name == name; });
            if (const Json* t = add ? dataType(name) : nullptr) {
                DataComponent d{name, uint32_t(t->getNumber("size")), uint32_t(t->getNumber("align")), {}};
                const std::string hex = t->getString("defaults");
                for (size_t i = 0; i + 1 < hex.size() && d.bytes.size() < d.size; i += 2)
                    d.bytes.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
                d.bytes.resize(d.size);
                s.data.push_back(std::move(d));
            }
        } else if (component == "Animator") {
            s.animator.reset();
            if (add) {  // the project's first graph for its skeleton
                s.animator = AnimatorEdit{};
                const std::vector<uint32_t> records = animation_->recordsUnder(scene_->world(), records_[index].entity);
                for (const std::string& g : graphAssets())
                    if (const auto def = graphs_->get(g); def && !records.empty() && def->skeleton == skinning_->skeletonOfRecord(records[0])) {
                        s.animator->graph = g;
                        break;
                    }
            }
        } else if (component == "AudioSource") {
            s.audioSource.reset();
            if (add)
                s.audioSource = defaultAudioSource();
        } else if (component == "AudioListener") {
            s.audioListener.reset();
            if (add)
                s.audioListener = AudioListener{1.0f, 0u};
        } else if (component == "PhysicsBody") {
            s.body.reset();
            if (add)
                s.body = PhysicsBody{1.0f, 0.0f, 0.05f, 1.0f, 0u, {0u, 0u}};  // Unity's: 1 kg, falls
        } else if (component.ends_with("Collider")) {
            s.collider.reset();
            if (add)
                s.collider = defaultCollider(index, component == "MeshCollider"      ? 2u
                                                    : component == "SphereCollider"  ? 1u
                                                    : component == "CapsuleCollider" ? 3u
                                                                                     : 0u);
        } else if (component == "Light") {
            s.light.reset();
            if (add)
                s.light = defaultLight(lightTypeOf(m.getString("light", "point")));
        } else if (const int32_t c = vfxComponentIndex(component); c >= 0) {
            if (add)
                s.vfx.add(uint32_t(c));
            else
                s.vfx.remove(uint32_t(c));
        } else if (add) {  // Unity's defaults; the first camera is the main one
            bool haveMain = false;
            scene_->world().each([&](flecs::entity, const CameraComponent& c) { haveMain |= (c.flags & 1u) != 0; });
            s.camera = CameraComponent{60.0f, 0.05f, haveMain ? 0u : 1u};
        } else {
            s.camera.reset();
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.body.set" || type == "editor.collider.set") {
        // A Rigidbody's or a Collider's fields (any of them; one undo step).
        const bool isBody = type == "editor.body.set";
        if (!editable(index) || (isBody ? !capture(index).body : !capture(index).collider))
            return notice(isBody ? "Rigidbody: the entity has none" : "Collider: the entity has none", "error");
        State s = capture(index);
        if (isBody) {
            PhysicsBody& b = *s.body;
            b.mass = std::max(float(m.getNumber("mass", b.mass)), 1e-4f);
            b.linearDamping = std::max(float(m.getNumber("linearDamping", b.linearDamping)), 0.0f);
            b.angularDamping = std::max(float(m.getNumber("angularDamping", b.angularDamping)), 0.0f);
            b.gravityScale = float(m.getNumber("gravityScale", b.gravityScale));
            if (const Json* v = m.get("kinematic"))
                b.flags = (b.flags & ~1u) | (v->b ? 1u : 0u);
        } else {
            Collider& c = *s.collider;
            if (const Json* v = m.get("shape"); v && colliderShapeOf(v->str) != c.shape) {
                const uint32_t shape = colliderShapeOf(v->str);
                if (shape == 2 && !entityOf(index).has<MeshInstance>())
                    return notice("Collider: a mesh collider needs a mesh", "error");
                const Collider fitted = defaultCollider(index, shape);  // (the new shape fits the mesh)
                std::memcpy(c.center, fitted.center, sizeof(c.center));
                std::memcpy(c.size, fitted.size, sizeof(c.size));
                c.shape = shape;
            }
            readVec3(m, "center", c.center);
            if (float v[3]; readVec3(m, "size", v))
                for (int k = 0; k < 3; ++k)
                    c.size[k] = std::max(v[k], 1e-3f);
            c.friction = std::max(float(m.getNumber("friction", c.friction)), 0.0f);
            c.bounciness = std::clamp(float(m.getNumber("bounciness", c.bounciness)), 0.0f, 1.0f);
            c.layer = uint32_t(std::clamp(m.getNumber("layer", double(c.layer)), 0.0, 31.0));
            if (const Json* v = m.get("ignore"); v && v->type == Json::Type::Array) {
                c.ignore = 0;
                for (const Json& l : v->array)
                    if (l.number >= 0.0 && l.number < 32.0)
                        c.ignore |= 1u << uint32_t(l.number);
            }
            if (const Json* v = m.get("trigger"))
                c.flags = (c.flags & ~1u) | (v->b ? 1u : 0u);
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.animator.set") {
        // An Animator's graph and parameter values: {graph?, params?: {name: value | null (the
        // graph's default)}}; one undo step.
        if (!editable(index) || !capture(index).animator)
            return notice("Animator: the entity has none", "error");
        State s = capture(index);
        AnimatorEdit& a = *s.animator;
        if (const Json* g = m.get("graph"); g && g->type == Json::Type::String)
            a.graph = g->str;
        if (const Json* v = m.get("editorPreview"); v && v->type == Json::Type::Bool)
            a.editorPreview = v->b;
        // Entity parameters bound: {entities: {name: an entity's id or name (0, "" or null: none)}}.
        // Not the entity itself or a bone.
        if (const Json* e = m.get("entities"); e && e->isObject())
            for (const auto& [name, v] : e->object) {
                const uint32_t target = v.type == Json::Type::String ? (v.str.empty() ? 0u : idByName(v.str))
                                        : v.type == Json::Type::Number ? uint32_t(v.number) : 0u;
                std::erase_if(a.entities, [&](const auto& b) { return b.first == name; });
                if (target == 0) {
                    if (v.type == Json::Type::String && !v.str.empty())
                        return notice(("Animator: no entity named " + v.str).c_str(), "error");
                } else if (target - 1 == index || target > records_.size() || records_[target - 1].kind == EntityKind::Bone) {
                    return notice("Animator: an entity parameter is another entity (not itself or a bone)", "error");
                } else {
                    a.entities.push_back({name, keyOf(target - 1)});
                }
            }
        // {moveIk: true}: its own IK (an older scene's) into its graph's file (with entity parameters
        // for its targets), then gone from here.
        if (const Json* v = m.get("dropIk"); v && v->b)
            a.ik.clear();
        if (const Json* v = m.get("moveIk"); v && v->b) {
            std::string error;
            if (!moveIkToGraph(a, error))
                return notice(("Animator: " + error).c_str(), "error");
        }
        if (const Json* p = m.get("params"); p && p->isObject())
            for (const auto& [name, v] : p->object) {
                auto it = std::find_if(a.params.begin(), a.params.end(), [&](const auto& q) { return q.first == name; });
                if (v.type == Json::Type::Null) {
                    if (it != a.params.end())
                        a.params.erase(it);
                    continue;
                }
                const float value = v.type == Json::Type::Bool ? (v.b ? 1.0f : 0.0f) : float(v.number);
                if (it != a.params.end())
                    it->second = value;
                else
                    a.params.push_back({name, value});
            }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.audio.source.set" || type == "editor.audio.listener.set") {
        // An Audio Source's or Listener's fields (any of them; one undo step).
        const bool isSource = type == "editor.audio.source.set";
        if (!editable(index) || (isSource ? !capture(index).audioSource : !capture(index).audioListener))
            return notice(isSource ? "Audio Source: the entity has none" : "Audio Listener: the entity has none", "error");
        State s = capture(index);
        if (isSource) {
            AudioSource& a = *s.audioSource;
            if (const Json* v = m.get("sound"); v && v->type == Json::Type::String)
                a.sound = soundId(v->str);
            const auto bit = [&](const char* key, uint32_t b) {
                if (const Json* v = m.get(key); v && v->type == Json::Type::Bool)
                    a.flags = v->b ? (a.flags | b) : (a.flags & ~b);
            };
            bit("playOnStart", 1u);
            bit("loop", 2u);
            bit("spatial", 4u);
            bit("ownSettings", 8u);
            bit("airAbsorption", 16u);
            bit("occlusion", 64u);
            bit("transmission", 128u);
            bit("reflections", 256u);
            if (const Json* v = m.get("vary"); v && v->type == Json::Type::Bool)
                a.flags = v->b ? (a.flags & ~512u) : (a.flags | 512u);
            if (const Json* v = m.get("rolloff"); v && v->type == Json::Type::String)
                a.flags = v->str == "linear" ? (a.flags | 32u) : (a.flags & ~32u);
            a.volume = std::clamp(float(m.getNumber("volume", a.volume)), 0.0f, 4.0f);
            a.pitch = std::clamp(float(m.getNumber("pitch", a.pitch)), 0.01f, 8.0f);
            a.minDistance = std::max(float(m.getNumber("minDistance", a.minDistance)), 0.01f);
            a.maxDistance = std::max(float(m.getNumber("maxDistance", a.maxDistance)), a.minDistance);
            if (const Json* v = m.get("bus"); v && v->type == Json::Type::String)
                for (uint32_t b = 0; b < 5; ++b)
                    if (v->str == kSoundBuses[b])
                        a.bus = b;
        } else {
            s.audioListener->gain = std::clamp(float(m.getNumber("gain", s.audioListener->gain)), 0.0f, 4.0f);
        }
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.audio.surface.set") {
        // A mesh's acoustics: its material's (no component), values of its own, or left out.
        if (!editable(index) || !entityOf(index).has<MeshInstance>())
            return notice("Acoustics: not a mesh", "error");
        State s = capture(index);
        AcousticSurface a = s.acoustics.value_or(AcousticSurface{});
        const std::string mode = m.getString("mode", !s.acoustics ? "material" : (a.flags & 1u) ? "excluded" : (a.flags & 2u) ? "own" : "material");
        if (mode == "own" && !(a.flags & 2u)) {  // from the material's values (or its preset's)
            std::string preset;
            AcousticParams p;
            materialAcoustics(index, preset, p);
            a.preset = acousticPresetIndex(preset);
            std::memcpy(a.absorption, p.absorption, sizeof(a.absorption));
            a.scattering = p.scattering;
            std::memcpy(a.transmission, p.transmission, sizeof(a.transmission));
        }
        a.flags = (mode == "excluded" ? 1u : 0u) | (mode == "own" ? 2u : 0u) | (a.flags & (4u | 8u));
        if (const Json* v = m.get("surface"); v && v->type == Json::Type::String) {
            a.surface = surfacePresetIndex(v->str);
            a.flags = a.surface ? (a.flags | 8u) : (a.flags & ~8u);
            if (a.surface && a.impactGain == 0.0f && a.impactPitch == 0.0f)
                a.impactGain = a.impactPitch = 1.0f;
        }
        a.impactGain = std::clamp(float(m.getNumber("impactGain", a.impactGain)), 0.0f, 4.0f);
        a.impactPitch = std::clamp(float(m.getNumber("impactPitch", a.impactPitch)), 0.25f, 4.0f);
        if (const Json* v = m.get("moving"); v && v->type == Json::Type::Bool)
            a.flags = v->b ? (a.flags | 4u) : (a.flags & ~4u);
        if (mode == "own") {
            AcousticParams p;
            std::memcpy(p.absorption, a.absorption, sizeof(p.absorption));
            p.scattering = a.scattering;
            std::memcpy(p.transmission, a.transmission, sizeof(p.transmission));
            std::string preset = acousticPresetName(a.preset);
            readAcousticJson(m, preset, p);
            a.preset = acousticPresetIndex(preset);
            std::memcpy(a.absorption, p.absorption, sizeof(a.absorption));
            a.scattering = p.scattering;
            std::memcpy(a.transmission, p.transmission, sizeof(a.transmission));
        }
        if (a.flags == 0)
            s.acoustics.reset();  // the material's, not moving: nothing of its own
        else
            s.acoustics = a;
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.audio.play") {
        if (!audio_ || !audio_->active())
            return notice("Audio is off (--no-audio, or no device)", "error");
        if (const std::string path = m.getString("path"); !path.empty())
            audio_->preview(path);
        else if (editable(index) && capture(index).audioSource)
            audio_->previewSource(*scene_, entityOf(index).id());
        else
            return notice("Play: nothing to play (no sound, or no Audio Source)", "error");
        if (!audio_->previewing())
            return notice("Play: the sound did not play (no clips, or a clip that does not load)", "error");
    } else if (type == "editor.audio.stop") {
        if (audio_)
            audio_->stopPreview();
    } else if (type.starts_with("editor.audio.bake.")) {
        if (!audio_ || !audio_->active())
            return notice("Audio is off (--no-audio, or no device)", "error");
        const auto vec = [&](const char* key, float3& v) {
            if (const Json* a = m.get(key); a && a->array.size() == 3)
                v = {float(a->array[0].number), float(a->array[1].number), float(a->array[2].number)};
        };
        if (type == "editor.audio.bake.set") {
            AudioBakeSettings s = audio_->bakeSettings();
            s.spacing = std::clamp(float(m.getNumber("spacing", s.spacing)), 0.5f, 50.0f);
            s.height = std::clamp(float(m.getNumber("height", s.height)), 0.1f, 10.0f);
            s.quality = std::clamp(uint32_t(m.getNumber("quality", s.quality)), 1u, 3u);
            s.pathing = m.getBool("pathing", s.pathing);
            s.autoRegion = m.getBool("autoRegion", s.autoRegion);
            vec("center", s.center);
            vec("size", s.size);
            s.size = {std::max(s.size.x, 1.0f), std::max(s.size.y, 1.0f), std::max(s.size.z, 1.0f)};
            if (!audio_->setBakeSettings(s))
                notice("Could not save the bake settings (no scene file?)", "error");
        } else if (type == "editor.audio.bake.fit") {
            AudioBakeSettings s = audio_->bakeSettings();
            if (m.getString("to") == "view") {
                s.autoRegion = false;
                s.center = view_ ? view_->viewPosition() : float3{};
                s.size = {64.0f, 24.0f, 64.0f};
            } else {
                s.autoRegion = true;
            }
            audio_->setBakeSettings(s);
        } else if (type == "editor.audio.bake.start") {
            std::string error;
            if (!audio_->startBake(error))
                return notice(("Bake: " + error).c_str(), "error");
        } else if (type == "editor.audio.bake.cancel") {
            audio_->cancelBake();
        } else if (type == "editor.audio.bake.clear") {
            if (!audio_->clearBake())
                return notice("Bake: nothing to clear, or a bake is running", "error");
            notice("Baked acoustics cleared: realtime only", "info");
        } else if (type == "editor.audio.bake.show") {
            showAudioProbes_ = m.getBool("probes");
        }
        bakeSent_.clear();  // (the page hears the settings back)
    } else if (type == "editor.sound.set") {
        // A .fsound's fields (and clips) written; what plays it hears the change.
        if (!hasProject_)
            return notice("Sounds are edited in a project", "error");
        const std::string path = m.getString("path");
        SoundAsset sound;
        std::string error;
        if (!readSoundAsset(project_.path(path), sound, error))
            return notice(error.c_str(), "error");
        if (const Json* v = m.get("values"))
            readSoundFields(*v, sound);
        if (!writeSoundAsset(project_.path(path), sound, error))
            return notice(error.c_str(), "error");
        if (audio_)
            audio_->reloadSound(path);
        sendState(false);
    } else if (type == "editor.entity.parent") {
        const uint32_t pid = idOf("parent");
        const uint32_t parent = pid == 0 ? ~0u : pid - 1;
        if (!editable(index) || (parent != ~0u && !canHold(parent)))
            return;
        const std::vector<uint32_t> below = subtree(index);
        if (parent != ~0u && std::find(below.begin(), below.end(), parent) != below.end())
            return notice("Parent: an entity cannot go under itself or its own children", "error");
        State s = capture(index);
        if (s.parent == parent)
            return;
        s.parent = parent;  // the world transform stays (Unity's default)
        s.hasLocal = false;
        execute({{index, s}});
        sendState(true);
    } else if (type == "editor.entity.bones") {
        // A character's skeleton as entities under it (one undo step), or put away again.
        if (!editable(index) || !bones_ || !animation_ || !skinning_)
            return notice("Bones: not an editable entity", "error");
        const bool expand = m.getBool("expand", true);
        std::vector<uint32_t> mine = boneRecords(index);
        if (expand == bonesExpanded(index))
            return;
        if (expand && mine.empty()) {
            const BoneEntities::Expanded* x = bones_->expand(*scene_, entityOf(index), *animation_, *skinning_);
            if (!x)
                return notice("Expand Bones: there is no skinned mesh in it", "error");
            mine = addBoneRecords(index, *x);
            for (const uint32_t i : mine)  // (made shown: the command shows them, so undo can hide them)
                entityOf(i).disable();
        }
        if (!expand)  // (what hangs under a bone would lose its place)
            for (uint32_t i = 0; i < records_.size(); ++i)
                if (const uint32_t p = parentOf(i); p != ~0u && records_[p].kind == EntityKind::Bone &&
                                                    records_[p].boneRoot == index && records_[i].kind != EntityKind::Bone &&
                                                    capture(i).alive)
                    return notice(("Collapse Bones: " + defaultName(i) + " hangs under bone " + defaultName(p) +
                                   " (move or delete it first)").c_str(), "error");
        std::vector<std::pair<uint32_t, State>> after;
        for (const uint32_t i : mine) {
            State s = capture(i);
            s.alive = expand;
            after.emplace_back(i, std::move(s));
        }
        execute(after);
        sendState(true);
    } else if (type == "editor.level.op") {
        // A level mesh operation (level/mesh_ops.h) on the selected level mesh's chosen elements (the
        // whole mesh without any), one undo step: the level tools' page (UV editor, palette).
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        const Json* args = m.get("args");
        Json a = args && args->isObject() ? *args : Json::makeObject();
        if (m.getString("op") == "material" && hasProject_ && !a.get("size"))
            addMaterialSize(project_, a);
        applyLevelOp(target, chosen, m.getString("op"), std::move(a));
    } else if (type == "editor.level.wall.create" || type == "editor.level.wall" || type == "editor.level.convert") {
        // A wall or room ({"points": [[x, z], ...] on the ground, "closed", "height", "thickness",
        // "align", "floor", "ceiling", materials}): made (a level mesh entity at the origin, its mesh
        // from the wall) or the selected one's changed; convert: the selected wall a plain level mesh,
        // its openings plain cuts where they are. One undo step each.
        undoBegin();
        if (type == "editor.level.wall.create") {
            Json create = Json::makeObject();
            create.set("type", Json::makeString("editor.entity.create"));
            create.set("kind", Json::makeString("level"));
            create.set("name", Json::makeString(m.getString("name", m.getBool("closed") ? "Room" : "Wall")));
            handle(create, camera);
            if (selected_ != ~0u) {
                State s = capture(selected_);
                const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
                std::memcpy(s.world, identity, sizeof(identity));
                level::Wall wall;
                readWallFields(m, wall);
                s.wall = wall;
                s.levelMesh = std::make_shared<level::PolyMesh>(level::buildWall(wall));
                execute({{selected_, s}});
            }
        } else if (selected_ != ~0u && editable(selected_)) {
            State s = capture(selected_);
            if (!s.wall && !(type == "editor.level.convert" && s.roof)) {
                undoEnd();
                return notice("Select a wall first", "warning");
            }
            if (type == "editor.level.wall") {
                level::Wall wall = *s.wall;
                readWallFields(m, wall);
                s.wall = wall;
                s.levelMesh = std::make_shared<level::PolyMesh>(level::buildWall(wall));
                execute({{selected_, s}});
                std::vector<std::pair<uint32_t, State>> openings;  // (they stay on it, where they were along it)
                for (uint32_t k = 0; k < records_.size(); ++k)
                    if (State o = capture(k); parentOf(k) == selected_ && o.opening && frameOpening(o, false))
                        openings.push_back({k, o});
                if (!openings.empty())
                    execute(openings);
            } else {  // convert: the mesh kept, the parameters gone; the openings where they are
                std::vector<std::pair<uint32_t, State>> after;
                s.wall.reset();
                s.roof.reset();
                after.push_back({selected_, s});
                for (uint32_t k = 0; k < records_.size(); ++k)
                    if (parentOf(k) == selected_) {
                        State o = capture(k);
                        float w[3][4];
                        if (o.opening && LevelMeshes::openingWorld(entityOf(k), w)) {
                            o.opening.reset();
                            std::memcpy(o.world, w, sizeof(w));
                            after.push_back({k, o});
                        }
                    }
                execute(after);
            }
        }
        undoEnd();
        sendState(true);
    } else if (type == "editor.level.prototype") {
        // The prototype textures and their materials (level/prototype_materials.h) put into the
        // project again (written over), and made the level meshes' default.
        if (!hasProject_)
            return notice("Open a project first", "error");
        std::string error;
        int made = 0;
        const std::string content = std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "") + "content/prototype";
        if (!level::installPrototype(content, project_.dir, error, &made))
            return notice(error.c_str(), "error");
        project_.prototypeInstalled = true;
        project_.levelDefaultMaterial = level::kPrototypeDefault;
        if (!saveProject(project_, error))
            notice(error.c_str(), "error");
        notice(("Prototype textures and " + std::to_string(made) + " materials in " + level::kPrototypeFolder +
                " (the scenes are cooked again to show them)").c_str());
    } else if (type == "editor.level.roof.create" || type == "editor.level.roof") {
        // A roof ({"outline": [[x, z], ...], "style": flat | shed | gable | hip | pyramid, "base", "pitch",
        // "overhang", "thickness", "material", "under", "name"}): made over the selected room or wall
        // (its outline, on top of it, where it is) or over the outline given; or the selected roof's
        // changed. One undo step each.
        if (type == "editor.level.roof") {
            if (selected_ == ~0u || !editable(selected_))
                return;
            State s = capture(selected_);
            if (!s.roof)
                return notice("Select a roof first", "warning");
            level::readRoof(m, *s.roof);
            s.levelMesh = std::make_shared<level::PolyMesh>(level::buildRoof(*s.roof));
            execute({{selected_, s}});
            sendState(true);
            return;
        }
        level::Roof roof;
        float world[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        if (!m.get("outline") && selected_ != ~0u) {
            const State room = capture(selected_);
            if (room.wall && room.wall->points.size() >= 3 && worldOf(selected_, world)) {
                roof.outline = room.wall->points;
                roof.base = room.wall->height;
            }
        }
        level::readRoof(m, roof);
        if (roof.outline.size() < 3)
            return notice("Select a room first (a roof goes over its walls)", "warning");
        undoBegin();
        Json create = Json::makeObject();
        create.set("type", Json::makeString("editor.entity.create"));
        create.set("kind", Json::makeString("level"));
        create.set("name", Json::makeString(m.getString("name", "Roof")));
        handle(create, camera);
        if (selected_ != ~0u) {
            State s = capture(selected_);
            std::memcpy(s.world, world, sizeof(world));
            s.roof = roof;
            s.levelMesh = std::make_shared<level::PolyMesh>(level::buildRoof(roof));
            execute({{selected_, s}});
        }
        undoEnd();
        sendState(true);
    } else if (type == "editor.level.opening.set") {
        // The selected opening's {"distance", "width", "height", "sill"}: one undo step.
        if (selected_ == ~0u || !editable(selected_))
            return;
        State s = capture(selected_);
        if (!s.opening)
            return notice("Select an opening first", "warning");
        WallOpening& o = *s.opening;
        o.distance = std::max(float(m.getNumber("distance", o.distance)), 0.0f);
        o.width = std::max(float(m.getNumber("width", o.width)), 0.01f);
        o.height = std::max(float(m.getNumber("height", o.height)), 0.01f);
        o.sill = std::max(float(m.getNumber("sill", o.sill)), 0.0f);
        frameOpening(s, false);
        execute({{selected_, s}});
        sendState(true);
    } else if (type == "editor.level.opening.preview") {
        // The opening tool's outline where a click would cut it ({"at", "width", "height", "sill"};
        // "off" or no wall there: none): its box through the wall, drawn over the view.
        std::vector<SceneView::WorldLine> lines;
        uint32_t wallIndex = ~0u;
        float distance = 0.0f, ww[3][4], f[3][4];
        WallOpening o;
        o.width = float(m.getNumber("width", 1.0));
        o.height = float(m.getNumber("height", 2.1));
        o.sill = float(m.getNumber("sill", 0.0));
        if (!m.getBool("off") && openingPlace(m, wallIndex, distance, true)) {
            const LevelWall* wall = entityOf(wallIndex).try_get<LevelWall>();
            o.distance = distance;
            if (wall && worldOf(wallIndex, ww) && LevelMeshes::openingFrame(wall->wall, ww, o, f)) {
                const float hx = o.width * 0.5f, hz = wall->wall.thickness * 0.5f + 0.03f;  // (a little proud of both sides)
                float3 c[8];
                for (int k = 0; k < 8; ++k) {
                    const float x = k & 1 ? hx : -hx, y = k & 2 ? o.height : 0.0f, z = k & 4 ? hz : -hz;
                    c[k] = {f[0][0] * x + f[0][1] * y + f[0][2] * z + f[0][3], f[1][0] * x + f[1][1] * y + f[1][2] * z + f[1][3],
                            f[2][0] * x + f[2][1] * y + f[2][2] * z + f[2][3]};
                }
                const uint32_t ink = 0xff40d0ffu;  // (amber: ABGR)
                static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& e : edges)
                    lines.push_back({c[e[0]], c[e[1]], ink});
            }
        }
        if (view_)
            view_->setPreviewLines(std::move(lines));
    } else if (type == "editor.level.opening") {
        // An opening in a wall ({"at": a point of the view on the wall (else "distance" along the
        // selected wall), "width", "height", "sill", "round", "name"}): a cut under the wall, on its path.
        uint32_t wallIndex = ~0u;
        float distance = 0.0f;
        if (!openingPlace(m, wallIndex, distance, false))
            return notice("Click a wall (a parametric one)", "warning");
        undoBegin();
        Json create = Json::makeObject();
        create.set("type", Json::makeString("editor.entity.create"));
        create.set("kind", Json::makeString("empty"));
        create.set("name", Json::makeString(m.getString("name", "Opening")));
        create.set("parent", Json::makeNumber(double(wallIndex + 1)));
        handle(create, camera);
        if (selected_ != ~0u && selected_ != wallIndex) {
            State s = capture(selected_);
            WallOpening opening;
            opening.distance = distance;
            opening.width = float(m.getNumber("width", 1.0));
            opening.height = float(m.getNumber("height", 2.1));
            opening.sill = float(m.getNumber("sill", 0.0));
            s.opening = opening;
            frameOpening(s, false);
            CutVolume cut;
            cut.shape = m.getBool("round") ? level::CutShape::Cylinder : level::CutShape::Box;
            cut.capMaterial = m.getString("cap");
            s.cut = cut;
            execute({{selected_, s}});
        }
        undoEnd();
        sendState(true);
    } else if (type == "editor.level.cut.create" || type == "editor.level.cut") {
        // A cut volume ({"shape": "box" | "cylinder" | "sphere" | "mesh" | "damage", "intersect", "cap":
        // material path, damage's "seed", "roughness"}): made (an empty "Cut" at the view's centre,
        // 1 m) or the selected entity's changed, one undo step.
        undoBegin();
        if (type == "editor.level.cut.create") {
            Json create = Json::makeObject();
            create.set("type", Json::makeString("editor.entity.create"));
            create.set("kind", Json::makeString("empty"));
            create.set("name", Json::makeString(m.getString("name", "Cut")));
            handle(create, camera);
            if (selected_ != ~0u && view_) {  // (where the view looks)
                State s = capture(selected_);
                const float3 at = placementPoint(0.5f, 0.5f, camera, nullptr);
                s.world[0][3] = at.x, s.world[1][3] = at.y, s.world[2][3] = at.z;
                execute({{selected_, s}});
            }
        }
        if (selected_ != ~0u && editable(selected_)) {
            State s = capture(selected_);
            CutVolume v = s.cut ? *s.cut : CutVolume{};
            if (const std::string shape = m.getString("shape"); !shape.empty())
                for (uint32_t k = 0; k < level::kCutShapes; ++k)
                    if (shape == level::kCutShapeNames[k])
                        v.shape = level::CutShape(k);
            v.intersect = m.getBool("intersect", v.intersect);
            v.seed = uint32_t(m.getNumber("seed", v.seed));
            v.roughness = std::clamp(float(m.getNumber("roughness", v.roughness)), 0.0f, 0.9f);
            if (m.get("cap"))
                v.capMaterial = m.getString("cap");
            if (m.getBool("capPicked"))  // (the eyedropper's material)
                v.capMaterial = pickedMaterial_;
            if (m.getBool("remove"))
                s.cut.reset();
            else
                s.cut = v;
            execute({{selected_, s}});
        }
        undoEnd();
        sendState(true);
    } else if (type == "editor.level.damage") {
        // Damage over the selection (a spline's curve, a level mesh's chosen elements or its sharp
        // outside edges; or the "vertices", "edges" or "faces" it names), as the damage tool is
        // set; one undo step.
        if (playing_)
            return;
        std::optional<ElementSelection> named;
        static const char* const kKeys[] = {"vertices", "edges", "faces"};
        for (uint32_t k = 0; k < 3; ++k)
            if (const Json* items = m.get(kKeys[k]); items && items->type == Json::Type::Array) {
                named = ElementSelection{selected_, k + 1, {}};
                for (const Json& item : items->array)
                    named->items.push_back(uint64_t(item.number));
            }
        damageSettings(m);
        if (const uint32_t made = damageSelection(named ? &*named : nullptr); made == 0)
            notice("Select a spline over level meshes, or a level mesh (its chosen vertices, edges or faces; in the object mode its sharp edges)",
                   "warning");
        else
            notice((std::to_string(made) + (made == 1 ? " damage cut" : " damage cuts")).c_str());
    } else if (type == "editor.level.smart" || type == "editor.level.smart.list" || type == "editor.level.smart.starter") {
        // Smart material presets (level/smart_material.h): the project's .fsmart files (to the page);
        // one applied to the selected level mesh ({"path"}: its faces, layers, wear and grime, one
        // undo step); the starter presets made in Assets/Smart.
        if (!hasProject_)
            return notice("Smart materials live in a project", "warning");
        std::string error;
        if (type == "editor.level.smart.starter") {
            if (!level::makeStarterSmartMaterials(project_.dir, "Assets/Smart", error))
                return notice(error.c_str(), "error");
            assets_.rescanNow();
            notice("Made the starter smart materials in Assets/Smart");
        } else if (type == "editor.level.smart") {
            uint32_t target;
            ElementSelection chosen;
            level::SmartMaterial smart;
            if (!levelTarget(target, chosen))
                return notice("Select a level mesh first", "warning");
            if (!level::readSmartMaterial(project_.path(m.getString("path")), smart, error))
                return notice(error.c_str(), "error");
            State s = capture(target);
            level::PolyMesh mesh = *s.levelMesh;
            std::vector<uint8_t> mask;
            LevelWear wear;  // (live: made again as it is cut, edited, built around)
            if (!level::applySmartMaterial(smart, mesh, mask, error, &wear.rules))
                return notice(error.c_str(), "error");
            s.levelMesh = std::make_shared<level::PolyMesh>(std::move(mesh));
            s.levelMask = nullptr;
            if (wear.rules.empty())
                s.wear.reset();
            else
                s.wear = std::move(wear);
            execute({{target, s}});
            sendState(false);
            return;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        JsonWriter w;
        w.beginObject().value("type", "editor.level.smartList").beginArray("presets");
        std::vector<std::string> paths;
        const fs::path root = fs::path(project_.dir);
        for (fs::recursive_directory_iterator it(root / "Assets", ec), end; it != end && !ec; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".fsmart")
                paths.push_back(it->path().lexically_relative(root).generic_string());
        std::sort(paths.begin(), paths.end());
        for (const std::string& p : paths)
            w.value(nullptr, p);
        w.endArray().endObject();
        if (ui_)
            ui_->send(w.str());
    } else if (type == "editor.level.volume") {
        // The selected level mesh a gameplay volume ({"kind": "trigger" | "kill" | "ladder" | "audio" |
        // "navBlocker" | "navArea", "tag", "cost"}; "remove": an ordinary mesh again), or ("create")
        // a new 2 m box volume at the view's centre. One undo step.
        LevelVolume vol;
        const std::string kind = m.getString("kind", "trigger");
        for (uint32_t k = 0; k < kVolumeKinds; ++k)
            if (kind == kVolumeKindNames[k])
                vol.kind = VolumeKind(k);
        if (m.getBool("create")) {
            static const char* const kNames[] = {"Trigger Volume", "Kill Volume", "Ladder Volume", "Audio Volume", "Nav Blocker", "Nav Area"};
            const float3 at = camera_ ? camera_->pivot() : camera.position + camera.forward * 4.0f;
            State s;
            const float world[3][4] = {{2, 0, 0, at.x}, {0, 2, 0, at.y + 1.0f}, {0, 0, 2, at.z}};
            std::memcpy(s.world, world, sizeof(world));
            const uint32_t made = spawn(EntityKind::Level, "box", s.world, m.getString("name", kNames[uint32_t(vol.kind) % kVolumeKinds]));
            if (made == ~0u)
                return;
            s.alive = true;
            s.name = m.get("name") ? m.getString("name") : defaultName(made);
            s.levelMesh = std::make_shared<level::PolyMesh>(level::PolyMesh::box({1, 1, 1}));
            vol.tag = m.getString("tag");
            vol.cost = float(m.getNumber("cost", vol.cost));
            s.volume = vol;
            execute({{made, s}});
            select(made);
            return sendState(true);
        }
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh to make a volume", "warning");
        State s = capture(target);
        if (m.getBool("remove")) {
            s.volume.reset();
        } else {
            if (s.volume && !m.get("kind"))
                vol.kind = s.volume->kind;
            vol.tag = m.get("tag") ? m.getString("tag") : s.volume ? s.volume->tag : std::string();
            vol.cost = std::max(float(m.getNumber("cost", s.volume ? s.volume->cost : vol.cost)), 0.01f);
            s.volume = vol;
        }
        execute({{target, s}});
        sendState(true);
    } else if (type == "editor.level.snapPoint") {
        // A snap point under the selected entity at its origin, facing its +z ({"tag"}), selected;
        // or ("set") the selected snap point's tag. One undo step.
        if (m.getBool("set")) {
            if (selected_ == ~0u || !editable(selected_))
                return;
            State s = capture(selected_);
            if (!s.snap)
                return notice("Select a snap point first", "warning");
            s.snap->tag = m.getString("tag");
            execute({{selected_, s}});
            return sendState(true);
        }
        if (selected_ == ~0u || !canHold(selected_))
            return notice("Select the kit piece the snap point goes on", "warning");
        State s;
        float w[3][4];
        worldOf(selected_, w);
        for (int c = 0; c < 3; ++c) {  // (its turn, not its size)
            const float3 col = {w[0][c], w[1][c], w[2][c]};
            const float len = length(col);
            for (int r = 0; r < 3; ++r)
                w[r][c] = len > 1e-8f ? w[r][c] / len : (r == c ? 1.0f : 0.0f);
        }
        std::memcpy(s.world, w, sizeof(w));
        const uint32_t made = spawn(EntityKind::Empty, "", s.world, "Snap Point");
        if (made == ~0u)
            return;
        s.parent = selected_;
        s.alive = true;
        s.name = defaultName(made);
        s.snap = SnapPoint{m.getString("tag")};
        execute({{made, s}});
        select(made);
        sendState(true);
    } else if (type == "editor.view") {
        // The camera: {"orthographic": bool (none: the other way)}, {"look": "front" | "back" | "right" |
        // "left" | "top" | "bottom"}.
        if (!camera_)
            return;
        if (m.get("orthographic") || m.getString("look").empty())
            camera_->setOrthographic(m.get("orthographic") ? m.getBool("orthographic") : !camera_->orthographic());
        static const std::pair<const char*, float3> kLooks[] = {{"front", {0, 0, -1}}, {"back", {0, 0, 1}}, {"right", {-1, 0, 0}},
                                                                {"left", {1, 0, 0}},   {"top", {0, -1, 0}}, {"bottom", {0, 1, 0}}};
        for (const auto& [name, dir] : kLooks)
            if (m.getString("look") == name)
                camera_->lookAlong(dir);
        if (view_)
            view_->tools().orthographic = camera_->orthographic();
        toolsChanged();
    } else if (type == "editor.nav.bake") {  // the scene's navigation mesh baked now (level_navigation.cpp)
        std::string error;
        if (!navBake(error))
            notice(error.c_str(), "warning");
        sendState(false);
    } else if (type == "editor.nav.show") {  // the overlay ({"on"}; none: the other way)
        if (view_) {
            view_->tools().showNav = m.get("on") ? m.getBool("on") : !view_->tools().showNav;
            toolsChanged();
        }
    } else if (type == "editor.nav.clear") {  // none (its file gone)
        navClear();
        sendState(false);
    } else if (type == "editor.arrange") {
        // {"op": "align" | "distribute" | "ground", "axis": 0..2, "edge": -1 | 0 | 1} (level_arrange.cpp)
        arrange(m.getString("op"), uint32_t(m.getNumber("axis", 0)), int32_t(m.getNumber("edge", 0)));
    } else if (type == "editor.group") {
        group();
    } else if (type == "editor.ungroup") {
        ungroup();
    } else if (type == "editor.replace") {  // {"prefab": path}
        replaceWith(m.getString("prefab"));
    } else if (type == "editor.level.spline.create") {
        // A spline through "points" ([[x, y, z]...], world), "closed", lofted into "profile" (road,
        // kerb, wall, rail, pipe; none: a bare spline), named "name": its entity at its first point.
        const Json* points = m.get("points");
        if (!points || points->array.empty())
            return notice("A spline needs a point", "warning");
        LevelSpline spline;
        level::LoftSettings unused;
        level::readSpline(m, spline.spline, unused);
        spline.loft = level::loftPreset(m.getString("profile"));
        const float3 at = spline.spline.points[0].position;
        for (level::SplinePoint& p : spline.spline.points)
            p.position = p.position - at;
        State s;
        const float world[3][4] = {{1, 0, 0, at.x}, {0, 1, 0, at.y}, {0, 0, 1, at.z}};
        std::memcpy(s.world, world, sizeof(world));
        const uint32_t created = spawn(EntityKind::Level, "box", s.world, m.getString("name", "Spline"));
        if (created == ~0u)
            return;
        s.alive = true;
        s.name = m.get("name") ? m.getString("name") : defaultName(created);
        s.spline = spline;
        s.levelMesh = std::make_shared<level::PolyMesh>(level::loftSpline(spline.spline, spline.loft));
        execute({{created, s}});
        select(created);
        sendState(true);
    } else if (type == "editor.level.spline") {
        // The selected spline changed, one undo step: "closed", "profile" (its preset sizes with it),
        // "width", "height", "sides", "step", "uvLength", "material", "corner" (a point's index:
        // toggled), "corners" (all: true / false), "points" ([[x, y, z]...] in its space).
        if (selected_ == ~0u || !editable(selected_))
            return;
        State s = capture(selected_);
        if (!s.spline)
            return notice("Select a spline first", "warning");
        LevelSpline& sp = *s.spline;
        if (const Json* v = m.get("points")) {
            Json only = Json::makeObject();
            only.set("points", *v);
            level::LoftSettings unused;
            level::Spline read;
            level::readSpline(only, read, unused);
            read.closed = sp.spline.closed;
            sp.spline = read;
        }
        sp.spline.closed = m.getBool("closed", sp.spline.closed) && sp.spline.points.size() >= 3;
        if (const Json* v = m.get("profile"); v && v->str != sp.loft.profile) {
            const std::string material = sp.loft.material;
            sp.loft = level::loftPreset(v->str);
            sp.loft.material = material;
        }
        sp.loft.width = std::max(float(m.getNumber("width", sp.loft.width)), 0.001f);
        sp.loft.height = std::max(float(m.getNumber("height", sp.loft.height)), 0.001f);
        sp.loft.sides = uint32_t(std::clamp(m.getNumber("sides", sp.loft.sides), 3.0, 128.0));
        sp.loft.step = std::max(float(m.getNumber("step", sp.loft.step)), 0.02f);
        sp.loft.uvLength = std::max(float(m.getNumber("uvLength", sp.loft.uvLength)), 0.0f);
        if (m.get("material"))
            sp.loft.material = m.getString("material");
        if (const double k = m.getNumber("corner", -1); k >= 0 && k < double(sp.spline.points.size()))
            sp.spline.points[size_t(k)].corner = !sp.spline.points[size_t(k)].corner;
        if (const Json* v = m.get("corners"))
            for (level::SplinePoint& p : sp.spline.points)
                p.corner = v->b;
        execute({{selected_, s}});
        sendState(true);
    } else if (type == "editor.level.modifier") {
        // The selected level mesh's modifier stack (level/modifiers.h), one undo step: "add" {"kind",
        // "mode" (arrays), "deform" (deforms' type), "follow" (curves), "class" (C#)} at its end; "set" {"index", any of its fields}; "remove",
        // "up", "down" {"index"}; "apply" (the first, or "all") into the mesh itself (a wall becomes a
        // plain mesh); "clear".
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        State s = capture(target);
        if (!s.levelMesh)
            return notice("Select a level mesh first", "warning");
        LevelModifiers mods = s.modifiers ? *s.modifiers : LevelModifiers{};
        const std::string op = m.getString("op", "add");
        const double at = m.getNumber("index", -1);
        const bool indexed = at >= 0 && at < double(mods.stack.size());
        const size_t index = indexed ? size_t(at) : 0;
        if (op == "add") {
            Json j = Json::makeObject();
            for (const char* key : {"kind", "mode", "follow", "class"})
                if (const Json* v = m.get(key))
                    j.set(key, *v);
            if (const Json* v = m.get("deform"))  // (a deform's type: "type" is the message's own)
                j.set("type", *v);
            mods.stack.push_back(level::modifierFromJson(j));
            if (mods.stack.back().kind == level::ModifierKind::Curve) {
                // It follows the spline it is under: a spline selected with it becomes its parent.
                const auto isSpline = [&](uint32_t k) { return k < records_.size() && entityOf(k) && entityOf(k).has<LevelSpline>(); };
                if (!isSpline(s.parent)) {
                    uint32_t spline = ~0u;
                    for (const uint32_t k : selection_)
                        if (k != target && isSpline(k))
                            spline = k;
                    if (spline == ~0u)
                        return notice("Select a spline with it (or put it under one): it follows its parent spline", "warning");
                    s.parent = spline;
                }
            }
        } else if (op == "set" && indexed) {
            // Its fields as JSON, the message's over them, read back.
            JsonWriter w;
            level::writeModifierJson(w, nullptr, mods.stack[index]);
            Json j = Json::parse(w.str());
            for (const auto& [key, value] : m.object)
                if (key == "settings" && value.isObject()) {  // (a C# modifier's: merged into what it has)
                    Json& settings = j.child("settings");
                    for (const auto& [name, v] : value.object)
                        settings.set(name, v);
                } else if (key != "type" && key != "op" && key != "index") {
                    j.set(key, value);
                }
            if (const Json* v = m.get("deform"))  // ("type" is the message's own: a deform's is "deform" here)
                j.set("type", *v);
            mods.stack[index] = level::modifierFromJson(j);
        } else if (op == "remove" && indexed) {
            mods.stack.erase(mods.stack.begin() + ptrdiff_t(index));
        } else if ((op == "up" && indexed && index > 0) || (op == "down" && indexed && index + 1 < mods.stack.size())) {
            std::swap(mods.stack[index], mods.stack[op == "up" ? index - 1 : index + 1]);
        } else if (op == "apply" && !mods.stack.empty()) {
            const size_t n = m.getBool("all") ? mods.stack.size() : 1;
            const std::vector<level::Modifier> applied(mods.stack.begin(), mods.stack.begin() + ptrdiff_t(n));
            auto mesh = std::make_shared<level::PolyMesh>();
            std::string error;
            if (!level::applyModifiers(*s.levelMesh, applied, *mesh, nullptr, &error))
                return notice(error.c_str(), "error");
            s.levelMesh = std::move(mesh);
            mods.stack.erase(mods.stack.begin(), mods.stack.begin() + ptrdiff_t(n));
            s.wall.reset();  // (its mesh is no longer the wall's, or the roof's)
            s.roof.reset();
        } else if (op == "clear") {
            mods.stack.clear();
        } else {
            return;
        }
        if (mods.stack.empty())
            s.modifiers.reset();
        else
            s.modifiers = std::move(mods);
        execute({{target, s}});
        sendState(true);
    } else if (type == "editor.level.surface") {
        // One of the selected level mesh's surface materials (painted over its faces' own; the paint
        // panel's and the Inspector's list), one undo step: {"layer": 0..2, "material" (a project path;
        // "": removed, with its wear and what was painted of it), "use": "paint" (the brush only) |
        // "edges" | "grime" | "streaks" (live wear: kept up as the mesh is edited and cut)}.
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        const double layerAt = m.getNumber("layer", -1);
        if (layerAt < 0 || layerAt > 2)
            return;
        const uint32_t layer = uint32_t(layerAt);
        State s = capture(target);
        if (!s.levelMesh)
            return;
        level::PolyMesh mesh = *s.levelMesh;
        LevelWear wear = s.wear ? *s.wear : LevelWear{};
        const auto dropWear = [&] {
            std::erase_if(wear.rules, [&](const level::WearSettings& r) { return r.channel == layer; });
        };
        if (const Json* v = m.get("material"); v && v->type == Json::Type::String) {
            if (v->str.empty()) {
                mesh.layerMaterials[layer].clear();
                dropWear();
                if (s.levelMask)
                    s.levelMask = std::make_shared<const std::vector<uint8_t>>(level::maskFilled(s.levelMask.get(), layer, 0.0f));
            } else {  // (the layers op: the mask UVs made when it has none)
                Json args = Json::makeObject(), list;
                list.type = Json::Type::Array;
                for (uint32_t k = 0; k < 3; ++k)
                    list.array.push_back(Json::makeString(k == layer ? v->str : mesh.layerMaterials[k]));
                args.set("materials", std::move(list));
                args.set("blend", Json::makeNumber(mesh.layerBlend));
                level::OpResult r;
                if (!level::runOp("layers", mesh, args, r))
                    return notice(r.error.c_str(), "warning");
                mesh = std::move(r.mesh);
            }
        }
        if (const Json* u = m.get("use"); u && u->type == Json::Type::String && !mesh.layerMaterials[layer].empty()) {
            const auto kept = std::find_if(wear.rules.begin(), wear.rules.end(), [&](const level::WearSettings& r) {
                return r.channel == layer && u->str == level::kWearKindNames[uint32_t(r.kind) % level::kWearKinds];
            });
            if (u->str == "paint") {
                dropWear();
            } else if (kept == wear.rules.end()) {  // (the same use kept as it was: its width, strength)
                Json j = Json::makeObject();
                j.set("kind", Json::makeString(u->str));
                j.set("layer", Json::makeNumber(layer));
                const level::WearSettings r = level::wearFromJson(j, level::WearKind::Edges);
                if (level::kWearKindNames[uint32_t(r.kind) % level::kWearKinds] != u->str)
                    return;
                dropWear();
                wear.rules.push_back(r);
            } else {
                const level::WearSettings r = *kept;
                dropWear();
                wear.rules.push_back(r);
            }
        }
        if (!mesh.layered()) {
            wear.rules.clear();
            s.levelMask = nullptr;
        }
        s.levelMesh = std::make_shared<level::PolyMesh>(std::move(mesh));
        if (wear.rules.empty())
            s.wear.reset();
        else
            s.wear = std::move(wear);
        execute({{target, s}});
        sendState(true);
    } else if (type == "editor.level.wear") {
        // The selected level mesh's live wear (LevelWear: made again as it is edited or cut, or the
        // level meshes around it change), one undo step: "add" {"kind": "edges" | "grime" |
        // "streaks", "layer" (none: edges' 1, the others' 2), "width", "strength", "foot", "seed"}
        // (one of that kind into that layer already: replaced), "set" {"index", the fields},
        // "remove" {"index"}, "clear", "bake" (what is made into the painted mask, the rules gone).
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        State s = capture(target);
        if (!s.levelMesh || !s.levelMesh->layered())
            return notice("Give the level mesh layers first (wear goes into its mask)", "warning");
        LevelWear wear = s.wear ? *s.wear : LevelWear{};
        const std::string op = m.getString("op", "add");
        const double index = m.getNumber("index", -1);
        const bool indexed = index >= 0 && index < double(wear.rules.size());
        if (op == "add") {
            level::WearSettings r = level::wearFromJson(m, level::WearKind::Edges);
            if (m.getNumber("layer", -1) < 0)
                r.channel = r.kind == level::WearKind::Edges ? 0u : 1u;
            const auto same = std::find_if(wear.rules.begin(), wear.rules.end(),
                                           [&](const level::WearSettings& o) { return o.kind == r.kind && o.channel == r.channel; });
            if (same != wear.rules.end())
                *same = r;
            else
                wear.rules.push_back(r);
        } else if (op == "set" && indexed) {
            level::WearSettings& r = wear.rules[size_t(index)];
            r.channel = uint32_t(std::clamp(m.getNumber("layer", r.channel), 0.0, 2.0));
            r.width = std::max(float(m.getNumber("width", r.width)), 0.001f);
            r.strength = std::clamp(float(m.getNumber("strength", r.strength)), 0.0f, 1.0f);
            r.foot = std::max(float(m.getNumber("foot", r.foot)), 0.0f);
            r.seed = uint32_t(std::max(m.getNumber("seed", r.seed), 0.0));
        } else if (op == "remove" && indexed) {
            wear.rules.erase(wear.rules.begin() + ptrdiff_t(index));
        } else if (op == "clear") {
            wear.rules.clear();
        } else if (op == "bake") {
            const std::vector<uint8_t>* made = levelMeshes_ ? levelMeshes_->wearOf(entityOf(target).id()) : nullptr;
            if (wear.rules.empty() || !made)
                return notice(wear.rules.empty() ? "No live wear to bake" : "The wear is still being made", "warning");
            std::vector<uint8_t> mask = s.levelMask && s.levelMask->size() == made->size() ? *s.levelMask : *made;
            for (size_t i = 0; i < mask.size(); ++i)
                mask[i] = std::max(mask[i], (*made)[i]);
            s.levelMask = std::make_shared<const std::vector<uint8_t>>(std::move(mask));
            wear.rules.clear();
        } else {
            return;
        }
        if (wear.rules.empty())
            s.wear.reset();
        else
            s.wear = std::move(wear);
        execute({{target, s}});
        sendState(true);
    } else if (type == "editor.level.mask") {
        // The selected level mesh's painted mask ({"op": "fill", "channel": 0..3, "value": 0..1}: a
        // channel set everywhere; "clear": none), one undo step. (The paint brush: LV9b.)
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        State s = capture(target);
        if (const std::string op = m.getString("op"); op == "wear" || op == "grime") {
            // Generated wear (level/wear.h) into a layer's channel ("channel": -1 or none: the paint
            // tool's layer): "width", "strength", "foot" (grime), "seed".
            const auto mesh = levelMeshOf(target);
            level::WearSettings w;
            w.kind = op == "wear" ? level::WearKind::Edges : level::WearKind::Grime;
            const int32_t channel = int32_t(m.getNumber("channel", -1));
            w.channel = channel >= 0 ? uint32_t(channel) : view_ && view_->tools().paintMode < 3 ? view_->tools().paintMode : 0u;
            w.width = float(m.getNumber("width", w.kind == level::WearKind::Edges ? 0.08 : 0.15));
            w.strength = float(m.getNumber("strength", 1.0));
            w.foot = float(m.getNumber("foot", w.foot));
            w.seed = uint32_t(m.getNumber("seed", 1));
            std::vector<uint8_t> mask = s.levelMask ? *s.levelMask : std::vector<uint8_t>{};
            if (!mesh || !mesh->layered() || !level::generateWear(*mesh, w, mask))
                return notice("Give the level mesh layers first (wear goes into its mask)", "warning");
            s.levelMask = std::make_shared<const std::vector<uint8_t>>(std::move(mask));
            execute({{target, s}});
            sendState(false);
            return;
        }
        if (m.getString("op") == "clear")
            s.levelMask = nullptr;
        else
            s.levelMask = std::make_shared<const std::vector<uint8_t>>(
                level::maskFilled(s.levelMask.get(), uint32_t(m.getNumber("channel", 0)), float(m.getNumber("value", 1))));
        execute({{target, s}});
        sendState(false);
    } else if (type == "editor.level.uvs") {  // {"open": bool}: the page's UV editor (its UVs follow the state)
        uvPanel_ = m.getBool("open", uvPanel_);
        lastUvs_.clear();
        sendLevelUvs();
    } else if (type == "editor.trim.list" || type == "editor.trim.save" || type == "editor.trim.starter") {
        // Trim sheets (level/trim_sheet.h): the project's .ftrim files for the page's trim panel;
        // one written ({"path", "sheet": {...}}); the starter set made in Assets/Trims.
        if (!hasProject_)
            return notice("Trim sheets live in a project", "warning");
        std::string error;
        if (type == "editor.trim.save") {
            const std::string path = m.getString("path");
            level::TrimSheet sheet;
            const Json* s = m.get("sheet");
            if (!path.ends_with(".ftrim") || !s || !level::parseTrimSheet(s->dump(0), sheet, error) ||
                !level::writeTrimSheet(project_.path(path), sheet, error))
                return notice(error.empty() ? "A trim sheet is saved as a .ftrim" : error.c_str(), "error");
            assets_.rescanNow();
        } else if (type == "editor.trim.starter") {
            std::string made;
            if (!level::makeStarterTrims(project_.dir, "Assets/Trims", made, error))
                return notice(error.c_str(), "error");
            assets_.rescanNow();
            notice(("Made the starter trims: " + made).c_str());
        }
        sendTrimSheets();
    } else if (type == "editor.level.trim") {
        // The chosen faces onto a trim sheet's strip ({"path", "strip"}; none: the last one used):
        // the sheet's material and the strip's rows, one undo step.
        std::string path = m.getString("path", lastTrim_.first.c_str());
        const uint32_t strip = uint32_t(m.getNumber("strip", double(lastTrim_.second)));
        level::TrimSheet sheet;
        std::string error;
        if (path.empty() || !hasProject_)
            return notice("Pick a trim strip first (Alt+T: the trim sheets)", "warning");
        if (!level::readTrimSheet(project_.path(path), sheet, error) || strip >= sheet.strips.size())
            return notice(error.empty() ? "No such trim strip" : error.c_str(), "error");
        uint32_t target;
        ElementSelection chosen;
        if (!levelTarget(target, chosen))
            return notice("Select a level mesh first", "warning");
        lastTrim_ = {path, strip};
        Json args = Json::makeObject();
        args.set("v0", Json::makeNumber(sheet.strips[strip].v0));
        args.set("v1", Json::makeNumber(sheet.strips[strip].v1));
        args.set("aspect", Json::makeNumber(sheet.aspect));
        args.set("material", Json::makeString(sheet.material));
        applyLevelOp(target, chosen, "trim", std::move(args));
    } else if (type == "editor.level.trimPanel") {  // (the UV/Trim Sheets command: the page shows or hides it)
        if (ui_)
            ui_->send("{\"type\":\"editor.level.trimPanel\"}");
        sendTrimSheets();
    } else if (type == "editor.level.uvEditor") {  // (the UV/Editor command: the page shows or hides it)
        if (ui_)
            ui_->send("{\"type\":\"editor.level.uvEditor\"}");
    } else if (type == "editor.level.material" || type == "editor.level.pickMaterial") {
        // A material onto level mesh faces: "at" (a point of the view) picks the face, and the chosen
        // faces with it when it is one of them; without a point or a face there, the chosen faces
        // (the object mode: all). "material": its path (none: the eyedropper's).
        // pickMaterial: the eyedropper takes the material of the face at the point (else the mouse's).
        const Json* at = m.get("at");
        const float fx = at ? float(at->getNumber("x", -1)) : -1.0f, fy = at ? float(at->getNumber("y", -1)) : -1.0f;
        uint32_t record = ~0u, face = ~0u;
        if (at || type == "editor.level.pickMaterial")  // (putting one on without a point: the chosen faces, not the mouse's)
            levelFaceAt(fx, fy, record, face);
        if (type == "editor.level.pickMaterial") {
            const auto mesh = record != ~0u ? levelMeshOf(record) : nullptr;
            if (!mesh || face >= mesh->faces.size())
                return notice("No level mesh face under the mouse", "warning");
            const uint32_t slot = mesh->faces[face].material;
            pickedMaterial_ = slot < mesh->materials.size() ? mesh->materials[slot] : "";
            if (ui_) {
                JsonWriter w;
                w.beginObject().value("type", "editor.level.picked").value("material", pickedMaterial_).endObject();
                ui_->send(w.str());
            }
            return notice(("Picked " + (pickedMaterial_.empty() ? std::string("the default material") : pickedMaterial_)).c_str());
        }
        const std::string material = m.get("material") ? m.getString("material") : pickedMaterial_;
        uint32_t target;
        ElementSelection chosen;
        const bool haveTarget = levelTarget(target, chosen);
        if (record != ~0u && (!haveTarget || record != target || chosen.mode != 3 ||
                              std::find(chosen.items.begin(), chosen.items.end(), uint64_t(face)) == chosen.items.end())) {
            target = record;  // (just the face there)
            chosen = {};
            chosen.mode = 3;
            chosen.items = {face};
        } else if (!haveTarget) {
            return notice("No level mesh face there", "warning");
        }
        Json args = Json::makeObject();
        args.set("path", Json::makeString(material));
        if (hasProject_)
            addMaterialSize(project_, args);
        applyLevelOp(target, chosen, "material", std::move(args));
    } else if (type == "editor.entity.material" || type == "editor.asset.assign") {
        // A material asset onto a mesh (by id, or the one under a point of the view). On a level
        // mesh: the face there (and the chosen faces with it).
        if (const Json* at = m.get("at"); at && at->isObject() && type == "editor.asset.assign") {
            uint32_t record = ~0u, face = ~0u;
            if (levelFaceAt(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), record, face) && record != ~0u) {
                Json level = m;
                level.set("type", Json::makeString("editor.level.material"));
                return handle(level, camera);
            }
        }
        uint32_t target = index;
        if (const Json* at = m.get("at"); at && at->isObject())
            placementPoint(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), camera, &target);
        if (!editable(target) || records_[target].slot == ~0u)
            return notice("A material goes on a mesh (none there)", "error");
        State s = capture(target);
        s.material = m.getString("material");
        execute({{target, s}});
        noteDisplacement(target, s.material);
        select(target);
        sendState(false);
    } else if (type == "editor.material.set") {
        // A material asset's factors (and textures) written, and shown live where it is used.
        // A library's is written only (no scene uses it: projects use their copies).
        const std::string path = m.getString("path");
        const bool library = asset_library::isLibrary(path);
        if (library && asset_library::readOnly(path))
            return notice("Falcon's materials are read-only: use one (it is copied into the project) and change the copy", "error");
        if (!library && !hasProject_)
            return notice("Materials are edited in a project", "error");
        const std::string file = library ? asset_library::absolute(path) : project_.path(path);
        if (file.empty())
            return notice(("Not in the library: " + path).c_str(), "error");
        MaterialAsset mat;
        std::string error;
        if (!readMaterialAsset(file, mat, error))
            return notice(error.c_str(), "error");
        if (const Json* v = m.get("values")) {
            const auto floats = [&](const char* key, float* out, size_t n) {
                if (const Json* a = v->get(key))
                    for (size_t k = 0; k < n && k < a->array.size(); ++k)
                        out[k] = float(a->array[k].number);
            };
            floats("baseColor", mat.baseColor, 4);
            floats("emissive", mat.emissive, 3);
            floats("size", mat.size, 2);
            mat.size[0] = std::max(mat.size[0], 0.0f), mat.size[1] = std::max(mat.size[1], 0.0f);
            mat.metallic = float(v->getNumber("metallic", mat.metallic));
            mat.roughness = float(v->getNumber("roughness", mat.roughness));
            mat.roughnessMin = float(std::clamp(v->getNumber("roughnessMin", mat.roughnessMin), 0.0, 1.0));
            mat.emissiveStrength = float(v->getNumber("emissiveStrength", mat.emissiveStrength));
            mat.normalScale = float(v->getNumber("normalScale", mat.normalScale));
            mat.occlusionStrength = float(v->getNumber("occlusionStrength", mat.occlusionStrength));
            mat.alphaCutoff = float(v->getNumber("alphaCutoff", mat.alphaCutoff));
            mat.refraction = float(std::clamp(v->getNumber("refraction", mat.refraction), 0.0, 1.0));
            mat.ior = float(std::clamp(v->getNumber("ior", mat.ior), 1.0, 3.0));
            mat.shader = v->getString("shader", mat.shader.c_str());  // (R4: "" for none)
            mat.vertexBound = float(std::clamp(v->getNumber("vertexBound", mat.vertexBound), 0.0, 1000.0));
            mat.vertexDistance = float(std::clamp(v->getNumber("vertexDistance", mat.vertexDistance), 0.0, 100000.0));
            floats("params", mat.params, 4);
            mat.alpha = v->getString("alpha", mat.alpha.c_str());
            mat.doubleSided = v->getBool("doubleSided", mat.doubleSided);
            mat.name = v->getString("name", mat.name.c_str());
            if (const Json* t = v->get("textures")) {
                for (int k = 0; k < 4; ++k)
                    if (const Json* p = t->get(MaterialAsset::kTextureSlots[k]))
                        mat.textures[k] = p->str;
                if (const Json* p = t->get("height"))
                    mat.height = p->str;
            }
            if (const double d = v->getNumber("displacement", mat.displacement); std::isfinite(d))
                mat.displacement = float(d);
            mat.heightCenter = float(std::clamp(v->getNumber("heightCenter", mat.heightCenter), 0.0, 1.0));
            mat.displacementDetail = float(std::clamp(v->getNumber("displacementDetail", mat.displacementDetail), 0.0, 10.0));
            if (const double d = v->getNumber("parallax", mat.parallax); std::isfinite(d))
                mat.parallax = float(std::max(d, 0.0));
            if (const Json* a = v->get("acoustic"))
                readAcousticJson(*a, mat.acousticPreset, mat.acoustic);
            if (const Json* su = v->get("surface"))
                readSurfaceJson(*su, mat.surfacePreset, mat.surface);
        }
        if (library) {  // its textures are the library's: a project's image is copied beside it
            const auto u8 = [](const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); };
            const std::string folder = path.substr(0, path.rfind('/'));
            for (std::string* t : {&mat.textures[0], &mat.textures[1], &mat.textures[2], &mat.textures[3], &mat.height}) {
                if (t->empty() || asset_library::isLibrary(*t))
                    continue;
                if (!hasProject_)
                    return notice(("A library material's textures are the library's: " + *t + " isn't").c_str(), "error");
                const std::string to = folder + "/" + t->substr(t->rfind('/') + 1);
                std::error_code ec;
                if (!std::filesystem::exists(u8(asset_library::absolute(to)), ec))
                    std::filesystem::copy_file(u8(project_.path(*t)), u8(asset_library::absolute(to)), ec);
                if (ec)
                    return notice(("Could not copy " + *t + " into the library: " + ec.message()).c_str(), "error");
                *t = to;
            }
        }
        if (!writeMaterialAsset(file, mat, error))
            return notice(error.c_str(), "error");
        if (library) {
            assets_.rescanNow();
            return;
        }
        if (materials_)
            materials_->reload(path);
        if (audio_)
            audio_->materialChanged(path);  // (its acoustics: heard at once)
        sceneChanged();
    } else if (type == "editor.prefab.create") {
        // The entity and everything below it, saved as a prefab; it becomes an instance of it.
        // Prefab instances below it nest in it (their prefab and their overrides).
        if (!hasProject_)
            return notice("Prefabs are made in a project", "error");
        if (!editable(index) || records_[index].prefabRoot != ~0u)
            return notice(index < records_.size() && records_[index].prefabRoot != ~0u ? "It is a prefab instance already"
                                                                                       : "Select the entity to save as a prefab", "error");
        std::vector<uint32_t> members = subtree(index);
        for (const uint32_t i : members)
            if (records_[i].kind == EntityKind::Bone && capture(i).alive)
                return notice("A character with its bones expanded cannot go into a prefab yet: Collapse Bones first", "error");
        std::erase_if(members, [&](uint32_t i) { return records_[i].kind == EntityKind::Bone; });  // (collapsed ones)
        for (const uint32_t i : members)
            if (records_[i].uid == 0)
                return notice("Only entities made in the editor can go into a prefab (not the scene's cooked ones)", "error");
            else if (records_[i].kind == EntityKind::Water)
                return notice("Water can't go into a prefab yet", "error");
        Prefab def;
        def.name = defaultName(index);
        std::unordered_map<uint32_t, std::string> idOf;
        const auto pathId = [&](uint32_t i) -> std::string {  // a member's id here (a nested instance's: by path)
            if (const auto it = idOf.find(i); it != idOf.end())
                return it->second;
            const uint32_t r = i < records_.size() ? records_[i].prefabRoot : ~0u;
            const auto root = r != ~0u ? idOf.find(r) : idOf.end();
            return root != idOf.end() ? root->second + "/" + records_[i].prefabChild : std::string();
        };
        for (const uint32_t i : members) {
            const Record& r = records_[i];
            if (!capture(i).alive || (r.prefabRoot != ~0u && r.prefabRoot != i))
                continue;  // (a nested instance's members: written with it)
            PrefabEntity p = prefabEntityOf(i);
            p.id = std::to_string(idOf.size());
            p.parent = i == index ? "" : pathId(parentOf(i));
            if (i != index)
                localOf(i, p.local);
            idOf[i] = p.id;
            def.entities.push_back(std::move(p));
        }
        std::string folder = m.getString("folder", "Assets");
        if (folder != "Assets" && !folder.starts_with("Assets/"))
            folder = "Assets";
        std::error_code dirError;
        std::filesystem::create_directories(project_.path(folder), dirError);
        std::string stem = fileSafeName(def.name);
        if (stem.empty())
            stem = "Prefab";
        std::string relative = folder + "/" + stem + ".fprefab";
        for (uint32_t n = 2; std::filesystem::exists(project_.path(relative)); ++n)
            relative = folder + "/" + stem + " (" + std::to_string(n) + ").fprefab";
        std::string error;
        if (!writePrefab(project_.path(relative), def, error))
            return notice(error.c_str(), "error");
        prefabs_.clear();
        // Every member is the new instance's (a nested instance's by path: "<its id>/<theirs>").
        Links links;
        for (const uint32_t i : members) {
            const Record& r = records_[i];
            if (!capture(i).alive)
                continue;
            if (const auto it = idOf.find(i); it != idOf.end())
                links.push_back({i, {i == index ? relative : std::string(), index, it->second}});
            else if (const std::string id = pathId(i); !id.empty() && r.prefabRoot != ~0u)
                links.push_back({i, {std::string(), index, id}});
        }
        changeLinks(links);
        notice(("Saved " + relative + "; " + def.name + " is an instance of it").c_str());
        sendState(true);
    } else if (type == "editor.prefab.apply" || type == "editor.prefab.revert" || type == "editor.prefab.unpack") {
        if (index >= records_.size() || records_[index].prefabRoot == ~0u)
            return notice("Not part of a prefab instance", "error");
        const uint32_t root = records_[index].prefabRoot;
        const std::string path = records_[root].prefab;
        const Prefab* def = prefabDef(path);
        if (type == "editor.prefab.unpack") {
            // One level: what nests in it stays an instance of its own prefab. Undo links it again.
            Links links;
            std::unordered_map<std::string, uint32_t> byChild;
            for (const uint32_t i : instanceMembers(root))
                byChild[records_[i].prefabChild] = i;
            for (const uint32_t i : instanceMembers(root)) {
                const std::string& child = records_[i].prefabChild;
                const PrefabEntity* p = def ? def->find(child) : nullptr;
                Link l;
                if (p && !p->nestedIn.empty()) {
                    const std::string head = child.substr(0, child.find('/'));
                    if (const auto nested = byChild.find(head); nested != byChild.end()) {
                        l.root = nested->second;
                        l.child = p->nestedId;
                        if (i == nested->second)
                            l.prefab = p->nestedIn;
                    }
                }
                links.push_back({i, l});
            }
            changeLinks(links);
            notice((defaultName(root) + " is no longer linked to " + path).c_str());
            return sendState(true);
        }
        if (!def)
            return notice(("Cannot read the prefab " + path).c_str(), "error");
        std::vector<std::pair<uint32_t, State>> after;
        if (type == "editor.prefab.revert") {
            followPrefab(root, *def, {}, after);
            reshapeInstance(root, *def, after);
            if (!after.empty())
                execute(after);
            notice(("Reverted " + defaultName(root) + " to " + path).c_str());
            return sendState(true);
        }
        // Apply: the prefab file becomes this instance: its entities' values, the ones added below
        // it (prefab instances among them nest) and removed, and changes to nested prefabs' entities
        // as overrides of their nesting. The other instances follow, keeping their own overrides.
        const Prefab old = *def;
        Prefab raw;
        std::string error;
        if (!readPrefabFile(project_.path(path), raw, error))
            return notice(error.c_str(), "error");
        std::unordered_map<std::string, uint32_t> memberOf;
        for (const uint32_t i : instanceMembers(root))
            if (capture(i).alive)
                memberOf[records_[i].prefabChild] = i;
        std::unordered_set<std::string> dropped;
        std::vector<PrefabEntity> kept;
        for (PrefabEntity& p : raw.entities) {
            const auto member = memberOf.find(p.id);
            if (dropped.count(p.parent) || member == memberOf.end()) {
                dropped.insert(p.id);  // (deleted in the instance: gone from the prefab, with what hangs under it)
                continue;
            }
            const uint32_t i = member->second;
            const State st = capture(i);
            if (p.kind == "prefab") {
                // A nesting: its entities' changes against their own prefab, as its overrides.
                p.overrides.clear();
                if (const Prefab* inner = prefabDef(p.prefab))
                    for (const PrefabEntity& e : inner->entities) {
                        const bool innerRoot = &e == &inner->entities[0];
                        const auto m2 = memberOf.find(innerRoot ? p.id : p.id + "/" + e.id);
                        PrefabOverride o;
                        if (m2 != memberOf.end()) {
                            o = overrideOf(m2->second, e, !innerRoot);
                        } else if (!innerRoot) {
                            o.removed = true;  // (deleted in the instance: removed in this nesting)
                        }
                        o.id = e.id;
                        if (!o.empty())
                            p.overrides.push_back(std::move(o));
                    }
            } else {
                const PrefabEntity now = prefabEntityOf(i, i != root);
                p.kind = now.kind;
                p.mesh = now.mesh;
                p.material = now.material;
                p.light = now.light;
                p.camera = now.camera;
                p.body = now.body;
                p.collider = now.collider;
                p.audioSource = now.audioSource;
                p.audioListener = now.audioListener;
                p.acoustics = now.acoustics;
                p.vfx = now.vfx;
                p.scripts = now.scripts;
                p.levelMesh = now.levelMesh;
            }
            if (i != root) {
                p.name = st.name;
                p.hidden = st.hidden;
                localOf(i, p.local);
            }
            kept.push_back(std::move(p));
        }
        raw.entities = std::move(kept);
        // Added below the instance (under one of its entities): new entities of the prefab.
        uint32_t nextId = 0;
        for (const PrefabEntity& p : raw.entities)
            if (!p.id.empty() && p.id.find_first_not_of("0123456789") == std::string::npos)
                nextId = std::max(nextId, uint32_t(std::stoul(p.id)) + 1);
        std::unordered_map<uint32_t, std::string> idOf;
        for (const auto& [id, i] : memberOf)
            idOf[i] = id;
        Links links;
        uint32_t added = 0;
        for (const uint32_t i : subtree(root)) {
            const Record& r = records_[i];
            if (!capture(i).alive || r.kind == EntityKind::Bone || r.uid == 0 || r.prefabRoot == root ||
                (r.prefabRoot != ~0u && r.prefabRoot != i))
                continue;  // (members; a newly nested instance's members come with it)
            const auto up = idOf.find(parentOf(i));
            if (up == idOf.end())
                continue;
            PrefabEntity p = prefabEntityOf(i);
            p.id = std::to_string(nextId++);
            p.parent = up->second;
            localOf(i, p.local);
            idOf[i] = p.id;
            if (r.prefabRoot == i)  // another prefab's instance: nested from now on, its members by path
                for (const uint32_t j : instanceMembers(i))
                    links.push_back({j, {std::string(), root, j == i ? p.id : p.id + "/" + records_[j].prefabChild}});
            else
                links.push_back({i, {std::string(), root, p.id}});
            raw.entities.push_back(std::move(p));
            ++added;
        }
        if (!writePrefab(project_.path(path), raw, error))
            return notice(error.c_str(), "error");
        prefabs_.clear();
        const Prefab* next = prefabDef(path);
        if (!next)
            return notice(("Cannot read the prefab " + path + " again").c_str(), "error");
        if (!links.empty())
            changeLinks(links);
        const Prefab nextDef = *next;
        uint32_t others = 0;
        for (uint32_t r = 0; r < records_.size(); ++r)
            if (r != root && records_[r].prefabRoot == r && records_[r].prefab == path && capture(r).alive) {
                followPrefab(r, nextDef, overridesOf(r, old), after);
                reshapeInstance(r, nextDef, after);
                ++others;
            }
        if (!after.empty())
            execute(after);
        notice(("Applied to " + path + (added ? " (" + std::to_string(added) + " added)" : std::string()) +
                (others ? " (" + std::to_string(others) + " other instance" + (others == 1 ? "" : "s") + " follow)" : std::string()))
                   .c_str());
        sendState(true);
    } else if (type == "editor.asset.place") {
        // A mesh or a whole model where the page dropped it (or at the view's pivot).
        const uint32_t pid = idOf("parent");
        const uint32_t parent = pid != 0 && canHold(pid - 1) ? pid - 1 : ~0u;
        const Json* at = m.get("at");
        Json mouseAt = Json::makeObject();  // ("atMouse": where the mouse is over the view)
        if (float fx, fy; m.getBool("atMouse") && view_ && view_->mouseFraction(fx, fy)) {
            mouseAt.set("x", Json::makeNumber(fx));
            mouseAt.set("y", Json::makeNumber(fy));
            at = &mouseAt;
        }
        const float3 point = parent != ~0u ? float3{capture(parent).world[0][3], capture(parent).world[1][3], capture(parent).world[2][3]}
                             : at && at->isObject() ? placementPoint(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), camera)
                             : camera_ ? camera_->pivot() : camera.position + camera.forward * 4.0f;
        std::vector<std::pair<uint32_t, State>> after;
        const auto add = [&](EntityKind kind, const std::string& mesh, const float world[3][4], const std::string& name, uint32_t under,
                             const std::string& material = std::string()) {
            const uint32_t created = spawn(kind, mesh, world, name, material);
            if (created == ~0u)
                return created;
            State s;
            std::memcpy(s.world, world, sizeof(s.world));
            s.parent = under;
            s.alive = true;
            s.name = defaultName(created);
            after.emplace_back(created, std::move(s));
            return created;
        };
        if (const std::string mesh = m.getString("mesh"); !mesh.empty()) {
            const int32_t index = pak_->findMesh(mesh);
            float3 c, h;
            if (index < 0 || !meshBoundsOf(uint32_t(index), c, h))
                return notice(("No mesh " + mesh + " in this scene (after an import: Reload the scene)").c_str(), "error");
            // Standing on the point: the bounds' bottom center there.
            const float world[3][4] = {{1, 0, 0, point.x - c.x}, {0, 1, 0, point.y - (c.y - h.y)}, {0, 0, 1, point.z - c.z}};
            add(EntityKind::Primitive, mesh, world, "", parent);
        } else if (const std::string sound = m.getString("sound"); !sound.empty()) {
            // A sound or clip: an empty entity with an Audio Source playing it, a metre above the point.
            const float world[3][4] = {{1, 0, 0, point.x}, {0, 1, 0, point.y + 1.0f}, {0, 0, 1, point.z}};
            if (add(EntityKind::Empty, "", world, std::filesystem::path(sound).stem().string(), parent) == ~0u)
                return;
            after.back().second.audioSource = defaultAudioSource();
            after.back().second.audioSource->sound = soundId(sound);
        } else if (const std::string model = m.getString("model"); !model.empty()) {
            const ScenePak::Library* lib = pak_->library(model);
            if (!lib || lib->nodes.empty())
                return notice((model + " is not in this scene yet: Reload the scene (it re-cooks with the project's models)").c_str(), "error");
            // The model's bounds over its nodes; it stands on the point like a mesh.
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const ScenePak::LibraryNode& n : lib->nodes) {
                float3 c, h;
                if (!meshBoundsOf(n.mesh, c, h))
                    continue;
                for (int corner = 0; corner < 8; ++corner) {
                    const float p[3] = {corner & 1 ? c.x + h.x : c.x - h.x, corner & 2 ? c.y + h.y : c.y - h.y, corner & 4 ? c.z + h.z : c.z - h.z};
                    for (int r = 0; r < 3; ++r) {
                        const float v = n.world[r][0] * p[0] + n.world[r][1] * p[1] + n.world[r][2] * p[2] + n.world[r][3];
                        lo[r] = std::min(lo[r], v);
                        hi[r] = std::max(hi[r], v);
                    }
                }
            }
            if (lo[0] > hi[0])
                return notice((model + " has no meshes").c_str(), "error");
            const float rootWorld[3][4] = {{1, 0, 0, point.x - (lo[0] + hi[0]) * 0.5f}, {0, 1, 0, point.y - lo[1]},
                                           {0, 0, 1, point.z - (lo[2] + hi[2]) * 0.5f}};
            const std::string stem = std::filesystem::path(model).stem().string();
            const uint32_t root = add(EntityKind::Empty, "", rootWorld, stem, parent);
            if (root == ~0u)
                return;
            for (uint32_t k = 0; k < lib->nodes.size(); ++k) {
                const ScenePak::LibraryNode& n = lib->nodes[k];
                float world[3][4];
                affineMultiply(rootWorld, n.world, world);
                const uint32_t created = add(EntityKind::Primitive, pak_->meshName(n.mesh), world, n.name, root);
                if (created == ~0u)
                    break;
                records_[created].model = model;
                records_[created].modelNode = int32_t(k);
            }
            // Its cameras (the first one the main camera, when the scene has none): Play looks
            // through it, and it follows the model's animation.
            float mainWorld[3][4];
            CameraComponent mainCam;
            bool haveMain = scene_->mainCamera(mainWorld, mainCam);
            for (uint32_t k = 0; k < lib->cameras.size(); ++k) {
                const ScenePak::LibraryCamera& c = lib->cameras[k];
                float world[3][4];
                affineMultiply(rootWorld, c.world, world);
                const uint32_t created = add(EntityKind::Camera, "", world, c.name, root);
                if (created == ~0u)
                    break;
                CameraComponent cam = {};
                cam.fov = c.yfov * (180.0f / kPi);
                cam.nearClip = 0.05f;
                cam.flags = haveMain ? 0u : 1u;
                haveMain = true;
                after.back().second.camera = cam;
                records_[created].model = model;
                records_[created].modelCamera = int32_t(k);
            }
        } else if (const std::string path = m.getString("prefab"); !path.empty()) {
            float place[3][4] = {{1, 0, 0, point.x}, {0, 1, 0, point.y}, {0, 0, 1, point.z}};
            // A door or window prefab (an opening at its root) dropped on a wall: into the wall there,
            // on its path.
            uint32_t wallIndex = ~0u;
            State into;
            const Prefab* def = prefabDef(path);
            if (def && !def->entities.empty() && def->entities[0].level.opening && parent == ~0u && at && at->isObject()) {
                float3 o = camera.position, d = camera.forward, hitAt, normal;
                if (view_)
                    view_->rayAt(float(at->getNumber("x", 0.5)), float(at->getNumber("y", 0.5)), o, d);
                uint32_t hit = ~0u;
                if (!surfaceHit(o, d, {}, hitAt, normal, &hit))
                    hit = ~0u;
                for (uint32_t k = hit; k != ~0u && wallIndex == ~0u; k = parentOf(k))
                    if (capture(k).wall)
                        wallIndex = k;
                into.parent = wallIndex;
                into.opening = def->entities[0].level.opening;
                std::memcpy(into.world, place, sizeof(place));
                into.world[0][3] = hitAt.x, into.world[1][3] = hitAt.y, into.world[2][3] = hitAt.z;  // (where it hit the wall)
                if (wallIndex != ~0u && frameOpening(into, true, false))  // (at the prefab's own sill)
                    std::memcpy(place, into.world, sizeof(place));
                else
                    wallIndex = ~0u;
            }
            std::string error;
            const uint32_t root = spawnPrefab(path, place, wallIndex == ~0u, wallIndex != ~0u ? wallIndex : parent, after, error);
            if (root == ~0u)
                return notice(error.c_str(), "error");
            for (auto& [i, s] : after)
                if (i == root && s.opening && wallIndex != ~0u)
                    s.opening->distance = into.opening->distance;
        } else {
            return notice("Place: no mesh, model or prefab given", "error");
        }
        if (after.empty())
            return;
        execute(after);
        select(after.front().first);
        sendState(true);
    } else if (type == "editor.entity.create") {
        const std::string kindName = m.getString("kind", "primitive");
        const EntityKind kind = entityKindFromName(kindName);
        const uint32_t pid = idOf("parent");
        const uint32_t parent = pid != 0 && canHold(pid - 1) ? pid - 1 : ~0u;
        // Roots at the camera's pivot; children at their parent (local identity, as in Unity).
        State s;
        if (parent != ~0u) {
            std::memcpy(s.world, capture(parent).world, sizeof(s.world));
        } else {
            const float3 at = camera_ ? camera_->pivot() : camera.position + camera.forward * 4.0f;
            const std::string light = kind == EntityKind::Light ? m.getString("light", "point") : "";
            float rot[3] = {0, 0, 0};
            if (light == "spot")
                rot[0] = -90.0f;  // pointing down
            else if (light == "directional")
                rot[0] = -55.0f, rot[1] = 30.0f;  // an afternoon sun
            const float pos[3] = {at.x, at.y, at.z}, scale[3] = {1, 1, 1};
            compose(pos, rot, scale, s.world);
            if (kind == EntityKind::Camera) {  // a camera starts as the scene view's
                const float3 right = camera.right, up = camera.up, back = camera.forward * -1.0f;
                const float view[3][4] = {{right.x, up.x, back.x, camera.position.x},
                                          {right.y, up.y, back.y, camera.position.y},
                                          {right.z, up.z, back.z, camera.position.z}};
                std::memcpy(s.world, view, sizeof(s.world));
            }
        }
        const std::string sub = kind == EntityKind::Primitive ? m.getString("mesh", m.getString("primitive", "cube").c_str())
                                : kind == EntityKind::Light   ? m.getString("light", "point")
                                : kind == EntityKind::Water   ? m.getString("water", m.getString("primitive", "lake").c_str())
                                : kind == EntityKind::Level   ? m.getString("shape", "box")
                                                              : "";
        const uint32_t created = spawn(kind, sub, s.world, m.getString("name"));
        if (created == ~0u)
            return;
        s.parent = parent;
        s.alive = true;
        s.name = defaultName(created);
        if (const Light* l = entityOf(created).try_get<Light>())
            s.light = *l;
        if (const CameraComponent* c = entityOf(created).try_get<CameraComponent>())
            s.camera = *c;
        if (const LevelMesh* lm = entityOf(created).try_get<LevelMesh>())
            s.levelMesh = lm->mesh;
        if (const LevelMask* mask = entityOf(created).try_get<LevelMask>())
            s.levelMask = mask->rgba;
        // Create > Effects: an effect ready to show (a soft dot rising and fading) or a decal.
        if (const std::string vfx = m.getString("vfx"); vfx == "effect") {
            auto& fx = *static_cast<ParticleEffect*>(s.vfx.add(VfxIndexOf<ParticleEffect>::value));
            fx.lifetime = ParticleFloat::range(1.0f, 1.6f);
            fx.flags |= kEffectColor | kEffectSize;
            fx.gradient.mode = ParticleGradient::GradientMode;
            fx.gradient.gradientA.alphas = 3;
            fx.gradient.gradientA.alpha[0] = {0.0f, 0.0f};
            fx.gradient.gradientA.alpha[1] = {0.15f, 1.0f};
            fx.gradient.gradientA.alpha[2] = {1.0f, 0.0f};
            fx.scale = ParticleFloat::curve(Curve::line(0.6f, 1.4f));
            auto& cone = *static_cast<ParticleConeEmitter*>(s.vfx.add(VfxIndexOf<ParticleConeEmitter>::value));
            cone.emission.burst = 0.0f;
            cone.emission.rate = 20.0f;
            cone.radius = 0.2f;
            cone.velocity = ParticleFloat::range(0.8f, 1.5f);
            s.vfx.add(VfxIndexOf<ParticleSpriteRenderer>::value);
        } else if (vfx == "decal") {
            s.vfx.add(VfxIndexOf<Decal>::value);
        }
        execute({{created, s}});
        select(created);
        sendState(true);
    } else if (type == "editor.entity.duplicate") {
        if (!editable(index))
            return;
        if (records_[index].kind == EntityKind::Mesh)
            return notice("Duplicate: cooked meshes cannot be duplicated (they have no spare instance slots)", "error");
        // Unity's naming for the copy: "Name (1)", "Name (2)", ...
        std::string base = defaultName(index);
        if (const size_t open = base.rfind(" ("); open != std::string::npos && base.back() == ')')
            base.resize(open);
        uint32_t n = 1;
        for (uint32_t i = 0; i < records_.size(); ++i)
            if (const std::string v = defaultName(i); v.starts_with(base + " ("))
                n = std::max(n, uint32_t(std::atoi(v.c_str() + base.size() + 2)) + 1);
        // The entity and everything below it; cooked meshes inside are left out (their children
        // move up to the nearest copied ancestor).
        std::unordered_map<uint32_t, uint32_t> copyOf;
        std::vector<std::pair<uint32_t, State>> after;
        uint32_t skipped = 0;
        for (const uint32_t i : subtree(index)) {
            const Record& r = records_[i];
            State s = capture(i);
            if (!s.alive)
                continue;
            if (r.kind == EntityKind::Mesh) {
                ++skipped;
                continue;
            }
            if (r.kind == EntityKind::Bone)
                continue;  // (the copy's own, below)
            const std::string name = i == index ? base + " (" + std::to_string(n) + ")" : s.name;
            const uint32_t copy = spawn(r.kind, r.primitive, s.world, name, s.material);
            if (copy == ~0u)
                break;
            copyOf[i] = copy;
            if (s.camera)
                s.camera->flags &= ~1u;  // one main camera: copies are not
            if (i != index) {  // under the copy of its nearest copied ancestor (the root's copy at worst)
                uint32_t p = parentOf(i);
                while (!copyOf.count(p))
                    p = parentOf(p);
                s.parent = copyOf[p];
                s.hasLocal = false;
            }
            s.name = name;
            after.emplace_back(copy, std::move(s));
        }
        if (after.empty())
            return;
        execute(after);
        // Expanded bones: the copy's own, and what hung under the original's under the copy's (by
        // bone name), in the same undo step.
        for (const uint32_t i : subtree(index))
            if (const auto copy = copyOf.find(i); copy != copyOf.end() && bonesExpanded(i) && bones_ && animation_ && skinning_)
                if (const BoneEntities::Expanded* x = bones_->expand(*scene_, entityOf(copy->second), *animation_, *skinning_)) {
                    const std::vector<uint32_t> theirs = addBoneRecords(copy->second, *x);
                    for (const uint32_t b : theirs)  // (shown by the step below, so undo hides them)
                        entityOf(b).disable();
                    std::vector<std::pair<uint32_t, State>> more;
                    for (const uint32_t b : theirs) {
                        State s = capture(b);
                        s.alive = true;
                        more.emplace_back(b, std::move(s));
                    }
                    for (const auto& [from, to] : copyOf)
                        if (const uint32_t p = parentOf(from); p != ~0u && records_[p].kind == EntityKind::Bone && records_[p].boneRoot == i)
                            for (const uint32_t b : theirs)
                                if (records_[b].bone == records_[p].bone) {
                                    State s = capture(to);
                                    s.parent = b;
                                    std::memcpy(s.world, capture(from).world, sizeof(s.world));  // (where the original's is)
                                    s.hasLocal = false;
                                    more.emplace_back(to, std::move(s));
                                }
                    execute(more);
                    for (Change& ch : undo_.back())  // one step with the copy
                        undo_[undo_.size() - 2].push_back(std::move(ch));
                    undo_.pop_back();
                    --commands_;
                }
        select(after.front().first);
        if (skipped)
            notice((std::to_string(skipped) + " cooked mesh(es) below it were not copied (no spare instance slots)").c_str());
        sendState(true);
    } else if (type == "editor.frame") {
        select(index);
        if (view_)
            view_->frameSelection();
        sendState(false);
    } else if (type == "editor.tool.set") {
        if (!view_)
            return;
        SceneToolState& t = view_->tools();
        const std::string tool = m.getString("tool");
        if (tool == "view") t.tool = SceneTool::View;
        else if (tool == "move") t.tool = SceneTool::Move;
        else if (tool == "rotate") t.tool = SceneTool::Rotate;
        else if (tool == "scale") t.tool = SceneTool::Scale;
        else if (tool == "brush") t.tool = SceneTool::Brush;
        else if (tool == "paint") t.tool = SceneTool::Paint;
        else if (tool == "spline") t.tool = SceneTool::Spline;
        else if (tool == "measure") t.tool = SceneTool::Measure;
        t.paintMode = std::min(uint32_t(m.getNumber("paintMode", t.paintMode)), 7u);
        t.paintDecal = m.getString("paintDecal", t.paintDecal.c_str());
        t.paintDamage = std::min(uint32_t(m.getNumber("paintDamage", t.paintDamage)), 2u);
        t.paintRubble = m.getBool("paintRubble", t.paintRubble);
        damageSettings(m);
        t.splineProfile = m.getString("splineProfile", t.splineProfile.c_str());
        if (const Json* v = m.get("scatterPrefabs"); v && v->type == Json::Type::Array) {
            t.scatterPrefabs.clear();
            for (const Json& p : v->array)
                if (!p.str.empty())
                    t.scatterPrefabs.push_back(p.str);
        }
        t.scatterDensity = std::clamp(float(m.getNumber("scatterDensity", t.scatterDensity)), 0.0f, 100.0f);
        t.scatterSpacing = std::max(float(m.getNumber("scatterSpacing", t.scatterSpacing)), 0.0f);
        t.scatterSlope = std::clamp(float(m.getNumber("scatterSlope", t.scatterSlope)), 0.0f, 90.0f);
        t.scatterAlign = m.getBool("scatterAlign", t.scatterAlign);
        t.showNav = m.getBool("showNav", t.showNav);
        if (const Json* v = m.get("scatterScale"); v && v->array.size() == 2)
            t.scatterScale[0] = float(v->array[0].number), t.scatterScale[1] = float(v->array[1].number);
        t.paintCap = m.getString("paintCap", t.paintCap.c_str());
        t.paintRadius = std::clamp(float(m.getNumber("paintRadius", t.paintRadius)), 0.01f, 50.0f);
        t.paintStrength = std::clamp(float(m.getNumber("paintStrength", t.paintStrength)), 0.0f, 1.0f);
        t.paintColor = uint32_t(m.getNumber("paintColor", double(t.paintColor)));
        t.local = m.getBool("local", t.local);
        t.pivotCenter = m.getBool("pivotCenter", t.pivotCenter);
        t.grid = m.getBool("grid", t.grid);
        t.gameView = m.getBool("gameView", t.gameView);
        t.snapMove = std::max(float(m.getNumber("snapMove", t.snapMove)), 0.0f);
        t.snapRotate = std::max(float(m.getNumber("snapRotate", t.snapRotate)), 0.0f);
        t.snapScale = std::max(float(m.getNumber("snapScale", t.snapScale)), 0.0f);
        t.snapAbsolute = m.getBool("snapAbsolute", t.snapAbsolute);
        if (const std::string element = m.getString("element"); !element.empty())
            t.element = element == "vertex" ? ElementMode::Vertex : element == "edge" ? ElementMode::Edge
                        : element == "face" ? ElementMode::Face : ElementMode::Object;
        sendState(false);
    } else if (type == "editor.prefs.set") {
        if (m.getString("key") == "keymap" && m.get("value")) {  // (the commands' keys now, prefs or not)
            keymap_ = m.get("value")->dump(0);
            parseCommandKeys();
        }
        if (prefs_ && m.get("value")) {
            prefs_->section("page").set(m.getString("key"), *m.get("value"));
            if (m.getString("key") == "keymap")
                keymap_ = m.get("value")->dump(0);
            prefs_->touch();
        } else if (m.get("value")) {  // (--no-prefs, as tests run: the page keeps a script's too)
            JsonWriter w;
            w.beginObject().value("type", "editor.prefs.changed").value("key", m.getString("key")).raw("value", m.get("value")->dump(0)).endObject();
            ui_->send(w.str());
        }
    } else if (type == "editor.settings.set") {
        if (const Json* v = m.get("value"))
            setSetting(m.getString("key"), *v);
        sendState(false);
    } else if (type == "editor.pause") {
        if (!playing_)
            return;
        paused_ = !paused_;
        sendState(false);
    } else if (type == "editor.step") {
        if (!playing_) {
            handle(Json::parse(R"({"type":"editor.play"})"), camera);
            paused_ = true;
        }
        ++steps_;
        sendState(false);
    } else if (type == "editor.undo" || type == "editor.redo") {
        auto& from = type == "editor.undo" ? undo_ : redo_;
        auto& to = type == "editor.undo" ? redo_ : undo_;
        if (from.empty())
            return;
        if (terrain_ && terrain_->generating())
            return notice("Terrain generation is running: stop it first (its result is one undo step)", "warning");
        Command c = std::move(from.back());
        from.pop_back();
        if (type == "editor.undo") {
            for (auto it = c.rbegin(); it != c.rend(); ++it)
                if (it->links)
                    setLinks(it->linksBefore);
                else if (it->terrainEdit != ~0u)
                    terrain_->undo(it->terrainEdit);
                else
                    apply(it->index, it->before);
        } else {
            for (const Change& ch : c)
                if (ch.links)
                    setLinks(ch.linksAfter);
                else if (ch.terrainEdit != ~0u)
                    terrain_->redo(ch.terrainEdit);
                else
                    apply(ch.index, ch.after);
        }
        dirty_ = true;
        if (selected_ != ~0u && !capture(selected_).alive)
            select(~0u);
        to.push_back(std::move(c));
        sceneChanged();
        sendState(true);
    } else if (type == "editor.play") {
        if (playing_ || pendingPlay_)
            return;
        // The scripts were built in the background as they changed: Play starts that build at
        // once. Only a change the build has not caught up with yet (one being built, or saved this
        // moment) has Play wait for it, and a compile error in the sources as they are keeps it
        // from starting. Without the .NET SDK nothing can be built: Play goes on with the build
        // there is, if any.
        if (hasProject_ && gamescripts::exists(project_.dir)) {
            watchScripts(true);
            if (scriptBuild_.running()) {
                pendingPlay_ = true;
                notice("Building the scripts: Play starts when they are ready");
                sendState(false);
                return;
            }
            if (scriptBuildFailed_ && scriptBuilt_ == scriptSources_)
                return notice(("Play: the scripts did not build: " + scriptsError_).c_str(), "error");
        }
        startPlay();
    } else if (type == "editor.stop") {
        if (std::exchange(pendingPlay_, false)) {  // still building: Play called off
            sendState(false);
            return;
        }
        if (!playing_)
            return;
        // The engine ends the game, then restorePlay puts the scene back.
        playing_ = paused_ = false;
        steps_ = 0;
        sendState(false);
    } else if (type == "editor.tools.run") {
        toolRuns_.push_back(m.getString("menu"));
        lastCommand_ = m.getString("menu");
    } else if (type == "editor.tools.option") {
        // A command's setting from the Level window ({"menu", "name", "value"}): editor code's.
        JsonWriter w;
        w.beginObject().value("type", "option").value("menu", m.getString("menu")).value("name", m.getString("name"))
            .value("value", m.getNumber("value", 0.0)).endObject();
        editorEvents_.push_back(w.str());
    } else if (type == "editor.view.mouse") {
        // Where the mouse is over the scene view (fractions; the page's wheels open there), with the
        // asker's tag back.
        float fx = 0.5f, fy = 0.5f;
        const bool inside = view_ && view_->mouseFraction(fx, fy);
        JsonWriter w;
        w.beginObject().value("type", "editor.view.mouse").value("x", double(fx)).value("y", double(fy)).value("inside", inside)
            .value("tag", m.getString("tag")).endObject();
        if (ui_)
            ui_->send(w.str());
    } else if (type == "editor.window.open" || type == "editor.window.close") {
        // An editor window opened or closed in the page: its C# side made or told (editor code).
        JsonWriter w;
        w.beginObject().value("type", type == "editor.window.open" ? "window.open" : "window.close").value("window", m.getString("window")).endObject();
        editorEvents_.push_back(w.str());
    } else if (type == "editor.window.message") {
        // Its page's message (FalconPanel.send: JSON text) for its C# side.
        JsonWriter w;
        w.beginObject().value("type", "window.message").value("window", m.getString("window")).value("data", m.getString("data")).endObject();
        editorEvents_.push_back(w.str());
    } else if (type == "editor.gameui.message") {
        // The game's HTML UI (framed by the page while playing): its message for Ui.TryReceive.
        if (playing_)
            gameUiMessages_.push_back(m.getString("data"));
    } else if (type == "editor.window.show") {
        // Open an editor window (editor code, tests): the page opens it.
        JsonWriter w;
        w.beginObject().value("type", "editor.window.show").value("window", m.getString("window")).endObject();
        if (ui_)
            ui_->send(w.str());
    } else if (type == "editor.window.send") {
        // Its C# side's message (EditorWindow.Send) for its page.
        JsonWriter w;
        w.beginObject().value("type", "editor.window.send").value("window", m.getString("window")).value("data", m.getString("data")).endObject();
        if (ui_)
            ui_->send(w.str());
    } else if (type.starts_with("editor.libraries.")) {
        handleLibraries(type.substr(17), m);
    } else if (type == "editor.steam.upload") {
        // SteamPipe: the built game uploaded by steamcmd, in a console of its own where the player
        // signs in (their password and Steam Guard code typed there, never here).
        const std::string user = m.getString("user");
        const std::string build = buildDir(project_.dir, project_.name);
        const std::string steamcmd = (std::filesystem::path(SDL_GetBasePath() ? SDL_GetBasePath() : "") / "steamcmd" / "steamcmd.exe").string();
        std::string vdf, error;
        const bool userOk = !user.empty() && std::all_of(user.begin(), user.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; });
        if (!hasProject_ || !project_.steamAppId)
            notice("Upload to Steam needs the project's Steam App ID (Project Settings > Steam)", "error");
        else if (!userOk)
            notice("Upload to Steam: give your Steamworks account's name (letters, digits, _)", "error");
        else if (!std::filesystem::exists(std::filesystem::path(build) / (std::filesystem::path(build).filename().string() + platform::executableSuffix())))
            notice("Upload to Steam: build the game first (File > Build Game)", "error");
        else if (!std::filesystem::exists(steamcmd))
            notice("Upload to Steam: steamcmd isn't beside the engine (scripts/fetch-steamworks.ps1, then build)", "error");
        else if (!writeSteamPipe(project_, build, m.getString("description"), m.getString("branch", project_.steamBranch.c_str()), vdf, error))
            notice(error.c_str(), "error");
        else if (!openConsole({steamcmd, "+login", user, "+run_app_build", vdf, "+quit"}))
            notice("Upload to Steam: steamcmd didn't start", "error");
        else
            notice("steamcmd is uploading in its own window: sign in there (password, Steam Guard)");
    } else if (type == "editor.game.build") {
        if (!hasProject_ || !gamescripts::exists(project_.dir))
            notice("Build Game needs a project with scripts", "error");
        else if (gameBuild_.running())
            notice("The game is being built");
        else if (!gameBuild_.start(project_.dir, project_.name))
            notice(gameBuild_.error().c_str(), "error");
        else
            notice("Building the game (NativeAOT): this takes a minute");
    } else if (type == "editor.scripts.build") {
        if (!hasProject_)
            notice("Scripts belong to a project: open one to build its game");
        else
            buildScripts(true);
    } else if (type == "editor.scene.save") {
        if (playing_)
            return notice("Stop playing before saving", "error");
        saveScene();
        sendState(false);
    } else if (type.starts_with("editor.assets.")) {
        if (assets_.handle(type, m))  // the project's scenes changed (renamed, moved, deleted)
            sendState(true);
    } else if (type.starts_with("editor.project.")) {
        // Scenes of the project: another one (new or existing) or the launcher is another
        // process ("save": save this scene first; otherwise its changes are dropped).
        if (!hasProject_)
            return notice("This scene is not in a project (open it with --project, or from the launcher)", "error");
        if (playing_)
            return notice("Stop playing first", "error");
        const bool saveFirst = m.getBool("save");
        std::string error;
        if (type == "editor.project.newScene") {
            std::string relative;
            if (!addScene(project_, m.getString("name"), m.getString("folder", "Scenes"), relative, error, false))
                return notice(error.c_str(), "error");
            openScene(relative, saveFirst);
        } else if (type == "editor.project.openScene") {
            const std::string scene = m.getString("scene");
            if (std::find(project_.scenes.begin(), project_.scenes.end(), scene) == project_.scenes.end())
                return notice(("No scene " + scene + " in the project").c_str(), "error");
            openScene(scene, saveFirst);
        } else if (type == "editor.project.cancelCook") {
            if (cook_)
                cook_->cancel = true;
        } else if (type == "editor.project.setStartup") {
            const std::string scene = m.getString("scene", sceneInProject_.c_str());
            if (std::find(project_.scenes.begin(), project_.scenes.end(), scene) == project_.scenes.end())
                return notice(("No scene " + scene + " in the project").c_str(), "error");
            project_.startupScene = scene;
            if (!saveProject(project_, error))
                return notice(error.c_str(), "error");
            notice(("The project starts with " + scene).c_str());
        } else if (type == "editor.project.openLauncher") {
            switchTo({"--launcher"}, saveFirst);
        }
        sendState(true);
    } else if (type.starts_with("editor.terrain.")) {
        if (!terrain_)
            return;
        std::string message;
        bool error = false;
        terrain_->handle(type, m, message, error);
        if (!message.empty())
            notice(message.c_str(), error ? "error" : "info");
        if (type == "editor.terrain.create" || type == "editor.terrain.delete") {
            dirty_ = true;
            sceneChanged();
        }
        sendState(false);
    } else if (type == "test.exit") {
        exitRequested_ = true;
    } else if (type == "test.view") {  // {"position": [xyz], "target": [xyz]}: place the scene camera
        float p[3], t[3];
        if (camera_ && readVec3(m, "position", p) && readVec3(m, "target", t)) {
            const float3 d = normalize(float3{t[0] - p[0], t[1] - p[1], t[2] - p[2]});
            FlyCamera::Pose pose = camera_->pose();
            pose.position = {p[0], p[1], p[2]};
            pose.pitch = std::asin(std::clamp(d.y, -0.999f, 0.999f));
            pose.yaw = std::atan2(-d.x, -d.z);
            pose.pivotDistance = length(float3{t[0] - p[0], t[1] - p[1], t[2] - p[2]});
            camera_->setPose(pose);
        }
    } else if (type == "editor.pong") {
        lastPong_ = uint32_t(m.getNumber("n"));
    } else if (type == "test.capture") {
        captureRequest_ = m.getString("path");
        captureViewport_ = m.getBool("viewport");
        captureView_ = m.getString("view");
        captureWidth_ = 0;
    } else if (type == "editor.probed") {
        lastProbe_ = m.dump(0);
        scriptWaitProbe_ = 0;
    } else if (type == "test.probe") {
        // The page answers with element rects and its interaction state (editor.probed):
        // test.mouse / test.wheel aim at the rects, test.expect {probe: ...} checks the state.
        // The script holds until the answer comes (a busy page can take more than a frame or two).
        scriptWaitProbe_ = 600;
        JsonWriter w;
        w.beginObject().value("type", "editor.probe").beginArray("rects");
        if (const Json* r = m.get("rects"))
            for (const Json& s : r->array)
                w.value(nullptr, std::string_view(s.str));
        w.endArray().value("reveal", m.getBool("reveal")).endObject();  // ("reveal": scrolled into view first)
        ui_->send(w.str());
    } else if (type == "test.mouse" || type == "test.wheel") {
        // Real input: SDL events pushed onto the queue, through the UI layer and cef-lite to
        // the page. "at": a selector from the last probe, aimed at (fx, fy) of its rect.
        float x = 0.0f, y = 0.0f;
        if (!probePoint(m, x, y))
            return;
        SDL_Event e = {};
        const SDL_WindowID window = SDL_GetWindowID(ui_->window());
        if (const Json* mods = m.get("mods")) {  // ["ctrl", "shift", "alt"]: held from now (until another "mods")
            uint32_t held = SDL_KMOD_NONE;
            for (const Json& k : mods->array)
                held |= k.str == "ctrl" ? SDL_KMOD_LCTRL : k.str == "shift" ? SDL_KMOD_LSHIFT : k.str == "alt" ? SDL_KMOD_LALT : 0u;
            SDL_SetModState(SDL_Keymod(held));
        }
        if (type == "test.wheel") {
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = window;
            e.wheel.y = float(m.getNumber("y", -1.0));  // notches, + away from the user
            e.wheel.mouse_x = x;
            e.wheel.mouse_y = y;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            SDL_PushEvent(&e);
            return;
        }
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = window;
        e.motion.x = x;
        e.motion.y = y;
        SDL_PushEvent(&e);
        const std::string action = m.getString("action", "click");
        if (action == "move")
            return;
        const std::string button = m.getString("button", "left");
        e = {};
        e.button.windowID = window;
        e.button.button = button == "right" ? SDL_BUTTON_RIGHT : button == "middle" ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
        e.button.clicks = uint8_t(m.getNumber("clicks", 1));
        e.button.x = x;
        e.button.y = y;
        if (action != "up") {
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.down = true;
            SDL_PushEvent(&e);
        }
        if (action != "down") {
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.down = false;
            SDL_PushEvent(&e);
        }
    } else if (type == "test.drawer") {
        // (X3) A message to the selected entity's Inspector page of a class ({class, data}), as if
        // its component said it: the page hears it (FalconPanel.onMessage).
        JsonWriter w;
        w.beginObject().value("type", "editor.drawer.post").value("class", m.getString("class"));
        if (const Json* d = m.get("data"))
            w.raw("data", d->dump(0));
        w.endObject();
        ui_->send(w.str());
    } else if (type == "test.ping") {
        JsonWriter w;
        w.beginObject().value("type", "editor.ping").value("n", uint32_t(m.getNumber("n"))).endObject();
        ui_->send(w.str());
        pingFrames_ = ui_->stats().frames;
    } else if (type == "test.gamepad") {
        // A virtual gamepad (made by the first step; "remove": true takes it away): its buttons
        // (south, east, west, north, back, guide, start, ..., dpup, dpdown, dpleft, dpright) and axes (leftx, lefty, rightx, righty,
        // left_trigger, right_trigger: -1 .. 1).
        static SDL_JoystickID virtualPad = 0;
        static SDL_Joystick* joystick = nullptr;
        if (m.getBool("remove")) {
            if (joystick)
                SDL_CloseJoystick(joystick);
            if (virtualPad)
                SDL_DetachVirtualJoystick(virtualPad);
            virtualPad = 0;
            joystick = nullptr;
            return;
        }
        if (!virtualPad) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // (the test's window is hidden: never focused)
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
            desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
            desc.name = "Falcon test gamepad";
            virtualPad = SDL_AttachVirtualJoystick(&desc);
            joystick = virtualPad ? SDL_OpenJoystick(virtualPad) : nullptr;
            if (!joystick) {
                testFailed_ = true;
                FX_ERROR("test.gamepad: no virtual gamepad: %s", SDL_GetError());
                return;
            }
        }
        static const char* const kButtons[] = {"south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
                                               "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
        if (const Json* b = m.get("buttons"))
            for (const auto& [name, down] : b->object)
                for (int i = 0; i < int(std::size(kButtons)); ++i)
                    if (name == kButtons[i])
                        SDL_SetJoystickVirtualButton(joystick, i, down.b);
        if (const Json* a = m.get("axes"))
            for (const auto& [name, value] : a->object)
                if (const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str()); axis != SDL_GAMEPAD_AXIS_INVALID)
                    SDL_SetJoystickVirtualAxis(joystick, int(axis), Sint16(std::clamp(value.number, -1.0, 1.0) * 32767.0));
    } else if (type == "test.type") {
        // Typed text as SDL delivers it while text input is on (the page's focused field gets it).
        // The text lives until the next test.type: the event points at it.
        typed_ = m.getString("text");
        SDL_Event e = {};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(ui_->window());
        e.text.text = typed_.c_str();
        SDL_PushEvent(&e);
    } else if (type == "test.key") {
        // Pressed and released as if typed: SDL's queue -> UI layer -> page -> page shortcut.
        // "action": "down" or "up" only (a key held, as a chord's Shift: a held modifier is SDL's
        // modifier state too, so editor code sees it, and later keys carry it).
        const std::string name = m.getString("key"), action = m.getString("action");
        SDL_Event e = {};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDL_GetKeyFromName(name.c_str());
        e.key.scancode = SDL_GetScancodeFromKey(e.key.key, nullptr);
        const uint32_t modifier = e.key.key == SDLK_LSHIFT || e.key.key == SDLK_RSHIFT ? SDL_KMOD_LSHIFT
                                  : e.key.key == SDLK_LCTRL || e.key.key == SDLK_RCTRL ? SDL_KMOD_LCTRL
                                  : e.key.key == SDLK_LALT || e.key.key == SDLK_RALT   ? SDL_KMOD_LALT
                                                                                        : 0u;
        uint32_t held = SDL_GetModState() & (SDL_KMOD_LSHIFT | SDL_KMOD_LCTRL | SDL_KMOD_LALT);
        if (modifier && action == "down")
            held |= modifier;
        else if (modifier && action == "up")
            held &= ~modifier;
        SDL_SetModState(SDL_Keymod(held));
        e.key.mod = SDL_Keymod(held | (m.getBool("ctrl") ? SDL_KMOD_LCTRL : 0) | (m.getBool("shift") ? SDL_KMOD_LSHIFT : 0) |
                               (m.getBool("alt") ? SDL_KMOD_LALT : 0));
        if (action != "up") {
            e.key.down = true;
            SDL_PushEvent(&e);
        }
        if (action != "down") {
            e.type = SDL_EVENT_KEY_UP;
            e.key.down = false;
            SDL_PushEvent(&e);
        }
    } else if (type == "test.hold") {
        // A key held down (or let go) for the game's scripts, whose keyboard is SDL's state
        // (which events pushed here do not change).
        const SDL_Scancode code = SDL_GetScancodeFromKey(SDL_GetKeyFromName(m.getString("key").c_str()), nullptr);
        if (code > SDL_SCANCODE_UNKNOWN && size_t(code) < sizeof(testKeys_))
            testKeys_[code] = m.getBool("down", true) ? 1 : 0;
    } else if (type == "test.wait") {
        scriptWait_ = uint32_t(m.getNumber("frames", 0));
        scriptWaitUntilNs_ = SDL_GetTicksNS() + uint64_t(m.getNumber("ms", 0) * 1e6);
        // "generated": also until terrain generation is done (at most `frames` more);
        // "previews": until no preview is being rendered.
        scriptWaitGenerated_ = m.getBool("generated");
        scriptWaitWear_ = m.getBool("wear");  // until the level meshes' live wear is made
        scriptWaitWearFrames_ = 3;
        scriptWaitCuts_ = m.getBool("cuts");  // until no cut is being made (on the workers)
        scriptWaitPreviews_ = m.getBool("previews");
        scriptWaitGi_ = m.getBool("gi");  // until every GI probe sleeps (the indirect light no longer changes)
        scriptWaitCook_ = m.getBool("cook");  // until no scene is being cooked
        scriptWaitBake_ = m.getBool("audioBake");  // until the audio bake is done
        scriptWaitBuild_ = m.getBool("build");  // until the scripts' build is done and loaded
        scriptWaitGameBuild_ = m.getBool("gameBuild");  // until File > Build Game is done
        scriptWaitLibraries_ = m.getBool("libraries");  // until no library is being installed
        scriptWaitShaders_ = m.getBool("shaders");      // until no shader is being compiled
        if (scriptWaitShaders_)
            shaders_.update(true);
        if (scriptWaitBuild_)
            watchScripts(true);  // (a source just written: its build starts now)
    } else if (type == "test.newScene") {
        // A scene added to the project and cooked (empty), not opened: for games to load.
        std::string relative, error;
        if (!hasProject_ || !addScene(project_, m.getString("name"), m.getString("folder", "Scenes"), relative, error, true)) {
            FX_ERROR("editor test: test.newScene: %s", hasProject_ ? error.c_str() : "no project");
            testFailed_ = true;
        }
        sendState(false);
    } else if (type == "test.writeFile") {
        // A file in the project ("pid": this process's id as its text: a cook lock held by a
        // running process; "bundle": in the editor page's own directory instead); "remove":
        // deleted instead.
        const std::filesystem::path path =
            std::filesystem::path(m.getBool("bundle") ? ui_->bundle() : project_.dir) / m.getString("path");
        std::error_code ec;
        if (m.getBool("remove")) {
            std::filesystem::remove(path, ec);
        } else {
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream(path, std::ios::binary) << (m.getBool("pid") ? std::to_string(currentProcessId()) : m.getString("text"));
        }
    } else if (type == "test.editFile") {
        // A project file's text edited: each match of "find" (an ECMAScript regex) replaced by
        // "replace" ($1: its groups); an error when nothing matched (an older format made by hand).
        const std::filesystem::path path = std::filesystem::path(project_.dir) / m.getString("path");
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        in.close();
        try {
            const std::regex find(m.getString("find"));
            const std::string was = text.str();
            if (!std::regex_search(was, find)) {
                testFailed_ = true;
                FX_ERROR("test.editFile: nothing in %s matches", m.getString("path").c_str());
            } else {
                std::ofstream(path, std::ios::binary) << std::regex_replace(was, find, m.getString("replace"));
            }
        } catch (const std::regex_error& e) {
            testFailed_ = true;
            FX_ERROR("test.editFile: %s", e.what());
        }
    } else if (type == "test.pick") {
        if (view_)
            view_->pickAt(float(m.getNumber("x", 0.5)), float(m.getNumber("y", 0.5)));
    } else if (type == "test.expect") {
        std::string why;
        // "selection": [names]: every selected entity, the primary last.
        if (const Json* v = m.get("selection"); v && v->type == Json::Type::Array) {
            std::string got;
            for (const uint32_t i : selection_)
                got += (got.empty() ? "" : ",") + defaultName(i);
            std::string want;
            for (const Json& n : v->array)
                want += (want.empty() ? "" : ",") + n.str;
            if (got != want)
                why += " selection [" + got + "]";
        }
        // "viewport": {"captured", "hint", "previewLines"}: editor code holding the scene view (a modal
        // operation), its hint, the lines of a tool's outline (the opening tool's).
        if (const Json* g = m.get("viewport")) {
            if (const Json* v = g->get("captured"); v && view_ && v->b != view_->captured())
                why += view_->captured() ? " the view is captured" : " the view is not captured";
            if (const Json* v = g->get("hint"); v && hint_.find(v->str) == std::string::npos)
                why += " hint '" + hint_ + "'";
            if (const Json* v = g->get("previewLines"); v && view_ && view_->previewLines().size() != size_t(v->number))
                why += " " + std::to_string(view_->previewLines().size()) + " preview lines";
        }
        // "elements": {"mode", "count"}: the level tools' element selection.
        if (const Json* g = m.get("elements")) {
            static const char* const kModes[] = {"object", "vertex", "edge", "face"};
            const uint32_t mode = view_ ? uint32_t(view_->tools().element) : 0;
            if (const Json* v = g->get("mode"); v && v->str != kModes[mode])
                why += std::string(" element mode ") + kModes[mode];
            const size_t count = elements_.entity == selected_ && elements_.mode == mode ? elements_.items.size() : 0;
            if (const Json* v = g->get("count"); v && size_t(v->number) != count)
                why += " elements chosen " + std::to_string(count);
        }
        // "pick": {"live", "liveTriangle"}: the last click's live mesh (-1: none) and its source triangle.
        if (const Json* g = m.get("pick")) {
            if (const Json* v = g->get("live"); v && int32_t(v->number) != lastPick_.live)
                why += " picked live mesh " + std::to_string(lastPick_.live);
            if (const Json* v = g->get("liveTriangle"); v && int32_t(v->number) != lastPick_.liveTriangle)
                why += " picked live triangle " + std::to_string(lastPick_.liveTriangle);
            if (const Json* v = g->get("face"); v && int32_t(v->number) != lastPick_.face)
                why += " picked level face " + std::to_string(lastPick_.face);
            if (const Json* v = g->get("face>="); v && lastPick_.face < int32_t(v->number))
                why += " picked level face " + std::to_string(lastPick_.face);
            if (const Json* v = g->get("selected"); v && v->str != (selected_ == ~0u ? std::string() : defaultName(selected_)))
                why += " selected '" + (selected_ == ~0u ? std::string() : defaultName(selected_)) + "'";
        }
        if (const Json* v = m.get("decals"); v) {  // live entities with a decal
            uint32_t n = 0;
            for (uint32_t k = 0; k < records_.size(); ++k)
                if (const flecs::entity e = entityOf(k); e && e.is_alive() && e.enabled() && e.has<Decal>())
                    ++n;
            if (n != uint32_t(v->number))
                why += " decals " + std::to_string(n);
        }
        // The measure tool's ruler: "measured": [length, within]; the camera: "orthographic".
        if (const Json* v = m.get("measured"); v && v->array.size() == 2 &&
            std::fabs(measuredLength_ - float(v->array[0].number)) > float(v->array[1].number))
            why += " measured " + std::to_string(measuredLength_);
        if (const Json* v = m.get("orthographic"); v && camera_ && v->b != camera_->orthographic())
            why += std::string(" orthographic ") + (camera_->orthographic() ? "on" : "off");
        // The navigation mesh: "navPolys>" (polygons), "navPath" {"from", "to", "found", "length>", "length<"}.
        if (const Json* v = m.get("navPolys>"); v) {
            const auto nav = level::navigation();
            if (!nav || !(nav->polygonCount() > uint32_t(v->number)))
                why += " navigation polygons " + std::to_string(nav ? nav->polygonCount() : 0);
        }
        if (const Json* v = m.get("navPath"); v && v->isObject()) {
            const auto nav = level::navigation();
            float from[3] = {}, to[3] = {};
            readVec3(*v, "from", from);
            readVec3(*v, "to", to);
            std::vector<float3> path;
            const bool found = nav && nav->findPath({from[0], from[1], from[2]}, {to[0], to[1], to[2]}, path);
            float length = 0.0f;
            for (size_t k = 1; k < path.size(); ++k)
                length += fx::length(path[k] - path[k - 1]);
            if (const Json* f = v->get("found"); f && f->b != found)
                why += std::string(" navigation path ") + (found ? "found" : "not found");
            if (const Json* l = v->get("length>"); l && !(length > float(l->number)))
                why += " navigation path " + std::to_string(length) + " m";
            if (const Json* l = v->get("length<"); l && !(length < float(l->number)))
                why += " navigation path " + std::to_string(length) + " m";
        }
        if (const Json* v = m.get("rubble"); v) {  // live heaps of rubble (the damage brush's)
            uint32_t n = 0;
            for (uint32_t k = 0; k < records_.size(); ++k)
                if (const flecs::entity e = entityOf(k); e && e.is_alive() && e.enabled() && defaultName(k).starts_with("Rubble"))
                    ++n;
            if (n != uint32_t(v->number))
                why += " rubble " + std::to_string(n);
        }
        if (const Json *v = m.get("damage"), *more = m.get("damage>"), *fewer = m.get("damage<"); v || more || fewer) {  // live damage cuts
            uint32_t n = 0;
            for (uint32_t k = 0; k < records_.size(); ++k)
                if (const flecs::entity e = entityOf(k); e && e.is_alive() && e.enabled())
                    if (const CutVolume* c = e.try_get<CutVolume>(); c && c->shape == level::CutShape::Damage)
                        ++n;
            if ((v && n != uint32_t(v->number)) || (more && !(n > uint32_t(more->number))) || (fewer && !(n < uint32_t(fewer->number))))
                why += " damage cuts " + std::to_string(n);
        }
        if (const Json* v = m.get("damageShown"); v && view_ && view_->damageShown() != uint32_t(v->number))  // (their gizmos)
            why += " damage cuts drawn " + std::to_string(view_->damageShown());
        if (const Json* v = m.get("cutsShown"); v && view_ && view_->cutsShown() != uint32_t(v->number))  // (doorways' outlines too)
            why += " cuts drawn " + std::to_string(view_->cutsShown());
        if (const Json* v = m.get("outlinesShown"); v && view_ && view_->outlinesShown() != uint32_t(v->number))  // (volumes', splines')
            why += " volume and spline outlines drawn " + std::to_string(view_->outlinesShown());
        if (const Json* v = m.get("cutCopies"); v && levelMeshes_ && levelMeshes_->cutCopies() != uint32_t(v->number))
            why += " cut copies " + std::to_string(levelMeshes_->cutCopies());
        // Baked cuts: in the scene's bake as read, taken from it, and cut afresh (since it opened).
        if (const Json* v = m.get("cutBake"); v && levelMeshes_ && levelMeshes_->bakeSize() != size_t(v->number))
            why += " cut bake " + std::to_string(levelMeshes_->bakeSize());
        if (const Json* v = m.get("bakedCuts"); v && levelMeshes_ && levelMeshes_->stats().bakedCuts != uint32_t(v->number))
            why += " baked cuts " + std::to_string(levelMeshes_->stats().bakedCuts);
        // Cuts being made on the workers now; how long the last one took (at least).
        // "prototypeDefault": level meshes' faces without a material draw with the project's prototype
        // grid (its .fmat there, a material index for it).
        if (const Json* v = m.get("prototypeDefault"); v && hasProject_) {
            std::error_code ec;
            const bool there = std::filesystem::exists(project_.path(level::kPrototypeDefault), ec) &&
                               project_.levelDefaultMaterial == level::kPrototypeDefault && project_.prototypeInstalled;
            const int32_t index = levelMeshes_ ? levelMeshes_->defaultMaterialIndex() : -1;
            if (v->b != (there && index > 0))
                why += " prototype default " + std::string(there ? "installed" : "not installed") + ", material " + std::to_string(index);
        }
        if (const Json* v = m.get("cutsPending"); v && levelMeshes_ && levelMeshes_->cutsPending() != uint32_t(v->number))
            why += " cuts pending " + std::to_string(levelMeshes_->cutsPending());
        if (const Json* v = m.get("lastCutMs>"); v && levelMeshes_ && !(levelMeshes_->stats().lastCutMs > float(v->number)))
            why += " last cut " + std::to_string(levelMeshes_->stats().lastCutMs) + " ms";
        if (const Json* v = m.get("freshCuts"); v && levelMeshes_) {
            const LevelMeshStats& ls = levelMeshes_->stats();
            if (ls.cuts - ls.bakedCuts != uint32_t(v->number))
                why += " fresh cuts " + std::to_string(ls.cuts - ls.bakedCuts);
        }
        // "live": {"meshes", "triangles", "uploads>", "bvhTriangles>"}: the renderer's live meshes (and
        // their triangles in the ray-tracing BVH, which the probe GI's rays hit).
        if (const Json* g = m.get("live"); g && renderer_) {
            const LiveGeometryStats& ls = renderer_->live().stats();
            if (const Json* v = g->get("meshes"); v && uint32_t(v->number) != ls.meshes)
                why += " live meshes " + std::to_string(ls.meshes);
            if (const Json* v = g->get("triangles"); v && uint32_t(v->number) != ls.triangles)
                why += " live triangles " + std::to_string(ls.triangles);
            if (const Json* v = g->get("uploads>"); v && !(double(ls.uploads) > v->number))
                why += " live uploads " + std::to_string(ls.uploads);
            if (const Json* v = g->get("bvhTriangles>"); v && !(double(ls.bvhTriangles) > v->number))
                why += " live triangles in the BVH " + std::to_string(ls.bvhTriangles);
        }
        if (const Json* v = m.get("scripts"); v && v->str != scriptsState())
            why += " scripts '" + scriptsState() + "'" + (scriptsError_.empty() ? "" : " (" + scriptsError_ + ")");
        if (const Json* v = m.get("hud"); v && (v->str.empty() ? !hud_.empty() : hud_.find(v->str) == std::string::npos))
            why += " hud '" + hud_ + "'";
        if (const Json* g = m.get("game")) {
            if (const Json* v = g->get("running"); v && v->b != game_.running)
                why += game_.running ? " the game runs" : " the game does not run";
            if (const Json* v = g->get("entities"); v && uint32_t(v->number) != game_.entities)
                why += " game entities " + std::to_string(game_.entities);
            if (const Json* v = g->get("bodies"); v && uint32_t(v->number) != game_.bodies)
                why += " bodies " + std::to_string(game_.bodies);
        }
        // "runtimeMeshes": {"models>", "spawned>", "spawned", "starved"}: particles' models drawn, spawned meshes alive.
        if (const Json* g = m.get("runtimeMeshes"); g && runtimeMeshes_) {
            const RuntimeMeshes::Stats& rs = runtimeMeshes_->stats();
            if (const Json* v = g->get("models>"); v && !(double(rs.models) > v->number))
                why += " particle models " + std::to_string(rs.models);
            if (const Json* v = g->get("models<"); v && !(double(rs.models) < v->number))
                why += " particle models " + std::to_string(rs.models);
            if (const Json* v = g->get("spawned>"); v && !(double(rs.spawned) > v->number))
                why += " spawned meshes " + std::to_string(rs.spawned);
            if (const Json* v = g->get("spawned"); v && double(rs.spawned) != v->number)
                why += " spawned meshes " + std::to_string(rs.spawned);
            if (const Json* v = g->get("starved"); v && double(rs.starved) != v->number)
                why += " starved particle models " + std::to_string(rs.starved);
        }
        // "shader": {path, compiled, error (a substring), entries, uniforms [names], loads (a pipeline
        // made from it), revisionAtLeast}: a project shader as the editor compiled it.
        if (const Json* sh = m.get("shader"); sh && sh->isObject()) {
            const std::string path = sh->getString("path");
            const ProjectShaders::Status* st = shaders_.status(path);
            if (!st) {
                why += " shader " + path + " unknown";
            } else {
                if (const Json* v = sh->get("compiled"); v && v->b != st->compiled)
                    why += " shader " + path + (st->compiled ? " compiled" : " not compiled: " + st->error);
                if (const Json* v = sh->get("error"); v && st->error.find(v->str) == std::string::npos)
                    why += " shader error '" + st->error + "'";
                if (const Json* v = sh->get("revisionAtLeast"); v && shaders_.revision() < uint64_t(v->number))
                    why += " shaders revision " + std::to_string(shaders_.revision());
                ShaderFile file;
                std::string error;
                const bool read = (sh->get("entries") || sh->get("uniforms") || sh->get("loads")) &&
                                  loadShaderFile(ProjectShaders::blobPath(project_.dir, path), file, error);
                if (const Json* v = sh->get("entries"); v && (!read || file.entries.size() != size_t(v->number)))
                    why += " shader entries " + (read ? std::to_string(file.entries.size()) : error);
                if (const Json* v = sh->get("uniforms"); v && read) {
                    std::string names, expected;
                    for (const ShaderFile::Uniform& u : file.entries[0].uniforms)
                        names += (names.empty() ? "" : ",") + u.name;
                    for (const Json& n : v->array)
                        expected += (expected.empty() ? "" : ",") + n.str;
                    if (names != expected)
                        why += " shader uniforms " + names;
                }
                if (const Json* v = sh->get("loads"); v && v->b) {
                    SDL_GPUComputePipeline* pipe = read && gpu_ ? gpu_->createComputePipeline(file.entries[0].blob) : nullptr;
                    if (!pipe)
                        why += " shader did not load: " + error;
                    else
                        SDL_ReleaseGPUComputePipeline(gpu_->get(), pipe);
                }
            }
        }
        if (const Json* v = m.get("dirty"); v && v->b != dirty_)
            why += dirty_ ? " dirty" : " not dirty";
        if (const Json* v = m.get("sceneOpen"); v && v->str != sceneInProject_)
            why += " scene open '" + sceneInProject_ + "'";
        if (const Json* v = m.get("sceneSwitches"); v && uint32_t(v->number) != sceneSwitches_)
            why += " scene switches " + std::to_string(sceneSwitches_);
        if (const Json* v = m.get("launched"); v && launched_.find(v->str) == std::string::npos)
            why += " launched '" + launched_ + "'";
        // The last cook (editor.project.openScene of a scene needing one): the stages it went
        // through, at least this many textures copied from earlier cooks, still running or not.
        if (const Json* v = m.get("cookStages"))
            for (const Json& s : v->array)
                if (std::find(cookStages_.begin(), cookStages_.end(), s.str) == cookStages_.end())
                    why += " cook stage " + s.str + " not seen";
        if (const Json* v = m.get("cookReused"); v && cookReused_ < uint32_t(v->number))
            why += " cook reused " + std::to_string(cookReused_);
        if (const Json* v = m.get("cooking"); v && v->b != bool(cook_))
            why += cook_ ? " cooking" : " not cooking";
        if (const Json* v = m.get("projectScenes"); v && size_t(v->number) != project_.scenes.size())
            why += " project scenes " + std::to_string(project_.scenes.size());
        // "meshExists": a mesh name or a list (in this scene's pak); "modelLoaded": a model cooked into it.
        if (const Json* v = m.get("meshExists")) {
            const std::vector<Json> list = v->type == Json::Type::Array ? v->array : std::vector<Json>{*v};
            for (const Json& n : list)
                if (pak_->findMesh(n.str) < 0)
                    why += " no mesh " + n.str;
        }
        if (const Json* v = m.get("modelLoaded"); v && !pak_->library(v->str))
            why += " model " + v->str + " not cooked into the scene";
        if (const Json* v = m.get("previewRuns"); v && uint32_t(v->number) != assets_.previewRuns())
            why += " preview runs " + std::to_string(assets_.previewRuns());
        if (const Json* v = m.get("projectStartup"); v && v->str != project_.startupScene)
            why += " project startup '" + project_.startupScene + "'";
        // "debugLines": {atLeast, atMost}: scripts' debug lines this frame; "bloom": the frame's bloom intensity.
        if (const Json* d = m.get("gizmoLines"); d && d->isObject()) {  // (X3) {atLeast, atMost}
            if (const Json* v = d->get("atLeast"); v && gizmoLineCount_ < uint32_t(v->number))
                why += " gizmo lines " + std::to_string(gizmoLineCount_);
            if (const Json* v = d->get("atMost"); v && gizmoLineCount_ > uint32_t(v->number))
                why += " gizmo lines " + std::to_string(gizmoLineCount_);
        }
        if (const Json* d = m.get("debugLines"); d && d->isObject()) {
            if (const Json* v = d->get("atLeast"); v && debugLineCount_ < uint32_t(v->number))
                why += " debug lines " + std::to_string(debugLineCount_);
            if (const Json* v = d->get("atMost"); v && debugLineCount_ > uint32_t(v->number))
                why += " debug lines " + std::to_string(debugLineCount_);
        }
        // "pageLoads": how many times the editor page said it was ready (2: it reloaded once, as its
        // files changed); "pageChanges": at least this many live reloads of the project's pages.
        if (const Json* v = m.get("pageLoads"); v && pageReadies_ != uint32_t(v->number))
            why += " page loads " + std::to_string(pageReadies_);
        if (const Json* v = m.get("pageChanges"); v && pageChanges_ < uint32_t(v->number))
            why += " page changes " + std::to_string(pageChanges_);
        // "notice": a substring of one of the last few notices shown.
        if (const Json* v = m.get("notice");
            v && std::none_of(notices_.begin(), notices_.end(), [&](const std::string& n) { return n.find(v->str) != std::string::npos; }))
            why += " notice '" + v->str + "' not shown";
        // "gameBuild": "ok", or a substring of the last Build Game's error.
        if (const Json* v = m.get("gameBuild");
            v && (v->str == "ok" ? gameBuildResult_ != "ok" : gameBuildResult_ == "ok" || gameBuildResult_.find(v->str) == std::string::npos))
            why += " game build '" + gameBuildResult_ + "'";
        if (const Json* v = m.get("bloom"); v && settings_ && std::fabs(settings_->post.bloomIntensity - float(v->number)) > 1e-4f)
            why += " bloom " + std::to_string(settings_->post.bloomIntensity);
        // "library": {name, exists, trusted, enabled, error (a substring; "" none)}: the project's library.
        if (const Json* lib = m.get("library"); lib && lib->isObject()) {
            const std::string name = lib->getString("name");
            const std::vector<libraries::Library> all = hasProject_ ? libraries::scan(project_.dir) : std::vector<libraries::Library>{};
            const auto it = std::find_if(all.begin(), all.end(), [&](const libraries::Library& l) { return l.name == name; });
            if (const Json* v = lib->get("exists"); v && v->b != (it != all.end()))
                why += " library " + name + (it != all.end() ? " exists" : " missing");
            if (it != all.end()) {
                if (const Json* v = lib->get("trusted"); v && v->b != it->trusted)
                    why += " library " + name + (it->trusted ? " trusted" : " not trusted");
                if (const Json* v = lib->get("enabled"); v && v->b != it->enabled)
                    why += " library " + name + (it->enabled ? " enabled" : " disabled");
                if (const Json* v = lib->get("error"); v && (v->str.empty() ? !it->error.empty() : it->error.find(v->str) == std::string::npos))
                    why += " library " + name + " error '" + it->error + "'";
            }
        }
        // "fileExists" / "fileMissing": a path or a list, relative to the project (the scene's folder without one).
        for (const char* key : {"fileExists", "fileMissing"}) {
            const Json* v = m.get(key);
            const std::vector<Json> list = !v ? std::vector<Json>() : v->type == Json::Type::Array ? v->array : std::vector<Json>{*v};
            for (const Json& p : list)
                if (assets_.exists(p.str) != (key[4] == 'E'))
                    why += std::string(" ") + key + " " + p.str;
        }
        // "fileContains": {path: text}: a project file has the text (its JSON as written, say).
        if (const Json* v = m.get("fileContains"); v && v->isObject())
            for (const auto& [path, text] : v->object) {
                std::ifstream in(std::filesystem::path(project_.dir) / path, std::ios::binary);
                std::stringstream body;
                body << in.rdbuf();
                if (body.str().find(text.str) == std::string::npos)
                    why += " " + path + " lacks '" + text.str + "'";
            }
        if (const Json* n = m.get("selectedName")) {
            const std::string actual = selected_ == ~0u ? "" : defaultName(selected_);
            if (!actual.starts_with(n->str))
                why += " selectedName '" + actual + "' != '" + n->str + "'";
        }
        if (const Json* v = m.get("pong"); v && uint32_t(v->number) != lastPong_)
            why += " pong " + std::to_string(lastPong_) + " (the page did not answer)";
        if (const Json* v = m.get("pong"); v && ui_->stats().frames <= pingFrames_)
            why += " (the page did not repaint after the ping)";
        if (const Json* v = m.get("canUndo"); v && v->b == undo_.empty())
            why += " canUndo";
        if (const Json* v = m.get("canRedo"); v && v->b == redo_.empty())
            why += " canRedo";
        if (const Json* v = m.get("playing"); v && v->b != playing_)
            why += " playing";
        if (const Json* v = m.get("paused"); v && v->b != paused_)
            why += " paused";
        if (const Json* probe = m.get("probe"))
            why += probeMismatch(*probe);
        if (const Json* want = m.get("settings")) {  // {key: value} against the settings report
            JsonWriter w;
            w.beginObject();
            writeSettings(w);
            w.endObject();
            const Json report = Json::parse(w.str());
            const Json* settings = report.get("settings");
            for (const auto& [key, value] : want->object) {
                const Json* got = settings ? settings->get(key.c_str()) : nullptr;
                const bool ok = got && got->type == value.type &&
                                (value.type == Json::Type::Number ? std::fabs(got->number - value.number) < 1e-6
                                 : value.type == Json::Type::Bool ? got->b == value.b
                                                                  : got->str == value.str);
                if (!ok)
                    why += " settings." + key + " (got " + (got ? got->dump(0) : std::string("nothing")) + ")";
            }
        }
        // {"entity": {"name", "parent" (name, "" for a root), "alive", "position" [world xyz],
        //             "lightColor" [linear rgb, within 0.05], "lightType" point|spot|directional,
        //             "has" / "lacks" [component names], "yAbove" / "yBelow" (its world y),
        //             "mass" (its PhysicsBody's), "collider" box|sphere|mesh, "sound" (its AudioSource's path),
        //             "rowHidden" / "rowShadow" / "rowTint" [r, g, b, a] (its instance row as drawn)}}
        // "audio": {"voices", "sources" (scene sources playing), "previewing", "spatializer" ("Steam
        // Audio ..." or "panning"), "variedPlays" (since Play)}: the audio system now.
        if (const Json* ex = m.get("audio")) {
            const AudioMixerStats st = audio_ ? audio_->stats() : AudioMixerStats{};
            if (const Json* v = ex->get("voices"); v && (!audio_ || st.voices != uint32_t(v->number)))
                why += " audio voices " + std::to_string(st.voices);
            if (const Json* v = ex->get("sources"); v && (!audio_ || audio_->sourcesPlaying() != uint32_t(v->number)))
                why += " audio sources playing " + std::to_string(audio_ ? audio_->sourcesPlaying() : 0u);
            if (const Json* v = ex->get("variedPlays"); v && (!audio_ || audio_->variedPlays() != uint32_t(v->number)))
                why += " audio variedPlays " + std::to_string(audio_ ? audio_->variedPlays() : 0u);
            if (const Json* v = ex->get("previewing"); v && (!audio_ || audio_->previewing() != v->b))
                why += std::string(" audio ") + (audio_ && audio_->previewing() ? "previewing" : "not previewing");
            if (const Json* v = ex->get("spatializer"); v && (!audio_ || !std::string(audio_->spatializerName()).starts_with(v->str)))
                why += std::string(" audio spatializer ") + (audio_ ? audio_->spatializerName() : "none");
            // "impactsAbove" / "scrapesAbove" / "rollsAbove": physics sounds started since the start.
            if (audio_) {
                const PhysicsAudioStats& ps = audio_->physicsStats();
                for (auto [key, n] : {std::pair{"impactsAbove", ps.impacts}, std::pair{"scrapesAbove", ps.scrapes}, std::pair{"rollsAbove", ps.rolls}})
                    if (const Json* v = ex->get(key); v && !(double(n) > v->number))
                        why += std::string(" audio ") + key + " (" + std::to_string(n) + ")";
                // Water: "splashesAbove", "sloshingAbove" (loops now), "underwater"
                // (the listener's muffle: true over a half, false none).
                const WaterAudioStats& ws = audio_->waterStats();
                for (auto [key, n] : {std::pair{"splashesAbove", ws.splashes}, std::pair{"sloshingAbove", uint64_t(ws.sloshing)}})
                    if (const Json* v = ex->get(key); v && !(double(n) > v->number))
                        why += std::string(" audio ") + key + " (" + std::to_string(n) + ")";
                if (const Json* v = ex->get("underwater"); v && (v->b ? ws.underwater <= 0.5f : ws.underwater > 0.0f))
                    why += " audio underwater " + std::to_string(ws.underwater);
            }
            // (X4) "peakAbove" / "peakBelow": the last block mixed's loudest sample.
            if (const Json* v = ex->get("peakAbove"); v && !(audio_ && double(st.peak) > v->number))
                why += " audio peak " + std::to_string(st.peak);
            if (const Json* v = ex->get("peakBelow"); v && !(audio_ && double(st.peak) < v->number))
                why += " audio peak " + std::to_string(st.peak);
            // "pathsAbove": voices heard along their baked paths in the last block mixed.
            if (const Json* v = ex->get("pathsAbove"); v && !(audio_ && double(st.paths) > v->number))
                why += " audio voices along baked paths " + std::to_string(st.paths);
            // "reflecting": sounds with reflections of their own now.
            if (const Json* v = ex->get("reflecting"); v && (!audio_ || audio_->sceneStats().reflecting != uint32_t(v->number)))
                why += " audio reflecting " + std::to_string(audio_ ? audio_->sceneStats().reflecting : 0u);
            // "reverbAbove" / "reverbBelow": the listener's reverb time (RT60, mid band, seconds).
            for (const char* key : {"reverbAbove", "reverbBelow"})
                if (const Json* v = ex->get(key)) {
                    float rt[3] = {0, 0, 0};
                    const bool got = audio_ && audio_->listenerReverb(rt);
                    if (!got || (key[6] == 'A' ? !(rt[1] > float(v->number)) : !(rt[1] < float(v->number))))
                        why += std::string(" audio ") + key + " (" + (got ? std::to_string(rt[1]) : std::string("none")) + ")";
                }
            // "bake": {"state" (none, baked, stale, baking), "probesAbove", "pathing" (baked paths),
            // "listenerBaked" (the last reflections run's listener reverb came from the probes),
            // "pathingAbove" (sources whose baked paths the last run found), "staleWhy" (its start)}.
            if (const Json* bk = ex->get("bake")) {
                const AudioBakeStatus st = audio_ ? audio_->bakeStatus() : AudioBakeStatus{};
                const AudioSceneStats ss = audio_ ? audio_->sceneStats() : AudioSceneStats{};
                if (const Json* v = bk->get("state"); v && st.state != v->str)
                    why += " audio bake " + st.state + (st.error.empty() ? "" : " (" + st.error + ")");
                if (const Json* v = bk->get("probesAbove"); v && !(double(st.info.probes) > v->number))
                    why += " audio baked probes " + std::to_string(st.info.probes);
                if (const Json* v = bk->get("pathing"); v && st.info.pathing != v->b)
                    why += std::string(" audio baked pathing ") + (st.info.pathing ? "on" : "off");
                if (const Json* v = bk->get("listenerBaked"); v && ss.listenerBaked != v->b)
                    why += std::string(" audio listener reverb ") + (ss.listenerBaked ? "baked" : "realtime");
                if (const Json* v = bk->get("pathingAbove"); v && !(double(ss.pathing) > v->number))
                    why += " audio sources with baked paths " + std::to_string(ss.pathing);
                if (const Json* v = bk->get("staleWhy"); v && !st.staleWhy.starts_with(v->str))
                    why += " audio bake stale: '" + st.staleWhy + "'";
            }
            // "source": {"name", "occlusionAbove" / "occlusionBelow", "transmissionAbove" / "transmissionBelow"
            // (the mid band), "pathAbove" (its baked paths' mid band)}: what the scene does to that
            // entity's playing (or previewed) source.
            if (const Json* src = ex->get("source")) {
                const std::string name = src->getString("name");
                const uint32_t i = idByName(name);
                float occlusion = 1.0f, transmission[3] = {1, 1, 1};
                if (!i || !audio_ || !audio_->directOf(entityOf(i - 1).id(), occlusion, transmission)) {
                    why += " audio source '" + name + "' not simulated";
                } else {
                    const auto check = [&](const char* key, float value, bool above) {
                        if (const Json* v = src->get(key); v && (above ? !(value > float(v->number)) : !(value < float(v->number))))
                            why += " audio '" + name + "' " + key + " (" + std::to_string(value) + ")";
                    };
                    check("occlusionAbove", occlusion, true);
                    check("occlusionBelow", occlusion, false);
                    check("transmissionAbove", transmission[1], true);
                    check("transmissionBelow", transmission[1], false);
                    if (const Json* v = src->get("pathAbove")) {
                        float eq[3] = {0, 0, 0};
                        if (!audio_->pathingOf(entityOf(i - 1).id(), eq) || !(eq[1] > float(v->number)))
                            why += " audio '" + name + "' pathAbove (" + std::to_string(eq[1]) + ")";
                    }
                }
            }
        }
        // "water": {"count", "name", "shape", "material", "width", "field": {key: value}}: the water
        // bodies, one by its entity's name, and fields of its material as resolved (numbers within 0.01).
        if (const Json* ex = m.get("water")) {
            if (const Json* c = ex->get("count"); c && (!water_ || water_->count() != uint32_t(c->number)))
                why += " water count " + std::to_string(water_ ? water_->count() : 0u);
            if (const Json* name = ex->get("name")) {
                const uint32_t i = idByName(name->str);
                const int32_t body = i ? waterBody(i - 1) : -1;
                if (body < 0) {
                    why += " '" + name->str + "' is not water";
                } else {
                    const WaterSystem::Body& b = *water_->body(uint32_t(body));
                    if (const Json* s = ex->get("shape"); s && s->str != waterShapeName(b.desc.shape))
                        why += " shape " + std::string(waterShapeName(b.desc.shape));
                    if (const Json* mt = ex->get("material"); mt && mt->str != b.desc.material)
                        why += " material " + b.desc.material;
                    if (const Json* wd = ex->get("width"); wd && std::fabs(float(wd->number) - b.desc.width) > 0.01f)
                        why += " width " + std::to_string(b.desc.width);
                    if (const Json* f = ex->get("field"); f && f->isObject()) {
                        JsonWriter mw;
                        mw.beginObject();
                        writeWaterMaterial(mw, b.material);
                        mw.endObject();
                        const Json have = Json::parse(mw.str());
                        for (const auto& [key, want] : f->object) {
                            const Json* got = have.get(key.c_str());
                            bool ok = got && got->type == want.type;
                            if (ok && want.type == Json::Type::Number)
                                ok = std::fabs(got->number - want.number) <= 0.01;
                            else if (ok && want.type == Json::Type::Array)
                                for (size_t k = 0; ok && k < want.array.size(); ++k)
                                    ok = k < got->array.size() && std::fabs(got->array[k].number - want.array[k].number) <= 0.01;
                            else if (ok)
                                ok = got->dump(0) == want.dump(0);
                            if (!ok)
                                why += " water field " + key + " is " + (got ? got->dump(0) : std::string("missing"));
                        }
                    }
                }
            }
        }
        // "anim": an entity's Animator: {entity, graph, meshes (skinned meshes it drives), state (its
        // graph's root state machine's active state), params: {name: value}, maxCharacters (in all),
        // hidden / shown: [bones hidden or not (hideBone)], collapsed / whole: [bones whose vertices
        // the eye sees pulled into a joint or not, in each of its meshes; the shadows' skinning whole
        // either way]}.
        if (const Json* an = m.get("anim"); an && an->isObject() && animation_) {
            const uint32_t i = idByName(an->getString("entity"));
            const State s = i ? capture(i - 1) : State{};
            const std::vector<uint32_t> recs = i ? animation_->recordsUnder(scene_->world(), records_[i - 1].entity) : std::vector<uint32_t>{};
            anim::GraphInstance* g = recs.empty() ? nullptr : animation_->graph(recs[0]);
            if (i == 0)
                why += " entity '" + an->getString("entity") + "' not found";
            else if (const Json* v = an->get("graph"); v && (s.animator ? s.animator->graph : std::string("(no animator)")) != v->str)
                why += " animator graph '" + (s.animator ? s.animator->graph : std::string("(no animator)")) + "'";
            // "bonesAt": [{bone, at: [x, y, z] (world), within (m)}]: where bones are as posed (IK
            // included); "looking": {bone, at, within (degrees)}: the bone's forward (the model's +Z
            // at rest) pointing at a point; "aiming": {at, within (degrees)}: the aiming bone (AimAt)
            // pointing at a point; "notAiming": not aiming; "targets": the Animator's IK target slots
            // set (look, aim, hold, leftHand, rightHand), every one.
            const auto boneWorld = [&](const std::string& name, float3& at, float3* forward) {
                const anim::Pose* pose = recs.empty() ? nullptr : animation_->pose(recs[0]);
                const int32_t b = pose ? pose->skeleton()->findBone(name.c_str()) : -1;
                const flecs::entity mesh = b < 0 || !skinning_ ? flecs::entity() : scene_->entity(skinning_->instanceOfRecord(recs[0]));
                const WorldTransform* w = mesh ? mesh.try_get<WorldTransform>() : nullptr;
                if (!w)
                    return false;
                const anim::Xform& x = pose->model()[uint32_t(b)];
                const float(*m)[4] = rows3x4(w->rows);
                at = {m[0][0] * x.t.x + m[0][1] * x.t.y + m[0][2] * x.t.z + m[0][3], m[1][0] * x.t.x + m[1][1] * x.t.y + m[1][2] * x.t.z + m[1][3],
                      m[2][0] * x.t.x + m[2][1] * x.t.y + m[2][2] * x.t.z + m[2][3]};
                if (forward) {
                    const float3 f = anim::rotate(x.r, anim::inverseRotate(pose->skeleton()->referenceModel(uint32_t(b)).r, {0.0f, 0.0f, 1.0f}));
                    *forward = normalize(float3{m[0][0] * f.x + m[0][1] * f.y + m[0][2] * f.z, m[1][0] * f.x + m[1][1] * f.y + m[1][2] * f.z,
                                                m[2][0] * f.x + m[2][1] * f.y + m[2][2] * f.z});
                }
                return true;
            };
            if (const Json* v = an->get("bonesAt"); v && v->type == Json::Type::Array && i)
                for (const Json& b : v->array) {
                    float want[3];
                    float3 at;
                    if (!readVec3(b, "at", want) || !boneWorld(b.getString("bone"), at, nullptr)) {
                        why += " bone " + b.getString("bone") + " not found";
                        continue;
                    }
                    const float3 d = at - float3{want[0], want[1], want[2]};
                    if (length(d) > float(b.getNumber("within", 0.01)))
                        why += " bone " + b.getString("bone") + " at (" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", " +
                               std::to_string(at.z) + ")";
                }
            // "boneAt": {bone, entity, within (m)}: a bone as posed where an entity is (a hold).
            if (const Json* v = an->get("boneAt"); v && v->isObject() && i) {
                float3 at;
                const uint32_t e = idByName(v->getString("entity"));
                const WorldTransform* w = e ? entityOf(e - 1).try_get<WorldTransform>() : nullptr;
                if (!w || !boneWorld(v->getString("bone"), at, nullptr)) {
                    why += " boneAt: no bone " + v->getString("bone") + " or entity " + v->getString("entity");
                } else {
                    const float(*m)[4] = rows3x4(w->rows);
                    const float d = length(at - float3{m[0][3], m[1][3], m[2][3]});
                    if (d > float(v->getNumber("within", 0.01)))
                        why += " " + v->getString("bone") + " is " + std::to_string(d) + " m from " + v->getString("entity");
                }
            }
            if (const Json* v = an->get("looking"); v && v->isObject() && i) {
                float want[3];
                float3 at, forward;
                if (!readVec3(*v, "at", want) || !boneWorld(v->getString("bone"), at, &forward)) {
                    why += " looking bone not found";
                } else {
                    const float3 to = normalize(float3{want[0], want[1], want[2]} - at);
                    const float degrees = std::acos(std::clamp(dot(forward, to), -1.0f, 1.0f)) * 57.29578f;
                    if (degrees > float(v->getNumber("within", 2.0)))
                        why += " " + v->getString("bone") + " looks " + std::to_string(degrees) + " degrees away";
                }
            }
            if (const Json* v = an->get("aiming"); v && v->isObject() && i) {
                float want[3];
                uint32_t bone = 0;
                float3 axis, at;
                const anim::Pose* pose = recs.empty() ? nullptr : animation_->pose(recs[0]);
                const flecs::entity mesh = pose && skinning_ ? scene_->entity(skinning_->instanceOfRecord(recs[0])) : flecs::entity();
                const WorldTransform* w = mesh ? mesh.try_get<WorldTransform>() : nullptr;
                if (!readVec3(*v, "at", want) || !w || !animation_->aiming(recs[0], bone, axis) ||
                    !boneWorld(pose->skeleton()->name(bone), at, nullptr)) {
                    why += " not aiming";
                } else {
                    const float(*wm)[4] = rows3x4(w->rows);
                    const float3 f = anim::rotate(pose->model()[bone].r, axis);
                    const float3 forward = normalize(float3{wm[0][0] * f.x + wm[0][1] * f.y + wm[0][2] * f.z, wm[1][0] * f.x + wm[1][1] * f.y + wm[1][2] * f.z,
                                                            wm[2][0] * f.x + wm[2][1] * f.y + wm[2][2] * f.z});
                    const float3 to = normalize(float3{want[0], want[1], want[2]} - at);
                    const float degrees = std::acos(std::clamp(dot(forward, to), -1.0f, 1.0f)) * 57.29578f;
                    if (degrees > float(v->getNumber("within", 2.0)))
                        why += " " + std::string(pose->skeleton()->name(bone)) + " aims " + std::to_string(degrees) + " degrees away";
                }
            }
            // "bound": [the Animator's entity parameters bound, in order]; "entities": {name: the
            // entity's name it follows now ("": none; a script's over the Animator's)}; "ownIk": the
            // Animator has IK of its own (an older scene's).
            if (const Json* v = an->get("bound"); v && v->type == Json::Type::Array && i) {
                std::string got, want;
                for (const auto& [name, key] : s.animator ? s.animator->entities : decltype(s.animator->entities){})
                    got += (got.empty() ? "" : " ") + name;
                for (const Json& t : v->array)
                    want += (want.empty() ? "" : " ") + t.str;
                if (got != want)
                    why += " bound [" + got + "]";
            }
            if (const Json* v = an->get("entities"); v && v->isObject() && i)
                for (const auto& [name, want] : v->object) {
                    const uint64_t e = recs.empty() ? 0 : animation_->entity(recs[0], name);
                    std::string got;
                    for (uint32_t r = 0; e && r < records_.size(); ++r)
                        if (records_[r].entity == e)
                            got = records_[r].name;
                    if (got != want.str)
                        why += " entity " + name + " is '" + got + "'";
                }
            // "played": {above, below} seconds its graph has played since it started (edit mode: only
            // previewing Animators play); "editorPreview": the Animator's Preview in Editor.
            if (const Json* v = an->get("played"); v && v->isObject() && i) {
                const float t = recs.empty() ? 0.0f : animation_->playedTime(recs[0]);
                if (t <= float(v->getNumber("above", -1.0)) || t >= float(v->getNumber("below", 1e30)))
                    why += " played " + std::to_string(t) + " s";
            }
            if (const Json* v = an->get("editorPreview"); v && i && (recs.empty() || animation_->editorPreview(recs[0]) != v->b))
                why += std::string(" editor preview ") + (v->b ? "off" : "on");
            if (const Json* v = an->get("ownIk"); v && i && (s.animator && !s.animator->ik.empty()) != v->b)
                why += std::string(" own IK ") + (v->b ? "none" : "there");
            if (const Json* v = an->get("notAiming"); v && v->b && i) {
                uint32_t bone = 0;
                float3 axis;
                if (!recs.empty() && animation_->aiming(recs[0], bone, axis))
                    why += " still aiming";
            }
            // "footIk": foot IK on (the graph's or the Animator's own); "footIkWeight": its weight now.
            const auto feetOn = [&] {
                const anim::IkSettings* ik = recs.empty() ? nullptr : animation_->ikSettings(recs[0]);
                return ik && ik->feet.on;
            };
            if (const Json* v = an->get("footIk"); v && i && feetOn() != v->b)
                why += " foot IK " + std::string(v->b ? "off" : "on");
            if (const Json* v = an->get("footIkWeight"); v && i && (recs.empty() || std::fabs(animation_->footIkWeight(recs[0]) - float(v->number)) > 1e-3f))
                why += " foot IK weight " + std::to_string(recs.empty() ? 0.0f : animation_->footIkWeight(recs[0]));
            if (const Json* v = an->get("maxCharacters"); v && animation_->characterCount() > uint32_t(v->number))
                why += " " + std::to_string(animation_->characterCount()) + " characters";
            if (const Json* v = an->get("meshes"); v && i && uint32_t(v->number) != recs.size())
                why += " animator drives " + std::to_string(recs.size()) + " meshes";
            if (const Json* v = an->get("state"); v && i) {
                const int32_t n = g ? g->activeNode(g->definition().root) : -1;
                const auto& compiled = g ? g->definition().compiled : nullptr;
                const std::string id = n >= 0 && compiled && size_t(n) < compiled->nodeIds.size() ? compiled->nodeIds[n] : "";
                const size_t colon = id.rfind(':');
                const std::string got = colon == std::string::npos ? id : id.substr(colon + 1);
                if (got != v->str)
                    why += " anim state '" + got + "'";
            }
            const auto bones = [&](const char* key, const std::function<void(const std::string&)>& check) {
                if (const Json* v = an->get(key); v && v->type == Json::Type::Array && i)
                    for (const Json& b : v->array)
                        check(b.str);
            };
            bones("hidden", [&](const std::string& b) { if (recs.empty() || !animation_->boneHidden(recs[0], b)) why += " bone " + b + " not hidden"; });
            bones("shown", [&](const std::string& b) { if (!recs.empty() && animation_->boneHidden(recs[0], b)) why += " bone " + b + " hidden"; });
            // A bone in every mesh: -1 unknown, 1 collapsed to the eye, 0 not; "shrunk" when its
            // skinning matrix (the shadows') is not whole.
            const auto collapsed = [&](const std::string& b) {
                const anim::Pose* pose = recs.empty() || !skinning_ ? nullptr : animation_->pose(recs[0]);
                const int32_t k = pose ? pose->skeleton()->findBone(b.c_str()) : -1;
                if (k < 0)
                    return -1;
                int32_t seen = -2;
                for (const uint32_t r : recs) {
                    const float* m = skinning_->matrices(r) + size_t(k) * 12;
                    float scale = 0.0f;
                    for (int col = 0; col < 3; ++col)
                        scale = std::max(scale, std::sqrt(m[col] * m[col] + m[4 + col] * m[4 + col] + m[8 + col] * m[8 + col]));
                    if (scale < 0.5f)
                        why += " bone " + b + " shrunk";
                    const int32_t here = skinning_->hidden(r) && skinning_->collapse(r)[size_t(k) * 4 + 3] != 0.0f ? 1 : 0;
                    if (seen != -2 && here != seen)
                        why += " bone " + b + " collapsed in some meshes only";
                    seen = here;
                }
                return seen;
            };
            bones("collapsed", [&](const std::string& b) { if (collapsed(b) != 1) why += " bone " + b + " not collapsed"; });
            bones("whole", [&](const std::string& b) { if (collapsed(b) != 0) why += " bone " + b + " collapsed"; });
            if (const Json* p = an->get("params"); p && p->isObject() && i)
                for (const auto& [name, v] : p->object) {
                    const int32_t k = g ? g->parameterIndex(name.c_str()) : -1;
                    const float got = k < 0 ? NAN
                                      : g->parameterType(uint32_t(k)) == anim::fmt::NodeType::ParamBool ? (g->getBool(k) ? 1.0f : 0.0f)
                                                                                                        : g->getFloat(k);
                    const float want = v.type == Json::Type::Bool ? (v.b ? 1.0f : 0.0f) : float(v.number);
                    if (!(std::fabs(got - want) <= 1e-4f))
                        why += " anim param " + name + " = " + std::to_string(got);
                }
        }
        if (const Json* ex = m.get("entity")) {
            const uint32_t i = idByName(ex->getString("name"));
            if (i == 0) {
                why += " entity '" + ex->getString("name") + "' not found";
            } else {
                const State s = capture(i - 1);
                if (const Json* p = ex->get("parent"); p && (s.parent == ~0u ? "" : defaultName(s.parent)) != p->str)
                    why += " parent of '" + s.name + "' is '" + (s.parent == ~0u ? "" : defaultName(s.parent)) + "'";
                // "relative": {"to": name, "position": [x, y, z], "within"}: its position minus the other's.
                if (const Json* rel = ex->get("relative"); rel && rel->isObject())
                    if (const uint32_t other = idByName(rel->getString("to")); other) {
                        const State o = capture(other - 1);
                        float want[3];
                        if (readVec3(*rel, "position", want))
                            for (int k = 0; k < 3; ++k)
                                if (std::fabs(s.world[k][3] - o.world[k][3] - want[k]) > float(rel->getNumber("within", 0.01))) {
                                    why += " '" + s.name + "' relative to '" + rel->getString("to") + "'";
                                    break;
                                }
                    }
                // "movedFrom": [x, y, z]: at least "atLeast" (0.05) away from there.
                if (float from[3]; readVec3(*ex, "movedFrom", from)) {
                    const float d = std::sqrt((s.world[0][3] - from[0]) * (s.world[0][3] - from[0]) + (s.world[1][3] - from[1]) * (s.world[1][3] - from[1]) +
                                              (s.world[2][3] - from[2]) * (s.world[2][3] - from[2]));
                    if (d < float(ex->getNumber("atLeast", 0.05)))
                        why += " '" + s.name + "' moved only " + std::to_string(d);
                }
                if (const Json* v = ex->get("levelExtent>")) {  // its mesh's largest size along an axis
                    float3 lo = {}, hi = {};
                    if (s.levelMesh)
                        s.levelMesh->bounds(lo, hi);
                    const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
                    if (!s.levelMesh || !(extent > float(v->number)))
                        why += " '" + s.name + "' level extent " + std::to_string(extent);
                }
                if (const Json* v = ex->get("levelHeight>")) {
                    float3 lo = {}, hi = {};
                    if (s.levelMesh)
                        s.levelMesh->bounds(lo, hi);
                    if (!s.levelMesh || !(hi.y - lo.y > float(v->number)))
                        why += " '" + s.name + "' level height " + std::to_string(hi.y - lo.y);
                }
                // "levelTriangles", "levelHeight" (its mesh's own bounds, within 0.001): a level mesh.
                if (const Json* v = ex->get("levelTriangles"); v && (!s.levelMesh || s.levelMesh->triangleCount() != uint32_t(v->number)))
                    why += " '" + s.name + "' level triangles " + (s.levelMesh ? std::to_string(s.levelMesh->triangleCount()) : "(none)");
                if (const Json* v = ex->get("wall"); v && v->b != s.wall.has_value())
                    why += " '" + s.name + "' wall " + (s.wall ? "yes" : "no");
                if (const Json* v = ex->get("opening"); v && v->b != s.opening.has_value())
                    why += " '" + s.name + "' opening " + (s.opening ? "yes" : "no");
                if (const Json* v = ex->get("cut"); v && v->b != s.cut.has_value())
                    why += " '" + s.name + "' cut volume " + (s.cut ? "yes" : "no");
                // "cutShape", "cutSeed", "wallHeight", "openingWidth": their fields.
                if (const Json* v = ex->get("cutShape");
                    v && (!s.cut || v->str != level::kCutShapeNames[uint32_t(s.cut->shape) % level::kCutShapes]))
                    why += " '" + s.name + "' cut shape " + (s.cut ? level::kCutShapeNames[uint32_t(s.cut->shape) % level::kCutShapes] : "(none)");
                if (const Json* v = ex->get("cutSeed"); v && (!s.cut || s.cut->seed != uint32_t(v->number)))
                    why += " '" + s.name + "' cut seed " + (s.cut ? std::to_string(s.cut->seed) : "(none)");
                if (const Json* v = ex->get("wallHeight"); v && (!s.wall || std::fabs(s.wall->height - float(v->number)) > 1e-4f))
                    why += " '" + s.name + "' wall height " + (s.wall ? std::to_string(s.wall->height) : "(none)");
                // "roofStyle": its roof's ("": none).
                if (const Json* v = ex->get("roofStyle");
                    v && v->str != (s.roof ? level::kRoofStyleNames[uint32_t(s.roof->style) % level::kRoofStyles] : ""))
                    why += " '" + s.name + "' roof " + (s.roof ? level::kRoofStyleNames[uint32_t(s.roof->style) % level::kRoofStyles] : "(none)");
                if (const Json* v = ex->get("openingDistance"); v && (!s.opening || std::fabs(s.opening->distance - float(v->number)) > 1e-3f))
                    why += " '" + s.name + "' opening distance " + (s.opening ? std::to_string(s.opening->distance) : "(none)");
                if (const Json* v = ex->get("openingWidth"); v && (!s.opening || std::fabs(s.opening->width - float(v->number)) > 1e-4f))
                    why += " '" + s.name + "' opening width " + (s.opening ? std::to_string(s.opening->width) : "(none)");
                if (const Json* v = ex->get("openingSill"); v && (!s.opening || std::fabs(s.opening->sill - float(v->number)) > 1e-3f))
                    why += " '" + s.name + "' opening sill " + (s.opening ? std::to_string(s.opening->sill) : "(none)");
                // "clipped": drawn clipped (not closed: cut without caps).
                if (const Json* v = ex->get("clipped"); v && v->b != clippedOf(i - 1))
                    why += " '" + s.name + "' clipped " + (v->b ? "no" : "yes");
                // "levelDrawnTriangles" / "levelDrawnTriangles>": its live mesh's (cut, or as it is).
                for (const char* key : {"levelDrawnTriangles", "levelDrawnTriangles>"})
                    if (const Json* v = ex->get(key); v && levelMeshes_ && renderer_) {
                        const int32_t live = levelMeshes_->liveOf(entityOf(i - 1).id());
                        const uint32_t tris = live >= 0 ? renderer_->live().triangles(uint32_t(live)) : 0u;
                        if (key[19] == '>' ? !(tris > uint32_t(v->number)) : tris != uint32_t(v->number))
                            why += " '" + s.name + "' drawn triangles " + std::to_string(tris);
                    }
                // "levelLayered": its mesh has layers; "levelMask": it has a painted mask.
                if (const Json* v = ex->get("levelLayered"); v && v->b != (s.levelMesh && s.levelMesh->layered()))
                    why += " '" + s.name + "' layered " + (s.levelMesh && s.levelMesh->layered() ? "yes" : "no");
                if (const Json* v = ex->get("levelMask"); v && v->b != bool(s.levelMask))
                    why += " '" + s.name + "' mask " + (s.levelMask ? "yes" : "no");
                // "levelMaskMean>" / "levelMaskMean<": [channel, value]: the mask channel's mean (0..1).
                for (const char* key : {"levelMaskMean>", "levelMaskMean<"})
                    if (const Json* v = ex->get(key); v && v->array.size() == 2) {
                        double sum = 0.0;
                        const size_t ch = std::min<size_t>(size_t(v->array[0].number), 3);
                        if (s.levelMask)
                            for (size_t i = ch; i < s.levelMask->size(); i += 4)
                                sum += (*s.levelMask)[i];
                        const double mean = s.levelMask && !s.levelMask->empty() ? sum / 255.0 / double(s.levelMask->size() / 4) : 0.0;
                        if (key[13] == '>' ? !(mean > v->array[1].number) : !(mean < v->array[1].number))
                            why += " '" + s.name + "' mask mean " + std::to_string(mean);
                    }
                // "volume": its gameplay volume's kind ("": none).
                if (const Json* v = ex->get("volume"); v && v->str != (s.volume ? kVolumeKindNames[uint32_t(s.volume->kind) % kVolumeKinds] : ""))
                    why += " '" + s.name + "' volume " + (s.volume ? kVolumeKindNames[uint32_t(s.volume->kind) % kVolumeKinds] : "(none)");
                // "splinePoints": its spline's point count; "splineClosed"; "splineProfile".
                if (const Json* v = ex->get("splinePoints"); v && uint32_t(v->number) != (s.spline ? uint32_t(s.spline->spline.points.size()) : 0u))
                    why += " '" + s.name + "' spline points " + std::to_string(s.spline ? s.spline->spline.points.size() : 0);
                if (const Json* v = ex->get("splineClosed"); v && (!s.spline || v->b != s.spline->spline.closed))
                    why += " '" + s.name + "' spline closed " + (s.spline && s.spline->spline.closed ? "yes" : "no");
                if (const Json* v = ex->get("splineProfile"); v && (!s.spline || v->str != s.spline->loft.profile))
                    why += " '" + s.name + "' spline profile " + (s.spline ? s.spline->loft.profile : "(none)");
                for (const char* key : {"splineLength>", "splineLength<"})  // (metres, in its own space)
                    if (const Json* v = ex->get(key)) {
                        const float len = s.spline ? level::splineLength(s.spline->spline) : 0.0f;
                        if (key[12] == '>' ? !(len > float(v->number)) : !(len < float(v->number)))
                            why += " '" + s.name + "' spline length " + std::to_string(len);
                    }
                // "modifiers": its modifier stack's size; "modifierTitles": theirs in order ("Mirror X", "Array"...).
                if (const Json* v = ex->get("modifiers"); v && uint32_t(v->number) != (s.modifiers ? uint32_t(s.modifiers->stack.size()) : 0u))
                    why += " '" + s.name + "' modifiers " + std::to_string(s.modifiers ? s.modifiers->stack.size() : 0);
                if (const Json* v = ex->get("modifierTitles")) {
                    std::string want, have;
                    for (const Json& t : v->array)
                        want += t.str + ";";
                    if (s.modifiers)
                        for (const level::Modifier& mod : s.modifiers->stack)
                            have += level::modifierTitle(mod) + ";";
                    if (want != have)
                        why += " '" + s.name + "' modifiers " + have;
                }
                // "levelWear": its live wear's rule count; "levelWearMean>" / "levelWearMean<": [channel,
                // value]: what it made (the channel's mean, 0..1).
                if (const Json* v = ex->get("levelWear"); v && uint32_t(v->number) != (s.wear ? uint32_t(s.wear->rules.size()) : 0u))
                    why += " '" + s.name + "' live wear rules " + std::to_string(s.wear ? s.wear->rules.size() : 0);
                for (const char* key : {"levelWearMean>", "levelWearMean<"})
                    if (const Json* v = ex->get(key); v && v->array.size() == 2) {
                        const std::vector<uint8_t>* made = levelMeshes_ ? levelMeshes_->wearOf(entityOf(i - 1).id()) : nullptr;
                        double sum = 0.0;
                        const size_t ch = std::min<size_t>(size_t(v->array[0].number), 3);
                        if (made)
                            for (size_t k = ch; k < made->size(); k += 4)
                                sum += (*made)[k];
                        const double mean = made ? sum / 255.0 / double(made->size() / 4) : 0.0;
                        if (key[13] == '>' ? !(mean > v->array[1].number) : !(mean < v->array[1].number))
                            why += " '" + s.name + "' live wear mean " + std::to_string(mean);
                    }
                // "levelTinted>": more vertices than that with a colour (not white).
                if (const Json* v = ex->get("levelTinted>")) {
                    uint32_t n = 0;
                    if (s.levelMesh)
                        for (const uint32_t c : s.levelMesh->vertexColors)
                            n += c != 0xffffffffu;
                    if (!(double(n) > v->number))
                        why += " '" + s.name + "' tinted vertices " + std::to_string(n);
                }
                // "levelFaces": {"material": path, "count": n}: n faces with that material ("" the default);
                // with "mode" ("local", "world", "manual"), "offsetU>", "scale" ([u, v], within 0.001) and
                // "vWithin" ([a, b]) too: faces with all of them.
                if (const Json* v = ex->get("levelFaces"); v && v->isObject()) {
                    uint32_t n = 0;
                    static const char* const kModes[] = {"local", "world", "manual"};
                    if (s.levelMesh)
                        for (const level::Face& f : s.levelMesh->faces) {
                            const std::string mat = f.material < s.levelMesh->materials.size() ? s.levelMesh->materials[f.material] : "";
                            n += (!v->get("material") || v->getString("material") == mat) &&
                                 (!v->get("mode") || v->getString("mode") == kModes[uint32_t(f.uv.mode) % 3]) &&
                                 (!v->get("offsetU>") || f.uv.offset[0] > float(v->getNumber("offsetU>"))) &&
                                 (!v->get("scale") || (v->get("scale")->array.size() == 2 &&
                                                       std::fabs(f.uv.scale[0] - float(v->get("scale")->array[0].number)) < 1e-3f &&
                                                       std::fabs(f.uv.scale[1] - float(v->get("scale")->array[1].number)) < 1e-3f)) &&
                                 (!v->get("vWithin") || [&] {  // (every corner's v inside [a, b])
                                     const Json& r = *v->get("vWithin");
                                     const uint32_t index = uint32_t(&f - s.levelMesh->faces.data());
                                     for (const float2 uv : level::faceUvs(*s.levelMesh, index))
                                         if (r.array.size() != 2 || uv.y < float(r.array[0].number) - 1e-4f || uv.y > float(r.array[1].number) + 1e-4f)
                                             return false;
                                     return true;
                                 }());
                        }
                    if (n != uint32_t(v->getNumber("count", 0)))
                        why += " '" + s.name + "' level faces matching: " + std::to_string(n);
                }
                if (const Json* v = ex->get("levelHeight")) {
                    float3 lo = {}, hi = {};
                    if (s.levelMesh)
                        s.levelMesh->bounds(lo, hi);
                    if (!s.levelMesh || std::fabs(hi.y - lo.y - float(v->number)) > 0.001f)
                        why += " '" + s.name + "' level height " + std::to_string(hi.y - lo.y);
                }
                if (const Json* a = ex->get("alive"); a && a->b != s.alive)
                    why += " alive";
                // "local": [12] its transform under its parent (3x4 rows), within "within" (0.001).
                if (const Json* l = ex->get("local"); l && l->type == Json::Type::Array && l->array.size() == 12) {
                    float d = s.hasLocal ? 0.0f : 1e9f;
                    for (int k = 0; k < 12 && s.hasLocal; ++k)
                        d = std::max(d, std::fabs(s.local[k / 4][k % 4] - float(l->array[size_t(k)].number)));
                    if (d > float(ex->getNumber("within", 0.001))) {
                        why += " '" + s.name + "' local";
                        for (int k = 0; k < 12 && s.hasLocal; ++k)
                            why += " " + std::to_string(s.local[k / 4][k % 4]);
                    }
                }
                if (float c[3]; readVec3(*ex, "lightColor", c)) {
                    if (!s.light)
                        why += " '" + s.name + "' is not a light";
                    else
                        for (int k = 0; k < 3; ++k)
                            if (std::fabs(s.light->color[k] - c[k]) > 0.05f)
                                why += " lightColor[" + std::to_string(k) + "] " + std::to_string(s.light->color[k]);
                }
                if (const Json* t = ex->get("lightType"); t && (!s.light || lightTypeName(s.light->type) != t->str))
                    why += " lightType of '" + s.name + "'";
                // "bottom": its mesh's lowest point in the world (y, within 0.05); "children": how many below it.
                if (const Json* b = ex->get("bottom")) {
                    float3 c, h;
                    const uint32_t slot = records_[i - 1].slot;
                    if (slot == ~0u || !meshBoundsOf(pak_->instances()[slot].mesh, c, h)) {
                        why += " '" + s.name + "' has no mesh";
                    } else {
                        float lowest = 1e30f;
                        for (int corner = 0; corner < 8; ++corner) {
                            const float p[3] = {corner & 1 ? c.x + h.x : c.x - h.x, corner & 2 ? c.y + h.y : c.y - h.y, corner & 4 ? c.z + h.z : c.z - h.z};
                            lowest = std::min(lowest, s.world[1][0] * p[0] + s.world[1][1] * p[1] + s.world[1][2] * p[2] + s.world[1][3]);
                        }
                        if (std::fabs(lowest - float(b->number)) > 0.05f)
                            why += " bottom of '" + s.name + "' at " + std::to_string(lowest);
                    }
                }
                // Its instance row as drawn: "rowHidden", "rowShadow" (casts), "rowTint" ([r, g, b, a] bytes),
                // "rowMaterial" (drawn with a material of its own).
                if (const uint32_t slot = records_[i - 1].slot;
                    ex->get("rowHidden") || ex->get("rowShadow") || ex->get("rowTint") || ex->get("rowMaterial")) {
                    if (slot == ~0u) {
                        why += " '" + s.name + "' has no row";
                    } else {
                        const fpak::InstanceGpu& row = pak_->instances()[slot];
                        if (const Json* v = ex->get("rowHidden"); v && v->b != ((row.flags & fpak::kInstanceHidden) != 0))
                            why += " '" + s.name + "' row " + ((row.flags & fpak::kInstanceHidden) ? "hidden" : "shown");
                        if (const Json* v = ex->get("rowShadow"); v && v->b != !(row.flags & fpak::kInstanceNoShadow))
                            why += " '" + s.name + "' row " + ((row.flags & fpak::kInstanceNoShadow) ? "casts no shadow" : "casts shadows");
                        if (const Json* v = ex->get("rowTint"); v && v->type == Json::Type::Array && v->array.size() == 4) {
                            uint32_t want = 0;  // [r, g, b, a] bytes ([0, 0, 0, 0]: none)
                            for (size_t k = 0; k < 4; ++k)
                                want |= uint32_t(v->array[k].number) << (8 * k);
                            if (want != row.tint)
                                why += " '" + s.name + "' row tint " + std::to_string(row.tint);
                        }
                        if (const Json* v = ex->get("rowMaterial"); v && v->b != ((row.flags >> fpak::kInstanceMaterialShift) != 0))
                            why += " '" + s.name + "' row material " + std::to_string(row.flags >> fpak::kInstanceMaterialShift);
                    }
                }
                // (X2) "data": {component: {field: value or [values]} or null (it has none)}.
                if (const Json* v = ex->get("data"); v && v->isObject()) {
                    const std::vector<DataComponent> data = captureData(entityOf(i - 1));
                    for (const auto& [cname, want] : v->object) {
                        const auto it = std::find_if(data.begin(), data.end(), [&](const DataComponent& d) { return d.name == cname; });
                        if ((it != data.end()) != want.isObject()) {
                            why += " '" + s.name + "' " + (it != data.end() ? "has " : "lacks ") + cname;
                            continue;
                        }
                        const Json* fields = it != data.end() && dataType(cname) ? dataType(cname)->get("fields") : nullptr;
                        for (const auto& [fname, fwant] : want.object) {
                            std::string got;
                            for (const Json& f : fields ? fields->array : std::vector<Json>{})
                                if (f.getString("name") == fname) {
                                    uint32_t lanes, bytes;
                                    const std::string kind = f.getString("kind");
                                    dataKind(kind, lanes, bytes);
                                    const uint32_t offset = uint32_t(f.getNumber("offset"));
                                    for (uint32_t k = 0; bytes && k < lanes && offset + (k + 1) * bytes <= it->bytes.size(); ++k)
                                        got += (k ? "," : "") + Json::makeNumber(readDataLane(kind, it->bytes.data() + offset + k * bytes)).dump(0);
                                }
                            std::string expect;
                            if (fwant.type == Json::Type::Array)
                                for (size_t k = 0; k < fwant.array.size(); ++k)
                                    expect += (k ? "," : "") + fwant.array[k].dump(0);
                            else
                                expect = fwant.type == Json::Type::Bool ? (fwant.b ? "1" : "0") : fwant.dump(0);
                            if (got != expect)
                                why += " '" + s.name + "' " + cname + "." + fname + " is " + got;
                        }
                    }
                }
                // "scripts": [class names] its script components; "scriptProps": {class: {name: value}} their saved values.
                if (const Json* v = ex->get("scripts"); v && v->type == Json::Type::Array) {
                    std::string want, got;
                    for (const Json& c : v->array)
                        want += c.str + ",";
                    for (const ScriptEntry& e : records_[i - 1].scripts)
                        got += e.className + ",";
                    if (want != got)
                        why += " '" + s.name + "' scripts " + got;
                }
                if (const Json* v = ex->get("scriptProps"); v && v->isObject())
                    for (const auto& [cls, props] : v->object) {
                        const auto it = std::find_if(records_[i - 1].scripts.begin(), records_[i - 1].scripts.end(),
                                                     [&](const ScriptEntry& e) { return e.className == cls; });
                        Json saved;
                        try {
                            saved = Json::parse(it != records_[i - 1].scripts.end() ? it->properties : "{}");
                        } catch (const std::exception&) {
                        }
                        for (const auto& [key, value] : props.object)
                            if (const Json* sv = saved.get(key.c_str()); !sv || sv->dump(0) != value.dump(0))
                                why += " '" + s.name + "' " + cls + "." + key + " is " + (sv ? sv->dump(0) : "unset");
                    }
                if (const Json* v = ex->get("prefab")) {  // the instance's prefab ("": none)
                    const Record& r = records_[i - 1];
                    const std::string got = r.prefabRoot != ~0u ? records_[r.prefabRoot].prefab : "";
                    if (got != v->str)
                        why += " prefab of '" + s.name + "' is '" + got + "'";
                }
                if (const Json* v = ex->get("overridden")) {
                    const Record& r = records_[i - 1];
                    bool overridden = false;
                    if (const Prefab* def = r.prefabRoot != ~0u ? prefabDef(records_[r.prefabRoot].prefab) : nullptr)
                        for (const PrefabOverride& o : overridesOf(r.prefabRoot, *def))
                            overridden |= o.id == r.prefabChild;
                    if (overridden != v->b)
                        why += " '" + s.name + "' overridden " + (overridden ? "true" : "false");
                }
                // "has" / "lacks": flecs component names it carries or not ("Light", "CameraComponent").
                for (const char* key : {"has", "lacks"})
                    if (const Json* v = ex->get(key))
                        for (const Json& c : v->array) {
                            const flecs::entity ce = scene_->world().lookup(c.str.c_str());
                            const bool has = ce && entityOf(i - 1).has(ce);
                            if (has != (key[0] == 'h'))
                                why += " '" + s.name + "' " + (has ? "has " : "lacks ") + c.str;
                        }
                if (const Json* v = ex->get("at"); v && v->array.size() == 3) {  // its world position (within "within", 0.01)
                    float d = 0.0f;
                    for (int k = 0; k < 3; ++k)
                        d = std::max(d, std::fabs(s.world[k][3] - float(v->array[size_t(k)].number)));
                    if (d > float(ex->getNumber("within", 0.01)))
                        why += " '" + s.name + "' at " + std::to_string(s.world[0][3]) + " " + std::to_string(s.world[1][3]) + " " +
                               std::to_string(s.world[2][3]);
                }
                if (const Json* v = ex->get("yAbove"); v && !(s.world[1][3] > float(v->number)))
                    why += " '" + s.name + "' y " + std::to_string(s.world[1][3]) + " not above " + std::to_string(v->number);
                if (const Json* v = ex->get("yBelow"); v && !(s.world[1][3] < float(v->number)))
                    why += " '" + s.name + "' y " + std::to_string(s.world[1][3]) + " not below " + std::to_string(v->number);
                if (const Json* v = ex->get("mass"); v && (!s.body || std::fabs(s.body->mass - float(v->number)) > 1e-4f))
                    why += " mass of '" + s.name + "'";
                // Particles: "particles" (how many its effect has now: exactly, or "particles>" / "particles<"),
                // "particleMinY>" / "particleMaxY<" (every one above / below a height), and "vfx": {component key: {field: value}} (as saved).
                if (ex->get("particles") || ex->get("particles>") || ex->get("particles<") || ex->get("particleMinY>") || ex->get("particleMaxY<")) {
                    const ParticleSystem::Effect* fx = particles_ ? particles_->effectOf(entityOf(i - 1)) : nullptr;
                    const double n = fx ? double(fx->particles.size()) : -1.0;
                    float minY = 1e30f, maxY = -1e30f;
                    for (const Particle& p : fx ? fx->particles : std::vector<Particle>()) {
                        minY = std::min(minY, p.position[1]);
                        maxY = std::max(maxY, p.position[1]);
                    }
                    if (const Json* v = ex->get("particles"); v && n != v->number)
                        why += " '" + s.name + "' has " + std::to_string(int(n)) + " particles";
                    if (const Json* v = ex->get("particles>"); v && !(n > v->number))
                        why += " '" + s.name + "' has " + std::to_string(int(n)) + " particles";
                    if (const Json* v = ex->get("particles<"); v && !(n >= 0.0 && n < v->number))
                        why += " '" + s.name + "' has " + std::to_string(int(n)) + " particles";
                    if (const Json* v = ex->get("particleMinY>"); v && !(minY > float(v->number)))
                        why += " '" + s.name + "' has a particle at y " + std::to_string(minY);
                    if (const Json* v = ex->get("particleMaxY<"); v && !(maxY < float(v->number)))
                        why += " '" + s.name + "' has a particle at y " + std::to_string(maxY);
                }
                if (const Json* v = ex->get("vfx"); v && v->isObject())
                    for (const auto& [key, fields] : v->object) {
                        const int32_t c = vfxComponentIndex(key);
                        const void* data = c >= 0 ? s.vfx.find(uint32_t(c)) : nullptr;
                        if (!data) {
                            why += " '" + s.name + "' has no " + key;
                            continue;
                        }
                        uint32_t count = 0;
                        const VfxComponentInfo& info = vfxComponents(count)[c];
                        JsonWriter cw;
                        cw.beginObject();
                        writeVfxComponent(cw, info, data, true);
                        cw.endObject();
                        const Json got = Json::parse(cw.str());
                        for (const auto& [field, want] : fields.object) {
                            const Json* have = got.get(field.c_str());
                            if (!have || !jsonNear(*have, want))
                                why += " " + key + "." + field + " of '" + s.name + "' is " + (have ? have->dump(-1) : std::string("missing"));
                        }
                    }
                if (const Json* v = ex->get("sound"); v && (!s.audioSource || soundPath(s.audioSource->sound) != v->str))
                    why += " sound of '" + s.name + "' is '" + (s.audioSource ? soundPath(s.audioSource->sound) : std::string("none")) + "'";
                if (const Json* v = ex->get("collider"); v && (!s.collider || v->str != colliderShapeName(s.collider->shape)))
                    why += " collider of '" + s.name + "' is " + (s.collider ? colliderShapeName(s.collider->shape) : "none");
                if (const Json* v = ex->get("material"); v && records_[i - 1].material != v->str)
                    why += " material of '" + s.name + "' is '" + records_[i - 1].material + "'";
                // "drawnMesh": the mesh its slot draws (a displaced copy for its material, or its own).
                if (const Json* v = ex->get("drawnMesh")) {
                    const uint32_t slot = records_[i - 1].slot;
                    const std::string drawn = slot == ~0u ? std::string() : pak_->meshName(pak_->mutableInstances()[slot].mesh);
                    if (drawn != v->str)
                        why += " '" + s.name + "' draws '" + drawn + "'";
                }
                for (const char* key : {"children>", "children<"})
                    if (const Json* n = ex->get(key)) {
                        uint32_t count = 0;
                        for (uint32_t k = 0; k < records_.size(); ++k)
                            count += parentOf(k) == i - 1 && capture(k).alive;
                        if (key[8] == '>' ? !(count > uint32_t(n->number)) : !(count < uint32_t(n->number)))
                            why += " '" + s.name + "' has " + std::to_string(count) + " children";
                    }
                if (const Json* n = ex->get("children")) {
                    uint32_t count = 0;
                    for (uint32_t k = 0; k < records_.size(); ++k)
                        count += parentOf(k) == i - 1 && capture(k).alive;
                    if (count != uint32_t(n->number))
                        why += " '" + s.name + "' has " + std::to_string(count) + " children";
                }
                if (float pos[3]; readVec3(*ex, "position", pos))
                    for (int k = 0; k < 3; ++k)
                        if (std::fabs(s.world[k][3] - pos[k]) > 1e-3f)
                            why += " position[" + std::to_string(k) + "] " + std::to_string(s.world[k][3]);
            }
        }
        if (float pos[3]; view_ && readVec3(m, "viewPosition", pos)) {  // the camera the view is drawn from
            const float3 at = view_->viewPosition();
            if (std::fabs(at.x - pos[0]) + std::fabs(at.y - pos[1]) + std::fabs(at.z - pos[2]) > 1e-3f)
                why += " viewPosition (" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", " + std::to_string(at.z) + ")";
        }
        // {"viewMoved": {"from": [xyz], "along": [xyz]}}: the camera went at least 0.1 m that way.
        if (const Json* v = m.get("viewMoved"); v && view_) {
            float from[3], along[3];
            if (readVec3(*v, "from", from) && readVec3(*v, "along", along)) {
                const float3 at = view_->viewPosition();
                const float d = dot(at - float3{from[0], from[1], from[2]}, normalize(float3{along[0], along[1], along[2]}));
                if (d < 0.1f)
                    why += " viewMoved (" + std::to_string(d) + " m that way)";
            }
        }
        if (const Json* v = m.get("tool"); v && view_) {
            static const char* const kTools[] = {"view", "move", "rotate", "scale", "brush", "paint", "spline", "measure"};
            if (v->str != kTools[uint32_t(view_->tools().tool)])
                why += " tool";
        }
        // {"terrain": {"exists", "x", "z" (or "atStroke": where the last brush stroke began),
        //              "height" (within "tolerance", default 0.01), or ">" / "<" a height}}
        if (const Json* t = m.get("terrain")) {
            const bool exists = terrain_ && terrain_->valid();
            if (const Json* e = t->get("exists"); e && e->b != exists)
                why += " terrain.exists";
            if (const Json* g = t->get("generating"); g && terrain_ && g->b != terrain_->generating())
                why += " terrain.generating";
            const bool atStroke = t->getBool("atStroke");
            if (exists && (t->get("x") || atStroke)) {
                const float3 s = terrain_->lastStrokeStart();
                const float x = atStroke ? s.x : float(t->getNumber("x")), z = atStroke ? s.z : float(t->getNumber("z"));
                const float h = terrain_->terrain()->heightAt(x, z);
                FX_INFO("editor test: terrain height at %.2f, %.2f: %.3f", double(x), double(z), double(h));
                const float tol = float(t->getNumber("tolerance", 0.01));
                if (const Json* v = t->get("height"); v && std::fabs(h - float(v->number)) > tol)
                    why += " terrain height " + std::to_string(h);
                if (const Json* v = t->get(">"); v && !(h > float(v->number)))
                    why += " terrain height " + std::to_string(h) + " not > " + std::to_string(v->number);
                if (const Json* v = t->get("<"); v && !(h < float(v->number)))
                    why += " terrain height " + std::to_string(h) + " not < " + std::to_string(v->number);
                // "layer": the sample's strongest layer; "hole": its cell is cut out.
                const TerrainSettings& ts = terrain_->terrain()->settings();
                const uint32_t mat = terrain_->terrain()->material(int32_t(std::lround((x - ts.originX) / ts.spacing)),
                                                                 int32_t(std::lround((z - ts.originZ) / ts.spacing)));
                const uint32_t strongest = terrain::materialWeightB(mat) > 127 ? terrain::materialB(mat) : terrain::materialA(mat);
                if (const Json* v = t->get("layer"); v && uint32_t(v->number) != strongest)
                    why += " terrain layer " + std::to_string(strongest);
                if (const Json* v = t->get("hole"); v && v->b != ((mat & terrain::kHole) != 0))
                    why += " terrain hole";
                // "scatterMask": n, the sample is in scatter layer n's painted mask ("scatterMaskClear": not).
                if (const Json* v = t->get("scatterMask"); v && !(mat & (terrain::kScatterMask << uint32_t(v->number))))
                    why += " terrain scatter mask";
                if (const Json* v = t->get("scatterMaskClear"); v && (mat & (terrain::kScatterMask << uint32_t(v->number))))
                    why += " terrain scatter mask set";
            }
            // "scatter": {"picked", "deleted" (instances in all layers), "cells" > 0, "layers",
            // "materialLayers" (the terrain's material layers), "subs" (layer meshes drawn),
            // "unplaced" (layer meshes without slots), "meshes" (layer 0's), "yawRange", "jitter" (layer 0's),
            // "nowhere": n (no ground fits layer n's slope and height rules)}
            if (const Json* sc = t->get("scatter"); sc && exists) {
                const size_t layers = parseScatterLayers(terrain_->terrain()->scatterJson()).size();
                const size_t materials = parseTerrainLayers(terrain_->terrain()->layersJson()).size();
                if (const Json* v = sc->get("layers"); v && size_t(v->number) != layers)
                    why += " scatter layers " + std::to_string(layers);
                if (const Json* v = sc->get("materialLayers"); v && size_t(v->number) != materials)
                    why += " material layers " + std::to_string(materials);
                if (const Json* v = sc->get("picked"); v && v->b != terrain_->scatterPicked())
                    why += " scatter picked";
                if (const Json* v = sc->get("deleted"); v && uint32_t(v->number) != terrain_->scatterExclusions())
                    why += " scatter deleted " + std::to_string(terrain_->scatterExclusions());
                if (sc->getBool("cells") && renderer_->scatter().stats().cells == 0)
                    why += " scatter cells";
                if (sc->getBool("noCells") && renderer_->scatter().stats().cells != 0)  // (painted layers with nothing painted)
                    why += " scatter cells " + std::to_string(renderer_->scatter().stats().cells);
                const ScatterStats& st = renderer_->scatter().stats();
                if (const Json* v = sc->get("subs"); v && uint32_t(v->number) != st.subs)
                    why += " scatter subs " + std::to_string(st.subs);
                if (const Json* v = sc->get("unplaced"); v && uint32_t(v->number) != st.unplaced)
                    why += " scatter unplaced " + std::to_string(st.unplaced);
                const std::vector<ScatterLayer> all = parseScatterLayers(terrain_->terrain()->scatterJson());
                if (const Json* v = sc->get("meshes"); v && (all.empty() || all[0].assets.size() != size_t(v->number)))
                    why += " scatter meshes " + std::to_string(all.empty() ? 0 : all[0].assets.size());
                if (const Json* v = sc->get("yawRange"); v && (all.empty() || std::fabs(all[0].yawRange - v->number) > 1e-3))
                    why += " scatter yawRange";
                if (const Json* v = sc->get("jitter"); v && (all.empty() || std::fabs(all[0].jitter - v->number) > 1e-3))
                    why += " scatter jitter";
                if (const Json* v = sc->get("nowhere"); v && (size_t(v->number) >= all.size() || terrain_->scatterGround(all[size_t(v->number)]) > 0.0f))
                    why += " scatter nowhere";
            }
        }
        if (!why.empty()) {
            FX_ERROR("editor test: expectation failed:%s", why.c_str());
            testFailed_ = true;
        }
    }
}

// A test.mouse / test.wheel target in window coordinates: the rect of selector "at" from the
// last probe, at fractions (fx, fy) of it (default: its centre).
bool EditorBridge::probePoint(const Json& m, float& x, float& y)
{
    const std::string at = m.getString("at");
    if (at.starts_with("@splinePoint:")) {  // the selected spline's point N (and "dx", "dy" pixels from it)
        std::vector<float3> points;
        const size_t k = size_t(std::strtoul(at.c_str() + 13, nullptr, 10));
        if (view_ && selected_ != ~0u && splineWorld(selected_, &points) && k < points.size() &&
            view_->worldWindowPoint(points[k], float(m.getNumber("dx", 0)), float(m.getNumber("dy", 0)), x, y))
            return true;
        FX_ERROR("editor test: no spline point %s to aim at", at.c_str() + 13);
        testFailed_ = true;
        return false;
    }
    if (at == "@gizmo") {  // the transform gizmo's centre, offset by "dx", "dy" view pixels
        if (view_ && view_->gizmoWindowPoint(float(m.getNumber("dx", 0)), float(m.getNumber("dy", 0)), x, y))
            return true;
        FX_ERROR("editor test: no gizmo to aim at");
        testFailed_ = true;
        return false;
    }
    const Json probe = lastProbe_.empty() ? Json() : Json::parse(lastProbe_);
    const Json* rects = probe.get("rects");
    const Json* rect = rects ? rects->get(at.c_str()) : nullptr;
    if (!rect || rect->array.size() != 4) {
        FX_ERROR("editor test: no probed rect for '%s' (test.probe it first)", at.c_str());
        testFailed_ = true;
        return false;
    }
    const auto& r = rect->array;
    ui_->pageToWindow(float(r[0].number + r[2].number * m.getNumber("fx", 0.5)),
                      float(r[1].number + r[3].number * m.getNumber("fy", 0.5)), x, y);
    return true;
}

// test.expect {probe: {key: value}}: each key compared with the last probe. A key ending in
// '>' or '<' compares numbers ("hierarchyScroll>": 0); others must be equal.
std::string EditorBridge::probeMismatch(const Json& expected) const
{
    if (lastProbe_.empty())
        return " probe (no editor.probed received)";
    const Json probe = Json::parse(lastProbe_);
    std::string why;
    for (const auto& [rawKey, want] : expected.object) {
        std::string key = rawKey;
        const char op = !key.empty() && (key.back() == '>' || key.back() == '<') ? key.back() : '=';
        if (op != '=')
            key.pop_back();
        const Json* got = probe.get(key.c_str());
        bool ok = got && got->type == want.type;
        if (ok && want.type == Json::Type::Number)
            ok = op == '>' ? got->number > want.number : op == '<' ? got->number < want.number
                                                                   : std::abs(got->number - want.number) < 1e-6;
        else if (ok)
            ok = want.type == Json::Type::String ? got->str == want.str : want.type != Json::Type::Bool || got->b == want.b;
        if (!ok)
            why += " probe." + rawKey + " (got " + (got ? got->dump(0) : std::string("nothing")) + ")";
    }
    return why;
}

bool EditorBridge::loadScript(const char* path)
{
    size_t size = 0;
    void* text = SDL_LoadFile(path, &size);
    if (!text) {
        FX_ERROR("editor: cannot read script %s", path);
        return false;
    }
    try {
        const Json list = Json::parse(std::string_view(static_cast<const char*>(text), size));
        SDL_free(text);
        // Re-serialize each step: the bridge consumes messages as the page sends them.
        for (const Json& step : list.array)
            script_.push_back(step.dump(0));
    } catch (const std::exception& e) {
        FX_ERROR("editor: script %s: %s", path, e.what());
        return false;
    }
    return true;
}

void EditorBridge::startPlay()
{
    playSnapshot_.clear();
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (editable(i))
            playSnapshot_.emplace_back(i, capture(i));
    playing_ = true;
    paused_ = false;
    if (view_)
        view_->setCaptured(false);  // (an editor code's modal operation ends)
    if (animation_)
        animation_->reset();  // (characters previewing in the editor start again from the start)
    notice(game_.loaded ? "Playing: the game, physics and animation run; Stop restores the scene"
                        : "Playing: physics and animation run; Stop restores the scene");
    sendState(false);
}

void EditorBridge::restorePlay()
{
    if (playSnapshot_.empty())
        return;
    // Every entity the game (or the animation, or physics) changed back: moved, its light or
    // camera edited, destroyed (the engine has enabled it again). A played model's thousands of
    // parts: one propagate, then each one's own row; per entity, both walk the scene.
    const auto changed = [](const State& now, const State& before) {
        return std::memcmp(now.world, before.world, sizeof(now.world)) != 0 || now.alive != before.alive ||
               now.light.has_value() != before.light.has_value() || now.camera.has_value() != before.camera.has_value() ||
               (now.light && std::memcmp(&*now.light, &*before.light, sizeof(Light)) != 0) ||
               (now.camera && std::memcmp(&*now.camera, &*before.camera, sizeof(CameraComponent)) != 0) ||
               now.water.has_value() != before.water.has_value() || now.body.has_value() != before.body.has_value() ||
               now.collider.has_value() != before.collider.has_value() || now.audioSource.has_value() != before.audioSource.has_value() ||
               now.audioListener.has_value() != before.audioListener.has_value() || now.acoustics.has_value() != before.acoustics.has_value() ||
               now.animator != before.animator || !(now.vfx == before.vfx) || now.scripts != before.scripts ||
               now.data != before.data || now.levelMesh != before.levelMesh || now.levelMask != before.levelMask || now.cut != before.cut || now.wear != before.wear || now.modifiers != before.modifiers || now.spline != before.spline || now.scatter != before.scatter || now.snap != before.snap || now.volume != before.volume ||
               now.wall != before.wall || now.opening != before.opening || now.roof != before.roof;
    };
    // Parents before their children: a child put back by its world before its parent is (a grip
    // made before the target it hangs under) would keep its place against where the game left the
    // parent, and then go wherever the parent goes back to.
    std::unordered_map<uint32_t, uint32_t> parentAtPlay;
    for (const auto& [s, state] : playSnapshot_)
        parentAtPlay[s] = state.parent;
    std::unordered_map<uint32_t, uint32_t> depth;
    for (const auto& [s, state] : playSnapshot_) {
        uint32_t d = 0;
        for (auto it = parentAtPlay.find(s); it != parentAtPlay.end() && it->second != ~0u && d < 256; it = parentAtPlay.find(it->second))
            ++d;
        depth[s] = d;
    }
    std::stable_sort(playSnapshot_.begin(), playSnapshot_.end(), [&](const auto& a, const auto& b) { return depth[a.first] < depth[b.first]; });
    std::vector<uint32_t> moved;
    for (const auto& [s, state] : playSnapshot_)
        if (changed(capture(s), state)) {
            apply(s, state, true);
            moved.push_back(s);
        }
    scene_->propagate();
    for (const uint32_t s : moved)
        if (const flecs::entity e = entityOf(s); e && records_[s].slot != ~0u)
            if (const WorldTransform* t = e.try_get<WorldTransform>()) {
                std::memcpy(pak_->mutableInstances()[records_[s].slot].world, t->rows, sizeof(t->rows));
                streamed_.push_back(records_[s].slot);
            }
    playSnapshot_.clear();
    sceneChanged();
    notice("Stopped: the scene is as it was before Play");
    sendState(false);
}

std::string EditorBridge::projectGame() const
{
    if (!hasProject_ || !gamescripts::exists(project_.dir))
        return {};
    std::error_code ec;
    const std::string dll = gamescripts::dllPath(project_.dir);
    return std::filesystem::exists(dll, ec) && gamescripts::builtForThisEngine(project_.dir) ? dll : std::string();
}

void EditorBridge::gameUiSend(std::string_view json)
{
    if (!ui_)
        return;
    JsonWriter w;
    w.beginObject().value("type", "editor.gameui.send").value("data", json).endObject();
    ui_->send(w.str());
}

void EditorBridge::showGameUi(bool show)
{
    if (!ui_ || !hasProject_)
        return;
    std::error_code ec;
    const std::string page = project_.dir + "/UI/Game/index.html";
    JsonWriter w;
    w.beginObject().value("type", "editor.gameui");
    if (show && std::filesystem::exists(page, ec)) {
        std::string url = "file:///";
        for (const char c : page)
            if (c == ' ' || c == '#' || c == '%' || c == '?')
                url += "%" + std::string(1, "0123456789ABCDEF"[uint8_t(c) >> 4]) + std::string(1, "0123456789ABCDEF"[uint8_t(c) & 15]);
            else
                url += c == '\\' ? '/' : c;
        w.value("url", url);
    }
    w.endObject();
    gameUiMessage_ = show ? w.str() : std::string();
    ui_->send(w.str());
}

void EditorBridge::pollPageChanges()
{
    pagesWatch_.watch(hasProject_ ? project_.dir + "/UI" : std::string());
    libraryPagesWatch_.watch(hasProject_ ? project_.dir + "/Libraries" : std::string());
    std::vector<std::string> changed, files;
    if (pagesWatch_.poll(changed))
        for (const std::string& c : changed)
            files.push_back("UI/" + c);
    if (libraryPagesWatch_.poll(changed))
        for (const std::string& c : changed)  // <library>/UI/...: not its code or anything else
            if (const size_t slash = c.find('/'); slash != std::string::npos && c.compare(slash + 1, 3, "UI/") == 0)
                files.push_back("Libraries/" + c);
    if (files.empty() || !ui_)
        return;
    ++pageChanges_;
    FX_INFO("editor: %s%s changed: reloading the pages", files.front().c_str(), files.size() > 1 ? " (and more)" : "");
    JsonWriter w;
    w.beginObject().value("type", "editor.pages.changed").beginArray("files");
    for (const std::string& f : files)
        w.value(nullptr, f);
    w.endArray().endObject();
    ui_->send(w.str());
}

void EditorBridge::setTools(const std::string& json)
{
    toolsJson_ = json.empty() ? "{}" : json;
    // Editor windows' pages as file URLs: a library's UI/ folder, or the project's UI/.
    try {
        Json tools = Json::parse(toolsJson_);
        if (Json* windows = const_cast<Json*>(tools.get("windows")); windows && hasProject_)
            for (Json& w : windows->array) {
                const std::string library = w.getString("library");
                const std::string path = project_.dir + (library.empty() ? "" : "/Libraries/" + library) + "/UI/" + w.getString("page");
                std::string url = "file:///";
                for (const char c : path)
                    if (c == ' ' || c == '#' || c == '%' || c == '?')
                        url += "%" + std::string(1, "0123456789ABCDEF"[uint8_t(c) >> 4]) + std::string(1, "0123456789ABCDEF"[uint8_t(c) & 15]);
                    else
                        url += c == '\\' ? '/' : c;
                w.set("url", Json::makeString(url));
            }
        toolsJson_ = tools.dump(0);
    } catch (const std::exception&) {
    }
    parseCommandKeys();
    if (view_)  // (new code: what the old code held is let go)
        view_->setCaptured(false);
    hint_.clear();
    if (ui_)
        ui_->send("{\"type\":\"editor.tools\",\"tools\":" + toolsJson_ + "}");
}

void EditorBridge::parseCommandKeys()
{
    commandKeys_.clear();
    // The user's keymap (prefs "page" section "keymap": {path: key; "" unbinds}) over the code's keys.
    Json keymap;
    if (prefs_)
        if (const Json* k = prefs_->section("page").get("keymap"); k && k->isObject())
            keymap_ = k->dump(0);
    try {
        keymap = Json::parse(keymap_);
        const Json tools = Json::parse(toolsJson_);
        if (const Json* menus = tools.get("menus"))
            for (const Json& m : menus->array) {
                const std::string path = m.getString("path");
                std::string key = m.getString("key");
                if (const Json* o = keymap.get(path.c_str()); o && o->type == Json::Type::String)
                    key = o->str;
                if (key.empty())
                    continue;
                CommandKey c;
                c.path = path;
                // "Shift+E", "E", "Alt+N" (Ctrl combinations are the page's: Shortcut).
                std::string name = key;
                for (size_t plus; (plus = name.find('+')) != std::string::npos; name = name.substr(plus + 1)) {
                    const std::string mod = name.substr(0, plus);
                    c.mods |= mod == "Shift" ? SDL_KMOD_SHIFT : mod == "Alt" ? SDL_KMOD_ALT : mod == "Ctrl" ? SDL_KMOD_CTRL : 0;
                }
                c.key = int32_t(SDL_GetKeyFromName(name.c_str()));
                if (c.key == SDLK_UNKNOWN)
                    continue;
                if (const std::string modes = m.getString("modes"); !modes.empty()) {
                    c.modes = 0;
                    static const char* const kModes[] = {"object", "vertex", "edge", "face"};
                    for (uint32_t k = 0; k < 4; ++k)
                        if (modes.find(kModes[k]) != std::string::npos)
                            c.modes |= 1u << k;
                }
                commandKeys_.push_back(std::move(c));
            }
    } catch (const std::exception&) {
    }
}

bool EditorBridge::commandKey(int32_t key, uint16_t mods, uint32_t elementMode)
{
    const uint16_t held = mods & (SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_CTRL);
    for (const CommandKey& c : commandKeys_) {
        const uint16_t want = c.mods;
        const bool modsMatch = ((held & SDL_KMOD_SHIFT) != 0) == ((want & SDL_KMOD_SHIFT) != 0) &&
                               ((held & SDL_KMOD_ALT) != 0) == ((want & SDL_KMOD_ALT) != 0) &&
                               ((held & SDL_KMOD_CTRL) != 0) == ((want & SDL_KMOD_CTRL) != 0);
        if (c.key == key && modsMatch && (c.modes >> elementMode & 1u)) {
            toolRuns_.push_back(c.path);
            lastCommand_ = c.path;
            return true;
        }
    }
    return false;
}

bool EditorBridge::repeatCommand()
{
    if (lastCommand_.empty())
        return false;
    toolRuns_.push_back(lastCommand_);
    return true;
}

bool EditorBridge::viewportState(FxViewportState& out)
{
    if (!view_)
        return false;
    view_->codeInput(out);
    return true;
}

void EditorBridge::viewportCapture(bool on, bool wheel)
{
    if (view_)
        view_->setCapturedWheel(on && wheel);
    if (!view_ || view_->captured() == (on && !playing_))
        return;
    view_->setCaptured(on && !playing_);
    if (!on)
        captureEndedNs_ = SDL_GetTicksNS();
    sendState(false);  // (the page leaves the keys alone while it holds: Esc is the operation's, not Select None)
}

void EditorBridge::toolState(const char* json)
{
    const std::string s = json ? json : "";
    try {
        Json::parse(s);
    } catch (const std::exception&) {
        return;
    }
    if (s == toolState_)
        return;
    toolState_ = s;
    if (ui_)
        ui_->send("{\"type\":\"editor.tools.state\",\"state\":" + toolState_ + "}");
}

void EditorBridge::viewportHint(const char* text)
{
    const std::string t = text ? text : "";
    if (t == hint_)
        return;
    hint_ = t;
    sendState(false);
}

int32_t EditorBridge::elementsGet(uint64_t& entity, uint32_t& mode, uint64_t* items, int32_t capacity)
{
    entity = elements_.entity < records_.size() ? records_[elements_.entity].entity : 0;
    mode = elements_.mode;
    const uint32_t current = view_ ? uint32_t(view_->tools().element) : 0;
    if (elements_.entity != selected_ || elements_.mode != current)  // (stale: another entity or mode)
        return entity = 0, mode = current, 0;
    for (int32_t k = 0; k < capacity && size_t(k) < elements_.items.size(); ++k)
        items[k] = elements_.items[size_t(k)];
    return int32_t(elements_.items.size());
}

bool EditorBridge::meshLive(uint64_t entity, std::shared_ptr<const level::PolyMesh> mesh)
{
    const auto it = indexOf_.find(entity);
    if (playing_ || it == indexOf_.end())
        return false;
    setLevelMeshLive(it->second, std::move(mesh));
    return true;
}

bool EditorBridge::meshCommit(uint64_t entity, std::shared_ptr<const level::PolyMesh> before)
{
    const auto it = indexOf_.find(entity);
    if (playing_ || it == indexOf_.end())
        return false;
    commitLevelMesh(it->second, std::move(before));
    return true;
}

void EditorBridge::elementsSet(uint64_t entity, uint32_t mode, const uint64_t* items, int32_t count)
{
    const auto it = indexOf_.find(entity);
    if (it == indexOf_.end() || mode > 3)
        return;
    if (selected_ != it->second)
        select(it->second);
    if (view_)
        view_->tools().element = ElementMode(mode);
    elements_ = {it->second, mode, {}};
    for (int32_t k = 0; k < count; ++k)
        elements_.items.push_back(items[k]);
    sendState(false);
}

bool EditorBridge::levelTarget(uint32_t& target, ElementSelection& chosen) const
{
    if (selected_ == ~0u || !levelMeshOf(selected_))
        return false;
    target = selected_;
    const uint32_t current = view_ ? uint32_t(view_->tools().element) : 0;
    chosen = elements_.entity == selected_ && elements_.mode == current ? elements_ : ElementSelection{selected_, current, {}};
    return true;
}

bool EditorBridge::applyLevelOp(uint32_t target, const ElementSelection& chosen, const std::string& op, Json args)
{
    const auto mesh = levelMeshOf(target);
    if (!mesh)
        return false;
    // The chosen elements under their mode's name (unless the caller named some), the transform.
    static const char* const kKeys[] = {nullptr, "vertices", "edges", "faces"};
    if (chosen.mode >= 1 && chosen.mode <= 3 && !args.get("vertices") && !args.get("edges") && !args.get("faces")) {
        Json items;
        items.type = Json::Type::Array;
        for (const uint64_t item : chosen.items)
            items.array.push_back(Json::makeNumber(double(item)));
        args.set(kKeys[chosen.mode], std::move(items));
    }
    if (float w[3][4]; !args.get("world") && worldOf(target, w)) {
        Json rows;
        rows.type = Json::Type::Array;
        for (int k = 0; k < 12; ++k)
            rows.array.push_back(Json::makeNumber(w[k / 4][k % 4]));
        args.set("world", std::move(rows));
    }
    // A wall is made again from its own materials whenever it is loaded or changed: a material put
    // on its faces goes to the sides, top, floor or ceiling those faces are.
    if (State s = capture(target); op == "material" && s.wall && args.get("path")) {
        level::Wall roles = *s.wall;
        roles.sideA = "\x01" "A", roles.sideB = "\x01" "B", roles.top = "\x01" "T", roles.floorMaterial = "\x01" "F", roles.ceilingMaterial = "\x01" "C";
        const level::PolyMesh tagged = level::buildWall(roles);
        if (tagged.faces.size() == mesh->faces.size()) {
            std::vector<uint32_t> faces;
            if (const Json* list = args.get("faces"))
                for (const Json& f : list->array)
                    faces.push_back(uint32_t(f.number));
            level::Wall wall = *s.wall;
            const std::string path = args.getString("path");
            for (uint32_t f = 0; f < tagged.faces.size(); ++f) {
                if (!faces.empty() && std::find(faces.begin(), faces.end(), f) == faces.end())
                    continue;
                const std::string& tag = tagged.materials[tagged.faces[f].material];
                std::string* field = tag == "\x01" "A" ? &wall.sideA : tag == "\x01" "B" ? &wall.sideB : tag == "\x01" "T" ? &wall.top
                                     : tag == "\x01" "F" ? &wall.floorMaterial : tag == "\x01" "C" ? &wall.ceilingMaterial : nullptr;
                if (field)
                    *field = path;
            }
            s.wall = wall;
            s.levelMesh = std::make_shared<level::PolyMesh>(level::buildWall(wall));
            execute({{target, s}});
            sendState(false);
            return true;
        }
    }
    level::OpResult out;
    if (!level::runOp(op, *mesh, args, out)) {
        notice((op + ": " + (out.error.empty() ? std::string("nothing done") : out.error)).c_str(), "warning");
        return false;
    }
    State s = capture(target);
    s.levelMesh = std::make_shared<level::PolyMesh>(std::move(out.mesh));
    execute({{target, s}});
    if (out.selectionMode != 0 && chosen.mode != 0 && elements_.entity == target) {
        elements_.mode = chosen.mode;
        elements_.items = out.selectionMode == chosen.mode ? out.selection : std::vector<uint64_t>{};
    }
    sendState(false);
    return true;
}

bool EditorBridge::levelFaceAt(float fx, float fy, uint32_t& record, uint32_t& face) const
{
    record = face = ~0u;
    if (!view_ || ((fx < 0.0f || fy < 0.0f) && !view_->mouseFraction(fx, fy)))
        return false;
    float3 o, d, point, normal;
    view_->rayAt(fx, fy, o, d);
    return surfaceHit(o, d, {}, point, normal, &record, &face) && record != ~0u;
}

void EditorBridge::sendLevelUvs()
{
    // The UV editor's view: the chosen faces' UVs as drawn (the face mode; else every face), with
    // their settings and material.
    if (!ui_)
        return;
    JsonWriter w;
    w.beginObject().value("type", "editor.level.uvs").beginArray("faces");
    uint32_t target;
    ElementSelection chosen;
    if (levelTarget(target, chosen)) {
        const auto mesh = levelMeshOf(target);
        float world[3][4];
        const bool hasWorld = worldOf(target, world);
        std::vector<uint32_t> faces;
        if (chosen.mode == 3)
            for (const uint64_t f : chosen.items)
                faces.push_back(uint32_t(f));
        else
            for (uint32_t f = 0; f < mesh->faces.size() && f < 512; ++f)
                faces.push_back(f);
        static const char* const kModes[] = {"local", "world", "manual"};
        for (const uint32_t f : faces) {
            if (f >= mesh->faces.size())
                continue;
            const level::Face& face = mesh->faces[f];
            w.beginObject().value("face", f).value("mode", kModes[uint32_t(face.uv.mode) % 3]);
            w.value("material", face.material < mesh->materials.size() ? mesh->materials[face.material] : std::string());
            w.beginArray("offset").value(nullptr, double(face.uv.offset[0])).value(nullptr, double(face.uv.offset[1])).endArray();
            w.beginArray("scale").value(nullptr, double(face.uv.scale[0])).value(nullptr, double(face.uv.scale[1])).endArray();
            w.value("rotation", double(face.uv.rotation)).beginArray("uv");
            for (const float2 p : level::faceUvs(*mesh, f, hasWorld ? world : nullptr))
                w.value(nullptr, double(p.x)).value(nullptr, double(p.y));
            w.endArray().endObject();
        }
    }
    w.endArray().value("picked", pickedMaterial_).endObject();
    if (w.str() == lastUvs_)
        return;
    lastUvs_ = w.str();
    ui_->send(w.str());
}

void EditorBridge::sendTrimSheets()
{
    if (!ui_ || !hasProject_)
        return;
    namespace fs = std::filesystem;
    std::vector<std::string> paths;
    std::error_code ec;
    const fs::path root = fs::path(project_.dir);
    for (fs::recursive_directory_iterator it(root / "Assets", ec), end; it != end && !ec; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".ftrim")
            paths.push_back(it->path().lexically_relative(root).generic_string());
    std::sort(paths.begin(), paths.end());
    JsonWriter w;
    w.beginObject().value("type", "editor.trim.list").beginArray("sheets");
    for (const std::string& path : paths) {
        level::TrimSheet sheet;
        std::string error;
        if (!level::readTrimSheet(project_.path(path), sheet, error))
            continue;
        MaterialAsset mat;
        const std::string image = readMaterialAsset(project_.path(sheet.material), mat, error) ? mat.textures[0] : std::string();
        w.beginObject().value("path", path).value("material", sheet.material).value("image", image).value("aspect", double(sheet.aspect));
        w.beginArray("strips");
        for (const level::TrimStrip& s : sheet.strips)
            w.beginObject().value("name", s.name).value("v0", double(s.v0)).value("v1", double(s.v1)).endObject();
        w.endArray().endObject();
    }
    w.endArray().value("lastPath", lastTrim_.first).value("lastStrip", lastTrim_.second).endObject();
    ui_->send(w.str());
}

std::string EditorBridge::editorQuery(const std::string& what) const
{
    JsonWriter w;
    if (what == "entities") {
        w.beginArray();
        for (uint32_t i = 0; i < records_.size(); ++i) {
            const flecs::entity e = entityOf(i);
            const uint32_t parent = parentOf(i);
            w.beginObject().value("id", i + 1).value("name", defaultName(i)).value("parent", parent == ~0u ? 0u : parent + 1)
                .value("kind", int32_t(records_[i].kind)).value("alive", bool(e) && e.is_alive() && e.enabled())
                .value("entity", e ? uint64_t(e.id()) : uint64_t(0)).endObject();
        }
        w.endArray();
    } else if (what == "selection") {
        w.beginObject().value("id", selected_ == ~0u ? 0u : selected_ + 1).endObject();
    } else if (what == "project") {
        w.beginObject().value("dir", hasProject_ ? project_.dir : "").value("name", hasProject_ ? project_.name : "")
            .value("scene", scenePath_).endObject();
    } else if (what == "playing") {
        w.beginObject().value("playing", playing_).value("paused", paused_).endObject();
    } else if (what == "undo") {
        // The newest undo step as a token (how many, and where its changes are kept: a step keeps
        // its storage as it moves between the undo and redo lists): editor code's last operation
        // checks it is still the newest.
        w.beginObject().value("count", uint64_t(undo_.size()))
            .value("top", undo_.empty() ? uint64_t(0) : uint64_t(reinterpret_cast<uintptr_t>(undo_.back().data()))).endObject();
    } else {
        return "null";
    }
    return w.str();
}

// Editor code's changes between begin and end: one undo step.
void EditorBridge::undoBegin()
{
    if (undoGroupDepth_++ == 0)
        undoGroupStart_ = undo_.size();
}

void EditorBridge::undoEnd()
{
    if (undoGroupDepth_ == 0 || --undoGroupDepth_ != 0)
        return;
    if (undo_.size() <= undoGroupStart_ + 1)
        return;
    Command merged;
    for (size_t i = undoGroupStart_; i < undo_.size(); ++i)
        for (Change& c : undo_[i])
            merged.push_back(std::move(c));
    undo_.resize(undoGroupStart_);
    undo_.push_back(std::move(merged));
}

void EditorBridge::sendLibraries()
{
    JsonWriter w;
    w.beginObject().value("type", "editor.libraries").value("installing", installing_ > 0).beginArray("libraries");
    if (hasProject_)
        for (const libraries::Library& l : libraries::scan(project_.dir)) {
            w.beginObject().value("name", l.name).value("version", l.version).value("description", l.description).value("author", l.author)
                .value("source", l.source).value("ref", l.ref).value("enabled", l.enabled).value("trusted", l.trusted).value("code", l.code)
                .value("editor", l.editor).value("ui", l.ui).value("assets", l.assets).value("error", l.error).beginArray("dependencies");
            for (const std::string& d : l.dependencies)
                w.value(nullptr, d);
            w.endArray().endObject();
        }
    w.endArray().endObject();
    if (ui_)
        ui_->send(w.str());
}

void EditorBridge::librariesChanged(bool code)
{
    sendLibraries();
    if (code && hasProject_)
        buildScripts(false);  // (its code in or out of the game: the next build has it)
}

void EditorBridge::startInstall(const std::string& source, const std::string& location, const std::string& ref)
{
    ++installing_;
    sendLibraries();
    libraries::install(project_.dir, source, location, ref, [this](libraries::InstallResult r) {
        std::lock_guard<std::mutex> lock(libraryMutex_);
        installed_.push_back(std::move(r));
    });
}

void EditorBridge::handleLibraries(const std::string& op, const Json& m)
{
    if (!hasProject_)
        return notice("Libraries belong to a project: open one", "error");
    std::string error;
    const std::string name = m.getString("name");
    if (op == "list") {
        sendLibraries();
    } else if (op == "add") {
        // {source: folder | zip | git, path (a folder, a .zip, a repository's URL; none: a dialog
        // asks for the folder or the zip), ref (git: a branch or tag)}. The page warned first: a
        // library's code runs with full access, and adding one trusts it.
        const std::string source = m.getString("source"), path = m.getString("path");
        if (source != "folder" && source != "zip" && source != "git")
            return notice("Library: add from a folder, a zip or a git repository", "error");
        if (!path.empty() || source == "git") {
            if (path.empty())
                return notice("Library: the repository's URL?", "error");
            startInstall(source, path, m.getString("ref"));
            return;
        }
        struct Pick {
            EditorBridge* self;
            std::string source;
        };
        auto* pick = new Pick{this, source};
        const auto picked = [](void* user, const char* const* files, int) {
            std::unique_ptr<Pick> p(static_cast<Pick*>(user));
            if (files && files[0]) {
                std::lock_guard<std::mutex> lock(p->self->libraryMutex_);
                p->self->picked_.emplace_back(p->source, files[0]);
            }
        };
        if (source == "folder") {
            SDL_ShowOpenFolderDialog(picked, pick, nullptr, nullptr, false);
        } else {
            static const SDL_DialogFileFilter kZip[] = {{"Libraries (zip)", "zip"}};
            SDL_ShowOpenFileDialog(picked, pick, nullptr, kZip, 1, nullptr, false);
        }
    } else if (op == "create") {
        if (!libraries::create(project_.dir, name, error))
            return notice(("Library: " + error).c_str(), "error");
        notice(("Library " + name + " made: Libraries/" + name).c_str());
        librariesChanged(true);
    } else if (op == "enable") {
        if (!libraries::setEnabled(project_.dir, name, m.getBool("enabled", true), error))
            return notice(("Library: " + error).c_str(), "error");
        librariesChanged(true);
    } else if (op == "trust") {
        for (const libraries::Library& l : libraries::scan(project_.dir))
            if (l.name == name)
                libraries::setTrusted(l.name, l.version, m.getBool("trust", true));
        librariesChanged(true);
    } else if (op == "export") {
        const std::string zip = libraries::exportZip(project_.dir, name, m.getString("path"), error);
        if (zip.empty())
            return notice(("Library: " + error).c_str(), "error");
        notice(("Library " + name + " exported: " + zip).c_str());
        JsonWriter w;
        w.beginObject().value("type", "editor.libraries.exported").value("name", name).value("path", zip).endObject();
        ui_->send(w.str());
    } else if (op == "remove") {
        if (!libraries::remove(project_.dir, name, error))
            return notice(("Library: " + error).c_str(), "error");
        notice(("Library " + name + " removed").c_str());
        librariesChanged(true);
    }
}

bool EditorBridge::buildScripts(bool asked)
{
    std::string error;
    if (!gamescripts::exists(project_.dir) && !gamescripts::create(project_.dir, project_.name, error)) {
        notice(error.c_str(), "error");
        return false;
    }
    if (scriptBuild_.running()) {
        scriptBuildAsked_ = scriptBuildAsked_ || asked;
        return true;
    }
    scriptSources_ = gamescripts::sourcesStamp(project_.dir);
    if (!scriptBuild_.start(project_.dir)) {
        scriptAutoBuild_ = false;  // (no SDK: changes are not built until asked again)
        scriptsStage_ = "failed";
        scriptsError_ = scriptBuild_.error();
        notice(scriptsError_.c_str(), "error");
        sendState(false);
        return false;
    }
    scriptAutoBuild_ = true;
    scriptBuilding_ = scriptSources_;
    scriptBuildAsked_ = asked;
    scriptsStage_ = "building";
    scriptsError_.clear();
    sendState(false);
    return true;
}

void EditorBridge::watchScripts(bool now)
{
    if (!hasProject_ || !gamescripts::exists(project_.dir))
        return;
    const uint64_t t = SDL_GetTicksNS();
    if (!now && t - scriptScanNs_ < 250'000'000ull)
        return;
    scriptScanNs_ = t;
    const uint64_t sources = gamescripts::sourcesStamp(project_.dir);
    if (scriptSources_ == 0) {
        // Opened: the build loaded at startup is of these sources unless one is newer (or there is
        // none, or it is another engine's): then it is built now.
        scriptSources_ = sources;
        if (!gamescripts::stale(project_.dir))
            scriptBuilt_ = sources;
        scriptChangedNs_ = 0;
    } else if (sources != scriptSources_) {
        scriptSources_ = sources;
        scriptChangedNs_ = t;
    }
    // A change is built once the files hold still for a moment (an editor's save-all, a save
    // through a temporary file); a change during a build, once it is done (updateScriptBuild).
    if (scriptsBehind() && !scriptBuild_.running() && (now || t - scriptChangedNs_ > 150'000'000ull))
        buildScripts(false);
}

bool EditorBridge::scriptsBehind() const
{
    return scriptAutoBuild_ && scriptSources_ != 0 && scriptSources_ != scriptBuilt_;
}

void EditorBridge::updateGameBuild()
{
    const GameBuild::State state = gameBuild_.poll();
    if (state == GameBuild::State::Succeeded) {
        gameBuildResult_ = "ok";
        notice(("Game built: " + gameBuild_.output()).c_str());
    } else if (state == GameBuild::State::Failed) {
        gameBuildResult_ = gameBuild_.error();
        FX_ERROR("build game: %s", gameBuildResult_.c_str());
        notice(("Build Game: " + gameBuildResult_).c_str(), "error");
    }
}

void EditorBridge::updateScriptBuild()
{
    const ScriptBuild::State state = scriptBuild_.poll();
    if (state != ScriptBuild::State::Succeeded && state != ScriptBuild::State::Failed)
        return;
    scriptBuilt_ = scriptBuilding_;
    scriptBuildFailed_ = state == ScriptBuild::State::Failed;
    // The sources changed while it built: this build is already out of date, the next one starts
    // now (and Play, if it waits, waits for that one).
    scriptSources_ = gamescripts::sourcesStamp(project_.dir);
    if (scriptsBehind() && buildScripts(scriptBuildAsked_)) {
        FX_INFO("scripts: changed while building: building again");
        return;
    }
    if (state == ScriptBuild::State::Succeeded) {
        builtGame_ = true;  // the engine loads it (takeBuiltGame), then gameLoaded
        scriptsStage_ = "loading";
    } else {
        scriptsStage_ = "failed";
        scriptsError_ = scriptBuild_.error();
        scriptBuildAsked_ = false;
        notice(std::exchange(pendingPlay_, false) ? "Play: the scripts did not build (see the Console)"
                                                  : "The scripts did not build (see the Console)",
               "error");
    }
    sendState(false);
}

bool EditorBridge::takeBuiltGame(std::string& dll)
{
    if (!std::exchange(builtGame_, false))
        return false;
    dll = gamescripts::dllPath(project_.dir);
    return true;
}

void EditorBridge::gameLoaded(bool ok)
{
    scriptsStage_ = ok ? "" : "failed";
    if (!ok)
        scriptsError_ = "The game's code did not load (see the Console)";
    game_.loaded = game_.loaded || ok;
    if (std::exchange(pendingPlay_, false)) {
        if (ok)
            startPlay();
        else
            notice(("Play: " + scriptsError_).c_str(), "error");
    } else if (!ok || scriptBuildAsked_) {  // (a background build just shows in the status bar)
        notice(ok ? "Scripts built and loaded" : scriptsError_.c_str(), ok ? "info" : "error");
    }
    scriptBuildAsked_ = false;
    sendState(false);
}

void EditorBridge::setGameStatus(const GameStatus& status)
{
    const bool changed = status.loaded != game_.loaded || status.running != game_.running;
    game_ = status;
    if (changed && scene_ && ui_ && ui_->active())  // (the first frames come before the editor is set up)
        sendState(false);
}

void EditorBridge::setGameHud(const std::string& text, uint64_t revision)
{
    if (revision == hudRevision_ || !scene_ || !ui_ || !ui_->active())
        return;
    hudRevision_ = revision;
    hud_ = text;
    JsonWriter w;
    w.beginObject().value("type", "editor.hud").value("text", hud_).endObject();
    ui_->send(w.str());
}

std::string EditorBridge::scriptsState() const
{
    if (!hasProject_ || !gamescripts::exists(project_.dir))
        return "none";
    if (!scriptsStage_.empty())
        return scriptsStage_;
    return game_.loaded ? "ready" : "unbuilt";
}

void EditorBridge::update(const CameraState& camera)
{
    if (terrain_)
        terrain_->setView(camera.position, camera.forward);
    lastCamera_ = camera;
    if (selected_ != eventSelected_) {
        eventSelected_ = selected_;
        editorEvents_.push_back("{\"type\":\"selection\",\"id\":" + std::to_string(selected_ == ~0u ? 0u : selected_ + 1) + "}");
    }
    if (playing_ != eventPlaying_) {
        eventPlaying_ = playing_;
        showGameUi(playing_);
        editorEvents_.push_back(playing_ ? "{\"type\":\"play\"}" : "{\"type\":\"stop\"}");
    }
    // The selection keeps to live, editable entities (undo, a game's Destroy, Delete elsewhere).
    if (std::erase_if(selection_, [&](uint32_t i) { return !editable(i) || !capture(i).alive; }) > 0) {
        selected_ = selection_.empty() ? ~0u : selection_.back();
        sendState(false);
    }
    // Level meshes: their slot once LevelMeshes gave them a live mesh (then the eye and Delete
    // hide its row like any mesh's).
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (records_[i].kind == EntityKind::Level && records_[i].slot == ~0u)
            if (const MeshInstance* mi = entityOf(i) ? entityOf(i).try_get<MeshInstance>() : nullptr) {
                records_[i].slot = mi->gpuSlot;
                if (mi->gpuSlot >= indexOfSlot_.size())
                    indexOfSlot_.resize(mi->gpuSlot + 1, ~0u);
                indexOfSlot_[mi->gpuSlot] = i;  // (picks select it)
                refreshRows(i);
            }
    if (!ui_ || !ui_->active())
        return;
    FX_ZONE_CAT("EditorBridge::update", Editor);
    // Moved during a drag last frame: the GI follows live, the probes round what moved updating
    // first (once a frame at most); the commit at the end of the drag relights it all.
    if (std::exchange(liveMoved_, false) && settings_ && liveRegion_[0] <= liveRegion_[3]) {
        std::memcpy(settings_->gi.region, liveRegion_, sizeof(liveRegion_));
        ++settings_->gi.regionRevision;
    }
    std::string message;
    const auto run = [&](const std::string& m) {
        try {
            handle(Json::parse(m), camera);
        } catch (const std::exception& e) {
            FX_WARN("editor: bad message (%s): %.120s", e.what(), m.c_str());
        }
    };
    while (ui_->poll(message))
        run(message);
    pollPageChanges();
    // The upscaler's status (which one runs, at what size) follows a settings change by a frame.
    if (renderer_ && renderer_->upscalerStatus() != upscalerStatus_) {
        upscalerStatus_ = renderer_->upscalerStatus();
        sendState(false);
    }
    if (const uint32_t edit = terrain_ ? terrain_->takeFinishedEdit() : ~0u; edit != ~0u)
        terrainEdited(edit);  // a generation run (or the erosion brush) finished
    if (terrain_ && terrain_->takeChanged())  // brush radius keys
        sendState(false);
    flushLog();
    profiler_.update(*ui_);
    // The Network tab, four times a second while it shows (the running game's network; idle: none).
    if (netSubscribed_ && netStatus_ && SDL_GetTicksNS() - netSentNs_ > 250'000'000ull) {
        netSentNs_ = SDL_GetTicksNS();
        JsonWriter w;
        w.beginObject().value("type", "net.status").value("playing", playing_);
        const std::string status = playing_ ? netStatus_() : std::string();
        if (!status.empty())
            w.raw("status", status);
        w.endObject();
        ui_->send(w.str());
    }
    animEditor_.update(*ui_);
    assets_.update();
    // Settings, tools, sun and the scene camera are remembered (checked twice a second); the
    // project's own settings go to project.file() (with or without the editor's prefs).
    if (const uint64_t now = SDL_GetTicksNS(); now - persistNs_ > 500'000'000ull) {
        persistNs_ = now;
        if (prefs_) {
            persist();
            prefs_->save();
        }
        if (hasProject_ && std::exchange(projectSettingsDirty_, false)) {
            std::string error;
            if (!saveProject(project_, error))
                notice(error.c_str(), "error");
        }
    }
    if (scriptWaitWear_ && scriptWaitWearFrames_ > 0) {
        --scriptWaitWearFrames_;
    } else if (scriptWaitWear_ && !(levelMeshes_ && levelMeshes_->wearPending())) {
        scriptWaitWear_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (scriptWaitCuts_ && !(levelMeshes_ && levelMeshes_->cutsPending())) {
        scriptWaitCuts_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (levelMeshes_ && levelMeshes_->clippedVersion() != clippedVersion_) {  // (the Hierarchy's badges)
        clippedVersion_ = levelMeshes_->clippedVersion();
        sendState(true);
    }
    navUpdate();
    if (scriptWaitGenerated_ && !(terrain_ && terrain_->generating())) {
        scriptWaitGenerated_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    updateCook();
    updateScriptBuild();
    updateGameBuild();
    if (scriptWaitGameBuild_ && !gameBuild_.running()) {
        scriptWaitGameBuild_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (connected_)
        watchScripts(false);
    if (scriptWaitBuild_ && !scriptBuild_.running() && !builtGame_ && scriptsStage_ != "loading" && !pendingPlay_ && !scriptsBehind()) {
        scriptWaitBuild_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    shaders_.update();
    if (scriptWaitShaders_ && !shaders_.busy()) {
        scriptWaitShaders_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (scriptWaitCook_ && !cook_) {
        scriptWaitCook_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    // Libraries: installs that finished, folders and zips picked in a dialog.
    {
        std::vector<libraries::InstallResult> done;
        std::vector<std::pair<std::string, std::string>> picked;
        {
            std::lock_guard<std::mutex> lock(libraryMutex_);
            done.swap(installed_);
            picked.swap(picked_);
        }
        for (const auto& [source, path] : picked)
            startInstall(source, path, "");
        for (const libraries::InstallResult& r : done) {
            --installing_;
            if (r.ok)
                notice(("Library " + r.name + " added").c_str());
            else
                notice(("Library: " + r.error).c_str(), "error");
            librariesChanged(r.ok);
        }
    }
    if (scriptWaitLibraries_ && installing_ == 0) {
        scriptWaitLibraries_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    updateAudioBake();
    if (scriptWaitBake_ && !(audio_ && audio_->baking())) {
        scriptWaitBake_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (scriptWaitGi_ && (!renderer_ || renderer_->giSettled())) {
        scriptWaitGi_ = false;
        scriptWait_ = std::min(scriptWait_, 2u);
    }
    if (scriptWaitPreviews_ && !assets_.previewsBusy()) {
        scriptWaitPreviews_ = false;
        scriptWait_ = std::min(scriptWait_, 5u);
    }
    if (scriptWaitProbe_ > 0) {
        if (--scriptWaitProbe_ == 0)
            FX_WARN("editor test: the page did not answer test.probe");
    } else if (scriptWait_ > 0 || SDL_GetTicksNS() < scriptWaitUntilNs_) {
        if (scriptWait_ > 0)
            --scriptWait_;
    } else if (connected_ && SDL_GetTicksNS() - connectedNs_ > 500'000'000ull && scriptNext_ < script_.size()) {
        if (scriptWaitGi_) {  // (its frames ran out first: what comes next sees the light still changing)
            FX_WARN("editor test: the GI probes were still updating when test.wait's frames ran out");
            scriptWaitGi_ = false;
        }
        FX_INFO("editor: script step %zu: %s", scriptNext_, script_[scriptNext_].c_str());
        run(script_[scriptNext_++]);
    }
}

void EditorBridge::report(const EditorFrameStats& s)
{
    if (!connected_)
        return;
    JsonWriter w;
    w.beginObject().value("type", "engine.render").value("fps", s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0)
        .value("objects", s.objects).endObject();
    ui_->send(w.str());
    JsonWriter st;
    st.beginObject().value("type", "engine.stats").value("frameMs", s.frameMs).value("scriptMs", s.scriptMs)
        .value("physicsMs", s.physicsMs).value("animationMs", s.animationMs).value("uiMs", s.uiMs)
        .value("clusters", s.clusters).value("triangles", s.triangles).value("vsmPages", s.vsmPages)
        .value("vtResident", s.vtResident).value("vtCapacity", s.vtCapacity).value("bodies", s.bodies)
        .value("characters", s.characters).value("objects", s.objects).value("uiFrames", s.uiFrames)
        .value("uiCopyMs", s.uiCopyMs)
        .value("playing", playing_).endObject();
    ui_->send(st.str());
}

}  // namespace fx
