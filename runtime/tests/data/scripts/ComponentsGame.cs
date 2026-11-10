using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

public enum Axis { X, Y, Z }

// A script component (API v20): set up in the Inspector, saved with the scene, run while playing.
public sealed class Spinner : Component
{
    [Property, Range(0, 720)] public float Speed = 90f;
    [Property] public Axis Around = Axis.Y;
    [Property] public Entity Target;
    [Property] public Vector3 Lift = new(0, 1, 0);
    [Property] public string Label = "spinner";

    public static int Awakes, Starts, Destroys, Fixed, Updates;

    public override void OnAwake() => ++Awakes;
    public override void OnStart() => ++Starts;
    public override void OnFixedUpdate(float dt) => ++Fixed;
    public override void OnDestroy() => ++Destroys;

    public override void OnUpdate(float dt)
    {
        ++Updates;
        Entity.Position += Lift * dt;  // (rises while playing; Stop puts it back)
    }
}

// The test's report: what the components did, on the HUD (VERSION changes with a hot reload).
public sealed class ComponentsGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<ComponentsGame>();

    private Entity _spawned;

    public override void OnStart() => _spawned = Prefab.Spawn("Assets/Spinny.fprefab", new Vector3(0, 5, 5));

    public override void OnUpdate(float dt)
    {
        Spinner? s = Entity.Find("Spinny").GetComponent<Spinner>();
        Spinner? spawned = _spawned.GetComponent<Spinner>();
        string props = s == null ? "none" :
            $"speed {s.Speed} around {s.Around} target {(s.Target == Entity.Find("Goal") ? "ok" : "BAD")} lift {s.Lift.Y} label {s.Label}";
        Hud.Show($"VERSION 1 awakes {Spinner.Awakes} starts {Spinner.Starts} fixed {(Spinner.Fixed > 0 ? "yes" : "no")} " +
                 $"updates {(Spinner.Updates > 0 ? "yes" : "no")} spawned {(spawned != null ? "yes" : "no")} {props}");
    }
}
