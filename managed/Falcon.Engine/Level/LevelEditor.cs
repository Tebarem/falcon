using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon.Level;

// Editor code's side of the level tools: the scene view's input (polled once a frame, the same for
// every tool), its capture by a modal operation, the key hints bar, and the element selection.

/// <summary>What clicks and the gizmo act on in the scene view.</summary>
public enum ElementMode : uint
{
    Object = 0,
    Vertex = 1,
    Edge = 2,
    Face = 3,
}

/// <summary>The scene view this editor frame: the mouse and the camera's ray through it, and what
/// happened since the last frame (presses, releases, keys). Empty outside the editor.</summary>
public sealed class ViewportInput
{
    public Vector2 Mouse, Size;
    public Vector3 RayOrigin, RayDirection;
    public Vector3 CameraPosition, CameraForward, CameraRight, CameraUp;
    public float P00 = 1f, P11 = 1f;
    /// <summary>Held: 1 left, 2 right, 4 middle; and pressed / released since the last frame.</summary>
    public uint Buttons, Pressed, Released;
    public bool Shift, Ctrl, Alt;
    public float Wheel;
    public bool Inside, Captured;
    public ElementMode Element;
    /// <summary>Keys pressed since the last frame, in order.</summary>
    public readonly List<Key> Keys = new();

    public bool LeftPressed => (Pressed & 1) != 0;
    public bool RightPressed => (Pressed & 2) != 0;
    public bool LeftReleased => (Released & 1) != 0;
    public bool LeftHeld => (Buttons & 1) != 0;
    public bool KeyPressed(Key k) => Keys.Contains(k);

    /// <summary>A world point on the view, in pixels from its top left (false: behind the camera).</summary>
    public bool WorldToView(Vector3 p, out Vector2 view)
    {
        Vector3 d = p - CameraPosition;
        float z = Vector3.Dot(d, CameraForward);
        view = default;
        if (z <= 1e-4f)
            return false;
        float x = Vector3.Dot(d, CameraRight) / z * P00, y = Vector3.Dot(d, CameraUp) / z * P11;
        view = new Vector2((x * 0.5f + 0.5f) * Size.X, (0.5f - y * 0.5f) * Size.Y);
        return true;
    }
}

/// <summary>The editor's scene view for level tools.</summary>
public static unsafe class SceneViewport
{
    private static readonly ViewportInput s_input = new();
    private static string s_hint = "";

    /// <summary>This frame's input (the same for every tool).</summary>
    public static ViewportInput Input => s_input;

    /// <summary>Whether a modal operation holds the view (its clicks and keys).</summary>
    public static void Capture(bool on, bool wheel = false)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l != null)
            l->viewportCapture(l->context, on ? (wheel ? 3 : 1) : 0);
    }

    /// <summary>Text for the key hints bar under the view ("": the mode's own keys).</summary>
    public static void Hint(string text)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null || text == s_hint)
            return;
        s_hint = text;
        byte[] b = Encoding.UTF8.GetBytes(text + "\0");
        fixed (byte* p = b)
            l->viewportHint(l->context, p);
    }

    /// <summary>(Level API v9) The commands' state for the Level window (JSON): false when there is no
    /// editor to tell.</summary>
    internal static bool ToolState(string json)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return false;
        byte[] b = Encoding.UTF8.GetBytes(json + "\0");
        fixed (byte* p = b)
            l->toolState(l->context, p);
        return true;
    }

    /// <summary>Once an editor frame, before tools update (Falcon.Editor calls it).</summary>
    internal static void Refresh()
    {
        FxLevelApi* l = LevelNative.Api;
        FxViewportState s;
        s_input.Keys.Clear();
        if (l == null || l->viewport(l->context, &s) == 0)
        {
            s_input.Pressed = s_input.Released = 0;
            s_input.Wheel = 0;
            return;
        }
        ViewportInput i = s_input;
        i.Mouse = new Vector2(s.mouse[0], s.mouse[1]);
        i.Size = new Vector2(s.size[0], s.size[1]);
        i.RayOrigin = new Vector3(s.rayOrigin[0], s.rayOrigin[1], s.rayOrigin[2]);
        i.RayDirection = new Vector3(s.rayDir[0], s.rayDir[1], s.rayDir[2]);
        i.CameraPosition = new Vector3(s.cameraPos[0], s.cameraPos[1], s.cameraPos[2]);
        i.CameraForward = new Vector3(s.cameraForward[0], s.cameraForward[1], s.cameraForward[2]);
        i.CameraRight = new Vector3(s.cameraRight[0], s.cameraRight[1], s.cameraRight[2]);
        i.CameraUp = new Vector3(s.cameraUp[0], s.cameraUp[1], s.cameraUp[2]);
        i.P00 = s.p00;
        i.P11 = s.p11;
        i.Buttons = s.buttons;
        i.Pressed = s.pressed;
        i.Released = s.released;
        i.Shift = (s.mods & 1) != 0;
        i.Ctrl = (s.mods & 2) != 0;
        i.Alt = (s.mods & 4) != 0;
        i.Wheel = s.wheel;
        i.Inside = s.inside != 0;
        i.Captured = s.captured != 0;
        i.Element = (ElementMode)s.elementMode;
        for (int k = 0; k < s.keyCount && k < 16; ++k)
            i.Keys.Add((Key)s.keys[k]);
    }
}

