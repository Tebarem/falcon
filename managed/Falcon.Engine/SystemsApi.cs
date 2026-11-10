using System;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using Falcon.Native;

namespace Falcon;

// (API v25) The rest of the engine's systems: animation read back and clips over the graph, the
// terrain, water surfaces, particle and decal components as data, the UI's state.

internal static unsafe class NativeText
{
    /// <summary>A string a native call copies (its length; negative: the buffer is too small).</summary>
    public static string Read<T>(T state, delegate*<T, byte*, int, int> call)
    {
        Span<byte> small = stackalloc byte[256];
        int n;
        fixed (byte* p = small)
            n = call(state, p, small.Length);
        if (n >= 0)
            return Encoding.UTF8.GetString(small[..n]);
        byte[] big = new byte[-n];
        fixed (byte* p = big)
            n = call(state, p, big.Length);
        return Encoding.UTF8.GetString(big, 0, Math.Max(n, 0));
    }

    /// <summary>A string as UTF-8 with its terminating zero.</summary>
    public static byte[] Z(string s)
    {
        byte[] utf8 = new byte[Encoding.UTF8.GetByteCount(s) + 1];
        Encoding.UTF8.GetBytes(s, utf8);
        return utf8;
    }
}

public static unsafe partial class Animation
{
    /// <summary>A parameter's value now (a bool: 0 or 1; set by the game or by the graph's defaults).</summary>
    public static float GetFloat(Character character, int parameter) => Api.Native->animGetFloat(Api.Ctx, character.Id, parameter);

    public static float GetFloat(Character character, string parameter) => GetFloat(character, Parameter(character, parameter));

    public static bool GetBool(Character character, int parameter) => GetFloat(character, parameter) != 0f;

    public static bool GetBool(Character character, string parameter) => GetBool(character, Parameter(character, parameter));

    /// <summary>An id parameter's value as its hash (compare with <see cref="Hash"/>).</summary>
    public static uint GetId(Character character, int parameter) => Api.Native->animGetId(Api.Ctx, character.Id, parameter);

    /// <summary>An id's hash, as the graph keeps ids (FNV-1a of its UTF-8).</summary>
    public static uint Hash(string id)
    {
        uint h = 2166136261u;
        foreach (byte b in Encoding.UTF8.GetBytes(id))
            h = (h ^ b) * 16777619u;
        return h;
    }

    /// <summary>Whether a state machine of the character's graph (one playing now) is in the state
    /// of that name ("Jump"). A graph cooked into the scene's pak before states carried their names
    /// answers false; the project's .fgraph files always know them.</summary>
    public static bool InState(Character character, string state)
    {
        byte[] utf8 = NativeText.Z(state);
        fixed (byte* p = utf8)
            return Api.Native->animInState(Api.Ctx, character.Id, p) != 0;
    }

    /// <summary>The active state of the n-th state machine playing now (outer machines first),
    /// empty when there is none (or a pak's graph, which has no names).</summary>
    public static string StateName(Character character, int machine = 0) =>
        NativeText.Read((character.Id, machine), &StateNameOf);

    private static int StateNameOf((ulong Id, int Machine) s, byte* buffer, int capacity) =>
        Api.Native->animStateName(Api.Ctx, s.Id, s.Machine, buffer, capacity);

    /// <summary>How fast a character's animation runs (1: as authored, 0 holds it), on top of
    /// Time.Scale. Stop puts it back to 1.</summary>
    public static float GetRate(Character character) => Api.Native->animRate(Api.Ctx, character.Id, 0f, 0);

    public static void SetRate(Character character, float rate) => Api.Native->animRate(Api.Ctx, character.Id, MathF.Max(rate, 0f), 1);

    /// <summary>Plays a clip (by name, of the character's skeleton) over its graph: fading in over
    /// <paramref name="fade"/> seconds, at <paramref name="speed"/>, looping or once (then fading back
    /// to the graph by itself). The graph keeps running beneath, so it is where it should be after.
    /// False when the character has no such clip.</summary>
    public static bool Play(Character character, string clip, float speed = 1f, float fade = 0.2f, bool loop = false)
    {
        byte[] utf8 = NativeText.Z(clip);
        fixed (byte* p = utf8)
            return Api.Native->animPlay(Api.Ctx, character.Id, p, speed, fade, loop ? 1 : 0) != 0;
    }

    /// <summary>Fades a clip <see cref="Play"/> started back to the graph.</summary>
    public static void StopClip(Character character, float fade = 0.2f) =>
        Api.Native->animPlay(Api.Ctx, character.Id, null, 1f, fade, 0);

