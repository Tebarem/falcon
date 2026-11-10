using System.Collections.Generic;
using System.Numerics;
using System.Text;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>What a gameplay volume is for (Create/Volume/... in the editor).</summary>
public enum VolumeKind
{
    Trigger,
    Kill,
    Ladder,
    Audio,
    NavBlocker,
    NavArea,
}

/// <summary>
/// The scene's gameplay volumes: level meshes that mark space (not drawn in the game). Trigger, kill,
/// ladder and audio volumes are triggers too: a component on what moves hears OnTriggerEnter / Exit
/// with the volume's entity, and asks here what it is.
/// </summary>
public static unsafe class Volumes
{
    /// <summary>The volume `entity` is (null: not a volume), and its tag.</summary>
    public static VolumeKind? KindOf(Entity entity, out string tag)
    {
        tag = "";
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->volumeKind == null)
            return null;
        byte* buffer = stackalloc byte[256];
        int kind = l->volumeKind(l->context, entity.Id, buffer, 256);
        if (kind < 0)
            return null;
        int n = 0;
        while (n < 255 && buffer[n] != 0)
            ++n;
        tag = Encoding.UTF8.GetString(buffer, n);
        return (VolumeKind)kind;
    }

    /// <summary>The volume `entity` is (null: not a volume).</summary>
    public static VolumeKind? KindOf(Entity entity) => KindOf(entity, out _);

    /// <summary>The volumes `point` is in (inside their meshes' bounds), into `found`; how many.</summary>
    public static int At(Vector3 point, List<(Entity Entity, VolumeKind Kind)> found)
    {
        found.Clear();
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->volumesAt == null)
            return 0;
        const int capacity = 64;
        ulong* entities = stackalloc ulong[capacity];
        int* kinds = stackalloc int[capacity];
        int n = l->volumesAt(l->context, (float*)&point, entities, kinds, capacity);
        for (int i = 0; i < System.Math.Min(n, capacity); ++i)
            found.Add((new Entity(entities[i]), (VolumeKind)kinds[i]));
        return found.Count;
    }

    /// <summary>Whether `point` is in a volume of that kind.</summary>
    public static bool Inside(Vector3 point, VolumeKind kind)
    {
        var found = new List<(Entity, VolumeKind)>();
        At(point, found);
        foreach ((Entity _, VolumeKind k) in found)
            if (k == kind)
                return true;
        return false;
    }
}
