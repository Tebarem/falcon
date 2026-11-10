#pragma once

// Particle and decal components, as s&box's particle front end: an effect is built from parts.
//   - ParticleEffect, the master: the particles' lifetime, limits and features (move, rotation,
//     colour, size, force, collision, sheet).
//   - Emitters (sphere, box, cone, ring) say how many particles start and where.
//   - Renderers (sprite, trail, light, model) draw them.
//   - Controllers (attractor, noise, a C# script) change them as they live.
// Each part sits on the effect's entity or any entity below it, and works for the nearest
// ParticleEffect up the tree (itself included). Decal projects a material onto what is under it.
//
// Plain C++ components (not in components.fschema: curves don't fit it), trivially copyable. One
// reflection table per component (vfxComponents()) drives the JSON (edits, prefabs), the editor's
// Inspector and its field edits, and the undo snapshots.
//
// Axes: an entity's +Y is "up" for emitters (a cone opens along it, a ring lies across it) and for
// a decal, which projects along -Y (place it with +Y along the surface's normal). Units: metres,
// seconds, degrees.

#include "vfx/particle_value.h"

#include <flecs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

// Asset references (materials, prefabs, C# class names) as numbers: components hold no strings.
uint32_t vfxAssetId(const std::string& path);    // 0 for ""
const std::string& vfxAssetPath(uint32_t id);    // "" for 0 or an unknown id

enum : uint32_t {
    kEffectRotation = 1u << 0,
    kEffectColor = 1u << 1,
    kEffectAlpha = 1u << 2,
    kEffectSize = 1u << 3,
    kEffectForce = 1u << 4,
    kEffectCollision = 1u << 5,
    kEffectSheet = 1u << 6,
    kEffectTemporary = 1u << 7,   // the root goes when its emitters are done and no particle is left
    kEffectSheetFit = 1u << 8,    // a sheet's frames spread over the particle's life
    kEffectSheetSnap = 1u << 9,   // whole frames (no blend between them)
    kEffectPrefabAlign = 1u << 10,  // a collision prefab turned to the surface (its +Y along the normal)
    kEffectForceLocal = 1u << 11,   // the force's direction in the effect's space
    kEffectWaitForPlay = 1u << 12,  // starts stopped: a script's Play or Restart starts it (a muzzle flash)
};

struct ParticleEffect {
    uint32_t maxParticles = 1000;
    uint32_t flags = 0;
    float timeScale = 1.0f;
    float preWarm = 0.0f;  // seconds run at once when it starts
    ParticleFloat lifetime = 1.0f;
    ParticleFloat startDelay = 0.0f;
    ParticleFloat perParticleTimeScale = 1.0f;
    // Move
    ParticleVec3 initialVelocity;       // in the effect's space
    ParticleFloat randomVelocity = 0.0f;  // speed in a random direction, at birth
    ParticleFloat damping = 0.0f;       // per second
    float constantMovement[3] = {};     // m/s, not velocity: no collision, no damping
    ParticleFloat localSpace = 0.0f;    // 0: the world's; 1: moves with the effect
    // Rotation (degrees: start, then per second)
    ParticleFloat initialPitch, initialYaw, initialRoll;
    ParticleFloat pitch, yaw, roll;
    // Colour (linear; brightness above 1 for glowing things)
    float tint[4] = {1, 1, 1, 1};
    ParticleGradient gradient;
    ParticleFloat brightness = 1.0f;
    ParticleFloat alpha = 1.0f;
    // Size (metres)
    ParticleFloat initialScale = 0.25f;
    ParticleFloat scale = 1.0f;
    ParticleFloat stretch = 0.0f;  // < 0 wider, > 0 taller
    // Force
    float forceDirection[3] = {0.0f, -9.81f, 0.0f};
    ParticleFloat forceScale = 1.0f;
    ParticleVec3 orbital;          // degrees per second about the effect, per axis
    ParticleFloat pull = 0.0f;     // toward the effect, m/s per metre away
    // Collision
    float collisionRadius = 0.02f;
    uint32_t collisionPrefab = 0;  // vfxAssetId of a .fprefab spawned where a particle hits
    ParticleFloat bounce = 0.4f;
    ParticleFloat friction = 0.5f;
    ParticleFloat bumpiness = 0.0f;
    ParticleFloat dieChance = 0.0f;
    ParticleFloat push = 0.0f;     // impulse given to what it hits, per m/s
    ParticleFloat prefabChance = 1.0f;
    ParticleFloat prefabYaw = 0.0f;
    // Sheet (a flipbook's frames: the renderer's columns x rows)
    ParticleFloat sequenceTime = 0.0f;   // where it starts (0..1)
    ParticleFloat sequenceSpeed = 1.0f;  // sheets per second
};

