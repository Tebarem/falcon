using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Falcon.Native;

namespace Falcon;

public unsafe partial struct WorldTransform
{
    /// <summary>Largest axis scale of the transform.</summary>
    public readonly float MaxScale => System.MathF.Sqrt(System.MathF.Max(
        rows[0] * rows[0] + rows[4] * rows[4] + rows[8] * rows[8],
        System.MathF.Max(rows[1] * rows[1] + rows[5] * rows[5] + rows[9] * rows[9],
                         rows[2] * rows[2] + rows[6] * rows[6] + rows[10] * rows[10])));

    /// <summary>A mesh-space point transformed to world space.</summary>
    public readonly Vector3 TransformPoint(Vector3 p) => new(
        rows[0] * p.X + rows[1] * p.Y + rows[2] * p.Z + rows[3],
        rows[4] * p.X + rows[5] * p.Y + rows[6] * p.Z + rows[7],
        rows[8] * p.X + rows[9] * p.Y + rows[10] * p.Z + rows[11]);

    public Vector3 Position
    {
        readonly get => new(rows[3], rows[7], rows[11]);
        set
        {
            rows[3] = value.X;
            rows[7] = value.Y;
            rows[11] = value.Z;
        }
    }
}

/// <summary>Every SDL scancode (a key by where it is on the keyboard, not what it types).</summary>
public enum Key
{
    A = 4,
    B = 5,
    C = 6,
    D = 7,
    E = 8,
    F = 9,
    G = 10,
    H = 11,
    I = 12,
    J = 13,
    K = 14,
    L = 15,
    M = 16,
    N = 17,
    O = 18,
    P = 19,
    Q = 20,
    R = 21,
    S = 22,
    T = 23,
    U = 24,
    V = 25,
    W = 26,
    X = 27,
    Y = 28,
    Z = 29,
    D1 = 30,
    D2 = 31,
    D3 = 32,
    D4 = 33,
    D5 = 34,
    D6 = 35,
    D7 = 36,
    D8 = 37,
    D9 = 38,
    D0 = 39,
    Return = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,
    Minus = 45,
    Equals = 46,
    Leftbracket = 47,
    Rightbracket = 48,
    Backslash = 49,
    Nonushash = 50,
    Semicolon = 51,
    Apostrophe = 52,
    Grave = 53,
    Comma = 54,
    Period = 55,
    Slash = 56,
    Capslock = 57,
    F1 = 58,
    F2 = 59,
    F3 = 60,
    F4 = 61,
    F5 = 62,
    F6 = 63,
    F7 = 64,
    F8 = 65,
    F9 = 66,
    F10 = 67,
    F11 = 68,
    F12 = 69,
    Printscreen = 70,
    Scrolllock = 71,
    Pause = 72,
    Insert = 73,
    Home = 74,
    Pageup = 75,
    Delete = 76,
    End = 77,
    Pagedown = 78,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    Numlockclear = 83,
    KpDivide = 84,
    KpMultiply = 85,
    KpMinus = 86,
    KpPlus = 87,
    KpEnter = 88,
    Kp1 = 89,
    Kp2 = 90,
    Kp3 = 91,
    Kp4 = 92,
    Kp5 = 93,
    Kp6 = 94,
    Kp7 = 95,
    Kp8 = 96,
    Kp9 = 97,
    Kp0 = 98,
    KpPeriod = 99,
    Nonusbackslash = 100,
    Application = 101,
    Power = 102,
    KpEquals = 103,
    F13 = 104,
    F14 = 105,
    F15 = 106,
    F16 = 107,
    F17 = 108,
    F18 = 109,
    F19 = 110,
    F20 = 111,
    F21 = 112,
    F22 = 113,
    F23 = 114,
    F24 = 115,
    Execute = 116,
    Help = 117,
    Menu = 118,
    Select = 119,
    Stop = 120,
    Again = 121,
    Undo = 122,
    Cut = 123,
    Copy = 124,
    Paste = 125,
    Find = 126,
    Mute = 127,
    Volumeup = 128,
    Volumedown = 129,
    KpComma = 133,
    KpEqualsas400 = 134,
    International1 = 135,
    International2 = 136,
    International3 = 137,
    International4 = 138,
    International5 = 139,
    International6 = 140,
    International7 = 141,
    International8 = 142,
    International9 = 143,
    Lang1 = 144,
    Lang2 = 145,
    Lang3 = 146,
    Lang4 = 147,
    Lang5 = 148,
    Lang6 = 149,
    Lang7 = 150,
    Lang8 = 151,
    Lang9 = 152,
    Alterase = 153,
    Sysreq = 154,
    Cancel = 155,
    Clear = 156,
    Prior = 157,
    Return2 = 158,
    Separator = 159,
    Out = 160,
    Oper = 161,
    Clearagain = 162,
    Crsel = 163,
    Exsel = 164,
    Front = 165,
    Kp00 = 176,
    Kp000 = 177,
    Thousandsseparator = 178,
    Decimalseparator = 179,
    Currencyunit = 180,
    Currencysubunit = 181,
    KpLeftparen = 182,
    KpRightparen = 183,
    KpLeftbrace = 184,
    KpRightbrace = 185,
    KpTab = 186,
    KpBackspace = 187,
    KpA = 188,
    KpB = 189,
    KpC = 190,
    KpD = 191,
    KpE = 192,
    KpF = 193,
    KpXor = 194,
    KpPower = 195,
    KpPercent = 196,
    KpLess = 197,
    KpGreater = 198,
    KpAmpersand = 199,
    KpDblampersand = 200,
    KpVerticalbar = 201,
    KpDblverticalbar = 202,
    KpColon = 203,
    KpHash = 204,
    KpSpace = 205,
    KpAt = 206,
    KpExclam = 207,
    KpMemstore = 208,
    KpMemrecall = 209,
    KpMemclear = 210,
    KpMemadd = 211,
    KpMemsubtract = 212,
    KpMemmultiply = 213,
    KpMemdivide = 214,
    KpPlusminus = 215,
    KpClear = 216,
    KpClearentry = 217,
    KpBinary = 218,
    KpOctal = 219,
    KpDecimal = 220,
    KpHexadecimal = 221,
    LeftCtrl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    LeftGui = 227,
    RightCtrl = 228,
    RightShift = 229,
    RightAlt = 230,
    RightGui = 231,
    Mode = 257,
    Sleep = 258,
    Wake = 259,
    ChannelIncrement = 260,
    ChannelDecrement = 261,
    MediaPlay = 262,
    MediaPause = 263,
    MediaRecord = 264,
    MediaFastForward = 265,
    MediaRewind = 266,
    MediaNextTrack = 267,
    MediaPreviousTrack = 268,
    MediaStop = 269,
    MediaEject = 270,
    MediaPlayPause = 271,
    MediaSelect = 272,
    AcNew = 273,
    AcOpen = 274,
    AcClose = 275,
    AcExit = 276,
    AcSave = 277,
    AcPrint = 278,
    AcProperties = 279,
    AcSearch = 280,
    AcHome = 281,
    AcBack = 282,
    AcForward = 283,
    AcStop = 284,
    AcRefresh = 285,
    AcBookmarks = 286,
    Softleft = 287,
    Softright = 288,
    Call = 289,
    Endcall = 290,
    Reserved = 400,
}

