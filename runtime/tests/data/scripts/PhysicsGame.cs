using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_script_physics (API v19): bodies, queries with layers, triggers, a character, a hinge and a
// collider changed while playing, each check "ok" or "BAD" on the HUD once it is known.
public sealed class PhysicsGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<PhysicsGame>();

    private Entity _ball, _floor, _zone, _ghost, _door, _grow, _hero, _runner, _dropped;
    private Joint _hinge;
    private readonly CharacterController _controller = new();
    private int _frame, _entered;
    private double _started = -1;
    private string _start = "", _later = "";
    private Quaternion _doorStart;

    private static string Ok(bool ok) => ok ? "ok" : "BAD";

    public override void OnStart()
    {
        _ball = Entity.Find("Ball");
        _floor = Entity.Find("Floor");
        _zone = Entity.Find("Zone");
        _ghost = Entity.Find("Ghost");
        _door = Entity.Find("Door");
        _grow = Entity.Find("Grow");
        _hero = Entity.Find("Hero");
        _runner = Entity.Find("Runner");

        bool gravity = Vector3.Distance(Physics.Gravity, new Vector3(0, -9.81f, 0)) < 0.01f;
        bool body = Physics.Body(_ball, out BodyInfo info) && info.Type == BodyType.Dynamic && MathF.Abs(info.Mass - 1f) < 0.01f;
        Physics.SetVelocity(_ball, new Vector3(3, 0, 0));
        bool velocity = Physics.Body(_ball, out info) && MathF.Abs(info.Velocity.X - 3f) < 1e-3f;
        Physics.SetVelocity(_ball, Vector3.Zero);
        bool teleport = Physics.Teleport(_ball, new Vector3(2, 5, 0));

        bool ray = Physics.Raycast(new Vector3(-2, 10, -2), -Vector3.UnitY, out SurfaceHit hit, 50f, QueryFilter.All) &&
                   hit.Entity == _floor && MathF.Abs(hit.Point.Y) < 0.01f;
        bool layers = !Physics.Raycast(new Vector3(-2, 10, -2), -Vector3.UnitY, out _, 50f, QueryFilter.AllBut(0));
        bool ignoring = !Physics.Raycast(new Vector3(-2, 10, -2), -Vector3.UnitY, out _, 50f, QueryFilter.All.Ignoring(_floor));
        bool sphereCast = Physics.SphereCast(new Vector3(-2, 10, -2), 0.5f, -Vector3.UnitY, out hit) && MathF.Abs(hit.Distance - 9.5f) < 0.02f;
        Span<Entity> found = stackalloc Entity[8];
        int n = Physics.OverlapSphere(new Vector3(-2, 0, -2), 0.6f, found);
        bool overlap = n >= 1 && found[..Math.Min(n, 8)].Contains(_floor);

        _hinge = Physics.CreateJoint(JointType.Hinge, _door, default,
                                     new JointSettings { Anchor = new Vector3(0, 1, -6.75f), Axis = Vector3.UnitY, Motor = true, MotorSpeed = 2f, MotorMax = 500f });
        _doorStart = _door.Rotation;
        _dropped = Physics.Spawn(BodyShape.Sphere, new Vector3(0, 4, 6), new Vector3(0.3f, 0, 0), Vector3.Zero, Vector3.One);

        _start = $"gravity {Ok(gravity)} body {Ok(body)} velocity {Ok(velocity)} teleport {Ok(teleport)} ray {Ok(ray)} layers {Ok(layers)} " +
                 $"ignoring {Ok(ignoring)} sphereCast {Ok(sphereCast)} overlap {Ok(overlap)} joint {Ok(_hinge.IsValid)}";
    }

    public override void OnUpdate(float dt)
    {
        ++_frame;
        if (_started < 0)
            _started = Time.Total;
        foreach (TriggerEvent t in Physics.Triggers())
            if (t.Entered && t.Trigger == _zone && t.Other == _dropped)
                ++_entered;
        // The hero walks +X onto a 0.3 m step; the runner into a wall. Both fall at 0.1 m a frame.
        if (_frame <= 80)
        {
            _controller.Move(_hero, new Vector3(0.05f, -0.1f, 0));
            _controller.Move(_runner, new Vector3(0.1f, -0.1f, 0));
        }
        if (_frame == 5)
        {
            Collider c = Ecs.Get<Collider>(_grow);
            c.Size = new Vector3(3f, c.Size.Y, 3f);  // (its body is built again at the next step)
            Ecs.Set(_grow, c);
        }
        // Two seconds of physics later (the editor's test frames are short), the rest.
        if (_later.Length == 0 && _frame > 80 && Time.Total - _started >= 2.0)
        {
            Vector3 ball = _ball.Position;
            bool teleported = MathF.Abs(ball.X - 2f) < 0.05f;  // (it fell from there: not back at x 0)
            CharacterMove step = _controller.Move(_hero, new Vector3(0, -0.1f, 0));
            bool climbed = step.Grounded && MathF.Abs(step.Position.Y - 0.3f) < 0.03f && _hero.Position.X > -5.5f;
            CharacterMove wall = _controller.Move(_runner, new Vector3(0.1f, -0.1f, 0));
            bool walled = wall.Grounded && wall.Blocked && MathF.Abs(_runner.Position.X - 4.2f) < 0.05f;
            bool ghost = _ghost.Position.Y < -1f;
            bool hinge = MathF.Abs(Quaternion.Dot(_door.Rotation, _doorStart)) < 0.95f;
            Span<Entity> found = stackalloc Entity[8];
            int n = Physics.OverlapSphere(new Vector3(-8, 1, 7.2f), 0.1f, found);
            bool grown = n >= 1 && found[..Math.Min(n, 8)].Contains(_grow);
            _later = $"teleported {Ok(teleported)} climbed {Ok(climbed)} walled {Ok(walled)} ghost {Ok(ghost)} hinge {Ok(hinge)} " +
                     $"grown {Ok(grown)} trigger {Ok(_entered == 1)}";
        }
        Hud.Show(_later.Length > 0 ? $"{_start} {_later}" : _start);
    }
}
