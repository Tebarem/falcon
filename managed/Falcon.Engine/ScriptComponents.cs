using System;
using System.Buffers;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// The running game's script components (API v20): made from the scene's (the engine queues them
/// when the game starts, and a spawned prefab's since), and in code (AddComponent). Each frame,
/// after the game's OnUpdate: new ones are made (OnAwake), then every live one whose entity is
/// alive and enabled runs (OnEnable / OnDisable as that changes, OnStart before its first update,
/// OnUpdate); one whose entity went is destroyed (OnDestroy).
/// </summary>
public static unsafe class ScriptComponents
{
    private static readonly List<Component> s_all = new();
    private static readonly Dictionary<ulong, List<Component>> s_byEntity = new();
    private static byte[] s_buffer = new byte[16384];

    /// <summary>Adds a component of type T to an entity (not saved with the scene).</summary>
    public static T Add<T>(Entity entity) where T : Component, new()
    {
        Component.s_making = entity;
        var c = new T { Index = -1 };
        c.Type = ComponentTypes.Find(typeof(T).FullName?.Replace('+', '.') ?? "");
        Track(c);
        Awake(c);
        return c;
    }

    /// <summary>Removes a component (OnDestroy).</summary>
    public static void Remove(Component c)
    {
        if (c.Destroyed)
            return;
        Destroy(c);
    }

    /// <summary>The first component of type T on an entity, or null.</summary>
    public static T? Get<T>(Entity entity) where T : Component
    {
        if (s_byEntity.TryGetValue(entity.Id, out List<Component>? list))
            foreach (Component c in list)
                if (c is T t && !c.Destroyed)
                    return t;
        return null;
    }

    /// <summary>Every live component of type T (allocation-free over a reused list).</summary>
    public static void All<T>(List<T> into) where T : Component
    {
        into.Clear();
        foreach (Component c in s_all)
            if (c is T t && !c.Destroyed)
                into.Add(t);
    }

    internal static int Count => s_all.Count;

    // An entity's components (null: none), in the order they were made.
    internal static List<Component>? Of(Entity entity) => s_byEntity.TryGetValue(entity.Id, out List<Component>? list) ? list : null;

    // An entity's components ended now (Network.Destroy past the rollback window, a rollback undoing
    // a spawn): OnDestroy, and they no longer run; swept at the next update.
    internal static void RemoveAll(Entity entity)
    {
        if (s_byEntity.TryGetValue(entity.Id, out List<Component>? list))
            foreach (Component c in list)
                if (!c.Destroyed)
                    Destroy(c);
    }

    private static void Track(Component c)
    {
        s_all.Add(c);
        if (!s_byEntity.TryGetValue(c.Entity.Id, out List<Component>? list))
            s_byEntity[c.Entity.Id] = list = new List<Component>(2);
        list.Add(c);
    }

    private static void Awake(Component c)
    {
        if (c.Awake)
            return;
        c.Awake = true;
        Guard(c, static x => x.OnAwake(), "OnAwake");
    }

    private static void Destroy(Component c)
    {
        c.Destroyed = true;
        if (c.WasActive)
            Guard(c, static x => x.OnDisable(), "OnDisable");
        Guard(c, static x => x.OnDestroy(), "OnDestroy");
    }

    private static void Guard(Component c, Action<Component> call, string what)
    {
        try
        {
            call(c);
        }
        catch (Exception e)
        {
            Log.Error($"{c.GetType().Name}.{what} on entity {c.Entity.Id} failed: {e}");
        }
    }

