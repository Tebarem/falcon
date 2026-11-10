using System;
using System.Collections.Generic;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// The networked simulation's state after each tick, kept for the last <see cref="Window"/> ticks
/// so a rollback can restore one and simulate forward again. The state: the next NetId, then each
/// NetworkObject in NetId order (awake or dormant, its transform, its components' [Sync] members).
/// A tick simulated again is checked against what it was the first time (the rollback test).
/// </summary>
internal static unsafe class SimHistory
{
    private const int Capacity = 256;

    private sealed class Entry
    {
        public ulong Tick = ulong.MaxValue;
        public byte[] Data = new byte[1024];
        public int Length;
        public ulong Hash;
    }

    // The live state's hash, as Write goes: the next NetId and each awake object's id and state.
    // (Dormant objects are in the image, to be woken by a restore, but not in the hash: whether
    // one is still listed depends on when it went, which machines that rolled back across that
    // and ones that didn't may count differently.)
    private static ulong s_liveHash;

    private static ulong Mix(ulong h, ulong v)
    {
        h ^= v + 0x9e3779b97f4a7c15ul + (h << 6) + (h >> 2);
        return h * 0xff51afd7ed558ccdul;
    }

    private static readonly Entry[] s_ring = new Entry[Capacity];
    private static readonly SyncWriter s_writer = new();
    private static readonly List<NetworkObject> s_gone = new();

    /// <summary>Ticks kept (0: none; nothing is captured).</summary>
    internal static int Window { get; private set; }

    /// <summary>Ticks simulated again that came out differently.</summary>
    internal static long Mismatches { get; private set; }
    internal static long Checked { get; private set; }

    /// <summary>Ticks simulated again are compared with the first time (--sim-rollback); off in a
    /// rollback session, where new inputs change them.</summary>
    internal static bool Comparing { get; set; } = true;

    /// <summary>A kept tick's state hash (0: not kept).</summary>
    internal static ulong HashOf(ulong tick)
    {
        Entry e = Slot(tick);
        return e.Tick == tick ? e.Hash : 0;
    }

    internal static void SetWindow(int ticks)
    {
        Window = Math.Clamp(ticks, 0, Capacity - 2);
        if (Window == 0)
            Clear();
    }

    internal static void Clear()
    {
        foreach (Entry e in s_ring)
            if (e != null)
                e.Tick = ulong.MaxValue;
    }

    private static Entry Slot(ulong tick) => s_ring[tick % Capacity] ??= new Entry();

    // A dormant object is part of the state for a window of ticks after it went (a rollback can
    // wake it), then not (and is destroyed a window later, when no rollback can reach a tick that
    // listed it).
    private static bool Listed(NetworkObject o, ulong tick) => !o.Dormant || tick <= o.DespawnTick + (ulong)Window;

    private static void Write(SyncWriter w, ulong tick)
    {
        w.Clear();
        w.Write(Network.NextId);
        s_liveHash = Mix(0, Network.NextId);
        int listed = 0;
        foreach (NetworkObject o in Network.Sorted)
            if (Listed(o, tick))
                ++listed;
        w.Write(listed);
        foreach (NetworkObject o in Network.Sorted)
        {
            if (!Listed(o, tick))
                continue;
            int at = w.Length;
            w.Write(o.NetId);
            w.Write(o.Dormant);
            ObjectState.Write(o, w);
            if (!o.Dormant)
                s_liveHash = Mix(Mix(s_liveHash, o.NetId), ObjectState.Hash(w.Written[at..]));
        }
    }

    // Objects destroyed through Network.Destroy that no state a rollback can reach lists: gone for good.
    private static void Collect(ulong tick)
    {
        s_gone.Clear();
        foreach (NetworkObject o in Network.Sorted)
            if (o.Dormant && tick > o.DespawnTick + 2 * (ulong)Window)
                s_gone.Add(o);
        foreach (NetworkObject o in s_gone)
            Network.DestroyNow(o);
    }

