using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>
/// A modifier of your own for level meshes' modifier stacks: its [Property] members are its settings
/// (shown in the Inspector's Modifiers and saved with the scene, like a component's properties), and
/// <see cref="Apply"/> makes the modified mesh from the one before it in the stack. It runs in the
/// editor and in games, whenever the mesh, the stack or one of its settings changes.
/// </summary>
/// <example>
/// <code>
/// public sealed class Wobble : MeshModifier
/// {
///     [Property] public float Amount = 0.05f;
///     public override PolyMesh Apply(PolyMesh mesh)
///     {
///         for (int i = 0; i &lt; mesh.Positions.Count; ++i)
///             mesh.Positions[i] += new Vector3(0, MathF.Sin(mesh.Positions[i].X * 4) * Amount, 0);
///         return mesh;
///     }
/// }
/// </code>
/// </example>
public abstract class MeshModifier : Component
{
    /// <summary>The modified mesh (`mesh` is a copy: change it and give it back, or make another).</summary>
    public abstract PolyMesh Apply(PolyMesh mesh);
}

/// <summary>The game's <see cref="MeshModifier"/> classes (the generator registers them).</summary>
public static unsafe class MeshModifiers
{
    internal static readonly Dictionary<string, ComponentType> ByName = new();

    public static void Register(ComponentType type) => ByName[type.Name] = type;

    internal static void Forget() => ByName.Clear();  // (hot reload: the new code registers again)

    /// <summary>The engine's handler (level_api.h FxModifierHandler) given to it: at start and
    /// after a reload (its meshes made again with the new code).</summary>
    internal static void Install()
    {
        FxLevelApi* l = LevelNative.Api;
        if (l != null && l->modifierHandler != null)
            l->modifierHandler(l->context, &Call);
    }

    [UnmanagedCallersOnly]
    private static int Call(int op, byte* cls, byte* settings, byte* blob, int size, byte* output, int capacity)
    {
        try
        {
            if (op == 1)
                return Copy(Encoding.UTF8.GetBytes(ScriptComponents.Describe(ByName)), output, capacity);
            string name = Marshal.PtrToStringUTF8((nint)cls) ?? "";
            if (!ByName.TryGetValue(name, out ComponentType? type))
                return 0;
            PolyMesh? mesh = PolyMesh.FromBytes(new ReadOnlySpan<byte>(blob, size));
            if (mesh == null)
                return 0;
            var modifier = (MeshModifier)type.Create();
            string json = Marshal.PtrToStringUTF8((nint)settings) ?? "{}";
            using (JsonDocument doc = JsonDocument.Parse(json.Length == 0 ? "{}" : json))
                type.Read(modifier, doc.RootElement);
            return Copy(modifier.Apply(mesh).ToBytes(), output, capacity);
        }
        catch (Exception e)
        {
            Log.Error($"a C# modifier failed: {e.Message}");
            return 0;
        }
    }

    private static int Copy(byte[] bytes, byte* output, int capacity)
    {
        if (bytes.Length == 0)
            return 0;
        if (output == null || bytes.Length > capacity)
            return -bytes.Length;
        bytes.CopyTo(new Span<byte>(output, capacity));
        return bytes.Length;
    }
}
