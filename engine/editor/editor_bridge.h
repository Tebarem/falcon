#pragma once

// The editor shell's engine side: answers the page in ui/editor (outliner, inspector, assets,
// profiler) and applies its commands to the scene with undo/redo. Entities are addressed by id:
// cooked instances first (instance slot + 1, stable across runs), then created entities in
// creation order (their edits-file uid is what stays stable); 0 = none.
//
// Page -> engine:
//   editor.ready, editor.scene.reload, editor.scene.save, editor.undo, editor.redo
//   editor.select {id}, editor.frame {id} (select and frame it)
//   editor.transform.set {id, position, rotation (XYZ euler degrees), scale}
//   editor.transform.preview {...same}  (while scrubbing: live, the next set is one undo step)
//   editor.render.set {id, visible}
//   editor.entity.create {kind: primitive|mesh|empty|light|camera, primitive / mesh: any mesh of the scene by name, light: point|spot|directional, parent}
//   editor.asset.place {mesh: name | model: path, at: {x, y} (fractions of the view) | absent (the view's pivot), parent}
//   editor.entity.material {id, material} (a material asset, .fmat relative to the project; "": its own)
//   editor.asset.assign {material, id | at: {x, y}} (onto the entity, or the mesh under that point of the view)
//   editor.material.set {path, values: {baseColor, metallic, ...}} (the .fmat written; shown live)
//   editor.prefab.create {id, folder} (the entity and everything below it saved as a prefab; it becomes an instance)
//   editor.asset.place {prefab: path, ...} (an instance), editor.prefab.apply / revert / unpack {id} (any entity of an instance)
//   Prefab instances are linked: their entities follow the prefab; what an instance changes is kept as
//   overrides; Apply writes the instance into the prefab (other instances follow, keeping theirs).
//   (resting on what is under that point: terrain, a mesh, the ground plane; a model: an empty with its nodes below)
//   (a camera starts at the scene view's camera),
//   editor.entity.duplicate {id} (with its children), editor.entity.delete {id} (with its
//   children), editor.entity.rename {id, name}, editor.entity.parent {id, parent} (0: root;
//   the world transform is kept)
//   Transforms are local (relative to the parent) for children, world space for roots.
//   editor.light.set {id, lightType: point|spot|directional, color [rgb], intensity, range, inner, outer, shadows}
//   (the first directional light is the sun: rotating it moves the sun)
//   editor.light.preview {...same}  (while scrubbing: live, the next set is one undo step)
//   editor.camera.set {id, fov, near, main} (one main camera: setting it clears the others),
//   editor.camera.preview {...same}
//   editor.component.add {id, component: Light|Camera|PhysicsBody|BoxCollider|SphereCollider|
//   MeshCollider|AudioSource|AudioListener|Animator, light: point|spot|directional} (the Inspector's Add Component: on any entity
//   that has none, with defaults (a collider fits the mesh); one undo step),
//   editor.component.remove {id, component: Light|Camera|PhysicsBody|Collider|AudioSource|AudioListener|Animator} (one added that
//   way; a light's own Light, a camera's own camera and a prefab's components stay). The
//   inspector lists both ("addable", "removable").
//   editor.body.set {id, mass, linearDamping, angularDamping, gravityScale, kinematic}
//   editor.collider.set {id, shape: box|sphere|mesh|capsule, center, size, friction, bounciness, layer,
//                        ignore: [layers], trigger}
//   (Play simulates them: PhysicsWorld::buildEntityBodies; Stop puts the scene back)
//   editor.audio.source.set {id, sound (a project path, "" none), playOnStart, loop, spatial,
//   ownSettings, airAbsorption, rolloff: inverse|linear, volume, pitch, minDistance, maxDistance,
//   bus: sfx|physics|ambience|music|ui}, editor.audio.listener.set {id, gain} (Add Component:
//   AudioSource, AudioListener; Play plays them: AudioSystem::startGame)
//   editor.animator.set {id, graph (a project .fgraph), params: {name: value | null (its default)}}
//   (an Animator, on an entity with skinned meshes in it or below: its graph drives them all,
//   posed in edit mode, played by Play; Stop puts every character back to its start)
//   editor.audio.surface.set {id, mode: material|own|excluded, preset, absorption [3], scattering,
//   transmission [3], moving, surface ("" its material's surface sounds, else a preset of its own),
//   impactGain, impactPitch} (a mesh's acoustics: its material's, values of its own, or none; one
//   undo step)
//   editor.audio.play {id} (that entity's source, where it is) or {path} (a sound or clip, 2D),
//   editor.audio.stop (the preview; one plays at a time)
//   editor.audio.bake.set {spacing, height, quality 1..3, pathing, autoRegion, center [3], size [3]}
//   (the scene's bake settings, saved in <scene>.audio/), editor.audio.bake.fit {to: objects|view}
//   (the region: the static objects' bounds, or 64 x 24 x 64 m around the view),
//   editor.audio.bake.start | cancel | clear, editor.audio.bake.show {probes} (the probes and the
//   region drawn in the Scene view). The engine sends editor.audioBake {state, stage, fraction,
//   probes, staleWhy, error, info, settings} as they change (audio_system.h AudioBakeStatus).
//   editor.sound.set {path, values: {name, preset, clips [paths], and soundFields() keys}} (a
//   .fsound written, heard so from then on)
//   editor.play, editor.stop, editor.pause, editor.step
//   editor.tool.set {tool, local, pivotCenter, grid, gameView, snapMove, snapRotate, snapScale}
//   editor.settings.set {key, value} (see writeSettings), editor.prefs.set {key, value} (page
//   layout, remembered across sessions; sent back as editor.prefs {page} on editor.ready)
// Engine -> page: engine.connected, editor.state, editor.notice, editor.log (engine and script
//   log lines), engine.render, engine.stats.

#include "anim/bone_entities.h"
#include "assets/acoustic_material.h"
#include "assets/surface_sound.h"
#include "core/dir_watch.h"
#include "core/json.h"
#include "core/math.h"
#include "scene/fly_camera.h"
#include "editor/anim_editor.h"
#include "editor/asset_browser.h"
#include "editor/entity_marker.h"
#include "editor/prefab.h"
#include "script/level_api.h"
#include "editor/profiler_stream.h"
#include "editor/scene_edits.h"
#include "level/mask_brush.h"
#include "level/wear.h"
#include "scene/node_animator.h"
#include "project/game_scripts.h"
#include "project/project_shaders.h"
#include "project/libraries.h"
#include "script/script_api.h"
#include "project/project.h"
#include "scene/scene.h"

#include <functional>
#include <map>
#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fx {

class AnimPreview;
class ParticleSystem;
class RuntimeMeshes;
class LevelMeshes;
namespace level {
class PolyMesh;
}
class AnimationSystem;

class AudioSystem;
class GraphLibrary;
class SkinningSystem;
struct AudioBakedData;
class GpuDrivenRenderer;

class EditorPrefs;
class FlyCamera;
class MaterialLibrary;
class GpuDevice;
class JsonWriter;
class LightSystem;
class ScenePak;
class SceneView;
class TerrainEditor;
class UiLayer;
class VirtualShadowMap;
struct CameraState;
struct FrameSettings;
struct Json;

