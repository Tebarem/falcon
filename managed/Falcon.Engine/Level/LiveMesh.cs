using System;
using System.Numerics;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>A mesh made while the engine runs and drawn like the scene's own meshes: in the
/// visibility buffer with its materials (scene material indices, one per triangle), shadows, GI,
/// reflections and picking, and a triangle collider (a Collider with the mesh shape on
/// its entity uses it). It has an entity of its own: move, parent, hide or destroy it like any
/// other (destroying the entity frees the mesh). The game's end (the editor's Stop) frees the
/// game's. Room is reserved at load (--live-meshes, --live-clusters).</summary>
public sealed unsafe class LiveMesh : IDisposable
{
    private int _live;

    private LiveMesh(int live, Entity entity)
    {
        _live = live;
        Entity = entity;
    }

    /// <summary>The mesh's entity (<see cref="Entity"/>.Id 0 once disposed).</summary>
    public Entity Entity { get; private set; }

    /// <summary>Its index among the live meshes (-1 once disposed).</summary>
    public int Index => _live;

    /// <summary>A new, empty live mesh at a transform, under <paramref name="parent"/>. Null when
    /// none is left (or the engine has no renderer).</summary>
    public static LiveMesh? Create(Vector3 position, Quaternion rotation, Vector3 scale, Entity parent = default)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return null;
        WorldTransform w = WorldTransform.From(position, rotation, scale);
        ulong entity = 0;
        int live = l->liveCreate(l->context, w.rows, parent.Id, &entity);
        return live < 0 ? null : new LiveMesh(live, new Entity(entity));
    }

    public static LiveMesh? Create(Vector3 position) => Create(position, Quaternion.Identity, Vector3.One);

    /// <summary>Its triangles replaced (in its entity's space): positions and normals per vertex,
    /// UVs (empty: none; tangents then any), 3 indices per triangle, a scene material per triangle
    /// (empty: material 0). False: bad data or no room left (it keeps what it had).</summary>
    public bool Set(ReadOnlySpan<Vector3> positions, ReadOnlySpan<Vector3> normals, ReadOnlySpan<Vector2> uvs,
                    ReadOnlySpan<uint> indices, ReadOnlySpan<uint> materials = default, bool collider = true)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null || _live < 0 || normals.Length != positions.Length || (uvs.Length != 0 && uvs.Length != positions.Length) ||
            (materials.Length != 0 && materials.Length * 3 != indices.Length))
            return false;
        fixed (Vector3* p = positions)
        fixed (Vector3* n = normals)
        fixed (Vector2* t = uvs)
        fixed (uint* i = indices)
        fixed (uint* m = materials)
        {
            FxLiveMeshData d = new()
            {
                positions = (float*)p,
                normals = (float*)n,
                uvs = uvs.Length != 0 ? (float*)t : null,
                indices = i,
                materials = materials.Length != 0 ? m : null,
                vertexCount = positions.Length,
                indexCount = indices.Length,
                flags = collider ? 0u : 1u,
            };
            return l->liveSet(l->context, _live, &d) != 0;
        }
    }

    /// <summary>The triangle (its index in the indices given to <see cref="Set"/>, / 3) under a pick
    /// (its scene cluster and the triangle in it), or -1.</summary>
    public int TriangleOf(uint cluster, uint triangle)
    {
        FxLevelApi* l = LevelNative.Api;
        return l == null || _live < 0 ? -1 : l->liveTriangle(l->context, _live, cluster, triangle);
    }

    /// <summary>The live mesh an entity draws (-1 when none of the scripts').</summary>
    public static int IndexOf(Entity entity)
    {
        FxLevelApi* l = LevelNative.Api;
        return l == null ? -1 : l->liveOf(l->context, entity.Id);
    }

    /// <summary>Live meshes in use and the room left.</summary>
    public static FxLiveStats Stats
    {
        get
        {
            FxLevelApi* l = LevelNative.Api;
            FxLiveStats s = default;
            if (l != null)
                l->liveStats(l->context, &s);
            return s;
        }
    }

    /// <summary>Frees the mesh and deletes its entity.</summary>
    public void Dispose()
    {
        FxLevelApi* l = LevelNative.Api;
        if (l != null && _live >= 0)
            l->liveDestroy(l->context, _live);
        _live = -1;
        Entity = default;
    }
}
