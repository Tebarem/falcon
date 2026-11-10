#include "audio/procedural_synth.h"

#include <algorithm>
#include <cmath>

namespace fx {

namespace {

constexpr float kRate = float(kAudioSampleRate);

struct Random {
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed * 2654435761u + 0x9e3779b9u) {}
    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float unit() { return float(next() >> 8) / float(1u << 24); }  // 0 .. 1
    float signedUnit() { return unit() * 2.0f - 1.0f; }            // -1 .. 1
};

// One-pole filters, cutoff in Hz.
struct LowPass {
    float a, y = 0.0f;
    explicit LowPass(float hz) : a(1.0f - std::exp(-2.0f * kPi * std::min(hz, kRate * 0.45f) / kRate)) {}
    float operator()(float x) { return y += a * (x - y); }
};
struct HighPass {
    LowPass low;
    explicit HighPass(float hz) : low(hz) {}
    float operator()(float x) { return x - low(x); }
};

AudioClip clipOf(std::vector<float> samples, float peak)
{
    float m = 0.0f;
    for (float s : samples)
        m = std::max(m, std::fabs(s));
    if (m > 0.0f)
        for (float& s : samples)
            s *= peak / m;
    AudioClip c;
    c.path = "synth";
    c.channels = 1;
    c.frames = uint32_t(samples.size());
    c.samples = std::move(samples);
    return c;
}

// A loop: its last `fade` frames crossfaded into its first, so it repeats without a seam.
std::vector<float> loopable(std::vector<float> s, uint32_t fade)
{
    const uint32_t n = uint32_t(s.size());
    if (n <= 2 * fade)
        return s;
    for (uint32_t i = 0; i < fade; ++i) {
        const float t = float(i) / float(fade);
        s[i] = s[i] * t + s[n - fade + i] * (1.0f - t);
    }
    s.resize(n - fade);
    return s;
}

}  // namespace

AudioClip synthImpact(const SurfaceSound& s, uint32_t seed)
{
    Random rng(seed);
    const float hard = std::clamp(s.hardness, 0.0f, 1.0f);
    const float duration = std::clamp(s.decay * 6.0f + 0.06f, 0.08f, 2.0f);
    const uint32_t n = uint32_t(duration * kRate);
    std::vector<float> out(n, 0.0f);
    // Modes: a bar's or plate's inharmonic ratios (hard: brighter, more of them).
    static const float kRatios[2][5] = {{1.0f, 2.57f, 4.86f, 7.9f, 11.3f}, {1.0f, 2.76f, 5.40f, 8.93f, 13.3f}};
    const float* ratios = kRatios[hard > 0.8f ? 1 : 0];
    const int modes = 2 + int(hard * 3.0f);
    const float tone = 1.0f - std::clamp(s.noise, 0.0f, 1.0f);
    for (int k = 0; k < modes; ++k) {
        const float f = s.frequency * ratios[k] * (1.0f + 0.03f * rng.signedUnit());
        if (f > kRate * 0.45f)
            break;
        const float amp = tone * std::pow(0.35f + 0.6f * hard, float(k));
        const float tau = std::max(s.decay / (1.0f + 0.8f * float(k)), 0.004f);
        const float phase = rng.unit() * 2.0f * kPi;
        const float w = 2.0f * kPi * f / kRate;
        for (uint32_t i = 0; i < n; ++i)
            out[i] += amp * std::exp(-float(i) / (tau * kRate)) * std::sin(w * float(i) + phase);
    }
    // The transient: noise, darker and longer on soft surfaces.
    {
        LowPass lp(400.0f + 7000.0f * hard * hard);
        const float tau = 0.003f + 0.025f * (1.0f - hard);
        const float amp = 0.3f + 1.2f * s.noise;
        for (uint32_t i = 0; i < n; ++i)
            out[i] += amp * std::exp(-float(i) / (tau * kRate)) * lp(rng.signedUnit());
    }
    // A thud under soft hits.
    {
        const float f = 70.0f + 80.0f * rng.unit();
        const float amp = 0.8f * (1.0f - hard);
        for (uint32_t i = 0; i < n; ++i)
            out[i] += amp * std::exp(-float(i) / (0.03f * kRate)) * std::sin(2.0f * kPi * f * float(i) / kRate);
    }
    // 1 ms attack, so it starts without a click.
    for (uint32_t i = 0; i < std::min<uint32_t>(48, n); ++i)
        out[i] *= float(i) / 48.0f;
    return clipOf(std::move(out), 0.7f);
}

