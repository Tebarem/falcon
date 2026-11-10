using System;
using System.Numerics;

namespace Falcon;

/// <summary>
/// Deterministic math: the same bits on every x64 PC, in the editor (JIT) and in a shipped game
/// (NativeAOT). <see cref="MathF"/>'s Sin, Cos, Atan2, Exp, Pow... call the C runtime, whose
/// results depend on the CPU (FMA paths) and the Windows version, so a networked or replayed
/// simulation drifts apart with them. Use these in OnFixedUpdate / OnSimulate code instead
/// (the FALCON100 analyzer points the others out). +, -, *, / and Sqrt are already exact.
/// The engine's C++ twin is fx::dmath (same algorithms, same results).
/// </summary>
public static class DMath
{
    public const float Pi = MathF.PI;

    private const double kPi = 3.14159265358979311600e+00;
    private const double kPiLo = 1.22464679914735317720e-16;
    private const double kHalfPi = 1.57079632679489655800e+00;
    private const double kHalfPiLo = 6.12323399573676588610e-17;
    private const double kSixthPi = 5.23598775598298815659e-01;
    private const double kSqrt3 = 1.73205080756887719318e+00;
    private const double kTanTwelfthPi = 2.67949192431122696e-01;
    private const double kPio2_1 = 1.57079632673412561417e+00;
    private const double kPio2_2 = 6.07710050630396597660e-11;
    private const double kPio2_2t = 2.02226624879595063154e-21;
    private const double kInvPio2 = 6.36619772367581382433e-01;
    private const double kLn2Hi = 6.93147180369123816490e-01;
    private const double kLn2Lo = 1.90821492927058770002e-10;
    private const double kInvLn2 = 1.44269504088896338700e+00;

    // 1/n! for n = 0..18, computed as the C++ side does (a product, then one division).
    private static readonly double[] s_factorialInverse = MakeFactorials();

    private static double[] MakeFactorials()
    {
        var f = new double[19];
        for (int n = 0; n < f.Length; ++n)
        {
            double p = 1.0;
            for (int i = 2; i <= n; ++i)
                p *= i;
            f[n] = 1.0 / p;
        }
        return f;
    }

    private static double SinKernel(double r)
    {
        double[] f = s_factorialInverse;
        double z = r * r;
        double p = f[17];
        p = p * z - f[15];
        p = p * z + f[13];
        p = p * z - f[11];
        p = p * z + f[9];
        p = p * z - f[7];
        p = p * z + f[5];
        p = p * z - f[3];
        return r + r * z * p;
    }

    private static double CosKernel(double r)
    {
        double[] f = s_factorialInverse;
        double z = r * r;
        double p = -f[18];
        p = p * z + f[16];
        p = p * z - f[14];
        p = p * z + f[12];
        p = p * z - f[10];
        p = p * z + f[8];
        p = p * z - f[6];
        p = p * z + f[4];
        p = p * z - f[2];
        return 1.0 + z * p;
    }

    private static int Reduce(double x, out double r)
    {
        double k = Math.Floor(x * kInvPio2 + 0.5);
        r = ((x - k * kPio2_1) - k * kPio2_2) - k * kPio2_2t;
        double q = k - 4.0 * Math.Floor(k * 0.25);
        return (int)q;
    }

    private static double AtanUnit(double t)
    {
        double b = 0.0;
        if (t > kTanTwelfthPi)
        {
            t = (t * kSqrt3 - 1.0) / (kSqrt3 + t);
            b = kSixthPi;
        }
        double z = t * t;
        double p = -1.0 / 31.0;
        for (int k = 14; k >= 0; --k)
            p = ((k & 1) != 0 ? -1.0 : 1.0) / (2 * k + 1) + z * p;
        return b + t * p;
    }

    private static double CopySign(double magnitude, double sign) =>
        double.IsNegative(sign) ? -Math.Abs(magnitude) : Math.Abs(magnitude);

