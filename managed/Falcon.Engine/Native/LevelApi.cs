using System.Runtime.InteropServices;

namespace Falcon.Native;

// The level tools' C ABI (engine/script/level_api.h), with its own version: FxNativeApi.level.

/// <summary>A live mesh's triangles (level_api.h FxLiveMeshData).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxLiveMeshData
{
    public float* positions;
    public float* normals;
    public float* uvs;
    public float* tangents;
    public uint* indices;
    public uint* materials;
    public int vertexCount;
    public int indexCount;
    public uint flags;  // 1: no collider
    public int pad;
}

/// <summary>Live meshes in use and room (level_api.h FxLiveStats).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FxLiveStats
{
    public uint meshes, meshCapacity;
    public uint blocks, blockCapacity;
    public uint triangles;
    public uint uploads;
    public float lastBuildMs;
    public uint pad;
}

/// <summary>The editor's scene view (level_api.h FxViewportState).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxViewportState
{
    public fixed float mouse[2];
    public fixed float size[2];
    public fixed float rayOrigin[3];
    public fixed float rayDir[3];
    public fixed float cameraPos[3];
    public fixed float cameraForward[3];
    public fixed float cameraRight[3];
    public fixed float cameraUp[3];
    public float p00, p11;
    public uint buttons, pressed, released, mods;
    public float wheel;
    public uint inside, captured, elementMode;
    public int keyCount;
    public fixed uint keys[16];
    public fixed byte text[32];
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxLevelApi
{
    public const uint Version = 9;

    public uint version;
    public uint size;
    public nint context;
    public delegate* unmanaged<nint, float*, ulong, ulong*, int> liveCreate;
    public delegate* unmanaged<nint, int, FxLiveMeshData*, int> liveSet;
    public delegate* unmanaged<nint, int, void> liveDestroy;
    public delegate* unmanaged<nint, ulong, int> liveOf;
    public delegate* unmanaged<nint, int, uint, uint, int> liveTriangle;
    public delegate* unmanaged<nint, FxLiveStats*, void> liveStats;
    public delegate* unmanaged<nint, ulong, byte*, int, int> meshGet;
    public delegate* unmanaged<nint, ulong, byte*, int, int> meshSet;
    public delegate* unmanaged<nint, float*, ulong, byte*, int, ulong> meshCreate;
    public delegate* unmanaged<nint, FxViewportState*, int> viewport;
    public delegate* unmanaged<nint, int, void> viewportCapture;
    public delegate* unmanaged<nint, byte*, void> viewportHint;
    public delegate* unmanaged<nint, ulong*, uint*, ulong*, int, int> elementsGet;
    public delegate* unmanaged<nint, ulong, uint, ulong*, int, void> elementsSet;
    public delegate* unmanaged<nint, ulong, byte*, int, int> meshSetLive;
    public delegate* unmanaged<nint, ulong, byte*, int, int> meshCommit;
    public delegate* unmanaged<nint, byte*, byte*, int, byte*, byte*, int, int> meshOp;
    // (v3) The C# modifiers' handler (Falcon.Level.MeshModifiers).
    public delegate* unmanaged<nint, delegate* unmanaged<int, byte*, byte*, byte*, int, byte*, int, int>, void> modifierHandler;
    // (v4) The scene's navigation mesh (Falcon.Level.Navigation).
    public delegate* unmanaged<nint, float*, float*, float*, int, int> navPath;
    public delegate* unmanaged<nint, float*, float*, float*, int> navRaycast;
    public delegate* unmanaged<nint, float*, float*, float*, int> navNearest;
    // (v5) Gameplay volumes (Falcon.Level.Volumes).
    public delegate* unmanaged<nint, ulong, byte*, int, int> volumeKind;
    public delegate* unmanaged<nint, float*, ulong*, int*, int, int> volumesAt;
    // (v6) Agents on the navigation mesh (Falcon.Level.NavAgent).
    public delegate* unmanaged<nint, float*, float, float, float, float, int> crowdAdd;
    public delegate* unmanaged<nint, int, void> crowdRemove;
    public delegate* unmanaged<nint, int, float*, int> crowdTarget;
    public delegate* unmanaged<nint, float, void> crowdUpdate;
    public delegate* unmanaged<nint, int, float*, float*, float> crowdState;
    // (v7) Cutting while the game runs (Falcon.Level.Cuts).
    public delegate* unmanaged<nint, ulong, int, int, uint, float, byte*, int> cutSet;
    public delegate* unmanaged<nint, ulong> geometryHash;
    // (v8) Cuts being made on worker threads.
    public delegate* unmanaged<nint, int> cutsPending;
    // (v9) The commands' state for the editor's Level window (JSON).
    public delegate* unmanaged<nint, byte*, void> toolState;
}

internal static unsafe class LevelNative
{
    // The level table when the engine has one this version knows (null: none, or another version).
    internal static FxLevelApi* Api
    {
        get
        {
            FxLevelApi* l = Falcon.Native.Api.Native != null ? Falcon.Native.Api.Native->level : null;
            return l != null && l->version == FxLevelApi.Version && l->size == (uint)sizeof(FxLevelApi) ? l : null;
        }
    }
}