    /// <summary>Seconds into the clip <see cref="Play"/> started (-1: none, or it is fading out).</summary>
    public static float ClipTime(Character character) => Api.Native->animPlaying(Api.Ctx, character.Id);

    public static bool IsPlayingClip(Character character) => ClipTime(character) >= 0f;

    /// <summary>The root motion of the character's last update, in its own space: how far its clips
    /// moved and turned it. The engine doesn't move the entity by it: a script applies it (turned
    /// into the world by the entity's rotation), through a CharacterController or the transform.</summary>
    public static (Vector3 Translation, Quaternion Rotation) RootMotion(Character character)
    {
        float* m = stackalloc float[7];
        if (Api.Native->animRootMotion(Api.Ctx, character.Id, m) == 0)
            return (Vector3.Zero, Quaternion.Identity);
        return (new Vector3(m[0], m[1], m[2]), new Quaternion(m[3], m[4], m[5], m[6]));
    }
}

/// <summary>The terrain at a point: its height and slope, and its material there (two layers by
/// index in the terrain's layers, and how much of the second shows).</summary>
public readonly record struct TerrainSample(float Height, Vector3 Normal, int Layer, int LayerB, float Blend)
{
    /// <summary>The layer that shows most.</summary>
    public int Dominant => Blend > 0.5f ? LayerB : Layer;
}

/// <summary>The scene's heightfield terrain (none: every query answers false or NaN).</summary>
public static unsafe class Terrain
{
    /// <summary>The terrain under (or over) a point's x, z; false where there is no terrain.</summary>
    public static bool Sample(Vector3 position, out TerrainSample sample)
    {
        FxTerrainSample s;
        if (Api.Native->terrainSample(Api.Ctx, (float*)&position, &s) == 0)
        {
            sample = default;
            return false;
        }
        sample = new TerrainSample(s.height, new Vector3(s.normalX, s.normalY, s.normalZ), s.layer, s.layerB, s.blend);
        return true;
    }

    /// <summary>The terrain's height at (x, z), or NaN off it.</summary>
    public static float HeightAt(float x, float z) => Sample(new Vector3(x, 0f, z), out TerrainSample s) ? s.Height : float.NaN;

    /// <summary>The nearest hit of a ray with the terrain (the terrain only: Physics.Raycast sees
    /// it as the ground too, with everything else).</summary>
    public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out float distance)
    {
        float t = 0f;
        bool hit = Api.Native->terrainRaycast(Api.Ctx, (float*)&origin, (float*)&direction, maxDistance, &t) != 0;
        distance = t;
        return hit;
    }

    /// <summary>A material layer's name ("Grass"), empty when there is none.</summary>
    public static string LayerName(int layer) => NativeText.Read(layer, &LayerNameOf);

    private static int LayerNameOf(int layer, byte* buffer, int capacity) =>
        Api.Native->terrainLayerName(Api.Ctx, layer, buffer, capacity);
}

public static unsafe partial class Water
{
    /// <summary>The water body an entity is (-1: none): for <see cref="NearestSurface"/>.</summary>
    public static int BodyOf(Entity entity) => Api.Native->waterBody(Api.Ctx, entity.Id);

    /// <summary>The point of a body's still surface nearest a point (inside its outline: right
    /// below or above it; an ocean: always); false for no such body.</summary>
    public static bool NearestSurface(int body, Vector3 position, out Vector3 surface)
    {
        Vector3 s;
        bool ok = Api.Native->waterNearestSurface(Api.Ctx, body, (float*)&position, (float*)&s) != 0;
        surface = s;
        return ok;
    }
}

public static unsafe partial class Ui
{
    /// <summary>The game's page has the pointer (it is over a control): leave the click to it.</summary>
    public static bool HasPointer => (Api.Native->uiState(Api.Ctx) & 1u) != 0;

    /// <summary>The game's page has the keyboard (a text field has focus): don't read keys as play.</summary>
    public static bool HasKeyboard => (Api.Native->uiState(Api.Ctx) & 2u) != 0;
}

public static unsafe partial class Particles
{
    /// <summary>Whether an entity has a particle or decal component, by name ("ParticleEffect",
    /// "ParticleConeEmitter", "Decal"...) or its key in the scene file ("particleEffect").</summary>
    public static bool HasComponent(Entity entity, string component) => ComponentOp(entity, component, 0);

    /// <summary>Adds one with its defaults (true: it is there now).</summary>
    public static bool AddComponent(Entity entity, string component) => ComponentOp(entity, component, 1);

    public static void RemoveComponent(Entity entity, string component) => ComponentOp(entity, component, 2);

