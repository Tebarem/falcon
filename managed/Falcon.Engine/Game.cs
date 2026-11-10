using System;

namespace Falcon;

/// <summary>
/// Base class of a game. Keep gameplay state in ECS components, not in fields: the world
/// survives a hot reload, the game object does not (OnStart runs again on the new code).
/// </summary>
public abstract class Game
{
    internal static Func<Game>? Factory;

    /// <summary>The running game (null when none runs).</summary>
    public static Game? Current { get; internal set; }

    /// <summary>
    /// Registers the game type. Call it from a [ModuleInitializer] in the game assembly; it
    /// runs when the engine loads the game (JIT, also on every hot reload) or at startup (NativeAOT).
    /// </summary>
    public static void Register<T>() where T : Game, new() => Factory = static () => new T();

    /// <summary>Ends the game: a game's process quits; in the editor, Play stops.</summary>
    public static unsafe void Quit()
    {
        if (Native.Api.Native != null)
            Native.Api.Native->quit(Native.Api.Ctx);
    }

    public virtual void OnStart() { }
    public virtual void OnUpdate(float dt) { }
    public virtual void OnStop() { }

    /// <summary>The scene is going (Scene.Load): its world is still there, its components end
    /// after this. The game itself goes on into the next scene: keep what carries over in fields
    /// (or Storage). Entities, characters and Ref&lt;T&gt;s of this scene mean nothing in the next.</summary>
    public virtual void OnSceneUnloading(string scene) { }

    /// <summary>The next scene is in place (its components made): find its entities here.</summary>
    public virtual void OnSceneLoaded(string scene) { }
}
