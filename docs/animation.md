# Animation

## Animation

The runtime is a port of Esoterica's animation system (MIT, Bobby Anguelov and contributors;
pinned in `external/esoterica`). It lives in `engine/anim`.

```powershell
./build/release/bin/falcon.exe cook Fox.glb samples/cooked/fox.fpak --graph samples/graphs/fox.graph.json
./build/release/bin/falcon.exe --scene samples/cooked/fox.fpak --crowd 99 --anim-set Speed=2.5
```

- **Cooking:**
  - Each glTF skin becomes a skeleton. Bone 0 is the armature root, and parents always precede
    their children.
  - Clips are sampled at ~30 fps and compressed the Esoterica way: a 48-bit smallest-three
    rotation, a 48-bit translation quantized to each track's range, and a 16-bit uniform scale.
    Constant tracks are stored once. Root motion is bone 0's track.
  - `--verify` decodes every frame with the runtime sampler and checks it against the source,
    within one quantization step.
  - Fixed from the original: static values come from the clip itself, and sync markers are
    stored as percentages.
- **Runtime:**
  - Skeleton, pose, frame time, sync tracks, root motion and the blender, ported exactly.
    Esoterica's `a * b` (DirectXMath order) is written out as Hamilton products and
    `combine(local, parent)`.
  - A pose task system: a flat per-frame list, at most two earlier dependencies per task, and
    pooled pose buffers.
  - Animation graphs. A graph is instanced into one memory block, nodes initialize lazily
    (refcounted and owned by their parent), and value nodes evaluate once per update. Ported
    nodes:
    - pose: clip, Blend1D, Blend2D, SpeedScale, selector, reference pose, layers, state
      machine, state, transition (synchronized or not, clamped, matching source time or sync
      events, sync offsets, smoothstep easing, root-motion blend modes)
    - values: parameters, constants, and/or/not, float compare/range/math/remap/clamp, ID
      compare, state-completed and time conditions, event and event-percent conditions, bone
      masks and mask blends
  - **Blend2D:**
    - The compiler triangulates the points (Delaunay, with up to 32 points; cocircular grids get
      one triangulation) and keeps their convex hull. Fewer than three points, two at the same
      place, or all on one line are errors.
    - Inside, the input's triangle gives a three-way blend by barycentric weights. Outside, the
      nearest hull edge gives a two-way blend.
    - Sync tracks, durations and events are blended by the same shares.
  - **Layers:**
    - A base pose, and layers over it in local space. Each layer is overlaid or additive, has a
      weight (a node or a constant) and an optional bone mask, and can be synchronized to the
      base.
    - Events are weighted by the layer's weight, or left out.
    - Root motion is the base's, unless a layer adds its own.
    - A layer at weight 0 drops its pose tasks.
  - **Bone masks:**
    - The graph's `boneMasks` give per-bone weights, propagated down the hierarchy; an unknown
      bone is an error on its node.
    - Mask Blend lerps two masks. Masked blends are tasks.
  - **Graph result:** a graph's pose is its root's result, not whichever task ran last.
  - **Not ported yet:** model-space (global) layers, state-machine layers, cached-pose (forced)
    transitions, referenced graphs.
- **Authoring:** graphs are JSON (`.fgraph`). The format is documented in
  `engine/anim/anim_graph_compiler.h`.
  - Nodes are written inline, or in a `nodes` map and referenced as `"@id"`. A value node can
    feed several inputs; a pose node has one parent.
  - `"$Name"` refers to a parameter, and bare numbers are constants.
  - The compiler lives in the engine, so the cooker and the editor share it. It turns a graph
    into a flat node table and keeps each runtime node's id.
  - Errors are reported per node, all in one pass, for example
    `machine:Idle->Nowhere: unknown state 'Nowhere'`.
  - Examples: `samples/graphs/fox.graph.json` (inline) and `fox.fgraph` (`@id`), which animate
    identically.
