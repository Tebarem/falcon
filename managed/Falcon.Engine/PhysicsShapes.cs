using System;
using System.Numerics;
using Falcon.Native;

namespace Falcon;

/// <summary>(API v35) A collision shape from a script, in its entity's space (scaled with it).</summary>
public enum ShapeKind : uint
{
    Box = 0,     // Size: its full size
    Sphere = 1,  // Size.X: its radius
    Capsule = 2, // Size.X: its radius, Size.Y: its height (along Y)
}

public static unsafe partial class Physics
{
    /// <summary>(API v35) The entity's collision shape becomes the convex hull of these points (in
    /// its space, scaled with it), in place of its Collider's shape (whose friction, bounce, layers
    /// and trigger flag it keeps). The entity needs a PhysicsBody or a Collider; its body is built
    /// again at the next step. False: too few points, or no physics.</summary>
    public static bool SetConvexHull(Entity e, ReadOnlySpan<Vector3> points, Vector3 center = default, Quaternion rotation = default)
    {
        ClearShapes(e);
        return AddConvexHull(e, points, center, rotation);
    }

    /// <summary>Adds a convex hull to the entity's shapes (several: a compound).</summary>
    public static bool AddConvexHull(Entity e, ReadOnlySpan<Vector3> points, Vector3 center = default, Quaternion rotation = default)
    {
        var shape = new FxShape { kind = 3, center = center, rotation = rotation == default ? Quaternion.Identity : rotation, size = Vector3.One };
        fixed (Vector3* p = points)
            return Api.Native->physicsShapeAdd(Api.Ctx, e.Id, &shape, (float*)p, points.Length) != 0;
    }

    /// <summary>Adds a box, sphere or capsule to the entity's shapes (several: a compound), in place
    /// of its Collider's shape.</summary>
    public static bool AddShape(Entity e, ShapeKind kind, Vector3 size, Vector3 center = default, Quaternion rotation = default)
    {
        var shape = new FxShape { kind = (uint)kind, center = center, rotation = rotation == default ? Quaternion.Identity : rotation, size = size };
        return Api.Native->physicsShapeAdd(Api.Ctx, e.Id, &shape, null, 0) != 0;
    }

    /// <summary>The shapes scripts gave the entity go: its Collider's shape again.</summary>
    public static void ClearShapes(Entity e) => Api.Native->physicsShapesClear(Api.Ctx, e.Id);
}
