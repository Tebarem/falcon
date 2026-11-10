using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Falcon.Native;

namespace Falcon;

/// <summary>(generated plumbing for <see cref="IPredicted{TInput}"/>) Its input as bytes.</summary>
public interface IPredictedRaw
{
    int InputSize { get; }
    void BuildRaw(Span<byte> input);
    void SimulateRaw(ReadOnlySpan<byte> input);
}

/// <summary>
/// A component driven by a player's input, the same way everywhere. Each tick:
/// <list type="bullet">
/// <item><see cref="BuildInput"/> runs where its object's owner is (the owning client, or the host
/// for its own objects): read the keyboard, the mouse, an AI's choice.</item>
/// <item><see cref="Simulate"/> runs with that input on the host, and on the owning client as a
/// prediction (no waiting for the host). Put the logic that moves the object here.</item>
/// </list>
/// The owner sends its inputs to the host (each tick's, with the last ones again in case some are
/// lost). When the host's state for a tick arrives, the owner compares it with what it predicted
/// for that tick; deterministic code predicts it exactly, otherwise (another player's hit) it
/// takes the host's state and simulates its later ticks again with their inputs. Without a session
/// it simply runs BuildInput then Simulate. TInput: an unmanaged struct (bools, numbers, vectors).
/// </summary>
public interface IPredicted<TInput> : IPredictedRaw where TInput : unmanaged
{
    void BuildInput(ref TInput input);
    void Simulate(in TInput input);

    int IPredictedRaw.InputSize => Unsafe.SizeOf<TInput>();

    void IPredictedRaw.BuildRaw(Span<byte> input)
    {
        TInput value = default;
        BuildInput(ref value);
        MemoryMarshal.Write(input, in value);
    }

    void IPredictedRaw.SimulateRaw(ReadOnlySpan<byte> input)
    {
        TInput value = MemoryMarshal.Read<TInput>(input);
        Simulate(in value);
    }
}

/// <summary>
/// A capsule other players' shots test against, standing on its entity's position (Radius,
/// Height, Offset). The host keeps where it was each tick for a second, so
/// <see cref="LagCompensation.Raycast"/> can test a client's shot against what that client saw.
/// </summary>
public sealed class Hitbox : Component
{
    [Property] public float Radius = 0.4f;
    [Property] public float Height = 1.8f;
    [Property] public Vector3 Offset;

    internal static ComponentType Type_ => new("Falcon.Hitbox", "Hitbox", static () => new Hitbox(),
        new PropertyInfo[]
        {
            new("Radius", "float", float.NaN, float.NaN, null, null, null, null),
            new("Height", "float", float.NaN, float.NaN, null, null, null, null),
            new("Offset", "vec3", float.NaN, float.NaN, null, null, null, null),
        },
        static (c, w) =>
        {
            var x = (Hitbox)c;
            w.WriteNumber("Radius", x.Radius);
            w.WriteNumber("Height", x.Height);
            ComponentJson.Write(w, "Offset", x.Offset);
        },
        static (c, o) =>
        {
            var x = (Hitbox)c;
            if (o.TryGetProperty("Radius", out System.Text.Json.JsonElement v)) x.Radius = (float)ComponentJson.Number(v, x.Radius);
            if (o.TryGetProperty("Height", out v)) x.Height = (float)ComponentJson.Number(v, x.Height);
            if (o.TryGetProperty("Offset", out v)) x.Offset = ComponentJson.Vector3(v, x.Offset);
        });
}

/// <summary>A lag-compensated hit.</summary>
public readonly record struct LagHit(NetworkObject Object, Vector3 Point, float Distance);

/// <summary>
/// Shots as the shooter saw them. On the host, while a client's input is simulated, hitboxes are
/// tested where that client saw them (its view: interpolated, behind the host); elsewhere, where
/// they are now.
/// </summary>
public static class LagCompensation
{
    private const int Keep = 64;  // ticks of history (about a second)