public enum MouseButton
{
    Left = 1,
    Right = 2,
    Middle = 4,
}

/// <summary>Keyboard state, read straight from the engine's arrays, and the mouse of this frame
/// (in the editor, what happens over the game's view while it plays).</summary>
public static unsafe partial class Input
{
    public static bool IsDown(Key k) => (int)k < Api.Native->keyCount && Api.Native->keys[(int)k] != 0;
    public static bool Pressed(Key k) => IsDown(k) && Api.Native->keysPrev[(int)k] == 0;
    public static bool Released(Key k) => !IsDown(k) && (int)k < Api.Native->keyCount && Api.Native->keysPrev[(int)k] != 0;

    /// <summary>How far the mouse moved since the last frame (pixels; +Y down).</summary>
    public static Vector2 MouseDelta
    {
        get
        {
            FxMouse m = Mouse();
            return new Vector2(m.dx, m.dy);
        }
    }

    /// <summary>Where the mouse is (window pixels).</summary>
    public static Vector2 MousePosition
    {
        get
        {
            FxMouse m = Mouse();
            return new Vector2(m.x, m.y);
        }
    }

    /// <summary>Wheel notches since the last frame (+ away from the user).</summary>
    public static float MouseWheel => Mouse().wheel;

    public static bool IsDown(MouseButton b) => (Mouse().buttons & (uint)b) != 0;
    public static bool Pressed(MouseButton b)
    {
        FxMouse m = Mouse();
        return (m.buttons & (uint)b) != 0 && (m.buttonsPrev & (uint)b) == 0;
    }
    public static bool Released(MouseButton b)
    {
        FxMouse m = Mouse();
        return (m.buttons & (uint)b) == 0 && (m.buttonsPrev & (uint)b) != 0;
    }

