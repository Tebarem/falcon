# Rendering

## Renderer: the GPU-driven visibility buffer

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
   Instances of meshes whose every cluster is alpha-masked (grass, flowers, leaf cards: set at
   load, `ScenePak::open`) carry `kInstanceScatter`, so they are thinned with the terrain's
   scatter while the visible clusters run over budget: the far small ones go, the same ones every
   frame, instead of an overflow dropping different clusters each frame (flicker). The GPU stats
   line says "scatter thinned below N px" while it is on.
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

## Virtual texturing

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

## Lighting and shadows

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

## Post-processing

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
