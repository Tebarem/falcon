#pragma once

#include "assets/fpak_format.h"
#include "scene/fly_camera.h"
#include "script/script_entry.h"
#include "render/post_process.h"
#include "script/script_api.h"
#include "audio/audio_mixer.h"
#include "net/net_transport.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <array>
#include <map>
#include <memory>
#include <vector>

namespace fx {

class Scene;
class ScenePak;
class PhysicsWorld;
class AnimationSystem;
class SkinningSystem;
class UiLayer;
class WaterSystem;
class Terrain;
struct ShaderFile;
class MaterialLibrary;
class MeshRenderer;
class LiveGeometry;
namespace level {
class PolyMesh;
}
class AudioSystem;
class ParticleSystem;
class PrefabSpawner;
class RuntimeMeshes;
struct FrameSettings;
struct InstanceUpdate;

// M7: C# scripting. Two modes behind one C ABI (script_api.h):
//   JIT  - the game is a managed assembly: CoreCLR is hosted through hostfxr, Falcon.Engine
//          loads the game into a collectible AssemblyLoadContext and hot-reloads it when the
//          file changes (state lives in the ECS, so it survives).
//   AOT  - the game is a NativeAOT DLL exporting falcon_script_init: no runtime, no JIT.
// The mode follows the file: a managed (CLI) image is JIT, a native one AOT.
class ScriptHost {
public:
    // Starts the .NET runtime (JIT) or loads the DLL (AOT) on a worker thread, so it overlaps
    // GPU device creation at boot.
    bool beginLoad(const char* gamePath, bool hotReload);
    // Main thread, once the scene exists: initializes Falcon.Engine (the game's code is loaded,
    // not started).
    bool attach(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation,
                const SkinningSystem* skinning, UiLayer* ui);
    // A game (no editor): attach, then startGame.
    bool start(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation,
               const SkinningSystem* skinning, UiLayer* ui);
    // The editor's Play: the game's OnStart. From here until stopGame, the entities the game
    // creates and the components it registers are tracked, and entities that existed before
    // and that it destroys are only disabled.
    bool startGame();
    // The editor's Stop: the game's OnStop, then its entities deleted (their bodies too), its own
    // components removed from every entity, the entities it destroyed or disabled enabled again,
    // the rows it changed (visibility, shadows, tint, material) and the names it gave put back.
    void stopGame();
    bool running() const { return running_; }
    uint32_t gameEntities() const;  // alive entities the running game made (editor: tracked), but its bodies
    // Each frame, before update: its wall time (the scripts' frame timing).
    void frameTime(float ms);
    // The game's text over the view, and a counter that changes with it.
    const std::string& hud() const { return context_.hud; }
    uint64_t hudRevision() const { return context_.hudRevision; }
    // Once per frame while the game runs: hot reload check, then the game's update.
    void update(float dt, double time, uint64_t frame, const CameraState& camera);
    // Unload the game code and load the current file again (JIT only); a running game starts
    // again on the new code. Returns false if the new code failed to load or the old code
    // could not be unloaded.
    bool reload();
    void shutdown();

