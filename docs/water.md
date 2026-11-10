# Water

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