    public static void SinCos(double x, out double s, out double c)
    {
        if (double.IsNaN(x) || double.IsInfinity(x))
        {
            s = c = double.NaN;
            return;
        }
        int q = Reduce(x, out double r);
        double sr = SinKernel(r), cr = CosKernel(r);
        switch (q)
        {
        case 0: s = sr; c = cr; break;
        case 1: s = cr; c = -sr; break;
        case 2: s = -sr; c = -cr; break;
        default: s = -cr; c = sr; break;
        }
    }

    public static double Sin(double x) { SinCos(x, out double s, out _); return s; }
    public static double Cos(double x) { SinCos(x, out _, out double c); return c; }
    public static double Tan(double x) { SinCos(x, out double s, out double c); return s / c; }

    public static double Atan(double x)
    {
        if (double.IsNaN(x))
            return x;
        double a = Math.Abs(x);
        double r = a > 1.0 ? (kHalfPi - AtanUnit(1.0 / a)) + kHalfPiLo : AtanUnit(a);
        return x < 0.0 ? -r : r;
    }

    public static double Atan2(double y, double x)
    {
        if (double.IsNaN(x) || double.IsNaN(y))
            return double.NaN;
        if (y == 0.0)
            return double.IsNegative(x) ? CopySign(kPi, y) : y;
        if (x == 0.0)
            return CopySign(kHalfPi, y);
        if (double.IsInfinity(x) && double.IsInfinity(y))
            return CopySign(x > 0.0 ? 0.25 * kPi : 0.75 * kPi, y);
        if (double.IsInfinity(x))
            return x > 0.0 ? CopySign(0.0, y) : CopySign(kPi, y);
        if (double.IsInfinity(y))
            return CopySign(kHalfPi, y);
        double r = Atan(y / x);
        if (x > 0.0)
            return r;
        return y > 0.0 ? (r + kPi) + kPiLo : (r - kPi) - kPiLo;
    }

    public static double Asin(double x)
    {
        if (double.IsNaN(x))
            return x;
        x = x < -1.0 ? -1.0 : x > 1.0 ? 1.0 : x;
        return Atan2(x, Math.Sqrt((1.0 - x) * (1.0 + x)));
    }

    public static double Acos(double x)
    {
        if (double.IsNaN(x))
            return x;
        x = x < -1.0 ? -1.0 : x > 1.0 ? 1.0 : x;
        return Atan2(Math.Sqrt((1.0 - x) * (1.0 + x)), x);
    }

    public static double Exp(double x)
    {
        if (double.IsNaN(x))
            return x;
        if (x > 709.782712893384)
            return double.PositiveInfinity;
        if (x < -745.1332191019411)
            return 0.0;
        double k = Math.Floor(x * kInvLn2 + 0.5);
        double r = (x - k * kLn2Hi) - k * kLn2Lo;
        double[] f = s_factorialInverse;
        double p = f[14];
        for (int n = 13; n >= 1; --n)
            p = p * r + f[n];
        p = p * r + 1.0;
        return Math.ScaleB(p, (int)k);
    }

    public static double Log(double x)
    {
        if (double.IsNaN(x) || x < 0.0)
            return double.NaN;
        if (x == 0.0)
            return double.NegativeInfinity;
        if (double.IsInfinity(x))
            return x;
        int e = Math.ILogB(x) + 1;  // frexp: x = m 2^e, m in [0.5, 1)
        double m = Math.ScaleB(x, -e);
        if (m < 0.70710678118654757)
        {
            m *= 2.0;
            --e;
        }
        double f = m - 1.0;
        double s = f / (2.0 + f);
        double z = s * s;
        double p = 1.0 / 25.0;
        for (int n = 23; n >= 1; n -= 2)
            p = p * z + 1.0 / n;
        double logm = 2.0 * s * p;
        return e * kLn2Hi + (logm + e * kLn2Lo);
    }

