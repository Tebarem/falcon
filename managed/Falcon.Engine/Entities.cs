using System;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon;

// (API v18) Entities: enabled, names, the hierarchy and transforms; what a mesh draws with;
// meshes made at runtime; cached component references.

public readonly partial record struct Entity
{
    /// <summary>Destroys it and everything below it (Ecs.Destroy).</summary>
    public void Destroy() => Ecs.Destroy(this);

    /// <summary>Disabling it disables it and every entity below it that is enabled now: they are not
    /// drawn, their bodies stop, queries skip them. Enabling it again enables what it disabled (and
    /// nothing that was disabled before). The editor's Stop enables them again.</summary>
    public unsafe bool Enabled
    {
        get => Api.Native->entityEnabled(Api.Ctx, Id, 0) == 1;
        set => Api.Native->entityEnabled(Api.Ctx, Id, value ? 1 : 2);
    }

    /// <summary>Its name: the Hierarchy's (a game's scene edits', else its cooked name), or one the
    /// game gave it (until the editor's Stop). Find finds it by either.</summary>
    public unsafe string Name
    {
        get
        {
            Span<byte> utf8 = stackalloc byte[256];
            int n;
            fixed (byte* p = utf8)
                n = Api.Native->entityName(Api.Ctx, Id, p, utf8.Length);
            if (n >= 0)
                return Encoding.UTF8.GetString(utf8[..n]);
            byte[] big = new byte[-n];
            fixed (byte* p = big)
                n = Api.Native->entityName(Api.Ctx, Id, p, big.Length);
            return Encoding.UTF8.GetString(big, 0, Math.Max(n, 0));
        }
        set
        {
            byte[] utf8 = new byte[Encoding.UTF8.GetByteCount(value) + 1];
            Encoding.UTF8.GetBytes(value, utf8);
            fixed (byte* p = utf8)
                Api.Native->entitySetName(Api.Ctx, Id, p);
        }
    }

    /// <summary>The entity it hangs under (default: a root). Setting it keeps where it is in the
    /// world; default makes it a root again.</summary>
    public unsafe Entity Parent
    {
        get => new(Api.Native->entityParent(Api.Ctx, Id));
        set => SetParent(value);
    }

    /// <summary>Hangs it under <paramref name="parent"/> (default: a root again), keeping where it is
    /// in the world, or (<paramref name="keepWorld"/> false) taking its world transform as its
    /// transform relative to the parent. False when it can't be: the parent is below it.</summary>
    public unsafe bool SetParent(Entity parent, bool keepWorld = true) =>
        Api.Native->entitySetParent(Api.Ctx, Id, parent.Id, keepWorld ? 1 : 0) != 0;

    /// <summary>Its children, copied into <paramref name="into"/>; returns how many there are
    /// (more than fit: the rest are left out). Walks every child in the scene: keep what it finds.</summary>
    public unsafe int Children(Span<Entity> into)
    {
        fixed (Entity* p = into)
            return Api.Native->entityChildren(Api.Ctx, Id, (ulong*)p, into.Length);
    }

    /// <summary>Its world transform. Setting it moves the entity (a child's transform relative to
    /// its parent follows).</summary>
    public unsafe WorldTransform World
    {
        get => Ecs.Get<WorldTransform>(this);
        set => Api.Native->entitySetWorld(Api.Ctx, Id, value.rows);
    }

    /// <summary>Where it is in the world.</summary>
    public Vector3 Position
    {
        get => World.Position;
        set
        {
            WorldTransform w = World;
            w.Position = value;
            World = w;
        }
    }

    /// <summary>Its scale in the world (along its own axes).</summary>
    public Vector3 Scale => World.Scale;

    /// <summary>How it is turned in the world.</summary>
    public Quaternion Rotation
    {
        get => World.Rotation;
        set
        {
            WorldTransform w = World;
            w.Set(w.Position, value, w.Scale);
            World = w;
        }
    }

    /// <summary>Its axes in the world: forward is -Z (as the engine's cameras and lights look),
    /// right +X, up +Y.</summary>
    public Vector3 Forward => -World.Axis(2);
    public Vector3 Right => World.Axis(0);
    public Vector3 Up => World.Axis(1);

    /// <summary>Where it is relative to its parent (its world position for a root).</summary>
    public Vector3 LocalPosition
    {
        get => Ecs.Has<LocalTransform>(this) ? Ecs.Get<LocalTransform>(this).Position : Position;
        set
        {
            if (!Ecs.Has<LocalTransform>(this))
            {
                Position = value;
                return;
            }
            LocalTransform l = Ecs.Get<LocalTransform>(this);
            l.Position = value;
            Ecs.Set(this, l);  // (its world follows when the scene propagates, before drawing)
        }
    }

    /// <summary>How it is turned relative to its parent (in the world for a root).</summary>
    public Quaternion LocalRotation
    {
        get => Ecs.Has<LocalTransform>(this) ? Ecs.Get<LocalTransform>(this).Rotation : Rotation;
        set
        {
            if (!Ecs.Has<LocalTransform>(this))
            {
                Rotation = value;
                return;
            }
            LocalTransform l = Ecs.Get<LocalTransform>(this);
            l.Set(l.Position, value, l.Scale);
            Ecs.Set(this, l);
        }
    }

    /// <summary>Its scale relative to its parent (in the world for a root).</summary>
    public Vector3 LocalScale
    {
        get => Ecs.Has<LocalTransform>(this) ? Ecs.Get<LocalTransform>(this).Scale : Scale;
        set
        {
            if (!Ecs.Has<LocalTransform>(this))
            {
                WorldTransform w = World;
                w.Set(w.Position, w.Rotation, value);
                World = w;
                return;
            }
            LocalTransform l = Ecs.Get<LocalTransform>(this);
            l.Set(l.Position, l.Rotation, value);
            Ecs.Set(this, l);
        }
    }
}

