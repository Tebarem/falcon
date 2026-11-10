#include "water/ocean_spectrum.h"

#include "assets/water_asset.h"
#include "core/dmath.h"
#include "water/water_system.h"

#include <algorithm>
#include <cmath>

namespace fx {

namespace {

constexpr float kG = 9.81f;
constexpr float kFetch = 100000.0f;  // metres of open water the wind blows over

uint32_t hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t h = a * 0x9e3779b9u ^ b * 0x85ebca6bu ^ c * 0xc2b2ae35u ^ d * 0x27d4eb2fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float unit01(uint32_t h) { return (float(h >> 8) + 0.5f) / 16777216.0f; }

// Two independent standard normal numbers (Box-Muller).
void gaussian(uint32_t seed, uint32_t c, uint32_t x, uint32_t y, float& a, float& b)
{
    const float u1 = unit01(hash(seed, c, x, y)), u2 = unit01(hash(seed + 1, c, x, y));
    const float r = std::sqrt(-2.0f * dmath::log(u1)), t = 2.0f * kPi * u2;
    a = r * dmath::cos(t);
    b = r * dmath::sin(t);
}

// JONSWAP (Hasselmann et al. 1973): energy per angular frequency for a wind, peak `wp`.
float jonswap(float w, float wp, float alpha)
{
    if (w <= 0.0f)
        return 0.0f;
    const float sigma = w <= wp ? 0.07f : 0.09f;
    const float r = dmath::exp(-(w - wp) * (w - wp) / (2.0f * sigma * sigma * wp * wp));
    return alpha * kG * kG / dmath::pow(w, 5.0f) * dmath::exp(-1.25f * dmath::pow(wp / w, 4.0f)) * dmath::pow(3.3f, r);
}

}  // namespace

void OceanSpectrum::build(const WaterMaterial& m, uint32_t size, uint32_t seed)
{
    n = size;
    choppiness = m.choppiness;
    const float wind = std::max(m.windSpeed, 0.5f);
    const float wp = 22.0f * dmath::pow(kG * kG / (wind * kFetch), 1.0f / 3.0f);
    const float alpha = 0.076f * dmath::pow(wind * wind / (kFetch * kG), 0.22f);
    const float peak = 2.0f * kPi * kG / (wp * wp);  // the peak wavelength
    sizes[0] = std::clamp(peak * 4.0f, 64.0f, 2048.0f);
    sizes[1] = sizes[0] / 6.17f;
    sizes[2] = sizes[1] / 6.37f;
    // Each cascade its own band of wavenumbers (no wave twice): up to 4 waves per next cascade.
    const float edges[kCascades + 1] = {0.0f, 2.0f * kPi * 4.0f / sizes[1], 2.0f * kPi * 4.0f / sizes[2], 1e30f};
    const float windAngle = m.windDirection * kPi / 180.0f;
    // Spreading cos^2s of the half angle: s 1 (waves from everywhere) to 24 (all along the wind).
    const float s = 1.0f + (1.0f - std::clamp(m.waveSpread, 0.0f, 1.0f)) * 23.0f;
    const float spreadNorm = dmath::exp(2.0f * dmath::lgamma(s + 1.0f) - dmath::lgamma(2.0f * s + 1.0f)) * dmath::pow(2.0f, 2.0f * s - 1.0f) / kPi;
    const float w0 = float(2.0 * 3.14159265358979 / WaterSystem::kTimeWrap);

    cpuWaves.clear();
    std::vector<std::pair<float, Wave>> all;
    double variance = 0.0;
    for (uint32_t c = 0; c < kCascades; ++c) {
        const float dk = 2.0f * kPi / sizes[c];
        const float nyquist = kPi * float(n) / sizes[c];
        std::vector<float> amp(size_t(n) * n * 2, 0.0f);  // h0(k), complex
        omega[c].assign(size_t(n) * n, 0.0f);
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x) {
                const float kx = (float(x) - float(n / 2)) * dk, kz = (float(y) - float(n / 2)) * dk;
                const float k = std::sqrt(kx * kx + kz * kz);
                if (k < std::max(edges[c], 1e-6f) || k >= edges[c + 1] || k > nyquist)
                    continue;
                const float w = std::max(1.0f, std::round(std::sqrt(kG * k) / w0)) * w0;
                const float theta = dmath::atan2(kz, kx) - windAngle;
                const float spread = spreadNorm * dmath::pow(std::fabs(dmath::cos(theta * 0.5f)), 2.0f * s);
                const float e = jonswap(std::sqrt(kG * k), wp, alpha) * spread * (kG / (2.0f * std::sqrt(kG * k))) / k;
                const float sigma = std::sqrt(std::max(e, 0.0f) * dk * dk * 0.5f) * m.waveHeight;
                float a, b;
                gaussian(seed, c, x, y, a, b);
                amp[(size_t(y) * n + x) * 2 + 0] = a * sigma;
                amp[(size_t(y) * n + x) * 2 + 1] = b * sigma;
                omega[c][size_t(y) * n + x] = w;
                variance += double(sigma) * sigma * 2.0;
            }
        // h0(k) and conj(h0(-k)) per texel (-k: the texel mirrored about n / 2; none past the edge).
        h0[c].assign(size_t(n) * n * 4, 0.0f);
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x) {
                const size_t i = size_t(y) * n + x;
                float* o = &h0[c][i * 4];
                o[0] = amp[i * 2];
                o[1] = amp[i * 2 + 1];
                if (x > 0 && y > 0) {
                    const size_t j = size_t(n - y) * n + (n - x);
                    o[2] = amp[j * 2];
                    o[3] = -amp[j * 2 + 1];
                }
                const float mag = std::sqrt(o[0] * o[0] + o[1] * o[1]) + std::sqrt(o[2] * o[2] + o[3] * o[3]);
                if (mag > 0.0f)
                    all.push_back({mag, {(float(x) - float(n / 2)) * dk, (float(y) - float(n / 2)) * dk, o[0], o[1], o[2], o[3], omega[c][i]}});
            }
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < all.size() && i < kCpuWaves; ++i)
        cpuWaves.push_back(all[i].second);
    maxAmplitude = 3.0f * float(std::sqrt(variance));
    ++revision;
}

