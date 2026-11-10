# Particles and decals

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
