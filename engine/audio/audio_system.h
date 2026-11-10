#pragma once

// Audio: the mixer, its device, Steam Audio (HRTF, distance, air absorption; occlusion,
// reflections and the rest: docs/audio.md) and the sounds.
//
// Sounds are .fsound assets (assets/sound_asset.h: clips with variation, distance, bus) or clips
// (.wav / .ogg), by project path or sound id. The scene's AudioSource entities play while the game
// runs (the editor's Play to Stop; a game from its start): those marked "play on start" start with
// it, follow their entities, and stop with it. The listener is the first enabled AudioListener
// entity then, else the camera the view renders from. --no-audio leaves all of it off.

#include "assets/sound_asset.h"
#include "audio/audio_bake.h"
#include "audio/audio_device.h"
#include "audio/audio_mixer.h"
#include "audio/audio_scene.h"
#include "audio/audio_settings.h"
#include "audio/clip_cache.h"
#include "audio/physics_audio.h"
#include "audio/steam_audio.h"
#include "audio/water_audio.h"

#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

struct CameraState;
struct AudioSource;
class MaterialLibrary;
class Scene;
class ScenePak;
class Terrain;
class WaterSystem;
struct ScatterCellBodies;

enum class SurfaceKind : uint8_t { Impact, Scrape, Roll };

// The scene's baked acoustics (audio_bake.h), as the editor shows them.
struct AudioBakeStatus {
    // none (never baked), baked, stale (the geometry changed since), baking, unavailable (no Steam
    // Audio, or no scene)
    std::string state = "unavailable";
    std::string stage;  // while baking: scene, probes, reverb, paths, saving
    float fraction = 0.0f;
    uint32_t probes = 0;  // while baking: placed so far
    AudioBakeInfo info;   // the bake in use
    std::string staleWhy, error;  // what changed; the last bake's failure ("Cancelled")
};

struct AudioConfig {
    bool nullDevice = false;             // no playback device: mixed from the frame clock (tests)
    const char* capturePath = nullptr;   // also write the mix to this WAV
    uint32_t maxVoices = 128;
    uint32_t spatialVoices = 32;         // voices spatialized by Steam Audio at once (more: panned)
    enum class Spatializer { Auto, Steam, Pan } spatializer = Spatializer::Auto;  // Steam: required
    bool synchronousReflections = false;  // reflections in step with the frame clock (fixed-time tests)
};

// How a sound plays: where, and the AudioSource's volume and pitch (multiplying the sound's).
// settings: the source's own (loop, 3D, distances, rolloff, air, bus) instead of the .fsound's
// (a clip without them plays 2D, once).
struct SoundPlay {
    float3 position = {0.0f, 0.0f, 0.0f};
    bool listenerRelative = false;
    float volume = 1.0f, pitch = 1.0f;
    const SoundParams* settings = nullptr;
    bool force2D = false;  // previews of an asset: heard as recorded
    bool loop = false;     // loops (as well as a sound that does)
    bool vary = true;      // the sound's (and the project's) variation; false: exactly as made (previews)
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    bool init(const AudioConfig& config);  // false: --audio-spatializer steam without Steam Audio
    void shutdown();
    bool active() const { return active_; }

    void setProjectDir(const std::string& dir) { projectDir_ = dir; }
    // The scene loaded: its geometry is what occludes (audio_scene.h), and its baked acoustics
    // (<scenePath>.audio/; "": none) are used where they reach. Scatter cells as physics gets them.
    void attachScene(ScenePak* pak, Scene* scene, MaterialLibrary* materials, const Terrain* terrain, const std::string& scenePath = "");

