using System;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_script_entities (API v18): what a game does to the scene's entities, checked on the HUD
// (each check "ok" or "BAD"); the test then checks the rows as drawn and that Stop puts it all back.
public sealed class EntitiesGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<EntitiesGame>();

    private string _report = "";
    private Entity _box, _child, _spawned;

    private static string Ok(bool ok) => ok ? "ok" : "BAD";

    public override void OnStart()
    {
        _box = Entity.Find("Box");
        _child = Entity.Find("Child");
        Entity gone = Entity.Find("Gone"), tinted = Entity.Find("Tinted"), hidden = Entity.Find("Hidden");

        Span<Entity> children = stackalloc Entity[4];
        int n = _box.Children(children);
        bool names = _box.Name == "Box" && _child.Name == "Child";
        bool parent = _child.Parent == _box && n == 1 && children[0] == _child;

        tinted.Name = "Painted";
        bool renamed = Entity.Find("Painted") == tinted && tinted.Name == "Painted" && !Entity.Find("Tinted").IsValid;

        gone.Destroy();
        bool destroyed = !gone.Enabled;

        _box.Enabled = false;
        bool off = !_box.Enabled && !_child.Enabled;
        _box.Enabled = true;
        bool on = _box.Enabled && _child.Enabled;

        bool tint = Renderer.SetTint(tinted, new Vector4(1, 0, 0, 1)) && Renderer.Tint(tinted).X == 1f && Renderer.Tint(tinted).Y == 0f;
        bool shadows = Renderer.SetCastShadows(tinted, false) && !Renderer.CastsShadows(tinted);
        bool material = Renderer.SetMaterial(tinted, "Assets/New Material.fmat");
        bool visible = Renderer.SetVisible(hidden, false) && !Renderer.IsVisible(hidden) && hidden.Enabled;

        _box.Position = new Vector3(3, 1, 0);
        _child.LocalPosition = new Vector3(0, 2, 0);
        _box.Rotation = Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI / 2);
        Quaternion back = _box.Rotation;
        bool rotation = MathF.Abs(MathF.Abs(Quaternion.Dot(back, Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI / 2))) - 1f) < 1e-4f
                        && Vector3.Distance(_box.Forward, -Vector3.UnitX) < 1e-4f;
        _box.Rotation = Quaternion.Identity;

        var reference = new Ref<WorldTransform>(_box);
        bool refOk = reference.Has && Vector3.Distance(reference.Get().Position, new Vector3(3, 1, 0)) < 1e-5f;

        _spawned = Scene.SpawnMesh("cube", new Vector3(5, 1, 0));
        bool spawned = _spawned.IsValid && Renderer.IsVisible(_spawned);
        Renderer.SetTint(_spawned, new Vector4(0, 1, 0, 1));

        _report = $"names {Ok(names)} parent {Ok(parent)} renamed {Ok(renamed)} destroyed {Ok(destroyed)} enabled {Ok(off && on)} " +
                  $"tint {Ok(tint)} shadows {Ok(shadows)} material {Ok(material)} visible {Ok(visible)} rotation {Ok(rotation)} " +
                  $"ref {Ok(refOk)} spawned {Ok(spawned)}";
    }

    public override void OnUpdate(float dt)
    {
        // A frame later the child has followed its parent (Scene::propagate).
        bool follows = Vector3.Distance(_child.Position, new Vector3(3, 3, 0)) < 1e-4f;
        Hud.Show($"{_report} follows {Ok(follows)} spawnedAlive {Ok(_spawned.IsAlive)}");
    }
}