    /// <summary>The state after `tick`. `again`: the tick was simulated again (a rollback): its
    /// state must be what it was the first time.</summary>
    internal static void Capture(ulong tick, bool again)
    {
        if (Window == 0)
            return;
        if (!again)
            Collect(tick);
        Write(s_writer, tick);
        ReadOnlySpan<byte> data = s_writer.Written;
        ulong hash = s_liveHash;
        Entry e = Slot(tick);
        if (again && Comparing && e.Tick == tick)
        {
            ++Checked;
            if (e.Hash != hash)
            {
                if (Mismatches == 0)
                    Log.Error($"sim-rollback: tick {tick} simulated again came out differently: {Difference(e.Data.AsSpan(0, e.Length), data)}");
                ++Mismatches;
            }
        }
        if (e.Data.Length < data.Length)
            e.Data = new byte[data.Length * 2];
        data.CopyTo(e.Data);
        e.Length = data.Length;
        e.Hash = hash;
        e.Tick = tick;

    }

    /// <summary>The state as it was after `tick` (false: not kept). Objects spawned since are
    /// destroyed, dormant ones it had awake wake, transforms and [Sync] members are put back.</summary>
    internal static bool Restore(ulong tick)
    {
        Entry e = Slot(tick);
        if (Window == 0 || e.Tick != tick)
            return false;
        var r = new SyncReader(e.Data.AsSpan(0, e.Length));
        ulong nextId = r.ULong();
        int count = r.Int();
        var kept = new HashSet<ulong>();
        float* rows = stackalloc float[12];
        for (int i = 0; i < count && r.Ok; ++i)
        {
            ulong id = r.ULong();
            bool dormant = r.Bool();
            byte transform = ObjectState.ReadTransform(ref r, rows);
            NetworkObject? o = Network.Find(id);
            kept.Add(id);
            if (o == null || !Ecs.IsAlive(o.Entity))
            {
                // Gone for good since (its dormancy outlived the window): it can't come back.
                ObjectState.ReadComponents(null, ref r);
                continue;
            }
            if (o.Dormant != dormant)
            {
                o.Dormant = dormant;
                o.Entity.Enabled = !dormant;
            }
            ObjectState.ApplyTransform(o.Entity, transform, rows);
            ObjectState.ReadComponents(o, ref r);
        }
        // Spawned after that tick: they go (the ticks simulated again spawn them again).
        s_gone.Clear();
        foreach (NetworkObject o in Network.Sorted)
            if (!kept.Contains(o.NetId))
                s_gone.Add(o);
        foreach (NetworkObject o in s_gone)
            Network.DestroyNow(o);
        Network.NextId = nextId;
        return true;
    }

    // Where two states first differ: the object, and its transform or which component.
    private static string Difference(ReadOnlySpan<byte> before, ReadOnlySpan<byte> now)
    {
        var a = new SyncReader(before);
        var b = new SyncReader(now);
        if (a.ULong() != b.ULong())
            return "the next NetId";
        int na = a.Int(), nb = b.Int();
        if (na != nb)
            return $"{na} objects then, {nb} now";
        for (int i = 0; i < na && a.Ok && b.Ok; ++i)
        {
            ulong id = a.ULong();
            if (b.ULong() != id)
                return $"object {id} missing";
            string name = Network.Find(id) is NetworkObject o && Ecs.IsAlive(o.Entity) ? $" ({o.Entity.Name})" : "";
            if (a.Bool() != b.Bool())
                return $"object {id}{name}: awake or dormant";
            byte ta = a.Byte(), tb = b.Byte();
            if (ta != tb)
                return $"object {id}{name}: its transform kind";
            if (ta != 0 && !a.Bytes(48).SequenceEqual(b.Bytes(48)))
                return $"object {id}{name}: its transform";
            int ca = a.Int(), cb = b.Int();
            if (ca != cb)
                return $"object {id}{name}: {ca} synced components then, {cb} now";
            List<Component>? list = Network.Find(id) is NetworkObject x ? ScriptComponents.Of(x.Entity) : null;
            int synced = 0;
            for (int k = 0; k < ca; ++k)
            {
                ReadOnlySpan<byte> sa = a.Bytes(a.Int()), sb = b.Bytes(b.Int());
                if (!sa.SequenceEqual(sb))
                {
                    string type = "component " + k;
                    if (list != null)
                        foreach (Component c in list)
                            if (!c.Destroyed && c.Type?.SyncWrite != null && synced++ == k)
                                type = c.GetType().Name;
                    return $"object {id}{name}: {type}'s [Sync] members";
                }
            }
        }
        return "somewhere (the states' lengths differ)";
    }
}