    bool active() const { return managed_.update != nullptr; }
    // The water the scripts can ask about (Water.Sample).
    void setWater(const WaterSystem* water) { context_.water = water; }
    void setTerrain(const Terrain* terrain) { context_.terrain = terrain; }
    void setMaterials(MaterialLibrary* materials) { context_.materials = materials; }
    void setMeshes(MeshRenderer* meshes) { context_.meshes = meshes; }
    void setLive(LiveGeometry* live) { context_.live = live; }
    void setLevelEditorLink(LevelEditorLink* link) { context_.levelEditor = link; }
    // The editor's way of changing level meshes while editing (see Context::levelEdit).
    void setLevelEditor(std::function<bool(uint64_t, std::shared_ptr<const level::PolyMesh>)> edit,
                        std::function<uint64_t(const float*, uint64_t, std::shared_ptr<const level::PolyMesh>)> create)
    {
        context_.levelEdit = std::move(edit);
        context_.levelCreate = std::move(create);
    }
    // (R3) The game's post effects for the renderer: the enabled ones in order, each its compiled
    // shader's path and values.
    std::vector<PostProcess::EffectRequest> postEffects() const;
    // The audio the scripts play through (Audio.*), and what physics contacts sound like.
    void setAudio(AudioSystem* audio) { context_.audio = audio; }
    // Particle effects (Particles.*) and prefabs made while the game runs (Prefab.Spawn).
    void setParticles(ParticleSystem* particles) { context_.particles = particles; }
    void setPrefabs(PrefabSpawner* prefabs) { context_.prefabs = prefabs; }
    // Meshes made at runtime (Scene.SpawnMesh) from the pools reserved at upload, and materials by path.
    void setRuntimeMeshes(RuntimeMeshes* meshes) { context_.runtimeMeshes = meshes; }
    // Each frame, after the scene's rows: the rows the game changed (visibility, shadows, tint,
    // material; an entity destroyed), from the CPU instance table.
    void extract(std::vector<InstanceUpdate>& rows);
    // Decals placed by scripts (Decals.Place / Clear): the engine's decal system.
    using PlaceDecal = std::function<bool(const std::string& material, const float point[3], const float normal[3], float size, float depth,
                                          float angle, float lifetime, uint64_t attach)>;
    void setDecals(PlaceDecal place, std::function<void()> clear)
    {
        context_.placeDecal = std::move(place);
        context_.clearDecals = std::move(clear);
    }
    // Something the engine made for the running game (a collision prefab): the editor's Stop
    // deletes it with the game's own.
    void track(uint64_t entity)
    {
        if (context_.tracking && context_.createdSet.insert(entity).second)
            context_.created.push_back(entity);
    }
    // A ParticleScript's particles this frame: its controller's step (the game's C#), while it runs.
    void particleStep(uint64_t script, const char* className, void* particles, int32_t count, const void* hits, int32_t hitCount, float dt);
    // Entities by name (Find): the editor's names, or the scene edits' in a game.
    void setNames(std::function<uint64_t(std::string_view)> find) { context_.findName = std::move(find); }
    void setNamesUnder(std::function<uint64_t(uint64_t, std::string_view)> find) { context_.findNameUnder = std::move(find); }
    // (Scripts S3) Script components. The scene's, made when the game starts (the editor's records,
    // or a game's edits); a spawned prefab's (PrefabSpawner), made at the next update; an entity
    // property's key resolved; the classes the game has (after each load), a running entity's
    // values (the Inspector while playing), and each physics step's OnFixedUpdate.
    using SceneScripts = std::vector<std::pair<uint64_t, std::vector<ScriptEntry>>>;
    void setSceneScripts(std::function<SceneScripts()> provider) { sceneScripts_ = std::move(provider); }
    void addScripts(uint64_t entity, const std::vector<ScriptEntry>& scripts);
    void setEntityByKey(std::function<uint64_t(const std::string&)> find) { context_.entityByKey = std::move(find); }
    const std::string& componentTypes() const { return componentTypes_; }
    // (v45) The running game's network session as JSON (Falcon.Network.Status; "" none).
    std::string netStatus();
    const std::string& dataTypes() const { return dataTypes_; }  // (X2)
    std::string componentValues(uint64_t entity);
    // The simulation's tick (each physics step): OnFixedUpdate with the tick's input (keys as at
    // the tick, any tapped since the last one down for it; edges against the last tick; the
    // mouse's motion since the last tick), then afterStep once the step is done (its collision and
    // trigger events, phase 6). Ticks count from the game's start (Time.Tick).
    void fixedUpdate(float dt);
    void afterStep();
    uint64_t ticks() const { return ticks_; }
    // The networked simulation's rollback: its state put back as it was after the tick `ticksBack`
    // ticks before the last one, then those ticks simulated again with the input each had (only the
    // networked objects' components; no physics step). False when that tick's state isn't kept.
    bool rollback(uint64_t ticksBack);
    // The whole world rolled back too (physics: N5's rollback mode, the --sim-rollback test): the
    // engine puts physics back to the tick `from` and takes its steps again to `last`, each step's
    // scripts' tick simulated again with its kept input (resimulating()).
    void setPhysicsRollback(std::function<bool(uint64_t from, uint64_t last)> resimulate) { physicsRollback_ = std::move(resimulate); }
    // While physics takes a rollback's steps again: the tick its scripts simulate (~0: none).
    void resimulating(uint64_t tick) { resimTick_ = tick; }
    // (v44) The scripts' rollback (Network.Mode Rollback): the engine keeps physics for `ticks`
    // ticks when asked (its rollback set with setPhysicsRollback), and a past tick's physics hash.
    void setRollbackHooks(std::function<void(uint32_t)> window, std::function<uint64_t(uint64_t)> hash)
    {
        context_.rollbackWindow = std::move(window);
        context_.physicsHash = std::move(hash);
        context_.rollback = [this](uint64_t ticksBack) { return rollback(ticksBack); };
    }
    // FxManagedApi::simControl (0 restore, 1 mismatches, 2 window, 3 checked); 0 without a game.
    int64_t simControl(int32_t op, uint64_t arg) { return active() && managed_.simControl ? managed_.simControl(op, arg) : 0; }
    // The input a frame latched, before the game sees it: a recorder reads it, a replay writes it
    // (sim/sim_recording.h). keys: keyCount bytes (writable); text: typed this frame.
    struct FrameInput {
        uint8_t* keys;
        int32_t keyCount;
        FxMouse* mouse;
        FxGamepad* gamepads;  // 8
        std::string* text;
    };
    void setInputHook(std::function<void(FrameInput&)> hook) { inputHook_ = std::move(hook); }
    // (v38) The network: --net-sim's conditions around every transport the game opens, what the
    // command line asks of it (netLaunch's JSON), and how to start client processes (--net-clients).
    // (v40) A dedicated server's console lines (taken one at a time).
    void setServerConsole(std::function<bool(std::string&)> next) { context_.serverCommand = std::move(next); }
    void setNetwork(const net::Conditions& conditions, std::string launch, std::function<void(int32_t, const std::string&)> launchClients)
    {
        context_.netConditions = conditions;
        context_.netLaunch = std::move(launch);
        context_.launchClients = std::move(launchClients);
    }
    // (E2) Editor code: the editor's API (set once the editor is up), its per-frame update, a menu
    // item run, an event, and what it adds to the editor (menus; after each load).
    void setEditor(const FxEditorApi* editor) { native_.editor = editor; }
    // The game's HTML UI in the editor (no UiLayer of its own there): Ui.Send goes to `send`, and
    // the page's messages come in through uiReceive (Ui.TryReceive).
    void setUiSend(std::function<void(std::string_view)> send) { context_.uiSend = std::move(send); }
    void uiReceive(std::string json) { context_.uiInbox.push_back(std::move(json)); }
    // (S5) The game's time scale (1 when no game runs), and a quit the game asked for (taken).
    float timeScale() const { return running_ ? context_.timeScale : 1.0f; }
    // (v39) How fast ticks run against real time (a client's time dilation; 1 otherwise).
    float simRate() const { return running_ ? context_.simRate : 1.0f; }
    // Ticks this frame may run at most (a rollback session not to get further ahead of its inputs).
    uint32_t maxTicks() const { return running_ ? context_.maxTicks : ~0u; }
    // (v40) Where the game's characters are (terrain streaming and collision follow them).
    const std::vector<float3>& simFocus() const { return context_.simFocus; }
    bool takeQuit() { return std::exchange(context_.quitRequested, false); }
    // The scene is going (another follows, or the process ends): everything kept about it let go
    // (its systems, the lookups into it, what the game changed in it). attach() a new one after.
    void detachScene();
    // (L2) While the game runs, scenes one after another: unloadScene before the scene's systems
    // go (its components end, the game hears it), loadScene once the next one's are up (attached,
    // its components made, the game told; no game running yet: start() instead). The game, its
    // time scale, render settings, input and HUD stay.
    void unloadScene();
    // (X1) The frame's later phases (script_api.h FxManagedApi::phase): 3 after physics, 4 late, 5
    // before the render.
    enum Phase : int32_t { AfterPhysics = 3, LateUpdate = 4, PreRender = 5 };
    void phase(Phase phase, float dt);
    // The second half of loadScene once attached: the scene's components made, the game told.
    void continueGame();
    bool loadScene(Scene& scene, ScenePak& pak, PhysicsWorld* physics, AnimationSystem* animation, const SkinningSystem* skinning,
                   UiLayer* ui);
    // (S8) The scene playing (its path in the project), and the one a script asked to load next.
    void setScene(std::string projectDir, std::string scene)
    {
        context_.projectDir = std::move(projectDir), context_.scenePath = std::move(scene);
    }
    // (L3) Scene.LoadAsync's request, and how the engine's load of it goes (progress -1: none).
    bool takeSceneLoadAsync(std::string& scene)
    {
        if (context_.sceneAsync.empty())
            return false;
        scene = std::exchange(context_.sceneAsync, {});
        return true;
    }
    void setSceneLoading(float progress, std::string stage)
    {
        context_.loadProgress = progress, context_.loadStage = std::move(stage);
    }
    bool takeSceneLoad(std::string& scene)
    {
        if (context_.sceneNext.empty())
            return false;
        scene = std::exchange(context_.sceneNext, {});
        return true;
    }
    void editorUpdate(float dt);
    bool editorRun(const std::string& menu);
    void editorEvent(const std::string& json);
    const std::string& editorTools() const { return editorTools_; }
    // An entity's name (Entity.Name): the editor's, or the scene edits' (else its cooked name) in a game.
    void setNameOf(std::function<std::string(uint64_t)> nameOf) { context_.nameOf = std::move(nameOf); }
    // A window event: the mouse, for the game (the editor passes what happens over its game view).
    void event(const union SDL_Event& e);
    // Gamepads connected and disconnected (any time: the editor's game reads them while playing).
    void gamepadEvent(const union SDL_Event& e);
    // The game's view in the window, in the mouse's units (Input's view position; the camera's rays).
    void setView(float x, float y, float width, float height);
    // (S6) The frame's render settings scripts change (put back at Stop), and the debug lines they
    // draw (the editor shows them: lines() each frame, aged here).
    void setRenderSettings(FrameSettings* settings) { context_.render = settings; }
    struct DebugLine {
        float a[3], b[3];
        uint32_t color;
        float left;  // seconds (<= 0: one frame)
    };
    const std::vector<DebugLine>& debugLines() const { return context_.debugLines; }
    void ageDebugLines(float dt);
    // (X3) Each editor frame: the gizmos the components (the scene's while editing, the running ones
    // while playing) and the editor tools draw, `selected` the selected entity (0: none).
    void drawGizmos(uint64_t selected);
    const std::vector<DebugLine>& gizmoLines() const { return context_.gizmoLines; }
    // A component's [Button] method: run on the running one while playing (""), else on one made
    // from `properties` (a ScriptEntry's), its properties after returned ("": none or it failed).
    std::string componentButton(uint64_t entity, const std::string& className, const std::string& method,
                                const std::string& properties);
    // The view's camera before the game starts (OnStart reads Camera.*); each update sets it after.
    void setCamera(const CameraState& camera) { context_.camera = camera; }
    // Esc: the cursor the game holds (Input.MouseLocked) shown and free until a click in the
    // game's view (the game asking for it meanwhile waits). False when the game did not hold it.
    bool freeMouse();
    // Keys held down besides the keyboard's (an editor test's), by scancode; 512 of them.
    void setTestKeys(const uint8_t* keys) { testKeys_ = keys; }
    bool jit() const { return jit_; }
    const std::string& path() const { return path_; }