    private static readonly Dictionary<ulong, List<(ulong id, Vector3 position, Hitbox box)>> s_history = new();
    private static readonly Queue<ulong> s_order = new();

    /// <summary>The first hitbox along the ray (its entity's own, and `ignore`'s, left out).</summary>
    public static bool Raycast(Vector3 from, Vector3 direction, float maxDistance, out LagHit hit, Entity ignore = default)
    {
        hit = default;
        float best = maxDistance;
        Vector3 d = Vector3.Normalize(direction);
        double view = Network.IsHost && Network.SimulatingFor != null ? Network.SimulatingView : -1;
        foreach (NetworkObject o in Network.Sorted)
        {
            if (o.Dormant || o.Entity == ignore || o.Entity.GetComponent<Hitbox>() is not Hitbox box)
                continue;
            Vector3 position = view >= 0 ? PositionAt(o.NetId, view, o.Entity.Position) : o.Entity.Position;
            Vector3 a = position + box.Offset + new Vector3(0f, box.Radius, 0f);
            Vector3 b = position + box.Offset + new Vector3(0f, MathF.Max(box.Height - box.Radius, box.Radius), 0f);
            if (RayCapsule(from, d, a, b, box.Radius, out float t) && t < best)
            {
                best = t;
                hit = new LagHit(o, from + d * t, t);
            }
        }
        return hit.Object != null;
    }

    // The host: each tick's hitbox positions (after its step).
    internal static void Record(ulong tick)
    {
        List<(ulong, Vector3, Hitbox)>? list = null;
        foreach (NetworkObject o in Network.Sorted)
            if (!o.Dormant && o.Entity.GetComponent<Hitbox>() is Hitbox box)
                (list ??= new()).Add((o.NetId, o.Entity.Position, box));
        if (list == null)
            return;
        s_history[tick] = list;
        s_order.Enqueue(tick);
        while (s_order.Count > Keep)
            s_history.Remove(s_order.Dequeue());
    }

    internal static void Clear()
    {
        s_history.Clear();
        s_order.Clear();
    }

    private static Vector3 PositionAt(ulong id, double tick, Vector3 now)
    {
        ulong t0 = (ulong)Math.Floor(tick);
        if (!Find(t0, id, out Vector3 a))
            return now;
        if (!Find(t0 + 1, id, out Vector3 b))
            return a;
        return Vector3.Lerp(a, b, (float)(tick - t0));
    }

    private static bool Find(ulong tick, ulong id, out Vector3 position)
    {
        position = default;
        if (!s_history.TryGetValue(tick, out List<(ulong id, Vector3 position, Hitbox box)>? list))
            return false;
        foreach ((ulong i, Vector3 p, Hitbox _) in list)
            if (i == id)
            {
                position = p;
                return true;
            }
        return false;
    }

    // A ray against a capsule (segment a-b, radius r): the nearest t >= 0 along the unit direction.
    private static bool RayCapsule(Vector3 o, Vector3 d, Vector3 a, Vector3 b, float r, out float t)
    {
        t = float.MaxValue;
        Vector3 ab = b - a;
        Vector3 ao = o - a;
        float abab = Vector3.Dot(ab, ab), abd = Vector3.Dot(ab, d), abao = Vector3.Dot(ab, ao);
        float qa = abab - abd * abd, qb = abab * Vector3.Dot(ao, d) - abao * abd, qc = abab * Vector3.Dot(ao, ao) - abao * abao - r * r * abab;
        if (qa > 1e-6f)
        {
            float disc = qb * qb - qa * qc;
            if (disc >= 0f)
            {
                float s = (-qb - MathF.Sqrt(disc)) / qa;
                float y = abao + s * abd;
                if (s >= 0f && y > 0f && y < abab)
                    t = s;
            }
        }
        foreach (Vector3 c in new[] { a, b })  // (the end caps)
        {
            Vector3 oc = o - c;
            float pb = Vector3.Dot(oc, d), pc = Vector3.Dot(oc, oc) - r * r;
            float disc = pb * pb - pc;
            if (disc >= 0f)
            {
                float s = -pb - MathF.Sqrt(disc);
                if (s >= 0f && s < t)
                    t = s;
            }
        }
        return t != float.MaxValue;
    }
}

