# Audio

Sound goes through our own mixer on SDL3's audio (WASAPI on Windows), spatialized by Valve's
Steam Audio (see [Building](building.md)). It has sources and sounds with HRTF, distance and air absorption; acoustic materials, occlusion and transmission; realtime and baked reflections and reverb; physics and water sounds; and an audio API for scripts.

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