AudioClip synthScrape(const SurfaceSound& s, uint32_t seed)
{
    Random rng(seed);
    const float hard = std::clamp(s.hardness, 0.0f, 1.0f), rough = std::clamp(s.roughness, 0.0f, 1.0f);
    const uint32_t n = uint32_t(2.05f * kRate);
    std::vector<float> out(n, 0.0f);
    HighPass hp(200.0f + 1200.0f * (1.0f - rough));
    LowPass lp(1500.0f + 6000.0f * hard);
    // A bed of noise, and grains: short clicks at a rate that rises with roughness.
    const float grainRate = 40.0f + 500.0f * rough;
    float grain = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (rng.unit() < grainRate / kRate)
            grain += (0.5f + rng.unit()) * (0.4f + rough);
        grain *= 0.96f;
        out[i] = lp(hp(rng.signedUnit() * (0.35f + grain)));
    }
    return clipOf(loopable(std::move(out), uint32_t(0.05f * kRate)), 0.6f);
}

AudioClip synthRoll(const SurfaceSound& s, uint32_t seed)
{
    Random rng(seed);
    const float hard = std::clamp(s.hardness, 0.0f, 1.0f), rough = std::clamp(s.roughness, 0.0f, 1.0f);
    const uint32_t n = uint32_t(2.05f * kRate);
    std::vector<float> out(n, 0.0f);
    LowPass rumble(60.0f + 120.0f * hard), rumble2(60.0f + 120.0f * hard);
    LowPass surface(800.0f + 2500.0f * hard);
    HighPass surfaceHp(300.0f);
    float bump = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (rng.unit() < (8.0f + 60.0f * rough) / kRate)
            bump += 0.6f + rng.unit();
        bump *= 0.995f;
        const float low = rumble2(rumble(rng.signedUnit())) * 6.0f;
        const float high = surface(surfaceHp(rng.signedUnit())) * (0.15f + 0.3f * rough);
        out[i] = low * (0.6f + bump) + high;
    }
    return clipOf(loopable(std::move(out), uint32_t(0.05f * kRate)), 0.6f);
}

namespace {

// A bubble ringing from frame `start`: Minnaert's frequency for its radius (mm), rising as it nears
// the surface, damped over a few cycles.
void bubble(std::vector<float>& out, uint32_t start, float radiusMm, float amp, Random& rng)
{
    const float f0 = 3260.0f / std::max(radiusMm, 0.3f);
    if (f0 > kRate * 0.45f)
        return;
    const float tau = 0.003f + 0.003f * radiusMm;
    const float rise = 0.15f + 0.3f * rng.unit();
    const uint32_t n = std::min<uint32_t>(uint32_t(5.0f * tau * kRate), uint32_t(out.size()) - std::min<uint32_t>(start, uint32_t(out.size())));
    float phase = rng.unit() * 2.0f * kPi;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = float(i) / kRate;
        phase += 2.0f * kPi * f0 * (1.0f + rise * std::min(t / (3.0f * tau), 1.0f)) / kRate;
        out[start + i] += amp * std::exp(-t / tau) * std::sin(phase) * std::min(float(i) / 24.0f, 1.0f);
    }
}

}  // namespace

