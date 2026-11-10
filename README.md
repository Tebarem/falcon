# Falcon Engine

GPU-driven engine on SDL3 + SDL_GPU (D3D12 on Windows, Vulkan on Windows and Linux, Metal on
macOS), Slang shaders, flecs, Box3D, cef-lite UI and C# scripting. Windows and Linux run the
engine, the editor and the scripts; macOS builds them all (Falcon.app, the shaders in Metal
Shading Language) and runs the engine on Metal.

Falcon is MIT-licensed (`LICENSE`); builds with the UI include cef-lite and are GPL-3.0 (see
[License](#license)).

## Build

Requirements: Visual Studio 2022 (C++), CMake 3.25+, Ninja, Windows SDK (for `dxcompiler.dll`),
the Slang binary release (`FALCON_SLANG_ROOT`, default `%USERPROFILE%/slang-2026.10.2-windows-x86_64`)
and the .NET 10 SDK (C# scripts; the NativeAOT compiler package is restored from NuGet).

```powershell
git submodule update --init --depth 1
./build.ps1 -Preset release     # or debug / profile (profile enables Tracy)
./build/release/bin/falcon.exe
```

On Linux and macOS (Apple silicon, macOS 14+): CMake 3.25+, Ninja, a C++20 compiler (gcc 13+,
clang 17+ or Xcode 16+), the .NET 10 SDK and, on Linux, SDL's X11/Wayland/audio development
packages (the list is in `.github/workflows/build.yml`). `build.sh` fetches Slang into
`~/slang-<version>-<os>-<arch>` and CEF (the editor's browser) into `external/cef-lite/vendor/cef`,
and builds the `linux` or `macos` preset into `build/<preset>/`. Steam Audio and the Steamworks SDK
come from `cmake -DDEP=steamaudio -P scripts/fetch.cmake` and
`cmake -DDEP=steamworks -DZIP=steamworks_sdk_165.zip -P scripts/fetch.cmake`. Not there yet: QUIC
(MsQuic) and AMD FSR / NVIDIA DLSS (Direct3D 12 only). On macOS Chromium's processes run from
`Falcon Helper.app` and its variants beside `falcon`, and the framework is in
`bin/cef/Chromium Embedded Framework.framework`.

```bash
git submodule update --init --depth 1
./build.sh
./build/linux/bin/falcon --help
```

GitHub Actions builds all three on every push to main (`.github/workflows/build.yml`) and runs
the tests that need no GPU (`scripts/ci_tests.py`); the macOS job also compiles every shader's
MSL with Apple's compiler. GPU tests run on real machines (`ci_tests.py --kind gpu --only <regex>`
picks a set; Mesa's lavapipe under Xvfb works on Linux). The Steamworks SDK needs a Steamworks
partner login, so it isn't in this repository: CI fetches it only with the `STEAMWORKS_TOKEN`
secret, and forks and pull requests build without Steam. A `v*` tag packages the three builds
into a draft GitHub Release.

Like Godot, the engine is one executable, `falcon.exe`: the launcher (no arguments), the game,
the editor (`--editor`) and the cooker (`falcon cook ...`); `falcon --help` lists the modes. SDL is linked in. A build's
`bin/` holds:

```
falcon.exe      the game, the editor, the cooker, and Chromium's helper processes
cef/            the CEF runtime (libcef.dll and Chromium's data; CEF can't be linked statically)
fsr/            AMD's FSR DLLs and license (when fetched; see below)
dlss/           the DLSS plugin, NVIDIA's DLSS DLL and license (when fetched; see below)
audio/          Valve's Steam Audio (phonon.dll) and its third-party licences (when fetched; see below)
falcon-ui/      the HTML UI bundles (launcher, editor, hud)
shaders/        cef-lite's compositing shaders
scripts/        the C# scripts
```

Build-time tools (`falcon_shaderc`, `falcon_schemagen`) and the test tools `falcon_imgdiff` and
`falcon_wavdiff` go to `tools/`, not `bin/`.

The first configure copies SDL into the build tree and applies four patches there; the
submodule itself stays pristine:
- cef-lite's external-texture patch;
- `cmake/patches/sdl3-falcon-timestamps.patch`: GPU timestamp queries for D3D12 and Vulkan
  (`SDL3/SDL_falcon_timestamps.h`), which the profiler uses. This is a local patch, not meant
  for upstream SDL;
- `cmake/patches/sdl3-falcon-static.patch`: the two extensions' API macros drop
  `dllimport` / `dllexport`, since SDL is a static library here;
- `cmake/patches/sdl3-falcon-native.patch`: native Direct3D 12 access (device, command list,
  textures; `SDL3/SDL_falcon_native.h`) for AMD FSR. Also local only.

AMD FSR needs a few files from the AMD FSR SDK v2.3.0 (headers and the signed
`amd_fidelityfx_loader_dx12.dll` / `amd_fidelityfx_upscaler_dx12.dll`, about 29 MB):
`powershell -File scripts/fetch-fsr.ps1` puts them in `external/fidelityfx/` (not committed).
The next configure turns `FALCON_FSR` on and stages the DLLs, with AMD's `license.md` (the
DLLs are under its MIT-style terms), in `bin/fsr/`. They load at runtime, so falcon.exe runs
without them and upscales with TAAU instead.

NVIDIA DLSS needs a few files from NVIDIA's DLSS SDK v310.9.16 on GitHub (the NGX headers, the
static `nvsdk_ngx_s.lib` and `nvsdk_ngx_s_dbg.lib`, and `nvngx_dlss.dll`, about 60 MB):
`powershell -File scripts/fetch-dlss.ps1` puts them in `external/dlss/` (not committed: they are
under NVIDIA's license, `external/dlss/LICENSE.txt`). The next configure turns `FALCON_DLSS` on
and builds a plugin, `bin/dlss/falcon_dlss.dll` (`engine/render/dlss_plugin/`), the only thing
that links NVIDIA's library, next to `nvngx_dlss.dll` and NVIDIA's `LICENSE.txt`. falcon.exe has
no NVIDIA code: it loads the plugin when DLSS is first picked, and upscales with TAAU without it.

Spatial audio uses Valve's Steam Audio SDK v4.8.1 (Apache 2.0). `powershell -File
scripts/fetch-steamaudio.ps1` downloads `steamaudio_4.8.1.zip` (181 MB, every platform; or
`-Zip <path>` to use one already downloaded). It checks the zip's SHA-256 and keeps only the
headers, the Windows x64 `phonon.dll` (53 MB) with its import library, and `THIRDPARTY.md`, in
`external/steamaudio/` (not committed). The next configure turns `FALCON_STEAM_AUDIO` on and
stages the DLL and `THIRDPARTY.md` in `bin/audio/`. `phonon.dll` is delay-loaded when audio
starts, so falcon.exe runs without it and pans sounds instead of using the HRTF.

The UI (editor and in-game HTML) needs the CEF binary distribution in
`external/cef-lite/vendor/cef` (`powershell -File external/cef-lite/scripts/fetch-cef.ps1`).
Without it, or with `-DFALCON_UI=OFF`, the engine builds without UI. **cef-lite is GPL-3.0,
so a build with the UI is GPL-licensed.** `FALCON_UI=OFF` gives a build without GPL code.
Our changes to cef-lite live in `cmake/patches/` and are applied to the submodule's working
tree at configure time (once):
- `cef-lite-click-count` passes mouse click counts to Chromium, so double-click and word/line
  selection work.
- `cef-lite-single-exe` makes the host executable CEF's helper process as well
  (`cef_lite_run_subprocess`), in place of CEF's `bootstrap.exe` and a subprocess DLL. Chromium
  relaunches `falcon.exe --type=...`, and `main` hands those runs to CEF before anything else
  starts. The patch also finds Chromium's resources next to `libcef.dll` (in `cef/`).

The level tools use four more submodules, built static with the engine (`cmake/FalconDeps.cmake`):
- Manifold 3.5.4 (Apache-2.0): booleans for cut volumes. Built serial, without its 2D
  CrossSection (Clipper2), its downloads or its Tracy hook.
- MikkTSpace (zlib): tangents (`engine/core/mesh_tangents.h`).
- xatlas (MIT): unwrapping for paint-mask UVs.
- Recast/Detour 1.6.0 (zlib): navigation meshes. Its own CMake file predates CMake 3.5, so the
  libraries are built from their sources.

`falcon_level_test` (in `tools/`) checks them without the engine: ctest runs `level_tangents`,
`level_boolean` (a door and a window cut from a wall, exact volume, identical reruns),
`level_unwrap` and `level_navmesh` (a Detour path around a wall).

`falcon.exe` must not be marked CET compatible (never link it `/CETCOMPAT`): see the
renderer-crash note under UI and editor.

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
  [Rigid node animation](#rigid-node-animation)).

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

## Renderer (M2: GPU-driven visibility buffer)

Each frame runs entirely on the GPU:
1. Instance frustum and contribution culling, which emits 64-cluster chunks. Contribution
   culling skips an instance whose bounding sphere projects to a radius of less than 4 pixels
   (`--cull-pixels`, the Rendering panel's *Cull size*; 0 = off). Glowing meshes (an emissive
   material: `MeshGpu::posOffset.w`, set at load) and instances drawn with another material
   are spared: a light strip a few pixels wide, far off, still shows and blooms (culled, distant
   lights' glow stopped at a distance; Zero-Day: about 100 more instances, no measurable cost).
   Nor is anything within the full-detail distance (`--detail-distance`, a project setting;
   0 outside a project), where the LOD cut also takes the full-detail clusters (a group nearer
   than it counts as infinitely wrong when simplified; a parent's sphere holds its children's, so
   the cut stays crack-free) and the terrain splits to its finest patches.
   In the generated open world
   (183k instances), distant grass patches and flowers otherwise overflowed the 131,072
   visible-cluster budget, and the dropped clusters were foliage: trees lost their leaves.
2. Per cluster: the DAG LOD cut (projected error ≤ threshold < parent error), frustum culling,
   backface cones, and the previous frame's HiZ.
3. Visible clusters expand into the index buffer, drawn with **one indexed indirect draw** into
   an R32_UINT visibility buffer.
   - Only real triangles go in. Each cluster takes the next run of its draw's range (one
     atomic) and records where the run starts. The raster turns `SV_PrimitiveID` back into the
     cluster's own triangle from that, so the visibility buffer holds the same ids as before.
   - Before this, every cluster filled 128 slots, padded with degenerate triangles. That was
     40% more primitives in Zero-Day (clusters average about 90 triangles), and the raster's cost
     is mostly per primitive.
   - **Cost** (Zero-Day's game view, 1.2M triangles): the pass-1 raster went from 0.40 to
     0.35 ms at 1600×900, and from 0.47 to 0.41 ms at 2560×1440 with FSR quality.
   - **Back faces of single-sided materials** go in as degenerate triangles. The expand tests each
     triangle against the camera, in world space, using the cones' winding, flipped for mirrored
     instances. The raster itself culls nothing, so double-sided materials still draw both sides.
     - Before this, back faces in clusters the cone test kept were drawn. Where a mesh has a
       back-to-back copy (Sponza's curtains), the back face tied in depth with the front face,
       and the draw order picked which one showed. That order comes from the culling's atomics,
       so edge pixels changed from run to run, and `render_vsm_local_cache` failed most runs.
     - Sponza's pass-1 raster went from 0.080 to 0.063 ms. With `--no-cone` every triangle is
       drawn, as before.
   - **Tried and dropped:**
     - Culling triangles in the expand (off screen, or covering no pixel centre: 53% of them)
       cut the raster by a quarter. Transforming every vertex there cost more than that saved.
     - A compute raster for small clusters (Nanite-style 64-bit atomic max of depth and id, then
       a depth-tested merge; branch `prototype/compute-raster`) was slower at every size limit.
       At 16 px it took 47% of the triangles, yet the hardware raster only dropped from 0.33 to
       0.26 ms. The compute raster cost 0.13 ms, and its clear and two merges about 0.1 ms on
       their own (Zero-Day, RX 9060 XT, 1600×900).
4. HiZ is rebuilt from that depth, the occluded clusters are re-tested, and the survivors are
   drawn with a **second indirect draw**.
5. HiZ is built again for the next frame, then a compute resolve shades each pixel by
   intersecting the camera ray with its triangle.

The CPU only pushes uniforms. Counters come back asynchronously for stats and are never waited on.

## Virtual texturing (M3)

Every material texture lives in one virtual address space. This replaces bindless, which
SDL_GPU doesn't have.
- **Cooking:** the cooker resizes each material's albedo, normal and
  occlusion/roughness/metallic maps to a power of two (`--vt-max-size`, default 4096) and
  builds a mip chain. It cuts the mips into 128×128 pages with a 4-texel wrapped border and
  encodes them as BC7 (albedo sRGB, ORM) and BC5 (normal) into `<scene>.fvt`, at fixed offsets.
- **Cache:** at runtime, one physical cache of `--vt-pages` slots (default 1024, about 54 MB)
  holds whatever the camera needs, as three texture arrays indexed by a page table.
- **Resolve:** the resolve pass picks the mip from analytic UV derivatives, samples
  trilinearly with fallback to coarser resident pages, and reports the pages it wanted
  (1 pixel in 16 per frame, deduplicated).
- **Streaming:** reports come back without stalling. Missing pages are read with async I/O,
  coarse mips first, uploaded (at most 48 per frame), and evicted least-recently-used.
  Under cache pressure a global mip bias rises. Every material's last mip is pinned.
- **Alpha masks:** alpha-masked materials go through a second, small visibility pipeline
  with an alpha test, so opaque geometry keeps early depth. Masked materials whose texture
  never drops below the cutoff are cooked as opaque.

## Lighting and shadows (M4)

- **Many lights:** binned clustered shading. Each frame the CPU sorts lights by view depth and
  builds 1024 z-bins, each holding a contiguous range of sorted light indices. A compute pass
  rasterizes every light's screen bounds into 16×16-pixel tile bitmasks, and the resolve pass
  visits only lights that are in both its tile and its z-bin. Supports up to 16,384 lights.
  `KHR_lights_punctual` lights are cooked into the `.fpak`; `--test-lights N` adds random
  point lights.
- **Range:** a light's light stops at its range (inverse-square falloff, windowed to zero
  there). Range 0, the default for new lights, means no limit: the range is taken where the
  falloff drops under 0.001 (intensity 10: 100 units), past anything visible. Nothing culls
  lights by distance from the camera. (New lights had range 10, and range 0 stopped at a
  hundredth instead: lights stopped lighting things still in view.) Local-light shadows keep
  their near plane within 0.1 units however far the light reaches.
- **Sun shadows:** a directional virtual shadow map, entirely on the GPU and inside the frame.
  - **Clipmap:** 12 levels around the camera, each a scrolling 32×32 window of 128² pages.
    The coarsest spans twice the scene. The windows follow the camera clamped to the scene's
    light-space footprint, so a camera outside the scene (an editor overview) still gets
    shadows on its far side.
  - **Page selection:** pages are requested from the depth buffer at half resolution and
    allocated in a 4096² D32 atlas of 1,024 physical pages.
  - **Rendering:** only new pages are rendered, at most 256 per frame. Each level uses a DAG
    cut sized to its own shadow texels, and casters are clipped to their page with
    `SV_ClipDistance`. Pages stay cached while the scene and sun are static.
    - Alpha-masked casters (foliage cards) alpha-test in the shadow pass, so they cast their
      cut-out, not a quad. The test reads each material's last mip, which is always resident,
      so a cached page doesn't depend on how far texture streaming had got.
    - Foliage (alpha-masked and double-sided) casts a partial shadow: its cards write depth
      into half the atlas texels (a fixed 4×4 ordered dither), which the receivers' filter
      averages. Blades are thinner than a texel, and light passes between and through them.
      Covering every texel they touched, a dense field of grass shadowed itself and the ground
      completely. The pattern is the same for every card, so layers of grass don't stack past
      it.
    - Foliage receivers take the sun on whichever side faces it, look their shadow up a few
      texels toward the sun (no self-shadowing through the filter), and pass half the light
      through to their far side.
    - **Moving casters** (anything whose instance row changed in the last 60 frames: a
      played model's parts, physics bodies, a dragged object) are flagged
      (`kInstanceDynamic`). From the first one on, each page also keeps a static layer: the
      other casters, cached in a second 8192² atlas (256 MB, made only then) at the same
      physical page.
      - A page that only moving casters made stale is restored from its static layer (one
        quad per page, its depth copied texel for texel), and only the moving casters are
        drawn over it. Before, it re-rendered every caster under it, the room around a
        moving part included.
      - A caster's first move and its settling change the static layer, so those invalidate
        whole pages (both layers), as do new pages, terrain, scatter and lights.
      - Cached and fresh pages still agree: the layers are combined by the same depth test.
    - **Triangles drawn:** each (cluster, page) record draws its cluster into one page. The
      expand pass (compute) leaves out triangles outside the page or between its texel
      centres (with a margin past the raster's snapping, so nothing drawn changes: about 70%
      of a played model's). Then:
      - Triangles inside their page (within a quarter texel) are drawn with no clipping.
      - Small ones crossing its edge (up to 2 pages across) are drawn whole, their fragments
        outside the page discarded.
      - Only the large ones are clipped by clip distances. Hardware clipping was most of the
        shadows' cost: about 50k triangles crossing page edges took a millisecond.
    - When a frame's (cluster, page) records overflow (dense foliage under a coarse level),
      the pages that lost records are dropped. Fewer new pages then start per frame until they
      fit; otherwise the same set would overflow again every frame and never become
      resident.
  - **Eviction:** pages unused for a frame stay valid until an allocation takes them. The free
    list is built in physical-page order with a prefix sum, not by atomic append, so which
    cached pages get evicted doesn't depend on GPU thread scheduling. Shading falls back on
    cached pages where a pixel's own page wasn't requested, so a scheduling-dependent eviction
    made images differ from run to run.
  - **Sampling:** a 5×5-texel tent filter (9 bilinear comparison taps). Each tap reads its own
    page, so the filter crosses page borders. Sampling falls back to coarser levels where a page
    is missing.
  - **Level blending:** over the last 30% of each level's distance range, and near the edge of
    its window, the next level is cross-faded in. The mark pass also requests that level's
    pages. Without it, level switches show as lines that slide along with the camera, with
    shadow edges changing sharpness across them.
- **Local-light shadows:** up to 32 shadow-casting lights (`--shadowed-lights N`, default 8;
  scene spot lights first, then the brightest). Each gets 6 cube faces (point) or 1 face
  (spot): a 4096² perspective virtual texture with 6 mips of 128² pages.
  - They share the sun's page table, the 8192² physical atlas (4,096 pages, 256 MB) and the
    whole request → allocate → cull → render pipeline.
  - Casters are culled per face mip with a DAG cut sized to that mip's texels at the
    cluster's distance.
  - Faces are rasterized in homogeneous coordinates, so the page clip planes and the mapping
    into the atlas stay linear.
  - Shadowed lights are shaded in their own loop, with the same tent filter and a cross-fade
    between face mips.
- **Time budget** (`--shadow-budget MS`, default 2; Lighting panel's Shadow budget; 0 = none):
  a moving light or sun re-renders all its pages, so fast-moving lights used to cost 2+ ms a
  frame in Sponza. The cap works like this:
  - The Shadows section is timed every frame, even without `--profile`. Its time and that
    frame's new-page count keep running estimates of a page's cost and the fixed cost, and
    from them a frame's page budget.
  - Each item that moved, a shadowed light or the sun, costs its requested pages. Items are
    granted longest-waiting first while they fit. One bigger than the whole budget goes alone
    once it has waited long enough to average within it.
  - A light that waits keeps the shadow it last rendered. Each slot holds a snapshot of its
    light as rendered (`slotLight`, past the live lights in the lights buffer), and shadow
    lookups use that, while shading uses the live light. Its shadow lags a few frames but
    always matches its pages. A slot taken by a new light always renders at once.
  - A turned sun re-renders only its own pages, not the local lights'.
  - Measured in Sponza at 1600×900 (`--moving-lights 1`: the sun turns 30°/s and each light
    circles 1 m once a second). One point light plus the sun takes 2.0 ms a frame uncapped,
    1.55 ms at 2 ms, 1.0 ms at 1 ms, and 0.75 ms at 0.5 ms. That last is about the floor: any
    frame with a page to render culls every cluster in the scene (0.6–1 ms), whatever the
    page count. The worst single frame is about one item's full cost (2–2.7 ms).
  - The once-a-second `vsm:` log line ends with the mean and worst Shadows time and the budget.
- All scene sections live in one GPU buffer with an offset header (`scene_types.slang`). That
  keeps every pass within SDL_GPU's 8-storage-buffer limit.

## Indirect lighting (probe GI)

The ambient term comes from irradiance probes round the camera, lit by rays traced through the
scene's BVH (DDGI, Majercik et al. 2019; `engine/render/probe_gi.cpp`, `probe_*.slang`; the BVH
is the next section's). Enclosed spaces go dark, and lit surfaces bounce coloured light onto their
surroundings, update after update for further bounces. `--no-gi` falls back to the flat ambient.
It replaced a voxel cone tracer: its cones averaged blocks of space, which straddled level walls
a quarter of a metre thick, and every fix for the leaks was a cap that cost time. A probe's ray
hits a real triangle, so a wall blocks it exactly.
- **Grids:** three of 32×16×32 probes, centred on the camera, the spacing doubling from the first
  (1 m by default: 32×16×32 m, then 64×32×64, then 128×64×128). A grid moves in whole probes,
  each probe stored at its world index modulo the grid, so a move renews only the probes it
  brings in; they are fresh (not sampled) until their first update. Probes sit half a spacing
  off the spacing's whole multiples (at 0.5, 1.5, 2.5 m with the default spacing). Level geometry
  snaps to whole metres (walls centred on them, floors at 0, 3 m storeys), and probes on them sat
  inside walls and on wall tops: one on a closed room's wall top, pushed out above it, saw the
  wall's edge near and the ground far past it in one direction, passed the visibility test, and
  lit a band under the ceiling, worst in the corners, with the sky. Half a spacing off, a 0.25 m
  wall on a whole metre has 0.375 m clear on either side.
- **Atlases,** one texture each with the grids stacked down it:
  - irradiance: rgba16f, 8×8 texels a probe (6×6 octahedral directions and a border copied from
    the opposite edges, so bilinear filtering wraps over the seams): the cosine-weighted mean
    light arriving round each direction;
  - distance: rg16f, 16×16 a probe: the mean distance to the nearest surface round each direction
    and its square, for the visibility test below;
  - data: a texel a probe, its offset from its grid position and its state (fresh, active,
    inactive).
- **Updates** (`probe_trace.slang`, a group of 64 rays a probe): 16 fixed rays and 48 on a
  spherical Fibonacci set turned by a hash of the probe's age, so its light depends on what it
  saw, not on the frames it was updated in (captures match from run to run and across backends).
  - **A hit** is lit by the sun (a shadow ray toward it), the point and spot lights round it
    (below), the probes' light there from earlier updates (light bouncing on), and its glow, times
    the bounce intensity. Its colour comes from the material's coarsest mip (always resident),
    alpha-masked holes from its alpha, so the plants let light through their gaps.
  - **A miss** sees the sky times the sky intensity; an open hemisphere gets about a fifth of the
    sun's irradiance.
  - **A back face** means the probe is in a wall: no light, and the distance kept short and
    negative, which the visibility test sees as near and relocation as a reason to move.
- **Blend** (`probe_blend.slang`): a probe's light is the mean of its updates since it was fresh
  or since a change near it, at most 0.985 of it kept once it has many; the borders are then
  copied. Only the 48 turned rays count: the fixed ones look the same way every update, and a
  probe whose fixed ray happened to hit something bright (a doorway, a lamp-lit patch) kept that
  in its light for good, a blotch on the walls round it.
- **Relocation and classification** (`probe_relocate.slang`, from the fixed rays):
  - More than a quarter of back faces: inside something, it moves out through the nearest back
    face, or, that too far, away from it (a probe on a slab's underside sees the slab's top from
    within); it is traced fully again before it is sampled. One that can do neither is inactive.
  - A front face nearer than 0.2 spacings: it moves away from it; else back toward its grid
    position. Never more than 0.45 spacings from it.
  - No surface within its cells (or as far past them as sampling moves a point: 1.3 spacings):
    inactive, not sampled, traced lightly until one comes near. (Exactly within its cells, a
    ceiling exactly a spacing above a row of probes missed by float error, and the row in the
    middle of a room went dark.)
- **Scheduling:** a budget of rays a frame (256k by default: 4096 probe updates). Fresh probes
  first, then those round a change (an edit, a drag, a live mesh or the terrain written, a light
  changed: 8 updates in a row, their memory started again), then each grid in turn, the finest
  most often. After 64 updates (about 3,000 rays: fewer left blotches in rooms lit through a
  door or by a lamp) a probe sleeps until something wakes it: a change, something moving near
  it, the sun moving. A still scene's light then stops changing, and comes out the same however
  many frames it took; captures and `test.wait` with `"gi": true` wait for it. A new grid (the
  first frame, another spacing) is lit at once: 24 rounds over every probe in that frame (about
  250 ms in Sponza; all 64 took 565), the rest of its updates in the frames after (Sponza's sleep
  within two seconds).
- **Point and spot lights** (`render/probe_lights.cpp`, `probe_lights.slang`): a world grid of
  32×16×32 cells round the camera, twice the first grid's spacing (64×32×64 m), each listing up
  to 32 of the lights whose range reaches into it. Built on the CPU when a light changes or the
  camera moves a cell (the scene's, debug and test lights; not particles' short-lived ones). A
  hit sums its cell's lights unshadowed, picks one in proportion and sends a shadow ray toward
  it: one shadow ray a hit however many lights, the noise averaged over a probe's rays and
  updates. Test `editor_level_closed_room_lamp`.
- **Sampling** (`probe_common.slang`, by the resolve per pixel with the normal map, transparent
  surfaces, water, and the probe rays' own hits): the point is moved off the surface (0.2
  spacings along the normal, 0.1 toward the eye), then the 8 probes round it are weighted
  trilinearly, by whether they are in front of the surface (smoothly, never quite nothing), and
  by a Chebyshev test of whether each saw a surface nearer than the point in that direction.
  Weak weights are crushed (what a wall hides mostly goes). The finest grid that holds the point
  is used, the next taking over near its edges; past them all, the sky. GTAO and the material AO
  still multiply the result.
- **Settings:** the Lighting panel's Global Illumination group: on or off (`gi`), bounce and sky
  intensity (`giIntensity`, `skyIntensity`), the finest grid's spacing (`giProbeSpacing`,
  0.5–2 m: closer probes catch smaller rooms for the same cost but reach less far) and the rays a
  frame (`giProbeRays`, 64k–512k). Draw modes: *Indirect Light* (`--debug-view 12`, the GI term
  alone) and *Probes* (`--debug-view 15`: the finest grid as spheres showing their light, inactive
  ones grey, fresh ones dark red).
- **Cost** (Sponza at 1600×900, RX 9060 XT): updates 0.5 ms a frame while the camera moves (none
  once everything sleeps), 1.3 ms with 1024 test lights packed into it; sampling in the resolve
  about 0.1 ms. 80 MB of atlases and ray buffers. The voxel cone trace it replaced cost 2.5 ms at
  half resolution.
- **Limits:**
  - A probe's light is a blur over a spacing: detail below it comes from AO, light inside a
    corner within a spacing of a thin ceiling can come out a little soft.
  - Skinned characters are not in the BVH: they receive GI but don't block it.
  - Lights past the light grid (beyond about 32 m) don't reach the probes' hits there.

## Ray tracing (compute-shader BVH)

SDL_GPU gives no hardware ray tracing, so the engine traces rays through a BVH of its own in
compute shaders (`engine/render/ray_tracing/`, `ray_trace.slang`). Probe GI is built on it.
- **Bottom level, one per mesh:** a binned surface-area-heuristic BVH over the mesh's finest
  triangles (depth 0 of its cluster DAG), built on the CPU.
  - Cooked meshes are built at load on the job threads (Sponza's 118 meshes, 407 000
    triangles: about 50 ms, 37 MB).
  - Live level meshes are built when they are written, and terrain patches when they are
    rewritten.
  - Alpha-blended clusters are left out. Skinned characters are too: they receive light but
    don't block it. Instances a vertex function deforms are traced as their still mesh (as
    physics collides with it); left out, a sphere with a vertex function lost its shadow on the
    GI until it went past its vertexDistance (`editor_vertex_far`).
- **Top level, over the instances:** built on the CPU from the renderer's instance rows. When
  only transforms change (an editor drag, physics, scripts), it is refit instead, and rebuilt
  once a refit tree's cost passes 1.5× its build's.
  - A large instance can be opened into several top-level leaves, each a subtree of its BLAS
    (partial re-braiding), up to twice the instance count.
- **Layout:** one storage buffer holds both levels. Nodes are 64 bytes, two-wide: they hold
  both children's boxes, so each traversal step tests two boxes. Triangles are 40 bytes: three
  mesh-space positions plus the cluster and triangle they came from, from which a hit fetches
  its UVs, normal and material.
- **Traversal:**
  - Each thread's stack lives in groupshared memory, interleaved across the group's threads.
    Callers run groups of 64.
  - Triangle tests are Möller-Trumbore, edges inclusive, so a ray down a shared edge always hits.
  - Alpha-masked triangles are tested against a coarse mip of their colour's alpha.
- **Check:** the *Ray Traced* draw mode (`--debug-view 14`) sends a camera ray per pixel and
  colours each hit as *Triangles* (view 4) does. With the finest LOD rasterized
  (`--lod-pixels 0`) the two agree to about 350 pixels in Sponza (test
  `render_ray_traced_view`), and D3D12 and Vulkan agree to about 30 pixels.
  `falcon_bvh_test` checks the traversal's CPU copy against brute force: random soups,
  thousands of stacked triangles, refits, and a grid's shared edges.
- **Cost:** Sponza's 1.44 million camera rays at 1600×900 take 2.9 ms on an RX 9060 XT, about
  490 million rays a second.
## Reflections

Shiny surfaces reflect the scene (`engine/shaders/reflections.slang`), in a pass after the
resolve:

- **The resolve** writes the lit image without them, and for each pixel the specular weight
  (the split-sum BRDF, Karis's analytic fit, times material and screen-space AO), the roughness,
  and the shading and geometric normals (octahedral).
- **The reflection source** (`reflection_source.slang`): that image copied with mips 0–6 in one
  dispatch: a group takes a 64×64 tile, each thread its 2×2 pixels, and the group builds each
  level from the one below in shared memory (0.09 ms; a dispatch per level, each texel read
  from the image, took 2.3). NaN becomes 0 and overflow is clamped on the way. A coarse texel covers up to 64×64 pixels, and
  one bad value in it turned every reflection reading it black: patches for a frame or two as
  the camera moved. SDL's mip generation left such texels in coarse levels on some frames.
- **The reflection trace** (`cs_trace`): the screen-space part below, at half resolution, one
  ray per 2×2 pixels (its top left pixel's): uv, mip and trust per hit. It was most of the
  reflections' cost (0.6 of 0.85 ms at 1600×900); now 0.25.
- **The reflection pass** (`cs_reflect`), per pixel: the screen-space reflection put together
  from the four hits around it, each read where it hit and weighted bilinearly and by how well
  the surface its ray left from matches this pixel's (distance from its plane, normal), so
  nothing bleeds across an object's edge; then:
  - **Screen space** (roughness below 0.55): the reflected ray is walked across the screen
    (McGuire and Mara's DDA: its depth is linear in 1/z along the screen), a pixel per step, or
    a stride of them for long rays (32 steps at most).
    - A hit: a pixel whose depth lies within the ray's depth range there (or a thickness behind
      it), or the ray gone from in front of the depth buffer to behind it since the last step.
      The step is then refined (5 halvings) to where it went behind; if that is far behind what
      it found, it passed behind something nearer (a depth jump) and walks on.
    - Steps in world units (the trace before) crossed a surface at a different step in each
      pixel. Rows of a glossy ceiling hit and missed in turn: stripes like z-fighting, crawling
      as the camera moved (with or without upscaling). Counting hits that passed far behind a
      surface (a thickness test alone) smeared what was in front, in streaks.
    - The image is read at the hit from the mip its cone's footprint asks for (rough surfaces,
      far hits), or the one its spacing does: how far apart the neighbouring pixels' hits land
      (the nearer neighbour per axis, so an object's edge doesn't count). Far detail squeezed
      into a few rows (a floor's grilles in the ceiling), read a texel per pixel, aliased.
    - It fades near the screen's edges, at the roughness limit, where the ray only grazed
      behind a surface, and on strongly normal-mapped pixels (they would sparkle as the jitter
      moves).
  - **Elsewhere** (off screen, behind things; `reflection_far.slang`, at the trace's resolution
    and put together from the four around each pixel the same way): the reflection traced
    through the scene's BVH (aimed by the surface's own normal, the normal map only on glossy
    ones) and lit where it hits as the probe GI's rays are: the sun with a shadow ray, the probes'
    light there, its glow. Past 32 m, the probes' light there alone; a miss, the sky the diffuse
    light has (the background's is 3 to 4 times brighter, and reflected it put a sheen on
    everything). Rough surfaces (from 0.3, all of it at 0.6) take the probes' light round the
    reflection instead, a blur, and so do faint ones (the far reflection showing below 12% of the
    pixel, under half that wholly), most of a frame's pixels. Without the BVH, the probes; without
    GI, the flat ambient.
  - The reflection, times the weight, is added to the image.
  - **For FSR:** how much of each pixel is a screen-space reflection goes to FSR as its
    transparency-and-composition mask: the reflected content moves unlike the surface's motion
    vectors, so FSR keeps less history there instead of smearing it.
- Only this frame is read: no history, so no ghosting, and the same frame from the same scene
  however it got there (the cache, backend and drag comparisons still hold).
- **Settings:** the Lighting panel's Reflections section (`--no-reflections`: the specular sees
  the ambient, as before); the Reflections debug view shows them alone. Saved with the editor's
  prefs (`setSetting` keys in brackets):
  - Reflections on or off (`reflections`), and Intensity (`reflectIntensity`, 0–4, default 1),
    which multiplies what they add.
  - Screen space (`reflectScreenSpace`): off leaves the trace through the BVH only.
  - Max roughness (`reflectRoughness`, default 0.55): rougher surfaces skip the screen-space
    trace.
  - Thickness (`reflectThickness`, 0.1–8, default 1): times the default thickness above. Higher
    fills gaps behind thin things; lower avoids smears.
  - Quality (`reflectSteps`, 8–128, default 32): steps per ray.
  - Half resolution (`reflectHalfRes`, default on): off traces a ray per pixel, sharper, and the
    trace costs about twice as much (Zero-Day's editor view at 2560×1440: 0.14 → 0.25 ms). The
    hits texture is full size either way; half resolution uses its top left quarter.
- **Cost:** about 0.9 ms at 1600×900 in Sponza (source 0.08, trace 0.06, the trace through
  the BVH 0.65, the pass putting them together 0.12); traced at full resolution, the BVH alone
  took 3.2 ms.
- **Limits:** a ray through the BVH sees the finest level of detail, but lit only by the sun and
  the probes (local lights and shadows from them are the screen-space trace's); screen-space
  reflections can't show what is off screen or hidden. `render_reflections_differ` checks the
  frame changes with them.

## Post-processing (M5)

The resolve pass writes linear HDR (RGBA16F). Everything after it is compute, with no CPU
readback (`engine/render/post_process.cpp`):
- **Ambient occlusion:** GTAO (2 slices × 6 steps, cosine-weighted horizons) or SSAO
  (12 hemisphere samples), from depth alone, before the resolve. It multiplies the ambient
  term.
  - By default it runs at half resolution: one pixel of each 2×2 block, storing AO and view
    depth.
  - The resolve upsamples it the same way as the GI (see Indirect lighting).
  - `--full-res-ambient` or the Lighting panel's *AO half res* off runs it per pixel.
  - With TAA, the noise rotates every frame and the history integrates it.
  - Without TAA (the default), the noise pattern is fixed, so it never flickers, and a
    depth-aware 4×4 blur (`ao_blur.slang`) denoises it.
- **TAA** (off by default; `--taa` or the Lighting panel turns it on): the visibility raster is jittered with an 8-sample Halton(2,3) sequence. Culling
  stays unjittered, with a one-pixel HiZ margin.
  - Reprojection uses the closest depth in the 3×3 neighbourhood. History is read with a
    5-tap Catmull-Rom filter.
  - History is clipped to the neighbourhood's YCoCg variance (1.25σ), and samples are
    weighted by luminance (Karis) to stop fireflies from flickering.
  - The history is kept unjittered: motion vectors are measured from where each surface is
    without the jitter, and the current frame is reconstructed at the pixel centre (a
    Gaussian of each 3×3 neighbour's sample distance). So a still camera gives the same image
    at every jitter phase, and nothing wobbles.
- **Upscaling** (off by default; `--upscaler taau|fsr|dlss` and `--upscale-quality`, or the Lighting
  panel's *Upscaler* and *Quality*): the scene renders below the display resolution and an
  upscaler brings it to the display.
  - Presets use AMD's ratios: Native AA 1.0, Quality 1/1.5, Balanced 1/1.7, Performance 1/2
    and Ultra Performance 1/3 of the display size per axis. `--render-scale S` sets any scale.
  - Render-sized: the visibility buffer, depth, shading, velocity, AO, GI and the light grid.
    Display-sized: the upscaled frame and everything after it (exposure is applied there, plus
    bloom, AgX and FXAA).
  - Geometry LOD and shadow-map detail follow the display resolution, and texture LOD is
    biased by log2(render / display), so the upscaled frame keeps output-resolution detail.
  - The jitter cycle grows to AMD's 8 × (display / render)² frames, so every display pixel gets
    samples.
  - **TAAU** (both backends): the TAA pass with display-sized history. Each display pixel
    rebuilds the current frame from the nearby render samples (a Gaussian of their jittered
    distance), and weighs it by how close a sample landed, so detail builds up over the
    cycle. At scale 1 it is exactly TAA.
  - **AMD FSR** (Direct3D 12): through AMD's FSR API and its signed DLLs, which pick **FSR 4**
    on GPUs that support it (RDNA 3 / RDNA 4) and FSR 3.1 elsewhere. The Lighting panel shows
    which one runs.
    - It takes the render-sized color, depth and motion vectors, our jitter, and the exposure
      measured before upscaling. It replaces TAA and runs AMD's sharpening (RCAS,
      `--fsr-sharpness`).
    - SDL_GPU doesn't expose native handles, so `cmake/patches/sdl3-falcon-native.patch`
      (local, like the timestamps patch) gives FSR SDL's D3D12 device, command list and
      textures, and restores SDL's descriptor heaps afterwards.
    - On Vulkan, or without the DLLs, FSR falls back to TAAU (logged and shown in the panel).
      AMD's SDK 2.x has no Vulkan support.
  - **NVIDIA DLSS** (Direct3D 12, NVIDIA RTX GPUs): DLSS Super Resolution through NGX, and
    **DLAA** at Native AA. The Lighting panel shows the mode that runs.
    - It runs in the plugin `bin/dlss/falcon_dlss.dll` (the C interface in
      `render/dlss_plugin_api.h`), which falcon.exe loads the first time DLSS is picked, so
      start-up doesn't pay for NGX.
    - Inputs as FSR's: render-sized color (HDR), reverse-Z depth, motion vectors in render
      pixels, our jitter, the measured exposure as a 1×1 texture, and the particles' reactive
      mask as DLSS's "bias current color" mask. Inputs go to a compute-readable state and the
      output to UAV around the call, through the same SDL native-access patch.
    - The quality mode is the one whose render-size range holds our render scale (DLAA, Ultra
      Quality, Quality, Balanced, Performance or Ultra Performance), nearest first. A change of
      render or display size makes a new DLSS feature (and resets its history).
    - On Vulkan, without the plugin, or on another GPU or an old driver, DLSS falls back to TAAU
      and the panel says why ("NGX did not start ...: an NVIDIA RTX GPU and driver are needed").
  - MSAA is off while upscaling. The editor's picking and selection outline map the viewport
    onto the render-sized buffers.
- **MSAA** (off by default; `--msaa 2|4|8` or the Lighting panel's *MSAA*): multisampled edge
  coverage for the visibility-buffer renderer (`msaa_edges.slang`).
  - SDL_GPU can't read multisampled textures, so shading stays at one sample per pixel. After
    the resolve, the four visibility draws are replayed into an N-sample target, and each
    sample takes the shaded colour of the nearby pixel that sees the same triangle (or, failing
    that, the closest depth). Samples no triangle covers take the sky next to the pixel.
  - The target's resolve averages the samples, so silhouettes get coverage-weighted edges,
    as with hardware MSAA, which also shades once per pixel. Geometry thinner than a pixel that
    the 1x visibility buffer missed borrows a neighbour's colour.
  - 4x at 1600×900 adds one raster of the scene and about 80 MB of targets. It combines with
    TAA and FXAA. The count is clamped to what the GPU supports.
- **Auto exposure:** a 256-bin log-luminance histogram (group-shared atomics), then a single
  group averages the 50–95% percentile band. The exposure stays on the GPU and adapts with a
  ~1.5/s time constant. `-` and `=` change the EV compensation.
  - **Dark scenes lift by half:** past 2 EV above daylight's exposure (a grey ground in the
    sun and sky, from the sun's colour and height), the exposure rises at half the rate. A room
    lit only through its door was lifted to the outside's mid grey, so the opening was far
    over and its bloom a veil across the walls; now the sky shows through it. A hard limit at
    2.5 EV left Sponza's arcades near black; with the knee they read, but the scene is darker
    than before (mean 40 against 88 out of 255). Outdoors nothing changes. The Lighting panel's
    *Dark-scene lift* sets the knee (`exposureLiftKnee`, −1 for no limit).
- **Bloom:** a 6-level 13-tap downsample chain (Karis average on the first level), then a
  3×3 tent upsample that accumulates each level.
- **Tonemap:** AgX (Sobotka's curve, polynomial fit) to sRGB, then AgX's "Punchy" look (an ASC
  CDL power of 1.35 and saturation of 1.4 on the encoded image). AgX base alone is flat, with
  midtones high and shadows lifted. `--look base` turns the look off, and
  `--look <power>,<saturation>` sets your own.
- **FXAA** (off by default; `--fxaa` or the Lighting panel): FXAA 3.11 "quality" on the
  tonemapped frame (`fxaa.slang`): a local-contrast test, the edge's direction, a search
  for its two ends, and a blend across it, plus a subpixel term. It runs before the editor
  draws its grid and gizmos.
- Debug views bypass jitter, TAA, MSAA, exposure, the tonemapper and FXAA, so they stay
  pixel-exact.
- Motion vectors come from the resolve: each pixel's mesh-space point under last frame's
  instance transform and camera. Moving objects reproject with themselves, not the background.

## Physics (M6)

Box3D (pinned alpha), driven by the job system (enkiTS; Box3D's task callbacks).
- **Static collision is cooked:**
  - The cooker simplifies each mesh's render geometry to 2 cm. Alpha-masked and blended
    materials are left out (foliage, chains).
  - It stores the result as Box3D's relocatable `b3MeshData` blob, BVH included, in a CPU-only
    `.fpak` section.
  - At load, each cooked instance becomes one static body that points at the blob in place:
    no build step. This takes 0.4 ms for Khronos Sponza (89 shapes) and 1.0 ms for Intel
    Sponza (399 shapes, 82k triangles).
- **Dynamic bodies** use the cooked built-in cube and sphere meshes through instance pools
  (`--max-bodies`, default 4096: half boxes, half spheres).
  - Pool slots are reserved in the GPU instance table at load and start hidden. A body only
    streams its transform row (and a tint).
  - The oldest body is recycled when a pool is full. Bodies that fall below the scene are
    removed.
- **Stepping:** fixed 60 Hz with 4 substeps. Rendering interpolates between the last two
  steps. Under fixed time (captures, tests), exactly one step per frame.
  - Results are deterministic across runs, worker counts and backends.
- **Streaming:** changed rows go to the scene buffer in one copy pass, one upload per run of
  consecutive slots, together with last frame's rows (for motion vectors).
- **Shadows of moving bodies:** each moved body sends a sphere around its old and new pose.
  - A compute pass re-renders every cached shadow page (sun levels and local-light faces)
    under it in place, with no reallocation and no budget.
  - Cached pages that aren't requested this frame are dropped instead.
- **ECS:** each dynamic body is a flecs entity (`RigidBody`, `MeshInstance`,
  `WorldTransform`); the transform is updated per physics step.
- **Scene entities' bodies** (the Inspector's Rigidbody and colliders, the `PhysicsBody` and
  `Collider` components) exist while the game runs: from the start of a game, and from Play to
  Stop in the editor (`PhysicsWorld::buildEntityBodies`).
  - A Rigidbody moves its entity: the interpolated pose goes into its `WorldTransform` (a
    child's `LocalTransform`), so its children and its rows follow as any moved entity's do.
    A kinematic one follows its entity instead (scripts, animation), pushing others.
  - A collider without a Rigidbody is static. A cooked instance with either gives up its
    cooked static collider meanwhile.
  - A Mesh Collider is the mesh's cooked collider when static. On a Rigidbody it is the mesh's
    bounds as a box: Box3D moves no triangle meshes, and its hull builder asserts on some
    meshes (see Known limits).
  - Shapes scale with the entity. The Rigidbody's mass is set as given; the collider gives
    its distribution.
  - Scripts' body API (`Physics` impulse and velocity) takes these entities too.
  - Stop destroys the bodies and puts every entity back.
  - Water lifts them like the spawned bodies: a Rigidbody's box or ball (a Mesh Collider's
    bounds) floats at the depth its mass gives it.
  - Not yet: several colliders on one entity, triggers, joints.
- **Cost** (Intel Sponza, 1600×900):
  - Pile settled: about 2.0 ms per frame with 2,239 bodies resting.
  - While 4,000 bodies are awake: about 4.5 ms per frame. A Box3D step takes 2–4.5 ms on
    6 workers (about 0.8 ms per frame on average).
  - The rest is GPU work: re-rendering the ~100 shadow pages under moving bodies each frame
    (only the moving casters, over each page's cached static layer; see Sun shadows).
- **Known limits:**
  - Box3D's hull builder (alpha) asserts on some Intel Sponza props, so only the built-in
    primitives get cooked hulls.
  - Large bodies against dense static meshes hit Box3D's 256-triangle contact buffer (it
    logs a warning).

## Terrain

A streamed heightfield, up to 8×8 km at 0.5 m samples (64×64 tiles), drawn by the renderer's own
passes. It has material layers that don't visibly repeat, collision, and editor brushes for
sculpting and painting. Planned next: rule-based GPU scatter, then procedural generation with
erosion.

- **Data** (`engine/terrain/terrain.h`):
  - Tiles of 256×256 quads (257² samples; neighbours share their edge samples), 16-bit heights
    over the terrain's height range (3 cm steps over 2 km).
  - A material map per sample: two layer ids, how much the second shows, and a hole flag (the
    cell is cut out).
  - Saved in `<scene>.terrain/`: `terrain.json` (size, spacing, height range), region files of
    8×8 tiles, and `coarse.bin`.
  - Tiles are stored as prediction residuals through meshoptimizer's vertex codec; material maps
    go through the same codec. A sculpted 2 km terrain with hills takes 11.5 MB.
  - Version 1 files (heights only) still load, with layer 1 everywhere.
  - Saves rewrite only regions with changed tiles, write-then-rename.
- **Streaming:**
  - A loader thread reads the tiles within `--terrain-radius` of the camera (default 2 km);
    saved tiles beyond 1.5× that are dropped.
  - The coarse map (every 8th sample) and the coarse levels' nodes are always in memory, so the
    whole terrain is always drawn; tiles add the detail.
- **Levels of detail:**
  - A patch is 8×8 quads at a spacing of 2^L samples. Level 0 is full detail; levels 0–5 lie
    inside one tile; the root covers the terrain.
  - Every node knows its height bounds and its error: how far its surface is from full detail.
  - Each time the camera moves, a quadtree descent on the CPU splits nodes whose error projects
    to more than `--terrain-pixels` (default 1 px). Off-screen nodes allow 8× the error and are
    kept: shadows and GI need them. Flat ground stays coarse, rough ground goes fine.
  - Neighbours are kept within one level of each other (a split node's neighbours' parents
    split too). A patch next to a coarser one moves its odd edge vertices onto the coarser edge,
    so there are no cracks.
  - The cut is kept in per-level bitsets.
- **Rendering:** the scene reserves terrain slots at load (`--terrain-patches`, default 16384;
  always in the editor, otherwise only when the scene has a terrain).
  - Each slot is an instance, a mesh and a cluster of 81 vertices on one shared 128-triangle grid,
    so culling, the visibility buffer, the resolve, shadows, GI, MSAA and picking draw patches
    like cooked clusters.
  - Only patches that changed are rewritten: their vertices, bounds and instance rows, in one copy
    pass. The cached shadow pages under them are redrawn, their BVHs rebuilt, and edits relight
    the GI probes round them.
  - Shadows grow to cover the terrain's extent and its whole height range.
  - Holes are degenerate triangles in the patch's own triangle run. A coarse patch is cut where
    the cell at its middle is.
- **Material layers** (`terrain/terrain_layers.h`, up to 32):
  - **Textures:** generated tileable looks (grass, dirt, rock, sand, snow), or your images
    (colour, normal, roughness, occlusion, height; paths relative to the scene folder).
  - **Build and storage:** resized to 1024², mipped, and encoded BC7 / BC5 / BC7 on a background
    thread. Each layer takes about 0.8 s and is cached in `<scene>.terrain/layers/` by a key of
    its sources. The layers live in three texture arrays.
  - **Live settings:** tile size, hex tiling, random angle, cell contrast, blend width, variation
    and its size, projection, tint, roughness, normal strength, far scale. Changing them costs
    nothing to rebuild. They are kept in `terrain.json`.
  - **Surface pass** (`terrain_surface.slang`, before the resolve), per terrain pixel:
    - The normal comes from the heightmap over about a pixel's footprint, from a GPU atlas of
      the 96 nearest tiles, or the coarse map further out.
    - The four material samples around the point are blended bilinearly and the three strongest
      layers kept.
    - Each layer is hex-tiled (Mikkelsen 2022): three hex cells, each at its own random rotation
      and offset, blended with weights sharpened toward the taller texel. A larger copy of the
      texture is blended in with distance. World-space noise varies brightness and hue.
    - Steep ground is projected from the sides (biplanar), so cliffs don't stretch.
    - Layers blend by their texture's height, so rocks poke through sand rather than fading.
    - The resolve lights the result. GI takes each patch's average layer colour.
    - It costs 0.7 ms at 1600×900 with three layers filling the screen.
  - **No layers:** the default look, grass, earth and rock by slope, broken up by world-space
    noise (`shaders/terrain_shading.slang`), now with per-pixel normals.
- **Collision:** a Box3D height field per resident tile within 512 m of the camera, quantized
  like the terrain itself, with holes cut out. Tiles whose shape changed are rebuilt when the
  simulation runs.
- **Editor** (Terrain tab; Create → Terrain):
  - **Creating:** pick the size, spacing, height range, and hills with a seed.
  - **Import Heightmap:** a grayscale image in the scene folder (16-bit PNG for smooth slopes;
    8-bit gives steps of 1/255 of the range) stretched over the whole terrain, black and white
    at the heights you set, its top edge along −Z. One undo step (up to 256 tiles).
    (`editor.terrain.import {image, low, high}`.)
  - **Brush mode:** the top of the Terrain tab has a Brush on/off switch and the operations as
    tiles; picking one turns the brush on (so does B, or the brush icon, which also opens the
    Terrain tab). While it is on, a pill at the top of the Scene view shows the operation, radius
    and strength, the keys, and a Stop button (or says the scene has no terrain). **Esc** stops
    brushing (back to the Move tool).
  - **Brush tool:** drag on the terrain. Operations:
    - raise / lower, smooth, sharpen;
    - flatten (to the stroke's start), set height;
    - ramp (from where the stroke starts to where it ends);
    - terrace (step height, sharpness), noise (feature size, amplitude);
    - stamp (the shape image as a height map);
    - **paint layer** and **hole** (Shift fills), with filters: slope range, height range, and a
      noise mask that breaks the stroke up.
  - **Shift** inverts and **Ctrl** smooths. **[** and **]** resize the brush.
  - **The brush shape:**
    - radius and strength;
    - a falloff curve: presets, or drag its 9 points;
    - a shape image: any grayscale picture in the scene folder's `brushes/`;
    - rotation, and dab spacing;
    - per-dab random offset, angle and size.
  - Brushes can be saved as presets (`brushes.json` next to the scene).
  - **Layers** (Terrain tab): add generated looks or image sets, pick one to edit its settings
    and to paint it. Only the last layer can be removed: painted samples keep their layer
    numbers. Layer changes are not undo steps.
  - The Scene view shows the brush on the ground: its falloff and its edge.
  - Each stroke is one undo step (the tiles it touched, before and after). Ctrl+S saves the
    terrain with the scene's edits.
  - **Scatter** (Terrain tab): add a layer for a mesh (or a preset), then paint it onto the
    terrain. Clicking a scattered instance selects it, and **Delete** removes just that one
    (Restore brings deleted ones back).
    - **Placement:** a new layer is **painted by hand**: nothing grows until the brush's
      **Scatter Paint** operation paints it (Shift erases), and the brush comes on set to paint
      it when the layer is added. **Paint on the Terrain** (or the brush on its row) paints a
      layer again. The layer's rules (density, slope, height, clumping) still decide where in the
      painted ground they grow.
    - **Generated over the terrain:** the layer covers the terrain wherever its rules allow.
      **Generated, painting clears it** is the same, with painted ground kept clear.
- **Scatter** (`terrain/terrain_scatter.h`, `render/scatter_renderer.h`, `terrain_scatter.slang`):
  layers of the scene's meshes spread over the terrain by rules, generated on the GPU around the
  camera. The same every run, on both backends.
  - **A layer:**
    - up to 16 meshes, each with a share (shown as a percentage of the layer) and a scale range.
      In the panel the meshes are a list: pick one to edit it, Add Mesh, or drop meshes (or a
      model, for all its meshes) from the Project panel onto the list;
    - a density (instances per m²), a seed and a draw distance.
  - **Where they grow:**
    - on chosen material layers (with a minimum share);
    - slope and height ranges, with soft edges;
    - a clumping noise (size, amount, threshold);
    - a painted mask that removes them, or allows them only where painted (7 layers at most:
      the material map's spare bits). Each layer keeps its own bit (`paintBit`) when others are
      removed; a removed layer's paint is cleared over the terrain (a tile a frame) before its
      bit is used again. A layer that grows only where painted gives cells without paint no
      instance slots;
    - never in holes, never where deleted.
  - **Randomness:** the seed (the dice picks a new one), spacing jitter (0: an even grid, 1:
    anywhere in its grid square), the random turn's range (0 to 360 degrees; `yawRange`, older
    files' `randomYaw` read as 360 or 0), random lean, darker shades at random.
  - **How they stand:** follow slope (upright to along the ground's normal), sink into the
    ground.
  - **Shadows** (per layer, `castShadows`): off, the layer's instances cast none (they are left
    out of the shadow map's culling: cheaper for many small meshes). They still receive shadows.
  - **Nothing growing:** the panel samples the terrain (64 x 64 points) against each layer's slope
    and height rules; when no ground fits (a cliff preset on flat ground) the layer says so and
    which rules to widen. Each stands on its lowest point, in the
    mesh's own space (+Y up; the glTF node's transform is not applied).
  - **Cells:** a layer's ground is split into square cells holding about 512 candidates each, on a
    grid jittered by the layer's spacing jitter. Each candidate has its own instance slot and a hash that decides whether it
    grows. Cells within the draw distance are generated nearest first (up to 256 a frame), and are
    cleared beyond 1.15× it.
  - **Regeneration:** a cell is generated again when the terrain under it changes (sculpt, paint,
    holes), or when its tiles' full detail reaches the GPU (until then it stands on the 4 m coarse
    map).
  - **Live changes:** rule changes in the editor regenerate the layer's cells in place.
  - **Instance slots:** every mesh of every layer has a pool, reserved when the scene loads
    (`budget`, default 20000). The editor also reserves 131072 spare slots: on every change to
    the layer list the pools are handed out again, in layer order, from the reserved ones and the
    spare ones, so layers and meshes added while editing show at once. The editor warns when cells
    run out of slots, or a mesh gets none (spare ones used up: save and restart). Out of slots,
    a nearer cell takes the slots of the layer mesh's farthest one (up to 256 a frame).
  - **Dense scatter** (grass, flowers): its instances carry `kInstanceScatter`, and are thinned
    first when the GPU's per-frame budgets run short, instead of the budgets overflowing (which
    drops clusters of any mesh, different ones as the view changes: meshes flicker):
    - **Visible clusters** (131072): scatter smaller than a size on screen is left out. The size
      rises at once by about what the frame wanted over the budget (the counters go on past it),
      and falls slowly once well under it, so the edge where far grass ends stays put.
    - **Shadow records:** scatter casters smaller than a number of a shadow level's texels are
      left out (per level, not per camera, so cached pages stay valid), raised while records
      overflow.
    - The Terrain tab says when scatter is thinned. `render_terrain_scatter_dense` (20 cones per
      m²) fails on any overflow.
  - **Shadows:** cached shadow pages over generated or cleared cells are redrawn.
  - **Collision** (per layer): cells within 96 m of the camera are read back from the GPU (a few
    frames late) and become static bodies with the mesh's collider. The built-in cube and sphere
    become boxes and balls.
  - Layers live in `terrain.json` (`"scatter"`), deleted instances as (cell, mesh, candidate).
- **Generation** (`terrain/terrain_generate.h`, `render/terrain_generator.h`, `terrain_generate.slang`):
  a stack of steps run on the GPU over the whole terrain or a square of it, then written into the
  terrain as one undo step. The same seed gives the same ground.
  - **Steps**, each over what the ones before made:
    - **noise:** fbm, ridged (mountain ranges), billow (rounded), eroded (fbm whose detail
      fades on slopes), or Voronoi (cells: mounds; crags: ridges between cells; mesas: flat tops
      at random heights). Feature size, height, offset, octaves, lacunarity, gain, sharpness, and
      a domain warp that bends it into natural shapes.
    - **terrace:** steps of a height, a sharpness from slopes to cliffs, and strata noise that
      breaks them up.
    - **plateau:** heights above a level pressed down toward it.
    - **smooth:** a blur of a radius.
    - **erode:** hydraulic erosion (the virtual pipe model: rain, water flowing to the four
      neighbours, a sediment capacity from speed and slope, dissolving above it, depositing
      below it, sediment carried with the water) plus thermal erosion (slopes steeper than the
      talus angle slump).
      - It runs on a coarser grid (`cellSize`, default 2 m; coarser for wider valleys), and the
        change in height is added to the full-detail heights.
      - It leaves flow and deposit masks for the paint steps after it.
    - **paint:** a material layer where the step's masks allow (auto-painting). Paint steps can
      name a generated look (`grass`, `rock`, ...), which is added to the layers when missing.
  - **Every step:**
    - a blend (add, max, min, replace, multiply) and an amount;
    - masks: height, slope and curvature (ridges vs valleys) ranges with soft edges, and a noise
      mask; paint steps also have the flow and deposit ranges.
  - **Presets:** Rolling hills, Mountains, Canyons, Mesas, Dunes, Crags. Each is an ordinary stack
    to edit.
  - **Region:** the whole terrain, or a square (centre, size, falloff) blended into the ground
    around it. It starts from the current heights or from flat ground.
  - **Runs:** in chunks of up to 2049² samples with a margin for the steps' reach.
    - Tiles are pinned and loaded as needed.
    - A few GPU submissions per frame, each with its own fence, so the editor keeps drawing and
      shows progress.
    - Chunks read what the ones before wrote, so their edges meet.
  - The stack is kept in `terrain.json` (`"generator"`).
  - **Editor** (Terrain tab → Generate):
    - a preset to start from, the region ("Where I look" centres it on the Scene view);
    - the steps (add, reorder, switch off, edit);
    - Generate, with a progress bar and Stop.
  - **Undo:** a run is one undo step (up to 256 tiles).
  - **Erosion brush** (the brush's Erode operation): the stroke marks where; on release, hydraulic
    and thermal erosion run under it on the GPU. The strength sets how long it runs.
- **Rock formations:**
  - **Terrain-shaded instances** (`fpak::kInstanceTerrainShaded`): an instance can be shaded with a
    terrain material layer instead of its own material. Generated rocks shaded with the rock
    layer match the cliffs around them, with no textures of their own.
    - The surface pass rebuilds the mesh's smooth normal from the visibility buffer's triangle.
    - It projects the layer from three sides, hex-tiled like the ground.
    - Near the ground, it blends the ground's own strongest layer onto the up-facing parts, up to
      a height, its edge broken by noise. Rocks sit in the ground instead of on it.
  - **Scatter layers** choose this with `terrainLayer` (a number) or `terrainLook` (a generated look
    such as `rock`, added to the material layers when missing), and `baseBlend` (metres).
  - **Presets** (Scatter → Add Preset, or `--terrain-scatter <name>`):
    - **Boulders:** clumped, on gentle ground, with collision;
    - **Scree:** small stones in patches on 26–44° slopes;
    - **Cliff rocks:** slabs and boulders set into slopes over 34°, along the face;
    - **Spires and arches:** rare, in clumps, with collision.
  - Overhangs and arches are meshes. The heightfield stays a heightfield.
- **Cost** (RX 9060 XT, 1600×900):
  - **8 km, 300 m hills, flying at 40 m/s:** 5.2 ms per frame. That is 11–14k patches, 1.5M
    triangles drawn and 1.5–2.3 ms to pick levels.
  - **2 km, streamed with a 300 m radius:** 1.5 ms per frame; the slowest frame after start-up was
    6.6 ms.
  - **Creating an 8 km terrain** in memory with hills takes 14 s (all cores).
  - **Generating the Mountains preset** (3 noise steps, 400 iterations of erosion on 2 m cells,
    4 paint steps):
    - 1 km: 0.3 s;
    - 4 km: 5.2 s (16 chunks);
    - 8 km: 23 s (64 chunks).
  - **Rock presets on the Mountains preset** (1 km; Boulders, Spires and arches, Cliff rocks):
    2.5 ms per frame against 2.25 ms without them (about 2,200 more instances drawn).
  - **Scatter, 4 km terrain flying at 40 m/s:**
    - **Dense layer** (1.23M candidate slots in 2318 cells, ~97k instances drawn): 5.1 ms per
      frame.
    - **Lighter layers** (330k slots, ~24k drawn): 3.1 ms per frame, against 2.5 ms for the
      terrain alone.
- **Known limits:**
  - Picking levels runs on the main thread each time the camera moves 0.5 m.
  - Changing the layer list is not undoable.
  - Tiles that are not resident can't refine past 4 m samples, and their neighbours may then
    meet with a crack.
  - **Scatter:**
    - Layers and meshes added in the editor beyond the 131072 spare slots show after saving and
      restarting.
    - Adding or removing a layer or mesh regenerates every layer's cells.
    - Changing a layer's density or enabling it regenerates every layer's cells.
    - Scattered instances don't take part in GI.
    - Scatter changes are not undo steps (painted masks are).
  - **Generation:**
    - Erosion's water stops at a chunk's margin (256 samples or more), so a valley longer than
      that is carved in pieces.
    - Writing a finished 2049² chunk into the terrain takes about 0.15 s on the main thread: the
      editor hitches once per chunk.
    - Runs over more than 256 tiles (above 4 km² at 0.5 m) are not undoable.
    - Across backends, erosion's float rounding moves a few dithered material edges.
    - The flow and deposit masks live only during a run (they are not saved for scatter rules).
  - **Rocks:**
    - Only scattered instances can be terrain-shaded (not scene entities).
    - A terrain-shaded rock shows its own plain material until the layer's textures are built.
    - Scattered rocks don't take part in GI.
  - Contribution culling (instances under `--cull-pixels`) skips scene instances, not terrain
    patches: far cliffs keep fine patches, which are still ground.

## Water

Lakes, ponds, pools, rivers and the open ocean (rectangles, circles, drawn outlines, a river's
course, an endless sea)
with one shader and one parameter set: colours, clarity, murk, film (scum, algae), waves from
still to an ocean's swell, foam and whitecaps, edges. What is in the water floats, bobs on the
waves and drifts with the flow; scripts can ask where the water is. Under the surface: fog,
the world above through Snell's window, a waterline across the lens, caustics; tanks seen
from outside and inside. Things moving through the water leave ripples and wakes.

- **Bodies** (`water/water_system.h`): a Water entity (Create → Water ▸ Lake, Pond, Pool) is a
  horizontal surface at the entity's position, turned by its yaw: a rectangle (width × length),
  a circle, an outline of points, or a box (a tank: `depth` metres deep). Saved in the scene's
  edits (`"water": {shape, width, length, depth, points, material, overrides, buoyancy}`).
  Hidden, deleted and undone like any entity; not in prefabs yet.
- **Water materials** (`.fwater`, `assets/water_asset.h`): every parameter is one row of a field
  table (key, kind, range, group, label, tip). Reading, writing, per-body overrides and the
  editor's controls all come from it, so a new parameter is one line there and its use in the
  shaders. Built-in presets: Lake, Tropical lagoon, Ocean, Muddy river, Swamp, Pool, Oasis
  (`builtin:<name>`). A body names its material and overrides any field; the Inspector shows
  every field by group, overrides marked with a button back to the material's value. Its Sound
  group (splash, slosh, under water; see Audio) holds sounds by project path.
  **Save as Asset** writes the body's look as a `.fwater` (the body then uses it), **Apply to
  Asset** writes a body's overrides into its `.fwater`, and every body using it follows.
- **Waves:** still, or up to 8 Gerstner waves from the wind (Pierson–Moskowitz: wave length and
  height from the wind speed; direction and spread around the wind; choppiness sharpens crests
  without loops). Their frequencies are multiples of 2π / 1200 s, so the clock wraps exactly.
  The CPU evaluates the same sum: `WaterSystem::sample()` (height, normal, flow) for physics and
  scripts.
- **Ocean waves** (`water/ocean_spectrum.h`, `render/water_ocean.h`, `water_fft.slang`): a
  material's `waves: ocean` makes Tessendorf's FFT ocean: a JONSWAP spectrum for the wind
  (100 km fetch) with cos²ˢ spreading around it, as random amplitudes on three cascades
  (squares of 4× the peak wavelength, then about 6× and 40× smaller, each its own band of
  wavenumbers), 256 × 256 waves each.
  - **GPU, every frame the ocean is in view:** the spectrum at that time, a row and a column
    inverse FFT (radix-2 Stockham in shared memory, four fields at once, each pair packed as one
    complex number), then displacement (with choppiness) and slopes. Where the surface folds
    (the Jacobian below a threshold set by `crestFoam`) foam is made and fades over about a
    second. The vertex shader adds the three cascades, each at the detail the patch can show;
    the fragment shader takes their slopes and foam.
  - **CPU:** the same amplitudes; the 64 strongest waves evaluated directly give the sea's height
    for physics and scripts (bodies ride the swell the view shows).
  - One spectrum per scene (the first body with ocean waves); it is rebuilt only when the wind,
    height, choppiness or spread change. Frequencies wrap with the clock like the Gerstner waves.
  - **The sea** (Create → Water ▸ Ocean, shape `ocean`): endless, a 16 km square of patches
    around the camera moved in quarter steps (the finer levels never shift).
  - Mips of the displacement and slope maps are computed (four levels per dispatch,
    `water_mips.slang`): the same on both backends, unlike the drivers' blits. Foam fades
    with water time, not frames, so a capture (its clock stopped) doesn't depend on how long it
    waited for streaming.
  - **Cost** (1600×900, the sea to the horizon): FFT 0.27 ms and mips 0.17 ms (three cascades,
    mostly the barriers between their small dispatches), surface 0.16 ms, composite 0.29 ms.
- **Rendering** (`render/water_renderer.h`; nothing runs while no water is in view):
  - **Surface** (`water_surface.slang`), after the visibility buffer: the scene's depth copied
    into the water's depth target, then every patch of every body in one instanced draw. Patches
    are 32 × 32 quads from a CDLOD quadtree over each body (25 cm quads near the camera,
    coarser away from it, vertices morphing between levels so nothing pops or cracks), picked
    and frustum-culled on the CPU. The vertex shader adds the waves; the fragment shader clips
    to the outline and adds two drifting layers of small ripples, with geometric specular
    anti-aliasing (the normal's change across a pixel widens the highlight instead of letting
    it sparkle). It writes depth, normals, crest foam and the body.
  - **Shadows:** the water's depth also requests shadow pages, so the surface is shadowed.
  - **Composite** (`water_composite.slang`), after the resolve, over the screen rectangle
    the water covers: what is under the water, bent by the surface, from the reflection pass's
    mip pyramid of the frame before water (blurrier with depth in murky water), absorbed per
    colour over its length in the water (Beer–Lambert: clarity and the shallow tint), plus the
    light the water scatters back (sun, sky and GI above it). The edge fades into the shore over
    `edgeFade` metres of depth; foam where it is shallower than `shoreFoam` and on crests, and
    film, are lit as a layer on top; then the sun's highlight. It writes the surface's Fresnel
    weight and normals, and the reflection pass, starting from the water's depth, adds the
    reflection (screen space, the BVH, sky). TAA and FSR take the depth with the water in it.
  - **Planar reflections** (`water_sspr.slang`; Cichocki's screen-space planar reflections):
    water is a mirror, so for the body filling most of the view, every scene pixel above its plane
    is mirrored across it and projected to where its reflection lands (the one nearest the
    plane wins, by atomic min; ties: the lowest pixel, so runs match). The composite reads, at
    each of that body's pixels, which pixel of the frame it reflects, bent by how far the surface's
    normal leans from the plane, blurred by roughness, gaps between stretched rows filled from
    the rows next to them. A sharp, exact reflection per pixel: the reflection pass's trace (half
    resolution, coarse steps across the screen) showed as rows and stair steps on still water.
    What the frame doesn't hold (above its top edge, hidden behind something nearer) is traced
    through the scene's BVH instead, lit where it hits as the reflections' far trace lights it
    (past 32 m by the probes' light alone; a miss, the sky). Without the BVH, through the
    terrain's coarse heightmap (steps of most of a coarse sample, 5% of the distance further out,
    at most 64, then bisected), lit there from its layers' mean colours, the sun through the shadow
    map, and the sky and GI; else the sky. The mirror
    fades into that over the 8% of the frame below its top edge, so the reflection carries on
    past it instead of stopping in a line. The reflection pass skips the mirror's pixels. 0.06 ms
    at 1600×900 (the reflection pass 0.55 → 0.26 ms with a lake over Sponza's floor). Meshes off
    screen (trees, rocks) are not in the trace: their reflections still end at the frame's edge.
  - **Caustics:** a pattern made once on the CPU by tracing sunlight through a rippled surface
    onto a floor (rays bent by its slopes, gathered per texel: the bright network where they
    cross), read where the light entered the water above each point under it, two layers drifting
    apart and multiplied (the network moves). It brightens the lines more than it darkens between
    them, fades over `causticDepth` metres, and shows only where the sun reaches that point (its
    own shadow lookup).
  - **Under water** (the camera under a body's surface, inside its outline and depth): every
    pixel whose near-plane point is below the surface there takes the water's fog over its
    distance (`fogColor`, `fogDensity`, tinted like the water's absorption, lit by what reaches
    the camera's depth), caustics on what is under the surface, and the surface from below:
    inside Snell's window (water to air, index 1.33) the world above, bent, outside it total
    internal reflection. The others stay above water: a waterline across the lens when the camera
    is at the surface. No reflections are added under water.
  - **Tanks** (shape `box`): the surface, four sides and a bottom (the sides' top edges follow
    the waves). Seen from outside, the water is as thick as the ray's way through the box; from
    inside, the walls are windows (the fog stops at them).
  - **Cost** (Sponza, 1600×900, a lake over the whole floor): surface 0.09 ms, composite 0.19 ms,
    its shadow page requests 0.03 ms; reflecting it adds about 0.2 ms to the reflection pass.
- **Rivers** (shape `river`; Create → Water ▸ River): a centre line of points (each with a height,
  upstream first), smoothed by Catmull-Rom (8 steps a span), `width` wide. Drawn as stretches of
  up to 32 spans by 32 quads across, each culled by its box. The flow follows the course: ripples
  and foam move downstream, flow-mapped in two phases half a cycle apart (each reset when its
  weight is zero, so a bending course never stretches them for long), foam in streaks along the
  flow (`crestFoam`: how much). On the CPU the water's height follows the course down its points,
  and its flow is along it, fastest mid-stream. The Inspector edits the points (x, z, height) of
  a river or an outline.
- **Ripples and wakes** (`water_ripples.slang`): the wave equation on a square around the camera
  (`FrameSettings::water`: 64 m, 512² by default), anchored to the world's grid (as the camera
  moves, the square moves in whole texels and what it held moves with it), its edges absorbing.
  Steps of 1/60 s for the water time that passed (none while the clock stands: captures stay
  deterministic); splashes wait for a frame that steps (above 60 fps most don't). Splashes:
  bodies moving through the surface (the water they push aside),
  the camera at the surface, `Water.Splash` from scripts. The surface adds the height's slopes to
  its normals, per material (`ripples`, `rippleStrength`).
- **Physics** (`PhysicsWorld::applyWater`, before each step, in body order: deterministic):
  - **Buoyancy:** boxes are split into 2³ cells (3³ when longer than 2 m), each pushed up by the
    water it displaces at its own centre, so a tilted box rights itself; spheres by their
    submerged cap. Bodies (500 kg/m³) float half under in fresh water (`density`). The scene's
    Rigidbody entities float the same way, by their collider's box or ball and their mass (a
    1 m ball floats half under at about 260 kg; at the default 1 kg it rides on top like a
    beach ball).
  - **Stability:** each cell's depth is taken where it will be at the step's end, so a bob
    loses energy instead of gaining it.
  - **Added mass:** what a body pushes also moves the water around it (half its submerged
    volume), so it answers the water as if that much heavier, never lighter than a 1/60 s step
    resolves; the water's push is scaled to its own share and the depth it floats at is kept.
    A drop plunges by how fast it came in (a 1 m ball from 2.75 m: half a metre at 2 kg, fully
    under at 260 kg), comes back up and bobs.
  - **Drag:** on each cell's velocity relative to the flow (quadratic, with a little linear so
    bodies settle), capped so one step never reverses a motion (`drag`); spheres' spin damped.
  - **Heave:** moving up or down through the surface makes waves that carry energy off: damped
    by the area the waterline cuts, at 0.15 of critical (`drag` 1), so a drop bobs two or three
    times, each smaller.
  - **Wakes:** each step a body crossing the surface raises the ripples by the water it pushes
    through it (waterline area × vertical speed) over twice its width, plus a little for
    sliding along it: a drop rings the water, each bob again.
  - **Currents:** the flow (`flowSpeed`, along the wind's direction for lakes) carries what
    floats (`current`). Bodies in moving water stay awake.
  - A body's `buoyancy` off: nothing floats in it.
- **Scripts** (C# API v6; v7 adds audio): `Water.Sample(position)` (body, height, normal, flow, depth below the
  surface), `Water.HeightAt(x, z)` (NaN without water), `Water.IsUnderwater(position)`,
  `Water.Splash(position, radius, strength)` (ripples); the native `waterSample`, `waterSplash`.
- **Settings:** `FrameSettings::water`: enabled, ripples (on, the square's size and resolution),
  the FFT's size (at most 256).
- **Limits:** the ocean doesn't yet calm in shallow water or break on shores (only the shore
  fade and foam); one ocean spectrum per scene; ripples are one square around the camera (they
  pass through walls, and bodies at different heights share them); one water layer per pixel (a lake seen through another body's water shows the
  nearer one); no MSAA edge on the waterline; reflections of water in water show what is under
  it.

## Audio

Sound goes through our own mixer on SDL3's audio (WASAPI on Windows), spatialized by Valve's
Steam Audio (see Build). Milestones A0 (groundwork), A1 (sources, sounds, HRTF, distance and
air), A2 (acoustic materials, occlusion and transmission), A3 (realtime reflections and
reverb), A4 (physics sounds), A5 (water sounds), A6 (the scripts' audio API) and A7 (baked
acoustics) are in.

**Sounds and clips:**
- **Clips** are `.wav` and `.ogg` files in the project, imported like any file (the Import
  dialog has a Sounds filter).
- **A sound** (`.fsound`, JSON; `engine/assets/sound_asset.*`) is what plays:
  - its clips: one picked each time, at random, shuffled (none twice until all have played) or
    in order;
  - volume and pitch, and how much each varies per play (dB, semitones), and Vary (off: it plays
    exactly as made, its amounts ignored);
  - loop, 3D or 2D, min and max distance, rolloff (inverse distance or linear), air absorption;
  - its bus, how many may play at once (the oldest stops), and a cooldown.
- One field table drives a sound's JSON and its editor (as `.fwater`'s does). Presets: UI click,
  Footstep, Ambience loop, Impact, Music.
- Create > Sound (with clips selected, they become its variations), or "Create Sound from Clips"
  on a clip. Selected, a sound or clip shows in the Inspector with ▶ / ■; a sound's clips and
  fields are edited there, and what plays hears the change at once.

**Components** (schema, so scripts see them too):
- **Audio Source** (Add Component): its sound or clip (dropped on its slot, or picked), Play On
  Start, volume and pitch (multiplying the sound's), and Vary (off: exactly as made).
  - A `.fsound`'s loop, 3D, distances, rolloff, air and bus apply unless Override Sound is on;
    a clip always uses the source's own.
  - ▶ previews it where it is; while the game runs (Play to Stop, a game from its start) the
    ones marked Play On Start play and follow their entities.
  - Selected and 3D, the scene view shows its full-volume and silent distances; an entity with
    no icon of its own shows a speaker.
  - A sound or clip dragged into the scene view (or onto a hierarchy row) becomes an empty
    entity with an Audio Source, a metre above the point.
- **Audio Listener**: where the game hears from (the first enabled one) while it runs, with a
  gain; without one, the main camera, and in the editor the scene view.
- Both are saved in the edits and prefabs, with undo, and Stop puts the scene back.

**Acoustics: what the scene does to sound** (`audio_scene.*`, `assets/acoustic_material.*`):
- **Acoustic materials** say how a surface treats sound, in Steam Audio's three bands (low under
  800 Hz, mid to 8 kHz, high above): what it absorbs on a bounce, how much of the bounce scatters,
  and what passes through it. Presets: Steam Audio's (generic, brick, concrete, ceramic, gravel,
  carpet, glass, plaster, wood, metal, rock) and sand, grass, dirt, snow, water, foliage, cloth,
  plastic.
- **Where they come from:**
  - **A material** (`.fmat`, "acoustic"; an Acoustics group in its editor, heard at once, not
    cooked).
  - **A mesh entity's own** (the Inspector's Acoustics: its material's, its own values, or none:
    sound passes as if it were not there). That is an `AcousticSurface` component, saved in the
    edits and prefabs. A Rigidbody occludes only with Occludes Moving on.
  - **A terrain layer** ("Sounds like"; by default from its look: grass, dirt, rock, sand, snow).
  - Anything else is generic; scatter (rocks) is rock.
- **The geometry** is kept in step with the scene every frame, on the main thread:
  - every visible mesh entity, from its mesh's cooked collider (the built-in cube and sphere: their
    shapes; meshes without a collider, such as foliage, let sound through), as a world-space
    static mesh of its own. One that changes material is placed again.
  - A mesh that moves (a held weapon, a door, an occluding body) is instanced from its first move
    on: its triangles in their own space, in a Steam Audio scene of their own, placed by a
    transform. Moving it then costs a transform at the next commit, not a new mesh. Before this,
    Desert's 4,400-triangle pistol in the soldier's hand was placed again every frame: 3.3 ms of
    main-thread time, plus a rebuild of the whole audio scene every ~230 frames as removed meshes
    piled up. (Steam Audio's Embree scenes lose instanced meshes added after one was removed;
    the runtime uses Steam Audio's own tracer, which does not.)
  - Not instanced, and removed meshes are kept until the scene is rebuilt (more than 256 of them,
    or a million triangles): with Steam Audio 4.8.1's Embree scenes, releasing a removed mesh
    leaves meshes added later unseen by occlusion rays (reproduced outside the engine);
  - the terrain's tiles within 256 m of the listener, at 2 m quads, each with its dominant layer's
    material (holes stay open);
  - scatter cells as physics gets them.
- **Occlusion and transmission:** every sound placed in the world is simulated each frame by
  Steam Audio: volumetric occlusion (16 rays toward a 0.5 m ball around it) and transmission
  through the nearest surface in the way, per band. Steam Audio's direct effect then muffles it.
  Sounds (`.fsound`) and Audio Sources can turn either off.
  - **On a thread of its own** ("Audio direct" in the Profiler). Each frame the main thread takes
    the last run's results, commits the scene's and the sources' changes (only while that thread
    and the reflections are idle), and hands it the listener and the sources as they are now. What
    is heard is a frame behind the simulation. If a run takes longer than a frame, the main thread
    does not wait: that frame hands it nothing. Fixed-time runs wait for each run, so they stay in
    step and repeatable.
  - Panned sounds (no HRTF) get the same as a level.
- **In the editor** a playing or previewed source's speaker turns from white to red as its path to
  the listener is blocked.
- **Ray tracing:** Steam Audio's own tracer while the game runs. Intel's Embree (inside `phonon.dll`)
  keeps a thread per core spinning whether there is work or not: about 9 of 12 cores, idle. Bakes
  still use Embree, on a device of their own that goes when the bake ends.
- **Cost** (Sponza with a 4×4-tile terrain and boulder scatter: 92 meshes, 1434 placed, 16
  terrain tiles, 344k triangles): about 0.02 ms a frame keeping it in step; a source simulates in
  a few microseconds.
  - Desert (27 meshes, 4 terrain tiles, 37k triangles, the soldier holding the pistol):
    `AudioScene::update` went from 3.3-3.5 ms to 0.02-0.03 ms on the main thread, and frames from
    4.3 to 3.0 ms (235 to 320-350 fps, hidden at 1600×900). The direct run itself is about 0.01 ms
    a source, on its thread.
  - The Profiler shows it as `AudioScene sync entities`, `AudioScene sync terrain` and
    `AudioScene commit` on the main thread, and `AudioScene run direct` on its own.

**Reflections and reverb** (Steam Audio's realtime reflections; `audio_scene.*`, `steam_audio.*`):
- **Simulated on a thread of their own**, a few times a second: rays from the listener bounce
  around the scene's geometry and acoustic materials.
  - The nearest sounds get echoes of their own: each a reflection effect of its own.
  - Every other sound shares the listener's reverb: a simulated source at the listener, whose
    effect takes them all.
  - It all goes into one ambisonic field (first order), decoded once a block for the listener
    through the HRTF.
- **Reverb types:**
  - Convolution: the simulated echoes exactly, through Steam Audio's reflection mixer.
  - Parametric: a smooth tail of the simulated length.
  - Hybrid (the default): convolution for the first second, then a parametric tail.
- **Quality** (Project Settings > Audio > Reflections):

  | quality | rays | bounces | tail | own echoes | runs a second |
  |---|---|---|---|---|---|
  | off | | | | | |
  | low | 1024 | 16 | 1.0 s | 4 | 10 |
  | medium (default) | 2048 | 32 | 1.5 s | 8 | 15 |
  | high | 4096 | 64 | 2.0 s | 16 | 20 |

  There is also a reverb level. Sounds (`.fsound`) and Audio Sources can turn reflections off.
- **Bounces matter more than rays.** With 8 bounces an 8 m concrete room rang for 0.28 s; with
  32, for 0.84 s.
- **Scene changes are committed between runs** (reflections and direct). The main thread takes
  the simulation lock only when it is free, else it tries again next frame.
- **Fixed-time runs** (`--hidden` with `--fixed-time`) run the reflections in step with the frame
  clock (every 4 frames at medium) instead. Rays are sampled at random, so two runs still differ
  slightly (within about -50 dB).
- **Cost** (Sponza, 213k triangles, 3 of 12 cores, measured with Embree):
  - a run: 6 ms (low), 19 ms (medium), 31 ms (high);
  - the mixer's share of real time: about 2 - 4%;
  - with Steam Audio's own tracer (now used while the game runs), a medium run took about 95 ms
    there, on the reflections thread, so large scenes update their reverb less often. Desert's
    37k triangles take about 3.5 ms a run either way.
- **The stats line** adds the direct run's time on its thread and `update`'s on the main thread,
  how many sounds have their own echoes, the last run's time, and the
  listener's reverb time (RT60 per band).

**Baked acoustics** (Steam Audio's probes; `audio_bake.*`, `audio_scene.*`; the editor's Audio tab):
- **What a bake makes:** probes on a uniform grid a fixed height above the floor under them
  (Steam Audio's "uniform floor": over the colliders and the terrain), within a region. At each
  probe, the listener's reverb (impulse responses and reverb times, so any reverb type plays
  it); with Pathing on, also the paths sound takes between probes that see each other.
- **Where it is used:** wherever a probe reaches the listener, the listener's reverb comes from
  the probes around it, not from rays traced as you go; elsewhere it stays realtime. Sounds with
  reflections also take the baked paths when probes reach both them and the listener. Pathing
  runs with the reflections, and checks each path against the scene as it is now (a door shut
  since the bake blocks it).
- **Paths are heard as much as the direct path is blocked:** a sound behind a wall is heard
  through the doorway around it, coming from the doorway. A sound in plain sight is heard once,
  not twice. A path falls off as 1 / its length, scaled to the sound's minimum distance like the
  direct path. In the test's two rooms, a sound behind the dividing wall went from 0.0007 RMS
  (through the concrete) to 0.02 (through the doorway).
- **Where it is kept:** `<scene>.audio/` beside the scene's `.fpak` (moved and renamed with it):
  - `bake.json`: the bake's settings, and what the last bake made, from what geometry;
  - `probes.bin`: Steam Audio's probe batch (energy fields, reverb times, paths; about 4.5 KB a
    probe at first order);
  - `probes.pos`: the probes, for the stale check and the Scene view.
- **The region:** around the scene's static objects (the terrain's when there are none, which can
  be very large), or a box of your own (center and size, or "Around the view": 64 x 24 x 64 m
  around the Scene view's camera). Geometry within 16 m beyond it is baked too. A bake places at
  most 20 000 probes.
- **Settings** (per scene, in `bake.json`): spacing (3 m by default), height above the floor
  (1.5 m), quality, and pathing (on).

  | quality | rays | bounces | simulated tail |
  |---|---|---|---|
  | low | 8192 | 16 | 1.0 s |
  | medium (default) | 16 384 | 32 | 1.5 s |
  | high | 32 768 | 64 | 2.0 s |
- **Baking** runs on a thread of the engine's with its own copy of the geometry, so the editor
  goes on meanwhile. The Audio tab shows the stage (placing probes, reverb, paths, saving) and
  its progress, with Cancel. The baked data is used as soon as it is done.
  - The geometry is the static meshes as the audio scene places them, plus the terrain's coarse
    map (4 m quads at 0.5 m spacing, holes left open, each its layer's acoustics).
  - Moving bodies, and the terrain's scatter (it streams with the view), are not baked.
- **Headless:** `falcon --project P --scene P/Scenes/Main.fpak --audio-bake` bakes with the
  scene's settings, writes `<scene>.audio/` and exits (non-zero when it fails), for builds and
  tests.
- **Stale warnings:** the bake keeps a fingerprint of the geometry it heard: the static meshes in
  its region (shape, place, acoustics) and the terrain under it (heights, holes, layers'
  acoustics). The editor and the game check the scene against it half a second after it last
  changed. Out of date, the bake is still used:
  - a notice says what changed (objects added, removed or moved, or the terrain);
  - the Audio tab shows a warning until you bake again;
  - the log says so once.
  Put back as it was, the bake is up to date again.
- **The Scene view** (Show probes, in the Audio tab) draws the region as a box and the nearest
  2000 probes as dots.
- **The stats line** reports the baked probes on load; `test.expect` `audio.bake` reads the state,
  the probes, whether the last run's reverb was baked, and the sounds with baked paths.
- **Found along the way:** Steam Audio 4.8.1's probe generation scales a unit cube centred on the
  origin (the documentation says 0 .. 1), moved by the matrix's last column. A path's ambisonic
  weight carries its 1 / length attenuation (0.28 at 1 m, 0.14 at 2 m).
- **Not baked:** a sound's own echoes (the nearest sounds keep realtime reflections), and Steam
  Audio's static-source and static-listener variants.

**Physics sounds** (`physics_audio.*`, `procedural_synth.*`, `assets/surface_sound.*`): what
Box3D simulates is heard while the game runs, on the Physics bus.
- **Surface sounds** say what a surface sounds like hit, scraped or rolled on:
  - an impact, a scrape and a roll sound (`.fsound` or clip; an impact `.fsound` with
    variations is best, scrapes and rolls loop);
  - volume and pitch;
  - its character: hardness (a soft thud to a hard ring; also how much it lets the other side
    ring), roughness (a scrape's grain), and for the synth a ring pitch, ring time and noise;
  - Synthesize: without a sound set, one is synthesized from the character (else it is silent).
  - Presets: generic, wood, concrete, brick, rock, metal, glass, ceramic, plastic, gravel, sand,
    dirt, grass, snow, carpet, cloth, water, foliage.
- **Where they come from** (as the acoustics do):
  - a material (`.fmat`, "surface"; a Surface Sounds group in its editor below Acoustics). Picking
    an acoustic preset while the surface sounds are still generic picks the same one for them;
  - a mesh entity's own (the Inspector's Acoustics > Sounds Like, with Hit Volume and Hit Pitch);
  - a terrain layer ("Hit sounds"; by default as it sounds like): Box3D's height field carries the
    layer under each cell as its user material id, so a box on sand thuds and on rock clacks;
  - anything else is generic; scatter is rock.
- **Contacts** (`physics_world.*`): dynamic shapes report hit events (over 0.3 m/s) and touches.
  Every shape's user data says whose it is (an entity, a placed object or the ground). After each
  step the touching contacts with a moving side are read: the normal and friction force, the slip
  speed at the contact point and the rolling speed. All of it is on the main thread, so a run
  sounds the same every time.
- **Impacts:** the loudness comes from the approach speed and the mass that meets the hit (two
  moving bodies: their reduced mass). Both surfaces sound, each scaled by the other's hardness:
  metal on sand is mostly a thud. Heavier bodies sound lower.
- **Scrapes and rolls:** a touching contact rolling faster than 0.15 m/s is a roll; one sliding
  under some force is a scrape. Each is a loop that follows its contact (the still or heavier
  side's sound), its volume and pitch following the speed and force, faded out when it stops or
  parts.
- **Synth:** an impact is a few damped inharmonic modes over a filtered noise transient (with a
  thud on soft surfaces); a scrape is band-limited noise with grains; a roll is a low rumble with
  bumps. Each surface's are made once (four variations of an impact) and kept.
- **Budgets:** at most 8 new impacts a frame (the loudest), 60 ms between two hits of the same
  pair, 16 scrape or roll loops (the loudest), nothing beyond 60 m from the listener.
- **Settings:** Project Settings > Audio > Physics sounds (on by default).
- **The stats line** adds `audio: physics sounds: N impacts, N scrapes, N rolls; N sliding or
  rolling now`. Sponza with 64 bodies dropped: 161 impacts, 95 scrapes and 127 rolls in 400 frames.

**Water sounds** (`water_audio.*`, `procedural_synth.*`): while the game runs, from each body's
water material.
- **A material's Sound group** (`.fwater`, and per body as overrides, like its other fields):
  - Splash and Slosh sounds (a `.fsound` or clip each; none: synthesized), each with a volume;
  - Underwater muffle (how muffled the world is from under it), and an Underwater sound (none: a
    low synthesized rumble) with its volume.
- **Splashes:** physics notes a body that is in the water this step and was not the last, moving
  down (over 0.5 m/s): where it broke the surface, how fast, its size and mass. Louder for faster
  and bigger things, lower for bigger ones. Synthesized in five sizes (4 cm to 1.2 m), four of
  each: a slap of noise, the churn, a burst of bubbles (each ringing at Minnaert's frequency for
  its size, rising), a thump and the drops falling back for big ones.
- **Slosh:** a body at the surface moving through the water (its flow taken out) plays a loop
  that follows it, its level with its speed and size.
- **Under water** (the listener inside a body, as the renderer decides):
  - the world's buses (effects, physics, ambience; not music or UI) are low-passed in the mixer,
    from 20 kHz down to 350 Hz at full muffle, and a quarter quieter, over 0.15 s;
  - a sound across the surface (above it while the listener is under, or under it while above)
    loses most of it, more in the highs (45%, 10%, 2% per band at full muffle);
  - the underwater loop plays.
- **Budgets:** 6 new splashes a frame (the loudest), 8 slosh loops, nothing
  splashing or sloshing beyond 80 m.
- **Settings:** Project Settings > Audio > Water sounds (on by default).
- **The stats line** adds `audio: water: N splashes; N sloshing now; under water`.

**Scripts** (C# API v7; `managed/Falcon.Engine/Audio.cs`, `World.cs`):
- `Audio.Play(sound, position, volume, pitch, loop)`, `Audio.Play2D(...)` and
  `Audio.PlayOn(entity, sound, ...)` (it follows the entity) return a `SoundHandle`: `Stop(fade)`,
  `SetVolume`, `SetPitch`, `SetPosition`, `IsPlaying`.
  - A sound is a project path: a `.fsound` (its own settings), or a clip (placed with the defaults
    when given a place, else 2D). Volume and pitch multiply the sound's.
  - Ids are looked up once per path.
- `Audio.PlayImpact(surface, position, volume, pitch)`: a surface preset's impact as physics would
  play it (synthesized), for footsteps without sounds of their own.
- `Audio.SetBusVolume(Bus.Music, 0.5f)`: multiplies the project's bus volume.
- Everything a game plays, and its bus volumes, are undone when it stops.
- `Physics.Contacts()`: this frame's hits, the same ones the physics sounds hear: the two
  entities (default: the scene's own geometry or the ground), point, normal, speed, and each
  side's `Surface`.
- `Physics.SurfaceAt(position, out hit, maxDistance)`: a ray straight down. It returns where it
  hit, how far down, the `Surface` there (an entity's own, its material's, a terrain layer's) and
  whose it is.
- `Surface` is the 18 presets, plus `Unknown` for a surface with its own values.
- **The Sandbox's footsteps:** the walking characters (the foxes) step faster, louder and a
  little higher as they speed up. Each step asks `Physics.SurfaceAt` what is underfoot and plays
  `Footsteps/<Surface>.fsound` (`Footsteps/Grass.fsound`, ...) when the project has one, else that
  surface's synthesized impact.

**Project settings** (File > Project Settings > Audio, saved in `project.falcon` for the
editor and the game alike):
- master volume, and a volume per bus (effects, physics, ambience, music, UI);
- the HRTF switch (off: 3D sounds are panned);
- reflections (off, low, medium, high), the reverb level, and the reverb type (from the next start);
- physics sounds and water sounds on or off;
- **Sound variation** (on by default): every one-shot sound that doesn't vary on its own plays a
  little different each time, about ±1 dB and ±0.5 semitone, and with several clips never the same
  one twice in a row. This covers `.fsound`s without amounts, clips on Audio Sources, scripts' and
  physics' clips. Loops and the Music bus are left as they are, and so are previews and test sounds.
  A sound's or an Audio Source's **Vary** off plays it exactly as made.

**How it works:**
- **Mixer** (`engine/audio/audio_mixer.*`):
  - 48 kHz stereo float, mixed in blocks of 512 frames (10.7 ms, Steam Audio's frame).
  - The main thread sends commands (play, stop, set, listener, bus gain) through a lock-free
    ring and hears back about finished voices through another. The mixer thread never locks or
    allocates.
  - Voices are clips or test tones, with gain, pitch (linear resampling), looping and fades.
    Gains and pans are ramped across each block, so changes don't click.
  - Buses: Master, Sfx, Physics, Ambience, Music, Ui.
- **Spatial voices** (Steam Audio, `steam_audio.*`):
  - Each voice has a direct effect for air absorption (Steam Audio's model, 3 bands: at 100 m a
    10 kHz tone keeps about a quarter of its level, a 500 Hz one 94%) and a binaural effect
    (HRTF). 32 voices are spatialized at once.
  - Distance: Steam Audio's inverse-distance model, scaled to full volume within the minimum
    distance (its own gives 1 / max(distance, min)), faded out over the last tenth of the
    maximum. Linear rolloff is a straight line from the minimum to the maximum.
  - A voice beyond the 32, or a build or machine without Steam Audio, is panned (equal power;
    no air absorption).
- **Clips** (`clip_cache.*`): `.wav` (SDL) and `.ogg` (stb_vorbis), converted to 48 kHz float
  with at most 2 channels, decoded once and kept by path.
- **Sounds and the scene** (`audio_system.*`): `.fsound`s are read once (again after an edit);
  variation uses a fixed seed, so a run sounds the same every time. A source's voice id is in
  its `AudioSource.voice` while it plays. Sound paths become ids for the components
  (`soundId`); the edits save the paths.
- **Device** (`audio_device.*`): the default playback device (SDL's thread pulls blocks), or
  the null device. `--hidden` and `--audio-null` mix on the main thread from the frame clock.
  Under fixed time that is exactly 800 samples a frame, so a test run's capture is the same
  every time. `--audio-capture out.wav` writes either one's mix (32-bit float stereo).
- **Cost:** about 0.03% of real time to mix silence, and 0.09% for one HRTF voice (profile
  build).
- **Stats:** the stats line every 2 s reports `audio: N voices (M HRTF), mixing X% of real
  time`.

**Tests** (`falcon_wavdiff` checks the captures):
- `ctest -R audio_` (no scene needed):
  - a 2D tone at its level;
  - a repeat run identical to the sample;
  - a 44.1 kHz WAV played back at 48 kHz as the same tone;
  - a tone to the right or left only in that ear (panned), and louder in that ear through the
    HRTF (2 kHz, where the head shadows it);
  - 10 m away is a tenth of 1 m away;
  - air absorption at 100 m, for a 10 kHz tone and a 500 Hz one.
- `editor_audio_occlusion(_reload)`: a wall between the view and a previewed source occludes it
  and lets through its material's share (generic, its own concrete or foliage, undo, left out, a
  `.fmat`'s, the `.fmat` edited); moved aside, it doesn't; reopened, its own acoustics are kept.
- `editor_audio_terrain`: a source under a sand terrain is blocked; the layer sounding like foliage
  lets most through.
- `editor_audio_reverb`:
  - in the open, the listener's reverb is next to nothing;
  - six cube walls make a closed concrete room, which rings (RT60 over 0.8 s);
  - a source in it gets echoes of its own;
  - carpeted, the room hardly rings.
- `audio_sponza_*` (the render tests' Sponza cook): a 50 ms burst with reflections off leaves
  silence after it; with them, its reverb. Two runs agree within 0.01.
- `audio_physics_drop_*`: 64 bodies dropped in Sponza, synthesized sounds only: the capture has
  sound, and a second run is identical.
- `editor_audio_physics`: a crate dropped on a concrete floor sounds an impact, a frictionless
  box sliding down a ramp a scrape, and a ball on it a roll; Stop puts them back.
- `audio_water_splash_*`: 64 bodies dropped into a pool in Sponza: splashes play, and a second run
  is identical. `audio_water_muffle_check`: a 4 kHz tone keeps under 2% of its level heard from
  under the pool's surface (`falcon_wavdiff --band 4000`: the tone's level alone, without the
  water's own sounds).
- `audio_script_footsteps_*`: the Sandbox's foxes walking (synthesized steps, heard from beside
  one): it logs its footsteps, the capture has sound, and JIT and NativeAOT give the same samples.
- `editor_audio_scripts`: a project game (built by Play) plays a sound at a point and stops it,
  plays a clip on a ball it drops onto a concrete floor (following it) and re-pitches it, and turns
  a bus down. The HUD shows what plays, what the ball hit (`Physics.Contacts`) and what is under a
  point (`Physics.SurfaceAt`: concrete, 2 m down). Stop silences it.
- `editor_audio_water`: a pool's Sound fields (the preset's, an override set and reset); a crate
  and a ball dropped in splash; the listener under its surface is muffled;
  Stop silences it all.
- `editor_audio_bake`, `audio_bake_cli`, `editor_audio_bake_reload`:
  - two concrete rooms joined by a doorway are baked in the editor (45 probes, reverb and paths,
    written to `Scenes/Main.audio/`);
  - in the rooms the listener's reverb comes from the probes, out of their reach it is realtime;
  - a source behind the dividing wall is occluded, finds its path through the doorway, and is
    heard along it;
  - moving the wall makes the bake out of date ("objects moved"), and putting it back makes it
    up to date;
  - the scene is baked again headless (`--audio-bake`);
  - reopened, it is baked and up to date (the editor and the game fingerprint the scene alike);
  - Clear removes the baked files and keeps the settings.
- `editor_audio(_reload)` (the project fixture):
  - a clip imported and made a sound;
  - an Audio Source added, set, undone and redone, shown in the Inspector, previewed;
  - an Audio Listener added;
  - Play plays the source and Stop stops it;
  - a clip placed in the scene;
  - the master volume saved, and all of it there again on reopening.

## C# scripting (M7)

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
- **Inverse kinematics (API v12):** see Animation below. `Animation.SetFootIk(character, weight)` scales the
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
  see Particles and decals below.
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

### Project scripts

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

### Shipping a game (S8)

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

### Scenes in process (L1–L4)

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

### Rendering from C# (R1–R5)

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

### Transparency (T1)

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

### Vertex functions (V1)

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

### Engine systems from C# (X1)

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

### Libraries

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

#### Editor code (API v21)

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

#### Custom UI: editor windows and the game's HTML UI

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

## Networking: the deterministic simulation

Networking is being built on one deterministic core. The plan: client prediction against an
authoritative server by default, full-world input rollback as an option, QUIC and Steam
transports, and thousands of players per server. The first part is the core: given the same
inputs, the game simulates **the same bits** on every x64 PC, with any number of threads, in the
editor's JIT and in a shipped NativeAOT game.

**Ticks.** The simulation runs in ticks, the 60 Hz physics steps. Each tick runs in this order:

1. The tick's input.
2. `OnFixedUpdate` on systems, then components.
3. The physics step.
4. That step's `OnCollisionEnter` / `OnTriggerEnter` / `OnTriggerExit` (now delivered per tick, no
   longer once per frame in the after-physics phase).

Game logic that must agree across replays and the network belongs there. `OnUpdate` /
`OnLateUpdate` are presentation: cameras, UI, effects.

Inside a tick:

- `Time.InTick` is true.
- `Time.Tick` counts ticks since the game started.
- `Time.Delta` is `Time.TickDelta` (1/60 s).
- `Time.Total` is `Tick × TickDelta`.
- Input is the tick's:
  - `Input.Pressed` / `Released` and actions' edges compare against the last tick, so a press counts once whatever the frame rate.
  - A tap shorter than a tick still shows as down for the next tick.
  - `Input.MouseDelta` is the motion since the last tick.
- Dynamic bodies' entities hold the step's pose, not the render's interpolated one, from the tick's start until the frame's ticks are done.
- Water buoyancy samples the tick's time (`WaterSystem::setSimTime`), so floating things don't depend on frame times.

**Deterministic math.** These are the float hazards and what the engine does about each:

| Hazard | What the engine does |
|---|---|
| `MathF`/`Math`'s `Sin`, `Cos`, `Atan2`, `Exp`, `Pow`… call the C runtime, whose results depend on the CPU (FMA paths) and the Windows version. | `DMath` (C#) and `fx::dmath` (C++) compute them from `+ - * / sqrt` in double, within ~2e-16 of the C runtime. Engine code on the simulation's path uses `fx::dmath`: animation's slerps and IK, the character mover's slope, water and ocean sampling. `falcon dmath-hash` and the Sandbox's log both give `38d7ca740fd695b0`, the same for MSVC, the JIT and NativeAOT. |
| `System.Numerics` code gives different bits with AVX on (measured: the same `Vector3` loop hashes differently). | The engine starts the JIT with `DOTNET_EnableAVX=0`, and NativeAOT games build with `IlcInstructionSet=x86-64-v2` (`Falcon.Game.targets`). The editor and shipped games then compute alike. |
| Floating-point control state. | The C runtime's FMA3 paths are off, and the main and job threads' SSE control word is reset (rounding, no flush-to-zero). |
| Randomness. | `Sim.Random` (`Float`, `Range`, `Chance`, `InsideUnitSphere`…) depends only on `Sim.Seed`, the tick and how many numbers were drawn before in it. A tick run again draws the same numbers. |
| Non-deterministic calls in game code. | Analyzer **FALCON100** warns about `MathF`/`Math` transcendentals, `Quaternion.Slerp`/`CreateFrom*`, `System.Random` and the clocks (`DateTime.Now`, `Stopwatch`, `Environment.TickCount`) inside `OnFixedUpdate` and the collision callbacks. It suggests the replacement. |

**Recording and replaying.** `--sim-record run.fxsim` saves:

- each frame's length;
- the input the scripts saw (keys, mouse, gamepads, text);
- each tick's hashes: Box3D's body state, and every entity's transform;
- each frame's transform hash.

`--sim-replay run.fxsim` plays those frames and that input back, and compares every tick. The first
difference is logged with its tick, frame and what differed (physics bodies, entity transforms, or
only after the frame: animation, `OnUpdate`), and the run exits with code 1.
`--sim-input-demo <seed>` plays a synthetic player instead of the real input: keys, mouse and
frame times from 4 to 40 ms. Tests use it.

| Test | What it checks |
|---|---|
| `sim_record` | The Sandbox with 256 falling bodies, 600 frames recorded (~790 ticks). |
| `sim_replay_jit`, `sim_replay_1thread`, `sim_replay_aot` | The recording replays identically with the default threads, one thread and the NativeAOT build. |
| `sim_replay_detects` | A different run (300 bodies) fails at tick 1. |
| `sim_dmath_native`, `sim_dmath_jit`, `sim_dmath_aot` | The math's bits. |
| `editor_script_ticks` | Per-tick input edges (including a one-frame tap), `Time` in ticks, collisions inside their tick. |

**Networked objects and rollback (API v37).** The networked simulation is made of
`NetworkObject`s (Add Component > Network Object in the editor, or `AddComponent<NetworkObject>()`).

- **What an object's state is:**
  - its transform (unless `SyncTransform` is off);
  - its components' **`[Sync]`** fields and properties. Supported types: bool, byte, int, uint, long, ulong, float, double, enums, string, Vector2/3/4, Quaternion, Entity. The source generator writes the code, with no reflection. **FALCON003** warns about members it can't sync: private, readonly, or another type.
- **Each object has a `NetId`** that names it on every machine. Ids are given in the order objects wake: the scene's in scene order, then in the order the simulation spawns them. `Network.Find(id)`, `Network.Of(entity)` and `Network.Objects` (in NetId order) look them up.
- **`Network.Destroy(entity)`** only puts an object to sleep (`Dormant`, disabled) while a rollback could still need it, and destroys it two windows later.
- **`CharacterMover`** is a built-in component: a character moved in ticks. `Move(wishVelocity, jump)` once per tick accelerates (with air control), applies gravity and jumping, then slides, climbs steps and keeps to slopes through Box3D's mover. Its `Velocity` and `Grounded` are `[Sync]`, so a rollback restores them.
- **Ticks are self-contained,** so a tick runs the same whatever frames it falls in:
  - components queued since the last tick (a spawned prefab's) are made first;
  - whether one runs is its own and its entity's enabled state at that moment;
  - one that hasn't started starts there (`OnStart` before its first `OnFixedUpdate`).
- **The history:** when the simulation keeps one (`SimHistory`; FxManagedApi `simControl`), each tick's state is saved after its step and events. A rollback restores the state after an earlier tick:
  - objects spawned since are destroyed;
  - dormant ones are woken;
  - transforms and `[Sync]` members are put back.

  It then simulates the ticks after it again, with the input each tick had (`ScriptHost::rollback`). Only the networked objects' components run then, and the physics world isn't stepped. That is the authoritative mode's prediction: predicted logic moves with movers and queries, and doesn't rely on collision callbacks.
- **Checking a re-run tick:** each tick simulated again is compared with the first time's. The first difference is logged with the object, its name, and which component's `[Sync]` members or transform differed.
- **Not restored:** state a component keeps outside `[Sync]` members is its own.
- **`--sim-rollback K`:** every frame, the game is rolled back K ticks and simulated forward again. It exits with code 1 if any tick comes out differently.
- **Arena** (`managed/Arena`, `bin/scripts/arena/{jit,aot}/Arena.dll`) is the networked sample game:
  - six players with CharacterMovers (player 0 on the keyboard and mouse, the others bots choosing with `Sim.Random`);
  - `[Sync]` health, score, aim and cooldowns;
  - projectiles spawned and destroyed in ticks, hitting players.
- **Tests:**

| Test | What it checks |
|---|---|
| `sim_rollback_arena` | 1,200 frames of the synthetic player, rolled back 8 ticks every frame: about 9,500 ticks simulated again, all identical. |
| `sim_rollback_arena_aot` | That recording replays identically on NativeAOT while rolling back 12 ticks every frame. |

**Transports (`engine/net`, the `falcon_net` library).** A transport carries connections, each with:

- reliable messages, on 4 channels, each channel ordered, up to 64 MB each;
- unreliable datagrams, about 1,200 bytes each.

Everything is polled from the game's thread. Addresses name the transport:

| Address | Transport |
|---|---|
| `quic://host:port[#sha256]` | **QUIC**, through Microsoft's MsQuic 2.6.3 (MIT, its OpenSSL build). |
| `loopback://port` | In process: a listen server's own player, tests. |

**QUIC** (MsQuic is fetched by `scripts/fetch-msquic.ps1` into `external/msquic`):

- `msquic.dll` is loaded at run time from `bin/net`. A build or machine without it runs without `quic://`.
- Reliable channels are one unidirectional stream per channel and direction. Unreliable messages are QUIC datagrams (RFC 9221).
- ALPN `falcon/1`. Keep-alive every 2 s, idle timeout 10 s.
- **The server's certificate:** the server makes itself a self-signed one at start, a fresh ECDSA P-256 key.
  - It is made with BCrypt alone, the X.509 structure written by the engine. No Windows key store or certificate store is touched, and nothing prompts (Windows' certificate APIs can ask for a smart card).
  - Its SHA-256 is logged. A client can pin it (`quic://1.2.3.4:27015#<sha256>`): another certificate is then refused.
  - Unpinned clients rely on the session's own authentication.

**The conditioner** simulates network conditions around any transport: one-way latency, jitter, datagram loss and duplication, and a bandwidth cap (`"latency,jitter,loss[,duplicate[,kbps]]"`).

- Reliable messages are only delayed, in order per channel.

**Tests** (`falcon_net_test`, two endpoints in one process):

| Test | What it checks |
|---|---|
| `net_loopback` | Ordered channels, a 4 MB message, datagrams, disconnects. |
| `net_conditioner` | 40 ms latency, 10% loss, 5% doubles. |
| `net_quic` | On localhost: a 10 MB message beside 2,000 small ones on three channels, 500 datagrams, the server answering. |
| `net_quic_pinned` | The pinned certificate accepted, a wrong pin refused. |
| `net_quic_loss` | 20 ms and 10% loss: about 900 of 1,000 datagrams, every reliable message. |
| `net_quic_reconnect` | Connecting again after a disconnect. |
| `net_quic_idle` | A silent connection times out; keep-alive holds one open. |

0-RTT reconnects are not done yet.

**Sessions and replication (API v38, `Network/Session.cs`).**

**Starting a session:**

- In code: `Network.Host(port)` or `Network.Connect("quic://host:port[#sha256]")`.
- On the command line: `--host [[address:]port]` (default 27015, 0: any; with an address such as `127.0.0.1:0` it listens there only, so a local test raises no firewall prompt) or `--connect <address>`, read before the game's `OnStart`. In code: `Network.Host(port, scheme, address)`.
- `Network.Role`, `IsHost`, `IsClient`, `Connections`, `Local`, `HostConnection`.
- The `Connected` and `Disconnected` events, and `INetworkListener` on the Game, systems and components.

**The handshake:** the client sends the protocol and a hash of the game's component classes. A client of another build is refused with the reason. The host welcomes it with its connection id and tick.

**What the host sends each client:**

- **Spawns:** each NetworkObject once, reliably (channel 1), in NetId order: its recipe and its first state.
  - The recipe is the prefab it came from (`Network.Spawn(prefab, position, rotation, owner)`), or its components by class with their `[Property]` values (objects built in code).
  - The client builds the same object under the host's NetId.
  - The scene's own objects already exist there with the same NetIds, so they only take the state.
  - Late joiners get everything that exists.
- **Despawns:** when an object goes, reliably.
- **Snapshots:**
  - At `SendRate` (30/s by default), the state of each object that changed since the client last acknowledged it, in datagrams that fit the path.
  - An object's state is encoded once a send for all clients (`ObjectState`: its transform and its `[Sync]` members). Each datagram is acknowledged; what a lost datagram carried goes again.
  - A state too big for a datagram goes reliably.
  - Entity references (in `[Sync]` members and RPC arguments) travel as NetIds.

**On clients, the host's objects are proxies:**

- Their components don't run `OnFixedUpdate`; the host's simulation is the one.
- Their `[Sync]` members take the host's values as they come.
- Their transforms are interpolated `InterpolationDelay` (0.1 s) behind the newest host tick heard of.
- A client's own objects get NetIds of their own (from 2^62) and aren't sent.

**RPCs:** `[Rpc.Broadcast]`, `[Rpc.Host]` and `[Rpc.Owner]` on a component's methods (`Reliable`, `AnyCaller`).

- The source generator intercepts every call to one (C# interceptors, enabled for `Falcon.Generated`), so it is called like any method.
- The arguments are sent to where it runs, then it runs here too when it should:
  - a broadcast runs everywhere, a client's going through the host;
  - a host RPC runs on the host;
  - an owner RPC runs on the object's owner.
- The host only takes a client's call when the client owns the object, unless `AnyCaller`.
- While one runs from the network, `Rpc.Caller` is who called it.
- **FALCON004** warns about methods that can't be RPCs: static, non-void, private, ref parameters, or unsendable types.

**Testing many machines on one:**

- `--net-clients N`: the host starts N processes of the same game connecting to it (hidden, with `--net-child`: each ends when the host goes), and its run fails with theirs. The tests host on `127.0.0.1:0`, as does the editor's Play with clients.
- `--net-sim latency,jitter,loss` applies to every machine.
- `--net-verify`: each client reports hashes of the states it holds, and the host checks them against its own history of changes.

| Test | What it checks |
|---|---|
| `net_session_jit`, `net_session_aot` | An Arena host with two late-joining clients, everyone at 40 ms ±10 and 10% loss. About 1,000 states checked, all identical. |
| `net_session_rpc` | The host's unreliable Hit broadcast heard on a client. |

**Prediction (API v39, `Network/Prediction.cs`).** A component driven by a player's input implements
`IPredicted<TInput>` (TInput: any unmanaged struct).

- **`BuildInput(ref input)`** runs each tick where the object's owner is (`NetworkObject.Owner`: a client, or the host for -1). It reads the keyboard, the mouse or an AI's choice.
- **`Simulate(in input)`** runs with that input:
  - on the host;
  - on the owning client, as a prediction (`Network.IsPredicting`: don't spawn networked objects or send RPCs there).
- Without a session it simply runs BuildInput, then Simulate.

**How a client predicts:**

- **Inputs:** it sends each tick's inputs to the host in datagrams, every one also carrying the 7 before it, so lost packets cost nothing. The host keeps them by tick and repeats a client's last input when one is missing.
- **Keeping ahead:** the client measures its round trip and the host's tick with pings the host echoes. It keeps its own ticks half a round trip plus 3 ticks ahead of the host's, so its inputs arrive in time.
  - **Late inputs:** each pong also says how early the client's inputs came lately (the fewest ticks ahead of the tick that used them). Busy frames on either side (a host running several ticks a frame, a client sending several ticks' inputs at once) eat into that, so the client's lead grows at once when they came with fewer than 2 ticks to spare, up to 20 more, and shrinks slowly while there is room (`Network.InputLead`).
  - **Small drift:** its simulation runs a little faster or slower (time dilation, 0.9 to 1.1×; `simSetRate`).
  - **Large gaps:** at the start, after a stall, or whenever its ticks fall behind the point where its inputs would come late, its tick number jumps once (`simSetTick`). Its inputs for the ticks from there on start again: the host drops those it had for the old ones (input datagrams carry the jump count, protocol 7).
- **Checking:** the host's state for the client's own object at tick T is compared with the state the client predicted for T.
  - Deterministic code predicts it exactly.
  - Otherwise (another player's hit, a lost input) the client takes the host's state and simulates the later ticks again with their inputs.
  - The first correction is logged with what differed.
- **The numbers:** `Network.PredictedTicks`, `PredictionChecks`, `Corrections`, `TickJumps`, `RoundTrip`; on the host, `InputsUsed` and `InputsMissed`.

**Lag compensation:**

- A built-in **`Hitbox`** component (a capsule) is recorded on the host every tick for a second.
- **`LagCompensation.Raycast`**, while the host simulates a client's input (`Network.SimulatingFor`), tests the hitboxes where that client saw them: the host tick it was showing, sent with its inputs. Elsewhere it tests them where they are.
- Arena's right mouse button fires a lag-compensated rail.

**Arena:** each client that joins gets a player of its own, which it predicts. Its synthetic player then drives it; in a session the synthetic input keeps real frame times.

| Test | What it checks |
|---|---|
| `net_predict` | Two predicting clients at a 150 ms round trip with jitter and 5% loss. About 440 predictions checked per client, about 4% corrected (the start, hits by others). A client fails with fewer than 50 checks or more than 1 in 10 corrected. |

**The dedicated server (`--server`).** The game with no window, GPU device, browser or audio device.

**What runs:**

- the scene's entities, from the pak, read on the CPU and never uploaded;
- physics, water, terrain, animation and bones;
- particles, decals, prefabs;
- the game's scripts.

**How it runs:**

- **Pacing:** 60 ticks a second, sleeping between frames.
- **Hosting:** it hosts a session on port 27015 unless `--host` gives another.
- **Terrain:** tiles and their collision load around every networked character (a NetworkObject with a CharacterMover). The game reports them each frame (`simFocus`, API v40), and clients' and single-player views stream around them too. A server has no camera to stream around.
- **Limits:** `--max-players N` refuses clients beyond N. `--password` makes clients give the same one (clients pass `--password` too; `Network.MaxConnections`, `Network.Password`).
- **Console:** lines typed on its standard input are commands:
  - `status`: the players with their ping, loss and traffic;
  - `kick <id> [why]`;
  - `say <text>`: shown on every client, `Network.Message`;
  - `quit`;
  - anything else goes to the game: `Network.Command`.
- **Build Game** also writes `<Name> Server.cmd` beside the game: the same executable with `--server`.

| Test | What it checks |
|---|---|
| `net_server` | A server with two verified client processes. |
| `net_server_full` | A one-player server refusing the second. |

**Thousands of players.** One server holds 1,000 players in one world. The engine's part of a send costs about 30 ms at 1,000 players.

**What a client is sent:**

- **Interest:** an object with a `RelevanceRadius` reaches a client within that distance of one of the client's own objects. It stays until it is 1.2 times that far away. The host finds them through a grid of 32 m cells, rebuilt once a send. Objects without a radius go to everyone, and a client always gets its own.
- **Budget:** `Network.ClientBandwidth` (bits a second, default 512,000) covers each client's spawns and snapshots:
  - Spawns go nearest cells first, up to three quarters of a send's share. The rest arrive at later sends.
  - Changed objects go by priority: the client's own first, then the ones that have waited longest. Objects that don't fit wait, and their priority grows.
  - Everyone's objects, a client's own objects, despawns and RPCs always go.
- **Encoded once:** each object's state is encoded once a send, for every client. Spawns refer to recipes, a prefab or components with their properties, sent to each client once and then named by hash (protocol 5).
- **Fewer messages:** a send's reliable messages (spawns, despawns) go to each client as one batch.
- **Parallel:** with 8 clients or more, each client's packets are built on the thread pool. Sending stays on the game thread.

**For game code:**

- **`Network.Overlap(centre, radius, results)`** finds the networked objects near a point, in NetId order. It is cheap enough for thousands of queries a tick, because it uses a grid built at the tick's first query. An object that moved more than a metre since then, or was spawned since, is found from the next tick on.
- **Arena's projectiles** look for players this way. Before, each one checked every object every tick: about 300 ms a tick at 1,000 players. Now it takes about 6 ms.

**QUIC threading:** MsQuic is called only on the game thread. Its callbacks only set flags and queue events, and a connection's handle is closed when the game polls its disconnect. (A lock shared with the callbacks deadlocked a server when 200 bots left at once.)

**Bots: `--bots N --connect <address>`.** One headless process runs N clients over QUIC:

- each does the full handshake;
- the game's own `BuildInput` gives them input;
- they acknowledge snapshots but don't simulate.

Several bot processes can join one server. `--bots N --net-clients 1` on a server starts one bot process itself.

**Server statistics:** the server logs its stats every 5 seconds; `Network.ServerStats` holds the latest. They show:

- how long encoding, clients' packets and sending took;
- traffic out, split into spawns, snapshots and other;
- objects per player;
- the game code's time per tick.

**Measured on the development machine,** with Arena in Sponza: a 60 Hz server on QUIC, and bots in 5 processes of 200 on the same machine, all within the courtyard.

| Players | Objects | A send (encode / clients / send) | Out a player | Objects a player | Game code a tick |
|---|---|---|---|---|---|
| 150 (`net_load`) | ~650 | 2 / 1 / 0.4 ms | ~450–515 kbit/s | ~270 | ~5–10 ms |
| 1,000 | ~4,300 | 12–25 / 5–18 / 5–14 ms | ~330–515 kbit/s (spawns ~half) | ~250–400 | ~5–10 ms |

Arena is a stress test, not a typical game. Every bot fires five shots a second inside a ~60 m courtyard, so a few hundred objects are within reach of every player. A game spread over a larger map sends far less. Lower `ClientBandwidth` to trade update rate for traffic.

| Test | What it checks |
|---|---|
| `net_load` | A server with 150 bots in one process: the server's stats show all 150 playing and being sent objects. |

**Not on a server yet:** scatter collision (it comes from the GPU), scene changes for every machine, and a Linux build.

**Not done yet:**

- Owner-written `[Sync(FromOwner)]` members, `[Change]` callbacks, smoothing of visual corrections.
- The Desert soldier as a networked player.
- Scene changes for every machine.
- Sharding across several servers (NetIds and connections leave room for it).
- `[Sync]` members marked in the Inspector.

**In the editor.**

**Project Settings > Network**, saved with the project (`"network"` in `project.falcon`):

- A preset sets the rest:

  | Preset | Players | Snapshots a second | Budget a client | Other |
  |---|---|---|---|---|
  | Competitive | 64 | 30 | 512 kbit/s | |
  | Large battle | 1,000 | 20 | 256 kbit/s | |
  | MMO / survival | 2,000 | 10 | 128 kbit/s | |
  | Rollback | 8 | | | the whole world from inputs |

  Changing any value makes it Custom.
- **Fields:** the mode, snapshots a second, bandwidth a client, max players, interpolation delay, rollback mode's input delay and window, and the default port.
- **Where they apply:** the game gets them at its start, in the launch JSON's `"project"`. That holds in the editor and in a built game alike. Command-line flags win over them.

**Play with clients:**

- The Play bar's **Clients** (alone, or +1 to +4) has Play start client processes of the scene as saved. The editor's own game hosts in the project's mode, and the clients join.
- The clients are windowed, or hidden when the editor is. They play under the chosen **Conditions**: no lag, LAN (5 ms), internet (50 ms, 1% loss) or bad (125 ms, 5% loss), each way.
- Stop ends them.

**The Network tab** (beside the Profiler) shows the running game's session four times a second:

- its role, mode, tick, port and objects;
- each player's ping, loss, traffic and SteamID;
- the host's send timings and traffic;
- a client's predictions and corrections;
- rollback's rollbacks, waits, late inputs, hash checks and desyncs.

The game reports it through `Network.Status()`, a JSON string, which the API v45 export `netStatus` hands to the editor.

| Test | What it checks |
|---|---|
| `editor_network` | A preset sets the rest and a change makes it Custom; Play with a client: it joins, and the Network tab shows the host with one player; Stop ends it. |

**Rollback mode: the whole world from inputs (`Network.Mode = Rollback`, `--net-mode rollback`).** Every machine simulates everything, physics included, from every player's input. This is the GGPO-style mode, for 2 to 8 players: fighting games, RTS, physics puzzles. Bandwidth is tiny, only inputs are sent, and there is nothing to replicate.

**Inputs:**

- A player's input for a tick is built `Network.InputDelay` ticks earlier (default 2) and sent to the host, which passes it on to everyone. Each datagram also repeats the last 8 ticks' inputs.
- A tick whose remote inputs haven't arrived runs with each player's last input repeated: a guess.
- When the real input differs from the guess, the world goes back to the tick before it and is simulated again to now. That restores the scripts' kept state and physics', and re-runs each tick with its inputs.
- A frame rolls back at most once a tick, to the earliest tick that needs it.

**Never too late:**

- A machine never gets further ahead of the inputs it has than a rollback can reach (`RollbackWindow`, default 32 ticks). It runs fewer ticks that frame, or none while waiting (`RollbackStalled`): a hitch, never a desync.
- Clients keep to the host's tick by time dilation only. They never jump, because every tick must be simulated with its inputs.
- They aim using the lowest recent round trip: slow frames would inflate it.

**Desyncs:**

- Every 30 ticks each machine hashes a tick everyone's inputs are in for: the scripts' live state plus physics' state.
- The host compares the others' hashes with its own and logs a desync (`RollbackStats`).
- Dormant objects aren't in the hash: whether one is still listed depends on when it went.

**Starting:** everyone waits, paused, until the host has its players. Those are `--net-clients N`, or `MaxConnections`. Then all start at tick 0 together.

- Each player's objects are made on every machine (`INetworkListener.OnConnected`, in connection order). No objects get client-local ids, and there are no proxies.
- There is no late join yet.

**The rule for game code:** `BuildInput` only reads input. It runs on its player's machine alone, so changing simulation state there desyncs everyone else. Arena's bots now choose in `Simulate`, from `Sim.Random`.

**Physics rollback (`engine/physics/rollback/physics_snapshot.c`):**

- It is a fork of Box3D's world snapshot, built into Box3D's library (with its flags, so its struct layouts) without touching the submodule.
- Geometry is kept by pointer instead of copied and hashed. A snapshot holds a reference on each hull it uses.
- Bodies', shapes' and joints' user data are kept.
- The engine's own body records are kept with it: entity bodies, pools, joints, touching contacts, water.
- **When:** each step's state is kept after the step and its events, where the scripts keep theirs.
- **Restoring** puts both back, then each later step is taken again with its tick's kept input inside it (API v43's flag 2). Re-simulated steps' events are dropped, since they were heard already, and the render's in-between pose is put back.
- **Static collision:** terrain or scatter streaming in drops what was kept, since the kept worlds point at it.
- **Cost:** in Arena (Sponza plus a pile of crates) a kept state is 160 KB, kept in 0.03 ms and restored in 0.06 ms. `--sim-rollback K` now rolls physics back too: 9,544 steps came out identical.
- **API v44:** `simWindow`, `simRollback`, `simPhysicsHash`, `simMaxTicks`. `simSetRate` takes 0 (no ticks).

**Measured:** 3 players (a host and two client processes) at a 100 ms round trip with 2% loss, over 3,000 frames:

- about 850 rollbacks each (about 7 ticks back on average);
- a few waits for inputs;
- no input too late;
- about 100 hashes checked: no desync.

**Cost of game code under rollback:** every rolled-back tick runs the game's code again. In Arena that cost is the character mover:

- A move in Sponza's atrium costs about 0.45 ms: its capsule queries against the scene's mesh take 25–85 µs each.
- The mover now skips a slide pass once a cast covers the whole move, and reuses its last ground check.
- A rollback session's ticks need cheap simulation code.

| Test | What it checks |
|---|---|
| `net_rollback` | Three processes in rollback mode at a 100 ms round trip with 2% loss: rollbacks, no input too late, the host's hash checks with no desync. |
| `sim_rollback_arena`, `sim_rollback_arena_aot` | Now with physics rolled back too (and a pile of crates the shots knock about). |

**Not deterministic yet:**

- Terrain and scatter colliders stream around the view camera. Scatter colliders come from the GPU, a few frames late. Both will follow the players' simulated positions, computed on the CPU, with the dedicated server.
- Particles that push bodies run on frame time. They are applied in entity order, so replays match, but they are presentation and won't push networked bodies.
- The engine's fly camera (no main camera) isn't replayed.

## Steam

Games get the whole Steamworks API (SDK 1.65) from C#, with a friendly layer for the common things.

**Setup:**

- **The SDK** needs a Steamworks partner login, so it isn't downloaded. Unpack the zip you downloaded:

  ```bash
  powershell -File scripts/fetch-steamworks.ps1 -Zip <path>\steamworks_sdk_165.zip
  ```

  It keeps the public headers, `steam_api.json`, `steam_api64.dll` and the SDK's readme in `external/steamworks/` (git-ignored). The zip and each file are checked by SHA-256.
- **The build** copies `steam_api64.dll` beside `falcon.exe` (`cmake/FalconSteamworks.cmake`). The engine loads it at run time. A build or a machine without it runs as before, without Steam.
- **The App ID:** `"steam": {"appId": N}` in `project.falcon` (480 is Valve's Spacewar, for development). Or pass `--steam [appid]`; with no number it uses the project's ID, else 480.

**Starting:**

- Steam starts before the GPU, so its overlay can draw over the game.
- A development run is started as if Steam had launched it for that App ID (the `SteamAppId` environment variable). No `steam_appid.txt` is written.
- Without the Steam client running and signed in, `Steam.IsRunning` is false and `Steam.Error` says why. The game runs on.

**From C# (`Falcon.Steam`):**

| Area | What |
|---|---|
| People | `Steam.Me`, `Steam.Friends`: `SteamPlayer` with its `Id`, `Name`, `State`, `Playing`, `RichPresence(key)`, `AvatarAsync()` |
| Rich presence | `Steam.SetRichPresence(key, value)`, `ClearRichPresence()` |
| Overlay | `OpenOverlay(dialog)`, `OpenProfile(id)`, `OpenWebPage(url)`, `OpenStore(appId)`, `OpenInviteDialog(lobby or connect string)`, `SetNotificationPosition`, `OverlayEnabled`, `OverlayOpen` and `OverlayChanged` |
| The app | `AppId`, `Language`, `UiLanguage`, `IsSubscribed`, `Beta`, `Dlc`, `HasDlc(appId)` |
| The machine | `Hardware` (Steam Deck, Machine, Frame), `HardwareConfig`, `IsSteamDeck`, `IsProton`, `ServerTime`, `Country` |
| Input | `TextInputAsync(...)`: Steam's on-screen keyboard (Big Picture, Steam Deck) |
| Images | `SteamImage`: RGBA pixels, `ToPng()`, and `ToDataUrl()` for the HTML UI |

Everything is safe to call without Steam: it gives empty answers.

**The whole API (`Falcon.Steamworks`):**

- One static class per interface: `SteamFriends`, `SteamUserStats`, `SteamUGC`, `SteamNetworkingSockets`, `SteamGameServer` and the rest, with all their methods (1,018 functions).
- Every struct and callback, every enum and constant. Strings come and go as C# strings, and `bool` as `bool`.
- `Steam.On<T>(handler)` hears any callback, such as `PersonaStateChange_t` or `GameLobbyJoinRequested_t`.
- Every call that returns a call result also has an `...Async` version that gives a `Task` of its result, for example `await SteamUserStats.FindLeaderboardAsync("Feet Traveled")`.
- **Dispatch:** the engine dispatches callbacks and call results by hand once a frame, on the game's thread, before its update. A game's handlers and the results it waits for go when it stops, so hot reload is safe.
- `SteamFlat` has the raw functions, as they are.

**The bindings are generated:** `python tools/steamgen/steamgen.py` turns `steam_api.json` into `managed/Falcon.Engine/Steam/Generated/*.g.cs`, which are committed.

- **Layouts aren't guessed.** A probe built with MSVC from the SDK's own headers measures every struct's size and every field's offset, and the C# structs use them. This covers Steam's packing (8 bytes for callbacks, 1 for the networking addresses). A field whose C# type would be a different size becomes bytes of the measured size; only one does, in a nested union.
- **Constants** come from the probe too.
- **Calls** go through function pointers looked up by name in `steam_api64.dll`. That works with NativeAOT and needs no import library.

| Test | What it checks |
|---|---|
| `steam_jit`, `steam_aot` | With `--steam 480`: the signed-in player, a call result (Spacewar's leaderboard) and the player's avatar. Skipped when the Steam client isn't running or signed in. |

**Networking over Steam.** Two more transports, made by `engine/steam/steam_transport.cpp` on `ISteamNetworkingSockets`:

| Address | What |
|---|---|
| `steam://<steamid64>[:port]` | Peer to peer by SteamID, through Valve's relays (SDR). It handles NAT traversal and keeps players' IP addresses hidden. `port` is a virtual port. |
| `steamip://host:port` | Steam's sockets over plain UDP, for dedicated servers and LAN. |

- **Channels** are lanes 0–3 (reliable, each ordered); datagrams go unreliable on lane 4. Steam encrypts every connection.
- **Hosting:** a host picks its transport with `--net-transport steam|steamip` (or `Network.Host(port, "steamip")`). The client processes it launches get the matching address.
- **Readiness:** before the first connection, the transport waits for this machine's Steam certificate and Steam's network config (about half a second, once a process).

**Callbacks:** the engine owns Steam's callback queue (API v42).

- Its pump runs Steam's frame and takes the callbacks into a queue, fetching each call result as its completion arrives. The scripts read the queue each frame.
- Code that waits on Steam between frames pumps too. Steam's own work, its networking certificate for one, only moves on while its callbacks are taken.

**Auth tickets.** A client running Steam sends its SteamID and an auth ticket in its hello, on any transport.

- A host running Steam has Steam check the ticket (`BeginAuthSession`). It drops the player when Steam says no, at join or later: a VAC ban, no license, signed in elsewhere.
- `Connection.SteamId` and `SteamVerified` say who a player is.
- `Network.RequireSteam` refuses players who send no ticket.
- `Network.Kick(connection, why)` drops a player.
- Players' names are their Steam names when they run Steam.
- Protocol 8 (7 until RPC messages got a kind byte of their own: 30. At 20 they shared rollback's
  start message, so clients took every reliable RPC from the host for it and dropped it).

| Test | What it checks |
|---|---|
| `net_steam` | A session over `steamip://`: a host and one client process, verified, with the player's ticket checked by Steam. Skipped without Steam. |

**Not checked automatically:** `steam://` peer to peer needs two Steam accounts (Steam won't connect an account to itself). It is a manual check on two machines.

**Lobbies (`SteamLobby`).** A lobby is a group of up to 250 players before or around a game, with shared data, per-member data and a chat.

| What | How |
|---|---|
| Making | `CreateAsync(type, max)` |
| Finding | `FindAsync(filters on its data, slots, distance)` |
| Joining and leaving | `JoinAsync()`, `Leave()` |
| Who's in it | `Owner`, `Members`, `MemberLimit`, `Joinable`, `Type` |
| Data | `lobby[key]` (the owner writes), `Data`, `MemberData` and `SetMyData` |
| Chat | `Say(text)` |
| Invites | `Invite(player)`, `OpenInviteDialog()` |
| Events | `MemberJoined`, `MemberLeft`, `DataChanged`, `ChatMessage` |

- **Joining from Steam:** `SteamLobby.JoinRequested` fires when a friend's invite or "Join game" is accepted in the overlay, or when the game was started with `+connect_lobby <id>`.
- **Rich presence:** entering a lobby sets the player's rich presence `connect`, so friends can join from their friends list.
- **A session from a lobby:** `HostSession(port)` has the owner host over Steam peer to peer and writes the address into the lobby. Its members connect at once (`ConnectMembers`). For a dedicated server, `SetGameServer(server)` points the lobby at it.

**Dedicated servers on Steam (`SteamServer`).** `--server --steam` starts Steam's game server instead of a player's Steam (`SteamGameServer_Init`).

- **Ports:** the game port is the session's; queries go on the next port. On the default port 27015, Steam's LAN search finds the server.
- **Logon:** the server logs on anonymously, or with `SteamServer.LoginToken` (a game server login token: a persistent SteamID).
- **Listing:** it is listed while it runs, with `Name`, `Map`, `Tags`, `Description` and rules (`SetRule`). Max players and the password follow the session's settings.
- **Players:** each player's ticket is checked by the server itself (`SteamGameServer.BeginAuthSession`, VAC secure).
- **Sockets:** `steamip://` hosting uses the game server's own sockets, and the transport waits for its certificate once it has logged on.
- **`RestartRequested`** says Steam has an update for the game.

**The server browser:** `SteamServer.FindLanAsync()`, `FindInternetAsync(filters)`, `FindFriendsAsync()` and `FindFavoritesAsync(history)`.

- Each returns `SteamServerInfo`: name, map, players, ping, tags, password and secure flags. Its `Address` goes straight to `Network.Connect`.
- The browser's response object is a C++ interface. It is built in C# as a vtable of `[UnmanagedCallersOnly]` functions.

| Test | What it checks |
|---|---|
| `steam_jit`, `steam_aot` | Also a lobby: created, found by its data, left. |
| `net_steam_server` | A dedicated server on Steam (anonymous logon, VAC secure): a client verified, its ticket checked by the server, and the server in the client's LAN server list. |

**Stats and achievements (`SteamStats`):**

- **Stats:** `GetInt`/`GetFloat`, `Set`, `Add`, `AddToAverage` (rate stats).
- **Achievements:** `Achievements` (their API names), `IsUnlocked`, `UnlockedAt`, `Display(name, "name"|"desc"|"hidden")`, `Unlock`, `Clear`, `ShowProgress`.
- **Global numbers:** `GlobalPercentagesAsync()` (the share of players with each achievement) and `PlayersNowAsync()`.
- **Storing:** changes stay local until `Store()` sends them; an achievement's popup shows then, and `Unlock` stores at once by default. The engine stores what changed when the game stops.

**Leaderboards (`SteamLeaderboard`):**

- `FindAsync(name, create, sort, display)`, then `Name` and `EntryCount`.
- `UploadAsync(score, keepBest, details)` gives back whether it changed and the rank before and after.
- `EntriesAsync(global | around the player | friends, start, end)` gives `SteamLeaderboardEntry` values: player, rank, score, details.

**Steam Cloud (`SteamCloud`):**

- `IsEnabled` (the player's and the app's switches), `Quota`, `Write`, `Read`, `Exists`, `Delete`, `Files`.
- `Storage` (the game's saves) uses it by itself when it is on. A save also goes to the cloud, and a load reads the cloud's copy first: the newest, from whichever PC. `Storage.UseSteamCloud = false` turns that off.

**More on `Steam`:**

- **Screenshots:** `Screenshot()` takes one into the player's library (as F12 does); `AddScreenshot(path, w, h, location)` adds an image file.
- **Game recording timeline:** `TimelineEvent(title, description, icon, priority, secondsAgo, clip)`, `SetTimelineMode`, `SetTimelineTooltip`, and `GamePhase(start, id)`, which groups the recording by match or level.
- **Remote Play:** `RemotePlaySessions`, `InviteToRemotePlay(friend)`, `ShowRemotePlayTogether()`.

**Tests:** `steam_jit` and `steam_aot` also read the achievements, a stat, the top five entries of Spacewar's leaderboard and the cloud quota. They only read: unlocking, uploading and cloud writes would change the player's account. The leaderboard download is tried twice, since Steam's servers fail one now and then.

**The Workshop (`SteamWorkshop`):**

- **Finding:** `FindAsync(rank, page, search, tags)` (ranked by vote, newest, trend...) and `DetailsAsync(ids)`. They give back `SteamWorkshopItem` values: title, description, tags, owner, votes, score, preview URL, size.
- **Subscribing:** `SubscribeAsync`/`UnsubscribeAsync`. Steam downloads the items and keeps them updated. `Subscribed` lists them, `Installed` gives each one's folder, and there are `Download`/`DownloadProgress`.
- **Publishing:** `PublishAsync(folder, title, description, preview, tags, visibility, existing, changeNote, progress)` uploads a folder of files as a new item, or updates `existing`. It gives back the item's id, and whether the player must first accept the Workshop's agreement.

**Inventory (`SteamItems`):** each call goes through Steam's servers, and its result comes back as the player's items (`SteamItem`: instance, definition, quantity).

- `AllAsync`, `ConsumeAsync`, `TriggerDropAsync` (timed drops), `GrantPromosAsync`, `ExchangeAsync` (the schema's recipes).
- `BuyAsync` opens Steam's checkout for the item store.
- `Definitions` and `Property(definition, "name"|"icon_url"|...)` read the item schema.

**Steam Input (`SteamControllers`):**

- `Start(manifest)` turns it on with the game's action manifest. Every controller Steam supports, with the player's own bindings, then reads through it.
- **Reading:** `Digital(action)`, `Analog(action)`, `ActivateSet(set)`.
- **Prompts:** `Glyph(action, set)` gives the PNG of what to press.
- **Also:** `ShowBindings()` (Steam's binding screen) and `Vibrate`.
- **With `InputAction`:** `Input.Action("jump").BindSteam("jump")`, or `BindSteam("move", 0)` / `(…, 1)` for an analog action's x and y, so games read Steam Input through the same names as keys and pads.
- **Determinism:** Steam Input values aren't in the tick input ring, so a replay doesn't reproduce them. Over the network they travel as the inputs `BuildInput` builds.

**Voice chat (`SteamVoice`):**

- **Talking:** `Talking` (push to talk: true while its key is held) or `Open` (an always-open mic; Steam leaves out the silence).
- **The path:** the voice is compressed by Steam and sent as datagrams to the session. The host plays it and passes it on (it ignores a client's claim to speak for another).
- **Hearing:** each speaker plays through an `AudioStream`, on their character when `SpeakerEntity` names it (spatialized like any sound), otherwise flat.
- **Controls:** `Volume`, `Muted` (by connection), and `Speaking` (who was heard in the last half second).

**Tests:** `steam_jit` and `steam_aot` also query Spacewar's Workshop (its 50 top-voted items) and start Steam Input. Voice needs a microphone and a second player, so it is a manual check. Publishing isn't tested automatically: it would put an item on the Workshop.

**Not checked here:** Spacewar's inventory refuses `GetAllItems` on the development account, so the inventory calls are untested against Steam.

**Not done:** the editor's Publish to Workshop (scenes, prefabs, libraries) and Steam Input glyphs in the HTML UI.

**Shipping on Steam:**

- **Project Settings > Steam:** the App ID, the Windows depot (0: the App ID + 1, Steamworks' default), the branch an upload is set live on, and **Start through Steam**.
- **Build Game for a Steam game:**
  - The build has `steam_api64.dll` and no `steam_appid.txt`. The shipped game starts Steam for its project's App ID.
  - Started outside Steam (from its folder), it starts again through Steam (`SteamAPI_RestartAppIfNecessary`), which brings the overlay, DRM and launch options. Development runs and servers aren't asked.
  - SteamPipe's build scripts go beside the build, in `Build/<name> SteamPipe/`, so they aren't uploaded: `app_build_<app>.vdf` (description, content root, build output, the branch set live; Steam refuses to set `default` live from a script) and `depot_build_<depot>.vdf` (everything under the build, `.pdb` files left out).
  - steamcmd isn't shipped with the game.
- **File > Upload to Steam…**
  - It uploads the last build through SteamPipe. steamcmd comes from the SDK: `scripts/fetch-steamworks.ps1` unpacks it to `bin/steamcmd`.
  - steamcmd runs in a console window of its own: `+login <account> +run_app_build <vdf> +quit`. You type your password and Steam Guard code there, and they never pass through the editor.
  - The dialog asks for the account name, a description and the branch each time. Then set the build live on Steamworks, or name a branch.
- **A dedicated server on a machine without the Steam client** needs Steam's server runtime beside the executable: `steamclient64.dll`, `tier0_s64.dll` and `vstdlib_s64.dll`, from SteamCMD's "Steamworks SDK Redist" (app 1007, available anonymously). Install them with `steamcmd +login anonymous +app_update 1007 +quit`. The engine doesn't download them.

**Tests:** `editor_game_build` also builds a Steam game (App ID 480). It checks the SteamPipe scripts beside the build and that steamcmd stays out of it. `game_build_run` then starts the built game with Start through Steam off.

## Level design

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

### Live meshes

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

### Level meshes

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

### Selection, snapping and elements

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

### Commands, modal operations and the level controls

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

### Shapes and mesh operations

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

### Materials and UVs on faces

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

### Trim sheets

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

### Layered surfaces and vertex colours

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

### Live cuts

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

### Walls, rooms and openings

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

### Wear and tear

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

### Modifiers

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

### Splines

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

### Placing and arranging

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

### Navigation

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

### Views and measuring

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

### Cutting while the game runs

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

### The sample level

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

## Animation (M8)

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
- at full detail the output matches the M1 reference renderer
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
doors, a camera move) plays with the scene. Skinned characters keep their clips (M8); this is
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

## Particles and decals (M16)

Effects are built from components, like s&box's particle front end (`engine/vfx`):

- **Particle Effect**, the master, owns the particles. It sets lifetime, limits and the features: move (initial and random velocity, damping, constant movement, local space), rotation, colour (tint, gradient, brightness above 1 to glow, alpha), size (scale, stretch), force (direction or gravity, orbital, pull), collision, sheet (flipbook frames, fit to lifetime), Temporary (the entity goes when it is done) and Wait for Play (it starts stopped until a script starts it).
- **Emitters** decide how many particles start and where: burst, rate, rate over distance, delay, duration and loop.
  - Shapes are sphere, box, cone and ring.
  - A cone opens along the entity's +Y, and a ring lies across it.
- **Renderers**:
  - **Sprite**: a material's base colour with its alpha. Blend is alpha, additive or premultiplied, lit or not. It faces the camera, stretches along its velocity, stands upright or lies flat. It can show a flipbook's columns × rows, with a depth feather and camera fades. With no material it is a soft dot.
  - **Trail**: each particle's last points, with a width and a colour along it.
  - **Light**: particles as point lights.
  - **Model**: particles as meshes (shell casings, debris), each an instance of the mesh. They are lit, shadowed (or not), take decals and show in reflections like any mesh. They turn by the particle's rotation (plus an offset, or along their velocity), are sized by its size and tinted by its colour, and can draw with another material. A renderer draws at most its Max models, the oldest particles first.
- **Controllers**:
  - **Attractor** pulls toward its entity.
  - **Noise** is curl-noise turbulence.
  - **Particle Script** names a C# controller.

Any part can sit on the effect's entity or on any entity below it. It works for the nearest Particle Effect up the tree: a child's renderer draws its parent's particles.

- **Values** (s&box's ParticleFloat): constant, random range, curve over the particle's life (up to 8 Hermite keys), or a random pick between two curves. They are evaluated over life, per particle (a seed) or every frame. Colours take the same forms with gradients (8 colour and 8 alpha keys).
- **Saved** in the scene's edits and in prefabs, by key (`"particleEffect"`, `"particleConeEmitter"`, `"particleSprite"`, ..., `"decal"`). Fields at their defaults are left out.
  - One reflection table per component (`vfx/vfx_components.cpp`) drives the JSON, the Inspector, field edits and undo.
  - `falcon vfx-test` checks the values and the round trips.
- **Simulation** (`vfx/particle_system.h`) runs on the CPU, as s&box's does, after physics and the bones each frame, so a muzzle on a hand emits from this frame's hand.
  - Emitters start particles on the main thread, then the particles step on the job threads in groups of 512.
  - Collision casts a swept sphere through Box3D from the job threads (`PhysicsWorld::castSphere`: queries only read the world). Particles bounce, slide by friction, turn by bumpiness, die by chance, push the dynamic bodies they hit (an impulse) and can spawn a prefab where they hit.
  - Each effect is seeded by its entity and how often it started, so it plays the same way each time.
  - It runs in the editor's edit mode too: effects preview where they are, and a finished one starts again a second later. A paused Play holds it, and Play restarts every effect.
- **Rendering** (`render/particle_renderer.h`, `shaders/particles.slang`): one instanced pass of every sprite, sorted back to front across effects, into the HDR frame after MSAA and reflections, before TAA, FSR and the post chain.
  - **Textures** come through the virtual texture. The CPU asks for the pages a material's frames show at (graphics shaders can't write the VT's feedback).
  - **Lighting:** lit sprites take the shadowed sun, the clustered lights and the sky's ambient at their centre.
  - **Depth:** there is no depth target. The scene's depth, water included, hides what is behind and softens where a sprite meets a surface.
  - **Reactive mask:** the sprites' coverage goes to TAA and FSR, so moving particles don't smear.
  - **Trails** are drawn as stretched segments.
  - **Light Renderer** particles are short-lived point lights (`LightSystem::setTransientLights`).
- **Editor**:
  - **Inspector:** each component's section is built from the engine's schema. Feature groups have their toggle in the header. Particle values have a mode menu, a curve editor (drag, double-click to add, right-click to remove, presets) and a gradient editor (colour and alpha stops).
  - **Viewport:** the effect has a sparkle icon. Selected, its emitter's shape or its decal's box shows.
  - **Create > Effects** adds a Particle Effect (a soft dot rising and fading) or a Decal. Restart and Pause preview an effect.
- **Scripts (API v15)**:
  - `Particles.Play / Stop(clear) / Restart(entity)` act on the effects at the entity and below it, so a muzzle's flash, smoke and sparks start at once. `Emit(entity, count)`, `Count` and `Hits(entity, span)` give this frame's collisions.
  - **C# controllers** subclass `ParticleController` (`Particles.cs`) and register with `Particles.Register<T>()` from the game's `[ModuleInitializer]`. A Particle Script names one, or `Particles.Attach` attaches one in code. Each frame after the engine's step a controller gets the effect's particles as a `Span<Particle>` (128 bytes, as the engine keeps them; New, Hit and Dead flagged) and the hits. `OnStep` by default calls `OnParticleCreated / OnParticleStep / OnParticleDestroyed / OnCollision`.
  - `Prefab.Spawn(path, position, rotation, parent)` makes a prefab while the game runs (`editor/prefab_spawn.h`): its meshes, empties, lights, cameras, audio, bodies and colliders (they simulate at once), particle effects and decals. The editor's Stop deletes what the game made. `Prefab.Facing(normal, yaw)` turns +Y to a surface's normal.
  - `Decals.Place(material, point, normal, size, angle, lifetime, attach)` and `Decals.Clear()`.
- **Decals** (`vfx/decal_system.h`, `render/decal_renderer.h`): a Decal component, or one placed by a script, projects its material along its box onto what is inside it.
  - **Placed decals** are a pool of 2048. The oldest go first, and each fades in the last second of its lifetime. A decal can follow an entity.
  - **Each frame** up to 1024 visible ones are sorted by their order and binned into 32×32 pixel tiles. They are passed to the resolve as read-only storage textures (its buffer slots are full).
  - **In the resolve**, between the material and the lighting, each covering decal fades by the surface's angle to it and by the box's depth. It steps the view ray through the material's height map (parallax occlusion, more steps at grazing angles), then blends its colour (darkened by depth: Cavity), roughness and normal (turned onto the surface) over the surface's. SSR and water see the result, and MSAA reuses the resolve's pixels.
  - **Characters** (skinned meshes) take a decal only when it allows them.
  - **The height map** is the material's `"textures": {"height"}`, cooked into its ORM pages' alpha. Without one there is no parallax.
- **Limits**:
  - Particles cast no shadows and don't show in reflections or water's refraction: they are drawn after both.
  - Collision sees physics shapes, not the rendered surface.
  - **Runtime meshes** (`vfx/runtime_meshes.h`): the GPU's instance table is fixed when the scene loads, so slots are kept then for the meshes models and spawned prefabs may draw. Those are the meshes named by the scene's Model Renderers and by every prefab in the project's Assets (their mesh entities and their Model Renderers, several times over), plus the built-in shapes in the editor. A mesh chosen later shows after Reload Scene; the Inspector says so. Skinned meshes can't be particles. A particle keeps its slot while it lives, so motion vectors and shadows follow it. A spawned mesh is hidden while it or an entity above it is disabled, and goes when it or one above it goes.
  - A new material, or a changed texture, needs a cook, as for any material.
- **Tests**:
  - `vfx_values`.
  - `editor_particles` and `_reload`: parts bound to their parent's effect, undo, gravity, collision resting on a box, Temporary.
  - `_render`: drawn, velocity-stretched and lit, trails, a light on a floor.
  - `_textured`: a flipbook streamed from the VT.
  - `editor_particle_scripts`: a C# controller, `Prefab.Spawn`, `Hits`.
  - `editor_decals` and `_reload`: a crater drawn, deeper with parallax than flat.
  - `editor_particle_models`: particles drawn as cubes (at most Max models, none without slots, `_drawn`), and a game's spawned prefab of a cube with a body and a collider coming to rest on a floor, deleted at Stop.

## UI and editor (M9)

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

## Profiler

The editor's Profiler tab (Ctrl+7; Maximize gives it the window) records every frame.

**What it records:**
- **CPU zones** on every thread: `FX_ZONE` / `FX_ZONE_CAT` (`engine/core/profiler.h`), which
  still feed Tracy when it is compiled in.
  - Each zone has a category (Rendering, Scripts, Physics, Animation, UI, Editor, Wait, Other).
  - Workers are named and get Box3D's task zones (named by Box3D) and animation zones.
- **GPU time per pass** with timestamp queries (`engine/gpu/gpu_profiler.h`, our SDL patch).
  - One timestamp before each pass. A mark lasts until the next one, so the passes partition
    the frame's GPU time.
  - Sections (Culling, Visibility, HiZ, Shadows, Ambient Occlusion, Resolve, Post, GI, UI…)
    group about 85 passes.
  - Results come back through the renderer's fenced stats readback a few frames later.
- **Counters:**
  - culling and triangles per pass;
  - shadow pages and records;
  - virtual-texture residency and streaming;
  - physics bodies and Box3D's step stages;
  - animation, UI frames and messages;
  - present mode;
  - process working set and private bytes;
  - GPU memory per module.
- **GPU memory:** every SDL_GPU buffer, texture and transfer buffer is created through
  `engine/gpu/gpu_memory.h`, which records its module, name and size.

**What the tab shows:**
- **Charts** over the last 600 frames:
  - CPU main-thread time stacked by category;
  - GPU time stacked by section;
  - one chart per counter module. Their legends toggle series.
- **Selecting a frame:** click or drag on a chart (←/→ step through frames). Selecting pauses
  the view, and Current resumes it.
- **Views of the selected frame:**
  - **Timeline:** every thread plus a GPU lane on its own clock. Wheel zooms, drag pans, and
    hovering shows total / self times.
  - **Hierarchy:** merged calls, sortable, with search, for any thread or the GPU.
  - **Counters:** each counter with min / avg / max over the history.
  - **Memory:** every GPU resource by module and size.

**How it gets to the page:**
- Zones and GPU marks are recorded in the editor, and GPU marks only while the tab is open.
- The page gets `profiler.frames` ten times a second (numbers only; names, counters and threads
  go out incrementally), and one frame's full detail on request (`profiler.frame`).

**Cost:** on Sponza at 1600×900, `--profile` costs nothing measurable (2.002 vs 2.003 ms per
frame).

**Outside the editor:** `--profile` records the same data and logs CPU categories and GPU
sections every 2 s.

## Freeze reports

When the main thread stops finishing frames for 8 seconds (`--freeze-seconds`), a watchdog thread
(`core/freeze_watch.*`) writes what the process was doing to `FreezeReports/` beside the executable
(under `%LOCALAPPDATA%\Falcon Engine\FreezeReports` when that can't be written):

- `freeze-<date>-<time>.txt`: every thread's call stack, the main thread first. Function names and
  source lines come from `falcon.pdb` beside `falcon.exe`; release builds write it too (`/DEBUG`
  with `/OPT:REF,ICF`, so the executable is unchanged). If the main thread is still stuck 12 s
  later, its stack is added again: the same place means a deadlock or a long wait, another one a
  loop. Then the last 200 log lines.
- `freeze-<date>-<time>.dmp`: a minidump (threads, stacks and the memory they point at), for Visual
  Studio or WinDbg with the pdb.

The stacks are taken by suspending each thread in turn and unwinding it from the images' unwind
data. Nothing is allocated while a thread is suspended, because it may hold the heap's lock. Unwinding
stops at code outside the loaded images (C#'s JIT code); the minidump has the rest.

When the main thread runs again, it logs a warning naming the report, which the editor's Log tab
shows. Nothing is watched while a debugger is attached, because breakpoints stop the main thread too.
A minimized window still counts as running. Test: `freeze_report` (a 3 s sleep with a 1 s limit)
and `freeze_report_contents` (the main thread's stack at `freezewatch::testFreeze`, the log, the
dump).

## Runtime flags

| flag | meaning |
|---|---|
| `--driver direct3d12\|vulkan` | force a backend (default: SDL's choice, D3D12 first) |
| `--present immediate\|mailbox\|vsync` | present mode (default immediate: uncapped; falls back to mailbox, then vsync. macOS: vsync) |
| `--frames-in-flight N` | 1–3, default 2 |
| `--capture out.png [--capture-frame N]` | render with fixed time, save frame N, exit |
| `--capture-phase P` | with TAA, capture on a frame of jitter phase P (0–7; default 7) |
| `--capture-sequence N` | with `--capture`: frames N.. from the capture frame each saved as rendered (`out_NNNN.png`), nothing waited for (a moving view, frame by frame) |
| `--size WxH` | window size (default 1600x900; the editor remembers its own) |
| `--profile` | record CPU zones, GPU pass timings and counters; log averages every 2 s |
| `--hidden` | never show the window (tests): frames render to an offscreen target instead of a swapchain image, paced by waiting for the GPU. The UI gets a browser profile of its own (removed at exit), so a test never collides with an editor that is open |
| `--frames N` | exit after N frames |
| `--scene file.fpak` | load a cooked scene; with `--project`, relative to the project (without either, the M0 bring-up triangle runs) |
| `--project dir` | open a project (its folder or `project.falcon`) on its startup scene, or on `--scene`; a missing scene file is cooked empty |
| `--launcher` | the project launcher (also what no arguments at all opens) |
| `--no-relaunch` | test: the launcher and the editor log the `falcon.exe` they would start instead of starting it |
| `--launcher-script file.json` | test: replay launcher page messages (`runtime/tests/launcher_*.json`) |
| `--preview jobs.json` | render asset previews (meshes of a scene, or a scene) to PNGs, then exit; the editor runs it in the background (`engine/app/preview.h` has the format) |
| `--debug-view N` | force a view: 0 lit, 1 clusters, 2 normals, 3 instances, 4 triangles, 5 LOD, 6 pass, 7 HiZ, 8 VT mip, 9 albedo, 10 lights, 11 shadow, 12 indirect light, 13 reflections, 14 ray traced (camera rays through the BVH, coloured as 4), 15 probes (the probe GI's finest grid as spheres) |
| `--lod-pixels P` | LOD error threshold in pixels (default 1; 0 = full detail) |
| `--cull-pixels P` | skip instances whose projected radius is under P pixels (default 4; 0 = off) |
| `--no-occlusion`, `--no-cone` | disable HiZ occlusion / backface cone culling |
| `--force-retest` | debug: send every cluster through the occlusion re-test pass |
| `--dolly U` | with fixed time (captures): move the camera U units/s forward |
| `--naive` | M1 reference renderer (every leaf cluster, no culling) |
| `--vt-pages N` | physical virtual-texture cache size in 136×136 pages (default 1024) |
| `--test-lights N` | add N random point lights (many-light stress test) |
| `--no-shadow-cache` | re-render every shadow page each frame (cache reference / worst case) |
| `--shadow-budget MS` | GPU time a frame for re-rendering moved lights' shadows (default 2, 0: no cap) |
| `--moving-lights SPEED` | benchmark: the sun turns (30 degrees a second) and point and spot lights circle 1 m once a second, times SPEED |
| `--no-reflections` | no reflection pass: specular sees the ambient |
| `--shadowed-lights N` | local lights with virtual shadow maps (default 8, max 32) |
| `--camera-light D` | debug: bright shadowed point light D units in front of the start camera |
| `--sun-scale S` | debug: multiply the sun intensity |
| `--taa` | enable temporal anti-aliasing (off by default) |
| `--no-taa` | disable temporal anti-aliasing (and the raster jitter) |
| `--fxaa` | FXAA on the tonemapped frame |
| `--msaa N` | multisampled edges, N = 1 (off), 2, 4 or 8 |
| `--upscaler off\|taau\|fsr\|dlss` | render below the display resolution and upscale (TAAU: ours; FSR: AMD, Direct3D 12; DLSS: NVIDIA RTX, Direct3D 12) |
| `--upscale-quality q` | native, quality, balanced, performance or ultra (render scale 1, 1/1.5, 1/1.7, 1/2, 1/3) |
| `--render-scale S` | render scale while upscaling, 0.25–1 (default: quality) |
| `--fsr-sharpness S` | FSR's sharpening (RCAS), 0–1 (default 0.5) |
| `--no-gi` | flat ambient instead of probe GI |
| `--full-res-ambient` | AO at full resolution (the GI is sampled per pixel either way) |
| `--ao gtao\|ssao\|off` | ambient occlusion technique (default GTAO) |
| `--bloom I` | bloom intensity (default 0.06; 0 disables it) |
| `--exposure EV` | exposure compensation on top of auto exposure |
| `--manual-exposure X` | fixed exposure multiplier instead of auto exposure |
| `--look base\|P,S` | AgX look: `base` (flat) or power,saturation (default 1.35,1.4) |
| `--physics-drop N` | drop N mixed boxes and spheres in front of the start camera |
| `--max-bodies N` | dynamic body pool size (default 4096) |
| `--no-physics` | no physics world (and no dynamic pools) |
| `--threads N` | job system threads (default: half the hardware threads) |
| `--script game.dll` | run a C# game: managed assembly (JIT, hot reload) or NativeAOT DLL |
| `--no-hot-reload` | JIT: don't watch the game file |
| `--freeze-seconds S` | write a freeze report when the main thread stops for S seconds (default 8; 0: off) |
| `--freeze-reports DIR` | where freeze reports go (default: `FreezeReports/` beside the executable) |
| `--test-freeze FRAME:MS` | test: the main thread sleeps MS milliseconds at frame FRAME |
| `--script-reload-at N` | test: force a hot reload at frame N (fails the run if unloading fails) |
| `--reload-scene-at N[,count]` | test: tear the scene down and load it again in process at frame N and its multiples, `count` times (with a count: fails if memory grows from one load to the next) |
| `--anim-set Name=value` | set a float graph parameter on every character at start (repeatable) |
| `--anim-events file.fevents` | clip markup (sync events, events) for every skeleton's clips by name; projects read each model's own |
| `--crowd N` | stress test: N extra copies of every skinned instance |
| `--sim-record file` | record the run's frame lengths, input and every tick's state hashes (Networking: the deterministic simulation) |
| `--sim-replay file` | replay a recording and check every tick comes out the same (exit code 1 at the first difference) |
| `--sim-input-demo seed` | a synthetic player's input and frame times (4–40 ms) instead of the real ones |
| `--host [[address:]port]` | host a network session (default port 27015; 0: any free one); an address (`127.0.0.1:0`) listens on it only |
| `--connect <address>` | join a session (`quic://host:port[#sha256]`, `loopback://port`) |
| `--net-sim latency,jitter,loss[,duplicate[,kbps]]` | simulate network conditions on every transport |
| `--net-clients N` | the host starts N client processes of this game (hidden) and fails with them |
| `--net-child` | (given to those client processes) the run ends when the host goes |
| `--server` | a dedicated server: no window, GPU, browser or audio device; hosts on 27015 (or `--host`); console commands on standard input |
| `--max-players N` | the host refuses clients beyond N |
| `--net-transport quic\|steam\|steamip` | what a host listens with (default QUIC) |
| `--net-mode authoritative\|rollback` | how a session simulates: the host's simulation, or everyone simulating the whole world from inputs (clients get it too) |
| `--steam [appid]` | start Steam for this App ID (no number: the project's, else 480, Spacewar) |
| `--bots N` | with `--connect`: a headless process of N bot clients (load testing); on a host with `--net-clients 1`, the client process runs them |
| `--password <text>` | the host's password; a client gives it the same way |
| `--net-verify` | clients report hashes of their replicated state; the host checks them (exit code 1 on a difference) |
| `--sim-rollback K` | every frame, roll the networked simulation back K ticks and simulate forward again; exit code 1 if a tick comes out differently |
| `--view inside\|outside` | start camera: stand inside (architecture) or look from outside (objects) |
| `--editor` | editor shell over the scene (needs a UI build); edit mode until Play |
| `--ui dir\|name` | in-game HTML UI bundle for the scripts (e.g. `hud`) |
| `--no-edits` | ignore `<scene>.edits.json` |
| `--no-prefs` | editor: don't load or save the user's editor preferences |
| `--library <dir>` | editor: My Library's folder (the Project panel's Library; else the prefs' or `Documents/Falcon Library`, the project's `Cache/Library` with `--no-prefs`) |
| `--editor-script file.json` | test: replay editor page messages (`runtime/tests/editor_*.json`) |
| `--no-terrain` | ignore `<scene>.terrain/` |
| `--no-water` | water bodies not drawn |
| `--water-demo preset[:x,y,z,width,length]` | a water body in the scene (tests): a built-in material's name, its surface's centre and size |
| `--terrain N` | a new N×N-tile terrain in memory (tiles of 256 quads), centered on the origin; not saved |
| `--terrain-spacing S`, `--terrain-hills H`, `--terrain-seed N` | that terrain's sample spacing (default 0.5 m), hills (default 120 m; 0 flat) and seed |
| `--terrain-pixels P` | terrain patch levels: largest projected height error, pixels (default 1) |
| `--terrain-patches N` | terrain patch slots reserved in the scene (default 16384) |
| `--live-meshes N`, `--live-clusters N` | live mesh slots and cluster blocks reserved in the scene (editor 4096 / 16384, game 64 / 512) |
| `--terrain-radius R` | tiles kept loaded around the camera, metres (default 2048) |
| `--terrain-layers a,b,...` | generated layers on a `--terrain` (grass, dirt, rock, sand, snow; the first covers it) |
| `--terrain-scatter mesh[:density[:budget]],...` | a scatter layer per entry on a `--terrain`: a mesh, or a preset's name (`Boulders`, `Scree`, `Cliff rocks`, `Spires and arches`); or a JSON list of layers, as in `terrain.json` |
| `--terrain-generate <preset \| JSON>` | run a generator stack (a preset's name, e.g. `Mountains`, or a stack as in `terrain.json`) over the terrain at start-up |
| `--camera x,y,z,tx,ty,tz` | the start camera's position and the point it looks at |
| `-- <switches>` | the rest of the command line is for Chromium (the engine ignores it) |
| `--gpu-debug` | SDL_GPU debug mode / API validation |
| `--no-audio` | no sound at all |
| `--audio-null` | no playback device: mix from the frame clock (as `--hidden` does) |
| `--audio-capture out.wav` | also write the mix to a WAV (32-bit float stereo, 48 kHz) |
| `--audio-tone hz[:x,y,z]` | test: a sine (amplitude 0.25), 2D, or at x,y,z from the listener (x right, y up, -z ahead) |
| `--audio-play file[:x,y,z]` | test: play a `.wav` / `.ogg` once, likewise |
| `--audio-spatializer auto\|steam\|pan` | Steam Audio's HRTF when available (`auto`), required (`steam`: fail without it), or panning |
| `--audio-no-air` | test: `--audio-tone` / `--audio-play` without air absorption |
| `--audio-reflections 0..3` | reflections off, low, medium or high (instead of the project's) |
| `--audio-bake` | bake the scene's acoustics (its `<scene>.audio/` settings), write them, exit (non-zero on failure) |

Captures are deterministic:
- The dolly stops at the capture frame.
- The capture waits until texture streaming, shadow rendering, terrain tile loads and terrain layer builds are idle.
- With TAA, it also waits 64 idle frames for the history to converge, and lands on the same
  jitter phase each time.
- Exposure adapts instantly under fixed time.
- Physics steps once per frame and freezes after the capture frame. So do scripts and
  animation, which see a fixed 1/60 s step.

Boot phases are logged as `[boot] <phase> <ms since process creation>`.

## Layout

- `engine/` – runtime (core, gpu, render, anim, physics, scene, script, terrain, water, audio, ui, editor, app) and `shaders/*.slang`
- `ui/` – HTML bundles: `launcher` (project selector), `editor` (editor shell), `hud` (Sandbox HUD); copied to `bin/falcon-ui/`
- `samples/graphs/` – animation graphs (JSON)
- `managed/` – C# scripting API (`Falcon.Engine`) and the `Sandbox` sample game
- `tools/schemagen/` – shared ECS component schema → C++ / C#
- `tools/shaderc/` – build-time Slang compiler + SDL_GPU binding validator
- `tools/cook/` – glTF → `.fpak` cooker, `falcon cook` (format: `engine/assets/fpak_format.h`)
- `tools/imgdiff/` – image comparison for the capture tests
- `tools/wavdiff/` – WAV checks and comparison for the audio tests
- `runtime/` – `falcon.exe`: dispatches to the engine, the cooker or CEF's helper
- `external/` – pinned submodules
- `docs/sdl_gpu_notes.md` – SDL_GPU behaviour the renderer relies on

## License

Falcon's own code, shaders, UI and tools are under the MIT license (`LICENSE`). What it builds
with keeps its own license:
- cef-lite (the editor's and in-game HTML UI) is GPL-3.0, so a build with the UI is
  GPL-3.0 as a whole. `-DFALCON_UI=OFF` builds the engine without it.
- The submodules in `external/` (SDL, flecs, Box3D, meshoptimizer, Manifold, Recast/Detour and
  the rest) carry their licenses in their folders.
- `content/library` (Poly Haven) and `content/prototype` (Kenney) are CC0: their `License.txt`.
- SDKs fetched at build time are not part of this repository and come under their vendors'
  terms: CEF (BSD), Steam Audio, the Steamworks SDK (needs a partner login), AMD FidelityFX
  (FSR) and NVIDIA DLSS (only linked into the separate `falcon_dlss` plugin).
