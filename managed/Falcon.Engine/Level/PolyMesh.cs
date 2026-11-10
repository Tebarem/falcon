using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>How a face's UVs come about (engine/level/poly_mesh.h UvMode).</summary>
public enum UvMode : uint
{
    /// <summary>Projected onto the face in the mesh's own space: the texture moves with the mesh.</summary>
    Local = 0,
    /// <summary>Projected in world space: the texture stays put while the mesh moves, so neighbouring meshes line up.</summary>
    World = 1,
    /// <summary>The corners' own UVs (<see cref="PolyMesh.CornerUvs"/>), then offset, scale and rotation.</summary>
    Manual = 2,
}

/// <summary>A face's UV settings.</summary>
public struct FaceUv
{
    public UvMode Mode;
    public Vector2 Offset;
    /// <summary>Metres per tile (Local, World); a multiplier (Manual).</summary>
    public Vector2 Scale;
    /// <summary>Degrees.</summary>
    public float Rotation;

    public static FaceUv Default => new() { Mode = UvMode.Local, Scale = Vector2.One };
}

/// <summary>A face: a loop of corners (<see cref="PolyMesh.Corners"/>[First .. First + Count]),
/// counter-clockwise seen from its front, with its material slot, smoothing group (0: flat) and UVs.</summary>
public struct Face
{
    public int First;
    public int Count;
    public int Material;
    public uint Smoothing;
    public FaceUv Uv;
}

/// <summary>An editable level mesh (engine/level/poly_mesh.h): shared vertices, n-gon faces over
/// them, material slots (.fmat paths, "" the default). The engine triangulates it, smooths it
/// within smoothing groups and projects its UVs. Edit a copy and give it back with
/// <see cref="LevelMeshes.Set"/>: in the editor each change is an undo step.</summary>
public sealed class PolyMesh
{
    private const uint Format = 2;  // (1: no mask UVs, vertex colours or layers)

    public List<Vector3> Positions { get; } = new();
    public List<int> Corners { get; } = new();
    /// <summary>Per corner (parallel to <see cref="Corners"/>) when any face is Manual; else empty.</summary>
    public List<Vector2> CornerUvs { get; } = new();
    public List<Face> Faces { get; } = new();
    public List<string> Materials { get; } = new() { "" };
    /// <summary>Per corner when the mesh has layers: the painted mask's UVs (0..1; the maskUnwrap operation makes them).</summary>
    public List<Vector2> CornerUv1 { get; } = new();
    /// <summary>Per vertex, RGBA8 (r | g &lt;&lt; 8 | b &lt;&lt; 16 | a &lt;&lt; 24): a tint and (alpha) occlusion; empty: white.</summary>
    public List<uint> VertexColors { get; } = new();
    /// <summary>Up to three materials blended over the faces' own by height where the mask says ("" none).</summary>
    public string[] LayerMaterials { get; } = { "", "", "" };
    /// <summary>The layers' height blend width (0..1 of their heights).</summary>
    public float LayerBlend { get; set; } = 0.15f;

    public int TriangleCount
    {
        get
        {
            int n = 0;
            foreach (Face f in Faces)
                n += Math.Max(f.Count - 2, 0);
            return n;
        }
    }

    /// <summary>A face over existing vertices (counter-clockwise from its front); its index.</summary>
    public int AddFace(ReadOnlySpan<int> loop, int material = 0, uint smoothing = 0)
    {
        Face f = new() { First = Corners.Count, Count = loop.Length, Material = material, Smoothing = smoothing, Uv = FaceUv.Default };
        foreach (int c in loop)
            Corners.Add(c);
        if (CornerUvs.Count != 0)
            while (CornerUvs.Count < Corners.Count)
                CornerUvs.Add(Vector2.Zero);
        Faces.Add(f);
        return Faces.Count - 1;
    }

    /// <summary>A face's corner (0 .. Count - 1) as a vertex index.</summary>
    public int Corner(int face, int corner) => Corners[Faces[face].First + corner];

    /// <summary>A face's normal (Newell's method) and centre.</summary>
    public Vector3 FaceNormal(int face)
    {
        Face f = Faces[face];
        Vector3 n = Vector3.Zero;
        for (int i = 0; i < f.Count; ++i)
        {
            Vector3 a = Positions[Corners[f.First + i]], b = Positions[Corners[f.First + (i + 1) % f.Count]];
            n.X += (a.Y - b.Y) * (a.Z + b.Z);
            n.Y += (a.Z - b.Z) * (a.X + b.X);
            n.Z += (a.X - b.X) * (a.Y + b.Y);
        }
        return n.LengthSquared() > 0 ? Vector3.Normalize(n) : Vector3.Zero;
    }

