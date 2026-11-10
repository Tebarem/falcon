#include "audio/audio_system.h"

#include "assets/material_asset.h"
#include "assets/scene_pak.h"
#include "audio/procedural_synth.h"
#include "core/log.h"
#include "editor/material_library.h"
#include "terrain/terrain.h"
#include "terrain/terrain_layers.h"
#include "scene/fly_camera.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fx {

namespace {

enum SourceFlags : uint32_t {
    kPlayOnStart = 1, kLoop = 2, kSpatial = 4, kOwnSettings = 8, kAir = 16, kLinear = 32, kOcclusion = 64, kTransmission = 128,
    kReflections = 256, kNoVariation = 512,
};

// Sound variation without amounts of its own (AudioSettings::variation): +- dB, +- semitones.
constexpr float kDefaultVolumeRandom = 1.0f, kDefaultPitchRandom = 0.5f;

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
float semitonesToPitch(float st) { return std::pow(2.0f, st / 12.0f); }

float3 positionOf(const WorldTransform& w) { return {w.rows[3], w.rows[7], w.rows[11]}; }

// An AudioSource's own settings (as a .fsound's).
SoundParams settingsOf(const AudioSource& a)
{
    SoundParams s;
    s.loop = (a.flags & kLoop) != 0;
    s.spatial = (a.flags & kSpatial) != 0;
    s.airAbsorption = (a.flags & kAir) != 0;
    s.occlusion = (a.flags & kOcclusion) != 0;
    s.transmission = (a.flags & kTransmission) != 0;
    s.reflections = (a.flags & kReflections) != 0;
    s.rolloff = (a.flags & kLinear) ? 1u : 0u;
    s.minDistance = std::max(a.minDistance, 0.01f);
    s.maxDistance = std::max(a.maxDistance, s.minDistance);
    s.bus = std::min(a.bus, 4u);
    s.vary = (a.flags & kNoVariation) == 0;
    return s;
}

}  // namespace

AudioSystem::AudioSystem() : mixer_(std::make_unique<AudioMixer>()), steam_(std::make_unique<SteamAudio>()) {}
AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::init(const AudioConfig& config)
{
    shutdown();
    Spatializer* spatializer = nullptr;
    // Reflections: effects for the nearest sounds (as many as the highest quality has), sized for
    // its tail; the type is fixed until the next start.
    SteamAudio::Reflections reflections;
    reflections.type = settings_.reverbType;
    reflections.order = 1;
    reflections.seconds = reflectionQuality(3).seconds;
    reflections.slots = reflectionQuality(3).sources;
    if (config.spatializer != AudioConfig::Spatializer::Pan && steam_->init(config.spatialVoices, reflections)) {
        spatializer = steam_.get();
        spatializerName_ = std::string("Steam Audio ") + steam_->version() + " HRTF";
    } else if (config.spatializer == AudioConfig::Spatializer::Steam) {
        FX_ERROR("audio: --audio-spatializer steam, but Steam Audio did not start");
        return false;
    } else {
        spatializerName_ = "panning";
    }
    mixer_->init(config.maxVoices, spatializer);
    if (spatializer)
        scene_.init(
            *steam_,
            [this](const std::string& fmat, AcousticParams& out) {
                std::string preset;
                return materialAcoustics(fmat, preset, out);
            },
            true, config.synchronousReflections);
    freeReflectionSlots_.clear();
    for (int s = int(steam_->reflectionSlots()); s-- > 0;)
        freeReflectionSlots_.push_back(s);
    device_.open(mixer_.get(), config.nullDevice, config.capturePath);
    active_ = true;
    applySettings(settings_);
    FX_INFO("audio: %u Hz, %u-sample blocks, %s, %s%s%s", kAudioSampleRate, kAudioFrameSize, device_.name().c_str(),
            spatializerName_.c_str(), config.capturePath ? ", capturing to " : "", config.capturePath ? config.capturePath : "");
    return true;
}

void AudioSystem::shutdown()
{
    if (!active_)
        return;
    device_.close();  // the device thread stops before the mixer and effects go
    baker_.stop();    // (it bakes with the scene's context)
    scene_.shutdown();
    simulated_.clear();
    materialCache_.clear();
    steam_->shutdown();
    clips_.clear();
    sounds_.clear();
    sceneVoices_.clear();
    base_.clear();
    previewVoice_ = 0;
    active_ = false;
}

void AudioSystem::applySettings(const AudioSettings& settings)
{
    settings_ = settings;
    if (!active_)
        return;
    applyBusGains(listenerGain_);
    mixer_->setSpatializerEnabled(settings_.hrtf);
    mixer_->setReflectionGain(settings_.reflections > 0 && scene_.active() ? std::max(settings_.reverb, 0.0f) : 0.0f);
    scene_.setReflectionQuality(settings_.reflections);
}

void AudioSystem::applyBusGains(float listenerGain)
{
    mixer_->setBusGain(AudioBus::Master, std::max(settings_.master, 0.0f) * listenerGain * scriptBus_[0]);
    for (uint32_t b = 0; b < 5; ++b)
        mixer_->setBusGain(AudioBus(b + 1), std::max(settings_.buses[b], 0.0f) * scriptBus_[b + 1]);
}

std::string AudioSystem::resolve(const std::string& path) const
{
    if (path.empty() || projectDir_.empty() || std::filesystem::path(path).is_absolute())
        return path;
    return projectDir_ + "/" + path;
}

