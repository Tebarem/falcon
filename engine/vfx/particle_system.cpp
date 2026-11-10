#include "vfx/particle_system.h"

#include "core/jobs.h"
#include "physics/physics_world.h"
#include "core/profile.h"
#include "scene/scene.h"

#include <SDL3/SDL_timer.h>
#include <TaskScheduler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace fx {

namespace {

// Salts: each value of a particle gets a random number of its own from the particle's seed.
enum Salt : uint32_t {
    kSaltLifetime = 1,
    kSaltDelay,
    kSaltTimeScale,
    kSaltVelX,
    kSaltVelY,
    kSaltVelZ,
    kSaltRandomSpeed,
    kSaltDamping,
    kSaltLocal,
    kSaltPitch0,
    kSaltYaw0,
    kSaltRoll0,
    kSaltPitch,
    kSaltYaw,
    kSaltRoll,
    kSaltGradient,
    kSaltBrightness,
    kSaltAlpha,
    kSaltScale0,
    kSaltScale,
    kSaltStretch,
    kSaltForce,
    kSaltOrbitX,
    kSaltOrbitY,
    kSaltOrbitZ,
    kSaltPull,
    kSaltSequence0,
    kSaltSequenceSpeed,
    kSaltAttract,
    kSaltNoise,
    kSaltBounce,
    kSaltFriction,
    kSaltBump,
    kSaltDie = kSaltBump + 2,
    kSaltPush,
    kSaltPrefab,
    kSaltEmitter = 64,  // emitters' own (per cycle)
};

float3 translation(const float m[3][4]) { return {m[0][3], m[1][3], m[2][3]}; }
float3 xformPoint(const float m[3][4], float3 p)
{
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
}
float3 xformDir(const float m[3][4], float3 d)
{
    return {m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z, m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z,
            m[2][0] * d.x + m[2][1] * d.y + m[2][2] * d.z};
}
float3 column(const float m[3][4], int c) { return {m[0][c], m[1][c], m[2][c]}; }

void inverse(const float m[3][4], float out[3][4])
{
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::fabs(det) > 1e-20f ? 1.0f / det : 0.0f;
    float r[3][3] = {{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s},
                     {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s},
                     {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            out[row][col] = r[row][col];
        out[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
    }
}

void multiply(const float a[3][4], const float b[3][4], float out[3][4])
{
    float r[3][4];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            r[row][col] = a[row][0] * b[0][col] + a[row][1] * b[1][col] + a[row][2] * b[2][col] + (col == 3 ? a[row][3] : 0.0f);
    std::memcpy(out, r, sizeof(r));
}

float3 lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }

// Rodrigues: v turned `angle` radians about the unit `axis`.
float3 rotateAbout(float3 v, float3 axis, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
}

float3 randomUnit(float u, float v)
{
    const float z = 1.0f - 2.0f * u, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * v;
    return {r * std::cos(phi), z, r * std::sin(phi)};
}

// ---- Noise: gradient noise and its curl (divergence free, so particles swirl without bunching).
uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

float gradDot(uint32_t h, float x, float y, float z)
{
    switch (h % 12u) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    default: return -y - z;
    }
}

float gradientNoise(float3 p, uint32_t seed)
{
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int x = int(fx), y = int(fy), z = int(fz);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float u = fade(tx), v = fade(ty), w = fade(tz);
    const auto lerpf = [](float a, float b, float t) { return a + (b - a) * t; };
    float c[2][2][2];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k)
                c[i][j][k] = gradDot(hash3(x + i, y + j, z + k, seed), tx - float(i), ty - float(j), tz - float(k));
    return lerpf(lerpf(lerpf(c[0][0][0], c[1][0][0], u), lerpf(c[0][1][0], c[1][1][0], u), v),
                 lerpf(lerpf(c[0][0][1], c[1][0][1], u), lerpf(c[0][1][1], c[1][1][1], u), v), w);
}

float3 curlNoise(float3 p, uint32_t octaves)
{
    float3 sum = {};
    float amp = 1.0f, freq = 1.0f;
    for (uint32_t o = 0; o < std::max(octaves, 1u); ++o) {
        const float3 q = p * freq;
        const float e = 0.05f;
        const auto n = [&](float3 at, uint32_t s) { return gradientNoise(at, s + o * 31u); };
        const float3 dx = {e, 0, 0}, dy = {0, e, 0}, dz = {0, 0, e};
        // The potential's three fields: psi = (n1, n2, n3); curl = (d n3/dy - d n2/dz, d n1/dz - d n3/dx, d n2/dx - d n1/dy).
        const float d3dy = n(q + dy, 3) - n(q - dy, 3), d2dz = n(q + dz, 2) - n(q - dz, 2);
        const float d1dz = n(q + dz, 1) - n(q - dz, 1), d3dx = n(q + dx, 3) - n(q - dx, 3);
        const float d2dx = n(q + dx, 2) - n(q - dx, 2), d1dy = n(q + dy, 1) - n(q - dy, 1);
        sum += float3{d3dy - d2dz, d1dz - d3dx, d2dx - d1dy} * (amp / (2.0f * e));
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum;
}

const ParticleEmission& emissionOf(const void* data) { return *static_cast<const ParticleEmission*>(data); }  // (each emitter's first member)

bool isEmitter(uint32_t c)
{
    return c == VfxIndexOf<ParticleSphereEmitter>::value || c == VfxIndexOf<ParticleBoxEmitter>::value ||
           c == VfxIndexOf<ParticleConeEmitter>::value || c == VfxIndexOf<ParticleRingEmitter>::value;
}

}  // namespace