- **GPU skinning:**
  - Each skinned instance gets a record, skinning matrices (model bone × inverse bind) and two
    generations of skinned vertices, so motion vectors see the deformation.
  - Per frame, a compute pass skins every vertex (4 weights), and another rebuilds every
    cluster's bounding sphere.
  - Culling uses those dynamic spheres and the character's animated bounds. A single
    instance-wide distance picks the LOD, so the DAG cut stays crack-free while bind-pose
    group spheres move. Backface cones are off for skinned clusters.
  - Visibility, resolve and shadow passes read the skinned vertices. A character whose pose
    changed invalidates the shadow pages under its old and new bounds.
- **Characters:** every skinned instance is a character. It runs its skeleton's first graph,
  or loops its first clip. Characters update in parallel on the job system.
  - Hidden instances (a pool's unused slots) are neither evaluated nor skinned.
  - A skinned mesh gets only two spare editor slots, since each slot carries a skinning record.
- **In projects:**
  - Models in `Assets/` keep their skins and clips. Each skeleton is named `<model>#<skin>`, and a
    graph's `"skeleton": "Assets/Hero.glb"` means the model's first one.
  - An animations-only export of the same rig shares the model's skeleton. A clip whose name is
    already taken on that skeleton becomes `<model>#<clip>`.
  - Graphs (`.fgraph` in `Assets/`) are compiled when first used and again when the file
    changes, so editing a graph never needs a cook.
- **Animator component** (Inspector → Add Component, on an entity with skinned meshes in it or
  below it):
  - One graph instance drives every skinned mesh of the entity and its children. The followers
    take the leader's pose.
  - It picks the project's first graph for that skeleton. Its parameters start at the values set
    here, or at the graph's defaults.
  - The pose shows in edit mode, Play plays it, and Stop puts every character back to its start.
  - **Preview in Editor** (a checkbox) plays the graph in edit mode too, with its parameters at the values
    set here. The IK follows its entities as you move them: drag an aim's hold or a look's target and the
    pose follows. The root stays put (nothing applies root motion while editing), and scripts don't run.
    Play and Stop start the character again from the start. Only the setting is saved
    (`"editorPreview": true`), never the pose, and a game ignores it.
  - It is saved in the scene's edits (`"animator": {"graph", "params", "entities", "editorPreview"}`), and a game
    binds it the same way at load. Animators are not in prefabs yet.
  - **Entity parameters:** the graph's entity parameters (what its IK follows, see the Animator tab's IK) show
    as entity slots. Each one is bound to another entity (an Empty; not itself or a bone): pick it from the list,
    or press the slot's pick button and then click the entity in the Hierarchy (Esc cancels). Bindings are saved
    by the entity's key (`"entities": {"AimHold": "c45"}`), so a reopened scene and a game find them again. A
    deleted entity's bindings go with it (Undo brings both back). Scripts can set them
    (`Animation.SetEntity`).
  - **IK settings live in the graph** now, not here, because not every Animator drives a humanoid. Older scenes'
    Animator fields (`"footIk"`, `"footReach"`, `"targets"`: look, aim, hold, leftHand, rightHand, aimBone,
    aimAxis) are migrated as they are read. They become the Animator's own IK (`"ik"`, the same form as a graph's)
    and bindings named LookTarget, AimTarget, AimHold, LeftHandTarget and RightHandTarget, so the character moves
    as it did. The Animator's own IK is used only while its graph has none. The Inspector flags it, and **Move IK
    to the Graph** writes it into the graph file (its `"ik"`, plus entity parameters for its targets) and drops
    it from the Animator, which keeps the bindings.
