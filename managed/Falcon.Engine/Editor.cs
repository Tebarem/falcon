using System;
using System.Buffers;
using System.Collections.Generic;
using System.Numerics;
using System.Text;
using System.Text.Json;
using Falcon.Native;

namespace Falcon.Editor;

// (API v21) Editor code: what libraries' Editor/ folders and a project's Scripts/Editor/ add to the
// editor. It is built into the editor's build of the game only (FalconEditor; FALCON_EDITOR is
// defined), loaded and hot-reloaded with it, and never shipped.
//
//   public static class AlignTools
//   {
//       [EditorMenu("Tools/Drop to Ground")]
//       public static void Drop()
//       {
//           using var _ = Undo.Scope("Drop to ground");
//           EditorEntity e = Selection.Entity; ...
//       }
//   }
//
// The generator registers [EditorMenu] methods and EditorTool classes (no reflection).

/// <summary>A menu item in the editor (the Tools menu for "Tools/..."): a static method without
/// parameters, a command. Shortcut: "Ctrl+Shift+A" (the page's). Key: a key in the scene view
/// ("E", "Shift+D", "Alt+N"), working in Modes ("vertex|edge|face"; "" any), before the view's own
/// keys (E extrudes faces in a face mode and still rotates objects). Icon: one of the editor's icons,
/// for the level toolbar. Wheel: the directional wheel it is on ("ops": Alt+Q, "create": Shift+A).
/// Help: one line for tooltips and the command palette. The user's keymap may rebind Key.</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class EditorMenuAttribute(string path) : Attribute
{
    public string Path { get; } = path;
    public string? Shortcut { get; set; }
    public string? Key { get; set; }
    public string? Modes { get; set; }
    public string? Icon { get; set; }
    public string? Wheel { get; set; }
    public string? Help { get; set; }
}

/// <summary>
/// Editor code that lives while the editor runs (edit mode and play): made when the game's code is
/// loaded (and again after each hot reload), told what happens in the editor.
/// </summary>
public abstract class EditorTool
{
    /// <summary>Each editor frame.</summary>
    public virtual void OnUpdate(float dt) { }
    public virtual void OnSelectionChanged(EditorEntity selected) { }
    public virtual void OnPlay() { }
    public virtual void OnStop() { }
    public virtual void OnSceneSaved() { }
    /// <summary>(API v34) Each editor frame: its gizmos (Gizmos.*).</summary>
    public virtual void OnDrawGizmos() { }
    /// <summary>Its code is about to be replaced (a hot reload) or the editor closes.</summary>
    public virtual void OnUnload() { }
}

/// <summary>An editor window of HTML (a library's UI/ page, or the project's UI/): opened from the
/// Tools menu at `menu`, a floating window in the editor. Page: its path in the library's UI/ folder
/// (the project's UI/ for Scripts/Editor/ code).</summary>
[AttributeUsage(AttributeTargets.Class)]
public sealed class EditorWindowAttribute(string menu) : Attribute
{
    public string Menu { get; } = menu;
    public string? Title { get; set; }
    public string Page { get; set; } = "index.html";
    public int Width { get; set; } = 360;
    public int Height { get; set; } = 420;
}

/// <summary>
/// The C# side of an editor window ([EditorWindow]): made when the window opens, gone when it closes.
/// Its page talks to it with FalconPanel.send(value) (falcon-panel.js: any JSON value), which arrives
/// in OnMessage as JSON text; Send(json) reaches the page's FalconPanel.onMessage.
/// </summary>
public abstract class EditorWindow
{
    /// <summary>Its menu path (one window of a kind at a time).</summary>
    public string Id { get; internal set; } = "";

    public virtual void OnOpen() { }
    public virtual void OnClose() { }
    public virtual void OnMessage(string json) { }
    /// <summary>Each editor frame while it is open.</summary>
    public virtual void OnUpdate(float dt) { }

