#include "app/engine.h"

#include "anim/anim_data.h"
#include "app/process.h"
#include "core/platform.h"
#include "anim/animation_system.h"
#include "anim/bone_entities.h"
#include "editor/prefab_spawn.h"
#include "vfx/decal_system.h"
#include "vfx/particle_system.h"
#include "vfx/runtime_meshes.h"
#include "level/level_meshes.h"
#include "level/prototype_materials.h"
#include "anim/graph_library.h"
#include "assets/scene_pak.h"
#include "audio/audio_system.h"
#include "core/boot.h"
#include "core/dmath.h"
#include "core/freeze_watch.h"
#include "core/jobs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/anim_preview.h"
#include "editor/editor_bridge.h"
#include "app/cook_window.h"
#include "app/preview.h"
#include "editor/launcher.h"
#include "editor/material_library.h"
#include "editor/editor_prefs.h"
#include "editor/scene_edits.h"
#include "editor/scene_view.h"
#include "editor/terrain_editor.h"
#include "gpu/capture.h"
#include "gpu/gpu_memory.h"
#include "gpu/gpu_profiler.h"
#include "physics/physics_world.h"
#include "render/bringup_renderer.h"
#include "render/gpu_driven_renderer.h"
#include "render/light_system.h"
#include "render/scene_renderer.h"
#include "render/skinning.h"
#include "render/virtual_shadow_map.h"
#include "render/virtual_texture.h"
#include "scene/fly_camera.h"
#include "scene/node_animator.h"
#include "scene/scene.h"
#include "project/asset_library.h"
#include "project/game_scripts.h"
#include "project/project_shaders.h"
#include "project/libraries.h"
#include "script/script_host.h"
#include "sim/sim_recorder.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "project/project.h"
#include "steam/steam_client.h"
#include "terrain/terrain_generate.h"
#include "terrain/terrain_scatter.h"
#include "ui/ui_layer.h"
#include "water/water_system.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <future>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace fx {

// --host's value, when there is one: a port, an IP with one (127.0.0.1:0, [::1]:0) or without.
static bool isHostValue(const char* v)
{
    return (v[0] >= '0' && v[0] <= '9') || v[0] == '[' || !std::strncmp(v, "localhost", 9);
}

bool parseEngineArgs(int argc, char** argv, EngineConfig& c)
{
    if (argc <= 1)
        c.launcher = true;  // falcon.exe alone: pick or make a project (a shipped game: below)
    c.args.assign(argv + std::min(argc, 1), argv + argc);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--"))
            break;  // the rest is for Chromium (CEF reads the process command line itself)
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        auto take = [&]() -> const char* {
            if (!next) {
                FX_ERROR("%s needs a value", a);
                return nullptr;
            }
            ++i;
            return next;
        };
        if (!std::strcmp(a, "--driver")) {
            if (!(c.gpu.driver = take())) return false;
        } else if (!std::strcmp(a, "--present")) {
            const char* v = take();
            if (!v) return false;
            if (!std::strcmp(v, "vsync")) c.gpu.presentMode = SDL_GPU_PRESENTMODE_VSYNC;
            else if (!std::strcmp(v, "immediate")) c.gpu.presentMode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (!std::strcmp(v, "mailbox")) c.gpu.presentMode = SDL_GPU_PRESENTMODE_MAILBOX;
            else { FX_ERROR("unknown present mode %s", v); return false; }
        } else if (!std::strcmp(a, "--frames-in-flight")) {
            const char* v = take();
            if (!v) return false;
            c.gpu.framesInFlight = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--capture")) {
            if (!(c.capturePath = take())) return false;
            c.fixedTimestep = true;
        } else if (!std::strcmp(a, "--capture-frame")) {
            const char* v = take();
            if (!v) return false;
            c.captureFrame = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--capture-sequence")) {
            const char* v = take();
            if (!v) return false;
            c.captureSequence = uint32_t(std::max(0, std::atoi(v)));
        } else if (!std::strcmp(a, "--capture-phase")) {
            const char* v = take();
            if (!v) return false;
            c.capturePhase = uint32_t(std::atoi(v)) % 8;
        } else if (!std::strcmp(a, "--frames")) {
            const char* v = take();
            if (!v) return false;
            c.exitAfterFrames = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--fixed-time")) {
            c.fixedTimestep = true;
        } else if (!std::strcmp(a, "--sim-record")) {
            if (!(c.simRecord = take())) return false;
        } else if (!std::strcmp(a, "--sim-replay")) {
            if (!(c.simReplay = take())) return false;
        } else if (!std::strcmp(a, "--host")) {
            c.netHost = 27015;
            if (i + 1 < argc && isHostValue(argv[i + 1])) {
                const std::string v = argv[++i];
                const size_t colon = v.rfind(':');
                if (v.find_first_of(".:[") == std::string::npos) {
                    c.netHost = std::clamp(std::atoi(v.c_str()), 0, 65535);
                } else if (colon != std::string::npos && (v[0] == '[' ? v.find(']') < colon : v.find(':') == colon)) {
                    c.netHostAddress = v.substr(0, colon);
                    c.netHost = std::clamp(std::atoi(v.c_str() + colon + 1), 0, 65535);
                } else {
                    c.netHostAddress = v;  // (no port: the default)
                }
            }
        } else if (!std::strcmp(a, "--net-transport")) {
            if (!(c.netTransport = take())) return false;
        } else if (!std::strcmp(a, "--net-mode")) {
            if (!(c.netMode = take())) return false;
        } else if (!std::strcmp(a, "--steam")) {
            c.steamAppId = UINT32_MAX;  // (the project's, else 480)
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9')
                c.steamAppId = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(a, "--connect")) {
            if (!(c.netConnect = take())) return false;
        } else if (!std::strcmp(a, "--net-sim")) {
            if (!(c.netSim = take())) return false;
            net::Conditions check;
            if (!net::parseConditions(c.netSim, check)) {
                FX_ERROR("--net-sim wants latency,jitter,loss[,duplicate[,kbps]] (ms, ms, %%, %%, kbit/s)");
                return false;
            }
        } else if (!std::strcmp(a, "--net-clients")) {
            const char* v = take();
            if (!v) return false;
            c.netClients = std::clamp(std::atoi(v), 0, 64);
        } else if (!std::strcmp(a, "--net-verify")) {
            c.netVerify = true;
        } else if (!std::strcmp(a, "--net-child")) {
            c.netChild = true;
        } else if (!std::strcmp(a, "--server")) {
            c.server = true;
        } else if (!std::strcmp(a, "--bots")) {
            const char* v = take();
            if (!v) return false;
            c.netBots = std::clamp(std::atoi(v), 0, 100000);
        } else if (!std::strcmp(a, "--max-players")) {
            const char* v = take();
            if (!v) return false;
            c.maxPlayers = std::clamp(std::atoi(v), 0, 100000);
        } else if (!std::strcmp(a, "--password")) {
            if (!(c.password = take())) return false;
        } else if (!std::strcmp(a, "--sim-rollback")) {
            const char* v = take();
            if (!v) return false;
            c.simRollback = uint32_t(std::clamp(std::atoi(v), 1, 200));
        } else if (!std::strcmp(a, "--sim-input-demo")) {
            const char* v = take();
            if (!v) return false;
            c.simDemo = std::max(0, std::atoi(v));
        } else if (!std::strcmp(a, "--scene")) {
            if (!(c.scenePath = take())) return false;
        } else if (!std::strcmp(a, "--debug-view")) {
            const char* v = take();
            if (!v) return false;
            c.debugMode = std::atoi(v);
        } else if (!std::strcmp(a, "--naive")) {
            c.naiveRenderer = true;
        } else if (!std::strcmp(a, "--no-occlusion")) {
            c.occlusion = false;
        } else if (!std::strcmp(a, "--no-shadow-cache")) {
            c.shadowCache = false;
        } else if (!std::strcmp(a, "--shadow-budget")) {
            const char* v = take();
            if (!v) return false;
            c.shadowBudgetMs = std::max(float(std::atof(v)), 0.0f);
        } else if (!std::strcmp(a, "--no-reflections")) {
            c.reflections = false;
        } else if (!std::strcmp(a, "--no-terrain")) {
            c.terrain = false;
        } else if (!std::strcmp(a, "--no-water")) {
            c.water = false;
        } else if (!std::strcmp(a, "--water-demo")) {
            if (!(c.waterDemo = take())) return false;
        } else if (!std::strcmp(a, "--terrain-layers")) {
            if (!(c.terrainLayers = take())) return false;
        } else if (!std::strcmp(a, "--camera")) {
            const char* v = take();
            if (!v) return false;
            float* p = c.cameraPose;
            if (std::sscanf(v, "%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) != 6) return false;
            c.hasCameraPose = true;
        } else if (!std::strcmp(a, "--live-meshes") || !std::strcmp(a, "--live-clusters")) {
            const char* v = take();
            if (!v) return false;
            (a[7] == 'm' ? c.liveMeshes : c.liveClusters) = uint32_t(std::max(std::atoi(v), 1));
        } else if (!std::strcmp(a, "--terrain-generate")) {
            if (!(c.terrainGenerate = take())) return false;
        } else if (!std::strcmp(a, "--terrain-scatter")) {
            if (!(c.terrainScatter = take())) return false;
        } else if (!std::strcmp(a, "--terrain") || !std::strcmp(a, "--terrain-spacing") || !std::strcmp(a, "--terrain-hills") ||
                   !std::strcmp(a, "--terrain-seed") || !std::strcmp(a, "--terrain-pixels") ||
                   !std::strcmp(a, "--terrain-patches") || !std::strcmp(a, "--terrain-radius")) {
            const char* v = take();
            if (!v) return false;
            const float f = float(std::atof(v));
            if (!std::strcmp(a, "--terrain")) c.terrainTiles = uint32_t(std::max(std::atoi(v), 0));
            else if (!std::strcmp(a, "--terrain-spacing")) c.terrainSpacing = std::max(f, 0.05f);
            else if (!std::strcmp(a, "--terrain-hills")) c.terrainHills = std::max(f, 0.0f);
            else if (!std::strcmp(a, "--terrain-seed")) c.terrainSeed = uint32_t(std::atoi(v));
            else if (!std::strcmp(a, "--terrain-pixels")) c.terrainPixels = std::max(f, 0.1f), c.terrainPixelsSet = true;
            else if (!std::strcmp(a, "--terrain-patches")) c.terrainPatches = uint32_t(std::max(std::atoi(v), 256));
            else c.terrainRadius = std::max(f, 64.0f);
        } else if (!std::strcmp(a, "--no-alpha-mask")) {
            c.alphaMask = false;
        } else if (!std::strcmp(a, "--force-retest")) {
            c.forceRetest = true;
        } else if (!std::strcmp(a, "--no-cone")) {
            c.coneCulling = false;
        } else if (!std::strcmp(a, "--lod-pixels")) {
            const char* v = take();
            if (!v) return false;
            c.lodPixels = float(std::atof(v));
            c.lodPixelsSet = true;
        } else if (!std::strcmp(a, "--detail-distance")) {
            const char* v = take();
            if (!v) return false;
            c.detailDistance = std::max(float(std::atof(v)), 0.0f);
            c.detailDistanceSet = true;
        } else if (!std::strcmp(a, "--dolly")) {
            const char* v = take();
            if (!v) return false;
            c.cameraDolly = float(std::atof(v));
        } else if (!std::strcmp(a, "--camera-light")) {
            const char* v = take();
            if (!v) return false;
            c.cameraLight = float(std::atof(v));
        } else if (!std::strcmp(a, "--sun-scale")) {
            const char* v = take();
            if (!v) return false;
            c.sunScale = float(std::atof(v));
        } else if (!std::strcmp(a, "--shadowed-lights")) {
            const char* v = take();
            if (!v) return false;
            c.shadowedLights = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--moving-lights")) {
            const char* v = take();
            if (!v) return false;
            c.movingLights = float(std::atof(v));
        } else if (!std::strcmp(a, "--test-lights")) {
            const char* v = take();
            if (!v) return false;
            c.testLights = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--vt-pages")) {
            const char* v = take();
            if (!v) return false;
            c.vtPages = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--taa")) {
            c.taa = true;
        } else if (!std::strcmp(a, "--no-taa")) {
            c.taa = false;
        } else if (!std::strcmp(a, "--fxaa")) {
            c.fxaa = true;
        } else if (!std::strcmp(a, "--cull-pixels")) {
            const char* v = take();
            if (!v) return false;
            c.cullPixels = std::max(float(std::atof(v)), 0.0f);
            c.cullPixelsSet = true;
        } else if (!std::strcmp(a, "--upscaler")) {
            const char* v = take();
            if (!v) return false;
            if (!std::strcmp(v, "off")) c.upscaler = 0;
            else if (!std::strcmp(v, "taau")) c.upscaler = 1;
            else if (!std::strcmp(v, "fsr")) c.upscaler = 2;
            else if (!std::strcmp(v, "dlss")) c.upscaler = 3;
            else {
                FX_ERROR("--upscaler takes off, taau, fsr or dlss");
                return false;
            }
        } else if (!std::strcmp(a, "--render-scale")) {
            const char* v = take();
            const float scale = v ? float(std::atof(v)) : 0.0f;
            if (!(scale >= 0.25f && scale <= 1.0f)) {
                FX_ERROR("--render-scale takes 0.25 to 1");
                return false;
            }
            c.renderScale = scale;
        } else if (!std::strcmp(a, "--upscale-quality")) {
            const char* v = take();
            int preset = -1;
            for (int k = 0; v && k < 5; ++k)
                if (!std::strcmp(v, kUpscalePresetNames[k]))
                    preset = k;
            if (preset < 0) {
                FX_ERROR("--upscale-quality takes native, quality, balanced, performance or ultra");
                return false;
            }
            c.renderScale = kUpscalePresets[preset];
        } else if (!std::strcmp(a, "--fsr-sharpness")) {
            const char* v = take();
            if (!v) return false;
            c.sharpness = std::clamp(float(std::atof(v)), 0.0f, 1.0f);
        } else if (!std::strcmp(a, "--msaa")) {
            const char* v = take();
            const int n = v ? std::atoi(v) : 0;
            if (n != 1 && n != 2 && n != 4 && n != 8) {
                FX_ERROR("--msaa takes 1, 2, 4 or 8");
                return false;
            }
            c.msaa = uint32_t(n);
        } else if (!std::strcmp(a, "--size")) {
            const char* v = take();
            if (!v || std::sscanf(v, "%dx%d", &c.width, &c.height) != 2 || c.width < 64 || c.height < 64) {
                FX_ERROR("--size takes WxH, e.g. 1920x1080");
                return false;
            }
        } else if (!std::strcmp(a, "--profile")) {
            c.profile = true;
        } else if (!std::strcmp(a, "--hidden")) {
            c.hidden = true;
        } else if (!std::strcmp(a, "--no-gi")) {
            c.gi = false;
        } else if (!std::strcmp(a, "--full-res-ambient")) {
            c.halfResAmbient = false;
        } else if (!std::strcmp(a, "--ao")) {
            const char* v = take();
            if (!v) return false;
            if (!std::strcmp(v, "gtao")) c.aoMode = 0;
            else if (!std::strcmp(v, "ssao")) c.aoMode = 1;
            else if (!std::strcmp(v, "off")) c.aoMode = 2;
            else return false;
        } else if (!std::strcmp(a, "--bloom")) {
            const char* v = take();
            if (!v) return false;
            c.bloomIntensity = float(std::atof(v));
        } else if (!std::strcmp(a, "--exposure")) {
            const char* v = take();
            if (!v) return false;
            c.exposureCompensation = float(std::atof(v));
        } else if (!std::strcmp(a, "--look")) {
            const char* v = take();
            if (!v) return false;
            if (!std::strcmp(v, "base")) {
                c.lookPower = c.lookSaturation = 1.0f;
            } else if (std::sscanf(v, "%f,%f", &c.lookPower, &c.lookSaturation) != 2 || c.lookPower <= 0.0f) {
                FX_ERROR("--look takes base or <power>,<saturation> (default 1.35,1.4)");
                return false;
            }
        } else if (!std::strcmp(a, "--manual-exposure")) {
            const char* v = take();
            if (!v) return false;
            c.manualExposure = float(std::atof(v));
        } else if (!std::strcmp(a, "--no-physics")) {
            c.physics = false;
        } else if (!std::strcmp(a, "--max-bodies")) {
            const char* v = take();
            if (!v) return false;
            c.maxBodies = uint32_t(std::max(2, std::atoi(v)));
        } else if (!std::strcmp(a, "--physics-drop")) {
            const char* v = take();
            if (!v) return false;
            c.physicsDrop = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--threads")) {
            const char* v = take();
            if (!v) return false;
            c.jobThreads = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--script")) {
            if (!(c.scriptPath = take())) return false;
        } else if (!std::strcmp(a, "--anim-set")) {
            const char* v = take();
            if (!v || c.animSetCount >= 8) return false;
            c.animSet[c.animSetCount++] = v;
        } else if (!std::strcmp(a, "--anim-events")) {
            if (!(c.animEvents = take())) return false;
        } else if (!std::strcmp(a, "--crowd")) {
            const char* v = take();
            if (!v) return false;
            c.crowd = uint32_t(std::max(0, std::atoi(v)));
        } else if (!std::strcmp(a, "--view")) {
            const char* v = take();
            if (!v) return false;
            c.cameraView = !std::strcmp(v, "outside") ? 1 : !std::strcmp(v, "inside") ? 0 : -1;
        } else if (!std::strcmp(a, "--no-hot-reload")) {
            c.hotReload = false;
        } else if (!std::strcmp(a, "--reload-scene-at")) {
            const char* v = take();
            if (!v) return false;
            c.reloadSceneAt = uint32_t(std::atoi(v));
            if (const char* comma = std::strchr(v, ','))
                c.reloadSceneCount = uint32_t(std::max(std::atoi(comma + 1), 1));
        } else if (!std::strcmp(a, "--script-reload-at")) {
            const char* v = take();
            if (!v) return false;
            c.scriptReloadAt = uint32_t(std::atoi(v));
        } else if (!std::strcmp(a, "--freeze-seconds")) {
            const char* v = take();
            if (!v) return false;
            c.freezeSeconds = std::max(float(std::atof(v)), 0.0f);
        } else if (!std::strcmp(a, "--freeze-reports")) {
            if (!(c.freezeReports = take())) return false;
        } else if (!std::strcmp(a, "--test-freeze")) {
            const char* v = take();
            if (!v) return false;
            c.testFreezeFrame = uint32_t(std::atoi(v));
            if (const char* colon = std::strchr(v, ':'))
                c.testFreezeMs = uint32_t(std::max(std::atoi(colon + 1), 0));
        } else if (!std::strcmp(a, "--editor")) {
            c.editor = true;
        } else if (!std::strcmp(a, "--ui")) {
            if (!(c.uiBundle = take())) return false;
        } else if (!std::strcmp(a, "--editor-script")) {
            if (!(c.editorScript = take())) return false;
        } else if (!std::strcmp(a, "--launcher")) {
            c.launcher = true;
        } else if (!std::strcmp(a, "--no-relaunch")) {
            c.relaunch = false;
        } else if (!std::strcmp(a, "--launcher-script")) {
            if (!(c.launcherScript = take())) return false;
            c.launcher = true;
        } else if (!std::strcmp(a, "--project")) {
            if (!(c.projectPath = take())) return false;
        } else if (!std::strcmp(a, "--preview")) {
            if (!(c.previewPath = take())) return false;
        } else if (!std::strcmp(a, "--library")) {
            if (!(c.libraryPath = take())) return false;
        } else if (!std::strcmp(a, "--no-prefs")) {
            c.editorPrefs = false;
        } else if (!std::strcmp(a, "--no-edits")) {
            c.sceneEdits = false;
        } else if (!std::strcmp(a, "--gpu-debug")) {
            c.gpu.debug = true;
        } else if (!std::strcmp(a, "--no-audio")) {
            c.audio = false;
        } else if (!std::strcmp(a, "--audio-null")) {
            c.audioNull = true;
        } else if (!std::strcmp(a, "--audio-capture")) {
            if (!(c.audioCapture = take())) return false;
        } else if (!std::strcmp(a, "--audio-tone")) {
            if (!(c.audioTone = take())) return false;
        } else if (!std::strcmp(a, "--audio-play")) {
            if (!(c.audioPlay = take())) return false;
        } else if (!std::strcmp(a, "--audio-no-air")) {
            c.audioAir = false;
        } else if (!std::strcmp(a, "--audio-bake")) {
            c.audioBake = true;
        } else if (!std::strcmp(a, "--audio-reflections")) {
            const char* v = take();
            if (!v) return false;
            c.audioReflections = std::clamp(std::atoi(v), 0, 3);
        } else if (!std::strcmp(a, "--audio-spatializer")) {
            const char* v = take();
            if (!v) return false;
            if (!std::strcmp(v, "auto")) c.audioSpatializer = 0;
            else if (!std::strcmp(v, "steam")) c.audioSpatializer = 1;
            else if (!std::strcmp(v, "pan")) c.audioSpatializer = 2;
            else { FX_ERROR("--audio-spatializer takes auto, steam or pan"); return false; }
        } else {
            FX_ERROR("unknown argument %s", a);
            return false;
        }
    }
    // A shipped game (File > Build Game: Game/ beside the executable) plays its project unless told
    // otherwise (--hidden, --frames and the like still apply).
    static const std::string shipped = gamescripts::shippedProject();
    if (!shipped.empty() && !c.projectPath && !c.editor && !c.previewPath && !c.launcherScript) {  // (--scene: one of its scenes)
        c.projectPath = shipped.c_str();
        c.launcher = false;
    }
    if (c.netBots > 0 && c.netConnect)  // (bots: headless like a server, connecting instead)
        c.server = true;
    if (c.server) {  // (a dedicated server: hosts, nothing shown or heard)
        c.hidden = true;
        c.audio = false;
        c.launcher = c.editor = false;
        if (c.netHost < 0 && !c.netConnect)
            c.netHost = 27015;
    }
    return true;
}

namespace {

// F1-F12 debug views, C freeze culling, O occlusion, B backface cones, [ ] LOD threshold,
// PageUp/PageDown HiZ level (in the HiZ view), T TAA, G cycle GTAO/SSAO/off, - = exposure.
// Physics keys (F fire, R drop a pile, P pause) are handled in the main loop.
void handleRenderKey(SDL_Keycode key, FrameSettings& s)
{
    if (key >= SDLK_F1 && key <= SDLK_F12)
        s.debugView = DebugView(uint32_t(key - SDLK_F1));
    else if (key == SDLK_C)
        s.freezeCulling = !s.freezeCulling;
    else if (key == SDLK_O)
        s.occlusion = !s.occlusion;
    else if (key == SDLK_B)
        s.coneCulling = !s.coneCulling;
    else if (key == SDLK_LEFTBRACKET)
        s.lodPixels = SDL_max(s.lodPixels * 0.5f, 0.0625f);
    else if (key == SDLK_RIGHTBRACKET)
        s.lodPixels = SDL_min(s.lodPixels * 2.0f, 256.0f);
    else if (key == SDLK_PAGEUP)
        s.hizViewLevel = SDL_min(s.hizViewLevel + 1, 15u);
    else if (key == SDLK_PAGEDOWN && s.hizViewLevel > 0)
        s.hizViewLevel -= 1;
    else if (key == SDLK_T)
        s.post.taa = !s.post.taa;
    else if (key == SDLK_G)
        s.post.ao = AoMode((uint32_t(s.post.ao) + 1) % 3);
    else if (key == SDLK_MINUS)
        s.post.exposureCompensation -= 0.5f;
    else if (key == SDLK_EQUALS)
        s.post.exposureCompensation += 0.5f;
}

// Profiler counters, sampled once per frame (core/profiler.h). GPU-side numbers come from the
// renderer's stats readback and are a few frames old.
void sampleCounters(const GpuDrivenRenderer& r, const VirtualTextureSystem& vt, const PhysicsWorld& physics, bool usePhysics,
                    const AnimationSystem& animation, const UiLayer& ui, const SkinningSystem& skinning,
                    const LightSystem& lights, const GpuDevice& gpu)
{
    using profiler::counter;
    using profiler::setCounter;
    static const struct Ids {
        uint32_t instances = counter("Instances drawn", "", "Rendering"), chunks = counter("Cluster chunks", "", "Rendering"),
                 candidates = counter("Occlusion candidates", "", "Rendering"), visible1 = counter("Clusters pass 1", "", "Rendering"),
                 visible2 = counter("Clusters pass 2", "", "Rendering"), masked = counter("Alpha-masked clusters", "", "Rendering"),
                 triangles1 = counter("Triangles pass 1", "", "Rendering"), triangles2 = counter("Triangles pass 2", "", "Rendering"),
                 triangles = counter("Triangles", "", "Rendering"), overflow = counter("Cluster overflow", "", "Rendering"),
                 gpuMarks = counter("GPU passes timed", "", "Rendering"), lights = counter("Lights", "", "Rendering"),
                 shadowed = counter("Shadowed lights", "", "Rendering"), skinned = counter("Skinned instances", "", "Rendering"),
                 present = counter("Present mode", "0 vsync 1 immediate 2 mailbox", "Rendering"),
                 vsmRequested = counter("Pages requested", "", "Shadows"), vsmNew = counter("Pages rendered", "", "Shadows"),
                 vsmFree = counter("Free pages", "", "Shadows"), vsmRecords = counter("Caster records", "", "Shadows"),
                 vsmOverflow = counter("Record overflow", "", "Shadows"),
                 vtResident = counter("Resident pages", "", "Virtual Texturing"), vtCapacity = counter("Cache pages", "", "Virtual Texturing"),
                 vtQueued = counter("Queued reads", "", "Virtual Texturing"), vtInFlight = counter("Reads in flight", "", "Virtual Texturing"),
                 vtUploaded = counter("Uploaded pages", "", "Virtual Texturing"), vtEvicted = counter("Evicted pages", "", "Virtual Texturing"),
                 vtRequests = counter("Feedback requests", "", "Virtual Texturing"),
                 bodies = counter("Bodies", "", "Physics"), awake = counter("Awake bodies", "", "Physics"),
                 statics = counter("Static shapes", "", "Physics"), step = counter("Step", "ms", "Physics"),
                 pairs = counter("Broadphase pairs", "ms", "Physics"), collide = counter("Collide", "ms", "Physics"),
                 solve = counter("Solve", "ms", "Physics"), sleep = counter("Sleep islands", "ms", "Physics"),
                 characters = counter("Characters", "", "Animation"), animMs = counter("Update", "ms", "Animation"),
                 uiFrames = counter("Page frames", "", "UI"), uiCopy = counter("Frame copy (average)", "ms", "UI"),
                 uiCopyMax = counter("Frame copy (max)", "ms", "UI"), uiIn = counter("Messages in", "", "UI"),
                 uiOut = counter("Messages out", "", "UI"),
                 workingSet = counter("Working set", "MB", "Memory"), privateBytes = counter("Private bytes", "MB", "Memory");
    } id;
    const GpuStats& g = r.stats();
    if (g.valid) {
        setCounter(id.instances, g.instances);
        setCounter(id.chunks, g.chunks);
        setCounter(id.candidates, g.candidates);
        setCounter(id.visible1, g.visible1);
        setCounter(id.visible2, g.visible2);
        setCounter(id.masked, g.masked1 + g.masked2);
        setCounter(id.triangles1, g.triangles1);
        setCounter(id.triangles2, g.triangles2);
        setCounter(id.triangles, double(g.triangles1) + double(g.triangles2));
        setCounter(id.overflow, g.overflow & 3);
        setCounter(id.vsmRequested, g.vsmRequested);
        setCounter(id.vsmNew, g.vsmDirty);
        setCounter(id.vsmFree, g.vsmFree);
        setCounter(id.vsmRecords, g.vsmRecords);
        setCounter(id.vsmOverflow, g.vsmOverflow);
    }
    setCounter(id.gpuMarks, gpuprof::stats().marks);
    setCounter(id.lights, lights.count());
    setCounter(id.shadowed, double(lights.shadowSlots().size()));
    setCounter(id.skinned, skinning.recordCount());
    setCounter(id.present, gpu.presentMode() == SDL_GPU_PRESENTMODE_VSYNC ? 0 : gpu.presentMode() == SDL_GPU_PRESENTMODE_IMMEDIATE ? 1 : 2);
    const VtStats& v = vt.stats();
    setCounter(id.vtResident, v.resident);
    setCounter(id.vtCapacity, v.capacity);
    setCounter(id.vtQueued, v.queued);
    setCounter(id.vtInFlight, v.inFlight);
    setCounter(id.vtUploaded, v.uploadedLastFrame);
    setCounter(id.vtEvicted, v.evictedLastFrame);
    setCounter(id.vtRequests, v.requestsLastFeedback);
    if (usePhysics) {
        const PhysicsStats p = physics.stats();
        const b3Profile bp = physics.profile();
        setCounter(id.bodies, p.bodies);
        setCounter(id.awake, p.awake);
        setCounter(id.statics, p.staticShapes);
        setCounter(id.step, bp.step);
        setCounter(id.pairs, bp.pairs);
        setCounter(id.collide, bp.collide);
        setCounter(id.solve, bp.solve);
        setCounter(id.sleep, bp.sleepIslands);
    }
    setCounter(id.characters, animation.characterCount());
    setCounter(id.animMs, animation.lastUpdateMs());
    const UiStats us = ui.stats();
    setCounter(id.uiFrames, double(us.frames));
    setCounter(id.uiCopy, us.copyMs);
    setCounter(id.uiCopyMax, us.copyMaxMs);
    setCounter(id.uiIn, double(us.messagesIn));
    setCounter(id.uiOut, double(us.messagesOut));
    uint64_t gpuTotal = 0;
    for (const auto& [module, bytes] : gpumem::modules(&gpuTotal))
        setCounter(counter(("GPU " + module).c_str(), "MB", "Memory"), double(bytes) / (1024.0 * 1024.0));
    setCounter(counter("GPU total", "MB", "Memory"), double(gpuTotal) / (1024.0 * 1024.0));
    if (const platform::ProcessMemory pm = platform::processMemory(); pm.residentBytes) {
        setCounter(id.workingSet, double(pm.residentBytes) / (1024.0 * 1024.0));
        setCounter(id.privateBytes, double(pm.privateBytes) / (1024.0 * 1024.0));
    }
}

// --profile: averages over the frames recorded since the last call (GPU sections, CPU categories).
// --moving-lights (a benchmark of moving lights' shadows; by the frame, so runs match): at time
// `t` the sun has turned 30 degrees a second about the vertical, and each point and spot light
// circles where it started, 1 m out, once a second (the debug and test lights too: setMotion).
void moveLights(Scene& scene, float t, std::unordered_map<flecs::entity_t, WorldTransform>& start)
{
    scene.world().each([&](flecs::entity e, const Light& l, WorldTransform& w) {
        const WorldTransform& from = start.try_emplace(e.id(), w).first->second;
        w = from;
        if (l.type == 2) {
            const float a = t * 0.5236f, c = std::cos(a), s = std::sin(a);
            for (int col = 0; col < 3; ++col) {
                const float x = from.rows[col], z = from.rows[8 + col];
                w.rows[col] = c * x + s * z;
                w.rows[8 + col] = -s * x + c * z;
            }
        } else {
            const float a = t * 6.2832f;
            w.rows[3] += std::cos(a);
            w.rows[11] += std::sin(a);
        }
        e.modified<WorldTransform>();
    });
}

void logProfile(uint64_t& lastFrame)
{
    struct Sum {
        double ms = 0.0;
        uint32_t n = 0;
    };
    std::vector<std::pair<uint32_t, Sum>> sections, passes;  // in first-seen order
    Sum gpu, cpu, worst;
    std::pair<uint32_t, double> worstSection = {};  // the slowest frame's largest section
    std::string worstPasses;
    std::array<double, size_t(profiler::Category::Count)> categories{};
    for (const profiler::FrameRecord& f : profiler::history()) {
        if (f.index <= lastFrame && lastFrame != ~0ull)
            continue;
        if (!f.gpuDone)
            break;
        lastFrame = f.index;
        cpu.ms += f.cpuMs();
        ++cpu.n;
        // A hitch (a frame over 25 ms on the CPU or the GPU): what took it, so a stutter doesn't
        // vanish into the averages (the main thread's zones of 2 ms or more, the GPU's passes of 1).
        if (f.cpuMs() > 25.0 || f.gpuMs > 25.0) {
            std::string what;
            char buf[160];
            for (const profiler::ThreadZones& t : f.threads)
                if (t.thread == 0)
                    for (const profiler::Zone& z : t.zones)
                        if (profiler::ticksToMs(z.end - z.start) >= 2.0) {
                            std::snprintf(buf, sizeof(buf), " | %s%s %.1f", std::string(z.depth, '.').c_str(),
                                          profiler::nameOf(z.name).c_str(), profiler::ticksToMs(z.end - z.start));
                            what += buf;
                        }
            for (const profiler::GpuSpan& s : f.gpu)
                if (s.endMs - s.startMs >= 1.0) {
                    std::snprintf(buf, sizeof(buf), " | gpu %s %.1f", profiler::nameOf(s.name).c_str(), s.endMs - s.startMs);
                    what += buf;
                }
            FX_INFO("profile hitch: frame %llu cpu %.1f ms gpu %.1f ms%s", (unsigned long long)f.index, f.cpuMs(), f.gpuMs,
                    what.c_str());
        }
        for (size_t c = 0; c < categories.size(); ++c)
            categories[c] += f.categoryMs[c];
        if (f.gpu.empty())
            continue;
        gpu.ms += f.gpuMs;
        ++gpu.n;
        if (f.gpuMs > worst.ms) {  // (and the section that took most of it, and its passes of 0.2 ms or more)
            worst.ms = f.gpuMs;
            worstSection = {};
            worstPasses.clear();
            char buf[128];
            for (const profiler::GpuSpan& s : f.gpu) {
                if (s.level == 0 && s.endMs - s.startMs > worstSection.second)
                    worstSection = {s.name, s.endMs - s.startMs};
                if (s.level == 1 && s.endMs - s.startMs >= 0.2) {
                    std::snprintf(buf, sizeof(buf), " | %s %.3f", profiler::nameOf(s.name).c_str(), s.endMs - s.startMs);
                    worstPasses += buf;
                }
            }
        }
        for (const profiler::GpuSpan& s : f.gpu) {
            if (s.level > 1)
                continue;
            auto& list = s.level == 0 ? sections : passes;
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == s.name; });
            if (it == list.end())
                it = list.insert(list.end(), {s.name, {}});
            it->second.ms += s.endMs - s.startMs;
        }
    }
    if (cpu.n) {
        std::string line;
        char buf[96];
        for (size_t c = 0; c < categories.size(); ++c) {
            std::snprintf(buf, sizeof(buf), " | %s %.3f", profiler::categoryName(profiler::Category(c)), categories[c] / cpu.n);
            line += buf;
        }
        FX_INFO("profile cpu: %.3f ms over %u frames%s", cpu.ms / cpu.n, cpu.n, line.c_str());
    }
    if (gpu.n) {
        std::string line;
        char buf[128];
        for (const auto& [name, sum] : sections) {
            std::snprintf(buf, sizeof(buf), " | %s %.3f", profiler::nameOf(name).c_str(), sum.ms / gpu.n);
            line += buf;
        }
        FX_INFO("profile gpu: %.3f ms%s", gpu.ms / gpu.n, line.c_str());
        FX_INFO("profile gpu worst frame: %.3f ms (%s %.3f)%s", worst.ms,
                worstSection.second > 0.0 ? profiler::nameOf(worstSection.first).c_str() : "-", worstSection.second,
                worstPasses.c_str());
        // The passes inside them that take a measurable share (0.05 ms or more).
        line.clear();
        for (const auto& [name, sum] : passes)
            if (sum.ms / gpu.n >= 0.05) {
                std::snprintf(buf, sizeof(buf), " | %s %.3f", profiler::nameOf(name).c_str(), sum.ms / gpu.n);
                line += buf;
            }
        if (!line.empty())
            FX_INFO("profile gpu passes:%s", line.c_str() + 2);
    }
}

