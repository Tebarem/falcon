#pragma once

// Particle simulation (vfx/vfx_components.h's effects), on the CPU as s&box's: each frame the
// emitters start particles on the main thread, then the particles step on the job threads, and
// what they hit (collision) or ask for (prefabs, impulses) is done back on the main thread.
//
// An effect is the entity with a ParticleEffect; its parts (emitters, renderers, controllers) are
// gathered each frame from it and the entities below it, each part working for its nearest effect
// up the tree. Particles live in world space; Local space moves them with the effect as it moves.
//
// It runs in the editor's edit mode too (effects preview where they are; a finished one starts
// again after a second), and in Play and a game. Effects start again when Play starts.

#include "assets/fpak_format.h"
#include "core/math.h"
#include "vfx/vfx_components.h"

#include <flecs.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fx {

class PhysicsWorld;
class Scene;

enum : uint32_t {
    kParticleDelayed = 1u << 0,  // born, not shown yet (its start delay)
    kParticleDead = 1u << 1,     // removed after this step
    kParticleHit = 1u << 2,      // hit something this step
    kParticleNew = 1u << 3,      // became live this step (scripts' OnParticleCreated)
};

// One particle: 128 bytes, the same in C# (managed/Falcon.Engine/Particles.cs: Particle).
struct Particle {
    float position[3];
    float age;          // seconds lived
    float velocity[3];
    float lifetime;     // seconds it lives
    float color[4];     // linear, alpha in w
    float size[3];      // x, y (the sprite's), z: its start size
    float sequence;     // a sheet's position (sheets: frames = 1)
    float angles[3];    // pitch, yaw, roll (degrees)
    float timeScale;
    float startAngles[3];
    float delay;        // seconds before it shows
    uint32_t seed;
    uint32_t flags;
    float sinceHit;     // seconds since it last hit something
    uint32_t hits;
    float user[4];      // scripts' own
};
static_assert(sizeof(Particle) == 128);

// A particle's hit, this step (Particles.Hits; collision prefabs and pushes).
struct ParticleHit {
    float point[3];
    float speed;     // into the surface, m/s
    float normal[3];
    uint32_t particle;  // its index this frame
    uint64_t entity;    // what it hit (0: the ground or scene geometry)
    uint32_t surface;   // as SurfaceHit's (0: unknown)
    uint32_t pad;
};
static_assert(sizeof(ParticleHit) == 48);

class ParticleSystem {
public:
    // Spawns a prefab (a collision prefab) at a world transform, on the main thread: the engine's
    // (Prefab.Spawn's); none: collision prefabs are skipped.
    using PrefabSpawner = std::function<void(const std::string& path, const float world[3][4])>;
    void setPrefabSpawner(PrefabSpawner spawner) { spawner_ = std::move(spawner); }
    // A ParticleScript's controller (the game's C#), after each step while the game runs: the
    // effect's particles (New, Hit and Dead flagged) and this frame's hits, changed in place.
    using ScriptStep = std::function<void(flecs::entity_t script, const std::string& className, Particle* particles, uint32_t count,
                                          const ParticleHit* hits, uint32_t hitCount, float dt)>;
    void setScriptStep(ScriptStep step) { scriptStep_ = std::move(step); }
    struct Stats {
        uint32_t effects = 0, particles = 0;
        double updateMs = 0.0;
    };

    void setPhysics(PhysicsWorld* physics) { physics_ = physics; }
    // A frame: `dt` 0 holds every effect still (paused, a frozen capture). `editing`: the editor's
    // edit mode (finished effects start again; no pushes, nothing destroyed).
    void update(Scene& scene, float dt, bool editing);
    // Every effect from its start (Play), or emptied and stopped (Stop).
    void restartAll();
    void clear();
    // Entities finished effects (Temporary) and emitters (Destroy on end) leave: removed by the
    // engine (in the editor's Play only disabled, and enabled again at Stop: playDisabled()).
    std::vector<flecs::entity_t> takeFinished();