AudioSystem::Sound& AudioSystem::sound(const std::string& path)
{
    Sound& s = sounds_[path];
    if (!s.loaded) {
        s.loaded = true;
        std::string error;
        s.valid = readSoundAsset(resolve(path), s.asset, error);
        if (!s.valid)
            FX_WARN("audio: %s", error.c_str());
        s.bag.clear();
        s.next = 0;
    }
    return s;
}

void AudioSystem::reloadSound(const std::string& path)
{
    if (auto it = sounds_.find(path); it != sounds_.end())
        it->second.loaded = false;
}

bool AudioSystem::soundSettings(const std::string& path, SoundAsset& out)
{
    if (!isSoundAssetPath(path))
        return false;
    Sound& s = sound(path);
    out = s.asset;
    return s.valid;
}

AudioVoice AudioSystem::play(uint32_t id, const SoundPlay& how)
{
    const std::string& path = soundPath(id);
    if (!active_ || path.empty())
        return 0;
    std::string clip = path;
    SoundParams p = how.settings ? *how.settings : SoundParams{};
    float gain = how.volume, pitch = how.pitch;
    Sound* asset = nullptr;
    if (isSoundAssetPath(path)) {
        asset = &sound(path);
        if (!asset->valid || asset->asset.clips.empty())
            return 0;
        const SoundAsset& a = asset->asset;
        if (!how.settings)
            p = a;
        // Cooldown and the instance cap.
        if (a.cooldown > 0.0f && (time_ - asset->lastPlay) * 1000.0 < double(a.cooldown))
            return 0;
        std::erase_if(asset->voices, [&](AudioVoice v) { return !mixer_->playing(v); });
        if (a.maxInstances >= 1.0f && asset->voices.size() >= size_t(a.maxInstances)) {
            mixer_->stop(asset->voices.front(), 0.03f);
            asset->voices.erase(asset->voices.begin());
        }
        // Varied: its amounts, or the project's default for a one-shot with none (not music).
        const bool vary = how.vary && p.vary && a.vary;
        float volumeRandom = vary ? a.volumeRandom : 0.0f, pitchRandom = vary ? a.pitchRandom : 0.0f;
        const bool byDefault = vary && settings_.variation && volumeRandom == 0.0f && pitchRandom == 0.0f && !p.loop && !how.loop &&
                               p.bus != 3;
        if (byDefault) {
            volumeRandom = kDefaultVolumeRandom;
            pitchRandom = kDefaultPitchRandom;
        }
        variedPlays_ += volumeRandom > 0.0f || pitchRandom > 0.0f;
        // Which clip.
        const uint32_t n = uint32_t(a.clips.size());
        uint32_t pick = 0;
        if (a.selection == 2) {
            pick = asset->next++ % n;
        } else if (a.selection == 1) {
            if (asset->bag.empty())
                for (uint32_t i = 0; i < n; ++i)
                    asset->bag.push_back(i);
            const uint32_t at = std::uniform_int_distribution<uint32_t>(0, uint32_t(asset->bag.size()) - 1)(rng_);
            pick = asset->bag[at];
            asset->bag.erase(asset->bag.begin() + at);
        } else if (byDefault && n > 1) {  // (varied: never the same clip twice running)
            pick = (asset->last + 1 + std::uniform_int_distribution<uint32_t>(0, n - 2)(rng_)) % n;
        } else {
            pick = std::uniform_int_distribution<uint32_t>(0, n - 1)(rng_);
        }
        asset->last = pick;
        clip = a.clips[pick];
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        gain *= dbToGain(a.volume + volumeRandom * unit(rng_));
        pitch *= semitonesToPitch(a.pitch + pitchRandom * unit(rng_));
    } else {
        if (!how.settings)
            p.spatial = false;  // a bare clip: 2D, once
        // A clip (an Audio Source's, a script's, a physics sound's): the project's variation.
        if (how.vary && p.vary && settings_.variation && !p.loop && !how.loop && p.bus != 3) {
            ++variedPlays_;
            std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
            gain *= dbToGain(kDefaultVolumeRandom * unit(rng_));
            pitch *= semitonesToPitch(kDefaultPitchRandom * unit(rng_));
        }
    }
    const AudioClip* data = clips_.load(resolve(clip));
    if (!data)
        return 0;
    const AudioVoice voice = launch(data, p, how, gain, pitch);
    if (voice && asset) {
        asset->lastPlay = time_;
        asset->voices.push_back(voice);
    }
    return voice;
}

AudioVoice AudioSystem::launch(const AudioClip* data, const SoundParams& p, const SoundPlay& how, float gain, float pitch)
{
    AudioVoiceParams v;
    v.gain = gain;
    v.pitch = pitch;
    v.position = how.position;
    v.minDistance = p.minDistance;
    v.maxDistance = p.maxDistance;
    v.bus = AudioBus(std::min(p.bus, 4u) + 1);
    if (p.loop || how.loop)
        v.flags |= kVoiceLoop;
    if (p.spatial && !how.force2D) {
        v.flags |= kVoiceSpatial;
        if (how.listenerRelative)
            v.flags |= kVoiceListenerRelative;
        if (p.airAbsorption)
            v.flags |= kVoiceAirAbsorption;
        if (p.rolloff == 1)
            v.flags |= kVoiceLinearRolloff;
        if (p.reflections)
            v.flags |= kVoiceReflections;
    }
    const AudioVoice voice = mixer_->play(data, v);
    // Placed in the world: the scene simulates what is in its way, and its echoes.
    if (voice && (v.flags & kVoiceSpatial) && !(v.flags & kVoiceListenerRelative) && (p.occlusion || p.transmission || p.reflections) &&
        scene_.active())
        if (const uint32_t source = scene_.addSource()) {
            scene_.setSource(source, how.position, p.occlusion, p.transmission, false);
            simulated_[voice] = {source, how.position, p.occlusion, p.transmission, p.reflections};
        }
    if (voice)
        base_[voice] = {gain / std::max(how.volume, 1e-6f), pitch / std::max(how.pitch, 1e-6f)};
    return voice;
}