struct EditorFrameStats {
    double frameMs = 0.0, scriptMs = 0.0, physicsMs = 0.0, animationMs = 0.0, uiMs = 0.0;
    uint32_t objects = 0, clusters = 0, vsmPages = 0, vtResident = 0, vtCapacity = 0, bodies = 0, characters = 0;
    double triangles = 0.0;
    uint64_t uiFrames = 0;
    double uiCopyMs = 0.0;
};

class EditorBridge : public LevelEditorLink {
public:
    struct Init {
        Scene* scene = nullptr;
        ScenePak* pak = nullptr;
        const char* scenePath = nullptr;
        const char* backend = "";
        const SceneEdits* edits = nullptr;               // as applied at boot
        std::vector<PrimitivePool>* pools = nullptr;     // slots for created cubes / spheres
        FrameSettings* settings = nullptr;               // live render / post settings
        LightSystem* lights = nullptr;
        VirtualShadowMap* vsm = nullptr;
        FlyCamera* camera = nullptr;
        GpuDevice* gpu = nullptr;          // vsync
        const GpuDrivenRenderer* renderer = nullptr;  // the upscaler's status
        EditorPrefs* prefs = nullptr;      // settings, tools, page layout, per-scene sun / camera
        TerrainEditor* terrain = nullptr;  // terrain tools (editor.terrain.*), brush strokes
        const char* projectDir = nullptr;  // the project the scene is in (--project), if any
        bool relaunch = true;              // switching scenes starts another editor (false: tests record it)
        MaterialLibrary* materials = nullptr;  // material assets -> material indices (assigned, edited live)
        WaterSystem* water = nullptr;          // water bodies (created, edited live)
        AudioSystem* audio = nullptr;          // previews, sound edits, the project's volumes
        AnimationSystem* animation = nullptr;  // Animators bind their graphs
        GraphLibrary* graphs = nullptr;        // the project's animation graphs
        const SkinningSystem* skinning = nullptr;
        BoneEntities* bones = nullptr;          // expanded skeletons (Expand Bones)
        AnimPreview* animPreview = nullptr;    // the Animator tab's preview pane
        ParticleSystem* particles = nullptr;   // particle effects (their preview, counts)
        const RuntimeMeshes* runtimeMeshes = nullptr;  // particles' models' and spawned meshes' slots
        const LevelMeshes* levelMeshes = nullptr;      // level meshes' live meshes (faces under a pick)
    };
    EditorBridge();
    void init(UiLayer& ui, const Init& init);
    ~EditorBridge();
    // (L4) The scene the editor works on, swapped in process: detachScene lets go of it (its
    // records, selection, undo, the systems it was given; its camera and sun remembered first),
    // attachScene takes the next one (init's scene parts) and sends the page its state. init()
    // attaches the first.
    void attachScene(const Init& init);
    void detachScene();
    // A scene to open (Open Scene, New Scene, a cook done; its path in the project), once.
    bool takeSceneSwitch(std::string& scene)
    {
        if (sceneSwitch_.empty())
            return false;
        scene = std::exchange(sceneSwitch_, {});
        return true;
    }
    // A game's Scene.Load while playing: the scene being edited kept as the editor has it (its
    // edits as before Play, unsaved changes, selection, undo) for Stop to come back to. False
    // (Play stopped, a notice) when it can't be kept: the terrain has unsaved changes.
    bool keepForPlay();
    // (R4) The project's shaders compiled so far (a change: their users load them again).
    uint64_t shadersRevision() const { return shaders_.revision(); }
    bool comingBack() const { return kept_ != nullptr; }
    // Stop: back to it. Its path in the project, and its edits as the file would have them (the
    // engine loads it with those); attachScene then puts back the rest.
    std::string keptScene() const;
    // Play with the network (the Play bar's Clients and Conditions; Project Settings > Network):
    // client processes to start with the game (0: none), --net-sim's conditions for them ("" none),
    // and the project's network settings.
    uint32_t playClients() const { return hasProject_ ? playClients_ : 0; }
    std::string playConditions() const;
    const NetworkSettings& networkSettings() const { return project_.network; }
    // The Network tab's data: the running game's network as JSON (Falcon.Network.Status), sent to
    // the page while the tab shows.
    void setNetworkStatus(std::function<std::string()> status) { netStatus_ = std::move(status); }
    std::string comeBack();
    // The scene just attached plays at once (the game went on into it).
    void playNow() { startPlay(); }
    void setSceneView(SceneView* view);
    // Writes the current settings, tools, sun and camera into the prefs (saved when changed).
    void persist();

    // Applies the page's commands. `camera` places created entities.
    void update(const CameraState& camera);
    // Twice a second: fps and timings for the toolbar and the profiler tab.
    void report(const EditorFrameStats& stats);
    // test.capture: the path to write the next composited frame to (empty: none), once;
    // {"viewport": true} captures the scene view alone (no page around it).
    std::string takeCaptureRequest() { return std::exchange(captureRequest_, {}); }
    bool captureViewport() const { return captureViewport_; }
    const std::string& captureView() const { return captureView_; }  // {"view": "anim"}: the Animator's preview
    uint32_t captureWidth() const { return captureWidth_; }  // > 0: resampled to this width (previews)
    // Rows changed by the editor that the extraction query cannot see (disabled entities).
    void extract(std::vector<InstanceUpdate>& rows);

    // Tests: page messages (a JSON array) applied one per frame from 0.5 s after the page connected
    // (as a person would: heavy page updates while its first frames are in flight can wedge
    // CEF 151's off-screen renderer),
    // plus test steps: {"type":"test.wait","frames":N | "ms":T}, {"type":"test.pick","x":0.5,"y":0.5}
    // (a GPU pick at a viewport fraction), {"type":"test.ping","n":N} (the page answers
    // editor.pong and repaints: proves it is alive, handling messages and painting),
    // {"type":"test.key","key":"z","ctrl":true,"shift":false} (a key press through SDL, the UI
    // layer and the page), {"type":"test.type","text":"abc"} (typed into the page's focused
    // field), {"type":"test.expect", selectedName (prefix), playing, paused, tool,
    // pong, canUndo, canRedo, settings {key: value}, scripts (none|unbuilt|building|loading|
    // ready|failed), game {running, entities, bodies}, hud (text it contains; "" for none)}
    // (a mismatch fails the run) and
    // {"type":"test.exit"}.
    bool loadScript(const char* path);
    bool exitRequested() const { return exitRequested_; }
    bool testFailed() const { return testFailed_; }
    bool connected() const { return connected_; }  // the page said it is ready (and has the state)
    // The Audio tab's probes in the Scene view (editor.audio.bake.show): the baked data's probes
    // and the region the next bake covers.
    bool showAudioProbes() const { return showAudioProbes_ && audio_; }
    const AudioBakedData* audioBaked() const;
    bool audioBakeRegion(float3& lo, float3& hi) const;