uint32_t ParticleSystem::nextRandom(Effect& fx)
{
    // xorshift32 (never 0)
    uint32_t x = fx.rng ? fx.rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return fx.rng = x;
}

float ParticleSystem::random01(Effect& fx) { return float(nextRandom(fx) >> 8) * (1.0f / 16777216.0f); }

void ParticleSystem::gather(Scene& scene)
{
    scene_ = &scene;
    flecs::world& world = scene.world();
    for (auto& [id, fx] : effects_) {
        fx.seen = false;
        fx.parts.clear();
    }
    world.each([&](flecs::entity e, const ParticleEffect& pe) {
        Effect& fx = effects_[e.id()];
        const bool fresh = fx.entity == 0;
        fx.entity = e.id();
        fx.effect = &pe;
        fx.seen = true;
        if (const WorldTransform* w = e.try_get<WorldTransform>())
            std::memcpy(fx.world, w->rows, sizeof(fx.world));
        else
            std::memcpy(fx.world, std::array<float, 12>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}.data(), sizeof(fx.world));
        if (fresh)
            start(fx);
    });
    for (auto it = effects_.begin(); it != effects_.end();)
        it = it->second.seen ? std::next(it) : effects_.erase(it);
    if (effects_.empty())
        return;

    // The parts: each component's entities, bound to their nearest effect up the tree.
    if (partQueries_.empty() || queryWorld_ != world.c_ptr()) {
        partQueries_.clear();
        queryWorld_ = world.c_ptr();
        uint32_t count = 0;
        const VfxComponentInfo* infos = vfxComponents(count);
        for (uint32_t c = 0; c < count; ++c)
            partQueries_.push_back(infos[c].kind == VfxKind::Effect || infos[c].kind == VfxKind::Decal
                                       ? flecs::query<>()
                                       : world.query_builder().with(infos[c].id(world)).cached().build());
    }
    uint32_t count = 0;
    const VfxComponentInfo* infos = vfxComponents(count);
    for (uint32_t c = 0; c < count; ++c) {
        if (infos[c].kind == VfxKind::Effect || infos[c].kind == VfxKind::Decal)
            continue;
        const flecs::entity_t id = infos[c].id(world);
        partQueries_[c].each([&](flecs::entity e) {
            Effect* fx = nullptr;
            for (flecs::entity at = e; at && !fx; at = scene.parentOf(at))
                if (auto it = effects_.find(at.id()); it != effects_.end())
                    fx = &it->second;
            if (!fx)
                return;
            Effect::Part part;
            part.entity = e.id();
            part.component = c;
            part.data = ecs_get_id(world, e, id);
            if (const WorldTransform* w = e.try_get<WorldTransform>())
                std::memcpy(part.world, w->rows, sizeof(part.world));
            else
                std::memcpy(part.world, fx->world, sizeof(part.world));
            fx->parts.push_back(part);
        });
    }
}

ParticleSystem::Effect* ParticleSystem::effectOf(flecs::entity e)
{
    for (flecs::entity at = e; at; at = scene_ ? scene_->parentOf(at) : flecs::entity())
        if (auto it = effects_.find(at.id()); it != effects_.end())
            return &it->second;
    return nullptr;
}

void ParticleSystem::start(Effect& fx)
{
    // Seeded by the entity and how often it started: the same effect plays the same way.
    fx.seed = uint32_t(fx.entity * 2654435761u) ^ (fx.restarts * 0x85EBCA6Bu) ^ 0x27D4EB2Fu;
    fx.rng = fx.seed | 1u;
    fx.particles.clear();
    fx.trails.clear();
    fx.hits.clear();
    fx.emitters.clear();
    fx.time = 0.0;
    fx.playing = !(fx.effect && (fx.effect->flags & kEffectWaitForPlay));
    fx.finished = false;
    fx.idle = 0.0f;
    fx.prewarmed = false;
}

void ParticleSystem::restartAll()
{
    for (auto& [id, fx] : effects_) {
        ++fx.restarts;
        start(fx);
    }
}

void ParticleSystem::clear() { effects_.clear(); }

std::vector<flecs::entity_t> ParticleSystem::takeFinished()
{
    std::vector<flecs::entity_t> out;
    out.swap(finished_);
    return out;
}

std::vector<ParticleSystem::Effect*> ParticleSystem::effectsAt(flecs::entity e)
{
    std::vector<Effect*> out;
    for (auto& [id, fx] : effects_)
        for (flecs::entity at(e.world(), id); at && scene_; at = scene_->parentOf(at))
            if (at == e) {
                out.push_back(&fx);
                break;
            }
    if (out.empty())
        if (Effect* fx = effectOf(e))
            out.push_back(fx);
    return out;
}

bool ParticleSystem::play(flecs::entity e)
{
    const std::vector<Effect*> list = effectsAt(e);
    for (Effect* fx : list) {
        if (fx->finished) {
            ++fx->restarts;
            start(*fx);
        }
        fx->playing = true;
    }
    return !list.empty();
}

