using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_script_render (API v24): render settings by name (bloom set, an unknown one NaN), and debug
// lines: a box (12 lines) every frame, a line for 5 s.
public sealed class RenderGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<RenderGame>();

    private string _report = "";

    public override void OnStart()
    {
        float was = Render.Bloom;
        Render.Bloom = 0.5f;
        bool bloom = MathF.Abs(Render.Bloom - 0.5f) < 1e-5f && MathF.Abs(was - 0.5f) > 1e-5f;
        bool unknown = float.IsNaN(Render.Get("noSuchSetting"));
        bool clamped = Render.Set("renderScale", 5f) == 1f;
        Falcon.Debug.Line(Vector3.Zero, Vector3.UnitY * 3f, Falcon.Debug.Red, 5f);
        _report = $"bloom {(bloom ? "ok" : "BAD")} unknown {(unknown ? "ok" : "BAD")} clamped {(clamped ? "ok" : "BAD")}";
    }

    public override void OnUpdate(float dt)
    {
        Falcon.Debug.Box(new Vector3(0, 1, 0), Vector3.One, Falcon.Debug.Green);
        Hud.Show(_report);
    }
}