enum : uint32_t {
    kEmitLoop = 1u << 0,
    kEmitDestroyOnEnd = 1u << 1,  // not looping: its entity goes when it is done and its particles too
    kEmitOnEdge = 1u << 2,        // from the shape's surface, not inside it
};

// What every emitter has: when, and how many.
struct ParticleEmission {
    uint32_t flags = kEmitLoop;
    ParticleFloat duration = 10.0f;
    ParticleFloat delay = 0.0f;
    ParticleFloat burst = 100.0f;           // at the start of each run
    ParticleFloat rate = 0.0f;              // per second
    ParticleFloat rateOverDistance = 0.0f;  // per metre the emitter moves
};

struct ParticleSphereEmitter {
    ParticleEmission emission;
    ParticleFloat radius = 0.5f;
    ParticleFloat velocity = 0.0f;  // outward
};

struct ParticleBoxEmitter {
    ParticleEmission emission;
    float size[3] = {1, 1, 1};
};

struct ParticleConeEmitter {
    ParticleEmission emission;
    ParticleFloat angle = 30.0f;    // half angle
    ParticleFloat radius = 0.0f;    // at its base
    ParticleFloat length = 0.0f;    // > 0: from anywhere along it, not only its base
    ParticleFloat velocity = 1.0f;  // along the cone
};

struct ParticleRingEmitter {
    ParticleEmission emission;
    ParticleFloat radius = 1.0f;
    ParticleFloat thickness = 0.0f;
    ParticleFloat arc = 360.0f;
    ParticleFloat velocity = 0.0f;  // outward
};

enum : uint32_t {
    kSpriteLit = 1u << 0,
    kSpriteSort = 1u << 1,          // back to front (else as they come)
    kSpriteFaceVelocity = 1u << 2,  // turned toward where it moves, on screen
};
enum class SpriteBlend : uint32_t { Alpha, Additive, Premultiplied };
enum class SpriteAlign : uint32_t { Camera, Velocity, Upright, Flat };

struct ParticleSpriteRenderer {
    uint32_t material = 0;  // vfxAssetId of a .fmat: its base colour (and alpha) is the sprite
    uint32_t blend = uint32_t(SpriteBlend::Alpha);
    uint32_t alignment = uint32_t(SpriteAlign::Camera);
    uint32_t flags = kSpriteSort;
    float scale = 1.0f;
    float depthFeather = 0.1f;  // soft where it meets what is behind it (metres)
    float cameraFadeNear = 0.0f, cameraFadeFar = 0.0f;  // gone closer than near, faded in by far (0: off)
    float rotationOffset = 0.0f;
    float stretch = 0.05f;  // Velocity alignment: length per m/s
    uint32_t columns = 1, rows = 1;  // a flipbook's frames
};

struct ParticleTrailRenderer {
    uint32_t material = 0;
    uint32_t blend = uint32_t(SpriteBlend::Additive);
    uint32_t flags = 0;
    uint32_t points = 8;          // the most kept per particle (2..32)
    float spacing = 0.05f;        // metres between points
    float pointLifetime = 0.25f;  // seconds a point stays
    ParticleFloat width = 0.05f;  // along the trail (life: 0 at the particle .. 1 at its end)
    ParticleGradient color;       // the same
};

enum : uint32_t {
    kLightParticleColor = 1u << 0,  // the particle's colour (else the light's own)
};