    /// <summary>Hides the cursor and holds it in place (its motion still reported), for looking
    /// around; freed when the game stops.</summary>
    public static bool MouseLocked
    {
        get => Mouse().locked != 0;
        set => Api.Native->mouseLock(Api.Ctx, value ? 1 : 0);
    }

    private static FxMouse Mouse()
    {
        FxMouse m;
        Api.Native->mouseGet(Api.Ctx, &m);
        return m;
    }
}

/// <summary>Read-only data of the loaded scene.</summary>
public static unsafe partial class Scene
{
    /// <summary>Mesh-space bounding sphere of a scene mesh.</summary>
    public static (Vector3 Center, float Radius) MeshBounds(uint mesh)
    {
        Vector3 c;
        float r;
        Api.Native->meshBounds(Api.Ctx, mesh, (float*)&c, &r);
        return (c, r);
    }
}

/// <summary>The engine camera of the current frame.</summary>
public static unsafe partial class Camera
{
    public static Vector3 Position => Get(0);
    public static Vector3 Forward => Get(1);
    public static Vector3 Right => Get(2);
    public static Vector3 Up => Get(3);

    private static Vector3 Get(int which)
    {
        Vector3* v = stackalloc Vector3[4];
        Api.Native->cameraGet(Api.Ctx, (float*)&v[0], (float*)&v[1], (float*)&v[2], (float*)&v[3]);
        return v[which];
    }
}

/// <summary>Animation graph parameters of characters: entities rendering a skinned mesh, or a model
/// placed whole (its root entity, whose Animator drives the meshes below it).</summary>
/// <summary>A character to animate: an entity (its skinned mesh, or a model's root, whose Animator
/// drives the meshes below it), or a handle from <see cref="Of"/>. A handle skips finding the
/// character on every call (about 15 ns): keep one where it is used every frame (a component).
/// Entities convert to it, so every Animation call takes either.</summary>
public readonly unsafe record struct Character(ulong Id)
{
    /// <summary>The character an entity is, as a handle; default when it is none (no skinned mesh on
    /// it or below it). Valid while the scene is loaded.</summary>
    public static Character Of(Entity entity) => new(Api.Native->animCharacter(Api.Ctx, entity.Id));

    /// <summary>Not default: a handle <see cref="Of"/> found, or a (non-zero) entity.</summary>
    public bool IsValid => Id != 0;

    public static implicit operator Character(Entity entity) => new(entity.Id);
}

public static unsafe partial class Animation
{
    /// <summary>Index of a graph control parameter, or -1 (no graph / unknown name).</summary>
    public static int Parameter(Character character, string name)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(name, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->animParameter(Api.Ctx, character.Id, p);
    }

