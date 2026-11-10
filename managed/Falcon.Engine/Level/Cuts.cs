using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>A cut volume's shape (level/mesh_cut.h): a unit shape sized by its entity's scale.</summary>
public enum CutShape
{
    Box,
    Cylinder,
    Sphere,
    Mesh,
    Damage,
}

/// <summary>One cut: what the level meshes (and the imported art) it overlaps are drawn and collided
/// with taken away (or, Intersect, kept). The same op gives the same geometry on every machine.</summary>
public struct CutOp
{
    public CutShape Shape;
    public Vector3 Position;
    public Quaternion Rotation;
    public Vector3 Size;
    public bool Intersect;
    public uint Seed;          // (Damage) its lumps
    public float Roughness;    // (Damage) how far they reach, 0..0.9
    public string? Cap;        // the faces it makes (a material path; null: the target's own)

    public static CutOp Box(Vector3 position, Vector3 size) => new() { Shape = CutShape.Box, Position = position, Rotation = Quaternion.Identity, Size = size };

    public static CutOp Damage(Vector3 position, float size, uint seed, float roughness = 0.35f) =>
        new() { Shape = CutShape.Damage, Position = position, Rotation = Quaternion.Identity, Size = new Vector3(size), Seed = seed, Roughness = roughness };

    internal void Write(SyncWriter w)
    {
        w.Write((byte)Shape);
        w.Write(Position);
        w.Write(Rotation);
        w.Write(Size);
        w.Write(Intersect);
        w.Write(Seed);
        w.Write(Roughness);
        w.Write(Cap ?? "");
    }

    internal static CutOp Read(ref SyncReader r) => new()
    {
        Shape = (CutShape)r.Byte(),
        Position = r.Vector3(),
        Rotation = r.Quaternion(),
        Size = r.Vector3(),
        Intersect = r.Bool(),
        Seed = r.UInt(),
        Roughness = r.Float(),
        Cap = r.String() is { Length: > 0 } s ? s : null,
    };
}

/// <summary>
/// Cutting the level while the game runs (destruction: holes blown in walls, doors cut where a
/// player builds one): each cut is an entity with a cut volume; the level meshes it overlaps are
/// cut again on worker threads (drawn as they were until then, usually a frame or two; their
/// colliders too), imported art drawn as a cut copy. Destroy the entity and the cut is gone.
/// </summary>
public static unsafe class Cuts
{
    /// <summary>A cut where the op says: its entity.</summary>
    public static Entity Cut(in CutOp op)
    {
        Entity e = Ecs.Create();
        var w = new WorldTransform();
        w.Set(op.Position, op.Rotation == default ? Quaternion.Identity : op.Rotation, op.Size == default ? Vector3.One : op.Size);
        e.World = w;
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->cutSet == null)
            return e;
        byte[] cap = Encoding.UTF8.GetBytes((op.Cap ?? "") + "\0");
        fixed (byte* c = cap)
            l->cutSet(l->context, e.Id, (int)op.Shape, op.Intersect ? 1 : 0, op.Seed, op.Roughness, c);
        return e;
    }

    /// <summary>A cut taken back.</summary>
    public static void Remove(Entity cut)
    {
        if (cut.Id != 0)
            cut.Destroy();
    }

    /// <summary>Whether cuts are still being made (on worker threads): until they are, the meshes
    /// they cut are drawn, collide and hash as they were.</summary>
    public static bool Pending
    {
        get
        {
            FxLevelApi* l = LevelNative.Api;
            return l != null && l->cutsPending != null && l->cutsPending(l->context) > 0;
        }
    }

    /// <summary>A hash of the level as it is drawn now (the level meshes' geometry, cuts and all):
    /// machines that cut the same way have the same one once no cuts are Pending (a networked game's
    /// check).</summary>
    public static ulong GeometryHash()
    {
        FxLevelApi* l = LevelNative.Api;
        return l == null || l->geometryHash == null ? 0 : l->geometryHash(l->context);
    }
}

/// <summary>
/// Cuts made on every machine of a networked game, in the same order (an op list): put it on an
/// entity with a NetworkObject and call Cut on the host (or on a client that owns it: one machine
/// cuts). Without a session it just cuts here. Ops, Count and GeometryHash say what each machine
/// has.
/// <para>Each cut is numbered, and applied in number order wherever it arrives. The host's count
/// is part of the object's synced state, so a machine that joins late (or missed some) sees it is
/// behind and asks the host for the cuts from its own count; the host sends them again (to
/// everyone: the machines that have them already let them be).</para>
/// </summary>
public sealed class NetworkedCuts : Component
{
    private const uint MethodCut = 0x4C56_4332u;      // ("LVC2": a numbered cut)
    private const uint MethodCatchUp = 0x4C56_4333u;  // ("LVC3": a machine asking for the cuts from a number)