    private static bool ComponentOp(Entity entity, string component, int op)
    {
        byte[] utf8 = NativeText.Z(component);
        fixed (byte* p = utf8)
            return Api.Native->vfxComponent(Api.Ctx, entity.Id, p, op) != 0;
    }

    /// <summary>A field of a particle or decal component as JSON, as the scene file has it
    /// ("rate": 20, "color": [1, 0.5, 0, 1]); null when the entity has no such component or field.</summary>
    public static string? GetField(Entity entity, string component, string field) => Field(entity, component, field, null);

    /// <summary>Sets a field from JSON (a number, true, [r, g, b, a], "path.fmat"...); false when
    /// there is no such component or field on the entity or the value doesn't fit it.</summary>
    public static bool SetField(Entity entity, string component, string field, string json) =>
        Field(entity, component, field, json) != null;

    public static bool SetField(Entity entity, string component, string field, float value) =>
        SetField(entity, component, field, value.ToString(System.Globalization.CultureInfo.InvariantCulture));

    public static bool SetField(Entity entity, string component, string field, bool value) =>
        SetField(entity, component, field, value ? "true" : "false");

    public static bool SetField(Entity entity, string component, string field, Vector4 color)
    {
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        return SetField(entity, component, field,
                        $"[{color.X.ToString(ci)},{color.Y.ToString(ci)},{color.Z.ToString(ci)},{color.W.ToString(ci)}]");
    }

    /// <summary>A number field's value (NaN: none).</summary>
    public static float GetFloat(Entity entity, string component, string field) =>
        float.TryParse(GetField(entity, component, field), System.Globalization.NumberStyles.Float,
                       System.Globalization.CultureInfo.InvariantCulture, out float v)
            ? v
            : float.NaN;

    private static string? Field(Entity entity, string component, string field, string? json)
    {
        byte[] c = NativeText.Z(component), f = NativeText.Z(field);
        byte[]? j = json == null ? null : NativeText.Z(json);
        Span<byte> small = stackalloc byte[256];
        fixed (byte* pc = c, pf = f, pj = j)
        {
            int n;
            fixed (byte* p = small)
                n = Api.Native->vfxField(Api.Ctx, entity.Id, pc, pf, pj, p, small.Length);
            if (n == -1)  // (no such component or field; a value is never 256 bytes short)
                return null;
            if (n >= 0)
                return Encoding.UTF8.GetString(small[..n]);
            byte[] big = new byte[-n];
            fixed (byte* p = big)
                n = Api.Native->vfxField(Api.Ctx, entity.Id, pc, pf, null, p, big.Length);  // (set already)
            return n < 0 ? null : Encoding.UTF8.GetString(big, 0, n);
        }
    }
}

public static unsafe partial class Scene
{
    /// <summary>The scene playing: its path in the project ("Scenes/Main.fpak"; empty without a project).</summary>
    public static string Current => NativeText.Read(0, &CurrentOf);

    private static int CurrentOf(int _, byte* buffer, int capacity) => Api.Native->sceneCurrent(Api.Ctx, buffer, capacity);

    /// <summary>Plays another scene of the project (its path, as <see cref="Current"/>) in this
    /// one's place, after this frame: the game goes on (Game.OnSceneUnloading, then OnSceneLoaded),
    /// the scenes' components end and start with them. In the editor, Play stops instead (the
    /// editor plays the scene it has open). False when there is no such scene.</summary>
    public static bool Load(string scene)
    {
        byte[] utf8 = NativeText.Z(scene);
        fixed (byte* p = utf8)
            return Api.Native->sceneLoad(Api.Ctx, p) != 0;
    }

    /// <summary>Loads another scene of the project in the background (cooked first if it needs it,
    /// then read) while this one keeps running: show a loading screen (the game's UI page, the HUD)
    /// and watch <see cref="IsLoading"/>. Once it is read it takes this one's place after that
    /// frame, as <see cref="Load"/> does. False when there is no such scene. A Load meanwhile wins.</summary>
    public static bool LoadAsync(string scene)
    {
        byte[] utf8 = NativeText.Z(scene);
        fixed (byte* p = utf8)
            return Api.Native->sceneLoadAsync(Api.Ctx, p) != 0;
    }

    /// <summary>A LoadAsync is under way (until the next scene is in place).</summary>
    public static bool IsLoading => Api.Native->sceneLoading(Api.Ctx, null, 0) >= 0f;

    /// <summary>How far it is, roughly (0..1; 0 when nothing loads): a cook's share of its work,
    /// then reading, then ready.</summary>
    public static float LoadProgress => MathF.Max(Api.Native->sceneLoading(Api.Ctx, null, 0), 0f);

