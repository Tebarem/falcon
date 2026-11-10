// `falcon vfx-test`: the particle values (curves, gradients, every ParticleFloat mode and
// evaluation) and the components' JSON and flecs round trips. Prints what fails; 0 when all pass.

#include "vfx/vfx_selftest.h"

#include "core/json.h"
#include "vfx/particle_value.h"
#include "vfx/vfx_components.h"

#include <flecs.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace fx {

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("vfx-test: FAILED %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void curves()
{
    check(near(Curve{}.evaluate(0.3f), 0.0f), "an empty curve is 0");
    check(near(Curve::constant(2.5f).evaluate(0.7f), 2.5f), "a one-key curve is its value");
    const Curve line = Curve::line(1.0f, 3.0f);
    check(near(line.evaluate(0.0f), 1.0f) && near(line.evaluate(0.5f), 2.0f) && near(line.evaluate(1.0f), 3.0f), "a line's tangents keep it straight");
    check(near(line.evaluate(-1.0f), 1.0f) && near(line.evaluate(2.0f), 3.0f), "a curve holds its ends outside 0..1");
    Curve ease;
    ease.count = 2;
    ease.keys[0] = {0.0f, 0.0f, 0.0f, 0.0f};
    ease.keys[1] = {1.0f, 1.0f, 0.0f, 0.0f};
    check(near(ease.evaluate(0.5f), 0.5f) && ease.evaluate(0.25f) < 0.25f && ease.evaluate(0.75f) > 0.75f, "flat tangents ease in and out");
    Curve three;
    three.count = 3;
    three.keys[0] = {0.0f, 0.0f};
    three.keys[1] = {0.5f, 4.0f};
    three.keys[2] = {1.0f, 0.0f};
    check(near(three.evaluate(0.5f), 4.0f), "a middle key is hit exactly");
}

void floats()
{
    const ParticleFloat c = 3.0f;
    check(c.isConstant() && near(c.evaluate(0.2f, 0.9f, 0.1f), 3.0f), "a constant");
    ParticleFloat r = ParticleFloat::range(2.0f, 4.0f);
    check(r.eval == ParticleEval::Seed && near(r.evaluate(0.0f, 0.25f, 0.9f), 2.5f), "a range picks by the seed");
    r.eval = ParticleEval::Life;
    check(near(r.evaluate(0.5f, 0.0f, 0.0f), 3.0f), "a range over life");
    r.eval = ParticleEval::Frame;
    check(near(r.evaluate(0.0f, 0.0f, 1.0f), 4.0f), "a range by the frame's random");
    const ParticleFloat curve = ParticleFloat::curve(Curve::line(0.0f, 10.0f));
    check(curve.eval == ParticleEval::Life && near(curve.evaluate(0.3f, 0.8f, 0.5f), 3.0f), "a curve over life");
    ParticleFloat cr;
    cr.mode = ParticleFloat::CurveRange;
    cr.eval = ParticleEval::Life;
    cr.curveA = Curve::constant(0.0f);
    cr.curveB = Curve::constant(10.0f);
    check(near(cr.evaluate(0.3f, 0.5f, 0.0f), 5.0f) && near(cr.evaluate(0.9f, 0.5f, 1.0f), 5.0f), "a curve range picks between them by the seed");
    check(ParticleFloat(0.0f).isNearlyZero() && !ParticleFloat::range(0.0f, 0.0f).isNearlyZero(), "only a constant 0 is nearly zero");

    // The random: in [0, 1), spread, different per salt, the same for the same inputs.
    double sum = 0.0;
    float lo = 1.0f, hi = 0.0f;
    for (uint32_t i = 0; i < 10000; ++i) {
        const float v = particleRandom(i, 3);
        sum += v;
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }
    check(lo >= 0.0f && hi < 1.0f && lo < 0.01f && hi > 0.99f && std::fabs(sum / 10000.0 - 0.5) < 0.02, "particleRandom spreads over 0..1");
    check(particleRandom(7, 1) != particleRandom(7, 2) && particleRandom(7, 1) == particleRandom(7, 1), "particleRandom: salts differ, inputs repeat");
}

void gradients()
{
    Gradient g;
    g.colors = 2;
    g.color[0] = {0.0f, 1.0f, 0.0f, 0.0f};
    g.color[1] = {1.0f, 0.0f, 0.0f, 1.0f};
    g.alphas = 2;
    g.alpha[0] = {0.0f, 1.0f};
    g.alpha[1] = {0.5f, 0.0f};
    float c[4];
    g.evaluate(0.5f, c);
    check(near(c[0], 0.5f) && near(c[2], 0.5f) && near(c[3], 0.0f), "a gradient between its keys");
    g.evaluate(0.25f, c);
    check(near(c[3], 0.5f), "alpha keys of their own");
    ParticleGradient pg;
    pg.mode = ParticleGradient::GradientMode;
    pg.gradientA = g;
    pg.evaluate(1.0f, 0.3f, 0.0f, c);
    check(near(c[0], 0.0f) && near(c[2], 1.0f), "a particle gradient over life");
    pg.mode = ParticleGradient::Range;
    const float a[4] = {0, 0, 0, 0}, b[4] = {1, 1, 1, 1};
    std::memcpy(pg.a, a, sizeof(a));
    std::memcpy(pg.b, b, sizeof(b));
    pg.evaluate(0.9f, 0.25f, 0.0f, c);
    check(near(c[1], 0.25f), "a colour range picks by the seed");
}

VfxSet sample()
{
    VfxSet set;
    auto* effect = static_cast<ParticleEffect*>(set.add(VfxIndexOf<ParticleEffect>::value));
    effect->maxParticles = 250;
    effect->flags |= kEffectColor | kEffectCollision | kEffectSheetFit;
    effect->lifetime = ParticleFloat::range(0.5f, 1.5f);
    effect->scale = ParticleFloat::curve(Curve::line(1.0f, 0.0f));
    effect->initialVelocity.y = ParticleFloat::range(2.0f, 3.0f);
    effect->gradient.mode = ParticleGradient::GradientMode;
    effect->gradient.gradientA.colors = 2;
    effect->gradient.gradientA.color[1] = {1.0f, 0.25f, 0.5f, 0.75f};
    effect->collisionPrefab = vfxAssetId("Assets/VFX/Spark.fprefab");
    auto* cone = static_cast<ParticleConeEmitter*>(set.add(VfxIndexOf<ParticleConeEmitter>::value));
    cone->emission.flags = kEmitOnEdge;
    cone->angle = 12.0f;
    auto* sprite = static_cast<ParticleSpriteRenderer*>(set.add(VfxIndexOf<ParticleSpriteRenderer>::value));
    sprite->material = vfxAssetId("Assets/VFX/Smoke.fmat");
    sprite->blend = uint32_t(SpriteBlend::Additive);
    sprite->columns = sprite->rows = 8;
    auto* decal = static_cast<Decal*>(set.add(VfxIndexOf<Decal>::value));
    decal->order = -3;
    return set;
}

void json()
{
    const VfxSet set = sample();
    JsonWriter w;
    w.beginObject().value("name", "Effect");
    writeVfxJson(w, set);
    w.endObject();
    const Json j = Json::parse(w.str());
    VfxSet back;
    readVfxJson(j, back);
    check(back == set, "components survive a JSON round trip");
    check(j.get("particleEffect") && j.get("particleEffect")->getNumber("maxParticles") == 250.0, "fields are written by key");
    check(j.get("particleEffect") && !j.get("particleEffect")->get("timeScale"), "fields at their defaults are left out");
    check(j.get("particleSprite") && j.get("particleSprite")->getString("blend") == "additive", "enums are written by name");
    check(j.get("particleSprite") && j.get("particleSprite")->getString("material") == "Assets/VFX/Smoke.fmat", "assets are written by path");

    // A bare number is a constant; a wrong type leaves the field alone.
    VfxSet one;
    readVfxJson(Json::parse(R"({"particleEffect": {"lifetime": 2.5, "maxParticles": "lots"}})"), one);
    const auto* e = one.get<ParticleEffect>();
    check(e && e->lifetime.isConstant() && near(e->lifetime.a, 2.5f) && e->maxParticles == 1000, "a bare number is a constant; a bad value is ignored");

    // One field, as an editor message sets it.
    uint32_t count = 0;
    const VfxComponentInfo* infos = vfxComponents(count);
    check(count == kVfxComponentCount && vfxComponentIndex("particleSprite") == int32_t(VfxIndexOf<ParticleSpriteRenderer>::value), "the components by key");
    ParticleSpriteRenderer s;
    const VfxComponentInfo& info = infos[VfxIndexOf<ParticleSpriteRenderer>::value];
    for (uint32_t i = 0; i < info.fieldCount; ++i) {
        if (std::strcmp(info.fields[i].key, "lit") == 0)
            check(readVfxField(info.fields[i], &s, Json::parse("true")) && (s.flags & kSpriteLit), "a bool field sets its bit");
        if (std::strcmp(info.fields[i].key, "alignment") == 0)
            check(readVfxField(info.fields[i], &s, Json::parse("\"flat\"")) && s.alignment == uint32_t(SpriteAlign::Flat), "an enum by name");
        if (std::strcmp(info.fields[i].key, "columns") == 0)
            check(readVfxField(info.fields[i], &s, Json::parse("500")) && s.columns == 32, "a count is clamped to its range");
    }
}

void ecs()
{
    flecs::world world;
    registerVfxComponents(world);
    const VfxSet set = sample();
    flecs::entity e = world.entity("Effect");
    setVfx(e, set);
    check(e.has<ParticleEffect>() && e.has<Decal>() && !e.has<ParticleNoise>(), "set on an entity");
    check(captureVfx(e) == set, "captured as set");
    check(e.get<ParticleEffect>().maxParticles == 250, "the component's data");
    VfxSet less = set;
    less.remove(VfxIndexOf<Decal>::value);
    setVfx(e, less);
    check(!e.has<Decal>() && e.has<ParticleSpriteRenderer>(), "set again: what is left out goes");
}

}  // namespace

int vfxSelfTest()
{
    failures = 0;
    curves();
    floats();
    gradients();
    json();
    ecs();
    std::printf(failures ? "vfx-test: %d failed\n" : "vfx-test: all passed\n", failures);
    return failures ? 1 : 0;
}

}  // namespace fx
