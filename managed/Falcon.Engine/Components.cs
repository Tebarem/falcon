using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;

namespace Falcon;

// (API v20) Script components: C# classes attached to entities in the editor (s&box's / Unity's),
// their [Property] values saved with the scene and edited in the Inspector.
//
//   public sealed class Spinner : Component
//   {
//       [Property, Range(0, 720)] public float Speed = 90f;
//       public override void OnUpdate(float dt) => Entity.Rotation *= Quaternion.CreateFromAxisAngle(Vector3.UnitY, Speed * dt * MathF.PI / 180f);
//   }
//
// Falcon.Engine.Generators registers every Component subclass (no reflection: NativeAOT-safe).

/// <summary>A property of a script component: saved with the scene, edited in the Inspector, and
/// kept through a hot reload. Fields and properties of type float, double, int, uint, long, bool,
/// string, an enum, Vector2/3/4, Quaternion or Entity.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class PropertyAttribute : Attribute
{
    public string? Tooltip { get; set; }
    public string? Group { get; set; }
}

/// <summary>A number property's range (the Inspector's slider).</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class RangeAttribute(float min, float max) : Attribute
{
    public float Min { get; } = min;
    public float Max { get; } = max;
}

/// <summary>A string property naming a project asset of this kind (".fsound", ".fprefab", ".fmat").</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class AssetAttribute(string extension) : Attribute
{
    public string Extension { get; } = extension;
}

/// <summary>
/// A script component: attached to an entity (the Inspector's Add Component, or AddComponent), it
/// lives while the game runs. OnAwake when it is made (every component of the scene first), OnStart
/// before its first update, OnUpdate every frame and OnFixedUpdate every physics step while it and
/// its entity are enabled (OnEnable / OnDisable when that changes), OnDestroy when its entity goes or
/// the game stops. Keep its state in [Property] members to see it in the Inspector and keep it
/// through a hot reload (other fields start over).
/// </summary>
public abstract class Component
{
    /// <summary>The entity it is on (a field: `Entity.Position = ...` works on it).</summary>
    public readonly Entity Entity;

    [ThreadStatic] internal static Entity s_making;  // the entity the component being made is for

    /// <summary>Made by the engine (the scene's) or AddComponent: its Entity is set already.</summary>
    protected Component() => Entity = s_making;

    private bool _enabled = true;
    internal bool Started, Awake, Destroyed, WasActive;
    internal int Index;  // its place on the entity (the scene's list), -1: added in code
    internal ComponentType? Type;  // its class's description (null: one the generator doesn't know)

    /// <summary>Whether it runs (its entity being enabled too). The Inspector's checkbox.</summary>
    public bool Enabled
    {
        get => _enabled;
        set => _enabled = value;
    }

    public virtual void OnAwake() { }
    public virtual void OnStart() { }
    public virtual void OnEnable() { }
    public virtual void OnDisable() { }
    public virtual void OnUpdate(float dt) { }
    public virtual void OnFixedUpdate(float dt) { }
    public virtual void OnDestroy() { }

    /// <summary>(API v32) After this frame's physics (its ticks and their events), and once
    /// animation and physics are done.</summary>
    public virtual void OnAfterPhysics(float dt) { }
    public virtual void OnLateUpdate(float dt) { }

    /// <summary>(API v32) Its entity's body hit something (contact begins, as the physics sounds
    /// hear them): the other entity, where, which way, how hard. (v36) Called inside the tick whose
    /// physics step made it, right after the step.</summary>
    public virtual void OnCollisionEnter(Collision collision) { }

    /// <summary>(API v32) Something came into, or went out of, a trigger: on the trigger's
    /// components (`other`: what came) and on what came's (`other`: the trigger). (v36) Inside the
    /// tick whose step it was.</summary>
    public virtual void OnTriggerEnter(Entity other) { }
    public virtual void OnTriggerExit(Entity other) { }

    /// <summary>(API v34) Each editor frame, editing or playing: its gizmos (Gizmos.*). While
    /// editing it is one made from the scene's values (no OnAwake / OnStart: its fields as
    /// constructed and loaded). OnDrawGizmosSelected: while its entity is selected.</summary>
    public virtual void OnDrawGizmos() { }
    public virtual void OnDrawGizmosSelected() { }

    /// <summary>A component of type T on the same entity (null when none).</summary>
    public T? GetComponent<T>() where T : Component => ScriptComponents.Get<T>(Entity);

    /// <summary>A component of type T on another entity (null when none).</summary>
    public static T? GetComponent<T>(Entity entity) where T : Component => ScriptComponents.Get<T>(entity);
}

/// <summary>(API v34) A component method run by a button in its Inspector section (public or
/// internal, no parameters). While editing, it runs on one made from the scene's values and the
/// properties it changes are kept (one undo step); while playing, on the running one.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class ButtonAttribute(string? label = null) : Attribute
{
    public string? Label { get; } = label;
}

/// <summary>(API v34) A component's own Inspector: an HTML page (in the project's UI/ folder, or its
/// library's) drawn in its section, Height pixels tall. With falcon-panel.js the page hears the
/// component's values (FalconPanel.onMessage({type: "values", values})) and sets them
/// (FalconPanel.send({set: {name: value}}): one undo step).</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class InspectorPageAttribute(string page) : Attribute
{
    public string Page { get; } = page;
    public int Height { get; set; } = 120;
}

/// <summary>(API v34) A [Button] method: its name and label.</summary>
public sealed record ButtonInfo(string Name, string Label);