    public static void SetFloat(Character character, int parameter, float value) =>
        Api.Native->animSetFloat(Api.Ctx, character.Id, parameter, value);

    public static void SetBool(Character character, int parameter, bool value) =>
        Api.Native->animSetBool(Api.Ctx, character.Id, parameter, value ? 1 : 0);

    public static void SetId(Character character, int parameter, string id)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(id, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            Api.Native->animSetId(Api.Ctx, character.Id, parameter, p);
    }

    public static void SetFloat(Character character, string parameter, float value) =>
        SetFloat(character, Parameter(character, parameter), value);

    /// <summary>The events of a character's last update (its clips' markers and footsteps, its
    /// states' events: a model's .fevents and its graph), copied into <paramref name="into"/>;
    /// returns how many there were (more than fit: the rest are left out).</summary>
    public static int Events(Character character, Span<AnimEvent> into)
    {
        fixed (AnimEvent* p = into)
            return Api.Native->animEvents(Api.Ctx, character.Id, p, into.Length);
    }

    /// <summary>Hides a character's bone and everything below it (or shows it again): first
    /// person's head. Vertices on hidden bones shrink into the bone's joint; a part on hidden bones
    /// only (a helmet) still casts its shadow. False when the character has no such bone. Stopping
    /// the game shows every bone again.</summary>
    public static bool HideBone(Character character, string bone, bool hidden = true) => HideBone(character, bone, hidden ? 1 : 0) >= 0;

    /// <summary>Whether a character's bone is hidden (it or a bone above it: HideBone).</summary>
    public static bool IsBoneHidden(Character character, string bone) => HideBone(character, bone, -1) == 1;

    /// <summary>Foot IK's weight (0 to 1) over the graph's foot IK: 0 lets the clips' feet be,
    /// in the air. False when the character has no legs to find (foot_l / LeftFoot and so on).</summary>
    public static bool SetFootIk(Character character, float weight) =>
        Api.Native->animFootIk(Api.Ctx, character.Id, weight) != 0;

    /// <summary>Sets an entity parameter of the character's graph (what its IK follows: an aim's
    /// hold, a look's target) to <paramref name="target"/> (default: none), over the Animator's
    /// binding until the game stops or <see cref="ResetEntity"/>. False when the graph has no such
    /// parameter.</summary>
    public static bool SetEntity(Character character, string parameter, Entity target) => EntityParameter(character, parameter, 1, target.Id) != ~0ul;

    /// <summary>The Animator's binding of an entity parameter back (a script's gone). False when the
    /// graph has no such parameter.</summary>
    public static bool ResetEntity(Character character, string parameter) => EntityParameter(character, parameter, 2, 0) != ~0ul;

    /// <summary>The entity an entity parameter follows now (default: none, or no such parameter).</summary>
    public static Entity GetEntity(Character character, string parameter)
    {
        ulong id = EntityParameter(character, parameter, 0, 0);
        return id == ~0ul ? default : new Entity(id);
    }

    static ulong EntityParameter(Character character, string parameter, int op, ulong value)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(parameter, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->animEntity(Api.Ctx, character.Id, p, op, value);
    }

