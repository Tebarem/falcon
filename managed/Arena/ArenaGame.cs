using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;
using Falcon.Level;

namespace Arena;

// The networked sample game: players run, jump and shoot in the scene. Everything runs in the
// simulation's ticks on networked objects (NetworkObject, CharacterMover, [Sync] state), so a
// rollback and a replay give the same ticks, and the host's simulation is what clients show.
// Player 0 is the host's keyboard (WASD, Space, the mouse turns it, left button shoots, right
// button fires a lag-compensated rail); each client that joins gets a player of its own,
// predicted on it (IPredicted); the others are bots whose choices come from Sim.Random.

public struct PlayerInput
{
    public Vector2 Move;
    public float Turn;
    public byte Jump, Fire, Rail;
    public byte Bot;  // (a bot: its choices are made in Simulate, alike everywhere)
}

public sealed class Player : Component, IPredicted<PlayerInput>
{
    [Sync] public int Index;
    [Sync] public int Health = 100;
    [Sync] public int Score;
    [Sync] public float Yaw;
    [Sync] public int Cooldown;
    [Sync] public int BotTurn;  // ticks left of the bot's current choice
    [Sync] public Vector2 BotMove;
    [Sync] public bool BotFire;

    public CharacterMover? Mover;

    /// <summary>Hits heard on this machine (Hit: an RPC the host broadcasts), and rail hits.</summary>
    public static int HitsHeard, RailHits;

    public bool IsBot => (Index > 0 && Index < 100) || Network.BuildingForBot;

    // Everywhere: where it was hit (effects would go here).
    [Rpc.Broadcast(Reliable = false)]
    public void Hit(Vector3 at) => ++HitsHeard;

    // BuildInput reads input only: it runs on the player's own machine alone, so it mustn't change
    // the simulation's state (rollback mode simulates everyone everywhere). A bot's choices are
    // made in Simulate, from Sim.Random: the same on every machine.
    public void BuildInput(ref PlayerInput input)
    {
        if (IsBot)
        {
            input.Bot = 1;
            return;
        }
        input.Move = new Vector2((Input.IsDown(Key.D) ? 1f : 0f) - (Input.IsDown(Key.A) ? 1f : 0f),
                                 (Input.IsDown(Key.W) ? 1f : 0f) - (Input.IsDown(Key.S) ? 1f : 0f));
        input.Turn = -Input.MouseDelta.X * 0.004f;
        input.Jump = Input.Pressed(Key.Space) ? (byte)1 : (byte)0;
        input.Fire = Input.IsDown(MouseButton.Left) ? (byte)1 : (byte)0;
        input.Rail = Input.Pressed(MouseButton.Right) ? (byte)1 : (byte)0;
    }

    private PlayerInput BotChoice()
    {
        var input = new PlayerInput();
        if (--BotTurn <= 0)
        {
            BotTurn = Sim.Random.Range(20, 90);
            BotMove = Sim.Random.InsideUnitCircle();
            BotFire = Sim.Random.Chance(0.4f);
            input.Turn = Sim.Random.Range(-1.2f, 1.2f);
        }
        input.Move = BotMove;
        input.Jump = Sim.Random.Chance(0.01f) ? (byte)1 : (byte)0;
        input.Fire = BotFire ? (byte)1 : (byte)0;
        return input;
    }