    public static double Pow(double x, double y)
    {
        if (y == 0.0 || x == 1.0)
            return 1.0;
        if (double.IsNaN(x) || double.IsNaN(y))
            return double.NaN;
        if (x == 0.0)
            return y > 0.0 ? 0.0 : double.PositiveInfinity;
        if (x < 0.0)
        {
            if (Math.Floor(y) != y)
                return double.NaN;
            double r = Exp(y * Log(-x));
            bool odd = Math.Floor(y * 0.5) * 2.0 != y;
            return odd ? -r : r;
        }
        return Exp(y * Log(x));
    }

    public static float Sin(float x) => (float)Sin((double)x);
    public static float Cos(float x) => (float)Cos((double)x);
    public static void SinCos(float x, out float s, out float c)
    {
        SinCos((double)x, out double sd, out double cd);
        s = (float)sd;
        c = (float)cd;
    }
    public static float Tan(float x) => (float)Tan((double)x);
    public static float Atan(float x) => (float)Atan((double)x);
    public static float Atan2(float y, float x) => (float)Atan2((double)y, (double)x);
    public static float Asin(float x) => (float)Asin((double)x);
    public static float Acos(float x) => (float)Acos((double)x);
    public static float Exp(float x) => (float)Exp((double)x);
    public static float Log(float x) => (float)Log((double)x);
    public static float Pow(float x, float y) => (float)Pow((double)x, (double)y);

    /// <summary>A rotation of `radians` about a unit axis (Quaternion.CreateFromAxisAngle, deterministic).</summary>
    public static Quaternion AxisAngle(Vector3 axis, float radians)
    {
        SinCos(radians * 0.5f, out float s, out float c);
        return new Quaternion(axis.X * s, axis.Y * s, axis.Z * s, c);
    }

    /// <summary>Quaternion.CreateFromYawPitchRoll, deterministic (yaw about Y, pitch about X, roll about Z).</summary>
    public static Quaternion YawPitchRoll(float yaw, float pitch, float roll)
    {
        SinCos(roll * 0.5f, out float sr, out float cr);
        SinCos(pitch * 0.5f, out float sp, out float cp);
        SinCos(yaw * 0.5f, out float sy, out float cy);
        return new Quaternion(cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                              cy * cp * cr + sy * sp * sr);
    }

    /// <summary>Spherical interpolation along the shorter arc (Quaternion.Slerp, deterministic).</summary>
    public static Quaternion Slerp(Quaternion a, Quaternion b, float t)
    {
        float cosOmega = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
        if (cosOmega < 0f)
        {
            b = new Quaternion(-b.X, -b.Y, -b.Z, -b.W);
            cosOmega = -cosOmega;
        }
        float s0, s1;
        if (cosOmega > 0.9999f)
        {
            s0 = 1f - t;
            s1 = t;
        }
        else
        {
            float sinOmega = MathF.Sqrt(1f - cosOmega * cosOmega);
            float omega = Atan2(sinOmega, cosOmega);
            s0 = Sin((1f - t) * omega) / sinOmega;
            s1 = Sin(t * omega) / sinOmega;
        }
        return new Quaternion(a.X * s0 + b.X * s1, a.Y * s0 + b.Y * s1, a.Z * s0 + b.Z * s1, a.W * s0 + b.W * s1);
    }

    /// <summary>A hash of the functions over fixed inputs: equal to fx::dmath::selfHash on every
    /// machine (the determinism tests compare them).</summary>
    public static ulong SelfHash()
    {
        ulong h = 1469598103934665603ul;
        void Mix(double v) => h = (h ^ (ulong)BitConverter.DoubleToInt64Bits(v)) * 1099511628211ul;
        for (int i = -2000; i <= 2000; ++i)
        {
            double x = i * 0.01237;
            SinCos(x * 37.0, out double s, out double c);
            Mix(s);
            Mix(c);
            Mix(Tan(x));
            Mix(Atan(x * 3.0));
            Mix(Atan2(x, 1.0 - x * 0.3));
            Mix(Asin(x * 0.04));
            Mix(Acos(x * 0.04));
            Mix(Exp(x * 0.3));
            Mix(Log(Math.Abs(x) + 1e-3));
            Mix(Pow(Math.Abs(x) + 0.5, 1.7));
        }
        return h;
    }
}