public static unsafe partial class Network
{
    private const byte DgInput = 3, DgPing = 4, DgPong = 5;

    // A client's clock against the host's: round trips measured through the whole path (pings the
    // host echoes with its tick), and the host's tick then.
    private static double s_rtt = -1, s_syncTick, s_syncAt, s_lastPing;

    /// <summary>A client's round trip to the host (seconds) as the game sees it (its pings).</summary>
    public static double RoundTrip => s_rtt < 0 ? 0 : s_rtt;

    // A client: a ping now and then (4 a second).
    private static void Ping()
    {
        if (HostConnection is not Connection host || Now - s_lastPing < 0.25)
            return;
        s_lastPing = Now;
        s_out.Clear();
        s_out.Write(DgPing);
        s_out.Write(Now);
        fixed (byte* p = s_out.Written)
            Api.Native->netSend(Api.Ctx, Transport, host.Wire, -1, p, s_out.Length);
    }

    // The host echoes a ping with its tick; the client learns the round trip and the host's tick.
    private static void OnPing(Connection from, ref SyncReader r, bool pong)
    {
        double sent = r.Double();
        if (!pong)
        {
            s_out.Clear();
            s_out.Write(DgPong);
            s_out.Write(sent);
            s_out.Write(Time.Tick);
            // How early its inputs came since the last pong: the fewest ticks they were ahead of
            // the tick simulating them (below 0: late, its last input repeated; none: long.MaxValue).
            long slack = long.MaxValue;
            if (s_clientInputs.TryGetValue(from.Id, out ClientInputs? inputs))
            {
                slack = inputs.MinSlack;
                inputs.MinSlack = long.MaxValue;
            }
            s_out.Write(slack);
            fixed (byte* p = s_out.Written)
                Api.Native->netSend(Api.Ctx, Transport, from.Wire, -1, p, s_out.Length);
            return;
        }
        ulong hostTick = r.ULong();
        long inputSlack = r.Long();
        if (s_rtt < 0)
            s_leadQuietUntil = Now + 0.75;  // (its ticks are set from now on)
        if (r.Ok && inputSlack != long.MaxValue)
            AdjustLead(inputSlack);
        double rtt = Math.Max(Now - sent, 0.0);
        s_rtt = s_rtt < 0 ? rtt : s_rtt * 0.8 + rtt * 0.2;
        // The trip without the frames' own delays: the lowest lately (rising slowly: a slower path).
        s_rttLow = s_rttLow < 0 ? rtt : Math.Min(s_rttLow + 0.0005, rtt);
        s_syncTick = hostTick + (Mode == NetworkMode.Rollback ? s_rttLow : rtt) * 0.5 * 60.0;  // (the host's tick now: it answered half a trip ago)
        s_syncAt = Now;
    }

    // A client's lead over the host beyond half a round trip and 3 ticks, from the host's reports
    // of how early its inputs come: half a round trip doesn't cover the frames in between (a busy
    // host runs several ticks a frame from the inputs it had at the frame's start; a busy client
    // sends several ticks' at once), so a fixed lead had inputs come late, the host repeating the
    // last one and the prediction corrected. It grows at once when they came with fewer than
    // InputSlack ticks to spare (less what the client's ticks are still catching up to), and
    // shrinks by up to a tick a report while they come with more.
    private const double InputSlack = 2.0, MaxLeadExtra = 20.0;
    private static double s_leadExtra, s_behind, s_leadQuietUntil, s_tickAt, s_longFrameAt;