// 3x4 transforms (rows of a matrix acting on column vectors; columns 0..2 are the scaled axes).
internal static unsafe class Affine
{
    public static Vector3 Axis(float* rows, int i) => new(rows[i], rows[4 + i], rows[8 + i]);

    public static Vector3 Scale(float* rows)
    {
        var s = new Vector3(Axis(rows, 0).Length(), Axis(rows, 1).Length(), Axis(rows, 2).Length());
        // A mirrored transform: the first axis takes the sign.
        float det = Vector3.Dot(Axis(rows, 0), Vector3.Cross(Axis(rows, 1), Axis(rows, 2)));
        return det < 0f ? s with { X = -s.X } : s;
    }

    public static Quaternion Rotation(float* rows)
    {
        Vector3 s = Scale(rows);
        Vector3 x = s.X != 0f ? Axis(rows, 0) / s.X : Vector3.UnitX;
        Vector3 y = s.Y != 0f ? Axis(rows, 1) / s.Y : Vector3.UnitY;
        Vector3 z = s.Z != 0f ? Axis(rows, 2) / s.Z : Vector3.UnitZ;
        // System.Numerics is row-vector: a matrix's rows are the axes.
        var m = new Matrix4x4(x.X, x.Y, x.Z, 0, y.X, y.Y, y.Z, 0, z.X, z.Y, z.Z, 0, 0, 0, 0, 1);
        return Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(m));
    }

    public static void Set(float* rows, Vector3 position, Quaternion rotation, Vector3 scale)
    {
        Matrix4x4 m = Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(rotation));
        rows[0] = m.M11 * scale.X; rows[1] = m.M21 * scale.Y; rows[2] = m.M31 * scale.Z; rows[3] = position.X;
        rows[4] = m.M12 * scale.X; rows[5] = m.M22 * scale.Y; rows[6] = m.M32 * scale.Z; rows[7] = position.Y;
        rows[8] = m.M13 * scale.X; rows[9] = m.M23 * scale.Y; rows[10] = m.M33 * scale.Z; rows[11] = position.Z;
    }
}

public unsafe partial struct WorldTransform
{
    /// <summary>Its scale along its own axes (the first negative when it mirrors).</summary>
    public readonly Vector3 Scale { get { fixed (float* r = rows) return Affine.Scale(r); } }

    /// <summary>Its rotation.</summary>
    public readonly Quaternion Rotation { get { fixed (float* r = rows) return Affine.Rotation(r); } }

    /// <summary>Axis i (0 X, 1 Y, 2 Z) in the world, scaled.</summary>
    public readonly Vector3 Axis(int i) { fixed (float* r = rows) return Affine.Axis(r, i); }

    /// <summary>Made from a position, a rotation and a scale.</summary>
    public void Set(Vector3 position, Quaternion rotation, Vector3 scale) { fixed (float* r = rows) Affine.Set(r, position, rotation, scale); }

    public static WorldTransform From(Vector3 position, Quaternion rotation, Vector3 scale)
    {
        WorldTransform w = default;
        w.Set(position, rotation, scale);
        return w;
    }
}

public unsafe partial struct LocalTransform
{
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

    public readonly Vector3 Scale { get { fixed (float* r = rows) return Affine.Scale(r); } }
    public readonly Quaternion Rotation { get { fixed (float* r = rows) return Affine.Rotation(r); } }
    public void Set(Vector3 position, Quaternion rotation, Vector3 scale) { fixed (float* r = rows) Affine.Set(r, position, rotation, scale); }
}

/// <summary>What an entity's mesh draws with (its own row: not a body Physics.Spawn made). The
/// editor's Stop puts each back as it was.</summary>
public static unsafe class Renderer
{
    /// <summary>Hides or shows its mesh, apart from its being enabled (a disabled entity is never
    /// drawn). False when it draws no mesh of its own.</summary>
    public static bool SetVisible(Entity e, bool visible) => Api.Native->renderSet(Api.Ctx, e.Id, 0, visible ? 1u : 0u) != 0;