    // Edit mode: scripts, physics and animation hold still until Play; Pause holds them again
    // and Step runs one frame. True when this frame simulates (consumes a step).
    bool simulateFrame();
    bool playing() const { return playing_; }
    // Level meshes changed by editor code while editing (the level API): an entity's mesh replaced,
    // and a new level mesh entity (selected), each an undo step. False / 0 while playing, or for an
    // entity that isn't the editor's (the caller changes the scene directly then).
    bool setLevelMesh(flecs::entity_t entity, std::shared_ptr<const level::PolyMesh> mesh);
    flecs::entity_t createLevelMesh(const float* rows, flecs::entity_t parent, std::shared_ptr<const level::PolyMesh> mesh);
    // Stops playing (the game asked to quit: Game.Quit), as the Stop button does.
    // Scene.Load while playing: Play stops (the editor plays the scene it has open), with a notice.
    std::vector<std::string> notices_;  // the last few shown (tests: "notice")
    void stopPlaying()
    {
        if (playing_)
            handle(Json::parse("{\"type\":\"editor.stop\"}"), lastCamera_);
    }

    // The project's C# game (project/game_scripts.h). File > Build Scripts builds it, and so does
    // Play when a source changed since the last build (Play starts once it is built and loaded).
    // The engine starts the game when playing() turns true and, when it turns false, stops it
    // and then calls restorePlay.
    std::string projectGame() const;  // its build, to load at startup ("": none yet)
    // A build finished: the engine loads it (or reloads the code), then reports with gameLoaded.
    bool takeBuiltGame(std::string& dll);
    void gameLoaded(bool ok);
    struct GameStatus {
        bool loaded = false, running = false;
        uint32_t entities = 0, bodies = 0;  // alive entities the game made; dynamic bodies
    };
    void setGameStatus(const GameStatus& status);  // each frame (the page, tests)
    // The game's text over the view (Hud.Show): to the page when its revision changes.
    void setGameHud(const std::string& text, uint64_t revision);
    // After Stop (and the game's end): every entity back as it was before Play.
    void restorePlay();
    // Entities placed from a model that has animation, each with its track (Play runs them).
    std::vector<NodeAnimator::Link> animationLinks() const;
    uint64_t commandCount() const { return commands_; }

    // Scene view (gizmos, picking). Entities are indices (id - 1); ~0u is none.
    uint32_t selected() const { return selected_; }
    // The selection: every selected entity (the primary, selected(), among them: the last one
    // clicked; the gizmo and the Inspector follow it), and those without a selected ancestor (what
    // moves, deletes and duplicates).
    const std::vector<uint32_t>& selection() const { return selection_; }
    std::vector<uint32_t> selectionRoots() const;
    uint32_t parentOf(uint32_t index) const;  // ~0u: none
    // Selection modes of a click or a box: replace, add (Shift), toggle (Ctrl).
    enum class SelectMode { Replace, Add, Toggle };
    void selectMode(uint32_t index, SelectMode mode);
    void selectMany(const std::vector<uint32_t>& indices, SelectMode mode);
    // Entities a box in the view selects: each editable, shown entity's world centre (its mesh
    // bounds' centre, else its origin).
    void selectableCenters(std::vector<std::pair<uint32_t, float3>>& out) const;
    // A drag of several entities: each one's world live, then one undo step for all of them.
    void commitWorlds(const std::vector<std::pair<uint32_t, std::array<float, 12>>>& before);

