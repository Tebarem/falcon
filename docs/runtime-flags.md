# Runtime flags

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
| `--scene file.fpak` | load a cooked scene; with `--project`, relative to the project (without either, a bring-up triangle runs) |
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
| `--naive` | the reference renderer (every leaf cluster, no culling) |
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
