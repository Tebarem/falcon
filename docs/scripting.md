# C# scripting

Games are C# (`managed/`): `Falcon.Engine` is the API, and `Sandbox` is the sample game. The build
produces each game twice from the same source:

| mode | file | use |
|---|---|---|
| JIT | `bin/scripts/jit/Sandbox.dll` (IL) | development: CoreCLR hosted via hostfxr, hot reload |
| NativeAOT | `bin/scripts/aot/Sandbox.dll` (native, ~1 MB) | shipping: no runtime, no JIT |

```powershell
./build/release/bin/falcon.exe --scene samples/cooked/sponza.fpak --script build/release/bin/scripts/jit/Sandbox.dll
```

- **One C ABI** (`engine/script/script_api.h`): plain function pointers both ways, no marshaling
  and no per-call allocations. The engine calls `start`, `update` (once per frame, before
  physics) and `reload`. Scripts call back for the ECS, input, camera, physics, logging, performance numbers and a HUD.
  The mode is picked from the file: an image with a CLI header is JIT.
- **ECS in place:**
  - Shared components (`WorldTransform`, `MeshInstance`, `RigidBody`) come from one schema
    (`engine/scene/components.fschema`). `falcon_schemagen` turns it into the C++ header and
    the C# structs.
  - Scripts can also define their own components as plain structs, registered by name on
    first use.
  - Queries are cached flecs queries. C# gets one `Span<T>` per table column, pointing straight
    into flecs storage.
  - Writes through `InOut` terms mark the table changed, and a render-extraction query
    re-streams changed tables' transforms to the GPU. Moved instances also invalidate their
    shadow pages.
  - Component layouts are checked by size and alignment on lookup, so a mismatch fails loudly.
- **Input and lookup (API v9):**
  - Keys: `Input.IsDown / Pressed / Released(Key)`.
  - Mouse: `Input.MouseDelta`, `MousePosition`, `MouseWheel` and `IsDown / Pressed / Released(MouseButton)`.
    In the editor these report only what happens over the game's view while it plays, and a click within one frame
    counts as down for that frame.
  - `Input.MouseLocked = true` hides and holds the cursor; its motion is still reported. Stop frees it.
  - `Entity.Find(name)` returns the entity with that name: the Hierarchy's in the editor, the scene edits' in a game.
  - `Animation.*` also takes a model placed whole, by its root (the entity with the Animator); its first skinned
    mesh below stands for it.
  - Editor tests can hold keys down for the game with `test.hold {"key", "down"}`, since pushed key events don't
    change SDL's keyboard state.
- **Finding under an entity (API v10):** `Entity.Find(name, under)` finds a descendant of `under` by name, for
  example a character's bone once its skeleton is expanded: `Entity.Find("foot_l", soldier)`. Bone names repeat
  across characters, so `Entity.Find(name)` alone finds the first one.
