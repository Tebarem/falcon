using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_script_flow (API v23): time scale, saves, the camera (main, field of view, rays both ways),
// then Game.Quit (in the editor: Play stops).
public sealed class FlowGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<FlowGame>();

    private int _frames;
    private string _report = "";

    private static string Ok(bool ok) => ok ? "ok" : "BAD";

    public override void OnStart()
    {
        Time.Scale = 0.5f;
        Storage.Save("flow.txt", "saved");
        bool storage = Storage.Load("flow.txt") == "saved" && Storage.Exists("flow.txt");
        Storage.Delete("flow.txt");
        storage &= !Storage.Exists("flow.txt");
        bool main = Camera.Main == Entity.Find("Cam");
        bool fov = MathF.Abs(Camera.FieldOfView - 50f) < 0.5f;
        Vector2 center = Input.ViewSize / 2f;
        var (origin, direction) = Camera.ScreenToRay(center);
        bool ray = Vector3.Distance(origin, Camera.Position) < 1e-3f && Vector3.Dot(direction, Camera.Forward) > 0.9999f;
        var (pixel, inFront) = Camera.WorldToScreen(Camera.Position + Camera.Forward * 10f + Camera.Right * 2f);
        var (back, _) = Camera.ScreenToRay(pixel);
        bool screen = inFront && pixel.X > center.X && MathF.Abs(pixel.Y - center.Y) < 0.5f && back == origin;
        _report = $"storage {Ok(storage)} main {Ok(main)} fov {Ok(fov)} ray {Ok(ray)} screen {Ok(screen)}";
    }

    public override void OnUpdate(float dt)
    {
        ++_frames;
        bool scaled = MathF.Abs(Time.Delta - 0.5f * Time.UnscaledDelta) < 1e-5f && Time.Scale == 0.5f;
        Hud.Show($"{_report} scale {Ok(scaled)} frames {(_frames >= 30 ? "30" : "few")}");
        if (_frames == 60)
            Quit();
    }
}
