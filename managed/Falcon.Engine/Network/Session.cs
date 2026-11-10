using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text;
using System.Text.Json;
using Falcon.Native;

namespace Falcon;

/// <summary>What this machine is in the session.</summary>
/// <summary>How a session simulates (Network.Mode; the host's choice, the same for everyone).</summary>
public enum NetworkMode
{
    /// <summary>The host simulates and sends the networked objects' state; clients predict their
    /// own (2 to thousands of players; late join; cheat-resistant).</summary>
    Authoritative,
    /// <summary>Every machine simulates the whole world, physics included, from everyone's inputs;
    /// a late input rolls the world back and simulates it again (2 to 8 players; tiny bandwidth;
    /// fighting games, RTS).</summary>
    Rollback,
}

public enum NetworkRole
{
    None,
    Host,    // runs the simulation and sends it (a dedicated server, or a listen server's player)
    Client,  // shows the host's simulation
}

/// <summary>Another machine of the session: on the host, each client; on a client, the host
/// (<see cref="Network.HostConnection"/>).</summary>
public sealed partial class Connection
{
    /// <summary>The host's number for it (the host itself: 0).</summary>
    public int Id { get; internal set; }
    public string Name { get; internal set; } = "";
    public bool IsHost => Id == 0;

    /// <summary>A load-testing bot (--bots): the host builds its input with the game's logic.</summary>
    public bool IsBot { get; internal set; }

    /// <summary>Round trip time (ms) and the share of packets sent again (%).</summary>
    public unsafe float Ping => Stats.rttMs;
    public unsafe float Loss => Stats.lossPercent;
    public unsafe ulong BytesSent => Stats.bytesSent;
    public unsafe ulong BytesReceived => Stats.bytesReceived;

    internal uint Wire;     // the transport's connection
    internal bool Welcomed;  // past the handshake
    internal unsafe FxNetStats Stats
    {
        get
        {
            FxNetStats s;
            Api.Native->netStats(Api.Ctx, Network.Transport, Wire, &s);
            return s;
        }
    }

    // The host's replication to it: the objects it has (and the change of each it acknowledged),
    // what each snapshot datagram carried (until acknowledged), and its verify reports.
    internal readonly HashSet<ulong> Spawned = new();
    internal readonly Dictionary<ulong, ulong> Acked = new();
    internal readonly Dictionary<ulong, List<(ulong id, ulong change)>> InFlight = new();
    internal readonly Dictionary<ulong, float> Priority = new();
    internal readonly List<Vector3> Focus = new();
    internal readonly List<(float priority, ulong id)> Candidates = new();
    internal readonly List<ulong> Gone = new();
    internal readonly List<(int channel, byte[] data)> Outgoing = new();
    internal readonly SyncWriter Writer = new();
    internal int MaxDatagram = 1200;
    internal readonly HashSet<ulong> Recipes = new();  // the recipes it has been sent
    internal readonly SyncWriter Batch = new();         // this send's reliable object messages
    internal int BatchCount;
    internal int SpawnSpent;  // this send's spawn bytes (from its budget)
    internal readonly HashSet<(int, int)> Visited = new();
    internal long SpawnBytes, SnapshotBytes, OtherBytes;  // this send's

    public override string ToString() => $"connection {Id} ({Name})";
}

/// <summary>Told about connections coming and going: components and systems implementing it, and
/// the Game.</summary>
public interface INetworkListener
{
    void OnConnected(Connection connection) { }
    void OnDisconnected(Connection connection) { }
}

/// <summary>
/// The session: the host runs the simulation and replicates it; clients show it.
/// <list type="bullet">
/// <item>Start one with <see cref="Host"/> or <see cref="Connect"/>, or from the command line
/// (--host [port], --connect quic://host:port[#sha256]) before the game's OnStart.</item>
/// <item>The host sends each client every NetworkObject: its recipe once (a prefab, or its
/// components and their [Property] values) over a reliable channel, then its state (transform and
/// [Sync] members) in datagrams at <see cref="SendRate"/> whenever it changed, until the client
/// has acknowledged it. Destroyed objects go on every client.</item>
/// <item>On clients, the host's objects are proxies: their components don't run OnFixedUpdate (the
/// host's simulation is the one), their [Sync] members take the host's values as they come, and
/// their transforms are interpolated <see cref="InterpolationDelay"/> behind the host.</item>
/// </list>
/// </summary>
public static unsafe partial class Network
{
    private const uint Protocol = 8;  // (7: input datagrams carry the jump count; 8: RPC messages' kind byte 30)
    internal static uint ProtocolVersion => Protocol;
    internal static ulong SchemaHashOf() => SchemaHash();
    internal const ulong LocalIds = 1ul << 62;  // NetIds a client gives objects only it has

    // Message kinds: reliable (channel 0: the session, 1: objects) and datagrams.
    private const byte MsgHello = 1, MsgWelcome = 2, MsgRefuse = 3, MsgVerify = 4, MsgSay = 5, MsgSpawn = 10, MsgDespawn = 11, MsgState = 12;
    private const byte MsgBatch = 13;  // (a send's reliable object messages in one: their count, then each's length and bytes)
    private const byte DgSnapshot = 1, DgAck = 2;

    public static NetworkRole Role { get; private set; }

    /// <summary>How the session simulates (set before hosting; clients get the host's).</summary>
    public static NetworkMode Mode { get; set; }
    public static bool IsActive => Role != NetworkRole.None;
    public static bool IsHost => Role == NetworkRole.Host;
    public static bool IsClient => Role == NetworkRole.Client;

    /// <summary>Host: its clients past the handshake. Client: the host, once welcomed.</summary>
    public static IReadOnlyList<Connection> Connections => s_welcomed;

    /// <summary>A client's own connection (its Id is the host's number for it); null on the host.</summary>
    public static Connection? Local { get; private set; }

    /// <summary>A client's connection to the host (null on the host, or before it is welcomed).</summary>
    public static Connection? HostConnection => IsClient && s_welcomed.Count > 0 ? s_welcomed[0] : null;

    /// <summary>The host's port (after Host), and its certificate's SHA-256 (QUIC: what clients pin).</summary>
    public static int Port { get; private set; }
    public static string Certificate { get; private set; } = "";

    /// <summary>Snapshots a second the host sends (at most one a tick).</summary>
    public static int SendRate { get; set; } = 30;

    /// <summary>How far behind the host's newest state clients show its objects (seconds): room for
    /// late and lost datagrams.</summary>
    public static float InterpolationDelay { get; set; } = 0.1f;

    /// <summary>The newest host tick a client has heard of (the host: its own tick).</summary>
    public static ulong HostTick => IsClient ? s_hostTick : Time.Tick;

    public static event Action<Connection>? Connected;
    public static event Action<Connection>? Disconnected;

    /// <summary>The host: at most this many clients (0: no limit); a password they must give.
    /// Clients: the password they give. (--max-players, --password.)</summary>
    public static int MaxConnections { get; set; }
    public static string Password { get; set; } = "";

    /// <summary>A dedicated server's console line the engine doesn't handle itself (status, kick,
    /// say, quit are its own): the game's commands.</summary>
    public static event Action<string>? Command;

    /// <summary>A message from the host's console (say).</summary>
    public static event Action<string>? Message;

