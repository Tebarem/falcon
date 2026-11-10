using System;
using System.Collections.Generic;
using System.Text;
using Falcon.Native;

namespace Falcon;

// The whole world's rollback (Network.Mode = Rollback): every machine simulates everything, physics
// included, from every player's input. A player's input for a tick is built InputDelay ticks
// before it and sent to the host, which passes it on to everyone; a tick whose remote inputs haven't
// come yet runs with each one's last input repeated. When the real input differs, the world goes
// back to the tick before it (its state kept: SimHistory, physics' own) and is simulated again to
// now. Every few seconds each machine hashes a tick everyone's inputs were in for, and the host
// checks the others' hashes against its own (a desync: code that isn't deterministic).
//
// Players: everyone waits, paused, until the host has the players it expects (--net-clients, or
// MaxConnections); then all start at tick 0 together, each player's objects made on every machine
// (INetworkListener.OnConnected / Network.Connected, in connection order). No late join.

public static unsafe partial class Network
{
    /// <summary>Rollback mode: ticks between building an input and simulating it (0-8; more hides
    /// more latency, fewer rollbacks; it is felt as delay).</summary>
    public static int InputDelay { get; set; } = 2;

    /// <summary>Rollback mode: how far back a late input can roll the world (ticks).</summary>
    public static int RollbackWindow { get; set; } = 32;

    /// <summary>Rollback mode: this machine is waiting for others' inputs (it got too far ahead of
    /// them for its rollback window: a hitch rather than a desync).</summary>
    public static bool RollbackStalled { get; private set; }

    /// <summary>Rollback mode: the session has started (everyone's in).</summary>
    public static bool RollbackStarted { get; private set; }

    /// <summary>Rollback mode: rollbacks done, ticks simulated again, inputs that came too late to
    /// roll back to, hashes compared and desyncs found (the host).</summary>
    public static (long rollbacks, long resimulated, long tooLate, long hashChecks, long desyncs) RollbackStats =>
        (s_rbRollbacks, s_rbResimulated, s_rbTooLate, s_rbHashChecks, s_rbDesyncs);

    private const byte MsgRbStart = 20, MsgRbHash = 21;
    private const byte DgRbInput = 6;
    private const ulong HashEvery = 30;

    private static int s_rbExpected;
    private static readonly List<int> s_rbPlayers = new();
    // Inputs by (owner, tick): each object's (NetId, input type, bytes); the owner's own too.
    private static readonly Dictionary<(int owner, ulong tick), List<(ulong id, int type, byte[] data)>> s_rbInputs = new();
    // What each remote input simulated with (a hash) where it was a guess.
    private static readonly Dictionary<(int owner, ulong tick, ulong id, int type), ulong> s_rbUsed = new();
    private static readonly Dictionary<int, ulong> s_rbConfirmed = new();  // each remote owner: the first tick its input isn't in for
    private static ulong s_rbRollbackFrom = ulong.MaxValue, s_rbLast, s_rbNextHash = HashEvery, s_rbRolledAt = ulong.MaxValue;
    private static readonly Dictionary<ulong, ulong> s_rbHashes = new();  // the host's own, by tick
    private static readonly List<(ulong tick, ulong hash, int from)> s_rbPending = new();  // others', waiting for the host's
    private static long s_rbRollbacks, s_rbResimulated, s_rbTooLate, s_rbHashChecks, s_rbDesyncs, s_rbStalls;

    private static int LocalOwner => IsHost ? 0 : Local?.Id ?? -1;
    private static int OwnerOf(NetworkObject o) => o.Owner > 0 ? o.Owner : 0;

    // Launch: a rollback session waits, paused, for its players.
    private static void RollbackLaunch(int expectedClients)
    {
        s_rbExpected = expectedClients;
        Time.Scale = 0f;  // (no ticks until everyone's in)
        SimHistory.SetWindow(RollbackWindow + 2);
        SimHistory.Comparing = false;  // (a rollback's ticks come out differently: new inputs)
        Api.Native->simWindow(Api.Ctx, (uint)(RollbackWindow + 2));
        Log.Info($"network: rollback mode, waiting for {expectedClients} players (input delay {InputDelay} ticks, window {RollbackWindow})");
    }

