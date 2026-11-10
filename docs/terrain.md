# Terrain

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