    /// <summary>Turns the character's spine, neck and head (up to <paramref name="tip"/>) toward
    /// <paramref name="target"/> (a world point), at most 75 degrees; call it every frame it looks.
    /// Weight 0 stops. False when the character has no such bone.</summary>
    public static bool LookAt(Character character, Vector3 target, float weight = 1f, string tip = "head")
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(tip, utf8[..255]);
        utf8[n] = 0;
        float* t = stackalloc float[3] { target.X, target.Y, target.Z };
        fixed (byte* p = utf8)
            return Api.Native->animLookAt(Api.Ctx, character.Id, t, weight, p) != 0;
    }

    /// <summary>Looks at an entity (an Empty) where it is now (LookAt); false when it has no
    /// transform.</summary>
    public static bool LookAt(Character character, Entity target, float weight = 1f, string tip = "head") =>
        Ecs.Has<WorldTransform>(target) && LookAt(character, Ecs.Get<WorldTransform>(target).Position, weight, tip);

    /// <summary>Stops looking (LookAt).</summary>
    public static void StopLooking(Character character) => LookAt(character, Vector3.Zero, 0f);

    /// <summary><paramref name="bone"/> (a hand) reaches <paramref name="target"/> (a world point)
    /// with its parent and grandparent (the forearm and upper arm), after the look; the bone keeps its
    /// turn. Weight 0 stops it. Up to 4 bones at once; false when there is no such bone.</summary>
    public static bool Reach(Character character, string bone, Vector3 target, float weight = 1f)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(bone, utf8[..255]);
        utf8[n] = 0;
        float* t = stackalloc float[3] { target.X, target.Y, target.Z };
        fixed (byte* p = utf8)
            return Api.Native->animReach(Api.Ctx, character.Id, p, t, weight) != 0;
    }

    /// <summary>A hand reaches an entity (an Empty) where it is now (Reach); false when it has no
    /// transform.</summary>
    public static bool Reach(Character character, string bone, Entity target, float weight = 1f) =>
        Ecs.Has<WorldTransform>(target) && Reach(character, bone, Ecs.Get<WorldTransform>(target).Position, weight);

    /// <summary>Stops a reach (Reach).</summary>
    public static void StopReaching(Character character, string bone) => Reach(character, bone, Vector3.Zero, 0f);

    /// <summary>Aims <paramref name="bone"/> (the hand holding a weapon) at <paramref name="target"/>
    /// (a world point): the spine it hangs from turns, at most 80 degrees, until the bone's
    /// <paramref name="axis"/> points there, before any look (which then turns the neck and head
    /// only). The axis is in the bone's own space: the weapon's barrel (as the weapon's mesh lies
    /// in the bone's space at bind). Without one, the bone's ahead in the graph's pose (a clip that
    /// aims straight ahead). With <paramref name="hold"/> (a world point) the bone is brought there by
    /// its arm, after the look (the weapon before the eyes), still pointing at the target, and
    /// <paramref name="support"/> (the other hand; null: none) keeps its grip on it. Call it every
    /// frame it aims; weight 0 stops. False when there is no such bone or no spine above it.</summary>
    public static bool AimAt(Character character, Vector3 target, float weight = 1f, string bone = "hand_r", Vector3 axis = default,
                             Vector3? hold = null, string? support = "hand_l")
    {
        Span<byte> utf8 = stackalloc byte[256], supportUtf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(bone, utf8[..255]);
        utf8[n] = 0;
        n = System.Text.Encoding.UTF8.GetBytes(support ?? "", supportUtf8[..255]);
        supportUtf8[n] = 0;
        Vector3 h = hold ?? Vector3.Zero;
        float* t = stackalloc float[3] { target.X, target.Y, target.Z };
        float* a = stackalloc float[3] { axis.X, axis.Y, axis.Z };
        float* at = stackalloc float[3] { h.X, h.Y, h.Z };
        fixed (byte* p = utf8, q = supportUtf8)
            return Api.Native->animAimAt(Api.Ctx, character.Id, t, weight, p, a, hold.HasValue ? at : null, support != null ? q : null) != 0;
    }

    /// <summary>Aims at an entity (an Empty) where it is now (AimAt); false when it has no
    /// transform.</summary>
    public static bool AimAt(Character character, Entity target, float weight = 1f, string bone = "hand_r", Vector3 axis = default,
                             Vector3? hold = null, string? support = "hand_l") =>
        Ecs.Has<WorldTransform>(target) && AimAt(character, Ecs.Get<WorldTransform>(target).Position, weight, bone, axis, hold, support);

    /// <summary>Aims at a point, the weapon held at an entity (an Empty, say under the camera) where
    /// it is now (AimAt's hold); without a transform, not held.</summary>
    public static bool AimAt(Character character, Vector3 target, float weight, string bone, Vector3 axis, Entity hold,
                             string? support = "hand_l") =>
        AimAt(character, target, weight, bone, axis, Ecs.Has<WorldTransform>(hold) ? Ecs.Get<WorldTransform>(hold).Position : (Vector3?)null, support);

    /// <summary>Stops aiming (AimAt).</summary>
    public static void StopAiming(Character character) => AimAt(character, Vector3.Zero, 0f);

    static int HideBone(Character character, string bone, int hidden)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = System.Text.Encoding.UTF8.GetBytes(bone, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->animHideBone(Api.Ctx, character.Id, p, hidden);
    }

    /// <summary>An event's (or a sync event's, an ID parameter's) id: FNV-1a of its name, as the
    /// engine hashes them.</summary>
    public static uint Id(string name)
    {
        uint h = 2166136261u;
        foreach (byte b in System.Text.Encoding.UTF8.GetBytes(name))
            h = (h ^ b) * 16777619u;
        return h;
    }
}

