#pragma once

// Deterministic transcendental functions: the same bits on every x64 PC, whatever its CPU or
// Windows version. The C runtime's sin/cos/exp/pow pick FMA paths by CPU and change with
// ucrtbase updates, so the simulation (physics forces, animation that hitboxes follow,
// buoyancy) uses these instead. Only +, -, *, / and sqrt are used (IEEE exact under SSE2,
// /fp:precise, no contraction), evaluated in double and rounded once.
// The managed twin is Falcon.DMath (same algorithms).

#include <cstdint>

namespace fx::dmath {

double sin(double x);
double cos(double x);
void sincos(double x, double& s, double& c);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);  // clamped to [-1, 1]
double acos(double x);  // clamped to [-1, 1]
double exp(double x);
double log(double x);
double pow(double x, double y);
double lgamma(double x);  // x > 0

inline float sin(float x) { return float(sin(double(x))); }
inline float cos(float x) { return float(cos(double(x))); }
inline void sincos(float x, float& s, float& c)
{
    double sd, cd;
    sincos(double(x), sd, cd);
    s = float(sd);
    c = float(cd);
}
inline float tan(float x) { return float(tan(double(x))); }
inline float atan(float x) { return float(atan(double(x))); }
inline float atan2(float y, float x) { return float(atan2(double(y), double(x))); }
inline float asin(float x) { return float(asin(double(x))); }
inline float acos(float x) { return float(acos(double(x))); }
inline float exp(float x) { return float(exp(double(x))); }
inline float log(float x) { return float(log(double(x))); }
inline float pow(float x, float y) { return float(pow(double(x), double(y))); }
inline float lgamma(float x) { return float(lgamma(double(x))); }

// The process's floating-point setup for a deterministic simulation: the C runtime's FMA3
// paths off (what still calls it agrees across CPUs) and SSE's default rounding, no
// flush-to-zero. Called once at start-up.
void setupFloatingPoint();

// A hash of the functions' outputs over a fixed set of inputs (determinism self-check).
uint64_t selfHash();

}  // namespace fx::dmath