    /// <summary>(API v29) The entity's own copy of its material (its material, else its mesh's
    /// first), which it draws with from now: change it without changing the others. Invalid when
    /// it isn't a mesh or the 256 runtime slots are taken; the editor's Stop frees them.</summary>
    public static Material MaterialInstance(Entity e) => new(Api.Native->materialInstance(Api.Ctx, e.Id));

    public static bool IsVisible(Entity e)
    {
        uint v;
        return Api.Native->renderGet(Api.Ctx, e.Id, 0, &v) != 0 && v != 0;
    }

    /// <summary>Whether its mesh casts shadows.</summary>
    public static bool SetCastShadows(Entity e, bool cast) => Api.Native->renderSet(Api.Ctx, e.Id, 1, cast ? 1u : 0u) != 0;

    public static bool CastsShadows(Entity e)
    {
        uint v;
        return Api.Native->renderGet(Api.Ctx, e.Id, 1, &v) != 0 && v != 0;
    }

    /// <summary>A colour multiplying its base colour (0..1 each; alpha too). White: none.</summary>
    public static bool SetTint(Entity e, Vector4 rgba)
    {
        static uint Pack(float v) => (uint)Math.Clamp(MathF.Round(v * 255f), 0f, 255f);
        uint packed = Pack(rgba.X) | Pack(rgba.Y) << 8 | Pack(rgba.Z) << 16 | Pack(rgba.W) << 24;
        return Api.Native->renderSet(Api.Ctx, e.Id, 2, packed == 0xffffffffu ? 0u : packed) != 0;
    }

    public static Vector4 Tint(Entity e)
    {
        uint v;
        if (Api.Native->renderGet(Api.Ctx, e.Id, 2, &v) == 0 || v == 0)
            return Vector4.One;
        return new Vector4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255f;
    }

    /// <summary>The material its mesh draws with: an .fmat (project path), or null for the mesh's
    /// own. False when it can't (no mesh of its own; no room for another material this session).</summary>
    public static bool SetMaterial(Entity e, string? material)
    {
        if (material == null)
            return Api.Native->renderMaterial(Api.Ctx, e.Id, null) != 0;
        Span<byte> utf8 = stackalloc byte[512];
        int n = Encoding.UTF8.GetBytes(material, utf8[..511]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return Api.Native->renderMaterial(Api.Ctx, e.Id, p) != 0;
    }
}

public static unsafe partial class Scene
{
    /// <summary>A mesh of the scene (by name) drawn at a place, with a material (.fmat; null: its
    /// own), under <paramref name="parent"/>. The GPU's instance table is fixed when the scene is
    /// uploaded, so only meshes it reserved slots for can be made: those of the project's prefabs
    /// and Model Renderers (and the built-in shapes in the editor). Default when none is free.
    /// Destroy it like any entity; the editor's Stop does.</summary>
    public static Entity SpawnMesh(string mesh, Vector3 position, Quaternion rotation, Vector3 scale, string? material = null,
                                   Entity parent = default)
    {
        WorldTransform w = WorldTransform.From(position, rotation, scale);
        Span<byte> name = stackalloc byte[256], mat = stackalloc byte[512];
        int n = Encoding.UTF8.GetBytes(mesh, name[..255]);
        name[n] = 0;
        n = Encoding.UTF8.GetBytes(material ?? "", mat[..511]);
        mat[n] = 0;
        fixed (byte* pn = name, pm = mat)
            return new Entity(Api.Native->meshSpawn(Api.Ctx, pn, w.rows, material != null ? pm : null, parent.Id));
    }

    public static Entity SpawnMesh(string mesh, Vector3 position, string? material = null) =>
        SpawnMesh(mesh, position, Quaternion.Identity, Vector3.One, material);
}

/// <summary>A cached reference to an entity's component: <see cref="Get"/> finds it again only
/// when the entity's table changed (a component added or removed), so reading the same component
/// every frame costs a few ns. Keep it in a field (not readonly: it updates itself) or a local.
/// Read only: write through Ecs.Set or a query, which tell the engine the component changed.</summary>
public unsafe struct Ref<T> where T : unmanaged
{
    private FxRef _ref;

    public Ref(Entity entity)
    {
        fixed (FxRef* r = &_ref)
            Api.Native->refInit(Api.Ctx, r, entity.Id, Ecs.Id<T>());
    }

    /// <summary>Whether the entity has the component (and is alive).</summary>
    public bool Has
    {
        get
        {
            fixed (FxRef* r = &_ref)
                return Api.Native->refGet(Api.Ctx, r, Ecs.Id<T>()) != null;
        }
    }

    /// <summary>A copy of the component, or default when it is gone.</summary>
    public T Get()
    {
        fixed (FxRef* r = &_ref)
        {
            void* p = Api.Native->refGet(Api.Ctx, r, Ecs.Id<T>());
            return p == null ? default : *(T*)p;
        }
    }
}