    /// <summary>A client: the ticks its lead over the host grew by for its inputs to come in time
    /// (now, and the most so far).</summary>
    public static double InputLead => s_leadExtra;
    public static double InputLeadMost { get; private set; }

    private static void AdjustLead(long slack)
    {
        if (Now < s_leadQuietUntil)
            return;  // (the host's reports still of inputs from before its ticks were set)
        if (slack < InputSlack)
        {
            s_leadExtra = Math.Min(s_leadExtra + Math.Max(InputSlack - slack - Math.Max(s_behind, 0.0), 0.0), MaxLeadExtra);
            InputLeadMost = Math.Max(InputLeadMost, s_leadExtra);
        }
        else if (slack > InputSlack + 2.0)
            s_leadExtra = Math.Max(s_leadExtra - Math.Min((slack - InputSlack) * 0.25, 1.0), 0.0);
    }

    private const int InputRing = 256;

    /// <summary>On the owning client, while a predicted tick runs (the first time or again after a
    /// correction): don't spawn networked objects or make other lasting effects here, the host's
    /// simulation does them.</summary>
    public static bool IsPredicting { get; private set; }

    /// <summary>On the host, while BuildInput runs for a load-testing bot's object (--bots): build
    /// it as the game's AI would.</summary>
    public static bool BuildingForBot { get; private set; }

    /// <summary>On the host, while a client's input is simulated: that client, and the host tick it
    /// was showing (lag compensation).</summary>
    public static Connection? SimulatingFor { get; private set; }
    internal static double SimulatingView { get; private set; }

    /// <summary>A client's prediction so far: ticks predicted, checked against the host's, and
    /// corrected (simulated again from the host's state).</summary>
    public static long PredictedTicks { get; private set; }
    public static long PredictionChecks { get; private set; }
    public static long Corrections { get; private set; }
    /// <summary>A client: times its ticks were set to keep ahead of the host's.</summary>
    public static long TickJumps { get; private set; }
    /// <summary>The host: clients' inputs found for their tick, and missing (the last one repeated).</summary>
    public static long InputsUsed { get; private set; }
    public static long InputsMissed { get; private set; }

    // A client: each tick's inputs (sent to the host, kept to simulate again), its view, and its
    // owned objects' predicted states; the host's newest state of each, still to be checked.
    private sealed class InputFrame
    {
        public ulong Tick = ulong.MaxValue;
        public ulong View;
        public readonly List<(ulong id, int type, byte[] data)> Inputs = new();
    }
    private static readonly InputFrame[] s_inputs = new InputFrame[InputRing];
    private static readonly Dictionary<ulong, (ulong tick, ulong hash, byte[]? bytes)[]> s_predicted = new();
    private static readonly Dictionary<ulong, (ulong change, byte[] bytes)> s_hostStates = new();

    // The host: each client's inputs by tick.
    private sealed class ClientInputs
    {
        public readonly Dictionary<ulong, InputFrame> Frames = new();
        public readonly Dictionary<(ulong, int), byte[]> Last = new();
        public long Missed;
        public ulong Newest;                   // the client's tick when it sent the latest inputs
        public long Jumps;                     // its ticks' jumps then (TickJumps): inputs of older ticks go
        public long MinSlack = long.MaxValue;  // the fewest ticks they were ahead since the last pong
    }
    private static readonly Dictionary<int, ClientInputs> s_clientInputs = new();

    private static int TypeKey(Component c) => (int)ObjectState.Hash(System.Text.Encoding.UTF8.GetBytes(c.GetType().FullName ?? ""));

    internal static bool OwnsLocally(NetworkObject o) => IsClient && Local != null && o.Owner == Local.Id;

