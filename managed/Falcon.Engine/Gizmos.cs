using System;
using System.Numerics;

namespace Falcon;

/// <summary>
/// (API v34) Gizmos: lines over the editor's views drawn by components' OnDrawGizmos /
/// OnDrawGizmosSelected and editor tools' OnDrawGizmos, every editor frame (editing and playing).
/// They last the frame they are drawn in; outside those calls they draw nothing.
/// </summary>
public static unsafe class Gizmos
{
    /// <summary>The colour of the lines that name none (white; set again before each component's).</summary>
    public static Vector4 Color = Vector4.One;

    private static uint Pack(Vector4 c)
    {
        static uint B(float v) => (uint)Math.Clamp(MathF.Round(v * 255f), 0f, 255f);
        return B(c.X) | B(c.Y) << 8 | B(c.Z) << 16 | B(c.W) << 24;
    }

    public static void Line(Vector3 a, Vector3 b, Vector4 color) =>
        Native.Api.Native->gizmoLine(Native.Api.Ctx, (float*)&a, (float*)&b, Pack(color));

    public static void Line(Vector3 a, Vector3 b) => Line(a, b, Color);

    public static void Ray(Vector3 from, Vector3 direction) => Line(from, from + direction, Color);

    /// <summary>An arrow from a to b (its head a tenth of its length).</summary>
    public static void Arrow(Vector3 a, Vector3 b)
    {
        Line(a, b);
        Vector3 d = b - a;
        float len = d.Length();
        if (len < 1e-6f)
            return;
        d /= len;
        Vector3 side = Vector3.Normalize(Vector3.Cross(d, MathF.Abs(d.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitX));
        Vector3 up = Vector3.Cross(side, d);
        float h = len * 0.1f;
        Line(b, b - d * h + side * h * 0.5f);
        Line(b, b - d * h - side * h * 0.5f);
        Line(b, b - d * h + up * h * 0.5f);
        Line(b, b - d * h - up * h * 0.5f);
    }

    /// <summary>A box's 12 edges (centre, half extents), turned.</summary>
    public static void Box(Vector3 center, Vector3 half, Quaternion rotation)
    {
        Span<Vector3> c = stackalloc Vector3[8];
        for (int i = 0; i < 8; ++i)
            c[i] = center + Vector3.Transform(new Vector3((i & 1) != 0 ? half.X : -half.X, (i & 2) != 0 ? half.Y : -half.Y,
                                                          (i & 4) != 0 ? half.Z : -half.Z), rotation);
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1)
                if ((i & bit) == 0)
                    Line(c[i], c[i | bit]);
    }

    public static void Box(Vector3 center, Vector3 half) => Box(center, half, Quaternion.Identity);

    /// <summary>A circle around an axis (24 segments).</summary>
    public static void Circle(Vector3 center, Vector3 axis, float radius)
    {
        axis = Vector3.Normalize(axis);
        Vector3 u = Vector3.Normalize(Vector3.Cross(axis, MathF.Abs(axis.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitX));
        Vector3 v = Vector3.Cross(axis, u);
        Vector3 prev = center + u * radius;
        for (int i = 1; i <= 24; ++i)
        {
            float a = i * (MathF.PI * 2f / 24f);
            Vector3 p = center + (u * MathF.Cos(a) + v * MathF.Sin(a)) * radius;
            Line(prev, p);
            prev = p;
        }
    }

    /// <summary>A sphere as three circles.</summary>
    public static void Sphere(Vector3 center, float radius)
    {
        Circle(center, Vector3.UnitX, radius);
        Circle(center, Vector3.UnitY, radius);
        Circle(center, Vector3.UnitZ, radius);
    }
}