- **Inverse kinematics** (`anim/anim_ik.*`; `AnimationSystem` runs it after the graph's pose, before
  skinning, on the main thread because it casts physics rays). What runs is the graph's `"ik"` section
  (`anim/anim_ik_settings.h`; the Animator tab's IK), or a script's calls. In order:
  - **Feet** (the graph's `"feet"`, scaled by a script's `Animation.SetFootIk`):
    - A ray down at each foot finds its ground. The foot is lifted or lowered to it, as far as the reach,
      keeping the clip's own lift as it swings.
    - The hips come down as far as the lower foot needs.
    - The thigh and calf bend to put the ankle there (a two-bone solve in the plane the leg bends in).
    - The foot tilts to the slope as it plants, at most about 35 degrees.
    - All of it is smoothed, and snaps in edit mode.
    - The legs are found by name: `foot_l` / `LeftFoot` / `Foot.L`, its calf and thigh. UE's `ik_foot_*`
      helpers, toes and twist bones are skipped.
    - In the editor, colliders added there are bodies in Play only. In edit mode the feet find the terrain and
      the cooked colliders.
  - **Aim** (the graph's `"aim"`, or `Animation.AimAt`), before the look:
    - The spine bones the aim bone hangs from (its nearest ancestors named like spine or chest) share the turn
      until the bone's axis points at the target, at most 80 degrees. The bone moves as the spine turns, so the
      turn is found again from there (4 passes).
    - The axis is in the bone's own space: a weapon's barrel, as its mesh lies in the bone at bind. Without
      one, it is the model's +Z in the graph's pose (for a clip that aims straight ahead).
    - With a hold point, after the look the bone's arm brings it there (a two-bone solve) and it turns to
      point at the target from there. The support hand keeps where it held the bone in the pose (its grip),
      its own arm reaching there.
    - While aiming, the look leaves the spine to the aim (its spine shares scaled by the aim's weight) and turns
      the neck and head.
  - **Look** (the graph's `"look"`, or `Animation.LookAt`): the bones from the spine up to the tip (named like spine, chest, neck or
    head) share the turn toward the point. The head turns most, and the tip ends exactly on the point, at most
    75 degrees from where the clip looks. The tip's forward is the model's +Z (glTF's front) at rest.
  - **Reach** (the graph's `"reach"`, or `Animation.Reach`): a bone (a hand) reaches a point with its parent and grandparent. The bone
    keeps its turn, and up to 4 bones can reach at once. Reaches run after the look, so they reach from where
    the look turned the arms.
  - Everything is in the character's world (its mesh's transform, brought up to date before the update).
    Bone entities (Expand Bones) follow the IK pose. Stop drops scripts' aims, looks, reaches, weights and
    entities.
  - **The graph's IK** (`"ik"`): each part is on while it is present. Targets are entity parameters
    (`"parameters": {"AimHold": {"type": "entity"}}`), read where their entities are each frame. A missing or
    disabled entity lets that part go. Weights are a constant or `"$Param"` (a float or bool parameter),
    1 by default.
    ```
    "ik": { "feet":  {"reach": 0.45, "weight": "$Grounded"},
            "look":  {"target": "$LookTarget", "bone": "head"},
            "aim":   {"hold": "$AimHold", "target": "$AimTarget", "bone": "hand_r", "axis": [-0.981, 0.192, 0],
                      "support": "", "spine": 1, "weight": "$Aim"},
            "reach": [{"bone": "left hand", "target": "$HandTarget"}] }
    ```
    The aim's `hold` holds the bone at its entity, pointing at `target` or else where the hold faces (its -Z, as a
    camera looks), so turning the entity turns the weapon. `support` is the hand that keeps its grip: empty for
    the other hand, `"none"` for no hand. `spine` (0 to 1, 1 by default) is how much the spine turns toward the
    target. At 0 the body stays as the clip has it: the arms alone bring the weapon to the hold and the wrist
    points it (aiming down the sights without leaning in; it needs a hold). While aiming, the look leaves the
    spine alone either way. A reach's bone can be `"left hand"` / `"right hand"` (found by name).
    An entity parameter used as a value input is a compile error. A script's `LookAt`, `AimAt` or `Reach` (for
    that hand) takes that part over until Stop. Cooked (pak) graphs have no IK.
- **Animator tab** (centre dock, beside Scene and Game). Open it by double-clicking a `.fgraph`
  in the Project panel, or with the Animator component's Open button.
  - **Graph panel:** the graph's parameters, which drive the preview, and its compile errors,
    each with its node. A parameter's type is float, bool, id or **entity**: a reference each Animator binds
    to an entity. It has no value in the preview.
  - **IK** (in the side panel): the graph's IK, solved in the scene and the game (not in the preview):
    - **Foot IK** with its reach.
    - **Look** with its target and bone.
    - **Aim** with its target, hold, bone, axis (the barrel in the bone's space), support hand and spine (how
      much the spine turns; 0 for the arms only).
    - Up to 4 **Reaches**.
    - Each part has a weight: a constant, or a float or bool parameter.
    - A target slot lists the graph's entity parameters. **New entity parameter** adds one named for the slot
      (AimHold, LookTarget...).
  - **Preview model:** any model of the scene with the graph's skeleton.
  - **Preview pane:** its own small renderer (`engine/render/preview_renderer.h`,
    `animator_preview.slang`):
    - one character on a ground disc with a grid;
    - a sun with its own 2048² shadow map and sky ambient;
    - the scene's virtual-textured materials, whose pages are asked for from the CPU because a
      graphics pass can't write the feedback list;
    - 4x MSAA, tonemapped with AgX.
  - **Character:** posed by its own graph instance on the skinning system's floating records,
    which have no instance slot. It plays in edit mode too.
  - **Toolbar:** play/pause, restart, frame step, speed, bones, grid, and root motion (in place,
    or moving the ground under the character).
  - **Camera:** left or right drag orbits, middle or Shift+drag pans, the wheel zooms, F frames.
  - **Scene renderer while hidden:** it skips its frames. Skinning, texture streaming and
    readbacks carry on, and its history, HiZ, shadow pages and exposure wait as they were, so
    coming back is instant. Instance changes made meanwhile are streamed on return.
- **Graph editor** (the tab's canvas, `ui/editor/animgraph.js`). It edits the `.fgraph`
  document itself.
  - **On open:** every inline node moves into `nodes` with an id, and every node records the
    view it is drawn in and its position (`editor` members, which the compiler ignores).
  - **Views and navigation:**
    - Flow graphs: the root, each state's pose, each state's entry condition, and each
      transition's condition. Each has a Result node.
    - State machines: states are boxes, the default state has an arrow, and transitions are
      arrows.
    - Double-click a state machine to open it, a state for its pose, or an arrow for its
      condition. Breadcrumbs (or Esc) go back.
  - **Flow graphs:**
    - Typed pins (pose, bool, float, ID). Drag pin to pin to link; a pose has one parent and
      loops are refused.
    - Pick a wire up from its input to move or drop it. Dropping on empty space opens the
      palette with what fits there, parameters included.
    - Double-click, right-click or Space also opens the palette.
  - **State machines:** drag from a state's right edge (or Ctrl+drag a state) onto another to
    add a transition. Double-click or right-click empty space to add a state.
  - **Editing:** box select, move with grid snapping, Delete, Ctrl+C / Ctrl+V, F to fit, and
    wheel zoom.
  - **Side panel:**
    - Parameters: add, rename (references follow), type, default, delete. Their live values
      drive the preview.
    - The selection's fields, including each value input as a link, parameter or constant.
    - Compile errors: click one to show its node.
  - **Every edit:**
    - It is undoable (Ctrl+Z / Ctrl+Y while the tab is open).
    - It recompiles at once. With no errors, the preview and every scene character playing that
      graph swap to it and keep their parameter values. With errors, the last good graph keeps
      playing and the faulty nodes are marked red.
    - Ctrl+S saves.
  - **Live overlay:**
    - Nodes the last update touched glow, and pose nodes show how far through they are.
    - The active state is highlighted, and running transitions show their progress.
- **Events.** A model's clip markup lives beside it as `<model>.fevents` (format:
  `engine/anim/anim_events.h`). Outside a project, `--anim-events file` applies one file to every
  skeleton's clips by name.
  - **Sync events** split a clip's cycle, for example a walk's foot downs. Synchronized blends and
    transitions match them.
  - **Events** are markers or footsteps (left or right), either instantaneous or lasting.
  - **Sampling:** each update, a clip node samples the events between its last and current time.
    This handles loops and reverse playback, and gives how far through a lasting event it is.
  - **Weighting:** a blend weights its inputs' events, and a transition weights its source's and
    target's events (the source's are marked inactive).
  - **States** emit entry, exit and fully-in events (`"events": {"entry", "exit", "fullyIn"}`).
  - **Conditions:** Event (any or all of some ids, optionally from inactive branches too) and
    Event Percent read the update's events, so a transition can wait for one.
  - **Scripts (API v8):** `Animation.Events(entity, span)` and `Animation.Id(name)`. With markup
    (`samples/graphs/fox.fevents`), the Sandbox's foxes step on their footstep events instead of a
    stride's rhythm.
  - **Bones as entities** (`anim/bone_entities.*`): right-click a placed character in the Hierarchy
    and choose **Expand Bones**. Its skeleton appears under it as a folded "Skeleton" entity, in the
    skinned mesh's place, with one entity per bone parented as the skeleton is (each has a `Bone`
    component).
    - Every frame after the animation update, each bone's LocalTransform takes the character's pose,
      and the hierarchy is propagated right away. Anything parented to a bone (Create Child on it, or
      a drop onto it) follows the animation in the editor, in Play and in a game, and physics, audio
      and the render see this frame's place. Scripts see last frame's, because they run before the
      animation.
    - Bones are driven, so they can't be moved, renamed, deleted or given components. Their Inspector
      shows where the animation has them. Collapse Bones is refused while something hangs under a
      bone. Expand and collapse are one undo step each.
    - Saved as the root's `"bones": true`; the bones themselves are rebuilt at load. What hangs under
      a bone names it `"parent": "b<root key>#<bone name>"`, so it survives re-imports. Undo and Stop
      put such an entity back relative to its bone, wherever the bone is by then.
    - Duplicate copies the expanded skeleton, with what hangs on it, onto the copy's bones. A prefab
      can't hold expanded bones yet, so collapse them first. One skeleton per model: the first skinned
      mesh's.
  - **The Animator's G5 tools:**
    - Blend 2D and Layers nodes; mask pins (purple) with Bone Mask and Mask Blend nodes.
    - A Bone Masks list in the side panel. A mask's editor lists the skeleton's bones as a tree,
      each with its weight (blank means inherit).
    - A blend space view in a Blend 2D node's fields: its points, triangles and input point.
      Dragging the point sets its parameters, so the preview follows.
  - **The Animator's Events strip** (under the graph): the preview model's clips on a timeline.
    - Double-click the top row to add a sync event, or the bottom row to add an event.
    - Drag to move. Edit an event's id, type, foot and duration below the timeline.
    - It saves at once, and the graphs start again on the new markup.
    - Sampled events flash on the timeline, and the live clip nodes show playheads.
  - `--crowd N` adds N copies of each one, with varied speeds.
  - Scripts set parameters with `Animation.SetFloat/SetBool/SetId(character, ...)`, where the
    character is an entity or a handle from `Character.Of(entity)` (faster: see C# scripting > Speed).
    The Sandbox walks every fox through idle, walk and run.
- **Cost** (Fox, 25 bones, 1600×900): animation takes about 1 µs of CPU per character across
  6 job threads.
  - 1,000 foxes: 1.5 ms per frame.
  - 4,000 foxes: 5.1 ms per frame, CPU-bound (4 ms of animation).
  - Updating distant characters less often (animation LOD) is the next lever.
- Scenes with fewer than 100 instances are framed from outside (`--view inside|outside`
  overrides it).

Controls:
- **Camera:** hold the right mouse button to look, WASD/QE to move, Shift to go faster,
  mouse wheel to change speed.
- **Debug views:** F1 lit, F2 clusters, F3 normals, F4 instances, F5 triangles,
  F6 LOD level, F7 occlusion pass (green = pass 1, orange = re-tested), F8 HiZ
  (PageUp/PageDown change the level), F9 virtual-texture mip (dim = still streaming),
  F10 albedo only, F11 lights per pixel (heatmap), F12 shadow-map level and visibility.
- **Culling:** `C` freezes culling at the current camera, `O` toggles occlusion,
  `B` toggles backface cones, `[` `]` halve or double the LOD threshold (pixels).
- **Post:** `T` toggles TAA, `G` cycles GTAO / SSAO / off, `-` `=` exposure ±0.5 EV.
- **Physics:** `F` fires a sphere from the camera, `R` drops a pile of 256 bodies, `P` pauses.
- **Sandbox game:** `Space` toggles the fountain, `X` kicks every body upwards.
- **Stats:** shown in the window title and logged every 2 s.

`ctest` also runs render tests on Khronos Sponza. They run the game with `--hidden`, so no
window appears or takes focus:
- occlusion culling is exactly conservative with a moving camera
- the forced re-test path matches
- at full detail the output matches the reference renderer (`--naive`)
- D3D12 and Vulkan agree, with AO and GI at half and at full resolution
- cached shadow pages match pages rendered fresh (sun and local lights)
- repeated captures match, with and without TAA
- FXAA and MSAA change only the edges, MSAA is deterministic and matches across D3D12 and
  Vulkan, and it runs under TAA
- shadows of falling bodies with page caching match shadows rendered fresh, and physics
  matches between 1 and 6 worker threads
- the Sandbox game renders the same frame under JIT and NativeAOT, and a hot reload mid-run
  unloads the old code and leaves the frame unchanged; its footsteps sound the same in both
- on the Fox:
  - every compressed clip frame decodes to its source pose;
  - shadows of an animated crowd match with and without page caching;
  - D3D12 and Vulkan agree;
  - the script-driven crowd matches under JIT and NativeAOT.
- with the UI, scripted editor sessions (Chromium included):
  - move, hide, delete, create, rename, parent (a cooked mesh, a shape, a light and a camera
    under an empty), undo/redo, play/stop and save;
  - empties and parenting: children follow their parent, undo / delete / duplicate cover
    subtrees, reparenting keeps world positions, and a click on an empty's icon selects it
    (`editor_hierarchy`);
  - point and spot lights created, edited and moved between frames: the frame with the shadow
    page cache matches the one rendered without it (`editor_lights`);
  - cameras: Play and the Game tab look through the main camera, and there is one main camera
    (`editor_camera`);
  - a dragged entity lights the scene mid-drag exactly as once the move is committed
    (`editor_drag`);
  - create, duplicate and delete primitives, a GPU pick in the middle of the viewport, tools,
    sun and exposure settings, play / pause / step, and a ping the page must answer
    (`test.expect` steps fail the run);
  - real mouse input (SDL events through the UI layer and cef-lite): a double-click selects a
    whole field, the wheel scrolls the hierarchy, right-click opens a row's menu, a Lighting
    slider drag keeps tracking, and the + button's 3D Object submenu creates the shape clicked
    there. `test.probe` asks the page for element rects and
    its interaction state, `test.mouse` / `test.wheel` aim at those rects, and
    `test.expect {probe: ...}` checks the state;
  - the Profiler: frames stream in with zones, named worker threads, GPU marks and counters, a
    chart click selects a frame, and the Hierarchy fills (`editor_profiler`);
  - saving loaded edits again writes the same file;
  - captures with the edits applied differ from the scene without them;
  - projects:
    - the launcher creates a project (`launcher_create`): the folders, `project.falcon`, the
      cooked Main scene and the recent entry; creating it again fails;
    - the editor on that project (`editor_project`) adds a terrain to the empty scene, saves it,
      adds a second scene and opens it in the same process, then asks to go back to the launcher
      (`--no-relaunch` records that relaunch);
    - reopening the project finds the terrain again (`editor_project_reload`);
    - the Project panel with real mouse, keys and typing (`editor_assets`): a scene is one
      item; a folder made, renamed in place and dragged into another; search; a scene
      duplicated (with its `.fvt`, into `project.falcon`), renamed and deleted;
    - previews (`editor_previews`): the rocks rendered in the background and shown; opened again
      (`editor_previews_cached`), all from the cache and no preview run;
    - placing and importing (`editor_assets_place`, `_model`, `_model_reload`, with the Khronos
      DamagedHelmet when the samples are there):
      - a rock dragged into the view stands on the terrain;
      - a model imported: the scene asks to be reloaded, and Reload cooks it in the
        background; opened again in the same process, the cook is moved in, with the model;
      - the model placed whole, its preview rendered;
      - everything saved and found again;
    - materials (`editor_materials`, `_reload`):
      - a material made and made red, dropped on a sphere in the view: only the sphere changes
        (`editor_materials_differs`);
      - undo and redo;
      - its preview renders;
      - reopened, the sphere draws with it again;
    - prefabs (`editor_prefabs`, `_reload`, `_unpacked`):
      - a group saved as a prefab becomes an instance; another one placed;
      - a change applied reaches the other instance, whose own override stays;
      - a change reverted; a linked entity can't be deleted;
      - reopened, built from the prefab and the overrides;
      - an instance unpacked stays plain entities;
    - cooking before opening (`project_cook`, `_reload`, with the DamagedHelmet):
      - a broken model: the cook fails, and its error shows in a dialog;
      - another running process holds the scene's cook lock: the cook waits, then runs;
      - the model imported again: both copies' textures are copied from the earlier cook;
      - reopened: a lock left by a gone process is ignored, and the cook is moved in.
- asset previews (`falcon --preview`): repeatable, and D3D12 matches Vulkan (`preview_*`).
- an empty scene (`falcon cook --empty --rocks`): sky only, repeatable (`empty_sky`); with a
  generated Mountains terrain and rock scatter, repeatable, and D3D12 matches Vulkan
  (`empty_world`).

## Rigid node animation

A model's own animation (glTF node translation, rotation and scale channels: machines,
doors, a camera move) plays with the scene. Skinned characters keep their clips (above); this is
for everything else.