    internal static uint Transport;
    private static readonly Dictionary<uint, Connection> s_byWire = new();
    private static readonly List<Connection> s_welcomed = new();
    private static readonly Dictionary<int, Connection> s_connById = new();

    internal static Connection? ConnectionOf(int id) => s_connById.TryGetValue(id, out Connection? c) ? c : null;
    private static int s_nextConnection = 1;
    private static bool s_verify;
    private static bool s_child;  // (--net-child: a host's client process, its run ending with the host)
    private static ulong s_lastSend = ulong.MaxValue;
    private static byte[] s_buffer = new byte[64 * 1024];
    private static readonly SyncWriter s_out = new();
    private static readonly SyncWriter s_state = new();

    // The host: each object's state as last sent, the tick it last changed, and its changes' hashes
    // (verify reports name a change).
    private sealed class Record
    {
        public NetworkObject? Object;
        public Vector3 Position;
        public float Radius;
        public byte[]? Recipe;  // its prefab, or its components and their properties (built at its first spawn)
        public ulong RecipeHash;
        public byte[] Bytes = Array.Empty<byte>();
        public ulong Change;
        public ulong Hash;
        public readonly Dictionary<ulong, ulong> History = new();  // change tick -> hash
        public readonly Queue<ulong> HistoryOrder = new();
    }
    private static readonly Dictionary<ulong, Record> s_records = new();
    private static long s_verifyChecks, s_verifyMismatches;

    // A client: each object's last change applied, its hash, and its transforms to interpolate.
    private sealed class Proxy
    {
        public ulong Change;
        public ulong Hash;
        public readonly List<(double tick, float[] rows, byte kind)> Poses = new();
    }
    private static readonly Dictionary<ulong, Proxy> s_proxies = new();
    private static readonly Dictionary<ulong, ulong> s_pendingIds = new();  // entity -> NetId it is to have
    private static readonly Dictionary<ulong, (string prefab, List<(string type, string properties)> components)> s_recipes = new();
    private static ulong s_hostTick;
    private static double s_hostTickAt;
    private static ulong s_nextLocal = LocalIds;
    private static bool s_assignLocal;  // a client past its scene's start: its own objects get local ids
    private static bool s_refused, s_wasWelcomed;
    private static int s_botsJoined;
    private static NetworkRole s_lastRole;
    private static long s_snapshots;
    private static int s_verifyFrame;

    private static double Now => System.Diagnostics.Stopwatch.GetTimestamp() / (double)System.Diagnostics.Stopwatch.Frequency;

    // ---- starting and ending

    /// <summary>Hosts a session on `port` (0: any free one), on every address or only on
    /// `address` (QUIC: an IP; "127.0.0.1" takes only this machine's clients, with no firewall
    /// prompt). False when it can't listen.</summary>
    public static bool Host(int port = 27015, string scheme = "quic", string address = "")
    {
        Shutdown();
        if (!Open(scheme + "://" + address))
            return false;
        int bound = Api.Native->netListen(Api.Ctx, Transport, port);
        if (bound < 0)
        {
            Log.Error($"network: cannot host on port {port}");
            Close();
            return false;
        }
        Role = s_lastRole = NetworkRole.Host;
        Port = bound;
        Certificate = ReadCertificate();
        Log.Info($"network: hosting on {scheme}://{(address.Length > 0 ? address + ":" : "")}{port}{(Certificate.Length > 0 ? " (certificate " + Certificate + ")" : "")}, port {bound}");
        return true;
    }

    /// <summary>Joins the session at `address` (quic://host:port[#sha256], loopback://port). The
    /// game then shows the host's objects; Connected fires once the host welcomes it.</summary>
    public static bool Connect(string address)
    {
        Shutdown();
        if (!Open(address))
            return false;
        int scheme = address.IndexOf("://", StringComparison.Ordinal);
        string rest = scheme < 0 ? address : address[(scheme + 3)..];
        byte[] utf8 = Encoding.UTF8.GetBytes(rest + "\0");
        uint wire;
        fixed (byte* p = utf8)
            wire = Api.Native->netConnect(Api.Ctx, Transport, p);
        Role = s_lastRole = NetworkRole.Client;
        s_byWire[wire] = new Connection { Id = 0, Name = "host", Wire = wire };
        Log.Info($"network: connecting to {address}");
        return true;
    }

    /// <summary>Leaves the session (a host ends it for every client).</summary>
    public static void Disconnect() => Shutdown();

    private static bool Open(string address)
    {
        byte[] utf8 = Encoding.UTF8.GetBytes(address + "\0");
        fixed (byte* p = utf8)
            Transport = Api.Native->netOpen(Api.Ctx, p);
        if (Transport == 0)
            Log.Error($"network: no transport for {address}");
        return Transport != 0;
    }

    private static void Close()
    {
        if (Transport != 0)
            Api.Native->netClose(Api.Ctx, Transport);
        Transport = 0;
    }

    private static string ReadCertificate()
    {
        byte* buffer = stackalloc byte[128];
        int n = Api.Native->netCertificate(Api.Ctx, Transport, buffer, 128);
        return n > 0 ? Encoding.UTF8.GetString(buffer, n) : "";
    }

    // The game ends, or a session gives way to another.
    internal static void Shutdown()
    {
        if (Transport != 0)
        {
            foreach (Connection c in s_byWire.Values)
            {
                EndSteamAuth(c);
                Send(c, 0, Array.Empty<byte>(), disconnect: true);
            }
            Close();
        }
        ShutdownSteamAuth();
        s_byWire.Clear();
        s_welcomed.Clear();
        s_connById.Clear();
        LogRollbackStats();
        ResetPrediction();
        ResetRollback();
        s_records.Clear();
        s_proxies.Clear();
        s_pendingIds.Clear();
        s_recipes.Clear();
        Role = NetworkRole.None;
        Local = null;
        s_lastSend = ulong.MaxValue;
    }

