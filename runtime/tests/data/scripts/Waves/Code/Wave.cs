using System;
using System.Numerics;
using Falcon;

namespace Waves;

// A library's script component (editor_library): built into the game once the library is trusted.
public sealed class Wave : Component
{
    [Property] public float Height = 2.5f;

    public override void OnUpdate(float dt) => Hud.Show($"wave {Height}");
}