    // Scripts. Play, stop and restart: the effects of the entity and below it (a muzzle's flash,
    // smoke and sparks at once), else the nearest above it. Emit and count: its nearest effect.
    bool play(flecs::entity e);
    bool stop(flecs::entity e, bool clearParticles);
    bool restart(flecs::entity e);
    uint32_t emit(flecs::entity e, uint32_t count);
    uint32_t count(flecs::entity e) const;

    // An effect as the renderer and scripts see it after update().
    struct Effect {
        flecs::entity_t entity = 0;
        const ParticleEffect* effect = nullptr;
        float world[3][4] = {};
        std::vector<Particle> particles;
        // With a Trail Renderer: each particle's last points (newest first), beside `particles`.
        struct Trail {
            static constexpr uint32_t kMaxPoints = 32;
            float points[kMaxPoints][4];  // xyz, w: age (seconds)
            uint32_t count;
        };
        std::vector<Trail> trails;
        std::vector<ParticleHit> hits;  // this frame's
        // This frame's parts bound to it: (entity, its component), by kind.
        struct Part {
            flecs::entity_t entity;
            uint32_t component;  // index in vfxComponents()
            const void* data;
            float world[3][4];
        };
        std::vector<Part> parts;
        // State
        uint32_t seed = 1, rng = 1, restarts = 0;
        bool playing = true, seen = false, hasPrev = false, finished = false, prewarmed = false, moved = false;
        float idle = 0.0f;  // edit mode: seconds since it finished (it starts again at 1)
        double time = 0.0;
        float prev[3][4] = {};   // last frame's world (Local space)
        float delta[3][4] = {};  // world from last frame's (moved)
        struct Emitter {
            float time = 0.0f, delay = 0.0f, duration = 0.0f, rateCarry = 0.0f, distanceCarry = 0.0f;
            uint32_t cycle = 0;
            bool started = false, burstDone = false, finished = false;
            float last[3] = {};
            bool hasLast = false;
        };
        std::unordered_map<uint64_t, Emitter> emitters;  // by (entity, component)
    };
    const std::unordered_map<flecs::entity_t, Effect>& effects() const { return effects_; }
    Effect* effectOf(flecs::entity e);  // e's nearest effect up the tree (its own included)
    std::vector<Effect*> effectsAt(flecs::entity e);  // e's and those below it, else its nearest above
    const Stats& stats() const { return stats_; }
    // This frame's lights from Light Renderers (LightSystem::setTransientLights).
    const std::vector<fpak::LightGpu>& lights() const { return lights_; }

private:
    void gatherLights();
    void recordTrails(Effect& fx, float dt);
    void removeDead(Effect& fx);
    void gather(Scene& scene);
    void emitFrom(Effect& fx, Effect::Part& part, Effect::Emitter& state, float dt, bool editing);
    // `from`: where the emitter was at the frame's start (spread along the way it came).
    void spawn(Effect& fx, const Effect::Part& part, uint32_t count, const float3* from);
    void start(Effect& fx);
    // What a step leaves for the main thread: hits, impulses for what particles hit, prefabs to spawn.
    struct StepOut {
        std::vector<ParticleHit> hits;
        struct Impulse {
            uint64_t body;
            float3 impulse, point;
        };
        std::vector<Impulse> impulses;
        struct Spawn {
            uint32_t prefab;
            float world[3][4];
        };
        std::vector<Spawn> spawns;
    };
    void stepEffect(Effect& fx, float dt, uint32_t first, uint32_t end, uint64_t frame, bool editing, StepOut& out);
    uint32_t nextRandom(Effect& fx);
    float random01(Effect& fx);

    PhysicsWorld* physics_ = nullptr;
    PrefabSpawner spawner_;
    ScriptStep scriptStep_;
    Scene* scene_ = nullptr;
    std::vector<flecs::query<>> partQueries_;  // by component (emitters, renderers, controllers)
    const void* queryWorld_ = nullptr;
    std::unordered_map<flecs::entity_t, Effect> effects_;
    std::vector<flecs::entity_t> finished_;
    std::vector<fpak::LightGpu> lights_;
    Stats stats_;
    uint64_t frame_ = 0;
};

}  // namespace fx
