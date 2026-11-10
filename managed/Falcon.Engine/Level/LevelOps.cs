using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>What a mesh operation gave: the new mesh, what to select next, and (detach) the part it
/// took out. <see cref="Ok"/> false: it did nothing (<see cref="Error"/> says why).</summary>
public sealed class MeshOpResult
{
    public bool Ok;
    public PolyMesh? Mesh;
    public PolyMesh? Detached;
    /// <summary>The selection's mode after it (Object: keep what was chosen).</summary>
    public ElementMode SelectionMode;
    public readonly List<ulong> Selection = new();
    public string Error = "";
}

/// <summary>
/// The engine's mesh operations (engine/level/mesh_ops.h) on a <see cref="PolyMesh"/>: ProBuilder's
/// set by name with JSON arguments. They run on a copy; give the result back with
/// <see cref="LevelMeshes.Set"/> (or SetLive while a modal operation goes on).
/// <code>
/// MeshOpResult r = LevelOps.Run("extrude", mesh, ElementMode.Face, faces, w => w.WriteNumber("distance", 0.5));
/// if (r.Ok) LevelMeshes.Set(entity, r.Mesh!);
/// </code>
/// Operations: extrude, extrudeEdges, inset, bevel, connect, loop, ring, loopCut, subdivide, merge,
/// weld, fill, delete, detach, flip, conform, triangulate, quadrangulate, mergeFaces, smooth,
/// autoSmooth, material, uv, grow, shrink, byMaterial, byNormal; shapes through <see cref="Shape"/>.
/// </summary>
public static unsafe class LevelOps
{
    /// <summary>An operation with its arguments as JSON text (an object). Mesh: null for "shape".</summary>
    public static MeshOpResult Run(string op, PolyMesh? mesh, string argsJson)
    {
        var result = new MeshOpResult();
        FxLevelApi* l = LevelNative.Api;
        if (l == null)
        {
            result.Error = "no engine";
            return result;
        }
        byte[] blob = mesh?.ToBytes() ?? Array.Empty<byte>();
        byte[] name = Encoding.UTF8.GetBytes(op + "\0");
        byte[] args = Encoding.UTF8.GetBytes(argsJson + "\0");
        byte[] buffer = new byte[Math.Max(4096, blob.Length * 2 + 1024)];
        int n;
        fixed (byte* b = blob)
        fixed (byte* o = name)
        fixed (byte* a = args)
        {
            byte* bp = mesh != null ? b : null;
            fixed (byte* p = buffer)
                n = l->meshOp(l->context, o, bp, blob.Length, a, p, buffer.Length);
            if (n < 0)
            {
                buffer = new byte[-n];
                fixed (byte* p = buffer)
                    n = l->meshOp(l->context, o, bp, blob.Length, a, p, buffer.Length);
            }
        }
        if (n <= 0)
        {
            result.Error = "bad arguments";
            return result;
        }
        ReadOnlySpan<byte> s = buffer.AsSpan(0, n);
        int meshSize = (int)BinaryPrimitives.ReadUInt32LittleEndian(s);
        ReadOnlySpan<byte> meshBlob = s.Slice(4, meshSize);
        s = s[(4 + meshSize)..];
        int detachedSize = (int)BinaryPrimitives.ReadUInt32LittleEndian(s);
        ReadOnlySpan<byte> detachedBlob = s.Slice(4, detachedSize);
        s = s[(4 + detachedSize)..];
        using JsonDocument d = JsonDocument.Parse(s.ToArray());
        JsonElement root = d.RootElement;
        result.Ok = root.GetProperty("ok").GetBoolean();
        result.SelectionMode = (ElementMode)root.GetProperty("mode").GetUInt32();
        foreach (JsonElement item in root.GetProperty("selection").EnumerateArray())
            result.Selection.Add(item.GetUInt64());
        result.Error = root.GetProperty("error").GetString() ?? "";
        if (meshSize > 0)
            result.Mesh = PolyMesh.FromBytes(meshBlob);
        if (detachedSize > 0)
            result.Detached = PolyMesh.FromBytes(detachedBlob);
        result.Ok &= result.Mesh != null;
        return result;
    }

    /// <summary>An operation on chosen elements ("vertices", "edges" or "faces" by the mode), plus
    /// its own numbers written by <paramref name="args"/>.</summary>
    public static MeshOpResult Run(string op, PolyMesh mesh, ElementMode mode, IReadOnlyList<ulong> items, Action<Utf8JsonWriter>? args = null) =>
        Run(op, mesh, Args(mode, items, args));

    /// <summary>A shape (box, plane, cylinder, cone, sphere, torus, pipe, prism, arch, stairs,
    /// curvedStairs, polygon) centred on the origin, from its arguments ("size": [x, y, z],
    /// "radius", "height", "sides", ...). Null when there is no such shape.</summary>
    public static PolyMesh? Shape(string name, Action<Utf8JsonWriter>? args = null)
    {
        MeshOpResult r = Run("shape", null, Args(ElementMode.Object, Array.Empty<ulong>(), w =>
        {
            w.WriteString("name", name);
            args?.Invoke(w);
        }));
        return r.Ok ? r.Mesh : null;
    }

    /// <summary>The entity's transform as "world" (UV operations: World faces project through it).</summary>
    public static void WriteWorld(Utf8JsonWriter w, Entity entity)
    {
        WorldTransform t = entity.World;
        w.WriteStartArray("world");
        for (int k = 0; k < 12; ++k)
            w.WriteNumberValue(t.rows[k]);
        w.WriteEndArray();
    }

    /// <summary>Arguments as JSON text: the elements under their mode's name, then the rest.</summary>
    public static string Args(ElementMode mode, IReadOnlyList<ulong> items, Action<Utf8JsonWriter>? more = null)
    {
        var buffer = new ArrayBufferWriter<byte>(256);
        using (var w = new Utf8JsonWriter(buffer))
        {
            w.WriteStartObject();
            string? key = mode switch { ElementMode.Vertex => "vertices", ElementMode.Edge => "edges", ElementMode.Face => "faces", _ => null };
            if (key != null)
            {
                w.WriteStartArray(key);
                foreach (ulong item in items)
                    w.WriteNumberValue(item);
                w.WriteEndArray();
            }
            more?.Invoke(w);
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }
}