/// <summary>The level tools' element selection: the selected level mesh's chosen vertices (their
/// indices), edges (<see cref="EdgeKey"/>) or faces (their indices).</summary>
public static unsafe class Elements
{
    public static ulong EdgeKey(int a, int b) => a < b ? (ulong)(uint)a << 32 | (uint)b : (ulong)(uint)b << 32 | (uint)a;
    public static (int A, int B) EdgeOf(ulong key) => ((int)(key >> 32), (int)(key & 0xffffffff));

    /// <summary>The mode, the level mesh's entity (default: none) and the chosen items.</summary>
    public static ElementMode Get(out Entity entity, List<ulong> items)
    {
        items.Clear();
        entity = default;
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return ElementMode.Object;
        ulong e;
        uint mode;
        int n = l->elementsGet(l->context, &e, &mode, null, 0);
        if (n > 0)
        {
            ulong[] buffer = new ulong[n];
            fixed (ulong* p = buffer)
                n = l->elementsGet(l->context, &e, &mode, p, n);
            for (int k = 0; k < n && k < buffer.Length; ++k)
                items.Add(buffer[k]);
        }
        entity = new Entity(e);
        return (ElementMode)mode;
    }

    /// <summary>The chosen elements replaced (the entity selected, its mode on).</summary>
    public static void Set(Entity entity, ElementMode mode, IReadOnlyList<ulong> items)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return;
        ulong[] buffer = new ulong[items.Count];
        for (int k = 0; k < buffer.Length; ++k)
            buffer[k] = items[k];
        fixed (ulong* p = buffer)
            l->elementsSet(l->context, entity.Id, (uint)mode, p, buffer.Length);
    }

    /// <summary>The vertices the chosen elements are made of.</summary>
    public static HashSet<int> Vertices(PolyMesh mesh, ElementMode mode, IEnumerable<ulong> items)
    {
        var out_ = new HashSet<int>();
        foreach (ulong item in items)
        {
            if (mode == ElementMode.Vertex)
                out_.Add((int)item);
            else if (mode == ElementMode.Edge)
            {
                (int a, int b) = EdgeOf(item);
                out_.Add(a);
                out_.Add(b);
            }
            else if (mode == ElementMode.Face && (int)item < mesh.Faces.Count)
            {
                Face f = mesh.Faces[(int)item];
                for (int k = 0; k < f.Count; ++k)
                    out_.Add(mesh.Corners[f.First + k]);
            }
        }
        out_.RemoveWhere(v => v < 0 || v >= mesh.Positions.Count);
        return out_;
    }
}