public enum AnimEventKind : byte
{
    Marker = 0,
    Footstep = 1,
    State = 2,
}

/// <summary>An animation event of a character's last update (Animation.Events).</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct AnimEvent
{
    public readonly uint Id;
    /// <summary>How much its branch shows (blends, transitions): 0 .. 1.</summary>
    public readonly float Weight;
    /// <summary>How far through it (events that last), else 1.</summary>
    public readonly float Percent;
    public readonly AnimEventKind Kind;
    private readonly byte _flags;
    private readonly byte _active;
    private readonly byte _pad;
    public bool LeftFoot => (_flags & 1) != 0;
    public bool RightFoot => (_flags & 2) != 0;
    public bool StateEntry => (_flags & 4) != 0;
    public bool StateExit => (_flags & 8) != 0;
    public bool StateFullyIn => (_flags & 16) != 0;
    /// <summary>From the active branch (false: a transition's source).</summary>
    public bool Active => _active != 0;
}

/// <summary>
/// The in-game HTML UI (falcon --ui &lt;bundle&gt;): JSON messages to and from the page, which
/// uses window.falcon.postMessage(json) and window.falcon.onMessage = json =&gt; ... .
/// Without a UI, sends are dropped and polls return nothing.
/// </summary>
public static unsafe partial class Ui
{
    [ThreadStatic] static byte[]? buffer;

    public static void Send(string json)
    {
        int max = System.Text.Encoding.UTF8.GetMaxByteCount(json.Length);
        if (max <= 1024)
        {
            Span<byte> utf8 = stackalloc byte[max];
            int n = System.Text.Encoding.UTF8.GetBytes(json, utf8);
            fixed (byte* p = utf8)
                Api.Native->uiSend(Api.Ctx, p, n);
            return;
        }
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(json);
        fixed (byte* p = bytes)
            Api.Native->uiSend(Api.Ctx, p, bytes.Length);
    }

    /// <summary>Sends a message already in UTF-8: nothing allocated (with Utf8.TryWrite into a
    /// stackalloc'd span, say).</summary>
    public static void Send(ReadOnlySpan<byte> utf8Json)
    {
        fixed (byte* p = utf8Json)
            Api.Native->uiSend(Api.Ctx, p, utf8Json.Length);
    }

    /// <summary>Next message from the page, or false when there is none this frame.</summary>
    public static bool TryReceive(out string json)
    {
        buffer ??= new byte[4096];
        int n;
        fixed (byte* p = buffer)
            n = Api.Native->uiPoll(Api.Ctx, p, buffer.Length);
        if (n < 0)
        {
            buffer = new byte[-n];
            fixed (byte* p = buffer)
                n = Api.Native->uiPoll(Api.Ctx, p, buffer.Length);
        }
        json = n > 0 ? System.Text.Encoding.UTF8.GetString(buffer, 0, n) : string.Empty;
        return n > 0;
    }
}