    public Vector3 FaceCenter(int face)
    {
        Face f = Faces[face];
        Vector3 c = Vector3.Zero;
        for (int i = 0; i < f.Count; ++i)
            c += Positions[Corners[f.First + i]];
        return f.Count > 0 ? c / f.Count : c;
    }

    public PolyMesh Clone()
    {
        var m = new PolyMesh();
        m.Positions.AddRange(Positions);
        m.Corners.AddRange(Corners);
        m.CornerUvs.AddRange(CornerUvs);
        m.Faces.AddRange(Faces);
        m.Materials.Clear();
        m.Materials.AddRange(Materials);
        m.CornerUv1.AddRange(CornerUv1);
        m.VertexColors.AddRange(VertexColors);
        LayerMaterials.CopyTo(m.LayerMaterials, 0);
        m.LayerBlend = LayerBlend;
        return m;
    }

    /// <summary>A box centred on the origin, faces +X, -X, +Y, -Y, +Z, -Z.</summary>
    public static PolyMesh Box(Vector3 size)
    {
        var m = new PolyMesh();
        Vector3 h = size * 0.5f;
        for (int i = 0; i < 8; ++i)
            m.Positions.Add(new Vector3((i & 1) != 0 ? h.X : -h.X, (i & 2) != 0 ? h.Y : -h.Y, (i & 4) != 0 ? h.Z : -h.Z));
        m.AddFace(stackalloc int[] { 1, 3, 7, 5 });
        m.AddFace(stackalloc int[] { 0, 4, 6, 2 });
        m.AddFace(stackalloc int[] { 2, 6, 7, 3 });
        m.AddFace(stackalloc int[] { 0, 1, 5, 4 });
        m.AddFace(stackalloc int[] { 4, 5, 7, 6 });
        m.AddFace(stackalloc int[] { 0, 2, 3, 1 });
        return m;
    }

    /// <summary>The engine's blob (PolyMesh::save).</summary>
    public byte[] ToBytes()
    {
        using var s = new MemoryStream();
        using var w = new BinaryWriter(s);
        w.Write("FXLM"u8);
        w.Write(Format);
        w.Write((uint)Positions.Count);
        w.Write((uint)Corners.Count);
        w.Write((uint)Faces.Count);
        w.Write((uint)Materials.Count);
        bool uvs = CornerUvs.Count != 0, uv1 = CornerUv1.Count != 0, colors = VertexColors.Count != 0;
        bool layers = LayerMaterials[0].Length + LayerMaterials[1].Length + LayerMaterials[2].Length != 0 || LayerBlend != 0.15f;
        w.Write((uvs ? 1u : 0u) | (uv1 ? 2u : 0u) | (colors ? 4u : 0u) | (layers ? 8u : 0u));
        foreach (Vector3 p in Positions)
        {
            w.Write(p.X);
            w.Write(p.Y);
            w.Write(p.Z);
        }
        foreach (int c in Corners)
            w.Write((uint)c);
        if (uvs)
            foreach (Vector2 t in CornerUvs)
            {
                w.Write(t.X);
                w.Write(t.Y);
            }
        foreach (Face f in Faces)
        {
            w.Write((uint)f.First);
            w.Write((uint)f.Count);
            w.Write((uint)f.Material);
            w.Write(f.Smoothing);
            w.Write((uint)f.Uv.Mode);
            w.Write(f.Uv.Offset.X);
            w.Write(f.Uv.Offset.Y);
            w.Write(f.Uv.Scale.X);
            w.Write(f.Uv.Scale.Y);
            w.Write(f.Uv.Rotation);
        }
        foreach (string m in Materials)
            WriteString(w, m);
        if (uv1)
            foreach (Vector2 t in CornerUv1)
            {
                w.Write(t.X);
                w.Write(t.Y);
            }
        if (colors)
            foreach (uint c in VertexColors)
                w.Write(c);
        if (layers)
        {
            foreach (string m in LayerMaterials)
                WriteString(w, m);
            w.Write(LayerBlend);
        }
        w.Flush();
        return s.ToArray();
    }

    private static void WriteString(BinaryWriter w, string text)
    {
        byte[] b = Encoding.UTF8.GetBytes(text);
        w.Write((uint)b.Length);
        w.Write(b);
    }

