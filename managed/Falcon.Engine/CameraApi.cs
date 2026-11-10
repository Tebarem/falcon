using System;
using System.Numerics;

namespace Falcon;

// (API v23) The camera: the main camera entity, the view's field of view, and rays and points
// between the screen (the game's view, pixels from its top left: Input.MouseViewPosition) and the
// world.
public static unsafe partial class Camera
{
    /// <summary>The view's vertical field of view, degrees (the main camera's, or the editor's).</summary>
    public static float FieldOfView => Native.Api.Native->cameraFov(Native.Api.Ctx);

    /// <summary>The main camera's entity (a CameraComponent flagged main, the oldest if several;
    /// invalid: none, the editor's or the fly camera has the view).</summary>
    public static Entity Main
    {
        get
        {
            Entity best = default;
            using var q = new Query(Query.In<CameraComponent>());
            foreach (Chunk c in q)
            {
                ReadOnlySpan<CameraComponent> cams = c.Read<CameraComponent>(0);
                for (int i = 0; i < c.Count; ++i)
                    if ((cams[i].flags & 1u) != 0 && (!best.IsValid || c.Entities[i].Id < best.Id))
                        best = c.Entities[i];
            }
            return best;
        }
    }

    /// <summary>Makes an entity's camera the main one (its CameraComponent; the others lose it).</summary>
    public static bool SetMain(Entity e)
    {
        if (!Ecs.Has<CameraComponent>(e))
            return false;
        var others = new System.Collections.Generic.List<Entity>();
        using (var q = new Query(Query.In<CameraComponent>()))
            foreach (Chunk c in q)
            {
                ReadOnlySpan<CameraComponent> cams = c.Read<CameraComponent>(0);
                for (int i = 0; i < c.Count; ++i)
                    if ((cams[i].flags & 1u) != 0 && c.Entities[i] != e)
                        others.Add(c.Entities[i]);
            }
        foreach (Entity o in others)
        {
            CameraComponent cc = Ecs.Get<CameraComponent>(o);
            cc.flags &= ~1u;
            Ecs.Set(o, cc);
        }
        CameraComponent mine = Ecs.Get<CameraComponent>(e);
        mine.flags |= 1u;
        Ecs.Set(e, mine);
        return true;
    }

    private static Vector2 View()
    {
        Vector2 v = Input.ViewSize;
        return v.X > 0 && v.Y > 0 ? v : new Vector2(16, 9);
    }

    /// <summary>The ray from the camera through a point of the view (pixels from its top left):
    /// its origin (the camera) and direction (normalized). Physics.Raycast it to pick.</summary>
    public static (Vector3 Origin, Vector3 Direction) ScreenToRay(Vector2 pixel)
    {
        Vector2 size = View();
        float tanHalf = MathF.Tan(FieldOfView * MathF.PI / 360f), aspect = size.X / size.Y;
        float x = (2f * pixel.X / size.X - 1f) * tanHalf * aspect, y = (1f - 2f * pixel.Y / size.Y) * tanHalf;
        return (Position, Vector3.Normalize(Forward + Right * x + Up * y));
    }

    /// <summary>A world point on the view (pixels from its top left), and whether it is in front of
    /// the camera.</summary>
    public static (Vector2 Pixel, bool InFront) WorldToScreen(Vector3 point)
    {
        Vector2 size = View();
        float tanHalf = MathF.Tan(FieldOfView * MathF.PI / 360f), aspect = size.X / size.Y;
        Vector3 d = point - Position;
        float z = Vector3.Dot(d, Forward);
        if (z <= 1e-5f)
            return (default, false);
        float x = Vector3.Dot(d, Right) / (z * tanHalf * aspect), y = Vector3.Dot(d, Up) / (z * tanHalf);
        return (new Vector2((x + 1f) * 0.5f * size.X, (1f - y) * 0.5f * size.Y), true);
    }
}