bool ParticleSystem::stop(flecs::entity e, bool clearParticles)
{
    const std::vector<Effect*> list = effectsAt(e);
    for (Effect* fx : list) {
        fx->playing = false;
        if (clearParticles) {
            fx->particles.clear();
            fx->trails.clear();
        }
    }
    return !list.empty();
}

bool ParticleSystem::restart(flecs::entity e)
{
    const std::vector<Effect*> list = effectsAt(e);
    for (Effect* fx : list) {
        ++fx->restarts;
        start(*fx);
        fx->playing = true;
    }
    return !list.empty();
}

uint32_t ParticleSystem::emit(flecs::entity e, uint32_t n)
{
    Effect* fx = effectOf(e);
    if (!fx || !fx->effect)
        return 0;
    const size_t before = fx->particles.size();
    // From its first emitter's shape, else from the effect's own origin.
    for (const Effect::Part& part : fx->parts)
        if (isEmitter(part.component)) {
            spawn(*fx, part, n, nullptr);
            return uint32_t(fx->particles.size() - before);
        }
    Effect::Part origin = {};
    origin.entity = fx->entity;
    origin.component = ~0u;
    std::memcpy(origin.world, fx->world, sizeof(origin.world));
    spawn(*fx, origin, n, nullptr);
    return uint32_t(fx->particles.size() - before);
}

uint32_t ParticleSystem::count(flecs::entity e) const
{
    const Effect* fx = const_cast<ParticleSystem*>(this)->effectOf(e);
    if (!fx)
        return 0;
    uint32_t n = 0;
    for (const Particle& p : fx->particles)
        n += (p.flags & kParticleDelayed) ? 0u : 1u;
    return n;
}

void ParticleSystem::spawn(Effect& fx, const Effect::Part& part, uint32_t n, const float3* from)
{
    const ParticleEffect& pe = *fx.effect;
    const float3 origin = translation(part.world);
    for (uint32_t i = 0; i < n; ++i) {
        if (fx.particles.size() >= std::max(pe.maxParticles, 1u))
            break;
        // Where, and which way, in the emitter's space (+Y up).
        float3 at = {}, dir = {0, 1, 0};
        float speed = 0.0f;
        const float r0 = random01(fx), r1 = random01(fx), r2 = random01(fx), r3 = random01(fx);
        const uint32_t salt = nextRandom(fx);
        const auto value = [&](const ParticleFloat& v, uint32_t s) { return v.evaluate(0.0f, particleRandom(salt, s), random01(fx)); };
        if (part.component == VfxIndexOf<ParticleSphereEmitter>::value) {
            const auto& s = *static_cast<const ParticleSphereEmitter*>(part.data);
            dir = randomUnit(r0, r1);
            const float radius = std::max(value(s.radius, 1), 0.0f);
            at = dir * ((s.emission.flags & kEmitOnEdge) ? radius : radius * std::cbrt(r2));
            speed = value(s.velocity, 2);
        } else if (part.component == VfxIndexOf<ParticleBoxEmitter>::value) {
            const auto& b = *static_cast<const ParticleBoxEmitter*>(part.data);
            float3 u = {r0 - 0.5f, r1 - 0.5f, r2 - 0.5f};
            if (b.emission.flags & kEmitOnEdge) {
                // A face, by area.
                const float ax = b.size[1] * b.size[2], ay = b.size[0] * b.size[2], az = b.size[0] * b.size[1];
                const float pick = r3 * (ax + ay + az);
                const float side = (salt & 1u) ? 0.5f : -0.5f;
                if (pick < ax)
                    u.x = side;
                else if (pick < ax + ay)
                    u.y = side;
                else
                    u.z = side;
            }
            at = {u.x * b.size[0], u.y * b.size[1], u.z * b.size[2]};
            dir = randomUnit(r3, r0);
        } else if (part.component == VfxIndexOf<ParticleConeEmitter>::value) {
            const auto& c = *static_cast<const ParticleConeEmitter*>(part.data);
            const float half = std::clamp(value(c.angle, 1), 0.0f, 90.0f) * (kPi / 180.0f);
            // A direction inside the cone (on its edge: at its angle), evenly over the cap.
            const float cosT = (c.emission.flags & kEmitOnEdge) ? std::cos(half) : 1.0f - r0 * (1.0f - std::cos(half));
            const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT)), phi = 2.0f * kPi * r1;
            dir = {sinT * std::cos(phi), cosT, sinT * std::sin(phi)};
            const float len = std::max(value(c.length, 2), 0.0f) * r3;
            const float radius = std::max(value(c.radius, 3), 0.0f);
            const float ring = (c.emission.flags & kEmitOnEdge) ? 1.0f : std::sqrt(r2);
            const float3 base = float3{std::cos(phi), 0.0f, std::sin(phi)} * (radius * ring);
            at = base + dir * (len / std::max(cosT, 1e-3f));
            speed = value(c.velocity, 4);
        } else if (part.component == VfxIndexOf<ParticleRingEmitter>::value) {
            const auto& g = *static_cast<const ParticleRingEmitter*>(part.data);
            const float arc = std::clamp(value(g.arc, 1), 0.0f, 360.0f) * (kPi / 180.0f);
            const float phi = arc * r0;
            const float3 out = {std::cos(phi), 0.0f, std::sin(phi)};
            const float radius = std::max(value(g.radius, 2), 0.0f) + (r1 - 0.5f) * std::max(value(g.thickness, 3), 0.0f);
            at = out * radius;
            dir = out;
            speed = value(g.velocity, 4);
        } else {
            dir = randomUnit(r0, r1);
        }
        Particle p = {};
        float3 pos = xformPoint(part.world, at);
        if (from)  // spread along the way the emitter moved this frame
            pos += (*from - origin) * (1.0f - (float(i) + 0.5f) / float(n));
        const float3 wdir = normalize(xformDir(part.world, dir));
        p.seed = nextRandom(fx);
        const auto rnd = [&](uint32_t s) { return particleRandom(p.seed, s); };
        const float frame = random01(fx);
        const auto at0 = [&](const ParticleFloat& v, uint32_t s) { return v.evaluate(0.0f, rnd(s), frame); };
        p.lifetime = std::max(at0(pe.lifetime, kSaltLifetime), 0.01f);
        p.delay = std::max(at0(pe.startDelay, kSaltDelay), 0.0f);
        p.timeScale = 1.0f;
        float3 v = wdir * speed;
        v += xformDir(fx.world, {at0(pe.initialVelocity.x, kSaltVelX), at0(pe.initialVelocity.y, kSaltVelY), at0(pe.initialVelocity.z, kSaltVelZ)});
        if (!pe.randomVelocity.isNearlyZero())
            v += randomUnit(random01(fx), random01(fx)) * at0(pe.randomVelocity, kSaltRandomSpeed);
        std::memcpy(p.position, &pos, 12);
        std::memcpy(p.velocity, &v, 12);
        const float size = std::max(at0(pe.initialScale, kSaltScale0), 0.0f);
        p.size[0] = p.size[1] = p.size[2] = size;
        if (pe.flags & kEffectRotation) {
            p.startAngles[0] = at0(pe.initialPitch, kSaltPitch0);
            p.startAngles[1] = at0(pe.initialYaw, kSaltYaw0);
            p.startAngles[2] = at0(pe.initialRoll, kSaltRoll0);
            std::memcpy(p.angles, p.startAngles, sizeof(p.angles));
        }
        if (pe.flags & kEffectSheet)
            p.sequence = std::max(at0(pe.sequenceTime, kSaltSequence0), 0.0f);
        p.color[0] = p.color[1] = p.color[2] = p.color[3] = 1.0f;
        p.sinceHit = 1e9f;
        p.flags = p.delay > 0.0f ? kParticleDelayed : kParticleNew;
        fx.particles.push_back(p);
    }
}

