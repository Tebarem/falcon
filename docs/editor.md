# Projects, scenes and the editor

## Projects and the launcher

`falcon.exe` with no arguments opens the launcher, a project selector like Unity Hub
(`ui/launcher`, `engine/editor/launcher.*`):
- **Recent projects:** newest first, with the folder and when each was last opened. A click
  opens one; a project whose folder is gone shows as Missing and can be removed from the list.
  The list is kept in `projects.json` in the editor's app-data folder (`%APPDATA%/Falcon/Editor`).
- **New project:** a name, a location (default `Documents/Falcon Projects`, Browse picks a
  folder) and a template (Empty). The project gets a folder of its own, named after it.
- **Open project:** a folder picker; any folder with a `project.falcon` opens.

A project is a folder (`engine/project/project.*`):

```
My World/
  project.falcon    {"format": 1, "name": "My World", "startupScene": "Scenes/Main.fpak",
                     "scenes": ["Scenes/Main.fpak"],
                     "rendering": {"lodPixels": 0.5, "cullPixels": 4, "terrainPixels": 1,
                                   "detailDistance": 10}}
  Assets/           source files (glTF, images) imported into the project
  Scripts/          the game's C# sources (see Project scripts)
  Scenes/           each scene's .fpak, with its edits (.edits.json) and terrain (.terrain/)
  Cache/            rebuildable data
```

**Project settings** (File > Project Settings, saved in `project.falcon` as `rendering`): the
LOD error (how far simplified geometry may stray from the full mesh on screen, in pixels: lower
keeps more detail at a distance, 0 draws full meshes; 0.5 by default, half the engine's own 1:
Zero-Day draws ~28% more triangles, ~0.1 ms of 3.6 at 1600×900, distant silhouettes sharper)
the cull size (objects under this
projected radius are not drawn; glowing ones always are), the terrain error (the same for the
terrain's patch levels, 1 by default) and the full-detail distance (10 m by default: nearer the
camera, meshes and terrain keep their full detail and nothing is culled for its size, so a
character's small parts or a pebble at your feet never drop out; 0 applies the pixel limits
everywhere). They apply as they change, in the editor and when the project runs as a game;
`--lod-pixels` / `--cull-pixels` / `--terrain-pixels` / `--detail-distance` override them. The
Rendering panel's sliders edit the same values while a project is open (the editor's own prefs
keep them only for scenes outside a project).

A scene of a project has no file behind it: it is cooked empty, with the built-in shapes, the
procedural rocks and every model of the project (`.gltf` / `.glb` under `Assets/`) as a library.
That is `falcon cook --empty Scenes/Main.fpak --rocks --source-root <project> --library ...`,
run as a child process. Nothing is placed by the cook: what is placed lives in the scene's edits,
by mesh name. Its bounds are a 100 × 20 × 100 m box at the origin, which the shadows, GI and the
camera start from.
- **When it cooks:** opening a scene cooks it again only when the project's models changed
  since the last cook. A stamp (`Cache/Stamps/<scene>.models`) records each model's path, size
  and date.
- **Where it cooks, with progress:** never silently, since 100+ models can take minutes.
  - The editor's Reload Scene (or opening another scene that needs it) cooks in the
    background. The Project panel's banner shows the stage and a bar, with Cancel; then the
    scene opens in the same editor (L4). A failure shows the cooker's own error in a dialog.
  - The launcher cooks a project's startup scene before starting the editor, on its busy
    overlay ("Cooking Main: 140 models · encoding textures 37%…"). A failure shows as its
    error.
  - Anything else (`--project` from the command line) gets a small window whose title is the
    stage, with a progress bar (closing it cancels), and a message box if it fails.
- **One cook per scene:** `Cache/Stamps/<scene>.cooking` holds the cooking process's id.
  Another process that needs the scene waits for it ("another window is cooking it") instead
  of cooking it too. A lock whose process is gone is ignored.
- **Staged, then moved in:** the cook is written to `Cache/Cook/` and moved over the scene's
  `.fpak`/`.fvt` when done. The editor can't replace the pages it has open, so its cook waits
  there until the scene is let go as it opens again (in the same process, L4). A cook still
  waiting there when the editor closes is moved in by the next one before it loads.
