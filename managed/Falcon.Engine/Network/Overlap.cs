using System;
using System.Collections.Generic;
using System.Numerics;

namespace Falcon;

public static partial class Network
{
    // A grid of the objects (X and Z), built at a tick's first query from where they were then:
    // thousands of queries a tick (every projectile looking for players) look at a few cells each.
    private const float OverlapCell = 4f;
    private const float OverlapSlack = 1f;  // (how far an object may have moved since the grid was built)
    private static readonly Dictionary<(int, int), List<NetworkObject>> s_grid = new();
    private static readonly List<List<NetworkObject>> s_gridLists = new();
    private static ulong s_gridBuilt = ulong.MaxValue;
    private static readonly Comparison<NetworkObject> s_byNetId = (a, b) => a.NetId.CompareTo(b.NetId);

    /// <summary>
    /// The networked objects (not dormant) within <paramref name="radius"/> of a point, in NetId
    /// order, into <paramref name="results"/> (cleared first). Cheap enough for thousands of
    /// queries a tick: objects are found through a grid built at the tick's first query, from
    /// where they were then, so one that moved more than a metre since, or was spawned since, is
    /// found from the next tick on.
    /// </summary>
    public static void Overlap(Vector3 centre, float radius, List<NetworkObject> results)
    {
        results.Clear();
        BuildOverlapGrid();
        float reach = radius + OverlapSlack;
        int x0 = (int)MathF.Floor((centre.X - reach) / OverlapCell), x1 = (int)MathF.Floor((centre.X + reach) / OverlapCell);
        int z0 = (int)MathF.Floor((centre.Z - reach) / OverlapCell), z1 = (int)MathF.Floor((centre.Z + reach) / OverlapCell);
        float r2 = radius * radius;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                if (s_grid.TryGetValue((x, z), out List<NetworkObject>? cell))
                    foreach (NetworkObject o in cell)
                        if (!o.Dormant && o.NetId != 0 && Ecs.IsAlive(o.Entity) && Vector3.DistanceSquared(o.Entity.Position, centre) < r2)
                            results.Add(o);
        if (results.Count > 1)
            results.Sort(s_byNetId);
    }

    // Once a tick (re-simulated ones too), or once a frame outside ticks.
    private static void BuildOverlapGrid()
    {
        ulong run = Time.InTick ? Time.TickRuns : (ulong)Time.Frame | (1UL << 63);
        if (s_gridBuilt == run)
            return;
        s_gridBuilt = run;
        foreach (List<NetworkObject> list in s_grid.Values)
        {
            list.Clear();
            s_gridLists.Add(list);
        }
        s_grid.Clear();
        foreach (NetworkObject o in s_sorted)
        {
            if (o.Dormant || !Ecs.IsAlive(o.Entity))
                continue;
            Vector3 p = o.Entity.Position;
            var key = ((int)MathF.Floor(p.X / OverlapCell), (int)MathF.Floor(p.Z / OverlapCell));
            if (!s_grid.TryGetValue(key, out List<NetworkObject>? cell))
            {
                if (s_gridLists.Count > 0)
                {
                    cell = s_gridLists[^1];
                    s_gridLists.RemoveAt(s_gridLists.Count - 1);
                }
                else
                {
                    cell = new List<NetworkObject>();
                }
                s_grid[key] = cell;
            }
            cell.Add(o);
        }
    }

    private static void ClearOverlapGrid()
    {
        s_grid.Clear();
        s_gridLists.Clear();
        s_gridBuilt = ulong.MaxValue;
    }
}
