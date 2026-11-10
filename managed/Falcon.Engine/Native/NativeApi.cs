using System.Runtime.InteropServices;

namespace Falcon.Native;

// Mirrors FxNativeApi / FxManagedApi in engine/script/script_api.h field for field.
// Function pointers only: calls cost an indirect call, nothing is marshaled.
// Leaf calls (`unmanaged[SuppressGCTransition]`) also skip the GC transition (a few ns each): only
// calls that return quickly, never block on a lock or I/O and never call back into managed code.
// Anything that can take long (physics queries, spawning, logging, UI, audio's mixer) keeps it.

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxNativeApi
{
    public const uint Version = 46;

    public uint version;
    public uint size;
    public nint context;

    public delegate* unmanaged<nint, int, byte*, void> log;

    public delegate* unmanaged<nint, byte*, uint, uint, ulong> componentId;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong> entityCreate;
    public delegate* unmanaged<nint, ulong, void> entityDestroy;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int> entityAlive;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, void*, uint, void> entitySet;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, void*> entityGet;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong, void> entityRemove;

    public delegate* unmanaged<nint, ulong*, uint*, uint, nint> queryCreate;
    public delegate* unmanaged<nint, nint, void> queryDestroy;
    public delegate* unmanaged[SuppressGCTransition]<nint, nint, void**, ulong**, int> queryNext;
    public delegate* unmanaged[SuppressGCTransition]<nint, nint, void> queryEnd;

    public delegate* unmanaged[SuppressGCTransition]<nint, uint, float*, float*, void> meshBounds;

    public delegate* unmanaged[SuppressGCTransition]<nint, float*, float*, float*, float*, void> cameraGet;

    public delegate* unmanaged<nint, uint, float*, float*, float*, uint, ulong> bodySpawn;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, void> bodyImpulse;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, void> bodyVelocity;

    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, byte*, int> animParameter;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, float, void> animSetFloat;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, int, void> animSetBool;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, byte*, void> animSetId;

    public byte* keys;
    public byte* keysPrev;
    public int keyCount;
    public int pad;

    public delegate* unmanaged<nint, byte*, int, void> uiSend;
    public delegate* unmanaged<nint, byte*, int, int> uiPoll;

    public delegate* unmanaged<nint, PerformanceStats*, void> perfStats;
    public delegate* unmanaged<nint, byte*, int, void> hudSet;

    public delegate* unmanaged[SuppressGCTransition]<nint, float*, FxWaterSample*, int> waterSample;
    public delegate* unmanaged<nint, float*, float, float, void> waterSplash;

    public delegate* unmanaged<nint, byte*, uint> audioSound;
    public delegate* unmanaged<nint, uint, float*, ulong, float, float, uint, uint> audioPlay;
    public delegate* unmanaged<nint, uint, float*, float, float, uint> audioImpact;
    public delegate* unmanaged<nint, uint, float, void> audioStop;
    public delegate* unmanaged<nint, uint, float, float, float*, void> audioSet;
    public delegate* unmanaged<nint, uint, int> audioPlaying;
    public delegate* unmanaged<nint, uint, float, void> audioBusVolume;
    public delegate* unmanaged[SuppressGCTransition]<nint, Contact*, int, int> contactPoll;
    public delegate* unmanaged<nint, float*, float, SurfaceHit*, int> surfaceAt;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, AnimEvent*, int, int> animEvents;
    public delegate* unmanaged[SuppressGCTransition]<nint, FxMouse*, void> mouseGet;
    public delegate* unmanaged<nint, int, void> mouseLock;
    public delegate* unmanaged<nint, byte*, ulong> entityFind;
    public delegate* unmanaged<nint, ulong, byte*, ulong> entityFindUnder;
    public delegate* unmanaged<nint, ulong, byte*, int, int> animHideBone;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float, int> animFootIk;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, float, byte*, int> animLookAt;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, byte*, float*, float, int> animReach;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, float, byte*, float*, float*, byte*, int> animAimAt;
    public delegate* unmanaged<nint, float*, float*, float, SurfaceHit*, int> raycast;
    public delegate* unmanaged<nint, byte*, float*, float*, ulong, ulong> prefabSpawn;
    public delegate* unmanaged<nint, ulong, int, int> particlesControl;
    public delegate* unmanaged<nint, ulong, int, int> particlesEmit;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int> particlesCount;
    public delegate* unmanaged<nint, ulong, ParticleHit*, int, int> particlesHits;
    public delegate* unmanaged<nint, ulong, byte*, void> particlesScript;
    public delegate* unmanaged<nint, byte*, float*, float*, float, float, float, float, ulong, int> decalPlace;
    public delegate* unmanaged<nint, void> decalsClear;
    public delegate* unmanaged<nint, ulong, byte*, int, ulong, ulong> animEntity;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong> animCharacter;

    // (v18) Entities, rendering, meshes, refs. Enabled walks the subtree and names may call into the
    // editor: those keep the transition.
    public delegate* unmanaged<nint, ulong, int, int> entityEnabled;
    public delegate* unmanaged<nint, ulong, byte*, int, int> entityName;
    public delegate* unmanaged<nint, ulong, byte*, void> entitySetName;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, ulong> entityParent;
    public delegate* unmanaged<nint, ulong, ulong, int, int> entitySetParent;
    public delegate* unmanaged<nint, ulong, ulong*, int, int> entityChildren;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, void> entitySetWorld;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, uint*, int> renderGet;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, uint, int> renderSet;
    public delegate* unmanaged<nint, ulong, byte*, int> renderMaterial;
    public delegate* unmanaged<nint, byte*, float*, byte*, ulong, ulong> meshSpawn;
    public delegate* unmanaged[SuppressGCTransition]<nint, FxRef*, ulong, ulong, void> refInit;
    public delegate* unmanaged[SuppressGCTransition]<nint, FxRef*, ulong, void*> refGet;

    // (v19) Physics. Body calls are Box3D leaf calls; queries, joints and the character keep the transition.
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, FxBodyState*, int> bodyGet;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, FxBodyState*, uint, int> bodySet;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, float*, float*, int, int> bodyPush;
    public delegate* unmanaged<nint, ulong, float*, float*, int> bodyTeleport;
    public delegate* unmanaged<nint, ulong, int> bodyRebuild;
    public delegate* unmanaged<nint, float*, float*, void> physicsGravity;
    public delegate* unmanaged<nint, float*, float*, float, FxQueryFilter*, SurfaceHit*, int> raycastFiltered;
    public delegate* unmanaged<nint, FxQueryShape*, float*, float, FxQueryFilter*, SurfaceHit*, int> shapeCast;
    public delegate* unmanaged<nint, FxQueryShape*, FxQueryFilter*, ulong*, int, int> overlap;
    public delegate* unmanaged[SuppressGCTransition]<nint, TriggerEvent*, int, int> triggerPoll;
    public delegate* unmanaged<nint, ulong, float*, float, float, float, float, float*, FxQueryFilter*, CharacterMove*, int> characterMove;
    public delegate* unmanaged<nint, int, ulong, ulong, FxJointSettings*, ulong> jointCreate;
    public delegate* unmanaged<nint, ulong, int> jointDestroy;
    public delegate* unmanaged<nint, ulong, float, float, void> jointMotor;

    // (v20) Script components.
    public delegate* unmanaged<nint, byte*, int, int> scriptPoll;
    public delegate* unmanaged<nint, byte*, ulong> scriptEntityByKey;
    // (v21) The editor (null in a game).
    public FxEditorApi* editor;
    // (v22) Gamepads, text input.
    public FxGamepad* gamepads;
    public int gamepadCount;
    public int pad2;
    public delegate* unmanaged<nint, int, float, float, uint, int> gamepadRumble;
    public delegate* unmanaged[SuppressGCTransition]<nint, byte*, int, int> textInput;
    public delegate* unmanaged<nint, int, void> textMode;
    // (v23) Game time, quitting, the view's field of view.
    public delegate* unmanaged[SuppressGCTransition]<nint, float, float> timeScale;
    public float realDelta;
    public uint pad3;
    public delegate* unmanaged<nint, void> quit;
    public delegate* unmanaged[SuppressGCTransition]<nint, float> cameraFov;
    // (v24) Render settings, debug lines.
    public delegate* unmanaged<nint, byte*, float, int, float> renderSetting;
    public delegate* unmanaged[SuppressGCTransition]<nint, float*, float*, uint, float, void> debugLine;
    // (v25) Animation read back, terrain, water, particle fields, the UI's state.
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, float> animGetFloat;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int, uint> animGetId;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, byte*, int> animInState;
    public delegate* unmanaged<nint, ulong, int, byte*, int, int> animStateName;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float, int, float> animRate;
    public delegate* unmanaged<nint, ulong, byte*, float, float, int, int> animPlay;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float> animPlaying;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, float*, int> animRootMotion;
    public delegate* unmanaged[SuppressGCTransition]<nint, float*, FxTerrainSample*, int> terrainSample;
    public delegate* unmanaged<nint, float*, float*, float, float*, int> terrainRaycast;
    public delegate* unmanaged<nint, int, byte*, int, int> terrainLayerName;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int> waterBody;
    public delegate* unmanaged[SuppressGCTransition]<nint, int, float*, float*, int> waterNearestSurface;
    public delegate* unmanaged<nint, ulong, byte*, int, int> vfxComponent;
    public delegate* unmanaged<nint, ulong, byte*, byte*, byte*, byte*, int, int> vfxField;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint> uiState;
    // (v26) Scenes.
    public delegate* unmanaged<nint, byte*, int, int> sceneCurrent;
    public delegate* unmanaged<nint, byte*, int> sceneLoad;
    // (v28) Loading in the background.
    public delegate* unmanaged<nint, byte*, int> sceneLoadAsync;
    public delegate* unmanaged[SuppressGCTransition]<nint, byte*, int, float> sceneLoading;
    // (v29) Materials at runtime.
    public delegate* unmanaged<nint, byte*, int> materialFind;
    public delegate* unmanaged[SuppressGCTransition]<nint, int, byte*, float*, int, int> materialParam;
    public delegate* unmanaged[SuppressGCTransition]<nint, ulong, int> materialInstance;
    // (v30) Post effects.
    public delegate* unmanaged<nint, byte*, int, int, uint> postCreate;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, byte*, float*, int, int> postSet;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, int, int, void> postState;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, void> postDestroy;
    // (v31) Runtime meshes.
    public delegate* unmanaged<nint, void*, uint, uint*, uint, uint> meshCreate;
    public delegate* unmanaged<nint, uint, void*, uint, uint*, uint, int> meshUpdate;
    public delegate* unmanaged<nint, uint, void> meshDestroy;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, float*, float*, float*, float, float, void> meshDraw;
    // (v34) Gizmo lines.
    public delegate* unmanaged[SuppressGCTransition]<nint, float*, float*, uint, void> gizmoLine;
    // (v35) Audio streams.
    public delegate* unmanaged<nint, uint, float, uint> streamCreate;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, float*, uint, uint> streamWrite;
    public delegate* unmanaged[SuppressGCTransition]<nint, uint, uint*, uint*, uint*, void> streamInfo;
    public delegate* unmanaged<nint, uint, float*, ulong, float, uint, uint> streamPlay;
    public delegate* unmanaged<nint, uint, void> streamDestroy;
    // (v35) Collision shapes from scripts.
    public delegate* unmanaged<nint, ulong, FxShape*, float*, int, int> physicsShapeAdd;
    public delegate* unmanaged<nint, ulong, void> physicsShapesClear;
    // (v38) Network transports.
    public delegate* unmanaged<nint, byte*, uint> netOpen;
    public delegate* unmanaged<nint, uint, void> netClose;
    public delegate* unmanaged<nint, uint, int, int> netListen;
    public delegate* unmanaged<nint, uint, byte*, uint> netConnect;
    public delegate* unmanaged<nint, uint, uint, int, byte*, int, int> netSend;
    public delegate* unmanaged<nint, uint, uint, byte*, void> netDisconnect;
    public delegate* unmanaged<nint, uint, FxNetEvent*, byte*, int, int> netPoll;
    public delegate* unmanaged<nint, uint, uint, FxNetStats*, void> netStats;
    public delegate* unmanaged<nint, uint, byte*, int, int> netCertificate;
    public delegate* unmanaged<nint, byte*, int, int> netLaunch;
    public delegate* unmanaged<nint, int, byte*, void> netLaunchClients;
    // (v39) A client's ticks kept ahead of the host's: a jump, and time dilation.
    public delegate* unmanaged<nint, ulong, void> simSetTick;
    public delegate* unmanaged<nint, float, void> simSetRate;
    // (v40) Where the simulation needs the world loaded.
    public delegate* unmanaged<nint, float*, int, void> simFocus;
    // (v40) A dedicated server's console line.
    public delegate* unmanaged<nint, byte*, int, int> serverCommand;
    // (v41) Steam: a steam_api64.dll function by name, and Steam's state.
    public delegate* unmanaged<nint, byte*, void*> steamProc;
    public delegate* unmanaged<nint, uint*, byte*, int, int> steamStatus;
    // (v42) Steam's callbacks, queued by the engine.
    public delegate* unmanaged<nint, void> steamPump;
    public delegate* unmanaged<nint, int*, void*, int, int> steamNextCallback;
    // (v44) The whole world's rollback: its window, a rollback, a past tick's physics hash.
    public delegate* unmanaged<nint, uint, void> simWindow;
    public delegate* unmanaged<nint, ulong, int> simRollback;
    public delegate* unmanaged<nint, ulong, ulong> simPhysicsHash;
    public delegate* unmanaged<nint, uint, void> simMaxTicks;
    // (v46) The level tools (LevelApi.cs), with their own version.
    public FxLevelApi* level;
}

