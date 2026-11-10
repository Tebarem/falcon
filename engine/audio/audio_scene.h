#pragma once

// The scene as Steam Audio hears it, and what stands between each sound and the listener.
//
// Geometry (kept in step with the scene, on the main thread):
//   - every mesh entity, from its mesh's cooked collider (the built-in cube and sphere: their
//     shapes; meshes without a collider, e.g. foliage, let sound through), in world space: a static
//     mesh each (Steam Audio's Embree scenes lose instanced meshes added after one was removed), so
//     one that moves or changes material is placed again. Moving bodies (Rigidbodies) take part
//     only when their AcousticSurface says so; one marked excluded, or hidden, takes no part.
//   - the terrain's tiles near the listener, coarsened to 2 m quads, each quad with its dominant
//     layer's acoustic preset (holes left open);
//   - the scatter cells physics has (rocks), as the "rock" preset.
// A mesh's acoustic material is its AcousticSurface's values, else its .fmat's (the resolver the
// owner gives), else generic.
//
// Direct simulation (Steam Audio's): per sound, volumetric occlusion (16 rays toward a 0.5 m ball)
// and frequency-dependent transmission through the nearest surface in the way, every frame, on a
// thread of its own: update() hands it the sources and takes the last run's results (a frame
// behind; in step when synchronous). Commits happen only while it is idle.
//
// Reflections (Steam Audio's, realtime): on a thread of their own, a few times a second, for the
// sources marked for them and for one at the listener (its reverb, which the other sounds share).
// Scene and source changes are committed only between those runs (a frame later when one is
// running).
//
// Baked acoustics (audio_bake.h), where the scene has them: the listener's reverb comes from the
// probes around it while it is within their reach (realtime elsewhere), and sounds with
// reflections also take the baked paths around what blocks them (pathing, in the same runs).
// The geometry's fingerprint (its static meshes and the terrain) tells a bake gone stale.

#include "assets/acoustic_material.h"
#include "audio/audio_bake.h"
#include "audio/audio_mixer.h"
#include "audio/audio_settings.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fx {

class MaterialLibrary;
class Scene;
class ScenePak;
class SteamAudio;
class Terrain;
struct ScatterCellBodies;

struct AudioSceneStats {
    uint32_t meshes = 0;     // mesh shapes in use
    uint32_t instances = 0;  // placed meshes (entities and scatter)
    uint32_t terrainTiles = 0;
    uint64_t triangles = 0;  // instanced and terrain, as placed
    uint32_t sources = 0, reflecting = 0;
    float simulateMs = 0.0f;  // the last direct run (on its thread)
    float syncMs = 0.0f;      // the last update() on the main thread (geometry, commits, hand-off)
    float reflectionsMs = 0.0f;              // the last reflections run (on its thread)
    uint32_t reflectionRuns = 0;             // since the start
    uint32_t bakedProbes = 0;                // baked data in use (0: none)
    bool listenerBaked = false;              // the last run's listener reverb came from it
    uint32_t pathing = 0;                    // sources whose paths the last run found
};

class AudioScene {
public:
    // The .fmat at a project path: its acoustics (false: no such material).
    using MaterialResolver = std::function<bool(const std::string& fmat, AcousticParams& out)>;

    AudioScene();
    ~AudioScene();
    // reflections: Steam Audio's set up for them (its type, order and longest tail: SteamAudio's).
    // synchronous: reflections run in update() every few frames (the frame clock's rate) instead of
    // on their thread, and update() waits for each direct run, so a fixed-time run hears the same
    // every time (tests).
    bool init(SteamAudio& steam, MaterialResolver resolver, bool reflections, bool synchronous = false);  // false: no Steam Audio
    void setReflectionQuality(uint32_t level);  // AudioSettings::reflections (0: none run)
    void shutdown();
    bool active() const;

    void attach(ScenePak* pak, Scene* scene, MaterialLibrary* materials, const Terrain* terrain);
    void syncScatter(const std::vector<ScatterCellBodies>& added, const std::vector<uint64_t>& removed);
    void materialsChanged();  // a .fmat's or a terrain layer's acoustics: resolved again

    // Once a frame: geometry in step with the scene (terrain around `focus`), then the direct
    // simulation for every source from the listener (handed to its thread).
    void update(float3 focus, const ListenerPose& listener);

    uint32_t addSource();  // 0: no room
    void removeSource(uint32_t source);
    // pathing: its paths around obstacles, from the baked data (where there is some).
    void setSource(uint32_t source, float3 position, bool occlusion, bool transmission, bool reflections, bool pathing = false);
    void* sourceHandle(uint32_t source) const;  // the simulated source (opaque; the mixer reads its reflections)
    void* listenerSource() const;               // the listener's reverb's
    bool listenerReverb(float rt60[3]) const;   // its reverb times per band (seconds; false: none yet)
    // After update(): occlusion (1 clear .. 0 blocked) and transmission per band, as the last
    // direct run found them.
    bool direct(uint32_t source, float& occlusion, float transmission[3]) const;

    // Its paths as the last run found them (an EQ per band, 0 .. 1); false: none (not covered by
    // the baked probes, or no pathing).
    bool pathing(uint32_t source, float eq[3]) const;

    // Baked data (null: none), used from the next run; the old is released.
    void setBaked(std::unique_ptr<AudioBakedData> baked);
    const AudioBakedData* baked() const;
    // The static geometry as the bake hears it: the meshes placed now (not those that move) and the
    // terrain's coarse map, within the region (settings: auto, the static meshes' bounds) and
    // kAudioBakeMargin beyond it, with its fingerprint.
    void bakeGeometry(const AudioBakeSettings& settings, AudioBakeGeometry& out);
    // The fingerprint alone, of a region (and the margin): of the static meshes in it as placed
    // (shape, place, acoustics), and of the terrain under it (heights, holes, layers' acoustics).
    // Changes anywhere count up geometryRevision().
    void fingerprint(float3 lo, float3 hi, uint64_t& objects, uint64_t& terrain, uint32_t& count);
    uint64_t geometryRevision() const;
    void* context() const;  // the IPLContext (bakes)

    AudioSceneStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fx
