using System.Globalization;
using Falcon.Editor;

namespace Falcon.Level;

/// <summary>
/// Help/Make Starter Kit: door, double door, window and vent prefabs in Assets/Kit ("Kit Door"...). Each is a wall
/// opening with its frame under it (jambs, a head, a sill for the windows): drop one on a wall and it
/// goes in there, cutting it; move it and it slides along the wall. They are made on a bench wall out
/// of the way, saved, and the bench deleted (one undo step each, like the tools).
/// </summary>
internal static class StarterKit
{
    private const float Frame = 0.08f, Depth = 0.32f;

    internal static void Make()
    {
        Send("""{"type":"editor.level.wall.create","points":[[-60,-60],[-40,-60]],"height":3,"thickness":0.25,"name":"Kit Bench"}""");
        Piece("Kit Door", 2, 1.0f, 2.1f, 0.0f, false);
        Piece("Kit Double Door", 6, 1.8f, 2.2f, 0.0f, false);
        Piece("Kit Window", 10, 1.2f, 1.1f, 0.9f, true);
        Piece("Kit Vent", 14, 0.5f, 0.5f, 2.0f, true);
        Send("""{"type":"editor.entity.delete","id":"Kit Bench"}""");
    }

    // An opening `distance` along the bench, its frame under it (its own space: x along the wall, y up
    // from its sill, z through it), saved as Assets/Kit/<name>.fprefab.
    private static void Piece(string name, float distance, float width, float height, float sill, bool withSill)
    {
        Send("""{"type":"editor.select","id":"Kit Bench"}""");
        Send($$"""{"type":"editor.level.opening","distance":{{F(distance)}},"width":{{F(width)}},"height":{{F(height)}},"sill":{{F(sill)}},"name":"{{name}}"}""");
        float side = width * 0.5f + Frame * 0.5f;
        Box(name, $"{name} Jamb L", -side, height * 0.5f, Frame, height, Depth);
        Box(name, $"{name} Jamb R", side, height * 0.5f, Frame, height, Depth);
        Box(name, $"{name} Head", 0, height + Frame * 0.5f, width + Frame * 2, Frame, Depth);
        if (withSill)
            Box(name, $"{name} Sill", 0, -Frame * 0.5f, width + Frame * 2, Frame, Depth + 0.06f);
        Send($$"""{"type":"editor.prefab.create","id":"{{name}}","folder":"Assets/Kit"}""");
    }

    private static void Box(string parent, string name, float x, float y, float sx, float sy, float sz)
    {
        Send($$"""{"type":"editor.entity.create","kind":"level","shape":"box","name":"{{name}}","parent":"{{parent}}"}""");
        Send($$"""{"type":"editor.transform.set","id":"{{name}}","position":[{{F(x)}},{{F(y)}},0],"rotation":[0,0,0],"scale":[{{F(sx)}},{{F(sy)}},{{F(sz)}}]}""");
    }

    private static string F(float v) => v.ToString("0.###", CultureInfo.InvariantCulture);

    private static void Send(string json) => EditorApp.Send(json);
}