void logStats(SDL_Window* window, const GpuDrivenRenderer& r, VirtualShadowMap& vsm, const VirtualTextureSystem& vt,
              const FrameSettings& s, double frameMs)
{
    const GpuStats& g = r.stats();
    if (!g.valid)
        return;
    char timing[96] = "";
    float meanMs = 0.0f, worstMs = 0.0f;
    if (vsm.takeTiming(meanMs, worstMs))
        std::snprintf(timing, sizeof(timing), ", GPU %.2f ms (worst %.2f, budget %.2g)", double(meanMs), double(worstMs),
                      double(vsm.budget()));
    FX_INFO("vsm: %u pages requested, %u new (%u records, %u of moving casters%s), %u free, levels 0x%x%s", g.vsmRequested,
            g.vsmDirty, g.vsmRecords, g.vsmMovingRecords, g.vsmOverflow ? ", OVERFLOW" : "", g.vsmFree, g.vsmLevels, timing);
    const VtStats& v = vt.stats();
    FX_INFO("vt: %u/%u pages resident (by mip %u %u %u %u %u %u), %u queued, %u reading, %llu uploaded total, "
            "%u requested last feedback, mip bias %.2f",
            v.resident, v.capacity, v.residentPerMip[0], v.residentPerMip[1], v.residentPerMip[2], v.residentPerMip[3],
            v.residentPerMip[4], v.residentPerMip[5], v.queued, v.inFlight, (unsigned long long)v.uploadedTotal,
            v.requestsLastFeedback, double(vt.mipBias()));
    FX_INFO("gpu: %u instances, %u chunks, %u+%u clusters (%u+%u masked, %u re-tested), %.2fM triangles, lod %.3g px%s%s%s",
            g.instances, g.chunks, g.visible1, g.visible2, g.masked1, g.masked2, g.candidates,
            double(g.triangles1 + g.triangles2) * 1e-6,
            double(s.lodPixels), s.occlusion ? "" : ", occlusion off", s.freezeCulling ? ", FROZEN" : "",
            (g.overflow & 3) ? ", OVERFLOW" : "");
    if (r.deformedInstances())
        FX_INFO("vertex functions: %u instances deformed, %u vertices", r.deformedInstances(), r.deformedVertices());
    if (g.transparentCandidates)
        FX_INFO("transparent: %u clusters in view, %u drawn (sorted)%s", g.transparentCandidates,
                std::min(g.transparent, uint32_t(TransparentRenderer::kMaxClusters)), (g.overflow & 4) ? ", OVERFLOW" : "");
    if (const TerrainRenderStats& t = r.terrainStats(); t.capacity && t.patches)
        FX_INFO("terrain: %u/%u patches (%u written last change, picked in %.2f ms), error %.3g px", t.patches,
                t.capacity, t.rewritten, double(t.selectMs), double(t.pixels));
    if (const ScatterStats& sc = r.scatter().stats(); sc.cells)
        FX_INFO("scatter: %u cells, %u slots, %u generated or cleared last update%s", sc.cells, sc.slots, sc.jobs,
                sc.starved ? " (OUT OF SLOTS: raise an asset's budget)" : "");
    char title[256];
    SDL_snprintf(title, sizeof(title), "Falcon | %.2f ms | %u clusters | %.2fM tris | lod %.3g px%s", frameMs,
                 g.visible1 + g.visible2 + g.masked1 + g.masked2, double(g.triangles1 + g.triangles2) * 1e-6, double(s.lodPixels),
                 s.freezeCulling ? " | FROZEN" : "");
    SDL_SetWindowTitle(window, title);
}

}  // namespace