    // The engine's queue: the scene's components (as the game starts), spawned prefabs' since.
    private static void Poll()
    {
        int n;
        fixed (byte* p = s_buffer)
            n = Api.Native->scriptPoll(Api.Ctx, p, s_buffer.Length);
        if (n < 0)
        {
            s_buffer = new byte[-n * 2];
            fixed (byte* p = s_buffer)
                n = Api.Native->scriptPoll(Api.Ctx, p, s_buffer.Length);
        }
        if (n <= 0)
            return;
        using JsonDocument doc = JsonDocument.Parse(s_buffer.AsMemory(0, n));
        var made = new List<Component>();
        var counts = new Dictionary<ulong, int>();
        foreach (JsonElement e in doc.RootElement.EnumerateArray())
        {
            ulong entity = e.GetProperty("entity").GetUInt64();
            string name = e.GetProperty("class").GetString() ?? "";
            counts[entity] = counts.TryGetValue(entity, out int k) ? k + 1 : 1;
            ComponentType? type = ComponentTypes.Find(name);
            if (type == null)
            {
                Log.Warning($"no script component {name} (renamed or removed?): entity {entity} goes without it");
                continue;
            }
            Component.s_making = new Entity(entity);
            Component c = type.Create();
            c.Type = type;
            c.Index = counts[entity] - 1;
            c.Enabled = !e.TryGetProperty("enabled", out JsonElement en) || en.ValueKind != JsonValueKind.False;
            if (e.TryGetProperty("properties", out JsonElement props) && props.ValueKind == JsonValueKind.Object)
                try
                {
                    type.Read(c, props);
                }
                catch (Exception ex)
                {
                    Log.Error($"{name}: its properties failed to load: {ex.Message}");
                }
            Track(c);
            made.Add(c);
        }
        foreach (Component c in made)  // (all made before any wakes: OnAwake finds the others)
            Awake(c);
    }

    internal static void Start() => Poll();

    internal static void Update(float dt)
    {
        Poll();
        bool removed = false;
        for (int i = 0; i < s_all.Count; ++i)
        {
            Component c = s_all[i];
            if (c.Destroyed)
            {
                removed = true;
                continue;
            }
            if (!Ecs.IsAlive(c.Entity))
            {
                Destroy(c);
                removed = true;
                continue;
            }
            bool active = c.Enabled && c.Entity.Enabled;
            if (active != c.WasActive)
            {
                c.WasActive = active;
                Guard(c, active ? static x => x.OnEnable() : static x => x.OnDisable(), active ? "OnEnable" : "OnDisable");
            }
            if (!active)
                continue;
            if (!c.Started)
            {
                c.Started = true;
                Guard(c, static x => x.OnStart(), "OnStart");
            }
            try
            {
                c.OnUpdate(dt);
            }
            catch (Exception e)
            {
                Log.Error($"{c.GetType().Name}.OnUpdate on entity {c.Entity.Id} failed: {e}");
            }
        }
        if (removed)
            Sweep();
    }

    // A tick: every active component's OnFixedUpdate (`networked`: only those on NetworkObjects,
    // a rollback's ticks simulated again). Self-contained, so a tick runs the same whatever frames
    // it falls in: components queued since (a prefab spawned) are made first, whether one runs is
    // its and its entity's enabled state now, and one that hasn't started starts here.
    internal static void FixedUpdate(float dt, bool networked = false)
    {
        Poll();
        for (int i = 0; i < s_all.Count; ++i)
        {
            Component c = s_all[i];
            if (c.Destroyed || !c.Enabled || !Ecs.IsAlive(c.Entity) || !c.Entity.Enabled)
                continue;
            NetworkObject? net = Network.IsActive || networked ? Network.Of(c.Entity) : null;
            if ((networked && net == null) || (net != null && net.IsProxy))
                continue;  // (a client's proxies: the host's simulation moves them)
            if (!c.Started)
            {
                c.Started = true;
                Guard(c, static x => x.OnStart(), "OnStart");
            }
            try
            {
                if (c is IPredictedRaw p)  // (input-driven: BuildInput / Simulate where they belong)
                    Network.RunPredicted(c, p, net ?? Network.Of(c.Entity), networked);
                c.OnFixedUpdate(dt);
            }
            catch (Exception e)
            {
                Log.Error($"{c.GetType().Name}.OnFixedUpdate on entity {c.Entity.Id} failed: {e}");
            }
        }
    }

