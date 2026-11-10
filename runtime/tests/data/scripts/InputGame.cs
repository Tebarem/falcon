using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_script_input (API v22): a virtual gamepad, actions bound to keys and the gamepad, typed text,
// a key the old enum lacked (F13 is not: the full scancode enum has it), and the game's view, on the HUD.
public sealed class InputGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<InputGame>();

    private string _typed = "";
    private bool _jumped;

    public override void OnStart()
    {
        Input.Action("Jump").Bind(Key.Space).Bind(GamepadButton.South);
        Input.Action("MoveX").Bind(Key.A, Key.D).Bind(GamepadAxis.LeftX);
        Input.TextInput = true;
    }

    public override void OnUpdate(float dt)
    {
        _typed += Input.Text;
        if (Input.Action("Jump").Pressed)
            _jumped = true;
        Gamepad pad = Gamepad.First;
        Hud.Show($"pads {Gamepad.Count} south {(pad.IsDown(GamepadButton.South) ? "down" : "up")} lx {pad.Axis(GamepadAxis.LeftX):F1} " +
                 $"jump {(_jumped ? "yes" : "no")} movex {Input.Action("MoveX").Value:F1} typed [{_typed}] " +
                 $"view {(Input.ViewSize.X > 0 && Input.ViewSize.Y > 0 ? "ok" : "BAD")} f5 {(Input.IsDown(Key.F5) ? "down" : "up")}");
    }
}