    // Level meshes in the view (element modes, snapping): an entity's mesh (null: not one).
    std::shared_ptr<const level::PolyMesh> levelMeshOf(uint32_t index) const;
    // A mesh shown while a drag goes on (no undo step), then the drag's end: one undo step from
    // the mesh it started with.
    void setLevelMeshLive(uint32_t index, std::shared_ptr<const level::PolyMesh> mesh);
    void commitLevelMesh(uint32_t index, std::shared_ptr<const level::PolyMesh> before);
    // The level tools' element selection: vertices, edges (as their two vertices' pair key) or faces
    // of one level mesh (the primary selection). Mode 0: objects (no elements).
    struct ElementSelection {
        uint32_t entity = ~0u;
        uint32_t mode = 0;  // 0 object, 1 vertex, 2 edge, 3 face
        std::vector<uint64_t> items;  // vertex index, edge key (lo << 32 | hi), face index
    };
    ElementSelection& elements() { return elements_; }
    const ElementSelection& elements() const { return elements_; }
    // Points to snap to (world): level meshes' vertices and edges' middles, other meshes' bounds corners, without
    // the entities in `skip` (what is being dragged).
    void snapPoints(const std::vector<uint32_t>& skip, std::vector<float3>& out) const;
    // The nearest surface along a ray: level meshes exactly, other meshes by their bounds, the
    // terrain, the ground (y = 0); without the entities in `skip`. False: none.
    // `record` / `face`: the level mesh and its face hit (~0u: something else).
    bool surfaceHit(float3 origin, float3 dir, const std::vector<uint32_t>& skip, float3& point, float3& normal, uint32_t* record = nullptr,
                    uint32_t* face = nullptr) const;
    // The level paint brush (editor/level_paint.cpp; SceneTool::Paint): a stroke on the selected level
    // mesh along the mouse's ray, its mask (layers, erase) or vertex colours changed live, one undo
    // step at its end. paintHover: where the brush is (false: off the mesh). `missed` (paintBegin): the
    // ray is off the selected level mesh (or none is selected), so the click is an ordinary one.
    bool paintBegin(float3 origin, float3 dir, bool* missed = nullptr);
    void paintTo(float3 origin, float3 dir);
    void paintEnd();
    // The spline tool (editor/level_splines.cpp; SceneTool::Spline): a spline entity's points and
    // curve in world space (false: not a spline); a point added (or put in between: `insert`) where
    // the ray meets a surface (a new spline when none is selected), taken out, a corner toggled (-1:
    // closed); a point dragged over the surfaces (one undo step at the end).
    bool splineWorld(uint32_t index, std::vector<float3>* controls, std::vector<float3>* curve = nullptr, bool* closed = nullptr) const;
    // Arranging (editor/level_arrange.cpp): the selection's roots aligned on an axis to the primary
    // one's bounds ("align", edge -1 min / 0 centre / 1 max), spread evenly ("distribute"), dropped
    // onto what is under them ("ground"); grouped under a new empty, ungrouped; replaced with a
    // prefab. Each one undo step. An entity's world bounds: its subtree's meshes (else its origin).
    void arrange(const std::string& op, uint32_t axis, int32_t edge);
    // The measure tool's ruler (SceneView): its length and its run along each axis, in the hints bar.
    void measuredTo(float3 a, float3 b);
    // The navigation mesh (editor/level_navigation.cpp): baked from the scene's level meshes (as
    // drawn) and meshes for the project's agent, saved beside the scene, the scene's navigation
    // from then on; taken away; baked again a moment after the scene changes (the project's
    // navAutoBake, once there is one); its polygons' edges for the view's overlay.
    bool navBake(std::string& error);
    void navClear();
    void navUpdate();
    const std::vector<float3>& navLines() const { return navLines_; }
    // (editor/level_sockets.cpp) `world` (where the piece `root` is going) moved and turned so its
    // nearest snap point sits on another's within reach, facing it; false: none near.
    bool snapPiece(uint32_t root, float world[3][4]) const;
    void group();
    void ungroup();
    void replaceWith(const std::string& prefab);
    bool worldBounds(uint32_t root, float3& lo, float3& hi) const;
    void splineAdd(float3 origin, float3 dir, bool insert);
    void splineRemove(uint32_t point);
    void splineToggle(int32_t corner);
    bool splineDragBegin(uint32_t point);
    void splineDragTo(float3 origin, float3 dir);
    void splineDragEnd(bool cancel);
    bool paintHover(float3 origin, float3 dir, float3& point, float3& normal) const;
    uint32_t selectedSlot() const;   // the selection's GPU instance slot (outline), ~0u without a mesh
    // A GPU pick: ~0u (or a non-editable slot) clears the selection (Replace), else as the mode says.
    void selectSlot(uint32_t slot, SelectMode mode = SelectMode::Replace);
    // A GPU pick's answer (before selectSlot): its instance slot, scene cluster and triangle in it.
    // On a live mesh, the triangle of its source data (lastPick).
    void notePick(uint32_t slot, uint32_t cluster, uint32_t triangle);
    struct Pick {
        uint32_t slot = ~0u, cluster = ~0u, triangle = ~0u;
        int32_t live = -1, liveTriangle = -1;  // the live mesh and its source triangle (-1: none)
        int32_t face = -1;                     // a level mesh's face (-1: none)
    };
    const Pick& lastPick() const { return lastPick_; }
    void selectEntity(uint32_t index);
    bool editable(uint32_t index) const;
    bool worldOf(uint32_t index, float world[3][4]) const;
    // Mesh-space bounds of the entity's mesh (center, half extents); false without a mesh.
    bool meshBounds(uint32_t index, float3& center, float3& half) const;
    void setWorldLive(uint32_t index, const float world[3][4]);    // during a drag: no undo step
    void commitWorld(uint32_t index, const float before[3][4]);   // end of a drag: one undo step
    void toolsChanged();                                          // tool keys: tell the page
    TerrainEditor* terrainEditor() const { return terrain_; }
    // A finished brush stroke (TerrainEditor::endStroke's id): one undo step.
    void terrainEdited(uint32_t edit);
    // Files dropped on the window: imported into the project (the Project panel's folder).
    void dropped(const std::vector<std::string>& paths) { assets_.dropped(paths); }
    // Entities without a mesh, drawn and picked as icons in the scene view.
    void markers(std::vector<EntityMarker>& out) const;
    // Keys an editor test holds down for the game (test.hold), by SDL scancode.
    const uint8_t* testKeys() const { return testKeys_; }
    // The entity with this name (the Hierarchy's), 0 when none (scripts' Entity.Find).
    flecs::entity_t entityNamed(std::string_view name) const;
    // The same among `root`'s descendants (Entity.Find(name, under)).
    flecs::entity_t entityNamedUnder(flecs::entity_t root, std::string_view name) const;
    // (E2) Editor code (libraries' Editor/, Scripts/Editor/): the editor's API for it, the menu items
    // it adds (the script host's editorDescribe() after each load: the Tools menu), the menu items
    // run since the last call and the editor's events since (selection, play, stop, saved) as JSON:
    // the engine hands them to the scripts outside the editor's own message handling.
    const FxEditorApi* editorApi() { return &editorApi_; }
    void setTools(const std::string& json);
    // Editor code's commands bound to keys in the scene view ([EditorMenu(Key = "E", Modes = "face")],
    // the user's keymap over them): one for this key, modifiers and element mode runs (true). The
    // last one run again (Shift+R).
    bool commandKey(int32_t key, uint16_t mods, uint32_t elementMode);
    bool repeatCommand();
    // LevelEditorLink: the level API's view of the editor.
    bool viewportState(FxViewportState& out) override;
    void viewportCapture(bool on, bool wheel = false) override;
    void viewportHint(const char* text) override;
    void toolState(const char* json) override;
    int32_t elementsGet(uint64_t& entity, uint32_t& mode, uint64_t* items, int32_t capacity) override;
    void elementsSet(uint64_t entity, uint32_t mode, const uint64_t* items, int32_t count) override;
    bool meshLive(uint64_t entity, std::shared_ptr<const level::PolyMesh> mesh) override;
    bool meshCommit(uint64_t entity, std::shared_ptr<const level::PolyMesh> before) override;
    std::vector<std::string> takeToolRuns() { return std::exchange(toolRuns_, {}); }
    std::vector<std::string> takeEditorEvents() { return std::exchange(editorEvents_, {}); }
    // The game's HTML UI while playing (the project's UI/Game/index.html, framed over the game view):
    // the game's Ui.Send to it, and its messages since the last call (Ui.TryReceive).
    void gameUiSend(std::string_view json);
    std::vector<std::string> takeGameUiMessages() { return std::exchange(gameUiMessages_, {}); }
    // How many debug lines scripts draw this frame (tests: test.expect {debugLines}).
    void setDebugLineCount(uint32_t count) { debugLineCount_ = count; }
    // (X3) The gizmo lines this frame (test.expect {gizmoLines}); the selected entity (0: none).
    void setGizmoLineCount(uint32_t count) { gizmoLineCount_ = count; }
    uint64_t selectedEntity() const;
    // (X3) Component buttons pressed in the Inspector since the last call ([Button] methods), and
    // what one run while editing left its properties at (one undo step).
    struct ButtonRun {
        uint32_t index = 0;
        uint64_t entity = 0;
        std::string className, method, properties;
    };
    std::vector<ButtonRun> takeButtonRuns() { return std::exchange(buttonRuns_, {}); }
    void buttonDone(const ButtonRun& run, const std::string& properties);
    // The game's script component classes (the script host's describe(): JSON [{name, label,
    // properties: [{name, kind, ...}], defaults}]), after each build is loaded; and, while playing,
    // a running entity's components' values (JSON array by index).
    void setScriptTypes(const std::string& json);
    // (X2) The game's data components ([Component] structs): Add Component, the Inspector.
    void setDataTypes(const std::string& json);
    void setScriptValues(std::function<std::string(uint64_t)> values) { scriptValues_ = std::move(values); }
    // The scene's script components as the game starts them: every entity's (enabled ones).
    std::vector<std::pair<uint64_t, std::vector<ScriptEntry>>> sceneScripts() const;
    // An entity by its key in the edits (a script's Entity property), 0 when none.
    flecs::entity_t entityByKey(const std::string& key) const;
    // An entity's name in the Hierarchy ("" when it has no record).
    std::string nameOfEntity(flecs::entity_t entity) const;

private:
    // One editor entity; its id is its index + 1.
    struct Record {
        flecs::entity_t entity = 0;
        EntityKind kind = EntityKind::Mesh;
        uint32_t slot = ~0u;    // GPU instance slot (meshes and primitives)
        uint32_t uid = 0;       // created entities: stable id in the edits file (0: cooked)
        uint32_t light = ~0u;   // cooked lights: index in the pak
        std::string primitive;  // EntityKind::Primitive: a fpak::kBuiltinNames entry
        std::string name;       // empty: the cooked name
        bool hidden = false;    // the eye: hides this entity and everything below it
        std::string material;   // meshes: the material asset it draws with ("": the mesh's own)
        // Prefab instances: every entity of one knows its root (the root itself included) and its
        // id in the prefab; the root knows the prefab.
        std::string prefab;          // the root: the prefab (.fprefab, relative to the project)
        uint32_t prefabRoot = ~0u;   // entity index of the instance root; ~0u: not in an instance
        std::string prefabChild;     // its id in the prefab ("0": the root)
        // Placed from a model: its node (or camera) there, for the model's animation.
        std::string model;
        int32_t modelNode = -1, modelCamera = -1;
        std::optional<AnimatorEdit> animator;  // its graph drives the skinned meshes of it and below
        std::vector<ScriptEntry> scripts;      // its script components (the game makes them at Play)
        // EntityKind::Bone: the character's root (entity index) and the bone (-1: the Skeleton group).
        uint32_t boneRoot = ~0u;
        int32_t bone = -1;
    };
    struct State {
        float world[3][4] = {};
        // Relative to the parent as captured (LocalTransform). Under a bone it is what comes back
        // (undo, Stop): the bone has moved with the animation since. Whatever sets a new world or
        // parent clears it (the world is used then).
        float local[3][4] = {};
        bool hasLocal = false;
        uint32_t parent = ~0u;  // entity index, ~0u for roots
        bool hidden = false, alive = false;
        std::string name;
        std::optional<Light> light;  // lights (the hidden flag is the editor's, not part of it)
        std::optional<CameraComponent> camera;
        std::optional<WaterBodyDesc> water;
        std::optional<PhysicsBody> body;  // (bodyId 0: what the edits keep)
        std::optional<Collider> collider;
        std::optional<AudioSource> audioSource;  // (voice 0: what the edits keep)
        std::optional<AudioListener> audioListener;
        std::optional<AcousticSurface> acoustics;  // meshes: their own acoustics (else the material's)
        std::string material;  // meshes
        std::optional<AnimatorEdit> animator;
        VfxSet vfx;  // particle and decal components (vfx/vfx_components.h)
        std::vector<ScriptEntry> scripts;  // script components (script/script_entry.h)
        std::vector<DataComponent> data;   // (X2) the game's data components (by name, as bytes)
        std::shared_ptr<const level::PolyMesh> levelMesh;  // level meshes (shared: never changed in place)
        std::shared_ptr<const std::vector<uint8_t>> levelMask;  // their painted mask (LevelMask), the same way
        std::optional<LevelWear> wear;  // their live wear's rules
        std::optional<LevelModifiers> modifiers;  // their modifier stack
        std::optional<LevelSpline> spline;  // a spline (its LevelMesh: its loft)
        std::optional<ScatterSet> scatter;  // a scatter set (the scatter brush's pieces under it)
        std::optional<SnapPoint> snap;      // a snap point (a kit piece joins another there)
        std::optional<LevelVolume> volume;  // a gameplay volume (its level mesh not drawn)
        std::optional<CutVolume> cut;  // a cut volume on it (level/level_meshes.h)
        std::optional<level::Wall> wall;     // a parametric wall (its LevelMesh follows it)
        std::optional<WallOpening> opening;  // an opening in its parent wall
        std::optional<level::Roof> roof;     // a parametric roof (its LevelMesh follows it)
    };
    // An entity's prefab link (Record::prefab, prefabRoot, prefabChild).
    struct Link {
        std::string prefab;
        uint32_t root = ~0u;
        std::string child;
    };
    using Links = std::vector<std::pair<uint32_t, Link>>;
    struct Change {
        uint32_t index;
        State before, after;
        uint32_t terrainEdit = ~0u;  // a terrain stroke instead (TerrainEditor undo id)
        bool links = false;          // prefab links instead (Save as Prefab, Apply, Unpack)
        Links linksBefore, linksAfter;
    };
    using Command = std::vector<Change>;  // undone in reverse order

