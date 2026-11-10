#pragma once

// C ABI between the engine and managed scripts (Falcon.Engine). Mirrored field for field in
// managed/Falcon.Engine/Native/NativeApi.cs; bump kFxScriptApiVersion on any change.
// Everything is plain function pointers and raw data: no marshaling, no allocations per call.
// ECS data is handed out as pointers into flecs columns, so scripts read and write in place.

#include "script/level_api.h"

#include <cstdint>

extern "C" {

inline constexpr uint32_t kFxScriptApiVersion = 46;

// (v17) A character handle (animCharacter): every animation call that takes a character's entity
// takes one instead, and skips finding the character (its skinned record) from the entity. The
// bit is flecs' pair flag, so no entity has it.
inline constexpr uint64_t kFxCharacterTag = 1ull << 63;

enum FxAccess : uint32_t {
    kFxAccessIn = 0,     // read only (does not mark the table changed)
    kFxAccessInOut = 1,  // read and write (marks the table changed => re-streamed to the GPU)
    kFxAccessNot = 2,    // entity must not have the component
};

// Frame timing (this process's frames, wall time) and CPU, memory and GPU use (core/system_stats.h:
// sampled twice a second). -1: unknown.
// The water at a point (water/water_system.h WaterSample): body -1 when there is none there.
struct FxWaterSample {
    int32_t body;
    float height;     // the surface's height (world y)
    float normal[3];
    float flow[3];    // the water's velocity (m/s)
    float depth;      // how far below the surface the point is (negative: above it)
    uint32_t pad;
};

// (v25) The terrain at a point (terrain/terrain.h): its height and slope, and its material there
// (two layers, by index in the terrain's layers, and how much of the second shows).
struct FxTerrainSample {
    float height;
    float normal[3];
    int32_t layer, layerB;
    float blend;  // 0..1: how much of layerB
    uint32_t pad;
};

// (v7) A hit between two things physics simulated this frame (as the physics sounds hear it).
// Entities: 0 for the scene's own geometry and the ground. Surfaces: what each side sounds like,
// a surface preset (assets/surface_sound.h) by its 1-based index, 0 when its own (no preset).
struct FxContact {
    uint64_t entityA, entityB;
    float point[3], normal[3];
    float speed;  // approach speed, m/s
    uint32_t surfaceA, surfaceB;
    uint32_t pad;
};

// (v8) An animation event of a character's last update: a clip's marker or footstep, or a state's
// event, weighted by how much its branch shows (blends, transitions).
struct FxAnimEvent {
    uint32_t id;         // FNV-1a of its name (Animation.Id)
    float weight;        // 0 .. 1
    float percent;       // how far through it (events that last), else 1
    uint8_t kind;        // 0 marker, 1 footstep, 2 state
    uint8_t flags;       // footstep: 1 left, 2 right; state: 4 entry, 8 exit, 16 fully in
    uint8_t active;      // 1: from the active branch (0: a transition's source)
    uint8_t pad;
};

// (v7) What is under a point: where a ray down hit, how far, what it sounds like, and whose it is.
struct FxSurfaceHit {
    float point[3], normal[3];
    float distance;
    uint32_t surface;  // a surface preset's 1-based index (0: its own)
    uint64_t entity;   // 0: the scene's geometry or the ground
};

// (v9) The mouse this frame: its motion and wheel since the last frame, where it is (pixels, the
// window's), and its buttons now and last frame (bits: 1 left, 2 right, 4 middle). In the editor
// only what happens over the game's view while it plays.
struct FxMouse {
    float dx, dy;
    float wheel;
    float x, y;
    uint32_t buttons, buttonsPrev;
    uint32_t locked;  // 1: hidden and held in place (mouseLock), motion still reported
    // (v22) The game's view in the window (the editor's game view; the whole window in a game), in
    // the same units as x, y: where the mouse is in it is (x - viewX, y - viewY).
    float viewX, viewY, viewWidth, viewHeight;
};

// (v22) A gamepad this frame: its buttons now and last frame (bit i: SDL_GamepadButton i: south,
// east, west, north, back, guide, start, left stick, right stick, left shoulder, right shoulder,
// d-pad up, down, left, right, ...) and its axes (left x, left y, right x, right y: -1 .. 1, +y down;
// left trigger, right trigger: 0 .. 1). In the editor only while the game plays.
// (v38) A network event (netPoll) and a connection's numbers (netStats).
struct FxNetEvent {
    int32_t type;  // 1 connected, 2 disconnected, 3 message, 4 datagram
    uint32_t connection;
    int32_t channel;
    int32_t size;
};
struct FxNetStats {
    float rttMs, lossPercent;
    uint64_t bytesSent, bytesReceived;
    uint32_t maxDatagram, pad;
};

struct FxGamepad {
    uint32_t buttons, buttonsPrev;
    float axes[6];
    uint32_t id;    // SDL's instance id (0: the slot is empty)
    uint32_t type;  // SDL_GamepadType (Xbox, PlayStation, Switch, ...)
};

// (v15) A particle's hit this frame (vfx/particle_system.h ParticleHit): where, how fast into the
// surface, its normal, which particle (its index this frame), and what it hit (0: the scene's
// geometry or the ground; surface: the ground's terrain layer + 1, else 0).
struct FxParticleHit {
    float point[3];
    float speed;
    float normal[3];
    uint32_t particle;
    uint64_t entity;
    uint32_t surface;
    uint32_t pad;
};

struct FxPerfStats {
    float frameMs;     // the last frame
    float frameMsAvg;  // over the last half second
    float frameMsMax;  // the slowest frame in it
    float fps;         // frames per second over it
    float cpuProcess, cpuSystem;  // percent of all cores
    float gpuProcess, gpuSystem;  // percent: the busiest engine type (Task Manager's figure)
    float ramProcessMB, ramUsedMB, ramTotalMB;  // process: in physical RAM (Task Manager's figure)
    float vramProcessMB, vramTotalMB;
    float ramProcessCommittedMB;                 // process: all its private memory, committed
};

// (v18) A cached reference to an entity's component (flecs' ecs_ref_t): refGet finds it again only
// when the entity's table changed, so a script reading the same component every frame skips the
// lookup. Opaque; zeroed is empty.
struct FxRef {
    uint64_t data[6];
};

// (v19) Physics. A body's state: velocities (m/s, rad/s), mass (kg), gravity scale, damping, its type
// (0 static, 1 kinematic, 2 dynamic), the motions it is locked out of (bits: 1, 2, 4 moving along x, y,
// z; 8, 16, 32 turning about them), awake, and continuous collision (bullet: fast and thin).
struct FxBodyState {
    float linear[3], angular[3];
    float mass, gravityScale, linearDamping, angularDamping;
    uint32_t type, locks, awake, bullet;
};
// Which of FxBodyState bodySet sets (bits).
enum FxBodySet : uint32_t {
    kFxBodyLinear = 1, kFxBodyAngular = 2, kFxBodyMass = 4, kFxBodyGravityScale = 8, kFxBodyDamping = 16,
    kFxBodyType = 32, kFxBodyLocks = 64, kFxBodyAwake = 128, kFxBodyBullet = 256,
};
// What scripts' queries hit: Collider layers (bit i: layer i; everything else is on 0), passing
// through one entity's shapes (0: none).
// (v35) A collision shape from a script, in its entity's space: kind 0 box (size: full size),
// 1 sphere (size[0]: radius), 2 capsule (size[0] radius, size[1] height, along Y), 3 convex hull
// (the points given with it); at center, turned by rotation (x, y, z, w).
struct FxShape {
    uint32_t kind;
    float center[3];
    float rotation[4];
    float size[3];
};

struct FxQueryFilter {
    uint32_t layers;
    uint32_t pad;
    uint64_t ignore;
};
// A shape to query with: type 0 box (size: half extents), 1 sphere (size[0]: radius), 2 capsule
// (size[0]: radius, size[1]: height, along its Y), at position3, turned by rotation4 (x, y, z, w).
struct FxQueryShape {
    uint32_t type;
    float size[3];
    float position[3];
    float rotation[4];
};
// A trigger something came into (begin 1) or went out of (0): the trigger's entity and the other's
// (0: the scene's ground or a cooked collider).
struct FxTrigger {
    uint64_t trigger, other;
    uint32_t begin, pad;
};
// Where a character's capsule ended up (its base), whether it stands on ground no steeper than its
// slope limit (normal, and whose: 0 the scene's), and whether something stopped it.
struct FxCharacterMove {
    float position[3];
    uint32_t grounded;
    float groundNormal[3];
    uint32_t blocked;
    uint64_t ground;
};
// A joint: type 0 fixed, 1 hinge, 2 ball, 3 slider, 4 distance, 5 wheel. anchor (world), axis (world:
// a hinge's or wheel's turning axis, a slider's), anchorB (distance: the point on b). Limits: a
// hinge's angles (radians), a slider's translations, a distance's lengths. Motor: speed (rad/s or
// m/s) and the most torque or force it uses. spring: hertz (0: rigid), damping ratio. collide: the
// two bodies still collide.
struct FxJointSettings {
    float anchor[3], axis[3], anchorB[3];
    uint32_t limit;
    float lower, upper;
    uint32_t motor;
    float motorSpeed, motorMax, spring, damping;
    uint32_t collide;
};

// (v21) The editor, for editor code (libraries' Editor/, a project's Scripts/Editor/: built into the
// editor's build of the game only). Null in a game.
struct FxEditorApi {
    uint32_t version;
    uint32_t size;  // sizeof(FxEditorApi)
    void* context;
    // An editor message, as the editor's page sends them ({"type": "editor.entity.create", ...};
    // ids: Hierarchy ids or names), handled at once, each change an undo step (or part of one).
    void (*message)(void* ctx, const char* utf8Json, int32_t length);
    // The editor's state as JSON: "entities" (the Hierarchy: [{id, name, parent, kind, alive}]),
    // "selection" ({id}), "project" ({dir, name, scene}), "playing". Copies it; length / -length.
    int32_t (*query)(void* ctx, const char* what, char* buffer, int32_t capacity);
    // The changes between begin and end are one undo step, named.
    void (*undoBegin)(void* ctx, const char* utf8Name);
    void (*undoEnd)(void* ctx);
    // A Hierarchy id (0: none) and the entity it is, both ways.
    uint64_t (*entityOf)(void* ctx, uint32_t id);
    uint32_t (*idOf)(void* ctx, uint64_t entity);
};

struct FxNativeApi {
    uint32_t version;
    uint32_t size;  // sizeof(FxNativeApi)
    void* context;

    void (*log)(void* ctx, int32_t level, const char* utf8);  // 0 info, 1 warning, 2 error

    // ECS. Component ids are looked up (or registered) by name; size/align must match.
    uint64_t (*componentId)(void* ctx, const char* name, uint32_t size, uint32_t align);  // 0 = mismatch
    uint64_t (*entityCreate)(void* ctx);
    void (*entityDestroy)(void* ctx, uint64_t entity);
    int32_t (*entityAlive)(void* ctx, uint64_t entity);
    void (*entitySet)(void* ctx, uint64_t entity, uint64_t component, const void* data, uint32_t size);
    const void* (*entityGet)(void* ctx, uint64_t entity, uint64_t component);  // nullptr = absent
    void (*entityRemove)(void* ctx, uint64_t entity, uint64_t component);

    // Queries: cached flecs queries, iterated one table (archetype) at a time.
    void* (*queryCreate)(void* ctx, const uint64_t* components, const uint32_t* access, uint32_t count);
    void (*queryDestroy)(void* ctx, void* query);
    // Begins (or continues) iterating; returns the row count of the next table, 0 when done.
    // columns[i] points at the table's column for term i (nullptr for kFxAccessNot terms).
    int32_t (*queryNext)(void* ctx, void* query, void** columns, const uint64_t** entities);
    void (*queryEnd)(void* ctx, void* query);  // stop iterating early

    // Mesh-space bounding sphere of a scene mesh (radius 0 for an unknown mesh).
    void (*meshBounds)(void* ctx, uint32_t mesh, float* center3, float* radius);

    // Camera of the current frame (world space).
    void (*cameraGet)(void* ctx, float* position3, float* forward3, float* right3, float* up3);

    // Physics. shape: 0 box (size = half extents), 1 sphere (size.x = radius). Returns the entity.
    uint64_t (*bodySpawn)(void* ctx, uint32_t shape, const float* position3, const float* size3,
                          const float* velocity3, uint32_t tint);
    void (*bodyImpulse)(void* ctx, uint64_t entity, const float* impulse3);
    void (*bodyVelocity)(void* ctx, uint64_t entity, float* velocity3);  // reads the linear velocity

    // Animation graphs of characters (skinned instances). Parameter index -1: no graph / unknown.
    int32_t (*animParameter)(void* ctx, uint64_t entity, const char* name);
    void (*animSetFloat)(void* ctx, uint64_t entity, int32_t parameter, float value);
    void (*animSetBool)(void* ctx, uint64_t entity, int32_t parameter, int32_t value);
    void (*animSetId)(void* ctx, uint64_t entity, int32_t parameter, const char* id);

    // Keyboard state by SDL scancode, this frame and last frame (1 = down).
    const uint8_t* keys;
    const uint8_t* keysPrev;
    int32_t keyCount;
    int32_t pad;

    // HTML UI (--ui): JSON messages to and from the page (window.falcon.postMessage / onMessage).
    // uiPoll copies the next page message (UTF-8, no terminator) and returns its length: 0 when
    // there is none, -length (message kept) when `capacity` is too small.
    void (*uiSend)(void* ctx, const char* utf8, int32_t length);
    int32_t (*uiPoll)(void* ctx, char* buffer, int32_t capacity);

    void (*perfStats)(void* ctx, FxPerfStats* out);
    // Text over the game's view (the editor's, while playing): lines, monospace. Empty clears it;
    // it stays until set again, and goes when the game stops.
    void (*hudSet)(void* ctx, const char* utf8, int32_t length);

    // Water (v5): the water at a world point (1: there is water over or under it).
    int32_t (*waterSample)(void* ctx, const float* position3, FxWaterSample* out);
    // (v6) Ripples: a disturbance of the water at world (x, z): radius (m), strength (m of height).
    void (*waterSplash)(void* ctx, const float* position3, float radius, float strength);

    // Audio (v7). A sound by project path (a .fsound, or a .wav / .ogg clip): its id (0: no path).
    uint32_t (*audioSound)(void* ctx, const char* utf8Path);
    // Plays a sound at position3, or following `entity` (non-zero), else 2D; volume and pitch
    // multiply the sound's. flags: 1 loop. Returns a handle, 0 when it did not play (no such
    // sound, cooldown, no audio). Every sound a script plays stops with the game.
    uint32_t (*audioPlay)(void* ctx, uint32_t sound, const float* position3, uint64_t entity, float volume, float pitch,
                          uint32_t flags);
    // A surface's impact (its sound, else synthesized), as physics would play it: surface a
    // preset's 1-based index; volume 0 .. 1.3 like a hit's loudness.
    uint32_t (*audioImpact)(void* ctx, uint32_t surface, const float* position3, float volume, float pitch);
    void (*audioStop)(void* ctx, uint32_t handle, float fadeSeconds);
    // volume < 0, pitch <= 0, position3 null: unchanged.
    void (*audioSet)(void* ctx, uint32_t handle, float volume, float pitch, const float* position3);
    int32_t (*audioPlaying)(void* ctx, uint32_t handle);
    // A bus's volume from the game (multiplying the project's): 0 master, 1 effects, 2 physics,
    // 3 ambience, 4 music, 5 UI. Back to 1 when the game stops.
    void (*audioBusVolume)(void* ctx, uint32_t bus, float volume);
    // This frame's hits (the last physics update's): copies up to `capacity`, returns how many
    // there are.
    int32_t (*contactPoll)(void* ctx, FxContact* out, int32_t capacity);
    // The surface under position3: a ray straight down, at most maxDistance. 1: hit.
    int32_t (*surfaceAt)(void* ctx, const float* position3, float maxDistance, FxSurfaceHit* out);
    // (v8) The animation events of a character's last update: copies up to `capacity`, returns how
    // many there are (0: none, or not a character).
    int32_t (*animEvents)(void* ctx, uint64_t entity, FxAnimEvent* out, int32_t capacity);
    // (v9) The mouse this frame (FxMouse).
    void (*mouseGet)(void* ctx, FxMouse* out);
    // (v9) 1: the cursor hidden and held (relative motion, for looking around); 0: free. Freed when
    // the game stops.
    void (*mouseLock)(void* ctx, int32_t locked);
    // (v9) The entity with this name (the editor's name for it: the Hierarchy's), 0 when none.
    uint64_t (*entityFind)(void* ctx, const char* utf8Name);
    // (v10) The same among `root`'s descendants (a character's bones: every soldier has a foot_l).
    uint64_t (*entityFindUnder)(void* ctx, uint64_t root, const char* utf8Name);
    // (v11) Hides a character's bone and what is below it (1), shows it again (0), or asks (-1):
    // first person's head (AnimationSystem::hideBone). Returns 1 when it is hidden (now), 0 when
    // shown, -1 when the character has no such bone.
    int32_t (*animHideBone)(void* ctx, uint64_t entity, const char* utf8Bone, int32_t hidden);
    // (v12) Inverse kinematics (AnimationSystem). Foot IK's weight over the graph's (0 off: in
    // the air); 1, or 0 when the character has no legs to find.
    int32_t (*animFootIk)(void* ctx, uint64_t entity, float weight);
    // Looking at target3 (world) with the spine up to `utf8Tip` (null: "head"); weight 0 stops.
    // 1, or 0 when there is no such bone.
    int32_t (*animLookAt)(void* ctx, uint64_t entity, const float* target3, float weight, const char* utf8Tip);
    // `utf8Bone` (a hand) reaching target3 (world) with its parent and grandparent; weight 0
    // stops. 1, or 0 when there is no such bone (or already 4 reaching).
    int32_t (*animReach)(void* ctx, uint64_t entity, const char* utf8Bone, const float* target3, float weight);
    // (v13) `utf8Bone` (null: "hand_r"; a hand holding a weapon) aiming at target3 (world): the
    // spine above it turns until axis3 (in the bone's space: the barrel; null or zero: ahead in the
    // graph's pose) points there; weight 0 stops. With hold3 (world; null: none) the bone is brought
    // there by its arm, still aiming, and `utf8Support` (the other hand; null: none) keeps its grip.
    // 1, or 0 when there is no such bone or no spine above it.
    int32_t (*animAimAt)(void* ctx, uint64_t entity, const float* target3, float weight, const char* utf8Bone, const float* axis3,
                         const float* hold3, const char* utf8Support);
    // (v14) The first surface along a ray from from3 along direction3 (any length), at most
    // maxDistance metres: 1 and *out (distance: from from3), else 0.
    int32_t (*raycast)(void* ctx, const float* from3, const float* direction3, float maxDistance, FxSurfaceHit* out);
    // (v15) A prefab (.fprefab, project path) made at position3 turned by rotation4 (a quaternion x, y,
    // z, w; null: none), under `parent` (0: a root). Its empties, lights, cameras, audio, particle
    // effects, decals, bodies and meshes (from the pools reserved when the scene was uploaded:
    // vfx/runtime_meshes.h). Returns its root, 0 when it could not. The game made it: the editor's
    // Stop deletes it.
    uint64_t (*prefabSpawn)(void* ctx, const char* utf8Path, const float* position3, const float* rotation4, uint64_t parent);
    // (v15) A particle effect (`entity`, or its nearest effect up the tree): 0 play, 1 stop
    // emitting, 2 stop and clear, 3 restart. 1, or 0 when there is no effect.
    int32_t (*particlesControl)(void* ctx, uint64_t entity, int32_t op);
    // Starts `count` particles now (from its first emitter's shape); returns how many started.
    int32_t (*particlesEmit)(void* ctx, uint64_t entity, int32_t count);
    // How many particles it has (shown: not those still delayed); -1 when there is no effect.
    int32_t (*particlesCount)(void* ctx, uint64_t entity);
    // This frame's hits of its particles: copies up to `capacity`, returns how many there are.
    int32_t (*particlesHits)(void* ctx, uint64_t entity, FxParticleHit* out, int32_t capacity);
    // A ParticleScript on `entity` naming a controller class (the game's ParticleController);
    // its particles go through FxManagedApi::particleStep while the game runs.
    void (*particlesScript)(void* ctx, uint64_t entity, const char* utf8Class);
    // (v15) A decal of `utf8Material` (.fmat) at point3, projected against normal3 (its +Y along it),
    // turned `angle` degrees about it, size metres across, `depth` deep; gone after `lifetime`
    // seconds (0: until cleared), following `attach` (0: none). 1, or 0 when it could not.
    int32_t (*decalPlace)(void* ctx, const char* utf8Material, const float* point3, const float* normal3, float size, float depth,
                          float angle, float lifetime, uint64_t attach);
    void (*decalsClear)(void* ctx);
    // (v16) A character's entity parameter `utf8Name` (its graph's, which its IK follows): op 0 asks,
    // 1 sets it to `value` (0: none; a script's, over the Animator's binding until Stop), 2 puts the
    // Animator's binding back. Returns the entity it is now (0: none), ~0 when the character has no
    // such parameter.
    uint64_t (*animEntity)(void* ctx, uint64_t entity, const char* utf8Name, int32_t op, uint64_t value);
    // (v17) The character an entity is (its own skinned mesh, or a model's root: its first below) as
    // a handle (kFxCharacterTag | its record), 0 when it is none. Valid while the scene is loaded.
    uint64_t (*animCharacter)(void* ctx, uint64_t entity);

    // (v18) Entities.
    // Enabled: op 0 asks, 1 enables what the game disabled with it, 2 disables it and every entity
    // below it (Parent) that is enabled now. Disabled entities are not drawn, their bodies stop, and
    // queries skip them; the editor's Stop enables them again. Returns 1 enabled, 0 disabled, -1 dead.
    int32_t (*entityEnabled)(void* ctx, uint64_t entity, int32_t op);
    // Its name (the Hierarchy's; a game's scene edits', else its cooked name), or one the game gave
    // (until the editor's Stop): copies it (UTF-8, no terminator) and returns its length, -length
    // when `capacity` is short.
    int32_t (*entityName)(void* ctx, uint64_t entity, char* buffer, int32_t capacity);
    void (*entitySetName)(void* ctx, uint64_t entity, const char* utf8);
    // Its parent (0: a root); under another (0: a root again), keeping where it is (keepWorld 1) or
    // taking its world transform as its local one. 0 when it can't be (the parent below it).
    uint64_t (*entityParent)(void* ctx, uint64_t entity);
    int32_t (*entitySetParent)(void* ctx, uint64_t entity, uint64_t parent, int32_t keepWorld);
    // Its children: copies up to `capacity`, returns how many there are.
    int32_t (*entityChildren)(void* ctx, uint64_t entity, uint64_t* out, int32_t capacity);
    // Moves it in the world (rows12: WorldTransform's 3x4 rows); its local transform follows.
    void (*entitySetWorld)(void* ctx, uint64_t entity, const float* rows12);
    // An entity's own mesh row: what 0 visible, 1 casts shadows, 2 tint (RGBA8, 0: none). 0 when it
    // draws none of its own (not a mesh, or a spawned body). The editor's Stop puts them back.
    int32_t (*renderGet)(void* ctx, uint64_t entity, int32_t what, uint32_t* value);
    int32_t (*renderSet)(void* ctx, uint64_t entity, int32_t what, uint32_t value);
    // The material it draws with: an .fmat (project path), null for the mesh's own. 0 when it can't.
    int32_t (*renderMaterial)(void* ctx, uint64_t entity, const char* utf8Material);
    // A mesh of the scene (by name) at rows12 (world), with a material (null: its own), under
    // `parent` (0: a root), from the slots reserved when the scene was uploaded
    // (vfx/runtime_meshes.h: the meshes of prefabs and Model Renderers). 0 when it has none free.
    uint64_t (*meshSpawn)(void* ctx, const char* utf8Mesh, const float* rows12, const char* utf8Material, uint64_t parent);
    // Cached component references (FxRef).
    void (*refInit)(void* ctx, FxRef* ref, uint64_t entity, uint64_t component);
    const void* (*refGet)(void* ctx, FxRef* ref, uint64_t component);  // nullptr: absent

    // (v19) Physics. An entity's body (one Physics.Spawn made; a scene entity's PhysicsBody or
    // Collider while the game runs): its state (0: it has none), and what `mask` (FxBodySet) says set.
    int32_t (*bodyGet)(void* ctx, uint64_t entity, FxBodyState* out);
    int32_t (*bodySet)(void* ctx, uint64_t entity, const FxBodyState* in, uint32_t mask);
    // Pushes it: force3 at point3 (world; null: its centre of mass) and torque3 (null: none). mode 0:
    // a force (N) and torque (N m) for this step; 1: an impulse (N s) and angular impulse.
    int32_t (*bodyPush)(void* ctx, uint64_t entity, const float* force3, const float* point3, const float* torque3, int32_t mode);
    // Moves it there at once (nothing swept on the way; rotation4 null: as it is), its entity with it.
    int32_t (*bodyTeleport)(void* ctx, uint64_t entity, const float* position3, const float* rotation4);
    // Builds its body again from its PhysicsBody and Collider as they are now (setting or removing
    // either does this by itself at the next step).
    int32_t (*bodyRebuild)(void* ctx, uint64_t entity);
    // The world's gravity: copied to get3 (non-null), then set3 (non-null) set. Stop puts it back.
    void (*physicsGravity)(void* ctx, float* get3, const float* set3);
    // Queries: the first surface along a ray, or swept by a shape (along direction3 for at most
    // maxDistance; distance: how far the shape went), and the entities overlapping a shape (copies up
    // to `capacity`, returns how many). Triggers are never hit.
    int32_t (*raycastFiltered)(void* ctx, const float* from3, const float* direction3, float maxDistance, const FxQueryFilter* filter,
                               FxSurfaceHit* out);
    int32_t (*shapeCast)(void* ctx, const FxQueryShape* shape, const float* direction3, float maxDistance, const FxQueryFilter* filter,
                         FxSurfaceHit* out);
    int32_t (*overlap)(void* ctx, const FxQueryShape* shape, const FxQueryFilter* filter, uint64_t* out, int32_t capacity);
    // This frame's trigger events (the last physics update's): copies up to `capacity`, returns how many.
    int32_t (*triggerPoll)(void* ctx, FxTrigger* out, int32_t capacity);
    // A character's capsule (base at position3) moved by delta3: slides along what it meets, climbs
    // steps up to stepHeight, keeps to the ground it walks down, ground steeper than slopeLimit
    // (degrees) a wall. `entity`'s own shapes are passed through (0: none); nothing is moved.
    int32_t (*characterMove)(void* ctx, uint64_t entity, const float* position3, float radius, float height, float stepHeight,
                             float slopeLimit, const float* delta3, const FxQueryFilter* filter, FxCharacterMove* out);
    // Joints between two entities' bodies (b 0: the world): a handle, 0 when they have no bodies.
    // Gone with either body, jointDestroy, or the editor's Stop.
    uint64_t (*jointCreate)(void* ctx, int32_t type, uint64_t a, uint64_t b, const FxJointSettings* settings);
    int32_t (*jointDestroy)(void* ctx, uint64_t joint);
    void (*jointMotor)(void* ctx, uint64_t joint, float speed, float maxForce);

    // (v20) Script components to make: the scene's when the game starts, a spawned prefab's since.
    // Copies them as a JSON array [{"entity", "class", "enabled", "properties": {...}}] (UTF-8, no
    // terminator) and returns its length, taking them; -length when `capacity` is short (kept); 0: none.
    int32_t (*scriptPoll)(void* ctx, char* buffer, int32_t capacity);
    // The entity a script's Entity property names (the scene edits' key for it), 0 when none.
    uint64_t (*scriptEntityByKey)(void* ctx, const char* utf8Key);
    // (v21) The editor (null in a game, or before the editor is up).
    const FxEditorApi* editor;

    // (v22) Gamepads, as connected (8 slots, kept while connected: a slot's index is stable).
    const FxGamepad* gamepads;
    int32_t gamepadCount;  // slots (8)
    int32_t pad2;
    // Rumble: low and high frequency motors 0 .. 1, for `ms`. 1: done, 0: no such gamepad or no rumble.
    int32_t (*gamepadRumble)(void* ctx, int32_t slot, float low, float high, uint32_t ms);
    // Text typed this frame while text input is on (textMode 1; 0 off): copies it (UTF-8), returns
    // its length (-length when `capacity` is short). In the editor only over the game's view.
    int32_t (*textInput)(void* ctx, char* buffer, int32_t capacity);
    void (*textMode)(void* ctx, int32_t on);

    // (v23) Game time: the scale of the game's time (scripts' dt, animation, physics, particles; 0:
    // paused), set when `scale` >= 0; returns it. The editor's Stop puts it back to 1. realDelta: the
    // last frame's seconds, unscaled.
    float (*timeScale)(void* ctx, float scale);
    float realDelta;
    uint32_t pad3;
    // Ends the game: a game's process quits; in the editor, Play stops.
    void (*quit)(void* ctx);
    // The view's camera: its vertical field of view, degrees (the main camera's, or the editor's).
    float (*cameraFov)(void* ctx);

    // (v24) Render settings, by the Lighting panel's names: exposure, manualExposure (0: auto),
    // bloom, ao (0 off, 1 GTAO, 2 SSAO), aoRadius, aoPower, lookPower, lookSaturation, gi (0 / 1),
    // giIntensity, skyIntensity, reflections (0 / 1), reflectIntensity, fxaa, taa, renderScale. Set when
    // `set` is 1; returns the value (NaN: no such setting). The editor's Stop puts them all back.
    float (*renderSetting)(void* ctx, const char* utf8Key, float value, int32_t set);
    // Debug lines in the world (a to b, RGBA8), shown for `seconds` (0: this frame) over the editor's
    // views (none in a game: debug drawing is the editor's).
    void (*debugLine)(void* ctx, const float* a3, const float* b3, uint32_t color, float seconds);

    // (v25) Animation, read back: a parameter's value (bools 0 / 1; an id's hash as its bits: animGetId).
    float (*animGetFloat)(void* ctx, uint64_t entity, int32_t parameter);
    uint32_t (*animGetId)(void* ctx, uint64_t entity, int32_t parameter);
    // Whether a state machine of the graph (one its last update ran) is in the state named; the
    // name of the active state of the n-th such machine (outer first; a cooked pak's graph has none),
    // copied as UTF-8 (returns its length; negative: the buffer is too small, -length).
    int32_t (*animInState)(void* ctx, uint64_t entity, const char* utf8State);
    int32_t (*animStateName)(void* ctx, uint64_t entity, int32_t n, char* buffer, int32_t capacity);
    // How fast the character's animation runs (1: as authored; set when `set` is 1); returns it.
    float (*animRate)(void* ctx, uint64_t entity, float rate, int32_t set);
    // A clip (by name) played over the graph, fading in over `fade` seconds, looping or once; a null
    // or empty name fades it out. 0: no such clip. animPlaying: seconds into it (-1: none).
    int32_t (*animPlay)(void* ctx, uint64_t entity, const char* utf8Clip, float speed, float fade, int32_t loop);
    float (*animPlaying)(void* ctx, uint64_t entity);
    // The root motion of the character's last update (character space): translation (3), rotation (4).
    int32_t (*animRootMotion)(void* ctx, uint64_t entity, float* out7);

    // Terrain: the surface at a point's x, z (0: no terrain there), a ray's hit (t along dir), and a
    // material layer's name (as animStateName).
    int32_t (*terrainSample)(void* ctx, const float* position3, FxTerrainSample* out);
    int32_t (*terrainRaycast)(void* ctx, const float* origin3, const float* direction3, float maxDistance, float* t);
    int32_t (*terrainLayerName)(void* ctx, int32_t layer, char* buffer, int32_t capacity);

    // Water: the body an entity is (-1: none), and the point of a body's still surface nearest a point.
    int32_t (*waterBody)(void* ctx, uint64_t entity);
    int32_t (*waterNearestSurface)(void* ctx, int32_t body, const float* position3, float* out3);

    // Particle and decal components (vfx/vfx_components.h, by name "ParticleEffect" or key
    // "particleEffect"): op 0 has, 1 add (its defaults), 2 remove; returns whether it is there now.
    int32_t (*vfxComponent)(void* ctx, uint64_t entity, const char* utf8Component, int32_t op);
    // A field of one (its key, as in the scene file): sets it from JSON when `json` is not null, then
    // copies its value as JSON (as animStateName). -1 (or a negative length): no such component on
    // the entity or field, or the value doesn't fit it.
    int32_t (*vfxField)(void* ctx, uint64_t entity, const char* utf8Component, const char* utf8Field, const char* json,
                        char* buffer, int32_t capacity);

    // The game's HTML UI: 1 the page has the pointer (over a control), 2 the keyboard (a text field).
    uint32_t (*uiState)(void* ctx);

    // (v26) Scenes: the scene playing (its path in the project, "Scenes/Main.fpak"; as animStateName),
    // and another to play (after this frame, in its place: the game goes on, v27; the editor stops
    // playing). 0: no such scene.
    int32_t (*sceneCurrent)(void* ctx, char* buffer, int32_t capacity);
    int32_t (*sceneLoad)(void* ctx, const char* utf8Scene);

    // (v28) A scene loaded in the background (cooked if it needs it, then read) while this one runs,
    // in this one's place once it is read (0: no such scene); and how that goes: its progress
    // (0..1, rough; -1: nothing loading) and its stage ("cooking: meshes", "reading", "ready"),
    // copied as sceneCurrent.
    int32_t (*sceneLoadAsync)(void* ctx, const char* utf8Scene);
    float (*sceneLoading)(void* ctx, char* stage, int32_t capacity);

    // (v29) Materials at runtime. A material asset's index (-1: not in this scene, or no slot for
    // it). A parameter by name, read into `values4` or set from it (`set` 1): baseColor (4),
    // emissive (3, its colour), emissiveStrength, roughness, metallic, normalScale,
    // occlusionStrength, alphaCutoff; every user of the material changes. Returns how many values
    // the parameter has (0: no such material or parameter).
    int32_t (*materialFind)(void* ctx, const char* utf8Source);
    int32_t (*materialParam)(void* ctx, int32_t material, const char* utf8Name, float* values4, int32_t set);
    // An entity's own copy of its material (its material, else its mesh's first), which it draws
    // with from now: the copy's index (-1: not a mesh, or no runtime slot left). The editor's Stop
    // puts back every material scripts changed and frees the copies.
    int32_t (*materialInstance)(void* ctx, uint64_t entity);

    // (v30) Post effects: a project shader (its path, "Shaders/Grey.slang", compiled by the editor
    // into Cache/Shaders) run on the frame each frame, before the tonemap (stage 0: the HDR frame)
    // or after it (1: the display frame), in `order` (lower first). 0: no such compiled compute
    // shader. Its uniform block's fields set by name (floats; ints and uints converted): returns how
    // many values it took (0: no such field). Enabled and order changed; destroyed. The editor's
    // Stop and a game's end remove the game's.
    uint32_t (*postCreate)(void* ctx, const char* utf8Shader, int32_t stage, int32_t order);
    int32_t (*postSet)(void* ctx, uint32_t effect, const char* utf8Name, const float* values, int32_t count);
    void (*postState)(void* ctx, uint32_t effect, int32_t enabled, int32_t order);
    void (*postDestroy)(void* ctx, uint32_t effect);

    // (v31) Meshes made at runtime: vertices (position 3, normal 3, uv 2, colour 4 floats: 48 bytes
    // each) and triangles (3 indices each): its id (0: failed). Update replaces them (any sizes);
    // Destroy frees it. The editor's Stop and a game's end free the game's.
    uint32_t (*meshCreate)(void* ctx, const void* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);
    int32_t (*meshUpdate)(void* ctx, uint32_t mesh, const void* vertices, uint32_t vertexCount, const uint32_t* indices,
                          uint32_t indexCount);
    void (*meshDestroy)(void* ctx, uint32_t mesh);
    // Drawn this frame (each frame it shows): its transform (System.Numerics' Matrix4x4, 16
    // floats), colour (rgba; alpha under 1 blends it), emissive (rgb, strength), roughness, metallic.
    void (*meshDraw)(void* ctx, uint32_t mesh, const float* world16, const float* color4, const float* emissive4, float roughness,
                     float metallic);
    // (v34) A gizmo line this editor frame (Gizmos.*, from drawGizmos): over the editor's views.
    void (*gizmoLine)(void* ctx, const float* a3, const float* b3, uint32_t color);
    // (v35) Audio streams: made with 1 or 2 channels and room for `seconds` (its id; 0: no audio);
    // frames written (as many as there is room for: how many); queued and free frames and the
    // underruns so far; played at position3, on an entity, else 2D, on a bus (its sound handle:
    // audioStop / audioSet / audioPlaying take it); destroyed (stopped, freed once the mixer is done
    // with it). The game's end destroys them.
    uint32_t (*streamCreate)(void* ctx, uint32_t channels, float seconds);
    uint32_t (*streamWrite)(void* ctx, uint32_t stream, const float* interleaved, uint32_t frames);
    void (*streamInfo)(void* ctx, uint32_t stream, uint32_t* queued, uint32_t* free, uint32_t* underruns);
    uint32_t (*streamPlay)(void* ctx, uint32_t stream, const float* position3, uint64_t entity, float volume, uint32_t bus);
    void (*streamDestroy)(void* ctx, uint32_t stream);
    // (v35) Collision shapes from scripts in place of the entity's Collider shape (a compound when
    // several; its body built again at the next step): added (0: not a shape, or no physics),
    // cleared (its Collider's again). The game's end clears them.
    int32_t (*physicsShapeAdd)(void* ctx, uint64_t entity, const FxShape* shape, const float* points3, int32_t pointCount);
    void (*physicsShapesClear)(void* ctx, uint64_t entity);
    // (v38) Network transports (engine/net/net_transport.h) for Falcon.Network's sessions:
    // - netOpen: one for an address's scheme ("quic://...", "loopback://..."), with the command
    //   line's --net-sim conditions around it (its handle; 0: none, e.g. no msquic.dll).
    // - netListen: accept connections on `port` (0: any): the port bound, -1 when it can't.
    // - netConnect: a connection to `address` (without its scheme); its event follows.
    // - netSend: a reliable message on `channel` 0..3, or (channel -1) a datagram; 0 when refused.
    // - netPoll: the next event (0: none): its type 1 connected, 2 disconnected (data: why), 3
    //   message, 4 datagram; data copied into `buffer` (event->size; when it doesn't fit, -1 and
    //   the event stays: poll again with event->size bytes).
    // - netLaunch: what the command line asks of the game's network as JSON ({"host": port},
    //   {"connect": "quic://..."}, "clients": n, "verify": true; "{}": nothing); length / -length.
    // - netLaunchClients: n more processes of this game (the same scene and script, hidden)
    //   connecting to `address`; the host's run fails if one of them fails (--net-clients).
    uint32_t (*netOpen)(void* ctx, const char* utf8Address);
    void (*netClose)(void* ctx, uint32_t transport);
    int32_t (*netListen)(void* ctx, uint32_t transport, int32_t port);
    uint32_t (*netConnect)(void* ctx, uint32_t transport, const char* utf8Address);
    int32_t (*netSend)(void* ctx, uint32_t transport, uint32_t connection, int32_t channel, const uint8_t* data, int32_t size);
    void (*netDisconnect)(void* ctx, uint32_t transport, uint32_t connection, const char* utf8Reason);
    int32_t (*netPoll)(void* ctx, uint32_t transport, FxNetEvent* event, uint8_t* buffer, int32_t capacity);
    void (*netStats)(void* ctx, uint32_t transport, uint32_t connection, FxNetStats* out);
    int32_t (*netCertificate)(void* ctx, uint32_t transport, char* buffer, int32_t capacity);
    int32_t (*netLaunch)(void* ctx, char* buffer, int32_t capacity);
    void (*netLaunchClients)(void* ctx, int32_t count, const char* utf8Address);
    // (v39) A client keeping its ticks ahead of the host's (prediction): the next tick's number
    // (ticks then count on from it: big gaps), and how fast its ticks run against real time (time
    // dilation: small drift, about 0.9 to 1.1; (v44) 0: no ticks, a rollback session waiting for
    // inputs).
    void (*simSetTick)(void* ctx, uint64_t tick);
    void (*simSetRate)(void* ctx, float rate);
    // (v40) Where the simulation needs the world loaded (the networked characters: a server has no
    // camera): terrain tiles and their collision stream around these points (and the view's).
    void (*simFocus)(void* ctx, const float* points3, int32_t count);
    // (v40) A dedicated server's console: the next line typed on its standard input (length; 0:
    // none; -length when `capacity` is short, the line kept).
    int32_t (*serverCommand)(void* ctx, char* buffer, int32_t capacity);
    // (v41) Steam (engine/steam/steam_client.h): a function of steam_api64.dll by name (null: Steam
    // isn't running, or no such function), and Steam's state: 1 running, 0 off (no App ID), -1
    // failed; its App ID, and why it failed (copied with a NUL, cut to fit).
    void* (*steamProc)(void* ctx, const char* name);
    int32_t (*steamStatus)(void* ctx, uint32_t* appId, char* error, int32_t capacity);
    // (v42) Steam's callbacks, queued by the engine (engine/steam/steam_client.h): pump, then each
    // one's id and bytes (its size; 0: none left; -size when `capacity` is short, it kept). A call
    // result is SteamAPICallCompleted_t's 16 bytes, a failed flag (8), then the result.
    void (*steamPump)(void* ctx);
    int32_t (*steamNextCallback)(void* ctx, int32_t* id, void* buffer, int32_t capacity);
    // (v44) The whole world's rollback (Network.Mode Rollback): keep the last `ticks` ticks' state,
    // physics included (0: none); the world back `ticksBack` ticks and simulated again to now, each
    // tick with its kept input (1: done; 0: not that far back, or not kept); a past tick's physics
    // hash (after its step; 0: not kept).
    void (*simWindow)(void* ctx, uint32_t ticks);
    int32_t (*simRollback)(void* ctx, uint64_t ticksBack);
    uint64_t (*simPhysicsHash)(void* ctx, uint64_t ticksBack);
    // (v44) Ticks the coming frames may run at most (a rollback session not to get further ahead of
    // the inputs in than it can roll back; ~0: no limit).
    void (*simMaxTicks)(void* ctx, uint32_t ticks);
    // (v46) The level tools (level_api.h: live meshes), with their own version.
    const FxLevelApi* level;
};

struct FxManagedApi {
    int32_t (*start)();                                     // create the game, OnStart; 0 = ok
    void (*update)(float dt, double time, uint64_t frame);  // once per frame
    int32_t (*reload)(const char* gamePathUtf8);            // JIT: 0 ok, 1 load failed, 2 old code not unloaded
    void (*stop)();
    // (v15) A ParticleScript's particles this frame, after the native step (`script`: the entity
    // with the ParticleScript; `className` its class): particles as vfx/particle_system.h's Particle
    // (128 bytes; New, Hit and Dead flagged), and the hits. Scripts change them in place.
    void (*particleStep)(uint64_t script, const char* className, void* particles, int32_t count, const FxParticleHit* hits, int32_t hitCount,
                         float dt);
    // (v20) The game's script component classes as JSON [{name, label, properties: [{name, kind, min,
    // max, asset, enums, tooltip, group}], defaults: {...}}] (the editor's Inspector and Add
    // Component): copied, its length returned (-length when `capacity` is short).
    int32_t (*describe)(char* buffer, int32_t capacity);
    // Each physics step while the game runs (the simulation's tick): the components' OnFixedUpdate,
    // with the tick's input. (v36) `tick`: ticks run since the game started (Time.Tick). (v37)
    // flags 1: a tick simulated again after a rollback (only the networked objects' components run;
    // the state after it is checked against the first time's). (v43) flags 2 with 1: inside physics'
    // step simulated again (the whole world's rollback): the state is kept and checked after the
    // step and its events (phase 6), not at the tick's end.
    void (*fixedUpdate)(float dt, uint64_t tick, int32_t flags);
    // A running entity's components' property values: a JSON array by its scene index (null for
    // ones added in code), as describe() writes them; length / -length / 0 when it has none.
    int32_t (*componentValues)(uint64_t entity, char* buffer, int32_t capacity);
    // (v21) Editor code: each editor frame (editing or playing), a menu item run ("Tools/Align"),
    // an editor event ({"type": "selection" | "play" | "stop" | "saved"}), and what it adds to the
    // editor as JSON ({"menus": [{path, shortcut}]}; length / -length).
    void (*editorUpdate)(float dt);
    int32_t (*editorRun)(const char* utf8Menu);
    void (*editorEvent)(const char* utf8Json, int32_t length);
    int32_t (*editorDescribe)(char* buffer, int32_t capacity);
    // (v27) Scenes one after another while the game runs (Scene.Load): the scene going (its path in
    // the project; the game's OnSceneUnloading, its components' OnDestroy, while its world is still
    // there), then the next one in place (its components made, the game's OnSceneLoaded; 0 = ok).
    void (*sceneUnloading)(const char* utf8Scene);
    int32_t (*sceneLoaded)(const char* utf8Scene);
    // (v32) The frame's later phases while the game runs: 3 after physics, 4 late (animation and
    // physics done), 5 before the render. (v36) 6: after each tick's physics step, inside the tick
    // (its collision and trigger events; contactPoll / triggerPoll are the step's then).
    void (*phase)(int32_t phase, float dt);
    // (v33) The game's data components (its [Component] structs) as JSON [{name, label, size, align,
    // fields: [{name, kind, offset}]}] (the editor's Inspector and Add Component; length / -length).
    int32_t (*dataTypes)(char* buffer, int32_t capacity);
    // (v34) Each editor frame: the components' OnDrawGizmos (OnDrawGizmosSelected too on the
    // selected entity's) and the editor tools'. While editing, the scene's script components as
    // scriptPoll writes them ([{entity, class, enabled, properties}]) when they changed since the
    // last call (null: as before); while playing the running ones draw.
    void (*drawGizmos)(const char* utf8Scene, uint64_t selected);
    // A component's [Button] method run: while playing on the running one; while editing on one made
    // from `properties` (scriptPoll's form), its properties after written back as JSON (length /
    // -length; 0: none, or it failed).
    int32_t (*componentButton)(uint64_t entity, const char* utf8Class, const char* utf8Method, const char* utf8Properties, char* buffer,
                               int32_t capacity);
    // (v37) The networked simulation's history (Falcon.SimHistory): op 0 restore the state after
    // tick `arg` (1: done, 0: not kept), 1 ticks simulated again that came out differently, 2 keep
    // `arg` ticks (0: none), 3 ticks simulated again and checked.
    int64_t (*simControl)(int32_t op, uint64_t arg);
    // (v45) The session's state as JSON (Falcon.Network.Status: the editor's Network tab; length /
    // -length when `capacity` is short; 0: none).
    int32_t (*netStatus)(char* buffer, int32_t capacity);
};

// Entry point of both script modes:
//   JIT (CoreCLR): Falcon.Host.Initialize in Falcon.Engine.dll; gamePath = the game assembly.
//   AOT (NativeAOT): exported as falcon_script_init from the game's native DLL; gamePath = null.
typedef int32_t (*FxScriptInit)(const FxNativeApi* native, FxManagedApi* managed, const char* gamePathUtf8);

}  // extern "C"
