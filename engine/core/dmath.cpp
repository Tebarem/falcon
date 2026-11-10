#include "core/dmath.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#include <float.h>
#include <math.h>
#include <xmmintrin.h>
#endif

namespace fx::dmath {

namespace {

constexpr double kPi = 3.14159265358979311600e+00;
constexpr double kPiLo = 1.22464679914735317720e-16;  // pi - kPi
constexpr double kHalfPi = 1.57079632679489655800e+00;
constexpr double kHalfPiLo = 6.12323399573676588610e-17;
constexpr double kSixthPi = 5.23598775598298815659e-01;
constexpr double kSqrt3 = 1.73205080756887719318e+00;
constexpr double kTanTwelfthPi = 2.67949192431122696e-01;  // tan(pi/12)
// pi/2 in three parts of 33 bits (fdlibm): k * part is exact for |k| < 2^20.
constexpr double kPio2_1 = 1.57079632673412561417e+00;
constexpr double kPio2_2 = 6.07710050630396597660e-11;
constexpr double kPio2_2t = 2.02226624879595063154e-21;
constexpr double kInvPio2 = 6.36619772367581382433e-01;
constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;
constexpr double kInvLn2 = 1.44269504088896338700e+00;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool isNan(double x) { return x != x; }
bool isInf(double x) { return x == kInf || x == -kInf; }
double copySign(double magnitude, double sign) { return std::signbit(sign) ? -std::fabs(magnitude) : std::fabs(magnitude); }

constexpr double factorialInverse(int n)
{
    double f = 1.0;
    for (int i = 2; i <= n; ++i)
        f *= double(i);
    return 1.0 / f;
}

// |r| <= pi/4: Taylor series to r^17 (r^18 for cos), Horner from the inside out.
double sinKernel(double r)
{
    const double z = r * r;
    double p = factorialInverse(17);
    p = p * z - factorialInverse(15);
    p = p * z + factorialInverse(13);
    p = p * z - factorialInverse(11);
    p = p * z + factorialInverse(9);
    p = p * z - factorialInverse(7);
    p = p * z + factorialInverse(5);
    p = p * z - factorialInverse(3);
    return r + r * z * p;
}

double cosKernel(double r)
{
    const double z = r * r;
    double p = -factorialInverse(18);
    p = p * z + factorialInverse(16);
    p = p * z - factorialInverse(14);
    p = p * z + factorialInverse(12);
    p = p * z - factorialInverse(10);
    p = p * z + factorialInverse(8);
    p = p * z - factorialInverse(6);
    p = p * z + factorialInverse(4);
    p = p * z - factorialInverse(2);
    return 1.0 + z * p;
}

// x = k * pi/2 + r, |r| <= ~pi/4; returns k mod 4.
int reduce(double x, double& r)
{
    const double k = std::floor(x * kInvPio2 + 0.5);
    r = ((x - k * kPio2_1) - k * kPio2_2) - k * kPio2_2t;
    const double q = k - 4.0 * std::floor(k * 0.25);  // (exact: k is an integer)
    return int(q);
}

// t in [0, 1]: above tan(pi/12) shifted by pi/6 (|t| <= 0.268 after), then the series
// t - t^3/3 + t^5/5 - ... to t^31.
double atanUnit(double t)
{
    double base = 0.0;
    if (t > kTanTwelfthPi) {
        t = (t * kSqrt3 - 1.0) / (kSqrt3 + t);
        base = kSixthPi;
    }
    const double z = t * t;
    double p = -1.0 / 31.0;
    for (int k = 14; k >= 0; --k)
        p = (k & 1 ? -1.0 : 1.0) / double(2 * k + 1) + z * p;
    return base + t * p;
}

}  // namespace

void sincos(double x, double& s, double& c)
{
    if (isNan(x) || isInf(x)) {
        s = c = kNaN;
        return;
    }
    double r;
    const int q = reduce(x, r);
    const double sr = sinKernel(r), cr = cosKernel(r);
    switch (q) {
    case 0: s = sr; c = cr; break;
    case 1: s = cr; c = -sr; break;
    case 2: s = -sr; c = -cr; break;
    default: s = -cr; c = sr; break;
    }
}

double sin(double x)
{
    double s, c;
    sincos(x, s, c);
    return s;
}

double cos(double x)
{
    double s, c;
    sincos(x, s, c);
    return c;
}

double tan(double x)
{
    double s, c;
    sincos(x, s, c);
    return s / c;
}

double atan(double x)
{
    if (isNan(x))
        return x;
    const double a = std::fabs(x);
    double r;
    if (a > 1.0)
        r = (kHalfPi - atanUnit(1.0 / a)) + kHalfPiLo;
    else
        r = atanUnit(a);
    return x < 0.0 ? -r : r;
}

double atan2(double y, double x)
{
    if (isNan(x) || isNan(y))
        return kNaN;
    if (y == 0.0) {
        if (std::signbit(x))
            return copySign(kPi, y);
        return y;  // (+-0)
    }
    if (x == 0.0)
        return copySign(kHalfPi, y);
    if (isInf(x) && isInf(y))
        return copySign(x > 0.0 ? 0.25 * kPi : 0.75 * kPi, y);
    if (isInf(x))
        return x > 0.0 ? copySign(0.0, y) : copySign(kPi, y);
    if (isInf(y))
        return copySign(kHalfPi, y);
    const double r = atan(y / x);
    if (x > 0.0)
        return r;
    return y > 0.0 ? (r + kPi) + kPiLo : (r - kPi) - kPiLo;
}

double asin(double x)
{
    if (isNan(x))
        return x;
    x = x < -1.0 ? -1.0 : x > 1.0 ? 1.0 : x;
    return atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x)
{
    if (isNan(x))
        return x;
    x = x < -1.0 ? -1.0 : x > 1.0 ? 1.0 : x;
    return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}

double exp(double x)
{
    if (isNan(x))
        return x;
    if (x > 709.782712893384)
        return kInf;
    if (x < -745.1332191019411)
        return 0.0;
    const double k = std::floor(x * kInvLn2 + 0.5);
    const double r = (x - k * kLn2Hi) - k * kLn2Lo;
    double p = factorialInverse(14);
    for (int n = 13; n >= 1; --n)
        p = p * r + factorialInverse(n);
    p = p * r + 1.0;
    return std::ldexp(p, int(k));
}

double log(double x)
{
    if (isNan(x) || x < 0.0)
        return kNaN;
    if (x == 0.0)
        return -kInf;
    if (isInf(x))
        return x;
    int e = 0;
    double m = std::frexp(x, &e);  // [0.5, 1)
    if (m < 0.70710678118654757) {
        m *= 2.0;
        --e;
    }
    const double f = m - 1.0;
    const double s = f / (2.0 + f);
    const double z = s * s;
    double p = 1.0 / 25.0;
    for (int n = 23; n >= 1; n -= 2)
        p = p * z + 1.0 / double(n);
    const double logm = 2.0 * s * p;
    return double(e) * kLn2Hi + (logm + double(e) * kLn2Lo);
}

double pow(double x, double y)
{
    if (y == 0.0 || x == 1.0)
        return 1.0;
    if (isNan(x) || isNan(y))
        return kNaN;
    if (x == 0.0)
        return y > 0.0 ? 0.0 : kInf;
    if (x < 0.0) {
        if (std::floor(y) != y)
            return kNaN;
        const double r = exp(y * log(-x));
        const bool odd = std::floor(y * 0.5) * 2.0 != y;
        return odd ? -r : r;
    }
    return exp(y * log(x));
}

double lgamma(double x)
{
    if (isNan(x))
        return x;
    if (x < 0.5)  // reflection (x > 0 here: (0, 0.5))
        return log(kPi / std::fabs(sin(kPi * x))) - lgamma(1.0 - x);
    static constexpr double g = 7.0;
    static constexpr double c[9] = {0.99999999999980993,  676.5203681218851,     -1259.1392167224028,
                                    771.32342877765313,   -176.61502916214059,   12.507343278686905,
                                    -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
    x -= 1.0;
    double a = c[0];
    for (int i = 1; i < 9; ++i)
        a += c[i] / (x + double(i));
    const double t = x + g + 0.5;
    return 0.91893853320467274178 + (x + 0.5) * log(t) - t + log(a);  // (0.5 log(2 pi))
}

void setupFloatingPoint()
{
#if defined(_MSC_VER) && defined(_M_X64)
    _set_FMA3_enable(0);
    _mm_setcsr(0x1F80);  // round to nearest, all exceptions masked, no flush-to-zero / denormals-are-zero
#endif
}

uint64_t selfHash()
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        h = (h ^ bits) * 1099511628211ull;
    };
    for (int i = -2000; i <= 2000; ++i) {
        const double x = double(i) * 0.01237;
        double s, c;
        sincos(x * 37.0, s, c);
        mix(s);
        mix(c);
        mix(tan(x));
        mix(atan(x * 3.0));
        mix(atan2(x, 1.0 - x * 0.3));
        mix(asin(x * 0.04));
        mix(acos(x * 0.04));
        mix(exp(x * 0.3));
        mix(log(std::fabs(x) + 1e-3));
        mix(pow(std::fabs(x) + 0.5, 1.7));
    }
    return h;
}

}  // namespace fx::dmath
