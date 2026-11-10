using System.Runtime.CompilerServices;
using Falcon;

namespace ScriptTest;

// editor_game_ui: the project's HTML UI (UI/Game/index.html) talks to the game while the editor plays.
public sealed class GameUiGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<GameUiGame>();

    public override void OnUpdate(float dt)
    {
        while (Ui.TryReceive(out string json))
        {
            Hud.Show("ui said " + json);
            if (json.Contains("ready"))
                Ui.Send("{\"type\":\"hi\"}");
        }
    }
}
