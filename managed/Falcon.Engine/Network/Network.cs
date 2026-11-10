using System;
using System.Collections.Generic;

namespace Falcon;

/// <summary>
/// The networked simulation's objects (<see cref="NetworkObject"/>): found by NetId, destroyed
/// so that a rollback can bring them back.
/// </summary>
public static partial class Network
{
    // By NetId, and a list in NetId order (the state is written in it: the same on every machine).
    // Iterating the list allocates nothing (a SortedDictionary's enumerator does, every time).
    private static readonly Dictionary<ulong, NetworkObject> s_objects = new();
    private static readonly List<NetworkObject> s_sorted = new();
    internal static ulong NextId = 1;

    /// <summary>The objects, in NetId order (dormant ones too). Don't spawn or destroy objects
    /// for good while going through it.</summary>
    public static IReadOnlyList<NetworkObject> Objects => s_sorted;

    /// <summary>How many there are (dormant ones too).</summary>
    public static int Count => s_sorted.Count;
    internal static List<NetworkObject> Sorted => s_sorted;

    private static int IndexOf(ulong netId)
    {
        int lo = 0, hi = s_sorted.Count - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) >> 1;
            ulong id = s_sorted[mid].NetId;
            if (id == netId)
                return mid;
            if (id < netId)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return ~lo;
    }

    /// <summary>The object with this NetId (null: none).</summary>
    public static NetworkObject? Find(ulong netId) => s_objects.TryGetValue(netId, out NetworkObject? o) ? o : null;

    /// <summary>An entity's NetworkObject (null: it is not networked).</summary>
    public static NetworkObject? Of(Entity entity) => ScriptComponents.Get<NetworkObject>(entity);

    internal static void Register(NetworkObject o)
    {
        if (o.NetId == 0)
            o.NetId = AssignId(o);
        if (s_prefabs.Remove(o.Entity.Id, out string? prefab))
            o.Prefab = prefab;
        s_objects[o.NetId] = o;
        int at = IndexOf(o.NetId);
        if (at >= 0)
            s_sorted[at] = o;
        else
            s_sorted.Insert(~at, o);
    }

    private static readonly Dictionary<ulong, string> s_prefabs = new();  // entity -> the prefab it came from

    /// <summary>Spawns a prefab with a NetworkObject (the host: every client spawns it too, from
    /// the same prefab). Its components are made at once.</summary>
    public static Entity Spawn(string prefab, System.Numerics.Vector3 position, System.Numerics.Quaternion rotation, int owner = -1)
    {
        Entity e = Falcon.Prefab.Spawn(prefab, position, rotation);
        if (!Ecs.IsAlive(e))
            return e;
        s_prefabs[e.Id] = prefab;
        ScriptComponents.Start();  // (its components now: the NetworkObject takes its NetId and prefab)
        s_prefabs.Remove(e.Id);
        if (Of(e) is NetworkObject o)
            o.Owner = owner;
        else
            Log.Warning($"Network.Spawn: {prefab} has no Network Object: it is not networked");
        return e;
    }

    internal static void Unregister(NetworkObject o)
    {
        if (s_objects.TryGetValue(o.NetId, out NetworkObject? now) && ReferenceEquals(now, o))
        {
            s_objects.Remove(o.NetId);
            if (IndexOf(o.NetId) is int at && at >= 0)
                s_sorted.RemoveAt(at);
        }
    }

    /// <summary>Destroys a networked entity. While a rollback could still need it (the simulation
    /// keeps a history), it only sleeps (disabled, <see cref="NetworkObject.Dormant"/>) and goes
    /// once no state a rollback can reach lists it; a rollback past the tick it went wakes it.
    /// Other entities are destroyed at once.</summary>
    public static void Destroy(Entity entity)
    {
        NetworkObject? o = Of(entity);
        if (o == null || SimHistory.Window == 0)
        {
            entity.Destroy();
            return;
        }
        if (o.Dormant)
            return;
        o.Dormant = true;
        o.DespawnTick = Time.Tick;
        entity.Enabled = false;
    }

    // Gone for good: its components ended now (not at the next update), so a rollback that spawns
    // it again finds its NetId free.
    internal static void DestroyNow(NetworkObject o)
    {
        Entity e = o.Entity;
        ScriptComponents.RemoveAll(e);
        Unregister(o);
        if (Ecs.IsAlive(e))
            e.Destroy();
    }

    // The game stopped, or its scene went: no object carries over.
    internal static void Reset()
    {
        s_objects.Clear();
        s_sorted.Clear();
        s_prefabs.Clear();
        NextId = 1;
        SimHistory.Clear();
        ClearOverlapGrid();
    }
}