- **Only what changed is encoded:** textures take nearly all of a cook. Each `.fvt` ends with a
  key per textured material: a hash of its images' bytes, its size and its alpha settings.
  - The cooker copies the pages of a material whose key is in an earlier `.fvt` (`--reuse`:
    the scene's own and the project's other scenes'), instead of encoding them again. The
    output is byte for byte the same.
  - With 140 models (1.9 GB of `.glb`; 168 textured materials, 5.7 GB of pages, 12 threads), a
    cook from scratch takes 2 min 35 s, of which 151 s is textures. A re-cook takes 8.6 s:
    import 3.3 s, meshes 0.3 s, textures copied 4.5 s. Adding a model only encodes its own
    textures.
  - Meshes are rebuilt every time (0.3 s for the 140 models), so they have no cache.
- **Each model's layout is kept:** the cooker records where each library mesh came from and the
  model's nodes and cameras, in a trailer of the Names section that older readers skip
  (`ScenePak::libraries()`). A model can then be placed whole and previewed.
- **And its animation:** a model's moving parts and cameras play when the scene does (see
  [Rigid node animation](animation.md#rigid-node-animation)).

Quick start: run `falcon.exe`, then **New project** and **Create**. The editor opens on the
empty Main scene. In the Terrain tab, use **New Terrain**, then **Generate**, then **Save**.

- **Opening:** the launcher starts `falcon.exe --editor --project <dir>` and quits. Chromium
  initialises once per process, so each session is its own process.
- **In the editor (File menu):**
  - New Scene… (cooks `Scenes/<Name>.fpak` and adds it to the project)
  - Open Scene (the project's scenes)
  - Save Scene
  - Set as Startup Scene
  - Open Project… (back to the launcher)

  Switching scenes opens the new one in the same editor (L4: the page, the scripts' runtime and
  editor code stay; undo starts over). Switching projects restarts the editor on the new one.
  With unsaved changes it asks first: Save, Don't save or Cancel.
- **The Project panel** is the project's asset browser (`ui/editor/assets.js`,
  `engine/editor/asset_browser.*`):
  - **Left:** Favorites (every scene, model, material, prefab or image in the project, plus
    pinned folders), the project's folders, Library (Falcon's materials and My Library, below),
    and Engine (the open scene's meshes: shapes, rocks, the scene's own). Each section folds.
  - **Right:** the folder's items as a grid (a size slider) or a list sortable by name, type,
    size and date. Images show their picture.
  - **A scene is one item:** its `.fvt`, edits and terrain go wherever it goes, and the open
    scene (★ startup, ● open) can't be moved or deleted.
  - **Toolbar:** back and forward, a breadcrumb, search across the whole project and the
    libraries, and type filters.
  - **Selecting:** click, Ctrl / Shift or a rubber band selects. The arrows, Home / End and
    typing a name move through the items.
  - **Keys and mouse:**
    - Enter opens (a folder, a scene, a shape into the scene), Backspace goes up;
    - F2 renames in place, Ctrl+D duplicates, Delete deletes (to the Recycle Bin);
    - Ctrl+Shift+N makes a folder;
    - items dragged onto a folder (in the tree, the grid or the breadcrumb) move there.
  - **Right-click:** Show in Explorer, Copy Path, Set as Startup Scene, Add to Favorites, and
    Create (Folder, Scene).
  - **Inspector:** a selected asset shows there (path, type, size, date; a scene's startup
    and open state) until an entity is selected.
  - **Previews:** scenes, models, materials (a sphere), prefabs and meshes show pictures the
    engine rendered, and a folder shows a few of its items'.
    - **How they're made:** a background `falcon --preview` (`engine/app/preview.*`) renders
      them with the real renderer, scaled down from 640 px:
      - a mesh stands on a grey ground, seen from the front-right and a little above;
      - a scene is seen from the editor's last camera there.
    - **Cached:** they're rendered once, into `Cache/Thumbnails` (`index.json`: key → PNG and the
      hash it was rendered from). A mesh renders again only when its geometry or material
      changes. Saving a scene retakes its preview from the view. Opening the project renders
      nothing that already has a preview.
    - **Regenerate Preview** (right-click) asks again.
  - **Models:** a model opens like a folder (its meshes) and has its own preview.
  - **Placing:**
    - a mesh or a model dragged into the scene view stands on what is under the mouse
      (terrain, a mesh's box, the ground plane);
    - onto a hierarchy row: as its child;
    - a mesh onto a scatter layer's mesh field: that layer uses it; meshes or a model onto the
      layer's mesh list: they join the layer;
    - double-click (Enter) places a mesh at the view's pivot.
    A model becomes an empty named after it, with its nodes below. Any mesh of the scene can be
    created, not only the built-in shapes: edits save it by name (`"mesh"`), and every cooked
    mesh gets spare instance slots in the editor. A model's meshes get at least as many as its
    nodes use, so any model can be placed whole once more without a reload (a kitbashed scene
    repeats a mesh hundreds of times).
  - **Materials** (`.fmat`, `engine/assets/material_asset.h`):
    - **Making one:** Create ▸ Material makes one in `Assets/`.
    - **Editing it:** selected, the Inspector edits it: base colour and opacity, metallic,
      roughness, emission, normal and occlusion strength, alpha (opaque, cutout, transparent),
      double-sided, and four texture slots (images dropped from the panel, or picked).
    - **Assigning it:** drag it onto a mesh in the view, onto a hierarchy row, or onto a mesh's
      Material slot in the Inspector (which also picks one from a menu). It's saved in the
      edits (`"material"`) and can be undone.
    - **How it draws:** an instance's flags carry its material (bits 16..31: the index + 1),
      and every pass draws with that instead of its clusters' material
      (`instanceMaterial()`, `scene_types.slang`).
    - **What needs a cook:** project scenes are cooked with every material asset
      (`falcon cook --material x.fmat`: its textures go into the virtual texture). Its factors
      are read from the file whenever a scene loads, and an edit shows at once
      (`engine/editor/material_library.*`). So only textures need a cook: the banner offers
      Reload Scene.
    - **Until then:** a material made since the scene was cooked takes a spare slot (the
      editor keeps 32, a game as many as its edits use), without its textures. Its colour is
      its base colour times its colour texture's average, so it isn't drawn bare white.
    - **Parallax** (`"parallax"`, metres; the Inspector's Height ▸ Parallax): the height map
      drawn as relief without new geometry. The resolve steps the view ray down through the
      map (parallax occlusion, more steps at grazing angles, none when the shift is under a
      pixel), then steps toward the sun from where it landed, so the relief shades itself.
      Bricks about 0.02, cobbles 0.04, with no upper limit (the slider's span is a guide: type
      a value or drag the label past it); the steps grow with how far the relief shifts the
      view, so deep relief stays smooth. A live factor (flags bits 16..31, a half); only the
      height map needs a cook, which goes into the ORM pages' alpha. With height displacement
      too, it draws over the displaced geometry: the cooked triangles carry the shape and
      silhouette, parallax the relief finer than them.
    - **Roughness range** (`"roughnessMin"`; the Inspector's Roughness Min): the roughness the
      roughness map's black gives, its white giving Roughness (`materialRoughness()`,
      `scene_types.slang`). At 0 (the default) the map is multiplied by Roughness, as before.
      Raise it to make a material rougher than its map while keeping the map's variation:
      several Poly Haven maps floor at about 0.5 and read polished on walls, and a Roughness
      Min of 0.7 to 0.8 makes them matte. A live factor (the material record's `vtSize` top
      byte, in 255ths), and scripts reach it as the `"roughnessMin"` material param.
    - **Height displacement** (`tools/cook/build_displace.cpp`): real geometry from a grey height
      map, so a cube or a plane gets studs, cobbles or bark without anyone modelling them.
      - **On the material:** a Height texture slot and Amount (metres at white, negative pushes
        in; no limit past the slider), Mid Level (the grey that stays on the surface) and Detail (the triangle size, in
        metres; 0: about one texel of the map) — `"textures": {"height": ...}`,
        `"displacement"`, `"heightCenter"`, `"displacementDetail"`.
      - **Cooked:** each mesh an entity of the scene uses with such a material (prefabs' too) is
        cooked again as `<mesh>~<material>`: subdivided until its edges fit the detail, pushed
        out along its normal by the map, its normals and tangents rebuilt, then made into the
        usual cluster LOD DAG, so far away it costs what a simpler mesh would. It's watertight:
        positions shared across UV seams and hard edges are welded, split on both sides and
        moved together. Up to about a million triangles a mesh (the detail is coarsened past
        that); it collides like the mesh it came from. The sphere is cooked with every
        displaced material, for its preview.
      - **Loading:** an entity whose mesh and material have a displaced copy draws it (duplicates
        and placed prefabs too, while its slots last). The copies don't show in the Project
        panel.
      - **What needs a cook:** the height map, the amounts, and which meshes use the material.
        The banner offers Reload Scene once the scene is saved, and the editor says so when a
        displaced material goes on a mesh.
      - `falcon cook ... --material m.fmat --displace mesh=m.fmat`; tests: `cook_displace`
        (verified, byte-identical twice), `editor_displace` and `_reload` (the project's cube
        before and after: `editor_displace_differs`).
    - **Real-world size:** `"size": [w, h]` (metres one repeat of its textures covers; the
      Inspector's Size X / Y). Put on level-mesh faces with world or local UVs, it becomes their
      UV scale, so planks and bricks land at true scale. A wall's faces take the material
      through its sides (Side A, Side B, Top, the floor and ceiling), which it is rebuilt from;
      its UVs stay one tile a metre.
  - **Library** (`engine/project/asset_library.*`): materials, textures and models every project
    sees, in the panel's Library section, shared by them all.
    - **Falcon:** 30 CC0 materials shipped with the engine (`content/library`, from
      [Poly Haven](https://polyhaven.com), `License.txt`): wood, concrete, plaster, brick, metal,
      tiles, ground and roofing, each with its colour, normal, AO / roughness / metal and
      height maps at 2K, its real-world size and a parallax depth for its relief. Only the bare
      metals (Corrugated Iron, Metal Plate) are metallic; the rest are dielectric whatever
      their maps' blue. Read-only. `tools/library/fetch_polyhaven.py` and
      `make_library.py` remake it.
    - **My Library:** a folder of the user's (`Documents/Falcon Library`; the section's ⚙ picks
      another, kept in the editor prefs; `--library <dir>`; under `--no-prefs`, the project's
      `Cache/Library`). It takes what a project's `Assets/` does: folders, imports, New
      Material, the material Inspector, rename, move, duplicate, delete. Its textures are its
      own: a project image set on one of its materials is copied beside it.
    - **Paths:** `@falcon/Wood/Oak.fmat`, `@library/Bricks/Red.fmat`; a library material's
      texture paths carry them too. A library material's picture is its colour texture.
    - **Using one:** dragged onto a face, a mesh, a hierarchy row, a slot or the palette, picked
      in a material slot (its menu's Falcon / My Library groups) or placed, a library item is
      first copied into the project: `@falcon/X` into `Assets/Falcon/X`, `@library/X` into
      `Assets/Library/X`, a material with its textures (their paths made the project's), a
      `.gltf` with its files. A copy made before is used as it is, so the project's edits to it
      stay. Its textures show after Reload Scene (the banner), like any new material's.
    - **Add to Library** (right-click a project material, image, model or clip, or drag it onto
      a My Library folder) copies it there: a material into a folder of its own with its
      textures. Items move only within the project or one library.
    - **Games** ship only their projects' copies: a game build leaves `content/library` out.
    - Test: `editor_asset_library`.
  - **Prefabs** (`.fprefab`, `engine/editor/prefab.h`): a group of entities saved as an asset,
    placed as linked instances.
    - **Making one:** drag an entity from the Hierarchy onto a folder of the panel (or use
      Save as Prefab, or Create ▸ Prefab with it selected). It and its children are saved
      (meshes, level meshes, empties, lights, cameras made in the editor), and it becomes an
      instance. Instances of other prefabs below it **nest**: a wall prefab in a room prefab
      in a building prefab.
    - **Placing one:** drag it into the view or onto a row like a model; another one is named
      "Crate (1)", "Crate (2)", and so on.
    - **In the Hierarchy:** an instance shows in blue. An entity changed from its prefab
      (transform, visibility, material, name, light, camera) has a dot; those changes are its
      overrides.
    - **Commands** (row menu and the Inspector's prefab bar):
      - Apply writes the instance into the prefab, and the other instances follow, keeping
        their own overrides. That includes entities added below it (an instance of another
        prefab added becomes a nesting), entities deleted in it, and changes to nested
        prefabs' entities, which become overrides of their nesting (the nested prefab's file is
        left as it is). The other instances gain and lose entities to match.
      - Deleting an instance's entity removes it from that instance only: an override
        (`"removed": true`), kept when saved.
      - Revert drops the overrides.
      - Unpack makes plain entities of it, one level: what nests in it stays an instance of its
        own prefab. Undo links it again.
    - **How it's saved:** an instance is one entry of the edits (`"kind": "prefab"`: the prefab,
      the root's transform, the overrides by the prefab's ids), built from the prefab when the
      scene loads. Entities placed under an instance's entity point at it by a stable key
      (`p<root>:<id>`).
    - **Nesting** (`readPrefab`): an entity of kind `"prefab"` in a prefab nests another one,
      with its own `"overrides"`.
      - Reading flattens it: the nested prefab's entities take path ids (`"<entity id>/<their id>"`,
        any depth), so an instance's overrides reach into nestings (`"3/1"`).
      - A prefab that contains itself, or nesting deeper than 8, stops there with a warning (an
        empty stands in).
      - `readPrefabFile` reads the file as it is, which is what Save and Apply write.
      - `Prefab.Spawn` and the Model Renderer's slot reservation see the flattened prefab too.
  - **Import:** the Import button (or files dropped on the window) copies files into the
    folder shown, if it is in `Assets/` (else into `Assets/`). A `.gltf` with files of its own
    (buffers, images) gets a folder for them.
    - **New models:** the panel then says the scene needs reloading; Reload Scene saves (if
      asked), cooks it again with the models (progress on the banner) and opens it again.
  - **Live:** the engine rescans the project about once a second while the tab shows.
  - **Remembered:** the folder, view, sort, size, favorites and folded sections.
- **Editor preferences** (window layout, camera) are kept per project and scene.
- `--scene file.fpak` without a project keeps working, for the game and the tests.

## Scenes

glTF is cooked offline into `.fpak`. Every section is page-aligned and already laid out
as its GPU buffer, so loading is one async read plus one upload. The read overlaps GPU
device creation.

```powershell
./build/release/bin/falcon.exe cook samples/_src/khronos/Models/Sponza/glTF/Sponza.gltf samples/cooked/sponza.fpak --verify
./build/release/bin/falcon.exe --scene samples/cooked/sponza.fpak
```

**Asset libraries:** `--library more.gltf` (repeatable) adds another glTF's meshes, materials and
textures to the cook without placing them, for terrain scatter and other runtime spawning. Every
mesh is named in the `.fpak` (its node's name, the library node's, or the built-in's: `cube`,
`sphere`, `capsule`, `cylinder`, `plane`, `quad`, `cone`), and the editor lists them by that name.

**Reusing pages:** `--reuse earlier.fvt` (repeatable; the output's own `.fvt` always counts)
copies the pages of textures that are the same as in there instead of encoding them again.
`--progress file` rewrites `<stage> <done> <total> <reused>` a few times a second while
cooking (import, meshes, textures, writing, done), for the editor and the launcher.

**Procedural rocks:** `--rocks` adds generated rock meshes for terrain scatter.
- **The set:** `rock_boulder_a`..`c`, `rock_slab_a`..`b`, `rock_spire_a`..`b`, `rock_arch`.
- **How they are made** (`tools/cook/build_rocks.cpp`):
  - each is a signed distance field (an ellipsoid, a rounded box, a tapering column, half a
    torus);
  - random planes cut it into facets, fractal noise roughens it, and strata band the slabs and
    spires;
  - surface nets turn it into triangles, with normals from the field's gradient.
- They then go through the cooker like any mesh: cluster LOD and colliders.
- It adds about 0.8 s to a cook, and `--verify` passes.

**From Blender:** `scripts/blend-to-fpak.ps1 scene.blend` exports a `.blend` with Blender
(headless; the file's own Python never runs) and cooks it into `samples/cooked/<name>.fpak`.
- Exported: visible objects with modifiers applied, and Geometry Nodes instances as shared
  meshes (a scatter of thousands of plants stays instanced). Also lights, and textures,
  including packed ones.
- Cut-out materials (a Transparent BSDF mixed in, or a Principled BSDF with Alpha connected)
  become alpha-masked.
- Not exported: procedural textures, displacement, and Geometry Nodes behaviour (the result is
  baked as it is in the file).
- The cooker applies `KHR_texture_transform` (Blender's Mapping node, texture tiling) to each
  material's UVs.
- The sun's strength carries over 1:1 (Blender's W/m² as the engine's sun irradiance; the
  engine's default sun is about 3.2).
- A linear Map Range between a roughness texture and the Principled BSDF is baked into a copy
  of the texture. glTF only has the raw texture: the generated open world's ground rendered at
  roughness 0.46 instead of about 0.9, and its slopes glared white in the sun.

Foliage (alpha-masked, double-sided materials: grass and leaf cards) is cooked differently:
- **Stochastic pruning instead of simplification** (Cook et al. 2007). Simplification measures
  geometric error, and collapsing a blade a centimetre wide into a line costs almost none. The
  simplified levels lost whole blades, and a grass field thinned to bare ground with distance.
  - Level L keeps a fixed 1/2^L of the mesh's separate pieces, chosen by a hash, so the same
    pieces at every level and in every cook. Each survivor is widened 2^L× across its width
    (across its length, in its surface), which keeps the area the foliage covers.
  - Pieces are disconnected, so dropping one opens no crack. The levels go down to 1/16.
  - The widened survivors are extra vertices. clusterlod builds the DAG with them through a
    simplifier override hook (`cmake/patches/meshoptimizer-clusterlod-override.patch`, applied
    to a copy of `clusterlod.h` in the build tree).
  - A level's error is how far widening moved the blade edges: half their width times 2^L − 1.
  - Meshes of fewer than 8 pieces (a single flower) keep full detail instead: pruning would
    drop the same parts from every instance.
  - On the Blender grass field (6,477 instances), the full view draws 2.8 M triangles instead
    of 7.2 M. The grass pass takes 0.73 ms instead of 1.37 ms at 1600×900, and looks the same
    (3% of pixels change by more than 8/255).
- **Coverage-preserving alpha mips** (all alpha-masked textures). Each mip's alpha is scaled
  so the share of texels passing the cutoff matches mip 0 (Castaño). Averaged mips thin thin
  cut-outs away.

The cooker builds a meshoptimizer cluster LOD DAG (128 vertices / 128 triangles per cluster)
and a BVH over cluster groups, plus Box3D collision blobs (see Physics), and appends the
built-in shapes the editor creates (Unity's sizes): cube, sphere, capsule, cylinder, plane
(10×10), quad and cone. Primitives with UVs but no tangents get MikkTSpace tangents (the convention glTF, Blender
and Substance bake normal maps against); they used to get +X. It quantizes positions to 16 bits per axis, and skips work
when the inputs' hash is unchanged. `ctest` in a build directory runs the cooker tests
(structural verification + byte-identical output across cooks).

Sample sources go in `samples/_src/` (git-ignored): Khronos glTF-Sample-Assets
(Sponza, DamagedHelmet) and Intel Sponza (`main_sponza`, CC-BY).

## UI and editor

The editor shell and in-game UI are HTML pages rendered by Chromium through cef-lite
(Tebarem/cef-lite, GPL-3.0). cef-lite composites them over the frame with SDL_GPU.

```powershell
./build/release/bin/falcon.exe --scene samples/cooked/sponza.fpak --editor
./build/release/bin/falcon.exe --scene samples/cooked/sponza.fpak --script build/release/bin/scripts/jit/Sandbox.dll --ui hud
```

- **Layer** (`engine/ui/ui_layer.*`):
  - Chromium runs off-screen in its own processes (`falcon.exe --type=...`). `libcef.dll` is
    delay-loaded from `cef/` (`runtime/cef_runtime.cpp`) when the UI is first created. Tracy
    starts in `runEngine` only, so the helpers don't each run a profiler.
  - Each frame, the page is drawn in one render pass on top of the swapchain image, after the
    scene blit.
  - Chromium renders the page on the GPU and reads it back; the engine uploads the parts that
    changed (every backend). cef-lite's D3D12 shared-texture import is off: under sustained
    repaints (a CSS animation at the windowless frame rate) it hung the device
    (`DXGI_ERROR_DEVICE_HUNG`), and it stalled the main thread on every page frame.
  - Pages talk to the engine with `window.falcon.postMessage(json)` and
    `window.falcon.onMessage`.
  - Messages handled by the layer itself:
    - `ui.layout {viewport, exclude}` gives the part of the window the game owns. The scene
      renders there at that size, and pointer input inside it goes to the game, except over
      `exclude` rectangles (page panels floating over the scene).
    - `ui.input {pointer, keyboard}` lets the page take all input (an open menu, a focused text
      field).
    - `ui.error` reports a page script error to the engine log.
  - **Live reload:** the layer watches its page's folder (`core/dir_watch.*`: Windows' change
    notifications, settled for 150 ms). When a file there changes (saved in an editor, copied over),
    the page reloads. That covers the editor (`falcon-ui/editor/`), the launcher, the HUD and a
    game's `UI/Game`. A reloaded editor page says `editor.ready` again and gets the whole state
    back: prefs, editor code's tools, the scene and settings, and the game's UI frame while playing.
  - Keys reach both the page and the game unless a text field has focus. A drag keeps going
    to whichever side it started on (the camera, a gizmo, a splitter) until the button is
    released.
- **Boot:**
  - `libcef.dll` is delay-loaded. Runs without `--editor` / `--ui` never load it.
  - The UI is created right after the first frame is submitted (~0.2 s for libcef and the
    browser process), so it never delays the first present.
  - The page appears ~0.4 s later. Before that, frames render without it.
- **Editor** (`--editor`): a Unity 6 style shell. The page is `ui/editor`; the engine side is
  `engine/editor/editor_bridge.*` (state and commands) and `engine/editor/scene_view.*` (the
  viewport).
  - **Layout:**
    - App bar: menus and Play / Pause / Step.
    - Docked panels:
      - Hierarchy on the left;
      - Scene / Game in the center;
      - Inspector / Lighting on the right;
      - Project / Console / Profiler at the bottom.
    - Resizable splitters, remembered per machine. Shift+Space (or the scene dock's maximize
      button) maximizes the panel under the mouse; the other panels are hidden meanwhile (the
      scene view is a transparent hole the engine draws through, and they showed through it).
    - **Help > Keyboard Shortcuts** (Ctrl+/) lists every binding.
    - **Dropdowns** open the page's own menu (the off-screen browser draws no native popups). It
      scrolls, opens upward when there is more room above, shows the current choice, and a list
      of more than 12 has a search field on top (Enter picks the first match).
  - **Keys:** Unity's.
    - Tools: Q view, W move, E rotate, R scale. X toggles Global / Local, Z Pivot / Center.
      Center is the default: glTF scenes often keep every node at the world origin with the
      vertices already placed, so Pivot would put every gizmo there.
    - F frames the selection. Esc cancels a drag.
    - Snapping while moving:
      - hold Ctrl for the grid (absolute: the pivot lands on grid lines; off in the snap menu
        snaps the move's length instead);
      - hold V to snap onto the nearest vertex, level mesh edge middle or mesh bounds corner
        on screen;
      - hold Ctrl+Shift to snap onto the surface under the mouse (level meshes exactly, else the
        terrain or the ground).
    - Selection: click, Shift+click adds, Ctrl+click toggles, and a drag on empty space boxes
      (Shift / Ctrl too). The Hierarchy takes Ctrl+click and Shift+click (a range). The gizmo
      moves every selected entity, but not one whose parent is selected too, and the drag is one
      undo step. Delete and Duplicate take all of them (the Delete key, the Edit menu, and the
      Hierarchy's right-click menu on one of them, which keeps the others selected).
    - Element modes of a selected level mesh (the scene view's toolbar, or 1 objects, 2
      vertices, 3 edges, 4 faces): clicks and boxes pick elements, and the gizmo moves, turns
      and scales them about their centre. Each drag is one undo step.
    - Edit: Ctrl+S, Ctrl+Z / Ctrl+Y, Ctrl+D duplicate, Delete, F2 rename.
    - Ctrl+P play, Ctrl+Shift+P pause, Ctrl+Alt+P step. Ctrl+F searches the Hierarchy.
    - Ctrl+1/2/3/4/5 open panels, Ctrl+Shift+N creates an empty.
    - The page owns the panels and Ctrl shortcuts; the engine owns the scene-view keys. A
      focused text field keeps the keyboard.
  - **Scene view:**
    - Camera:
      - right-drag to look, plus WASD/QE to fly (the wheel sets the speed);
      - Alt + left-drag orbits the pivot;
      - middle-drag pans;
      - the wheel or Alt + right-drag zooms;
      - framing glides.
    - Click to select, via a GPU pick: one compute thread reads the visibility buffer, read
      back with the async stats. Clicking empty space clears the selection.
    - Transform gizmos:
      - move: axis and plane handles, plus view-plane movement from the center;
      - rotate: axis rings (front half grabbable), plus a view ring;
      - scale: per axis, or uniform from the center.
      - Handles keep a constant size on screen. A drag edits live and lands as one undo step.
    - An orientation gizmo; clicking an axis looks along it.
    - An infinite, depth-tested ground grid that adapts to the camera height, with red / blue
      world axes.
    - Unity's orange outline around the selection's visible pixels.
    - The overlays are one full-screen pass plus one draw of CPU-built triangles, drawn into
      the viewport after the scene.
    - The Game tab hides them and offers Stats and Maximize On Play.
  - **Hierarchy:**
    - Virtualized, so thousands of instances scroll smoothly.
    - A tree: children sit indented under their parent, with arrows to fold them (left / right
      keys too). Drag a row onto another to make it its child, or onto the scene row to make it
      a root again; the world position stays.
    - Search (a flat list of matches), eye toggles for visibility (a hidden parent hides its
      children), inline rename (F2) and a context menu (Create Child, Frame, Rename,
      Duplicate, Hide, Delete).
    - Arrow keys move the selection; double-click frames.
    - Names come from the cooker's `Names` section (glTF node names; multi-material meshes add
      the material name).
  - **Inspector:**
    - Header: active (visible) toggle and name.
    - Transform: Position / Rotation (XYZ euler degrees) / Scale, relative to the parent for
      children. Drag a label to scrub (Shift x10, Alt x0.1). Fields take arithmetic (`2*3`;
      `+0.5` is relative). The ⋮ menu resets, copies and pastes.
    - Light: type, colour (linear RGB with a swatch), intensity (candela), range (0: no limit),
      spot and inner angles, cast shadows.
    - Camera: field of view, near clip, Main Camera.
    - Rigidbody: mass (kg), linear and angular damping, gravity scale, Is Kinematic.
    - Box / Sphere / Mesh Collider: center and size (a sphere's radius), friction, bounciness.
    - Every other flecs component on the entity is its own section. Sections hold only what
      can be edited. Mesh Instance is its header with the visibility checkbox, as Unity's
      renderers are, and its tooltip names the mesh; engine-internal fields (GPU slots, mesh
      indices) are not shown.
    - **Add Component** (the button under the components, as in Unity): a list of what the
      entity doesn't have yet, with a search on top (typing filters, Enter adds the first
      match). These go on any entity: a mesh, an empty, a cooked instance, water.
      - A Light (a point light, Unity's defaults) or a Camera (the main one when there is
        none). The light system and the main camera find them by query. A light or camera
        added this way shows its icon in the scene view, even on a mesh.
      - A Rigidbody (1 kg) and one collider: Box, Sphere (both fitted to the mesh's bounds) or
        Mesh (needs a mesh). They simulate under Play (see Physics).
      - An Audio Source (plays on start, 3D) or an Audio Listener (see Audio).
      - The edits file saves them on any entry (`"light"`, `"camera"`, `"body"`,
        `"collider"`, `"audioSource"`, `"audioListener"`, `"acoustics"`), and so do prefabs, so the game has
        them too. Undo takes one off again.
      - Remove Component (the ⋮ menu on a component's header) takes one off. A light's own
        Light, a camera's own camera and the components a prefab gives its instances stay
        (`editor.component.add` / `editor.component.remove`, `editor.body.set`,
        `editor.collider.set`).
    - Explanations are tooltips on the rows they explain.
    - Scrubbing previews live (`editor.transform.preview`) and commits one undo step on
      release.
  - **Lighting:**
    - Global illumination: on/off, bounce and sky intensity.
    - Exposure, manual exposure and the AgX look.
    - TAA, FXAA, MSAA, AO technique / radius / power, bloom.
    - Draw mode (every debug view), LOD error, cull size, culling switches, shadow cache, VSync.
    - Camera speed.
  - **Creating** (GameObject menu, the Hierarchy's + button and right-click, Unity's layout):
    - Create Empty (Ctrl+Shift+N); 3D Object ▸ Cube, Sphere, Capsule, Cylinder, Plane, Quad,
      Cone; Light ▸ Point Light, Spot Light; Camera. Roots appear at the camera's pivot, a row's
      Create Child under that row.
    - Shapes take slots from per-shape pools of GPU instance rows reserved at load (64 spare
      each in the editor). Empties, lights and cameras need no slot.
    - Lights and cameras have no mesh: the scene view draws an icon for them (and for empties),
      and a click on the icon selects it. A selected light shows its range sphere or cone, a
      selected camera its frustum.
  - **Lights** are entities (a `Light` component), including the scene's cooked ones:
    - **The sun is the Directional Light** in the Hierarchy: the scene's first cooked directional
      light, or a default one (at the scene's centre) when the pak has none.
      - Rotating it moves the sun: the shadow map rebuilds its light basis and re-renders every
        page, and the GI probes wake and relight.
      - Its colour and intensity are the sun's. Hiding or deleting it turns the sun off.
      - It saves with the scene edits, so the game sees it.
      - Light ▸ Directional Light adds one. Only the first (oldest) one lights the scene, since
        the renderer has one sun.
    - `LightSystem` gathers them every frame and uploads the light array when it changes.
    - Up to 32 cast shadows. A shadow slot stays with its light while it casts, and when a
      shadowed light moves, changes shape or loses its slot, the shadow map re-renders that
      slot's cached pages in place (`cs_invalidate_slots`).
    - Created lights cast shadows by default while slots are free. Cooked ones follow
      `--shadowed-lights`.
  - **Cameras** (a `CameraComponent`): the first one created is the main one, and a new one
    starts as the scene view's camera. Play, the Game tab and the game itself (with the saved
    edits) look through the main camera; the Scene tab keeps the fly camera.
  - **Editing:**
    - Ctrl+D duplicates an entity with everything below it, named Unity-style ("Cube 1 (1)").
      Cooked meshes have no spare instance slots, so they are left out of copies.
    - Rename, delete (with the children), hide, reparent. Undo/redo covers all of it.
    - Children keep a `LocalTransform` and a `Parent`; `Scene::propagate` derives their world
      transform every frame, parents first.
  - **Play / Pause / Step:**
    - The editor starts in edit mode: scripts, physics and animation hold still (characters
      keep their pose).
    - Pause holds play mode; Step runs one frame. Stop restores every entity's transform;
      bodies spawned during play remain.
  - **Window:** borderless, with the page's own title bar.
    - Drag the bar to move the window; double-click it or use the button to maximize. Windows
      snap works too.
    - Minimize / maximize / close buttons are on the right. The edges resize.
    - (`ui.chrome` / `ui.window` in `engine/ui/ui_layer.h`; an SDL hit-test callback does the
      dragging.)
  - **Preferences** persist in `%APPDATA%/Falcon/Editor/editor.json`
    (`engine/editor/editor_prefs.*`):
    - the Lighting panel's settings and VSync;
    - the tools and snaps;
    - the page layout (panel sizes, tabs, folded sections, Maximize On Play, Stats);
    - the window's position, size and maximized state;
    - per scene: the scene camera.
    - They are written at most once a second when something changes, and at exit.
    - `--no-prefs` runs without them (the tests do).
  - **Save** (Ctrl+S) writes `<scene>.edits.json` next to the `.fpak`.
    - Version 2: moved, hidden, renamed, deleted and reparented cooked instances and lights,
      and created entities (shapes, empties, lights, cameras) with uids that stay the same
      across saves. Children store their local transform. Version 1 files still load.
    - Every run applies it at load, editor or not (`--no-edits` skips it), before physics is
      built. So moved or deleted static meshes get matching colliders on the next load.
    - During an editing session, colliders stay where they were cooked.
  - **Console:**
    - Engine and script log lines (the periodic stats lines go to the Profiler instead), with
      level filters and counts, Collapse, search, and the full text of the selected line.
    - Page script errors are logged too (`ui.error`).
  - **Profiler:** per-frame CPU zones on every thread, per-pass GPU timings, counters and
    memory. See Profiler below.
  - **Project:** the files next to the scene.
  - Not yet: multi-select, adding components, multiple scenes, duplicating cooked meshes,
    colliders on created shapes, the cooked glTF hierarchy (the cooker flattens it).
- **In-game UI** (`--ui <dir>`; a bare name picks a bundle shipped in `bin/falcon-ui/`):
  - Scripts use `Ui.Send(json)` and `Ui.TryReceive(out json)`.
  - The Sandbox HUD (`ui/hud`) shows live counts four times a second. Tab opens a menu whose
    buttons send `hud.fountain` / `hud.kick` back to the game.
- **Cost:**
  - Pumping the browser and handling messages: ~0.05 ms per frame.
  - Uploading a changed page frame (the readback): ~1 ms. Keep pages static between
    updates. Unchanged text should not be re-set, because a repaint only happens when the page
    changes.
  - The D3D12 shared-texture import (off, above) waited on the GPU after copying each frame
    (Chromium may reuse its texture once the paint callback returns), which drained the queued
    frames: 4 ms on average (14 ms max) for the HUD, up to ~20 ms in the editor. Bringing it
    back needs cef-lite to keep the copy in flight until the texture is released (or a
    GPU-side fence with Chromium), and the hang above understood.
  - **Renderer crashes (fixed):** the page's renderer used to die (exit code 0xC0000409, the
    page stops painting and answering) whenever its JavaScript got busy for a few
    milliseconds.
    - Crash dumps showed fast-fail code 57 (`FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS`): a CET
      shadow-stack violation.
    - The renderer ran from CEF's `bootstrap.exe`, which is marked CET compatible. cef-lite
      runs Chromium without its sandbox, and the sandbox is what normally turns CET off for
      renderers.
    - With CET enforced, V8 is killed as soon as it swaps optimized code under a running
      function.
    - The build cleared that flag on the copied bootstrap. Renderers are now `falcon.exe`,
      which is not marked CET compatible; it must stay that way.
    - The older workarounds were blamed on specific triggers: clickable elements built as
      `<span role="button">` instead of runtime `<button>`s, tooltips on wrapping elements
      rather than `<label>`s, and scripted tests waiting 0.5 s after the page connects. Those
      triggers were most likely just moments when the page was busy. The workarounds stay until
      that is checked.
    - The UI layer delivers engine messages at most every 16 ms (batched as `ui.batch`), since
      each delivery makes Chromium begin a frame.
    - The editor test pings the page (`test.ping` and `editor.pong`), so a dead page fails the
      run.