- **Hiding bones (API v11), for first person:** `Animation.HideBone(character, "head")` hides a bone and
  everything below it; `HideBone(character, "head", false)` shows it again, and `Animation.IsBoneHidden` asks.
  - Only the eye loses them; the shadows keep the whole character, head included. Each skinned vertex has two
    positions (`render/skinning.h`, 32 bytes a vertex): the whole one, and the eye's, where the hidden bones'
    share of the vertex is pulled into the hidden bone's joint. The visibility and resolve passes draw the
    eye's, and the shadow pages the whole one.
  - The rest of the body stays, and the neck closes over where the head was.
  - It applies to every part of the character (followers take the leader's). Stopping the game shows every bone again.
- **Esc frees the cursor:** while a game holds the cursor (`Input.MouseLocked`), Esc shows it and frees it.
  The game's requests for it then wait until a click in the game's view, like a browser's pointer lock.
  `Input.MouseLocked` reads false meanwhile. In a game run on its own, the first Esc frees the cursor and the
  next one quits.
- **Inverse kinematics (API v12):** see [Animation](animation.md). `Animation.SetFootIk(character, weight)` scales the
  graph's foot IK (0 in the air). `Animation.LookAt(character, target, weight, tip = "head")` and
  `StopLooking` turn the spine, neck and head toward a world point. `Animation.Reach(character, bone, target,
  weight)` and `StopReaching` make a hand reach a point. Call them every frame they apply.
- **Aiming (API v13):** `Animation.AimAt(character, target, weight, bone = "hand_r", axis, hold, support =
  "hand_l")` and `StopAiming` aim a held weapon: the spine turns until the bone's axis (the barrel, in the
  bone's own space) points at the target, and with `hold` the arms bring it to that point (before the eyes),
  the other hand keeping its grip. See Animation below. `LookAt`, `AimAt` and `Reach` also take an `Entity` (an
  Empty) instead of a point: where it is now.
- **Rays (API v14):** `Physics.Raycast(from, direction, out SurfaceHit hit, maxDistance = 100)`: the first surface
  along a ray (its point, normal, distance from the start, surface and entity), as `SurfaceAt` does straight
  down. A third-person camera finds what is under the view's centre with it, to aim at.
- **Particles, prefabs and decals (API v15):** `Particles.Play/Stop/Restart/Emit/Count/Hits`, C# `ParticleController`s
  (`Particles.Register<T>()`), `Prefab.Spawn(path, position, rotation)` and `Decals.Place(material, point, normal, size)`:
  see [Particles and decals](particles-decals.md).
- **Entity parameters (API v16):** `Animation.SetEntity(character, parameter, entity)` points a graph's entity
  parameter (what its IK follows: an aim's hold, a look's target) at another entity, over the Animator's binding
  until Stop; `default` is none. `Animation.ResetEntity` puts the Animator's binding back, and
  `Animation.GetEntity` returns what it follows now. `SetEntity` and `ResetEntity` are false for a parameter the
  graph doesn't have.
- **Hot reload (JIT):**
  - The game assembly is loaded from bytes into a collectible `AssemblyLoadContext`, so the
    file stays unlocked. `Falcon.Engine` lives in the default context and is loaded from a
    shadow copy.
  - When the game DLL changes (for example after `dotnet build`), the host stops the game,
    drops every query and reference, unloads the context and checks it was collected. Then it
    loads the new code and calls `OnStart` again.
  - Game state belongs in components: the world survives a reload; fields don't.
  - Reloading the Sandbox takes 14–70 ms. Changing `Falcon.Engine` itself needs a restart.
- **Registration:** a game calls `Game.Register<T>()` from a `[ModuleInitializer]`. For
  NativeAOT, the `falcon_script_init` export is compiled into the game's own assembly
  (`FalconGame=true`), so calling it runs that initializer first.
- **Boot:**
  - JIT: CoreCLR (70–290 ms) and the game load (60–180 ms) run on a worker thread during GPU
    device creation. Only `OnStart` (~20 ms of JIT) is on the main thread.
  - NativeAOT: 3 ms to load and ~17 ms for runtime init, off the main thread, then a 2 ms start.
- **Entities, rendering and meshes (API v18):** `managed/Falcon.Engine/Entities.cs`.
  - **Destroy** (`Ecs.Destroy`, `entity.Destroy()`) takes the entity and everything below it (Parent).
    Their meshes stop drawing (before v18 a destroyed mesh stayed drawn), and bodies the game spawned
    (`Physics.Spawn`) go with them; before v18 those kept simulating, and a game's wrote into its dead
    entities. While the editor plays, the scene's own entities are disabled instead and Stop brings them
    back. Bodies the game spawns are the game's entities, so Stop removes them.
  - **`entity.Enabled`:** false disables it and every entity below it that is enabled now. They are not
    drawn, their bodies stop (Box3D disables them), and queries skip them. True enables what it
    disabled, and nothing that was disabled before (an entity deleted in the editor stays deleted).
  - **`entity.Name`:** its Hierarchy name. In a game that is the scene edits' name, else its cooked name;
    `Entity.Find` now finds cooked names in a game too, as the editor does. A game can rename an
    entity until Stop: `Find` then finds it by the new name and no longer by the old one.
  - **Hierarchy:** `entity.Parent` (setting it keeps where it is), `SetParent(parent, keepWorld)`, and
    `Children(span)`, which walks every child in the scene, so keep what it finds.
  - **Transforms:** `entity.World`, `Position`, `Rotation`, `Scale`, `LocalPosition`, `LocalRotation`,
    `LocalScale`, and `Forward` (-Z, as cameras and lights look), `Right` and `Up`. Setting a world
    transform updates a child's transform relative to its parent (`Scene::setWorld`).
    `WorldTransform` and `LocalTransform` decompose into position, rotation and scale, and
    `WorldTransform.From(position, rotation, scale)` makes one.
  - **`Renderer`:** `SetVisible` / `IsVisible` (apart from being enabled), `SetCastShadows`,
    `SetTint(rgba)` / `Tint` (multiplying the base colour), and `SetMaterial(".fmat" or null)`. They
    change the entity's instance row: in the CPU table, streamed by `ScriptHost::extract` after the
    scene's rows. Each row is kept as it was the first time the game changed it, and the editor's
    Stop puts it back. A body `Physics.Spawn` made has no row of its own to change.
  - **`Scene.SpawnMesh(mesh, position, rotation, scale, material, parent)`:** a mesh of the scene, by
    name. The GPU's instance table is fixed at upload, so only meshes with reserved slots can be made
    (`vfx/runtime_meshes.h`): those of the project's prefabs and Model Renderers, and the built-in
    shapes in the editor. Otherwise it returns default and warns. The meshes are the game's entities:
    destroyed with them, hidden while disabled, deleted at Stop.
  - **`Ref<T>`:** a cached component reference (flecs' `ecs_ref_t`). `Get()` looks the component up
    again only when the entity's table changed, so it costs about 7 ns where `Ecs.Get` costs about 20.
    It is read only: writes go through `Ecs.Set` or a query, which mark the change for the renderer.
  - The script host resolves its component ids once (`MeshInstance`, `RigidBody`, `Parent`), because
    flecs' typed `try_get<T>()` costs tens of ns per call. `Animation.SetFloat` through an entity is
    about 30 ns now.
  - **Test:** `editor_script_entities`, on a project of its own ("Script Test", for every scripting
    test from now on). A game checks all of the above on the HUD; the test checks the rows as drawn
    (`test.expect` entity `rowHidden` / `rowShadow` / `rowTint` [r, g, b, a] / `rowMaterial`) while
    playing, and that Stop puts every one back.
- **The rest of the engine's systems (API v25):** `managed/Falcon.Engine/SystemsApi.cs`.
  - **Animation:**
    - Parameters read back: `Animation.GetFloat`, `GetBool`, `GetId` (an id's hash; `Animation.Hash`).
    - States: `Animation.InState(c, "Jump")` asks whether a state machine playing now is in that state,
      and `StateName(c, n)` names the n-th playing machine's state (outer first). State nodes now carry
      their name's hash, so a graph cooked into a pak before this answers false; the project's
      `.fgraph` files always know their names.
    - Speed: `Animation.SetRate(c, rate)` runs one character's animation faster or slower, on top of
      `Time.Scale` (0 holds it). Stop puts it back to 1.
    - Clips: `Animation.Play(c, "Wave", speed, fade, loop)` plays a clip of the character's skeleton
      over its graph, fading in. The graph keeps running beneath, so it is where it should be when the
      clip fades out, which happens by itself after a clip played once, or with `StopClip`.
      `ClipTime` / `IsPlayingClip` tell where it is. Stop ends it.
    - Root motion: `Animation.RootMotion(c)` gives the last update's translation and rotation in the
      character's space. The engine doesn't move the entity by it; a script applies it (through
      `CharacterController.Move` or the transform).
  - **Terrain:** `Terrain.Sample(p, out sample)` gives the height, normal and material (two layers by
    index, the blend, `Dominant`) at a point's x, z. `Terrain.HeightAt`, `Terrain.Raycast` (the terrain
    alone) and `Terrain.LayerName(i)` cover the rest.
  - **Water:** `Water.BodyOf(entity)` and `Water.NearestSurface(body, p)` give the point of a body's still
    surface nearest a point (`Water.Sample` already gives the waves').
  - **Particles and decals as data:** `Particles.HasComponent` / `AddComponent` / `RemoveComponent` take a
    component's name (`"ParticleEffect"`, `"ParticleConeEmitter"`, `"Decal"`...) or its key in the scene
    file. `Particles.GetField` / `SetField` read and write a field as JSON, the way the scene file has it
    (`SetField(e, "ParticleEffect", "timeScale", 0.5f)`; colours as `Vector4`, `GetFloat` for numbers).
  - **UI:** `Ui.HasPointer` (the page's controls have the pointer) and `Ui.HasKeyboard` (a text field has
    focus: don't read keys as play).
  - Not covered: sound lengths (the sound bank doesn't keep them yet) and a scripted audio listener (the
    first enabled `AudioListener` entity is the listener; scripts pick it by enabling one).
  - **Test:** `editor_anim_systems` (Anim Test project) reads back parameters it set, follows the bar's
    state machine from Straight to Bent, plays the Bend clip over the graph (an unknown clip refused),
    holds it at rate 0, fades it out, finds no terrain or water, and adds a particle effect, sets a
    field, reads it back and removes it.
- **Render settings and debug drawing (API v24):** `managed/Falcon.Engine/RenderApi.cs`.
  - `Render.Set(name, value)` / `Render.Get(name)` use the Lighting panel's names: `exposure`,
    `manualExposure`, `bloom`, `ao`, `aoRadius`, `aoPower`, `lookPower`, `lookSaturation`, `gi`,
    `giIntensity`, `skyIntensity`, `reflections`, `reflectIntensity`, `fxaa`, `taa`, `renderScale`.
    Values are clamped to each setting's range, and an unknown name gives NaN.
  - The frame's settings are kept as they were when the game first changes one, and the editor's
    Stop puts them back. There are shortcuts for `Render.Bloom`, `Exposure`, `Gi`, `SkyIntensity` and
    `Reflections`.
  - `Debug.Line`, `Ray`, `Arrow`, `Box`, `Sphere` and `Point` draw world-space lines over the editor's
    views (the Scene tab and now the Game tab), on top, for a while (seconds; 0: this frame). They
    are projected by the scene view's overlay. A game shows none, since debug drawing is for
    developing, and Stop clears them.
  - Per-material parameters at runtime aren't exposed: a material's factors live in its `.fmat`, and
    tint and material per instance are `Renderer.*` (API v18). Fog and procedural meshes are engine
    features still to come.
  - **Test:** `editor_script_render` sets bloom (put back at Stop), checks that an unknown setting is NaN
    and that `renderScale` is clamped, and draws a box each frame plus a lasting line (13 lines),
    gone at Stop.
- **Game flow and the camera (API v23):**
  - **Time:**
    - `Time.Scale` scales the game's time: scripts' `Time.Delta`, animation, physics and particles.
      0.5 is slow motion, 0 is paused, and the most is 100. `Time.Paused` is a shortcut for 0.
    - The editor keeps its own time (editing animations previewing, the UI), and Stop puts the scale
      back to 1. `Time.UnscaledDelta` is the real frame time.
    - Under `--fixed-time` physics takes one step a frame whatever the scale.
  - **Quitting:** `Game.Quit()` quits a game's process. In the editor it stops Play.
  - **Saves:** `Storage.Save` / `Load` / `Exists` / `Delete` (text or bytes) keep files in the player's
    app data, at `%APPDATA%/Falcon/Games/<game>/`. A save is written aside, then moved in, so a crash
    never leaves half a save.
  - **The camera:**
    - `Camera.Main` is the main camera entity, and `Camera.SetMain(e)` makes another one main.
    - `Camera.FieldOfView` is the view's vertical angle.
    - `Camera.ScreenToRay(pixel)` turns a point of the game's view (`Input.MouseViewPosition`) into a
      ray to pick with `Physics.Raycast`, and `Camera.WorldToScreen(point)` goes the other way.
    - The camera and the view are now set before the game's `OnStart`, which used to see zeros
      there.
  - **Test:** `editor_script_flow` checks half-speed time, a save written, read and deleted, the main
    camera and its 50° field of view, rays and points that agree, and `Game.Quit` stopping Play.
- **Input (API v22):** `managed/Falcon.Engine/InputApi.cs`.
  - **Gamepads:** `Gamepad.First`, `new Gamepad(slot)` (8 slots, each kept while its pad is connected),
    and `Gamepad.Count`.
    - Buttons by position (`GamepadButton.South` is A on Xbox and Cross on PlayStation) through
      `IsDown`, `Pressed` and `Released`.
    - `Axis(axis, deadZone)`: sticks -1..1 with +y down, triggers 0..1. `LeftStick` and `RightStick`
      have a radial dead zone. `Rumble(low, high, seconds)`.
    - SDL's gamepad subsystem is up from the start, and pads connected at any time are opened
      (`ScriptHost::gamepadEvent`). In the editor they read only while the game plays.
  - **Keys:** `Key` covers every SDL scancode (it had 70 keys; now `F13`, `KpEnter`, `Grave`, ...).
  - **Text:** `Input.TextInput = true` turns on SDL text input (IME included); `Input.Text` is what was
    typed this frame. Stop turns it off.
  - **The game's view:** `Input.MouseViewPosition` (the mouse from the view's top left) and
    `Input.ViewSize`. The view is the editor's game view, or the window in a game (`FxMouse.view*`).
    `MousePosition` is still in window pixels.
  - **Actions:**
    - `Input.Action("Jump").Bind(Key.Space).Bind(GamepadButton.South)` binds an action to keys, mouse
      buttons, gamepad buttons and axes on any pad. `Bind(Key.A, Key.D)` makes an axis.
    - `Value` is the strongest binding, from -1 to 1. `IsDown`, `Pressed` and `Released` are past
      half way. `Input.Vector("MoveX", "MoveY")` combines two, clamped to length 1.
    - Rebind any time (`Clear`, then `Bind`).
  - **Tests:** `test.gamepad {buttons: {south, ...}, axes: {leftx, ...}, remove}` drives a virtual SDL
    gamepad. Background joystick events are allowed for it, since test windows never have focus.
    `editor_script_input` checks the virtual pad's button and stick, actions bound to it, a key of
    the full enum, the view size, and the pad going away.
- **Script components (API v20):** C# classes attached to entities, in the style of s&box's and
  Unity's components. Sources: `managed/Falcon.Engine/Components.cs`, `ScriptComponents.cs`, and the
  generator in `managed/Falcon.Engine.Generators`.
  ```csharp
  public sealed class Spinner : Component
  {
      [Property, Range(0, 720)] public float Speed = 90f;
      [Property] public Entity Target;
      public override void OnUpdate(float dt) => Entity.Position += Vector3.UnitY * dt;
  }
  ```
  - **Properties:** `[Property]` fields and properties of type float, double, int, uint, long, bool,
    string, an enum, Vector2/3/4, Quaternion or Entity. They are saved with the scene, edited in the
    Inspector, and kept through a hot reload; other fields start over. Optional extras:
    `[Range(min, max)]`, `[Asset(".fsound")]` for asset paths, `[Property(Tooltip = ..., Group = ...)]`.
  - **Lifecycle:**
    - `OnAwake` runs when a component is made. The scene's components are all made first, before
      the `Game`'s `OnStart`.
    - `OnStart` runs before the component's first update.
    - `OnUpdate` runs every frame, after the `Game`'s, while the component and its entity are enabled.
    - `OnFixedUpdate` runs every physics step (`PhysicsWorld::setStepCallback`, before Box3D's step).
    - `OnEnable` / `OnDisable` run when that changes.
    - `OnDestroy` runs when the entity goes or the game stops.
    - Each call is guarded on its own: an exception is logged and the rest go on.
  - **Using components from code:** `Component.Entity` is a readonly field, so `Entity.Position = ...`
    works. Also `GetComponent<T>()`, `entity.AddComponent<T>()` (not saved), `entity.GetComponent<T>()`
    and `ScriptComponents.All<T>(list)`.
  - **Registration:** a Roslyn source generator registers every `Component` subclass from a module
    initializer. It emits a factory, the property schema, and JSON read and write code, with no
    reflection, so components work under NativeAOT and trimming. Project games get it through
    `Falcon.Game.targets`, which the editor's build imports (`CustomAfterMicrosoftCommonTargets`), so
    existing projects need no change. The Sandbox and Bench reference its project.
  - **Saved** as `"scripts": [{"class", "enabled", "properties": {...}}]` per entity, in the scene edits
    and in prefabs (`script/script_entry.h`). Only the properties that were set are stored; the
    class's defaults fill in the rest. Entity properties are stored as the edits' keys. Prefab
    instances override a prefab's list as a whole, the way vfx components do.
  - **Editor:**
    - Add Component lists the game's classes, from `describe()` after each build loads.
    - Each component gets an Inspector section built from its schema: numbers (ranged), checkboxes,
      enums, vectors, text and asset paths, and an entity picker.
    - It has an enable checkbox and Remove Component, all undoable.
    - While playing, the section shows the running component's values.
    - A class the game no longer has keeps its values and shows a warning.
    - Making a prefab and Apply now also keep vfx components; before, they were left out.
  - **Runtime:**
    - The engine queues the scene's components when the game starts (`scriptPoll`), and a spawned
      prefab's components since then.
    - A hot reload while playing writes every component's properties out, makes the components
      again on the new code, and reads the properties back.
    - A project may have components and no `Game` class at all.
  - **Tests:**
    - `editor_script_components` adds a component in the Inspector, sets its properties (an entity
      by name, an enum, a string), removes it and brings it back with undo, saves it in a prefab,
      then plays: the scene's component and a spawned prefab's go through OnAwake, OnStart,
      OnFixedUpdate and OnUpdate. A hot reload keeps the properties, and Stop puts the scene back.
    - `_reload` checks all of it from the saved scene.
    - The Sandbox's props now bob by a `Bobber` component, added in code, so `script_modes` and
      `script_reload` cover components under NativeAOT and hot reload.
- **Physics (API v19):** `managed/Falcon.Engine/PhysicsApi.cs`; physics_world.h has the native side.
  - **Bodies:** a body `Physics.Spawn` made, or a scene entity's (PhysicsBody, Collider) while the game
    runs.
    - `Physics.Body(e, out BodyInfo)` reads velocity, angular velocity, mass, gravity scale, damping,
      type, locks, awake and bullet.
    - `SetVelocity`, `SetAngularVelocity`, `SetMass` (the shapes' distribution is kept),
      `SetGravityScale`, `SetDamping`, `SetType` (Static, Kinematic, Dynamic), `SetLocks` (MoveX..TurnZ),
      `SetAwake`, `SetBullet`.
    - `AddForce` / `AddImpulse` (at a world point, or the centre of mass), `AddTorque`,
      `AddAngularImpulse`.
    - `Physics.Teleport(e, position, rotation)` moves a body there at once. Nothing is swept on the
      way and it doesn't interpolate from where it was. The entity moves with it, so a kinematic
      body's target is there too.
    - `Physics.Gravity` gets and sets the world's gravity; the editor's Stop puts it back.
  - **Changing bodies while playing:** setting or removing an entity's PhysicsBody or Collider
    (`Ecs.Set`, `Ecs.Remove`, or the Inspector) builds its body again at the next step, through flecs
    observers (`PhysicsWorld::rebuild`). The body keeps its index, so contacts and water still find
    it. `Physics.Rebuild(e)` does it at once. Before v19 these changes waited for the next Play.
  - **Collision layers:** `Collider.layer` (0..31) and `ignore` (the layers it passes through; the
    Inspector's Layer and Ignores Layers). A shape's Box3D category is its layer's bit and its mask
    leaves out what it ignores. Everything without a Collider of its own (the ground, cooked
    colliders, spawned bodies) is on layer 0.
  - **Queries:** each takes a `QueryFilter` (`All`, `Only(layers)`, `AllBut(layers)`,
    `.Ignoring(entity)`), and none of them hits a trigger.
    - `Physics.Raycast(from, dir, out hit, max, filter)`.
    - `SphereCast` / `BoxCast` / `CapsuleCast`, where `hit.Distance` is how far the shape went.
    - `OverlapSphere` / `OverlapBox` / `OverlapCapsule`, which return the entities overlapping, each
      once.
  - **Triggers:** a Collider's Is Trigger (`flags` 1) makes it a Box3D sensor. It collides with nothing,
    and `Physics.Triggers()` lists this frame's events (`Trigger`, `Other`, `Entered` / `Exited`).
    Spawned bodies and the scene's moving bodies are seen by triggers.
  - **Capsule colliders:** shape 3 (`size[0]` the radius, `size[1]` the height, along Y), and Capsule
    Collider in Add Component.
  - **Character controller:** `CharacterController { Radius, Height, StepHeight, SlopeLimit, Filter }`
    with `Move(entity, delta)`. The entity's position is the capsule's base.
    - It uses Box3D's mover: collide (contact planes), `b3SolvePlanes`, then cast, up to 5 times.
    - It slides along walls.
    - It climbs steps up to `StepHeight`: when a move is blocked it goes up by the step height, probes
      a radius further than the move for walkable ground, and takes the move at that height.
    - It stays on the ground when walking down (snapped straight down, by up to the step height).
    - Ground steeper than `SlopeLimit` counts as a wall.
    - It returns `Grounded`, `GroundNormal`, `Ground` and `Blocked`.
    - It is kinematic: gravity and jumping are the game's, added to the delta.
  - **Joints:** `Physics.CreateJoint(type, a, b, settings)`, or `Physics.Hinge(a, b, anchor, axis)`.
    `b` default is the world.
    - Types: Fixed, Hinge (about its axis: limits, motor, spring), Ball, Slider (along its axis), Distance
      (rope or spring), Wheel (spin motor, suspension).
    - `Joint.SetMotor(speed, max)` and `Joint.Destroy()`. A joint goes with either body, and at the
      editor's Stop.
  - **Components as vectors:** components' 3- and 4-float fields also come as `Vector3` / `Vector4`
    properties (`Collider.Size`, `Collider.Center`, `Light.Color`), so scripts change them without
    unsafe code (`falcon_schemagen`).
  - **Test:** `editor_script_physics` builds a floor, a wall, a 0.3 m step, a trigger zone, a body that
    ignores layer 0 (it falls through the floor), a hinged door, a collider that grows while playing,
    and two characters. A test game checks each feature on the HUD.
- **Speed (API v17):**
  - **The bridge:** a game's empty `OnUpdate` costs under 1 µs per frame. Calls are plain function
    pointers, and the leaf ones (ECS reads and writes, queries, the camera, the mouse, animation
    parameters and events, body velocity and impulse, water samples) skip .NET's GC transition
    (`unmanaged[SuppressGCTransition]` in `Native/NativeApi.cs`). Calls that can take long or block
    keep it: physics queries, spawning, logging, UI and audio.
  - **Character handles:** `Character.Of(entity)` finds a character once (its skinned record), and
    every `Animation.*` call takes the handle or the entity. Entities convert implicitly, so existing
    code still compiles. A handle skips finding the character on every call: `SetFloat` takes 8 ns
    through a handle, 37 through an entity, and took 135 before v17, when every call looked the
    `MeshInstance` component up by name. Keep handles in a component; they are valid while the
    scene is loaded.
  - **Allocation-free frames:** `Performance.ManagedAllocatedBytes` (this thread's managed
    allocations so far; read it around a frame) and `Ui.Send(ReadOnlySpan<byte>)` (UTF-8 JSON
    written in place with `Utf8.TryWrite`). The Sandbox allocates nothing in a steady frame: it
    finds its characters once (a `Walker` component with the handle and the Speed parameter's
    index), reuses its lists, and makes each surface's footstep path once.
  - **Measured** (`managed/Bench`, CTest `script_bench_jit` / `_aot` on the fox crowd; JIT and
    NativeAOT agree within a few ns):

    | call | ns |
    |---|---|
    | `Input.IsDown` | 0.4–1.3 |
    | `Camera.Position` | 4 |
    | `Animation.Events` (handle) | 6 |
    | `Ui.TryReceive` (nothing waiting) | 6–7 |
    | `Animation.SetFloat` (handle / entity) | 8–9 / 36–38 |
    | `Ecs.IsAlive` / `Has` / `Get` | 11–14 / 18 / 19 (flecs' own lookup) |
    | `Ecs.Set` (an existing component) | 41–42 |
    | `Animation.Parameter` (by name) | 41–65 |
    | a query pass writing 256 entities | 200–230 |
    | `Ref<T>.Get` (v18) | 7 |
    | `entity.Parent` / `Renderer.IsVisible` (v18) | 16–33 / 26–28 |

    The test fails when a call goes far above its ceiling (a lookup or transition per call coming
    back), or when a steady frame (queries writing components, a character's parameter and events,
    input, the camera, the UI) allocates anything.
  - **The Sandbox** on Sponza takes 33–45 µs per frame in either mode, from 50–70 µs before v17. Most
    of what is left is not the API:
    - `Physics.Spawn`, once every 6 frames, is about 20 µs per frame: Box3D creating a body.
    - The rest is cache misses: Sponza's frames with the Sandbox running take 25–75 ms of other
      work, so the script starts each frame with cold caches. Its 12 bobbing props take 6 µs, where
      the bench's 256 movers take 0.2 µs warm.
- **Notes:**
  - NativeAOT runs a module's initializers lazily, on first access to that module. That's why
    the export has to live in the game assembly.
  - The NativeAOT link uses the MSVC tools of the build environment
    (`IlcUseEnvironmentalTools`), because ILCompiler's own Visual Studio discovery fails from
    some shells. Publishing by hand needs a VS developer prompt.

## Project scripts

Each project has its own C# game, made, built and run from the editor
(`engine/project/game_scripts.h`):

```
MyWorld/
  Scripts/                    the sources: Game.csproj, Game.cs (and anything the game adds)
    Directory.Build.props     the build goes to Cache/; the engine's Falcon.Engine.dll by default
  Cache/Scripts/bin/Game.dll  the build the editor loads (JIT), with Falcon.Engine.dll
```

- **Made** with every new project: `Game.cs` has a `Game` subclass named after the project,
  registered from a `[ModuleInitializer]`. A project from before gets it from File > Create
  Scripts.
- **`PerformanceHud.cs`** (in the template, called from `OnUpdate`): frame time (average and
  slowest over the last half second), fps, and CPU, GPU, RAM and VRAM use, the game's and the
  system's, at the top left of the view while playing. Refreshed four times a second; F3 hides
  it. Built on two API calls:
  - `Performance.Current`: frame timing from the engine's loop (wall time), and CPU, memory and
    GPU use sampled twice a second on a thread of its own (`core/system_stats.h`, started by the
    first call). GPU use is Task Manager's figure, from Windows' GPU Engine counters (the busiest
    engine type); VRAM is the process's dedicated memory and the adapter's (DXGI).
  - The game's RAM is Task Manager's figure too: its private memory in physical RAM (the private
    working set). In brackets, all it has committed: in the editor most of that is allocated but
    paged out or never touched (Zero-Day: about 0.7 GB in RAM of 2.2 GB committed).
  - `Hud.Show(text)` / `Hud.Clear()`: monospace text over the game's view (the editor's page,
    `editor.hud`); it goes when the game stops.
  - Sounds (`Audio.Play`, `SoundHandle`, bus volumes), physics contacts and the surface under a
    point: see Audio > Scripts.
- **Built in the background as the sources change**, so Play has nothing to build: it starts the
  game on the next frame. The editor looks at the sources (`.cs`, `.csproj`, `.props`,
  `.targets` under `Scripts/`, not in `bin`, `obj` or dot folders) four times a second; a change
  is built with the .NET 10 SDK (`dotnet build`) once the files have held still for 150 ms (an
  IDE's save-all), and loaded into the warm runtime as soon as it is built.
  - At startup, a build older than a source (or another engine's, below) is built again at once.
  - Play waits only for a change the build has not caught up with (one building, or one saved
    the moment before Play): it starts once that build is loaded. A change during a build is
    built again right after it.
  - File > Build Scripts (Ctrl+B) builds now, and says when it is done; a background build only
    shows in the pill by the play controls (building, loading, failed: a click opens the
    Console).
  - Compiler errors go to the Console. While the sources as they are do not compile, Play does
    not start (it says why at once, without building again); the next change builds again.
  - A build takes about 1.5 s warm (MSBuild's and the compiler's servers stay up between
    builds); NuGet's restore is skipped unless the project files changed since the last one.
  - The build references the running engine's `Falcon.Engine.dll` (`bin/scripts/jit/`), so it
    always matches the engine that loads it.
  - Without the .NET SDK nothing builds: Play goes on with the last build, if there is one
    (changes are not built again until File > Build Scripts).
- **Loaded** at startup (the last build, on CoreCLR's worker thread as for `--script`), and its
  code replaced by every build. CoreCLR stays up in the editor, so Play only calls `OnStart`.
  Nothing runs until Play.
- **Play and Stop:**
  - Play calls `OnStart`; `OnUpdate` runs every frame while playing (Pause and Step hold it,
    like physics and animation).
  - Stop calls `OnStop`, then puts the scene back as it was before Play:
    - the entities the game created are deleted;
    - its own components (registered by the game) are removed from every entity;
    - the physics bodies spawned while playing are removed;
    - entities of the scene it destroyed come back (while playing they are only disabled);
    - every editor entity it moved, or whose light or camera it changed, is restored.
  - A change saved while playing is built and replaces the code, and the game starts again on
    it (its state stays in the ECS), as hot reload does.
- **A build for another engine** (its copy of `Falcon.Engine.dll` differs from the running
  engine's: the engine was rebuilt since) is not loaded at startup, and is built again at once. The
  process holds one `Falcon.Engine`, so loading an old one would leave the game unable to load
  until a restart.
- **Limits:** one `Game` per project (optional when it has script components).
- Tests: `editor_scripts` (the template built in the background; a test game saved just
  before Play, built, run and undone by Stop, twice; a compile error keeps Play from starting),
  `_reload` (the last build loads at startup and plays within two frames) and `_instant`
  (another engine's build rebuilt at startup; a saved change seen building without Play, then
  played within two frames on the new code; a compile error refusing Play without a build).

## Shipping a game (S8)

- **File > Build Game** publishes the project's game with NativeAOT (`dotnet publish -r win-x64
  -p:PublishAot=true`, a minute or so). Its libraries' code goes in, editor code doesn't, and the
  engine's `falcon_script_init` export is compiled into the game's own module (`AotEntry.cs`, which
  `Falcon.Game.targets` adds when publishing). The result is `Cache/Scripts/aot/Game.dll`; the
  editor's own build (Debug, `Cache/Scripts/bin`) is left alone.
- It then fills `Build/<name>/` on a thread:
  - the engine: `falcon.exe` as `<name>.exe` with its DLLs and folders (CEF, audio, FSR, DLSS, shaders,
    UI bundles), without debug files or the engine's script builds;
  - `Game/`: the project (scenes, assets, libraries' assets, its UI, the cooked `Cache` with the
    published game), without `Scripts/`, libraries' `Code/` and `Editor/`, `Build/` or `Exports/`.
  - For the Script Test project that comes to 437 MB, mostly CEF.
- The NativeAOT link finds Visual Studio through `vcvarsall.bat`, which breaks on PATH entries cmd
  can't take unquoted (`s&box projects`: `&` ends the command). Its errors then end up in the
  linker's path. So the publish gets a PATH without such entries, plus Visual Studio's installer
  folder (for `vswhere.exe`).
- `<name>.exe` started alone plays `Game/` (`gamescripts::shippedProject`). `--hidden`, `--frames`,
  `--scene` and the like still apply.
- **A project's game on its own:** `falcon.exe --project <dir>` without `--editor` loads the published
  game, or else the editor's last build (when it was built for this engine). `--audio-bake` doesn't
  (a tool: it bakes the scene as saved). The project's `UI/Game/index.html`, when there is one, is
  the game's HTML UI.
- **Scene.Load (API v26):** `Scene.Current` is the scene playing (its path in the project), and
  `Scene.Load("Scenes/Level2.fpak")` plays another. Since L2 that happens in the same process (see
  Scenes in process below), and since L4 the editor plays on into it too.
- **Tests:**
  - `editor_game_build`: Build Game's files are all there, and the sources and debug files aren't.
  - `game_build_run` / `game_build_ran`: the built `<name>.exe`, started with no project argument,
    runs the native game, which writes that it ran and quits.

## Scenes in process (L1–L4)

The engine is being taught to change scenes without restarting (the roadmap's L1–L4: games'
`Scene.Load`, loading screens, the editor's Open Scene).

- **L1: the session split.** A process is now two levels (`engine/app/engine.cpp`):
  - `Process`: what outlives a scene. That is the window, the GPU device, the HTML UI, the scripts'
    runtime, the audio device, the frame's settings and the frame loop's counters.
  - `runScene`: one scene's systems (its pak, ECS world, edits, materials, physics, animation,
    terrain, water, particles, the renderer and its virtual textures, lights and shadows), its
    frames, and its teardown. `runSession` runs scenes one after another until none follows.
- **Teardown order:** physics' observers before the world, then the audio scene detached
  (`AudioSystem::detachScene`: its sources stopped, its geometry and bake let go). The scripts let
  go of everything they kept about the scene (`ScriptHost::detachScene`), and the pak goes last.
- **Test flag:** `--reload-scene-at <frame>[,<count>]` tears the scene down and loads it again from
  its files at that frame and its multiples. It is for games without an editor; the editor comes
  with L4. Between scenes the engine logs what the GPU accounting holds and the process's RAM.
  With a count it fails if the GPU's figures change at all from one load to the next, or if RAM
  grew by more than 64 MB by the last load.
- **Measured on Sponza:**
  - Reloaded at frame 30, the frame captured at 120 matches a plain run (1 pixel differs by 5).
  - Over ten loads the GPU holds exactly the same between scenes (the hidden frame target alone).
  - RAM moves by tens of MB as freed memory goes back, without growing.
- **Tests:** `render_scene_reload` (the reloaded frame against a plain one) and
  `render_scene_reload_leak` (ten loads).
- **L2: `Scene.Load` in process (API v27).** A game's `Scene.Load` ends the scene after that frame
  and loads the next in its place, in the same process.
  - **Cooking:** a project's scene that needs a cook is cooked first (a shipped game's always is
    cooked).
  - **The `Game` object lives on** across scenes:
    - `Game.OnSceneUnloading(scene)` runs while the old world is still there; the scene's
      components then end (`OnDestroy`).
    - Once the next scene is up, its components are made and `Game.OnSceneLoaded(scene)` runs.
    - Fields and statics carry over.
    - Entities, character handles and `Ref<T>`s of the old scene mean nothing in the next one.
  - **What stays:** time scale, render settings the game changed, input, the HUD and the game's
    HTML page.
  - **Component ids follow the world.** `Ecs.Id<T>` looks ids up again in a new world (a world
    generation checked with the cached id).
  - **Queries follow the world too.** A query made of `Query.In / InOut / Not` terms lets go of its
    native query while the old world is still there, and makes it again in the new one the next
    time it is used. The game's queries, made once, keep working.
  - **`--reload-scene-at`** keeps the game going the same way (the Sandbox runs through its scene
    loaded three times).
  - **Measured:** a switch takes about 95 ms between the Script Test project's small scenes and
    110 ms for Sponza (from the last frame of one scene to the next scene's first). It all happens
    in that frame; L3 moves the reading off the main thread.
  - **Tests:**
    - `editor_script_scene` makes a second scene (`test.newScene`: added to the project and cooked,
      not opened), and the editor stops Play when the game asks for it.
    - `script_scene_run` / `script_scene_loaded`: the game alone goes Main → Second → Main → Second
      → Main in one process, and writes what it saw:
      - four loads, counted by a field and a static;
      - the Main scene's Spinner component made three times and ended twice;
      - its entity found in Main and not in Second;
      - the game's own component on one new entity in each world, counted by a query made once;
      - an unknown scene refused.
    - `render_script_scene_reloaded`: the Sandbox through its scene loaded twice.
- **L3: loading in the background (API v28).** `Scene.LoadAsync(path)` loads the next scene while
  this one keeps running. When it is read, it takes this one's place after that frame, as
  `Scene.Load` does.
  - **The work off the main thread:**
    - A scene that needs a cook is cooked on a thread.
    - The scene's file is then read with SDL's async I/O (`ScenePak::readDone` checks without
      waiting).
  - **Watching it:** `Scene.IsLoading`, `Scene.LoadStage` (`"cooking: meshes"`, `"reading"`,
    `"ready"`) and a rough `Scene.LoadProgress` (a cook's share of its work, then reading, then
    ready).
  - **Loading screens:** the game keeps updating, so its loading screen is its own HTML page or HUD.
  - **What waits:**
    - A `Scene.Load` meanwhile wins (the load in the background is cancelled, its cook stopped).
    - What remains is the switch itself: the scene before torn down, the next one built and
      uploaded. Each one is logged (`scene: ... in place, N ms after the last frame of the one
      before`).
  - **Test:** in `script_scene_run`, loads 2 and 4 go through `LoadAsync`. The game counts frames
    it updated while loading and the stages it saw (`reading+ready`).
- **L4: the editor's scenes in process.**
  - **One bridge for the process.** The `EditorBridge` (the page's side) belongs to the process;
    scenes attach to it (`attachScene`: init's scene parts; the page gets the new state) and
    detach from it (`detachScene`: records, selection, undo and the systems it was given let
    go, its camera and sun remembered first). The page, the console, the script builds, the
    library manager, editor windows and the test runner carry on across scenes.
  - **Open Scene, New Scene and a cook done** open the scene in the same process at the end of
    the frame. A cook of the open scene waits in `Cache/Cook` and is moved in as the scene is let
    go (each project scene after the first goes through `cookProjectScene`: a no-op when
    installed). A switch takes about 220 ms in the editor.
  - **A game's `Scene.Load` while playing:**
    - The game plays on into the target scene, which starts already in Play (`playNow`; the game
      continues through `ScriptHost::continueGame`). Later loads go on the same way.
    - The edited scene is kept as the editor had it before Play (`keepForPlay`): its edits as
      text (`SceneEdits::text` / `loadText`), the unsaved flag, the selection and undo.
    - Stop comes back to it with all of that (`comeBack`). Terrain strokes don't come back in
      the undo history; with unsaved terrain changes Play stops instead, with a notice.
  - `--reload-scene-at` works in the editor too.
  - **Tests:**
    - `editor_script_scene`: an unsaved entity made and Play. The game goes Main → Second → Main
      → Second → Main (two of those in the background) and quits. Stop comes back to the edited
      Main with the entity, the unsaved flag and undo, after five scenes in process
      (`test.expect` `sceneOpen`, `sceneSwitches`, `dirty`).
    - `editor_project`, `project_cook`, `editor_assets_place`, `editor_anim_import` and
      `editor_anim_ik_import` open their scenes in process now, with the cook moved in (they
      used to record the relaunch).

## Rendering from C# (R1–R5)

The roadmap's R1–R5 open the renderer to scripts: material parameters, project shaders, post
effects, surface shaders, procedural meshes.

- **R1: runtime material parameters (API v29).**
  - **The API:**
    - `Material.Find("Assets/Brick.fmat")` gives a scene's material (invalid when the scene has none
      of it).
    - `Get` / `Set` by name: `baseColor`, `emissive`, `emissiveStrength`, `roughness`, `metallic`,
      `normalScale`, `occlusionStrength`, `alphaCutoff`.
    - Properties: `BaseColor`, `Emissive`, `Roughness` and the rest.
    - A change shows on every user of the material from the next frame.
  - **Per-entity copies:** `Renderer.MaterialInstance(entity)` copies the entity's material (its
    override, else its mesh's first) into one of 256 runtime slots reserved with the scene. The
    entity draws with the copy from then on, so changing it changes nothing else.
  - **Put back:** the editor's Stop (and a game's end) puts back every material scripts changed and
    frees the copies.
  - **One upload a frame:** material changes (the editor's live edits too) are queued in
    `MaterialLibrary` and uploaded once a frame, in the frame's own command buffer: the scene's
    copy (the raster's) and the virtual textures' copy (shading's), from one transfer buffer.
    Before, every change submitted a command buffer of its own, twice.
  - **Test:** `editor_script_material`. Two cubes share a material asset; the game turns it red
    and gives one cube a blue copy, and reads the values back. `_changed`: the frame while
    playing differs. `_restored`: after Stop it matches the one before.
- **R2: project shaders.** A project's `Shaders/*.slang` (and enabled libraries' `Shaders/`) are
  compiled by the editor in the background as they change, one at a time, into
  `Cache/Shaders/<path, folders as _>.fshader` (`project/project_shaders.h`).
  - **The compiler ships with the editor.** It is `falcon_shaderc`, the tool that builds the engine's
    own shaders, in `bin/shaderc/` with Slang's DLLs (not its 100 MB LLVM back end) and DXC.
    Build Game leaves it out and ships the compiled shaders, which are in `Cache`.
  - **`--blob` mode** writes one file: `"FXSH"`, a version, then JSON listing each entry point
    (stage, resource counts, threads, where its SPIR-V and DXIL are, its first uniform block's
    fields: name, type, offset, size), then the code. `--include DIR` adds search paths for
    imports.
  - **The same rules as the engine's shaders:** SDL_GPU's binding layout is checked, and so is a
    uniform layout that differs between SPIR-V and DXIL (a `float3` after a `float`: rows of 16
    bytes).
  - **Errors** go to the Console with their file and line (Slang's `--> path:line:column`, joined
    to the error).
  - **The engine reads a `.fshader`** without Slang (`render/shader_file.h`): its entries as
    `ShaderBlob`s, which `GpuDevice::createShader` / `createComputePipeline` take as they take the
    engine's own, and each entry's uniforms by name.
  - **Test:** `editor_shaders`. A compute shader is compiled; its uniforms are listed and a
    pipeline is made from it. A syntax error is reported with its line (`Grey.slang:19`). Fixed,
    with a field renamed, it compiles again (`test.wait {shaders}`, `test.expect {shader}`).
- **R3: post effects (API v30).**
  - **What an effect is:** a compute shader in the project's `Shaders/` that imports
    `falcon_post` (`engine/shaders/include/falcon_post.slang`, shipped in `bin/shaderc/include`):
    - `falconSource`: the frame so far;
    - `falconDepth`: reverse-Z, at the render size (`falconViewDistance(uv)` turns it into metres);
    - `falconTarget` before the tonemap, `falconTargetLdr` after it: where it writes;
    - `falconFrame`: size, time and the projection;
    - helpers `falconUv`, `falconOutside`.
    The effect's own values are its uniform block at slot 0.
  - **Running one:** `new PostEffect("Shaders/Grey.slang", PostStage.BeforeTonemap, order)` runs it
    every frame while `Enabled`, effects in `Order`. `Set("Amount", 1f)` writes a field of its
    uniform block by name, as reflected (ints and uints converted). An unknown shader or field is
    refused. The editor's Stop removes the game's effects; they carry on across scene loads.
  - **Where they run (`PostProcess`):**
    - `BeforeTonemap`: on the linear HDR frame after TAA or the upscaler (display size), between
      two textures of their own, before exposure, bloom and AgX.
    - `AfterTonemap`: on the display frame before FXAA, between the post chain's two display
      textures (`output()` is whichever holds the last result).
    - Pipelines are made from the `.fshader` when first used and made again when the file changes,
      so the editor's compile shows at once while playing.
  - **Not yet:**
    - normals and velocity as inputs;
    - effects saved with a scene (a component), which comes with X2's data components.
  - **Test:** `editor_post_effect`. A greyscale effect before the tonemap and an inverting one after
    it, held on by I.
    - `editor_post_grey`: the frame differs while playing.
    - `_inverted`: inverting changes it again.
    - `_restored`: after Stop it is as before.
- **Fixed on the way:** `.fshader` resource counts were read in the wrong order (uniform buffers and
  read-write textures swapped), which a shader with one of each hid.
- **R4: surface shaders.**
  - **What one is:** a file in the project's `Shaders/` with a `surface(SurfaceIn i, inout SurfaceOut o)`
    function, and if it wants, a `light(LightIn l, inout float3 color)` function. Its types come
    from `falcon_surface` (`engine/shaders/falcon_surface.slang`).
    - `surface` runs after the material's own textures and factors (albedo, roughness, metallic,
      AO, emissive, the shading normal).
    - `light` runs after the engine's lighting (toon bands, unlit) and gets the sun, its shadow and
      the ambient.
  - **Using one:** a material names it in its `.fmat` with `"shader": "Shaders/Toon.slang"`, plus up
    to four `"params"` (`SurfaceIn.params`; scripts: `Material.Params`).
  - **How it's built:**
    - The resolve imports `project_surfaces`. The engine's own build has none (an empty default),
      and a material's surface shader id lives in its flags (bits 8–15: its place among the
      project's surface shaders, sorted, + 1).
    - The editor generates the project's `project_surfaces` (`Cache/Shaders/gen`): each surface
      file's text in a namespace of its own, with `#line`, so errors point at the file. It compiles
      the engine's resolve with it (the engine's shader sources ship in `bin/shaderc/engine`) into
      `Cache/Shaders/resolve2.fshader`.
    - The renderer uses that instead of its own while it is there (`setProjectResolve`): at a
      scene's start, and in the editor whenever the shaders are compiled again. Then the materials
      are read again, for their ids.
    - With the last surface shader gone, the file goes and the engine's resolve is back.
  - **Params:** they live after the virtual textures' page table in the material buffer
    (`VirtualTextureSystem::paramsOffset`), uploaded with the rest in `MaterialLibrary::flush`.
  - **Limits:**
    - A surface shader can't change coverage (alpha masks are the base colour's alpha in the
      visibility raster).
    - No vertex animation.
    - The resolve takes a few seconds to compile.
  - **Test:** `editor_surface_shader`. A sphere with a toon material: its colour and three light
    bands differ from the plain one (`editor_surface_toon`). A script changes its params while
    playing (`_params`). After Stop it is as it was (`_restored`). The shader removed, the
    project's resolve goes too.
- **R5: runtime meshes (API v31).**
  - **Making one:** `new Mesh(vertices, indices)` takes `MeshVertex`: position, normal, uv and
    colour, 48 bytes, and triangles of 3 indices. `Mesh.Quad(size)` makes a quad, `Update` replaces
    the vertices and triangles (any counts, from the next frame), `Dispose` frees it.
  - **Drawing it:** `mesh.Draw(world, color, emissive, roughness, metallic)` draws it this frame;
    call it each frame it shows. A colour alpha under 1 makes it see-through.
  - **How it's drawn (`render/mesh_renderer.h`, `runtime_meshes.slang`):**
    - Its own vertex and index buffers, uploaded at the frame's start.
    - Drawn after the resolve, before particles. It's lit like the scene (the sun and its shadow,
      the local lights, the sky) and hidden behind the scene by its depth, tested in the fragment
      shader.
    - Opaque draws first, then see-through ones back to front, blended (premultiplied).
  - **It belongs to the process,** so a game's meshes carry on across scene loads. The editor's Stop
    and a game's end free them.
  - **Limits:**
    - No textures (the colour and vertex colours).
    - Not in shadows, GI or reflections.
    - It writes no depth or velocity (TAA can smear one that moves fast).
    - Scene materials with `"alpha": "blend"` have their own pass (T1, below).
  - **Test:** `editor_script_mesh`. An opaque red quad and a see-through blue one over it
    (`editor_mesh_drawn`). The red one's vertices replaced by a triangle (`_updated`). Gone after
    Stop (`_gone`).

## Transparency (T1)

- **T1: alpha-blend materials.** A material with `"alpha": "blend"` (or glTF's BLEND) is drawn
  see-through, blended over the lit scene, farthest first. Before this it drew opaque.
  - **Culling (`cluster_cull.slang`):** the first cluster cull sends blend clusters to a list of their
    own instead of the visibility buffer, after the LOD cut, the frustum and (single-sided
    materials) the cone test.
  - **After the opaque passes (`transparent_cull.slang`, `render/transparent_renderer.h`):**
    - **Cull:** the list against this frame's final HiZ, so glass behind a wall costs nothing more.
    - **Sort:** one group of 1,024 threads sorts up to 4,096 clusters farthest first, in
      groupshared memory (bitonic). The key is the cluster's view depth, then a hash of the cluster
      and instance, so ties sort the same every run. A scan of their triangle counts gives each
      cluster its place in the draw. Over 4,096 clusters, the rest are dropped and the overflow
      counted (`transparent:` in the stats log).
    - **Expand:** the index buffer in sort order. A single-sided material's back faces become
      degenerate triangles. A double-sided one's back faces go first, so a closed shape's far side
      draws before its near side.
  - **Draw (`transparent.slang`):** one indexed indirect draw over the lit frame, after
    reflections and MSAA, before runtime meshes and particles.
    - The opaque depth is its depth target, tested (reverse-Z GREATER) and never written, so what
      the scene hides is rejected before shading.
    - **Shading** is the resolve's: the material's textures and factors, its surface shader (R4),
      the sun and its shadow, the local lights and their shadows, specular from the ambient.
    - **Ambient:** the probe GI, sampled as the resolve samples it. Without GI, the flat sky.
    - **Premultiplied:** the surface's own light fades with its alpha. What it reflects and what it
      emits don't, so clear glass still shows highlights.
  - **TAA and FSR:** see-through surfaces have no motion vectors, so the pass writes the reactive
    mask (its coverage). The renderer now owns the mask: the transparent pass and particles each
    add to it, and the first to draw clears it.
  - **Textures:** graphics stages can't write virtual-texture feedback, so the renderer asks for
    the pages of the blend materials in view itself, at the mip each instance's size on screen
    asks for (no more than 64 pages per level), as particles do.
  - **Shadows:** a blend material casts where its alpha reaches `alphaCutoff`, like an alpha-masked
    one. Clear glass casts nothing, while dense parts still cast.
  - **GI:** blend materials are left out of the BVH; they neither block nor bounce light.
  - **Picking:** the editor's pick reads the visibility buffer, which has no transparent surfaces.
    A second pass (`transparent_pick.slang`) casts the pixel's ray against this frame's
    transparent triangles; the nearest hit in front of the opaque depth wins.
  - **Not drawn** in the debug views, reflections or water's refraction.
  - **Cost:** 200 overlapping double-sided glass spheres (563 clusters in view): cull, sort and
    expand 0.05 ms; the draw 0.12 ms (the scene's resolve: 0.11 ms).
  - **Tests:** `editor_transparent`.
    - A red wall with a half-transparent blue pane and a green one in front: the panes change the
      frame (`editor_transparent_drawn`).
    - A yellow pane behind the wall draws nothing (`_hidden`).
    - Without the wall, the nearer pane is over the farther one from the front (green,
      `_order_front`) and from the back (blue, `_order_back`).
    - Picking selects the pane in front, the one behind through it, and the wall past them.
  - `falcon_imgdiff img --dominant x y r|g|b` checks which channel is a pixel's largest.
- **T2: refraction, and surface shaders on transparents.**
  - **A refracting material:** `"refraction"` (0..1) and `"ior"` (1..3, default 1.5) in a blend
    material's `.fmat` (the editor's material panel: Refraction, IOR). The pass then draws what is
    behind the surface itself, from the frame before transparents with its mips (the reflection
    source, `reflection_source`), and writes alpha 1.
    - **Bent:** the lookup moves by the shading normal's sideways part on screen, times
      `refraction` and `1 - 1/ior`. A pane facing the camera shows what is behind it straight; a
      ball shows it magnified and turned over, like a lens.
    - **Blurred:** rough surfaces look up a coarser mip (roughness squared over the mips): frosted
      glass.
    - **Tinted:** what is seen through it takes the surface's colour.
    - **Nothing in front leaks in:** where the bent lookup lands on something nearer than the
      surface (this frame's HiZ says), the straight one is used.
    - **Cost:** the reflection source is made every frame anyway while reflections or water are on;
      otherwise only while a refracting material is in the scene. One extra sample per fragment.
  - **Material params** are now 32 bytes each after the page table: the surface shader's four,
    then refraction, ior and (V1) the vertex function's bound and distance
    (`editor/material_library.h`). Flag bit 4 marks a refracting material.
  - **Surface shaders on transparent materials:** the editor builds the transparent pass with the
    project's surface shaders too (`Cache/Shaders/transparent.fshader`), alongside the resolve, both
    compiled at once; the renderer takes both builds or neither (`setProjectShaders`).
    - `SurfaceIn` gains `screenUv` and `transparent`.
    - `SurfaceOut` gains `alpha` (the coverage), `refraction` (the screen offset, from the
      material's) and `refractionBlur` (a mip): custom glass, water or heat haze.
  - **Limits:**
    - A refracting surface hides transparents behind it (it sees the frame before them).
    - What it sees has no screen-space reflections or water in it.
  - **Tests:** `editor_refraction`. A glass ball in front of a striped wall: bent
    (`editor_refraction_bent`), blurred when rough (`_rough`), and with a surface shader of the
    project on it, tinted red, more opaque at its rim and bent twice as far (`_surface`).

## Vertex functions (V1)

- **V1: vertex functions in surface shaders (wind, waves, flags).**
  - **Writing one:** a surface shader with `void vertex(VertexIn i, inout VertexOut o)`
    (`falcon_surface.slang`). It gets the vertex's mesh and world position, world normal, uv, the
    material's params, the time, the instance's position and a per-instance random number. It
    gives a world offset and, if it wants, a normal.
    - It must be a pure function of what it gets.
    - `.fmat` gains `vertexBound`, how far it moves vertices at most in metres (default 1; the
      instance's culling sphere grows by it), and `vertexDistance`, the camera distance it runs
      within (0: any; past it the surface stays still).
    - The editor's material panel shows both for a material with a surface shader.
  - **How it runs: deformed like a character.**
    - The instances drawn with such a material, nearest first and within its distance, each get a
      deform record of the skinning system (`render/skinning.h`) from a budget, bound and freed at
      runtime (`GpuDrivenRenderer::updateDeformers`).
    - Their rows are streamed with `kInstanceSkinned` and the record. A material override counts,
      and so does any cluster of the mesh.
    - Once a frame, before anything is drawn, `vertex_deform.slang` writes their vertices into the
      skinned-vertex buffer, one group per cluster, the cluster's material deciding whether they
      move. The skinning's bounds pass then bounds their clusters.
    - So the function runs once per vertex per frame. Everything that reads a character's vertices
      sees the moved surface: the visibility raster and MSAA, the resolve (motion vectors from last
      frame's generation, so TAA follows it), transparents, picking, cluster culling (exact
      bounds), the shadows (their pages re-rendered each frame, as a posed character's), GI's trace.
    - `vertex_deform.slang` is the only engine shader built with the project's surface shaders for
      this (`Cache/Shaders/deform.fshader`, a third job beside the resolve and the transparent
      pass). The engine's own build leaves vertices where they are, and with no vertex functions
      no record is bound: it costs nothing.
  - **Budget:** 512 records, 256K vertices (two generations of 32 bytes: 16 MB) and 16K cluster
    bounds when the project has vertex functions as a scene loads; 64 records and 16K vertices
    otherwise (a vertex function added later works within that until the scene is loaded again).
    Past the budget the farthest stay still. The stats log: `vertex functions: N instances
    deformed, V vertices`.
  - **Cost:** 200 waving spheres: the vertex pass and the cluster bounds 0.10 ms; their shadow
    pages are re-rendered every frame (shadows 0.20 ms).
  - **Limits:**
    - Not on skinned (boned) meshes.
    - A vertex in clusters of two materials moves by the last one written.
    - Physics collision and the ray tracing BVH use the still mesh.
    - Instances drawn many thousands of times (grass) each take their own vertices: keep
      `vertexDistance` short for them.
  - **Tests:** `editor_vertex_shader`. `Shaders/Wave.slang` lifts a sphere by `params.x` and waves it
    by `params.y`.
    - Lifted 1.5 m: the frame changes (`editor_vertex_lifted`), and picking where it is now
      selects it.
    - Waving: two moments differ (`_waving`).
    - Past its `vertexDistance`: as still (`_far`).

## Engine systems from C# (X1)

The roadmap's X1–X4 let C# take part in the engine's systems, not just call them.

- **X1: systems, phases and physics events (API v32).**
  - **Systems:** a `GameSystem` is the game's logic without an entity. Every non-abstract one is
    registered by the source generator, made when the game starts (`Order`, lower first) and kept
    across scene loads. It is made again on a hot reload (its fields start over); `Systems.Get<T>()`
    finds one.
  - **The frame's phases, in order:**
    1. `OnEarlyUpdate`, then the Game's `OnUpdate`, the systems' `OnUpdate` and the components'.
    2. `OnFixedUpdate` with the components' at each physics step (the simulation's tick), and
       after the step that step's collision and trigger events to the components of the entities in
       them (since API v36; before, they came once per frame in the next phase).
    3. After physics: `OnAfterPhysics` (systems and components).
    4. `OnLateUpdate` (systems, then components), with animation and physics done.
    5. `OnPreRender` (systems), where the frame's transforms are final. What it moves is
       propagated again before it is drawn.

    The engine calls the later phases through `FxManagedApi::phase`
    (`ScriptHost::phase(AfterPhysics | LateUpdate | PreRender)`).
  - **Physics events on components:** `OnCollisionEnter(Collision)` (what was hit, where, which way,
    how hard: contact begins, as the physics sounds hear them) and `OnTriggerEnter` /
    `OnTriggerExit(Entity other)`, on both sides. They come inside the tick whose step made them
    (API v36); polling `Physics.Contacts()` in `OnAfterPhysics` sees all of the frame's, in `OnUpdate`
    the frame before's.
  - **Test:** `editor_script_systems`. A system's phases run in their order in a frame (`EUALP`). A
    ball falls through a trigger zone onto a floor: its component hears the zone once in and once
    out, the floor's hit, and its after-physics and late updates.
- **X2: data components saved with the scene (API v33).**
  - **What one is:** a game's `[Component]` struct (the name is optional now: without it, the full
    name) is a data component. The source generator registers it with its public fields' kinds and
    offsets (float, double, int, uint, long, ulong, byte, bool, enums by their underlying type,
    Vector2/3/4, Quaternion, Entity) and its defaults (`new T()`: a parameterless constructor's
    values). `FxManagedApi::dataTypes` describes them (`ScriptHost::dataTypes()`).
  - **In the editor:** Add Component lists them (`Data:<name>`). The Inspector draws a section per
    data component on the entity, its fields editable (`editor.data.set`, one undo step; entity
    fields are shown, not set, as their ids change from run to run). Remove Component takes one off.
    Stop and undo bring them back with the rest of the entity (`State.data`).
  - **Saved with the scene:** an entity's edits keep `"data": [{name, size, align, bytes}]` (hex,
    `DataComponent` in `editor/scene_edits.h`). Loading sets them by name, registering the
    component with that layout when the world has none, so the game's queries find them at its
    start. A struct the game no longer has keeps its bytes, with a warning in its section. A struct
    whose size changed no longer matches: its saved values are dropped. Not in prefabs yet.
  - **Test:** `editor_script_data`. A `Health` struct is added to a cube with its defaults, three
    fields are set (a fourth set is undone), and a system's query reads them while playing. They
    are kept at Stop, saved, and read again after opening another scene and back. Removing the
    component is undone.
- **X3: gizmos, buttons and Inspector pages (API v34).**
  - **Gizmos:** a component's `OnDrawGizmos()` runs every editor frame, editing and playing, and
    `OnDrawGizmosSelected()` too while its entity is selected; an editor tool's `OnDrawGizmos()`
    as well. They draw with `Gizmos` (`Line`, `Ray`, `Arrow`, `Box`, `Circle`, `Sphere`; `Color`),
    lines over the editor's views for that frame. While editing, the components are made from the
    scene's values (`FxManagedApi::drawGizmos` gets the scene's script components when they
    change; no `OnAwake` / `OnStart`); while playing the running ones draw. `test.expect
    {gizmoLines}` counts them.
  - **Buttons:** a component's `[Button]` method (public or internal, no parameters; a label
    optional) is a button in its Inspector section (`editor.script.button`). While editing it runs
    on one made from the scene's values, and the properties it changes are kept as one undo step
    (`componentButton`; entity properties are left as they were). While playing it runs on the
    running component.
  - **Inspector pages:** `[InspectorPage("page.html", Height = 120)]` on a component class draws
    that HTML page (in the project's `UI/`, or its library's) in its section. With
    `falcon-panel.js`, the page hears the component's values (`{type: "values", values}`) and
    sets them with `FalconPanel.send({set: {name: value}})`, one undo step each. The frames live
    over the Inspector (`Drawers` in `editor.js`), kept while their section is there, since the
    Inspector is rebuilt on every change and a moved frame would load again.
  - **Test:** `editor_script_gizmos`. A component's sphere (72 lines, 84 with its box while
    selected) while editing and playing. Its buttons double and reset a property, each undone in
    one step. Its page, poked (`test.drawer`), sets a property, undone.
- **X4: audio streams and shapes from scripts (API v35).**
  - **Audio streams:** `new AudioStream(channels, seconds)` is a ring of float samples (48 kHz,
    interleaved) the game writes on the main thread (`Write`: what there is room for; `Queued`,
    `Free`) and a mixer voice plays on its thread, with no managed code there (`AudioStream` in
    `audio/audio_mixer.h`, `AudioVoiceParams::stream`). `Play2D`, `Play(position)` and
    `PlayOn(entity)` place it like any sound, on a bus. Running dry plays silence and counts an
    `Underruns` (once it has played something). `Dispose`, or the game's end, stops it; its memory
    goes once the mixer is done with it.
  - **Shapes from scripts:** `Physics.SetConvexHull(entity, points)`, `AddConvexHull` and
    `AddShape(entity, Box | Sphere | Capsule, size, center, rotation)` give an entity (with a
    PhysicsBody or a Collider) shapes in place of its Collider's, several making a compound. They
    are in its space, scaled with it, and keep its Collider's friction, bounce, layers and trigger
    flag. `ClearShapes` puts its Collider's back. The body is built again at the next step
    (`PhysicsWorld::addScriptShape`); the game's end clears them.
  - **Test:** `editor_script_stream`. A 440 Hz sine written as it plays is heard (the mixer's peak,
    `test.expect {audio: {peakAbove}}`) and is silent once disposed (K held). A convex hull (a box of half
    size 0.2 as 8 points) and a compound of two spheres (radius 0.4) rest on a floor at those
    heights.

## Libraries

Shareable libraries bring code, editor tools, UI and assets to a project (`project/libraries.h`):

```
MyWorld/Libraries/<name>/
  library.json   {"name", "version", "description", "author", "dependencies": [names]}
  Code/          C# built into the game (script components, systems): ships with it
  Editor/        C# for the editor only (E2)
  UI/            HTML / JS / CSS for its editor panels (E3)
  Assets/        models, materials, sounds, prefabs
```

- **Sharing:** as a folder, a `.zip` or a git repository. File > Libraries… (the Library Manager) adds
  one from any of the three, makes a new one, exports one to `Exports/<name>-<version>.zip`, turns one
  off, or removes it.
  - A zip is extracted with Windows' `tar`, and a repository is cloned with `git clone --depth 1` (at a
    branch or tag).
  - `library.json` may be at the top or one folder down.
  - Installs run on a thread and finish in the editor's loop (`test.wait {libraries}`).
  - `project.falcon` lists each library's source and whether it is on:
    `"libraries": [{"name", "source", "ref", "enabled"}]`. `saveProject` now keeps keys it doesn't
    know, so the project settings don't drop that list.
- **Trust:**
  - A library's code is built into the game and runs with full access, like the project's own
    scripts, so it is built only once this user trusts it.
  - Trust is per user (`trusted-libraries.json` in the editor's app data) and per name@version.
  - Adding or making a library through the manager trusts it, after a warning. A library found in
    the folder (copied in by hand, or part of a project made elsewhere) is listed as not trusted
    until the user trusts it.
  - With `--no-prefs` (tests), trust is kept in the project's Cache instead.
- **Building:**
  - Before each build the editor writes `Cache/Scripts/Libraries.props`, which compiles the `Code/` of
    every enabled, trusted library into the game. `Falcon.Game.targets` imports it.
  - Libraries' components are registered by the same source generator, and Add Component lists them.
  - The script watcher also watches `Libraries/*/Code` and `library.json`. A dependency the project
    lacks is an error on the library that needs it, and dependencies come first in the list.
- **Assets:** an enabled library's `Assets/` is the project's too. Its models are cooked into the
  scenes and its materials are listed (`projectModels`, `projectMaterials`). Paths are
  `Libraries/<name>/Assets/...`, so they don't collide.
- **Test:** `editor_library`:
  - A library copied in by hand is found but not trusted, and its component can't be added.
  - Once trusted, its component is added.
  - It is exported, removed, added back from the zip (trusted, its source in `project.falcon`) and
    played, and its component shows on the HUD.
  - It is turned off, and a new library is made from the manager.
  - Git installs aren't covered by a test (it would need a repository).

### Editor code (API v21)

A library's `Editor/` folder and a project's `Scripts/Editor/` hold C# for the editor itself:
`managed/Falcon.Engine/Editor.cs`, namespace `Falcon.Editor`.

```csharp
public static class AlignTools
{
    [EditorMenu("Tools/Align/Drop to Ground", Shortcut = "Ctrl+Alt+D")]
    public static void Drop()
    {
        using var _ = Undo.Scope("Drop to ground");  // every change below: one undo step
        EditorEntity e = Selection.Entity;
        EditorScene.SetPosition(e, e.Entity.Position with { Y = 0 });
    }
}

public sealed class Watcher : EditorTool  // lives while the editor runs
{
    public override void OnSelectionChanged(EditorEntity e) { }
    public override void OnPlay() { }
}
```

- **Built into the editor's build of the game only.** The editor builds with `FalconEditor=true`: it
  defines `FALCON_EDITOR`, adds libraries' `Editor/` code (`Libraries.props`), and keeps
  `Scripts/Editor/`, which other builds leave out. It is loaded and hot-reloaded with the game, so
  editing a tool and saving it updates the editor in about a second. A shipped game has none of it.
- **Registration:** the source generator registers `[EditorMenu]` static methods and `EditorTool`
  classes, with no reflection. Menu items appear under the Tools menu, with `Tools/A/B` as nested
  submenus (`editor.tools`, sent after each load). Running one (`editor.tools.run`) is queued, and the
  engine hands it to the game's code outside the editor's own message handling.
- **The editor API** (`FxEditorApi`; `null` in a game, so `EditorApp.Available` is false there):
  - `EditorApp.Send(json)` sends an editor message the way the editor's page does
    (`editor.entity.create`, `editor.transform.set`, `editor.component.add`, `editor.script.set`,
    `editor.prefab.create`, `editor.play`, ...). Each one is handled at once and can be undone, so
    editor code can do anything the page can.
  - `EditorApp.Query("entities" | "selection" | "project" | "playing")` returns the editor's state
    as JSON.
  - Helpers over those: `Selection.Entity` (get and set), and `EditorScene.Entities`, `Find`,
    `Create`, `Delete`, `Rename`, `SetTransform`, `SetPosition`, `AddComponent`, `RemoveComponent`
    and `SetProperty` (a script component's property). Also `EditorApp.Play` / `Stop` / `SaveScene`
    and `Undo.Perform` / `Redo`.
  - `EditorEntity` is a Hierarchy id. Its `.Entity` is the ECS entity, and `EditorEntity.Of(entity)`
    converts back.
  - `Undo.Scope(name)`: the changes between it and its `Dispose` are merged into one undo step.
    Scopes can nest.
- **EditorTool:** made when the game's code loads, and again after a hot reload. Its callbacks are
  `OnUpdate` (every editor frame, editing or playing), `OnSelectionChanged`, `OnPlay`, `OnStop`,
  `OnSceneSaved` and `OnUnload` (before a hot reload). Each call is guarded on its own.
- **Test:** `editor_library_api`, using a library whose `Editor/` holds two Tools menu items and an
  `EditorTool`:
  - One menu item renames the selection, and a single undo reverts it.
  - The other makes an entity and moves it, as one undo step that undoes and redoes as one.
  - The tool hears the selection, Play and Stop, and writes them to a file the test reads.

### Custom UI: editor windows and the game's HTML UI

Editor windows and the game's own UI are HTML pages, framed by the editor's page. They talk to
C# through it by `postMessage`, so cef-lite needs nothing new: the editor's CSP allows
`frame-src file:`.

- **Editor windows:** an `[EditorWindow]` class shows one of its library's pages (`UI/`; the project's
  `UI/` for `Scripts/Editor/` code) in a floating window of the editor, listed in the Tools menu.
  ```csharp
  [EditorWindow("Tools/Terrain/Painter", Title = "Painter", Page = "painter.html", Width = 360, Height = 420)]
  public sealed class Painter : EditorWindow
  {
      public override void OnMessage(string json) => Send("\"done\"");  // its page's message; an answer
  }
  ```
  - **The page** uses `falcon-panel.js`: `FalconPanel.send(value)` arrives in `OnMessage` as JSON text,
    and `Send(json)` reaches `FalconPanel.onMessage`. The page also gets the editor's colours as CSS
    variables (`--panel`, `--text`, `--accent`, `--field`, ...), so `var(--panel)` matches the editor.
  - **In the editor:** the window can be dragged by its title bar and resized. It closes from its title
    bar (`OnClose`), and `editor.window.show` opens it from code or a test.
  - **Hot reload:** a window that is open gets its C# side made again on the new code.
  - **Starting point:** a new library comes with an example window (`Editor/ExampleWindow.cs`,
    `UI/example.html`) and a copy of `falcon-panel.js`. The window's library is worked out by the
    generator from its source file's path.
- **Inspector:** `[Property(Group = "...")]` puts a heading over a component's properties, and
  `Tooltip`, `Range` and `Asset` shape their rows. Custom inspector drawers built from a page aren't
  there yet.
- **The game's HTML UI:**
  - A project's `UI/Game/index.html` is framed over the game's view while the editor plays. Its
    messages reach `Ui.TryReceive`, and `Ui.Send` reaches it. Before, a game's UI did nothing in the
    editor.
  - The page uses `falcon-ui.js` (`FalconUi.send(json)`, `FalconUi.onMessage`), copied from the
    engine's `falcon-ui/editor/`. The same page works in a game with `--ui <project>/UI/Game`, through
    the engine's own bridge.
  - In the editor, the game's mouse input takes the view, so the page is for showing things there.
- **Live reload of the project's pages:** the editor watches the project's `UI/` and its libraries'
  `Libraries/<name>/UI/` folders. When a file there changes, every frame showing a project page loads
  it again (`editor.pages.changed`): the game's UI while playing, editor windows, Inspector pages. A
  reloaded page says hello again, so editor windows' C# sides and the game hear from it as before.
- **Tests:**
  - `editor_library_ui`: a library's window and its C# side talk both ways (the page says hello, the C#
    side answers, the page tells it what came back).
  - `editor_game_ui`: the game's page and the game talk both ways while playing; `game.js` edited
    while playing, the frame runs the new script.
  - `editor_library_ui` also edits its window's script: the window reloads and says the new thing.
  - `editor_page_reload`: a file written into (a copy of) the editor's bundle reloads the editor page,
    which gets the state again; removed, it reloads once more.