    // At the game's start, before its components wake: what the command line asks.
    internal static void Launch()
    {
        s_assignLocal = false;
        Span<byte> buffer = stackalloc byte[512];
        int n;
        fixed (byte* p = buffer)
            n = Api.Native->netLaunch(Api.Ctx, p, buffer.Length);
        if (n <= 2)
            return;
        using JsonDocument doc = JsonDocument.Parse(Encoding.UTF8.GetString(buffer[..n]));
        JsonElement root = doc.RootElement;
        s_verify = root.TryGetProperty("verify", out JsonElement v) && v.ValueKind == JsonValueKind.True;
        s_child = root.TryGetProperty("child", out JsonElement child) && child.ValueKind == JsonValueKind.True;
        if (root.TryGetProperty("project", out JsonElement project))
            ApplyProjectSettings(project);  // (the project's settings; the command line's below win)
        if (root.TryGetProperty("mode", out JsonElement mode))
            Mode = mode.GetString() == "rollback" ? NetworkMode.Rollback : NetworkMode.Authoritative;
        if (root.TryGetProperty("maxPlayers", out JsonElement max))
            MaxConnections = max.GetInt32();
        if (root.TryGetProperty("password", out JsonElement password))
            Password = password.GetString() ?? "";
        if (root.TryGetProperty("bots", out JsonElement bots) && root.TryGetProperty("connect", out JsonElement at))
        {
            BotSwarm.Start(bots.GetInt32(), at.GetString() ?? "");
            return;
        }
        if (root.TryGetProperty("host", out JsonElement host))
        {
            string scheme = root.TryGetProperty("transport", out JsonElement t) ? t.GetString() ?? "quic" : "quic";
            int clientCount = root.TryGetProperty("clients", out JsonElement cc) ? cc.GetInt32() : 0;
            string hostAddress = root.TryGetProperty("hostAddress", out JsonElement ha) ? ha.GetString() ?? "" : "";
            if (Host(host.GetInt32(), scheme, hostAddress) && Mode == NetworkMode.Rollback)
                RollbackLaunch(clientCount > 0 ? clientCount : Math.Max(MaxConnections, 1));
            if (IsHost && root.TryGetProperty("clients", out JsonElement clients) && clients.GetInt32() > 0)
            {
                string address = scheme switch
                {
                    "steam" => $"steam://{Steamworks.SteamUser.GetSteamID()}:{Port}",
                    "steamip" => $"steamip://127.0.0.1:{Port}",
                    _ => $"quic://127.0.0.1:{Port}" + (Certificate.Length > 0 ? "#" + Certificate : ""),
                };
                byte[] utf8 = Encoding.UTF8.GetBytes(address + "\0");
                fixed (byte* p = utf8)
                    Api.Native->netLaunchClients(Api.Ctx, clients.GetInt32(), p);
            }
        }
        else if (root.TryGetProperty("connect", out JsonElement connect))
        {
            Connect(connect.GetString() ?? "");
            if (Mode == NetworkMode.Rollback)
                RollbackLaunch(0);
        }
    }

    // The scene's own objects have woken (the same NetIds on every machine): a client's next ones are its own.
    internal static void SceneStarted() => s_assignLocal = IsClient && Mode != NetworkMode.Rollback;

    // ---- NetIds

    internal static ulong AssignId(NetworkObject o)
    {
        if (s_pendingIds.Remove(o.Entity.Id, out ulong id))
            return id;
        return s_assignLocal ? s_nextLocal++ : NextId++;
    }

    /// <summary>On a client, an object the host replicates (its components don't run
    /// OnFixedUpdate there: the host's simulation is the one).</summary>
    public static bool IsProxy(NetworkObject o) => IsClient && Mode != NetworkMode.Rollback && o.NetId < LocalIds && !OwnsLocally(o);

    // ---- each frame

    // Before the game's update: what came in.
    internal static void FrameStart()
    {
        if (Transport == 0)
            return;
        FxNetEvent e;
        while (true)
        {
            int got;
            fixed (byte* p = s_buffer)
                got = Api.Native->netPoll(Api.Ctx, Transport, &e, p, s_buffer.Length);
            if (got == 0)
                break;
            if (got < 0)
            {
                s_buffer = new byte[Math.Max(e.size, s_buffer.Length * 2)];
                continue;
            }
            ReadOnlySpan<byte> data = s_buffer.AsSpan(0, e.size);
            switch (e.type)
            {
            case 1:
                OnTransportConnected(e.connection);
                break;
            case 2:
                OnTransportDisconnected(e.connection, Encoding.UTF8.GetString(data));
                break;
            case 3:
                if (s_byWire.TryGetValue(e.connection, out Connection? from))
                    OnMessage(from, e.channel, data);
                break;
            case 4:
                if (s_byWire.TryGetValue(e.connection, out Connection? sender))
                    OnDatagram(sender, data);
                break;
            }
            if (Transport == 0)
                return;  // (a refusal ended it)
        }
        Reconcile();
        RollbackFrame();  // (a rollback the inputs asked for; the hashes due)
        ServerConsole();
    }

    // A dedicated server's console: status, kick <id> [why], say <text>, quit; else the game's.
    private static void ServerConsole()
    {
        if (Api.Native->serverCommand == null)
            return;
        Span<byte> buffer = stackalloc byte[1024];
        while (true)
        {
            int n;
            fixed (byte* p = buffer)
                n = Api.Native->serverCommand(Api.Ctx, p, buffer.Length);
            if (n <= 0)
                return;
            string line = Encoding.UTF8.GetString(buffer[..n]).Trim();
            int space = line.IndexOf(' ');
            string verb = space < 0 ? line : line[..space], rest = space < 0 ? "" : line[(space + 1)..].Trim();
            switch (verb)
            {
            case "":
                break;
            case "status":
                Log.Info($"server: {Role}, tick {Time.Tick}, {s_welcomed.Count} of {(MaxConnections > 0 ? MaxConnections.ToString() : "any")} players, {Count} objects");
                foreach (Connection c in s_welcomed)
                    Log.Info($"server:   {c.Id} {c.Name}: ping {c.Ping:0} ms, loss {c.Loss:0.0}%, sent {c.BytesSent / 1024} KB, received {c.BytesReceived / 1024} KB");
                break;
            case "stats":
                LogServerStats();
                break;
            case "kick":
            {
                int id = int.TryParse(rest.Split(' ')[0], out int k) ? k : -1;
                Connection? who = s_welcomed.Find(c => c.Id == id);
                if (who == null)
                {
                    Log.Warning($"server: no player {rest}");
                    break;
                }
                Kick(who, rest.Contains(' ') ? rest[(rest.IndexOf(' ') + 1)..] : "kicked");
                Log.Info($"server: kicked {who}");
                break;
            }
            case "say":
                s_out.Clear();
                s_out.Write(MsgSay);
                s_out.Write(rest);
                foreach (Connection c in s_welcomed)
                    Send(c, 0, s_out.Written);
                Log.Info($"server: said \"{rest}\"");
                break;
            case "quit":
                Log.Info("server: quitting");
                Game.Quit();
                break;
            default:
                if (Command != null)
                    Command.Invoke(line);
                else
                    Log.Warning($"server: no command {verb} (status, stats, kick <id>, say <text>, quit)");
                break;
            }
        }
    }

    private static float[] s_focus = new float[3 * 64];

    // Where the simulation needs the world (terrain and its collision) loaded: every networked
    // character (a CharacterMover on a NetworkObject). A server has no camera to stream around.
    private static void ReportFocus()
    {
        int n = 0;
        foreach (NetworkObject o in Sorted)
        {
            if (o.Dormant || o.Entity.GetComponent<CharacterMover>() == null)
                continue;
            if (3 * (n + 1) > s_focus.Length)
                Array.Resize(ref s_focus, s_focus.Length * 2);
            Vector3 p = o.Entity.Position;
            s_focus[3 * n] = p.X;
            s_focus[3 * n + 1] = p.Y;
            s_focus[3 * n + 2] = p.Z;
            ++n;
        }
        if (n == 0 && s_focusCount == 0)
            return;
        s_focusCount = n;
        fixed (float* p = s_focus)
            Api.Native->simFocus(Api.Ctx, p, n);
    }
    private static int s_focusCount;

