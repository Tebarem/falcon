using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon;

// (API v22) Input: gamepads, text, the mouse in the game's view, and actions bound to keys, mouse
// buttons and gamepads.

/// <summary>SDL's gamepad buttons, by position (South: A on Xbox, Cross on PlayStation).</summary>
public enum GamepadButton
{
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc1, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2, Touchpad,
    Misc2, Misc3, Misc4, Misc5, Misc6,
}

public enum GamepadAxis { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger }

/// <summary>A gamepad: a slot (0..7) kept while it is connected. In the editor it reads only while
/// the game plays.</summary>
public readonly unsafe struct Gamepad
{
    public readonly int Slot;
    public Gamepad(int slot) => Slot = slot;

    /// <summary>How many slots there are (8); Connected says whether one holds a gamepad.</summary>
    public static int Slots => Api.Native->gamepadCount;

    /// <summary>The first connected gamepad (Connected false: none).</summary>
    public static Gamepad First
    {
        get
        {
            for (int i = 0; i < Slots; ++i)
                if (Api.Native->gamepads[i].id != 0)
                    return new Gamepad(i);
            return new Gamepad(0);
        }
    }

    /// <summary>How many are connected.</summary>
    public static int Count
    {
        get
        {
            int n = 0;
            for (int i = 0; i < Slots; ++i)
                n += Api.Native->gamepads[i].id != 0 ? 1 : 0;
            return n;
        }
    }

    private FxGamepad* Data => (uint)Slot < (uint)Slots ? &Api.Native->gamepads[Slot] : null;

    public bool Connected => Data != null && Data->id != 0;
    public bool IsDown(GamepadButton b) => Data != null && (Data->buttons & (1u << (int)b)) != 0;
    public bool Pressed(GamepadButton b) => Data != null && (Data->buttons & ~Data->buttonsPrev & (1u << (int)b)) != 0;
    public bool Released(GamepadButton b) => Data != null && (~Data->buttons & Data->buttonsPrev & (1u << (int)b)) != 0;

    /// <summary>An axis (sticks -1..1, +y down; triggers 0..1), zero within the dead zone.</summary>
    public float Axis(GamepadAxis a, float deadZone = 0.15f)
    {
        if (Data == null)
            return 0f;
        float v = Data->axes[(int)a];
        return MathF.Abs(v) < deadZone ? 0f : v;
    }

    /// <summary>A stick as a vector (+y down), zero within the dead zone (radial).</summary>
    public Vector2 LeftStick(float deadZone = 0.15f) => Stick(GamepadAxis.LeftX, deadZone);
    public Vector2 RightStick(float deadZone = 0.15f) => Stick(GamepadAxis.RightX, deadZone);

    private Vector2 Stick(GamepadAxis x, float deadZone)
    {
        if (Data == null)
            return default;
        var v = new Vector2(Data->axes[(int)x], Data->axes[(int)x + 1]);
        return v.Length() < deadZone ? default : v;
    }

    /// <summary>Rumbles it: low and high frequency motors 0..1, for some seconds.</summary>
    public bool Rumble(float low, float high, float seconds) =>
        Api.Native->gamepadRumble(Api.Ctx, Slot, low, high, (uint)Math.Max(0f, seconds * 1000f)) != 0;
}

public static unsafe partial class Input
{
    /// <summary>The mouse in the game's view (pixels from its top left: the editor's game view,
    /// the window in a game).</summary>
    public static Vector2 MouseViewPosition
    {
        get
        {
            FxMouse m;
            Api.Native->mouseGet(Api.Ctx, &m);
            return new Vector2(m.x - m.viewX, m.y - m.viewY);
        }
    }

    /// <summary>The game's view's size (pixels).</summary>
    public static Vector2 ViewSize
    {
        get
        {
            FxMouse m;
            Api.Native->mouseGet(Api.Ctx, &m);
            return new Vector2(m.viewWidth, m.viewHeight);
        }
    }

    /// <summary>Text input: on, the keyboard types (an IME too) and Text holds this frame's.</summary>
    public static bool TextInput
    {
        get => s_textOn;
        set
        {
            s_textOn = value;
            Api.Native->textMode(Api.Ctx, value ? 1 : 0);
        }
    }

    private static bool s_textOn;

    /// <summary>The text typed this frame (TextInput on): "" when none.</summary>
    public static string Text
    {
        get
        {
            Span<byte> utf8 = stackalloc byte[256];
            int n;
            fixed (byte* p = utf8)
                n = Api.Native->textInput(Api.Ctx, p, utf8.Length);
            if (n == 0)
                return "";
            if (n > 0)
                return Encoding.UTF8.GetString(utf8[..n]);
            byte[] big = new byte[-n];
            fixed (byte* p = big)
                n = Api.Native->textInput(Api.Ctx, p, big.Length);
            return Encoding.UTF8.GetString(big, 0, Math.Max(n, 0));
        }
    }

    // ---- actions

    private static readonly Dictionary<string, InputAction> s_actions = new();