- **Cooked** (`importNodeTracks`, `tools/cook/import_anim.cpp`): every channel on a node that is
  not a skin's joint, all of a file's animations together. Each frame (30 fps over the longest
  channel: the loop), the hierarchy is composed, and each mesh node and camera whose world
  transform changes gets a track: its transform (3x4) at each frame where it moves, held
  before and after. A node's primitives share one track. They go into the `NodeAnimation`
  section (`fpak::NodeAnimHeader`, `NodeTrackEntry`), a library's by library and node, the
  scene's own by instance. Zero-Day (Measure One): 3056 tracks over 250 frames, 4.8 MB.
- **Cameras:** a model's perspective cameras are cooked with it (name, vertical field of view,
  where they are). Placing the model adds them as camera entities under its root, the first one
  the main camera when the scene has none.
- **Placing:** each entity placed from a model remembers its node (or camera) there
  (`"model"`, `"modelNode"`, `"modelCamera"` in the edits).
- **Playing** (`NodeAnimator`, `engine/scene/node_animator.h`): Play in the editor (from the
  start each time; Stop restores every transform, in one pass) or a game (from its start)
  moves each linked entity along its track, interpolated between frames and looping, and the
  view follows the main camera. A placed entity takes the track as its transform under the
  model's root, so the model moves wherever it was placed.
- **Streaming:** moving instance rows go to the GPU in runs merged across small gaps (a played
  model's thousands of scattered parts were thousands of copies: 1.8 ms of uploads, now 0.04).
  Shadow pages under moving parts are redrawn each frame, with only the moving casters (see
  Sun shadows): Zero-Day's shadows while playing went from 1.8–3.5 ms to 0.55–0.8 ms, Measure
  Seven's from 2.8–3.5 ms to 0.7–1.0 ms (1600×900; the whole GPU frame from 4.6–6.5 ms to
  3.3–4.0).
- **Getting one in:** glTF only. From another format, convert in Blender with the animation
  baked (glTF export, Animation mode "Scene", cameras on).
- Tests: `editor_node_anim` (a cooked test model: its arm turns the box when played, Stop puts
  it back) and `_reload` (the links survive saving).