/// <summary>
/// Frame timing (wall time of this process's frames; averages over the last half second) and
/// CPU, memory and GPU use, sampled twice a second. GPU use is Task Manager's figure: the busiest
/// engine type (3D, compute, copy...). -1: unknown on this system.
/// </summary>
public struct PerformanceStats
{
    public float FrameMs, FrameMsAvg, FrameMsMax, Fps;
    /// <summary>Percent of all cores: this process's threads, and the whole system's.</summary>
    public float CpuProcess, CpuSystem;
    /// <summary>Percent: this process's GPU work, and everyone's.</summary>
    public float GpuProcess, GpuSystem;
    /// <summary>
    /// This process's private memory in physical RAM (Task Manager's Memory column); the system's
    /// in use and installed (MB).
    /// </summary>
    public float RamProcessMB, RamUsedMB, RamTotalMB;
    /// <summary>Dedicated video memory this process uses, and the adapter's (MB).</summary>
    public float VramProcessMB, VramTotalMB;
    /// <summary>All of this process's private memory, committed: part of it may be paged out or never touched (MB).</summary>
    public float RamProcessCommittedMB;
}

public static unsafe class Performance
{
    public static PerformanceStats Current
    {
        get
        {
            PerformanceStats s;
            Api.Native->perfStats(Api.Ctx, &s);
            return s;
        }
    }

    /// <summary>Bytes the game's code has allocated on the managed heap so far (this thread: the
    /// game's). Read it twice around a frame to see what the frame allocated; a steady game
    /// allocates nothing, so the garbage collector never has to pause it.</summary>
    public static long ManagedAllocatedBytes => GC.GetAllocatedBytesForCurrentThread();
}

/// <summary>
/// Text over the game's view (in the editor, while playing): monospace lines at the top left.
/// It stays until shown again or cleared, and goes when the game stops.
/// </summary>
public static unsafe class Hud
{
    public static void Show(string text)
    {
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(text);
        fixed (byte* p = bytes)
            Api.Native->hudSet(Api.Ctx, p, bytes.Length);
    }

    public static void Clear() => Api.Native->hudSet(Api.Ctx, null, 0);
}

/// <summary>The water at a point: which body (-1: none), its surface height, normal and flow there,
/// and how deep the point is below the surface (negative: above it).</summary>
public readonly struct WaterSample
{
    public readonly int Body;
    public readonly float Height;
    public readonly Vector3 Normal;
    public readonly Vector3 Flow;
    public readonly float Depth;

    public WaterSample(int body, float height, Vector3 normal, Vector3 flow, float depth)
    {
        Body = body;
        Height = height;
        Normal = normal;
        Flow = flow;
        Depth = depth;
    }

    public bool InWater => Body >= 0;
}

/// <summary>Water bodies: lakes, pools, the ocean (the same waves the view shows).</summary>
public static unsafe partial class Water
{
    public static WaterSample Sample(Vector3 position)
    {
        FxWaterSample s;
        Api.Native->waterSample(Api.Ctx, (float*)&position, &s);
        return new WaterSample(s.body, s.height, new Vector3(s.normalX, s.normalY, s.normalZ),
                               new Vector3(s.flowX, s.flowY, s.flowZ), s.depth);
    }

    /// <summary>The surface's height over (x, z), or NaN where there is no water.</summary>
    public static float HeightAt(float x, float z)
    {
        WaterSample s = Sample(new Vector3(x, 0f, z));
        return s.InWater ? s.Height : float.NaN;
    }

    public static bool IsUnderwater(Vector3 position) => Sample(position).Depth > 0f;