    /// <summary>An action by name (made when first asked for): bind keys, mouse buttons and gamepad
    /// buttons and axes to it, then read it the same way whatever is bound.</summary>
    public static InputAction Action(string name)
    {
        if (!s_actions.TryGetValue(name, out InputAction? a))
            s_actions[name] = a = new InputAction(name);
        return a;
    }

    /// <summary>A two-axis action as a vector (+y as bound: "MoveX", "MoveY"), its length at most 1.</summary>
    public static Vector2 Vector(string x, string y)
    {
        var v = new Vector2(Action(x).Value, Action(y).Value);
        return v.LengthSquared() > 1f ? Vector2.Normalize(v) : v;
    }
}

/// <summary>
/// A named input bound to keys, mouse buttons, gamepad buttons and axes (any gamepad): Value is the
/// strongest of them (a key or button 1, an axis as it is; a negative binding counts as -1), and
/// IsDown / Pressed / Released are its value past half way. Rebind any time.
/// </summary>
public sealed class InputAction
{
    private readonly record struct Binding(int Kind, int Code, float Sign)  // kind 0 key, 1 mouse, 2 pad button, 3 pad axis, 4-6 Steam Input
    {
        public string? Name { get; init; }
    }
    private readonly List<Binding> _bindings = new();
    // Two streams, each with its own edges: the frame's (OnUpdate) and the simulation's ticks
    // (OnFixedUpdate: Pressed once per press, whatever the frame rate).
    private float _value, _previous, _tickValue, _tickPrevious;
    private ulong _frame = ulong.MaxValue, _tick = ulong.MaxValue;

    public string Name { get; }
    internal InputAction(string name) => Name = name;

    public InputAction Bind(Key key, float sign = 1f) { _bindings.Add(new(0, (int)key, sign)); return this; }
    public InputAction Bind(MouseButton button, float sign = 1f) { _bindings.Add(new(1, (int)button, sign)); return this; }
    public InputAction Bind(GamepadButton button, float sign = 1f) { _bindings.Add(new(2, (int)button, sign)); return this; }
    public InputAction Bind(GamepadAxis axis, float sign = 1f) { _bindings.Add(new(3, (int)axis, sign)); return this; }
    /// <summary>A Steam Input action (<see cref="SteamControllers"/>, started by the game): a digital
    /// one, or an analog one's x (<paramref name="axis"/> 0) or y (1).</summary>
    public InputAction BindSteam(string action, int axis = -1) { _bindings.Add(new(axis < 0 ? 4 : 5 + Math.Clamp(axis, 0, 1), 0, 1f) { Name = action }); return this; }
    /// <summary>Two keys as an axis: negative -1, positive +1 (A / D).</summary>
    public InputAction Bind(Key negative, Key positive) => Bind(negative, -1f).Bind(positive, 1f);
    public void Clear() => _bindings.Clear();

    private void Update()
    {
        if (Time.InTick)
        {
            if (_tick == Time.Tick)
                return;
            _tickPrevious = _tick == Time.Tick - 1 ? _tickValue : 0f;
            _tick = Time.Tick;
            _tickValue = Read();
            return;
        }
        if (_frame == Time.Frame)
            return;
        _previous = _frame == Time.Frame - 1 ? _value : 0f;
        _frame = Time.Frame;
        _value = Read();
    }

    private float Read()
    {
        float best = 0f;
        foreach (Binding b in _bindings)
        {
            float v = b.Kind switch
            {
                0 => Input.IsDown((Key)b.Code) ? 1f : 0f,
                1 => Input.IsDown((MouseButton)b.Code) ? 1f : 0f,
                2 => AnyPad(p => p.IsDown((GamepadButton)b.Code) ? 1f : 0f),
                3 => AnyPad(p => p.Axis((GamepadAxis)b.Code)),
                4 => SteamControllers.Digital(b.Name!) ? 1f : 0f,
                5 => SteamControllers.Analog(b.Name!).X,
                _ => SteamControllers.Analog(b.Name!).Y,
            } * b.Sign;
            if (MathF.Abs(v) > MathF.Abs(best))
                best = v;
        }
        return best;
    }

    private static float AnyPad(Func<Gamepad, float> read)
    {
        float best = 0f;
        for (int i = 0; i < Gamepad.Slots; ++i)
        {
            var p = new Gamepad(i);
            if (!p.Connected)
                continue;
            float v = read(p);
            if (MathF.Abs(v) > MathF.Abs(best))
                best = v;
        }
        return best;
    }

    /// <summary>-1 .. 1 (keys and buttons: 0 or the binding's sign).</summary>
    public float Value
    {
        get
        {
            Update();
            return Time.InTick ? _tickValue : _value;
        }
    }

    public bool IsDown => MathF.Abs(Value) >= 0.5f;
    public bool Pressed
    {
        get
        {
            Update();
            (float now, float before) = Time.InTick ? (_tickValue, _tickPrevious) : (_value, _previous);
            return MathF.Abs(now) >= 0.5f && MathF.Abs(before) < 0.5f;
        }
    }
    public bool Released
    {
        get
        {
            Update();
            (float now, float before) = Time.InTick ? (_tickValue, _tickPrevious) : (_value, _previous);
            return MathF.Abs(now) < 0.5f && MathF.Abs(before) >= 0.5f;
        }
    }
}
