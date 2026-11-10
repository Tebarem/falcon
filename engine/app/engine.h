#pragma once

#include "gpu/device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct EngineConfig {
    const char* title = "Falcon";
    int width = 1600, height = 900;
    GpuConfig gpu;
    const char* capturePath = nullptr;  // write the scene of frame `captureFrame` to PNG, then exit
    uint32_t captureFrame = 10;
    uint32_t capturePhase = 7;          // with TAA: capture on this jitter phase (mod the cycle length)
    // --capture-sequence N: frames captureFrame.. captureFrame + N - 1 each written as rendered
    // (<capture>_<frame>.png), nothing waited for: what a moving view shows frame by frame.
    uint32_t captureSequence = 0;
    uint32_t exitAfterFrames = 0;       // 0 = run until closed
    bool fixedTimestep = false;         // deterministic time (frame / 60) for captures
    const char* scenePath = nullptr;    // cooked .fpak; without one the M0 bring-up renderer runs
    int debugMode = -1;                 // force a debug view (see DebugView)
    bool naiveRenderer = false;         // M1 reference renderer (all leaf clusters, no culling)
    bool occlusion = true;
    bool coneCulling = true;
    bool alphaMask = true;              // off: alpha-masked materials are drawn opaque
    bool shadowCache = true;            // off: re-render every shadow page every frame
    float shadowBudgetMs = 2.0f;        // --shadow-budget: GPU ms a frame for moved lights' shadows (0: no cap)
    bool reflections = true;            // off: specular sees the ambient only (--no-reflections)
    bool forceRetest = false;           // debug: exercise the occlusion re-test path for every cluster
    float lodPixels = 1.0f;
    bool lodPixelsSet = false, cullPixelsSet = false;  // given on the command line (else a project's)
    bool terrainPixelsSet = false, detailDistanceSet = false;
    float detailDistance = 0.0f;        // --detail-distance: full detail, nothing culled for its size, nearer than this
    float cameraDolly = 0.0f;           // with fixed time: move forward this many units per second
    uint32_t vtPages = 1024;            // physical virtual-texture cache size (pages)
    uint32_t testLights = 0;            // extra random point lights (many-light stress test)
    uint32_t shadowedLights = 8;        // local lights with virtual shadow maps (max 32)
    float cameraLight = 0.0f;           // debug: point light this far in front of the start camera
    float sunScale = 1.0f;              // debug: sun intensity multiplier
    float movingLights = 0.0f;          // --moving-lights: the sun turns and local lights circle (shadow cost)
    bool taa = false;                   // temporal AA (--taa); off: sharper, no history
    bool fxaa = false;                  // --fxaa: FXAA on the tonemapped frame
    uint32_t msaa = 1;                  // --msaa 2|4|8: edge coverage samples per pixel
    uint32_t upscaler = 0;              // --upscaler off|taau|fsr|dlss (Upscaler)
    float cullPixels = 4.0f;            // --cull-pixels: skip instances smaller than this (projected radius; 0 = off)
    float renderScale = 1.0f / 1.5f;    // --render-scale S / --upscale-quality (while upscaling)
    float sharpness = 0.5f;             // --fsr-sharpness
    bool hidden = false;                // --hidden: never show the window (tests)
    bool gi = true;                     // probe GI for the ambient term (--no-gi: flat ambient)
    bool halfResAmbient = true;         // AO at half resolution (--full-res-ambient: off)
    bool profile = false;               // --profile: record zones, GPU marks and counters; log them
    uint32_t aoMode = 0;                // AoMode: 0 GTAO, 1 SSAO, 2 off
    float bloomIntensity = 0.12f;
    float exposureCompensation = 0.0f;  // EV on top of auto exposure
    float manualExposure = 0.0f;        // > 0: fixed exposure multiplier instead of auto
    float lookPower = 1.35f;            // AgX look: midtone contrast (1 = AgX base)
    float lookSaturation = 1.4f;        // AgX look: saturation (1 = AgX base)
    bool physics = true;
    uint32_t maxBodies = 4096;          // dynamic body pool (boxes + spheres)
    uint32_t physicsDrop = 0;           // drop this many bodies in front of the camera at start
    uint32_t jobThreads = 0;            // 0 = one per performance core (estimate)
    const char* scriptPath = nullptr;   // game scripts: managed assembly (JIT) or NativeAOT DLL
    bool hotReload = true;              // JIT: reload the game when its file changes
    uint32_t scriptReloadAt = 0;        // test: force a hot reload at this frame (0 = never)
    float freezeSeconds = 8.0f;         // --freeze-seconds: a freeze report after this long (0 = off)
    const char* freezeReports = nullptr;  // --freeze-reports DIR (default: FreezeReports/ beside the exe)
    uint32_t testFreezeFrame = 0, testFreezeMs = 0;  // --test-freeze FRAME:MS (tests)
    // test: tear the scene down and load it again (in process) at this frame and its multiples,
    // this many times (--reload-scene-at <frame>[,<count>]; 0 = never)
    uint32_t reloadSceneAt = 0, reloadSceneCount = 1;
    const char* animSet[8] = {};         // "Name=value" graph parameters set on every character at start
    uint32_t animSetCount = 0;
    const char* animEvents = nullptr;    // clip markup (.fevents) for every skeleton's clips, by name (a scene outside a project)
    uint32_t crowd = 0;                 // stress test: this many extra copies of each skinned instance
    int cameraView = -1;                // start camera: 0 inside, 1 outside, -1 auto (outside for < 100 instances)
    bool editor = false;                // editor shell (needs a UI build): simulation paused until Play
    const char* uiBundle = nullptr;     // HTML UI bundle directory (default with --editor: <exe>/falcon-ui/editor)
    bool sceneEdits = true;             // apply <scene>.edits.json saved by the editor
    bool editorPrefs = true;            // editor: load / save the user's editor preferences
    const char* editorScript = nullptr; // test: editor page messages to replay (JSON array)
    bool launcher = false;              // the project launcher (falcon.exe with no arguments; --launcher)
    bool relaunch = true;               // --no-relaunch: the launcher / editor only record what they would open (tests)
    const char* launcherScript = nullptr;  // test: launcher page messages to replay (JSON array)
    const char* projectPath = nullptr;  // --project <dir>: its startup scene (or --scene, relative to it)
    const char* previewPath = nullptr;  // --preview jobs.json: render asset previews (app/preview.h), then exit
    const char* libraryPath = nullptr;  // --library <dir>: the editor's My Library folder (tests; else the prefs' or Documents/Falcon Library)
    bool terrain = true;                // load <scene>.terrain/ (--no-terrain: don't)
    uint32_t terrainTiles = 0;          // --terrain N: a new N x N-tile terrain (in memory) instead
    float terrainSpacing = 0.5f;        // --terrain-spacing: its sample spacing (m)
    float terrainHills = 120.0f;        // --terrain-hills: its hills' height (m; 0 = flat)
    uint32_t terrainSeed = 1;           // --terrain-seed
    float terrainPixels = 1.0f;         // --terrain-pixels: patch levels' max projected error
    uint32_t terrainPatches = 16384;    // --terrain-patches: patch slots reserved for a terrain
    // --live-meshes / --live-clusters: live mesh slots and cluster blocks (128 vertices and
    // triangles each) reserved in the scene (render/live_geometry.h). 0: the default, 4096 and
    // 16384 in the editor (~68 MB), 64 and 512 in a game.
    uint32_t liveMeshes = 0, liveClusters = 0;
    float terrainRadius = 2048.0f;      // --terrain-radius: tiles kept resident around the camera (m)
    const char* terrainLayers = nullptr; // --terrain-layers grass,rock,...: generated layers on a --terrain
    float cameraPose[6] = {};              // --camera x,y,z,tx,ty,tz: start camera position and target
    bool hasCameraPose = false;
    const char* terrainGenerate = nullptr; // --terrain-generate <preset name | JSON stack>: run a generator stack at start-up
    const char* terrainScatter = nullptr; // --terrain-scatter mesh[:density[:budget]],... (a layer per entry) or a JSON layer list, on a --terrain
    bool water = true;                  // --no-water: water bodies not drawn
    const char* waterDemo = nullptr;    // --water-demo preset[:x,y,z,width,length]: a water body (tests)
    bool audio = true;                  // --no-audio: no sound at all
    bool audioNull = false;             // --audio-null: no playback device, mixed from the frame clock (--hidden too)
    const char* audioCapture = nullptr; // --audio-capture out.wav: also write the mix (32-bit float stereo, 48 kHz)
    // --audio-tone hz[:x,y,z] / --audio-play file[:x,y,z]: a test tone (amplitude 0.25) or a .wav /
    // .ogg played once at start, 2D, or at x,y,z in the listener's space (x right, y up, -z ahead).
    const char* audioTone = nullptr;
    const char* audioPlay = nullptr;
    uint32_t audioSpatializer = 0;      // --audio-spatializer auto|steam|pan (steam: fail without Steam Audio)
    bool audioAir = true;               // --audio-no-air: test sounds without air absorption
    int audioReflections = -1;          // --audio-reflections 0..3: off, low, medium, high (-1: the project's)
    // --audio-bake: bake the scene's acoustics (its <scene>.audio/ settings), write them, exit
    // (non-zero when the bake fails). Builds and tests bake without the editor.
    bool audioBake = false;
    // The simulation's determinism checks (sim/sim_recorder.h): a run recorded, or replayed and
    // compared tick by tick; a synthetic player's input (seed; -1: the real input).
    const char* simRecord = nullptr;
    const char* simReplay = nullptr;
    int32_t simDemo = -1;
    // --sim-rollback K: every frame, the networked simulation rolled back K ticks and simulated
    // forward again; each tick must come out as it did the first time (exit code 1 otherwise).
    uint32_t simRollback = 0;
    // The network (Falcon.Network): --host [[address:]port] (default 27015; 0: any; an address,
    // e.g. 127.0.0.1:0, listens there only: no firewall prompt for a local test), --connect <address>,
    // --net-sim latency,jitter,loss[,duplicate[,kbps]] around every transport, --net-clients N (the
    // host starts N client processes of this game and fails with them), --net-verify (clients
    // send hashes of what they replicated, the host checks them against its own history),
    // --net-child (given to those client processes: the run ends when the host goes).
    int32_t netHost = -1;
    std::string netHostAddress;
    const char* netConnect = nullptr;
    const char* netSim = nullptr;
    int32_t netClients = 0;
    bool netVerify = false;
    bool netChild = false;
    // --server: a dedicated server. No window, GPU, browser or audio device: the scene's entities,
    // physics, water, animation and scripts at the tick rate, hosting a session (port 27015 unless
    // --host gives one). Terrain loads around the networked characters.
    bool server = false;
    // A session's limits (the host): at most this many clients (0: no limit), and a password they
    // must give (--password; clients give it with --password too).
    int32_t maxPlayers = 0;
    // --bots N (with --connect): a headless load-testing process of N bot connections.
    int32_t netBots = 0;
    // --net-transport quic|steam|steamip: what a host listens with (default quic).
    const char* netTransport = nullptr;
    // --net-mode authoritative|rollback: how a session simulates (Network.Mode; clients get it too).
    const char* netMode = nullptr;
    // The project's network settings (Project::network as the launch JSON's "project"; set from
    // the project when the game runs one).
    std::string netProject;
    const char* password = nullptr;
    // --steam [appid]: start Steam for this App ID (default: the project's, else 480, Valve's
    // Spacewar). A project with a Steam App ID starts it without the flag.
    uint32_t steamAppId = 0;
    std::vector<std::string> args;  // the command line (after the executable), for client processes
};

