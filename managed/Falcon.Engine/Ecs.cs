using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using Falcon.Native;

namespace Falcon;

/// <summary>Names a component in the native world (without a name, or without the attribute, a
/// struct's full name is used). (API v33) A [Component] struct of the game's is also a data
/// component: the editor's Add Component lists it, the Inspector shows its public fields, and the
/// scene saves it with its entities.</summary>
[AttributeUsage(AttributeTargets.Struct)]
public sealed class ComponentAttribute(string? name = null) : Attribute
{
    public string? Name { get; } = name;
}

/// <summary>(API v33) A data component's field as the editor sees it: its name, kind (float,
/// double, int, uint, long, ulong, byte, bool, vec2, vec3, vec4, quat, entity) and offset.</summary>
public readonly record struct DataField(string Name, string Kind, int Offset);

/// <summary>(API v33) The game's data components ([Component] structs), registered by the source
/// generator for the editor.</summary>
public static unsafe class DataComponents
{
    private sealed record Type(string Name, string Label, int Size, int Align, byte[] Defaults, DataField[] Fields);
    private static readonly List<Type> s_types = new();

    public static void Register<T>(string label, DataField[] fields) where T : unmanaged
    {
        T value = new();  // (its defaults: field initializers run by a parameterless constructor)
        var defaults = new byte[sizeof(T)];
        new ReadOnlySpan<byte>(&value, sizeof(T)).CopyTo(defaults);
        s_types.Add(new Type(Ecs.NameOf<T>(), label, sizeof(T), Ecs.AlignOf<T>(), defaults, fields));
    }

    internal static void Forget() => s_types.Clear();  // (hot reload: the new code registers again)

