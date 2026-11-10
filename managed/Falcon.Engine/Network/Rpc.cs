using System;
using System.Collections.Generic;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// Remote procedure calls: a component's method marked with one of these attributes runs on other
/// machines when it is called (its arguments sent: bool, byte, int, uint, long, ulong, float,
/// double, enums, string, Vector2/3/4, Quaternion, Entity). Its component must be on a
/// NetworkObject. The source generator routes every call to it (C# interceptors), so it is called
/// like any method. Without a session it just runs.
/// <list type="bullet">
/// <item><see cref="BroadcastAttribute"/>: runs everywhere (the caller at once). A client's call
/// goes through the host, allowed when it owns the object.</item>
/// <item><see cref="HostAttribute"/>: runs on the host. A client's call is allowed when it owns
/// the object.</item>
/// <item><see cref="OwnerAttribute"/>: runs on the object's owner (the host for -1).</item>
/// </list>
/// <see cref="AnyCaller"/> lets any client call it. While one runs from the network,
/// <see cref="Caller"/> says who called it.
/// </summary>
public static unsafe class Rpc
{
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class BroadcastAttribute : Attribute
    {
        public bool Reliable { get; init; } = true;
        public bool AnyCaller { get; init; }
    }

    [AttributeUsage(AttributeTargets.Method)]
    public sealed class HostAttribute : Attribute
    {
        public bool Reliable { get; init; } = true;
        public bool AnyCaller { get; init; }
    }

    [AttributeUsage(AttributeTargets.Method)]
    public sealed class OwnerAttribute : Attribute
    {
        public bool Reliable { get; init; } = true;
    }

    /// <summary>The flag for any client being allowed (generated code passes it with the kind).</summary>
    public const int AnyCaller = 4;
    public const int KindBroadcast = 0, KindHost = 1, KindOwner = 2;

    /// <summary>While an RPC from the network runs: the connection it came from (null otherwise,
    /// and for calls made here).</summary>
    public static Connection? Caller { get; private set; }

    public delegate void Dispatch(Component target, ref SyncReader args);

    private static readonly Dictionary<uint, (Dispatch call, int kind)> s_methods = new();
    private static readonly SyncWriter s_args = new();
    private static readonly SyncWriter s_message = new();
    private static NetworkObject? s_object;
    private static Component? s_target;
    private static uint s_method;
    private static int s_kind;
    private static bool s_reliable, s_running;
    private static readonly HashSet<uint> s_warned = new();

    // (a reliable message's or a datagram's kind: none of the session's, prediction's, rollback's or
    // voice's. It was 20, rollback's MsgRbStart: clients took every reliable RPC for a rollback start.)
    internal const byte Message = 30;

    internal static void Forget()
    {
        s_methods.Clear();
        RpcOwners.Clear();
    }

    /// <summary>(generated) A call starting: the writer for its arguments, or null when nothing is
    /// sent (no session, not networked, or it runs from the network now).</summary>
    public static SyncWriter? Begin(Component target, uint method, int kind, bool reliable)
    {
        if (!Network.IsActive || s_running || Network.IsPredicting)
            return null;  // (a prediction's calls run here only: the host's simulation makes them for real)
        NetworkObject? o = Network.Of(target.Entity);
        if (o == null || o.NetId >= Network.LocalIds)
        {
            if (s_warned.Add(method))
                Log.Warning($"{target.GetType().Name}: an RPC on an entity without a NetworkObject runs here only");
            return null;
        }
        s_object = o;
        s_target = target;
        s_method = method;
        s_kind = kind;
        s_reliable = reliable;
        s_args.Clear();
        return s_args;
    }

    /// <summary>(generated) The call's arguments written: sent where it runs.</summary>
    public static void End()
    {
        if (s_object == null || s_target == null)
            return;
        WriteMessage(s_object, s_target, s_method);
        int kind = s_kind & 3;
        if (Network.IsHost)
        {
            if (kind == KindBroadcast)
                foreach (Connection c in Network.Connections)
                    Network.SendRpc(c, s_message.Written, s_reliable, s_object.NetId);
            else if (kind == KindOwner && s_object.Owner > 0)
                foreach (Connection c in Network.Connections)
                    if (c.Id == s_object.Owner)
                        Network.SendRpc(c, s_message.Written, s_reliable, s_object.NetId);
        }
        else if (Network.HostConnection is Connection host && !(kind == KindOwner && RunsHere(s_target, s_kind)))
        {
            Network.SendRpc(host, s_message.Written, s_reliable, s_object.NetId);
        }
        s_object = null;
        s_target = null;
    }

