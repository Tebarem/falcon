using System.IO;
using System.Numerics;
using System.Text.Json;
using Falcon;
using Falcon.Editor;

namespace Toolbox;

// editor_library_api: editor code from a library's Editor/ folder (built into the editor's build only).
public static class Tools
{
    [EditorMenu("Tools/Toolbox/Rename Selection", Shortcut = "Ctrl+Alt+R")]
    public static void RenameSelection()
    {
        using var _ = Undo.Scope("Rename selection");
        EditorEntity e = Selection.Entity;
        if (e.IsValid)
            EditorScene.Rename(e, "Renamed");
    }

    // Two changes, one undo step.
    [EditorMenu("Tools/Toolbox/Make Marker")]
    public static void MakeMarker()
    {
        using var _ = Undo.Scope("Make marker");
        EditorEntity m = EditorScene.Create("empty", "Marker");
        EditorScene.SetTransform(m, new Vector3(1, 2, 3), Vector3.Zero, Vector3.One);
    }
}

// Hears the editor: what it heard goes to Cache/toolbox-events.txt (the test reads it).
public sealed class Watcher : EditorTool
{
    private static void Note(string what)
    {
        using JsonDocument project = EditorApp.Query("project");
        string dir = project.RootElement.GetProperty("dir").GetString() ?? ".";
        File.AppendAllText(Path.Combine(dir, "Cache", "toolbox-events.txt"), what + "\n");
    }

    public override void OnSelectionChanged(EditorEntity selected)
    {
        if (selected.IsValid)
            Note("select " + selected.Name);
    }

    public override void OnPlay() => Note("play " + (EditorApp.Playing ? "playing" : "not playing"));
    public override void OnStop() => Note("stop");
}