    /// <summary>Sends JSON (a value) to its page.</summary>
    public void Send(string json) => EditorApp.Send("editor.window.send", w =>
    {
        w.WriteString("window", Id);
        w.WriteString("data", json);
    });
}

/// <summary>An entity as the editor knows it: its Hierarchy id (stable while the scene is open).</summary>
public readonly record struct EditorEntity(uint Id)
{
    public bool IsValid => Id != 0;

    /// <summary>The ECS entity it is (scripts' Entity).</summary>
    public unsafe Entity Entity => Id == 0 || EditorApp.Api == null ? default : new Entity(EditorApp.Api->entityOf(EditorApp.Ctx, Id));

    /// <summary>The editor entity an ECS entity is (invalid: not one of the editor's).</summary>
    public static unsafe EditorEntity Of(Entity entity) => EditorApp.Api == null ? default : new EditorEntity(EditorApp.Api->idOf(EditorApp.Ctx, entity.Id));

    public string Name => Entity.IsValid ? Entity.Name : "";
}

/// <summary>Registers editor code (the generated code calls it).</summary>
public static class EditorRegistry
{
    internal sealed record MenuItem(string Path, string? Shortcut, Action Run)
    {
        public string? Key, Modes, Icon, Wheel, Help;
        public bool Builtin;  // the engine's own (the level tools): on the Level window, menus and palette, not the Tools menu
        // Where the Level window shows it: its section ("create", "edit", "select", "surface",
        // "object", "world"; "view" and "starter" are menus only) and the heading inside it, its
        // label there (the path's last part when null), and what it needs to run ("level" a level
        // mesh selected, "elements" chosen elements, "wall", "cut", "spline" that kind selected).
        public string? Group, Section, Label, Needs;
        public List<CommandOption>? Options;  // its settings (the window's ⚙)
    }
    internal sealed record WindowKind(string Menu, string Title, string Library, string Page, int Width, int Height, Func<EditorWindow> Create);
    internal static readonly List<MenuItem> Menus = new();
    internal static readonly List<Func<EditorTool>> Tools = new();
    internal static readonly List<WindowKind> Windows = new();

    public static void Menu(string path, string? shortcut, Action run) => Menus.Add(new MenuItem(path, shortcut, run));
    /// <summary>A command: a menu item with a scene key, element modes, an icon, a wheel and help.</summary>
    public static void Command(string path, string? shortcut, string? key, string? modes, string? icon, string? wheel, string? help, Action run) =>
        Menus.Add(new MenuItem(path, shortcut, run) { Key = key, Modes = modes, Icon = icon, Wheel = wheel, Help = help });
    public static void Tool(Func<EditorTool> create) => Tools.Add(create);
    private static bool s_builtins;

    // The engine's own commands (Falcon.Level.LevelCommands), after the code's: its keys come first.
    internal static void EnsureBuiltins()
    {
        if (s_builtins)
            return;
        s_builtins = true;
        Falcon.Level.LevelCommands.Register();
    }

    internal static MenuItem Builtin(string path, string? key, string? modes, string? icon, string? wheel, string? help, Action run)
    {
        var m = new MenuItem(path, null, run) { Key = key, Modes = modes, Icon = icon, Wheel = wheel, Help = help, Builtin = true };
        Menus.Add(m);
        return m;
    }
    /// <summary>A window kind: its library ("" the project's UI/), its page in that UI/ folder.</summary>
    public static void Window(string menu, string title, string library, string page, int width, int height, Func<EditorWindow> create) =>
        Windows.Add(new WindowKind(menu, title, library, page, width, height, create));

    internal static void Forget()
    {
        Menus.Clear();
        Tools.Clear();
        Windows.Clear();
        s_builtins = false;
    }
}