    // Native API callbacks operate on this.
    struct Context {
        Scene* scene = nullptr;
        ScenePak* pak = nullptr;
        // Component ids, found once (flecs' typed lookups cost tens of ns per call).
        uint64_t idMeshInstance = 0, idRigidBody = 0, idParent = 0, idPhysicsBody = 0;
        bool gravitySet = false;  // the game changed the world's gravity: Stop puts this back
        float gravity[3] = {};
        PhysicsWorld* physics = nullptr;
        AnimationSystem* animation = nullptr;
        const SkinningSystem* skinning = nullptr;
        UiLayer* ui = nullptr;     // in-game UI; may become active after start
        const WaterSystem* water = nullptr;
        const Terrain* terrain = nullptr;
        MaterialLibrary* materials = nullptr;
        MeshRenderer* meshes = nullptr;     // (R5: the process's)
        LiveGeometry* live = nullptr;       // the renderer's live meshes (level API)
        // Level meshes edited in the editor while editing: through it (an undo step). Each returns
        // false when it doesn't take it (playing): the scene is changed directly then.
        std::function<bool(uint64_t entity, std::shared_ptr<const level::PolyMesh> mesh)> levelEdit;
        std::function<uint64_t(const float* rows, uint64_t parent, std::shared_ptr<const level::PolyMesh> mesh)> levelCreate;
        LevelEditorLink* levelEditor = nullptr;  // the editor's scene view and element selection (null in a game)
        std::unordered_map<int32_t, uint64_t> liveEntities;  // the scripts' live meshes -> their entities
        std::vector<uint32_t> gameMeshes;   // the game's (freed at its end)
        struct PostEffect {
            std::string blob;  // Cache/Shaders/...fshader
            int32_t stage = 0, order = 0;
            bool enabled = true;
            std::vector<std::pair<std::string, std::array<uint32_t, 3>>> fields;  // name -> offset, components, int (1) / uint (2)
            std::vector<uint8_t> params;
        };
        std::map<uint32_t, PostEffect> postEffects;  // (the game's: kept across scenes)
        uint32_t nextPostEffect = 1;
        AudioSystem* audio = nullptr;
        ParticleSystem* particles = nullptr;
        PrefabSpawner* prefabs = nullptr;
        RuntimeMeshes* runtimeMeshes = nullptr;
        PlaceDecal placeDecal;
        std::function<void()> clearDecals;
        std::string uiMessage;     // polled, not yet taken (larger than the script's buffer)
        CameraState camera = {};
        // Frame timing: this frame's, and the last half second's (published every half second).
        float frameMs = 0.0f, frameMsAvg = 0.0f, frameMsMax = 0.0f, fps = 0.0f;
        double windowMs = 0.0, windowMax = 0.0;
        uint32_t windowFrames = 0;
        std::string hud;      // the game's text over the view
        uint64_t hudRevision = 0;
        // While a game started by the editor runs (tracking): what Stop undoes.
        bool tracking = false;
        std::vector<uint64_t> created;    // entities it created
        std::unordered_set<uint64_t> createdSet;
        std::vector<uint64_t> disabled;   // entities from before Play it destroyed (disabled instead)
        std::unordered_set<uint64_t> components;  // components it registered (any time)
        std::function<uint64_t(std::string_view)> findName;
        std::function<uint64_t(uint64_t, std::string_view)> findNameUnder;
        std::function<std::string(uint64_t)> nameOf;
        std::function<uint64_t(const std::string&)> entityByKey;
        std::function<void(std::string_view)> uiSend;  // the editor's game UI (no UiLayer)
        std::vector<std::string> uiInbox;               // its page's messages, oldest first
        std::vector<std::pair<uint64_t, ScriptEntry>> pendingScripts;  // script components to make (scriptPoll)
        std::unordered_map<uint64_t, std::string> names;  // names the game gave (over nameOf, findName)
        // Entities the game disabled (Enabled = false), each with the subtree it took with it:
        // enabling it again enables what it disabled, and nothing that was disabled before.
        std::unordered_set<uint64_t> scriptDisabled;
        // Instance rows the game changed: each as it was before (Stop puts it back) and why it is
        // hidden now (bits: 1 disabled or destroyed, 2 Renderer.Visible false).
        struct Row {
            fpak::InstanceGpu original;
            uint8_t hide = 0;
        };
        std::unordered_map<uint32_t, Row> rows;
        std::vector<uint32_t> touched;  // rows to stream on the next extract
        uint32_t meshWarnings = 0;
        // The mouse: this frame's (scripts read it), and what the events since then add up to.
        FxMouse mouse = {};
        float pendingDx = 0.0f, pendingDy = 0.0f, pendingWheel = 0.0f;
        uint32_t pendingButtons = 0, pendingPresses = 0;
        bool mouseKnown = false;  // a motion event seen (where it was)
        bool mouseFreed = false;  // Esc freed the cursor: the game holds it again after a click in its view
        // Gamepads (slots kept while connected), and text typed this frame while text input is on.
        FxGamepad gamepads[8] = {};
        void* gamepadHandles[8] = {};  // SDL_Gamepad*
        std::string text;
        bool textOn = false;
        float timeScale = 1.0f;
        bool quitRequested = false;
        bool stepEvents = false;  // phase 6 runs: contactPoll / triggerPoll give the step's events
        // (v38) The game's network transports (handle: index + 1), an event a too-small buffer left
        // waiting, the command line's conditions and request, and how to start client processes.
        struct Transport {
            std::unique_ptr<net::Transport> transport;
            bool waiting = false;
            net::Event event;
        };
        std::vector<Transport> transports;
        net::Conditions netConditions;
        std::string netLaunch = "{}";
        std::function<void(int32_t, const std::string&)> launchClients;
        uint64_t nextTick = ~0ull;  // (v39) simSetTick's, taken by the next tick
        float simRate = 1.0f;       // (v39) simSetRate's: the physics clock's speed against real time
        uint32_t maxTicks = ~0u;    // (v44) simMaxTicks's: ticks the next frame may run at most
        std::vector<float3> simFocus;  // (v40) simFocus's points
        std::function<bool(std::string&)> serverCommand;  // (v40) a server console's next line
        // (v44) The whole world's rollback: physics' window, and a past tick's physics hash.
        std::function<void(uint32_t)> rollbackWindow;
        std::function<uint64_t(uint64_t)> physicsHash;
        std::function<bool(uint64_t)> rollback;  // (ScriptHost::rollback)
        std::string projectDir, scenePath, sceneNext;  // (S8) the scene playing, the one asked for next
        std::string sceneAsync;                        // (L3) Scene.LoadAsync's
        float loadProgress = -1.0f;                    // how the engine's load goes (-1: none)
        std::string loadStage;
        FrameSettings* render = nullptr;
        std::vector<uint8_t> renderSaved;  // the settings as they were before the game changed them
        std::vector<DebugLine> debugLines;
        std::vector<DebugLine> gizmoLines;  // (X3) this editor frame's
        // (X4) Audio streams by id, and destroyed ones the mixer may still read (freed once their
        // voice is done).
        struct Stream {
            std::unique_ptr<AudioStream> stream;
            uint32_t voice = 0;
        };
        std::unordered_map<uint32_t, Stream> streams;
        std::vector<Stream> dyingStreams;
        uint32_t nextStream = 1;
        // Characters by the entity scripts name them by (a model's root: its first skinned record).
        std::unordered_map<uint64_t, uint32_t> characterRecords;
    };

private:
    bool loadJit();
    bool loadAot();
    void describeTypes();  // componentTypes_ from the loaded code