    // Baking (audio_bake.h): its settings (kept in <scene>.audio/bake.json), a bake of the scene as
    // it is now on a thread of its own (false: one runs already, or nothing to bake: error), and
    // the baked data used as soon as it is done. Clearing removes the baked files.
    const AudioBakeSettings& bakeSettings() const { return bakeSettings_; }
    bool setBakeSettings(const AudioBakeSettings& settings);  // saved
    bool startBake(std::string& error);
    void cancelBake() { baker_.cancel(); }
    bool clearBake();
    bool baking() const { return baker_.busy(); }
    AudioBakeStatus bakeStatus() const;
    const AudioBakedData* bakedData() const { return scene_.baked(); }
    // An entity's playing (or previewed) source's baked paths (the EQ they let through per band);
    // false: none.
    bool pathingOf(uint64_t entity, float eq[3]) const;
    // Physics sounds from its contacts while the game runs (physics_audio.h); null: none.
    void setPhysics(const PhysicsWorld* physics) { physics_ = physics; }
    // The scene is going (another may attach after): its sources stopped, its geometry and bake let
    // go, nothing of it kept (physics and water too).
    void detachScene(Scene* scene);
    // Water sounds while the game runs (water_audio.h): splashes and slosh (from physics), each
    // the listener under water. null: none.
    void setWater(const WaterSystem* water) { water_ = water; }
    const WaterAudioStats& waterStats() const { return waterAudio_.stats(); }
    // A clip made here (synthesized), kept by key: made once.
    const AudioClip* synthesized(const std::string& key, const std::function<AudioClip()>& make);
    // A sound or clip by path (a .fsound as made; a clip with p), else `fallback` with p (null or
    // no fallback: silent).
    AudioVoice playOr(const std::string& path, const AudioClip* fallback, const SoundParams& p, const SoundPlay& how);
    // The world's buses muffled as from under water (0 .. 1; the mixer's low-pass).
    void setUnderwater(float amount) { mixer_->setUnderwater(amount); }
    // A surface's impact (once) or scrape / roll (a loop): its sound, else synthesized; at a point.
    AudioVoice playSurface(const SurfaceSound& surface, SurfaceKind kind, float3 position, float gain, float pitch, uint32_t seed);
    // A voice placed in the world: its level, pitch and place now.
    void setVoice(AudioVoice voice, float gain, float pitch, float3 position);
    // What a contact side sounds like (an entity's own, its material's, a terrain layer's, rock),
    // and its preset's name ("" when its own values).
    SurfaceSound surfaceOf(const ContactSide& side, float3 point, std::string* preset = nullptr);
    bool materialSurface(const std::string& fmat, std::string& preset, SurfaceSound& out);
    const PhysicsAudioStats& physicsStats() const { return physicsAudio_.stats(); }
    void syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed);
    // A .fmat's acoustics (read once, again after materialChanged); false: not a material.
    bool materialAcoustics(const std::string& fmat, std::string& preset, AcousticParams& out);
    void materialChanged(const std::string& fmat);
    void terrainLayersChanged() { scene_.materialsChanged(); }
    // What the scene does to an entity's source (playing or previewed): occlusion (1 clear .. 0
    // blocked) and transmission per band. False: not playing, or not simulated.
    bool directOf(uint64_t entity, float& occlusion, float transmission[3]) const;
    AudioSceneStats sceneStats() const { return scene_.stats(); }
    // The listener's reverb times (RT60, seconds) per band, as last simulated; false: none.
    bool listenerReverb(float rt60[3]) const { return scene_.listenerReverb(rt60); }
    void applySettings(const AudioSettings& settings);
    const AudioSettings& settings() const { return settings_; }
    // Sounds played with variation since the game started (tests).
    uint32_t variedPlays() const { return variedPlays_; }

    // Once a frame: the scene's sources (with `gameRunning`), the listener (an AudioListener
    // entity while the game runs, else `camera`), finished voices, and (null device) the frame's
    // samples. scene may be null.
    void update(Scene* scene, bool gameRunning, const CameraState& camera, float dt);
    // The game starts (play the sources marked play on start) or stops (stop all of them).
    void startGame(Scene& scene);
    void stopGame(Scene& scene);

    // A sound by id (soundId) or path, or a test tone (params.toneHz).
    AudioVoice play(uint32_t sound, const SoundPlay& how);
    AudioVoice play(const std::string& path, const SoundPlay& how) { return play(soundId(path), how); }
    AudioVoice play(const std::string& clipPath, const AudioVoiceParams& params);  // a clip, as given
    AudioVoice playTone(const AudioVoiceParams& params);
    void stop(AudioVoice voice, float fadeSeconds = 0.02f) { mixer_->stop(voice, fadeSeconds); }
    bool playing(AudioVoice voice) const { return mixer_->playing(voice); }

    // The editor: one sound at a time, an asset (2D) or an entity's AudioSource (at it). A new
    // preview stops the last.
    // Scripts (API v7): a sound at a point, following an entity (non-zero), else 2D; `loop` as well
    // as the sound's own. A surface preset's impact (1-based index). Both stop with the game.
    AudioVoice playScript(uint32_t sound, const float3* position, uint64_t entity, float volume, float pitch, bool loop);
    AudioVoice playScriptImpact(uint32_t surface, float3 position, float volume, float pitch);
    // (X4) A script's stream (it outlives the voice): at a place, following an entity, else 2D;
    // on a bus (0 sfx, 1 physics, 2 ambience, 3 music, 4 ui). Stops with the game like its sounds.
    AudioVoice playScriptStream(AudioStream* stream, const float3* position, uint64_t entity, float volume, uint32_t bus);
    // volume < 0, pitch <= 0, position null: unchanged.
    void setScriptVoice(AudioVoice voice, float volume, float pitch, const float3* position);
    // A bus's volume from the game (0 master, 1 effects .. 5 UI), multiplying the project's.
    void setScriptBusVolume(uint32_t bus, float volume);

    void preview(const std::string& path);
    void previewSource(Scene& scene, uint64_t entity);
    void stopPreview();
    bool previewing() const { return previewVoice_ && mixer_->playing(previewVoice_); }
    // A .fsound changed on disk (edited): read again when next played.
    void reloadSound(const std::string& path);
    // A sound's settings (a .fsound's, or the defaults for a clip): for the Inspector.
    bool soundSettings(const std::string& path, SoundAsset& out);
    std::string resolve(const std::string& path) const;  // project-relative (or absolute) -> file

    const char* spatializerName() const;  // "Steam Audio 4.8.1 HRTF" or "panning"
    AudioMixerStats stats() const { return mixer_->stats(); }
    uint32_t sourcesPlaying() const;  // scene sources with a voice