struct ParticleLightRenderer {
    uint32_t flags = kLightParticleColor;
    uint32_t maxLights = 4;
    float color[4] = {1, 0.75f, 0.45f, 1};  // (alpha unused)
    ParticleFloat intensity = 20.0f;
    ParticleFloat range = 3.0f;
};

enum : uint32_t {
    kModelShadows = 1u << 0,        // the models cast shadows
    kModelParticleColor = 1u << 1,  // tinted by the particle's colour
    kModelFaceVelocity = 1u << 2,   // its +Z turned along where it moves (then its rotation)
};

// Particles as meshes (shell casings, debris): each live particle is an instance of the mesh, lit,
// shadowed and in reflections like any mesh, turned by the particle's rotation and sized by its
// size (1: the mesh's own). The instance slots are kept when the scene loads, for the meshes the
// scene's and the project's prefabs' Model Renderers name (vfx/runtime_meshes.h): a mesh chosen
// later shows after the scene is reloaded.
struct ParticleModelRenderer {
    uint32_t mesh = 0;      // vfxAssetId of a mesh's name ("cube", a model's mesh)
    uint32_t material = 0;  // a .fmat drawn instead of the mesh's own (0: its own)
    uint32_t flags = kModelShadows | kModelParticleColor;
    uint32_t maxModels = 64;  // the most drawn at once (the oldest particles first)
    float scale = 1.0f;
    float rotationOffset[3] = {0, 0, 0};  // pitch, yaw, roll (degrees), before the particle's own
};

enum : uint32_t {
    kAttractFalloff = 1u << 0,  // weaker with distance (to 0 at the radius)
    kAttractKill = 1u << 1,     // particles that reach it die
};

// Pulls particles toward its entity.
struct ParticleAttractor {
    uint32_t flags = 0;
    float radius = 0.0f;      // 0: everywhere
    float killRadius = 0.1f;
    ParticleFloat strength = 5.0f;  // m/s per second
};

// Curl-noise turbulence.
struct ParticleNoise {
    uint32_t octaves = 1;
    float frequency = 1.0f;  // per metre
    float scroll = 0.5f;     // how fast it changes (per second)
    ParticleFloat strength = 1.0f;
};

// A C# ParticleController (managed/Falcon.Engine/Particles.cs), by its class name.
struct ParticleScript {
    uint32_t className = 0;  // vfxAssetId of the name
};

enum : uint32_t {
    kDecalSkinned = 1u << 0,   // also on characters (skinned meshes)
    kDecalParallax = 1u << 1,  // the material's height map steps the view ray (parallax occlusion)
};

struct Decal {
    uint32_t material = 0;
    uint32_t flags = kDecalParallax;
    float size[3] = {0.5f, 0.25f, 0.5f};  // x and z across it, y its depth (along -Y)
    float tint[4] = {1, 1, 1, 1};
    float opacity = 1.0f;
    float angleFade = 0.3f;   // fades out on surfaces this far from facing it (0..1 of 90 degrees)
    float edgeFade = 0.1f;    // of its depth
    uint32_t parallaxSteps = 16;
    float parallaxDepth = 0.02f;  // metres the material's height map goes down (its ORM alpha: 1 the surface, 0 the deepest)
    float cavity = 0.5f;      // how much the height darkens its colour
    int32_t order = 0;        // higher on top
};

// ---- Reflection

enum class VfxFieldType : uint8_t {
    Float,      // float
    Uint,       // uint32_t
    Int,        // int32_t
    Bool,       // a bit of the uint32_t at the offset
    Enum,       // uint32_t, one of `options` ("a|b|c")
    Vec3,       // float[3]
    Color,      // float[4]
    PFloat,     // ParticleFloat
    PVec3,      // ParticleVec3
    PGradient,  // ParticleGradient
    Material,   // uint32_t vfxAssetId (.fmat)
    Prefab,     // uint32_t vfxAssetId (.fprefab)
    Name,       // uint32_t vfxAssetId (any text)
    Mesh,       // uint32_t vfxAssetId (a mesh's name)
};