    public void Simulate(in PlayerInput given)
    {
        PlayerInput input = given.Bot != 0 ? BotChoice() : given;
        Mover ??= Entity.GetComponent<CharacterMover>();
        if (Mover == null)
            return;
        Yaw += input.Turn;
        DMath.SinCos(Yaw, out float s, out float c);
        var forward = new Vector3(-s, 0f, -c);
        var right = new Vector3(c, 0f, -s);
        Mover.Move((forward * input.Move.Y + right * input.Move.X) * 5f, input.Jump != 0);
        if (Cooldown > 0)
            --Cooldown;
        Vector3 eye = Entity.Position + new Vector3(0f, 1.4f, 0f);
        if (input.Fire != 0 && Cooldown == 0 && Health > 0)
        {
            Cooldown = 12;
            if (!Network.IsPredicting)  // (the host's simulation spawns it, or every machine's in rollback mode)
                Projectile.Fire(this, eye + forward * 0.6f, forward * 30f);
        }
        // The rail: an instant shot, tested where its shooter saw the others (lag compensation).
        if (input.Rail != 0 && Health > 0 && !Network.IsPredicting &&
            LagCompensation.Raycast(eye, forward, 100f, out LagHit hit, Entity) && hit.Object.Entity.GetComponent<Player>() is Player target)
        {
            ++RailHits;
            target.Health -= 50;
            target.Hit(hit.Point);
            if (target.Health <= 0)
                ++Score;
        }
        if (Entity.Position.Y < -20f || Health <= 0)
            ArenaGame.Respawn(this);
    }
}

public sealed class Projectile : Component
{
    [Sync] public Vector3 Velocity;
    [Sync] public int Life = 90;
    [Sync] public ulong Shooter;
    private static readonly List<NetworkObject> s_near = new();

    internal static void Fire(Player shooter, Vector3 from, Vector3 velocity)
    {
        Entity e = Ecs.Create();
        var w = new WorldTransform();
        w.Set(from, Quaternion.Identity, Vector3.One);
        e.World = w;
        e.AddComponent<NetworkObject>().RelevanceRadius = ArenaGame.Relevance;
        Projectile p = e.AddComponent<Projectile>();
        p.Velocity = velocity;
        p.Shooter = Network.Of(shooter.Entity)?.NetId ?? 0;
    }

    public override void OnFixedUpdate(float dt)
    {
        Vector3 at = Entity.Position;
        Vector3 step = Velocity * dt;
        float length = step.Length();
        Velocity += new Vector3(0f, -4f * dt, 0f);
        SurfaceHit wall = default;
        bool hitWall = length > 0f && Physics.Raycast(at, step / length, out wall, length);
        if (hitWall && wall.Entity.IsValid && Ecs.Has<PhysicsBody>(wall.Entity))
            Physics.AddImpulse(wall.Entity, Velocity * 2f, wall.Point);  // (a crate knocked)
        if (--Life <= 0 || hitWall)
        {
            Network.Destroy(Entity);
            return;
        }
        Vector3 next = at + step;
        // Players hit: one whose centre (a metre up) is within 0.6 m of where it got to, the first
        // by NetId.
        Network.Overlap(next - new Vector3(0f, 1f, 0f), 0.6f, s_near);
        foreach (NetworkObject o in s_near)
        {
            if (o.NetId == Shooter || o.Entity.GetComponent<Player>() is not Player target || target.Health <= 0)
                continue;
            target.Hit(next);
            target.Health -= 25;
            if (Network.Find(Shooter)?.Entity.GetComponent<Player>() is Player shooter && target.Health <= 0)
                ++shooter.Score;
            Network.Destroy(Entity);
            return;
        }
        Entity.Position = next;
    }
}

// Level cuts and late join (net_session_cuts, only with live mesh room: --live-meshes): every
// machine makes the same level wall, high above the scene; the host cuts it through a
// NetworkedCuts, twice before any client can have joined and once after. The host's geometry hash
// and count go to the clients here, so a client can say whether its level came out the same.
public sealed class LevelCheck : Component
{
    [Sync] public ulong HostHash;
}

public sealed class ArenaGame : Game, INetworkListener
{
    [ModuleInitializer]
    internal static void Register() => Register<ArenaGame>();

    private const int Players = 6;
    internal const float Relevance = 60f;  // (players and shots are sent to clients within this of their own)
    private readonly List<Player> _players = new();
    private bool _client;
    private static readonly Vector3 WallAt = new(0f, 40f, 0f);
    private bool _wall;
    private NetworkedCuts? _levelCuts;  // (the host's)
    private LevelCheck? _levelCheck;

