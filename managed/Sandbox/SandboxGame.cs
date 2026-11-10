using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Falcon;
using Falcon.Steamworks;

namespace Sandbox;

// Script-defined components: registered with the engine on first use and stored in flecs,
// so they survive hot reloads (change their layout and the engine refuses them, loudly).

/// <summary>An instance that floats up and down around its rest position.</summary>
[Component("Sandbox.Bob")]
public struct Bob
{
    public Vector3 Rest;
    public float Phase;
}

/// <summary>A levitating prop's motion: a script component (API v20), added in code to each prop
/// tagged Bob; its [Property] values survive a hot reload.</summary>
public sealed class Bobber : Component
{
    [Property] public Vector3 Rest;
    [Property] public float Phase;

    public override void OnUpdate(float dt) =>
        Entity.Position = Rest + new Vector3(0f, 0.35f + 0.25f * MathF.Sin((float)Time.Total * 1.7f + Phase), 0f);
}

/// <summary>Singleton: the sphere fountain.</summary>
[Component("Sandbox.Fountain")]
public struct Fountain
{
    public Vector3 Origin;
    public float Timer;
    public uint Emitted;
    public uint Enabled;
}

/// <summary>A character the game drives: its handle (Character.Of, found once: no lookup per call)
/// and its graph's "Speed" parameter.</summary>
[Component("Sandbox.Walker")]
public struct Walker
{
    public Character Character;
    public int Speed;
}

/// <summary>A character's footsteps: how far into its stride it is, and the steps it took (from
/// its animation's footstep events once it has shown one: its model's .fevents).</summary>
[Component("Sandbox.Stride")]
public struct Stride
{
    public float Phase;
    public uint Steps;
    public uint FromEvents;
}

/// <summary>
/// Sample game: the prop-sized scene instances nearest the camera levitate, a fountain sprays
/// spheres (Space toggles it) and X kicks every body upwards. All state is in the ECS.
/// Characters walk and run, and their footsteps sound like what they walk on: the surface under
/// each step (Physics.SurfaceAt) picks Footsteps/&lt;Surface&gt;.fsound when the project has one
/// (Footsteps/Grass.fsound, say), else that surface's synthesized impact.
/// With the HUD (falcon --ui hud) the page shows live numbers and has the same controls.
/// A steady frame allocates nothing: lists are kept and reused, characters are found once.
/// </summary>
public sealed class SandboxGame : Game
{
    private const float Interval = 0.1f;
    private Query? _bobbing, _fountain, _bodies, _speeds, _walkers;
    private float _hudTimer;
    private uint _footsteps;
    private bool _eventFootsteps;
    // Reused every frame (cleared, never reallocated once grown).
    private readonly List<(Vector3 origin, uint index)> _spawns = new();
    private readonly List<Entity> _kicked = new();
    private readonly List<(Vector3 at, float speed, uint n)> _steps = new();
    private static readonly string?[] s_footstepPaths = new string?[32];

    [ModuleInitializer]
    internal static void Register() => Register<SandboxGame>();

    public override void OnStart()
    {
        _bobbing = new Query(Query.In<Bob>(), Query.In<WorldTransform>());
        _fountain = new Query(Query.InOut<Fountain>());
        _bodies = new Query(Query.In<RigidBody>());
        _speeds = new Query(Query.In<Walker>());
        _walkers = new Query(Query.InOut<Stride>(), Query.In<WorldTransform>(), Query.In<Walker>());

        // First start only (after a hot reload the components are already there).
        if (_bobbing.Count() == 0)
            TagLevitating();
        TagWalkers();
        if (_fountain.Count() == 0)
        {
            Entity f = Ecs.Create();
            Ecs.Set(f, new Fountain
            {
                Origin = Camera.Position + Camera.Forward * 6f - Camera.Up * 1.5f,
                Enabled = 1,
            });
        }
        Log.Info($"Sandbox started: {_bobbing.Count()} levitating instances, {_bodies.Count()} bodies");
        // (sim_dmath_*: the managed deterministic math gives the native side's bits, JIT and AOT)
        Log.Info($"Sandbox: DMath self-hash {DMath.SelfHash():x16}");
        if (Steam.IsRunning)
            _ = SteamHello();
        else if (Steam.Error.Length > 0)
            Log.Info($"Sandbox: Steam not running ({Steam.Error})");
    }

