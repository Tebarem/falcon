# Level design

The level tools build whole levels inside the engine, from blockout to finished art. The C++ kernel
holds the geometry; the tools are C# (`Falcon.Level`), reached through their own API table
(`engine/script/level_api.h`, `FxNativeApi::level`, with its own version). What they cover:

- **Level meshes** (ProBuilder-style): shapes, object, vertex, edge and face modes, extrude, inset,
  bevel, loop cuts and the rest, as modal operations (axis locks, typed amounts, Esc).
- **Texturing:** materials per face, auto UVs with texture lock, a UV editor, trim sheets, layered
  surfaces painted with masks, vertex colours.
- **Live cuts:** cut volumes that cut level meshes, imported art and prefabs as you drag them.
- **Architecture:** walls and rooms drawn as floor plans, doors and windows that slide along them,
  roofs over them.
- **Wear and tear:** smart materials with edge wear, grime and streaks; decal stamps; damage holes
  with rubble.
- **Modifiers** (mirror, array, solidify, bevel, deform, follow spline, and C# ones) and **splines**
  (roads, kerbs, walls, rails, pipes).
- **Placing:** a prefab shelf, a scatter brush, snap points for modular kits, align and distribute,
  group, replace.
- **Gameplay:** volumes (triggers, kill, ladder, audio, nav), a navigation mesh with agents.
- **Views:** orthographic views and a ruler.
- **Games:** C# builds and cuts meshes at runtime, and networked games share their cuts.

Help/Make Sample Level builds a small level with them in the open scene, to take apart (below).
Help > Keyboard Shortcuts lists their keys under Level design.

## Live meshes

Live meshes are meshes made or changed while the engine runs: level meshes in the editor, and
meshes games build. They draw like cooked meshes (`engine/render/live_geometry.*`):
- Visibility buffer, VT materials, shadows, GI, reflections.
- Picking, down to the triangle.
- A triangle collider for physics, and a mesh for audio occlusion.

**Room.** `ScenePak::reserveLive` reserves it at load, after the terrain's patch slots:
- `--live-meshes N` mesh slots, each with one instance and one mesh row.
- `--live-clusters N` cluster blocks. A block is one cluster with room for 128 vertices and 128
  triangles in the Vertices, ClusterVertices and ClusterTriangles sections.
- Defaults: 4096 meshes and 16384 blocks in the editor (about 68 MB of scene buffer); 64 and 512
  in a game.
- A mesh takes a run of blocks (first fit; neighbours merge when freed). The meshlets that come
  out of clustering usually fill 70 to 95% of a block.

**Writing one** (`LiveGeometry::set`: `prepare`, which reads only its arguments and runs on any
thread, then `commit` on the main thread). Triangles come in the entity's space, with normals, UVs
and one scene material per triangle. Then:
1. Tangents are MikkTSpace from the UVs (`engine/core/mesh_tangents.h`). Corners split where they
   disagree and are welded back.
2. Each material's triangles become meshoptimizer meshlets (128/128). A cluster has one
   material, which is written into its `counts` as the cooker does.
3. Positions are quantized to the mesh's bounds.
4. Clusters are a single LOD level (`parentError` = 3e38, like the terrain's patches), so the
   cull draws every visible one.
5. The rows go up with the frame (`flush`, inside `render()` after `streamInstances`). The cached
   shadow pages under the old and new bounds are invalidated, and the GI probes there update
   first.
   - The shadow map's caster list (every cooked instance's clusters, 64 a chunk, made at load) has
     room after it for the live meshes' chunks (`VirtualShadowMap::setLiveChunks`, at most
     blocks / 64 plus one a mesh). `flush` sends the whole live list whenever a mesh is written or
     destroyed, since its clusters may have moved. Before this, live meshes cast no shadows: the
     list was made while they were still empty. Test: `editor_level_shadows`.
   - The GI's rays see them too: a live mesh's BVH is built when it is written, and the top
     level reads the renderer's instance rows as they are now (the mirror of the GPU table). A row
     the stream shows, hides or gives another mesh relights the probes round it. Before this (in
     the voxel GI the probes replaced), the GI saw no level geometry: the sky lit closed level
     rooms and their walls reflected it. The same test checks `"live": {"bvhTriangles>": 0}`.
6. A pick of a live mesh maps the scene cluster and triangle back to the triangle as it was given
   (`sourceTriangle`).

**Colliders.** `b3CreateMesh` makes one per write, in the mesh's space. `ScenePak::setLiveCollider`
owns it and hands it out through `staticCollider`, where physics and audio already look.
- Each change bumps the mesh's revision.
- Physics rebuilds entity bodies whose live mesh changed (a `Collider` with the mesh shape). Audio
  drops its cached triangles and places the entity again.

**Picking.** The pick shader (`editor_pick.slang`) now reports the scene cluster and the triangle as
well as the instance (`GpuDrivenRenderer::pickResult`). The editor keeps the last answer
(`EditorBridge::lastPick`), and the live mesh's triangle when it hits one.

**From C#:**

```csharp
using Falcon.Level;

var mesh = LiveMesh.Create(new Vector3(0, 1, 0));     // an entity of its own, empty
mesh.Set(positions, normals, uvs, indices);            // materials per triangle optional
Collider c = default; c.shape = 2; Ecs.Set(mesh.Entity, c);  // its triangles collide
int t = mesh.TriangleOf(cluster, triangle);            // a pick's triangle in what Set got
mesh.Dispose();                                        // or destroy its entity
```

- `LiveMesh.Stats` gives the meshes, blocks and triangles in use, and the last write's time.
- The game's end (the editor's Stop) frees the game's live meshes.
- Level meshes (below) are live meshes the scene saves.

`editor_level_live` (Script Test project) checks it:
- A slab made while playing: a ball lands on it, and a ray hits its top.
- A panel facing the camera: a click at the view's centre picks its third triangle.
- Bad indices are refused.
- The images show both while playing and neither after Stop.

## Level meshes

A level mesh is an entity with an editable mesh (`engine/level/poly_mesh.*`, `level_meshes.*`).
It is saved with the scene and drawn as a live mesh.

**The mesh** (`PolyMesh`):
- Shared vertices, with faces as loops over them: n-gons, concave allowed, counter-clockwise
  seen from the front.