struct VfxField {
    const char* key;    // in the JSON and the editor's messages
    const char* label;
    const char* group;  // "" or a group's title (a feature's group starts with its toggle)
    VfxFieldType type;
    uint32_t offset;
    uint32_t bit = 0;   // Bool
    const char* options = nullptr;  // Enum
    float min = 0.0f, max = 0.0f;   // the editor's range (max <= min: none)
    bool feature = false;  // Bool: its group's toggle (the group's other fields apply only when on)
    const char* tip = "";
};

enum class VfxKind : uint8_t { Effect, Emitter, Renderer, Controller, Decal };

struct VfxComponentInfo {
    const char* name;   // the component's (flecs) name: "ParticleEffect"
    const char* json;   // its key in an entity's JSON: "particleEffect"
    const char* label;  // "Particle Effect"
    const char* icon;   // the editor's (ui/editor/icons.js)
    VfxKind kind;
    uint32_t size;
    const VfxField* fields;
    uint32_t fieldCount;
    void (*init)(void* data);                   // the defaults
    flecs::entity_t (*id)(flecs::world& world);  // its component id in `world`
};

const VfxComponentInfo* vfxComponents(uint32_t& count);
int32_t vfxComponentIndex(const std::string& nameOrKey);  // -1 if none
void registerVfxComponents(flecs::world& world);

// The particle and decal components of one entity, as bytes (edits, prefabs, undo).
struct VfxBlob {
    uint32_t component = 0;  // index in vfxComponents()
    std::vector<uint8_t> data;
    bool operator==(const VfxBlob& o) const { return component == o.component && data == o.data; }
};
struct VfxSet {
    std::vector<VfxBlob> items;  // by component index, at most one of each
    bool empty() const { return items.empty(); }
    bool has(uint32_t component) const;
    void* find(uint32_t component);
    const void* find(uint32_t component) const;
    void* add(uint32_t component);  // its defaults, if not there yet
    void remove(uint32_t component);
    template <typename T> const T* get() const;
    bool operator==(const VfxSet& o) const { return items == o.items; }
};

// The entity's object: each component's key ("particleEffect": {...}) that is there. Fields left
// out keep their defaults; writing leaves out those at their defaults.
void readVfxJson(const Json& entity, VfxSet& out);
void writeVfxJson(JsonWriter& w, const VfxSet& set);
// One field from JSON (a message's value: as in the file). False if `value` doesn't fit it.
bool readVfxField(const VfxField& f, void* data, const Json& value);
void writeVfxField(JsonWriter& w, const VfxField& f, const void* data);
void writeVfxComponent(JsonWriter& w, const VfxComponentInfo& info, const void* data, bool all);  // its fields (all: the defaults too)

// On an entity: `set`'s components set, the others of these removed.
void setVfx(flecs::entity e, const VfxSet& set);
VfxSet captureVfx(flecs::entity e);
const void* vfxGet(flecs::entity e, uint32_t component);

template <typename T> struct VfxIndexOf;
template <typename T> const T* VfxSet::get() const { return static_cast<const T*>(find(VfxIndexOf<T>::value)); }

#define FX_VFX_INDEX(T, i) template <> struct VfxIndexOf<T> { static constexpr uint32_t value = i; };
FX_VFX_INDEX(ParticleEffect, 0)
FX_VFX_INDEX(ParticleSphereEmitter, 1)
FX_VFX_INDEX(ParticleBoxEmitter, 2)
FX_VFX_INDEX(ParticleConeEmitter, 3)
FX_VFX_INDEX(ParticleRingEmitter, 4)
FX_VFX_INDEX(ParticleSpriteRenderer, 5)
FX_VFX_INDEX(ParticleTrailRenderer, 6)
FX_VFX_INDEX(ParticleLightRenderer, 7)
FX_VFX_INDEX(ParticleAttractor, 8)
FX_VFX_INDEX(ParticleNoise, 9)
FX_VFX_INDEX(ParticleScript, 10)
FX_VFX_INDEX(Decal, 11)
FX_VFX_INDEX(ParticleModelRenderer, 12)
#undef FX_VFX_INDEX
constexpr uint32_t kVfxComponentCount = 13;

}  // namespace fx
