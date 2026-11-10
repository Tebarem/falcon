using System;
using System.Collections.Generic;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// A NetworkObject's state as bytes, the same for a tick's history (rollback) and for what the
/// host sends (replication): its transform kind (0 none, 1 world, 2 local: under a parent) and 12
/// floats, then its components with [Sync] members, each its byte length and bytes.
/// </summary>
internal static unsafe class ObjectState
{
    internal static void Write(NetworkObject o, SyncWriter w)
    {
        Entity e = o.Entity;
        if (o.SyncTransform && Ecs.IsAlive(e))
        {
            bool local = Ecs.Has<LocalTransform>(e);
            w.Write(local ? (byte)2 : (byte)1);
            if (local)
            {
                LocalTransform l = Ecs.Get<LocalTransform>(e);
                for (int i = 0; i < 12; ++i)
                    w.Write(l.rows[i]);
            }
            else
            {
                WorldTransform t = Ecs.Get<WorldTransform>(e);
                for (int i = 0; i < 12; ++i)
                    w.Write(t.rows[i]);
            }
        }
        else
        {
            w.Write((byte)0);
        }
        List<Component>? components = ScriptComponents.Of(e);
        int count = 0;
        if (components != null)
            foreach (Component c in components)
                if (!c.Destroyed && c.Type?.SyncWrite != null)
                    ++count;
        w.Write(count);
        if (components == null)
            return;
        foreach (Component c in components)
        {
            if (c.Destroyed || c.Type?.SyncWrite == null)
                continue;
            int lengthAt = w.Length;
            w.Write(0);  // (its length, patched below)
            int start = w.Length;
            c.Type.SyncWrite(c, w);
            w.Patch(lengthAt, w.Length - start);
        }
    }

    /// <summary>The transform part read: its kind (0: none) and rows.</summary>
    internal static byte ReadTransform(ref SyncReader r, float* rows)
    {
        byte kind = r.Byte();
        if (kind != 0)
            for (int i = 0; i < 12; ++i)
                rows[i] = r.Float();
        return kind;
    }

    /// <summary>A transform put on the entity (kind 2: its LocalTransform, 1: its world one).</summary>
    internal static void ApplyTransform(Entity e, byte kind, float* rows)
    {
        if (kind == 2 && Ecs.Has<LocalTransform>(e))
        {
            LocalTransform l = Ecs.Get<LocalTransform>(e);
            for (int i = 0; i < 12; ++i)
                l.rows[i] = rows[i];
            Ecs.Set(e, l);
        }
        else if (kind != 0)
        {
            Api.Native->entitySetWorld(Api.Ctx, e.Id, rows);
        }
    }

    /// <summary>The components part read and put on the object's components (its k-th component
    /// with [Sync] members takes the k-th entry; extra entries are skipped).</summary>
    internal static void ReadComponents(NetworkObject? o, ref SyncReader r)
    {
        int count = r.Int();
        List<Component>? list = o != null ? ScriptComponents.Of(o.Entity) : null;
        int at = 0;
        for (int k = 0; k < count && r.Ok; ++k)
        {
            ReadOnlySpan<byte> bytes = r.Bytes(r.Int());
            Component? c = null;
            if (list != null)
                for (; at < list.Count; ++at)
                    if (!list[at].Destroyed && list[at].Type?.SyncRead != null)
                    {
                        c = list[at++];
                        break;
                    }
            if (c == null)
                continue;
            var cr = new SyncReader(bytes);
            c.Type!.SyncRead!(c, ref cr);
        }
    }

    internal static ulong Hash(ReadOnlySpan<byte> data)
    {
        ulong h = 1469598103934665603ul;
        foreach (byte b in data)
            h = (h ^ b) * 1099511628211ul;
        return h;
    }
}