AudioVoice AudioSystem::play(const std::string& clipPath, const AudioVoiceParams& params)
{
    if (!active_)
        return 0;
    const AudioClip* clip = clips_.load(resolve(clipPath));
    return clip ? mixer_->play(clip, params) : 0;
}

AudioVoice AudioSystem::playTone(const AudioVoiceParams& params)
{
    return active_ ? mixer_->play(nullptr, params) : 0;
}

AudioVoice AudioSystem::startSource(uint64_t entity, AudioSource& a, float3 position)
{
    if (!a.sound)
        return 0;
    const bool own = (a.flags & kOwnSettings) || !isSoundAssetPath(soundPath(a.sound));
    const SoundParams settings = settingsOf(a);
    SoundPlay how;
    how.position = position;
    how.volume = std::max(a.volume, 0.0f);
    how.pitch = std::max(a.pitch, 0.01f);
    how.settings = own ? &settings : nullptr;
    how.vary = (a.flags & kNoVariation) == 0;
    const AudioVoice v = play(a.sound, how);
    if (v) {
        const auto b = base_[v];
        sceneVoices_.push_back({entity, v, b.first, b.second});
    }
    return v;
}

void AudioSystem::startGame(Scene& scene)
{
    if (!active_)
        return;
    stopPreview();
    variedPlays_ = 0;
    uint32_t sources = 0;
    scene.world().each([&](flecs::entity e, AudioSource& a, const WorldTransform& w) {
        ++sources;
        a.voice = (a.flags & kPlayOnStart) ? startSource(e.id(), a, positionOf(w)) : 0u;
    });
    FX_INFO("audio: the game starts: %u of %u sources playing", uint32_t(sceneVoices_.size()), sources);
}

void AudioSystem::stopGame(Scene& scene)
{
    if (!active_)
        return;
    for (const SceneVoice& v : sceneVoices_) {
        mixer_->stop(v.voice, 0.05f);
        const flecs::entity e(scene.world(), flecs::entity_t(v.entity));
        if (e.is_alive())
            if (AudioSource* a = e.try_get_mut<AudioSource>())
                a->voice = 0;
    }
    sceneVoices_.clear();
    for (const ScriptVoice& v : scriptVoices_)
        mixer_->stop(v.voice, 0.05f);
    scriptVoices_.clear();
    std::fill(std::begin(scriptBus_), std::end(scriptBus_), 1.0f);
    physicsAudio_.stopAll(*this);
    waterAudio_.stopAll(*this);
    listenerGain_ = 1.0f;
    applyBusGains(1.0f);
}

void AudioSystem::preview(const std::string& path)
{
    stopPreview();
    SoundPlay how;
    how.force2D = true;
    how.vary = false;  // (heard as made)
    previewVoice_ = play(path, how);
}

void AudioSystem::previewSource(Scene& scene, uint64_t entity)
{
    stopPreview();
    const flecs::entity e(scene.world(), flecs::entity_t(entity));
    const AudioSource* a = e.is_alive() ? e.try_get<AudioSource>() : nullptr;
    const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr;
    if (!a || !w || !a->sound)
        return;
    AudioSource copy = *a;
    const bool own = (copy.flags & kOwnSettings) || !isSoundAssetPath(soundPath(copy.sound));
    const SoundParams settings = settingsOf(copy);
    SoundPlay how;
    how.position = positionOf(*w);
    how.volume = std::max(copy.volume, 0.0f);
    how.pitch = std::max(copy.pitch, 0.01f);
    how.settings = own ? &settings : nullptr;
    how.vary = false;  // (heard as made)
    previewVoice_ = play(copy.sound, how);
    previewEntity_ = previewVoice_ ? entity : 0;
}

void AudioSystem::stopPreview()
{
    if (previewVoice_)
        mixer_->stop(previewVoice_, 0.05f);
    previewVoice_ = 0;
    previewEntity_ = 0;
}

uint32_t AudioSystem::sourcesPlaying() const
{
    uint32_t n = 0;
    for (const SceneVoice& v : sceneVoices_)
        n += mixer_->playing(v.voice) ? 1u : 0u;
    return n;
}

