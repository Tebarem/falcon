using System.Collections.Generic;
using System.Numerics;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>
/// The scene's navigation mesh (baked in the editor: Navigation/Bake, saved beside the scene): paths
/// over the level, straight walks along it, the nearest walkable point. Without one, the queries
/// find nothing.
/// </summary>
public static unsafe class Navigation
{
    /// <summary>A path from `from` to `to` (each found on the mesh within a couple of metres): its
    /// corners, the first and last on the mesh, into `path`. False when there is no whole way (the
    /// corners then go as near as they can; none when either end is off the mesh).</summary>
    public static bool FindPath(Vector3 from, Vector3 to, List<Vector3> path)
    {
        path.Clear();
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->navPath == null)
            return false;
        const int capacity = 256;
        float* corners = stackalloc float[capacity * 3];
        int n = l->navPath(l->context, (float*)&from, (float*)&to, corners, capacity);
        for (int i = 0; i < System.Math.Abs(n); ++i)
            path.Add(new Vector3(corners[i * 3], corners[i * 3 + 1], corners[i * 3 + 2]));
        return n > 0;
    }

    /// <summary>A straight walk from `from` towards `to` along the mesh: true when an edge stops it
    /// (at `hit`), false when it gets there (or there is no mesh under `from`).</summary>
    public static bool Raycast(Vector3 from, Vector3 to, out Vector3 hit)
    {
        hit = to;
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->navRaycast == null)
            return false;
        Vector3 at;
        bool stopped = l->navRaycast(l->context, (float*)&from, (float*)&to, (float*)&at) == 1;
        if (stopped)
            hit = at;
        return stopped;
    }

    /// <summary>The nearest point on the mesh to `point` (within `reach`: half extents, default 2 m
    /// across and 4 m up and down); false when there is none.</summary>
    public static bool NearestPoint(Vector3 point, out Vector3 onMesh, Vector3? reach = null)
    {
        onMesh = point;
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->navNearest == null)
            return false;
        Vector3 extents = reach ?? new Vector3(2, 4, 2), at;
        if (l->navNearest(l->context, (float*)&point, (float*)&extents, (float*)&at) != 1)
            return false;
        onMesh = at;
        return true;
    }

    /// <summary>Whether the scene has a navigation mesh.</summary>
    public static bool Available
    {
        get
        {
            FxLevelApi* l = LevelNative.Api;
            return l != null && l->navNearest != null && l->navNearest(l->context, null, null, null) != -1;
        }
    }
}