private:
    struct Sound {  // a .fsound, as last read, and what it played
        bool loaded = false, valid = false;
        SoundAsset asset;
        std::vector<uint32_t> bag;  // shuffle: clips left this round
        uint32_t next = 0;          // sequence
        uint32_t last = 0;          // the clip it played last
        double lastPlay = -1e30;
        std::vector<AudioVoice> voices;
    };
    struct SceneVoice {  // a scene source's voice (its base gain and pitch, before the source's)
        uint64_t entity = 0;
        AudioVoice voice = 0;
        float gain = 1.0f, pitch = 1.0f;
    };
    struct Simulated {  // a voice placed in the world: its source in the scene
        uint32_t source = 0;
        float3 position = {};
        bool occlusion = true, transmission = true, reflections = true;
        int reflectionSlot = -1;  // its own reflections (else the listener's reverb)
        bool pathing = false;     // the mixer plays its baked paths
    };
    struct ScriptVoice {  // a voice a script played
        AudioVoice voice = 0;
        uint64_t entity = 0;  // followed (0: none)
        float3 position = {};
        float volume = 1.0f, pitch = 1.0f;
    };
    Sound& sound(const std::string& path);
    AudioVoice startSource(uint64_t entity, AudioSource& source, float3 position);
    void applyBusGains(float listenerGain);

    bool active_ = false;
    std::unique_ptr<AudioMixer> mixer_;
    std::unique_ptr<SteamAudio> steam_;
    std::string spatializerName_;
    AudioDevice device_;
    ClipCache clips_;
    std::string projectDir_;
    AudioSettings settings_;
    uint32_t variedPlays_ = 0;
    float listenerGain_ = 1.0f;
    std::unordered_map<std::string, Sound> sounds_;
    std::vector<SceneVoice> sceneVoices_;
    std::vector<ScriptVoice> scriptVoices_;
    float scriptBus_[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t scriptSeed_ = 0;
    std::unordered_map<AudioVoice, std::pair<float, float>> base_;  // play()'s gain and pitch, by voice
    AudioScene scene_;
    std::unordered_map<AudioVoice, Simulated> simulated_;
    std::vector<int> freeReflectionSlots_;
    struct MaterialEntry {
        bool valid = false;
        std::string preset;
        AcousticParams params;
        std::string surfacePreset;
        SurfaceSound surface;
    };
    MaterialEntry& material(const std::string& fmat);
    AudioVoice launch(const AudioClip* data, const SoundParams& p, const SoundPlay& how, float gain, float pitch);
    const PhysicsWorld* physics_ = nullptr;
    PhysicsAudio physicsAudio_;
    const WaterSystem* water_ = nullptr;
    WaterAudio waterAudio_;
    ScenePak* pak_ = nullptr;
    Scene* ecs_ = nullptr;
    MaterialLibrary* materials_ = nullptr;
    const Terrain* terrain_ = nullptr;
    std::string layersJson_;
    std::vector<SurfaceSound> layerSurfaces_;
    std::vector<std::string> layerPresets_;
    std::unordered_map<std::string, std::unique_ptr<AudioClip>> synth_;  // synthesized clips, by character and variant
    std::unordered_map<std::string, MaterialEntry> materialCache_;
    // Baking: the baker, the scene's bake folder and settings, whether the bake in use is stale.
    AudioBaker baker_;
    std::string bakeDir_, bakeError_, staleWhy_;
    AudioBakeSettings bakeSettings_;
    uint64_t checkedRevision_ = ~0ull, seenRevision_ = ~0ull;
    double revisionAt_ = 0.0;  // when the geometry last changed (time_)
    bool stale_ = false, staleLogged_ = false;
    void updateBake();
    AudioVoice previewVoice_ = 0;
    uint64_t previewEntity_ = 0;
    std::mt19937 rng_{0x5eed};  // the same variation every run (tests)
    double time_ = 0.0;         // seconds of audio updates (cooldowns)
};

}  // namespace fx