    /// <summary>What it is doing: "cooking: meshes", "reading", "ready" ("" when nothing loads).</summary>
    public static string LoadStage
    {
        get
        {
            byte* stage = stackalloc byte[128];
            Api.Native->sceneLoading(Api.Ctx, stage, 128);
            return Marshal.PtrToStringUTF8((nint)stage) ?? "";
        }
    }
}

/// <summary>(API v29) A material of the scene: a material asset's (<see cref="Find"/>) or an
/// entity's own copy (<see cref="Renderer.MaterialInstance"/>). Changing it changes every user of
/// it, from the next frame; the editor's Stop puts every material back as it was.</summary>
public readonly unsafe record struct Material(int Index)
{
    /// <summary>A material asset (its path in the project: "Assets/Brick.fmat"); invalid when this
    /// scene has none of it.</summary>
    public static Material Find(string source)
    {
        byte[] utf8 = NativeText.Z(source);
        fixed (byte* p = utf8)
            return new Material(Api.Native->materialFind(Api.Ctx, p));
    }

    public bool IsValid => Index >= 0;

    /// <summary>A parameter by name (baseColor, emissive, emissiveStrength, roughness, metallic,
    /// normalScale, occlusionStrength, alphaCutoff), up to 4 values.</summary>
    public Vector4 Get(string name)
    {
        Vector4 v = default;
        Param(name, &v, 0);
        return v;
    }

    /// <summary>Sets a parameter by name; false for no such material or parameter.</summary>
    public bool Set(string name, Vector4 value) => Param(name, &value, 1) > 0;

    private int Param(string name, Vector4* v, int set)
    {
        Span<byte> utf8 = stackalloc byte[32];
        int n = Encoding.UTF8.GetBytes(name, utf8[..31]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->materialParam(Api.Ctx, Index, p, (float*)v, set);
    }

    public Vector4 BaseColor { get => Get("baseColor"); set => Set("baseColor", value); }

    /// <summary>The emitted colour (its strength apart: <see cref="EmissiveStrength"/>).</summary>
    public Vector3 Emissive
    {
        get { Vector4 v = Get("emissive"); return new Vector3(v.X, v.Y, v.Z); }
        set => Set("emissive", new Vector4(value, 0f));
    }

    public float EmissiveStrength { get => Get("emissiveStrength").X; set => Set("emissiveStrength", new Vector4(value)); }
    public float Roughness { get => Get("roughness").X; set => Set("roughness", new Vector4(value)); }
    public float Metallic { get => Get("metallic").X; set => Set("metallic", new Vector4(value)); }
    public float NormalScale { get => Get("normalScale").X; set => Set("normalScale", new Vector4(value)); }
    public float OcclusionStrength { get => Get("occlusionStrength").X; set => Set("occlusionStrength", new Vector4(value)); }
    public float AlphaCutoff { get => Get("alphaCutoff").X; set => Set("alphaCutoff", new Vector4(value)); }

    /// <summary>(R4) Its surface shader's four values (SurfaceIn.params / LightIn.params).</summary>
    public Vector4 Params { get => Get("params"); set => Set("params", value); }
}

/// <summary>Where a <see cref="PostEffect"/> runs: on the linear HDR frame before the exposure,
/// bloom and tonemap, or on the display frame after it (before FXAA).</summary>
public enum PostStage
{
    BeforeTonemap = 0,
    AfterTonemap = 1,
}

/// <summary>(API v30) A post effect: a compute shader of the project's (its path,
/// "Shaders/Grey.slang", compiled by the editor into Cache/Shaders; built on falcon_post.slang)
/// run on the frame every frame while enabled, in <see cref="Order"/> (lower first). Its uniform
/// block's fields are set by name. The editor's Stop removes the game's effects.</summary>
public sealed unsafe class PostEffect : IDisposable
{
    private uint _handle;
    private bool _enabled = true;
    private int _order;

    public PostEffect(string shader, PostStage stage = PostStage.BeforeTonemap, int order = 0)
    {
        _order = order;
        byte[] utf8 = NativeText.Z(shader);
        fixed (byte* p = utf8)
            _handle = Api.Native->postCreate(Api.Ctx, p, (int)stage, order);
    }

    /// <summary>False when the project has no such compiled compute shader (see the Console).</summary>
    public bool IsValid => _handle != 0;

    public bool Enabled
    {
        get => _enabled;
        set
        {
            _enabled = value;
            if (_handle != 0)
                Api.Native->postState(Api.Ctx, _handle, value ? 1 : 0, _order);
        }
    }

    public int Order
    {
        get => _order;
        set
        {
            _order = value;
            if (_handle != 0)
                Api.Native->postState(Api.Ctx, _handle, _enabled ? 1 : 0, value);
        }
    }

    /// <summary>A field of its uniform block (ints and uints converted); false for no such field.</summary>
    public bool Set(string name, float value) => Set(name, &value, 1);
    public bool Set(string name, Vector2 value) => Set(name, (float*)&value, 2);
    public bool Set(string name, Vector3 value) => Set(name, (float*)&value, 3);
    public bool Set(string name, Vector4 value) => Set(name, (float*)&value, 4);

    private bool Set(string name, float* values, int count)
    {
        if (_handle == 0)
            return false;
        Span<byte> utf8 = stackalloc byte[64];
        int n = Encoding.UTF8.GetBytes(name, utf8[..63]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->postSet(Api.Ctx, _handle, p, values, count) > 0;
    }

    public void Dispose()
    {
        if (_handle != 0)
            Api.Native->postDestroy(Api.Ctx, _handle);
        _handle = 0;
    }
}

/// <summary>A runtime mesh's vertex (48 bytes): position, normal, uv, colour (linear rgba).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MeshVertex
{
    public Vector3 Position;
    public Vector3 Normal;
    public Vector2 Uv;
    public Vector4 Color;

    public MeshVertex(Vector3 position, Vector3 normal, Vector2 uv, Vector4 color)
    {
        Position = position;
        Normal = normal;
        Uv = uv;
        Color = color;
    }

    public MeshVertex(Vector3 position, Vector3 normal) : this(position, normal, Vector2.Zero, Vector4.One) { }
}

/// <summary>(API v31) A mesh made at runtime: vertices and triangles (3 indices each), drawn where
/// and as often as <see cref="Draw"/> is called each frame: lit like the scene (the sun and its
/// shadow, the local lights, the sky), hidden behind it, opaque or (colour alpha under 1)
/// blended, back to front. Not in shadows, GI or reflections. The editor's Stop frees the game's
/// meshes; they carry on across scene loads.</summary>
public sealed unsafe class Mesh : IDisposable
{
    private uint _id;

    public Mesh(ReadOnlySpan<MeshVertex> vertices, ReadOnlySpan<uint> indices)
    {
        fixed (MeshVertex* v = vertices)
        fixed (uint* i = indices)
            _id = Api.Native->meshCreate(Api.Ctx, v, (uint)vertices.Length, i, (uint)indices.Length);
    }

    /// <summary>False when it couldn't be made (no triangles, an index past the vertices).</summary>
    public bool IsValid => _id != 0;

    /// <summary>New vertices and triangles (any counts), shown from the next frame.</summary>
    public bool Update(ReadOnlySpan<MeshVertex> vertices, ReadOnlySpan<uint> indices)
    {
        if (_id == 0)
            return false;
        fixed (MeshVertex* v = vertices)
        fixed (uint* i = indices)
            return Api.Native->meshUpdate(Api.Ctx, _id, v, (uint)vertices.Length, i, (uint)indices.Length) != 0;
    }

    /// <summary>Drawn this frame at a transform, tinted (alpha under 1: see-through), glowing,
    /// with its roughness and metallic.</summary>
    public void Draw(Matrix4x4 world, Vector4 color, Vector4 emissive = default, float roughness = 0.5f, float metallic = 0f)
    {
        if (_id != 0)
            Api.Native->meshDraw(Api.Ctx, _id, (float*)&world, (float*)&color, (float*)&emissive, roughness, metallic);
    }

    public void Draw(Matrix4x4 world) => Draw(world, Vector4.One);

    public void Dispose()
    {
        if (_id != 0)
            Api.Native->meshDestroy(Api.Ctx, _id);
        _id = 0;
    }

    /// <summary>A quad in the XY plane (facing +Z), `size` across.</summary>
    public static Mesh Quad(float size = 1f)
    {
        float h = size * 0.5f;
        Vector3 n = Vector3.UnitZ;
        Span<MeshVertex> v = stackalloc MeshVertex[4]
        {
            new(new Vector3(-h, -h, 0), n, new Vector2(0, 1), Vector4.One),
            new(new Vector3(h, -h, 0), n, new Vector2(1, 1), Vector4.One),
            new(new Vector3(h, h, 0), n, new Vector2(1, 0), Vector4.One),
            new(new Vector3(-h, h, 0), n, new Vector2(0, 0), Vector4.One),
        };
        Span<uint> i = stackalloc uint[6] { 0, 1, 2, 0, 2, 3 };
        return new Mesh(v, i);
    }
}