    // (steam_*: with --steam 480, the player, their friends, their avatar, and a call result: Spacewar's leaderboard)
    private static async System.Threading.Tasks.Task SteamHello()
    {
        try
        {
            SteamPlayer me = Steam.Me;
            Log.Info($"Sandbox: Steam signed in as {me.Name} ({me.Id}) for App ID {Steam.AppId}, {Steam.Friends.Count} friends, language {Steam.Language}");
            LeaderboardFindResult_t board = await SteamUserStats.FindLeaderboardAsync("Feet Traveled");
            Log.Info($"Sandbox: Steam leaderboard 'Feet Traveled' {(board.m_bLeaderboardFound != 0 ? "found" : "not found")} " +
                     $"({SteamUserStats.GetLeaderboardEntryCount(board.m_hSteamLeaderboard)} entries)");
            SteamImage? avatar = await me.AvatarAsync();
            Log.Info($"Sandbox: Steam avatar {(avatar == null ? "none" : $"{avatar.Width}x{avatar.Height}, PNG {avatar.ToPng().Length} bytes")}");
            // A lobby, found by its data, then left.
            SteamLobby? lobby = await SteamLobby.CreateAsync(ELobbyType.k_ELobbyTypePublic, 4);
            if (lobby == null)
                return;
            string tag = $"{Random.Shared.Next():x8}";
            lobby["falcon_test"] = tag;
            int found = 0;
            for (int attempt = 0; attempt < 5 && found == 0; ++attempt)
                found = (await SteamLobby.FindAsync([("falcon_test", tag)], slots: 0)).FindAll(l => l.Id == lobby.Id).Count;
            Log.Info($"Sandbox: Steam lobby created ({lobby.Members.Count} member, owner {(lobby.IsOwner ? "me" : "someone else")}), {(found > 0 ? "found" : "not found")} by its data");
            lobby.Leave();
            // Stats, achievements, a leaderboard's entries, Cloud: read only (the player's account isn't changed).
            var top = new List<SteamLeaderboardEntry>();
            if (await SteamLeaderboard.FindAsync("Feet Traveled") is SteamLeaderboard feet)
                for (int attempt = 0; attempt < 2 && top.Count == 0; ++attempt)  // (Steam's servers fail a download now and then)
                    try
                    {
                        top = await feet.EntriesAsync(ELeaderboardDataRequest.k_ELeaderboardDataRequestGlobal, 1, 5);
                    }
                    catch (SteamException) when (attempt == 0)
                    {
                    }
            List<string> achievements = SteamStats.Achievements;
            Log.Info($"Sandbox: Steam stats: {achievements.Count} achievements ({achievements.FindAll(SteamStats.IsUnlocked).Count} unlocked), " +
                     $"NumGames {SteamStats.GetInt("NumGames")}, top {top.Count} on Feet Traveled (first {(top.Count > 0 ? top[0].Score : 0)}), " +
                     $"cloud {(SteamCloud.IsEnabled ? $"on, {SteamCloud.Quota.available / 1024} KB free" : "off")}");
            // The Workshop, the inventory, Steam Input: read only.
            var items = new List<SteamWorkshopItem>();
            for (int attempt = 0; attempt < 2 && items.Count == 0; ++attempt)  // (Steam's servers fail a query now and then)
                try
                {
                    items = await SteamWorkshop.FindAsync(EUGCQuery.k_EUGCQuery_RankedByVote);
                }
                catch (SteamException) when (attempt == 0)
                {
                }
            List<SteamItem>? owned = await SteamItems.AllAsync();
            bool input = SteamControllers.Start();
            Log.Info($"Sandbox: Steam workshop: {items.Count} items found (first '{(items.Count > 0 ? items[0].Title : "")}'), " +
                     $"inventory {(owned == null ? $"refused ({SteamItems.LastResult})" : $"{owned.Count} items")}, Steam Input {(input ? "on" : "off")}, {SteamControllers.Count} controllers");
        }
        catch (Exception e)
        {
            Log.Error($"Sandbox: Steam failed: {e.Message}");
        }
    }