    /// <summary>The cuts made here since the last Clear, in order.</summary>
    public IReadOnlyList<CutOp> Ops => _ops;

    /// <summary>The cuts made here since the start, Clear or not (the next one's number).</summary>
    public int Count => _all.Count;

    /// <summary>The times this machine found it was behind the host and asked for the cuts it
    /// missed (a late joiner: once or so).</summary>
    public int CatchUps { get; private set; }

    /// <summary>(A client) the host's Count as last synced: more than Count while it catches up.</summary>
    public int HostCount => _hostCount;

    private readonly List<CutOp> _ops = new();
    private readonly List<CutOp> _all = new();  // (every one by its number: sent again to a machine behind)
    private readonly List<Entity> _cuts = new();
    private readonly Dictionary<int, CutOp> _early = new();  // (come before the ones before them)
    private int _hostCount;                  // the host's Count, as last synced
    private int _resendFrom = int.MaxValue;  // (the host) a machine asked for the cuts from here
    private long _askedAt;                  // (a client) when it last asked (0: never)

    public override void OnAwake() => Register();

    /// <summary>The cut, on every machine (the caller's at once).</summary>
    public void Cut(CutOp op)
    {
        Register();
        int number = _all.Count;
        Send(number, op);
        if (Rpc.RunsHere(this, Rpc.KindBroadcast))
            Receive(number, op);
    }

    /// <summary>All its cuts taken back (here only; the numbering goes on).</summary>
    public void Clear()
    {
        foreach (Entity e in _cuts)
            Cuts.Remove(e);
        _cuts.Clear();
        _ops.Clear();
    }

    public override void OnDestroy() => Clear();

    public override void OnUpdate(float dt)
    {
        if (!Network.IsActive)
            return;
        if (Network.IsHost)
        {
            // (sent here, not in the asking RPC: an RPC can't send while one runs)
            for (int i = _resendFrom; i < _all.Count; ++i)
                Send(i, _all[i]);
            _resendFrom = int.MaxValue;
        }
        else if (_hostCount > _all.Count && (_askedAt == 0 || Environment.TickCount64 - _askedAt > 1000))
        {
            _askedAt = Environment.TickCount64;  // (again after a second if they don't come)
            ++CatchUps;
            Register();
            if (Rpc.Begin(this, MethodCatchUp, Rpc.KindHost | Rpc.AnyCaller, true) is SyncWriter w)
            {
                w.Write(_all.Count);
                Rpc.End();
            }
        }
    }

    private void Send(int number, in CutOp op)
    {
        if (Rpc.Begin(this, MethodCut, Rpc.KindBroadcast, true) is SyncWriter w)
        {
            w.Write(number);
            op.Write(w);
            Rpc.End();
        }
    }

    // A numbered cut: applied in order (one had already let be, an early one kept for its turn).
    private void Receive(int number, in CutOp op)
    {
        if (number < _all.Count)
            return;
        if (number > _all.Count)
        {
            _early[number] = op;
            return;
        }
        Apply(op);
        while (_early.Remove(_all.Count, out CutOp next))
            Apply(next);
    }

    private void Apply(in CutOp op)
    {
        _all.Add(op);
        _ops.Add(op);
        _cuts.Add(Cuts.Cut(op));
    }

    private static void Register()
    {
        Rpc.Register(MethodCut, Rpc.KindBroadcast, typeof(NetworkedCuts), static (Component target, ref SyncReader r) =>
        {
            int number = r.Int();
            ((NetworkedCuts)target).Receive(number, CutOp.Read(ref r));
        });
        Rpc.Register(MethodCatchUp, Rpc.KindHost | Rpc.AnyCaller, typeof(NetworkedCuts), static (Component target, ref SyncReader r) =>
        {
            var c = (NetworkedCuts)target;
            c._resendFrom = Math.Min(c._resendFrom, Math.Max(r.Int(), 0));
        });
    }

    // Synced: the host's count (a client's own history: the most it knows of).
    internal static ComponentType Type_ => new("Falcon.Level.NetworkedCuts", "Networked Cuts", static () => new NetworkedCuts(),
        System.Array.Empty<PropertyInfo>(), static (c, w) => { }, static (c, o) => { })
    {
        SyncWrite = static (c, w) =>
        {
            var x = (NetworkedCuts)c;
            w.Write(Math.Max(x._all.Count, x._hostCount));
        },
        SyncRead = static (Component c, ref SyncReader r) => ((NetworkedCuts)c)._hostCount = r.Int(),
    };
}