    void handle(const Json& m, const CameraState& camera);
    bool probePoint(const Json& m, float& x, float& y);
    std::string probeMismatch(const Json& expected) const;
    State capture(uint32_t index) const;
    // (X2) The game's data components an entity has (those of a known name: its scripts' types and
    // the scene's saved ones), and setting exactly those (the others of a known name taken off).
    std::vector<DataComponent> captureData(flecs::entity e) const;
    void applyData(flecs::entity e, const std::vector<DataComponent>& data);
    const Json* dataType(const std::string& name) const;
    // The Inspector's particle and decal components: the schema, each one's fields, the effect it
    // works for (and that effect's particles).
    void writeVfxInspector(JsonWriter& w, uint32_t index, const State& s) const;
    // batch: no propagate or row refresh (the caller does them once, for many entities).
    void apply(uint32_t index, const State& s, bool batch = false);
    void execute(const std::vector<std::pair<uint32_t, State>>& after);  // apply + push one undo step
    void select(uint32_t index);
    flecs::entity entityOf(uint32_t index) const;
    std::vector<uint32_t> subtree(uint32_t index) const;  // index and its descendants, parents first
    bool hiddenBelow(uint32_t index) const;               // hidden itself or under a hidden entity
    void refreshRows(uint32_t index);                     // GPU rows of the subtree (visibility, world)
    uint32_t addRecord(Record r);
    // A new entity (disabled until its create command applies), or ~0u when no slot is free.
    // `material`: a mesh takes a slot of its displaced copy for that material when there is one.
    uint32_t spawn(EntityKind kind, const std::string& primitive, const float world[3][4], std::string name,
                   const std::string& material = std::string());
    // A prefab's instance: its entities spawned, their states added to `after` (executed by the
    // caller), its root at `place` (`standOn`: its meshes' bounds' bottom centre there) under
    // `parent`. The root's index (~0u: none; `error` says why).
    // An opening's own transform put at its frame on its wall (LevelMeshes::openingFrame); `slide`: its
    // distance along the wall and (`lift`) its sill first taken from where its transform was put
    // (moved up, down or along; the sill kept inside the wall; a prefab dropped: its own sill). False:
    // not an opening under a wall.
    bool frameOpening(State& s, bool slide, bool lift = true);
    // Where an opening goes ("at": a point of the view on a wall, else "distance" along the selected
    // wall): the wall and the distance along its path. False: no wall there (`onWall`: the point must
    // be on one, else the selected wall is used).
    bool openingPlace(const Json& m, uint32_t& wallIndex, float& distance, bool onWall);
    uint32_t spawnPrefab(const std::string& path, const float place[3][4], bool standOn, uint32_t parent,
                         std::vector<std::pair<uint32_t, State>>& after, std::string& error);
    // An entity's level tools components (its cut, wall, opening, modifiers, spline, live wear,
    // scatter set, snap point) as a prefab carries them, and back.
    static LevelParts levelPartsOfState(const State& s);
    static void setLevelPartsOfState(State& s, const LevelParts& parts);
    // A material put on (or taken off) a mesh entity: whether its displacement shows only after a cook.
    void noteDisplacement(uint32_t index, const std::string& material);
    std::string createdName(EntityKind kind, const std::string& primitive, uint32_t slot) const;
    // Where a ray through the view at (fx, fy) meets the scene: terrain, a mesh's box, the y = 0
    // plane, or 10 m ahead.
    // `entity`: the mesh hit there (~0u: none, or something nearer than any mesh).
    float3 placementPoint(float fx, float fy, const CameraState& camera, uint32_t* entity = nullptr) const;
    void setMaterialRow(uint32_t index, const std::string& material);  // the instance's flags
    // Add Component (Light, CameraComponent, PhysicsBody, Collider): whether the entity can take
    // one, or give up one it was given that way.
    bool canAddComponent(uint32_t index, const std::string& component) const;
    bool canRemoveComponent(uint32_t index, const std::string& component);
    // A new Collider of a shape (0 box, 1 sphere, 2 mesh), fitted to the entity's mesh.
    Collider defaultCollider(uint32_t index, uint32_t shape) const;
    // The acoustics a mesh gets from its material (its .fmat's, else generic), and a label for them.
    std::string materialAcoustics(uint32_t index, std::string& preset, AcousticParams& params) const;
    std::string materialSurface(uint32_t index, std::string& preset, SurfaceSound& sound) const;  // (likewise, its surface sounds)
    // Prefabs.
    const Prefab* prefabDef(const std::string& path);  // read once (again after a write)
    std::vector<uint32_t> instanceMembers(uint32_t root) const;  // parents first
    void localOf(uint32_t index, float out[3][4]) const;  // relative to its parent (world for roots)
    // What an instance changes on its prefab, by prefab id.
    std::vector<PrefabOverride> overridesOf(uint32_t root, const Prefab& def) const;
    // States that make an instance follow `def` (but for `keep`); the root keeps its transform and name.
    // A member as a prefab entity (its kind and components; another prefab's instance: a nesting,
    // its prefab and overrides, unless `nest` is off: the instance being applied), without id, parent or local.
    PrefabEntity prefabEntityOf(uint32_t index, bool nest = true) const;
    // Links as they are, set, and changed as an undo step (with the commands of the same group).
    Links linksOf(const std::vector<uint32_t>& indices) const;
    void setLinks(const Links& links);
    void changeLinks(const Links& after);
    // What an instance member changes on its prefab entity (`place`: its local, name and
    // visibility too); empty when nothing.
    PrefabOverride overrideOf(uint32_t index, const PrefabEntity& p, bool place) const;
    // Another instance after Apply changed the prefab's structure: its members the prefab no longer
    // has deleted, the ones it gained made (states into `after`, links set).
    void reshapeInstance(uint32_t root, const Prefab& def, std::vector<std::pair<uint32_t, State>>& after);
    void followPrefab(uint32_t root, const Prefab& def, const std::vector<PrefabOverride>& keep,
                      std::vector<std::pair<uint32_t, State>>& after) const;
    bool linkedChild(uint32_t index) const;  // in an instance, not its root
    uint32_t idByPath(const std::string& name) const;  // "Root/Child/..." (test names)
    // A mesh's bounds in its own space (center, half extents); false when it has none.
    bool meshBoundsOf(uint32_t mesh, float3& center, float3& half) const;
    std::string keyOf(uint32_t index) const;  // the edits file's parent key
    // Expanded skeletons: the bone records of a character's root (its Skeleton group first; none
    // when never expanded), whether they are shown, and new records for a skeleton just expanded.
    std::vector<uint32_t> boneRecords(uint32_t root) const;
    bool bonesExpanded(uint32_t root) const;
    bool canHold(uint32_t index) const;  // may be a parent: editable, or a shown bone
    std::vector<uint32_t> addBoneRecords(uint32_t root, const BoneEntities::Expanded& x);
    std::string defaultCookedLightName(uint32_t lightIndex) const;
    void setSetting(const std::string& key, const Json& value);
    // Settings saved with the project while one is open (project.falcon "rendering"), not with
    // the editor's prefs.
    static bool projectSetting(const std::string& key)
    {
        return key == "lodPixels" || key == "cullPixels" || key == "terrainPixels" || key == "detailDistance" ||
               (key.starts_with("net") && !key.starts_with("netPlay")) || key.starts_with("steam");
    }
    bool projectSettingsDirty_ = false;
    void writeSettings(JsonWriter& w) const;
    void writeTools(JsonWriter& w) const;
    void applyPrefs();
    std::string defaultName(uint32_t index) const;
    const std::vector<PrimitivePool>& pools() const;  // empty without pools
    void sendState(bool entities);
    void notice(const char* message, const char* level = "info");
    bool save();
    SceneEdits currentEdits();  // the scene's edits as the editor has them now
    void flushLog();
    // Live reload of the project's pages (its UI/ and its libraries' UI/: the game's UI, editor
    // windows, Inspector pages): the page reloads its frames showing them when their files change.
    void pollPageChanges();
    static void SDLCALL logOutput(void* user, int category, SDL_LogPriority priority, const char* message);