/// <summary>A command's setting, shown under it in the Level window (its ⚙) and kept while the editor
/// runs: a number (Min..Max, Unit), a whole number, a choice (Value: the index into Choices) or a
/// switch (0 / 1). The command reads Value when it runs.</summary>
internal sealed class CommandOption(string name, string kind, double value)
{
    public string Name { get; } = name;
    public string Kind { get; } = kind;
    public double Value = value;
    public double Min = double.NegativeInfinity, Max = double.PositiveInfinity;
    public string? Unit;
    public string[]? Choices;

    public static CommandOption Number(string name, double value, double min, double max, string? unit = null) =>
        new(name, "number", value) { Min = min, Max = max, Unit = unit };
    public static CommandOption Whole(string name, int value, int min, int max) => new(name, "int", value) { Min = min, Max = max };
    public static CommandOption Choice(string name, int value, params string[] choices) =>
        new(name, "choice", value) { Min = 0, Max = choices.Length - 1, Choices = choices };

    public int Index => (int)Value;

    /// <summary>Set from the page: clamped (whole for int and choice).</summary>
    public void Set(double v)
    {
        if (double.IsNaN(v))
            return;
        if (Kind != "number")
            v = Math.Round(v);
        Value = Math.Clamp(v, Min, Max);
    }
}

/// <summary>The editor: messages to it, its state (null outside the editor: a game has none).</summary>
public static unsafe class EditorApp
{
    internal static FxEditorApi* Api => Native.Api.Native == null ? null : Native.Api.Native->editor;
    internal static nint Ctx => Api->context;

    /// <summary>Whether editor code runs in the editor now (false in a game).</summary>
    public static bool Available => Api != null;

    /// <summary>Sends an editor message ({"type": "editor.entity.create", ...}) as the editor's page
    /// does: it is handled at once, its changes undoable (Undo.Scope makes several one step).</summary>
    public static void Send(string json)
    {
        if (Api == null)
            return;
        byte[] utf8 = Encoding.UTF8.GetBytes(json);
        fixed (byte* p = utf8)
            Api->message(EditorApp.Ctx, p, utf8.Length);
    }

    /// <summary>Sends a message built from a type and fields (written as JSON).</summary>
    public static void Send(string type, Action<Utf8JsonWriter> fields)
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteString("type", type);
            fields(w);
            w.WriteEndObject();
        }
        Send(Encoding.UTF8.GetString(buffer.WrittenSpan));
    }

    /// <summary>The editor's state: "entities", "selection", "project", "playing" (a JSON document).</summary>
    public static JsonDocument Query(string what)
    {
        if (Api == null)
            return JsonDocument.Parse("null");
        Span<byte> name = stackalloc byte[64];
        int n = Encoding.UTF8.GetBytes(what, name[..63]);
        name[n] = 0;
        byte[] buffer = new byte[4096];
        int length;
        fixed (byte* w = name)
        {
            fixed (byte* p = buffer)
                length = Api->query(EditorApp.Ctx, w, p, buffer.Length);
            if (length < 0)
            {
                buffer = new byte[-length];
                fixed (byte* p = buffer)
                    length = Api->query(EditorApp.Ctx, w, p, buffer.Length);
            }
        }
        return JsonDocument.Parse(buffer.AsMemory(0, Math.Max(length, 0)));
    }

    public static bool Playing
    {
        get
        {
            using JsonDocument d = Query("playing");
            return d.RootElement.ValueKind == JsonValueKind.Object && d.RootElement.GetProperty("playing").GetBoolean();
        }
    }

    public static void Play() => Send("{\"type\":\"editor.play\"}");
    public static void Stop() => Send("{\"type\":\"editor.stop\"}");
    public static void SaveScene() => Send("{\"type\":\"editor.scene.save\"}");

    /// <summary>A notice in the editor's status line (level: info, warning, error).</summary>
    public static void Notice(string text, string level = "info") => Log.Info($"[{level}] {text}");
}