namespace {

// Scene.LoadAsync: the next scene cooked on a thread when it needs a cook, then read (async I/O)
// while the scene before runs; once read, it takes that one's place (runSession uses its pak).
struct SceneLoader {
    std::string path, relative;  // absolute, and in the project
    std::future<bool> cook;
    std::mutex lock;
    CookProgress cookProgress;  // (the cook's thread writes it)
    std::atomic<bool> cancel = false;
    std::string cookError;
    std::unique_ptr<ScenePak> pak;  // reading
};

// What outlives a scene: the window, the GPU, the HTML UI, the scripts' runtime, the audio device,
// the frame's settings and the frame loop's counters. runScene builds a scene's systems around it,
// runs its frames and tears them down again (another scene may follow: Process::nextScene).
struct Process {
    GpuDevice gpu;
    ScriptHost scripts;
    bool useScripts = false;
    SDL_Window* window = nullptr;
    EditorPrefs prefs;
    int normalRect[4] = {};  // last restored (not maximized) placement, saved at exit
    bool editorChrome = false;
    UiLayer ui;
    std::string uiBundle;
    EditorBridge editor;  // (the page's; scenes attached to it one after another)
    bool editorStarted = false;
    bool playOnLoad = false;    // the next scene plays at once (a game's Scene.Load in the editor)
    std::string editsOverride;  // the next scene's edits instead of its file's (coming back to it)
    FrameSettings settings;
    AudioSystem audio;  // (the device; scenes attach to it)
    MeshRenderer meshes;  // runtime meshes (R5; scripts' Mesh), kept across scenes
    int exitCode = 0;
    std::filesystem::path privateProfile;
    // The frame loop's.
    SDL_GPUTexture* hiddenTarget = nullptr;  // --hidden: the frame's target instead of a swapchain image
    uint32_t hiddenW = 0, hiddenH = 0;
    uint64_t lastTicks = 0, frame = 0, startTicks = 0, statsTicks = 0;
    AudioMixerStats audioLogged;  // at the last stats line
    uint64_t physicsNs = 0, scriptNs = 0;  // CPU time in physics / scripts this stats window
    std::vector<std::string> droppedFiles;  // a drag from Explorer, until it completes
    uint32_t statsFrames = 0;
    uint64_t worstFrameNs = 0, lastFrameTicks = 0;  // the slowest frame of the stats window
    // Editor profiler: its own, shorter window.
    uint64_t editorTicks = 0, editorScriptNs = 0, editorPhysicsNs = 0, editorAnimNs = 0, editorUiNs = 0;
    uint32_t editorFrames = 0;
    bool uiShown = false;
    uint64_t profileLogged = ~0ull;  // --profile: last frame logged
    UiRect viewport = {};
    // The scene to run next (empty: the process ends), and how many times --reload-scene-at has.
    std::string nextScene;
    uint32_t reloads = 0;
    std::unique_ptr<SceneLoader> loader;  // Scene.LoadAsync's, while it cooks and reads
    uint64_t switchNs = 0;                // when the scene before began its teardown (the switch's time)
    std::unique_ptr<ScenePak> preloaded;  // the next scene, read already (the loader's)
    std::string preloadedPath;            // (its path: the pak borrows it)
    SimRecorder sim;                      // --sim-record / --sim-replay / --sim-input-demo
    std::vector<void*> netClients;        // --net-clients' processes (and the editor's Play's)
    // The game's network as the command line has it (setNetwork's): the editor's Play with clients
    // replaces it for that play, and puts it back after.
    net::Conditions netConditions;
    std::string netLaunch;
    std::function<void(int32_t, const std::string&)> netLauncher;
};

// The editor's Play with client processes (the Play bar's Clients): the editor's game hosts (the
// project's network settings), N processes of this game on the scene as saved connect to it,
// windowed (hidden when the editor is), under the chosen conditions; Stop ends them.
void editorNetworkPlay(Process& p, const EngineConfig& config, const EditorBridge& editor)
{
    const uint32_t clients = editor.playClients();
    if (!clients || !config.projectPath) {
        p.scripts.setNetwork(p.netConditions, p.netLaunch, p.netLauncher);
        return;
    }
    net::Conditions conditions;
    const std::string sim = editor.playConditions();
    if (!sim.empty())
        net::parseConditions(sim.c_str(), conditions);
    JsonWriter w;
    // (on 127.0.0.1 only: its clients are this machine's processes, and no firewall prompt)
    w.beginObject().value("host", int64_t(0)).value("hostAddress", std::string_view("127.0.0.1")).value("clients", int64_t(clients));
    const std::string project = networkJson(editor.networkSettings());
    w.value("mode", std::string_view(editor.networkSettings().mode)).raw("project", project).endObject();
    const std::string projectDir = config.projectPath, scene = config.scenePath ? config.scenePath : "";
    const bool hidden = config.hidden;
    p.scripts.setNetwork(conditions, w.str(), [&p, projectDir, scene, sim, hidden](int32_t count, const std::string& address) {
        std::vector<std::string> args = {"--project", projectDir, "--scene", scene, "--connect", address};
        if (!sim.empty())
            args.insert(args.end(), {"--net-sim", sim});
        if (hidden)
            args.push_back("--hidden");
        for (int32_t i = 0; i < count; ++i)
            if (void* child = startFalconTracked(args))
                p.netClients.push_back(child);
            else
                FX_ERROR("net: client process %d did not start", int(i));
        FX_INFO("editor: playing with %d client processes", int(count));
    });
}

// The editor's Stop: its client processes end with its game.
void endEditorClients(Process& p)
{
    for (void* child : p.netClients) {
        int code = 0;
        if (!falconFinished(child, code))
            stopFalcon(child);
    }
    p.netClients.clear();
}

// Scene.LoadAsync: a load started (a project's scene; it replaces one in progress).
void startSceneLoad(Process& p, const char* projectDir, const std::string& relative)
{
    if (p.loader) {
        p.loader->cancel = true;
        if (p.loader->cook.valid())
            p.loader->cook.wait();
    }
    auto l = std::make_unique<SceneLoader>();
    l->relative = relative;
    l->path = (std::filesystem::path(projectDir) / relative).lexically_normal().generic_string();
    Project project;
    std::string error;
    if (loadProject(projectDir, project, error) && sceneNeedsCook(project, relative)) {
        SceneLoader* raw = l.get();
        l->cook = std::async(std::launch::async, [raw, project] {
            return cookProjectScene(
                project, raw->relative, raw->cookError,
                [raw](const CookProgress& c) {
                    std::lock_guard g(raw->lock);
                    raw->cookProgress = c;
                    return !raw->cancel.load();
                },
                true);
        });
        FX_INFO("scene: cooking %s, then reading it (Scene.LoadAsync)", relative.c_str());
    } else {
        l->pak = std::make_unique<ScenePak>();
        if (!l->pak->beginRead(l->path.c_str())) {
            p.scripts.setSceneLoading(-1.0f, "");
            return;
        }
        FX_INFO("scene: reading %s (Scene.LoadAsync)", relative.c_str());
    }
    p.loader = std::move(l);
}

// Each frame while one loads: its progress for the scripts, and true once the scene is read (it
// takes this scene's place after this frame).
bool pumpSceneLoad(Process& p)
{
    SceneLoader& l = *p.loader;
    if (l.cook.valid()) {
        if (l.cook.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            std::lock_guard g(l.lock);
            const CookProgress& c = l.cookProgress;
            p.scripts.setSceneLoading(c.total ? 0.9f * float(c.done) / float(c.total) : 0.0f,
                                      "cooking" + (c.stage.empty() ? std::string() : ": " + c.stage));
            return false;
        }
        if (!l.cook.get()) {
            FX_ERROR("scene: %s did not cook: %s", l.relative.c_str(), l.cookError.c_str());
            p.scripts.setSceneLoading(-1.0f, "");
            p.loader.reset();
            return false;
        }
        l.pak = std::make_unique<ScenePak>();
        if (!l.pak->beginRead(l.path.c_str())) {
            p.scripts.setSceneLoading(-1.0f, "");
            p.loader.reset();
            return false;
        }
    }
    if (!l.pak->readDone()) {
        p.scripts.setSceneLoading(0.9f, "reading");
        return false;
    }
    p.scripts.setSceneLoading(1.0f, "ready");
    p.nextScene = p.preloadedPath = l.path;
    p.preloaded = std::move(l.pak);
    p.preloaded->setPath(p.preloadedPath.c_str());
    p.loader.reset();
    return true;
}

// Memory between scenes (nothing of a scene left): what the GPU accounting holds, and the
// process's private bytes.
struct SceneMemory {
    uint64_t gpuBytes = 0;
    uint32_t gpuResources = 0;
    uint64_t ramBytes = 0;
};

SceneMemory sceneMemory()
{
    SceneMemory m;
    m.gpuResources = uint32_t(gpumem::resources().size());
    gpumem::modules(&m.gpuBytes);
    m.ramBytes = platform::processMemory().privateBytes;
    return m;
}

int runScene(Process& p, const EngineConfig& config, ScenePak& pak, const bool hasScene, const bool firstScene)
{
    // The process's, by the names this code has always used.
    GpuDevice& gpu = p.gpu;
    ScriptHost& scripts = p.scripts;
    SDL_Window* const window = p.window;
    EditorPrefs& prefs = p.prefs;
    int (&normalRect)[4] = p.normalRect;
    const bool editorChrome = p.editorChrome;
    UiLayer& ui = p.ui;
    const std::string& uiBundle = p.uiBundle;
    FrameSettings& settings = p.settings;
    AudioSystem& audio = p.audio;
    int& exitCode = p.exitCode;
    bool& useScripts = p.useScripts;
    SDL_GPUTexture*& hiddenTarget = p.hiddenTarget;
    uint32_t& hiddenW = p.hiddenW;
    uint32_t& hiddenH = p.hiddenH;
    uint64_t& lastTicks = p.lastTicks;
    uint64_t& frame = p.frame;
    const uint64_t startTicks = p.startTicks;
    uint64_t& statsTicks = p.statsTicks;
    AudioMixerStats& audioLogged = p.audioLogged;
    uint64_t& physicsNs = p.physicsNs;
    uint64_t& scriptNs = p.scriptNs;
    std::vector<std::string>& droppedFiles = p.droppedFiles;
    uint32_t& statsFrames = p.statsFrames;
    uint64_t& worstFrameNs = p.worstFrameNs;
    uint64_t& lastFrameTicks = p.lastFrameTicks;
    uint64_t& editorTicks = p.editorTicks;
    uint64_t& editorScriptNs = p.editorScriptNs;
    uint64_t& editorPhysicsNs = p.editorPhysicsNs;
    uint64_t& editorAnimNs = p.editorAnimNs;
    uint64_t& editorUiNs = p.editorUiNs;
    uint32_t& editorFrames = p.editorFrames;
    bool& uiShown = p.uiShown;
    uint64_t& profileLogged = p.profileLogged;
    UiRect& viewport = p.viewport;


    Scene scene;
    FlyCamera camera;
    PhysicsWorld physics;
    anim::AnimData animData;
    SkinningSystem skinning;
    AnimationSystem animation;
    GraphLibrary graphs;  // a project's .fgraph files (Animators)
    SceneEdits edits;
    NodeAnimator nodeAnimator;  // models' rigid animation (scene/node_animator.h)
    BoneEntities bones;         // characters' skeletons expanded into entities (anim/bone_entities.h)
    ParticleSystem particles;   // particle effects (vfx/particle_system.h)
    PrefabSpawner prefabSpawner;  // prefabs made while a game runs (Prefab.Spawn, collision prefabs)
    DecalSystem decals;           // Decal components and decals placed by scripts (vfx/decal_system.h)
    RuntimeMeshes runtimeMeshes;  // slots for particles' models and spawned prefabs' meshes (vfx/runtime_meshes.h)
    LevelMeshes levelMeshes;      // level meshes drawn as live meshes (level/level_meshes.h)
    std::vector<flecs::entity_t> particleDisabled;  // the editor's Play: what finished effects left, enabled again at Stop
    bool nodeAnimPlaying = false;
    Terrain terrain;
    WaterSystem water;
    std::vector<PrimitivePool> primitivePools;
    std::vector<ScatterPool> scatterPools;
    uint32_t scatterSpareFirst = 0, scatterSpareCount = 0;
    std::vector<ScatterCellBodies> scatterAdded;
    std::vector<uint64_t> scatterRemoved;
    const bool usePhysics = hasScene && config.physics && !config.naiveRenderer;
    // --preview: asset previews, one after another (app/preview.h).
    PreviewSession preview;
    const bool usePreview = hasScene && config.previewPath && !config.naiveRenderer;
    if (usePreview && !preview.load(config.previewPath))
        return 1;
    MaterialLibrary materials;
    if (hasScene) {
        scene.init();
        if (!pak.finishRead())
            return 1;
        scene.spawnFromPak(pak);
        // Point / spot lights are entities too; the debug camera light takes a shadow first.
        scene.spawnLights(pak, config.shadowedLights - std::min(config.shadowedLights, config.cameraLight > 0.0f ? 1u : 0u));
        // Editor edits (moved, hidden, renamed, deleted, created entities) before physics and the
        // upload see the instance table; created entities are built-in cubes in a slot pool.
        if (config.sceneEdits && !config.naiveRenderer) {
            if (!p.editsOverride.empty())  // (the editor coming back to a scene as it had it)
                edits.loadText(std::exchange(p.editsOverride, {}), SceneEdits::pathFor(config.scenePath));
            else
                edits.load(SceneEdits::pathFor(config.scenePath));
            edits.expandPrefabs(config.projectPath ? config.projectPath : "");  // prefab instances: their entities
        }
        // Material assets: the cooked ones' factors from their files; spare slots for the others
        // (the editor's new ones, those the edits use that are not cooked yet, a preview run's).
        uint32_t uncooked = 0;
        {
            std::vector<std::string> seen;
            for (const auto* list : {&edits.instances, &edits.created})
                for (const EntityEdit& e : *list)
                    if (!e.material.empty() && pak.findMaterial(e.material) < 0 &&
                        std::find(seen.begin(), seen.end(), e.material) == seen.end())
                        seen.push_back(e.material);
            uncooked = uint32_t(seen.size());
        }
        // (and 256 for scripts' per-entity copies: Renderer.MaterialInstance)
        materials.init(pak, config.projectPath ? config.projectPath : usePreview ? preview.root() : std::string(),
                       (config.editor ? 32u : 0u) + uncooked + (usePreview ? preview.spareMaterials() : 0u),
                       config.naiveRenderer ? 0u : 256u);
        if (!config.naiveRenderer) {
            primitivePools = edits.reservePools(pak, config.editor ? 64u : 0u);
            runtimeMeshes.reserve(pak, RuntimeMeshes::needs(edits, config.projectPath ? config.projectPath : "", config.editor));
            water.setProject(config.projectPath ? config.projectPath : "");  // (.fwater paths of the edits' bodies)
            edits.apply(pak, scene, primitivePools, &materials, &water);
            if (!config.editor) {  // a game: models' animation from the start (the editor's waits for Play)
                std::vector<NodeAnimator::Link> links;
                for (const EntityEdit& e : edits.created) {
                    const int32_t library = e.model.empty() ? -1 : pak.libraryIndex(e.model);
                    const bool camera = e.modelCamera >= 0;
                    if (library >= 0 && e.entity && (camera || e.modelNode >= 0))
                        if (const ScenePak::NodeTrack* t = pak.nodeTrack(uint32_t(library), uint32_t(camera ? e.modelCamera : e.modelNode), camera))
                            links.push_back({e.entity, t});
                }
                for (const ScenePak::NodeTrack& t : pak.nodeTracks())
                    if (t.library == fpak::kNodeAnimScene && !t.camera)
                        if (const flecs::entity ent = scene.entity(t.target))
                            links.push_back({ent.id(), &t});
                nodeAnimator.set(std::move(links), pak.nodeFps());
            }
        }
        // Terrain: a new one (--terrain N) or the scene's saved one, and its patch slots (always
        // in the editor, which can create one).
        if (!config.naiveRenderer) {
            if (config.terrainTiles) {
                TerrainSettings ts;
                ts.tiles = config.terrainTiles;
                ts.spacing = config.terrainSpacing;
                const float size = float(ts.tiles * terrain::kTileQuads) * ts.spacing;
                ts.originX = ts.originZ = -0.5f * size;
                terrain.create(ts, 0.0f, config.terrainHills, config.terrainSeed);
                if (config.terrainLayers) {  // generated looks, in order (layer 0 everywhere)
                    std::vector<TerrainLayer> layers;
                    for (const char* p = config.terrainLayers; *p;) {
                        const char* end = std::strchr(p, ',');
                        TerrainLayer l;
                        l.generated.assign(p, end ? end : p + std::strlen(p));
                        l.name = l.generated;
                        layers.push_back(l);
                        p = end ? end + 1 : p + std::strlen(p);
                    }
                    terrain.setLayersJson(terrainLayersJson(layers));
                }
                if (config.terrainScatter && config.terrainScatter[0] == '[') {  // a whole scatter list (JSON)
                    terrain.setScatterJson(scatterLayersJson(parseScatterLayers(config.terrainScatter)));
                } else if (config.terrainScatter) {  // a preset's name, or mesh[:density[:budget]], per layer
                    std::vector<ScatterLayer> layers;
                    for (const char* p = config.terrainScatter; *p;) {
                        const char* end = std::strchr(p, ',');
                        const std::string entry(p, end ? end : p + std::strlen(p));
                        ScatterLayer l;
                        l.seed = uint32_t(layers.size() + 1);
                        if (const char* preset = scatterPreset(entry)) {
                            readScatterLayer(Json::parse(preset), l);
                            layers.push_back(l);
                            p = end ? end + 1 : p + std::strlen(p);
                            continue;
                        }
                        ScatterAsset asset;
                        const size_t c1 = entry.find(':'), c2 = c1 == std::string::npos ? c1 : entry.find(':', c1 + 1);
                        asset.mesh = entry.substr(0, c1);
                        if (c1 != std::string::npos)
                            l.density = std::max(float(std::atof(entry.c_str() + c1 + 1)), 1e-4f);
                        if (c2 != std::string::npos)
                            asset.budget = uint32_t(std::max(std::atoi(entry.c_str() + c2 + 1), 256));
                        l.name = asset.mesh;
                        l.assets.push_back(asset);
                        layers.push_back(l);
                        p = end ? end + 1 : p + std::strlen(p);
                    }
                    terrain.setScatterJson(scatterLayersJson(layers));
                }
                addScatterLooks(terrain);
            } else if (config.terrain) {
                terrain.load(Terrain::dirFor(config.scenePath));
            }
            if (terrain.valid() || config.editor)
                pak.reserveTerrain(config.terrainPatches);
            // Live meshes (after the terrain's slots): the scene's level meshes, and room for what is
            // made while it runs (level meshes in the editor, scripts' meshes).
            uint32_t levelLive = 0, levelBlocks = 0;
            LevelMeshes::needs(edits, levelLive, levelBlocks);
            pak.reserveLive((config.liveMeshes ? config.liveMeshes : config.editor ? 4096u : 64u) + levelLive,
                            (config.liveClusters ? config.liveClusters : config.editor ? 16384u : 512u) + levelBlocks);
            // Scatter: a slot pool per asset of each layer.
            if (terrain.valid())
                scatterPools = reserveScatterPools(parseScatterLayers(terrain.scatterJson()), pak);
            // The editor's spare slots: layers and meshes added while editing are given slots
            // from them (the pools above and these, again in layer order, on every change).
            if (config.editor && pak.cookedMeshCount() > 0) {
                scatterSpareCount = 131072;
                // (a built-in mesh until a layer takes them: mesh 0 may be skinned, and every
                // skinned slot would get a skinning record and a character)
                scatterSpareFirst = pak.addInstancePool(pak.header().builtinMeshes[uint32_t(fpak::Builtin::Cube)], scatterSpareCount);
            }
        }
        if (usePreview)
            preview.reserve(pak, materials);  // a slot per mesh to preview
        const bool outside = config.cameraView >= 0 ? config.cameraView == 1 : pak.cookedInstanceCount() < 100;
        const float* sceneLo = pak.header().boundsMin;
        const float* sceneHi = pak.header().boundsMax;
        if (terrain.valid() && std::max(sceneHi[0] - sceneLo[0], sceneHi[2] - sceneLo[2]) * 4.0f < terrain.worldSize()) {
            // The terrain is the world (the scene is small next to it): start over its middle, a
            // little above the ground.
            const float c[2] = {terrain.settings().originX + terrain.worldSize() * 0.5f,
                                terrain.settings().originZ + terrain.worldSize() * 0.5f};
            const float h = terrain.heightAt(c[0], c[1]);
            const float lo[3] = {c[0] - 8.0f, h, c[1] - 8.0f}, hi[3] = {c[0] + 8.0f, h + 4.0f, c[1] + 8.0f};
            camera.frame(lo, hi, true);
        } else if (pak.cookedInstanceCount() == 0) {
            // Nothing placed (a new scene): standing a little above the origin, looking at it.
            const float lo[3] = {-4.0f, 0.0f, -4.0f}, hi[3] = {4.0f, 2.0f, 4.0f};
            camera.frame(lo, hi, true);
        } else {
            camera.frame(pak.header().boundsMin, pak.header().boundsMax, outside);
        }
        boot::mark("ecs scene spawned");
        jobs::init(config.jobThreads);  // physics and animation
        if (usePhysics) {
            // Before the scene upload: physics reserves the dynamic instance pools.
            PhysicsConfig pc;
            pc.maxBodies = config.maxBodies;
            if (!physics.init(pak, scene, pc))
                return 1;
            physics.setWater(&water);
            // The simulation's tick: the water at the tick's time, OnFixedUpdate; after the step its
            // collision callbacks and (recording, replaying) the tick's hashes.
            physics.setStepCallback([&scripts, &physics, &water](float step) {
                water.setSimTime(physics.simTime());
                scripts.fixedUpdate(step);
            });
            physics.setAfterStepCallback([&p, &scripts, &physics, &scene] {
                scripts.afterStep();
                if (p.sim.active() && !physics.resimulating())  // (a rollback's steps again: not new ticks)
                    p.sim.tick({physics.stepIndex(), physics.stateHash(), SimRecorder::hashEntities(scene)});
            });
            // Rollback of the whole world (--sim-rollback): physics kept after each step, put back
            // with the scripts' state and stepped again, each step's tick simulated again inside it.
            // (A step runs one tick: the last tick is the last step's.)
            auto physicsRollback = [&scripts, &physics](uint32_t window) {
                physics.setRollbackWindow(window);
                if (!window) {
                    scripts.setPhysicsRollback(nullptr);
                    return;
                }
                scripts.setPhysicsRollback([&scripts, &physics](uint64_t from, uint64_t last) {
                    if (!physics.rollbackTo(physics.stepIndex() - (last - from)))
                        return false;
                    for (uint64_t t = from + 1; t <= last; ++t) {
                        scripts.resimulating(t);
                        physics.stepOnce();
                    }
                    scripts.resimulating(~0ull);
                    physics.rollbackDone();
                    return true;
                });
            };
            if (config.simRollback)
                physicsRollback(config.simRollback + 2);
            scripts.setRollbackHooks(physicsRollback, [&physics](uint64_t ticksBack) {
                return physics.stepIndex() >= ticksBack ? physics.stepHash(physics.stepIndex() - ticksBack) : 0;
            });
            boot::mark("physics world built");
        }
        // Skeletons and clips (used in place); skinned instances get their GPU records.
        const fpak::SectionEntry& animSection = pak.header().sections[uint32_t(fpak::Section::Animation)];
        if (!animData.init(pak.view<uint8_t>(fpak::Section::Animation), size_t(animSection.size))) {
            FX_ERROR("scene: corrupt animation section (re-cook it)");
            return 1;
        }
        // Crowd: copies of every cooked skinned instance in a square grid, each its own character.
        if (config.crowd > 0) {
            uint32_t meshCount = 0;
            const auto* meshes = pak.view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
            const auto* skins = pak.view<fpak::MeshSkinGpu>(fpak::Section::MeshSkins);
            const uint32_t cooked = pak.cookedInstanceCount();
            const uint32_t side = uint32_t(std::ceil(std::sqrt(float(config.crowd + 1))));
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t i = 0; i < cooked; ++i) {
                const fpak::InstanceGpu src = pak.instances()[i];
                if (src.mesh >= meshCount || skins[src.mesh].firstSkinVertex == fpak::kNotSkinned)
                    continue;
                const fpak::MeshGpu& m = meshes[src.mesh];
                const float spacing = std::max(m.posScale[0], m.posScale[2]) * 65535.0f * 1.3f;
                const uint32_t first = pak.addInstancePool(src.mesh, config.crowd);
                for (uint32_t k = 0; k < config.crowd; ++k) {
                    fpak::InstanceGpu& row = pak.mutableInstances()[first + k];
                    row = src;
                    const uint32_t cell = k + 1;  // cell 0 is the original
                    row.world[0][3] += (float(cell % side) - 0.5f * float(side - 1)) * spacing;
                    row.world[2][3] -= float(cell / side) * spacing;
                    scene.spawnInstance(first + k, row);
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = std::min(lo[a], row.world[a][3] - spacing);
                        hi[a] = std::max(hi[a], row.world[a][3] + spacing);
                    }
                }
            }
            if (lo[0] < hi[0]) {
                lo[1] = pak.header().boundsMin[1];
                hi[1] = pak.header().boundsMax[1];
                camera.frame(lo, hi, true);
            }
        }
        skinning.reserve(pak, [&](uint32_t skeleton) {
            return skeleton < animData.skeletonCount() ? animData.skeleton(skeleton).boneCount : 1u;
        });
        if (!animation.init(animData, pak, skinning))
            return 1;
        // IK's world: the ground (physics rays) and where each character is.
        animation.setWorld(
            [&](float3 from, float3 translation, float3& point, float3& normal) {
                ContactSide side;
                float fraction = 0.0f;
                return usePhysics && physics.castRay(from, translation, side, point, normal, fraction);
            },
            [&](uint32_t record, float world[3][4]) {
                const flecs::entity e = scene.entity(skinning.instanceOfRecord(record));
                const WorldTransform* t = e ? e.try_get<WorldTransform>() : nullptr;
                if (t)
                    std::memcpy(world, t->rows, sizeof(t->rows));
                return t != nullptr;
            });
        animation.setEntityWorld([&](uint64_t id, float world[3][4]) {  // IK targets: where an entity is
            if (!id || !scene.world().is_alive(flecs::entity_t(id)))
                return false;
            const flecs::entity e(scene.world(), flecs::entity_t(id));
            const WorldTransform* t = e.enabled() ? e.try_get<WorldTransform>() : nullptr;
            if (t)
                std::memcpy(world, t->rows, sizeof(t->rows));
            return t != nullptr;
        });
        if (config.editor) {  // the Animator's preview: skinning records of no instance
            uint32_t maxBones = 0;
            for (uint32_t k = 0; k < animData.skeletonCount(); ++k)
                maxBones = std::max(maxBones, animData.skeleton(k).boneCount);
            skinning.reserveFloating(AnimPreview::kMaxParts, maxBones);
        }
        {
            // (V1) Deform records for the vertex functions' instances: a larger budget when the
            // project has vertex functions now (more need a scene loaded again).
            bool vertex = false;
            if (config.projectPath)
                for (const std::string& s : ProjectShaders::surfaceShaders(config.projectPath))
                    vertex |= ProjectShaders::hasVertexFunction(config.projectPath, s);
            skinning.reserveDeform(vertex ? 512 : 64, vertex ? 262144 : 16384, vertex ? 16384 : 1024);
        }
        // Clip markup (events, sync events): a project's models' .fevents, or one file for all.
        animation.loadProjectMarkup(config.projectPath ? config.projectPath : "");
        if (config.animEvents) {
            std::ifstream in(config.animEvents, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            anim::Markup markup;
            std::string error;
            if (!in || !anim::parseMarkup(text.str(), markup, error)) {
                FX_ERROR("--anim-events %s: %s", config.animEvents, in ? error.c_str() : "cannot read it");
                return 1;
            }
            FX_INFO("animation: markup for %u clips from %s", animation.applyMarkup(markup, ~0u), config.animEvents);
        }
        // Animators: each one's graph drives the skinned meshes of its entity and its children.
        graphs.init(animData, config.projectPath ? config.projectPath : "");
        for (const auto* list : {&edits.instances, &edits.created})
            for (const EntityEdit& e : *list)
                if (e.hasAnimator && e.entity && !e.deleted) {
                    const std::vector<uint32_t> records = animation.recordsUnder(scene.world(), e.entity);
                    animation.bind(records, graphs.get(e.animator.graph), e.animator.params);
                    if (!records.empty()) {
                        // Its entity parameters' bindings (by key) and its own IK (an older scene's).
                        std::vector<std::pair<std::string, uint64_t>> bound;
                        for (const auto& [name, key] : e.animator.entities)
                            bound.push_back({name, uint64_t(edits.entityOfKey(pak, scene, key))});
                        animation.bindEntities(records[0], bound);
                        animation.setOwnIk(records[0], ownIkOf(e.animator));
                        animation.setEditorPreview(records[0], config.editor && e.animator.editorPreview);
                    }
                }
        // Expanded skeletons (posed as the Animators start them), and what hangs under their bones.
        bool anyBones = false;
        for (const auto* list : {&edits.instances, &edits.created})
            for (const EntityEdit& e : *list)
                anyBones |= e.bones && e.entity && !e.deleted;
        if (anyBones) {
            animation.update(0.0f);
            FX_INFO("animation: %u skeleton(s) expanded into entities", edits.expandBones(pak, scene, bones, animation, skinning));
        }
        if (config.crowd > 0)  // varied, deterministic gaits: some idle, most walking or running
            for (uint32_t r = 0; r < skinning.recordCount(); ++r)
                if (anim::GraphInstance* g = animation.graph(r)) {
                    uint32_t h = r * 2654435761u;
                    h ^= h >> 15;
                    g->setFloat(g->parameterIndex("Speed"), float(h % 1000) / 1000.0f * 5.0f);
                }
    }