void AudioSystem::update(Scene* scene, bool gameRunning, const CameraState& camera, float dt)
{
    if (!active_)
        return;
    time_ += double(dt);
    mixer_->collect();
    ListenerPose l;
    l.position = camera.position;
    l.right = camera.right;
    l.up = camera.up;
    l.forward = camera.forward;
    float listenerGain = 1.0f;
    if (scene) {
        flecs::world& world = scene->world();
        // Sources follow their entities; one disabled or gone (or whose sound ended) stops.
        std::erase_if(sceneVoices_, [&](const SceneVoice& v) {
            const flecs::entity e(world, flecs::entity_t(v.entity));
            AudioSource* a = e.is_alive() ? e.try_get_mut<AudioSource>() : nullptr;
            const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr;
            if (!a || !w || !e.enabled() || a->voice != v.voice || !mixer_->playing(v.voice)) {
                mixer_->stop(v.voice, 0.05f);
                if (a && a->voice == v.voice)
                    a->voice = 0;
                return true;
            }
            mixer_->set(v.voice, v.gain * std::max(a->volume, 0.0f), v.pitch * std::max(a->pitch, 0.01f), positionOf(*w));
            if (auto s = simulated_.find(v.voice); s != simulated_.end())
                s->second.position = positionOf(*w);
            return false;
        });
        // Scripts' sounds: those on entities follow them (one gone: the sound stays where it was).
        std::erase_if(scriptVoices_, [&](ScriptVoice& v) {
            if (!mixer_->playing(v.voice))
                return true;
            if (v.entity) {
                const flecs::entity e(world, flecs::entity_t(v.entity));
                if (const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr) {
                    v.position = positionOf(*w);
                    setVoice(v.voice, v.volume, v.pitch, v.position);
                }
            }
            return false;
        });
        if (previewEntity_ && previewVoice_) {
            const flecs::entity e(world, flecs::entity_t(previewEntity_));
            const AudioSource* a = e.is_alive() ? e.try_get<AudioSource>() : nullptr;
            const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr;
            const auto b = base_.find(previewVoice_);
            if (a && w && b != base_.end()) {
                mixer_->set(previewVoice_, b->second.first * std::max(a->volume, 0.0f), b->second.second * std::max(a->pitch, 0.01f),
                            positionOf(*w));
                if (auto s = simulated_.find(previewVoice_); s != simulated_.end())
                    s->second.position = positionOf(*w);
            }
        }
        // The game's ears: its first AudioListener.
        if (gameRunning) {
            bool found = false;
            world.each([&](flecs::entity e, const AudioListener& al, const WorldTransform& w) {
                if (found || !e.enabled())
                    return;
                found = true;
                const float* r = w.rows;
                l.position = {r[3], r[7], r[11]};
                l.right = normalize(float3{r[0], r[4], r[8]});
                l.up = normalize(float3{r[1], r[5], r[9]});
                l.forward = normalize(float3{-r[2], -r[6], -r[10]});
                listenerGain = std::max(al.gain, 0.0f);
            });
        }
    }
    if (listenerGain != listenerGain_) {
        listenerGain_ = listenerGain;
        applyBusGains(listenerGain_);
    }
    if (previewVoice_ && !mixer_->playing(previewVoice_)) {
        previewVoice_ = 0;
        previewEntity_ = 0;
    }
    // Physics sounds while the game runs.
    if (physics_ && gameRunning && settings_.physicsSounds)
        physicsAudio_.update(*physics_, *this, [this](const ContactSide& side, float3 point) { return surfaceOf(side, point); }, l.position,
                             time_);
    else
        physicsAudio_.stopAll(*this);
    if (water_ && gameRunning && settings_.waterSounds)
        waterAudio_.update(*water_, physics_, *this, l, dt);
    else
        waterAudio_.stopAll(*this);
    std::erase_if(base_, [&](const auto& kv) { return !mixer_->playing(kv.first); });
    // The scene: geometry in step, then what stands between each placed voice and the listener.
    std::erase_if(simulated_, [&](const auto& kv) {
        if (mixer_->playing(kv.first))
            return false;
        scene_.removeSource(kv.second.source);  // (the mixer is done with it: the voice finished)
        if (kv.second.reflectionSlot >= 0)
            freeReflectionSlots_.push_back(kv.second.reflectionSlot);
        return true;
    });
    // Reflections of their own for the nearest sounds (the rest share the listener's reverb).
    {
        const uint32_t room = std::min<uint32_t>(reflectionQuality(settings_.reflections).sources, steam_->reflectionSlots());
        std::vector<std::pair<float, AudioVoice>> nearest;
        for (const auto& [voice, s] : simulated_)
            if (s.reflections)
                nearest.emplace_back(length(s.position - l.position), voice);
        std::sort(nearest.begin(), nearest.end());
        if (nearest.size() > room)
            nearest.resize(room);
        const auto chosen = [&](AudioVoice v) {
            return std::any_of(nearest.begin(), nearest.end(), [&](const auto& n) { return n.second == v; });
        };
        for (auto& [voice, s] : simulated_)
            if (s.reflectionSlot >= 0 && !chosen(voice)) {
                mixer_->setReflections(voice, -1, nullptr);
                freeReflectionSlots_.push_back(s.reflectionSlot);
                s.reflectionSlot = -1;
            }
        for (const auto& [d, voice] : nearest) {
            Simulated& s = simulated_[voice];
            if (s.reflectionSlot < 0 && !freeReflectionSlots_.empty()) {
                s.reflectionSlot = freeReflectionSlots_.back();
                freeReflectionSlots_.pop_back();
                mixer_->setReflections(voice, s.reflectionSlot, scene_.sourceHandle(s.source));
            }
        }
    }
    // Baked paths for the sounds with reflections (where the probes reach them and the listener).
    const bool pathing = scene_.baked() && scene_.baked()->info.pathing;
    for (auto& [voice, s] : simulated_) {
        scene_.setSource(s.source, s.position, s.occlusion, s.transmission, s.reflectionSlot >= 0, pathing && s.reflections);
        float eq[3];
        const bool found = pathing && s.reflections && scene_.pathing(s.source, eq);
        if (found != s.pathing) {
            mixer_->setPathing(voice, found ? scene_.sourceHandle(s.source) : nullptr);
            s.pathing = found;
        }
    }
    scene_.update(l.position, l);
    updateBake();
    for (const auto& [voice, s] : simulated_) {
        float occlusion = 1.0f, transmission[3] = {1.0f, 1.0f, 1.0f};
        const bool direct = scene_.direct(s.source, occlusion, transmission);
        // Across the water's surface (the listener under it and the sound above, or the other way):
        // most of it bounces off; what gets through is low.
        float muffle = 0.0f;
        const bool under = water_ && gameRunning && settings_.waterSounds && WaterAudio::submerged(*water_, s.position, muffle);
        if (water_ && gameRunning && settings_.waterSounds && under != waterAudio_.listenerUnder()) {
            muffle = under ? muffle : waterAudio_.listenerMuffle();
            static const float kThrough[3] = {0.45f, 0.1f, 0.02f};
            for (int b = 0; b < 3; ++b)
                transmission[b] = (occlusion + (1.0f - occlusion) * transmission[b]) * (1.0f - muffle * (1.0f - kThrough[b]));
            occlusion = 0.0f;
            mixer_->setDirect(voice, occlusion, transmission);
        } else if (direct) {
            mixer_->setDirect(voice, occlusion, transmission);
        }
    }
    mixer_->setListener(l);
    device_.pump(double(dt));
}