    // The host: a player joined; everyone in: start.
    private static void RollbackJoined()
    {
        if (RollbackStarted || s_welcomed.Count < s_rbExpected)
            return;
        s_out.Clear();
        s_out.Write(MsgRbStart);
        var players = new List<Connection>(s_welcomed);
        players.Sort((a, b) => a.Id.CompareTo(b.Id));
        s_out.Write(players.Count);
        foreach (Connection c in players)
        {
            s_out.Write(c.Id);
            s_out.Write(c.Name);
        }
        foreach (Connection c in players)
            Send(c, 0, s_out.Written);
        var ids = new List<(int, string)>();
        foreach (Connection c in players)
            ids.Add((c.Id, c.Name));
        RollbackBegin(ids);
    }

    // Everyone: the players, in order; their objects made; the ticks begin.
    private static void RollbackBegin(List<(int id, string name)> players)
    {
        RollbackStarted = true;
        s_rbPlayers.Clear();
        s_rbPlayers.Add(0);
        foreach ((int id, string name) in players)
        {
            s_rbPlayers.Add(id);
            if (!s_connById.TryGetValue(id, out Connection? c))
            {
                c = id == LocalOwner && Local != null ? Local : new Connection { Id = id, Name = name, Welcomed = true };
                s_connById[id] = c;
            }
        }
        foreach (int owner in s_rbPlayers)
            if (owner != LocalOwner)
                s_rbConfirmed[owner] = (ulong)InputDelay;  // (the ticks before have no inputs: none were built)
        foreach ((int id, string _) in players)
            Tell(s_connById[id], connected: true);
        Time.Scale = 1f;
        Log.Info($"network: rollback session started: {s_rbPlayers.Count} players");
    }

    // A tick of a component driven by input, in rollback mode.
    private static void RunRollback(IPredictedRaw p, int type, NetworkObject o, Span<byte> input, bool again)
    {
        ulong tick = Time.Tick;
        int owner = OwnerOf(o);
        if (owner == LocalOwner)
        {
            if (!again)
            {
                // This player's input for a later tick, built now (and sent after the tick).
                p.BuildRaw(input);
                Frame(owner, tick + (ulong)InputDelay).Add((o.NetId, type, input.ToArray()));
            }
            Simulate(p, Find(owner, tick, o.NetId, type), input.Length);
            return;
        }
        // Another's: its input when in, else its last one again (a guess, checked when it comes).
        byte[]? data = Find(owner, tick, o.NetId, type);
        if (data == null)
        {
            for (ulong back = 1; back <= 64 && back <= tick && data == null; ++back)
                data = Find(owner, tick - back, o.NetId, type);
            s_rbUsed[(owner, tick, o.NetId, type)] = data == null ? 0 : ObjectState.Hash(data);
        }
        Simulate(p, data, input.Length);
    }

    private static void Simulate(IPredictedRaw p, byte[]? data, int size)
    {
        if (data != null && data.Length == size)
        {
            p.SimulateRaw(data);
            return;
        }
        Span<byte> none = stackalloc byte[size];  // (nothing yet: no input)
        none.Clear();
        p.SimulateRaw(none);
    }

    private static List<(ulong id, int type, byte[] data)> Frame(int owner, ulong tick)
    {
        if (!s_rbInputs.TryGetValue((owner, tick), out List<(ulong, int, byte[])>? f))
            s_rbInputs[(owner, tick)] = f = new List<(ulong, int, byte[])>();
        return f;
    }

    private static byte[]? Find(int owner, ulong tick, ulong id, int type)
    {
        if (s_rbInputs.TryGetValue((owner, tick), out List<(ulong id, int type, byte[] data)>? f))
            foreach ((ulong i, int t, byte[] d) in f)
                if (i == id && t == type)
                    return d;
        return null;
    }