    const bool server = config.server;
    if (firstScene && !server) {
        // The page loads only while Chromium is pumped (on this thread).
        while (ui.active() && !gpu.created()) {
            ui.pump();
            SDL_Delay(1);
        }
        if (!gpu.attachWindow(window))
            return 1;
        boot::mark("gpu attached");
        if (ui.active())
            ui.attach(gpu.get());  // (its first frame, when it came already, uploaded now)
        if (gpuprof::init(gpu.get()))  // (sections are timed always: the shadow map's budget)
            gpuprof::setActive(profiler::enabled() && config.profile);
    }
    BringupRenderer bringup;
    SceneRenderer naiveRenderer;
    GpuDrivenRenderer renderer;
    VirtualTextureSystem vt;
    AnimPreview animPreview;  // the editor's Animator tab
    std::vector<InstanceUpdate> heldRows;  // rows changed while the scene view was hidden
    LightSystem lights;
    VirtualShadowMap vsm;
    const bool naive = hasScene && config.naiveRenderer;
    if (hasScene) {
        if (!server && !pak.upload(gpu.get()))  // (a server keeps the pak on the CPU)
            return 1;
        if (naive) {
            if (!naiveRenderer.init(gpu, pak))
                return 1;
        } else {
          if (!server) {  // (the renderer's part; a server has none)
            std::string vtPath = config.scenePath;
            const size_t dot = vtPath.find_last_of('.');
            vtPath = (dot == std::string::npos ? vtPath : vtPath.substr(0, dot)) + ".fvt";
            // Debug light: a bright point light in front of the start camera, slightly below the eye.
            std::vector<fpak::LightGpu> extra;
            if (config.cameraLight > 0.0f) {
                const CameraState cs = camera.state(1.0f);
                const float3 p = cs.position + cs.forward * config.cameraLight + cs.up * -0.5f;
                fpak::LightGpu l = {};
                l.positionRange[0] = p.x;
                l.positionRange[1] = p.y;
                l.positionRange[2] = p.z;
                l.positionRange[3] = config.cameraLight * 4.0f;
                l.color[0] = l.color[1] = l.color[2] = config.cameraLight * config.cameraLight * 1.5f;
                l.color[3] = float(fpak::kLightPoint);
                l.direction[3] = l.params[0] = -1.0f;
                extra.push_back(l);
            }
            if (!vt.init(gpu, pak, vtPath.c_str(), config.vtPages) ||
                !lights.init(gpu, pak, scene, config.testLights, config.shadowedLights, extra, config.sunScale) ||
                !skinning.init(gpu) ||
                !vsm.init(gpu, pak, lights, skinning, vt, config.editor ? LightSystem::kMaxShadowSlots : 0u) ||
                !renderer.init(gpu, pak, vt, lights, vsm, skinning))
                return 1;
            renderer.setScatterPools(scatterPools, scatterSpareFirst, scatterSpareCount);
            renderer.live().setColliderPak(&pak);  // (live meshes' colliders: physics and audio read them there)
            level::loadNavigation(config.scenePath ? config.scenePath : "");  // (its baked navigation mesh, if it has one)
            levelMeshes.attach(scene, pak, renderer.live(), &materials, &renderer.surfaceLayers(),
                               [&renderer](uint32_t slot, int32_t layer) { renderer.setInstanceLayer(slot, layer); },
                               [&renderer](uint32_t slot, bool hidden) { renderer.setInstanceHidden(slot, hidden); });
            levelMeshes.setManageVisibility(!config.editor);  // (the editor's rows are its own: the eye, Delete)
            levelMeshes.loadBake(config.scenePath ? config.scenePath : "");  // (its cuts as saved: not cut again)
            if (config.projectPath) {  // (faces without a material: the project's blockout grid)
                Project p;
                std::string error;
                if (loadProject(config.projectPath, p, error))
                    levelMeshes.setDefaultMaterial(p.levelDefaultMaterial);
            }
            if (firstScene && !p.meshes.init(gpu))
                FX_WARN("meshes: runtime meshes unavailable");
            renderer.setMeshes(&p.meshes);
            renderer.setMaterialParams([&](uint32_t m) { return materials.engineParams(m); });  // (V1: vertex bound, distance)
            scripts.setMeshes(&p.meshes);
            scripts.setLive(&renderer.live());
            materials.setGpu(gpu.get(), &vt);  // live material edits reach the renderer
            if (config.editor)
                animPreview.init({&gpu, &pak, &skinning, &animation, &vt});
            // A generator stack (a preset's name or JSON) over the terrain, before it is drawn.
            if (terrain.valid() && config.terrainGenerate) {
                const char* preset = terrainGeneratorPreset(config.terrainGenerate);
                if (!preset && config.terrainGenerate[0] != '{') {
                    FX_ERROR("terrain: no generator preset '%s'", config.terrainGenerate);
                    return 1;
                }
                TerrainGenStack stack = parseGenStack(preset ? preset : config.terrainGenerate);
                resolveGenLayers(stack, terrain);
                terrain.setGeneratorJson(genStackJson(stack));
                if (!renderer.terrainGenerator().run(terrain, stack))
                    return 1;
                // The start camera above the new ground.
                FlyCamera::Pose pose = camera.pose();
                pose.position.y = std::max(pose.position.y, terrain.heightAt(pose.position.x, pose.position.z) + 3.0f);
                camera.setPose(pose);
            }
            if (config.hasCameraPose) {
                const float* p = config.cameraPose;
                const float3 d = normalize(float3{p[3] - p[0], p[4] - p[1], p[5] - p[2]});
                FlyCamera::Pose pose = camera.pose();
                pose.position = {p[0], p[1], p[2]};
                pose.pitch = std::asin(std::clamp(d.y, -0.999f, 0.999f));
                pose.yaw = std::atan2(-d.x, -d.z);
                camera.setPose(pose);
            }
            if (terrain.valid())
                renderer.setTerrain(&terrain, config.scenePath);
            if (config.projectPath)  // (R4: the project's resolve, with its surface shaders, if it has one)
                renderer.setProjectShaders(ProjectShaders::resolvePath(config.projectPath), ProjectShaders::transparentPath(config.projectPath),
                                           ProjectShaders::deformPath(config.projectPath));
            renderer.setWater(&water);
            // Particle sprites: their materials by path (the project's .fmat files).
            renderer.particles().setSource(&particles, [&materials](const std::string& source) { return materials.indexOf(source); });
            renderer.decals().setSource(&decals, &scene, [&materials](const std::string& source) { return materials.indexOf(source); });
          }
            scripts.setWater(&water);
            if (config.projectPath && config.scenePath)
                scripts.setScene(config.projectPath,
                                 std::filesystem::path(config.scenePath).lexically_relative(config.projectPath).generic_string());
            scripts.setTerrain(&terrain);
            // Particles and prefabs for the game; collision prefabs made for it (Stop deletes them).
            prefabSpawner.setProject(config.projectPath ? config.projectPath : "");
            runtimeMeshes.setMaterials([&materials](const std::string& source) { return materials.indexOf(source); });
            prefabSpawner.setMeshes(&runtimeMeshes, &pak);
            prefabSpawner.setPhysics(usePhysics ? &physics : nullptr);
            scripts.setParticles(&particles);
            scripts.setPrefabs(&prefabSpawner);
            prefabSpawner.setScripts([&scripts](uint64_t e, const std::vector<ScriptEntry>& list) { scripts.addScripts(e, list); });
            scripts.setRuntimeMeshes(&runtimeMeshes);
            scripts.setMaterials(&materials);
            scripts.setDecals(
                [&](const std::string& material, const float point[3], const float normal[3], float size, float depth, float angle, float lifetime,
                    uint64_t attach) {
                    const flecs::entity on = attach && scene.world().is_alive(attach) ? flecs::entity(scene.world(), attach) : flecs::entity();
                    return decals.place(scene, material, point, normal, size, depth, angle, lifetime, on);
                },
                [&] { decals.clear(); });
            particles.setPrefabSpawner([&](const std::string& path, const float world[3][4]) {
                std::vector<uint64_t> made;
                prefabSpawner.spawn(scene, path, world, {}, &made);
                for (const uint64_t id : made)
                    scripts.track(id);
            });
            particles.setScriptStep([&](flecs::entity_t script, const std::string& className, Particle* p, uint32_t count, const ParticleHit* hits,
                                        uint32_t hitCount, float dt) {
                scripts.particleStep(script, className.c_str(), p, int32_t(count), hits, int32_t(hitCount), dt);
            });
            scripts.setAudio(&audio);  // (Audio.*; inactive with --no-audio: scripts' sounds are silent)
            if (config.waterDemo) {  // preset[:x,y,z,width,length]
                WaterBodyDesc d;
                std::string preset = config.waterDemo;
                float v[5] = {0.0f, 0.5f, 0.0f, 24.0f, 12.0f};
                if (const size_t colon = preset.find(':'); colon != std::string::npos) {
                    std::sscanf(preset.c_str() + colon + 1, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
                    preset.resize(colon);
                }
                d.material = "builtin:" + preset;
                d.width = v[3];
                d.length = v[4];
                if (preset == "ocean") {  // (the ocean preset: an endless sea)
                    d.shape = WaterShape::Ocean;
                    d.depth = 50.0f;
                }
                water.setPose(water.add(d), {v[0], v[1], v[2]}, 0.0f);
            }
        }
        FX_INFO("scene: %u entities", scene.instanceCount());
    } else if (!server && !bringup.init(gpu)) {
        return 1;
    }
    boot::mark("pipelines + uploads recorded");
    EditorBridge& editor = p.editor;
    Launcher launcher;
    SceneView sceneView;
    TerrainEditor terrainEditor;
    // Edit mode (simulation paused until Play) from the start; the bridge connects with the UI.
    bool useEditor = config.editor && hasScene && !naive && !uiBundle.empty() && UiLayer::available();
    std::function<void()> startEditor;  // (below)
    const auto startUi = [&] {
        if (!ui.active()) {  // (it failed to start, runSession, or to take the device, above)
            FX_WARN("ui: continuing without UI");
            useEditor = false;
            SDL_SetWindowBordered(window, true);  // no page title bar after all
            return;
        }
        if (editorChrome && (useEditor || config.launcher))
            ui.enableWindowChrome();
        if (config.launcher) {
            Launcher::Init li;
            li.ui = &ui;
            li.window = window;
            li.persistent = config.editorPrefs;
            li.relaunch = config.relaunch;
            launcher.init(li);
            if (config.launcherScript && !launcher.loadScript(config.launcherScript))
                exitCode = 1;
            return;
        }
        if (useEditor)
            startEditor();
    };
    // The editor onto this scene: the first time once the page is up (startUi), a scene opened
    // after another straight away (the page is there already).
    startEditor = [&] {
        EditorBridge::Init ei;
        ei.scene = &scene;
        ei.pak = &pak;
        ei.scenePath = config.scenePath;
        ei.backend = SDL_GetGPUDeviceDriver(gpu.get());
        ei.edits = &edits;
        ei.pools = &primitivePools;
        ei.settings = &settings;
        ei.lights = &lights;
        ei.vsm = &vsm;
        ei.camera = &camera;
        ei.gpu = &gpu;
        ei.renderer = &renderer;
        ei.prefs = config.editorPrefs ? &prefs : nullptr;
        if (!config.editorPrefs && config.projectPath) {  // (tests: libraries' trust kept with the project, not the user's)
            std::filesystem::path dir(config.projectPath);
            if (dir.extension() == ".falcon")
                dir = dir.parent_path();
            libraries::setTrustFile((dir / "Cache" / "trusted-libraries.json").generic_string());
        }
        {  // The Project panel's Library: Falcon's materials and My Library (tests: their own folder)
            std::string user;
            if (config.libraryPath) {
                user = std::filesystem::absolute(std::filesystem::path(config.libraryPath)).generic_string();
            } else if (!config.editorPrefs) {
                if (config.projectPath) {
                    std::filesystem::path dir(config.projectPath);
                    if (dir.extension() == ".falcon")
                        dir = dir.parent_path();
                    user = (dir / "Cache" / "Library").generic_string();
                }
            } else if (const Json* f = prefs.section("library").get("folder"); f && !f->str.empty()) {
                user = f->str;
            } else {
                user = asset_library::defaultUserRoot();
            }
            asset_library::setRoots(std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "") + "content/library", user);
        }
        ei.projectDir = config.projectPath;
        ei.materials = &materials;
        ei.water = &water;
        ei.audio = &audio;
        ei.particles = &particles;
        ei.runtimeMeshes = &runtimeMeshes;
        ei.levelMeshes = &levelMeshes;
        ei.animation = &animation;
        ei.graphs = &graphs;
        ei.skinning = &skinning;
        ei.bones = &bones;
        ei.animPreview = &animPreview;
        ei.relaunch = config.relaunch;
        TerrainEditor::Init ti;
        ti.terrain = &terrain;
        ti.renderer = &renderer;
        ti.pak = &pak;
        ti.settings = &settings;
        ti.scenePath = config.scenePath;
        terrainEditor.init(ti);
        ei.terrain = &terrainEditor;
        if (!p.editorStarted) {
            editor.init(ui, ei);
            p.editorStarted = true;
            if (config.editorScript && !editor.loadScript(config.editorScript))
                exitCode = 1;
        } else {
            editor.attachScene(ei);
        }
        // The scene view: Unity's camera controls, picking, gizmos, grid and outline.
        camera.setSceneControls(true);
        if (sceneView.init(gpu, renderer, pak, camera, editor))
            editor.setSceneView(&sceneView);
        if (std::exchange(p.playOnLoad, false))
            editor.playNow();  // (the game went on into this scene)
    };
    if (!firstScene && useEditor && p.editorStarted)
        startEditor();
    // The editor owns the page's messages; a game UI (--ui) belongs to the scripts.
    const auto attachScripts = [&] {
        return scripts.attach(scene, pak, usePhysics ? &physics : nullptr, &animation, &skinning, config.editor ? nullptr : &ui);
    };
    // A game's scene simulates from the start (the editor's from Play): its entities' bodies.
    if (usePhysics && !config.editor)
        physics.buildEntityBodies();
    // Scripts find entities by name (Entity.Find): the editor's names, or a game's edits.
    if (config.editor) {
        scripts.setNames([&](std::string_view name) { return editor.entityNamed(name); });
        scripts.setNamesUnder([&](uint64_t root, std::string_view name) { return editor.entityNamedUnder(root, name); });
        scripts.setNameOf([&](uint64_t entity) { return editor.nameOfEntity(entity); });
        // Script components: the editor's records' (made at Play), entity properties by key, and
        // the Inspector's classes and live values.
        scripts.setSceneScripts([&] { return editor.sceneScripts(); });
        scripts.setEditor(editor.editorApi());  // (editor code: libraries' Editor/, Scripts/Editor/)
        scripts.setLevelEditorLink(&editor);    // (the level tools: the scene view's input, element selection)
        scripts.setLevelEditor(  // (level meshes changed by editor code while editing: undo steps)
            [&editor](uint64_t e, std::shared_ptr<const level::PolyMesh> m) { return editor.setLevelMesh(e, std::move(m)); },
            [&editor](const float* rows, uint64_t parent, std::shared_ptr<const level::PolyMesh> m) {
                return uint64_t(editor.createLevelMesh(rows, parent, std::move(m)));
            });
        scripts.setUiSend([&](std::string_view json) { editor.gameUiSend(json); });  // (the project's game UI, framed)
        scripts.setEntityByKey([&](const std::string& key) { return uint64_t(editor.entityByKey(key)); });
        editor.setScriptValues([&](uint64_t entity) { return scripts.componentValues(entity); });
        scripts.setTestKeys(editor.testKeys());
    }
    else
        scripts.setNames([&](std::string_view name) -> uint64_t {
            for (const auto* list : {&edits.instances, &edits.created})
                for (const EntityEdit& e : *list)
                    if (e.entity && !e.deleted && e.name == name)
                        return e.entity;
            for (uint32_t slot = 0; slot < scene.instanceCount(); ++slot)  // (cooked names, as the editor names them)
                if (const char* n = pak.instanceName(slot); n && name == n)
                    if (const flecs::entity e = scene.entity(slot); e && e.enabled())
                        return e.id();
            return bones.named(scene.world(), name);
        });
    if (!config.editor)
        scripts.setNameOf([&](uint64_t entity) -> std::string {
            for (const auto* list : {&edits.instances, &edits.created})
                for (const EntityEdit& e : *list)
                    if (e.entity == entity && !e.name.empty())
                        return e.name;
            if (const MeshInstance* mi = flecs::entity(scene.world(), entity).try_get<MeshInstance>())
                if (const char* n = pak.instanceName(mi->gpuSlot))
                    return n;
            for (const BoneEntities::Expanded& x : bones.all())
                for (uint32_t b = 0; b < x.bones.size(); ++b)
                    if (x.bones[b] == entity)
                        return x.skeleton->name(b);
            return {};
        });
    if (!config.editor) {  // a game's script components: its edits'
        scripts.setSceneScripts([&] {
            ScriptHost::SceneScripts out;
            for (const auto* list : {&edits.instances, &edits.created, &edits.lights})
                for (const EntityEdit& e : *list)
                    if (e.entity && !e.deleted && !e.scripts.empty())
                        out.emplace_back(e.entity, e.scripts);
            return out;
        });
        scripts.setEntityByKey([&](const std::string& key) { return uint64_t(edits.entityOfKey(pak, scene, key)); });
    }
    if (!config.editor)
        scripts.setNamesUnder([&](uint64_t root, std::string_view name) -> uint64_t {
            const auto under = [&](flecs::entity_t id) {
                uint32_t depth = 0;
                for (flecs::entity p = scene.parentOf(flecs::entity(scene.world(), id)); p && depth < 256; p = scene.parentOf(p), ++depth)
                    if (p.id() == root)
                        return true;
                return false;
            };
            for (const auto* list : {&edits.instances, &edits.created})
                for (const EntityEdit& e : *list)
                    if (e.entity && !e.deleted && e.name == name && under(e.entity))
                        return e.entity;
            for (const BoneEntities::Expanded& x : bones.all())
                if (const int32_t b = x.skeleton->findBone(std::string(name).c_str()); b >= 0 && under(x.bones[uint32_t(b)]))
                    return x.bones[uint32_t(b)];
            return 0;
        });
    if (useScripts) {
        // A game starts now; the editor's starts with Play (and a broken build doesn't stop it).
        if (!config.editor) {  // (a game's OnStart reads the camera it starts from)
            float world[3][4];
            CameraComponent c;
            scripts.setCamera(scene.mainCamera(world, c) ? cameraStateFromWorld(world, c.fov, c.nearClip, 1.0f) : camera.state(1.0f));
            scripts.setView(0.0f, 0.0f, float(config.width), float(config.height));  // (the window: updated each frame)
        }
        // (A game going on from the scene before: loadScene keeps it; the first scene starts it.)
        if (config.editor ? !attachScripts() : !scripts.loadScene(scene, pak, usePhysics ? &physics : nullptr, &animation, &skinning, &ui)) {
            if (!config.editor)
                return 1;
            useScripts = false;
        }
        if (useScripts && config.editor) {
            editor.setScriptTypes(scripts.componentTypes());
            editor.setDataTypes(scripts.dataTypes());
            editor.setTools(scripts.editorTools());
        }
        boot::mark("scripts started");
    }
    if (config.audio && !config.launcher && !usePreview && firstScene) {  // (the launcher and preview jobs are silent)
        AudioConfig ac;
        ac.nullDevice = config.audioNull || config.hidden;
        ac.capturePath = config.audioCapture;
        ac.spatializer = AudioConfig::Spatializer(config.audioSpatializer);
        AudioSettings settings;
        if (config.projectPath) {  // its volumes, HRTF switch and reflections (before init: the reverb type)
            audio.setProjectDir(config.projectPath);
            Project p;
            std::string error;
            if (loadProject(config.projectPath, p, error))
                settings = p.audio;
        }
        if (config.audioReflections >= 0)
            settings.reflections = uint32_t(config.audioReflections);
        audio.applySettings(settings);
        ac.synchronousReflections = ac.nullDevice && config.fixedTimestep;
        if (!audio.init(ac))
            return 1;
        // Test sounds: "<tone hz | file>[:x,y,z]", 2D without a position.
        const auto testSound = [&](const char* spec, bool tone) {
            std::string what = spec;
            AudioVoiceParams p;
            p.maxDistance = 1e5f;  // (heard however far: tests measure the distance and the air)
            const size_t colon = what.rfind(':');
            float3 at{};
            if (colon != std::string::npos && colon > 1 &&
                std::sscanf(what.c_str() + colon + 1, "%f,%f,%f", &at.x, &at.y, &at.z) == 3) {
                what.resize(colon);
                p.flags |= kVoiceSpatial | kVoiceListenerRelative | kVoiceReflections | (config.audioAir ? kVoiceAirAbsorption : 0u);
                p.position = at;
            }
            AudioVoice v = 0;
            if (tone) {
                p.toneHz = float(std::atof(what.c_str()));
                p.gain = 0.25f;
                v = audio.playTone(p);
            } else {
                v = audio.play(what, p);
            }
            if (!v)
                FX_WARN("audio: %s did not play", spec);
        };
        if (config.audioTone)
            testSound(config.audioTone, true);
        if (config.audioPlay)
            testSound(config.audioPlay, false);
    }
    if (config.audio && !config.launcher && !usePreview) {  // the scene's: what occludes, its sources
        if (hasScene)  // what occludes: the scene's meshes, terrain and scatter
            audio.attachScene(&pak, &scene, &materials, &terrain, config.scenePath ? config.scenePath : "");
        if (usePhysics)  // its contacts' sounds
            audio.setPhysics(&physics);
        audio.setWater(&water);  // splashes, slosh, under water
        if (hasScene && !config.editor)
            audio.startGame(scene);  // a game's sources play from its start (the editor's from Play)
        boot::mark("audio started");
    }
    bool gamePlaying = false;        // the editor's Play, as the game last saw it
    uint64_t playBodies = 0;         // physics bodies spawned before Play
    // The editor (or launcher) onto the page before the first frame: the page's ready message,
    // when it came already, is answered in that frame, so its panels fill from the start.
    if (frame == 0 && !uiBundle.empty())
        startUi();
    // Tests run hidden (--hidden): nothing appears or takes focus; frames render offscreen.
    if (!config.hidden && firstScene) {
        // The editor's and launcher's page draws their title bar: the window waits a little for
        // its first frame (Chromium started alongside the GPU device) rather than show up blank;
        // the editor's for one drawn after it took the state (startUi), its panels filled.
        const uint64_t pageDeadline = SDL_GetTicksNS() + 500'000'000ull;
        const uint64_t framesBefore = useEditor && editor.connected() ? ui.stats().frames : 0;
        while (editorChrome && ui.active() && ui.stats().frames <= framesBefore && SDL_GetTicksNS() < pageDeadline) {
            ui.pump();
            SDL_Delay(1);
        }
        SDL_ShowWindow(window);
    }
    if (!firstScene) {
        lastTicks = SDL_GetTicksNS();  // (the load is not a frame's time)
        FX_INFO("scene: %s in place, %.0f ms after the last frame of the one before", config.scenePath,
                double(lastTicks - p.switchNs) * 1e-6);
    }