const char* AudioSystem::spatializerName() const { return spatializerName_.c_str(); }

void AudioSystem::attachScene(ScenePak* pak, Scene* scene, MaterialLibrary* materials, const Terrain* terrain, const std::string& scenePath)
{
    pak_ = pak;
    ecs_ = scene;
    materials_ = materials;
    terrain_ = terrain;
    scene_.attach(pak, scene, materials, terrain);
    // Its baked acoustics, if it has some (checked against the scene once that is in step).
    baker_.stop();
    bakeDir_ = scenePath.empty() ? std::string() : audioBakeDirFor(scenePath);
    bakeSettings_ = {};
    AudioBakeInfo info;
    readAudioBake(bakeDir_, bakeSettings_, info);
    std::unique_ptr<AudioBakedData> baked;
    std::string error;
    if (scene_.active() && !bakeDir_.empty()) {
        if (loadAudioBake(scene_.context(), bakeDir_, baked, error))
            FX_INFO("audio: baked acoustics: %u probes (%s%s), %.1f MB", baked->info.probes, baked->info.reverb ? "reverb" : "",
                    baked->info.pathing ? ", paths" : "", double(baked->info.bytes) / 1048576.0);
        else if (!error.empty())
            FX_WARN("audio: %s: %s; realtime acoustics only", bakeDir_.c_str(), error.c_str());
    }
    scene_.setBaked(std::move(baked));
    stale_ = staleLogged_ = false;
    staleWhy_.clear();
    bakeError_.clear();
    checkedRevision_ = seenRevision_ = ~0ull;
}

void AudioSystem::detachScene(Scene* scene)
{
    if (scene)
        stopGame(*scene);
    baker_.stop();
    scene_.attach(nullptr, nullptr, nullptr, nullptr);
    pak_ = nullptr;
    ecs_ = nullptr;
    materials_ = nullptr;
    terrain_ = nullptr;
    physics_ = nullptr;
    water_ = nullptr;
    bakeDir_.clear();
}

bool AudioSystem::setBakeSettings(const AudioBakeSettings& settings)
{
    bakeSettings_ = settings;
    if (bakeDir_.empty())
        return false;
    AudioBakeSettings old;
    AudioBakeInfo info;
    readAudioBake(bakeDir_, old, info);  // (what is baked stays as it is)
    return writeAudioBake(bakeDir_, settings, info);
}

bool AudioSystem::startBake(std::string& error)
{
    if (!active_ || !scene_.active() || bakeDir_.empty() || !pak_) {
        error = active_ && !scene_.active() ? "Baking needs Steam Audio (scripts/fetch-steamaudio.ps1)" : "No scene to bake";
        return false;
    }
    if (baker_.busy()) {
        error = "A bake is running";
        return false;
    }
    AudioBaker::Job job;
    job.context = scene_.context();
    job.settings = bakeSettings_;
    job.order = steam_->reflections().order;
    job.savedSeconds = steam_->reflections().seconds;
    job.dir = bakeDir_;
    scene_.bakeGeometry(bakeSettings_, job.geometry);
    if (job.geometry.triangles.empty() || !(job.geometry.lo.x < job.geometry.hi.x)) {
        error = "Nothing to bake: no static geometry with a collider, and no terrain";
        return false;
    }
    FX_INFO("audio: baking %u objects%s (%zu triangles) over %.0f x %.0f x %.0f m, probes every %.1f m", job.geometry.objects,
            job.geometry.terrain ? " and the terrain" : "", job.geometry.triangles.size() / 3, double(job.geometry.hi.x - job.geometry.lo.x),
            double(job.geometry.hi.y - job.geometry.lo.y), double(job.geometry.hi.z - job.geometry.lo.z), double(bakeSettings_.spacing));
    bakeError_.clear();
    return baker_.start(std::move(job));
}