// Parses --driver, --present, --frames-in-flight, --capture, --capture-frame,
// --frames, --fixed-time, --scene, --debug-view, --naive, --no-occlusion, --no-cone, --no-alpha-mask, --no-shadow-cache, --no-reflections, --force-retest, --lod-pixels, --detail-distance,
// --dolly, --vt-pages, --test-lights, --shadowed-lights, --camera-light, --sun-scale,
// --no-taa, --ao gtao|ssao|off, --bloom, --exposure (EV), --manual-exposure, --no-physics,
// --max-bodies, --physics-drop, --threads, --script, --no-hot-reload, --script-reload-at,
// --freeze-seconds, --freeze-reports, --test-freeze,
// --anim-set, --crowd, --view, --editor, --ui, --no-edits, --no-prefs, --editor-script, --taa,
// --fxaa, --msaa, --cull-pixels, --upscaler, --render-scale, --upscale-quality, --fsr-sharpness, --capture-phase,
// --look, --gpu-debug, --no-terrain, --terrain, --terrain-spacing, --terrain-hills, --terrain-seed,
// --terrain-pixels, --terrain-patches, --terrain-radius, --terrain-layers, --terrain-scatter, --terrain-generate, --camera, --launcher, --no-relaunch, --launcher-script, --project, --preview, --library,
// --no-audio, --audio-null, --audio-capture, --audio-tone, --audio-play, --audio-spatializer, --audio-no-air, --audio-reflections,
// --sim-record, --sim-replay, --sim-input-demo, --sim-rollback, --host, --connect, --net-sim,
// --net-clients, --net-verify, --net-child, --server, --max-players, --password. With no
// arguments at all: the launcher. Everything after "--" is left to Chromium. Returns false on unknown arguments.
bool parseEngineArgs(int argc, char** argv, EngineConfig& config);

int runEngine(const EngineConfig& config);

}  // namespace fx