    UiLayer* ui_ = nullptr;
    Scene* scene_ = nullptr;
    ScenePak* pak_ = nullptr;
    SceneView* view_ = nullptr;
    FrameSettings* settings_ = nullptr;
    // Scene changes relight the GI: committed ones, and drags / previews once a frame.
    void sceneChanged();
    LightSystem* lights_ = nullptr;
    VirtualShadowMap* vsm_ = nullptr;
    FlyCamera* camera_ = nullptr;
    GpuDevice* gpu_ = nullptr;
    const GpuDrivenRenderer* renderer_ = nullptr;
    std::string upscalerStatus_;  // as last sent
    EditorPrefs* prefs_ = nullptr;
    TerrainEditor* terrain_ = nullptr;
    MaterialLibrary* materials_ = nullptr;
    WaterSystem* water_ = nullptr;
    AudioSystem* audio_ = nullptr;
    ParticleSystem* particles_ = nullptr;
    const RuntimeMeshes* runtimeMeshes_ = nullptr;
    const LevelMeshes* levelMeshes_ = nullptr;
    AnimationSystem* animation_ = nullptr;
    GraphLibrary* graphs_ = nullptr;
    const SkinningSystem* skinning_ = nullptr;
    BoneEntities* bones_ = nullptr;
    AnimEditor animEditor_;  // editor.anim.*: the Animator tab
    // An entity's Animator (or none) onto the characters of its subtree.
    void bindAnimator(uint32_t index);
    bool moveIkToGraph(AnimatorEdit& a, std::string& error);  // its own IK into its graph (editor.animator.set moveIk)
    uint32_t indexOfKey(const std::string& key) const;  // the record an edits key names (~0u: none)
    const std::vector<std::string>& graphAssets();  // the project's .fgraph files
    std::vector<std::string> graphAssets_;
    uint64_t graphAssetsNs_ = 0;
    int32_t waterBody(uint32_t index) const;  // the record's water body, or -1
    // The project's water materials (.fwater, relative to the project), rescanned every 2 s at most.
    const std::vector<std::string>& waterAssets();
    std::vector<std::string> waterAssets_;
    uint64_t waterAssetsNs_ = 0;
    std::unordered_map<std::string, Prefab> prefabs_;
    std::string sceneKey_;
    uint64_t persistNs_ = 0;
    std::string scenePath_, editsPath_, backend_;
    std::vector<PrimitivePool>* pools_ = nullptr;
    std::vector<Record> records_;
    uint8_t testKeys_[512] = {};
    std::unordered_map<flecs::entity_t, uint32_t> indexOf_;
    std::vector<uint32_t> indexOfSlot_;  // GPU instance slot -> entity index
    uint32_t nextUid_ = 1;
    Pick lastPick_;
    uint32_t selected_ = ~0u;
    std::vector<uint32_t> selection_;  // (selected_ among them, last)
    ElementSelection elements_;
    // The level tools' page messages (editor.level.*): the selected level mesh and its chosen
    // elements (none chosen yet: none, in the view's mode); a mesh operation on them as one undo
    // step (args: its own; the elements and the entity's transform added); the level mesh face at
    // a point of the view (fx, fy; <0: under the mouse).
    bool levelTarget(uint32_t& target, ElementSelection& chosen) const;
    bool applyLevelOp(uint32_t target, const ElementSelection& chosen, const std::string& op, Json args);
    bool levelFaceAt(float fx, float fy, uint32_t& record, uint32_t& face) const;
    void sendLevelUvs();
    void sendTrimSheets();  // the project's .ftrim files to the page (editor.trim.list)
    std::pair<std::string, uint32_t> lastTrim_;  // the last trim strip used (T: again)
    std::string pickedMaterial_;  // the eyedropper's (editor.level.pickMaterial)
    struct Paint {
        bool active = false;
        uint32_t target = ~0u;
        std::shared_ptr<const level::PolyMesh> meshBefore;
        std::shared_ptr<const std::vector<uint8_t>> maskBefore;
        std::vector<uint8_t> mask;            // the stroke's working mask
        level::MaskStroke stroke;             // (each texel: the most one dab gave it)
        std::shared_ptr<level::PolyMesh> mesh;  // the stroke's working mesh (vertex colours)
        float3 last = {};
        bool dabbed = false;
        bool stamping = false;  // a stroke of decals, damage or scattered pieces (entities made: an undo group)
        bool erasing = false;   // (scatter) Shift: the set's pieces taken out
        uint32_t stamps = 0;    // (their random turns and sizes)
    } paint_;
    uint32_t damageRuns_ = 0;  // (damageSelection: each run's holes somewhere new)
    std::vector<float3> navLines_;
    float measuredLength_ = -1.0f;  // (the ruler's, for tests; -1: none)
    bool navStale_ = false;        // the scene (or the agent) changed since the bake
    uint64_t sceneChanges_ = 0;    // edits, undos and redos (sceneChanged)
    uint64_t navCommands_ = 0;     // the scene's changes it was baked at
    uint64_t navChangedNs_ = 0;    // when they last changed
    struct SplineDrag {
        bool active = false;
        uint32_t target = ~0u, point = 0;
        State before;
    } splineDrag_;
    // The selected level mesh's face under a ray: the hit's face, its fan triangle's corners (indices
    // into the face) and barycentrics, the world point (false: none).
    bool paintRay(uint32_t target, float3 origin, float3 dir, uint32_t& face, uint32_t corners[3], float3& bary, float3& point) const;
    void paintDab(float3 point, uint32_t face, const uint32_t corners[3], float3 bary);
    void stampDecal(float3 point, float3 normal);  // (paint mode 5) a decal entity there, facing out
    // (paint mode 6) a damage cut there (on the face's edge or corner, as the tool says), under the
    // level mesh `record`.
    void stampDamage(uint32_t record, uint32_t face, float3 point, float3 normal);
    // One damage cut of that kind (a hole, a chip, a corner: its size) at `point`, out along
    // `normal`, under `record`, with its heap of rubble if `rubble` (and the tool) says; `n` its
    // random draw.
    void damageCut(uint32_t record, uint32_t face, float3 point, float3 normal, level::DamageSnap kind, uint32_t n, bool rubble = true,
                   float scale = 1.0f);
    void damageSettings(const Json& m);  // (editor.tool.set, editor.level.damage: spacing, randomness, density; told to editor code)
    // Damage over the selection, as the damage tool's size, strength, rubble and inside say
    // (editor.level.damage): the selected spline's curve (on the level meshes it runs over), the
    // chosen vertices, edges or faces of a level mesh (or those `named`), or every sharp outside edge
    // of one. One undo step; how many cuts (0: nothing to damage).
    uint32_t damageSelection(const ElementSelection* named = nullptr);
    // The nearest level mesh surface within `reach` of `p`: the mesh, its face, the point and the
    // way out there.
    bool nearestLevelSurface(float3 p, float reach, uint32_t& record, uint32_t& face, float3& point, float3& normal) const;
    // (paint mode 7, editor/level_scatter.cpp) the scatter brush: the set its pieces go under (the
    // selected one, or the one the selected piece is in; `make`: else a new one at `at`), a dab of
    // pieces in the brush's disc, the set's pieces under the brush deleted (Shift).
    uint32_t scatterSet(float3 at, bool make);
    void scatterDab(float3 point, float3 normal);
    void scatterErase(float3 point);
    bool uvPanel_ = false;        // the page's UV editor is open: its UVs follow every state (when they change)
    std::string lastUvs_;
    std::vector<Command> undo_, redo_;
    std::vector<uint32_t> streamed_;  // slots to stream explicitly this frame
    std::vector<std::pair<uint32_t, State>> playSnapshot_;
    // The project's game: its build, Play waiting for it, what the engine reports.
    void startPlay();
    bool buildScripts(bool asked = true);
    void updateScriptBuild();
    // The sources looked at (4 times a second, or now): a change builds them in the background,
    // once they have held still a moment (now: at once), so Play has nothing to build.
    void watchScripts(bool now);
    bool scriptsBehind() const;  // a change the background build has yet to start on
    std::string scriptsState() const;
    ScriptBuild scriptBuild_;
    GameBuild gameBuild_;  // File > Build Game
    ProjectShaders shaders_;  // (R2) the project's Shaders/, compiled as they change
    bool scriptWaitShaders_ = false;
    std::string gameBuildResult_;  // "ok" or the error of the last one (tests)
    bool scriptWaitGameBuild_ = false;
    void updateGameBuild();
    bool pendingPlay_ = false, builtGame_ = false, scriptWaitBuild_ = false;
    // (E1) The Library Manager: editor.libraries.{list, add, create, enable, trust, export, remove}.
    // Installs run on a thread and finish here (update()); a folder or zip picked in a dialog too.
    void handleLibraries(const std::string& op, const Json& m);
    void sendLibraries();
    void startInstall(const std::string& source, const std::string& location, const std::string& ref);
    void librariesChanged(bool code);  // the list sent, and the game built again when code changed
    std::mutex libraryMutex_;
    std::vector<libraries::InstallResult> installed_;  // finished installs (the install thread's)
    std::vector<std::pair<std::string, std::string>> picked_;  // (source, path) from a dialog
    uint32_t installing_ = 0;
    bool scriptWaitLibraries_ = false;  // test.wait {libraries}: until no install is running
    // (E2) The editor API (FxEditorApi), undo steps grouped by editor code, its menu items and events.
    FxEditorApi editorApi_ = {};
    CameraState lastCamera_ = {};
    uint32_t undoGroupDepth_ = 0;
    size_t undoGroupStart_ = 0;
    std::string toolsJson_ = "{}";
    std::vector<std::string> toolRuns_, editorEvents_, gameUiMessages_;
    std::string gameUiMessage_;  // the last editor.gameui (sent again to a reloaded page)
    DirectoryWatch pagesWatch_, libraryPagesWatch_;
    uint32_t pageReadies_ = 0, pageChanges_ = 0;  // tests: test.expect {pageLoads, pageChanges}
    struct CommandKey {
        std::string path;
        int32_t key = 0;
        uint16_t mods = 0;      // SDL_KMOD_SHIFT etc. that must be held (others must not)
        uint32_t modes = ~0u;   // element modes it works in (bit per mode; all: any)
    };
    std::vector<CommandKey> commandKeys_;
    std::string lastCommand_;   // (Shift+R)
    std::string keymap_ = "{}"; // the user's keymap ({path: key}): the page's prefs, kept here too (--no-prefs)
    std::string hint_;          // editor code's text for the key hints bar
    std::string toolState_;     // editor code's commands' state (their settings): the Level window's
    uint64_t captureEndedNs_ = 0;  // when editor code last let the view go (Esc ended it: not Select None too)
    void parseCommandKeys();
    uint32_t debugLineCount_ = 0;
    uint32_t gizmoLineCount_ = 0;
    std::vector<ButtonRun> buttonRuns_;
    void showGameUi(bool show);  // the page frames the project's game UI (Play) or drops it (Stop)
    uint32_t eventSelected_ = ~0u;
    bool eventPlaying_ = false;
    std::string editorQuery(const std::string& what) const;
    void undoBegin();
    void undoEnd();
    // Stamps (gamescripts::sourcesStamp; 0: none): the sources as last seen, those the last build
    // was of (and whether it failed), those the running one is of.
    uint64_t scriptSources_ = 0, scriptBuilt_ = 0, scriptBuilding_ = 0;
    uint64_t scriptScanNs_ = 0, scriptChangedNs_ = 0;
    bool scriptBuildFailed_ = false;
    bool scriptBuildAsked_ = false;  // the running build was asked for (File > Build Scripts): it says when it is done
    bool scriptAutoBuild_ = true;    // false once dotnet was not found (Build Scripts tries again)
    std::string scriptsStage_, scriptsError_;  // stage: "", "building", "loading", "failed"
    GameStatus game_;
    std::string hud_;
    uint64_t hudRevision_ = 0;
    bool playing_ = false, paused_ = false, connected_ = false, dirty_ = false, exitRequested_ = false;
    std::string scriptTypesJson_ = "[]";
    Json scriptTypes_;  // parsed: each class's properties (their kinds: entity properties are keys)
    std::string dataTypesJson_ = "[]";
    Json dataTypes_;  // (X2) parsed: each data component's size, align, fields (name, kind, offset)
    std::map<std::string, std::pair<uint32_t, uint32_t>> dataLayouts_;  // known names: size, align
    std::function<std::string(uint64_t)> scriptValues_;
    // A script's properties as the Inspector shows them (entity keys as entity ids) and back.
    std::string scriptPropsForUi(const ScriptEntry& e) const;
    const Json* scriptType(const std::string& className) const;
    bool testFailed_ = false;
    // The project (when the scene is in one): its scenes, the startup one, the current one.
    // Another scene (or the launcher) means another process: save (when asked), start it, exit.
    Project project_;
    bool hasProject_ = false, relaunch_ = true;
    std::string sceneInProject_;  // the scene's path relative to the project
    std::string launched_;        // --no-relaunch: the last command that would have run
    std::string sceneSwitch_;     // a scene to open in process (takeSceneSwitch)
    uint32_t sceneSwitches_ = 0;  // scenes opened in process (tests)
    struct Kept;                  // the scene a game's Scene.Load left while playing (keepForPlay)
    std::unique_ptr<Kept> kept_;
    bool returning_ = false;  // the next attachScene is the kept scene coming back (comeBack)
    void switchScene(const std::string& scene, bool save);
    void switchTo(const std::vector<std::string>& args, bool save);
    // A scene whose models changed since it was cooked is cooked here first, in the background
    // (editor.cook messages: the Project panel's progress), then opened.
    struct SceneCook {
        std::string scene;
        std::future<bool> done;
        std::string error;
        std::mutex lock;
        CookProgress progress;
        std::atomic<bool> cancel{false};
        std::string sent;  // the last progress sent to the page
    };
    std::unique_ptr<SceneCook> cook_;
    std::vector<std::string> cookStages_;  // the last cook's stages (test.expect "cookStages")
    uint32_t cookReused_ = 0;
    bool scriptWaitCook_ = false;
    void openScene(const std::string& scene, bool save);
    void updateCook();
    // The bake's status to the page as it changes, and notices as one finishes or goes stale.
    void updateAudioBake();
    std::string bakeSent_, bakeState_, bakeError_;
    bool scriptWaitBake_ = false, showAudioProbes_ = false;
    bool saveScene();
    uint32_t lastPong_ = 0;
    uint64_t pingFrames_ = 0;  // page frames when the last ping went out
    std::string lastProbe_;    // the page's last editor.probed message (test.probe)
    std::string typed_;        // test.type's text (SDL's text event points at it)
    std::string captureRequest_;  // test.capture: PNG of the composited window, page included
    bool captureViewport_ = false;
    std::string captureView_;
    uint32_t captureWidth_ = 0;
    ProfilerStream profiler_;  // the Profiler tab (profiler.* messages)
    uint32_t playClients_ = 0, playConditions_ = 0;  // the Play bar's network choices
    std::function<std::string()> netStatus_;
    bool netSubscribed_ = false;  // the Network tab shows (net.subscribe)
    uint64_t netSentNs_ = 0;
    AssetBrowser assets_;      // the Project panel (editor.assets.* messages)
    uint32_t scriptWait_ = 0;
    bool scriptWaitGenerated_ = false;
    bool scriptWaitWear_ = false;  // until no level mesh's live wear is still to make
    uint32_t scriptWaitWearFrames_ = 0;  // (frames first: what was just asked for seen)
    bool scriptWaitCuts_ = false;  // until no level mesh cut is being made
    // An entity drawn clipped (its level mesh, or the art under its cut copy, isn't closed: cut
    // without caps): a badge in the Hierarchy and the Inspector; the list sent again when it changes.
    bool clippedOf(uint32_t index) const;
    uint32_t clippedVersion_ = 0;
    bool scriptWaitPreviews_ = false;
    bool scriptWaitGi_ = false;  // until the GI probes all sleep
    uint32_t scriptWaitProbe_ = 0;  // frames left for the page to answer a test.probe (the script holds)
    uint64_t scriptWaitUntilNs_ = 0;
    uint64_t connectedNs_ = 0;
    uint32_t steps_ = 0;
    bool liveDirty_ = false;  // a drag moved something since the last state message
    bool liveMoved_ = false;  // a drag or preview moved something this frame (the GI follows)
    float liveRegion_[6] = {};  // world bounds of the meshes it moved, before and after
    void growLiveRegion(uint32_t index);
    std::optional<State> previewBase_;  // state before a run of transform previews
    uint32_t previewIndex_ = ~0u;
    // Log lines from any thread, sent to the page's console each update.
    std::mutex logMutex_;
    std::vector<std::pair<int, std::string>> log_;
    SDL_LogOutputFunction previousLog_ = nullptr;
    void* previousLogData_ = nullptr;
    bool logHooked_ = false;
    std::vector<std::string> script_;
    size_t scriptNext_ = 0;
    uint64_t commands_ = 0;
};

}  // namespace fx