/// <summary>The editor's selection.</summary>
public static class Selection
{
    /// <summary>The selected entity (invalid: none). Setting it selects (and shows) it.</summary>
    public static EditorEntity Entity
    {
        get
        {
            using JsonDocument d = EditorApp.Query("selection");
            return d.RootElement.ValueKind == JsonValueKind.Object ? new EditorEntity(d.RootElement.GetProperty("id").GetUInt32()) : default;
        }
        set => EditorApp.Send("editor.select", w => w.WriteNumber("id", value.Id));
    }
}

/// <summary>Changes made by editor code as one undo step: `using var _ = Undo.Scope("Align")`.</summary>
public static unsafe class Undo
{
    public static IDisposable Scope(string name)
    {
        if (EditorApp.Api != null)
        {
            Span<byte> utf8 = stackalloc byte[256];
            int n = Encoding.UTF8.GetBytes(name, utf8[..255]);
            utf8[n] = 0;
            fixed (byte* p = utf8)
                EditorApp.Api->undoBegin(EditorApp.Ctx, p);
        }
        return new End();
    }

    public static void Perform() => EditorApp.Send("{\"type\":\"editor.undo\"}");
    public static void Redo() => EditorApp.Send("{\"type\":\"editor.redo\"}");

    private sealed class End : IDisposable
    {
        private bool _done;

        public void Dispose()
        {
            if (_done || EditorApp.Api == null)
                return;
            _done = true;
            EditorApp.Api->undoEnd(EditorApp.Ctx);
        }
    }
}

/// <summary>The open scene, as the editor edits it (each change undoable, saved with the scene).</summary>
public static class EditorScene
{
    /// <summary>The scene's entities (the Hierarchy).</summary>
    public static List<EditorEntity> Entities()
    {
        var list = new List<EditorEntity>();
        using JsonDocument d = EditorApp.Query("entities");
        if (d.RootElement.ValueKind == JsonValueKind.Array)
            foreach (JsonElement e in d.RootElement.EnumerateArray())
                if (e.GetProperty("alive").GetBoolean())
                    list.Add(new EditorEntity(e.GetProperty("id").GetUInt32()));
        return list;
    }

    /// <summary>The first live entity with this name (invalid: none).</summary>
    public static EditorEntity Find(string name)
    {
        using JsonDocument d = EditorApp.Query("entities");
        if (d.RootElement.ValueKind == JsonValueKind.Array)
            foreach (JsonElement e in d.RootElement.EnumerateArray())
                if (e.GetProperty("alive").GetBoolean() && e.GetProperty("name").GetString() == name)
                    return new EditorEntity(e.GetProperty("id").GetUInt32());
        return default;
    }

    /// <summary>A new entity: kind "empty", "primitive" (with a primitive: cube, sphere, cylinder,
    /// capsule, cone, plane), "light", "camera"; under a parent (invalid: a root). It is selected.</summary>
    public static EditorEntity Create(string kind, string name, string? primitive = null, EditorEntity parent = default)
    {
        EditorApp.Send("editor.entity.create", w =>
        {
            w.WriteString("kind", kind);
            w.WriteString("name", name);
            if (primitive != null)
                w.WriteString("primitive", primitive);
            if (parent.IsValid)
                w.WriteNumber("parent", parent.Id);
        });
        return Selection.Entity;
    }

    public static void Delete(EditorEntity e) => EditorApp.Send("editor.entity.delete", w => w.WriteNumber("id", e.Id));

    public static void Rename(EditorEntity e, string name) => EditorApp.Send("editor.entity.rename", w =>
    {
        w.WriteNumber("id", e.Id);
        w.WriteString("name", name);
    });

    /// <summary>Its transform: position, rotation (Euler degrees, as the Inspector shows it), scale.</summary>
    public static void SetTransform(EditorEntity e, Vector3 position, Vector3 rotationDegrees, Vector3 scale) =>
        EditorApp.Send("editor.transform.set", w =>
        {
            w.WriteNumber("id", e.Id);
            ComponentJson.Write(w, "position", position);
            ComponentJson.Write(w, "rotation", rotationDegrees);
            ComponentJson.Write(w, "scale", scale);
        });