/// <summary>
/// A modal operation (Blender's): started by a command, it holds the scene view; moving the mouse
/// changes its amount, X / Y / Z lock an axis (Shift+X keeps the other two), typed numbers set the
/// amount exactly (Backspace, '-', '.'), Shift slows it (once let go, when it was held as it began:
/// a chord menu's Shift), Ctrl snaps it, a click or Enter confirms,
/// right-click (a click: a right drag flies the camera) or Esc cancels; the building tools' right-click
/// finishes them (RightClickFinishes). Subclasses say what the amount does (Apply) and how to undo it
/// (Cancel). Start one with <see cref="Begin"/>; tools update it each frame (<see cref="Update"/>).
/// </summary>
public abstract class ModalOperation
{
    private static ModalOperation? s_active;
    private Vector2 _mouse0;
    private string _typed = "";
    private bool _fresh;  // its first frame: this frame's input started it (a click, Enter in the palette)
    private bool _shiftLatched;  // Shift held when it started (a chord menu's): not "slow" until let go once
    private Vector2 _rightAt;
    private long _rightTime = -1;  // a right press (ms): a click when let go soon and near, not a camera drag

    /// <summary>The operation in progress, if any.</summary>
    public static ModalOperation? Active => s_active;
    /// <summary>Its name on the hints bar ("Extrude").</summary>
    public abstract string Name { get; }
    /// <summary>The amount now: from the mouse (pixels times Sensitivity), or typed.</summary>
    public float Amount { get; private set; }
    /// <summary>0: free; 1..3: X, Y, Z; -1..-3: the plane without that axis (Shift+X..Z).</summary>
    public int AxisLock { get; private set; }
    /// <summary>World units per pixel of mouse movement (Shift: a tenth).</summary>
    public virtual float Sensitivity => 0.01f;
    /// <summary>The step Ctrl snaps the amount to.</summary>
    public virtual float Snap => 0.25f;
    /// <summary>Whether the amount was typed (not from the mouse).</summary>
    protected bool Typed => _typed.Length > 0;
    /// <summary>Shift held, not counting a Shift held since it began (a chord menu's) until let go.</summary>
    protected bool ShiftHeld => SceneViewport.Input.Shift && !_shiftLatched;
    /// <summary>Whether the mouse wheel is its own while it holds the view (OnWheel), not a zoom.</summary>
    public virtual bool WantsWheel => false;
    /// <summary>More on the hints bar after the amount (" · 3 segments").</summary>
    protected virtual string Details => "";
    /// <summary>Its own keys on the hints bar ("wheel segments · C curve · ").</summary>
    protected virtual string KeyHints => "";
    /// <summary>The command that started it (its settings shown in the Level window while it runs).</summary>
    internal virtual string? Menu => null;
    /// <summary>Whether the hints bar shows the amount (tools that only take clicks: no).</summary>
    protected virtual bool ShowsAmount => true;
    /// <summary>Whether a right-click finishes it, making what is drawn so far (the building tools: a
    /// wall's corners), rather than cancelling it.</summary>
    protected virtual bool RightClickFinishes => false;
    /// <summary>The keys on the hints bar: by default the amount's (the mouse, typing, axes).</summary>
    protected virtual string Keys =>
        $"move the mouse or type a number · {KeyHints}X Y Z axis · Shift slow · Ctrl snap · click / Enter: done · right-click / Esc: cancel";

    /// <summary>Starts it: the view is held until it ends.</summary>
    public static void Begin(ModalOperation op)
    {
        s_active?.End(false);
        s_active = op;
        op._mouse0 = SceneViewport.Input.Mouse;
        op._fresh = true;
        op._shiftLatched = SceneViewport.Input.Shift;
        SceneViewport.Capture(true, op.WantsWheel);
        op.OnBegin();
        op.ShowHint();
    }