    /// <summary>Ripples at a point of the water: radius in metres, strength in metres of height.</summary>
    public static void Splash(Vector3 position, float radius, float strength) =>
        Api.Native->waterSplash(Api.Ctx, (float*)&position, radius, strength);
}

/// <summary>What a hit two things made (Physics.Contacts): entities are default for the scene's own
/// geometry and the ground; each side's surface is what it sounds like.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct Contact
{
    public readonly Entity A, B;
    public readonly Vector3 Point, Normal;
    /// <summary>How fast they met, m/s.</summary>
    public readonly float Speed;
    public readonly Surface SurfaceA, SurfaceB;
    private readonly uint _pad;
}

/// <summary>What is under a point (Physics.SurfaceAt): where, how far down, what it sounds like, and
/// whose it is (default: the scene's geometry or the ground).</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct SurfaceHit
{
    public readonly Vector3 Point, Normal;
    public readonly float Distance;
    public readonly Surface Surface;
    public readonly Entity Entity;
}

public enum BodyShape : uint
{
    Box = 0,
    Sphere = 1,
}

/// <summary>Dynamic bodies (Box3D, pooled; the oldest body is recycled when a pool is full).</summary>
public static unsafe partial class Physics
{
    /// <summary>Spawns a body. size: half extents for boxes, radius in X for spheres. tint: linear RGB.</summary>
    public static Entity Spawn(BodyShape shape, Vector3 position, Vector3 size, Vector3 velocity, Vector3 tint)
    {
        uint packed = Pack(tint.X) | Pack(tint.Y) << 8 | Pack(tint.Z) << 16 | 0xffu << 24;
        return new Entity(Api.Native->bodySpawn(Api.Ctx, (uint)shape, (float*)&position, (float*)&size,
                                                (float*)&velocity, packed));
    }

    public static void Impulse(Entity body, Vector3 impulse) => Api.Native->bodyImpulse(Api.Ctx, body.Id, (float*)&impulse);

    public static Vector3 Velocity(Entity body)
    {
        Vector3 v;
        Api.Native->bodyVelocity(Api.Ctx, body.Id, (float*)&v);
        return v;
    }

    [ThreadStatic] static Contact[]? contacts;

    /// <summary>
    /// This frame's hits (the last physics update's): what touched what, where, how hard, and what
    /// each side sounds like. The span is valid until the next call.
    /// </summary>
    public static ReadOnlySpan<Contact> Contacts()
    {
        contacts ??= new Contact[64];
        int n;
        fixed (Contact* p = contacts)
            n = Api.Native->contactPoll(Api.Ctx, p, contacts.Length);
        if (n > contacts.Length)
        {
            contacts = new Contact[Math.Max(n, contacts.Length * 2)];
            fixed (Contact* p = contacts)
                n = Api.Native->contactPoll(Api.Ctx, p, contacts.Length);
        }
        return new ReadOnlySpan<Contact>(contacts, 0, Math.Min(n, contacts.Length));
    }

    /// <summary>The surface under a point: a ray straight down, at most maxDistance metres.</summary>
    public static bool SurfaceAt(Vector3 position, out SurfaceHit hit, float maxDistance = 2f)
    {
        SurfaceHit h;
        bool found = Api.Native->surfaceAt(Api.Ctx, (float*)&position, maxDistance, &h) != 0;
        hit = found ? h : default;
        return found;
    }

    /// <summary>The first surface along a ray from <paramref name="from"/> along
    /// <paramref name="direction"/>, at most maxDistance metres (hit.Distance: from the start).</summary>
    public static bool Raycast(Vector3 from, Vector3 direction, out SurfaceHit hit, float maxDistance = 100f)
    {
        SurfaceHit h;
        bool found = Api.Native->raycast(Api.Ctx, (float*)&from, (float*)&direction, maxDistance, &h) != 0;
        hit = found ? h : default;
        return found;
    }

    private static uint Pack(float v) => (uint)(System.Math.Clamp(v, 0f, 1f) * 255f + 0.5f);
}