    std::function<SceneScripts()> sceneScripts_;
    std::string componentTypes_ = "[]";
    std::string dataTypes_ = "[]";  // (X2) the game's data components
    std::string gizmoScene_;        // (X3) the scene's script components as last given to drawGizmos
    std::string editorTools_ = "{}";

    std::string path_;
    bool jit_ = false, hotReload_ = false, loadOk_ = false, running_ = false;
    std::thread loader_;
    FxScriptInit init_ = nullptr;
    void* library_ = nullptr;  // AOT DLL / hostfxr
    void* hostContext_ = nullptr;
    FxNativeApi native_ = {};
    FxLevelApi level_ = {};
    FxManagedApi managed_ = {};
    Context context_;
    uint8_t keys_[512] = {}, keysPrev_[512] = {};
    const uint8_t* sdlKeys_ = nullptr;
    const uint8_t* testKeys_ = nullptr;
    std::function<void(FrameInput&)> inputHook_;
    // The ticks' input (fixedUpdate): keys at the tick and the last tick's, taps and mouse motion
    // since the last tick, mouse and pad buttons at the last tick.
    uint8_t tickKeys_[512] = {}, tickKeysPrev_[512] = {}, tickTaps_[512] = {};
    float tickDx_ = 0.0f, tickDy_ = 0.0f, tickWheel_ = 0.0f;
    uint32_t tickMouseTaps_ = 0, tickMousePrev_ = 0;
    uint32_t tickPadTaps_[8] = {}, tickPadPrev_[8] = {};
    uint64_t ticks_ = 0;
    // Each tick's input, by tick (a rollback's ticks are simulated again with it).
    struct TickInput {
        uint8_t keys[512];
        FxMouse mouse;
        uint32_t padButtons[8];
        float padAxes[8][6];
    };
    static constexpr uint64_t kTickInputs = 256;
    std::unique_ptr<TickInput[]> tickInputs_ = std::make_unique<TickInput[]>(kTickInputs);
    void runTick(float dt, uint64_t tick, int32_t flags);
    std::function<bool(uint64_t, uint64_t)> physicsRollback_;
    uint64_t resimTick_ = ~0ull;
    std::filesystem::file_time_type stamp_ = {};
    uint64_t lastPollNs_ = 0;
    int pendingChange_ = 0;  // polls the new file has been stable for
};

}  // namespace fx
