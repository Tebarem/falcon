using Falcon.Editor;

namespace Falcon.Level;

/// <summary>
/// Help/Make Sample Level: a small level built with the level tools in the open scene, to take
/// apart: a room (parametric walls, a floor) with a door and a window cut in it, worn old concrete
/// (a smart material with live wear and grime), a road along a spline with fence posts copied
/// along it, a hip roof, a ground, an entrance trigger and a nav area, and the navigation mesh baked over it
/// all. Each step is the same message the tools send (one undo step each).
/// </summary>
internal static class SampleLevel
{
    internal static void Make()
    {
        // The ground and the room (its walls round a 10 x 8 m floor, 3 m high).
        Send("""{"type":"editor.entity.create","kind":"level","shape":"plane","name":"Sample Ground"}""");
        Send("""{"type":"editor.transform.set","id":"Sample Ground","position":[0,0,0],"rotation":[0,0,0],"scale":[12,1,12]}""");
        Send("""{"type":"editor.level.wall.create","points":[[-5,-4],[5,-4],[5,4],[-5,4]],"closed":true,"floor":true,"ceiling":false,"height":3,"thickness":0.25,"name":"Sample Room"}""");
        // A door and a window in it (openings: they slide along the wall, cut as they go).
        Send("""{"type":"editor.select","id":"Sample Room"}""");
        Send("""{"type":"editor.level.opening","distance":5,"width":1.2,"height":2.2,"name":"Sample Door"}""");
        Send("""{"type":"editor.select","id":"Sample Room"}""");
        Send("""{"type":"editor.level.opening","distance":14,"sill":1,"width":1.8,"height":1.2,"name":"Sample Window"}""");
        // Old concrete: the starter smart materials, one on the room (its wear and grime live).
        Send("""{"type":"editor.level.smart.starter"}""");
        Send("""{"type":"editor.select","id":"Sample Room"}""");
        Send("""{"type":"editor.level.smart","path":"Assets/Smart/Old Concrete.fsmart"}""");
        // A hip roof over the room.
        Send("""{"type":"editor.select","id":"Sample Room"}""");
        Send("""{"type":"editor.level.roof.create","style":"hip","pitch":30,"overhang":0.5,"name":"Sample Roof"}""");
        // A road along a spline past the door, fence posts copied along it every 2 m.
        Send("""{"type":"editor.level.spline.create","points":[[-14,0,8],[-4,0,10],[6,0,9],[14,0,11]],"profile":"road","name":"Sample Road"}""");
        Send("""{"type":"editor.entity.create","kind":"level","shape":"box","name":"Sample Fence Post"}""");
        Send("""{"type":"editor.transform.set","id":"Sample Fence Post","position":[0,0.6,-2.4],"rotation":[0,0,0],"scale":[0.12,1.2,0.12]}""");
        Send("""{"type":"editor.select","ids":["Sample Road","Sample Fence Post"]}""");
        Send("""{"type":"editor.level.modifier","op":"add","kind":"curve","follow":"copies"}""");
        Send("""{"type":"editor.level.modifier","op":"set","index":0,"spacing":2}""");
        // An entrance trigger at the door, a nav area (mud: paths go round) by the window.
        Send("""{"type":"editor.level.volume","create":true,"kind":"trigger","name":"Sample Entrance","tag":"entrance"}""");
        Send("""{"type":"editor.transform.set","id":"Sample Entrance","position":[0,1.2,-4.6],"rotation":[0,0,0],"scale":[2,2.4,1.2]}""");
        Send("""{"type":"editor.level.volume","create":true,"kind":"navArea","name":"Sample Mud","cost":8}""");
        Send("""{"type":"editor.transform.set","id":"Sample Mud","position":[8,0.5,0],"rotation":[0,0,0],"scale":[4,1,6]}""");
        // The navigation mesh over all of it.
        Send("""{"type":"editor.nav.bake"}""");
        Send("""{"type":"editor.select","id":"Sample Room"}""");
        Send("""{"type":"editor.frame","id":"Sample Room"}""");
    }

    private static void Send(string json) => EditorApp.Send(json);
}
