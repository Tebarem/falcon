#pragma once

// Particle values, as s&box's ParticleFloat and ParticleGradient: a number (or colour) that is a
// constant, a random range, a curve over the particle's life, or a random pick between two curves.
// Fixed size and trivially copyable, so the particle components hold them in place (flecs, undo
// snapshots, prefabs).

#include <cstdint>

namespace fx {

struct Json;
class JsonWriter;

// A curve: up to 8 keys, by t (0..1), Hermite between them (in/out: the slopes either side of a key).
struct CurveKey {
    float t = 0.0f, value = 0.0f, in = 0.0f, out = 0.0f;
};
struct Curve {
    static constexpr uint32_t kMaxKeys = 8;
    uint32_t count = 0;  // 0: 0 everywhere
    CurveKey keys[kMaxKeys] = {};
    float evaluate(float t) const;
    static Curve constant(float v);
    static Curve line(float from, float to);
};

// What a particle value is evaluated at.
enum class ParticleEval : uint8_t {
    Life,   // the particle's normalized age (0 born .. 1 dead)
    Seed,   // a random number fixed for the particle (and for this value)
    Frame,  // a new random number every evaluation
};

struct ParticleFloat {
    enum Mode : uint8_t { Constant, Range, CurveMode, CurveRange };
    uint8_t mode = Constant;
    ParticleEval eval = ParticleEval::Seed;
    uint8_t pad[2] = {};
    float a = 0.0f, b = 0.0f;  // Constant: a; Range: a .. b
    Curve curveA, curveB;      // Curve: A; CurveRange: between A and B (by the seed)

    ParticleFloat() = default;
    ParticleFloat(float v) : a(v), b(v) {}  // a constant
    static ParticleFloat range(float lo, float hi);
    static ParticleFloat curve(const Curve& c);  // over the particle's life

    // life: the normalized age; seed: the particle's fixed random (0..1); frame: a fresh random.
    float evaluate(float life, float seed, float frame) const;
    bool isConstant() const { return mode == Constant; }
    bool isNearlyZero() const;
};

struct ParticleVec3 {
    ParticleFloat x, y, z;
};

struct GradientColorKey {
    float t = 0.0f, r = 1.0f, g = 1.0f, b = 1.0f;
};
struct GradientAlphaKey {
    float t = 0.0f, a = 1.0f;
};
// Colour and alpha keys of their own (linear colour), linear between keys.
struct Gradient {
    static constexpr uint32_t kMaxKeys = 8;
    uint32_t colors = 1, alphas = 1;
    GradientColorKey color[kMaxKeys] = {};
    GradientAlphaKey alpha[kMaxKeys] = {};
    void evaluate(float t, float out[4]) const;
};

struct ParticleGradient {
    enum Mode : uint8_t { Constant, Range, GradientMode, GradientRange };
    uint8_t mode = Constant;
    ParticleEval eval = ParticleEval::Life;
    uint8_t pad[2] = {};
    float a[4] = {1, 1, 1, 1}, b[4] = {1, 1, 1, 1};  // Constant: a; Range: a .. b (by the seed)
    Gradient gradientA, gradientB;                   // Gradient: A over life; GradientRange: A .. B (by the seed)
    void evaluate(float life, float seed, float frame, float out[4]) const;
};

// A fixed random number (0..1) for one particle and one of its values: `salt` tells the values apart.
float particleRandom(uint32_t seed, uint32_t salt);

// JSON: a constant is a bare number (a colour: an [r, g, b, a] array); anything else an object
// {"mode": "range" | "curve" | "curveRange", "eval": "life" | "seed" | "frame", "a", "b",
//  "curveA": [[t, value, in, out], ...], "curveB"} (gradients: "gradientA": {"colors": [[t, r, g, b]...],
//  "alphas": [[t, a]...]}).
bool readParticleFloat(const Json& j, ParticleFloat& out);
void writeParticleFloat(JsonWriter& w, const char* key, const ParticleFloat& v);
bool readParticleGradient(const Json& j, ParticleGradient& out);
void writeParticleGradient(JsonWriter& w, const char* key, const ParticleGradient& v);
bool operator==(const ParticleFloat& a, const ParticleFloat& b);
bool operator==(const ParticleGradient& a, const ParticleGradient& b);

}  // namespace fx
