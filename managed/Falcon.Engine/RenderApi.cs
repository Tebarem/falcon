using System;
using System.Numerics;
using System.Text;

namespace Falcon;

/// <summary>
/// (API v24) The frame's render settings, by the editor's Lighting panel names: Exposure,
/// ManualExposure (0: auto), Bloom, Ao (0 off, 1 GTAO, 2 SSAO), AoRadius, AoPower, LookPower,
/// LookSaturation, Gi, GiIntensity, SkyIntensity, Reflections, ReflectIntensity, Fxaa, Taa,
/// RenderScale. The editor's Stop puts them all back.
/// </summary>
public static unsafe class Render
{
    /// <summary>A setting by name (NaN: no such setting).</summary>
    public static float Get(string key) => Call(key, 0f, false);

    /// <summary>Sets a setting by name; returns what it is now (clamped to its range).</summary>
    public static float Set(string key, float value) => Call(key, value, true);

    private static float Call(string key, float value, bool set)
    {
        Span<byte> utf8 = stackalloc byte[64];
        int n = Encoding.UTF8.GetBytes(key, utf8[..63]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Native.Api.Native->renderSetting(Native.Api.Ctx, p, value, set ? 1 : 0);
    }

    public static float Exposure { get => Get("exposure"); set => Set("exposure", value); }
    public static float Bloom { get => Get("bloom"); set => Set("bloom", value); }
    public static bool Gi { get => Get("gi") != 0f; set => Set("gi", value ? 1f : 0f); }
    public static float SkyIntensity { get => Get("skyIntensity"); set => Set("skyIntensity", value); }
    public static bool Reflections { get => Get("reflections") != 0f; set => Set("reflections", value ? 1f : 0f); }
}

/// <summary>
/// (API v24) Debug drawing: lines in the world over the editor's views (the Scene and Game tabs),
/// for a while (seconds; 0: this frame), drawn on top. A game shows none: it is for developing.
/// </summary>
public static unsafe class Debug
{
    private static uint Pack(Vector4 c)
    {
        static uint B(float v) => (uint)Math.Clamp(MathF.Round(v * 255f), 0f, 255f);
        return B(c.X) | B(c.Y) << 8 | B(c.Z) << 16 | B(c.W) << 24;
    }

    public static readonly Vector4 Red = new(1, 0.2f, 0.2f, 1), Green = new(0.3f, 1, 0.3f, 1), Blue = new(0.3f, 0.5f, 1, 1),
                                   Yellow = new(1, 0.9f, 0.2f, 1), White = Vector4.One;

    public static void Line(Vector3 a, Vector3 b, Vector4 color, float seconds = 0f) =>
        Native.Api.Native->debugLine(Native.Api.Ctx, (float*)&a, (float*)&b, Pack(color), seconds);

    public static void Line(Vector3 a, Vector3 b) => Line(a, b, White);

    public static void Ray(Vector3 from, Vector3 direction, Vector4 color, float seconds = 0f) => Line(from, from + direction, color, seconds);

    /// <summary>An arrow from a to b (its head a tenth of its length).</summary>
    public static void Arrow(Vector3 a, Vector3 b, Vector4 color, float seconds = 0f)
    {
        Line(a, b, color, seconds);
        Vector3 d = b - a;
        float len = d.Length();
        if (len < 1e-6f)
            return;
        d /= len;
        Vector3 side = Vector3.Normalize(Vector3.Cross(d, MathF.Abs(d.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitX));
        Vector3 up = Vector3.Cross(side, d);
        float h = len * 0.1f;
        Line(b, b - d * h + side * h * 0.5f, color, seconds);
        Line(b, b - d * h - side * h * 0.5f, color, seconds);
        Line(b, b - d * h + up * h * 0.5f, color, seconds);
        Line(b, b - d * h - up * h * 0.5f, color, seconds);
    }

    /// <summary>A box (centre, half extents), turned.</summary>
    public static void Box(Vector3 center, Vector3 half, Quaternion rotation, Vector4 color, float seconds = 0f)
    {
        Span<Vector3> c = stackalloc Vector3[8];
        for (int i = 0; i < 8; ++i)
            c[i] = center + Vector3.Transform(new Vector3((i & 1) != 0 ? half.X : -half.X, (i & 2) != 0 ? half.Y : -half.Y,
                                                          (i & 4) != 0 ? half.Z : -half.Z), rotation);
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1)
                if ((i & bit) == 0)
                    Line(c[i], c[i | bit], color, seconds);
    }

    public static void Box(Vector3 center, Vector3 half, Vector4 color, float seconds = 0f) => Box(center, half, Quaternion.Identity, color, seconds);

    /// <summary>A sphere: three circles.</summary>
    public static void Sphere(Vector3 center, float radius, Vector4 color, float seconds = 0f)
    {
        const int Segments = 24;
        for (int axis = 0; axis < 3; ++axis)
            for (int i = 0; i < Segments; ++i)
            {
                float a0 = i * MathF.Tau / Segments, a1 = (i + 1) * MathF.Tau / Segments;
                Line(center + Circle(axis, a0) * radius, center + Circle(axis, a1) * radius, color, seconds);
            }
    }

    private static Vector3 Circle(int axis, float a) => axis switch
    {
        0 => new Vector3(0, MathF.Cos(a), MathF.Sin(a)),
        1 => new Vector3(MathF.Cos(a), 0, MathF.Sin(a)),
        _ => new Vector3(MathF.Cos(a), MathF.Sin(a), 0),
    };

    /// <summary>A small cross at a point.</summary>
    public static void Point(Vector3 p, float size, Vector4 color, float seconds = 0f)
    {
        Line(p - Vector3.UnitX * size, p + Vector3.UnitX * size, color, seconds);
        Line(p - Vector3.UnitY * size, p + Vector3.UnitY * size, color, seconds);
        Line(p - Vector3.UnitZ * size, p + Vector3.UnitZ * size, color, seconds);
    }
}