    bool running = true;
    uint64_t shadersSeen = 0;  // the editor's shaders revision this scene took (R4)
    uint32_t streamingIdleFrames = 0;
    bool fireRequested = false, dropRequested = false, physicsPaused = false;
    std::vector<InstanceUpdate> instanceRows;
    std::vector<float3> simFocus;  // the view's position and the game's characters' (terrain loads around them)
    // A server's frames: at most one per tick (nothing to draw: it waits for the next tick).
    uint64_t serverNextNs = SDL_GetTicksNS();

    // The view: the main camera entity in Play, on the Game tab and in the game; else the fly camera.
    bool mainCameraView = false;
    const auto viewCamera = [&](float aspect) {
        float world[3][4];
        CameraComponent c;
        if (mainCameraView && scene.mainCamera(world, c))
            return cameraStateFromWorld(world, c.fov, c.nearClip, aspect);
        return camera.state(aspect);
    };
    std::unordered_map<flecs::entity_t, WorldTransform> movedLights;  // --moving-lights: where each started
    while (running) {
        {
            float world[3][4];
            CameraComponent c;
            mainCameraView = hasScene && (!useEditor || editor.playing() || sceneView.tools().gameView) && scene.mainCamera(world, c);
        }
        SDL_Event e;
        int pixelW = 0, pixelH = 0, logicalW = 0, logicalH = 0;
        SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
        SDL_GetWindowSize(window, &logicalW, &logicalH);
        const float pixelScaleX = logicalW > 0 ? float(pixelW) / float(logicalW) : 1.0f;
        const float pixelScaleY = logicalH > 0 ? float(pixelH) / float(logicalH) : 1.0f;
        {
        FX_ZONE_CAT("Events", Editor);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)
                scripts.gamepadEvent(e);  // (connected any time; read while the game runs)
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }
            if ((e.type == SDL_EVENT_WINDOW_MOVED || e.type == SDL_EVENT_WINDOW_RESIZED) &&
                !(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
                SDL_GetWindowPosition(window, &normalRect[0], &normalRect[1]);
                SDL_GetWindowSize(window, &normalRect[2], &normalRect[3]);
            }
            camera.keyEvent(e);  // (the fly keys by their events too, whoever takes them)
            if (!ui.handleEvent(e) || config.launcher)  // the page took it (the launcher's page takes all)
                continue;
            // Files dropped on the editor: imported into the project once the drop is complete.
            if (useEditor && (e.type == SDL_EVENT_DROP_BEGIN || e.type == SDL_EVENT_DROP_FILE || e.type == SDL_EVENT_DROP_COMPLETE)) {
                if (e.type == SDL_EVENT_DROP_BEGIN)
                    droppedFiles.clear();
                else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
                    droppedFiles.push_back(e.drop.data);
                else if (e.type == SDL_EVENT_DROP_COMPLETE && !droppedFiles.empty())
                    editor.dropped(std::exchange(droppedFiles, {}));
                continue;
            }
            if (useEditor) {
                // Editor: the scene view and the camera; no game or debug hotkeys (the page and
                // the settings panel cover them, and F2 / F mean rename / frame there). The fly
                // camera holds still while a main camera entity has the view. The Animator's
                // preview pane takes what happens over it.
                if (UiRect r; ui.pointerView() == "anim" && ui.view("anim", uint32_t(pixelW), uint32_t(pixelH), r)) {
                    animPreview.handleEvent(e, r, pixelScaleX, pixelScaleY);
                    continue;
                }
                if (!ui.sceneShown())
                    continue;
                if (editor.playing())  // the game's mouse: what happens over its view
                    scripts.event(e);
                if (!sceneView.handleEvent(e, pixelScaleX, pixelScaleY) && !mainCameraView)
                    camera.handleEvent(window, e);
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) {
                if (!scripts.freeMouse())  // (the game held the cursor: Esc shows it; again quits)
                    running = false;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F && !e.key.repeat)
                fireRequested = true;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_R && !e.key.repeat)
                dropRequested = true;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_P && !e.key.repeat)
                physicsPaused = !physicsPaused;
            else if (e.type == SDL_EVENT_KEY_DOWN)
                handleRenderKey(e.key.key, settings);
            scripts.event(e);
            if (!mainCameraView)  // (a main camera entity has the view: the game moves it)
                camera.handleEvent(window, e);
        }
        }  // events
        const uint64_t nowTicks = SDL_GetTicksNS();
        // (a recording's replay, the synthetic player: their frame lengths, not the clock's)
        const float dt = p.sim.active() ? p.sim.frameDelta(float(double(nowTicks - lastTicks) * 1e-9))
                                        : float(double(nowTicks - lastTicks) * 1e-9);
        // Eye adaptation: ~1.5/s time constant; fixed time adapts instantly so captures only
        // depend on the captured frame.
        settings.post.exposureBlend = config.fixedTimestep ? 1.0f : 1.0f - std::exp(-dt * 1.5f);
        settings.seconds = config.fixedTimestep ? float(frame) / 60.0f : float(double(nowTicks - startTicks) * 1e-9);
        settings.frameTimeMs = config.fixedTimestep ? 1000.0f / 60.0f : dt * 1000.0f;
        if (!config.fixedTimestep) {
            if ((ui.gameKeyboard() || useEditor) && !mainCameraView)  // the editor camera only flies while right-dragging
                camera.update(dt);
        } else if (frame > 0 && (!config.capturePath || frame <= config.captureFrame))
            camera.dolly(config.cameraDolly / 60.0f);  // captures: stop moving at the capture frame
        lastTicks = nowTicks;
        const uint64_t u0 = SDL_GetTicksNS();
        ui.pump();
        if (config.launcher)
            launcher.update();
        if (useEditor) {
            editor.update(camera.state(float(viewport.w) / float(viewport.h)));
            terrainEditor.tick(dt);  // this frame's brush dabs, before the terrain is drawn
            // The project's game built: its code loaded, or replaced (a running game starts again).
            std::string built;
            if (editor.takeBuiltGame(built)) {
                bool ok;
                if (useScripts) {
                    ok = scripts.reload();
                } else {
                    ok = scripts.beginLoad(built.c_str(), true) && attachScripts();
                    useScripts = ok;
                }
                editor.gameLoaded(ok);
                editor.setScriptTypes(scripts.componentTypes());  // (the classes the new code has)
                editor.setDataTypes(scripts.dataTypes());
                editor.setTools(scripts.editorTools());
            }
            // (R4) The project's shaders compiled again: its resolve (surface shaders) and the
            // materials' ids into it.
            if (const uint64_t r = editor.shadersRevision(); r != shadersSeen && hasScene && !naive && config.projectPath) {
                shadersSeen = r;
                renderer.setProjectShaders(ProjectShaders::resolvePath(config.projectPath), ProjectShaders::transparentPath(config.projectPath),
                                           ProjectShaders::deformPath(config.projectPath));
                materials.reloadAll();
            }
            if (std::string next; editor.takeSceneSwitch(next) && config.projectPath) {  // (Open Scene, New Scene, a cook done)
                p.nextScene = (std::filesystem::path(config.projectPath) / next).lexically_normal().generic_string();
                running = false;
            }
            // Editor code (libraries' Editor/, Scripts/Editor/): the menu items run, the events, its update.
            if (useScripts) {
                for (const std::string& menu : editor.takeToolRuns())
                    if (!scripts.editorRun(menu))
                        FX_WARN("editor: no menu item %s", menu.c_str());
                for (const std::string& event : editor.takeEditorEvents())
                    scripts.editorEvent(event);
                for (std::string& message : editor.takeGameUiMessages())
                    scripts.uiReceive(std::move(message));
                // (X3) Components' buttons; then the editor's update and the frame's gizmos (while
                // editing they are all the lines over the views; while playing, with the game's).
                for (const EditorBridge::ButtonRun& b : editor.takeButtonRuns())
                    if (const std::string after = scripts.componentButton(b.entity, b.className, b.method, b.properties); !after.empty())
                        editor.buttonDone(b, after);
                scripts.editorUpdate(dt);
                scripts.drawGizmos(editor.selectedEntity());
                editor.setGizmoLineCount(uint32_t(scripts.gizmoLines().size()));
                if (!editor.playing()) {
                    std::vector<SceneView::WorldLine> lines;
                    lines.reserve(scripts.gizmoLines().size());
                    for (const ScriptHost::DebugLine& l : scripts.gizmoLines())
                        lines.push_back({{l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.color});
                    sceneView.setWorldLines(std::move(lines));
                }
            } else {
                editor.takeToolRuns();
                editor.takeEditorEvents();
                editor.takeButtonRuns();
            }
            // Play starts the game. Stop ends it, removes the bodies spawned meanwhile and puts the
            // scene back (after the game's OnStop, so nothing it does then stays).
            if (editor.playing() != gamePlaying) {
                gamePlaying = editor.playing();
                if (gamePlaying) {
                    playBodies = usePhysics ? physics.spawnCount() : 0;
                    if (usePhysics)
                        physics.buildEntityBodies();  // (before the game: its OnStart finds them)
                    audio.startGame(scene);
                    particles.restartAll();
                    if (useScripts) {  // (OnStart reads the camera it plays from: the main camera's, else the editor's)
                        float world[3][4];
                        CameraComponent c;
                        scripts.setCamera(scene.mainCamera(world, c) ? cameraStateFromWorld(world, c.fov, c.nearClip, 1.0f) : camera.state(1.0f));
                        scripts.setView(float(viewport.x) / pixelScaleX, float(viewport.y) / pixelScaleY, float(viewport.w) / pixelScaleX,
                                        float(viewport.h) / pixelScaleY);
                        if (scripts.running()) {
                            scripts.continueGame();  // (it went on from the scene before: Scene.Load)
                        } else {
                            if (useEditor) {
                                editor.setNetworkStatus([&scripts] { return scripts.netStatus(); });  // (the Network tab)
                                editorNetworkPlay(p, config, editor);
                            }
                            scripts.startGame();
                        }
                    }
                } else {
                    if (useScripts)
                        scripts.stopGame();
                    if (useEditor)
                        endEditorClients(p);
                    sceneView.setWorldLines({});  // (the game's debug lines go with it)
                    editor.setDebugLineCount(0);
                    if (usePhysics) {
                        physics.removeSpawnedSince(playBodies);
                        physics.clearEntityBodies();
                    }
                    audio.stopGame(scene);
                    for (const flecs::entity_t id : particleDisabled)
                        if (flecs::entity e(scene.world(), id); e.is_alive())
                            e.enable();
                    particleDisabled.clear();
                    particles.restartAll();
                    decals.clear();  // (what the game placed)
                    editor.restorePlay();
                    if (editor.comingBack()) {  // (Play began in another scene: back to it, as it was)
                        p.nextScene = (std::filesystem::path(config.projectPath) / editor.keptScene()).lexically_normal().generic_string();
                        p.editsOverride = editor.comeBack();
                        running = false;
                    }
                }
            }
            editor.setGameStatus({useScripts && scripts.active(), scripts.running(), scripts.gameEntities(),
                                  usePhysics ? physics.stats().bodies : 0});
            editor.setGameHud(scripts.hud(), scripts.hudRevision());
        }
        editorUiNs += SDL_GetTicksNS() - u0;
        if (!uiShown && ui.stats().frames > 0) {
            boot::mark("ui first frame");
            uiShown = true;
        }
        if (useEditor && frame > 0 && !ui.active())
            useEditor = false;  // the UI failed: run the game
        // Edit mode: the scene holds still until Play.
        const bool simulate = !useEditor || editor.simulateFrame();

        // Scripts, then physics: fixed 60 Hz (one step per frame under fixed time). Both freeze
        // once a capture frame is reached so the captured state cannot depend on streaming timing.
        const bool captureFrozen = config.capturePath && !config.captureSequence && frame > config.captureFrame;
        const bool frozen = captureFrozen || !simulate;
        if (useScripts)
            scripts.frameTime(float(dt) * 1000.0f);  // (Performance: wall time, whether it simulates or not)
        if (useScripts && !frozen) {
            if (config.scriptReloadAt && frame == config.scriptReloadAt && !scripts.reload())
                exitCode = 1;  // reported by the reload test
            const CameraState cs = viewCamera(1.0f);
            const float scriptDt = config.fixedTimestep ? 1.0f / 60.0f : dt;
            const double scriptTime = config.fixedTimestep ? double(frame) / 60.0
                                      : p.sim.active()        ? p.sim.clock()
                                                              : double(nowTicks - startTicks) * 1e-9;
            const uint64_t s0 = SDL_GetTicksNS();
            if (p.loader && pumpSceneLoad(p))
                running = false;  // (read: in this scene's place after this frame, the game updated once more)
            scripts.setView(float(viewport.x) / pixelScaleX, float(viewport.y) / pixelScaleY, float(viewport.w) / pixelScaleX,
                            float(viewport.h) / pixelScaleY);
            scripts.update(scriptDt, scriptTime, frame, cs);
            if (useEditor) {  // (scripts' debug lines over the editor's views; aged after)
                std::vector<SceneView::WorldLine> lines;
                lines.reserve(scripts.debugLines().size() + scripts.gizmoLines().size());
                for (const ScriptHost::DebugLine& l : scripts.debugLines())
                    lines.push_back({{l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.color});
                editor.setDebugLineCount(uint32_t(lines.size()));
                for (const ScriptHost::DebugLine& l : scripts.gizmoLines())  // (X3) the editor frame's gizmos
                    lines.push_back({{l.a[0], l.a[1], l.a[2]}, {l.b[0], l.b[1], l.b[2]}, l.color});
                sceneView.setWorldLines(std::move(lines));
            }
            scripts.ageDebugLines(scriptDt);
            if (std::string next; scripts.takeSceneLoad(next)) {
                // Scene.Load: this scene ends after this frame and the next is loaded in its place,
                // the game going on (runSession); the editor stops playing and says so.
                if (useEditor) {
                    // The game plays on in that scene; Stop comes back to this one as the editor
                    // has it (keepForPlay).
                    if (config.projectPath && editor.keepForPlay()) {
                        p.nextScene = (std::filesystem::path(config.projectPath) / next).lexically_normal().generic_string();
                        p.playOnLoad = true;
                        running = false;
                    }
                } else if (config.projectPath) {
                    p.nextScene = (std::filesystem::path(config.projectPath) / next).lexically_normal().generic_string();
                    FX_INFO("scripts: loading %s", next.c_str());
                    running = false;
                    if (p.loader)  // (a load in the background gives way)
                        p.loader->cancel = true;
                }
            }
            if (std::string next; scripts.takeSceneLoadAsync(next)) {
                if (useEditor) {  // (the editor loads it as Scene.Load: the switch is the editor's)
                    scripts.setSceneLoading(-1.0f, "");
                    if (config.projectPath && editor.keepForPlay()) {
                        p.nextScene = (std::filesystem::path(config.projectPath) / next).lexically_normal().generic_string();
                        p.playOnLoad = true;
                        running = false;
                    }
                } else if (config.projectPath)
                    startSceneLoad(p, config.projectPath, next);
            }
            if (scripts.takeQuit()) {  // (Game.Quit: a game ends; the editor stops playing)
                if (useEditor)
                    editor.stopPlaying();
                else
                    running = false;
            }
            scriptNs += SDL_GetTicksNS() - s0;
            editorScriptNs += SDL_GetTicksNS() - s0;
        }
        if (hasScene && !naive && frame == 0)
            for (uint32_t i = 0; i < config.animSetCount; ++i) {
                const char* eq = std::strchr(config.animSet[i], '=');
                if (!eq)
                    continue;
                const std::string name(config.animSet[i], eq);
                for (uint32_t r = 0; r < skinning.recordCount(); ++r)
                    if (anim::GraphInstance* g = animation.graph(r))
                        g->setFloat(g->parameterIndex(name.c_str()), float(std::atof(eq + 1)));
            }
        if (hasScene && !naive && !captureFrozen) {  // edit mode: zero-length steps keep characters posed
            const uint64_t a0 = SDL_GetTicksNS();
            if (animation.ikActive())
                scene.propagate();  // (IK places limbs in the world: where scripts moved the characters)
            // (the editor's edit mode: Animators previewing in the editor play; the rest hold)
            const bool editing = useEditor && !editor.playing();
            animation.update(!simulate && !editing ? 0.0f : (config.fixedTimestep ? 1.0f / 60.0f : dt) * (editing ? 1.0f : scripts.timeScale()),
                             editing);
            if (UiRect r; useEditor && animPreview.active() && ui.view("anim", uint32_t(pixelW), uint32_t(pixelH), r))
                animPreview.update(config.fixedTimestep ? 1.0f / 60.0f : dt, renderer.frameIndex());
            // Models' rigid animation (their moving parts and cameras): from the start of each
            // Play in the editor (Stop restores the transforms), from the start of a game.
            if (useEditor && editor.playing() != nodeAnimPlaying) {
                nodeAnimPlaying = editor.playing();
                if (nodeAnimPlaying) {
                    nodeAnimator.set(editor.animationLinks(), pak.nodeFps());
                } else {
                    nodeAnimator.clear();
                    animation.reset();  // characters back to their start (graphs, parameters)
                    animation.update(0.0f);
                }
            }
            if (nodeAnimator.active() && simulate)
                nodeAnimator.update(scene, config.fixedTimestep ? 1.0f / 60.0f : dt);
            // Bones follow the poses, and what hangs under them follows now (physics, audio and the
            // render see this frame's).
            if (!bones.empty() && bones.update(scene.world(), animation))
                scene.propagate();
            editorAnimNs += SDL_GetTicksNS() - a0;
        }
        if (usePhysics) {
            const CameraState cs = viewCamera(1.0f);
            if (frame == 0 && config.physicsDrop)
                physics.dropPile(cs.position + cs.forward * 7.0f + cs.up * 1.5f, config.physicsDrop, 1);
            if (dropRequested) {
                physics.dropPile(cs.position + cs.forward * 6.0f + cs.up * 2.0f, 256, uint32_t(frame));
                dropRequested = false;
            }
            if (fireRequested) {
                physics.fire(cs.position, cs.forward);
                fireRequested = false;
            }
            const uint64_t p0 = SDL_GetTicksNS();
            // Terrain collision near the view (changed tiles rebuilt while simulating).
            simFocus.assign(1, cs.position);
            simFocus.insert(simFocus.end(), scripts.simFocus().begin(), scripts.simFocus().end());
            physics.syncTerrain(terrain.valid() ? &terrain : nullptr, simFocus, 512.0f, !frozen);
            // Scatter collision: cells near the view, read back from the GPU a few frames late.
            renderer.scatter().setCollisionFocus(cs.position, 96.0f);
            renderer.scatter().takeCollision(scatterAdded, scatterRemoved);
            physics.syncScatter(scatterAdded, scatterRemoved);
            audio.syncScatter(scatterAdded, scatterRemoved);
            water.setTime(config.capturePath && !config.captureSequence && config.fixedTimestep
                              ? double(std::min<uint64_t>(frame, config.captureFrame)) / 60.0
                              : config.fixedTimestep ? double(frame) / 60.0 : double(nowTicks - startTicks) * 1e-9);
            if (!physicsPaused && !frozen) {
                if (config.simRollback && useScripts)  // (the history kept from the first tick)
                    scripts.simControl(2, config.simRollback + 2);
                // (Time.Scale, and a client's time dilation; its steps are the ticks)
                physics.update(dt * scripts.timeScale() * scripts.simRate(), config.fixedTimestep, scripts.maxTicks());
                if (config.simRollback && useScripts)  // (test: back and forward again, every frame)
                    scripts.rollback(config.simRollback);
                if (useScripts)  // (X1: the scripts after physics, this frame's events delivered)
                    scripts.phase(ScriptHost::AfterPhysics, (config.fixedTimestep ? 1.0f / 60.0f : dt) * scripts.timeScale());
            }
            physicsNs += SDL_GetTicksNS() - p0;
            editorPhysicsNs += SDL_GetTicksNS() - p0;
        }
        // Particles: after physics and the bones (they start where this frame's emitters are). In
        // the editor's edit mode they preview; a paused Play holds them.
        if (hasScene) {
            const bool held = captureFrozen || (useEditor && editor.playing() && !simulate);
            particles.setPhysics(usePhysics ? &physics : nullptr);
            particles.update(scene, held ? 0.0f : (config.fixedTimestep ? 1.0f / 60.0f : dt) * scripts.timeScale(), useEditor && !editor.playing());
            decals.update(scene, held ? 0.0f : config.fixedTimestep ? 1.0f / 60.0f : dt);
            for (const flecs::entity_t id : particles.takeFinished()) {
                flecs::entity e(scene.world(), id);
                if (!e.is_alive() || !e.enabled())
                    continue;
                if (useEditor) {  // (Stop brings it back)
                    e.disable();
                    particleDisabled.push_back(id);
                } else {
                    e.destruct();
                }
            }
        }
        if (useScripts && !frozen)  // (X1: animation and physics done)
            scripts.phase(ScriptHost::LateUpdate, (config.fixedTimestep ? 1.0f / 60.0f : dt) * scripts.timeScale());
        if (!usePhysics)  // (no ticks: the water's simulation clock follows the frames)
            water.setSimTime(config.fixedTimestep ? double(frame) / 60.0 : double(nowTicks - startTicks) * 1e-9);
        if (p.sim.active() && hasScene) {  // (the frame's transforms checked; a replay played out ends the run)
            p.sim.frameEnd(SimRecorder::hashEntities(scene));
            if (p.sim.finished()) {
                p.nextScene.clear();
                running = false;
            }
        }
        // Audio hears from the view's camera; fixed time mixes exactly a 60 Hz frame of samples.
        audio.update(hasScene ? &scene : nullptr, hasScene && (!useEditor || editor.playing()), viewCamera(1.0f),
                     config.fixedTimestep ? 1.0f / 60.0f : dt);

        SDL_GPUTexture* swapchain = nullptr;
        uint32_t width = 0, height = 0;
        if (server) {
            // A dedicated server: the render's CPU part (children following parents, PreRender, the
            // rows the game and physics changed drained, water bodies following their entities,
            // terrain around the characters), then a sleep until the next tick is due.
            if (hasScene) {
                instanceRows.clear();
                scene.propagate();
                if (useScripts && scripts.running()) {
                    scripts.phase(ScriptHost::PreRender, dt);
                    scene.propagate();
                }
                scene.extractChanged(pak, instanceRows);
                if (useScripts)
                    scripts.extract(instanceRows);
                if (usePhysics)
                    physics.extract(instanceRows);
                runtimeMeshes.update(scene, pak, particles, instanceRows);
                levelMeshes.update(instanceRows);
                for (uint32_t i = 0; i < water.bodies().size(); ++i) {
                    const WaterSystem::Body& b = water.bodies()[i];
                    const flecs::entity e = b.alive && b.entity ? flecs::entity(scene.world(), b.entity) : flecs::entity();
                    if (const WorldTransform* w = e && e.is_alive() ? e.try_get<WorldTransform>() : nullptr)
                        water.setPose(i, {w->rows[3], w->rows[7], w->rows[11]}, std::atan2(-w->rows[8], w->rows[0]));
                }
                if (terrain.valid()) {
                    simFocus.assign(1, viewCamera(1.0f).position);
                    simFocus.insert(simFocus.end(), scripts.simFocus().begin(), scripts.simFocus().end());
                    terrain.stream(simFocus, config.terrainRadius);
                }
            }
            const uint64_t tickNs = 1'000'000'000ull / 60;
            serverNextNs = std::max(serverNextNs + tickNs, SDL_GetTicksNS() - tickNs);
            if (const uint64_t now = SDL_GetTicksNS(); serverNextNs > now && !config.fixedTimestep)
                SDL_DelayPrecise(serverNextNs - now);
        } else {
        if (config.hidden) {
            // Hidden (tests): no swapchain image (Vulkan has none for a hidden window), so the
            // frame goes to an offscreen texture of the same format and size, and waiting for
            // the GPU replaces the swapchain's frame pacing.
            {
                FX_ZONE_CAT("Wait for GPU (hidden)", Wait);
                SDL_WaitForGPUIdle(gpu.get());
            }
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            width = uint32_t(std::max(pw, 1));
            height = uint32_t(std::max(ph, 1));
            if (!hiddenTarget || hiddenW != width || hiddenH != height) {
                if (hiddenTarget)
                    gpumem::releaseTexture(gpu.get(), hiddenTarget);
                SDL_GPUTextureCreateInfo t = {};
                t.type = SDL_GPU_TEXTURETYPE_2D;
                t.format = gpu.swapchainFormat();
                t.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                t.width = hiddenW = width;
                t.height = hiddenH = height;
                t.layer_count_or_depth = 1;
                t.num_levels = 1;
                hiddenTarget = gpumem::createTexture("Frame", gpu.get(), &t);
                gpumem::setTextureName(gpu.get(), hiddenTarget, "frame target (--hidden)");
            }
            swapchain = hiddenTarget;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.get());
        // The main CPU wait in the frame: blocks when frames-in-flight are all queued.
        bool acquired = true;
        if (!config.hidden) {
            FX_ZONE_CAT("Wait for swapchain", Wait);
            acquired = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &width, &height);
        }
        if (!acquired) {
            FX_ERROR("acquire swapchain: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            exitCode = 1;
            break;
        }
        if (!swapchain) {  // minimized
            SDL_SubmitGPUCommandBuffer(cmd);
            freezewatch::heartbeat(frame);  // (not frozen: just not drawing)
            continue;
        }
        gpuprof::beginFrame(cmd);

        // With an editor layout the scene renders into the page's viewport, at that size (unless
        // another view has the space: the Animator's preview).
        viewport = ui.viewport(width, height);
        const bool sceneShown = !useEditor || ui.sceneShown();
        UiRect animRect;
        const bool animShown = useEditor && animPreview.active() && ui.view("anim", width, height, animRect);
        const float time = config.fixedTimestep ? float(frame) / 60.0f
                                                : float(double(SDL_GetTicksNS() - startTicks) * 1e-9);
        SDL_GPUTexture* sceneColor = nullptr;
        if (naive) {
            const ViewParams view = {camera.viewProj(float(viewport.w) / float(viewport.h)), camera.position(),
                                     SDL_min(uint32_t(settings.debugView), 3u)};
            naiveRenderer.render(cmd, viewport.w, viewport.h, view);
            sceneColor = naiveRenderer.sceneColor();
        } else if (hasScene) {
            if (config.movingLights > 0.0f) {
                moveLights(scene, float(frame) / 60.0f * config.movingLights, movedLights);
                lights.setMotion(float(frame) / 60.0f * config.movingLights);
            }
            instanceRows.clear();
            scene.propagate();  // children follow their parents (editor or scripts moved them)
            if (useScripts && scripts.running()) {  // (X1: the frame's transforms final)
                scripts.phase(ScriptHost::PreRender, config.fixedTimestep ? 1.0f / 60.0f : dt);
                scene.propagate();  // (what it moved)
            }
            scene.extractChanged(pak, instanceRows);
            if (useScripts)
                scripts.extract(instanceRows);  // (rows the game changed: visibility, tint, material)
            if (useEditor)
                editor.extract(instanceRows);
            if (usePreview) {  // this frame's job: its instances and the camera
                preview.update(pak, camera, instanceRows, streamingIdleFrames);
                if (preview.done())
                    running = false;
            }
            if (usePhysics)
                physics.extract(instanceRows);
            runtimeMeshes.update(scene, pak, particles, instanceRows);  // (particles' models, spawned meshes)
            levelMeshes.update(instanceRows);                           // (level meshes made, changed, gone)
            const CameraState frameCamera = viewCamera(float(viewport.w) / float(viewport.h));
            // Water: its clock (captures stop it at the capture frame, like the simulation: the frame
            // captured must not depend on how long streaming took), and bodies following their entities.
            water.setTime(config.capturePath && !config.captureSequence && config.fixedTimestep
                              ? double(std::min<uint64_t>(frame, config.captureFrame)) / 60.0
                              : double(time));
            for (uint32_t i = 0; i < water.bodies().size(); ++i) {
                const WaterSystem::Body& b = water.bodies()[i];
                if (!b.alive || !b.entity)
                    continue;
                const flecs::entity e(scene.world(), b.entity);
                const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr;
                if (!w)
                    continue;
                water.setPose(i, {w->rows[3], w->rows[7], w->rows[11]}, std::atan2(-w->rows[8], w->rows[0]));
                water.setVisible(i, e.enabled() && !b.hidden);
            }
            if (terrain.valid()) {
                simFocus.assign(1, frameCamera.position);
                simFocus.insert(simFocus.end(), scripts.simFocus().begin(), scripts.simFocus().end());
                terrain.stream(simFocus, config.terrainRadius);
            }
            materials.flush(cmd);  // (material changes: the editor's, scripts')
            if (sceneShown) {
                if (!heldRows.empty()) {  // what changed meanwhile, then this frame's (later rows win)
                    heldRows.insert(heldRows.end(), instanceRows.begin(), instanceRows.end());
                    instanceRows.swap(heldRows);
                    heldRows.clear();
                }
                renderer.streamInstances(cmd, instanceRows);
                lights.setTransientLights(particles.lights());  // (particles' Light Renderers)
                renderer.setPostEffects(useScripts ? scripts.postEffects() : std::vector<PostProcess::EffectRequest>{}, time,
                                        config.fixedTimestep ? 1.0f / 60.0f : dt, frame);
                renderer.render(cmd, viewport.w, viewport.h, frameCamera, settings);
                sceneColor = renderer.sceneColor();
            } else {
                heldRows.insert(heldRows.end(), instanceRows.begin(), instanceRows.end());
                renderer.skipFrame(cmd);
                sceneColor = renderer.sceneColor();  // (its last frame: a scene thumbnail taken meanwhile)
            }
        } else {
            bringup.render(cmd, viewport.w, viewport.h, time);
            sceneColor = bringup.sceneColor();
        }

        if (animShown)
            animPreview.render(cmd, animRect);
        // Each shown view into its rect; around them (before the page covers it) a plain backdrop.
        bool cleared = false;
        const auto present = [&](SDL_GPUTexture* texture, const UiRect& r) {
            SDL_GPUBlitInfo blit = {};
            blit.source.texture = texture;
            blit.source.w = r.w;
            blit.source.h = r.h;
            blit.destination.texture = swapchain;
            blit.destination.x = r.x;
            blit.destination.y = r.y;
            blit.destination.w = r.w;
            blit.destination.h = r.h;
            const bool fullWindow = r.w == width && r.h == height;
            blit.load_op = cleared ? SDL_GPU_LOADOP_LOAD : fullWindow ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_CLEAR;
            blit.clear_color = {0.098f, 0.098f, 0.098f, 1.0f};  // the editor's --app (#191919)
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &blit);
            cleared = true;
        };
        FX_GPU_SECTION(cmd, "Present");
        FX_GPU_PASS(cmd, "blit to swapchain");
        if (sceneShown && sceneColor)
            present(sceneColor, viewport);
        if (animShown && animPreview.color())
            present(animPreview.color(), animRect);
        if (!cleared) {  // nothing shown: the backdrop alone
            SDL_GPUColorTargetInfo clear = {};
            clear.texture = swapchain;
            clear.load_op = SDL_GPU_LOADOP_CLEAR;
            clear.store_op = SDL_GPU_STOREOP_STORE;
            clear.clear_color = {0.098f, 0.098f, 0.098f, 1.0f};
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &clear, 1, nullptr));
        }
        if (useEditor && sceneShown) {
            sceneView.update(viewport, viewCamera(float(viewport.w) / float(viewport.h)));
            sceneView.draw(cmd, swapchain);
        }
        ui.draw(cmd, swapchain, width, height);

        // Captures wait until texture streaming has settled, so async read timing cannot change
        // the image (golden-image tests compare captures pixel for pixel).
        // With TAA the history must also converge (blend 0.1: 0.9^64 < 1/255) and the capture lands
        // on the same jitter phase every time (--capture-phase picks another one).
        // (Terrain layer textures still building, or tiles still loading, change the image too.)
        const bool terrainIdle = naive || !hasScene || !terrain.valid() || (!renderer.terrainBuilding() && !terrain.loading());
        streamingIdleFrames = vt.idle() && terrainIdle && (naive || !hasScene || (renderer.shadowsSettled() && renderer.giSettled())) ? streamingIdleFrames + 1 : 0;
        const bool taaActive = !naive && hasScene && settings.debugView == DebugView::Lit &&
                               (settings.post.taa || settings.post.upscaler != Upscaler::Off);
        const uint32_t idleNeeded = taaActive ? std::max(64u, 4 * renderer.jitterPhaseCount()) : 8;
        const bool settled = streamingIdleFrames >= idleNeeded &&
                             (!taaActive || renderer.jitterPhase() == config.capturePhase % renderer.jitterPhaseCount());
        const bool capture = config.capturePath && frame >= config.captureFrame &&
                             (config.captureSequence || naive || !hasScene || settled || frame >= config.captureFrame + 600);
        gpuprof::endFrame(cmd);
        // test.capture (editor scripts): the composited frame, page included.
        const std::string uiCapture = useEditor ? editor.takeCaptureRequest()
                                      : config.launcher ? launcher.takeCaptureRequest() : std::string();
        const std::string previewCapture = usePreview ? preview.captureTarget() : std::string();
        if (!previewCapture.empty()) {
            const uint32_t size = preview.captureSize();
            preview.captured(captureTextureToPng(gpu.get(), cmd, sceneColor, viewport.w, viewport.h, previewCapture.c_str(), false,
                                                 size, size * viewport.h / std::max(viewport.w, 1u)));
            renderer.frameSubmitted(nullptr);  // submitted by the capture, without a fence
            if (preview.done()) {
                running = false;
                if (preview.failures())
                    exitCode = 1;
            }
        } else if (!capture && !uiCapture.empty()) {
            const SDL_GPUTextureFormat f = gpu.swapchainFormat();
            const bool bgra = f == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || f == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
            const uint32_t outW = useEditor ? editor.captureWidth() : 0u;
            const bool ok = useEditor && editor.captureView() == "anim"
                                ? animShown && captureTextureToPng(gpu.get(), cmd, animPreview.color(), animRect.w, animRect.h, uiCapture.c_str(), false)
                                : useEditor && editor.captureViewport() && sceneColor
                                ? captureTextureToPng(gpu.get(), cmd, sceneColor, viewport.w, viewport.h, uiCapture.c_str(), false, outW,
                                                      outW ? std::max(1u, outW * viewport.h / std::max(viewport.w, 1u)) : 0u)
                                : captureTextureToPng(gpu.get(), cmd, swapchain, width, height, uiCapture.c_str(), bgra);
            if (!ok)
                exitCode = 1;
            if (hasScene && !naive)
                renderer.frameSubmitted(nullptr);  // submitted by the capture, without a fence
        } else if (capture && config.captureSequence) {
            std::string path = config.capturePath;
            const size_t dot = path.rfind('.');
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%04llu", static_cast<unsigned long long>(frame));
            path.insert(dot == std::string::npos ? path.size() : dot, suffix);
            if (!captureTextureToPng(gpu.get(), cmd, sceneColor, viewport.w, viewport.h, path.c_str()))
                exitCode = 1;
            if (hasScene && !naive)
                renderer.frameSubmitted(nullptr);  // submitted by the capture, without a fence
            if (frame + 1 >= config.captureFrame + config.captureSequence)
                running = false;
        } else if (capture) {
            if (!settled && !naive && hasScene)
                FX_WARN("capture: frame %llu without settling (texture streaming %s, shadows %s, GI %s, terrain %s)",
                        static_cast<unsigned long long>(frame), vt.idle() ? "idle" : "busy",
                        renderer.shadowsSettled() ? "settled" : "rendering", renderer.giSettled() ? "settled" : "updating",
                        terrainIdle ? "idle" : "building");
            if (!captureTextureToPng(gpu.get(), cmd, sceneColor, viewport.w, viewport.h, config.capturePath))
                exitCode = 1;
            running = false;
        } else {
            SDL_GPUFence* fence = nullptr;
            {
                FX_ZONE_CAT("Submit", Rendering);
                fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            }
            if (!fence) {
                FX_ERROR("submit: %s", SDL_GetError());
                exitCode = 1;
                break;
            }
            if (hasScene && !naive)
                renderer.frameSubmitted(fence);
            else
                SDL_ReleaseGPUFence(gpu.get(), fence);
        }
        }  // (not a server)

        if (frame == 0) {
            boot::mark("first frame submitted");
            if (config.editor && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                FX_WARN("SDL gamepads: %s", SDL_GetError());
        }
        ++frame;
        ++statsFrames;
        freezewatch::heartbeat(frame);
        if (config.testFreezeFrame && frame == config.testFreezeFrame)
            freezewatch::testFreeze(config.testFreezeMs);
        {
            const uint64_t t = SDL_GetTicksNS();
            worstFrameNs = std::max(worstFrameNs, t - lastFrameTicks);
            lastFrameTicks = t;
        }
        FX_FRAME_MARK();
        if (profiler::enabled() && !server) {
            sampleCounters(renderer, vt, physics, usePhysics, animation, ui, skinning, lights, gpu);
            profiler::endFrame();
        }

        const uint64_t now = SDL_GetTicksNS();
        ++editorFrames;
        if (useEditor && now - editorTicks >= 500'000'000ull) {
            const double n = double(editorFrames);
            EditorFrameStats es;
            es.frameMs = double(now - editorTicks) * 1e-6 / n;
            es.scriptMs = double(editorScriptNs) * 1e-6 / n;
            es.physicsMs = double(editorPhysicsNs) * 1e-6 / n;
            es.animationMs = double(editorAnimNs) * 1e-6 / n;
            es.uiMs = double(editorUiNs) * 1e-6 / n;
            const GpuStats& g = renderer.stats();
            es.objects = g.instances;
            es.clusters = g.visible1 + g.visible2 + g.masked1 + g.masked2;
            es.triangles = double(g.triangles1 + g.triangles2);
            es.vsmPages = g.vsmRequested;
            es.vtResident = vt.stats().resident;
            es.vtCapacity = vt.stats().capacity;
            es.bodies = usePhysics ? physics.stats().bodies : 0;
            es.characters = animation.characterCount();
            const UiStats us = ui.stats();
            es.uiFrames = us.frames;
            es.uiCopyMs = us.copyMs;
            editor.report(es);
            editorTicks = now;
            editorFrames = 0;
            editorScriptNs = editorPhysicsNs = editorAnimNs = editorUiNs = 0;
        } else if (!useEditor) {
            editorFrames = 0;
            editorScriptNs = editorPhysicsNs = editorAnimNs = editorUiNs = 0;
        }
        if (now - statsTicks >= 2'000'000'000ull) {
            const double ms = double(now - statsTicks) * 1e-6 / statsFrames;
            FX_INFO("frame: %.3f ms (%.0f fps, slowest %.1f ms) %ux%u", ms, 1000.0 / ms, double(worstFrameNs) * 1e-6, width, height);
            worstFrameNs = 0;
            if (hasScene && !naive && !server)
                logStats(window, renderer, vsm, vt, settings, ms);
            if (terrain.valid())
                FX_INFO("terrain: %u of %u tiles loaded", terrain.residentTiles(), terrain.tilesPerSide() * terrain.tilesPerSide());
            if (config.profile)
                logProfile(profileLogged);
            if (usePhysics) {
                const PhysicsStats p = physics.stats();
                FX_INFO("physics: %u bodies (%u awake), %u static shapes, step %.2f ms, %.2f ms/frame%s", p.bodies,
                        p.awake, p.staticShapes, double(p.stepMs), double(physicsNs) * 1e-6 / statsFrames,
                        physicsPaused ? " | PAUSED" : "");
                physicsNs = 0;
            }
            if (audio.active()) {
                // Mixing time as a share of the audio it made (100%: the device would starve).
                const AudioMixerStats a = audio.stats();
                const double mixed = double(a.blocks - audioLogged.blocks) * double(kAudioFrameSize) / double(kAudioSampleRate);
                const AudioSceneStats as = audio.sceneStats();
                FX_INFO("audio: %u voices (%u HRTF), mixing %.2f%% of real time; scene %u meshes, %u placed, %u terrain tiles, "
                        "%llu triangles, %u sources simulated in %.3f ms on their thread (%.3f ms on the main thread); reflections for %u (+ the "
                        "listener's reverb), %.2f ms a run, %u runs",
                        a.voices, a.spatialized, mixed > 0.0 ? double(a.mixNs - audioLogged.mixNs) * 1e-9 / mixed * 100.0 : 0.0,
                        as.meshes, as.instances, as.terrainTiles, (unsigned long long)as.triangles, as.sources, double(as.simulateMs),
                        double(as.syncMs), as.reflecting, double(as.reflectionsMs), as.reflectionRuns);
                if (const PhysicsAudioStats& ps = audio.physicsStats(); ps.impacts || ps.scrapes || ps.rolls)
                    FX_INFO("audio: physics sounds: %llu impacts, %llu scrapes, %llu rolls; %u sliding or rolling now",
                            (unsigned long long)ps.impacts, (unsigned long long)ps.scrapes, (unsigned long long)ps.rolls, ps.loops);
                if (const WaterAudioStats& ws = audio.waterStats(); ws.splashes || ws.sloshing || ws.underwater > 0.0f)
                    FX_INFO("audio: water: %llu splashes; %u sloshing now%s", (unsigned long long)ws.splashes, ws.sloshing,
                            ws.underwater > 0.0f ? "; under water" : "");
                if (float rt[3]; audio.listenerReverb(rt))
                    FX_INFO("audio: the listener's reverb: RT60 %.2f / %.2f / %.2f s (low / mid / high)", double(rt[0]), double(rt[1]),
                            double(rt[2]));
                audioLogged = a;
            }
            if (animation.characterCount() > 0)
                FX_INFO("animation: %u characters, %.3f ms (last update)", animation.characterCount(),
                        double(animation.lastUpdateMs()));
            if (useScripts) {
                FX_INFO("scripts: %s, %.3f ms/frame", scripts.jit() ? "JIT" : "NativeAOT",
                        double(scriptNs) * 1e-6 / statsFrames);
                scriptNs = 0;
            }
            if (ui.active()) {
                const UiStats us = ui.stats();
                FX_INFO("ui: %llu page frames (%llu shared-texture; %.3f ms per copy, max %.3f), %llu messages in, %llu out%s",
                        (unsigned long long)us.frames, (unsigned long long)us.accelerated, us.copyMs, us.copyMaxMs,
                        (unsigned long long)us.messagesIn,
                        (unsigned long long)us.messagesOut, useEditor && !editor.playing() ? " | EDITING" : "");
            }
            statsTicks = now;
            statsFrames = 0;
        }
        // --audio-bake: once the audio scene is in step with the scene (a few frames), bake it; exit
        // when done.
        if (config.audioBake) {
            if (frame == 5) {
                std::string error;
                if (!audio.startBake(error)) {
                    FX_ERROR("audio: no bake: %s", error.c_str());
                    exitCode = 1;
                    running = false;
                }
            } else if (frame > 5 && !audio.baking()) {
                const AudioBakeStatus st = audio.bakeStatus();
                if (st.state != "baked")
                    exitCode = 1;
                running = false;
            } else if (const AudioBakeStatus st = audio.bakeStatus(); st.state == "baking") {
                static std::string lastStage;  // (the stage, and each tenth of it, logged once)
                const std::string step = st.stage + " " + std::to_string(int(st.fraction * 10.0f) * 10) + "%";
                if (step != lastStage) {
                    FX_INFO("audio: baking: %s%s", step.c_str(), st.probes ? (" of " + std::to_string(st.probes) + " probes").c_str() : "");
                    lastStage = step;
                }
            }
        }
        if (config.reloadSceneAt && frame % config.reloadSceneAt == 0 && p.reloads < config.reloadSceneCount && config.scenePath &&
            !config.launcher) {
            p.nextScene = config.scenePath;  // (test: the scene torn down and loaded again, in process)
            ++p.reloads;
            running = false;
        }
        if (config.exitAfterFrames && frame >= config.exitAfterFrames) {
            p.nextScene.clear();
            running = false;
            if (config.editorScript || config.launcherScript)
                exitCode = 1;  // the script never finished (no UI, or it did not connect)
        }
        if (useEditor && editor.exitRequested())
            running = false, p.nextScene.clear();
        if (useEditor && editor.testFailed())
            exitCode = 1;
        if (config.launcher && launcher.exitRequested())
            running = false;
        if (config.launcher && launcher.testFailed())
            exitCode = 1;
    }

    if (config.netVerify && p.nextScene.empty()) {  // (--net-verify: the session's checks, before the game ends)
        const int64_t failures = scripts.simControl(4, 0), checks = scripts.simControl(5, 0);
        if (failures) {
            FX_ERROR("net-verify: failed (%lld checks)", (long long)checks);
            exitCode = 1;
        } else {
            FX_INFO("net-verify: %lld %s, all as the host's", (long long)checks,
                    config.netHost >= 0 ? "replicated states checked" : "snapshots received");
        }
    }
    if (config.simRollback && p.nextScene.empty()) {  // (before the game ends with the last scene)
        const int64_t checked = scripts.simControl(3, 0), mismatches = scripts.simControl(1, 0);
        const uint64_t steps = physics.rollbackChecks(), stepMismatches = physics.rollbackMismatches();
        if (mismatches || !checked || stepMismatches) {
            FX_ERROR("sim-rollback: %lld of %lld ticks simulated again came out differently (physics: %llu of %llu steps)",
                     (long long)mismatches, (long long)checked, (unsigned long long)stepMismatches, (unsigned long long)steps);
            exitCode = 1;
        } else {
            FX_INFO("sim-rollback: %lld ticks simulated again (%u back each frame): identical; physics: %llu steps, identical "
                    "(a kept state %zu KB, kept in %.3f ms, restored in %.3f ms)",
                    (long long)checked, config.simRollback, (unsigned long long)steps, physics.keptBytes() / 1024, physics.keepMs(),
                    physics.restoreMs());
        }
    }
    // The scene's teardown (the process's is runSession's): the last scene's also ends the game and
    // remembers the editor's layout; one followed by another stops its game and lets go of it.
    p.switchNs = SDL_GetTicksNS();
    if (gpu.get() || server) {
        if (gpu.get())
            SDL_WaitForGPUIdle(gpu.get());
        const bool last = p.nextScene.empty();
        if (useEditor && !last)
            editor.detachScene();
        // Remember the editor's settings, layout and window for next time.
        if (last && useEditor && config.editorPrefs) {
            editor.persist();
            Json& wp = prefs.section("window");
            wp.set("x", Json::makeNumber(normalRect[0]));
            wp.set("y", Json::makeNumber(normalRect[1]));
            wp.set("width", Json::makeNumber(normalRect[2]));
            wp.set("height", Json::makeNumber(normalRect[3]));
            wp.set("maximized", Json::makeBool((SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0));
            prefs.touch();
            prefs.save(true);
        }
        sceneView.shutdown();
        naiveRenderer.shutdown();
        if (last)
            scripts.shutdown();
        else if (useScripts)
            scripts.unloadScene();  // (the game goes on in the next scene)
        scripts.detachScene();
        levelMeshes.detach();  // (its query, before the world goes)
        renderer.shutdown();
        physics.shutdown();
        vt.shutdown();
        lights.shutdown();
        vsm.shutdown();
        animPreview.shutdown();
        skinning.shutdown();
        animation.shutdown();
        bringup.shutdown();
        audio.detachScene(hasScene ? &scene : nullptr);
        pak.release(gpu.get());
    }
    return 0;
}