    public override void OnStart()
    {
        Sim.Seed = 7;
        _client = Network.IsClient && Network.Mode != NetworkMode.Rollback;  // (rollback: every machine makes everything)
        _wall = Network.Mode != NetworkMode.Rollback && Array.IndexOf(Environment.GetCommandLineArgs(), "--live-meshes") >= 0 &&
                LevelMeshes.Create(PolyMesh.Box(new Vector3(4f, 3f, 0.2f)), WallAt).Id != 0;
        if (_wall && !_client)
        {
            Entity e = Ecs.Create();
            e.Name = "Level Cuts";
            e.AddComponent<NetworkObject>();
            _levelCuts = e.AddComponent<NetworkedCuts>();
            _levelCheck = e.AddComponent<LevelCheck>();
        }
        if (_client)
        {
            if (Steam.IsRunning)
                _ = ListLanServers();  // (net_steam_server: the dedicated server in Steam's LAN list)
            return;  // (the host's players come to clients through the network)
        }
        for (int i = 0; i < Players; ++i)
            _players.Add(MakePlayer(i, -1));
        if (!Network.IsActive || Network.Mode == NetworkMode.Rollback)
            MakeCrates();  // (a pile the shots knock about: every machine simulates it in rollback mode)
        Log.Info($"Arena started: {Players} players");
    }

    private static async System.Threading.Tasks.Task ListLanServers()
    {
        List<SteamServerInfo> servers = await SteamServer.FindLanAsync();
        Log.Info($"Arena: Steam LAN servers: {servers.Count}{(servers.Count > 0 ? " (" + string.Join(", ", servers) + ")" : "")}");
    }

    // A pile of dynamic crates in front of the players (the whole world's rollback: physics too).
    private static readonly List<(Entity crate, Vector3 start)> s_crates = new();

    private static void MakeCrates()
    {
        s_crates.Clear();
        for (int layer = 0; layer < 3; ++layer)
            for (int z = 0; z < 2; ++z)
                for (int x = 0; x < 4; ++x)
                {
                    Entity e = Ecs.Create();
                    e.Name = "Crate";
                    s_crates.Add((e, new Vector3(-1.5f + x + 0.1f * layer, 0.41f + 0.82f * layer, -5f - z)));
                    var w = new WorldTransform();
                    w.Set(new Vector3(-1.5f + x + 0.1f * layer, 0.41f + 0.82f * layer, -5f - z), Quaternion.Identity, Vector3.One);
                    e.World = w;
                    Ecs.Set(e, new PhysicsBody { mass = 20f, gravityScale = 1f });
                    var c = new Collider { shape = 0, friction = 0.6f };
                    c.Size = new Vector3(0.8f, 0.8f, 0.8f);
                    Ecs.Set(e, c);
                }
    }

    private static Player MakePlayer(int index, int owner)
    {
        Entity e = Ecs.Create();
        e.Name = $"Player {index}";
        NetworkObject net = e.AddComponent<NetworkObject>();
        net.Owner = owner;
        net.RelevanceRadius = Relevance;
        e.AddComponent<CharacterMover>();
        e.AddComponent<Hitbox>();
        Player p = e.AddComponent<Player>();
        p.Index = index;
        p.Yaw = index * 1.047f;
        Place(p);
        return p;
    }

    // The host: each client that joins plays a player of its own (it predicts it); it goes with it.
    public void OnConnected(Connection c)
    {
        if (Network.IsHost || Network.Mode == NetworkMode.Rollback)
            _players.Add(MakePlayer(100 + c.Id, c.Id));
        if (_levelCuts != null)
            Log.Info($"Arena: connection {c.Id} joined after {_levelCuts.Count} level cuts");
    }

    public void OnDisconnected(Connection c)
    {
        if (!Network.IsHost)
            return;
        foreach (Player p in _players)
            if (Network.Of(p.Entity) is NetworkObject o && o.Owner == c.Id && !o.Dormant)
                Network.Destroy(p.Entity);
    }