    // Props (0.15..1.2 m bounding radius) nearest the camera float up and down.
    private static void TagLevitating()
    {
        const int MaxProps = 12;
        using var meshes = new Query(Query.In<MeshInstance>(), Query.In<WorldTransform>(), Query.Not<RigidBody>());
        Vector3 eye = Camera.Position;
        var props = new List<(float distance, Entity entity, Vector3 rest)>();
        foreach (Chunk c in meshes)
        {
            ReadOnlySpan<MeshInstance> m = c.Read<MeshInstance>(0);
            ReadOnlySpan<WorldTransform> t = c.Read<WorldTransform>(1);
            for (int i = 0; i < c.Count; ++i)
            {
                var (center, radius) = Scene.MeshBounds(m[i].mesh);
                float r = radius * t[i].MaxScale;
                if (r >= 0.15f && r <= 1.2f)
                    props.Add((Vector3.Distance(t[i].TransformPoint(center), eye), c.Entities[i], t[i].Position));
            }
        }
        props.Sort((a, b) => a.distance != b.distance ? a.distance.CompareTo(b.distance) : a.entity.Id.CompareTo(b.entity.Id));
        // Structural changes after iterating (adding a component moves the entity to another table).
        for (int i = 0; i < Math.Min(MaxProps, props.Count); ++i)
        {
            Ecs.Set(props[i].entity, new Bob { Rest = props[i].rest, Phase = i * 0.9f });
            Bobber b = props[i].entity.AddComponent<Bobber>();
            b.Rest = props[i].rest;
            b.Phase = i * 0.9f;
        }
    }

    // Characters with a "Speed" parameter walk: found once (after a hot reload only new ones), so a
    // frame does no lookups by entity or by name.
    private static void TagWalkers()
    {
        var found = new List<(Entity entity, Walker walker)>();
        using (var meshes = new Query(Query.In<MeshInstance>(), Query.Not<RigidBody>(), Query.Not<Walker>()))
            foreach (Chunk c in meshes)
                foreach (Entity e in c.Entities)
                {
                    Character character = Character.Of(e);
                    int speed = character.IsValid ? Animation.Parameter(character, "Speed") : -1;
                    if (speed >= 0)
                        found.Add((e, new Walker { Character = character, Speed = speed }));
                }
        foreach (var (e, walker) in found)  // (adding a component moves the entity: after iterating)
        {
            Ecs.Set(e, walker);
            if (!Ecs.Has<Stride>(e))
                Ecs.Set(e, new Stride { Phase = (e.Id % 7) / 7f });
        }
    }

    public override void OnUpdate(float dt)
    {
        float t = (float)Time.Total;
        bool toggleFountain = Input.Pressed(Key.Space), kick = Input.Pressed(Key.X);
        // HUD buttons arrive as page messages.
        while (Ui.TryReceive(out string json))
        {
            using JsonDocument msg = JsonDocument.Parse(json);
            switch (msg.RootElement.TryGetProperty("type", out JsonElement type) ? type.GetString() : null)
            {
                case "hud.fountain": toggleFountain = true; break;
                case "hud.kick": kick = true; break;
                case "hud.ready": _hudTimer = 1f; break;
            }
        }
        // (The props bob by their Bobber components, after this.)

        // Characters cycle idle -> walk -> run, each with its own phase.
        foreach (Chunk c in _speeds!)
        {
            ReadOnlySpan<Walker> walker = c.Read<Walker>(0);
            ReadOnlySpan<Entity> entities = c.Entities;
            for (int i = 0; i < c.Count; ++i)
                Animation.SetFloat(walker[i].Character, walker[i].Speed, SpeedOf(entities[i], t));
        }
        Footsteps(dt, t);

        // Fountain state is read, spawned from and written back in place.
        List<(Vector3 origin, uint index)> spawns = _spawns;
        spawns.Clear();
        foreach (Chunk c in _fountain!)
        {
            Span<Fountain> f = c.Write<Fountain>(0);
            for (int i = 0; i < c.Count; ++i)
            {
                if (toggleFountain)
                    f[i].Enabled ^= 1;
                if (f[i].Enabled == 0)
                    continue;
                f[i].Timer += dt;
                for (; f[i].Timer >= Interval; f[i].Timer -= Interval)
                    spawns.Add((f[i].Origin, f[i].Emitted++));
            }
        }
        // Spawning creates entities: done outside the iteration.
        foreach (var (origin, n) in spawns)
        {
            float a = n * 2.39996f;  // golden angle: an even spray
            var velocity = new Vector3(MathF.Cos(a) * 1.5f, 7.5f, MathF.Sin(a) * 1.5f);
            Physics.Spawn(BodyShape.Sphere, origin, new Vector3(0.14f, 0f, 0f), velocity, Hue(n * 0.071f));
        }

        if (kick)
        {
            List<Entity> kicked = _kicked;
            kicked.Clear();
            foreach (Chunk c in _bodies!)
                kicked.AddRange(c.Entities);
            foreach (Entity e in kicked)
                Physics.Impulse(e, new Vector3(0f, 40f, 0f));
            Log.Info($"kicked {kicked.Count} bodies");
        }

        // Four HUD updates a second (each one makes the page repaint).
        _hudTimer += dt;
        if (_hudTimer >= 0.25f)
        {
            _hudTimer = 0f;
            bool fountainOn = false;
            foreach (Chunk c in _fountain!)
                fountainOn |= c.Count > 0 && c.Read<Fountain>(0)[0].Enabled != 0;
            Span<byte> json = stackalloc byte[256];  // (UTF-8 in place: nothing allocated)
            if (System.Text.Unicode.Utf8.TryWrite(json, $"{{\"type\":\"hud.stats\",\"bodies\":{_bodies!.Count()},\"levitating\":{_bobbing!.Count()},\"fountain\":{(fountainOn ? "true" : "false")},\"footsteps\":{_footsteps},\"fps\":{(dt > 0f ? 1f / dt : 0f):F0}}}", out int written))
                Ui.Send(json[..written]);
        }
    }