    // A tick of a component driven by input (ScriptComponents.FixedUpdate).
    internal static void RunPredicted(Component c, IPredictedRaw p, NetworkObject? o, bool again = false)
    {
        int size = p.InputSize;
        Span<byte> input = size <= 512 ? stackalloc byte[size] : new byte[size];
        if (o == null || !IsActive)
        {
            p.BuildRaw(input);
            p.SimulateRaw(input);
            return;
        }
        if (Mode == NetworkMode.Rollback)
        {
            if (RollbackStarted)
                RunRollback(p, TypeKey(c), o, input, again);
            return;
        }
        ulong tick = Time.Tick;
        if (IsHost)
        {
            if (o.Owner > 0)
            {
                // A client's object: its input for this tick (or its last one, repeated).
                Connection? owner = ConnectionOf(o.Owner);
                if (owner != null && owner.IsBot)
                {
                    // A bot's: its input built here, by the game's logic.
                    BuildingForBot = true;
                    try
                    {
                        p.BuildRaw(input);
                    }
                    finally
                    {
                        BuildingForBot = false;
                    }
                    p.SimulateRaw(input);
                    return;
                }
                ReadOnlySpan<byte> data = owner != null ? InputOf(owner, tick, o.NetId, TypeKey(c)) : default;
                if (owner == null || data.Length != size)
                    return;  // (nothing from it yet: it holds still)
                SimulatingFor = owner;
                SimulatingView = InputViewOf(owner, tick);
                try
                {
                    p.SimulateRaw(data);
                }
                finally
                {
                    SimulatingFor = null;
                }
                return;
            }
            p.BuildRaw(input);
            p.SimulateRaw(input);
            return;
        }
        if (!OwnsLocally(o))
            return;
        // The owning client: build, keep, (send after the tick), predict.
        p.BuildRaw(input);
        InputFrame frame = s_inputs[tick % InputRing] ??= new InputFrame();
        if (frame.Tick != tick)
        {
            frame.Tick = tick;
            frame.View = (ulong)Math.Max(0.0, ShownTick());
            frame.Inputs.Clear();
        }
        frame.Inputs.Add((o.NetId, TypeKey(c), input.ToArray()));
        IsPredicting = true;
        try
        {
            p.SimulateRaw(input);
        }
        finally
        {
            IsPredicting = false;
        }
        ++PredictedTicks;
    }

    private static ReadOnlySpan<byte> InputOf(Connection c, ulong tick, ulong id, int type)
    {
        if (!s_clientInputs.TryGetValue(c.Id, out ClientInputs? inputs))
            return default;
        if (inputs.Newest > 0)
            inputs.MinSlack = Math.Min(inputs.MinSlack, (long)inputs.Newest - (long)tick);
        if (inputs.Frames.TryGetValue(tick, out InputFrame? frame))
            foreach ((ulong i, int t, byte[] data) in frame.Inputs)
                if (i == id && t == type)
                {
                    inputs.Last[(id, type)] = data;
                    ++InputsUsed;
                    return data;
                }
        ++inputs.Missed;
        ++InputsMissed;
        return inputs.Last.TryGetValue((id, type), out byte[]? last) ? last : default;
    }

    private static double InputViewOf(Connection c, ulong tick) =>
        s_clientInputs.TryGetValue(c.Id, out ClientInputs? inputs) && inputs.Frames.TryGetValue(tick, out InputFrame? f) ? f.View : tick;

    // The host tick a client shows now (interpolated, behind the newest heard of).
    private static double ShownTick() => s_hostTick + (Now - s_hostTickAt) * 60.0 - InterpolationDelay * 60.0;