    internal static string Describe()
    {
        var buffer = new System.Buffers.ArrayBufferWriter<byte>(512);
        using (var w = new System.Text.Json.Utf8JsonWriter(buffer))
        {
            w.WriteStartArray();
            foreach (Type t in s_types)
            {
                w.WriteStartObject();
                w.WriteString("name", t.Name);
                w.WriteString("label", t.Label);
                w.WriteNumber("size", t.Size);
                w.WriteNumber("align", t.Align);
                w.WriteString("defaults", Convert.ToHexString(t.Defaults).ToLowerInvariant());
                w.WriteStartArray("fields");
                foreach (DataField f in t.Fields)
                {
                    w.WriteStartObject();
                    w.WriteString("name", f.Name);
                    w.WriteString("kind", f.Kind);
                    w.WriteNumber("offset", f.Offset);
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }
}

/// <summary>An entity of the engine's flecs world.</summary>
public readonly partial record struct Entity(ulong Id)
{
    public bool IsAlive => Ecs.IsAlive(this);
    public bool IsValid => Id != 0;

    /// <summary>The entity with this name (as the editor's Hierarchy names it), or an invalid one
    /// (Id 0) when there is none.</summary>
    public static unsafe Entity Find(string name)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = Encoding.UTF8.GetBytes(name, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return new Entity(Api.Native->entityFind(Api.Ctx, p));
    }

    /// <summary>The entity with this name among <paramref name="under"/>'s descendants (a
    /// character's bones, once its skeleton is expanded: every soldier has a foot_l), or an invalid
    /// one.</summary>
    public static unsafe Entity Find(string name, Entity under)
    {
        Span<byte> utf8 = stackalloc byte[256];
        int n = Encoding.UTF8.GetBytes(name, utf8[..255]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return new Entity(Api.Native->entityFindUnder(Api.Ctx, under.Id, p));
    }
}

/// <summary>
/// The engine's ECS world. Components are plain unmanaged structs, shared by name with the
/// engine (Generated/Components.g.cs) or defined by scripts (registered on first use).
/// </summary>
public static unsafe class Ecs
{
    public static Entity Create() => new(Api.Native->entityCreate(Api.Ctx));
    /// <summary>Destroys an entity and every entity below it (Parent): their meshes stop drawing and
    /// their bodies go. While the editor plays, the scene's own entities are disabled instead, and
    /// Stop brings them back.</summary>
    public static void Destroy(Entity e) => Api.Native->entityDestroy(Api.Ctx, e.Id);
    public static bool IsAlive(Entity e) => Api.Native->entityAlive(Api.Ctx, e.Id) != 0;

    public static void Set<T>(Entity e, in T value) where T : unmanaged
    {
        fixed (T* p = &value)
            Api.Native->entitySet(Api.Ctx, e.Id, Id<T>(), p, (uint)sizeof(T));
    }

    public static bool Has<T>(Entity e) where T : unmanaged => Api.Native->entityGet(Api.Ctx, e.Id, Id<T>()) != null;

    /// <summary>Copy of the component (default when absent). Writes go through Set or a query.</summary>
    public static T Get<T>(Entity e) where T : unmanaged
    {
        void* p = Api.Native->entityGet(Api.Ctx, e.Id, Id<T>());
        return p == null ? default : *(T*)p;
    }

    public static void Remove<T>(Entity e) where T : unmanaged => Api.Native->entityRemove(Api.Ctx, e.Id, Id<T>());

    // Bumped when the world is replaced (a scene loaded in its place): ids are looked up again.
    private static int s_world = 1;

    internal static void WorldChanged() => ++s_world;

    /// <summary>The native component id of T (looked up or registered once per world, then cached).</summary>
    public static ulong Id<T>() where T : unmanaged
    {
        ulong id = Cache<T>.Id;
        return id != 0 && Cache<T>.World == s_world ? id : Cache<T>.Resolve();
    }

    // Per component type. For script-defined structs the cache lives with the game's (collectible)
    // code, so a hot reload looks the component up again by name.
    private static class Cache<T> where T : unmanaged
    {
        public static ulong Id;
        public static int World;

        public static ulong Resolve()
        {
            string name = NameOf<T>();
            int n = Encoding.UTF8.GetByteCount(name);
            byte* utf8 = stackalloc byte[n + 1];
            Encoding.UTF8.GetBytes(name, new Span<byte>(utf8, n));
            utf8[n] = 0;
            ulong id = Api.Native->componentId(Api.Ctx, utf8, (uint)sizeof(T), (uint)AlignOf<T>());
            if (id == 0)
                throw new InvalidOperationException($"component {name}: layout differs from the native world");
            Id = id;
            World = s_world;
            return id;
        }
    }

    private struct AlignProbe<T> where T : unmanaged
    {
#pragma warning disable CS0649  // layout probe, never written
        public byte Pad;
#pragma warning restore CS0649
        public T Value;
    }

    /// <summary>T's name in the native world: its [Component] name, else its full name.</summary>
    public static string NameOf<T>() where T : unmanaged =>
        typeof(T).GetCustomAttribute<ComponentAttribute>()?.Name ?? typeof(T).FullName!;

    internal static int AlignOf<T>() where T : unmanaged
    {
        AlignProbe<T> probe = default;
        return (int)Unsafe.ByteOffset(ref Unsafe.As<AlignProbe<T>, byte>(ref probe),
                                      ref Unsafe.As<T, byte>(ref probe.Value));
    }
}

/// <summary>One query term: a component and how the query accesses it.</summary>
/// <summary>One query term: a component and how the query accesses it. Terms made by Query.In /
/// InOut / Not know their component's type, so a query made of them follows the world into the
/// next scene.</summary>
public readonly unsafe struct Term
{
    public ulong Component { get; }
    public FxAccess Access { get; }
    internal readonly delegate*<ulong> Lookup;  // the id in the world now (null: Component as given)

    public Term(ulong component, FxAccess access)
    {
        Component = component;
        Access = access;
        Lookup = null;
    }

    internal Term(delegate*<ulong> lookup, FxAccess access)
    {
        Component = lookup();
        Access = access;
        Lookup = lookup;
    }

    internal Term Again() => Lookup == null ? this : new Term(Lookup, Access);
}

/// <summary>
/// A cached native query. Iterating yields one chunk per table (archetype) with spans straight
/// into the engine's component columns. Writes through InOut terms mark the table changed,
/// which is what the renderer uses to re-stream transforms.
/// </summary>
public sealed unsafe class Query : IDisposable
{
    public const int MaxTerms = 16;
    private static readonly List<Query> s_live = new();

    private nint _handle;
    private readonly Term[] _terms;
    private bool _again;  // the world changed (a scene loaded): made again on its next use

    public Query(params Term[] terms)
    {
        if (terms.Length == 0 || terms.Length > MaxTerms)
            throw new ArgumentException($"a query takes 1..{MaxTerms} terms");
        _terms = terms;
        Create();
        s_live.Add(this);
    }

    private void Create()
    {
        ulong* ids = stackalloc ulong[_terms.Length];
        uint* access = stackalloc uint[_terms.Length];
        for (int i = 0; i < _terms.Length; ++i)
        {
            ids[i] = _terms[i].Component;
            access[i] = (uint)_terms[i].Access;
        }
        _handle = Api.Native->queryCreate(Api.Ctx, ids, access, (uint)_terms.Length);
        if (_handle == 0)
            throw new InvalidOperationException("invalid query");
    }

    public static Term In<T>() where T : unmanaged => new(&Ecs.Id<T>, FxAccess.In);
    public static Term InOut<T>() where T : unmanaged => new(&Ecs.Id<T>, FxAccess.InOut);
    public static Term Not<T>() where T : unmanaged => new(&Ecs.Id<T>, FxAccess.Not);

    /// <summary>Number of matching entities (iterates).</summary>
    public int Count()
    {
        int n = 0;
        foreach (Chunk c in this)
            n += c.Count;
        return n;
    }

    public Enumerator GetEnumerator()
    {
        if (_handle == 0 && _again)
        {
            for (int i = 0; i < _terms.Length; ++i)
                _terms[i] = _terms[i].Again();
            Create();
            _again = false;
        }
        if (_handle == 0)
            throw new ObjectDisposedException(nameof(Query));
        return new Enumerator(this);
    }

    public void Dispose()
    {
        if (_handle != 0)
        {
            Api.Native->queryDestroy(Api.Ctx, _handle);
            _handle = 0;
        }
        s_live.Remove(this);
    }

    // A scene loaded in the old one's place: the native queries go with the old world (now, while
    // it is there), each made again in the new one when it is next used.
    internal static void WorldChanged()
    {
        foreach (Query q in s_live)
            if (q._handle != 0)
            {
                Api.Native->queryDestroy(Api.Ctx, q._handle);
                q._handle = 0;
                q._again = true;
            }
    }

    // On stop / hot reload: native queries never outlive the game code that made them.
    internal static void DestroyAll()
    {
        foreach (Query q in s_live.ToArray())
            q.Dispose();
        s_live.Clear();
    }

    public ref struct Enumerator
    {
        private readonly Query _query;
        private Chunk _current;
        private bool _done;

        internal Enumerator(Query query)
        {
            _query = query;
            _current = default;
            _done = false;
        }

        public readonly Chunk Current => _current;

        public bool MoveNext()
        {
            if (_done)
                return false;
            void** columns = stackalloc void*[MaxTerms];
            ulong* entities;
            int count = Api.Native->queryNext(Api.Ctx, _query._handle, columns, &entities);
            if (count == 0)
            {
                _done = true;
                return false;
            }
            _current = new Chunk(_query._terms, count, entities, columns);
            return true;
        }

        public void Dispose()
        {
            if (!_done)
                Api.Native->queryEnd(Api.Ctx, _query._handle);
            _done = true;
        }
    }
}

/// <summary>The matching entities of one table and their component columns.</summary>
public readonly unsafe ref struct Chunk
{
    private readonly Term[] _terms;
    private readonly ulong* _entities;
    private readonly void* _c0, _c1, _c2, _c3, _c4, _c5, _c6, _c7;

    public int Count { get; }

    internal Chunk(Term[] terms, int count, ulong* entities, void** columns)
    {
        _terms = terms;
        Count = count;
        _entities = entities;
        _c0 = columns[0]; _c1 = columns[1]; _c2 = columns[2]; _c3 = columns[3];
        _c4 = columns[4]; _c5 = columns[5]; _c6 = columns[6]; _c7 = columns[7];
    }

    public ReadOnlySpan<Entity> Entities => new(_entities, Count);

    /// <summary>Writable column of term `index` (must be an InOut term of component T).</summary>
    public Span<T> Write<T>(int index) where T : unmanaged
    {
        if (_terms[index].Access != FxAccess.InOut)
            throw new InvalidOperationException($"term {index} is not InOut");
        return new Span<T>(Column<T>(index), Count);
    }

    /// <summary>Read-only column of term `index`.</summary>
    public ReadOnlySpan<T> Read<T>(int index) where T : unmanaged => new(Column<T>(index), Count);

    private void* Column<T>(int index) where T : unmanaged
    {
        if ((uint)index >= 8 || _terms[index].Access == FxAccess.Not)
            throw new ArgumentOutOfRangeException(nameof(index), "not a data term (terms 0..7 carry data)");
        if (_terms[index].Component != Ecs.Id<T>())
            throw new InvalidOperationException($"term {index} is not {typeof(T).Name}");
        return index switch
        {
            0 => _c0, 1 => _c1, 2 => _c2, 3 => _c3, 4 => _c4, 5 => _c5, 6 => _c6, _ => _c7,
        };
    }
}