void ParticleSystem::emitFrom(Effect& fx, Effect::Part& part, Effect::Emitter& state, float dt, bool)
{
    const ParticleEmission& em = emissionOf(part.data);
    if (state.finished)
        return;
    const float3 here = translation(part.world);
    const uint32_t cycleSeed = fx.seed ^ uint32_t(part.entity * 0x9E3779B1u) ^ (state.cycle * 0x85EBCA6Bu);
    const auto value = [&](const ParticleFloat& v, float d, uint32_t s) {
        return v.evaluate(d, particleRandom(cycleSeed, kSaltEmitter + s), random01(fx));
    };
    if (!state.started) {
        state.started = true;
        state.burstDone = false;
        state.time = 0.0f;
        state.rateCarry = state.distanceCarry = 0.0f;
        state.delay = std::max(value(em.delay, 0.0f, 0), 0.0f);
        state.duration = std::max(value(em.duration, 0.0f, 1), 0.0f);
    }
    state.time += dt;
    if (state.time < state.delay) {
        std::memcpy(state.last, &here, 12);
        state.hasLast = true;
        return;
    }
    const float run = state.time - state.delay;
    const float d = state.duration > 0.0f ? std::min(run / state.duration, 1.0f) : 1.0f;
    if (!state.burstDone) {
        state.burstDone = true;
        const float burst = value(em.burst, 0.0f, 2);
        if (burst >= 0.5f)
            spawn(fx, part, uint32_t(burst + 0.5f), nullptr);
    }
    // Over time (this frame's share of the run, past the delay).
    state.rateCarry += std::max(value(em.rate, d, 3), 0.0f) * std::min(dt, run);
    if (state.rateCarry >= 1.0f) {
        const uint32_t n = uint32_t(state.rateCarry);
        state.rateCarry -= float(n);
        spawn(fx, part, n, nullptr);
    }
    // Over distance: spread along the way it came.
    if (state.hasLast && !em.rateOverDistance.isNearlyZero()) {
        const float3 last = {state.last[0], state.last[1], state.last[2]};
        state.distanceCarry += length(here - last) * std::max(value(em.rateOverDistance, d, 4), 0.0f);
        if (state.distanceCarry >= 1.0f) {
            const uint32_t n = uint32_t(state.distanceCarry);
            state.distanceCarry -= float(n);
            spawn(fx, part, n, &last);
        }
    }
    std::memcpy(state.last, &here, 12);
    state.hasLast = true;
    if (run >= state.duration) {
        if (em.flags & kEmitLoop) {
            state.started = false;  // (the next run's delay, duration and burst are rolled again)
            ++state.cycle;
        } else {
            state.finished = true;
        }
    }
}