- Per face:
  - a material slot (the mesh's list of `.fmat` paths; `""` is the default);
  - a smoothing group (0 is flat; faces of one group share their normals at shared vertices);
  - UVs: *Local* (projected in the mesh's space: the texture moves with it), *World* (projected
    in world space: neighbouring meshes line up, and the texture stays put while the mesh moves)
    or *Manual* (the corners' own UVs). Each has an offset, a scale (metres per tile) and a
    rotation.
- Projected UVs use the axis the face mostly faces (box projection), so the sides of a box line
  up across their edges.

**Building it** (`level::build`). Each face is triangulated in its plane by Manifold's
triangulator. Corners become vertices of their own, with flat or smoothed normals and the face's
UVs. Tangents are MikkTSpace, as for live meshes.
- `LevelMeshes` turns every entity with a `LevelMesh` component into a live mesh and its
  `MeshInstance`. It builds again when the component holds another mesh (meshes are shared and
  never changed in place, so an undo step is a pointer), or when a mesh with world UVs moves.
- In a game, entities that are not enabled hide their rows. In the editor, the editor hides them
  (the eye, Delete), once the entity has its slot.

**Saved:** a created entity of kind `level` with `"levelMesh"`: base64 of the mesh's blob, inline
in the edits JSON. Play, Stop and scene switches carry the edits text whole, so there is no
sidecar file to keep in step. At load, the scene's level meshes reserve their live room
(`LevelMeshes::needs`) on top of `--live-meshes` / `--live-clusters`.

**In the editor:**
- **Create > Level Mesh > Box / Plane.**
- A click selects it, and the pick knows the face under the cursor (`EditorBridge::lastPick().face`).
- Undo and redo restore its mesh.

**From C#** (`Falcon.Level`): `PolyMesh` mirrors the engine's mesh and blob format (`ToBytes`,
`FromBytes`, `AddFace`, `FaceNormal`, `Box`).

```csharp
PolyMesh m = LevelMeshes.Get(entity)!;     // a copy
m.Positions[0] += Vector3.UnitY;
LevelMeshes.Set(entity, m);                // in the editor while editing: an undo step
Entity floor = LevelMeshes.Create(mesh, new Vector3(3, 0, 0));  // editor: like Create > Level Mesh
```

- Editor code's changes are undo steps (grouped by `Undo.Scope`).
- A game's changes last until Stop; level meshes it creates are deleted then.

**Blockout materials.** Faces without a material are drawn with a prototype grid, so a blockout
reads its sizes at a glance. The grid is [Kenney's Prototype Textures](https://www.kenney.nl) (CC0):
six colours (dark, light, green, orange, purple, red) of thirteen 1 m patterns. They are grids,
marks, quarters, diagonals, crosses, checkers, tiles, and stairs, door and window guides.
- **Where they come from:** the engine ships them in `content/prototype/<Colour>/<pattern>.png`
  (`bin/content` next to `falcon.exe`; Kenney numbers each colour's files differently, so here they
  are named by pattern). The licence is beside them.
- **Into each project, once:** the editor puts them into a project when it opens it, before the
  scene is cooked (`level/prototype_materials.*`, `installPrototype`).
  - Every texture goes to `Assets/Prototype/Textures/<Colour>/`.
  - Materials go to `Assets/Prototype/`: a Grid and a Checker in every colour, and Stairs, Door and
    Window guides in Dark and Light (18).
  - `project.falcon` remembers it (`"level": {"prototype": true}`), so deleting the folder keeps it
    gone. **Help/Install Prototype Materials** (`editor.level.prototype`) puts it back.
- **The default:** `"level": {"defaultMaterial"}` is what faces with no material draw with, in the
  editor and in the game (`LevelMeshes::setDefaultMaterial`).
  - It is Light Grid unless changed. A light base shows vertex colours, tints and painted layers; on
    Dark Grid a blue tint barely shows.
  - A missing file means the scene's first material, as before.
  - A face's own material (the palette, a drop, smart materials) wins.
- One texture is 1 m, the default UV scale: major lines every 0.5 m, minor every 12.5 cm. World or
  Local UVs keep it the right size on any mesh.
- `editor_level_prototype` checks the project got them and that the default is a real material
  (`prototypeDefault`), captures a box on a floor, and installs them again.

Level meshes go into prefabs like any entity (`"levelMesh"` inline). `editor_prefab_nesting` checks:
- a wall (a level mesh and a knob) nested twice in a room, and another room made from it;
- Apply adding a lamp, moving a nested knob (an override of the nesting) and deleting a nested
  wall, with the other room following;
- Unpack one level, and undo;
- a removal kept by one instance;
- saved and opened again;
- a prefab nesting itself stopped.

`editor_level_mesh` checks it, in a scene of its own:
- A box made from the Create menu, picked to a face.
- Editor code raises its top (undo, redo) and makes an L-shaped floor (one concave face: 4
  triangles).
- Saved and opened again.
- The game changes the box and makes another while playing; Stop puts it back.

`level_polymesh` (`falcon_level_test`) checks the kernel without the engine:
- a box's 12 outward triangles, and an L face's 4 triangles covering exactly its 3 m²;
- smoothed corners;
- world UVs lining up across two neighbouring boxes;
- the blob and base64 round trips, with bad data refused.

## Selection, snapping and elements

The editor's foundations for the level tools (`engine/editor/scene_elements.cpp`,
`EditorBridge::selection`, `elements`):
- **Selection:** a set with a primary entity (the last clicked: the gizmo, the Inspector and
  framing follow it).
  - `editor.select` takes `"mode": "add"` / `"toggle"` and `"ids"`, and the state sends
    `"selection"`.
  - A box selects editable, shown entities whose bounds' centre is inside it.
  - A drag moves the selection's roots by the primary's change, as one undo step
    (`commitWorlds`).
- **Snapping:** grid snapping is absolute by default (`snapAbsolute`).
  - V: the drag gathers its snap points when it starts (`EditorBridge::snapPoints`: level mesh
    vertices and edge middles, other meshes' bounds corners) and the pivot jumps to the nearest
    one within 16 px.
  - Ctrl+Shift casts against level mesh faces exactly (`surfaceHit`), else the terrain or the
    ground.
- **Elements:** vertices, edges (keyed by their two vertices) or faces of the primary level mesh.
  - Picked on the CPU from its `PolyMesh`: the nearest vertex within 12 px, edge within 8 px,
    or the face the mouse's ray meets first. A box takes vertices, edge middles or face centres
    inside it.
  - The overlay draws the mesh's edges, with the chosen elements in orange.
  - The gizmo sits at their centre. A drag moves their vertices through the gizmo's change: the
    mesh shows live, without an undo step per motion (`setLevelMeshLive`), and lands as one step
    (`commitLevelMesh`).

`editor_selection` checks it with real mouse input (`test.mouse` takes `"at": "@gizmo"` with
pixel offsets):
- add and toggle, Delete on two and one undo;
- a box;
- two boxes moved by the gizmo together, undone;
- a face picked and raised by the gizmo's Y axis, undone;
- a box of all six faces.

`editor_delete_many` selects two in the Hierarchy: a right click on one keeps both, its menu's
Delete 2 deletes both, the Delete key deletes two others, each undone.

## Commands, modal operations and the level controls

Level tools are editor code (C#): commands, modal operations and their keys. The editor page gives
them the Level window, a mode bar, a status strip, hover cards, menus and wheels at the mouse and a
command palette (`ui/editor/leveltools.js`). ProBuilder is the model: every command can be found by
looking, and none needs its key to be known.

**The Level window** floats over the scene view, beside the tools.
- **Opening it:** Window > Level Tools (Ctrl+Shift+L). It opens by itself the first time a level
  mesh is selected; closing it is remembered. (Tests run without saved prefs, so there it opens only
  when asked.)
- **Moving it:** drag its title bar; drag the grip at its foot for its height. Its place, height,
  folded sections and icon mode are kept in the page prefs (`levelWindow`).
- **Sections,** for the element mode in use, each with a colour down its side:
  - **Start here** (nothing selected): Box, Room, Wall, Sample level.
  - **Create** (the object mode): shapes and architecture open; cuts, volumes, splines and the kit
    folded (click a heading to open it).
  - **Edit:** the mode's mesh operations, shading, normals and topology.
  - **Select:** loop, ring, grow, shrink, by material, by normal.
  - **Surface:** the material palette, UVs, trims, paint, wear and damage.
  - **Object:** modifiers, arrange, and the selected wall's, cut's or spline's own commands.
  - **World:** volumes and navigation.
  - **Tools:** editor code's own commands with an icon.
  - **Place:** the prefab shelf.
- **Rows:** an icon, the name, the key and ⚙ when it has settings. A row that can't run now is
  greyed, and its hover card says why ("Choose some faces first"). Sections longer than eight rows
  show six and "N more".
- **⚙** opens the command's settings under its row: numbers typed, choices as buttons. They are kept
  while the editor runs and used the next time it runs.
- **Search** (the field at the top) filters every command of the mode, best match first; Enter runs
  the first one that can run, Esc clears it.
- **Icon mode** (the button beside the close button) shrinks it to a narrow rail of icons in the same
  sections; the hover cards name them.
- **The footer** shows what matters now:
  - while a tool draws (a shape, a wall, a door): its settings;
  - with the paint tool: the paint panel (sliders with their values, a row of colours);
  - after an operation: **Last: Bevel** with its amount and settings. A change takes it back and does
    it again (Blender's Adjust Last Operation), still one undo step. It lasts while that step is the
    newest one (an undo, a redo or another change ends it).
- The UV editor and trim sheets open beside it, level with its top.

**The mode bar** (the top of the view, while a level mesh is selected): Object, Vertex, Edge and
Face (keys 1–4, or the Tab wheel), and what is chosen ("Crate", "2 faces", "Click faces to choose
them").

**The status strip** (the foot of the view, while a level mesh is selected): the mode, how many
elements are chosen, and the keys that matter now (in a face mode: E extrude, I inset, X delete,
Shift+1–5 menus). While a modal operation runs, its name, amount and its own keys instead: a wall says
"click: a corner · Shift: off the grid · right-click / Enter: done · Esc: cancel", Bevel adds its wheel
and C.

**Hover cards:** every button over the scene view, on the scene toolbar and in the Inspector shows
its name, key and help after a moment (and why it can't run, when it can't). Moving along a bar
switches cards straight away. The editor's off-screen browser draws no `title` tooltips, so the page
draws these itself, reading plain titles such as "Move tool (W)" the same way.

**Menus and wheels at the mouse:**
- **Menus** (a click picks; the key can be let go): Shift+A create (the shapes, then a submenu a
  section: Architecture, Cuts, Volumes, Splines, Kit; Empty, Point Light), Shift+G arrange, Shift+M
  modifiers, Shift+P paint, Shift+U UVs, Shift+V view.
- **Wheels** (hold the key, flick toward an item, let go; a click picks too): Tab element modes,
  Shift+S snapping, Alt+Z draw modes, Alt+Q the mode's operations (its last item, More…, opens the
  menu of all of them).
- The page asks the engine where the mouse is (`editor.view.mouse`) and opens them there.
- GameObject > Level (and the Hierarchy's create menu) has Level Box, Level Plane and every create
  command. Help > Level Starter Content has the sample level, prototype materials, the starter kit,
  starter trims and starter smart materials. Help > Keyboard Shortcuts lists every level key.

**Chord menus** (Counter-Strike's buy menu): every command from the left hand, without aiming.
- **Shift+1–4** switch to the Object, Vertex, Edge or Face mode and open its ring at the mouse;
  **Shift+5** opens World & Tools (volumes, navigation, view, editor code's own commands).
- Still holding Shift, **1–6** pick a group, then a command, which runs at once: Shift+3, 1, 1 is the
  edge mode's Extrude; Shift+4, 1, 2 the face mode's Inset. Geometry is always 1, Select 2.
- **Backspace** goes back up; **Esc**, or letting go of Shift, closes it and runs nothing. A click
  picks a slot too.
- The slots are fixed, 1 at the top and then clockwise, so a command's keys and place never move.
  One that can't run now stays in its place, greyed; pressing it says why ("Choose some edges
  first"). Each slot shows the command's own key (E), to learn it by.
- A group longer than six shows five and More….
- A modal operation picked from a chord starts with Shift held: Shift doesn't slow it until it has been
  let go once (`ModalOperation.ShiftHeld`, which click tools use for "off the grid" too).
- Shift+2–4 with no level mesh selected: the status strip says to select one.
- **The layout:** `LevelCommands.Chords` (written into the tools description as `chords`: rings "1"
  to "5", each `{label, icon, slots}`, a slot a command's path, a group of its own, or null for
  empty). **Edit > Chord Menus…** (also from the Keymap and the palette) changes a ring's slots: a
  command, a new group, or cleared. The changed rings are kept as the user's own (prefs `"chords"`),
  and Defaults gives a ring the editor's again.
- Digits are read by their key code, so the rings (and the prefab shelf's Ctrl+Shift+1..9) work on any
  keyboard layout.

**Command palette:** F3 or Ctrl+Space (also Edit > Command Palette…). It lists every command, with
its icon and section, and the menu items, matched letter by letter in order. Arrows pick one, Enter
runs it.

**Commands** are `[EditorMenu]` static methods with more to say:

```csharp
[EditorMenu("Tools/Level/Push Faces", Key = "J", Modes = "face", Icon = "face", Wheel = "ops",
            Help = "Push the chosen faces along their normals")]
public static void Push() { ... ModalOperation.Begin(new PushFaces(...)); }
```

- **Key:** a key in the scene view (`"E"`, `"Shift+D"`, `"Alt+N"`; Ctrl combinations stay the page's
  `Shortcut`), in its element `Modes` (`"vertex|edge|face"`; none: any).
  - A command's key wins over the view's own keys, so E extrudes in a face mode and still rotates
    objects.
  - Shift+R runs the last command again.
  - Edit > Keymap… rebinds them (prefs `"keymap"`: {path: key}, conflicts in overlapping modes
    marked). It lists every command, keyed or not, under the Level window's sections, with a
    filter.
- **Icon:** the command is in the Level window's Tools section while its mode is on.
- **Wheel:** `"ops"` puts it on Alt+Q (and the status strip's keys).
- **Help:** for the hover cards and the palette.
- The generator registers them (`EditorRegistry.Command`), and `Describe` hands them to the editor.
  - The editor hands them to the page again when the page is ready: scripts already built load
    before it.
  - `editor_level_bar_start` checks the window's rows in that case.

**The engine's own commands** (`Falcon.Level.LevelCommands`, built-ins) say more, for the Level
window:
- **Section** (`Group`): create, edit, select, surface, object or world. View and starter content
  are in menus and the palette.
- **Heading** inside it (`Section`: Shapes, Architecture, Cuts, Geometry, UVs, Wear, ...) and a
  short **label** there ("Box" under Cuts).
- **Needs:** what must be selected for it to run: a level mesh, chosen elements (a level mesh in
  the object mode; commands that also work on the whole mesh run without), a selection, a wall, a cut
  or a spline.
- **Settings** (`CommandOption`: a number with a range and unit, a whole number, a choice or a
  switch), kept while the editor runs. The command reads them when it runs:
  - Bevel: segments and curve (its mouse wheel and C change them too);
  - Weld: distance;
  - Auto Smooth and Select By Normal: angle;
  - the shapes: sides, rings, segments or step height;
  - Wall and Room: height and thickness;
  - Door and Window: width, height and sill.
- The page sets one with `editor.tools.option {menu, name, value}` (clamped; whole numbers
  rounded); the menu `"Last"` sets the last operation's amount.
- Editor code tells the page their values whenever they change (level API v9 `toolState`:
  `{"options": {path: {name: value}}, "active": path, "last": {name, menu, amount, amountName,
  unit}}`, the page's `editor.tools.state`; again when the page is ready).
- **The last operation** (`LastOperation`): Extrude, Inset, Bevel, Weld, Auto Smooth, the shapes,
  walls and rooms record how to do themselves again. A change undoes and redoes; the editor's
  `"undo"` query (the newest step as a token) tells it whether its step is still the newest.
- Each command has its own icon.

**Editor code's view of the scene view** (`Falcon.Level`, the level API):
- `SceneViewport.Input`: once an editor frame, the same for every tool. It has the mouse, the
  camera's ray through it, buttons held, pressed and released, modifiers, the wheel, and the keys
  pressed since the last frame. `WorldToView` projects a point.
- `SceneViewport.Capture(on)` makes clicks, drags and keys the code's alone while the camera still
  flies; Play and a reload let go. `SceneViewport.Hint(text)` fills the status strip ("name: amount ·
  key · key").
- `Elements.Get` / `Set` / `Vertices`: the element selection (vertex indices, edge keys, face
  indices).
- `LevelMeshes.SetLive(entity, mesh)` shows a mesh without an undo step, and
  `LevelMeshes.Commit(entity, before)` lands it as one step.

**`ModalOperation`** (Blender's):
- `Begin` captures the view. Moving the mouse sets the amount (`Sensitivity` per pixel; Shift a
  tenth; Ctrl snaps to `Snap`).
- Typed numbers set it exactly (Backspace, `-`, `.`).
- X / Y / Z lock an axis (`Direction(natural)`), and Shift+X..Z keep the other two.
- A click or Enter confirms (`OnConfirm`), right-click or Esc cancels (`OnCancel`). `Apply`
  shows the amount live.
- A right-click is a press and release within 400 ms and 6 px: a right drag still flies the camera.
  The building tools (walls, rooms, shapes once a corner is down) finish on it instead
  (`RightClickFinishes`).
- The status strip says all of this as it goes. A tool that only takes clicks says so:
  `ShowsAmount => false` and its own `Keys`.

The tests:
- `editor_level_commands`: J in a face mode starting a push; "1" Enter pushing exactly 1 m, then
  undo; Esc cancelling; Shift+R; the keymap moving it to K; its row in the Level window and its hover
  card (name, J, help); the palette ("pushf", Enter).
- `editor_level_mode_bar`: the mode bar hidden with nothing selected, Object and the box's name, its
  Face button, a face chosen ("1 face"), keys 2 and 1.
- `editor_level_window`: closed at first; Ctrl+Shift+L opens it beside the tools with the object
  mode's sections; a cylinder's ⚙ set to 12 sides; the face mode greys Extrude ("Choose some faces
  first" on its card); search finds Extrude first; icon mode narrows it; dragged and kept; closed
  and opened again where it was.
- `editor_level_chords` (held keys: `test.key` with `"action": "down"` / `"up"`; a held modifier is
  SDL's modifier state too):
  - Shift+3 opens the edge ring (the edge mode on); its Geometry is all greyed with nothing chosen, and
    pressing Extrude says why;
  - Backspace and Esc;
  - a face chosen, Shift+4, 1, 1 extrudes, and the mouse moving with Shift still held gives the full
    amount; one undo step;
  - Shift+4 let go runs nothing; the user's own ring from prefs; Shift+2 with nothing selected.
  - `editor_level_options` checks the layout: every path a command, none twice on a ring.
- `editor_level_menus`: Shift+A's menu at the mouse, Cuts > Box making a cut; GameObject > Level;
  Help > Level Starter Content.
- `editor_level_options`: every command found once, with a section and an icon the editor has; the
  settings read back, set and clamped; a bevel starting with 4 segments, its wheel's 5 heard back; a
  6-sided cylinder (20 triangles), then as the last operation 8 sides (28) and 2 m high, an undo
  ending it.

## Shapes and mesh operations

The level tools come with ProBuilder's operations as built-in commands (`Falcon.Level.LevelCommands`
in Falcon.Engine). They are in the Level window, the menus and wheels at the mouse and the palette,
and the keymap rebinds them. They are not in the Tools menu (`"builtin": true` in `Describe`). A project's
own commands are listed first, so their keys win. Each change is one undo step.

| Key | Command | Modes | What it does |
| --- | --- | --- | --- |
| E | Mesh/Extrude | face, edge | Faces out along their normals with walls around them; open edges: a strip outward (modal) |
| I | Mesh/Inset | face | Each face shrunk inward, a ring of faces around it (modal) |
| B | Mesh/Bevel | edge | Each edge cut into a chamfer face (modal: the mouse or a typed number sets the width, the mouse wheel the segments, C round / flat / curved in) |
| Alt+R | Mesh/Loop Cut | edge | A new edge loop across the ring of the chosen edge |
| J | Mesh/Connect | vertex, edge | Faces split between chosen vertices, or between the middles of chosen edges |
| M | Mesh/Merge | vertex | The vertices into one at their centre |
| Alt+F | Mesh/Fill | vertex, edge | Two edges bridged, an open edge's hole filled, or a face over the vertices |
| X | Mesh/Delete | vertex, edge, face | The elements and the faces using them |
| P | Mesh/Detach | face | The faces taken out as a level mesh of their own |
| Alt+N | Mesh/Flip Normals | object, face | Faces turned inside out (the object mode: the whole mesh, a room) |
| L / Shift+L | Select/Loop, Select/Ring | edge | The edge loop or ring through the chosen edges |
| = / - | Select/Grow, Select/Shrink | any element | Add the neighbours, or drop the border |

Without keys: Subdivide, Weld (1 cm), Conform Normals, Triangulate, Quadrangulate, Merge Faces, Auto
Smooth (40°), Smooth / Flat Faces, Select/By Material and Select/By Normal.

- **Modal operations** follow the mouse, or a typed number. They run again on the starting mesh
  each time the amount changes, shown live, and land as one step when confirmed.
- **Selection after an operation:** what it made (the extruded faces, the new loop) when that is in
  the mode in use. Otherwise nothing is chosen, because the indices moved.
- **Unity keys still work:** F still frames the selection, and W / E / R move, rotate and scale in
  the object mode. In element modes, E, B and X belong to the operations.
- **Shapes** (Shift+A or the palette, `Create/...`): box, cylinder, cone, sphere, torus, pipe, prism,
  arch, stairs, curved stairs, plane.
  - A small block follows the mouse on the ground, where the first click would put the shape.
  - Click a corner on the ground, then the opposite corner, then move up for the height (or type it)
    and click. Sizes snap to 0.25 m; Shift turns that off.
  - Round shapes and the plane finish at the second corner. A right-click (or Enter) at the second
    corner finishes it as tall as it is wide.
  - The shape is one undo step, made with its bottom on the ground.

**Kernel** (`engine/level/mesh_ops.*`, `mesh_shapes.cpp`): `runOp(name, mesh, args JSON)` works on a
PolyMesh and returns the new mesh, what to select next, and (for detach) the faces it took out.
- Shapes are made closed and facing out: each piece's volume is checked and the piece is flipped
  if it is negative.
- Ear clipping triangulates concave faces.
- Weld uses a spatial hash.
- Bevelling an edge that meets other bevelled edges at a corner takes one bevel at a time (bevel
  again).
- A bevel's size and shape:
  - Its width is the mouse (or a typed number, "0.05 Enter").
  - The mouse wheel sets its segments, the faces across the edge. 1 is a flat chamfer; more make it
    round.
  - C cycles its curve:
    - round: a quarter circle touching both faces, its faces shaded smooth;
    - flat: the chamfer cut into strips;
    - curved in: the arc mirrored, a cove.
  - The hints bar shows both. The next bevel starts from them.
  - From code: `"segments"` (1 to 64) and `"curve"` (1 round, 0 flat, -1 curved in) next to
    `"amount"`.
- The C# side calls the kernel through the level API (v2):
  - `meshOp(op, blob, argsJson, buffer)` writes `[u32 n][mesh][u32 m][detached][JSON]`;
  - `LevelOps.Run(op, mesh, mode, items, args)`, `LevelOps.Shape(name, args)` and `LevelOps.Args`
    wrap it.

```csharp
MeshOpResult r = LevelOps.Run("bevel", mesh, ElementMode.Edge, edges, w => w.WriteNumber("amount", 0.05));
if (r.Ok) LevelMeshes.Set(entity, r.Mesh!);
```

The tests:
- `level_meshops` (falcon_level_test) checks every shape, and that every operation keeps the mesh
  closed and wound alike with the expected face count and volume. That covers extrude doubling a
  box, the bevel's exact volume, a round bevel on a true quarter circle (and a cove), a bevel inside a grid, ring and loop, fill closing a hole, flip
  and conform, triangulate and back, concave faces, weld, grow and by normal, auto smooth, and the
  same result twice.
- `editor_level_ops` covers in the editor:
  - E, a typed 1 and Enter;
  - I, .2;
  - X;
  - undo;
  - B on an edge, the wheel to 4 segments (round), .1 typed (its triangles counted), undone;
  - Shift+L and Alt+R on edges;
  - Alt+N;
  - a cylinder drawn from the palette.

## Materials and UVs on faces

Every face of a level mesh has its own material slot and its own UVs.

**Materials:**
- **Drop a material** from the Project panel onto a level mesh in the view. It goes on the face
  under the mouse. If that face is one of the chosen faces, it goes on all of them.
- **The material palette** is in the Level window's Surface section (object or face mode).
  - Drop materials onto it. Click one, or press Alt+1..9, to put it on the chosen faces (in the
    object mode: the whole mesh).
  - Right-click a material to remove it. The palette is kept in the prefs (`materialPalette`).
- **The eyedropper:** Alt+C (Material/Pick Under Mouse) takes the material of the face under the
  mouse and puts it first in the palette. Alt+V (Material/Apply Picked) puts it on the chosen faces.
- Each new path becomes a slot of the mesh (`materials`). Unused slots stay.

**UVs.** Each face projects its texture, or carries its own UVs:

| Mode | UVs |
| --- | --- |
| Local | Projected in the mesh's own space: the texture moves with the mesh. |
| World (texture lock) | Projected in world space: the texture stays put as the mesh moves, and neighbouring meshes line up. |
| Manual | The corners' own UVs. |

All three then apply the face's offset, scale and rotation.

The UV commands work on the chosen faces, or on the whole mesh in the object mode. They are on
the Shift+U menu, the Level window's Surface section and the palette:

| Command | What it does |
| --- | --- |
| UV/Unwrap (U) | Unfolds the faces into flat charts (xatlas) as Manual UVs, at a metre a tile, packed apart. |
| UV/Fit | Shows the texture once across each face. |
| UV/Align to Edge | Turns the texture so u runs along the face's longest edge, or the chosen edge. |
| UV/Match Neighbours | Carries the first chosen face's texture on across the others' shared edges, seamlessly (a strip of wall, a room). |
| UV/Texture Lock, UV/Local Projection, UV/Reset | Switch the mode, or go back to Local at 1 m tiles. |
| UV/Rotate 90, UV/Flip U, UV/Flip V | Turn or mirror the texture. |

Switching to Manual keeps what the face shows: its UVs become its corners' own.

**The UV editor** (Alt+U, UV/Editor) is a panel over the view.
- It shows the chosen faces' UVs over the tile grid, and follows every change (undo included).
- Drag inside it to move the UVs. The wheel scales them; Shift+wheel turns them 15°.
- Its buttons run the UV commands. Its fields set the first face's offset, scale and rotation on
  every chosen face.

**Under the hood:**
- The kernel's UV operations are in `level/mesh_ops`: `uv`, `uvNudge`, `uvReset`, `uvFit`,
  `uvAlign`, `uvMatch` and `uvUnwrap`. They take the entity's `world` rows, used by World faces.
- `level::faceUvs` gives a face's UVs as drawn; the build uses the same function.
- The page talks to the editor through:
  - `editor.level.op` {op, args}: one undo step on the chosen elements;
  - `editor.level.material` {material, at};
  - `editor.level.pickMaterial`;
  - `editor.level.uvs` {open}.
- C# code adds the transform with `LevelOps.WriteWorld(w, entity)`.

The tests:
- `level_uvops` checks fit (0..1), nudge, align to an edge, match around a box's sides (three
  seams continuous; the fourth closes the loop), unwrap (the UV area equals the faces' area), the
  switch to Manual looking the same, reset, and fit through a World transform.
- `editor_level_uvs` covers:
  - a material dropped on the top face, and undo;
  - the eyedropper with Alt+V, and Alt+1;
  - U;
  - the UV editor: a drag moving a face's UVs, and its Fit button.

## Trim sheets

A trim sheet is one texture cut into horizontal strips: a metal edge, a worn bevel, a row of bolts,
a panel seam. Faces put on a strip show it repeating along their length. Walls, frames and bevels
get detailed edges from one texture and little geometry.

A `.ftrim` file names the material and the strips (rows of the texture, 0 at the top):

```json
{"material": "Assets/Trims/Starter Trims.fmat", "aspect": 1,
 "strips": [{"name": "Metal edge", "v0": 0, "v1": 0.125}, ...]}
```

`aspect` is the texture's width over its height. It keeps texels square along u.

**Putting faces on a strip** (the `trim` operation) works on each chosen face:
- Its height across its longest edge fills the strip's rows.
- u runs along that edge at square texels.
- The face gets the sheet's material and Manual UVs.
- Faces in a run carry u on across their shared edges, turned or mirrored to meet. A run is a
  bevel's strip, or a band round a pillar or a room (the last seam of a ring closes it).

**In the editor:**
- **UV/Trim Sheets (Alt+T)** shows a panel with a sheet's texture and its strips over it. Click a
  strip to put the chosen faces on it.
  - **Edit** renames the strips, moves their rows, and adds or removes strips. **Save** writes
    the `.ftrim`.
  - **New** starts a sheet on the eyedropper's material.
- **UV/Trim Again (T)** puts the chosen faces on the last strip used.
- **UV/Make Starter Trims** writes a starter set to `Assets/Trims`: a generated 1024² texture
  with eight 128-pixel strips (metal edge, worn bevel, bolts, panel seam, chipped concrete,
  planks, hazard stripes, grime), its material and its `.ftrim`.
  - Like any newly made material, its texture shows once the scene is cooked again.

**Bridge messages:**
- `editor.trim.list`, `editor.trim.save` {path, sheet}, `editor.trim.starter`;
- `editor.level.trim` {path, strip}, with none: the last one used.

**Code:**
- `level/trim_sheet.*`: `TrimSheet`, `readTrimSheet` / `writeTrimSheet` / `parseTrimSheet`.
- `level/starter_trims.cpp`: the starter set, drawn with value noise and saved with
  `SDL_SavePNG`.
- C# reaches the operation through `LevelOps.Run("trim", mesh, ElementMode.Face, faces, w => { v0, v1, aspect, material })`.

The tests:
- `level_trims` checks that faces fill the strip's rows at square texels, a run of three quads
  carries u from 0 to 1.5, a band round a pillar has three continuous seams, an upside-down strip
  is refused, and the `.ftrim` text round trip.
- `editor_level_trims` covers:
  - the starter set;
  - Alt+T and a click on the first strip;
  - undo;
  - T;
  - a saved sheet used by message.

## Layered surfaces and vertex colours

A level mesh can carry up to three materials over its faces' own: moss on stone, rust on paint,
dirt on a floor.
- The layers are blended by height where a painted mask says, the way the terrain blends its
  layers.
- The vertex colours tint (rgb) and darken (alpha: occlusion) the result.
- One face gets several looks without more geometry.

**Data:**
- In the mesh (`level::PolyMesh`, blob format 2; format 1 blobs still load):
  - `layerMaterials[3]` and `layerBlend`, the blend's width;
  - `cornerUv1`: the mask's UVs, unique per face in 0..1;
  - `vertexColors`: RGBA8.
- **The mask** is a separate component, `LevelMask`: a 512² RGBA8 image. r, g and b are the weights
  of layers 1..3, and a is the wear (filled later by the wear tools).
  - It stays outside the mesh so mesh operations don't copy a megabyte each time.
  - It is saved in the scene's edits as `"levelMask"` (base64 of a PNG) and has its own undo.

**Operations** (`level/mesh_ops`):
- `layers` {materials, blend} sets the layers. A mesh with no mask UVs gets them (`maskUnwrap`).
- `maskUnwrap` packs xatlas charts at 512 texels into the lower 80% of 0..1 each way, leaving room
  for the charts of faces made later.
- `vertexColor` {color} colours the chosen vertices or faces' vertices, or all of them. All
  white is the same as none.
- Mesh operations keep all three through edits:
  - an extruded face keeps its mask UVs;
  - flip keeps each corner's UVs;
  - new vertices are white;
  - new faces (a bevel's, an extrude's walls, a modifier's) get mask UVs of their own
    (`maskFill`). Their charts go into the free space at about the density of the rest, and the
    rest keep theirs, so paint stays where it was. When they don't fit, they shrink, down to a
    sixteenth of that density. If they still don't fit, the whole mesh is laid out afresh, and
    paint done before then no longer lines up.
  - Running `maskUnwrap` lays the charts out afresh.

**Drawing:**
- Live meshes have an 8-byte extra stream per vertex (mask UV as unorm16x2, colour as RGBA8). It
  sits after the live vertices in the scene buffer's vertex section (`LiveGeometry::extraOffset`),
  and cooked vertices don't change.
- `render/surface_layers.*` keeps the masks as layers of one texture array, grown as meshes take
  them (64 at most). It also keeps a record per mesh: three material indices, the blend width and
  the mask's layer.
- A layered mesh's instance carries `kInstanceLayered` and its record in `InstanceGpu::skin`.
- The resolve (`blendLayers` in `visbuffer_resolve.slang`):
  - samples each layer material through the virtual textures, with feedback;
  - weights the base by what the mask leaves;
  - blends by height + weight within the blend width;
  - folds the material factors into the surface.
- Its bindings moved up: the mask array is sampler 13, the records are storage texture 18. The
  project's resolve is now built as `resolve2.fshader`, so an older build's blob is not read.

**In the editor:**
- **The surface materials** are a list, in the Inspector's Surface section (every level mesh) and in
  the paint panel. Each row is one material (up to three: the mask's channels) and a dropdown for
  what it is used for:
  - **Paint**: only where you paint it on with the brush;
  - **Edge wear**: along the sharp outside edges (chipped, worn);
  - **Stain**: in the inside corners, at the foot and where other level meshes meet it (dirt, moss);
  - **Streaks**: down the sides below ledges and sills (rain streaks).

  The last three are live wear (below), kept up as you build, and the brush can still paint any of
  them on by hand. For example: 1 Concrete (Paint), 2 Moss (Stain), 3 Broken Concrete (Edge wear).
  **Add material** at the bottom picks one (the project's or a library's), a row's material can be
  changed by clicking it or dropping one on it, and × removes it with its wear and what was painted
  of it. A mesh with no surface materials has nothing to paint, so start there.
- `editor.level.surface` {layer: 0..2, material ("" removes it), use: paint | edges | grime |
  streaks} changes one row, one undo step. `editor.level.op` with `layers` / `vertexColor` sets them
  all at once.
- `editor.level.mask` {op: fill, channel, value} or {op: clear} changes the mask, one undo step.
- **The paint brush** (Layer Brush in the Level window's Surface section, the Inspector Surface's
  Paint button, Paint/Layer Brush, Shift+B or the Shift+P menu; its settings in the window's footer):
  - Drag on the selected level mesh to paint one of its surface materials into its mask, erase
    them, or paint its vertex colours.
  - The paint panel (right of the view) picks the mode (Materials, Erase, Colour, Decals, Damage,
    Scatter), radius (or `[` `]`), strength and colour. The colour's alpha is occlusion. In
    Materials it lists the mesh's materials: click a row's number to paint that one (the brush's
    row is outlined), and Fill or Clear painting changes the whole mask.
  - Click another level mesh in the view to paint it instead (the brush stays out). A click on
    nothing keeps the one you are painting.
  - A stroke shows as it goes and is one undo step when the mouse comes up.
  - The mask brush is a ball round the point under the mouse, in the world: every texel whose
    point on the mesh lies inside it is painted, on whatever faces it reaches, so it wraps edges
    and corners. Faces turned away from the one under the mouse are left alone (a thin wall's far
    side). Dabs fall a quarter of the radius apart, with the ones between found along the camera's
    ray.
  - Each texel takes the most any one dab of the stroke gave it, so the strength is how far one
    stroke goes (a half-strength stroke stays half however slowly you drag) and the edge doesn't
    ridge with every dab. Another stroke goes on top.
  - Colours go on the vertices inside the brush, so a low-poly mesh needs a wide brush or more
    vertices.
  - The brush is drawn over the mesh as that ball (a ring round the point, wherever the surface
    faces). Esc puts the tool away.
  - The paint commands have their own icons in the Level window (roller, eraser, tint, decal,
    scatter, crack, wear, grime, streaks).
- The code is in `level/mask_brush.cpp` (`paintMaskDab`, `MaskStroke`) and `editor/level_paint.cpp`
  (`paintBegin`, `paintTo`, `paintEnd`, `paintHover`), with `SceneTool::Paint` in the scene view.

The tests:
- `level_layers` checks:
  - the layers and their mask UVs (all inside 0..1, charts apart);
  - format 2 round trips, and format 1 still loads;
  - colours and mask UVs kept through extrude and flip;
  - all white is none;
  - a dab on a box's top edge painting both faces round it and not the far ones, erasing, a thin
    wall's back left alone;
  - a 41-dab stroke matching the brightest single dab at every texel, at full and half strength.
- `editor_level_layers` gives a box a red surface material (the Surface section's Add material) and
  fills its mask (`editor_level_layers_drawn`: the view turns red), undoes that, then tints it blue
  (`editor_level_layers_tinted`).
- `editor_level_paint` covers:
  - the Paint button, then one stroke painting part of the mask (its mean above zero, under half);
  - one undo step taking the mask away;
  - a stroke of blue vertex colours;
  - Esc.
- `editor_level_paint_panel` covers the paint panel's list: Add material (the brush on it), a second
  one made a stain and back (one undo step each), a row's number picking the brush's, × removing the
  last (no layers left), a click on another mesh selecting it with the brush still out, a click on
  nothing keeping it, and the panel's close button.

## Live cuts

A cut volume takes a shape out of the level meshes it overlaps, or keeps only what is inside it.
Doors, windows, holes and broken corners need no geometry editing.

**Non-destructive:**
- The meshes themselves don't change. What is drawn and collided with is a cut copy, and the
  same live-mesh path that updates shadows and GI updates it too.
- The copy is made again whenever the mesh or a cut over it changes: moved, resized, set
  differently, deleted or undone.

**A cut volume** is the `CutVolume` component (`level/level_meshes.h`), on an Empty entity in the
editor:
- **Shape:** a unit box, a 1 m-wide cylinder standing on y, or a 1 m sphere. Its transform sizes
  and places it. A Mesh cut uses its entity's own level mesh as the shape (from code for now).
- **What it does:** subtract (the default), or intersect.
- **Its outline and icon** show only with it, the mesh it is under (a wall) or something under it
  (a door's leaf) selected. Select the wall to find its doorways, then click one's icon.
- **Its cap material:** the faces it makes (a door's sides and head). With none, they take the
  target's first material.

**Which meshes it cuts:**
- Those whose bounds overlap its bounds.
- The target's faces keep their material, UV settings, smoothing, mask UVs and colours: its
  triangles carry their face's index through the boolean (Manifold's faceID, under an original
  ID given to the target rather than `AsOriginal()`, which drops it).
- Picks on a cut mesh still find its own faces, so element modes work on it. The caps have no
  face.
- A mesh that isn't closed (a plane, a one-sided wall, much imported art) is clipped instead (see
  below): no caps, and a badge says so.

**In the editor:**
- **Create/Cut Box, Cylinder, Sphere** (Shift+A) puts one at the view's centre, drawn as a
  wireframe (orange; the accent colour when selected).
- **Cut/Subtract**, **Cut/Intersect** and **Cut/Cap With Picked Material** (the eyedropper's,
  Alt+C) change the selected one.
- Messages:
  - `editor.level.cut.create` {shape, name};
  - `editor.level.cut` {shape, intersect, cap, capPicked, remove}, one undo step.
- It is saved in the edits as `"cut": {"shape", "intersect", "cap"}`.

**Kernel** (`level/mesh_cut.*`): `cutMesh(target, world, cuts, result)`.
- Each face corner becomes a Manifold vertex. Its manual UV, mask UV and colour are interpolated
  properties, and the corners are merged by the mesh's vertices.
- Each triangle keeps its face as `faceID`. The cuts are subtracted or intersected in world
  space, in order.
- The result is a face per triangle, welded back by position. `sourceFace` maps each face to the
  target's face (caps: none).
- `cutBounds` gives a cut's world bounds.

**Imported art** (cooked or created mesh instances, prefab instances' meshes too) is cut the same
way:
- The mesh is read back from the pak (`level/pak_mesh.*`: its full detail, the cluster DAG's
  level 0) once per mesh. Positions are welded, and its UVs become Manual UVs. Its materials stay
  the scene's own (`scene:N` slots), and it is smoothed at under 40°.
- The read-back mesh is cut where the instance is and drawn as a live-mesh copy.
- The instance itself is hidden through the renderer
  (`GpuDrivenRenderer::setInstanceHidden`). Its own hidden state comes back when the copy goes,
  so a deleted one stays gone.
- A click on the copy selects the instance (`LevelMeshes::originalOf`), and the copy is
  outlined.
- The instance's own collider is still the uncut one. The copy collides too.

**Cuts run on worker threads** (`LevelMeshes::CutJob`, on the engine's job scheduler), so a heavy
cut doesn't hold up the frame:
- A changed mesh or cut starts a job. Until it is done, the mesh is drawn, collides and is hashed
  as it was (its last cut).
- Each mesh (and art copy) has one job at a time. When the job is done its result is drawn, even if
  the mesh or cut moved on meanwhile, and the next job starts from where things are then. Dragging
  a cut through a heavy mesh redraws as fast as the cuts come, without waiting for the drag to
  stop.
- **First cuts:** a mesh's first cut (a scene loading, a mesh or copy just made) is waited for in
  the update that starts it, with all the others started in that update running at once. A scene
  never shows a wall uncut before its door.
- **Built there too:** the job also triangulates its result and runs `LiveGeometry::prepare`
  (tangents, clusters, quantizing, the BVH, the collider), so the main thread only finds the mesh
  room and uploads it (`LiveGeometry::commit`). Built on the main thread, a damage brush dab on a
  big wall cost the frame 20 to 50 ms (MikkTSpace alone 12 to 21): the stutter while painting
  damage. Now no frame of three strokes over the sample room's walls reaches 25 ms. The materials it can name (the target's, the cuts' caps) are
  looked up when it starts; a result naming another is built on the main thread as before.
- **Uncut:** a mesh whose cuts all went away is built whole at once, and a job still running for
  it is let go.
- **Waiting for results:** `LevelMeshes::cutsPending()` counts the jobs still running.
  - Games: `Cuts.Pending` (level API v8).
  - Tests: `test.wait {"cuts": true}`, and the expectations `cutsPending` and `lastCutMs>`
    (`LevelMeshStats::lastCutMs`).
- **Measured:** a damage hole 50 m across through a welded 100 by 100 box slab (40,800 triangles)
  takes about 100 ms on a worker. The frame goes on meanwhile; the same cut used to stall it for
  that long.

**Baked cuts** (`level/cut_bake.*`): the scene's cuts are saved with it, so loading it doesn't cut
again.
- **When it is written:** when the editor saves the scene. Each level mesh and art copy drawn cut
  goes into `<scene>.cuts` beside its `.fpak`. Build Game ships it with the scene, and it is moved,
  renamed and deleted with the scene in the Assets panel. A scene with no cuts has no file.
- **The key:** each result is kept under a key of what it was cut from: the target's content (its
  mesh blob, after modifiers, or the art read back from the pak), where it is, and each cut's
  shape, settings, place, cap and mesh, in order.
- **At load** (a game, or the editor opening the scene), a cut whose key is in the bake takes the
  saved mesh and its faces as they are. Anything changed since gives another key, and is cut as
  usual. This covers a mesh or cut edited outside the editor, art cooked again, or a game's own
  cuts.
- **The file:** `FCUT`, version 2, then per result its key, the cut mesh's blob, each face's source
  face and its flags (clipped). Two cuts come to about 30 KB.
- **Measured** by `LevelMeshStats::bakedCuts` (of `cuts`). The editor's expectations are
  `cutBake` (results read), `bakedCuts` and `freshCuts` (cut since the scene opened).

**Open meshes are clipped** (`level/mesh_cut.cpp`, `clipMesh`). Manifold needs a solid, so when the
target isn't closed its triangles are split against the cutter instead:
- **Subtract** drops what is inside the cutter; **intersect** drops what is outside.
- There are **no caps**: there is no inside to close, so you see through the hole.
- Faces keep their settings. The pieces' UVs, mask UVs and colours are interpolated along the edges
  they were split on.
- **The cutter in convex pieces:**
  - a convex cutter (a box, cylinder or sphere) is one piece, made of its face planes;
  - otherwise it is a tetrahedron per face, from a centre it is star-shaped about (its origin, or
    its vertices' middle). Damage blobs are star-shaped.
  - A mesh cutter that is neither (a torus) can't clip: the target is left whole and the log says
    why.
- **Welding:** where an edge crosses a plane is worked out the same way from either side, so
  neighbouring triangles share their new points. Points another piece put on an edge are added to
  that edge (no T-junctions), so the result has no cracks.
- **Badges:** a clipped mesh, or art drawn as a clipped copy, gets a warning badge in the Hierarchy
  and a note at the top of the Inspector. Closing the mesh (Mesh/Fill on its open edges) gives
  capped cuts.
  - `LevelMeshes::clipped` / `copyClipped`; the list is sent again when they change
    (`clippedVersion`).
  - Expectations: the entity's `clipped`, and the probe's `clipBadges` and `clippedNote`.

The tests:
- `level_cuts` checks:
  - a door's exact volume, closed and facing out, with cap material on its sides;
  - a wall with a material per side cut: every face left keeps its own side's;
  - world space, using a moved wall and a moved door;
  - a cut that misses changes nothing;
  - a round window's volume;
  - intersect;
  - two cuts, one of them a level mesh;
  - an open wall clipped: no caps, the door's area gone exactly (intersect keeps just that);
  - a plane clipped by a sphere (about a 1 m disc gone) and by a damage blob;
  - a torus cutter refused;
  - the clipped plane's boundary only its outline and the hole (welded, no T-junctions);
  - UVs, mask UVs and layers carried through the cut;
  - the bake's keys: the same for a mesh saved and read back, another for a moved cut, another cap,
    another order or another wall; its blob read back to the very meshes; short or foreign data
    refused.
- `editor_level_cuts` covers:
  - a cut box through a wall (drawn cut, its mesh unchanged);
  - the cut moved away and back by undo;
  - the cut deleted and back;
  - saved and opened again.
- `editor_level_cut_art` covers:
  - a cut sphere over a created cube (`cutCopies`: 1);
  - a click on the cube selecting it;
  - the cut moved away and back;
  - the cube deleted (no copy; it stays gone) and back.
- `editor_level_clip` covers:
  - a level plane with a cut sphere through it: clipped (a hole, no caps), with a badge in the
    Hierarchy and a note in the Inspector;
  - a closed box beside it: capped as usual, no note;
  - saved and opened again: clipped from the bake;
  - the sphere moved away: the plane whole, the badge gone.
- `editor_level_async_cuts` covers:
  - the slab above with its hole: cut on a worker;
  - the hole moved: two frames later the cut is still pending and nothing new is drawn, then the
    hole is where it went;
  - undo the same way;
  - the cut deleted: the slab whole (40,800 triangles) at once.
- `editor_level_cut_bake` covers:
  - a cut wall and a cut cube saved (`.cuts` written);
  - another scene opened and this one again: both drawn from the bake, nothing cut afresh;
  - the door moved: cut again as usual.

## Walls, rooms and openings

Walls are drawn as a path on the floor plan and stay parametric until converted. Change their
height, thickness or path and the mesh is made again, with openings still through it.

**A wall** is the `LevelWall` component (`level/walls.h` `Wall`):
- `points`: its path on the entity's floor (x, z);
- `closed`: a room's walls;
- `height` and `thickness`;
- `align`: where the path runs (the wall's middle, its left face or its right face);
- `floor` and `ceiling` slabs for rooms;
- materials for each side, the top and ends, the floor's top and the ceiling's underside.

**Its mesh** comes from `buildWall` and is set as the entity's `LevelMesh` whenever the wall
changes (`LevelMeshes::updateWalls`):
- It is closed, so cut volumes cut it.
- It is mitred where segments meet; a sharp corner's miter is clipped at 4x.
- Its ends are capped.
- Faces are flat with World UVs, so bricks line up along it.
- A room's floor and ceiling are slabs over the inside outline. The floor's top is 5 mm above
  the room's base. At the base it lay in the plane of the ground the room stands on, and the
  two z-fought, so the floor flickered between brick and ground as the camera moved. The walls
  still start at the base.

**An opening** is a `WallOpening` {distance along the path, sill, width, height} on a cut volume
under the wall:
- `LevelMeshes::openingWorld` places its cut on the wall's path, through the wall's middle and a
  little deeper than the wall. It slides with the wall (its own transform isn't used).
- Its wireframe is drawn where its cut is, with it, its wall or its leaf selected.
- `round` makes it a cylinder.

**In the editor:**
- **Create/Wall** and **Create/Room** (Shift+A or the palette): a post of the wall's height follows
  the mouse until the first click. Click corners on the ground (0.25 m snapping; Shift turns it off).
  The wall shows as it grows; a right-click (or Enter) makes it as one undo step, Esc leaves it.
- **Create/Door** (1 x 2.1 m) and **Create/Window** (1.2 x 1.2 m, 0.9 m up): an amber box follows
  the mouse over walls where the cut would go (`editor.level.opening.preview`, drawn over the view);
  click a wall.
- **Wall/Convert to Mesh**: the wall becomes a plain level mesh and its openings plain cuts
  where they are, so every mesh tool applies.
- Messages, each one undo step:
  - `editor.level.wall.create` {points, closed, height, thickness, align, floor, ceiling,
    materials};
  - `editor.level.wall` (changes the selected wall);
  - `editor.level.opening` {at | distance, width, height, sill, round};
  - `editor.level.convert`.
- Saved in the edits as `"wall"` and `"opening"`. The mesh is made again from them on load.
- C# previews a wall with the kernel's `wall` operation (`LevelOps.Run("wall", null, args)`).

The tests:
- `level_walls` checks:
  - a straight wall, an L (mitred) and an aligned wall: closed, with their volumes;
  - a room, with a floor and a ceiling;
  - the path functions;
  - a door cut through.
- `editor_level_walls` covers:
  - the wall tool (two clicks, Enter);
  - an L with a door drawn cut;
  - the L made taller and back;
  - convert, and back by undo;
  - a room;
  - saved and opened again.

**Openings slide.** An opening's own transform is kept at its foot on its wall's path (x along the
wall, y up, z through it; `LevelMeshes::openingFrame`), so whatever is under it goes with it:
- Move it with the gizmo, type a position, or align it: it slides to the nearest place along its
  wall and stays on it. Up and down move it too: the height it was put at becomes its sill, kept
  inside the wall (from the floor to the wall's height less its own). Undo puts it back where it was.
- Change the wall (its points, thickness) and its openings stay on it at their distances.
- **Door and window prefabs:** make a prefab of an opening with what belongs to it underneath (a
  frame, a leaf). Drop it on a wall (the prefab shelf, Ctrl+Shift+1..9, or from the Project panel) and the
  copy goes into that wall where the mouse is, cutting it, at the prefab's own sill.
- **Help/Make Starter Kit** (`Falcon.Level.StarterKit`) saves four such prefabs in Assets/Kit: a
  door (1 by 2.1 m), a double door (1.8 by 2.2 m), a window (1.2 by 1.1 m, 0.9 m up) and a vent
  (0.5 m square, 2 m up), each with jambs and a head (the window and vent a sill too). They are made
  on a bench wall out of the way, which is deleted after. `editor_level_kit` makes the kit and drops
  its window on a wall: it goes in, framed, and cuts the wall.
- `editor_level_openings` checks it: an opening moved off its wall slides back onto it, at that
  height as its sill (1 m up a 3 m wall is 0.9 for a 2.1 m door, below the floor 0), and undoes;
  a door prefab with a leaf, dropped on the wall's middle, goes into it there; turning the wall takes
  its openings and their leaves with it. Test expectations: `"openingDistance"`, `"openingSill"`.

**Roofs** (`level/roofs.*`, the `LevelRoof` component) are parametric too: Create/Roof (Shift+A)
puts one over the selected room or wall, on top of its walls, and its mesh follows its settings.
- **Styles:** flat (a slab), shed (one slope), gable (two slopes up to a ridge, gable ends), hip
  (four slopes, the ridge shortened at both ends) and pyramid (all sides up to a point).
- The sloped styles cover the outline's bounding rectangle along its longest side, so the ridge
  runs the long way. A hip over a square is a pyramid.
- Its settings: base (the eaves' height: the walls' height to begin with), pitch in degrees,
  overhang past the outline all round, thickness (flat), and its top's and underside's materials.
- Each is a closed solid with a flat underside, so cut volumes cut it (a skylight).
- Convert to Mesh makes it a plain level mesh.
- In the edits and prefabs: `"roof": {"outline", "style", "base", "pitch", "overhang", "thickness",
  "material", "under"}`. Messages: `editor.level.roof.create` {outline (else the selected room's),
  style...} and `editor.level.roof` (the selected roof's settings).

The roofs' tests:
- `level_walls` builds each style over a square (and a hip over a 6 by 4 m rectangle, turned and
  wound the other way): each closed, with the volume it should have, its eaves and ridge where they
  belong. It also builds a flat roof over an L, grown by an overhang.
- `editor_level_roofs` puts a roof over a room (gable), makes it hip and then flat, undoes, converts
  it and undoes, then saves and opens the scene again; `editor_level_roofs_drawn` checks the roof is
  drawn. Test expectation: `"roofStyle"` ("" none).
- F (Frame) on a level mesh frames its own bounds (a room drawn away from its entity's origin).

**In the Inspector**, the level components have sections of their own; each change is one undo
step:
- **Wall** or **Room**: height, thickness, where the path runs (its middle or a face), closed,
  floor, ceiling and slab, the materials of each side, the top, the floor and the ceiling, and
  Convert to Mesh.
- **Roof**: style, base, pitch, overhang, thickness (flat), its materials, and Convert to Mesh.
- **Wall Opening**: distance along its wall, width, height and sill (`editor.level.opening.set`).
- **Cut Volume**: shape, intersect, cap material, and for damage its seed and roughness, with
  Another Break for a new seed.

`editor_level_inspect` types a door's width, a wall's height and a damage cut's seed into these
sections, and undoes the last.

## Wear and tear

Wear is generated into a layered mesh's mask, into one of its surface materials. Add a material
such as bare metal or moss to the mesh's list, then set what it is used for:
- **Edge wear** (the list's dropdown, or Paint/Edge Wear) puts it along the mesh's sharp outside
  edges: chipped paint, worn corners.
- **Stain** (the dropdown, or Paint/Grime) puts it in the inside corners, at the mesh's foot and
  where other level meshes come near it: dirt where walls meet floors, moss in corners and at the
  base.
- **Streaks** (the dropdown, or Paint/Streaks) run it down the sides from the ledges above them:
  rain marks below a wall's top and a window's sill.

**Live wear** (the `LevelWear` component) keeps the wear up as you build. These uses and commands
add a rule rather than paint pixels, and the rule is made again whenever the mesh changes:
- the mesh is edited, or turned or scaled;
- a cut changes it (cut a window into a worn wall and its rim wears too);
- level meshes around it move (a floor moved under a wall brings the grime with it).

How live wear works:
- It is made on a job thread, at most every 150 ms per mesh while something keeps changing. The
  last result stays drawn meanwhile.
- It is drawn under the painted mask: the higher of the two.
- Widths are the world's metres (a box scaled into a wall wears as wide along it as across), and
  streaks run down the world's up.
- Only the rules are saved (`"levelWear": [{"kind", "layer", "width", "strength", "foot",
  "seed"}]` in the edits); games make the wear when the scene loads.
- The Inspector's **Live Wear** section lists the rules: layer, width (streaks: length), strength,
  foot (grime) and seed, and Remove.
- **Bake into Mask** (or Paint/Bake Wear) puts what is made into the painted mask, and the wear
  stops following the mesh.
- Messages: `editor.level.wear` {op: "add" (kind, layer, width, ...; one of that kind in that
  layer is replaced), "set" (index, fields), "remove" (index), "clear", "bake"}. Each is one undo
  step.

**How it is made** (`level/wear.*` `generateWear`):
- Each face is drawn into the mask in its mask UVs. Each texel's value comes from its point on the
  mesh.
- Edges count when their faces turn by more than 30°. Convex ones wear; concave ones and the foot
  gather grime. A face looks at the edges in its plane, so a cut mesh's triangles wear like the
  faces they came from.
- Round edges count too: a round bevel's faces each turn less than 30°, but together they turn
  90° in a few centimetres.
  - Each edge that turns a little spreads its turn over the faces beside it.
  - A point wears (or, curved in, gathers grime) by how far the surface turns within the rule's
    width of it: from half of 30° up to 30°.
  - A round bevel wears all across, fading onto the flat faces beside it. A big gentle curve
    (a cylinder's sides) doesn't.
- Faces without mask UVs of their own (a cut's broken faces) are skipped.
- Grime also gathers within its width of the nearby surfaces' triangles.
- Streaks start at convex edges below a face that looks up. They run straight down within their
  length, thin and broken by noise stretched upright.
- Smooth value noise breaks the result up. The charts get two texels of padding.
- It is written as the higher of the value and what is already there, so painting stays.
- Painting over it afterwards works like any mask: layer up, or erase.

**Baked into the mask** instead (`editor.level.mask` {op: "wear" | "grime", channel, width,
strength, foot, seed}; in the mesh's own space, without the nearby surfaces):
- `channel`: the layer (-1: the paint tool's).
- `width`: how far from an edge it reaches.
- `foot`: how far up from the bottom grime reaches.
- One undo step each.

**Smart materials** (`.fsmart`, `level/smart_material.*`) put a whole look on a mesh in one click:
its faces' base material, its three layers, and its wear and grime as live wear rules.
- An example:

  ```json
  {"name": "Chipped Paint", "base": "Assets/Smart/Paint.fmat",
   "layers": ["Assets/Smart/Bare Metal.fmat", "Assets/Smart/Dirt.fmat", ""], "blend": 0.15,
   "wear": {"layer": 0, "width": 0.08}, "grime": {"layer": 1, "width": 0.15, "foot": 0.25}}
  ```

- The paint panel lists the project's presets (Apply). With none yet, its button makes the
  starters.
- **Paint/Make Starter Smart Materials** writes Old Concrete, Chipped Paint and Rusty Steel to
  `Assets/Smart`, with their materials.
- Applying one is a single undo step. Its wear and grime replace the mesh's live wear, and what
  was painted is cleared.
- Messages: `editor.level.smart` {path}, `editor.level.smart.list`, `editor.level.smart.starter`.

**The decal stamp brush** (Paint/Decal Brush, or the paint panel's Decals) stamps parallax decals
along a stroke: cracks, stains, bullet holes, posters.
- It works on any surface (level meshes, imported art, terrain, the ground), with nothing selected.
- Its material comes from the material palette (the panel's Decal list).
- A decal is stamped every 1.2 brush radii along the stroke. Each faces out of the surface, turned
  at random and sized 0.8–1.2 times the brush. The brush's strength sets their opacity.
- Each decal is an ordinary entity with a Decal component, so it can be moved, edited or deleted
  afterwards. The whole stroke is one undo step.
- Tools state: `paintMode` 5, `paintDecal` (the material).

**Geometry damage** breaks the level meshes themselves, with live cuts of a lumpy shape.
- A **damage cut** (`CutVolume` shape `damage`) is a ball pushed in and out by noise. Its `seed`
  shapes the lumps, and `roughness` (0–0.9 of its radius) sets how far they reach. It is sized by
  its entity's scale, like the other cuts. Create/Cut Damage makes one at the view's centre.
  - The ball has 64 segments round, so the noise's fine lumps come out as lumps, not spikes. The
    faces it breaks open are shaded smooth together (one smoothing group per cut; a sphere cut's
    too), so they read as broken material, not facets. (Cut bakes went to version 3 with this,
    so older scenes cut again once on opening.)
- The **damage brush** (Paint/Damage Brush, or the paint panel's Damage) cuts them into level
  meshes along a stroke, one every 1.5 brush radii:
  - **Holes**: where you drag, as wide as the brush.
  - **Chip edges**: on the nearest convex sharp edge of the face under the brush.
  - **Break corners**: on its nearest corner.
- Each cut is turned and stretched at random and goes under the mesh it broke, so it moves with
  it. The brush's strength sets how rough the cuts are.
- The panel's **Inside** picks the material of the broken faces (from the material palette). By
  default they get the mesh's own.
- **Rubble** (the panel's Rubble, on by default): each cut in a wall or a ceiling drops a heap of
  what it knocked out onto whatever is below it, on the side it was hit from. Holes in floors drop
  none, since the rubble would fall through.
  - A heap is a level mesh named Rubble (`level/rubble.*` `makeRubble`): 4–8 lumpy, flat-faced
    chunks resting on the ground. The middle chunks are bigger.
  - It is sized by the cut and gets the broken faces' material (or the Inside material).
  - It is an ordinary level mesh: move it, cut it, give it layers.
- The whole stroke is one undo step. Each cut is an ordinary entity: move, scale or delete it.
- **Damage over the selection** (Paint/Damage Selection, the paint panel's Damage selection; one
  undo step), with the brush's size, strength, rubble and inside:
  - a level mesh's chosen **vertices**: a broken corner at each;
  - its chosen **edges**: chips along each, 1.5 brush radii apart, out between the edge's faces;
  - its chosen **faces**: holes scattered over each, about one per 2.5 radii squared (at least one);
  - the whole mesh (the object mode, nothing chosen): chips along every sharp outside edge, and
    along the middle of every round one (a round bevel);
  - a selected **spline**: along its curve, 1.5 radii apart, on the level mesh nearest each spot,
    as the brush's kind (holes, chips, breaks). Its Inspector section's Damage buttons do the same.
  - Chips and breaks drop rubble at one in three, so a long edge isn't buried.
  - Three settings shape it, in the paint panel's Damage (under Damage selection) and in the
    Level window's ⚙ on Paint/Damage Selection. They are the same values, so a change in one shows
    in the other:
    - **Spacing**: how far apart the chips along edges (and the spline's spots) are, in metres;
      over faces, about one hole per spacing squared. **Auto** (0) takes it from the brush: 1.5
      radii, holes 2.5.
    - **Randomness** (0–100%): how far each chip strays from its even place along its edge (up to
      half the spacing either way), and how much its size varies (up to ±50%). At 0 they are evenly
      spaced and one size.
    - **Density** (0–100%): the share of those places that get damage, picked at random. 100% fills
      every one; 50% about half, leaving gaps.
    - The same settings give the same damage for one run's seed; each run is placed anew.
  - `editor.level.damage` ({"vertices" | "edges" | "faces": [...]} to name them), kernel
    `level::damageSpots` (`level/wear.h`).
- **Damage gizmos**: a damage cut's outline (and its icon) shows only while it, or the mesh it
  breaks, is selected, so a battered wall isn't lost under them; a box select takes the mesh, not
  its cuts. Door and window cuts still show all the time.
- The brush's chips snap to a face's sharp edges; on a round bevel's faces (none sharp), to their
  edges that turn.
- Tools state: `paintMode` 6, `paintDamage` (0 holes, 1 chips, 2 corners), `paintRubble`, `paintCap`,
  `damageSpacing` (m, 0 auto), `damageRandomness` and `damageDensity` (0..1). `editor.level.damage`
  also takes `spacing` (m), `randomness` and `density` (%), as the Level window's command sends them.
- In the edits: `"cut": {"shape": "damage", "seed", "roughness", "cap", "intersect"}`.

The tests:
- `level_wear` checks:
  - a box's corners worn and its faces' middles clean;
  - painting kept;
  - grime underneath, none on top;
  - grime in an L wall's inside corner;
  - grime where a floor comes near a box, none without it;
  - streaks down the sides from the top, none on the top or bottom and none past their length,
    another seed giving other streaks;
  - a rule read back from JSON;
  - nothing done without mask UVs;
  - a round bevel on the box:
    - its new faces get mask UVs apart from the others, and the faces it left alone keep theirs;
    - wear all across it;
    - 8 chips along its middle;
    - the brush's chip snapping onto it;
    - the Bevel modifier's faces getting mask UVs too;
  - a smart material read and applied.
- `editor_level_wear` puts edge wear and grime into a box's metal and dirt layers
  (`editor_level_wear_drawn`: the view changes), and undoes each. Then it puts the starter
  Chipped Paint on the box in one step (its two live wear rules made), and undoes that.
- `editor_level_live_wear` (`editor_level_live_wear_drawn`: the view changes) works on a worn
  wall standing on a floor:
  - edge wear made, then made again with more wear once a window is cut into the wall;
  - grime where the floor meets the wall, gone with the floor moved away, back when it returns;
  - streaks below the top and the sill;
  - a rule's width changed;
  - baked into the mask, then undone;
  - saved and opened again, the wear made again from its rules.
- `editor_level_round_wear` gives a worn box a round Bevel modifier (6 segments):
  - 300 triangles drawn, with wear all across the rounds;
  - then applied into the mesh, and Damage Selection makes 168 chips along the rounds' middles.
- `editor_level_paint` ends with a stroke of decals across the box: 4 decals, drawn
  (`editor_level_decals_drawn`), gone in one undo and back in one redo.
- `level_cuts` cuts a damage hole through a wall (closed, about a ball's volume, the same for one
  seed and different for another) and breaks a box's corner. It checks where the brush's dabs go
  on an edge, at a corner and in a hole. It also checks a heap of rubble: closed chunks facing
  out, resting on the ground within its spread, the same for one seed and different for another.
  Damage over a selection: 48 chips along a 1 m box's edges (96 when it is twice the size), one
  edge's, a corner out from its vertex, 16 holes over a face (the same for one seed), one on a
  face smaller than the spacing. With no randomness the chips are even and one size; with full
  randomness they stray along their edges (still on them) and vary in size; half the density keeps
  about half (the same for one seed), none at 0; 50 cm apart, 24.
- `editor_level_damage` cuts three holes across a box in one stroke (drawn:
  `editor_level_damage_drawn`), each with its heap of rubble. It undoes them (the rubble goes too),
  chips an edge and breaks a corner, and saves and opens the scene again. Nothing selected, no
  damage gizmo is drawn; the box selected, its two. Then it chips all of the box's edges (36,
  `level_damage_edges.png`) and undoes that, holes a face (4), and chips along a spline over its top
  edge (4, `level_damage_selection.png`). Between those it sets the spacing to 50 cm (24 chips, the
  Level window's setting following), then runs the Level window's Damage Selection at 25 cm with
  full randomness and half the density (fewer than 40, `level_damage_scattered.png`).

## Modifiers

A level mesh can carry a **modifier stack** (the `LevelModifiers` component, `level/modifiers.*`).
The modifiers are applied in order to the mesh before it is drawn, cut and collided with. The mesh
itself stays as you edited it: the element tools edit it, and a mirror's other half or an array's
copies follow. A pick on a copy gives the mesh's own face.

| Modifier | What it does | Its settings |
| --- | --- | --- |
| Mirror | The mesh and its mirror image across a plane. With Weld the seam is joined and the faces on it go. | across X/Y/Z, offset, weld |
| Array | Copies in a row, around an axis (radial) or in a grid. Each copy can be moved, turned and scaled at random (jitter, by seed); copies that touch are welded and the faces between them go. | mode, count, step (relative: times its size), axis, angle, radius, jitter, seed, weld |
| Solidify | A surface given thickness, its open edges closed by a rim (a closed mesh: a hollow shell). | thickness, offset (-1 inward .. 1 outward) |
| Bevel | Edges whose faces turn by more than Sharp are cut into faces (a chamfer), and the corners filled. Smooth edges (a cylinder's sides) stay. With 2 or more Segments the edges are rounded (Curve: Round, Flat, or Curved in) and a box's corners become rounded patches, shaded smooth. | width, sharp, segments, curve |
| Bend, Twist, Taper | Bent through an angle along an axis, twisted about it, or tapered to a factor at its far end. Add loop cuts for a smooth bend. | axis, angle or factor |
| Follow Spline | Bent along the spline it is under: its length axis along the curve, y up. With Fit, copies end to end are stretched to fill the spline exactly (a road of kerb pieces). | length axis, fit |
| Copies Along Spline | Copies every Spacing metres along the spline it is under (fence posts, lamps), turned with the curve or not. | length axis, spacing (0: its own length), turn |

In the editor:
- **Add them** from Shift+M (the modifiers menu), the command palette (Modifier/...) or the
  Inspector's **Modifiers** section (Add Modifier, on every level mesh).
- **The Inspector** shows each modifier's settings, a switch to turn it off, Up and Down (its place
  in the stack), Remove, and Apply on the first one. **Apply** puts it into the mesh itself (a wall
  becomes a plain mesh); Apply All, or Modifier/Apply All, does the whole stack.
- Every change is one undo step.
- In the scene's edits: `"modifiers": [{"kind": "mirror", "enabled", "axis", "offset", "weld"},
  {"kind": "array", "mode", "count", "step", ...}]`.
- Message: `editor.level.modifier` {op: "add" (kind, mode, deform (a deform's type), follow,
  class), "set" (index, fields; a deform's type as "deform", a C# one's "settings" merged), "remove"
  / "up" / "down" (index), "apply" (all?), "clear"}.

**Modifiers of your own** are C# classes in the project's scripts (or a library's): a
`Falcon.Level.MeshModifier` with `[Property]` settings and an `Apply` that makes the modified mesh.

```csharp
public sealed class SideBySide : MeshModifier
{
    [Property] public int Count = 2;
    [Property] public float Gap = 0.5f;

    public override PolyMesh Apply(PolyMesh mesh)
    {
        // ... a new PolyMesh with Count copies of `mesh`, Gap metres apart
    }
}
```

- The source generator registers them (no reflection, so they work in NativeAOT games too). The
  Inspector's Add Modifier lists them, marked "(C#)", and shows their settings like a component's.
- In the stack they are `{"kind": "script", "class": "ScriptTest.SideBySide", "settings": {...}}`.
  A settings change through `editor.level.modifier` "set" merges into what it has.
- The engine runs them through a handler the scripts give the level API (`FxLevelApi::
  modifierHandler`, level API v3) at start and again after every reload. Meshes with C# modifiers
  are made again with the new code.
- With the class gone (renamed, removed, or not built yet), the mesh is drawn as it is and the
  Inspector says so; the modifier and its settings are kept.
- `editor_level_script_modifier` writes one into the test project, builds it and puts it on a
  level box (two copies). It changes a setting (three) and undoes that, adds a Twist by its
  `deform` key, then removes the class and checks the box is plain again once the code reloads.

Games apply the stack when the scene loads, and the live room reserved for level meshes counts the
modified triangles (a spline's followers too). Arrays of prefabs (linked instances) come with the
scatter tools.

## Splines

A **spline** (the `LevelSpline` component, `level/splines.*`) is a smooth curve through points,
open or closed. Each point is smooth or a **corner**; a spline of corners runs straight from point to
point. Its points are in its entity's space, so moving the entity moves the curve.

- **Loft:** a spline can be swept into its own mesh by a profile. Its LevelMesh is made again
  whenever the spline changes, like a wall's.
  - **Road:** a slab 4 m wide, its top on the curve, 20 cm thick.
  - **Kerb**, **Wall:** standing on the curve.
  - **Rail:** around the curve.
  - **Pipe:** a smooth tube, with its sides.
  - **Custom:** points of your own.
  - Each takes width, height, step (how finely it follows the curve), UV length (metres a
    texture tile runs along it) and a material. The faces get UVs along the length, and the ends
    are capped unless the spline is closed.
- **Followers:** a level mesh under a spline with a Follow Spline or Copies Along Spline modifier
  follows it. Change the spline and they follow.
- **In the view:** its curve shows while the spline, its parent or one of its followers is
  selected, and every spline's while the Spline tool is on; not over the level otherwise.

The **Spline tool** (Create/Spline, or Create/Road, Kerb, Spline Wall, Rail or Spline Pipe for a loft;
Spline/Edit Points, or Edit in the Inspector):
- A click on any surface adds a point to the selected spline. With none selected, it starts a new
  one with the tool's profile.
- **Drag** a point to slide it over the surfaces under the mouse (live, one undo step).
- **Ctrl+click** puts a point in between the two it is nearest.
- Over a point, **X** takes it out and **V** makes it a corner (or smooth again).
- **C** closes the spline or opens it. **Enter** or **Esc** leaves the tool.
- The curve is always drawn in the view (a spline without a loft shows nothing else), and its points
  when selected. Click the curve to select it.

The Inspector's **Spline** section has Closed, All Smooth, All Corners, Edit (the tool), and the
loft's profile and settings. Each change is one undo step.

**Following a spline:** select the mesh with the spline (the mesh last, as primary), then use
Modifier/Follow Spline or Modifier/Copies Along Spline. The spline becomes the mesh's parent, in
the same undo step.

In the scene's edits: `"spline": {"points": [[x, y, z, corner]...], "closed", "loft": {"profile",
"width", "height", "sides", "step", "uvLength", "material", "points"}}`.

Messages:
- `editor.level.spline.create` {points (world), closed, profile, name}.
- `editor.level.spline` {closed, profile (its preset sizes with it), width, height, sides, step,
  uvLength, material, corner (an index, toggled), corners (all), points}.
- The tools state's `splineProfile` is what a new spline is lofted into.

The tests:
- `level_splines` checks:
  - a straight spline's length and frames (along it, y up, z to its right), past its end;
  - a smooth spline through its points, and a closed square of corners 40 m round;
  - a road loft's exact volume, its top on the curve, its material and UVs;
  - a kerb round a closed square (closed, no caps), and a pipe;
  - ten boxes bent end to end along 10 m, a box bent round a curve, and copies every 2 m;
  - no spline: the mesh as it is;
  - JSON back and forth.
- `editor_level_splines` (`editor_level_splines_drawn`: the road drawn):
  - a road along three points;
  - the Spline tool: a point dragged out (undone), one added at the end, one put in between
    (Ctrl+click), one taken out (X), closed and opened (C), Esc;
  - the loft made a pipe and undone;
  - a post copied along the road (it becomes the road's child), undone and redone;
  - saved and opened again.
- `test.mouse` takes `"at": "@splinePoint:N"` (the selected spline's point N, offset by `dx`,
  `dy`) and `"mods": ["ctrl", "shift", "alt"]` (held from then on; `[]` lets go).

The tests:
- `level_modifiers` checks:
  - a mirrored half box welded into one box (each face's source kept), a symmetric box mirrored onto
    itself, and an unwelded mirror (two boxes);
  - three boxes in a row, a ring of eight two metres out, a 3 x 3 grid (all closed, the right volume);
  - jitter: the first copy left where it is, another seed giving other places;
  - a plane solidified into a slab;
  - a box bevelled (its exact chamfered volume) and a cylinder's rims only;
  - a box bevelled round with 16 segments (a rounded box's volume), and flat with 16;
  - taper, twist and a bar bent a quarter turn;
  - a stack (a mirror, then an array), JSON back and forth, a disabled modifier skipped.
- `editor_level_modifiers` (`editor_level_modifiers_drawn`: the view changes) works on a level box:
  - a mirror at its +x face, then an array of that;
  - the stack reordered, a modifier switched off and undone;
  - one removed with the Inspector's Remove, a bevel added, then made round (6 segments);
  - the mirror applied into the mesh and undone;
  - saved and opened again.

## Placing and arranging

**Arrange** (Shift+G, or Arrange/... in the command palette) works on the selection's roots. Each
one's bounds are its subtree's meshes, or its origin. Every arrange is one undo step.

| Command | What it does |
| --- | --- |
| Align Min / Centre / Max X, Y, Z | Each one's bounds' minimum, centre or maximum on that axis at the last selected one's (that one stays) |
| Distribute X, Z | Their centres spread evenly between the first and the last on that axis (three or more) |
| Drop to Ground (End) | Each one down onto whatever is under its bounds' bottom centre |
| Group (Ctrl+G) | The selection under a new empty, "Group", at their bounds' bottom centre (under their parent when they share one); each stays where it is |
| Ungroup (Ctrl+Shift+G) | The selected group's children under its parent instead; the empty goes when nothing else is on it |

**Replace** (`editor.replace` {prefab}): each selected root becomes an instance of the prefab where it
was (its rotation, scale and parent too). The old ones are deleted, all in one undo step.

The **prefab shelf** (the Level window's Place section, prefs `prefabShelf`) holds up to nine kit
pieces:
- Drop prefabs onto it from the Project panel.
- A click places one at the view's centre. **Ctrl+Shift+1..9** places it where the mouse is
  (`editor.asset.place` {prefab, atMouse}).
- **Ctrl+click** replaces the selection with it. Right-click takes it off the shelf.

Prefab instances are spawned by one function (`EditorBridge::spawnPrefab`) for drops, the shelf,
Replace and the scatter brush. It stands an instance on its point by its meshes' bounds, or puts it
exactly at a transform.

Messages: `editor.arrange` {op: "align" | "distribute" | "ground", axis, edge (-1, 0, 1)},
`editor.group`, `editor.ungroup`, `editor.replace` {prefab}.

The **scatter brush** (Paint/Scatter Brush, or Scatter in the paint panel; `editor/level_scatter.cpp`)
scatters prefabs over any surface along a stroke: rocks, debris, plants, props.
- **Pieces:** the shelf's prefabs ticked in the panel (all of them when it opens). Each piece is a
  random one of them.
- **How many:** about Density pieces a square metre in each dab (a dab every brush radius). None
  lands nearer than Spacing to another piece of the set, or on a surface steeper than Max slope.
- **Each piece** is found on the surface under its spot (the set's own pieces aren't in the way),
  turned at random about the world's up or the surface's (Follow surface), and sized at random
  between Smallest and Largest.
- **Where they go:** pieces go under a **scatter set**, an empty with a `ScatterSet` component, named
  "Scatter". Strokes add to the selected set, or the one the selected piece is in, or a new one.
  Each piece is an ordinary prefab instance, to be moved or deleted on its own.
- **Shift+drag** erases the set's pieces under the brush.
- A stroke is one undo step.
- Tools state: `paintMode` 7, `scatterPrefabs`, `scatterDensity`, `scatterSpacing`,
  `scatterSlope`, `scatterScale` [min, max], `scatterAlign`.
- In the edits: `"scatterSet": {"seed"}`.

**Snap points** make modular kits click together (`SnapPoint`, `editor/level_sockets.cpp`).
- **Making one:** Create/Snap Point puts a snap point (an empty) on the selected kit piece, at its
  origin. Move and turn it to where the piece joins others; it faces out along its +z (the blue
  arrow in the view) with +y up.
- **Tags:** a snap point can have one (the Inspector's Snap Point). Only matching tags join, and an
  empty tag fits any.
- **Snapping:** drag a piece with the Move tool, and when one of its snap points comes within 40 cm
  of another piece's, it clicks onto it. The two points meet, facing each other (the piece turned
  to fit).
- **Arrange/Snap Together** does the same for the selection where it is.
- Snap points live in a piece's prefab like any child, so every instance has them.

**Prefabs carry the level components** of their entities: cut volumes, walls and openings, modifier
stacks, splines, live wear, scatter sets and snap points (`LevelParts` in `editor/scene_edits.h`,
written under the same keys as in the edits). A door prefab with its own cut volume cuts every wall
its instances stand in, also once the scene is saved and opened again.

`editor_level_snap` checks:
- a kit piece given a snap point and made a prefab;
- another instance snapped onto it (turned about, moved: by Snap Together, undone, then by a small
  gizmo drag);
- a cut volume made a prefab, whose placed instance cuts a wall;
- after a save and reload: the wall still cut and the piece snapping again.

`editor_level_scatter` scatters a rock prefab over a field in one stroke (a new set with more than
three pieces: undone, redone), saves and opens the scene again, then erases them all with Shift.

`editor_level_arrange` checks:
- three boxes aligned to the last selected (undone), and spread evenly along x;
- one dropped onto the ground, and two grouped (the group at their bottom centre) and ungrouped;
- one made a prefab and another replaced with it (undone), and one placed at the mouse.

The test expectation `"at": [x, y, z]` checks an entity's world position (within `"within"`, 0.01).

## Navigation

A scene's **navigation mesh** (`level/navmesh.*`, Recast and Detour) is where agents can walk.

**Baking** (Navigation/Bake, `editor.nav.bake`, `editor/level_navigation.cpp`):
- It is baked in tiles from the scene's level meshes as they are drawn (modifiers and cuts applied;
  cut volumes themselves left out) and its meshes (each read back from the pak once).
- It is baked for the agent in File > Project Settings > Navigation: height, radius, step height,
  max slope and cell size (`"navigation"` in project.falcon).
- It is saved beside the scene as `<scene>.nav` (moved and renamed with it). Games read it when
  the scene starts.
- **Bake as you build** (on by default): once a scene has a navigation mesh, every edit, undo or
  redo bakes it again half a second after the level stops changing.
- Navigation/Show Navigation Mesh (`editor.nav.show`) draws its polygons over the view.
  Navigation/Clear takes it away and deletes its file.
- The terrain is baked in too: its part within 20 m of the level's geometry (all of it when there is
  none), at most 512 quads a side (coarser beyond that), its holes left out.

**Queries from C#** (`Falcon.Level.Navigation`, level API v4):
- `FindPath(from, to, path)`: the path's corners into `path`. It returns false when there is no
  whole way, and the corners then go as near as they can.
- `Raycast(from, to, out hit)`: a straight walk along the mesh. It returns true when an edge stops
  it at `hit`.
- `NearestPoint(point, out onMesh, reach)` and `Available`.

**Agents** (`Falcon.Level.NavAgent`, a built-in component; level API v6) walk their entity over the
navigation mesh while the game runs, using Detour's crowd:
- It follows the path to its destination, steers round the other agents, keeps its feet on the
  mesh, and turns the way it goes.
- Its settings: Speed, Acceleration, Radius, Height, TurnToMove, and Destination (where it heads
  when the game starts; zero: nowhere).
- Scripts call `SetDestination(to)` and `Stop()`, and read `Velocity`, `Remaining` and `Arrived`.
- The crowd moves on once a frame, and starts afresh with a new navigation mesh (agents join it
  again with their destinations).

**Gameplay volumes** (`LevelVolume`) are level meshes that mark space; they aren't drawn in the game.
- Make one with Create/Volume/... (a 2 m box at the view's centre: shape it with the level tools) or
  Volume/Make ... on any level mesh. The Inspector's **Volume** section changes its kind, tag and
  cost, and Make It a Mesh turns it back into a plain mesh.
- In the editor each kind is drawn as an outline of its own colour: trigger green, kill red, ladder
  yellow, audio blue, nav blocker purple, nav area orange. The outline shows while the volume, its
  parent or one of its children is selected (pick it in the Hierarchy, or click its pivot), and
  a click on it then selects it.
- **Trigger, Kill, Ladder, Audio:** each gets a box trigger over its mesh's bounds, fitted again
  when the mesh changes. Scripts on what moves through it hear OnTriggerEnter and OnTriggerExit
  with the volume's entity. What a kill or ladder volume does is the game's to decide.
- **Nav Blocker:** keeps the navigation mesh out of its footprint.
- **Nav Area:** walking through it costs Cost times as much, so paths go round when they can.
- The navigation bake leaves volumes out of what is walked on.
- C# (`Falcon.Level.Volumes`, level API v5): `KindOf(entity, out tag)`, `At(point, found)` (the
  volumes a point is in) and `Inside(point, kind)`.
- In the edits and prefabs: `"volume": {"kind", "tag", "cost"}`. Message: `editor.level.volume`
  {kind, tag, cost, remove, create (name)}.

The tests:
- `level_navigation` also checks a blocker volume in the wall's gap (no way across), and paths
  round a costly area (read back with its costs).
- `level_navigation` also walks an agent round the wall to its target, and checks a stale id.
- `editor_level_agents` plays a scene with two NavAgents on either side of the wall, each heading
  for the other side: both get there (round the wall, past each other), and Stop puts them back.
- `editor_level_volumes`:
  - a trigger volume: not drawn, with a box trigger, and a C# component finding it, its kind and
    its tag;
  - a nav blocker in the wall's gap (no way across after the auto bake), made a nav area (a way
    again);
  - the trigger made a plain mesh (drawn) and undone;
  - saved and opened again.
- `editor_level_nav_terrain` bakes a flat terrain with a level wall standing on it: a path round
  the wall's end, over the terrain.
- `level_navigation` bakes a 30 m floor split by a long wall: several tiles, a path round the wall's
  end, a raycast stopped by it, the nearest point, and saving and reading back.
- `editor_level_navigation`:
  - bakes the same in the editor (a path round the wall);
  - moves the wall away and back, baked again each time (the path straight across, then round);
  - asks for a path and a raycast from a C# component's `[Button]`;
  - saves and opens the scene again (read from the `.nav`), then clears it.
- Test expectations: `"navPolys>"`, and `"navPath"` {from, to, found, "length>", "length<"}.

## Views and measuring

**Orthographic views** (`FlyCamera::setOrthographic`, `editor.view`):
- Numpad 5 (or View/Orthographic) switches between perspective and orthographic. Orthographic is a
  1.5 degree lens from far enough back that what is at the pivot keeps its size on screen, so every
  pass (shadows, GI, picking, the gizmos) works as it is. Panning and the near plane follow the
  distance.
- Numpad 1, 3 and 7 look at the pivot from the front, the right and the top; with Ctrl, from the
  back, the left and the bottom. Each switches to orthographic.
- Shift+V opens the **view wheel** with all of these. The camera's lens shows in the tools' state
  (`orthographic`).
- Message: `editor.view` {orthographic (none: the other way), look: front, back, right, left, top or
  bottom}.

**The ruler** (View/Measure, the Measure tool):
- Drag between two points on any surface; hold V to snap either end to the nearest vertex or edge
  middle.
- The view draws the line, its run along x, y and z (red, green, blue) and its ends. The readout
  shows the length and the run along each axis.
- Esc or Enter puts the ruler away (back to the Move tool).

Not done yet: the Hammer-style four-way split (a perspective view beside three orthographic ones)
needs the renderer to draw several views a frame; and visibility layers.

`editor_level_views` checks it:
- Numpad 5 makes the view orthographic (captured), and the top view looks straight down (captured).
- The ruler, dragged a twentieth of the view across the top of a 4 m slab, reads about 0.83 m.
- Esc goes back to the Move tool, and `editor.view` makes the view perspective again.
- Test expectations: `"orthographic"`, and `"measured": [length, within]`.

## Cutting while the game runs

Games cut the level as they run (destruction, a door cut where a player builds one) with the same
live cuts the editor uses (`Falcon.Level.Cuts`, level API v7; v8 adds `Pending`):

```csharp
using Falcon.Level;

Entity hole = Cuts.Cut(CutOp.Damage(position, 1.2f, seed: 7));   // a noisy hole, rubble-edged
Entity door = Cuts.Cut(CutOp.Box(position, new Vector3(1, 2.2f, 1)));
Cuts.Remove(hole);                                              // the wall whole again
ulong h = Cuts.GeometryHash();                                  // the level as it is drawn
```

- A cut is an entity with a cut volume (box, cylinder, sphere, its own level mesh, or damage), sized
  by its transform. The level meshes it overlaps are cut again on worker threads, usually within a
  frame or two (`Cuts.Pending` until then; they are drawn and collide as they were), and their
  colliders and
  audio with them. Imported art it overlaps is drawn as a cut copy.
- `CutOp` holds the shape, position, rotation, size, Intersect, the damage's Seed and Roughness, and
  Cap (the material of the faces it makes; null: the target's).
- `GeometryHash()` hashes each drawn level mesh (cuts and modifiers applied) and where it is (to the
  millimetre), in entity order, then the cut copies. Machines that made the same cuts have the same
  hash.
- The game's end (the editor's Stop) deletes the cut entities, so the level is whole again.

**Networked** (`NetworkedCuts`, a built-in component): put it on an entity with a NetworkObject and
call `Cut(op)` on the host or on the owning client (one machine cuts). Without a session it just
cuts here.
- **Numbered:** each cut is numbered by the machine that made it and goes to every machine as a
  reliable broadcast RPC. Every machine applies cuts in number order: one that comes early waits for
  the ones before it, and one it already has is let be. Cut order matters, since each cut works on
  what the ones before it left.
- **Late join:** the host's count is part of the object's synced state, so it reaches a machine as
  soon as the object does.
  - A machine that is behind (it joined late, or missed some) asks the host for the cuts from its
    own count. It asks again a second later if they don't come.
  - The host sends them again, to everyone; the machines that have them let them be.
  - The ask is an RPC any client may make.
- `Ops` lists the cuts made here since the last `Clear()`, which takes them back here only. `Count`
  is every cut since the start (the numbering goes on through a Clear). `HostCount` is the host's
  count as last synced, and `CatchUps` how often this machine asked.
- Compare `Cuts.GeometryHash()` between machines to check they agree (once `Cuts.Pending` is false).

`net_session_cuts` plays the Arena with live mesh room (`--live-meshes`), a host and one client
under 40 ms latency and 10% loss:
- every machine makes the same level wall, high above the scene;
- the host cuts it twice before the client can have joined (a door and a damage hole), and a window
  once it plays;
- the client catches up, and its geometry hash comes out as the host's (which a `[Sync]` field
  brings it).

`editor_level_runtime_cuts` plays a scene with a level wall and a script beside a NetworkedCuts:
- It blows a damage hole in the wall: the hash changes, and the wall draws more triangles.
- It clears the cuts and makes the same one again: the same hash as before.
- Stop: the wall is whole again (12 triangles).

## The sample level

Help/Make Sample Level (`Falcon.Level.SampleLevel`) builds a small level in the open scene, each
step the message a tool sends, so each is its own undo step:
- a ground, and a room of parametric walls (10 by 8 m, 3 m high, with a floor) under a hip roof;
- a door and a window cut in its walls (openings: drag them along the wall);
- the starter smart materials, with Old Concrete on the room (live edge wear and grime);
- a road along a spline past the door, and a fence post copied along it every 2 m (a Follow
  Spline modifier);
- an entrance trigger at the door and a muddy nav area by the window;
- the navigation mesh baked over all of it.

`editor_level_sample` makes it in an empty scene and checks each part (the room's wear, the
openings under it, the road's points, the posts' copies, the volumes, the navigation mesh), then
saves it. `editor_level_sample_drawn` checks that it is drawn.