int runSession(const EngineConfig& config)
{
    boot::mark("main");
    // Boot is ordered so slow steps overlap: the scene file streams in and the GPU device
    // is created on a worker while the window and ECS are set up on the main thread.
    auto firstPak = std::make_unique<ScenePak>();
    const bool hasScene = config.scenePath && firstPak->beginRead(config.scenePath);
    if (config.scenePath && !hasScene)
        return 1;

    SDL_SetAppMetadata("Falcon", "0.1", "gg.falcon.engine");
    if (!SDL_Init(config.server ? SDL_INIT_EVENTS : SDL_INIT_VIDEO)) {  // (a server: no video)
        FX_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    boot::mark("sdl video");
    // The scripts' gamepads (a game runs without them). Their start takes ~80 ms: the editor's
    // waits for its first frame (pads arrive as events either way); the launcher has none.
    if (!config.editor && !config.launcher && !config.server && !SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        FX_WARN("SDL gamepads: %s", SDL_GetError());
    boot::mark("sdl init");
    // Profiler: zones in the editor (its Profiler tab) and with --profile; GPU marks while the
    // tab is open (the bridge turns them on) or with --profile.
    profiler::nameThread("Main");
    profiler::setEnabled(config.editor || config.profile);

    Process p;
    GpuDevice& gpu = p.gpu;
    ScriptHost& scripts = p.scripts;
    EditorPrefs& prefs = p.prefs;
    // After SDL_Init: the Vulkan backend loads its loader through the video subsystem.
    if (!config.server)
        gpu.beginCreate(config.gpu);
    // Scripts: the .NET runtime (or the NativeAOT DLL) loads on its own thread meanwhile. The
    // editor loads a project's game (its last build) the same way; Play starts it.
    std::string projectGame;
    if (config.editor && config.projectPath && !config.scriptPath && gamescripts::exists(config.projectPath)) {
        std::error_code ec;
        if (std::filesystem::exists(gamescripts::dllPath(config.projectPath), ec) && gamescripts::builtForThisEngine(config.projectPath))
            projectGame = gamescripts::dllPath(config.projectPath);  // (one for another engine: the editor builds it again at once)
    } else if (!config.editor && !config.launcher && config.projectPath && !config.scriptPath && !config.audioBake) {
        // The game on its own: published (File > Build Game; a shipped game has only that), else
        // the editor's last build. (Not for --audio-bake: a tool, the scene as saved.)
        std::error_code ec;
        if (std::filesystem::exists(gamescripts::aotDllPath(config.projectPath), ec))
            projectGame = gamescripts::aotDllPath(config.projectPath);
        else if (std::filesystem::exists(gamescripts::dllPath(config.projectPath), ec) && gamescripts::builtForThisEngine(config.projectPath))
            projectGame = gamescripts::dllPath(config.projectPath);
    }
    const char* scriptPath = config.scriptPath ? config.scriptPath : projectGame.empty() ? nullptr : projectGame.c_str();
    bool& useScripts = p.useScripts;
    useScripts = hasScene && scriptPath && !config.naiveRenderer;
    if (useScripts && !scripts.beginLoad(scriptPath, config.hotReload && !config.capturePath)) {
        if (!config.editor)
            return 1;
        useScripts = false;
    }

    // Created hidden and shown just before the first acquire (Vulkan cannot acquire a
    // swapchain image for a hidden window), so it appears with its first frame.
    // The editor: borderless (the page draws the title bar), placed where it was last time.
    // (The launcher too: its page draws the title bar.)
    const bool editorChrome = (config.editor || config.launcher) && UiLayer::available();
    if (config.editor && config.editorPrefs)
        prefs.load();
    const Json& windowPrefs = prefs.section("window");
    const int windowW = config.launcher ? 1100 : config.editor ? int(windowPrefs.getNumber("width", config.width)) : config.width;
    const int windowH = config.launcher ? 700 : config.editor ? int(windowPrefs.getNumber("height", config.height)) : config.height;
    SDL_Window* window = config.server ? nullptr  // (a server: none)
                                       : SDL_CreateWindow(config.title, std::max(windowW, 640), std::max(windowH, 400),
                                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                                                              (editorChrome ? SDL_WINDOW_BORDERLESS : 0));
    p.window = window;
    p.editorChrome = editorChrome;
    if (!window && !config.server) {
        FX_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        gpu.shutdown();
        return 1;
    }
    if (config.editor && windowPrefs.get("x"))
        SDL_SetWindowPosition(window, int(windowPrefs.getNumber("x")), int(windowPrefs.getNumber("y")));
    if (config.editor && windowPrefs.getBool("maximized"))
        SDL_MaximizeWindow(window);
    // Last restored (not maximized) placement, saved at exit.
    int (&normalRect)[4] = p.normalRect;
    normalRect[2] = std::max(windowW, 640);
    normalRect[3] = std::max(windowH, 400);
    if (window)
        SDL_GetWindowPosition(window, &normalRect[0], &normalRect[1]);
    boot::mark("window created");

    // HTML UI: started below, once the window is there.
    std::string& uiBundle = p.uiBundle;
    uiBundle = config.uiBundle ? config.uiBundle : "";
    const std::string exeDir = SDL_GetBasePath() ? SDL_GetBasePath() : "";
    if (config.editor && uiBundle.empty())
        uiBundle = exeDir + "falcon-ui/editor";
    if (config.launcher && uiBundle.empty())
        uiBundle = exeDir + "falcon-ui/launcher";
    if (!config.editor && !config.launcher && !config.server && config.projectPath && uiBundle.empty()) {  // a game: the project's UI/Game
        std::error_code ec;
        const std::filesystem::path page = std::filesystem::path(config.projectPath) / "UI" / "Game";
        if (std::filesystem::exists(page / "index.html", ec))
            uiBundle = page.generic_string();
    }
    // A bare name picks one of the bundles shipped next to the executable (--ui hud).
    SDL_PathInfo bundleInfo = {};
    if (!uiBundle.empty() && !SDL_GetPathInfo(uiBundle.c_str(), &bundleInfo) &&
        SDL_GetPathInfo((exeDir + "falcon-ui/" + uiBundle).c_str(), &bundleInfo))
        uiBundle = exeDir + "falcon-ui/" + uiBundle;
    FrameSettings& settings = p.settings;
    scripts.setRenderSettings(&settings);  // (scripts' Render.Set: put back at the editor's Stop)
    settings.lodPixels = config.lodPixels;
    settings.cullPixels = config.cullPixels;
    settings.occlusion = config.occlusion;
    settings.coneCulling = config.coneCulling;
    settings.forceRetest = config.forceRetest;
    settings.alphaMask = config.alphaMask;
    settings.shadowCache = config.shadowCache;
    settings.shadowBudgetMs = config.shadowBudgetMs;
    settings.reflections = config.reflections;
    settings.terrainPixels = config.terrainPixels;
    settings.detailDistance = config.detailDistance;
    settings.water.enabled = config.water;
    settings.post.taa = config.taa;
    settings.post.fxaa = config.fxaa;
    settings.post.msaa = config.msaa;
    settings.post.upscaler = Upscaler(SDL_min(config.upscaler, 3u));
    settings.post.renderScale = config.renderScale;
    settings.post.sharpness = config.sharpness;
    settings.gi.enabled = config.gi;
    settings.post.aoHalfRes = config.halfResAmbient;
    settings.post.ao = AoMode(SDL_min(config.aoMode, 2u));
    settings.post.bloomIntensity = config.bloomIntensity;
    settings.post.exposureCompensation = config.exposureCompensation;
    settings.post.lookPower = config.lookPower;
    settings.post.lookSaturation = config.lookSaturation;
    settings.post.manualExposure = config.manualExposure;
    if (config.debugMode >= 0)
        settings.debugView = DebugView(SDL_min(uint32_t(config.debugMode), uint32_t(DebugView::Count) - 1));
    // Hidden runs (tests) get a browser profile of their own: Chromium allows one process per
    // profile, and cef-lite keeps it under %LOCALAPPDATA% per UI folder, so a test would hand its
    // window to an editor the user has open (and get no UI). The profile goes at exit.
    std::filesystem::path& privateProfile = p.privateProfile;
    if (config.hidden) {
        std::error_code ec;
        privateProfile = std::filesystem::temp_directory_path(ec) / ("falcon-hidden-" + std::to_string(currentProcessId()));
        std::filesystem::create_directories(privateProfile, ec);
        if (!ec)
            platform::setEnv("LOCALAPPDATA", platform::utf8(privateProfile));
    }
    // The page's Chromium starts now (libcef, its processes, the page: ~0.35 s) while the GPU
    // device is made on its worker; runScene pumps it while it waits, then gives it the device.
    if (!uiBundle.empty() && p.ui.init(nullptr, window, uiBundle.c_str()))
        boot::mark("ui created");
    p.startTicks = p.lastTicks = p.statsTicks = p.lastFrameTicks = p.editorTicks = SDL_GetTicksNS();
    p.viewport = {0, 0, uint32_t(config.width), uint32_t(config.height)};
    if (config.simReplay && !p.sim.startReplay(config.simReplay))
        return 1;
    if (config.simRecord)
        p.sim.startRecord(config.simRecord);
    if (config.simDemo >= 0)  // (in a network session: real frame times, its input only)
        p.sim.setDemo(uint32_t(config.simDemo), config.netHost >= 0 || config.netConnect);
    if (p.sim.active())
        p.scripts.setInputHook([&p](ScriptHost::FrameInput& in) { p.sim.input(in); });
    // A dedicated server's console: lines typed on its standard input, read on a thread of their
    // own (left running at exit: it blocks on the read), taken by the game each frame.
    struct ServerConsole {
        std::mutex lock;
        std::deque<std::string> lines;
    };
    static ServerConsole console;
    if (config.server) {
        std::thread([] {
            std::string line;
            while (std::getline(std::cin, line)) {
                std::lock_guard g(console.lock);
                console.lines.push_back(line);
            }
        }).detach();
        p.scripts.setServerConsole([](std::string& out) {
            std::lock_guard g(console.lock);
            if (console.lines.empty())
                return false;
            out = std::move(console.lines.front());
            console.lines.pop_front();
            return true;
        });
    }
    {
        // The game's network: what the command line asks (Falcon.Network reads it at the start), the
        // conditions around its transports, and client processes of this game for --net-clients.
        net::Conditions conditions;
        if (config.netSim)
            net::parseConditions(config.netSim, conditions);
        JsonWriter w;
        w.beginObject();
        if (config.netHost >= 0)
            w.value("host", int64_t(config.netHost));
        if (!config.netHostAddress.empty())
            w.value("hostAddress", std::string_view(config.netHostAddress));
        if (config.netConnect)
            w.value("connect", std::string_view(config.netConnect));
        if (config.netClients)
            w.value("clients", int64_t(config.netClients));
        if (config.netVerify)
            w.value("verify", true);
        if (config.netChild)
            w.value("child", true);
        if (config.maxPlayers)
            w.value("maxPlayers", int64_t(config.maxPlayers));
        if (config.netBots)
            w.value("bots", int64_t(config.netBots));
        if (config.netTransport)
            w.value("transport", std::string_view(config.netTransport));
        if (config.netMode)
            w.value("mode", std::string_view(config.netMode));
        if (config.password)
            w.value("password", std::string_view(config.password));
        if (!config.netProject.empty())
            w.raw("project", config.netProject);
        w.endObject();
        p.netConditions = conditions;
        p.netLaunch = w.str();
        p.netLauncher = [&p, &config](int32_t count, const std::string& address) {
            // This run's arguments without the host's own, connecting instead, always hidden.
            std::vector<std::string> args;
            for (size_t i = 0; i < config.args.size(); ++i) {
                const std::string& a = config.args[i];
                if (a == "--host") {
                    if (i + 1 < config.args.size() && isHostValue(config.args[i + 1].c_str()))
                        ++i;
                    continue;
                }
                if (a == "--net-clients" || a == "--sim-record" || a == "--sim-replay" || a == "--capture") {
                    ++i;
                    continue;
                }
                if (a == "--server")
                    continue;  // (clients show the game)
                args.push_back(a);
            }
            if (std::find(args.begin(), args.end(), "--hidden") == args.end())
                args.push_back("--hidden");
            // They end with the host (their --frames run on alone after it, and a busy machine's
            // slow frames could outlast the host's wait for them).
            args.push_back("--net-child");
            args.push_back("--connect");
            args.push_back(address);
            for (int32_t i = 0; i < count; ++i)
                if (void* child = startFalconTracked(args))
                    p.netClients.push_back(child);
                else
                    FX_ERROR("net: client process %d did not start", int(i));
            FX_INFO("net: started %d client processes", int(count));
        };
        p.scripts.setNetwork(p.netConditions, p.netLaunch, p.netLauncher);
    }

    // Scenes, one after another (--reload-scene-at, Scene.Load): each torn down before the next
    // is read; the window, GPU, UI, scripts' runtime and audio device stay.
    EngineConfig sceneConfig = config;
    std::string scenePath = config.scenePath ? config.scenePath : "";
    std::unique_ptr<ScenePak> pak = std::move(firstPak);
    bool sceneRead = hasScene;
    SceneMemory firstMemory;  // between scenes (--reload-scene-at with a count: nothing may grow)
    for (bool first = true;; first = false) {
        if (!first && p.preloaded && p.loader == nullptr && p.preloadedPath == p.nextScene) {
            // Scene.LoadAsync's: cooked and read already.
            scenePath = std::exchange(p.nextScene, {});
            sceneConfig.scenePath = scenePath.c_str();
            pak = std::move(p.preloaded);
            pak->setPath(sceneConfig.scenePath);
            sceneRead = true;
        } else if (!first) {
            p.preloaded.reset();
            if (p.loader) {
                p.loader->cancel = true;
                if (p.loader->cook.valid())
                    p.loader->cook.wait();
                p.loader.reset();
            }
            scenePath = std::exchange(p.nextScene, {});
            sceneConfig.scenePath = scenePath.c_str();
            // A project's scene cooked first when it needs it (a shipped game's always is).
            if (sceneConfig.projectPath) {
                Project project;
                std::string error;
                const std::string relative = std::filesystem::path(scenePath).lexically_relative(sceneConfig.projectPath).generic_string();
                // (cooked when it needs it; a cook waiting in Cache/Cook, made while the scene was open,
                // moved in now that nothing has its files)
                if (loadProject(sceneConfig.projectPath, project, error) && !cookProjectScene(project, relative, error))
                    FX_ERROR("scene: %s did not cook: %s", relative.c_str(), error.c_str());
            }
            pak = std::make_unique<ScenePak>();
            if (!(sceneRead = pak->beginRead(sceneConfig.scenePath))) {
                p.exitCode = 1;
                break;
            }
        }
        p.scripts.setSceneLoading(-1.0f, "");
        if (runScene(p, sceneConfig, *pak, sceneRead, first) != 0)
            return 1;
        pak.reset();
        if (p.nextScene.empty())
            break;
        const SceneMemory m = sceneMemory();
        FX_INFO("scene: next %s: GPU %.1f MB in %u resources, RAM %.1f MB between scenes", p.nextScene.c_str(),
                double(m.gpuBytes) / 1048576.0, m.gpuResources, double(m.ramBytes) / 1048576.0);
        if (sceneConfig.reloadSceneCount <= 1 || p.reloads == 0)
            continue;  // (only --reload-scene-at's loads of one scene are compared)
        // (RAM swings by tens of MB from one load to the next, as freed memory goes back at its own
        // pace: the last load is compared; the GPU's accounting is exact, each time.)
        if (p.reloads == 1)
            firstMemory = m;
        else if (m.gpuBytes != firstMemory.gpuBytes || m.gpuResources != firstMemory.gpuResources ||
                 (p.reloads == sceneConfig.reloadSceneCount && m.ramBytes > firstMemory.ramBytes + (64ull << 20))) {
            FX_ERROR("scene: memory grew from one load to the next (GPU %llu -> %llu bytes, %u -> %u resources; RAM %.1f -> %.1f MB)",
                     (unsigned long long)firstMemory.gpuBytes, (unsigned long long)m.gpuBytes, firstMemory.gpuResources,
                     m.gpuResources, double(firstMemory.ramBytes) / 1048576.0, double(m.ramBytes) / 1048576.0);
            p.exitCode = 1;
        }
    }

    if (p.loader) {  // (a load still going: its cook stopped)
        p.loader->cancel = true;
        if (p.loader->cook.valid())
            p.loader->cook.wait();
    }
    if (gpu.get() || config.server) {
        if (gpu.get()) {
            SDL_WaitForGPUIdle(gpu.get());
            gpuprof::shutdown();
            if (p.hiddenTarget)
                gpumem::releaseTexture(gpu.get(), p.hiddenTarget);
        }
        scripts.shutdown();  // (the last scene's teardown did; again if a scene failed to read)
        p.ui.shutdown();
        p.meshes.shutdown();
    }
    AudioSystem& audio = p.audio;
    audio.shutdown();
    gpu.shutdown();
    jobs::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!privateProfile.empty()) {  // best effort: a renderer process may still hold a file
        std::error_code ec;
        std::filesystem::remove_all(privateProfile, ec);
    }
    if (p.sim.active() && !p.sim.finish())
        p.exitCode = 1;  // (a replay that came out differently)
    // --net-clients: the host's run includes its clients' (they end when it does, or time out).
    for (size_t i = 0; i < p.netClients.size(); ++i) {
        int code = -1;
        const uint64_t until = SDL_GetTicksNS() + 30'000'000'000ull;
        while (!falconFinished(p.netClients[i], code) && SDL_GetTicksNS() < until)
            SDL_Delay(20);
        if (code == -1 && SDL_GetTicksNS() >= until) {
            stopFalcon(p.netClients[i]);
            FX_ERROR("net: client process %zu did not end", i);
            p.exitCode = 1;
        } else if (code != 0) {
            FX_ERROR("net: client process %zu failed (exit code %d)", i, code);
            p.exitCode = 1;
        } else {
            FX_INFO("net: client process %zu ended well", i);
        }
    }
    return p.exitCode;
}

}  // namespace