void ParticleSystem::stepEffect(Effect& fx, float dt, uint32_t first, uint32_t end, uint64_t frame, bool editing, StepOut& out)
{
    const bool collide = (fx.effect->flags & kEffectCollision) != 0 && physics_ && physics_->valid();
    const ParticleEffect& pe = *fx.effect;
    const float3 center = translation(fx.world);
    const bool color = (pe.flags & kEffectColor) != 0, alpha = (pe.flags & kEffectAlpha) != 0;
    const bool size = (pe.flags & kEffectSize) != 0, rotation = (pe.flags & kEffectRotation) != 0;
    const bool force = (pe.flags & kEffectForce) != 0, sheet = (pe.flags & kEffectSheet) != 0;
    const float3 axes[3] = {normalize(column(fx.world, 0)), normalize(column(fx.world, 1)), normalize(column(fx.world, 2))};
    const float3 cm = {pe.constantMovement[0], pe.constantMovement[1], pe.constantMovement[2]};
    const bool hasCm = cm.x != 0.0f || cm.y != 0.0f || cm.z != 0.0f;
    const float3 cmWorld = xformDir(fx.world, cm);
    const uint32_t frameSalt = uint32_t(frame * 2654435761u);
    for (uint32_t i = first; i < end; ++i) {
        Particle& p = fx.particles[i];
        if (p.flags & kParticleDead)
            continue;
        if (p.flags & kParticleDelayed) {
            p.delay -= dt;
            if (p.delay > 0.0f)
                continue;
            p.flags = (p.flags & ~kParticleDelayed) | kParticleNew;
        }
        const auto rnd = [&](uint32_t s) { return particleRandom(p.seed, s); };
        const auto frnd = [&](uint32_t s) { return particleRandom(p.seed ^ frameSalt, s); };
        // Its lifetime again (the editor changes it live), then its age.
        p.lifetime = std::max(pe.lifetime.evaluate(0.0f, rnd(kSaltLifetime), frnd(kSaltLifetime)), 0.01f);
        float life = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
        const auto at = [&](const ParticleFloat& v, uint32_t s) { return v.evaluate(life, rnd(s), frnd(s)); };
        p.timeScale = std::max(at(pe.perParticleTimeScale, kSaltTimeScale), 0.0f);
        const float ts = dt * p.timeScale;
        p.age += ts;
        p.sinceHit += ts;
        life = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);

        float3 pos = {p.position[0], p.position[1], p.position[2]};
        float3 vel = {p.velocity[0], p.velocity[1], p.velocity[2]};
        // Local space: carried along as the effect moved since last frame.
        const float local = fx.moved ? std::clamp(at(pe.localSpace, kSaltLocal), 0.0f, 1.0f) : 0.0f;
        if (local > 0.0f) {
            pos = lerp3(pos, xformPoint(fx.delta, pos), local);
            vel = lerp3(vel, xformDir(fx.delta, vel), local);
        }
        const float damping = at(pe.damping, kSaltDamping);
        if (damping > 0.0f)
            vel = vel * std::exp(-damping * ts);
        // Controllers.
        for (const Effect::Part& part : fx.parts) {
            if (part.component == VfxIndexOf<ParticleAttractor>::value) {
                const auto& a = *static_cast<const ParticleAttractor*>(part.data);
                const float3 to = translation(part.world) - pos;
                const float dist = length(to);
                if ((a.radius > 0.0f && dist > a.radius) || dist < 1e-5f)
                    continue;
                if ((a.flags & kAttractKill) && dist < a.killRadius) {
                    p.flags |= kParticleDead;
                    break;
                }
                float s = at(a.strength, kSaltAttract);
                if ((a.flags & kAttractFalloff) && a.radius > 0.0f)
                    s *= 1.0f - dist / a.radius;
                vel += to * (s * ts / dist);
            } else if (part.component == VfxIndexOf<ParticleNoise>::value) {
                const auto& n = *static_cast<const ParticleNoise*>(part.data);
                const float scroll = float(fx.time) * n.scroll;
                const float3 q = pos * n.frequency + float3{scroll * 0.31f, scroll, scroll * 0.17f};
                vel += curlNoise(q, n.octaves) * (at(n.strength, kSaltNoise) * ts);
            }
        }
        if (p.flags & kParticleDead)
            continue;
        if (force) {
            const float scale = at(pe.forceScale, kSaltForce);
            if (scale != 0.0f) {
                const float3 dir = (pe.flags & kEffectForceLocal) ? xformDir(fx.world, {pe.forceDirection[0], pe.forceDirection[1], pe.forceDirection[2]})
                                                                 : float3{pe.forceDirection[0], pe.forceDirection[1], pe.forceDirection[2]};
                vel += dir * (scale * ts);
                const float ox = at(pe.orbital.x, kSaltOrbitX), oy = at(pe.orbital.y, kSaltOrbitY), oz = at(pe.orbital.z, kSaltOrbitZ);
                if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
                    float3 off = pos - center;
                    const float k = scale * ts * (kPi / 180.0f);
                    off = rotateAbout(off, axes[0], ox * k);
                    off = rotateAbout(off, axes[1], oy * k);
                    off = rotateAbout(off, axes[2], oz * k);
                    pos = center + off;
                }
                const float pull = at(pe.pull, kSaltPull);
                if (pull != 0.0f)
                    vel += (center - pos) * (pull * scale * ts);
            }
        }
        if (hasCm)
            pos += lerp3(cm, cmWorld, local) * ts;
        const float3 move = vel * ts;
        ContactSide side;
        float3 hitPoint, normal;
        float fraction = 1.0f;
        uint64_t body = 0;
        const float radius = std::max(pe.collisionRadius, 0.0f);
        if (collide && dot(move, move) > 1e-12f && physics_->castSphere(pos, radius, move, side, hitPoint, normal, fraction, &body)) {
            // Up to the surface (a hair short), then bounced: the velocity into it reflected by
            // Bounce, along it slowed by Friction, turned a little by Bumpiness.
            pos += move * std::max(fraction - 1e-3f, 0.0f);
            const float into = dot(vel, normal);
            if (into < 0.0f) {
                const float3 vn = normal * into, vt = vel - vn;
                const float bounce = std::clamp(at(pe.bounce, kSaltBounce), 0.0f, 1.0f);
                const float friction = std::clamp(at(pe.friction, kSaltFriction), 0.0f, 1.0f);
                vel = vt * (1.0f - friction) - vn * bounce;
                if (const float bump = at(pe.bumpiness, kSaltBump); bump > 0.0f) {
                    const float speed = length(vel);
                    vel = vel + randomUnit(frnd(kSaltBump), frnd(kSaltBump + 1)) * (speed * bump);
                    if (const float back = dot(vel, normal); back < 0.0f)
                        vel = vel - normal * (2.0f * back);  // (still off the surface)
                }
                ParticleHit h = {};
                std::memcpy(h.point, &hitPoint, 12);
                std::memcpy(h.normal, &normal, 12);
                h.speed = -into;
                h.particle = i;
                h.entity = side.kind == ContactOwner::Entity ? side.id : 0;
                h.surface = side.material;
                out.hits.push_back(h);
                const bool spawnable = p.sinceHit > 0.3f;  // (a particle at rest spawns no more)
                p.flags |= kParticleHit;
                ++p.hits;
                p.sinceHit = 0.0f;
                if (frnd(kSaltDie) < at(pe.dieChance, kSaltDie))
                    p.flags |= kParticleDead;
                if (const float push = at(pe.push, kSaltPush); push > 0.0f && body && !editing)
                    out.impulses.push_back({body, normal * (into * push), hitPoint});
                if (pe.collisionPrefab && spawnable && frnd(kSaltPrefab) < at(pe.prefabChance, kSaltPrefab)) {
                    // Turned to the surface (+Y along its normal, a random yaw about it) or upright.
                    StepOut::Spawn s = {};
                    s.prefab = pe.collisionPrefab;
                    float3 up = {0, 1, 0}, x = {1, 0, 0}, z = {0, 0, 1};
                    if (pe.flags & kEffectPrefabAlign) {
                        up = normal;
                        x = normalize(cross(std::fabs(up.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0}, up));
                        const float yaw = at(pe.prefabYaw, kSaltPrefab + 1) * (kPi / 180.0f);
                        x = rotateAbout(x, up, yaw);
                        z = cross(x, up);
                    }
                    const float3 cols[3] = {x, up, z};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c)
                            s.world[r][c] = (&cols[c].x)[r];
                        s.world[r][3] = (&hitPoint.x)[r];
                    }
                    out.spawns.push_back(s);
                }
            }
        } else {
            pos += move;
        }
        std::memcpy(p.position, &pos, 12);
        std::memcpy(p.velocity, &vel, 12);

        // Colour, size, rotation, sheet.
        if (color) {
            float c[4];
            pe.gradient.evaluate(life, rnd(kSaltGradient), frnd(kSaltGradient), c);
            const float b = at(pe.brightness, kSaltBrightness);
            for (int k = 0; k < 3; ++k)
                p.color[k] = pe.tint[k] * c[k] * b;
            p.color[3] = pe.tint[3] * c[3] * (alpha ? at(pe.alpha, kSaltAlpha) : 1.0f);
        } else {
            p.color[0] = p.color[1] = p.color[2] = 1.0f;
            p.color[3] = alpha ? at(pe.alpha, kSaltAlpha) : 1.0f;
        }
        if (size) {
            const float s = p.size[2] * std::max(at(pe.scale, kSaltScale), 0.0f);
            const float st = at(pe.stretch, kSaltStretch);
            p.size[0] = s * (1.0f + std::max(-st, 0.0f));
            p.size[1] = s * (1.0f + std::max(st, 0.0f));
        } else {
            p.size[0] = p.size[1] = p.size[2];
        }
        if (rotation) {
            p.angles[0] += at(pe.pitch, kSaltPitch) * ts;
            p.angles[1] += at(pe.yaw, kSaltYaw) * ts;
            p.angles[2] += at(pe.roll, kSaltRoll) * ts;
        }
        if (sheet) {
            const float speed = (pe.flags & kEffectSheetFit) ? 1.0f / p.lifetime : at(pe.sequenceSpeed, kSaltSequenceSpeed);
            p.sequence += speed * ts;
        }
        if (p.age >= p.lifetime)
            p.flags |= kParticleDead;
    }
}