    // After a tick: this player's inputs of the last ticks (each sent several times) to the host, or
    // from the host to everyone.
    private static void SendRollbackInputs()
    {
        int owner = LocalOwner;
        if (owner < 0)
            return;
        ulong newest = Time.Tick + (ulong)InputDelay;
        s_out.Clear();
        s_out.Write(DgRbInput);
        s_out.Write(owner);
        int framesAt = s_out.Length;
        s_out.Write((byte)0);
        byte frames = 0;
        for (ulong back = 0; back < 8 && back <= newest; ++back)
        {
            if (!s_rbInputs.TryGetValue((owner, newest - back), out List<(ulong id, int type, byte[] data)>? f))
                continue;
            s_state.Clear();
            s_state.Write(newest - back);
            s_state.Write(f.Count);
            foreach ((ulong id, int type, byte[] data) in f)
            {
                s_state.Write(id);
                s_state.Write(type);
                s_state.Write(data.Length);
                s_state.Write(data);
            }
            if (s_out.Length + s_state.Length > 1100)
                break;
            s_out.Write(s_state.Written);
            ++frames;
        }
        if (frames == 0)
            return;
        byte[] bytes = s_out.Written.ToArray();
        bytes[framesAt] = frames;
        foreach (Connection c in s_welcomed)
            fixed (byte* p = bytes)
                Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, bytes.Length);
        ForgetOld();
    }

    // Inputs come in (the host passes them on): kept; one that differs from the guess a tick ran
    // with rolls the world back to it.
    private static void ReceiveRollbackInputs(Connection from, ref SyncReader r, ReadOnlySpan<byte> datagram)
    {
        int owner = r.Int();
        if (owner == LocalOwner || !s_rbConfirmed.ContainsKey(owner))
            return;
        if (IsHost)
            foreach (Connection c in s_welcomed)  // (passed on to the others)
                if (c != from)
                    fixed (byte* p = datagram)
                        Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, datagram.Length);
        int frames = r.Byte();
        for (int i = 0; i < frames && r.Ok; ++i)
        {
            ulong tick = r.ULong();
            int count = r.Int();
            bool known = s_rbInputs.ContainsKey((owner, tick));
            var frame = known ? null : new List<(ulong, int, byte[])>(count);
            for (int k = 0; k < count && r.Ok; ++k)
            {
                ulong id = r.ULong();
                int type = r.Int();
                byte[] data = r.Bytes(r.Int()).ToArray();
                if (frame == null)
                    continue;
                frame.Add((id, type, data));
                // A tick already simulated with a guess for it: wrong guess, roll back to it.
                if (tick <= s_rbLast && s_rbUsed.Remove((owner, tick, id, type), out ulong guessed) && guessed != ObjectState.Hash(data))
                    s_rbRollbackFrom = Math.Min(s_rbRollbackFrom, tick);
            }
            if (frame != null)
                s_rbInputs[(owner, tick)] = frame;
        }
        // Confirmed: in, without a gap, up to (not including) this tick.
        ulong next = s_rbConfirmed[owner];
        while (s_rbInputs.ContainsKey((owner, next)))
            ++next;
        s_rbConfirmed[owner] = next;
    }

    // Each frame, before its ticks: a rollback the inputs asked for, then the hashes due.
    private static void RollbackFrame()
    {
        if (!RollbackStarted)
            return;
        // (At most once a tick: frames faster than ticks would roll back again and again for the
        // same inputs; the next frame with a new tick does it.)
        if (s_rbRollbackFrom != ulong.MaxValue &&
            (s_rbLast != s_rbRolledAt || s_rbRollbackFrom + (ulong)RollbackWindow / 2 <= s_rbLast))  // (deep already: no waiting)
        {
            s_rbRolledAt = s_rbLast;
            ulong from = s_rbRollbackFrom;
            s_rbRollbackFrom = ulong.MaxValue;
            if (from <= s_rbLast)
            {
                ulong back = s_rbLast - from + 1;  // (to the state after the tick before it)
                if (from == 0 || back > (ulong)RollbackWindow || Api.Native->simRollback(Api.Ctx, back) == 0)
                {
                    ++s_rbTooLate;
                    if (s_rbTooLate <= 3)
                        Log.Warning($"network: an input for tick {from} came {back} ticks late: past the rollback window ({RollbackWindow})");
                }
                else
                {
                    ++s_rbRollbacks;
                    s_rbResimulated += (long)back;
                }
            }
        }
        ulong everyone = s_rbLast;
        foreach (ulong next in s_rbConfirmed.Values)
            everyone = Math.Min(everyone, next > 0 ? next - 1 : 0);
        // Never further ahead of the inputs in than a rollback can reach: this frame's ticks capped
        // (none while waiting for them: a hitch rather than a desync).
        if (s_rbRollbackFrom != ulong.MaxValue && s_rbRollbackFrom > 0)  // (a rollback waiting: from its tick)
            everyone = Math.Min(everyone, s_rbRollbackFrom - 1);
        ulong reach = everyone + (ulong)Math.Max(RollbackWindow - 4, 2);
        uint allowed = reach > s_rbLast ? (uint)Math.Min(reach - s_rbLast, 64) : 0u;
        Api.Native->simMaxTicks(Api.Ctx, allowed);
        bool stalled = allowed == 0;
        if (stalled && !RollbackStalled)
            ++s_rbStalls;
        RollbackStalled = stalled;
        // A tick everyone's inputs are in for, hashed (every HashEvery ticks).
        while (s_rbNextHash <= everyone && s_rbLast - s_rbNextHash < (ulong)RollbackWindow)
        {
            ulong tick = s_rbNextHash;
            s_rbNextHash += HashEvery;
            ulong hash = SimHistory.HashOf(tick) ^ Api.Native->simPhysicsHash(Api.Ctx, s_rbLast - tick);
            if (IsHost)
            {
                s_rbHashes[tick] = hash;
                CheckPendingHashes();
            }
            else if (HostConnection is Connection host)
            {
                s_out.Clear();
                s_out.Write(MsgRbHash);
                s_out.Write(tick);
                s_out.Write(hash);
                Send(host, 0, s_out.Written);
            }
        }
    }

    private static void ReceiveRollbackHash(Connection from, ref SyncReader r)
    {
        s_rbPending.Add((r.ULong(), r.ULong(), from.Id));
        CheckPendingHashes();
    }

    private static void CheckPendingHashes()
    {
        for (int i = s_rbPending.Count - 1; i >= 0; --i)
        {
            (ulong tick, ulong hash, int from) = s_rbPending[i];
            if (!s_rbHashes.TryGetValue(tick, out ulong mine))
                continue;
            s_rbPending.RemoveAt(i);
            ++s_rbHashChecks;
            if (mine != hash)
            {
                ++s_rbDesyncs;
                if (s_rbDesyncs <= 3)
                    Log.Error($"network: desync: connection {from}'s tick {tick} differs from the host's (code that isn't deterministic?)");
            }
        }
    }

    // After each tick's step (simulated again too): the newest tick.
    private static void RollbackAfterStep(ulong tick) => s_rbLast = Math.Max(s_rbLast, tick);

    // Inputs and guesses no rollback can reach go.
    private static void ForgetOld()
    {
        if (Time.Tick < 256 || Time.Tick % 64 != 0)
            return;
        ulong before = Time.Tick - 256;
        var old = new List<(int, ulong)>();
        foreach ((int owner, ulong tick) in s_rbInputs.Keys)
            if (tick < before)
                old.Add((owner, tick));
        foreach ((int, ulong) k in old)
            s_rbInputs.Remove(k);
        var used = new List<(int, ulong, ulong, int)>();
        foreach ((int owner, ulong tick, ulong id, int type) k in s_rbUsed.Keys)
            if (k.tick < before)
                used.Add(k);
        foreach ((int, ulong, ulong, int) k in used)
            s_rbUsed.Remove(k);
        var hashes = new List<ulong>();
        foreach (ulong t in s_rbHashes.Keys)
            if (t < before)
                hashes.Add(t);
        foreach (ulong t in hashes)
            s_rbHashes.Remove(t);
    }

    private static void LogRollbackStats()
    {
        if (Mode != NetworkMode.Rollback || !RollbackStarted)
            return;
        Log.Info($"network: rollback: {s_rbRollbacks} rollbacks ({s_rbResimulated} ticks simulated again), {s_rbStalls} waits for inputs, {s_rbTooLate} inputs too late; " +
                 (IsHost ? $"{s_rbHashChecks} hashes checked, {s_rbDesyncs} desyncs" : "hashes sent to the host"));
    }

    private static void ResetRollback()
    {
        RollbackStarted = false;
        s_rbPlayers.Clear();
        s_rbInputs.Clear();
        s_rbUsed.Clear();
        s_rbConfirmed.Clear();
        s_rbHashes.Clear();
        s_rbPending.Clear();
        s_rbRollbackFrom = ulong.MaxValue;
        s_rbLast = 0;
        s_rbRolledAt = ulong.MaxValue;
        s_rbNextHash = HashEvery;
        s_rbRollbacks = s_rbResimulated = s_rbTooLate = s_rbHashChecks = s_rbDesyncs = s_rbStalls = 0;
        RollbackStalled = false;
        if (Api.Native != null && Api.Native->simMaxTicks != null)
            Api.Native->simMaxTicks(Api.Ctx, uint.MaxValue);
    }
}