/// <summary>(v38) A network event (script_api.h FxNetEvent).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FxNetEvent
{
    public int type;  // 1 connected, 2 disconnected, 3 message, 4 datagram
    public uint connection;
    public int channel;
    public int size;
}

/// <summary>(v38) A connection's numbers (script_api.h FxNetStats).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FxNetStats
{
    public float rttMs, lossPercent;
    public ulong bytesSent, bytesReceived;
    public uint maxDatagram, pad;
}

/// <summary>(v35) A collision shape from a script (script_api.h FxShape).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FxShape
{
    public uint kind;
    public System.Numerics.Vector3 center;
    public System.Numerics.Quaternion rotation;
    public System.Numerics.Vector3 size;
}

/// <summary>(v25) The terrain at a point (script_api.h FxTerrainSample).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct FxTerrainSample
{
    public float height;
    public float normalX, normalY, normalZ;
    public int layer, layerB;
    public float blend;
    public uint pad;
}

/// <summary>(v22) A gamepad this frame (script_api.h FxGamepad).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxGamepad
{
    public uint buttons, buttonsPrev;
    public fixed float axes[6];
    public uint id;
    public uint type;
}

/// <summary>(v21) The editor's API for editor code (script_api.h FxEditorApi).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxEditorApi
{
    public uint version;
    public uint size;
    public nint context;
    public delegate* unmanaged<nint, byte*, int, void> message;
    public delegate* unmanaged<nint, byte*, byte*, int, int> query;
    public delegate* unmanaged<nint, byte*, void> undoBegin;
    public delegate* unmanaged<nint, void> undoEnd;
    public delegate* unmanaged<nint, uint, ulong> entityOf;
    public delegate* unmanaged<nint, ulong, uint> idOf;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxBodyState
{
    public fixed float linear[3];
    public fixed float angular[3];
    public float mass, gravityScale, linearDamping, angularDamping;
    public uint type, locks, awake, bullet;
}

[StructLayout(LayoutKind.Sequential)]
public struct FxQueryFilter
{
    public uint layers;
    public uint pad;
    public ulong ignore;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxQueryShape
{
    public uint type;
    public fixed float size[3];
    public fixed float position[3];
    public fixed float rotation[4];
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxJointSettings
{
    public fixed float anchor[3];
    public fixed float axis[3];
    public fixed float anchorB[3];
    public uint limit;
    public float lower, upper;
    public uint motor;
    public float motorSpeed, motorMax, spring, damping;
    public uint collide;
}

/// <summary>(v18) flecs' ecs_ref_t, opaque (script_api.h FxRef).</summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxRef
{
    public fixed ulong data[6];
}

[StructLayout(LayoutKind.Sequential)]
public struct FxMouse
{
    public float dx, dy;
    public float wheel;
    public float x, y;
    public uint buttons, buttonsPrev;
    public uint locked;
    public float viewX, viewY, viewWidth, viewHeight;  // (v22)
}

[StructLayout(LayoutKind.Sequential)]
public struct FxWaterSample
{
    public int body;
    public float height;
    public float normalX, normalY, normalZ;
    public float flowX, flowY, flowZ;
    public float depth;
    public uint pad;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct FxManagedApi
{
    public delegate* unmanaged<int> start;
    public delegate* unmanaged<float, double, ulong, void> update;
    public delegate* unmanaged<byte*, int> reload;
    public delegate* unmanaged<void> stop;
    public delegate* unmanaged<ulong, byte*, Particle*, int, ParticleHit*, int, float, void> particleStep;
    public delegate* unmanaged<byte*, int, int> describe;
    public delegate* unmanaged<float, ulong, int, void> fixedUpdate;  // (v36) with the tick; (v37) flags
    public delegate* unmanaged<ulong, byte*, int, int> componentValues;
    public delegate* unmanaged<float, void> editorUpdate;
    public delegate* unmanaged<byte*, int> editorRun;
    public delegate* unmanaged<byte*, int, void> editorEvent;
    public delegate* unmanaged<byte*, int, int> editorDescribe;
    // (v27) Scenes one after another.
    public delegate* unmanaged<byte*, void> sceneUnloading;
    public delegate* unmanaged<byte*, int> sceneLoaded;
    // (v32) The frame's later phases.
    public delegate* unmanaged<int, float, void> phase;
    // (v33) The game's data components described.
    public delegate* unmanaged<byte*, int, int> dataTypes;
    // (v34) Gizmos and component buttons.
    public delegate* unmanaged<byte*, ulong, void> drawGizmos;
    public delegate* unmanaged<ulong, byte*, byte*, byte*, byte*, int, int> componentButton;
    // (v37) The networked simulation's history.
    public delegate* unmanaged<int, ulong, long> simControl;
    // (v45) The session's state as JSON.
    public delegate* unmanaged<byte*, int, int> netStatus;
}

public enum FxAccess : uint
{
    In = 0,
    InOut = 1,
    Not = 2,
}

internal static unsafe class Api
{
    internal static FxNativeApi* Native;
    internal static nint Ctx => Native->context;
}