int runEngine(const EngineConfig& config)
{
    dmath::setupFloatingPoint();  // (the simulation's floats: the same on every PC)
    // Freeze reports: the main thread watched from here on (from its first frame).
    freezewatch::keepLog();
    freezewatch::start(config.freezeSeconds, config.freezeReports ? config.freezeReports : "");
    struct FreezeWatchLifetime {
        ~FreezeWatchLifetime() { freezewatch::stop(); }
    } freezeWatchLifetime;
    // The project's settings, for Steam's App ID (it's read again below; reading it is quick).
    Project early;
    if (config.projectPath && !config.launcher) {
        std::string error;
        loadProject(config.projectPath, early, error);
    }
    // Steam, before the GPU (its overlay hooks the device and swap chain as they're made): for
    // --steam's App ID, else the project's.
    if (const uint32_t appId = config.steamAppId == UINT32_MAX ? (early.steamAppId ? early.steamAppId : 480u)
                                                                : config.steamAppId ? config.steamAppId : early.steamAppId) {
        // A shipped game (its project beside the executable) started outside Steam: Steam starts it
        // again and this process ends (dev runs and servers aren't asked).
        if (!config.server && config.steamAppId == 0 && early.steamRestartIfNecessary && !gamescripts::shippedProject().empty() &&
            steam::restartThroughSteam(appId))
            return 0;
        if (config.server) {  // (a dedicated server: Steam's game server, queries on the next port)
            const uint16_t port = uint16_t(config.netHost > 0 ? config.netHost : 27015);
            steam::startServer(appId, port, uint16_t(port + 1), "1.0.0.0");
        } else {
            steam::start(appId);
        }
    }
    // First: the GPU driver's start, on a thread (GpuDevice::warmUp). (A server has no GPU.)
    if (!config.server)
        GpuDevice::warmUp(config.gpu);
#if defined(TRACY_ENABLE)
    // Tracy's lifetime is manual (FalconDeps.cmake): its threads, socket and crash handler start
    // here, in the engine only, not in the cooker or in Chromium's helpers (also falcon.exe).
    // Its start sleeps 200 ms (timer calibration), so it runs on a thread; zones wait for
    // profiler::tracyUp (core/profile.h). No zone may run outside this window.
    struct TracyLifetime {
        std::thread start{[] {
            tracy::StartupProfiler();
            profiler::tracyUp.store(true, std::memory_order_release);
        }};
        ~TracyLifetime()
        {
            start.join();
            profiler::tracyUp.store(false, std::memory_order_release);
            tracy::ShutdownProfiler();
        }
    } tracyLifetime;
#endif
    // A project: its startup scene (or --scene, relative to it), cooked empty first when missing.
    EngineConfig c = config;
    Project project;
    std::string scene;
    if (c.projectPath && !c.launcher) {
        std::string error;
        if (!loadProject(c.projectPath, project, error)) {
            FX_ERROR("%s", error.c_str());
            return 1;
        }
        scene = (std::filesystem::path(project.dir) / (c.scenePath ? c.scenePath : project.startupScene.c_str()))
                    .lexically_normal()
                    .generic_string();
        // The editor: the prototype textures put into the project once (blockouts' default
        // material), before the cook so it takes them in.
        if (c.editor && !project.prototypeInstalled) {
            const std::string content = std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "") + "content/prototype";
            int made = 0;
            if (level::installPrototype(content, project.dir, error, &made)) {
                project.prototypeInstalled = true;
                if (!saveProject(project, error))
                    FX_WARN("%s", error.c_str());
                FX_INFO("project: prototype textures put into %s (%d materials)", level::kPrototypeFolder, made);
            } else {
                FX_WARN("project: %s", error.c_str());
            }
            error.clear();
        }
        // Cooked with the project's models (skipped when it already is: the models' stamp), with
        // a progress window when it isn't.
        const std::string relative = std::filesystem::path(scene).lexically_relative(project.dir).generic_string();
        if (!cookSceneWithProgress(project, relative, c.hidden, error)) {
            FX_ERROR("%s", error.c_str());
            return 1;
        }
        c.scenePath = scene.c_str();
        c.projectPath = project.dir.c_str();
        c.netProject = networkJson(project.network);
        if (!c.lodPixelsSet)
            c.lodPixels = project.lodPixels;
        if (!c.cullPixelsSet)
            c.cullPixels = project.cullPixels;
        if (!c.terrainPixelsSet)
            c.terrainPixels = project.terrainPixels;
        if (!c.detailDistanceSet)
            c.detailDistance = project.detailDistance;
    }
    // Previews: the jobs file names the scene; hidden, square, fixed time, nothing simulated.
    if (c.previewPath) {
        PreviewSession plan;
        if (!plan.load(c.previewPath))
            return 1;
        scene = plan.scene();
        c.scenePath = scene.c_str();
        c.sceneEdits = plan.edits();
        c.terrain = plan.terrain();
        c.hidden = true;
        c.fixedTimestep = true;
        c.width = c.height = int(PreviewSession::kRenderSize);
        c.physics = false;
        c.editor = c.launcher = false;
        c.projectPath = nullptr;
    }
    const int code = runSession(c);  // (Tracy shuts down after it: tracyLifetime)
    steam::stop();
    return code;
}

}  // namespace fx
