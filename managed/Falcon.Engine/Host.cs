using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using Falcon.Native;

namespace Falcon;

// Engine-facing entry points. The engine calls Initialize (JIT: through hostfxr; NativeAOT:
// the game's exported falcon_script_init, see GameEntry/AotEntry.cs), then
// start/update/reload/stop through FxManagedApi.
// Only this assembly's [UnmanagedCallersOnly] methods are ever called by native code, so the
// game's code can be unloaded and replaced underneath them.
public static unsafe class Host
{
    private static GameLoadContext? s_context;
    private static Game? s_game;
    private static bool s_running;  // between start and stop (a game class or not: its components run)

    [UnmanagedCallersOnly]
    public static int Initialize(FxNativeApi* native, FxManagedApi* managed, byte* gamePath) =>
        InitializeCore(native, managed, gamePath);

    /// <summary>NativeAOT: the game is compiled into the same image and already registered.</summary>
    public static int InitializeLinked(FxNativeApi* native, FxManagedApi* managed) =>
        InitializeCore(native, managed, null);

    private static int InitializeCore(FxNativeApi* native, FxManagedApi* managed, byte* gamePath)
    {
        if (native == null || native->version != FxNativeApi.Version || native->size != (uint)sizeof(FxNativeApi))
            return 1;
        Api.Native = native;
        managed->start = &Start;
        managed->update = &Update;
        managed->reload = &Reload;
        managed->stop = &Stop;
        managed->particleStep = &ParticleStep;
        managed->describe = &Describe;
        managed->fixedUpdate = &FixedUpdate;
        managed->simControl = &SimControl;
        managed->componentValues = &ComponentValues;
        managed->editorUpdate = &EditorUpdate;
        managed->editorRun = &EditorRun;
        managed->editorEvent = &EditorEvent;
        managed->editorDescribe = &EditorDescribe;
        managed->sceneUnloading = &SceneUnloading;
        managed->sceneLoaded = &SceneLoaded;
        managed->phase = &Phase;
        managed->dataTypes = &DataTypes;
        managed->drawGizmos = &DrawGizmos;
        managed->componentButton = &ComponentButton;
        managed->netStatus = &NetStatus;
        try
        {
            // JIT: load the game into a collectible context. NativeAOT: the game is compiled
            // into this image and registered itself from its module initializer.
            if (gamePath != null && !LoadGame(Marshal.PtrToStringUTF8((nint)gamePath)!))
                return 2;
            Falcon.Level.MeshModifiers.Install();
            if (Game.Factory == null && ComponentTypes.ByName.Count == 0 && !Systems.HasAny && Falcon.Level.MeshModifiers.ByName.Count == 0)
            {
                Log.Error("no game registered: call Game.Register<T>() from a [ModuleInitializer], or make script components");
                return 3;
            }
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"initialization failed: {e}");
            return 4;
        }
    }

    [UnmanagedCallersOnly]
    private static int Start()
    {
        try
        {
            s_running = true;
            SteamServer.Start();  // (a dedicated server on Steam logs on before its session starts)
            Network.Launch();  // (the command line's session first: the components see their role)
            if (BotSwarm.Active)
                return 0;  // (a bot process: the game itself doesn't run)
            ScriptComponents.Start();  // the scene's components made (OnAwake) before the game starts
            Network.SceneStarted();
            s_game = Game.Current = Game.Factory?.Invoke();
            s_game?.OnStart();
            Systems.Start();
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"OnStart failed: {e}");
            s_game = null;
            return 1;
        }
    }

    [UnmanagedCallersOnly]
    private static void Update(float dt, double time, ulong frame)
    {
        Time.Delta = dt;
        Time.Total = time;
        Time.Frame = frame;
        if (!s_running)
            return;
        BotSwarm.Update();
        if (BotSwarm.Active)
            return;
        try
        {
            Steam.Frame();  // (Steam's callbacks and call results since the last frame)
        }
        catch (Exception e)
        {
            Log.Error($"Steam callbacks failed: {e}");
        }
        Network.FrameStart();  // (what came in: the host's objects, a client's input)
        Systems.Run(static (s, d) => s.OnEarlyUpdate(d), dt, "OnEarlyUpdate");
        try
        {
            s_game?.OnUpdate(dt);
        }
        catch (Exception e)
        {
            // A script bug must not take the engine down: log, and keep calling (hot reload fixes it).
            Log.Error($"OnUpdate failed: {e}");
        }
        Systems.Run(static (s, d) => s.OnUpdate(d), dt, "OnUpdate");
        ScriptComponents.Update(dt);  // (each component guarded on its own)
    }

    [UnmanagedCallersOnly]
    private static void Stop()
    {
        StopGame();
    }

    private static void StopGame()
    {
        Systems.Stop();
        try
        {
            s_game?.OnStop();
        }
        catch (Exception e)
        {
            Log.Error($"OnStop failed: {e}");
        }
        ScriptComponents.Stop();
        BotSwarm.Stop();
        Steam.Stop();
        Network.Shutdown();
        Network.Reset();
        s_game = Game.Current = null;
        s_running = false;
        Query.DestroyAll();
        Particles.ForgetControllers();
    }

    // Scene.Load: the game stays; the scene's components, queries' native handles, particle
    // controllers and component ids go with its world.
    [UnmanagedCallersOnly]
    private static void SceneUnloading(byte* scene)
    {
        try
        {
            s_game?.OnSceneUnloading(Marshal.PtrToStringUTF8((nint)scene) ?? "");
        }
        catch (Exception e)
        {
            Log.Error($"OnSceneUnloading failed: {e}");
        }
        ScriptComponents.Stop();
        Network.Reset();
        Particles.ForgetControllers();
        Query.WorldChanged();
        Ecs.WorldChanged();
    }

    // (X1) The frame's later phases: 3 after physics, 4 late, 5 before the render; and 6 after
    // each tick's physics step (its collision and trigger events, inside the tick).
    private static ulong s_resimulatedTick = ulong.MaxValue;  // (a rollback's tick simulated again with its physics step)

    [UnmanagedCallersOnly]
    private static void Phase(int phase, float dt)
    {
        if (!s_running)
            return;
        switch (phase)
        {
        case 3:
            ScriptComponents.AfterPhysics(dt);
            Systems.Run(static (s, d) => s.OnAfterPhysics(d), dt, "OnAfterPhysics");
            Network.AfterTicks();  // (the ticks' results sent; proxies placed)
            break;
        case 6:
            Time.BeginTick(Time.Tick);
            Sim.StartTick(Time.Tick, 1);
            try
            {
                ScriptComponents.StepEvents();
                SimHistory.Capture(Time.Tick, s_resimulatedTick == Time.Tick);  // (the tick's state, once its step and events are done)
                Network.AfterStep(Time.Tick);  // (predictions to check; the host's hitbox history)
            }
            finally
            {
                Time.EndTick();
            }
            break;
        case 4:
            Systems.Run(static (s, d) => s.OnLateUpdate(d), dt, "OnLateUpdate");
            ScriptComponents.LateUpdate(dt);
            break;
        case 5:
            Systems.Run(static (s, d) => s.OnPreRender(d), dt, "OnPreRender");
            break;
        }
    }

    [UnmanagedCallersOnly]
    private static int SceneLoaded(byte* scene)
    {
        ScriptComponents.Start();
        try
        {
            Systems.SceneLoaded(Marshal.PtrToStringUTF8((nint)scene) ?? "");
            s_game?.OnSceneLoaded(Marshal.PtrToStringUTF8((nint)scene) ?? "");
            return 0;
        }
        catch (Exception e)
        {
            Log.Error($"OnSceneLoaded failed: {e}");
            return 1;
        }
    }

    // A tick (the physics step's): the tick's input and time, OnFixedUpdate. flags 1: a tick
    // simulated again after a rollback: only the networked objects' components run (no systems, no
    // physics step), and the state after it is checked against the first time's.
    [UnmanagedCallersOnly]
    private static void FixedUpdate(float dt, ulong tick, int flags)
    {
        if (!s_running)
            return;
        bool again = (flags & 1) != 0;
        long started = System.Diagnostics.Stopwatch.GetTimestamp();
        Time.BeginTick(tick);
        Sim.StartTick(tick, 0);
        try
        {
            if (!again)
                Systems.Run(static (s, d) => s.OnFixedUpdate(d), dt, "OnFixedUpdate");
            ScriptComponents.FixedUpdate(dt, again);
            s_resimulatedTick = (flags & 2) != 0 ? tick : ulong.MaxValue;
            if (again && (flags & 2) == 0)  // (inside physics' step: kept once its step and events are done)
                SimHistory.Capture(tick, true);
            else
                Network.SendInputs();  // (a client's inputs of its last ticks)
            if (Network.IsHost)
                Network.TickTime((System.Diagnostics.Stopwatch.GetTimestamp() - started) * 1000.0 / System.Diagnostics.Stopwatch.Frequency);
        }
        finally
        {
            Time.EndTick();
        }
    }

    // The networked simulation's history: 0 restore the state after tick `arg` (1: done, 0: not
    // kept), 1 ticks simulated again that came out differently, 2 keep `arg` ticks (0: none), 3
    // ticks simulated again and checked; the session (--net-verify): 4 what went wrong, 5 checks.
    [UnmanagedCallersOnly]
    private static long SimControl(int op, ulong arg)
    {
        switch (op)
        {
        case 0:
            return SimHistory.Restore(arg) ? 1 : 0;
        case 1:
            return SimHistory.Mismatches;
        case 2:
            SimHistory.SetWindow((int)Math.Min(arg, int.MaxValue));
            return 0;
        case 3:
            return SimHistory.Checked;
        case 4:
            return Network.VerifyFailures;
        case 5:
            return Network.VerifyChecks;
        default:
            return 0;
        }
    }

    // UTF-8 into the engine's buffer: its length, -length when it doesn't fit.
    private static int Copy(string text, byte* buffer, int capacity)
    {
        int n = Encoding.UTF8.GetByteCount(text);
        if (buffer == null || capacity < n)
            return -n;
        Encoding.UTF8.GetBytes(text, new Span<byte>(buffer, capacity));
        return n;
    }

    // ---- editor code (Falcon.Editor): libraries' Editor/, Scripts/Editor/

    [UnmanagedCallersOnly]
    private static void EditorUpdate(float dt)
    {
        try
        {
            Falcon.Editor.EditorHost.Update(dt);
        }
        catch (Exception e)
        {
            Log.Error($"editor code failed: {e}");
        }
    }

    [UnmanagedCallersOnly]
    private static int EditorRun(byte* menu)
    {
        try
        {
            return Falcon.Editor.EditorHost.Run(Marshal.PtrToStringUTF8((nint)menu) ?? "") ? 1 : 0;
        }
        catch (Exception e)
        {
            Log.Error($"editor menu failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static void EditorEvent(byte* json, int length)
    {
        try
        {
            Falcon.Editor.EditorHost.Event(Encoding.UTF8.GetString(json, length));
        }
        catch (Exception e)
        {
            Log.Error($"editor event failed: {e}");
        }
    }

    [UnmanagedCallersOnly]
    private static int EditorDescribe(byte* buffer, int capacity)
    {
        try
        {
            return Copy(Falcon.Editor.EditorHost.Describe(), buffer, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"describing editor code failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static int NetStatus(byte* buffer, int capacity)
    {
        try
        {
            return s_running ? Copy(Network.Status(), buffer, capacity) : 0;
        }
        catch (Exception e)
        {
            Log.Error($"the network's status failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static int Describe(byte* buffer, int capacity)
    {
        try
        {
            return Copy(ScriptComponents.Describe(), buffer, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"describing the script components failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static void DrawGizmos(byte* scene, ulong selected)
    {
        try
        {
            ScriptComponents.DrawGizmos(scene, selected, s_running);
            Falcon.Editor.EditorHost.DrawGizmos();
        }
        catch (Exception e)
        {
            Log.Error($"gizmos failed: {e.Message}");
        }
    }

    [UnmanagedCallersOnly]
    private static int ComponentButton(ulong entity, byte* className, byte* method, byte* properties, byte* buffer, int capacity)
    {
        try
        {
            string? after = ScriptComponents.Button(entity, Marshal.PtrToStringUTF8((nint)className) ?? "", Marshal.PtrToStringUTF8((nint)method) ?? "",
                                                    Marshal.PtrToStringUTF8((nint)properties) ?? "", s_running);
            return after == null ? 0 : Copy(after, buffer, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"a component's button failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static int DataTypes(byte* buffer, int capacity)
    {
        try
        {
            return Copy(DataComponents.Describe(), buffer, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"describing the data components failed: {e}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static int ComponentValues(ulong entity, byte* buffer, int capacity)
    {
        try
        {
            string values = s_running ? ScriptComponents.Values(entity) : "";
            return values.Length == 0 ? 0 : Copy(values, buffer, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"a script component's values failed: {e.Message}");
            return 0;
        }
    }

    [UnmanagedCallersOnly]
    private static void ParticleStep(ulong script, byte* className, Particle* particles, int count, ParticleHit* hits, int hitCount, float dt)
    {
        if (!s_running)
            return;
        try
        {
            Particles.Step(script, className, particles, count, hits, hitCount, dt);
        }
        catch (Exception e)
        {
            Log.Error($"particle controller failed: {e}");
        }
    }

    // JIT hot reload. Everything that references game types must be dropped before the old
    // context can be collected: the game object, its registration and every query it made.
    // A running game starts again on the new code; one that wasn't (the editor, not playing)
    // only has its code replaced.
    [UnmanagedCallersOnly]
    private static int Reload(byte* gamePath)
    {
        bool wasRunning = s_running;
        // The components' [Property] values, made again on the new code (other fields start over).
        List<ScriptComponents.Saved> saved = wasRunning ? ScriptComponents.Snapshot() : new();
        StopGame();
        Falcon.Editor.EditorHost.Unload();  // (editor code: its tools told, its registrations dropped)
        Game.Factory = null;
        ComponentTypes.Forget();
        Rpc.Forget();
        Systems.Forget();
        DataComponents.Forget();
        Falcon.Level.MeshModifiers.Forget();
        ScriptComponents.ForgetEdit();
        Particles.ForgetTypes();  // (the game's: registered again by its new code)
        WeakReference unloaded = UnloadGame();
        for (int i = 0; i < 16 && unloaded.IsAlive; ++i)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        bool collected = !unloaded.IsAlive;
        try
        {
            if (!LoadGame(Marshal.PtrToStringUTF8((nint)gamePath)!) ||
                (Game.Factory == null && ComponentTypes.ByName.Count == 0 && Falcon.Level.MeshModifiers.ByName.Count == 0))
                return 1;
            Falcon.Level.MeshModifiers.Install();  // (again: the meshes with them made with the new code)
            if (wasRunning)
            {
                s_running = true;
                ScriptComponents.Restore(saved);
                s_game = Game.Factory?.Invoke();
                s_game?.OnStart();
                Systems.Start();
            }
        }
        catch (Exception e)
        {
            Log.Error($"reload failed: {e}");
            s_game = null;
            return 1;
        }
        return collected ? 0 : 2;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference UnloadGame()
    {
        var weak = new WeakReference(s_context);
        s_context?.Unload();
        s_context = null;
        return weak;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    [UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "JIT only: the NativeAOT build never loads assemblies")]
    private static bool LoadGame(string path)
    {
        // Loaded from bytes: the file stays unlocked, so the game can be rebuilt while running.
        var context = new GameLoadContext();
        Assembly assembly;
        using (var image = new MemoryStream(File.ReadAllBytes(path)))
        {
            string pdb = Path.ChangeExtension(path, ".pdb");
            if (File.Exists(pdb))
            {
                using var symbols = new MemoryStream(File.ReadAllBytes(pdb));
                assembly = context.LoadFromStream(image, symbols);
            }
            else
            {
                assembly = context.LoadFromStream(image);
            }
        }
        s_context = context;
        // Runs the game's [ModuleInitializer]s (compiled into the module constructor).
        RuntimeHelpers.RunModuleConstructor(assembly.ManifestModule.ModuleHandle);
        return true;
    }

    // Collectible; Falcon.Engine itself resolves from the default context (shared by all reloads).
    private sealed class GameLoadContext : AssemblyLoadContext
    {
        public GameLoadContext() : base("falcon-game", isCollectible: true) { }

        protected override Assembly? Load(AssemblyName name) => null;
    }
}

public static unsafe class Time
{
    private static float s_delta;
    private static double s_total;

    /// <summary>Game seconds since the last update (fixed 1/60 under --fixed-time), Scale applied.
    /// Inside a tick (OnFixedUpdate, the collision callbacks): the tick's length.</summary>
    public static float Delta
    {
        get => InTick ? TickDelta : s_delta;
        internal set => s_delta = value;
    }

    /// <summary>The simulation's tick length: physics steps, OnFixedUpdate (1/60 s).</summary>
    public const float TickDelta = 1f / 60f;

    /// <summary>The simulation's tick: ticks run since the game started (inside a tick, this
    /// one's). Ticks are what a replay or the network reproduces exactly: game logic that must
    /// agree everywhere runs in OnFixedUpdate and reads Tick, not frames or wall time.</summary>
    public static ulong Tick { get; private set; }

    /// <summary>Whether a tick is running now (OnFixedUpdate, collision callbacks): Delta, Total
    /// and Input's edges are the tick's then.</summary>
    public static bool InTick { get; private set; }

    internal static void BeginTick(ulong tick)
    {
        Tick = tick;
        InTick = true;
        ++TickRuns;
    }

    // Ticks begun (re-simulated ones too): what a tick's caches are keyed by.
    internal static ulong TickRuns;

    internal static void EndTick() => InTick = false;

    /// <summary>Real seconds since the last update (Scale not applied: menus while paused).</summary>
    public static float UnscaledDelta => Api.Native == null ? Delta : Api.Native->realDelta;

    /// <summary>The game's time scale: scripts' Delta, animation, physics and particles (0.5: slow
    /// motion, 0: paused; at most 100). The editor's Stop puts it back to 1.</summary>
    public static float Scale
    {
        get => Api.Native == null ? 1f : Api.Native->timeScale(Api.Ctx, -1f);
        set
        {
            if (Api.Native != null)
                Api.Native->timeScale(Api.Ctx, MathF.Max(value, 0f));
        }
    }

    /// <summary>Whether the game's time is stopped (Scale 0).</summary>
    public static bool Paused
    {
        get => Scale == 0f;
        set => Scale = value ? 0f : 1f;
    }
    /// <summary>Game seconds since the start (inside a tick: the tick's time, Tick x TickDelta).</summary>
    public static double Total
    {
        get => InTick ? Tick * (double)TickDelta : s_total;
        internal set => s_total = value;
    }
    public static ulong Frame { get; internal set; }
}

public static unsafe class Log
{
    public static void Info(string message) => Write(0, message);
    public static void Warning(string message) => Write(1, message);
    public static void Error(string message) => Write(2, message);

    private static void Write(int level, string message)
    {
        if (Api.Native == null)
            return;
        int max = Encoding.UTF8.GetMaxByteCount(message.Length) + 1;
        Span<byte> buffer = max <= 1024 ? stackalloc byte[max] : new byte[max];
        int n = Encoding.UTF8.GetBytes(message, buffer);
        buffer[n] = 0;
        fixed (byte* p = buffer)
            Api.Native->log(Api.Ctx, level, p);
    }
}
