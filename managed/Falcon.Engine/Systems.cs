using System;
using System.Collections.Generic;

namespace Falcon;

/// <summary>
/// (API v32) A system: the game's logic by the frame's phases, without an entity of its own.
/// Every non-abstract GameSystem with a public parameterless constructor is registered by the
/// source generator, made when the game starts (in <see cref="Order"/>, lower first), kept across
/// scene loads and hot-reloaded with the game's code (its fields start over). A frame runs:
/// <list type="number">
/// <item>OnEarlyUpdate, then the Game's OnUpdate, OnUpdate, then the components' OnUpdate;</item>
/// <item>OnFixedUpdate with the components' at each physics step (the simulation's tick), and after
/// the step the components' collision and trigger events (inside the tick);</item>
/// <item>after physics: OnAfterPhysics with the components';</item>
/// <item>OnLateUpdate with the components', once animation and physics are done;</item>
/// <item>OnPreRender, where the frame's transforms are final (just before they are drawn).</item>
/// </list>
/// </summary>
public abstract class GameSystem
{
    /// <summary>Its place among the systems (lower first).</summary>
    public virtual int Order => 0;

    public virtual void OnStart() { }
    public virtual void OnEarlyUpdate(float dt) { }
    public virtual void OnUpdate(float dt) { }
    public virtual void OnFixedUpdate(float dt) { }
    public virtual void OnAfterPhysics(float dt) { }
    public virtual void OnLateUpdate(float dt) { }
    public virtual void OnPreRender(float dt) { }
    public virtual void OnSceneLoaded(string scene) { }
    public virtual void OnStop() { }
}

/// <summary>The game's systems: registered (the source generator), made at the game's start.</summary>
public static class Systems
{
    private static readonly List<Func<GameSystem>> s_factories = new();
    private static readonly List<GameSystem> s_live = new();

    /// <summary>A system type (the source generator calls it from a module initializer).</summary>
    public static void Register(Func<GameSystem> make) => s_factories.Add(make);

    /// <summary>The running systems, in order.</summary>
    public static IReadOnlyList<GameSystem> All => s_live;

    public static T? Get<T>() where T : GameSystem
    {
        foreach (GameSystem s in s_live)
            if (s is T t)
                return t;
        return null;
    }

    internal static void Forget() => s_factories.Clear();  // (hot reload: the new code registers again)
    internal static bool HasAny => s_factories.Count > 0;

    internal static void Start()
    {
        s_live.Clear();
        foreach (Func<GameSystem> make in s_factories)
            try
            {
                s_live.Add(make());
            }
            catch (Exception e)
            {
                Log.Error($"a system could not be made: {e}");
            }
        s_live.Sort(static (a, b) => a.Order.CompareTo(b.Order));
        Run(static (s, _) => s.OnStart(), 0f, "OnStart");
    }

    internal static void Stop()
    {
        Run(static (s, _) => s.OnStop(), 0f, "OnStop");
        s_live.Clear();
    }

    internal static void SceneLoaded(string scene)
    {
        foreach (GameSystem s in s_live)
            try
            {
                s.OnSceneLoaded(scene);
            }
            catch (Exception e)
            {
                Log.Error($"{s.GetType().Name}.OnSceneLoaded failed: {e}");
            }
    }

    internal static void Run(Action<GameSystem, float> call, float dt, string what)
    {
        for (int i = 0; i < s_live.Count; ++i)
            try
            {
                call(s_live[i], dt);
            }
            catch (Exception e)
            {
                // A script bug must not take the engine down: logged, the others go on.
                Log.Error($"{s_live[i].GetType().Name}.{what} failed: {e}");
            }
    }
}

/// <summary>(API v32) A hit an entity's body took this frame (Component.OnCollisionEnter): what it
/// hit (invalid: the scene's own geometry or the ground), where, which way, how hard.</summary>
public readonly record struct Collision(Entity Other, System.Numerics.Vector3 Point, System.Numerics.Vector3 Normal, float Speed);