bool AudioSystem::clearBake()
{
    if (bakeDir_.empty() || baker_.busy())
        return false;
    scene_.setBaked(nullptr);
    stale_ = false;
    staleWhy_.clear();
    for (auto& [voice, s] : simulated_)
        if (s.pathing) {
            mixer_->setPathing(voice, nullptr);
            s.pathing = false;
        }
    return clearAudioBake(bakeDir_, bakeSettings_);
}

// A bake finished: its data used. The baked data in use checked against the scene, once the
// scene has kept still for a moment after a change.
void AudioSystem::updateBake()
{
    bool ok = false;
    std::string error;
    AudioBakeInfo info;
    if (baker_.collect(ok, error, info)) {
        std::unique_ptr<AudioBakedData> baked;
        std::string loadError;
        if (ok && loadAudioBake(scene_.context(), bakeDir_, baked, loadError)) {
            FX_INFO("audio: baked %u probes (%s%s) in %.1f s, %.1f MB in %s", info.probes, info.reverb ? "reverb" : "",
                    info.pathing ? ", paths" : "", info.seconds, double(info.bytes) / 1048576.0, bakeDir_.c_str());
            scene_.setBaked(std::move(baked));
            checkedRevision_ = ~0ull;
            stale_ = staleLogged_ = false;
            staleWhy_.clear();
        } else {
            bakeError_ = ok ? loadError : error;
            if (bakeError_ == "Cancelled")
                FX_INFO("audio: bake cancelled");
            else
                FX_WARN("audio: the bake failed: %s", bakeError_.c_str());
        }
    }
    const AudioBakedData* b = scene_.baked();
    const uint64_t revision = scene_.geometryRevision();
    if (revision != seenRevision_) {
        seenRevision_ = revision;
        revisionAt_ = time_;
    }
    if (!b || revision == checkedRevision_ || time_ - revisionAt_ < 0.5)
        return;
    checkedRevision_ = revision;
    uint64_t objects = 0, terrain = 0;
    uint32_t count = 0;
    scene_.fingerprint(b->info.lo, b->info.hi, objects, terrain, count);
    const bool objectsChanged = objects != b->info.objectsHash, terrainChanged = terrain != b->info.terrainHash;
    stale_ = objectsChanged || terrainChanged;
    if (!stale_)
        staleWhy_.clear();
    else if (objectsChanged && terrainChanged)
        staleWhy_ = "objects and the terrain changed since the bake";
    else if (terrainChanged)
        staleWhy_ = "the terrain changed since the bake";
    else if (count > b->info.objects)
        staleWhy_ = "objects were added since the bake";
    else if (count < b->info.objects)
        staleWhy_ = "objects were removed since the bake";
    else
        staleWhy_ = "objects moved or changed since the bake";
    if (stale_ && !staleLogged_) {
        FX_WARN("audio: the baked acoustics are out of date (%s): bake again", staleWhy_.c_str());
        staleLogged_ = true;
    }
}

AudioBakeStatus AudioSystem::bakeStatus() const
{
    AudioBakeStatus st;
    if (!active_ || !scene_.active() || bakeDir_.empty())
        return st;
    st.error = bakeError_;
    const AudioBakedData* b = scene_.baked();
    if (baker_.running()) {
        const AudioBaker::Progress p = baker_.progress();
        st.state = "baking";
        st.stage = p.stage;
        st.fraction = p.fraction;
        st.probes = p.probes;
    } else if (b) {
        st.state = stale_ ? "stale" : "baked";
        st.staleWhy = staleWhy_;
    } else {
        st.state = "none";
    }
    if (b)
        st.info = b->info;
    return st;
}

bool AudioSystem::pathingOf(uint64_t entity, float eq[3]) const
{
    for (const SceneVoice& v : sceneVoices_)
        if (v.entity == entity)
            if (auto s = simulated_.find(v.voice); s != simulated_.end())
                return scene_.pathing(s->second.source, eq);
    if (previewVoice_ && previewEntity_ == entity)
        if (auto s = simulated_.find(previewVoice_); s != simulated_.end())
            return scene_.pathing(s->second.source, eq);
    return false;
}

AudioVoice AudioSystem::playSurface(const SurfaceSound& s, SurfaceKind kind, float3 position, float gain, float pitch, uint32_t seed)
{
    if (!active_)
        return 0;
    const bool loop = kind != SurfaceKind::Impact;
    SoundParams p;  // (physics sounds: placed, on the physics bus, as far as 60 m)
    p.loop = loop;
    p.spatial = true;
    p.minDistance = 1.0f;
    p.maxDistance = 60.0f;
    p.bus = 1;
    SoundPlay how;
    how.position = position;
    how.volume = gain;
    how.pitch = pitch;
    const std::string& path = kind == SurfaceKind::Impact ? s.impact : kind == SurfaceKind::Scrape ? s.scrape : s.roll;
    if (!path.empty()) {  // its sound: a .fsound as made (its own settings), a clip as a physics sound
        how.settings = isSoundAssetPath(path) ? nullptr : &p;
        if (const AudioVoice v = play(soundId(path), how))
            return v;
    }
    if (!s.synth)
        return 0;
    // Synthesized: four variations of each impact, one loop each for scrapes and rolls.
    char key[160];
    std::snprintf(key, sizeof key, "%d|%.3f|%.3f|%.1f|%.4f|%.3f|%u", int(kind), double(s.hardness), double(s.roughness), double(s.frequency),
                  double(s.decay), double(s.noise), kind == SurfaceKind::Impact ? seed % 4u : 0u);
    std::unique_ptr<AudioClip>& clip = synth_[key];
    if (!clip)
        clip = std::make_unique<AudioClip>(kind == SurfaceKind::Impact  ? synthImpact(s, seed % 4u + 1u)
                                           : kind == SurfaceKind::Scrape ? synthScrape(s, 7u)
                                                                         : synthRoll(s, 11u));
    return launch(clip.get(), p, how, gain, pitch);
}