void ParticleSystem::update(Scene& scene, float dt, bool editing)
{
    FX_ZONE_CAT("Particles", Animation);
    const uint64_t t0 = SDL_GetTicksNS();
    ++frame_;
    gather(scene);
    stats_ = {};

    struct Range {
        Effect* fx;
        float dt;
        uint32_t first, end;
        StepOut out;
    };
    std::vector<Range> ranges;
    // In entity order (not the map's): the impulses and spawns the steps leave are applied in it,
    // the same on every run.
    std::vector<std::pair<uint64_t, Effect*>> order;
    order.reserve(effects_.size());
    for (auto& [id, fx] : effects_)
        order.push_back({uint64_t(id), &fx});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [id, effect] : order) {
        Effect& fx = *effect;
        if (!fx.effect)
            continue;
        const float fdt = dt * std::max(fx.effect->timeScale, 0.0f);
        // Last frame's flags and hits go.
        fx.hits.clear();
        for (Particle& p : fx.particles)
            p.flags &= ~(kParticleNew | kParticleHit);
        // How it moved since last frame (Local space).
        fx.moved = false;
        if (fx.hasPrev && std::memcmp(fx.prev, fx.world, sizeof(fx.world)) != 0) {
            float inv[3][4];
            inverse(fx.prev, inv);
            multiply(fx.world, inv, fx.delta);
            fx.moved = true;
        }
        if (dt <= 0.0f)
            continue;
        // Pre-warm: its first seconds at once (30 steps a second), here on this thread.
        if (!fx.prewarmed) {
            fx.prewarmed = true;
            const float warm = std::min(fx.effect->preWarm, 30.0f);
            for (float t = 0.0f; t < warm; t += 1.0f / 30.0f) {
                for (Effect::Part& part : fx.parts)
                    if (isEmitter(part.component))
                        emitFrom(fx, part, fx.emitters[(uint64_t(part.entity) << 8) | part.component], 1.0f / 30.0f, editing);
                StepOut warmOut;
                stepEffect(fx, 1.0f / 30.0f, 0, uint32_t(fx.particles.size()), frame_, editing, warmOut);
                recordTrails(fx, 1.0f / 30.0f);
                removeDead(fx);
                fx.time += 1.0 / 30.0;
            }
        }
        if (fx.playing)
            for (Effect::Part& part : fx.parts)
                if (isEmitter(part.component))
                    emitFrom(fx, part, fx.emitters[(uint64_t(part.entity) << 8) | part.component], fdt, editing);
        fx.time += fdt;
        const uint32_t n = uint32_t(fx.particles.size());
        for (uint32_t first = 0; first < n; first += 512)
            ranges.push_back({&fx, fdt, first, std::min(first + 512, n), {}});
    }

    // The particles' steps: on the job threads once there are enough of them.
    if (ranges.size() > 1) {
        enki::TaskSet task(uint32_t(ranges.size()), [&](enki::TaskSetPartition range, uint32_t) {
            FX_ZONE_CAT("Particles step", Animation);
            for (uint32_t i = range.start; i < range.end; ++i)
                stepEffect(*ranges[i].fx, ranges[i].dt, ranges[i].first, ranges[i].end, frame_, editing, ranges[i].out);
        });
        jobs::scheduler().AddTaskSetToPipe(&task);
        jobs::scheduler().WaitforTask(&task);
    } else {
        for (Range& r : ranges)
            stepEffect(*r.fx, r.dt, r.first, r.end, frame_, editing, r.out);
    }
    // What the steps left for this thread: hits (in particle order), pushes and collision prefabs.
    for (Range& r : ranges) {
        r.fx->hits.insert(r.fx->hits.end(), r.out.hits.begin(), r.out.hits.end());
        if (physics_)
            for (const StepOut::Impulse& imp : r.out.impulses)
                physics_->applyImpulse(imp.body, imp.impulse, imp.point);
        if (spawner_ && !editing)
            for (const StepOut::Spawn& s : r.out.spawns)
                spawner_(vfxAssetPath(s.prefab), s.world);
    }

    for (auto& [id, fx] : effects_) {
        if (!fx.effect)
            continue;
        if (dt > 0.0f) {
            const float fdt = dt * std::max(fx.effect->timeScale, 0.0f);
            if (scriptStep_ && !editing)
                for (const Effect::Part& part : fx.parts)
                    if (part.component == VfxIndexOf<ParticleScript>::value)
                        if (const auto& s = *static_cast<const ParticleScript*>(part.data); s.className)
                            scriptStep_(part.entity, vfxAssetPath(s.className), fx.particles.data(), uint32_t(fx.particles.size()), fx.hits.data(),
                                        uint32_t(fx.hits.size()), fdt);
            recordTrails(fx, fdt);
            removeDead(fx);
        }
        std::memcpy(fx.prev, fx.world, sizeof(fx.world));
        fx.hasPrev = true;
        // Done: every emitter finished (none looping) and no particle left.
        bool emitters = false, running = false;
        for (const Effect::Part& part : fx.parts) {
            if (!isEmitter(part.component))
                continue;
            emitters = true;
            const auto it = fx.emitters.find((uint64_t(part.entity) << 8) | part.component);
            const bool done = it != fx.emitters.end() && it->second.finished;
            running = running || !done;
            if (done && !editing && dt > 0.0f && fx.particles.empty() && (emissionOf(part.data).flags & kEmitDestroyOnEnd) &&
                part.entity != fx.entity)
                finished_.push_back(part.entity);
        }
        const bool wasFinished = fx.finished;
        fx.finished = emitters && !running && fx.particles.empty();
        if (fx.finished && dt > 0.0f) {
            if (editing) {
                // Edit mode: shown again a second after it ends.
                fx.idle += dt;
                if (fx.idle >= 1.0f) {
                    ++fx.restarts;
                    start(fx);
                }
            } else if (!wasFinished && (fx.effect->flags & kEffectTemporary)) {
                finished_.push_back(fx.entity);
            }
        }
        ++stats_.effects;
        stats_.particles += uint32_t(fx.particles.size());
    }
    gatherLights();
    stats_.updateMs = double(SDL_GetTicksNS() - t0) * 1e-6;
}