    /// <summary>A mesh from the engine's blob, or null when it isn't one.</summary>
    public static PolyMesh? FromBytes(ReadOnlySpan<byte> blob)
    {
        try
        {
            using var s = new MemoryStream(blob.ToArray());
            using var r = new BinaryReader(s);
            if (r.ReadByte() != 'F' || r.ReadByte() != 'X' || r.ReadByte() != 'L' || r.ReadByte() != 'M')
                return null;
            uint format = r.ReadUInt32();
            if (format < 1 || format > Format)
                return null;
            int np = (int)r.ReadUInt32(), nc = (int)r.ReadUInt32(), nf = (int)r.ReadUInt32(), nm = (int)r.ReadUInt32();
            uint flags = r.ReadUInt32();
            var m = new PolyMesh();
            m.Materials.Clear();
            for (int i = 0; i < np; ++i)
                m.Positions.Add(new Vector3(r.ReadSingle(), r.ReadSingle(), r.ReadSingle()));
            for (int i = 0; i < nc; ++i)
                m.Corners.Add((int)r.ReadUInt32());
            if ((flags & 1) != 0)
                for (int i = 0; i < nc; ++i)
                    m.CornerUvs.Add(new Vector2(r.ReadSingle(), r.ReadSingle()));
            for (int i = 0; i < nf; ++i)
            {
                Face f = new()
                {
                    First = (int)r.ReadUInt32(), Count = (int)r.ReadUInt32(), Material = (int)r.ReadUInt32(), Smoothing = r.ReadUInt32(),
                };
                f.Uv = new FaceUv
                {
                    Mode = (UvMode)r.ReadUInt32(),
                    Offset = new Vector2(r.ReadSingle(), r.ReadSingle()),
                    Scale = new Vector2(r.ReadSingle(), r.ReadSingle()),
                    Rotation = r.ReadSingle(),
                };
                m.Faces.Add(f);
            }
            for (int i = 0; i < nm; ++i)
                m.Materials.Add(Encoding.UTF8.GetString(r.ReadBytes((int)r.ReadUInt32())));
            if ((flags & 2) != 0)
                for (int i = 0; i < nc; ++i)
                    m.CornerUv1.Add(new Vector2(r.ReadSingle(), r.ReadSingle()));
            if ((flags & 4) != 0)
                for (int i = 0; i < np; ++i)
                    m.VertexColors.Add(r.ReadUInt32());
            if ((flags & 8) != 0)
            {
                for (int i = 0; i < 3; ++i)
                    m.LayerMaterials[i] = Encoding.UTF8.GetString(r.ReadBytes((int)r.ReadUInt32()));
                m.LayerBlend = r.ReadSingle();
            }
            return m;
        }
        catch (EndOfStreamException)
        {
            return null;
        }
    }
}

/// <summary>Level meshes in the scene: entities with a <see cref="PolyMesh"/>, saved with it.</summary>
public static unsafe class LevelMeshes
{
    /// <summary>An entity's mesh (a copy), or null when it has none.</summary>
    public static PolyMesh? Get(Entity entity)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return null;
        int n = l->meshGet(l->context, entity.Id, null, 0);
        if (n == 0)
            return null;
        byte[] blob = new byte[Math.Abs(n)];
        fixed (byte* b = blob)
            n = l->meshGet(l->context, entity.Id, b, blob.Length);
        return n > 0 ? PolyMesh.FromBytes(blob.AsSpan(0, n)) : null;
    }

    /// <summary>The entity's mesh replaced (in the editor while editing: an undo step). False: no
    /// such entity, or a mesh the engine refuses (a corner past the vertices, a face of under 3).</summary>
    public static bool Set(Entity entity, PolyMesh mesh)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return false;
        byte[] blob = mesh.ToBytes();
        fixed (byte* b = blob)
            return l->meshSet(l->context, entity.Id, b, blob.Length) != 0;
    }

    /// <summary>The entity's mesh shown while an operation goes on: no undo step (a game: set).</summary>
    public static bool SetLive(Entity entity, PolyMesh mesh)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return false;
        byte[] blob = mesh.ToBytes();
        fixed (byte* b = blob)
            return l->meshSetLive(l->context, entity.Id, b, blob.Length) != 0;
    }

    /// <summary>The operation's end: what the entity shows now becomes one undo step from
    /// <paramref name="before"/> (the mesh it started with).</summary>
    public static bool Commit(Entity entity, PolyMesh before)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return false;
        byte[] blob = before.ToBytes();
        fixed (byte* b = blob)
            return l->meshCommit(l->context, entity.Id, b, blob.Length) != 0;
    }

    /// <summary>A new level mesh entity (in the editor while editing: as Create > Level Mesh, an
    /// undo step, selected; while playing, deleted at the game's end). Default when it can't.</summary>
    public static Entity Create(PolyMesh mesh, Vector3 position, Quaternion rotation, Vector3 scale, Entity parent = default)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
            return default;
        WorldTransform w = WorldTransform.From(position, rotation, scale);
        byte[] blob = mesh.ToBytes();
        fixed (byte* b = blob)
            return new Entity(l->meshCreate(l->context, w.rows, parent.Id, b, blob.Length));
    }

    public static Entity Create(PolyMesh mesh, Vector3 position) => Create(mesh, position, Quaternion.Identity, Vector3.One);
}