void AudioSystem::setVoice(AudioVoice voice, float gain, float pitch, float3 position)
{
    if (!active_)
        return;
    const auto b = base_.find(voice);
    const float g = b != base_.end() ? b->second.first : 1.0f, p = b != base_.end() ? b->second.second : 1.0f;
    mixer_->set(voice, g * gain, p * pitch, position);
    if (auto s = simulated_.find(voice); s != simulated_.end())
        s->second.position = position;
}

const AudioClip* AudioSystem::synthesized(const std::string& key, const std::function<AudioClip()>& make)
{
    std::unique_ptr<AudioClip>& clip = synth_[key];
    if (!clip)
        clip = std::make_unique<AudioClip>(make());
    return clip.get();
}

AudioVoice AudioSystem::playOr(const std::string& path, const AudioClip* fallback, const SoundParams& p, const SoundPlay& how)
{
    if (!active_)
        return 0;
    if (!path.empty()) {
        SoundPlay h = how;
        h.settings = isSoundAssetPath(path) ? nullptr : &p;
        if (const AudioVoice v = play(soundId(path), h))
            return v;
    }
    return fallback ? launch(fallback, p, how, how.volume, how.pitch) : 0;
}

AudioVoice AudioSystem::playScript(uint32_t sound, const float3* position, uint64_t entity, float volume, float pitch, bool loop)
{
    if (!active_)
        return 0;
    const std::string& path = soundPath(sound);
    SoundPlay how;
    how.volume = std::max(volume, 0.0f);
    how.pitch = std::max(pitch, 0.01f);
    how.loop = loop;
    bool placed = position != nullptr;
    if (position)
        how.position = *position;
    if (entity && ecs_) {
        const flecs::entity e(ecs_->world(), flecs::entity_t(entity));
        if (const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr) {
            how.position = positionOf(*w);
            placed = true;
        }
    }
    SoundParams clip;  // a clip: placed in the world when given a place (the defaults), else 2D
    clip.spatial = placed;
    if (!isSoundAssetPath(path))
        how.settings = &clip;
    else if (!placed)
        how.force2D = true;
    const AudioVoice v = play(sound, how);
    if (v)
        scriptVoices_.push_back({v, placed ? entity : 0, how.position, how.volume, how.pitch});
    return v;
}

AudioVoice AudioSystem::playScriptStream(AudioStream* stream, const float3* position, uint64_t entity, float volume, uint32_t bus)
{
    if (!active_ || !stream)
        return 0;
    AudioVoiceParams p;
    p.stream = stream;
    p.gain = std::max(volume, 0.0f);
    static constexpr AudioBus kBuses[] = {AudioBus::Sfx, AudioBus::Physics, AudioBus::Ambience, AudioBus::Music, AudioBus::Ui};
    p.bus = kBuses[std::min(bus, 4u)];
    bool placed = position != nullptr;
    if (position)
        p.position = *position;
    if (entity && ecs_) {
        const flecs::entity e(ecs_->world(), flecs::entity_t(entity));
        if (const WorldTransform* w = e.is_alive() ? e.try_get<WorldTransform>() : nullptr) {
            p.position = positionOf(*w);
            placed = true;
        }
    }
    if (placed) {
        p.flags = kVoiceSpatial | kVoiceAirAbsorption;
        p.minDistance = 1.0f;
        p.maxDistance = 50.0f;
    }
    const AudioVoice v = mixer_->play(nullptr, p);
    if (v)
        scriptVoices_.push_back({v, placed ? entity : 0, p.position, p.gain, 1.0f});
    return v;
}

AudioVoice AudioSystem::playScriptImpact(uint32_t surface, float3 position, float volume, float pitch)
{
    SurfaceSound s = surfacePresets()[0].sound;
    surfacePreset(surface ? surfacePresetName(surface) : "generic", s);
    const float gain = std::clamp(volume, 0.0f, 1.3f) * s.gain, p = std::max(pitch, 0.01f) * s.pitch;
    const AudioVoice v = playSurface(s, SurfaceKind::Impact, position, gain, p, scriptSeed_++);
    if (v)
        scriptVoices_.push_back({v, 0, position, gain, p});
    return v;
}

void AudioSystem::setScriptVoice(AudioVoice voice, float volume, float pitch, const float3* position)
{
    for (ScriptVoice& v : scriptVoices_)
        if (v.voice == voice) {
            if (volume >= 0.0f)
                v.volume = volume;
            if (pitch > 0.0f)
                v.pitch = pitch;
            if (position) {
                v.position = *position;
                v.entity = 0;  // (placed: no longer following)
            }
            setVoice(voice, v.volume, v.pitch, v.position);
            return;
        }
}