    // A character's speed now: idle (0) to a run (5), each on its own cycle.
    private static float SpeedOf(Entity e, float t) => 2.5f - 2.6f * MathF.Cos(t * 0.6f + (e.Id % 97) * 0.37f);

    // Each walking character's steps: faster, louder and a little higher as it speeds up, sounding
    // like the surface under it. A character whose clips have footstep events steps on them (the
    // strongest branch's: one step per update at most); the others keep a stride's rhythm.
    private void Footsteps(float dt, float t)
    {
        List<(Vector3 at, float speed, uint n)> steps = _steps;
        steps.Clear();
        Span<AnimEvent> events = stackalloc AnimEvent[32];
        foreach (Chunk c in _walkers!)
        {
            Span<Stride> stride = c.Write<Stride>(0);
            ReadOnlySpan<WorldTransform> xf = c.Read<WorldTransform>(1);
            ReadOnlySpan<Walker> walker = c.Read<Walker>(2);
            ReadOnlySpan<Entity> entities = c.Entities;
            for (int i = 0; i < c.Count; ++i)
            {
                float speed = SpeedOf(entities[i], t);
                int count = Math.Min(Animation.Events(walker[i].Character, events), events.Length);
                bool stepped = false, foot = false;
                for (int k = 0; k < count; ++k)
                    if (events[k].Kind == AnimEventKind.Footstep)
                    {
                        foot = true;
                        stepped |= events[k].Active && events[k].Weight >= 0.5f;
                    }
                if (foot && stride[i].FromEvents == 0 && !_eventFootsteps)
                {
                    _eventFootsteps = true;
                    Log.Info("footsteps from animation events");
                }
                if (foot)
                    stride[i].FromEvents = 1;
                if (stride[i].FromEvents != 0)
                {
                    if (stepped)
                        steps.Add((xf[i].Position, speed, stride[i].Steps++));
                    continue;
                }
                if (speed < 0.4f)
                    continue;
                stride[i].Phase += dt * (1.4f + 0.35f * speed);  // steps a second
                if (stride[i].Phase < 1f)
                    continue;
                stride[i].Phase -= 1f;
                steps.Add((xf[i].Position, speed, stride[i].Steps++));
            }
        }
        foreach (var (at, speed, n) in steps)
        {
            Surface surface = Physics.SurfaceAt(at + new Vector3(0f, 0.5f, 0f), out SurfaceHit hit, 1.5f) ? hit.Surface : Surface.Generic;
            if (surface == Surface.Unknown)
                surface = Surface.Generic;
            float volume = Math.Clamp(0.4f + 0.12f * speed, 0.4f, 1f);
            float pitch = 0.95f + 0.1f * ((n * 7919u) % 101u) / 100f;
            if (!Audio.Play(FootstepPath(surface), at, volume, pitch).IsValid)
                Audio.PlayImpact(surface, at, 0.35f * volume, 1.3f * pitch);
            if (++_footsteps % 100 == 1)
                Log.Info($"{_footsteps} footsteps (the last on {surface.ToString().ToLowerInvariant()})");
        }
    }

    // Footsteps/<Surface>.fsound, made once per surface.
    private static string FootstepPath(Surface surface)
    {
        uint i = (uint)surface;
        if (i >= s_footstepPaths.Length)
            return $"Footsteps/{surface}.fsound";
        return s_footstepPaths[i] ??= $"Footsteps/{surface}.fsound";
    }

    private static Vector3 Hue(float h)
    {
        h -= MathF.Floor(h);
        Vector3 c = new(MathF.Abs(h * 6f - 3f) - 1f, 2f - MathF.Abs(h * 6f - 2f), 2f - MathF.Abs(h * 6f - 4f));
        return Vector3.Clamp(c, Vector3.Zero, Vector3.One) * 0.85f + new Vector3(0.05f);
    }
}