    // After the frame's ticks: where the world must be loaded, the host's snapshots, a client's
    // poses and reports.
    internal static void AfterTicks()
    {
        ReportFocus();
        if (IsHost)
        {
            ulong interval = (ulong)Math.Max(1, 60 / Math.Clamp(SendRate, 1, 60));
            if (Mode != NetworkMode.Rollback && (s_lastSend == ulong.MaxValue || Time.Tick >= s_lastSend + interval))
            {
                s_lastSend = Time.Tick;
                Replicate();
            }
        }
        else if (IsClient)
        {
            Interpolate();
            if (s_verify && HostConnection != null && ++s_verifyFrame % 30 == 0)
                SendVerify();
        }
    }

    // ---- connections

    private static void OnTransportConnected(uint wire)
    {
        if (IsHost)
        {
            s_byWire[wire] = new Connection { Id = s_nextConnection++, Wire = wire };
            return;
        }
        // A client: say who it is.
        s_out.Clear();
        s_out.Write(MsgHello);
        s_out.Write(Protocol);
        s_out.Write(SchemaHash());
        s_out.Write(Steam.IsRunning ? Steam.Me.Name : Environment.MachineName);
        s_out.Write(Password);
        s_out.Write(false);  // (not a bot)
        WriteSteamHello(s_out);
        Send(s_byWire[wire], 0, s_out.Written);
    }

    private static void OnTransportDisconnected(uint wire, string why)
    {
        if (!s_byWire.Remove(wire, out Connection? c))
            return;
        s_clientInputs.Remove(c.Id);
        Log.Info($"network: {c} gone: {why}");
        if (c.Welcomed)
        {
            s_welcomed.Remove(c);
            s_connById.Remove(c.Id);
            EndSteamAuth(c);
            Tell(c, connected: false);
        }
        if (IsClient)
        {
            Shutdown();  // (the host went: the session is over)
            if (s_child)
            {
                Log.Info("network: the host went: this client process ends");
                Game.Quit();
            }
        }
    }

    private static void Tell(Connection c, bool connected)
    {
        try
        {
            (connected ? Connected : Disconnected)?.Invoke(c);
        }
        catch (Exception e)
        {
            Log.Error($"Network.{(connected ? "Connected" : "Disconnected")} failed: {e}");
        }
        foreach (INetworkListener l in Listeners())
            try
            {
                if (connected)
                    l.OnConnected(c);
                else
                    l.OnDisconnected(c);
            }
            catch (Exception e)
            {
                Log.Error($"{l.GetType().Name}.On{(connected ? "Connected" : "Disconnected")} failed: {e}");
            }
    }

    private static IEnumerable<INetworkListener> Listeners()
    {
        if (Game.Current is INetworkListener g)
            yield return g;
        foreach (GameSystem s in Systems.All)
            if (s is INetworkListener l)
                yield return l;
        var components = new List<Component>();
        ScriptComponents.All(components);
        foreach (Component c in components)
            if (c is INetworkListener l)
                yield return l;
    }

    // What both sides must agree on: the protocol, the game's component classes.
    private static ulong SchemaHash()
    {
        var names = new List<string>(ComponentTypes.ByName.Keys);
        names.Sort(StringComparer.Ordinal);
        return ObjectState.Hash(Encoding.UTF8.GetBytes(string.Join(";", names)));
    }

    /// <summary>The host: drops a player, telling them why.</summary>
    public static void Kick(Connection c, string why)
    {
        byte[] text = Encoding.UTF8.GetBytes(why + "\0");
        fixed (byte* p = text)
            Api.Native->netDisconnect(Api.Ctx, Transport, c.Wire, p);
    }

    private static void Send(Connection c, int channel, ReadOnlySpan<byte> data, bool disconnect = false)
    {
        if (disconnect)
        {
            byte[] why = Encoding.UTF8.GetBytes("left\0");
            fixed (byte* p = why)
                Api.Native->netDisconnect(Api.Ctx, Transport, c.Wire, p);
            return;
        }
        fixed (byte* p = data)
            Api.Native->netSend(Api.Ctx, Transport, c.Wire, channel, p, data.Length);
    }

    private static void OnMessage(Connection from, int channel, ReadOnlySpan<byte> data)
    {
        var r = new SyncReader(data);
        byte kind = r.Byte();
        switch (kind)
        {
        case MsgHello when IsHost:
        {
            uint protocol = r.UInt();
            ulong schema = r.ULong();
            from.Name = r.String();
            string password = r.Ok ? r.String() : "";
            from.IsBot = r.Ok && r.Position < data.Length && r.Bool();
            string? refusal = protocol != Protocol ? $"protocol {protocol}, the host's is {Protocol}"
                            : schema != SchemaHash() ? "the game's component classes differ from the host's (another build?)"
                            : Password.Length > 0 && password != Password ? "wrong password"
                            : MaxConnections > 0 && s_welcomed.Count >= MaxConnections ? $"the server is full ({MaxConnections} players)"
                            : Mode == NetworkMode.Rollback && RollbackStarted ? "the game has started (rollback mode has no late join)"
                            : from.IsBot ? null
                                                     : CheckSteamHello(from, ref r);
            s_out.Clear();
            if (refusal != null)
            {
                s_out.Write(MsgRefuse);
                s_out.Write(refusal);
                Send(from, 0, s_out.Written);
                Log.Warning($"network: refused {from.Name}: {refusal}");
                return;
            }
            s_out.Write(MsgWelcome);
            s_out.Write(from.Id);
            s_out.Write(Time.Tick);
            Send(from, 0, s_out.Written);
            from.Welcomed = true;
            s_welcomed.Add(from);
            s_connById[from.Id] = from;
            if (!from.IsBot || ++s_botsJoined % 100 == 1)
                Log.Info($"network: {from} joined{(from.IsBot ? $" (a bot; {s_botsJoined} so far)" : "")}");
            if (Mode == NetworkMode.Rollback)
                RollbackJoined();  // (everyone's objects are made when all are in)
            else
                Tell(from, connected: true);
            break;
        }
        case MsgWelcome when IsClient:
        {
            Local = new Connection { Id = r.Int(), Name = Environment.MachineName };
            s_wasWelcomed = true;
            s_hostTick = r.ULong();
            s_hostTickAt = Now;
            from.Welcomed = true;
            s_welcomed.Add(from);
            Log.Info($"network: joined as connection {Local.Id}");
            if (Mode != NetworkMode.Rollback)
                Tell(from, connected: true);
            break;
        }
        case MsgRbStart when IsClient:
        {
            var players = new List<(int, string)>();
            int count = r.Int();
            for (int i = 0; i < count && r.Ok; ++i)
                players.Add((r.Int(), r.String()));
            RollbackBegin(players);
            break;
        }
        case MsgRbHash when IsHost:
            ReceiveRollbackHash(from, ref r);
            break;
        case MsgRefuse when IsClient:
            s_refused = true;
            Log.Error($"network: the host refused this game: {r.String()}");
            Shutdown();
            break;
        case MsgVerify when IsHost:
            Verify(from, ref r);
            break;
        case MsgSay when IsClient:
        {
            string text = r.String();
            Log.Info($"host: {text}");
            Message?.Invoke(text);
            break;
        }
        case MsgSpawn when IsClient:
            ReceiveSpawn(ref r);
            break;
        case MsgDespawn when IsClient:
        {
            ulong id = r.ULong();
            s_proxies.Remove(id);
            if (Find(id) is NetworkObject o)
                DestroyNow(o);
            break;
        }
        case MsgState when IsClient:
            ReceiveState(r.ULong(), r.ULong(), r.Bytes(r.Int()));
            break;
        case MsgBatch when IsClient:
        {
            int count = r.Int();
            for (int i = 0; i < count && r.Ok; ++i)
                OnMessage(from, channel, r.Bytes(r.Int()));
            break;
        }
        case Rpc.Message:
            Rpc.Receive(from, ref r, data);
            break;
        }
    }