void AudioSystem::setScriptBusVolume(uint32_t bus, float volume)
{
    if (!active_ || bus >= 6)
        return;
    scriptBus_[bus] = std::max(volume, 0.0f);
    applyBusGains(listenerGain_);
}

SurfaceSound AudioSystem::surfaceOf(const ContactSide& side, float3 point, std::string* preset)
{
    SurfaceSound s = surfacePresets()[0].sound;
    if (preset)
        *preset = surfacePresets()[0].name;
    const auto fromMaterial = [&](uint32_t slot) {  // an instance's .fmat (its material slot's)
        if (!pak_ || !materials_ || slot >= pak_->instances().size())
            return;
        if (const uint32_t m = pak_->instances()[slot].flags >> fpak::kInstanceMaterialShift) {
            std::string name;
            if (const std::string source = materials_->sourceOf(m - 1); !source.empty() && materialSurface(source, name, s) && preset)
                *preset = name;
        }
    };
    if (side.kind == ContactOwner::Entity && ecs_) {
        const flecs::entity e(ecs_->world(), flecs::entity_t(side.id));
        if (!e.is_alive())
            return s;
        if (const AcousticSurface* a = e.try_get<AcousticSurface>(); a && (a->flags & 8u)) {
            surfacePreset(surfacePresetName(a->surface), s);
            if (preset)
                *preset = surfacePresetName(a->surface);
            s.gain *= a->impactGain;
            s.pitch *= a->impactPitch;
            return s;
        }
        if (const MeshInstance* mi = e.try_get<MeshInstance>())
            fromMaterial(mi->gpuSlot);
    } else if (side.kind == ContactOwner::Instance) {
        fromMaterial(uint32_t(side.id));
    } else if (side.kind == ContactOwner::Ground) {
        // A terrain layer's (the contact's material, else the layer under the point), else rock.
        uint32_t layer = side.material;
        if (!layer && terrain_ && terrain_->valid()) {
            const TerrainSettings& ts = terrain_->settings();
            const int32_t gx = int32_t(std::lround((point.x - ts.originX) / ts.spacing));
            const int32_t gz = int32_t(std::lround((point.z - ts.originZ) / ts.spacing));
            const int32_t n = int32_t(terrain_->samplesPerSide());
            if (gx >= 0 && gz >= 0 && gx < n && gz < n && std::fabs(point.y - terrain_->heightAt(point.x, point.z)) < 0.5f) {
                std::vector<uint32_t> m;
                terrain_->readMaterials({gx, gz, gx, gz}, m);
                if (!m.empty())
                    layer = (terrain::materialWeightB(m[0]) > 127 ? terrain::materialB(m[0]) : terrain::materialA(m[0])) + 1;
            }
        }
        if (layer && terrain_ && terrain_->valid()) {
            if (terrain_->layersJson() != layersJson_) {
                layersJson_ = terrain_->layersJson();
                layerSurfaces_.clear();
                layerPresets_.clear();
                for (const TerrainLayer& l : parseTerrainLayers(layersJson_)) {
                    SurfaceSound ls = surfacePresets()[0].sound;
                    std::string name = l.surface;
                    if (!surfacePreset(name, ls) && !surfacePreset(name = l.acoustic, ls) &&
                        !surfacePreset(name = acousticPresetForLook(l.generated), ls))
                        name = surfacePresets()[0].name;
                    layerSurfaces_.push_back(ls);
                    layerPresets_.push_back(name);
                }
            }
            if (layer - 1 < layerSurfaces_.size()) {
                if (preset)
                    *preset = layerPresets_[layer - 1];
                return layerSurfaces_[layer - 1];
            }
        }
        surfacePreset("rock", s);  // (scatter: rocks)
        if (preset)
            *preset = "rock";
    }
    return s;
}

AudioSystem::MaterialEntry& AudioSystem::material(const std::string& fmat)
{
    auto [it, added] = materialCache_.try_emplace(fmat);
    MaterialEntry& e = it->second;
    if (added) {
        MaterialAsset m;
        std::string error;
        e.valid = readMaterialAsset(resolve(fmat), m, error);
        e.preset = m.acousticPreset;
        e.params = m.acoustic;
        e.surfacePreset = m.surfacePreset;
        e.surface = m.surface;
    }
    return e;
}

bool AudioSystem::materialSurface(const std::string& fmat, std::string& preset, SurfaceSound& out)
{
    const MaterialEntry& e = material(fmat);
    if (!e.valid)
        return false;
    preset = e.surfacePreset;
    out = e.surface;
    return true;
}

void AudioSystem::syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed)
{
    scene_.syncScatter(added, removed);
}

bool AudioSystem::materialAcoustics(const std::string& fmat, std::string& preset, AcousticParams& out)
{
    const MaterialEntry& e = material(fmat);
    if (!e.valid)
        return false;
    preset = e.preset;
    out = e.params;
    return true;
}

void AudioSystem::materialChanged(const std::string& fmat)
{
    materialCache_.erase(fmat);
    scene_.materialsChanged();
}

bool AudioSystem::directOf(uint64_t entity, float& occlusion, float transmission[3]) const
{
    AudioVoice voice = previewEntity_ == entity ? previewVoice_ : 0;
    for (const SceneVoice& v : sceneVoices_)
        if (v.entity == entity)
            voice = v.voice;
    const auto s = simulated_.find(voice);
    return s != simulated_.end() && scene_.direct(s->second.source, occlusion, transmission);
}

}  // namespace fx