AudioClip synthSplash(float size, uint32_t seed)
{
    Random rng(seed);
    const float big = std::clamp(std::log2(std::max(size, 0.01f) / 0.04f) / 5.0f, 0.0f, 1.0f);  // 4 cm .. 1.3 m
    const float duration = 0.3f + 1.4f * big;
    const uint32_t n = uint32_t(duration * kRate);
    std::vector<float> out(n, 0.0f);
    // The slap: bright and short for a pebble, darker for a body.
    {
        LowPass lp(6000.0f - 4500.0f * big);
        HighPass hp(120.0f);
        const float tau = 0.006f + 0.03f * big;
        for (uint32_t i = 0; i < n; ++i)
            out[i] += 0.6f * std::exp(-float(i) / (tau * kRate)) * hp(lp(rng.signedUnit()));
    }
    // The churn: the cavity closing, water thrown up (band-limited noise, longer for big ones).
    {
        LowPass lp(1800.0f - 900.0f * big), lp2(1800.0f - 900.0f * big);
        HighPass hp(250.0f - 150.0f * big);
        const float attack = 0.01f + 0.05f * big, tau = 0.05f + 0.35f * big;
        for (uint32_t i = 0; i < n; ++i) {
            const float t = float(i) / kRate;
            const float env = std::min(t / attack, 1.0f) * std::exp(-std::max(t - attack, 0.0f) / tau);
            out[i] += (0.3f + 1.2f * big) * env * hp(lp2(lp(rng.signedUnit())));
        }
    }
    // Bubbles: a pebble's one or two clear plips, a body's burst of many, bigger and lower.
    {
        const int count = 1 + int(2.0f * rng.unit()) + int(40.0f * big * big);
        for (int k = 0; k < count; ++k) {
            const float when = k == 0 ? 0.01f + 0.02f * rng.unit() : rng.unit() * rng.unit() * duration * 0.6f;
            const float radius = 0.8f + rng.unit() * (2.0f + 10.0f * big);
            bubble(out, uint32_t(when * kRate), radius, (k == 0 ? 0.9f : 0.35f) * (0.5f + rng.unit()), rng);
        }
    }
    // A thump under big ones, and the drops falling back.
    if (big > 0.3f) {
        const float f = 55.0f + 30.0f * rng.unit();
        for (uint32_t i = 0; i < n; ++i)
            out[i] += 1.2f * (big - 0.3f) * std::exp(-float(i) / (0.08f * kRate)) * std::sin(2.0f * kPi * f * float(i) / kRate);
        const int drops = int(30.0f * big);
        for (int k = 0; k < drops; ++k) {
            const float when = 0.25f * duration + rng.unit() * 0.6f * duration;
            bubble(out, uint32_t(when * kRate), 0.6f + 2.0f * rng.unit(), 0.15f + 0.2f * rng.unit(), rng);
        }
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(48, n); ++i)
        out[i] *= float(i) / 48.0f;
    return clipOf(std::move(out), 0.7f);
}

AudioClip synthSlosh(uint32_t seed)
{
    Random rng(seed);
    const uint32_t n = uint32_t(3.1f * kRate);
    std::vector<float> out(n, 0.0f);
    // Swirling water: low-passed noise swelling a few times a second, with gurgles.
    LowPass lp(900.0f), lp2(900.0f);
    HighPass hp(150.0f);
    float swell = 0.5f, target = 0.5f;
    for (uint32_t i = 0; i < n; ++i) {
        if (rng.unit() < 4.0f / kRate)
            target = 0.2f + rng.unit();
        swell += (target - swell) * (6.0f / kRate);
        out[i] = hp(lp2(lp(rng.signedUnit()))) * 3.0f * swell;
    }
    const int gurgles = 18;
    for (int k = 0; k < gurgles; ++k)
        bubble(out, uint32_t(rng.unit() * float(n)), 2.0f + 6.0f * rng.unit(), 0.25f + 0.25f * rng.unit(), rng);
    return clipOf(loopable(std::move(out), uint32_t(0.1f * kRate)), 0.6f);
}

AudioClip synthUnderwater(uint32_t seed)
{
    Random rng(seed);
    const uint32_t n = uint32_t(6.2f * kRate);
    std::vector<float> out(n, 0.0f);
    LowPass lp(180.0f), lp2(180.0f);
    float swell = 0.6f, target = 0.6f;
    for (uint32_t i = 0; i < n; ++i) {
        if (rng.unit() < 0.7f / kRate)
            target = 0.4f + 0.6f * rng.unit();
        swell += (target - swell) * (1.0f / kRate);
        out[i] = lp2(lp(rng.signedUnit())) * 10.0f * swell;
    }
    for (int k = 0; k < 6; ++k)
        bubble(out, uint32_t(rng.unit() * float(n)), 6.0f + 14.0f * rng.unit(), 0.2f, rng);
    return clipOf(loopable(std::move(out), uint32_t(0.2f * kRate)), 0.5f);
}

}  // namespace fx