void ParticleSystem::removeDead(Effect& fx)
{
    // (stable: the order particles were born in, their trails beside them)
    size_t out = 0;
    const bool trails = fx.trails.size() == fx.particles.size();
    for (size_t i = 0; i < fx.particles.size(); ++i) {
        if (fx.particles[i].flags & kParticleDead)
            continue;
        if (out != i) {
            fx.particles[out] = fx.particles[i];
            if (trails)
                fx.trails[out] = fx.trails[i];
        }
        ++out;
    }
    fx.particles.resize(out);
    if (trails)
        fx.trails.resize(out);
    else
        fx.trails.clear();
}

void ParticleSystem::recordTrails(Effect& fx, float dt)
{
    // The first Trail Renderer bound says how far apart points go and how long they stay.
    const ParticleTrailRenderer* trail = nullptr;
    for (const Effect::Part& part : fx.parts)
        if (part.component == VfxIndexOf<ParticleTrailRenderer>::value) {
            trail = static_cast<const ParticleTrailRenderer*>(part.data);
            break;
        }
    if (!trail) {
        fx.trails.clear();
        return;
    }
    const uint32_t most = std::clamp(trail->points, 2u, Effect::Trail::kMaxPoints);
    const float spacing = std::max(trail->spacing, 1e-3f), keep = std::max(trail->pointLifetime, 1e-3f);
    const size_t had = std::min(fx.trails.size(), fx.particles.size());
    fx.trails.resize(fx.particles.size());
    for (size_t i = 0; i < fx.particles.size(); ++i) {
        Effect::Trail& t = fx.trails[i];
        const Particle& p = fx.particles[i];
        if (i >= had)
            t.count = 0;
        // Older, and gone after their lifetime.
        uint32_t n = 0;
        for (uint32_t k = 0; k < t.count; ++k) {
            t.points[k][3] += dt;
            if (t.points[k][3] <= keep && n < most)
                std::memcpy(t.points[n++], t.points[k], sizeof(t.points[k]));
        }
        t.count = n;
        if (p.flags & (kParticleDelayed | kParticleDead))
            continue;
        const float3 at = {p.position[0], p.position[1], p.position[2]};
        const bool far = t.count == 0 || length(at - float3{t.points[0][0], t.points[0][1], t.points[0][2]}) >= spacing;
        if (!far)
            continue;
        const uint32_t moved = std::min(t.count, most - 1);
        std::memmove(t.points[1], t.points[0], moved * sizeof(t.points[0]));
        t.points[0][0] = at.x, t.points[0][1] = at.y, t.points[0][2] = at.z, t.points[0][3] = 0.0f;
        t.count = moved + 1;
    }
}

