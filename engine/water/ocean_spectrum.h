#pragma once

// The ocean's waves as a spectrum (Tessendorf, "Simulating Ocean Water"): a JONSWAP spectrum for
// the wind (speed, direction, spread), as random amplitudes h0(k) on three cascades, squares of
// the sea repeating at different sizes, each N x N waves (the GPU sums them with an FFT every
// frame: water_fft.slang). The same amplitudes are made here for the CPU, which evaluates the
// strongest waves directly: the height of the sea anywhere (physics, scripts), without reading
// the GPU's back. Frequencies are multiples of 2 pi / WaterSystem::kTimeWrap, so the sea repeats
// exactly after it and the clock can wrap.

#include "core/math.h"

#include <cstdint>
#include <vector>

namespace fx {

struct WaterMaterial;

struct OceanSpectrum {
    static constexpr uint32_t kCascades = 3;
    static constexpr uint32_t kCpuWaves = 64;  // the strongest waves, for the CPU's heights

    uint32_t n = 0;                 // waves per cascade side
    float sizes[kCascades] = {};    // metres per cascade square
    float choppiness = 0.0f;
    // Per cascade, per texel (k centred at n / 2): h0(k), conj(h0(-k)) as 4 floats, then omega
    // per texel: what the GPU's h0 buffer holds (n * n * 16 bytes, then n * n * 4).
    std::vector<float> h0[kCascades];
    std::vector<float> omega[kCascades];
    // The strongest waves over all cascades: wavenumber, h0(k), conj(h0(-k)), omega.
    struct Wave {
        float kx, kz, h0r, h0i, h0mr, h0mi, omega;
    };
    std::vector<Wave> cpuWaves;
    float maxAmplitude = 0.0f;  // an upper bound of the height (bounds, culling)
    uint64_t revision = 0;      // changes when the spectrum does

    // From a material's waves (wind speed and direction, height, choppiness, spread) and a seed.
    void build(const WaterMaterial& m, uint32_t n, uint32_t seed);
    bool valid() const { return n != 0; }

    // The displaced surface's height and normal over world (x, z) at time t, from cpuWaves
    // (the still-surface point found by two fixed-point steps, as for the Gerstner waves).
    void sample(float x, float z, float t, float& height, float3& normal) const;
};

}  // namespace fx