    // After each tick's physics step (inside the tick): its collision and trigger events to the
    // components of the entities in them (Physics.Contacts / Triggers are the step's here).
    internal static void StepEvents()
    {
        foreach (Contact h in Physics.Contacts())
        {
            Deliver(h.A, new Collision(h.B, h.Point, h.Normal, h.Speed));
            Deliver(h.B, new Collision(h.A, h.Point, -h.Normal, h.Speed));
        }
        foreach (TriggerEvent t in Physics.Triggers())
        {
            Deliver(t.Trigger, t.Other, t.Entered);
            Deliver(t.Other, t.Trigger, t.Entered);
        }
    }

    // (X1) After physics: OnAfterPhysics (the frame's events were delivered by their ticks); and
    // OnLateUpdate.
    internal static void AfterPhysics(float dt)
    {
        for (int i = 0; i < s_all.Count; ++i)
        {
            Component c = s_all[i];
            if (c.Destroyed || !c.WasActive || !c.Started)
                continue;
            try
            {
                c.OnAfterPhysics(dt);
            }
            catch (Exception e)
            {
                Log.Error($"{c.GetType().Name}.OnAfterPhysics on entity {c.Entity.Id} failed: {e}");
            }
        }
    }

    internal static void LateUpdate(float dt)
    {
        for (int i = 0; i < s_all.Count; ++i)
        {
            Component c = s_all[i];
            if (c.Destroyed || !c.WasActive || !c.Started)
                continue;
            try
            {
                c.OnLateUpdate(dt);
            }
            catch (Exception e)
            {
                Log.Error($"{c.GetType().Name}.OnLateUpdate on entity {c.Entity.Id} failed: {e}");
            }
        }
    }

    private static void Deliver(Entity e, Collision hit)
    {
        if (!e.IsValid || !s_byEntity.TryGetValue(e.Id, out List<Component>? list))
            return;
        foreach (Component c in list.ToArray())
            if (!c.Destroyed && c.WasActive)
                try
                {
                    c.OnCollisionEnter(hit);
                }
                catch (Exception ex)
                {
                    Log.Error($"{c.GetType().Name}.OnCollisionEnter on entity {c.Entity.Id} failed: {ex}");
                }
    }

    private static void Deliver(Entity e, Entity other, bool entered)
    {
        if (!e.IsValid || !s_byEntity.TryGetValue(e.Id, out List<Component>? list))
            return;
        foreach (Component c in list.ToArray())
            if (!c.Destroyed && c.WasActive)
                try
                {
                    if (entered)
                        c.OnTriggerEnter(other);
                    else
                        c.OnTriggerExit(other);
                }
                catch (Exception ex)
                {
                    Log.Error($"{c.GetType().Name}.OnTrigger{(entered ? "Enter" : "Exit")} on entity {c.Entity.Id} failed: {ex}");
                }
    }

    private static void Sweep()
    {
        s_all.RemoveAll(static c => c.Destroyed);
        foreach (List<Component> list in s_byEntity.Values)
            list.RemoveAll(static c => c.Destroyed);
    }

    /// <summary>The game stops: every component destroyed.</summary>
    internal static void Stop()
    {
        foreach (Component c in s_all)
            if (!c.Destroyed)
                Destroy(c);
        s_all.Clear();
        s_byEntity.Clear();
    }

    // ---- hot reload: the scene's components made again on the new code with their [Property] values

    internal readonly record struct Saved(ulong Entity, int Index, string Class, bool Enabled, string Properties);

    internal static List<Saved> Snapshot()
    {
        var saved = new List<Saved>();
        foreach (Component c in s_all)
            if (!c.Destroyed && c.Type is ComponentType type)
                saved.Add(new Saved(c.Entity.Id, c.Index, type.Name, c.Enabled, Write(c, type)));
        return saved;
    }

    internal static void Restore(List<Saved> saved)
    {
        var made = new List<Component>();
        foreach (Saved s in saved)
        {
            if (!Ecs.IsAlive(new Entity(s.Entity)) || ComponentTypes.Find(s.Class) is not ComponentType type)
                continue;
            Component.s_making = new Entity(s.Entity);
            Component c = type.Create();
            c.Type = type;
            c.Index = s.Index;
            c.Enabled = s.Enabled;
            using (JsonDocument doc = JsonDocument.Parse(s.Properties))
                type.Read(c, doc.RootElement);
            Track(c);
            made.Add(c);
        }
        foreach (Component c in made)
            Awake(c);
    }