/// <summary>A component's property as the generator describes it.</summary>
public sealed record PropertyInfo(string Name, string Kind, float Min, float Max, string? Asset, string[]? EnumNames, string? Tooltip,
                                  string? Group);

/// <summary>A component class: how to make one, its properties, and its JSON read / write.</summary>
public sealed class ComponentType(string name, string label, Func<Component> create, PropertyInfo[] properties,
                                  Action<Component, Utf8JsonWriter> write, Action<Component, JsonElement> read)
{
    public string Name { get; } = name;
    public string Label { get; } = label;
    public Func<Component> Create { get; } = create;
    public PropertyInfo[] Properties { get; } = properties;
    public Action<Component, Utf8JsonWriter> Write { get; } = write;
    public Action<Component, JsonElement> Read { get; } = read;
    /// <summary>(API v34) Its [Button] methods, run by name; its [InspectorPage] (its library: "" for
    /// the project's).</summary>
    public ButtonInfo[] Buttons { get; init; } = Array.Empty<ButtonInfo>();
    public Action<Component, string>? Invoke { get; init; }
    public string? Page { get; init; }
    public int PageHeight { get; init; }
    public string Library { get; init; } = "";
    /// <summary>(API v37) Its [Sync] members written and read back (the networked simulation's
    /// state; null: it has none).</summary>
    public SyncWriteFn? SyncWrite { get; init; }
    public SyncReadFn? SyncRead { get; init; }
}

/// <summary>A component's [Sync] members written (generated).</summary>
public delegate void SyncWriteFn(Component component, SyncWriter writer);

/// <summary>A component's [Sync] members read back (generated).</summary>
public delegate void SyncReadFn(Component component, ref SyncReader reader);

/// <summary>The component classes of the game (registered by the generated code).</summary>
public static class ComponentTypes
{
    internal static readonly Dictionary<string, ComponentType> ByName = new();

    static ComponentTypes() => BuiltinComponents.Register();  // (the engine's own: NetworkObject, CharacterMover)

    public static void Register(ComponentType type) => ByName[type.Name] = type;

    public static ComponentType? Find(string name) => ByName.TryGetValue(name, out ComponentType? t) ? t : null;

    internal static void Forget()  // (hot reload: the new code registers again; the engine's stay)
    {
        ByName.Clear();
        BuiltinComponents.Register();
    }
}

/// <summary>JSON for component properties (the generated read / write).</summary>
public static unsafe class ComponentJson
{
    public static double Number(JsonElement v, double fallback) =>
        v.ValueKind == JsonValueKind.Number ? v.GetDouble() : fallback;

    private static float At(JsonElement v, int i, float fallback) =>
        v.ValueKind == JsonValueKind.Array && v.GetArrayLength() > i && v[i].ValueKind == JsonValueKind.Number ? v[i].GetSingle() : fallback;

    public static Vector2 Vector2(JsonElement v, Vector2 fallback) => new(At(v, 0, fallback.X), At(v, 1, fallback.Y));
    public static Vector3 Vector3(JsonElement v, Vector3 fallback) => new(At(v, 0, fallback.X), At(v, 1, fallback.Y), At(v, 2, fallback.Z));
    public static Vector4 Vector4(JsonElement v, Vector4 fallback) =>
        new(At(v, 0, fallback.X), At(v, 1, fallback.Y), At(v, 2, fallback.Z), At(v, 3, fallback.W));
    public static Quaternion Quaternion(JsonElement v, Quaternion fallback) =>
        new(At(v, 0, fallback.X), At(v, 1, fallback.Y), At(v, 2, fallback.Z), At(v, 3, fallback.W));

    /// <summary>An entity: its id (written while playing), or the scene's key for it (saved).</summary>
    public static Entity Entity(JsonElement v, Entity fallback)
    {
        if (v.ValueKind == JsonValueKind.Number)
            return new Entity(v.GetUInt64());
        if (v.ValueKind == JsonValueKind.Null)
            return default;
        if (v.ValueKind != JsonValueKind.String)
            return fallback;
        string key = v.GetString() ?? "";
        if (key.Length == 0)
            return default;
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(key, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return new Entity(Native.Api.Native->scriptEntityByKey(Native.Api.Ctx, p));
    }

    public static void Write(Utf8JsonWriter w, string name, Vector2 v)
    {
        w.WriteStartArray(name);
        w.WriteNumberValue(v.X);
        w.WriteNumberValue(v.Y);
        w.WriteEndArray();
    }

    public static void Write(Utf8JsonWriter w, string name, Vector3 v)
    {
        w.WriteStartArray(name);
        w.WriteNumberValue(v.X);
        w.WriteNumberValue(v.Y);
        w.WriteNumberValue(v.Z);
        w.WriteEndArray();
    }

    public static void Write(Utf8JsonWriter w, string name, Vector4 v)
    {
        w.WriteStartArray(name);
        w.WriteNumberValue(v.X);
        w.WriteNumberValue(v.Y);
        w.WriteNumberValue(v.Z);
        w.WriteNumberValue(v.W);
        w.WriteEndArray();
    }

    public static void Write(Utf8JsonWriter w, string name, Quaternion q) => Write(w, name, new Vector4(q.X, q.Y, q.Z, q.W));

    /// <summary>An entity as its id (the editor shows its name; saving keeps the scene's key).</summary>
    public static void Write(Utf8JsonWriter w, string name, Entity e) => w.WriteNumber(name, e.Id);
}