    // A client after its tick: the inputs of its last ticks to the host (each sent several times).
    internal static void SendInputs()
    {
        if (Mode == NetworkMode.Rollback)
        {
            if (RollbackStarted)
                SendRollbackInputs();
            return;
        }
        if (!IsClient || HostConnection is not Connection host)
            return;
        ulong now = Time.Tick;
        s_out.Clear();
        s_out.Write(DgInput);
        s_out.Write(TickJumps);
        int framesAt = s_out.Length;
        s_out.Write((byte)0);
        byte frames = 0;
        for (ulong back = 0; back < 8 && back <= now; ++back)
        {
            InputFrame? f = s_inputs[(now - back) % InputRing];
            if (f == null || f.Tick != now - back || f.Inputs.Count == 0)
                continue;
            s_state.Clear();
            s_state.Write(f.Tick);
            s_state.Write(f.View);
            s_state.Write(f.Inputs.Count);
            foreach ((ulong id, int type, byte[] data) in f.Inputs)
            {
                s_state.Write(id);
                s_state.Write(type);
                s_state.Write(data.Length);
                s_state.Write(data);
            }
            if (s_out.Length + s_state.Length > 1100)
                break;  // (older ones don't fit: they went in earlier datagrams)
            s_out.Write(s_state.Written);
            ++frames;
        }
        if (frames == 0)
            return;
        byte[] bytes = s_out.Written.ToArray();
        bytes[framesAt] = frames;
        fixed (byte* p = bytes)
            Api.Native->netSend(Api.Ctx, Transport, host.Wire, -1, p, bytes.Length);
    }

    // The host: a client's inputs come in.
    private static void ReceiveInputs(Connection from, ref SyncReader r)
    {
        if (!s_clientInputs.TryGetValue(from.Id, out ClientInputs? inputs))
            s_clientInputs[from.Id] = inputs = new ClientInputs();
        // Its ticks jumped (Reconcile): the inputs it had sent for ticks still to come were for the
        // ticks before the jump (the same numbers, other moments) and go; datagrams sent before it
        // (late) are ignored.
        long jumps = r.Long();
        if (!r.Ok || jumps < inputs.Jumps)
            return;
        if (jumps > inputs.Jumps)
        {
            inputs.Jumps = jumps;
            var stale = new List<ulong>();
            foreach (ulong t in inputs.Frames.Keys)
                if (t > Time.Tick)
                    stale.Add(t);
            foreach (ulong t in stale)
                inputs.Frames.Remove(t);
        }
        int frames = r.Byte();
        for (int i = 0; i < frames && r.Ok; ++i)
        {
            ulong tick = r.ULong();
            ulong view = r.ULong();
            int count = r.Int();
            if (i == 0 && r.Ok)
                inputs.Newest = tick;  // (its first: the client's tick when it sent them)
            bool keep = tick > Time.Tick && !inputs.Frames.ContainsKey(tick);  // (copies of ticks gone: dropped)
            var frame = keep ? new InputFrame { Tick = tick, View = view } : null;
            for (int k = 0; k < count && r.Ok; ++k)
            {
                ulong id = r.ULong();
                int type = r.Int();
                byte[] data = r.Bytes(r.Int()).ToArray();
                frame?.Inputs.Add((id, type, data));
            }
            if (frame != null)
                inputs.Frames[tick] = frame;
        }
        if (inputs.Frames.Count > 192)  // (old ones go)
        {
            var old = new List<ulong>();
            foreach (ulong t in inputs.Frames.Keys)
                if (t + 128 < Time.Tick)
                    old.Add(t);
            foreach (ulong t in old)
                inputs.Frames.Remove(t);
        }
    }

    // A client after each tick's step: its owned objects' states, to check the host's against.
    internal static void AfterStep(ulong tick)
    {
        if (Mode == NetworkMode.Rollback)
        {
            RollbackAfterStep(tick);
            return;
        }
        if (IsHost)
        {
            LagCompensation.Record(tick);
            return;
        }
        if (!IsClient || Local == null)
            return;
        s_tickAt = Now;
        foreach (NetworkObject o in Sorted)
        {
            if (o.Dormant || !OwnsLocally(o))
                continue;
            s_state.Clear();
            ObjectState.Write(o, s_state);
            if (!s_predicted.TryGetValue(o.NetId, out (ulong tick, ulong hash, byte[]? bytes)[]? ring))
                s_predicted[o.NetId] = ring = new (ulong, ulong, byte[]?)[InputRing];
            ring[tick % InputRing] = (tick, ObjectState.Hash(s_state.Written), Corrections == 0 ? s_state.Written.ToArray() : null);
        }
    }

