using System.IO;
using System.Text.Json;
using Falcon;
using Falcon.Editor;

namespace Panels;

// editor_library_ui: an editor window whose page (UI/echo.html) and C# side talk both ways; what the
// C# side hears goes to Cache/panel-events.txt (the test reads it).
[EditorWindow("Tools/Panels/Echo", Title = "Echo", Page = "echo.html", Width = 320, Height = 200)]
public sealed class Echo : EditorWindow
{
    private static void Note(string what)
    {
        using JsonDocument project = EditorApp.Query("project");
        string dir = project.RootElement.GetProperty("dir").GetString() ?? ".";
        File.AppendAllText(Path.Combine(dir, "Cache", "panel-events.txt"), what + "\n");
    }

    public override void OnOpen() => Note("open");

    public override void OnMessage(string json)
    {
        Note("got " + json);
        if (json == "\"hello\"")
            Send("\"pong\"");
    }
}