    /// <summary>Each editor frame (LevelTools' tool does it): input in, Apply, maybe the end.</summary>
    public static void Update()
    {
        ModalOperation? op = s_active;
        if (op == null)
            return;
        if (op._fresh)
        {
            op._fresh = false;
            return;
        }
        ViewportInput i = SceneViewport.Input;
        if (!i.Shift)
            op._shiftLatched = false;
        bool shift = i.Shift && !op._shiftLatched;
        foreach (Key k in i.Keys)
        {
            if (op.OnKey(k, shift))
                continue;
            switch (k)
            {
            case Key.Escape: op.End(false); return;
            case Key.Return:
            case Key.KpEnter: op.End(true); return;
            case Key.X: op.AxisLock = shift ? -1 : op.AxisLock == 1 ? 0 : 1; break;
            case Key.Y: op.AxisLock = shift ? -2 : op.AxisLock == 2 ? 0 : 2; break;
            case Key.Z: op.AxisLock = shift ? -3 : op.AxisLock == 3 ? 0 : 3; break;
            case Key.Backspace: if (op._typed.Length > 0) op._typed = op._typed[..^1]; break;
            case Key.Minus:
            case Key.KpMinus: op._typed = op._typed.StartsWith('-') ? op._typed[1..] : "-" + op._typed; break;
            case Key.Period:
            case Key.KpPeriod: if (!op._typed.Contains('.')) op._typed += "."; break;
            default:
                if (k >= Key.D1 && k <= Key.D0)
                    op._typed += k == Key.D0 ? "0" : ((int)(k - Key.D1) + 1).ToString(CultureInfo.InvariantCulture);
                break;
            }
        }
        if (i.RightPressed)
        {
            op._rightAt = i.Mouse;
            op._rightTime = Environment.TickCount64;
        }
        if ((i.Released & 2) != 0 && op._rightTime >= 0)
        {
            bool click = Environment.TickCount64 - op._rightTime < 400 && Vector2.Distance(i.Mouse, op._rightAt) < 6f;
            op._rightTime = -1;
            if (click)
            {
                op.End(op.RightClickFinishes);
                return;
            }
        }
        if (i.Wheel != 0 && op.WantsWheel)
            op.OnWheel(i.Wheel);
        float amount;
        if (op._typed.Length > 0 && float.TryParse(op._typed, NumberStyles.Float, CultureInfo.InvariantCulture, out float typed))
            amount = typed;
        else
        {
            Vector2 d = i.Mouse - op._mouse0;
            amount = (d.X - d.Y) * op.Sensitivity * (shift ? 0.1f : 1f);
            if (i.Ctrl && op.Snap > 0)
                amount = MathF.Round(amount / op.Snap) * op.Snap;
        }
        op.Amount = amount;
        op.Apply(amount);
        op.ShowHint();
        if (i.LeftPressed && !op.OnClick())
            op.End(true);
    }

    /// <summary>The axis its amount moves along, given its natural direction (a face's normal):
    /// the locked world axis when there is one.</summary>
    public Vector3 Direction(Vector3 natural) => AxisLock switch
    {
        1 => Vector3.UnitX,
        2 => Vector3.UnitY,
        3 => Vector3.UnitZ,
        _ => natural,
    };

    private void ShowHint()
    {
        string axis = AxisLock switch { 1 => " along X", 2 => " along Y", 3 => " along Z", -1 => " not X", -2 => " not Y", -3 => " not Z", _ => "" };
        string typed = _typed.Length > 0 ? $" [{_typed}]" : "";
        string head = ShowsAmount ? $"{Name}: {Amount.ToString("0.###", CultureInfo.InvariantCulture)}{axis}{typed}{Details}" : $"{Name}{Details}";
        SceneViewport.Hint($"{head} · {Keys}");
    }

    private void End(bool confirm)
    {
        if (s_active != this)
            return;
        s_active = null;
        SceneViewport.Capture(false);
        SceneViewport.Hint("");
        if (confirm)
            OnConfirm(Amount);
        else
            OnCancel();
    }

    protected virtual void OnBegin() { }
    /// <summary>A key of its own (before the axis and number keys): true when it took it.</summary>
    protected virtual bool OnKey(Key key, bool shift) => false;
    /// <summary>The mouse wheel turned (notches; up positive), when it <see cref="WantsWheel"/>.</summary>
    protected virtual void OnWheel(float notches) { }
    /// <summary>A click in the view: true keeps it going (a step of its own, like a shape's next
    /// corner); false (the default) confirms it.</summary>
    protected virtual bool OnClick() => false;
    /// <summary>Shows the amount (live: no undo step).</summary>
    protected abstract void Apply(float amount);
    /// <summary>Done: the result as one undo step.</summary>
    protected abstract void OnConfirm(float amount);
    /// <summary>Cancelled: back to how it was.</summary>
    protected abstract void OnCancel();
}