void ParticleSystem::gatherLights()
{
    // Each Light Renderer lights its first particles (up to its most), a point light each.
    lights_.clear();
    for (auto& [id, fx] : effects_) {
        if (!fx.effect)
            continue;
        for (const Effect::Part& part : fx.parts) {
            if (part.component != VfxIndexOf<ParticleLightRenderer>::value)
                continue;
            const auto& r = *static_cast<const ParticleLightRenderer*>(part.data);
            uint32_t n = 0;
            for (const Particle& p : fx.particles) {
                if (n >= r.maxLights)
                    break;
                if (p.flags & kParticleDelayed)
                    continue;
                const float life = std::clamp(p.age / std::max(p.lifetime, 1e-4f), 0.0f, 1.0f);
                const float intensity = std::max(r.intensity.evaluate(life, particleRandom(p.seed, 101), particleRandom(p.seed, 102)), 0.0f);
                const float range = std::max(r.range.evaluate(life, particleRandom(p.seed, 103), particleRandom(p.seed, 104)), 0.01f);
                const bool own = (r.flags & kLightParticleColor) != 0;
                fpak::LightGpu g = {};
                std::memcpy(g.positionRange, p.position, 12);
                g.positionRange[3] = range;
                for (int k = 0; k < 3; ++k)
                    g.color[k] = (own ? p.color[k] * p.color[3] : r.color[k]) * intensity;
                g.color[3] = float(fpak::kLightPoint);
                g.direction[1] = -1.0f;
                g.direction[3] = g.params[0] = -1.0f;
                if (g.color[0] + g.color[1] + g.color[2] <= 0.0f)
                    continue;
                lights_.push_back(g);
                ++n;
            }
        }
    }
}

}  // namespace fx
