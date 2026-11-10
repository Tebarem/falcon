#include "vfx/vfx_components.h"

#include "core/json.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace fx {

namespace {

std::unordered_map<std::string, uint32_t>& idsByPath()
{
    static std::unordered_map<std::string, uint32_t> ids;
    return ids;
}

std::vector<std::string>& pathsById()
{
    static std::vector<std::string> paths{""};
    return paths;
}

using T = VfxFieldType;

VfxField F(const char* key, const char* label, const char* group, VfxFieldType type, size_t offset, const char* tip = "")
{
    VfxField f{key, label, group, type, uint32_t(offset)};
    f.tip = tip;
    return f;
}

VfxField R(VfxField f, float lo, float hi)
{
    f.min = lo;
    f.max = hi;
    return f;
}

VfxField B(const char* key, const char* label, const char* group, size_t offset, uint32_t bit, bool feature = false, const char* tip = "")
{
    VfxField f = F(key, label, group, T::Bool, offset, tip);
    f.bit = bit;
    f.feature = feature;
    return f;
}

VfxField E(const char* key, const char* label, const char* group, size_t offset, const char* options, const char* tip = "")
{
    VfxField f = F(key, label, group, T::Enum, offset, tip);
    f.options = options;
    return f;
}

#define OF(S, m) offsetof(S, m)

// ---- ParticleEffect
const VfxField kEffect[] = {
    R(F("maxParticles", "Max particles", "", T::Uint, OF(ParticleEffect, maxParticles)), 1, 100000),
    R(F("lifetime", "Lifetime", "", T::PFloat, OF(ParticleEffect, lifetime), "Seconds a particle lives"), 0, 30),
    R(F("timeScale", "Time scale", "Time", T::Float, OF(ParticleEffect, timeScale)), 0, 4),
    R(F("preWarm", "Pre-warm", "Time", T::Float, OF(ParticleEffect, preWarm), "Seconds run at once when it starts"), 0, 10),
    R(F("startDelay", "Start delay", "Time", T::PFloat, OF(ParticleEffect, startDelay), "Seconds before a new particle shows"), 0, 10),
    R(F("perParticleTimeScale", "Particle time scale", "Time", T::PFloat, OF(ParticleEffect, perParticleTimeScale)), 0, 4),
    F("initialVelocity", "Initial velocity", "Move", T::PVec3, OF(ParticleEffect, initialVelocity), "m/s, in the effect's space"),
    R(F("randomVelocity", "Random velocity", "Move", T::PFloat, OF(ParticleEffect, randomVelocity), "m/s in a random direction"), 0, 50),
    R(F("damping", "Damping", "Move", T::PFloat, OF(ParticleEffect, damping), "Velocity lost per second"), 0, 20),
    F("constantMovement", "Constant movement", "Move", T::Vec3, OF(ParticleEffect, constantMovement), "m/s, not velocity: no collision or damping"),
    R(F("localSpace", "Local space", "Move", T::PFloat, OF(ParticleEffect, localSpace), "1: particles move with the effect"), 0, 1),
    B("rotation", "Rotation", "Rotation", OF(ParticleEffect, flags), kEffectRotation, true),
    R(F("initialPitch", "Initial pitch", "Rotation", T::PFloat, OF(ParticleEffect, initialPitch)), -360, 360),
    R(F("initialYaw", "Initial yaw", "Rotation", T::PFloat, OF(ParticleEffect, initialYaw)), -360, 360),
    R(F("initialRoll", "Initial roll", "Rotation", T::PFloat, OF(ParticleEffect, initialRoll), "A sprite's turn on screen"), -360, 360),
    R(F("pitch", "Pitch", "Rotation", T::PFloat, OF(ParticleEffect, pitch), "Degrees per second"), -720, 720),
    R(F("yaw", "Yaw", "Rotation", T::PFloat, OF(ParticleEffect, yaw), "Degrees per second"), -720, 720),
    R(F("roll", "Roll", "Rotation", T::PFloat, OF(ParticleEffect, roll), "Degrees per second"), -720, 720),
    B("color", "Color", "Color", OF(ParticleEffect, flags), kEffectColor, true),
    F("tint", "Tint", "Color", T::Color, OF(ParticleEffect, tint)),
    F("gradient", "Gradient", "Color", T::PGradient, OF(ParticleEffect, gradient)),
    R(F("brightness", "Brightness", "Color", T::PFloat, OF(ParticleEffect, brightness), "Above 1 glows"), 0, 100),
    B("applyAlpha", "Alpha", "Color", OF(ParticleEffect, flags), kEffectAlpha, false, "Use Alpha below"),
    R(F("alpha", "Alpha amount", "Color", T::PFloat, OF(ParticleEffect, alpha)), 0, 1),
    B("size", "Size", "Size", OF(ParticleEffect, flags), kEffectSize, true),
    R(F("initialScale", "Initial size", "Size", T::PFloat, OF(ParticleEffect, initialScale), "Metres"), 0, 10),
    R(F("scale", "Scale", "Size", T::PFloat, OF(ParticleEffect, scale), "Times the initial size"), 0, 10),
    R(F("stretch", "Stretch", "Size", T::PFloat, OF(ParticleEffect, stretch), "Below 0 wider, above 0 taller"), -1, 1),
    B("force", "Force", "Force", OF(ParticleEffect, flags), kEffectForce, true),
    F("forceDirection", "Direction", "Force", T::Vec3, OF(ParticleEffect, forceDirection), "m/s per second (gravity: 0, -9.81, 0)"),
    B("forceLocal", "Local", "Force", OF(ParticleEffect, flags), kEffectForceLocal, false, "The direction in the effect's space"),
    R(F("forceScale", "Scale", "Force", T::PFloat, OF(ParticleEffect, forceScale)), 0, 10),
    F("orbital", "Orbital", "Force", T::PVec3, OF(ParticleEffect, orbital), "Degrees per second about the effect"),
    R(F("pull", "Pull", "Force", T::PFloat, OF(ParticleEffect, pull), "Toward the effect"), -20, 20),
    B("collision", "Collision", "Collision", OF(ParticleEffect, flags), kEffectCollision, true),
    R(F("collisionRadius", "Radius", "Collision", T::Float, OF(ParticleEffect, collisionRadius)), 0, 2),
    R(F("bounce", "Bounce", "Collision", T::PFloat, OF(ParticleEffect, bounce)), 0, 1),
    R(F("friction", "Friction", "Collision", T::PFloat, OF(ParticleEffect, friction)), 0, 1),
    R(F("bumpiness", "Bumpiness", "Collision", T::PFloat, OF(ParticleEffect, bumpiness), "Random turn when it bounces"), 0, 1),
    R(F("dieChance", "Die on hit", "Collision", T::PFloat, OF(ParticleEffect, dieChance), "Chance a hit kills it"), 0, 1),
    R(F("push", "Push", "Collision", T::PFloat, OF(ParticleEffect, push), "Impulse given to bodies it hits"), 0, 10),
    F("collisionPrefab", "Prefab on hit", "Collision", T::Prefab, OF(ParticleEffect, collisionPrefab)),
    R(F("prefabChance", "Prefab chance", "Collision", T::PFloat, OF(ParticleEffect, prefabChance)), 0, 1),
    B("prefabAlign", "Align to surface", "Collision", OF(ParticleEffect, flags), kEffectPrefabAlign),
    R(F("prefabYaw", "Prefab yaw", "Collision", T::PFloat, OF(ParticleEffect, prefabYaw)), -180, 180),
    B("sheet", "Sheet", "Sheet", OF(ParticleEffect, flags), kEffectSheet, true, "Play a flipbook's frames"),
    R(F("sequenceTime", "Start", "Sheet", T::PFloat, OF(ParticleEffect, sequenceTime)), 0, 1),
    R(F("sequenceSpeed", "Speed", "Sheet", T::PFloat, OF(ParticleEffect, sequenceSpeed), "Sheets per second"), 0, 30),
    B("sheetFit", "Fit to lifetime", "Sheet", OF(ParticleEffect, flags), kEffectSheetFit),
    B("sheetSnap", "Snap to frame", "Sheet", OF(ParticleEffect, flags), kEffectSheetSnap),
    B("temporary", "Temporary", "", OF(ParticleEffect, flags), kEffectTemporary, false, "Removed when it is done"),
    B("waitForPlay", "Wait for Play", "", OF(ParticleEffect, flags), kEffectWaitForPlay, false, "Starts stopped: a script's Particles.Play or Restart starts it"),
};

#define EMISSION(S)                                                                                                               \
    B("loop", "Loop", "Emission", OF(S, emission.flags), kEmitLoop),                                                              \
    R(F("duration", "Duration", "Emission", T::PFloat, OF(S, emission.duration)), 0, 60),                                         \
    R(F("delay", "Delay", "Emission", T::PFloat, OF(S, emission.delay)), 0, 10),                                                  \
    R(F("burst", "Burst", "Emission", T::PFloat, OF(S, emission.burst), "Particles at the start of each run"), 0, 1000),          \
    R(F("rate", "Rate", "Emission", T::PFloat, OF(S, emission.rate), "Particles per second"), 0, 1000),                          \
    R(F("rateOverDistance", "Rate over distance", "Emission", T::PFloat, OF(S, emission.rateOverDistance), "Per metre moved"), 0, 1000), \
    B("destroyOnEnd", "Destroy on end", "Emission", OF(S, emission.flags), kEmitDestroyOnEnd, false, "Not looping: removed when done")

const VfxField kSphere[] = {
    EMISSION(ParticleSphereEmitter),
    R(F("radius", "Radius", "", T::PFloat, OF(ParticleSphereEmitter, radius)), 0, 10),
    B("onEdge", "On edge", "", OF(ParticleSphereEmitter, emission.flags), kEmitOnEdge),
    R(F("velocity", "Velocity", "", T::PFloat, OF(ParticleSphereEmitter, velocity), "Outward, m/s"), -50, 50),
};

const VfxField kBox[] = {
    EMISSION(ParticleBoxEmitter),
    F("boxSize", "Size", "", T::Vec3, OF(ParticleBoxEmitter, size)),
    B("onEdge", "On edge", "", OF(ParticleBoxEmitter, emission.flags), kEmitOnEdge),
};

const VfxField kCone[] = {
    EMISSION(ParticleConeEmitter),
    R(F("angle", "Angle", "", T::PFloat, OF(ParticleConeEmitter, angle), "Half angle, about its +Y"), 0, 90),
    R(F("radius", "Radius", "", T::PFloat, OF(ParticleConeEmitter, radius), "At its base"), 0, 10),
    R(F("length", "Length", "", T::PFloat, OF(ParticleConeEmitter, length), "Above 0: from along it"), 0, 10),
    B("onEdge", "On edge", "", OF(ParticleConeEmitter, emission.flags), kEmitOnEdge),
    R(F("velocity", "Velocity", "", T::PFloat, OF(ParticleConeEmitter, velocity), "Along the cone, m/s"), -50, 50),
};

const VfxField kRing[] = {
    EMISSION(ParticleRingEmitter),
    R(F("radius", "Radius", "", T::PFloat, OF(ParticleRingEmitter, radius)), 0, 20),
    R(F("thickness", "Thickness", "", T::PFloat, OF(ParticleRingEmitter, thickness)), 0, 5),
    R(F("arc", "Arc", "", T::PFloat, OF(ParticleRingEmitter, arc)), 0, 360),
    R(F("velocity", "Velocity", "", T::PFloat, OF(ParticleRingEmitter, velocity), "Outward, m/s"), -50, 50),
};

const VfxField kSprite[] = {
    F("material", "Sprite", "", T::Material, OF(ParticleSpriteRenderer, material), "A material: its base colour and alpha"),
    E("blend", "Blend", "", OF(ParticleSpriteRenderer, blend), "alpha|additive|premultiplied"),
    B("lit", "Lit", "", OF(ParticleSpriteRenderer, flags), kSpriteLit, false, "By the sun and lights"),
    R(F("scale", "Scale", "", T::Float, OF(ParticleSpriteRenderer, scale)), 0, 10),
    E("alignment", "Alignment", "", OF(ParticleSpriteRenderer, alignment), "camera|velocity|upright|flat"),
    R(F("stretch", "Stretch", "", T::Float, OF(ParticleSpriteRenderer, stretch), "Velocity: length per m/s"), 0, 1),
    B("faceVelocity", "Face velocity", "", OF(ParticleSpriteRenderer, flags), kSpriteFaceVelocity),
    R(F("rotationOffset", "Rotation offset", "", T::Float, OF(ParticleSpriteRenderer, rotationOffset)), 0, 360),
    R(F("columns", "Columns", "Sheet", T::Uint, OF(ParticleSpriteRenderer, columns)), 1, 32),
    R(F("rows", "Rows", "Sheet", T::Uint, OF(ParticleSpriteRenderer, rows)), 1, 32),
    R(F("depthFeather", "Depth feather", "", T::Float, OF(ParticleSpriteRenderer, depthFeather), "Soft where it meets surfaces (m)"), 0, 2),
    R(F("cameraFadeNear", "Camera fade near", "", T::Float, OF(ParticleSpriteRenderer, cameraFadeNear)), 0, 10),
    R(F("cameraFadeFar", "Camera fade far", "", T::Float, OF(ParticleSpriteRenderer, cameraFadeFar)), 0, 20),
    B("sort", "Sort", "", OF(ParticleSpriteRenderer, flags), kSpriteSort, false, "Back to front"),
};

const VfxField kTrail[] = {
    F("material", "Texture", "", T::Material, OF(ParticleTrailRenderer, material)),
    E("blend", "Blend", "", OF(ParticleTrailRenderer, blend), "alpha|additive|premultiplied"),
    R(F("points", "Points", "", T::Uint, OF(ParticleTrailRenderer, points)), 2, 32),
    R(F("spacing", "Spacing", "", T::Float, OF(ParticleTrailRenderer, spacing), "Metres between points"), 0.001f, 2),
    R(F("pointLifetime", "Point lifetime", "", T::Float, OF(ParticleTrailRenderer, pointLifetime)), 0.01f, 5),
    R(F("width", "Width", "", T::PFloat, OF(ParticleTrailRenderer, width), "Along the trail"), 0, 2),
    F("trailColor", "Color", "", T::PGradient, OF(ParticleTrailRenderer, color), "Along the trail"),
};

const VfxField kLight[] = {
    B("particleColor", "Particle color", "", OF(ParticleLightRenderer, flags), kLightParticleColor),
    F("lightColor", "Color", "", T::Color, OF(ParticleLightRenderer, color)),
    R(F("intensity", "Intensity", "", T::PFloat, OF(ParticleLightRenderer, intensity)), 0, 1000),
    R(F("range", "Range", "", T::PFloat, OF(ParticleLightRenderer, range)), 0, 50),
    R(F("maxLights", "Max lights", "", T::Uint, OF(ParticleLightRenderer, maxLights)), 1, 64),
};

const VfxField kModel[] = {
    F("mesh", "Mesh", "", T::Mesh, OF(ParticleModelRenderer, mesh), "Any mesh of the scene (a new one: after Reload Scene)"),
    F("material", "Material", "", T::Material, OF(ParticleModelRenderer, material), "None: the mesh's own"),
    R(F("scale", "Scale", "", T::Float, OF(ParticleModelRenderer, scale), "Times the particle's size"), 0, 10),
    F("rotationOffset", "Rotation offset", "", T::Vec3, OF(ParticleModelRenderer, rotationOffset), "Pitch, yaw, roll (degrees)"),
    B("faceVelocity", "Face velocity", "", OF(ParticleModelRenderer, flags), kModelFaceVelocity, false, "Its +Z along where it moves"),
    B("particleColor", "Particle color", "", OF(ParticleModelRenderer, flags), kModelParticleColor, false, "Tinted by the particle's colour"),
    B("shadows", "Cast shadows", "", OF(ParticleModelRenderer, flags), kModelShadows, false, "Off: cheaper for many small ones"),
    R(F("maxModels", "Max models", "", T::Uint, OF(ParticleModelRenderer, maxModels), "The most drawn at once"), 1, 4096),
};

const VfxField kAttractor[] = {
    R(F("strength", "Strength", "", T::PFloat, OF(ParticleAttractor, strength), "m/s per second"), -100, 100),
    R(F("radius", "Radius", "", T::Float, OF(ParticleAttractor, radius), "0: everywhere"), 0, 100),
    B("falloff", "Falloff", "", OF(ParticleAttractor, flags), kAttractFalloff, false, "Weaker toward the radius"),
    B("kill", "Kill at center", "", OF(ParticleAttractor, flags), kAttractKill),
    R(F("killRadius", "Kill radius", "", T::Float, OF(ParticleAttractor, killRadius)), 0, 10),
};

const VfxField kNoise[] = {
    R(F("strength", "Strength", "", T::PFloat, OF(ParticleNoise, strength), "m/s per second"), 0, 50),
    R(F("frequency", "Frequency", "", T::Float, OF(ParticleNoise, frequency), "Per metre"), 0.01f, 20),
    R(F("scroll", "Scroll", "", T::Float, OF(ParticleNoise, scroll), "How fast it changes"), 0, 10),
    R(F("octaves", "Octaves", "", T::Uint, OF(ParticleNoise, octaves)), 1, 4),
};

const VfxField kScript[] = {
    F("class", "Class", "", T::Name, OF(ParticleScript, className), "A ParticleController the game registers"),
};

const VfxField kDecal[] = {
    F("material", "Material", "", T::Material, OF(Decal, material)),
    F("decalSize", "Size", "", T::Vec3, OF(Decal, size), "Across (x, z) and its depth (y)"),
    F("tint", "Tint", "", T::Color, OF(Decal, tint)),
    R(F("opacity", "Opacity", "", T::Float, OF(Decal, opacity)), 0, 1),
    R(F("angleFade", "Angle fade", "", T::Float, OF(Decal, angleFade), "Fades on surfaces turned away"), 0, 1),
    R(F("edgeFade", "Edge fade", "", T::Float, OF(Decal, edgeFade), "Of its depth"), 0, 1),
    B("parallax", "Parallax", "", OF(Decal, flags), kDecalParallax, false, "The material's height map"),
    R(F("parallaxDepth", "Parallax depth", "", T::Float, OF(Decal, parallaxDepth), "Metres its height map goes down"), 0, 0.2f),
    R(F("parallaxSteps", "Parallax steps", "", T::Uint, OF(Decal, parallaxSteps)), 4, 64),
    R(F("cavity", "Cavity", "", T::Float, OF(Decal, cavity), "Darkens it by its height"), 0, 1),
    F("order", "Order", "", T::Int, OF(Decal, order), "Higher on top"),
    B("skinned", "On characters", "", OF(Decal, flags), kDecalSkinned),
};

#undef EMISSION
#undef OF

template <typename C> void initOf(void* data) { new (data) C(); }
template <typename C> flecs::entity_t idOf(flecs::world& world) { return world.component<C>().id(); }

#define INFO(C, json, label, icon, kind, fields) \
    VfxComponentInfo{#C, json, label, icon, VfxKind::kind, uint32_t(sizeof(C)), fields, uint32_t(std::size(fields)), initOf<C>, idOf<C>}

const VfxComponentInfo kInfos[] = {
    INFO(ParticleEffect, "particleEffect", "Particle Effect", "sparkle", Effect, kEffect),
    INFO(ParticleSphereEmitter, "particleSphereEmitter", "Sphere Emitter", "sphere", Emitter, kSphere),
    INFO(ParticleBoxEmitter, "particleBoxEmitter", "Box Emitter", "cube", Emitter, kBox),
    INFO(ParticleConeEmitter, "particleConeEmitter", "Cone Emitter", "cone", Emitter, kCone),
    INFO(ParticleRingEmitter, "particleRingEmitter", "Ring Emitter", "ring", Emitter, kRing),
    INFO(ParticleSpriteRenderer, "particleSprite", "Sprite Renderer", "image", Renderer, kSprite),
    INFO(ParticleTrailRenderer, "particleTrail", "Trail Renderer", "trail", Renderer, kTrail),
    INFO(ParticleLightRenderer, "particleLight", "Light Renderer", "light", Renderer, kLight),
    INFO(ParticleAttractor, "particleAttractor", "Attractor", "target", Controller, kAttractor),
    INFO(ParticleNoise, "particleNoise", "Noise", "wave", Controller, kNoise),
    INFO(ParticleScript, "particleScript", "Particle Script", "code", Controller, kScript),
    INFO(Decal, "decal", "Decal", "decal", Decal, kDecal),
    INFO(ParticleModelRenderer, "particleModel", "Model Renderer", "cube", Renderer, kModel),
};
static_assert(std::size(kInfos) == kVfxComponentCount);

#undef INFO

uint32_t fieldSize(VfxFieldType t)
{
    switch (t) {
    case T::Vec3: return 12;
    case T::Color: return 16;
    case T::PFloat: return sizeof(ParticleFloat);
    case T::PVec3: return sizeof(ParticleVec3);
    case T::PGradient: return sizeof(ParticleGradient);
    default: return 4;
    }
}

bool readFloats(const Json& j, float* out, size_t n)
{
    if (j.type != Json::Type::Array || j.array.size() < n)
        return false;
    for (size_t k = 0; k < n; ++k)
        out[k] = float(j.array[k].number);
    return true;
}

void writeFloats(JsonWriter& w, const char* key, const float* v, size_t n)
{
    w.beginArray(key);
    for (size_t k = 0; k < n; ++k)
        w.value(nullptr, double(v[k]));
    w.endArray();
}

float clampTo(const VfxField& f, float v)
{
    // Only hard limits: counts and fractions. The editor's slider range is a hint, not a limit.
    if (f.type == T::Uint && f.max > f.min)
        return std::clamp(v, f.min, f.max);
    return v;
}

}  // namespace

uint32_t vfxAssetId(const std::string& path)
{
    if (path.empty())
        return 0;
    auto [it, added] = idsByPath().try_emplace(path, uint32_t(pathsById().size()));
    if (added)
        pathsById().push_back(path);
    return it->second;
}

const std::string& vfxAssetPath(uint32_t id)
{
    static const std::string none;
    return id < pathsById().size() ? pathsById()[id] : none;
}

const VfxComponentInfo* vfxComponents(uint32_t& count)
{
    count = uint32_t(std::size(kInfos));
    return kInfos;
}

int32_t vfxComponentIndex(const std::string& nameOrKey)
{
    for (uint32_t i = 0; i < std::size(kInfos); ++i)
        if (nameOrKey == kInfos[i].name || nameOrKey == kInfos[i].json)
            return int32_t(i);
    return -1;
}

void registerVfxComponents(flecs::world& world)
{
    world.component<ParticleEffect>("ParticleEffect");
    world.component<ParticleSphereEmitter>("ParticleSphereEmitter");
    world.component<ParticleBoxEmitter>("ParticleBoxEmitter");
    world.component<ParticleConeEmitter>("ParticleConeEmitter");
    world.component<ParticleRingEmitter>("ParticleRingEmitter");
    world.component<ParticleSpriteRenderer>("ParticleSpriteRenderer");
    world.component<ParticleTrailRenderer>("ParticleTrailRenderer");
    world.component<ParticleLightRenderer>("ParticleLightRenderer");
    world.component<ParticleAttractor>("ParticleAttractor");
    world.component<ParticleNoise>("ParticleNoise");
    world.component<ParticleScript>("ParticleScript");
    world.component<Decal>("Decal");
    world.component<ParticleModelRenderer>("ParticleModelRenderer");
}

bool VfxSet::has(uint32_t component) const { return find(component) != nullptr; }

void* VfxSet::find(uint32_t component)
{
    for (VfxBlob& b : items)
        if (b.component == component)
            return b.data.data();
    return nullptr;
}

const void* VfxSet::find(uint32_t component) const
{
    for (const VfxBlob& b : items)
        if (b.component == component)
            return b.data.data();
    return nullptr;
}

void* VfxSet::add(uint32_t component)
{
    if (void* d = find(component))
        return d;
    if (component >= std::size(kInfos))
        return nullptr;
    VfxBlob b;
    b.component = component;
    b.data.resize(kInfos[component].size);
    kInfos[component].init(b.data.data());
    const auto at = std::lower_bound(items.begin(), items.end(), component, [](const VfxBlob& x, uint32_t c) { return x.component < c; });
    return items.insert(at, std::move(b))->data.data();
}

void VfxSet::remove(uint32_t component)
{
    items.erase(std::remove_if(items.begin(), items.end(), [&](const VfxBlob& b) { return b.component == component; }), items.end());
}

bool readVfxField(const VfxField& f, void* data, const Json& value)
{
    uint8_t* at = static_cast<uint8_t*>(data) + f.offset;
    switch (f.type) {
    case T::Float: {
        if (value.type != Json::Type::Number)
            return false;
        const float v = clampTo(f, float(value.number));
        std::memcpy(at, &v, 4);
        return true;
    }
    case T::Uint:
    case T::Int: {
        if (value.type != Json::Type::Number)
            return false;
        if (f.type == T::Uint) {
            const uint32_t v = uint32_t(std::max(clampTo(f, float(value.number)), 0.0f));
            std::memcpy(at, &v, 4);
        } else {
            const int32_t v = int32_t(value.number);
            std::memcpy(at, &v, 4);
        }
        return true;
    }
    case T::Bool: {
        if (value.type != Json::Type::Bool)
            return false;
        uint32_t flags;
        std::memcpy(&flags, at, 4);
        flags = value.b ? (flags | f.bit) : (flags & ~f.bit);
        std::memcpy(at, &flags, 4);
        return true;
    }
    case T::Enum: {
        uint32_t v = 0;
        if (value.type == Json::Type::Number) {
            v = uint32_t(std::max(value.number, 0.0));
        } else if (value.type == Json::Type::String) {
            // The option's position in "a|b|c".
            std::string_view all = f.options ? f.options : "";
            uint32_t i = 0;
            bool found = false;
            while (!all.empty()) {
                const size_t bar = all.find('|');
                if (all.substr(0, bar) == value.str) {
                    found = true;
                    break;
                }
                ++i;
                all = bar == std::string_view::npos ? std::string_view() : all.substr(bar + 1);
            }
            if (!found)
                return false;
            v = i;
        } else {
            return false;
        }
        std::memcpy(at, &v, 4);
        return true;
    }
    case T::Vec3:
        return readFloats(value, reinterpret_cast<float*>(at), 3);
    case T::Color: {
        float c[4] = {1, 1, 1, 1};
        if (value.type != Json::Type::Array || value.array.size() < 3)
            return false;
        for (size_t k = 0; k < 4 && k < value.array.size(); ++k)
            c[k] = float(value.array[k].number);
        std::memcpy(at, c, sizeof(c));
        return true;
    }
    case T::PFloat: {
        ParticleFloat v;
        if (!readParticleFloat(value, v))
            return false;
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case T::PVec3: {
        ParticleVec3 v;
        if (value.type == Json::Type::Array && value.array.size() == 3) {
            if (!readParticleFloat(value.array[0], v.x) || !readParticleFloat(value.array[1], v.y) || !readParticleFloat(value.array[2], v.z))
                return false;
        } else {
            return false;
        }
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case T::PGradient: {
        ParticleGradient v;
        if (!readParticleGradient(value, v))
            return false;
        std::memcpy(at, &v, sizeof(v));
        return true;
    }
    case T::Material:
    case T::Prefab:
    case T::Name:
    case T::Mesh: {
        if (value.type != Json::Type::String && value.type != Json::Type::Null)
            return false;
        const uint32_t id = value.type == Json::Type::String ? vfxAssetId(value.str) : 0u;
        std::memcpy(at, &id, 4);
        return true;
    }
    }
    return false;
}

void writeVfxField(JsonWriter& w, const VfxField& f, const void* data)
{
    const uint8_t* at = static_cast<const uint8_t*>(data) + f.offset;
    switch (f.type) {
    case T::Float: {
        float v;
        std::memcpy(&v, at, 4);
        w.value(f.key, double(v));
        return;
    }
    case T::Uint: {
        uint32_t v;
        std::memcpy(&v, at, 4);
        w.value(f.key, v);
        return;
    }
    case T::Int: {
        int32_t v;
        std::memcpy(&v, at, 4);
        w.value(f.key, v);
        return;
    }
    case T::Bool: {
        uint32_t v;
        std::memcpy(&v, at, 4);
        w.value(f.key, (v & f.bit) != 0);
        return;
    }
    case T::Enum: {
        uint32_t v;
        std::memcpy(&v, at, 4);
        std::string_view all = f.options ? f.options : "";
        for (uint32_t i = 0; i < v && !all.empty(); ++i) {
            const size_t bar = all.find('|');
            all = bar == std::string_view::npos ? std::string_view() : all.substr(bar + 1);
        }
        w.value(f.key, all.substr(0, all.find('|')));
        return;
    }
    case T::Vec3:
        writeFloats(w, f.key, reinterpret_cast<const float*>(at), 3);
        return;
    case T::Color:
        writeFloats(w, f.key, reinterpret_cast<const float*>(at), 4);
        return;
    case T::PFloat:
        writeParticleFloat(w, f.key, *reinterpret_cast<const ParticleFloat*>(at));
        return;
    case T::PVec3: {
        const ParticleVec3& v = *reinterpret_cast<const ParticleVec3*>(at);
        w.beginArray(f.key);
        writeParticleFloat(w, nullptr, v.x);
        writeParticleFloat(w, nullptr, v.y);
        writeParticleFloat(w, nullptr, v.z);
        w.endArray();
        return;
    }
    case T::PGradient:
        writeParticleGradient(w, f.key, *reinterpret_cast<const ParticleGradient*>(at));
        return;
    case T::Material:
    case T::Prefab:
    case T::Name:
    case T::Mesh: {
        uint32_t id;
        std::memcpy(&id, at, 4);
        w.value(f.key, vfxAssetPath(id));
        return;
    }
    }
}

void writeVfxComponent(JsonWriter& w, const VfxComponentInfo& info, const void* data, bool all)
{
    std::vector<uint8_t> defaults(info.size);
    info.init(defaults.data());
    for (uint32_t i = 0; i < info.fieldCount; ++i) {
        const VfxField& f = info.fields[i];
        if (!all) {
            const uint8_t* a = static_cast<const uint8_t*>(data) + f.offset;
            const uint8_t* b = defaults.data() + f.offset;
            const bool same = f.type == T::Bool ? ((*reinterpret_cast<const uint32_t*>(a) ^ *reinterpret_cast<const uint32_t*>(b)) & f.bit) == 0
                                                : std::memcmp(a, b, fieldSize(f.type)) == 0;
            if (same)
                continue;
        }
        writeVfxField(w, f, data);
    }
}

void readVfxJson(const Json& entity, VfxSet& out)
{
    out.items.clear();
    if (!entity.isObject())
        return;
    for (uint32_t c = 0; c < std::size(kInfos); ++c) {
        const Json* j = entity.get(kInfos[c].json);
        if (!j || !j->isObject())
            continue;
        void* data = out.add(c);
        for (uint32_t i = 0; i < kInfos[c].fieldCount; ++i)
            if (const Json* v = j->get(kInfos[c].fields[i].key))
                readVfxField(kInfos[c].fields[i], data, *v);
    }
}

void writeVfxJson(JsonWriter& w, const VfxSet& set)
{
    for (const VfxBlob& b : set.items) {
        if (b.component >= std::size(kInfos))
            continue;
        w.beginObject(kInfos[b.component].json);
        writeVfxComponent(w, kInfos[b.component], b.data.data(), false);
        w.endObject();
    }
}

void setVfx(flecs::entity e, const VfxSet& set)
{
    flecs::world world = e.world();
    for (uint32_t c = 0; c < std::size(kInfos); ++c) {
        const flecs::entity_t id = kInfos[c].id(world);
        if (const void* data = set.find(c))
            ecs_set_id(world, e, id, kInfos[c].size, data);
        else if (ecs_has_id(world, e, id))
            ecs_remove_id(world, e, id);
    }
}

VfxSet captureVfx(flecs::entity e)
{
    VfxSet set;
    flecs::world world = e.world();
    for (uint32_t c = 0; c < std::size(kInfos); ++c) {
        const flecs::entity_t id = kInfos[c].id(world);
        if (!ecs_has_id(world, e, id))
            continue;
        const void* data = ecs_get_id(world, e, id);
        VfxBlob b;
        b.component = c;
        b.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + kInfos[c].size);
        set.items.push_back(std::move(b));
    }
    return set;
}

const void* vfxGet(flecs::entity e, uint32_t component)
{
    if (component >= std::size(kInfos))
        return nullptr;
    flecs::world world = e.world();
    const flecs::entity_t id = kInfos[component].id(world);
    return ecs_has_id(world, e, id) ? ecs_get_id(world, e, id) : nullptr;
}

}  // namespace fx