    // An RPC to a connection: reliably on channel 2, or as a datagram (when it fits; else reliably).
    internal static void SendRpc(Connection c, ReadOnlySpan<byte> message, bool reliable, ulong netId)
    {
        if (!c.Welcomed || (IsHost && !c.Spawned.Contains(netId)))
            return;  // (its object isn't there yet: the call would find nothing)
        if (!reliable && message.Length <= 1100)
        {
            fixed (byte* p = message)
                if (Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, message.Length) != 0)
                    return;
        }
        Send(c, 2, message);
    }

    private static void OnDatagram(Connection from, ReadOnlySpan<byte> data)
    {
        var r = new SyncReader(data);
        byte kind = r.Byte();
        if (kind == DgSnapshot && IsClient)
        {
            ulong tick = r.ULong();
            ushort part = (ushort)r.Int();
            if (tick > s_hostTick)
            {
                s_hostTick = tick;
                s_hostTickAt = Now;
            }
            ++s_snapshots;
            while (r.Ok && r.Position < data.Length)
            {
                ulong id = r.ULong();
                ulong change = r.ULong();
                ReceiveState(id, change, r.Bytes(r.Int()));
            }
            s_out.Clear();
            s_out.Write(DgAck);
            s_out.Write(tick);
            s_out.Write((int)part);
            fixed (byte* p = s_out.Written)
                Api.Native->netSend(Api.Ctx, Transport, from.Wire, -1, p, s_out.Length);
        }
        else if (kind == Rpc.Message)
        {
            Rpc.Receive(from, ref r, data);
        }
        else if (kind == DgInput && IsHost)
        {
            ReceiveInputs(from, ref r);
        }
        else if (kind == DgVoice)
        {
            ReceiveVoice(from, ref r, data);
        }
        else if (kind == DgRbInput && Mode == NetworkMode.Rollback)
        {
            ReceiveRollbackInputs(from, ref r, data);
        }
        else if (kind == DgPing || kind == DgPong)
        {
            OnPing(from, ref r, pong: kind == DgPong);
        }
        else if (kind == DgAck && IsHost)
        {
            ulong key = (r.ULong() << 16) | (uint)r.Int();
            if (from.InFlight.Remove(key, out List<(ulong id, ulong change)>? sent))
                foreach ((ulong id, ulong change) in sent)
                    if (!from.Acked.TryGetValue(id, out ulong had) || change > had)
                        from.Acked[id] = change;
        }
    }

    // ---- the host's replication
    //
    // Each send: every object's state encoded once (for all clients), then for each client (on the
    // thread pool when there are many) which objects are relevant (near its own characters), the
    // spawns and despawns that makes, and the changed objects it hasn't acknowledged, highest
    // priority first, within its byte budget; then everything is sent, in order, on this thread.

    /// <summary>The host: bits a second each client may be sent: spawns (nearest first, up to
    /// three quarters of it) then snapshots. Objects that don't fit wait: unspawned ones for the
    /// next send, changed ones gaining priority. (Everyone's objects, a client's own, despawns
    /// and RPCs go regardless.)</summary>
    public static int ClientBandwidth { get; set; } = 512_000;

    /// <summary>The host's last sends: how long encoding, the clients' packets and sending took (ms),
    /// what went out (bytes a second, all clients), and how many objects each client had on average.</summary>
    public static (double encodeMs, double clientsMs, double sendMs, double bytesPerSecond, double relevant) ServerStats { get; private set; }

    private static readonly System.Diagnostics.Stopwatch s_clock = System.Diagnostics.Stopwatch.StartNew();
    private static long s_sentBytes, s_sentSince = s_clock.ElapsedTicks;
    private static long s_spawnBytes, s_snapshotBytes, s_otherBytes;
    private static (double spawns, double snapshots, double other) s_split;
    private static double s_lastServerLog;

    private static void Replicate()
    {
        ulong tick = Time.Tick;
        long t0 = s_clock.ElapsedTicks;
        // Each object's state now, once for every client.
        s_live.Clear();
        foreach (NetworkObject o in Sorted)
        {
            if (o.Dormant || o.NetId >= LocalIds || !Ecs.IsAlive(o.Entity))
                continue;
            s_live.Add(o.NetId);
            if (!s_records.TryGetValue(o.NetId, out Record? rec))
                s_records[o.NetId] = rec = new Record();
            rec.Object = o;
            rec.Radius = o.RelevanceRadius;
            rec.Position = o.Entity.Position;
            if (rec.Recipe == null)
                BuildRecipe(o, rec);
            s_state.Clear();
            ObjectState.Write(o, s_state);
            ulong hash = ObjectState.Hash(s_state.Written);
            if (rec.Bytes.Length > 0 && rec.Hash == hash)
                continue;
            rec.Bytes = s_state.Written.ToArray();
            rec.Hash = hash;
            rec.Change = tick;
            rec.History[tick] = hash;
            rec.HistoryOrder.Enqueue(tick);
            while (rec.HistoryOrder.Count > 600)
                rec.History.Remove(rec.HistoryOrder.Dequeue());
        }
        s_gone.Clear();
        foreach (ulong id in s_records.Keys)
            if (!s_live.Contains(id))
                s_gone.Add(id);
        foreach (ulong id in s_gone)
            s_records.Remove(id);
        // Where each client's own characters are (what it is shown is near them).
        foreach (List<ulong> ids in s_ownedBy.Values)
            ids.Clear();
        foreach (NetworkObject o in Sorted)
            if (o.Owner > 0 && s_records.ContainsKey(o.NetId))
            {
                if (!s_ownedBy.TryGetValue(o.Owner, out List<ulong>? ids))
                    s_ownedBy[o.Owner] = ids = new List<ulong>();
                ids.Add(o.NetId);
            }
        foreach (Connection c in s_welcomed)
        {
            c.Focus.Clear();
            FxNetStats st = c.Stats;
            c.MaxDatagram = st.maxDatagram >= 64 ? (int)st.maxDatagram : 1200;
        }
        foreach (NetworkObject o in Sorted)
            if (o.Owner > 0 && s_records.TryGetValue(o.NetId, out Record? rec) && ConnectionOf(o.Owner) is Connection owner)
                owner.Focus.Add(rec.Position);
        BuildGrid();
        long t1 = s_clock.ElapsedTicks;

        int budget = Math.Max(ClientBandwidth / 8 / Math.Clamp(SendRate, 1, 60), 1200);
        if (s_welcomed.Count >= 8)
            System.Threading.Tasks.Parallel.For(0, s_welcomed.Count, i => Prepare(s_welcomed[i], tick, budget));
        else
            foreach (Connection c in s_welcomed)
                Prepare(c, tick, budget);
        long t2 = s_clock.ElapsedTicks;

        long relevant = 0;
        foreach (Connection c in s_welcomed)
        {
            relevant += c.Spawned.Count;
            s_spawnBytes += c.SpawnBytes;
            s_snapshotBytes += c.SnapshotBytes;
            s_otherBytes += c.OtherBytes;
            c.SpawnBytes = c.SnapshotBytes = c.OtherBytes = 0;
            foreach ((int channel, byte[] data) in c.Outgoing)
            {
                s_sentBytes += data.Length;
                if (channel < 0)
                {
                    fixed (byte* p = data)
                        Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, data.Length);
                }
                else
                {
                    Send(c, channel, data);
                }
            }
            c.Outgoing.Clear();
        }
        long t3 = s_clock.ElapsedTicks;
        double ms = 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        double seconds = (t3 - s_sentSince) / (double)System.Diagnostics.Stopwatch.Frequency;
        ServerStats = ((t1 - t0) * ms, (t2 - t1) * ms, (t3 - t2) * ms,
                       seconds > 0.5 ? s_sentBytes / seconds : ServerStats.bytesPerSecond,
                       s_welcomed.Count > 0 ? relevant / (double)s_welcomed.Count : 0);
        if (seconds > 0.5)
        {
            s_split = (s_spawnBytes / seconds, s_snapshotBytes / seconds, s_otherBytes / seconds);
            s_sentBytes = s_spawnBytes = s_snapshotBytes = s_otherBytes = 0;
            s_sentSince = t3;
        }
        if (Now - s_lastServerLog > 5.0 && s_welcomed.Count > 0)
        {
            s_lastServerLog = Now;
            LogServerStats();
        }
    }

    private static void LogServerStats()
    {
        var s = ServerStats;
        int players = s_welcomed.Count;
        Log.Info($"server: {players} players, {Count} objects; a send: encode {s.encodeMs:0.00} ms, clients {s.clientsMs:0.00} ms, " +
                 $"send {s.sendMs:0.00} ms; out {s.bytesPerSecond / 1024:0} KB/s ({(players > 0 ? s.bytesPerSecond * 8 / 1000 / players : 0):0.0} kbit/s a player: " +
                 $"spawns {s_split.spawns / 1024:0}, snapshots {s_split.snapshots / 1024:0}, other {s_split.other / 1024:0} KB/s), " +
                 $"{s.relevant:0} objects a player; a tick's game code {s_simMs:0.00} ms");
    }

    // A tick's game code (OnFixedUpdate, Simulate: the host's, for the server's stats), averaged.
    private static double s_simMs;
    internal static void TickTime(double simMs) => s_simMs = s_simMs * 0.95 + simMs * 0.05;

    private static readonly HashSet<ulong> s_live = new();
    private static readonly Dictionary<int, List<ulong>> s_ownedBy = new();
    private static readonly List<ulong> s_noIds = new();
    private static readonly List<ulong> s_gone = new();

    // Interest: the objects with a relevance radius by grid cell (each a radius-sized square on X
    // and Z), the ones without one (everyone's), and the largest radius. A client looks at the
    // cells around its characters only.
    private const float CellSize = 32f;
    private static readonly Dictionary<(int, int), List<ulong>> s_cells = new();
    private static readonly List<List<ulong>> s_cellLists = new();
    private static readonly List<ulong> s_everyones = new();
    private static float s_maxRadius;
    private static readonly List<(int dx, int dz)> s_rings = new();  // the cells within reach, nearest first
    private static int s_ringsReach = -1;

    private static (int, int) CellOf(Vector3 p) => ((int)MathF.Floor(p.X / CellSize), (int)MathF.Floor(p.Z / CellSize));

    private static void BuildGrid()
    {
        foreach (List<ulong> list in s_cells.Values)
        {
            list.Clear();
            s_cellLists.Add(list);
        }
        s_cells.Clear();
        s_everyones.Clear();
        s_maxRadius = 0f;
        foreach (NetworkObject o in Sorted)
        {
            if (!s_records.TryGetValue(o.NetId, out Record? rec))
                continue;
            if (rec.Radius <= 0f)
            {
                s_everyones.Add(o.NetId);
                continue;
            }
            s_maxRadius = MathF.Max(s_maxRadius, rec.Radius);
            (int, int) cell = CellOf(rec.Position);
            if (!s_cells.TryGetValue(cell, out List<ulong>? ids))
            {
                ids = s_cellLists.Count > 0 ? s_cellLists[^1] : new List<ulong>();
                if (s_cellLists.Count > 0)
                    s_cellLists.RemoveAt(s_cellLists.Count - 1);
                s_cells[cell] = ids;
            }
            ids.Add(o.NetId);
        }
        int reach = (int)MathF.Ceiling(s_maxRadius / CellSize);
        if (reach != s_ringsReach)
        {
            s_ringsReach = reach;
            s_rings.Clear();
            for (int z = -reach; z <= reach; ++z)
                for (int x = -reach; x <= reach; ++x)
                    s_rings.Add((x, z));
            s_rings.Sort((a, b) =>
            {
                int d = (a.dx * a.dx + a.dz * a.dz).CompareTo(b.dx * b.dx + b.dz * b.dz);
                return d != 0 ? d : a.dz != b.dz ? a.dz.CompareTo(b.dz) : a.dx.CompareTo(b.dx);
            });
        }
    }

    private static bool Near(List<Vector3> focus, Vector3 p, float radius)
    {
        float r2 = radius * radius;
        foreach (Vector3 f in focus)
            if (Vector3.DistanceSquared(f, p) <= r2)
                return true;
        return false;
    }

    // One client's messages for this send (thread-safe: only its own state and the shared records).
    private static void Prepare(Connection c, ulong tick, int budget)
    {
        SyncWriter w = c.Writer;
        c.Batch.Clear();
        c.Batch.Write(MsgBatch);
        c.Batch.Write(0);  // (the count, patched)
        c.BatchCount = 0;
        // What it has that is gone, or no longer near its characters: despawned there.
        c.Gone.Clear();
        foreach (ulong id in c.Spawned)
            if (!s_records.TryGetValue(id, out Record? rec) ||
                (rec.Radius > 0f && rec.Object?.Owner != c.Id && c.Focus.Count > 0 && !Near(c.Focus, rec.Position, rec.Radius * 1.2f)))
                c.Gone.Add(id);
        foreach (ulong id in c.Gone)
            Despawn(c, id);
        // What it doesn't have and should: everyone's objects and its own (always), then those in
        // the cells around its characters within their radius, nearest cells first, while three
        // quarters of its budget lasts (the rest come at the next sends). Without characters:
        // everything, as the budget allows.
        c.SpawnSpent = 0;
        int spawnBudget = budget * 3 / 4;
        foreach (ulong id in s_everyones)
            Consider(c, id, w, int.MaxValue);
        foreach (ulong id in s_ownedBy.TryGetValue(c.Id, out List<ulong>? own) ? own : s_noIds)
            Consider(c, id, w, int.MaxValue);
        if (c.Focus.Count == 0)
        {
            foreach (List<ulong> ids in s_cells.Values)
                foreach (ulong id in ids)
                    if (!Consider(c, id, w, spawnBudget))
                        goto spawned;
        }
        else
        {
            c.Visited.Clear();
            foreach ((int dx, int dz) in s_rings)
                foreach (Vector3 f in c.Focus)
                {
                    (int cx, int cz) = CellOf(f);
                    if (c.Visited.Add((cx + dx, cz + dz)) && s_cells.TryGetValue((cx + dx, cz + dz), out List<ulong>? ids))
                        foreach (ulong id in ids)
                            if (!Consider(c, id, w, spawnBudget))
                                goto spawned;
                }
        }
    spawned:
        if (c.BatchCount > 0)
        {
            c.Batch.Patch(1, c.BatchCount);
            c.Outgoing.Add((1, c.Batch.Written.ToArray()));
        }
        // Changed objects it hasn't acknowledged: by priority (its own first, then the longest
        // waiting), as many as its budget takes.
        if (c.InFlight.Count > 256)  // (unacknowledged for seconds: lost; what they carried goes again)
            c.InFlight.Clear();
        c.Candidates.Clear();
        foreach (ulong id in c.Spawned)
        {
            Record rec = s_records[id];
            if (c.Acked.TryGetValue(id, out ulong acked) && acked >= rec.Change)
                continue;
            float priority = (c.Priority.TryGetValue(id, out float p) ? p : 0f) + (rec.Object?.Owner == c.Id ? 10f : 1f);
            c.Priority[id] = priority;
            c.Candidates.Add((priority, id));
        }
        c.Candidates.Sort((a, b) => b.priority.CompareTo(a.priority));
        int part = 0, spent = c.SpawnSpent;
        List<(ulong id, ulong change)> carried = new();
        void Begin()
        {
            w.Clear();
            w.Write(DgSnapshot);
            w.Write(tick);
            w.Write(part);
            carried = new List<(ulong id, ulong change)>();
        }
        void Flush()
        {
            if (carried.Count == 0)
                return;
            c.Outgoing.Add((-1, w.Written.ToArray()));
            spent += w.Length;
            c.SnapshotBytes += w.Length;
            c.InFlight[(tick << 16) | (uint)part] = carried;
            carried = new List<(ulong id, ulong change)>();  // (sent: a second Flush sends nothing)
            ++part;
        }
        Begin();
        foreach ((float _, ulong id) in c.Candidates)
        {
            Record rec = s_records[id];
            int size = 8 + 8 + 4 + rec.Bytes.Length;
            if (13 + size > c.MaxDatagram)
            {
                // Too big for a datagram: reliably, as acknowledged.
                var big = new SyncWriter();
                big.Write(MsgState);
                big.Write(id);
                big.Write(rec.Change);
                big.Write(rec.Bytes.Length);
                big.Write(rec.Bytes);
                c.Outgoing.Add((1, big.Written.ToArray()));
                c.OtherBytes += big.Length;
                c.Acked[id] = rec.Change;
                c.Priority[id] = 0f;
                continue;
            }
            if (w.Length + size > c.MaxDatagram)
            {
                Flush();
                if (spent + c.MaxDatagram > budget)
                    break;  // (another datagram would go over: the rest wait, their priority growing)
                Begin();
            }
            w.Write(id);
            w.Write(rec.Change);
            w.Write(rec.Bytes.Length);
            w.Write(rec.Bytes);
            carried.Add((id, rec.Change));
            c.Priority[id] = 0f;
        }
        Flush();
    }

    // An object a client doesn't have: spawned there when it is relevant to it. False: this
    // send's spawns have used their part of the budget.
    private static bool Consider(Connection c, ulong id, SyncWriter w, int limit)
    {
        if (c.SpawnSpent >= limit)
            return false;
        if (c.Spawned.Contains(id) || !s_records.TryGetValue(id, out Record? rec) || rec.Object is not NetworkObject o)
            return true;
        if (rec.Radius > 0f && o.Owner != c.Id && c.Focus.Count > 0 && !Near(c.Focus, rec.Position, rec.Radius))
            return true;
        WriteSpawn(w, c, o, rec);
        c.SpawnBytes += w.Length;
        c.SpawnSpent += w.Length + 4;
        AddToBatch(c, w.Written);
        c.Spawned.Add(id);
        c.Acked[id] = rec.Change;
        return true;
    }

    private static void AddToBatch(Connection c, ReadOnlySpan<byte> message)
    {
        c.Batch.Write(message.Length);
        c.Batch.Write(message);
        ++c.BatchCount;
    }

    private static void Despawn(Connection c, ulong id)
    {
        c.Spawned.Remove(id);
        c.Acked.Remove(id);
        c.Priority.Remove(id);
        SyncWriter w = c.Writer;
        w.Clear();
        w.Write(MsgDespawn);
        w.Write(id);
        c.OtherBytes += w.Length;
        AddToBatch(c, w.Written);
    }

    // An object's recipe: its prefab, or its components by class with their [Property] values.
    // The same for every client and every object built alike (all of a game's projectiles): sent
    // to a client once, then named by its hash.
    private static void BuildRecipe(NetworkObject o, Record rec)
    {
        var w = new SyncWriter();
        w.Write(o.Prefab ?? "");
        List<Component>? components = o.Prefab == null ? ScriptComponents.Of(o.Entity) : null;
        int count = 0;
        if (components != null)
            foreach (Component comp in components)
                if (!comp.Destroyed && comp.Type != null)
                    ++count;
        w.Write(count);
        if (components != null)
            foreach (Component comp in components)
            {
                if (comp.Destroyed || comp.Type == null)
                    continue;
                w.Write(comp.Type.Name);
                w.Write(ScriptComponents.PropertiesJson(comp));
            }
        rec.Recipe = w.Written.ToArray();
        rec.RecipeHash = ObjectState.Hash(rec.Recipe);
    }

    private static void WriteSpawn(SyncWriter w, Connection c, NetworkObject o, Record rec)
    {
        w.Clear();
        w.Write(MsgSpawn);
        w.Write(o.NetId);
        w.Write(rec.Change);
        w.Write(o.Owner);
        w.Write(o.Entity.Name);
        w.Write(rec.RecipeHash);
        bool known = c.Recipes.Contains(rec.RecipeHash);
        w.Write(!known);
        if (!known)
        {
            w.Write(rec.Recipe!.Length);
            w.Write(rec.Recipe);
            c.Recipes.Add(rec.RecipeHash);
        }
        w.Write(rec.Bytes.Length);
        w.Write(rec.Bytes);
    }

    // ---- a client's side

    private static void ReceiveSpawn(ref SyncReader r)
    {
        ulong id = r.ULong();
        ulong change = r.ULong();
        int owner = r.Int();
        string name = r.String();
        ulong recipeHash = r.ULong();
        if (r.Bool())
        {
            var rr = new SyncReader(r.Bytes(r.Int()));
            string from = rr.String();
            int count = rr.Int();
            var list = new List<(string type, string properties)>(count);
            for (int i = 0; i < count; ++i)
                list.Add((rr.String(), rr.String()));
            s_recipes[recipeHash] = (from, list);
        }
        ReadOnlySpan<byte> state = r.Bytes(r.Int());
        if (!s_recipes.TryGetValue(recipeHash, out (string prefab, List<(string type, string properties)> components) known))
        {
            Log.Warning($"network: object {id} names a recipe this client wasn't sent");
            return;
        }
        string prefab = known.prefab;
        List<(string type, string properties)> recipe = known.components;
        NetworkObject? o = Find(id);
        if (o == null)
        {
            // Made here as the host made it: its prefab, or its components with their properties.
            float* rows = stackalloc float[12];
            var sr = new SyncReader(state);
            byte kind = ObjectState.ReadTransform(ref sr, rows);
            WorldTransform w = default;
            for (int i = 0; i < 12; ++i)
                w.rows[i] = rows[i];
            Entity e;
            if (prefab.Length > 0)
            {
                e = Prefab.Spawn(prefab, w.Position, w.Rotation);
                s_pendingIds[e.Id] = id;
                ScriptComponents.Start();  // (its components now: its NetworkObject takes the NetId)
            }
            else
            {
                e = Ecs.Create();
                if (kind != 0)
                    e.World = w;
                s_pendingIds[e.Id] = id;
                ScriptComponents.AddRecipe(e, recipe);
            }
            if (name.Length > 0)
                e.Name = name;
            o = Find(id);
            if (o == null)
            {
                Log.Warning($"network: object {id} ({(prefab.Length > 0 ? prefab : name)}) has no NetworkObject here");
                return;
            }
        }
        o.Owner = owner;
        ReceiveState(id, change, state, snap: true);
    }

    private static void ReceiveState(ulong id, ulong change, ReadOnlySpan<byte> bytes, bool snap = false)
    {
        NetworkObject? o = Find(id);
        if (o == null)
            return;  // (its spawn hasn't come yet: the host sends its state again until acknowledged)
        if (!snap && OwnsLocally(o))
        {
            // Ours: checked against the prediction before the next frame (Reconcile).
            if (!s_hostStates.TryGetValue(id, out (ulong change, byte[] bytes) had) || change > had.change)
                s_hostStates[id] = (change, bytes.ToArray());
            return;
        }
        if (!s_proxies.TryGetValue(id, out Proxy? proxy))
            s_proxies[id] = proxy = new Proxy();
        else if (change <= proxy.Change && !snap)
            return;  // (an older datagram, late)
        proxy.Change = change;
        proxy.Hash = ObjectState.Hash(bytes);
        var r = new SyncReader(bytes);
        float[] rows = new float[12];
        byte kind;
        fixed (float* p = rows)
            kind = ObjectState.ReadTransform(ref r, p);
        ObjectState.ReadComponents(o, ref r);
        if (kind == 0)
            return;
        if (snap)
        {
            proxy.Poses.Clear();
            fixed (float* p = rows)
                ObjectState.ApplyTransform(o.Entity, kind, p);
        }
        proxy.Poses.Add((change, rows, kind));
        if (proxy.Poses.Count > 32)
            proxy.Poses.RemoveAt(0);
    }

    // Proxies' transforms between the two poses around the shown tick (the newest one heard of,
    // the interpolation delay behind).
    private static void Interpolate()
    {
        double shown = s_hostTick + (Now - s_hostTickAt) * 60.0 - InterpolationDelay * 60.0;
        float* rows = stackalloc float[12];
        foreach ((ulong id, Proxy proxy) in s_proxies)
        {
            List<(double tick, float[] rows, byte kind)> poses = proxy.Poses;
            if (poses.Count == 0 || Find(id) is not NetworkObject o || !Ecs.IsAlive(o.Entity) || OwnsLocally(o))
                continue;
            int after = 0;
            while (after < poses.Count && poses[after].tick <= shown)
                ++after;
            if (after == 0 || after == poses.Count)
            {
                // Before the first pose or past the last: hold the nearest.
                (double _, float[] held, byte kind) = poses[after == 0 ? 0 : poses.Count - 1];
                fixed (float* p = held)
                    ObjectState.ApplyTransform(o.Entity, kind, p);
                continue;
            }
            (double t0, float[] a, byte k0) = poses[after - 1];
            (double t1, float[] b, byte _) = poses[after];
            float f = (float)((shown - t0) / Math.Max(t1 - t0, 1e-6));
            Blend(a, b, f, rows);
            ObjectState.ApplyTransform(o.Entity, k0, rows);
            if (after > 2)
                poses.RemoveRange(0, after - 2);
        }
    }

    // Two 3x4 transforms blended: positions lerped, rotations slerped, scales lerped.
    private static void Blend(float[] a, float[] b, float f, float* rows)
    {
        fixed (float* pa = a, pb = b)
        {
            Vector3 position = Vector3.Lerp(new Vector3(pa[3], pa[7], pa[11]), new Vector3(pb[3], pb[7], pb[11]), f);
            Quaternion rotation = Quaternion.Slerp(Affine.Rotation(pa), Affine.Rotation(pb), f);
            Vector3 scale = Vector3.Lerp(Affine.Scale(pa), Affine.Scale(pb), f);
            Affine.Set(rows, position, rotation, scale);
        }
    }

    // ---- verify (--net-verify): a client's replicated state checked by the host

    private static void SendVerify()
    {
        s_out.Clear();
        s_out.Write(MsgVerify);
        s_out.Write(s_proxies.Count);
        foreach ((ulong id, Proxy p) in s_proxies)
        {
            s_out.Write(id);
            s_out.Write(p.Change);
            s_out.Write(p.Hash);
        }
        Send(HostConnection!, 0, s_out.Written);
    }

    private static void Verify(Connection from, ref SyncReader r)
    {
        int count = r.Int();
        for (int i = 0; i < count && r.Ok; ++i)
        {
            ulong id = r.ULong(), change = r.ULong(), hash = r.ULong();
            if (!s_records.TryGetValue(id, out Record? rec) || !rec.History.TryGetValue(change, out ulong want))
                continue;  // (gone since, or a change older than the history)
            ++s_verifyChecks;
            if (want != hash)
            {
                if (s_verifyMismatches == 0)
                    Log.Error($"net-verify: {from}'s object {id} (as of tick {change}) differs from the host's");
                ++s_verifyMismatches;
            }
        }
    }

    /// <summary>(--net-verify) What went wrong: the host's mismatched checks (or none checked); a
    /// client never welcomed, refused, or without a snapshot.</summary>
    internal static long VerifyFailures => s_lastRole == NetworkRole.Host
        ? s_verifyMismatches + (s_verifyChecks == 0 ? 1 : 0)
        : (s_refused || !s_wasWelcomed ? 1 : 0) + (s_snapshots == 0 ? 1 : 0) + PredictionFailures();

    // A client that predicted: too few of its predictions checked, or more than 1 in 10 corrected.
    private static long PredictionFailures()
    {
        if (PredictedTicks == 0)
            return 0;
        if (PredictionChecks < 50 || Corrections * 10 > PredictionChecks)
        {
            Log.Error($"net-verify: predictions: {PredictionChecks} checked, {Corrections} corrected, lead raised up to {InputLeadMost:0.#} ticks");
            return 1;
        }
        Log.Info($"net-verify: predictions: {PredictionChecks} checked, {Corrections} corrected, lead raised up to {InputLeadMost:0.#} ticks");
        return 0;
    }
    internal static long VerifyChecks => s_lastRole == NetworkRole.Host ? s_verifyChecks : s_snapshots;
}