    // A component's [Property] values as JSON (what a spawn sends other machines).
    internal static string PropertiesJson(Component c) => c.Type != null ? Write(c, c.Type) : "{}";

    // An entity's components made from a recipe (a spawn from the host): each class by name with its
    // properties, all made before any wakes (as the scene's are).
    internal static void AddRecipe(Entity entity, List<(string type, string properties)> recipe)
    {
        var made = new List<Component>();
        var counts = 0;
        foreach ((string name, string properties) in recipe)
        {
            ComponentType? type = ComponentTypes.Find(name);
            if (type == null)
            {
                Log.Warning($"no script component {name} here (another build?): entity {entity.Id} goes without it");
                continue;
            }
            Component.s_making = entity;
            Component c = type.Create();
            c.Type = type;
            c.Index = -1 - counts++;
            try
            {
                using JsonDocument doc = JsonDocument.Parse(properties);
                type.Read(c, doc.RootElement);
            }
            catch (Exception ex)
            {
                Log.Error($"{name}: its properties failed to load: {ex.Message}");
            }
            Track(c);
            made.Add(c);
        }
        foreach (Component c in made)
            Awake(c);
    }

    private static string Write(Component c, ComponentType type)
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            type.Write(c, w);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }

    // ---- the editor: the classes (Add Component, the Inspector) and a running entity's values

    internal static string Describe(Dictionary<string, ComponentType>? types = null)  // (null: the components; else these, e.g. the C# modifiers)
    {
        types ??= ComponentTypes.ByName;
        var buffer = new ArrayBufferWriter<byte>(1024);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartArray();
            var names = new List<string>(types.Keys);
            names.Sort(StringComparer.Ordinal);
            foreach (string name in names)
            {
                ComponentType t = types[name];
                w.WriteStartObject();
                w.WriteString("name", t.Name);
                w.WriteString("label", t.Label);
                w.WriteStartArray("properties");
                foreach (PropertyInfo p in t.Properties)
                {
                    w.WriteStartObject();
                    w.WriteString("name", p.Name);
                    w.WriteString("kind", p.Kind);
                    if (!float.IsNaN(p.Min))
                        w.WriteNumber("min", p.Min);
                    if (!float.IsNaN(p.Max))
                        w.WriteNumber("max", p.Max);
                    if (p.Asset != null)
                        w.WriteString("asset", p.Asset);
                    if (p.Tooltip != null)
                        w.WriteString("tooltip", p.Tooltip);
                    if (p.Group != null)
                        w.WriteString("group", p.Group);
                    if (p.EnumNames != null)
                    {
                        w.WriteStartArray("enums");
                        foreach (string e in p.EnumNames)
                            w.WriteStringValue(e);
                        w.WriteEndArray();
                    }
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                // (API v34) Its buttons and its own Inspector page.
                w.WriteStartArray("buttons");
                foreach (ButtonInfo b in t.Buttons)
                {
                    w.WriteStartObject();
                    w.WriteString("name", b.Name);
                    w.WriteString("label", b.Label);
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                if (t.Page != null)
                {
                    w.WriteString("page", t.Page);
                    w.WriteNumber("pageHeight", t.PageHeight);
                    w.WriteString("library", t.Library);
                }
                // Its defaults: a new one's values.
                w.WriteStartObject("defaults");
                try
                {
                    t.Write(t.Create(), w);
                }
                catch (Exception e)
                {
                    Log.Warning($"{t.Name}: its defaults: {e.Message}");
                }
                w.WriteEndObject();
                w.WriteEndObject();
            }
            w.WriteEndArray();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }

    // ---- (API v34) gizmos and buttons, editing and playing

    // While editing: the scene's components, made from its values (no OnAwake / OnStart).
    private static readonly List<Component> s_edit = new();
    private static readonly HashSet<string> s_gizmoFailed = new();

    private static Component? Make(ulong entity, string name, JsonElement props)
    {
        ComponentType? type = ComponentTypes.Find(name);
        if (type == null)
            return null;
        Component.s_making = new Entity(entity);
        Component c = type.Create();
        c.Type = type;
        if (props.ValueKind == JsonValueKind.Object)
            type.Read(c, props);
        return c;
    }

    internal static void ForgetEdit()  // (hot reload: the old code's instances go)
    {
        s_edit.Clear();
        s_gizmoFailed.Clear();
    }

    internal static void DrawGizmos(byte* scene, ulong selected, bool running)
    {
        if (scene != null)
        {
            s_edit.Clear();
            string json = System.Runtime.InteropServices.Marshal.PtrToStringUTF8((nint)scene) ?? "";
            using JsonDocument doc = JsonDocument.Parse(json.Length == 0 ? "[]" : json);
            foreach (JsonElement e in doc.RootElement.EnumerateArray())
            {
                string name = e.GetProperty("class").GetString() ?? "";
                try
                {
                    Component? c = Make(e.GetProperty("entity").GetUInt64(), name, e.TryGetProperty("properties", out JsonElement p) ? p : default);
                    if (c != null)
                    {
                        c.Enabled = !e.TryGetProperty("enabled", out JsonElement en) || en.ValueKind != JsonValueKind.False;
                        s_edit.Add(c);
                    }
                }
                catch (Exception ex)
                {
                    if (s_gizmoFailed.Add(name + " make"))
                        Log.Error($"{name}: made for its gizmos, failed: {ex.Message}");
                }
            }
        }
        List<Component> list = running ? s_all : s_edit;
        for (int i = 0; i < list.Count; ++i)
        {
            Component c = list[i];
            if (c.Destroyed || !c.Enabled)
                continue;
            Gizmos.Color = System.Numerics.Vector4.One;
            try
            {
                c.OnDrawGizmos();
                if (c.Entity.Id == selected && selected != 0)
                    c.OnDrawGizmosSelected();
            }
            catch (Exception e)
            {
                if (s_gizmoFailed.Add(c.GetType().FullName ?? ""))  // (once: it would every frame)
                    Log.Error($"{c.GetType().Name}.OnDrawGizmos failed: {e}");
            }
        }
    }

    /// <summary>A [Button] method run: on the running component while playing (null back), else on
    /// one made from the scene's values, whose properties after come back as JSON.</summary>
    internal static string? Button(ulong entity, string name, string method, string properties, bool running)
    {
        ComponentType? type = ComponentTypes.Find(name);
        if (type?.Invoke == null)
        {
            Log.Warning($"{name}: no button {method}");
            return null;
        }
        if (running)
        {
            if (s_byEntity.TryGetValue(entity, out List<Component>? list))
                foreach (Component c in list)
                    if (!c.Destroyed && c.Type == type)
                    {
                        type.Invoke(c, method);
                        return null;
                    }
            return null;
        }
        using JsonDocument doc = JsonDocument.Parse(properties.Length == 0 ? "{}" : properties);
        Component? made = Make(entity, name, doc.RootElement);
        if (made == null)
            return null;
        type.Invoke(made, method);
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            type.Write(made, w);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }

    internal static string Values(ulong entity)
    {
        if (!s_byEntity.TryGetValue(entity, out List<Component>? list) || list.Count == 0)
            return "";
        int count = 0;
        foreach (Component c in list)
            count = Math.Max(count, c.Index + 1);
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartArray();
            for (int i = 0; i < count; ++i)
            {
                Component? c = list.Find(x => x.Index == i && !x.Destroyed);
                ComponentType? type = c?.Type;
                if (c == null || type == null)
                {
                    w.WriteNullValue();
                    continue;
                }
                w.WriteStartObject();
                type.Write(c, w);
                w.WriteEndObject();
            }
            w.WriteEndArray();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }
}

public static class EntityComponents
{
    /// <summary>Adds a script component in code (OnAwake now, OnStart before its first update).</summary>
    public static T AddComponent<T>(this Entity entity) where T : Component, new() => ScriptComponents.Add<T>(entity);

    /// <summary>The entity's first component of type T, or null.</summary>
    public static T? GetComponent<T>(this Entity entity) where T : Component => ScriptComponents.Get<T>(entity);
}