    /// <summary>Moves it, keeping its rotation and scale.</summary>
    public static void SetPosition(EditorEntity e, Vector3 position)
    {
        Entity entity = e.Entity;
        Quaternion q = entity.Rotation;
        // (Euler angles from the rotation, as the Inspector takes them: X, Y, Z degrees)
        Vector3 euler = ToEuler(q);
        SetTransform(e, position, euler, entity.Scale);
    }

    /// <summary>A component added in the Inspector's way: "Light", "PhysicsBody", "BoxCollider",
    /// "Script:MyGame.Spinner", ...</summary>
    public static void AddComponent(EditorEntity e, string component) => EditorApp.Send("editor.component.add", w =>
    {
        w.WriteNumber("id", e.Id);
        w.WriteString("component", component);
    });

    public static void RemoveComponent(EditorEntity e, string component) => EditorApp.Send("editor.component.remove", w =>
    {
        w.WriteNumber("id", e.Id);
        w.WriteString("component", component);
    });

    /// <summary>A script component's property (its class's full name), as the Inspector sets it.</summary>
    public static void SetProperty(EditorEntity e, string componentClass, string property, Action<Utf8JsonWriter, string> value) =>
        EditorApp.Send("editor.script.set", w =>
        {
            w.WriteNumber("id", e.Id);
            w.WriteString("class", componentClass);
            w.WriteStartObject("values");
            value(w, property);
            w.WriteEndObject();
        });

    public static void SetProperty(EditorEntity e, string componentClass, string property, float value) =>
        SetProperty(e, componentClass, property, (w, n) => w.WriteNumber(n, value));

    public static void SetProperty(EditorEntity e, string componentClass, string property, string value) =>
        SetProperty(e, componentClass, property, (w, n) => w.WriteString(n, value));

    internal static Vector3 ToEuler(Quaternion q)
    {
        // The engine's Euler order (the Inspector's): X, then Y, then Z, in degrees.
        Matrix4x4 m = Matrix4x4.CreateFromQuaternion(q);
        float y = MathF.Asin(Math.Clamp(-m.M13, -1f, 1f));
        float x, z;
        if (MathF.Abs(m.M13) < 0.9999f)
        {
            x = MathF.Atan2(m.M23, m.M33);
            z = MathF.Atan2(m.M12, m.M11);
        }
        else
        {
            x = MathF.Atan2(-m.M32, m.M22);
            z = 0f;
        }
        const float deg = 180f / MathF.PI;
        return new Vector3(x * deg, y * deg, z * deg);
    }
}

/// <summary>The editor code's side of the engine (Host calls it).</summary>
internal static class EditorHost
{
    private static readonly List<EditorTool> s_tools = new();
    private static readonly Dictionary<string, EditorWindow> s_windows = new();
    private static bool s_made;

    private static void Make()
    {
        if (s_made)
            return;
        s_made = true;
        EditorRegistry.EnsureBuiltins();
        foreach (Func<EditorTool> create in EditorRegistry.Tools)
            try
            {
                s_tools.Add(create());
            }
            catch (Exception e)
            {
                Log.Error($"editor tool failed to start: {e}");
            }
    }

    private static void Each(Action<EditorTool> call, string what)
    {
        foreach (EditorTool t in s_tools)
            try
            {
                call(t);
            }
            catch (Exception e)
            {
                Log.Error($"{t.GetType().Name}.{what} failed: {e}");
            }
    }

    internal static void DrawGizmos() => Each(static t => t.OnDrawGizmos(), "OnDrawGizmos");