    private static void Place(Player p)
    {
        // Spread along the atrium, above its floor; they fall onto it.
        var w = new WorldTransform();
        Vector3 at;
        if (p.Index < 100)  // (the host's: along the atrium)
            at = new Vector3(-6f + 2.4f * p.Index, 1.5f, (p.Index & 1) == 0 ? -1f : 1f);
        else  // (each joining player: on a grid 8 m apart, 32 a row, from the atrium outward)
            at = new Vector3(-6f + 8f * ((p.Index - 100) % 32), 1.5f, 4f + 8f * ((p.Index - 100) / 32));
        w.Set(at, Quaternion.Identity, Vector3.One);
        p.Entity.World = w;
    }

    internal static void Respawn(Player p)
    {
        p.Health = 100;
        if (p.Mover != null)
            p.Mover.Velocity = Vector3.Zero;
        Place(p);
    }

    private string Status()
    {
        int shots = 0;
        foreach (NetworkObject o in Network.Objects)
            if (!o.Dormant && o.Entity.GetComponent<Projectile>() != null)
                ++shots;
        string scores = "";
        int listed = 0;
        foreach (NetworkObject o in Network.Objects)
            if (listed < 8 && o.Entity.GetComponent<Player>() is Player p)
            {
                scores += $" {p.Score}/{p.Health}";
                ++listed;
            }
        string inputs = Network.IsHost || Network.InputsUsed + Network.InputsMissed > 0
            ? $" inputs used {Network.InputsUsed} missed {Network.InputsMissed}"
            : "";
        string prediction = Network.PredictedTicks > 0
            ? $" predicted {Network.PredictedTicks} checked {Network.PredictionChecks} corrected {Network.Corrections} tick jumps {Network.TickJumps}"
            : "";
        return $"tick {Time.Tick} objects {Network.Count} shots {shots} hits heard {Player.HitsHeard} rail hits {Player.RailHits}{inputs}{prediction} scores (kills/health){scores}";
    }

    public override void OnUpdate(float dt)
    {
        if (Time.Frame % 15 == 0)  // (four times a second: a big server has thousands of objects to count)
            Hud.Show("Arena " + Status());
        if (_levelCuts != null && _levelCheck != null)
        {
            // A door and a hole before the clients are up, a window once they play.
            if (Time.Frame == 30)
                _levelCuts.Cut(CutOp.Box(WallAt + new Vector3(-1f, -0.5f, 0f), new Vector3(1f, 2.2f, 1f)));
            if (Time.Frame == 60)
                _levelCuts.Cut(CutOp.Damage(WallAt + new Vector3(1f, 0.6f, 0f), 0.9f, 7));
            if (Time.Frame == 1500)
                _levelCuts.Cut(CutOp.Box(WallAt + new Vector3(0.6f, -0.6f, 0f), new Vector3(0.6f, 0.6f, 1f)));
            if (!Cuts.Pending)
                _levelCheck.HostHash = Cuts.GeometryHash();
        }
    }

    // A client: its level cuts and whether its level came out as the host's.
    private void ReportLevel()
    {
        foreach (NetworkObject o in Network.Objects)
            if (o.Entity.GetComponent<LevelCheck>() is LevelCheck check && o.Entity.GetComponent<NetworkedCuts>() is NetworkedCuts cuts)
            {
                bool same = check.HostHash != 0 && !Cuts.Pending && Cuts.GeometryHash() == check.HostHash;
                Log.Info($"Arena level (client): cuts {cuts.Count} (caught up {cuts.CatchUps}), the host's geometry: {(same ? "same" : "different")}");
                return;
            }
        Log.Info("Arena level (client): no level cuts object");
    }

    public override void OnStop()
    {
        Log.Info($"Arena stopped ({(_client ? "client" : "host")}): " + Status());
        if (_wall && _client)
            ReportLevel();
        else if (_levelCuts != null)
            Log.Info($"Arena level (host): cuts {_levelCuts.Count}");
        if (s_crates.Count > 0)
        {
            int moved = 0;
            foreach ((Entity crate, Vector3 start) in s_crates)
                if (Ecs.IsAlive(crate) && Vector3.Distance(crate.Position, start) > 0.25f)
                    ++moved;
            Log.Info($"Arena: {moved} of {s_crates.Count} crates knocked about");
        }
    }
}