void OceanSpectrum::sample(float x, float z, float t, float& height, float3& normal) const
{
    auto evaluate = [&](float sx, float sz, float& dx, float& h, float& dz, float& hx, float& hz) {
        dx = h = dz = hx = hz = 0.0f;
        for (const Wave& w : cpuWaves) {
            const float ct = dmath::cos(w.omega * t), st = dmath::sin(w.omega * t);
            // h(k, t) = h0 e^(i w t) + conj(h0(-k)) e^(-i w t)
            const float hr = w.h0r * ct - w.h0i * st + w.h0mr * ct + w.h0mi * st;
            const float hi = w.h0r * st + w.h0i * ct - w.h0mr * st + w.h0mi * ct;
            const float ph = w.kx * sx + w.kz * sz;
            const float cp = dmath::cos(ph), sp = dmath::sin(ph);
            const float re = hr * cp - hi * sp, im = hr * sp + hi * cp;  // h e^(i k.x)
            const float k = std::sqrt(w.kx * w.kx + w.kz * w.kz);
            h += re;
            dx += im * w.kx / k * choppiness;  // Re(-i k/|k| c)
            dz += im * w.kz / k * choppiness;
            hx -= im * w.kx;  // Re(i k c)
            hz -= im * w.kz;
        }
    };
    float sx = x, sz = z, dx, h, dz, hx, hz;
    for (int it = 0; it < 2; ++it) {
        evaluate(sx, sz, dx, h, dz, hx, hz);
        sx = x - dx;
        sz = z - dz;
    }
    evaluate(sx, sz, dx, h, dz, hx, hz);
    height = h;
    normal = normalize(float3{-hx, 1.0f, -hz});
}

}  // namespace fx