    // A client, before its frame: keeps its ticks ahead of the host's (half the round trip and a
    // few ticks: its inputs arrive before the host needs them), then checks its predictions.
    private static double s_rttLow = -1;

    private static void Reconcile()
    {
        if (!IsClient || Local == null || HostConnection == null)
            return;
        Ping();
        if (s_rtt < 0)
            return;
        // Ahead of the host by half a round trip (an input sent now arrives then), a few ticks and
        // what the host's reports of late inputs added (AdjustLead). Rollback mode: at the host's
        // tick now (everyone on the same tick: a late input is as late both ways), never jumping
        // (every tick is simulated, inputs and all).
        bool rollback = Mode == NetworkMode.Rollback;
        double hostNow = s_syncTick + (Now - s_syncAt) * 60.0;
        ulong target = rollback ? (ulong)hostNow : (ulong)(hostNow + s_rtt * 0.5 * 60.0 + 3.0 + s_leadExtra);
        ulong next = Time.Tick + 1;
        double behind = (double)target - next;
        s_behind = behind;
        // Far off (the start, a long stall), or behind by more than its few ticks to spare (a start
        // near the host's tick, time dropped after a long frame): its ticks go on from the target at
        // once. Catching up a little faster would take seconds, its inputs coming too late all the
        // while. (The ticks this frame still runs for the time since the last one, at most a
        // quarter second's, aren't behind.)
        double owed = s_tickAt > 0 ? Math.Clamp((Now - s_tickAt) * 60.0 * s_rate, 0.0, 15.0) : 0.0;
        bool late = behind - owed > 4.0 + s_leadExtra;
        // After a frame long enough to drop time, the host's tick isn't where the clock says (a
        // machine-wide stall drops the host's too): no jump until a pong since says where it is.
        if (s_tickAt > 0 && Now - s_tickAt > 0.2)
            s_longFrameAt = Now;
        bool known = s_syncAt > s_longFrameAt;
        if (!rollback && known && (Math.Abs(behind) > 30 || late) && target != s_setTo)
        {
            s_setTo = target;
            Api.Native->simSetTick(Api.Ctx, target);
            ++TickJumps;
            s_leadQuietUntil = Now + 0.75;
            // Its inputs kept for the ticks from the target on were the old ticks' (simulated again
            // with those, sent again with these).
            foreach (InputFrame? f in s_inputs)
                if (f != null && f.Tick >= target)
                {
                    f.Tick = ulong.MaxValue;
                    f.Inputs.Clear();
                }
        }
        else
        {
            // Near: its ticks run a little faster or slower until they are where they should be.
            s_rate = (float)Math.Clamp(1.0 + behind * 0.02, rollback ? 0.75 : 0.9, rollback ? 1.25 : 1.1);

            Api.Native->simSetRate(Api.Ctx, s_rate);
        }
        foreach ((ulong id, (ulong change, byte[] bytes)) in s_hostStates)
        {
            if (Find(id) is not NetworkObject o || !OwnsLocally(o))
                continue;
            ulong hash = ObjectState.Hash(bytes);
            if (s_predicted.TryGetValue(id, out (ulong tick, ulong hash, byte[]? bytes)[]? ring) && ring[change % InputRing].tick == change)
            {
                ++PredictionChecks;
                if (ring[change % InputRing].hash == hash)
                    continue;
                if (Corrections == 0 && ring[change % InputRing].bytes is byte[] predicted)
                    Log.Info($"prediction: the first correction (tick {change}, {o.Entity.Name}): {Difference(predicted, bytes)}");
                ++Corrections;
            }
            Resimulate(o, change, bytes);
        }
        s_hostStates.Clear();
    }