    /// <summary>(generated) Whether the call also runs on this machine.</summary>
    public static bool RunsHere(Component target, int kind)
    {
        if (!Network.IsActive || s_running)
            return true;
        NetworkObject? o = Network.Of(target.Entity);
        if (o == null)
            return true;
        int localId = Network.IsHost ? -1 : Network.Local?.Id ?? -2;
        return (kind & 3) switch
        {
            KindBroadcast => true,
            KindHost => Network.IsHost,
            _ => o.Owner == localId || (Network.IsHost && o.Owner <= 0),
        };
    }

    private static void WriteMessage(NetworkObject o, Component target, uint method)
    {
        s_message.Clear();
        s_message.Write(Message);
        s_message.Write(o.NetId);
        s_message.Write(OrdinalOf(target));
        s_message.Write(method);
        s_message.Write(s_args.Written);
    }

    // Which of its entity's components of its class it is (usually 0).
    private static int OrdinalOf(Component target)
    {
        int n = 0;
        if (ScriptComponents.Of(target.Entity) is List<Component> list)
            foreach (Component c in list)
            {
                if (ReferenceEquals(c, target))
                    return n;
                if (!c.Destroyed && c.GetType() == target.GetType())
                    ++n;
            }
        return 0;
    }

    // An RPC come in (the message after its kind byte).
    internal static void Receive(Connection from, ref SyncReader r, ReadOnlySpan<byte> whole)
    {
        ulong netId = r.ULong();
        int ordinal = r.Int();
        uint method = r.UInt();
        if (!s_methods.TryGetValue(method, out (Dispatch call, int kind) m))
            return;
        NetworkObject? o = Network.Find(netId);
        if (o == null || o.Dormant)
            return;
        Component? target = null;
        if (ScriptComponents.Of(o.Entity) is List<Component> list)
        {
            int n = 0;
            Type? type = null;
            foreach (Component c in list)
            {
                if (c.Destroyed)
                    continue;
                // (the method's class: the first component whose dispatch accepts it, by ordinal)
                if (type == null && Registered(c, method))
                    type = c.GetType();
                if (type != null && c.GetType() == type && n++ == ordinal)
                {
                    target = c;
                    break;
                }
            }
        }
        if (target == null)
            return;
        int kind = m.kind & 3;
        if (Network.IsHost)
        {
            // A client's call: allowed when it owns the object (or the method lets anyone).
            if ((m.kind & AnyCaller) == 0 && o.Owner != from.Id)
            {
                Log.Warning($"rpc: {from} may not call {target.GetType().Name}'s method on object {netId} (it doesn't own it)");
                return;
            }
            if (kind == KindBroadcast)
            {
                foreach (Connection other in Network.Connections)
                    if (other != from)
                        Network.SendRpc(other, whole, true, netId);
            }
            else if (kind == KindOwner && o.Owner > 0 && o.Owner != from.Id)
            {
                foreach (Connection owner in Network.Connections)
                    if (owner.Id == o.Owner)
                        Network.SendRpc(owner, whole, true, netId);
                return;
            }
        }
        Caller = from;
        s_running = true;
        try
        {
            SyncReader args = r;
            m.call(target, ref args);
        }
        catch (Exception e)
        {
            Log.Error($"rpc {target.GetType().Name} on object {netId} failed: {e}");
        }
        finally
        {
            s_running = false;
            Caller = null;
        }
    }

    // Whether `method` is one of this component's (its class's methods were registered with it).
    private static bool Registered(Component c, uint method) => RpcOwners.TryGetValue(method, out Type? t) && t.IsInstanceOfType(c);

    internal static readonly Dictionary<uint, Type> RpcOwners = new();

    /// <summary>(generated) An RPC method: its id, kind, class and how to run it from its
    /// arguments' bytes.</summary>
    public static void Register(uint method, int kind, Type owner, Dispatch call)
    {
        s_methods[method] = (call, kind);
        RpcOwners[method] = owner;
    }
}