    internal static void Update(float dt)
    {
        Make();
        Falcon.Level.SceneViewport.Refresh();  // (the scene view's input this frame, for every tool)
        try
        {
            Falcon.Level.ModalOperation.Update();
        }
        catch (Exception e)
        {
            Log.Error($"modal operation failed: {e}");
        }
        PublishState();
        Each(t => t.OnUpdate(dt), "OnUpdate");
        foreach (EditorWindow w in s_windows.Values)
            try
            {
                w.OnUpdate(dt);
            }
            catch (Exception e)
            {
                Log.Error($"{w.GetType().Name}.OnUpdate failed: {e}");
            }
    }

    private static void Window(string id, Action<EditorWindow> call, string what)
    {
        if (!s_windows.TryGetValue(id, out EditorWindow? w))
            return;
        try
        {
            call(w);
        }
        catch (Exception e)
        {
            Log.Error($"{w.GetType().Name}.{what} failed: {e}");
        }
    }

    internal static bool Run(string menu)
    {
        EditorRegistry.EnsureBuiltins();
        foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            if (m.Path == menu)
            {
                try
                {
                    m.Run();
                }
                catch (Exception e)
                {
                    Log.Error($"menu item {menu} failed: {e}");
                }
                return true;
            }
        return false;
    }