    // The host's state for tick `change` put back, the later ticks simulated again with their inputs.
    private static void Resimulate(NetworkObject o, ulong change, byte[] bytes)
    {
        var r = new SyncReader(bytes);
        float* rows = stackalloc float[12];
        byte kind = ObjectState.ReadTransform(ref r, rows);
        ObjectState.ApplyTransform(o.Entity, kind, rows);
        ObjectState.ReadComponents(o, ref r);
        if (ScriptComponents.Of(o.Entity) is not List<Component> components)
            return;
        (ulong tick, ulong hash, byte[]? bytes)[] ring = s_predicted.TryGetValue(o.NetId, out var had) ? had : s_predicted[o.NetId] = new (ulong, ulong, byte[]?)[InputRing];
        ring[change % InputRing] = (change, ObjectState.Hash(bytes), null);
        ulong last = Time.Tick;  // (the ticks below set Time.Tick: the last one run before, kept)
        for (ulong t = change + 1; t <= last; ++t)
        {
            InputFrame? f = s_inputs[t % InputRing];
            if (f == null || f.Tick != t)
                continue;
            Time.BeginTick(t);
            Sim.StartTick(t, 0);
            IsPredicting = true;
            try
            {
                foreach (Component c in components)
                {
                    if (c.Destroyed || c is not IPredictedRaw p)
                        continue;
                    int type = TypeKey(c);
                    foreach ((ulong id, int k, byte[] data) in f.Inputs)
                        if (id == o.NetId && k == type && data.Length == p.InputSize)
                            p.SimulateRaw(data);
                }
            }
            catch (Exception e)
            {
                Log.Error($"prediction: simulating {o.Entity.Name} again failed: {e}");
            }
            finally
            {
                IsPredicting = false;
                Time.EndTick();
            }
            s_state.Clear();
            ObjectState.Write(o, s_state);
            ring[t % InputRing] = (t, ObjectState.Hash(s_state.Written), null);
        }
        Time.BeginTick(last);
        Time.EndTick();
    }

    // What differs between a predicted state and the host's: the transform (how far) or which
    // component's [Sync] members.
    private static string Difference(byte[] predicted, byte[] host)
    {
        var a = new SyncReader(predicted);
        var b = new SyncReader(host);
        float* ra = stackalloc float[12];
        float* rb = stackalloc float[12];
        byte ka = ObjectState.ReadTransform(ref a, ra), kb = ObjectState.ReadTransform(ref b, rb);
        string what = "";
        if (ka != kb)
            what += "transform kind; ";
        else if (ka != 0)
        {
            var pa = new Vector3(ra[3], ra[7], ra[11]);
            var pb = new Vector3(rb[3], rb[7], rb[11]);
            bool same = true;
            for (int i = 0; i < 12; ++i)
                same &= ra[i] == rb[i];
            if (!same)
                what += $"transform (predicted {pa}, host {pb}); ";
        }
        int ca = a.Int(), cb = b.Int();
        for (int k = 0; k < Math.Min(ca, cb); ++k)
            if (!a.Bytes(a.Int()).SequenceEqual(b.Bytes(b.Int())))
                what += $"synced component {k}; ";
        return what.Length > 0 ? what : "nothing visible (lengths?)";
    }

    private static ulong s_setTo;
    private static float s_rate = 1f;

    private static void ResetPrediction()
    {
        s_rtt = -1;
        s_setTo = 0;
        s_leadExtra = 0;
        s_behind = 0;
        s_leadQuietUntil = 0;
        s_tickAt = 0;
        s_longFrameAt = 0;
        if (s_rate != 1f && Api.Native != null)
            Api.Native->simSetRate(Api.Ctx, 1f);
        s_rate = 1f;
        Array.Clear(s_inputs);
        s_predicted.Clear();
        s_hostStates.Clear();
        s_clientInputs.Clear();
        LagCompensation.Clear();
    }
}