    internal static void Event(string json)
    {
        Make();
        using JsonDocument d = JsonDocument.Parse(json);
        string type = d.RootElement.GetProperty("type").GetString() ?? "";
        switch (type)
        {
        case "selection":
        {
            var selected = new EditorEntity(d.RootElement.GetProperty("id").GetUInt32());
            Each(t => t.OnSelectionChanged(selected), "OnSelectionChanged");
            break;
        }
        case "option":  // the Level window's ⚙: {"menu", "name", "value"}
        {
            string menu = d.RootElement.GetProperty("menu").GetString() ?? "";
            string name = d.RootElement.GetProperty("name").GetString() ?? "";
            double value = d.RootElement.TryGetProperty("value", out JsonElement v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble()
                         : v.ValueKind == JsonValueKind.True ? 1 : v.ValueKind == JsonValueKind.False ? 0 : double.NaN;
            if (menu == Falcon.Level.LastOperation.Path)  // (the last operation's amount)
            {
                Falcon.Level.LastOperation.Change(value);
                break;
            }
            foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
                if (m.Path == menu && m.Options != null)
                    foreach (CommandOption o in m.Options)
                        if (o.Name == name)
                            o.Set(value);
            Falcon.Level.LastOperation.SettingChanged(menu);
            break;
        }
        case "play": Each(t => t.OnPlay(), "OnPlay"); break;
        case "stop": Each(t => t.OnStop(), "OnStop"); break;
        case "saved": Each(t => t.OnSceneSaved(), "OnSceneSaved"); break;
        case "window.open":
        {
            string id = d.RootElement.GetProperty("window").GetString() ?? "";
            if (s_windows.ContainsKey(id))
                break;
            foreach (EditorRegistry.WindowKind k in EditorRegistry.Windows)
                if (k.Menu == id)
                {
                    try
                    {
                        EditorWindow w = k.Create();
                        w.Id = id;
                        s_windows[id] = w;
                        w.OnOpen();
                    }
                    catch (Exception e)
                    {
                        Log.Error($"editor window {id} failed to open: {e}");
                    }
                }
            break;
        }
        case "window.message":
        {
            string data = d.RootElement.GetProperty("data").GetString() ?? "null";
            Window(d.RootElement.GetProperty("window").GetString() ?? "", w => w.OnMessage(data), "OnMessage");
            break;
        }
        case "window.close":
        {
            string id = d.RootElement.GetProperty("window").GetString() ?? "";
            Window(id, w => w.OnClose(), "OnClose");
            s_windows.Remove(id);
            break;
        }
        }
    }

    /// <summary>The code is being replaced or unloaded: tools told, everything dropped.</summary>
    internal static void Unload()
    {
        Each(t => t.OnUnload(), "OnUnload");
        foreach (EditorWindow w in s_windows.Values)  // (open windows: made again on the new code when their pages say so)
            try
            {
                w.OnClose();
            }
            catch (Exception e)
            {
                Log.Error($"{w.GetType().Name}.OnClose failed: {e}");
            }
        s_windows.Clear();
        s_tools.Clear();
        s_made = false;
        EditorRegistry.Forget();
    }

    private static string s_state = "";

    // The commands' settings as they are now ({"options": {"Mesh/Bevel": {"Segments": 4, ...}}}) to the
    // page when they change (an operation's keys change them too: Bevel's wheel).
    private static void PublishState()
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteStartObject("options");
            foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            {
                if (m.Options == null)
                    continue;
                w.WriteStartObject(m.Path);
                foreach (CommandOption o in m.Options)
                    w.WriteNumber(o.Name, o.Value);
                w.WriteEndObject();
            }
            w.WriteEndObject();
            if (Falcon.Level.ModalOperation.Active?.Menu is string active)  // (its settings in the Level window while it runs)
                w.WriteString("active", active);
            Falcon.Level.LastOperation.Write(w);
            w.WriteEndObject();
        }
        string state = Encoding.UTF8.GetString(buffer.WrittenSpan);
        if (state != s_state && Falcon.Level.SceneViewport.ToolState(state))
            s_state = state;
    }

    internal static string Describe()
    {
        EditorRegistry.EnsureBuiltins();
        s_state = "";  // (the page hears the state again with the new code)
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            w.WriteStartArray("menus");
            foreach (EditorRegistry.MenuItem m in EditorRegistry.Menus)
            {
                w.WriteStartObject();
                w.WriteString("path", m.Path);
                if (m.Shortcut != null)
                    w.WriteString("shortcut", m.Shortcut);
                if (m.Key != null)
                    w.WriteString("key", m.Key);
                if (m.Modes != null)
                    w.WriteString("modes", m.Modes);
                if (m.Icon != null)
                    w.WriteString("icon", m.Icon);
                if (m.Wheel != null)
                    w.WriteString("wheel", m.Wheel);
                if (m.Help != null)
                    w.WriteString("help", m.Help);
                if (m.Builtin)
                    w.WriteBoolean("builtin", true);
                if (m.Group != null)
                    w.WriteString("group", m.Group);
                if (m.Section != null)
                    w.WriteString("section", m.Section);
                if (m.Label != null)
                    w.WriteString("label", m.Label);
                if (m.Needs != null)
                    w.WriteString("needs", m.Needs);
                if (m.Options != null)
                {
                    w.WriteStartArray("options");  // (their values: the tool state, as they change)
                    foreach (CommandOption o in m.Options)
                    {
                        w.WriteStartObject();
                        w.WriteString("name", o.Name);
                        w.WriteString("kind", o.Kind);
                        if (!double.IsInfinity(o.Min))
                            w.WriteNumber("min", o.Min);
                        if (!double.IsInfinity(o.Max))
                            w.WriteNumber("max", o.Max);
                        if (o.Unit != null)
                            w.WriteString("unit", o.Unit);
                        if (o.Choices != null)
                        {
                            w.WriteStartArray("choices");
                            foreach (string c in o.Choices)
                                w.WriteStringValue(c);
                            w.WriteEndArray();
                        }
                        w.WriteEndObject();
                    }
                    w.WriteEndArray();
                }
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteStartArray("windows");
            foreach (EditorRegistry.WindowKind k in EditorRegistry.Windows)
            {
                w.WriteStartObject();
                w.WriteString("menu", k.Menu);
                w.WriteString("title", k.Title);
                w.WriteString("library", k.Library);
                w.WriteString("page", k.Page);
                w.WriteNumber("width", k.Width);
                w.WriteNumber("height", k.Height);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteNumber("tools", EditorRegistry.Tools.Count);
            Falcon.Level.LevelCommands.WriteChords(w);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }
}
