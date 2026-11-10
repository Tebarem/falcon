# falcon.exe: the editor (--editor), the game, the cooker (falcon cook ...) and CEF's helper
# processes (Chromium relaunches it with --type=...).
add_executable(falcon main.cpp)
if(WIN32)
  target_sources(falcon PRIVATE cef_runtime.cpp falcon.manifest)
else()
  # (freeze reports name the main thread's functions through dladdr: the symbols must be exported)
  set_target_properties(falcon PROPERTIES ENABLE_EXPORTS ON)
endif()
target_link_libraries(falcon PRIVATE falcon_engine falcon_cook_lib)
falcon_compile_options(falcon STRICT)
if(FALCON_UI)
  target_link_libraries(falcon PRIVATE cef_lite)
  target_compile_definitions(falcon PRIVATE FALCON_UI=1)
endif()

# The *_d3d12 captures run on this OS's own API: Direct3D 12 on Windows, Vulkan on Linux (the
# *_backends comparisons then compare Vulkan with itself), Metal on macOS.
if(WIN32)
  set(fx_native_driver direct3d12)
elseif(APPLE)
  set(fx_native_driver metal)
else()
  set(fx_native_driver vulkan)
endif()
# The scripts' files by mode (managed/CMakeLists.txt): JIT assemblies are .dll everywhere, NativeAOT
# libraries are this OS's (.dll, .so, .dylib).
set(fx_ext_jit .dll)
set(fx_ext_aot ${FALCON_AOT_EXT})

# Particle values (curves, gradients, every ParticleFloat mode) and the particle and decal
# components' JSON and flecs round trips (engine/vfx/vfx_selftest.cpp).
add_test(NAME vfx_values COMMAND falcon vfx-test)

# The transports (engine/net, tools/nettest/main.cpp), two endpoints in one process: loopback (ordered
# channels, datagrams, a 4 MB message, disconnects), the conditioner (latency, 10% loss, doubles),
# and with MsQuic: QUIC on localhost (a 10 MB message beside 2000 small ones on three channels,
# datagrams, the server answering), a pinned certificate (and a wrong pin refused), 10% loss and
# 20 ms latency (reliable messages all there), reconnecting, the idle timeout and keep-alive.
foreach(t loopback conditioner)
  add_test(NAME net_${t} COMMAND falcon_net_test ${t})
endforeach()
if(FALCON_QUIC)
  foreach(t quic quic_pinned quic_loss quic_reconnect quic_idle)
    add_test(NAME net_${t} COMMAND falcon_net_test ${t} "${CMAKE_BINARY_DIR}/bin/net/msquic.dll")
  endforeach()
endif()

# The level tools' geometry libraries (tools/leveltest/main.cpp): MikkTSpace tangents on a cube, a
# Manifold door and window cut from a wall (exact volume, genus, identical reruns), an xatlas unwrap,
# a Recast nav mesh whose Detour path goes around a wall, and the level kernel (level/poly_mesh: a box,
# a concave face, smoothing, world UVs across meshes, blob and base64 round trips) and its mesh
# operations and shapes (level/mesh_ops: each keeps the mesh closed and wound alike) and its UV
# operations (fit, nudge, align, match across seams, unwrap, manual, reset, world space) and trim
# sheets (faces onto a strip, a run of faces carrying u on, the .ftrim text) and layered surfaces
# data (layer materials, the mask UVs, vertex colours, format 2, kept through operations) and live
# cuts (level/mesh_cut: a door's exact volume, caps, world space, intersect, mesh cutters, open meshes)
# and walls (level/walls: straight, mitred, aligned, rooms with floors and ceilings, paths, a door),
# wear (level/wear: edge wear at edges not mid-face, painting kept, grime at the foot and corners),
# modifiers (level/modifiers: mirror, arrays, solidify, bevel, deform, a stack, JSON), splines
# (level/splines: frames, corners, lofts, the curve modifier), navigation (level/navmesh: a tiled bake,
# a path round a wall, a raycast, nearest, save and load).
foreach(t tangents boolean unwrap navmesh polymesh meshops uvops trims layers cuts walls wear modifiers splines navigation)
  add_test(NAME level_${t} COMMAND falcon_level_test ${t})
endforeach()

# The ray tracing BVH (tools/bvhtest/main.cpp): the CPU copy of the GPU traversal against brute
# force over random triangles, thousands stacked in one place (still shallow enough for the GPU's
# stack), refit after moving, rays down a grid's shared edges (no cracks), empty and single trees.
foreach(t soup stacked refit edges small)
  add_test(NAME bvh_${t} COMMAND falcon_bvh_test ${t})
endforeach()
# The deterministic math (core/dmath.h) gives these bits on every PC; the managed DMath the same
# (sim_dmath_jit, sim_dmath_aot below).
add_test(NAME sim_dmath_native COMMAND falcon dmath-hash)
set_tests_properties(sim_dmath_native PROPERTIES PASS_REGULAR_EXPRESSION "dmath self-hash 38d7ca740fd695b0")

# Render tests (need a GPU and the Khronos Sponza sample in samples/_src). Each compares two
# deterministic captures with falcon_imgdiff:
#   occlusion  - two-pass HiZ culling never removes a visible cluster (moving camera)
#   retest     - forcing every cluster through the second pass gives the same image
#   lod0_naive - at full detail the GPU-driven path covers exactly the M1 reference geometry
#                (per-instance colour view; shading differs since M3)
#   lod_detail_distance - inside the full-detail distance a coarse LOD error still picks the
#                full-detail clusters (LOD level view)
#   ray_traced_view - camera rays through the ray tracing BVH hit the triangles the raster draws
#   backends   - D3D12 and Vulkan agree (ties between coplanar surfaces may differ)
#   full_ambient_backends - the same with AO and GI at full resolution (GI traced in the resolve)
#   vsm_cache  - shadow pages cached while the camera moves match pages rendered fresh
#                (sun, and 16 shadowed local lights)
#   repeat     - two identical runs give the same capture
#   taa_repeat - the same with TAA (--taa): history, jitter phase and auto exposure are
#                deterministic
#   physics_vsm_cache - bodies falling through cached shadow pages invalidate exactly the
#                pages they touch (matches every page rendered fresh)
#   physics_threads - Box3D is deterministic across worker counts (1 thread vs the default)
#   script_modes - the Sandbox game gives the same frame JIT-hosted (CoreCLR) and NativeAOT
#   script_reload - a hot reload mid-run unloads the old code (else the run fails) and, since
#                game state lives in the ECS, changes nothing
#   scene_reload - the scene torn down and loaded again in process (--reload-scene-at) renders
#                the same frame; _leak: ten loads, nothing grows (GPU accounting exact, RAM bounded)
set(fx_sponza "${CMAKE_SOURCE_DIR}/samples/_src/khronos/Models/Sponza/glTF/Sponza.gltf")
if(EXISTS "${fx_sponza}")
  set(dir "${CMAKE_BINARY_DIR}/test/render")
  file(MAKE_DIRECTORY "${dir}")
  set(pak "${dir}/sponza.fpak")
  # With the procedural rocks (no instances: the scene renders as before; terrain tests scatter them).
  add_test(NAME render_cook COMMAND falcon cook "${fx_sponza}" "${pak}" --rocks)
  set_tests_properties(render_cook PROPERTIES FIXTURES_SETUP sponza_pak)

  function(fx_capture name)
    # AgX base look: the tolerances below are for renderer differences, which the default look's
    # extra contrast and saturation would amplify.
    add_test(NAME render_${name} COMMAND falcon --hidden --scene "${pak}" --capture "${dir}/${name}.png" --look base ${ARGN})
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP capture_${name})
  endfunction()
  function(fx_compare name a b maxPixels)
    add_test(NAME render_${name} COMMAND falcon_imgdiff "${dir}/${a}.png" "${dir}/${b}.png"
      --tolerance 3 --max-pixels ${maxPixels} --out "${dir}/${name}_diff.png")
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED "capture_${a};capture_${b}")
  endfunction()

  fx_capture(moving_occ --dolly 4 --capture-frame 60)
  fx_capture(moving_repeat --dolly 4 --capture-frame 60)
  fx_capture(taa_a --taa --dolly 4 --capture-frame 60)
  fx_capture(taa_b --taa --dolly 4 --capture-frame 60)
  fx_capture(moving_noocc --dolly 4 --capture-frame 60 --no-occlusion)
  fx_capture(moving_nocache --dolly 4 --capture-frame 60 --no-shadow-cache)
  set(fx_local --test-lights 64 --shadowed-lights 16 --camera-light 5)
  fx_capture(local_cache --dolly 2 --capture-frame 60 ${fx_local})
  fx_capture(local_nocache --dolly 2 --capture-frame 60 --no-shadow-cache ${fx_local})
  fx_capture(static_retest --force-retest --capture-frame 8)
  fx_capture(static_noocc --no-occlusion --capture-frame 8)
  fx_capture(lod0 --lod-pixels 0 --no-cone --no-alpha-mask --debug-view 3 --capture-frame 8)
  fx_capture(naive --naive --debug-view 3 --capture-frame 8)
  fx_capture(lod0_level --lod-pixels 0 --cull-pixels 0 --no-cone --no-alpha-mask --debug-view 5 --capture-frame 8)
  fx_capture(lod_near_level --lod-pixels 64 --detail-distance 100000 --no-cone --no-alpha-mask --debug-view 5 --capture-frame 8)
  # The ray tracer against the raster: the finest LOD's triangles (debug view 4) and the same seen by
  # camera rays through the BVH (debug view 14, render/ray_tracing/scene_bvh.h).
  fx_capture(raster_triangles --lod-pixels 0 --cull-pixels 0 --no-cone --no-alpha-mask --debug-view 4 --capture-frame 8)
  fx_capture(ray_triangles --lod-pixels 0 --cull-pixels 0 --no-cone --no-alpha-mask --debug-view 14 --capture-frame 8)
  # The backend pair also streams 256 falling bodies through both backends.
  fx_capture(d3d12 --driver ${fx_native_driver} --dolly 3 --capture-frame 60 --physics-drop 256)
  fx_capture(vulkan --driver vulkan --dolly 3 --capture-frame 60 --physics-drop 256)
  fx_capture(full_d3d12 --driver ${fx_native_driver} --dolly 3 --capture-frame 60 --full-res-ambient)
  fx_capture(full_vulkan --driver vulkan --dolly 3 --capture-frame 60 --full-res-ambient)
  set(fx_drop --physics-drop 512 --capture-frame 90)
  fx_capture(physics_cache ${fx_drop})
  fx_capture(physics_nocache ${fx_drop} --no-shadow-cache)
  fx_capture(physics_1thread ${fx_drop} --threads 1)
  set(fx_scripts "${CMAKE_BINARY_DIR}/bin/scripts")
  # The simulation's determinism (sim/sim_recorder.h): the Sandbox with 256 falling bodies, 600
  # frames of a synthetic player's input and uneven frame times recorded, then replayed tick by
  # tick: with the default threads, one thread and the NativeAOT build (the editor's JIT pinned to
  # its instruction set), every tick's physics and transform hashes match. sim_replay_detects: a
  # different run (300 bodies) fails at once. sim_dmath_*: DMath's bits are the native side's.
  set(fx_sim "${dir}/sim_sandbox.fxsim")
  set(fx_sim_run --hidden --scene "${pak}" --physics-drop 256)
  add_test(NAME sim_record COMMAND falcon ${fx_sim_run} --script "${fx_scripts}/jit/Sandbox.dll" --frames 600
    --sim-input-demo 7 --sim-record "${fx_sim}")
  set_tests_properties(sim_record PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP sim_recording)
  add_test(NAME sim_replay_jit COMMAND falcon ${fx_sim_run} --script "${fx_scripts}/jit/Sandbox.dll" --sim-replay "${fx_sim}")
  add_test(NAME sim_replay_1thread COMMAND falcon ${fx_sim_run} --script "${fx_scripts}/jit/Sandbox.dll" --threads 1
    --sim-replay "${fx_sim}")
  add_test(NAME sim_replay_aot COMMAND falcon ${fx_sim_run} --script "${fx_scripts}/aot/Sandbox${FALCON_AOT_EXT}" --sim-replay "${fx_sim}")
  add_test(NAME sim_replay_detects COMMAND falcon --hidden --scene "${pak}" --physics-drop 300 --script "${fx_scripts}/jit/Sandbox.dll"
    --sim-replay "${fx_sim}")
  set_tests_properties(sim_replay_jit sim_replay_1thread sim_replay_aot PROPERTIES FIXTURES_REQUIRED "sponza_pak;sim_recording"
    PASS_REGULAR_EXPRESSION "ticks: identical")
  set_tests_properties(sim_replay_detects PROPERTIES FIXTURES_REQUIRED "sponza_pak;sim_recording"
    PASS_REGULAR_EXPRESSION "came out differently")
  # Rollback (the networked simulation's history, Network/SimHistory.cs): Arena (players with
  # CharacterMovers, [Sync] state, projectiles spawned and destroyed in ticks, bots on Sim.Random)
  # rolled back 8 ticks every frame and simulated forward again, each tick as the first time
  # (sim_rollback_arena). A recording without rollbacks replays identically on NativeAOT rolling
  # back 12 ticks every frame (exit code 1 on any difference).
  set(fx_arena_sim "${dir}/sim_arena.fxsim")
  set(fx_arena --hidden --scene "${pak}")
  add_test(NAME sim_rollback_arena COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/jit/Arena.dll" --frames 1200
    --sim-input-demo 3 --sim-rollback 8 --sim-record "${fx_arena_sim}")
  set_tests_properties(sim_rollback_arena PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP sim_arena_recording
    PASS_REGULAR_EXPRESSION "ticks simulated again .8 back each frame.: identical")
  add_test(NAME sim_rollback_arena_aot COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/aot/Arena${FALCON_AOT_EXT}"
    --sim-replay "${fx_arena_sim}" --sim-rollback 12)
  set_tests_properties(sim_rollback_arena_aot PROPERTIES FIXTURES_REQUIRED "sponza_pak;sim_arena_recording")
  # Sessions (Network/Session.cs, QUIC): an Arena host with two client processes of itself
  # (--net-clients 2) joining late, everyone under 40 ms latency, 10 ms jitter and 10% loss
  # (--net-sim). The host replicates its objects (spawns, despawns, [Sync] state, transforms); each
  # client reports hashes of what it has and the host checks them against its history
  # (--net-verify); the run fails if a state differs, nothing was checked, or a client fails.
  # net_session_rpc: the host's Hit RPC (a broadcast, unreliable) heard on the clients.
  if(FALCON_QUIC)
    foreach(mode jit aot)
      add_test(NAME net_session_${mode} COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/${mode}/Arena${fx_ext_${mode}}" --frames 2400
        --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 2 --net-verify --net-sim 40,10,10)
      set_tests_properties(net_session_${mode} PROPERTIES FIXTURES_REQUIRED sponza_pak)
    endforeach()
    # Prediction: the same with a 150 ms round trip (75 ms each way, jitter, 5% loss), longer. Each
    # client plays its own Arena player (IPredicted) from the synthetic player's input, predicting
    # it; its run fails if fewer than 50 predictions were checked against the host's states or more
    # than 1 in 10 needed correcting (hits by others, the start).
    add_test(NAME net_predict COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/jit/Arena.dll" --frames 4000
      --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 2 --net-verify --net-sim 75,10,5)
    set_tests_properties(net_predict PROPERTIES FIXTURES_REQUIRED sponza_pak)
    # The dedicated server (--server): no window, GPU, browser or audio device, 60 ticks a second,
    # two client processes verified as above (net_server); a full server (--max-players 1)
    # refusing the second client (net_server_full). On a free port of 127.0.0.1 only, not the
    # default 27015 on every address: another checkout's tests may be running a server at the same
    # time, and listening only on this machine raises no firewall prompt. The clients get
    # the same --frames, hidden and not held to 60 a second: 900 ran them about 3.5 s, ~215 ticks,
    # barely the 50 checked predictions they need; 1800 gives them ~110.
    add_test(NAME net_server COMMAND falcon --server --scene "${pak}" --script "${fx_scripts}/arena/jit/Arena.dll" --frames 1800
      --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 2 --net-verify --net-sim 40,10,5)
    add_test(NAME net_server_full COMMAND falcon --server --scene "${pak}" --script "${fx_scripts}/arena/jit/Arena.dll" --frames 600
      --host 127.0.0.1:0 --net-clients 2 --max-players 1)
    set_tests_properties(net_server net_server_full PROPERTIES FIXTURES_REQUIRED sponza_pak)
    set_tests_properties(net_server_full PROPERTIES PASS_REGULAR_EXPRESSION "refused this game: the server is full")
    # A load test: a server and a process of 150 bots (--bots with --net-clients 1: the client
    # process runs the bots); the server's stats show them all playing, each sent objects.
    add_test(NAME net_load COMMAND falcon --server --scene "${pak}" --script "${fx_scripts}/arena/jit/Arena.dll" --frames 900
      --host 127.0.0.1:0 --bots 150 --net-clients 1)
    set_tests_properties(net_load PROPERTIES FIXTURES_REQUIRED sponza_pak
      PASS_REGULAR_EXPRESSION "server: 150 players, [0-9]+ objects;[^\n]* [1-9][0-9]* objects a player"
      FAIL_REGULAR_EXPRESSION "client process [0-9]+ (failed|did not end)")
    # Rollback mode (--net-mode rollback): a host and two client processes at a 100 ms round trip
    # with 2% loss, each simulating the whole world (players, shots, a crate pile) from everyone's
    # inputs, rolling back for late ones; the host checks the others' hashes of confirmed ticks.
    add_test(NAME net_rollback COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/jit/Arena.dll" --frames 3000
      --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 2 --net-mode rollback --net-sim 50,10,2)
    set_tests_properties(net_rollback PROPERTIES FIXTURES_REQUIRED sponza_pak
      PASS_REGULAR_EXPRESSION "rollback: [1-9][0-9]* rollbacks[^\n]* 0 inputs too late; [1-9][0-9]* hashes checked, 0 desyncs"
      FAIL_REGULAR_EXPRESSION "desync:|came [0-9]+ ticks late|client process [0-9]+ failed")
    add_test(NAME net_session_rpc COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/jit/Arena.dll" --frames 2400
      --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 1)
    set_tests_properties(net_session_rpc PROPERTIES FIXTURES_REQUIRED sponza_pak
      PASS_REGULAR_EXPRESSION "Arena stopped .client.: [^\n]*hits heard [1-9]")
    # Level cuts and late join: with live mesh room every machine makes a level wall; the host cuts
    # it (NetworkedCuts) twice before the client joins and once after. The client catches up (asks
    # for the cuts it missed) and its level's geometry hash is the host's. (No "(.|\n)*" across lines
    # in these: CMake's regex backtracks over the whole log and ctest hangs.)
    add_test(NAME net_session_cuts COMMAND falcon ${fx_arena} --script "${fx_scripts}/arena/jit/Arena.dll" --frames 2400
      --sim-input-demo 3 --host 127.0.0.1:0 --net-clients 1 --net-sim 40,10,10 --live-meshes 4 --live-clusters 256)
    set_tests_properties(net_session_cuts PROPERTIES FIXTURES_REQUIRED sponza_pak
      PASS_REGULAR_EXPRESSION "Arena level .client.: cuts 3 .caught up [1-9][0-9]*., the host's geometry: same"
      FAIL_REGULAR_EXPRESSION "the host's geometry: different|no level cuts object|client process [0-9]+ failed")
  endif()
  foreach(mode jit aot)
    add_test(NAME sim_dmath_${mode} COMMAND falcon --hidden --scene "${pak}" --script "${fx_scripts}/${mode}/Sandbox${fx_ext_${mode}}" --frames 3)
    set_tests_properties(sim_dmath_${mode} PROPERTIES FIXTURES_REQUIRED sponza_pak
      PASS_REGULAR_EXPRESSION "DMath self-hash 38d7ca740fd695b0")
  endforeach()
  # Steam (--steam 480, Valve's Spacewar): the player, a call result, an avatar and a lobby (made,
  # found by its data, left), JIT and AOT.
  # Skipped when the Steam client isn't running or signed in.
  if(FALCON_STEAMWORKS)
    foreach(mode jit aot)
      add_test(NAME steam_${mode} COMMAND falcon --hidden --scene "${pak}" --script "${fx_scripts}/${mode}/Sandbox${fx_ext_${mode}}" --frames 600 --steam 480)
      set_tests_properties(steam_${mode} PROPERTIES FIXTURES_REQUIRED sponza_pak
        PASS_REGULAR_EXPRESSION "Steam signed in as .*App ID 480.*Steam leaderboard 'Feet Traveled' found.*Steam lobby created .1 member, owner me., found by its data.*Steam stats: [1-9][0-9]* achievements.*top [1-9] on Feet Traveled.*Steam workshop: [1-9][0-9]* items found.*Steam Input on"
        SKIP_REGULAR_EXPRESSION "Steam not running")
    endforeach()
    # A session over Steam's sockets (steamip://, a host and one client process, verified), each
    # player's Steam auth ticket checked by Steam.
    if(FALCON_QUIC)
      add_test(NAME net_steam COMMAND falcon --hidden --scene "${pak}" --script "${fx_scripts}/arena/jit/Arena.dll" --frames 2400
        --sim-input-demo 3 --host 27260 --net-transport steamip --net-clients 1 --net-verify --steam 480)
      set_tests_properties(net_steam PROPERTIES FIXTURES_REQUIRED sponza_pak
        PASS_REGULAR_EXPRESSION "hosting on steamip://.*is Steam player [0-9]+.*replicated states checked, all as the host's"
        FAIL_REGULAR_EXPRESSION "net-verify: failed|client process [0-9]+ failed"
        SKIP_REGULAR_EXPRESSION "steam: not running")
      # A dedicated server on Steam (--server --steam: Steam's game server, logged on anonymously,
      # on 27015 so Steam's LAN search finds it): a client verified, its ticket checked by the
      # server, and the server in the client's LAN server list.
      add_test(NAME net_steam_server COMMAND falcon --server --scene "${pak}" --script "${fx_scripts}/arena/jit/Arena.dll" --frames 1800
        --sim-input-demo 3 --host 27015 --net-transport steamip --net-clients 1 --net-verify --steam 480)
      set_tests_properties(net_steam_server PROPERTIES FIXTURES_REQUIRED sponza_pak
        PASS_REGULAR_EXPRESSION "game server logged on as [0-9]+.*is Steam player [0-9]+.*Steam LAN servers: [1-9].*replicated states checked, all as the host's"
        FAIL_REGULAR_EXPRESSION "net-verify: failed|client process [0-9]+ failed"
        SKIP_REGULAR_EXPRESSION "steam: not running")
    endif()
  endif()
  fx_capture(script_jit --script "${fx_scripts}/jit/Sandbox.dll" --capture-frame 120)
  fx_capture(script_aot --script "${fx_scripts}/aot/Sandbox${FALCON_AOT_EXT}" --capture-frame 120)
  fx_capture(script_reloaded --script "${fx_scripts}/jit/Sandbox.dll" --capture-frame 120 --script-reload-at 30)

  # Atomic append order is nondeterministic, so ties between coplanar surfaces may flip a few pixels.
  fx_compare(occlusion moving_occ moving_noocc 50)
  fx_compare(retest static_retest static_noocc 50)
  fx_compare(lod0_naive lod0 naive 500)  # coplanar ties between instances
  fx_compare(lod_detail_distance lod0_level lod_near_level 50)
  fx_compare(ray_traced_view raster_triangles ray_triangles 1500)  # (about 350: edges and the near plane)
  fx_compare(backends d3d12 vulkan 500)
  fx_compare(full_ambient_backends full_d3d12 full_vulkan 500)
  fx_compare(vsm_cache moving_occ moving_nocache 50)
  fx_compare(vsm_local_cache local_cache local_nocache 50)
  # Reflections (screen space, the BVH, the sky; reflections.slang) change the frame: the
  # same view without them differs on the shiny surfaces.
  fx_capture(reflections_on --dolly 2 --capture-frame 60)
  fx_capture(reflections_off --dolly 2 --capture-frame 60 --no-reflections)
  add_test(NAME render_reflections_differ COMMAND falcon_imgdiff "${dir}/reflections_on.png" "${dir}/reflections_off.png"
    --tolerance 6 --min-pixels 5000 --max-pixels 1440000 --out "${dir}/reflections_differ_diff.png")
  set_tests_properties(render_reflections_differ PROPERTIES FIXTURES_REQUIRED "capture_reflections_on;capture_reflections_off")
  fx_compare(repeat moving_occ moving_repeat 50)
  fx_compare(taa_repeat taa_a taa_b 50)
  fx_compare(physics_vsm_cache physics_cache physics_nocache 50)
  fx_compare(physics_threads physics_cache physics_1thread 50)
  fx_compare(script_modes script_jit script_aot 50)
  fx_compare(script_reload script_jit script_reloaded 50)
  # Scenes in process (L1): the scene torn down and loaded again mid-run renders as it would have;
  # ten loads leave the GPU's accounting exactly as it was between the first two, and the
  # process's RAM within 64 MB.
  fx_capture(scene_static --capture-frame 120)
  fx_capture(scene_reloaded --capture-frame 120 --reload-scene-at 30)
  fx_compare(scene_reload scene_static scene_reloaded 50)
  add_test(NAME render_scene_reload_leak COMMAND falcon --hidden --scene "${pak}" --look base --frames 260 --reload-scene-at 20,10)
  set_tests_properties(render_scene_reload_leak PROPERTIES FIXTURES_REQUIRED sponza_pak)
  # The Sandbox game going on through three loads of its scene (L2: its queries and components'
  # ids follow the world), the same frame as without them.
  fx_capture(script_scene_reloaded --script "${fx_scripts}/jit/Sandbox.dll" --capture-frame 120 --reload-scene-at 40,2)

  # Water (a Gerstner lake flooding the floor, water_*.slang): deterministic, the same on
  # both backends.
  set(fx_water --water-demo tropical:0,0.4,0,26,10 --dolly 2 --capture-frame 60)
  fx_capture(water_d3d12 ${fx_water} --driver ${fx_native_driver})
  fx_capture(water_vulkan ${fx_water} --driver vulkan)
  fx_capture(water_repeat ${fx_water} --driver ${fx_native_driver})
  fx_compare(water_deterministic water_d3d12 water_repeat 50)
  fx_compare(water_backends water_d3d12 water_vulkan 1000)  # (the scene's own ~500, and a few sun glints)
  # Buoyancy: bodies dropped into wavy water float, bob and drift the same every run and with
  # one physics thread.
  set(fx_float --water-demo tropical:0,1.4,0,26,10 --physics-drop 128 --capture-frame 180)
  fx_capture(water_float ${fx_float})
  fx_capture(water_float_repeat ${fx_float})
  fx_capture(water_float_1thread ${fx_float} --threads 1)
  fx_compare(water_float_deterministic water_float water_float_repeat 50)
  fx_compare(water_float_threads water_float water_float_1thread 50)
  # The ocean (FFT waves, three cascades) around a generated coast: deterministic, and the same on
  # both backends (as the terrain's, up to 2000 pixels).
  set(fx_ocean --no-terrain --terrain 8 --terrain-hills 40 --terrain-seed 4 --water-demo ocean:0,22,0
    --camera 300,40,300,0,22,0 --capture-frame 60)
  fx_capture(ocean_d3d12 ${fx_ocean} --driver ${fx_native_driver})
  fx_capture(ocean_vulkan ${fx_ocean} --driver vulkan)
  fx_capture(ocean_repeat ${fx_ocean} --driver ${fx_native_driver})
  fx_compare(ocean_deterministic ocean_d3d12 ocean_repeat 50)
  fx_compare(ocean_backends ocean_d3d12 ocean_vulkan 2000)
  # A still lake on the same coast: the planar reflections, and the terrain traced where the
  # frame holds nothing to mirror.
  set(fx_mirror --no-terrain --terrain 8 --terrain-hills 40 --terrain-seed 4 --water-demo oasis:0,22,0,1200,1200
    --camera 300,40,300,0,22,0 --capture-frame 60)
  fx_capture(mirror_d3d12 ${fx_mirror} --driver ${fx_native_driver})
  fx_capture(mirror_vulkan ${fx_mirror} --driver vulkan)
  fx_capture(mirror_repeat ${fx_mirror} --driver ${fx_native_driver})
  fx_compare(mirror_deterministic mirror_d3d12 mirror_repeat 50)
  fx_compare(mirror_backends mirror_d3d12 mirror_vulkan 2000)
  # Under water (the camera below a lake's surface: fog, the surface from below, caustics on the
  # floor) and caustics seen from above: deterministic and the same on both backends.
  set(fx_under --water-demo tropical:0,1.6,0,26,10 --camera -8,0.6,0,8,1.0,0 --capture-frame 40)
  fx_capture(underwater_d3d12 ${fx_under} --driver ${fx_native_driver})
  fx_capture(underwater_vulkan ${fx_under} --driver vulkan)
  fx_capture(underwater_repeat ${fx_under} --driver ${fx_native_driver})
  fx_compare(underwater_deterministic underwater_d3d12 underwater_repeat 50)
  fx_compare(underwater_backends underwater_d3d12 underwater_vulkan 1000)
  set(fx_caustics --water-demo pool:0,0.8,0,26,10 --camera -3,3.5,1,2,0,0 --capture-frame 40)
  fx_capture(caustics_d3d12 ${fx_caustics} --driver ${fx_native_driver})
  fx_capture(caustics_vulkan ${fx_caustics} --driver vulkan)
  fx_compare(caustics_backends caustics_d3d12 caustics_vulkan 1000)

  # Terrain (a generated 1 km heightfield next to the scene): deterministic, the same on both
  # backends, cached shadow pages match fresh ones (patches change level as the camera moves),
  # and bodies dropped on it come to rest the same with one physics thread or many.
  set(fx_terrain_world --no-terrain --terrain 8 --terrain-hills 120 --terrain-seed 4)
  set(fx_terrain ${fx_terrain_world} --dolly 6)
  fx_capture(terrain_d3d12 ${fx_terrain} --driver ${fx_native_driver} --capture-frame 60)
  fx_capture(terrain_vulkan ${fx_terrain} --driver vulkan --capture-frame 60)
  fx_capture(terrain_repeat ${fx_terrain} --driver ${fx_native_driver} --capture-frame 60)
  fx_capture(terrain_nocache ${fx_terrain} --driver ${fx_native_driver} --capture-frame 60 --no-shadow-cache)
  fx_capture(terrain_physics ${fx_terrain_world} --physics-drop 256 --capture-frame 150)
  fx_capture(terrain_physics_1thread ${fx_terrain_world} --physics-drop 256 --capture-frame 150 --threads 1)
  fx_compare(terrain_deterministic terrain_d3d12 terrain_repeat 50)
  fx_compare(terrain_backends terrain_d3d12 terrain_vulkan 500)
  fx_compare(terrain_vsm_cache terrain_d3d12 terrain_nocache 50)
  fx_compare(terrain_physics_threads terrain_physics terrain_physics_1thread 50)
  # Material layers (generated grass and rock, built at start-up; the capture waits for them):
  # the surface pass is deterministic and the same on both backends.
  set(fx_layers ${fx_terrain} --terrain-layers grass,rock --capture-frame 60)
  fx_capture(terrain_layers_d3d12 ${fx_layers} --driver ${fx_native_driver})
  fx_capture(terrain_layers_vulkan ${fx_layers} --driver vulkan)
  fx_capture(terrain_layers_repeat ${fx_layers} --driver ${fx_native_driver})
  fx_compare(terrain_layers_deterministic terrain_layers_d3d12 terrain_layers_repeat 50)
  fx_compare(terrain_layers_backends terrain_layers_d3d12 terrain_layers_vulkan 500)
  # Scatter (two layers of built-in meshes generated on the GPU): deterministic, the same on both
  # backends, and cached shadow pages over the generated cells match fresh ones.
  set(fx_scatter ${fx_terrain} --terrain-scatter cone:0.05:40000,sphere:0.02 --capture-frame 60)
  fx_capture(terrain_scatter_d3d12 ${fx_scatter} --driver ${fx_native_driver})
  fx_capture(terrain_scatter_vulkan ${fx_scatter} --driver vulkan)
  fx_capture(terrain_scatter_repeat ${fx_scatter} --driver ${fx_native_driver})
  fx_capture(terrain_scatter_nocache ${fx_scatter} --driver ${fx_native_driver} --no-shadow-cache)
  fx_compare(terrain_scatter_deterministic terrain_scatter_d3d12 terrain_scatter_repeat 50)
  fx_compare(terrain_scatter_backends terrain_scatter_d3d12 terrain_scatter_vulkan 500)
  fx_compare(terrain_scatter_vsm_cache terrain_scatter_d3d12 terrain_scatter_nocache 50)
  # Dense scatter (20 cones per m^2, far past the visible-cluster and shadow-record budgets):
  # thinned to fit, so neither overflows (an overflow drops clusters of any mesh, different ones
  # as the view changes: meshes flicker). The stats are logged every 2 s, before the capture.
  # Generation (a preset's stack run on the GPU at start-up: noise, erosion, auto-paint): the same
  # every run; across backends, erosion's float rounding moves a few dithered material edges.
  # Canyons' cliffs keep fine patches far away (none may be culled as too small).
  set(fx_gen ${fx_terrain_world} --terrain-hills 0 --camera -450,560,450,0,150,0 --capture-frame 60)
  fx_capture(terrain_gen_d3d12 ${fx_gen} --terrain-generate Mountains --driver ${fx_native_driver})
  fx_capture(terrain_gen_repeat ${fx_gen} --terrain-generate Mountains --driver ${fx_native_driver})
  fx_capture(terrain_gen_vulkan ${fx_gen} --terrain-generate Mountains --driver vulkan)
  fx_capture(terrain_gen_canyons ${fx_gen} --terrain-generate Canyons --driver ${fx_native_driver})
  fx_capture(terrain_gen_canyons_repeat ${fx_gen} --terrain-generate Canyons --driver ${fx_native_driver})
  fx_compare(terrain_gen_deterministic terrain_gen_d3d12 terrain_gen_repeat 50)
  fx_compare(terrain_gen_backends terrain_gen_d3d12 terrain_gen_vulkan 2000)
  fx_compare(terrain_gen_canyons_deterministic terrain_gen_canyons terrain_gen_canyons_repeat 50)
  # Rocks (falcon cook --rocks) scattered by the rock presets, shaded with the terrain's rock layer
  # and the ground blended onto their bases: deterministic, both backends, cached shadows.
  set(fx_rocks ${fx_terrain_world} --terrain-hills 0 --terrain-generate Mountains
    --terrain-scatter "Boulders,Spires and arches,Cliff rocks" --camera -150,330,250,0,170,0 --capture-frame 90)
  fx_capture(terrain_rocks_d3d12 ${fx_rocks} --driver ${fx_native_driver})
  fx_capture(terrain_rocks_repeat ${fx_rocks} --driver ${fx_native_driver})
  fx_capture(terrain_rocks_vulkan ${fx_rocks} --driver vulkan)
  fx_capture(terrain_rocks_nocache ${fx_rocks} --driver ${fx_native_driver} --no-shadow-cache)
  fx_compare(terrain_rocks_deterministic terrain_rocks_d3d12 terrain_rocks_repeat 50)
  fx_compare(terrain_rocks_backends terrain_rocks_d3d12 terrain_rocks_vulkan 2000)
  fx_compare(terrain_rocks_vsm_cache terrain_rocks_d3d12 terrain_rocks_nocache 50)
  # An empty scene (falcon cook --empty: nothing placed, the built-ins and rocks): sky alone,
  # and with a generated terrain and rocks; deterministic, both backends.
  set(fx_empty "${dir}/empty.fpak")
  add_test(NAME render_empty_cook COMMAND falcon cook --empty "${fx_empty}" --rocks --verify)
  set_tests_properties(render_empty_cook PROPERTIES FIXTURES_SETUP empty_pak)
  function(fx_capture_empty name)
    add_test(NAME render_${name} COMMAND falcon --hidden --scene "${fx_empty}" --capture "${dir}/${name}.png" --look base ${ARGN})
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED empty_pak FIXTURES_SETUP capture_${name})
  endfunction()
  # Dense scatter (20 cones per m^2, far past the visible-cluster and shadow-record budgets):
  # thinned to fit, so neither overflows (an overflow drops clusters of any mesh, different ones
  # as the view changes: meshes flicker). The stats are logged every 2 s, before the capture.
  fx_capture_empty(terrain_scatter_dense --no-terrain --terrain 4 --terrain-hills 30 --terrain-scatter cone:20:2000000
    --camera 0,45,0,120,10,120 --capture-frame 900 --driver ${fx_native_driver})
  set_tests_properties(render_terrain_scatter_dense PROPERTIES FAIL_REGULAR_EXPRESSION "OVERFLOW"
    PASS_REGULAR_EXPRESSION "gpu: [0-9]+ instances")
  set(fx_empty_world --terrain 8 --terrain-hills 0 --terrain-generate Mountains --terrain-scatter "Boulders,Cliff rocks"
    --camera -150,330,250,0,170,0 --capture-frame 90)
  fx_capture_empty(empty_sky --capture-frame 30)
  fx_capture_empty(empty_sky_repeat --capture-frame 30)
  fx_capture_empty(empty_world_d3d12 ${fx_empty_world} --driver ${fx_native_driver})
  fx_capture_empty(empty_world_repeat ${fx_empty_world} --driver ${fx_native_driver})
  fx_capture_empty(empty_world_vulkan ${fx_empty_world} --driver vulkan)
  fx_compare(empty_sky_deterministic empty_sky empty_sky_repeat 0)
  fx_compare(empty_world_deterministic empty_world_d3d12 empty_world_repeat 50)
  fx_compare(empty_world_backends empty_world_d3d12 empty_world_vulkan 2000)
  # Asset previews (falcon --preview): meshes of the empty scene framed on a ground; repeatable,
  # and D3D12 matches Vulkan.
  function(fx_preview name)
    add_test(NAME render_preview${name} COMMAND falcon --preview "${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_jobs${name}.json" ${ARGN})
    set_tests_properties(render_preview${name} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED empty_pak
      FIXTURES_SETUP "capture_preview_rock${name};capture_preview_arch${name};capture_preview_cube${name}")
  endfunction()
  fx_preview("")
  fx_preview(_repeat)
  fx_preview(_vulkan --driver vulkan)
  foreach(asset rock arch cube)
    fx_compare(preview_${asset}_deterministic preview_${asset} preview_${asset}_repeat 0)
    fx_compare(preview_${asset}_backends preview_${asset} preview_${asset}_vulkan 300)
  endforeach()

  # Anti-aliasing changes the edges (about 4% of the frame: at least minPixels, so the pass ran)
  # and nothing else (at most maxPixels; a broken pass changes far more). MSAA is deterministic,
  # matches across backends, and runs under TAA.
  function(fx_compare_effect name a b minPixels maxPixels)
    add_test(NAME render_${name} COMMAND falcon_imgdiff "${dir}/${a}.png" "${dir}/${b}.png"
      --tolerance 3 --min-pixels ${minPixels} --max-pixels ${maxPixels} --out "${dir}/${name}_diff.png")
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED "capture_${a};capture_${b}")
  endfunction()
  fx_capture(aa_plain --capture-frame 8)
  fx_capture(aa_fxaa --fxaa --capture-frame 8)
  fx_capture(aa_msaa --msaa 4 --capture-frame 8)
  fx_capture(aa_msaa_repeat --msaa 4 --capture-frame 8)
  fx_capture(aa_msaa_vulkan --driver vulkan --msaa 4 --capture-frame 8)
  fx_capture(aa_msaa_taa --msaa 4 --taa --dolly 4 --capture-frame 60)
  fx_compare_effect(fxaa aa_plain aa_fxaa 20000 120000)
  fx_compare_effect(msaa aa_plain aa_msaa 20000 120000)
  fx_compare(msaa_repeat aa_msaa aa_msaa_repeat 50)
  fx_compare(msaa_backends aa_msaa aa_msaa_vulkan 500)
  fx_compare_effect(msaa_taa taa_a aa_msaa_taa 10000 80000)
  # A still camera under TAA looks the same at every jitter phase (motion vectors and the
  # current frame unjittered; with the jitter in them, edges wobbled: 11% of pixels changed).
  fx_capture(taa_phase7 --taa --capture-frame 8 --capture-phase 7)
  fx_capture(taa_phase3 --taa --capture-frame 8 --capture-phase 3)
  # (Reflections: ~4,600 of the pixels, ~7,600 since they are traced at half resolution, the
  # glossy normal-mapped emblems mostly; ~14,000 without them.) Auto exposure magnifies the same
  # edge differences in a darker shade: ~25,600 with the voxel GI's dark nave (at a fixed exposure
  # 6,900), ~11,500 with the probe GI's.
  fx_compare(taa_stable taa_phase7 taa_phase3 16000)
  # TAAU: at Native AA it is TAA; at Quality it keeps most detail (an upscaled image differs from
  # native at many texels, so the budget is wide) and is stable across its 18-frame cycle.
  fx_capture(taau_native --upscaler taau --upscale-quality native --capture-phase 7)
  fx_capture(taau_quality --upscaler taau --upscale-quality quality --capture-phase 7)
  fx_capture(taau_quality_phase3 --upscaler taau --upscale-quality quality --capture-phase 3)
  fx_compare(taau_is_taa taa_phase7 taau_native 50)
  fx_compare(taau_detail taa_phase7 taau_quality 150000)
  fx_compare(taau_stable taau_quality taau_quality_phase3 50000)  # ~42,000 (darker shade, as above)
  # AMD FSR (D3D12; FSR 4 where the GPU supports it, else 3.1): it runs (the log names the
  # version, and no FSR error), is stable across its jitter cycle, and on Vulkan falls back to
  # TAAU exactly.
  if(FALCON_FSR)
    fx_capture(fsr_quality --driver ${fx_native_driver} --upscaler fsr --upscale-quality quality --capture-phase 7)
    fx_capture(fsr_quality_phase3 --driver ${fx_native_driver} --upscaler fsr --upscale-quality quality --capture-phase 3)
    set_tests_properties(render_fsr_quality render_fsr_quality_phase3 PROPERTIES
      PASS_REGULAR_EXPRESSION "fsr: FSR [0-9]" FAIL_REGULAR_EXPRESSION "fsr: unavailable|fsr: .*failed|capture failed")
    fx_compare(fsr_stable fsr_quality fsr_quality_phase3 30000)
    fx_capture(fsr_vulkan_fallback --driver vulkan --upscaler fsr --upscale-quality quality --capture-phase 7)
    fx_compare(fsr_fallback taau_vulkan fsr_vulkan_fallback 50)
  endif()
  # NVIDIA DLSS (the plugin, bin/dlss/): on D3D12 it runs on an RTX GPU (the log names the mode)
  # or says why not (other GPUs), and never fails once it runs; on Vulkan it falls back to TAAU
  # exactly.
  if(FALCON_DLSS)
    fx_capture(dlss_quality --driver ${fx_native_driver} --upscaler dlss --upscale-quality quality --capture-phase 7)
    set_tests_properties(render_dlss_quality PROPERTIES
      PASS_REGULAR_EXPRESSION "dlss: (DLAA|Ultra Quality|Quality|Balanced|Performance|Ultra Performance), |dlss: unavailable: "
      FAIL_REGULAR_EXPRESSION "dlss: .*(failed|not created|no command list|no native)|capture failed")
    fx_capture(dlss_vulkan_fallback --driver vulkan --upscaler dlss --upscale-quality quality --capture-phase 7)
    fx_compare(dlss_fallback taau_vulkan dlss_vulkan_fallback 50)
  endif()
  if(FALCON_FSR OR FALCON_DLSS)
    fx_capture(taau_vulkan --driver vulkan --upscaler taau --upscale-quality quality --capture-phase 7)
  endif()
  # Freeze reports: the main thread stopped (3 s with a 1 s limit) leaves a report naming where it was
  # (its stack, symbolised from falcon.pdb), the log's last lines and a minidump; the run then logs it.
  add_test(NAME freeze_report_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${dir}/freeze")
  set_tests_properties(freeze_report_clean PROPERTIES FIXTURES_SETUP freeze_clean)
  # (Linux and macOS CI render in software or on a virtual GPU, whose first frames compile pipelines
  # for seconds: a longer threshold there, so the report is of the test's freeze)
  if(WIN32)
    set(fx_freeze --freeze-seconds 1 --test-freeze 30:3000)
  else()
    set(fx_freeze --freeze-seconds 20 --test-freeze 30:24000)
  endif()
  add_test(NAME freeze_report COMMAND falcon --hidden --scene "${pak}" --frames 90 ${fx_freeze}
    --freeze-reports "${dir}/freeze")
  set_tests_properties(freeze_report PROPERTIES FIXTURES_REQUIRED "sponza_pak;freeze_clean" FIXTURES_SETUP freeze_written
    PASS_REGULAR_EXPRESSION "freeze: the main thread stopped for")
  add_test(NAME freeze_report_contents COMMAND "${CMAKE_COMMAND}" -DDIR=${dir}/freeze
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_freeze_report.cmake")
  set_tests_properties(freeze_report_contents PROPERTIES FIXTURES_REQUIRED freeze_written)
endif()

# Animation (M8) on the Khronos Fox with samples/graphs/fox.graph.json:
#   anim_verify       every compressed clip frame decodes to the source pose (quantization bound)
#   anim_vsm_cache    25 foxes walking/running through cached shadow pages match pages rendered
#                     fresh (skinned characters invalidate the pages under them)
#   anim_backends     D3D12 and Vulkan skin, cull and render the crowd alike
#   anim_script_modes the Sandbox drives every fox's Speed from C#: JIT and NativeAOT agree
set(fx_fox "${CMAKE_SOURCE_DIR}/samples/_src/khronos/Models/Fox/glTF-Binary/Fox.glb")
set(fx_fox_graph "${CMAKE_SOURCE_DIR}/samples/graphs/fox.graph.json")
if(EXISTS "${fx_fox}")
  set(dir "${CMAKE_BINARY_DIR}/test/render")
  file(MAKE_DIRECTORY "${dir}")
  set(fox "${dir}/fox.fpak")
  add_test(NAME render_anim_verify COMMAND falcon cook "${fx_fox}" "${fox}" --verify --graph "${fx_fox_graph}")
  set_tests_properties(render_anim_verify PROPERTIES FIXTURES_SETUP fox_pak)

  function(fx_fox_capture name)
    add_test(NAME render_${name} COMMAND falcon --hidden --scene "${fox}" --capture "${dir}/${name}.png" --look base ${ARGN})
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED fox_pak FIXTURES_SETUP capture_${name})
  endfunction()
  set(fx_crowd --crowd 24 --capture-frame 90)
  fx_fox_capture(anim_cache ${fx_crowd})
  fx_fox_capture(anim_nocache ${fx_crowd} --no-shadow-cache)
  fx_fox_capture(anim_d3d12 ${fx_crowd} --driver ${fx_native_driver})
  fx_fox_capture(anim_vulkan ${fx_crowd} --driver vulkan)
  set(fx_scripts "${CMAKE_BINARY_DIR}/bin/scripts")
  fx_fox_capture(anim_script_jit ${fx_crowd} --script "${fx_scripts}/jit/Sandbox.dll")
  fx_fox_capture(anim_script_aot ${fx_crowd} --script "${fx_scripts}/aot/Sandbox${FALCON_AOT_EXT}")

  function(fx_fox_compare name a b maxPixels)
    add_test(NAME render_${name} COMMAND falcon_imgdiff "${dir}/${a}.png" "${dir}/${b}.png"
      --tolerance 3 --max-pixels ${maxPixels} --out "${dir}/${name}_diff.png")
    set_tests_properties(render_${name} PROPERTIES FIXTURES_REQUIRED "capture_${a};capture_${b}")
  endfunction()
  fx_fox_compare(anim_vsm_cache anim_cache anim_nocache 50)
  fx_fox_compare(anim_backends anim_d3d12 anim_vulkan 500)
  fx_fox_compare(anim_script_modes anim_script_jit anim_script_aot 50)
  # The scripting API's speed (managed/Bench): each call under a ceiling, and a steady frame
  # allocating nothing, in both script modes, on the fox crowd (characters to drive).
  foreach(mode jit aot)
    add_test(NAME script_bench_${mode} COMMAND falcon --hidden --scene "${fox}" --crowd 8 --frames 1010 --audio-null
      --script "${fx_scripts}/bench/${mode}/Bench${fx_ext_${mode}}" --no-hot-reload)
    set_tests_properties(script_bench_${mode} PROPERTIES FIXTURES_REQUIRED fox_pak
      PASS_REGULAR_EXPRESSION "bench: PASS" FAIL_REGULAR_EXPRESSION "bench: FAIL")
  endforeach()
  # The same locomotion as a v2 graph (samples/graphs/fox.fgraph: shared @id nodes) animates alike;
  # a graph with errors reports each, with its node, in one cook.
  set(fox_v2 "${dir}/fox_v2.fpak")
  add_test(NAME render_anim_graph_v2_cook COMMAND falcon cook "${fx_fox}" "${fox_v2}" --graph "${CMAKE_SOURCE_DIR}/samples/graphs/fox.fgraph")
  set_tests_properties(render_anim_graph_v2_cook PROPERTIES FIXTURES_SETUP fox_v2_pak)
  add_test(NAME render_anim_graph_v2 COMMAND falcon --hidden --scene "${fox_v2}" --capture "${dir}/anim_graph_v2.png" --look base ${fx_crowd})
  set_tests_properties(render_anim_graph_v2 PROPERTIES FIXTURES_REQUIRED fox_v2_pak FIXTURES_SETUP capture_anim_graph_v2)
  fx_fox_compare(anim_graph_v2_same anim_cache anim_graph_v2 50)
  # Blend2D and layers (samples/graphs/fox_layers.fgraph): a blend space of Survey, Walk, Run and a
  # slow walk, with Survey over it masked to the neck and head ("Look"). The layer and the blend
  # space's point each change the pose; a blend space on one line and a mask's unknown bone are errors.
  set(fox_layers "${dir}/fox_layers.fpak")
  add_test(NAME render_anim_layers_cook COMMAND falcon cook "${fx_fox}" "${fox_layers}" --graph "${CMAKE_SOURCE_DIR}/samples/graphs/fox_layers.fgraph")
  set_tests_properties(render_anim_layers_cook PROPERTIES FIXTURES_SETUP fox_layers_pak)
  foreach(c "look_on;Look=1;X=0.5" "look_off;Look=0;X=0.5" "space_run;Look=0;X=3.5")
    list(GET c 0 name)
    list(GET c 1 look)
    list(GET c 2 x)
    add_test(NAME render_anim_layers_${name} COMMAND falcon --hidden --scene "${fox_layers}" --capture "${dir}/anim_layers_${name}.png"
      --look base --capture-frame 50 --fixed-time --anim-set ${look} --anim-set ${x})
    set_tests_properties(render_anim_layers_${name} PROPERTIES FIXTURES_REQUIRED fox_layers_pak FIXTURES_SETUP capture_anim_layers_${name})
  endforeach()
  foreach(c "mask;look_on;look_off" "space;look_off;space_run")
    list(GET c 0 name)
    list(GET c 1 a)
    list(GET c 2 b)
    add_test(NAME render_anim_layers_differ_${name} COMMAND falcon_imgdiff "${dir}/anim_layers_${a}.png" "${dir}/anim_layers_${b}.png"
      --tolerance 20 --min-pixels 5000 --max-pixels 10000000)
    set_tests_properties(render_anim_layers_differ_${name} PROPERTIES FIXTURES_REQUIRED "capture_anim_layers_${a};capture_anim_layers_${b}")
  endforeach()
  add_test(NAME render_anim_layers_errors COMMAND falcon cook "${fx_fox}" "${dir}/fox_bad2.fpak" --force
    --graph "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/bad_blend.fgraph")
  set_tests_properties(render_anim_layers_errors PROPERTIES PASS_REGULAR_EXPRESSION
    "line: blend2d's points are on one line.*tailless: no bone 'b_Wing_01' in the skeleton")
  add_test(NAME render_anim_graph_errors COMMAND falcon cook "${fx_fox}" "${dir}/fox_bad.fpak" --force
    --graph "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/bad_graph.fgraph")
  set_tests_properties(render_anim_graph_errors PROPERTIES PASS_REGULAR_EXPRESSION
    "machine:Move/pose: @walk is a pose already used elsewhere.*machine:Idle->Nowhere: unknown state 'Nowhere'.*moving/input: unknown parameter [$]Sped")
  # The Sandbox's footsteps (scripts' audio, API v7): the walking foxes step on what Physics.SurfaceAt
  # finds (nothing here: generic), synthesized without Footsteps/<surface>.fsound, heard from beside
  # one fox; JIT and NativeAOT give the same samples.
  foreach(mode jit aot)
    add_test(NAME audio_script_footsteps_${mode} COMMAND falcon --hidden --scene "${fox}" --crowd 24 --camera 402,40,30,402,0,0
      --fixed-time --frames 300 --script "${fx_scripts}/${mode}/Sandbox${fx_ext_${mode}}" --audio-reflections 0
      --audio-capture "${CMAKE_BINARY_DIR}/test/audio/footsteps_${mode}.wav")
    set_tests_properties(audio_script_footsteps_${mode} PROPERTIES FIXTURES_REQUIRED fox_pak FIXTURES_SETUP audio_script_footsteps_${mode}
      PASS_REGULAR_EXPRESSION "201 footsteps")
  endforeach()
  add_test(NAME audio_script_footsteps_check COMMAND falcon_wavdiff "${CMAKE_BINARY_DIR}/test/audio/footsteps_jit.wav" --rms 0.0001,0.3)
  set_tests_properties(audio_script_footsteps_check PROPERTIES FIXTURES_REQUIRED audio_script_footsteps_jit)
  add_test(NAME audio_script_footsteps_modes COMMAND falcon_wavdiff "${CMAKE_BINARY_DIR}/test/audio/footsteps_jit.wav"
    "${CMAKE_BINARY_DIR}/test/audio/footsteps_aot.wav" --exact)
  set_tests_properties(audio_script_footsteps_modes PROPERTIES FIXTURES_REQUIRED "audio_script_footsteps_jit;audio_script_footsteps_aot")
  # The same with the Fox's clip markup (samples/graphs/fox.fevents: footstep events on Walk and
  # Run, script API v8 Animation.Events): the Sandbox steps on the events instead; JIT and NativeAOT alike.
  foreach(mode jit aot)
    add_test(NAME audio_script_footsteps_events_${mode} COMMAND falcon --hidden --scene "${fox}" --crowd 24 --camera 402,40,30,402,0,0
      --fixed-time --frames 300 --script "${fx_scripts}/${mode}/Sandbox${fx_ext_${mode}}" --audio-reflections 0
      --anim-events "${CMAKE_SOURCE_DIR}/samples/graphs/fox.fevents"
      --audio-capture "${CMAKE_BINARY_DIR}/test/audio/footsteps_events_${mode}.wav")
    set_tests_properties(audio_script_footsteps_events_${mode} PROPERTIES FIXTURES_REQUIRED fox_pak
      FIXTURES_SETUP audio_script_footsteps_events_${mode}
      PASS_REGULAR_EXPRESSION "markup for 2 clips.*footsteps from animation events.*201 footsteps")
  endforeach()
  add_test(NAME audio_script_footsteps_events_modes COMMAND falcon_wavdiff "${CMAKE_BINARY_DIR}/test/audio/footsteps_events_jit.wav"
    "${CMAKE_BINARY_DIR}/test/audio/footsteps_events_aot.wav" --exact)
  set_tests_properties(audio_script_footsteps_events_modes PROPERTIES
    FIXTURES_REQUIRED "audio_script_footsteps_events_jit;audio_script_footsteps_events_aot")
endif()

# Editor (M9; needs a UI build). runtime/tests/editor_*.json replay page messages through the
# editor bridge in a real editor session (Chromium included):
#   editor_edit       move, hide, delete, create, rename, parent (a cooked mesh and a shape under
#                     an empty), undo/redo, play/stop, then save
#   editor_roundtrip  loading the saved edits and saving again writes the same file
#   editor_applied    a capture with the edits applied differs from the scene without them
#   editor_tools      create, duplicate, delete, GPU pick in the viewport, tools, sun, exposure and GI
#   editor_tools_upscaled  the same at half the render resolution (TAAU)
#                     settings, play / pause / step (test.expect steps fail the run)
#   editor_hierarchy  empties and parenting: children follow, undo / delete / duplicate cover
#                     subtrees, reparenting keeps world positions, an icon click selects an empty
#   editor_lights     point and spot lights created, edited and moved: with the shadow page
#                     cache the frame matches the one without (moved lights re-render their pages)
#   editor_camera     camera objects: Play and the Game tab look through the main camera, one main
#   editor_physics    Rigidbody and Box / Sphere Colliders: Play drops a crate onto a static platform
#                     (its child with it) and a ball onto Sponza; Stop puts them back
#   editor_physics_water  Rigidbody entities in a pool: a light crate and a ball float, a heavy
#                     crate sinks; Stop puts them back
#   editor_drag       mid-drag, the scene (GI included) matches what the committed move shows
#   editor_input      real mouse events: double-click selects a field, the wheel scrolls the
#                     hierarchy, right-click opens a row's menu, a Lighting slider drag tracks
#   editor_profiler   the Profiler tab: frames stream in with CPU zones (worker lanes too), GPU
#                     marks and counters; a chart click selects a frame; the Hierarchy fills
#   editor_terrain    a new terrain sculpted with the brush (real mouse events), undo and redo
#   editor_terrain_paint  material layers painted and holes cut with the brush, undo and redo
#   editor_terrain_scatter  a scatter instance picked, deleted and restored; a scatter mask painted, undo
#   editor_terrain_scatter_live  scatter meshes and layers added live; turn range, jitter; brush mode on and off
#   editor_terrain_scatter_paint  a scatter layer painted by hand, generated, removed (its paint cleared)
#   editor_node_anim        a model's rigid animation played (an arm and its box), stopped; _reload: saved
#   editor_terrain_generate  a generator preset run, undone and redone; a region with a step added; the erosion brush
if(FALCON_UI AND EXISTS "${fx_sponza}")
  set(dir "${CMAKE_BINARY_DIR}/test/editor")
  file(MAKE_DIRECTORY "${dir}")
  set(pak "${dir}/sponza.fpak")
  set(edits "${dir}/sponza.edits.json")
  set(scripts "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME editor_cook COMMAND falcon cook "${fx_sponza}" "${pak}" --rocks)
  add_test(NAME editor_clean COMMAND ${CMAKE_COMMAND} -E rm -f "${edits}" "${dir}/first.edits.json")
  set_tests_properties(editor_clean PROPERTIES DEPENDS editor_cook)
  add_test(NAME editor_edit COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --editor-script "${scripts}/editor_edit.json"
    --frames 1000000)
  set_tests_properties(editor_edit PROPERTIES DEPENDS editor_clean)
  add_test(NAME editor_keep_first COMMAND ${CMAKE_COMMAND} -E copy "${edits}" "${dir}/first.edits.json")
  set_tests_properties(editor_keep_first PROPERTIES DEPENDS editor_edit)
  add_test(NAME editor_resave COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --editor-script "${scripts}/editor_save.json"
    --frames 1000000)
  set_tests_properties(editor_resave PROPERTIES DEPENDS editor_keep_first)
  add_test(NAME editor_roundtrip COMMAND ${CMAKE_COMMAND} -E compare_files "${dir}/first.edits.json" "${edits}")
  set_tests_properties(editor_roundtrip PROPERTIES DEPENDS editor_resave)
  add_test(NAME editor_capture_edited COMMAND falcon --hidden --scene "${pak}" --capture "${dir}/edited.png" --capture-frame 8)
  add_test(NAME editor_capture_plain COMMAND falcon --hidden --scene "${pak}" --capture "${dir}/plain.png" --capture-frame 8 --no-edits)
  set_tests_properties(editor_capture_edited editor_capture_plain PROPERTIES DEPENDS editor_roundtrip)
  add_test(NAME editor_applied COMMAND falcon_imgdiff "${dir}/edited.png" "${dir}/plain.png" --tolerance 3 --max-pixels 500)
  set_tests_properties(editor_applied PROPERTIES DEPENDS "editor_capture_edited;editor_capture_plain" WILL_FAIL TRUE)
  add_test(NAME editor_tools COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_tools.json" --frames 1000000)
  set_tests_properties(editor_tools PROPERTIES DEPENDS editor_applied)
  # The same at half the render resolution (TAAU): picking and the selection outline map the
  # display-sized viewport onto the render-sized visibility buffer.
  add_test(NAME editor_tools_upscaled COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --upscaler taau --upscale-quality performance --editor-script "${scripts}/editor_tools.json" --frames 1000000)
  set_tests_properties(editor_tools_upscaled PROPERTIES DEPENDS editor_tools)
  add_test(NAME editor_input COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_input.json" --frames 1000000)
  set_tests_properties(editor_input PROPERTIES DEPENDS editor_tools)
  add_test(NAME editor_hierarchy COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_hierarchy.json" --frames 1000000)
  set_tests_properties(editor_hierarchy PROPERTIES DEPENDS editor_input)
  foreach(mode cache nocache)
    set(flags "")
    if(mode STREQUAL "nocache")
      set(flags --no-shadow-cache)
    endif()
    add_test(NAME editor_lights_${mode} COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits ${flags}
      --editor-script "${scripts}/editor_lights.json" --frames 1000000)
    add_test(NAME editor_lights_${mode}_keep COMMAND ${CMAKE_COMMAND} -E copy "${dir}/light_capture.png" "${dir}/lights_${mode}.png")
    set_tests_properties(editor_lights_${mode} PROPERTIES WORKING_DIRECTORY "${dir}")
    set_tests_properties(editor_lights_${mode}_keep PROPERTIES DEPENDS editor_lights_${mode})
  endforeach()
  set_tests_properties(editor_lights_cache PROPERTIES DEPENDS editor_hierarchy)
  set_tests_properties(editor_lights_nocache PROPERTIES DEPENDS editor_lights_cache_keep)
  add_test(NAME editor_lights COMMAND falcon_imgdiff "${dir}/lights_cache.png" "${dir}/lights_nocache.png" --tolerance 3
    --max-pixels 50 --out "${dir}/lights_diff.png")
  set_tests_properties(editor_lights PROPERTIES DEPENDS editor_lights_nocache_keep)
  # Bodies to simulate, so physics tasks run on the workers (their timeline lanes).
  add_test(NAME editor_profiler COMMAND falcon --hidden --physics-drop 256 --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_profiler.json" --frames 1000000)
  add_test(NAME editor_camera COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_camera.json" --frames 1000000)
  set_tests_properties(editor_camera PROPERTIES DEPENDS editor_lights)
  #   editor_fly_keys   right-drag flies on WASD's key events alone, and with a page field focused
  add_test(NAME editor_fly_keys COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_fly_keys.json" --frames 1000000)
  #   editor_page_reload  live reload: a file of the editor's page bundle (a copy of it, so other tests'
  #                       pages stay) written and removed; the page loads again each time, with the state
  add_test(NAME editor_page_bundle COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/bin/falcon-ui/editor"
    "${dir}/editor-page-copy")
  set_tests_properties(editor_page_bundle PROPERTIES FIXTURES_SETUP editor_page_bundle)
  add_test(NAME editor_page_reload COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --ui "${dir}/editor-page-copy" --editor-script "${scripts}/editor_page_reload.json" --frames 1000000)
  set_tests_properties(editor_page_reload PROPERTIES FIXTURES_REQUIRED editor_page_bundle DEPENDS editor_cook)
  add_test(NAME editor_physics COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_physics.json" --frames 1000000)
  set_tests_properties(editor_physics PROPERTIES DEPENDS editor_camera)
  add_test(NAME editor_physics_water COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_physics_water.json" --frames 1000000)
  set_tests_properties(editor_physics_water PROPERTIES DEPENDS editor_physics)
  add_test(NAME editor_drag_run COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits
    --editor-script "${scripts}/editor_drag.json" --frames 1000000)
  set_tests_properties(editor_drag_run PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_camera)
  # (The probes round the ball settle twice, once after the drag and once after the commit, each
  # time reading their neighbours as those settle too: ~130 pixels on a few edges differ by up to
  # 12. With the GI left as it was before the drag, 168,000 do.)
  add_test(NAME editor_drag COMMAND falcon_imgdiff "${dir}/drag_live.png" "${dir}/drag_committed.png" --tolerance 3
    --max-pixels 600 --out "${dir}/drag_diff.png")
  set_tests_properties(editor_drag PROPERTIES DEPENDS editor_drag_run)
  set_tests_properties(editor_profiler PROPERTIES DEPENDS editor_drag)
  # A new terrain sculpted through real mouse events on the scene view; undo and redo restore it
  # exactly (not saved: the scene folder stays free of a terrain for the other tests).
  add_test(NAME editor_terrain COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --editor-script "${scripts}/editor_terrain.json" --frames 1000000)
  set_tests_properties(editor_terrain PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_profiler)
  add_test(NAME editor_terrain_paint COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --editor-script "${scripts}/editor_terrain_paint.json" --frames 1000000)
  set_tests_properties(editor_terrain_paint PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_terrain)
  # Scatter on a terrain given on the command line (its instance slots are reserved at load).
  add_test(NAME editor_terrain_scatter COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --terrain 1 --terrain-hills 0 --terrain-scatter "cube:2:40000" --editor-script "${scripts}/editor_terrain_scatter.json" --frames 1000000)
  set_tests_properties(editor_terrain_scatter PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_terrain_paint)
  # Scatter layers and meshes added while editing show at once; randomness; brush mode (real clicks).
  add_test(NAME editor_terrain_scatter_live COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --terrain 1 --terrain-hills 0 --terrain-scatter "cube:0.5:40000" --editor-script "${scripts}/editor_terrain_scatter_live.json" --frames 1000000)
  set_tests_properties(editor_terrain_scatter_live PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_terrain_scatter)
  # Scatter painted by hand: a new layer grows only where painted; its paint cleared when removed.
  add_test(NAME editor_terrain_scatter_paint COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --terrain 1 --terrain-hills 0 --editor-script "${scripts}/editor_terrain_scatter_paint.json" --frames 1000000)
  set_tests_properties(editor_terrain_scatter_paint PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_terrain_scatter_live)
  # Rigid node animation: a model with an animated arm and camera (runtime/tests/data/animated.gltf),
  # cooked as a library; placed, played (the box turns with its arm), stopped; reopened, played again.
  set(fx_anim_data "${CMAKE_SOURCE_DIR}/runtime/tests/data")
  add_test(NAME editor_node_anim_cook COMMAND falcon cook --empty "${dir}/anim.fpak" --source-root "${fx_anim_data}"
    --library "${fx_anim_data}/animated.gltf" --verify --force)
  set_tests_properties(editor_node_anim_cook PROPERTIES PASS_REGULAR_EXPRESSION "node animation: 2 tracks.*verify: 0 violations")
  add_test(NAME editor_node_anim_clean COMMAND "${CMAKE_COMMAND}" -E rm -f "${dir}/anim.edits.json")
  foreach(step "" _reload)
    add_test(NAME editor_node_anim${step} COMMAND falcon --hidden --scene "${dir}/anim.fpak" --editor --no-prefs --no-terrain
      --editor-script "${scripts}/editor_node_anim${step}.json" --frames 1000000)
    set_tests_properties(editor_node_anim${step} PROPERTIES WORKING_DIRECTORY "${dir}")
  endforeach()
  set_tests_properties(editor_node_anim_clean PROPERTIES DEPENDS editor_node_anim_cook)
  set_tests_properties(editor_node_anim PROPERTIES DEPENDS editor_node_anim_clean)
  set_tests_properties(editor_node_anim_reload PROPERTIES DEPENDS editor_node_anim)
  add_test(NAME editor_terrain_generate COMMAND falcon --hidden --scene "${pak}" --editor --no-prefs --no-edits --no-terrain
    --editor-script "${scripts}/editor_terrain_generate.json" --frames 1000000)
  set_tests_properties(editor_terrain_generate PROPERTIES WORKING_DIRECTORY "${dir}" DEPENDS editor_terrain_scatter)
endif()

# Projects (no source scene needed): the launcher creates one (its empty scene cooked by a child
# falcon cook --empty) in a fresh folder; the editor opens it, makes a terrain, saves, adds a scene;
# opened again, the terrain is there. --no-relaunch: opening records the command instead.
#   launcher_create         a project made through the launcher's messages, on the recent list
#   editor_project          a terrain saved with the empty scene; New Scene; back to the launcher
#   editor_project_reload   the saved terrain found again
#   editor_assets           the Project panel: folders made, renamed, moved; search; a scene
#                           duplicated, renamed and deleted with its companions
#   editor_previews         previews rendered in the background, shown, cached
#   editor_previews_cached  opened again: every preview from the cache, no preview run
#   editor_assets_place     a rock dragged into the view stands on the terrain; a model imported:
#                           the scene asks to be reloaded; Reload cooks it in the background
#   editor_assets_model     reopened: the cook moved in, with the model; the whole model placed;
#                           its preview
#   editor_assets_model_reload  both saved
#   editor_materials        a material made, edited, dropped on a sphere, undone and redone, saved
#   editor_materials_reload reopened: the sphere draws with it again
#   editor_prefabs          a prefab made from a group, placed again, applied, reverted, overridden
#   editor_prefabs_reload   reopened from the prefab and the overrides; one instance unpacked
#   editor_prefabs_unpacked the unpacked instance stays plain
#   editor_displace         a height map and a displaced material on a cube (drawn plain until a cook)
#   editor_displace_reload  reopened (cooked): the cube draws its displaced copy, a duplicate too
#   project_cook            cooks before opening: a broken model's error in a dialog; waiting
#                           for another process's cook lock; a model's copy reusing the textures
#   project_cook_reload     the scene (its cook moved in when project_cook opened it again) loads
#                           with both models; the lock project_cook left is never looked at
#   editor_scripts          the project's C# game: the template built; a test game built by Play,
#                           run, and undone by Stop (its entities, component and body gone, what
#                           it moved back), twice; a compile error keeps Play from starting
#   editor_scripts_reload   reopened: the last build loads at startup and plays within two frames
#   editor_scripts_instant  reopened with another engine's build: rebuilt at once; a saved change
#                           built in the background, then played within two frames on it; a
#                           compile error refuses Play without building
#   editor_project_settings LOD error, cull size, terrain error, full-detail distance set; _reload:
#                           back from project.falcon (no prefs)
#   editor_components       Add Component: a Light on a cube through the page's searchable list,
#                           a camera on an empty; undo, redo, Remove Component; _reload: both saved
if(FALCON_UI)
  set(dir "${CMAKE_BINARY_DIR}/test/project")
  file(MAKE_DIRECTORY "${dir}")
  set(scripts "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME project_clean COMMAND "${CMAKE_COMMAND}" "-DDIR=${dir}/projects" -P "${scripts}/clean_dir.cmake")
  set_tests_properties(project_clean PROPERTIES FIXTURES_SETUP project_dir)
  add_test(NAME launcher_create COMMAND falcon --hidden --no-prefs --no-relaunch
    --launcher-script "${scripts}/launcher_create.json" --frames 1000000)
  set_tests_properties(launcher_create PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_dir
    FIXTURES_SETUP project_made)
  add_test(NAME editor_project COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_project.json" --frames 1000000)
  set_tests_properties(editor_project PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made
    FIXTURES_SETUP project_saved)
  add_test(NAME editor_project_reload COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_project_reload.json" --frames 1000000)
  set_tests_properties(editor_project_reload PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved)
  add_test(NAME editor_assets COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_assets.json" --frames 1000000)
  set_tests_properties(editor_assets PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved
    DEPENDS editor_project_reload)
  add_test(NAME editor_previews COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_previews.json" --frames 1000000)
  set_tests_properties(editor_previews PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved
    DEPENDS editor_assets)
  add_test(NAME editor_previews_cached COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_previews_cached.json" --frames 1000000)
  set_tests_properties(editor_previews_cached PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved
    DEPENDS editor_previews)
  # Placing, importing a model (Khronos DamagedHelmet, when the samples are there) and placing it.
  set(fx_helmet "${CMAKE_SOURCE_DIR}/samples/_src/khronos/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb")
  if(EXISTS "${fx_helmet}")
    add_test(NAME editor_assets_sample COMMAND "${CMAKE_COMMAND}" -E copy "${fx_helmet}" "${dir}/samples/DamagedHelmet.glb")
    set_tests_properties(editor_assets_sample PROPERTIES DEPENDS editor_previews_cached)
    foreach(step place model model_reload)
      add_test(NAME editor_assets_${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
        --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_assets_${step}.json" --frames 1000000)
      set_tests_properties(editor_assets_${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved)
    endforeach()
    set_tests_properties(editor_assets_place PROPERTIES DEPENDS editor_assets_sample)
    set_tests_properties(editor_assets_model PROPERTIES DEPENDS editor_assets_place)
    set_tests_properties(editor_assets_model_reload PROPERTIES DEPENDS editor_assets_model)
  endif()
  # Materials: made, edited, dropped on a sphere (the frame changes), undone, saved; reopened, the same.
  add_test(NAME editor_materials COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_materials.json" --frames 1000000)
  set_tests_properties(editor_materials PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_saved
    DEPENDS "editor_previews_cached;editor_assets_model_reload" FIXTURES_SETUP material_saved)
  add_test(NAME editor_materials_reload COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_materials_reload.json" --frames 1000000)
  set_tests_properties(editor_materials_reload PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED material_saved
    DEPENDS editor_materials)
  # The sphere red where it was white: only it changes that much (exposure moves the rest a little).
  add_test(NAME editor_materials_differs COMMAND falcon_imgdiff "${dir}/material_before.png" "${dir}/material_after.png"
    --tolerance 60 --min-pixels 800 --max-pixels 20000 --out "${dir}/material_differs_diff.png")
  set_tests_properties(editor_materials_differs PROPERTIES FIXTURES_REQUIRED material_saved DEPENDS editor_materials)
  # Prefabs: made, placed again, applied, reverted, overridden; reopened; unpacked.
  foreach(step "" _reload _unpacked)
    add_test(NAME editor_prefabs${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_prefabs${step}.json" --frames 1000000)
    set_tests_properties(editor_prefabs${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED material_saved)
  endforeach()
  set_tests_properties(editor_prefabs PROPERTIES DEPENDS editor_materials_reload)
  set_tests_properties(editor_prefabs_reload PROPERTIES DEPENDS editor_prefabs)
  set_tests_properties(editor_prefabs_unpacked PROPERTIES DEPENDS editor_prefabs_reload)
  # Height displacement: a height map and a material with it on a cube; reopened (cooked), the
  # cube draws its displaced copy, and the view differs where the studs are.
  add_test(NAME editor_displace_sample COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/runtime/tests/data/height_studs.png"
    "${dir}/samples/height_studs.png")
  foreach(step "" _reload)
    add_test(NAME editor_displace${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_displace${step}.json" --frames 1000000)
    set_tests_properties(editor_displace${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED material_saved)
  endforeach()
  set_tests_properties(editor_displace PROPERTIES DEPENDS "editor_prefabs_unpacked;editor_displace_sample")
  set_tests_properties(editor_displace_reload PROPERTIES DEPENDS editor_displace)
  add_test(NAME editor_displace_differs COMMAND falcon_imgdiff "${dir}/displace_before.png" "${dir}/displace_after.png"
    --tolerance 30 --min-pixels 2000 --max-pixels 200000 --out "${dir}/displace_differs_diff.png")
  set_tests_properties(editor_displace_differs PROPERTIES FIXTURES_REQUIRED material_saved DEPENDS editor_displace_reload)
  # Cooking before opening: errors, the lock, textures reused (the helmet imported twice).
  if(EXISTS "${fx_helmet}")
    foreach(step "" _reload)
      add_test(NAME project_cook${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
        --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/project_cook${step}.json" --frames 1000000)
      set_tests_properties(project_cook${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED material_saved)
    endforeach()
    set_tests_properties(project_cook PROPERTIES DEPENDS editor_displace_differs)
    set_tests_properties(project_cook_reload PROPERTIES DEPENDS project_cook)
  endif()
  # The project's C# game (needs the .NET SDK, as the build does).
  foreach(step "" _reload _instant)
    add_test(NAME editor_scripts${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_scripts${step}.json" --frames 1000000)
    set_tests_properties(editor_scripts${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED material_saved)
  endforeach()
  set_tests_properties(editor_scripts PROPERTIES DEPENDS "editor_displace_differs;project_cook_reload")
  set_tests_properties(editor_scripts_reload PROPERTIES DEPENDS editor_scripts)
  set_tests_properties(editor_scripts_instant PROPERTIES DEPENDS editor_scripts_reload)
  # Project settings (LOD error, cull size, terrain error, full-detail distance): saved with the
  # project, not the editor's prefs.
  foreach(step "" _reload)
    add_test(NAME editor_project_settings${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_project_settings${step}.json" --frames 1000000)
    set_tests_properties(editor_project_settings${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made)
  endforeach()
  set_tests_properties(editor_project_settings PROPERTIES DEPENDS editor_scripts_instant)
  set_tests_properties(editor_project_settings_reload PROPERTIES DEPENDS editor_project_settings)
  # Project settings in a short window: on screen whole, its body scrolling.
  add_test(NAME editor_project_settings_scroll COMMAND falcon --hidden --editor --no-prefs --no-relaunch --size 1280x600
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_project_settings_scroll.json" --frames 1000000)
  set_tests_properties(editor_project_settings_scroll PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made
    DEPENDS editor_project_settings_reload)
  # The network: Project Settings > Network's presets (saved with the project), and Play with a
  # client process of the scene (the Play bar's Clients) seen in the Network tab.
  add_test(NAME editor_network COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_network.json" --frames 1000000)
  set_tests_properties(editor_network PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made
    DEPENDS editor_project_settings_reload)
  # Water: bodies made, sized, their material overridden and saved as a .fwater; reopened, the same.
  foreach(step "" _reload)
    add_test(NAME editor_water${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_water${step}.json" --frames 1000000)
    set_tests_properties(editor_water${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made)
  endforeach()
  set_tests_properties(editor_water PROPERTIES DEPENDS editor_project_settings_reload)
  set_tests_properties(editor_water_reload PROPERTIES DEPENDS editor_water)
  # Add Component: a Light and a camera added in the Inspector (clicks, search), undone, removed; reopened, saved.
  foreach(step "" _reload)
    add_test(NAME editor_components${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_components${step}.json" --frames 1000000)
    set_tests_properties(editor_components${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED project_made)
  endforeach()
  set_tests_properties(editor_components PROPERTIES DEPENDS editor_water_reload)
  set_tests_properties(editor_components_reload PROPERTIES DEPENDS editor_components)
  # Audio: a clip imported and made a sound, an Audio Source and Listener added (undo, preview,
  # Play, Stop), a clip placed in the scene, the master volume; reopened, the same.
  add_test(NAME editor_audio_clip COMMAND falcon_wavdiff --write-tone "${dir}/beep.wav" 880 44100 1 0.5)
  set_tests_properties(editor_audio_clip PROPERTIES FIXTURES_SETUP audio_clip)
  foreach(step "" _reload)
    add_test(NAME editor_audio${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio${step}.json" --frames 1000000)
    set_tests_properties(editor_audio${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_clip")
  endforeach()
  set_tests_properties(editor_audio PROPERTIES DEPENDS editor_components_reload FIXTURES_SETUP audio_project)
  set_tests_properties(editor_audio_reload PROPERTIES DEPENDS editor_audio)
  # Sound variation: a one-shot varied by default; the source's Vary or the project's setting off.
  add_test(NAME editor_audio_variation COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_variation.json" --frames 1000000)
  set_tests_properties(editor_audio_variation PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS editor_audio_reload)
  # Occlusion and transmission: a wall between the view and a source, its acoustics (generic, its
  # own, left out, a .fmat's), moved aside; reopened, its own acoustics kept.
  foreach(step "" _reload)
    add_test(NAME editor_audio_occlusion${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_occlusion${step}.json" --frames 1000000)
    set_tests_properties(editor_audio_occlusion${step} PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project")
  endforeach()
  # Reverb: next to none in the open; a closed concrete room rings, a carpeted one hardly; a source
  # in it gets reflections of its own.
  add_test(NAME editor_audio_reverb COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_reverb.json" --frames 1000000)
  set_tests_properties(editor_audio_reverb PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS editor_audio_terrain)
  # Physics sounds: a crate's impact, a box scraping down a ramp, a ball rolling.
  add_test(NAME editor_audio_physics COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_physics.json" --frames 1000000)
  set_tests_properties(editor_audio_physics PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS editor_audio_reverb)
  # Water sounds: a pool's Sound fields, splashes while the game runs, the listener
  # under the surface muffled; Stop silences them.
  add_test(NAME editor_audio_water COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_water.json" --frames 1000000)
  set_tests_properties(editor_audio_water PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS editor_audio_physics)
  # The scripts' audio (API v7): a game playing, following, stopping and re-pitching sounds, a bus
  # volume, its ball's contacts and the surface under a point, on the HUD; the earlier game put back.
  add_test(NAME editor_audio_scripts COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_scripts.json" --frames 1000000)
  set_tests_properties(editor_audio_scripts PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS "editor_audio_water;editor_scripts_instant")
  # Baked acoustics (Steam Audio's probes): two concrete rooms joined by a doorway baked in the editor
  # (the reverb from the probes in reach, a sound behind the wall heard through the doorway, the bake
  # out of date while the wall is moved); baked again headless (--audio-bake, as a build would);
  # reopened, up to date, and cleared.
  if(FALCON_STEAM_AUDIO)
    add_test(NAME editor_audio_bake COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_bake.json" --frames 1000000)
    set_tests_properties(editor_audio_bake PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
      DEPENDS editor_audio_scripts)
    add_test(NAME audio_bake_cli COMMAND falcon --hidden --project "${dir}/projects/Launcher Test"
      --scene "${dir}/projects/Launcher Test/Scenes/Main.fpak" --audio-bake --frames 100000)
    set_tests_properties(audio_bake_cli PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
      DEPENDS editor_audio_bake PASS_REGULAR_EXPRESSION "audio: baked [0-9]+ probes .reverb, paths. in")
    add_test(NAME editor_audio_bake_reload COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_bake_reload.json" --frames 1000000)
    set_tests_properties(editor_audio_bake_reload PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
      DEPENDS audio_bake_cli)
  endif()
  # The terrain: a source under the ground, blocked, through its layer's acoustics.
  add_test(NAME editor_audio_terrain COMMAND falcon --hidden --editor --no-prefs --no-relaunch
    --project "${dir}/projects/Launcher Test" --editor-script "${scripts}/editor_audio_terrain.json" --frames 1000000)
  set_tests_properties(editor_audio_terrain PROPERTIES WORKING_DIRECTORY "${dir}" FIXTURES_REQUIRED "project_made;audio_project"
    DEPENDS editor_audio_occlusion_reload)
  # Animation in a project (its own): a skinned model (two bones, two clips) and its graph imported
  # and cooked with the model's skin; placed, an Animator added (its graph and parameter values;
  # Play, Stop, undo); reopened, the same, removed and undone.
  set(adir "${CMAKE_BINARY_DIR}/test/anim")
  file(MAKE_DIRECTORY "${adir}/data")
  add_test(NAME anim_project_clean COMMAND "${CMAKE_COMMAND}" "-DDIR=${adir}/projects" -P "${scripts}/clean_dir.cmake")
  add_test(NAME anim_project_data COMMAND "${CMAKE_COMMAND}" -E copy "${scripts}/data/skinned.gltf" "${scripts}/data/skinned.fgraph" "${scripts}/data/legs.gltf" "${adir}/data")
  add_test(NAME anim_project_create COMMAND falcon --hidden --no-prefs --no-relaunch
    --launcher-script "${scripts}/launcher_anim.json" --frames 1000000)
  set_tests_properties(anim_project_create PROPERTIES WORKING_DIRECTORY "${adir}" DEPENDS "anim_project_clean;anim_project_data"
    FIXTURES_SETUP anim_project)
  foreach(step import place place_reload preview graph graph_reload events masks script bones bones_reload hide ik_import ik ik_reload ik_legacy
      ik_legacy_reload edit_preview edit_preview_reload systems)
    add_test(NAME editor_anim_${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${adir}/projects/Anim Test" --editor-script "${scripts}/editor_anim_${step}.json" --frames 1000000)
    set_tests_properties(editor_anim_${step} PROPERTIES WORKING_DIRECTORY "${adir}" FIXTURES_REQUIRED anim_project)
  endforeach()
  set_tests_properties(editor_anim_import PROPERTIES DEPENDS anim_project_create)
  set_tests_properties(editor_anim_place PROPERTIES DEPENDS editor_anim_import)
  set_tests_properties(editor_anim_place_reload PROPERTIES DEPENDS editor_anim_place)
  # The Animator tab's preview pane (its own renderer): a parameter and a step change the pose,
  # bones show, a drag orbits; the scene view comes back as it was.
  set_tests_properties(editor_anim_preview PROPERTIES DEPENDS editor_anim_place_reload FIXTURES_SETUP anim_preview)
  function(fx_anim_differs name a b)
    add_test(NAME editor_anim_preview_${name} COMMAND falcon_imgdiff "${adir}/${a}.png" "${adir}/${b}.png" ${ARGN}
      --out "${adir}/anim_preview_${name}_diff.png")
    set_tests_properties(editor_anim_preview_${name} PROPERTIES FIXTURES_REQUIRED anim_preview DEPENDS editor_anim_preview)
  endfunction()
  fx_anim_differs(pose anim_preview_a anim_preview_b --tolerance 20 --min-pixels 500 --max-pixels 10000000)
  fx_anim_differs(bones anim_preview_b anim_preview_bones --tolerance 20 --min-pixels 20 --max-pixels 10000000)
  fx_anim_differs(orbit anim_preview_bones anim_preview_orbit --tolerance 20 --min-pixels 1000 --max-pixels 10000000)
  fx_anim_differs(return anim_scene_before anim_scene_back --tolerance 3 --max-pixels 500)
  fx_anim_differs(instant anim_scene_before anim_scene_back_1 --tolerance 8 --max-pixels 2000)
  # The graph editor: edits through its canvas's operations (errors on their node, live swaps of
  # the preview and the scene's character, undo / redo, states, transitions, parameters); saved and
  # reopened.
  set_tests_properties(editor_anim_graph PROPERTIES DEPENDS editor_anim_preview)
  set_tests_properties(editor_anim_graph_reload PROPERTIES DEPENDS editor_anim_graph)
  # Events: a clip's markup made in the Animator (saved beside the model), a transition waiting for
  # an event of it.
  set_tests_properties(editor_anim_events PROPERTIES DEPENDS editor_anim_graph_reload)
  # Layers and bone masks made in the Animator (a mask's bones from the skeleton; an unknown mask
  # an error on its node).
  set_tests_properties(editor_anim_masks PROPERTIES DEPENDS editor_anim_events)
  # Script API v9: a model found by name, its graph driven through its root; held keys and the
  # mouse over the game's view reach the game.
  set_tests_properties(editor_anim_script PROPERTIES DEPENDS editor_anim_masks)
  # Bones as entities: Expand Bones, an empty under a bone follows the animation, saved and reopened.
  set_tests_properties(editor_anim_bones PROPERTIES DEPENDS editor_anim_script)
  set_tests_properties(editor_anim_bones_reload PROPERTIES DEPENDS editor_anim_bones)
  # Script API v11: Animation.HideBone (first person's head): a bone shrunk in the skinning, a mesh
  # on hidden bones only drawn into the shadows only, every bone shown again on Stop.
  set_tests_properties(editor_anim_hide PROPERTIES DEPENDS editor_anim_bones_reload)
  # Inverse kinematics on a small rig on a slope, from its graph's IK (foot IK, a look, an aim and
  # hold, a hand's reach) and entity parameters bound in the Animator (saved); scripts' calls (API
  # v12-v14) and entity parameters (v16).
  set_tests_properties(editor_anim_ik_import PROPERTIES DEPENDS editor_anim_hide)
  set_tests_properties(editor_anim_ik PROPERTIES DEPENDS editor_anim_ik_import)
  set_tests_properties(editor_anim_ik_reload PROPERTIES DEPENDS editor_anim_ik)
  # Older scenes' Animator IK fields (Foot IK, targets) migrated as they are read, then moved into
  # the graph from the Inspector; saved and reopened.
  set_tests_properties(editor_anim_ik_legacy PROPERTIES DEPENDS editor_anim_ik_reload)
  set_tests_properties(editor_anim_ik_legacy_reload PROPERTIES DEPENDS editor_anim_ik_legacy)
  # Preview in Editor: an Animator playing in edit mode (IK following its entities), started again by
  # Play and Stop; saved and reopened.
  set_tests_properties(editor_anim_edit_preview PROPERTIES DEPENDS editor_anim_ik_legacy_reload)
  set_tests_properties(editor_anim_edit_preview_reload PROPERTIES DEPENDS editor_anim_edit_preview)
  # Script API v25 (S7): parameters read back, the state machine's state, a clip played over the graph
  # (held at rate 0, faded out), no terrain or water, a particle component's fields as JSON.
  set_tests_properties(editor_anim_systems PROPERTIES DEPENDS editor_anim_edit_preview_reload)
  set_tests_properties(editor_audio_occlusion PROPERTIES DEPENDS editor_audio_reload FIXTURES_SETUP audio_occlusion_saved)
  set_tests_properties(editor_audio_occlusion_reload PROPERTIES DEPENDS editor_audio_occlusion FIXTURES_REQUIRED
    "project_made;audio_project;audio_occlusion_saved")

  # Particles and decals (engine/vfx), on a project of their own:
  #   editor_particles  - an effect and its parts added and set (undo, redo), a child's renderer
  #                       bound to its parent's effect, gravity, a Temporary effect gone in Play and
  #                       back at Stop; editor_particles_reload: all of it from the saved scene
  #   editor_particles_render - sprites drawn (captures with and without them, and velocity-stretched
  #                       and lit ones); compared by editor_particles_drawn / _aligned
  #   editor_particles_textured - a 4x4 flipbook made a material and drawn by a Sheet; _reload (cooked
  #                       first): its frames streamed from the virtual texture (editor_particles_textured_drawn)
  #   editor_particle_scripts - script API v15: a C# ParticleController, Prefab.Spawn, Particles.Hits
  #   editor_decals     - a crater's colour, normal and height made a material, a Decal on a floor;
  #                       _reload (cooked): drawn (editor_decals_drawn), deeper with parallax (_parallax)
  #   editor_particle_models - particles drawn as cubes (a Model Renderer: counted, at most its Max models,
  #                       none without slots; editor_particle_models_drawn), a game's spawned prefab
  #                       with a mesh, a body and a collider resting on a floor, deleted at Stop
  set(vdir "${CMAKE_BINARY_DIR}/test/vfx")
  file(MAKE_DIRECTORY "${vdir}")
  add_test(NAME vfx_project_clean COMMAND "${CMAKE_COMMAND}" "-DDIR=${vdir}/projects" -P "${scripts}/clean_dir.cmake")
  add_test(NAME vfx_project_create COMMAND falcon --hidden --no-prefs --no-relaunch
    --launcher-script "${scripts}/launcher_vfx.json" --frames 1000000)
  set_tests_properties(vfx_project_create PROPERTIES WORKING_DIRECTORY "${vdir}" DEPENDS vfx_project_clean FIXTURES_SETUP vfx_project)
  add_test(NAME vfx_samples COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/runtime/tests/data/vfx" "${vdir}/samples")
  foreach(step particles particles_reload particles_render particles_textured particles_textured_reload particle_scripts decals decals_reload particle_models)
    add_test(NAME editor_${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${vdir}/projects/VFX Test" --editor-script "${scripts}/editor_${step}.json" --frames 1000000)
    set_tests_properties(editor_${step} PROPERTIES WORKING_DIRECTORY "${vdir}" FIXTURES_REQUIRED vfx_project)
  endforeach()
  set_tests_properties(editor_particles PROPERTIES DEPENDS vfx_project_create)
  set_tests_properties(editor_particles_reload PROPERTIES DEPENDS editor_particles)
  set_tests_properties(editor_particles_render PROPERTIES DEPENDS editor_particles_reload FIXTURES_SETUP vfx_render)
  foreach(case "drawn;particles_on;particles_off;200" "aligned;particles_aligned;particles_off;100" "trail;particles_trail;particles_off;100"
               "light;particles_lit_floor;particles_unlit_floor;2000")
    list(GET case 0 name)
    list(GET case 1 a)
    list(GET case 2 b)
    list(GET case 3 pixels)
    add_test(NAME editor_particles_${name} COMMAND falcon_imgdiff "${vdir}/${a}.png" "${vdir}/${b}.png" --tolerance 20
      --min-pixels ${pixels} --max-pixels 100000000 --out "${vdir}/particles_${name}_diff.png")
    set_tests_properties(editor_particles_${name} PROPERTIES FIXTURES_REQUIRED vfx_render DEPENDS editor_particles_render)
  endforeach()
  set_tests_properties(editor_particles_textured PROPERTIES DEPENDS "editor_particles_render;vfx_samples")
  set_tests_properties(editor_particles_textured_reload PROPERTIES DEPENDS editor_particles_textured)
  set_tests_properties(editor_particle_scripts PROPERTIES DEPENDS editor_particles_textured_reload)
  set_tests_properties(editor_decals PROPERTIES DEPENDS "editor_particle_scripts;vfx_samples")
  set_tests_properties(editor_decals_reload PROPERTIES DEPENDS editor_decals)
  set_tests_properties(editor_particle_models PROPERTIES DEPENDS editor_decals_reload)
  add_test(NAME editor_particle_models_drawn COMMAND falcon_imgdiff "${vdir}/models_on.png" "${vdir}/models_off.png"
    --tolerance 20 --min-pixels 3000 --max-pixels 100000000 --out "${vdir}/models_diff.png")
  set_tests_properties(editor_particle_models_drawn PROPERTIES DEPENDS editor_particle_models)
  foreach(case "drawn;decals_on;decals_off;2000" "parallax;decals_on;decals_flat;300")
    list(GET case 0 name)
    list(GET case 1 a)
    list(GET case 2 b)
    list(GET case 3 pixels)
    add_test(NAME editor_decals_${name} COMMAND falcon_imgdiff "${vdir}/${a}.png" "${vdir}/${b}.png" --tolerance 12
      --min-pixels ${pixels} --max-pixels 100000000 --out "${vdir}/decals_${name}_diff.png")
    set_tests_properties(editor_decals_${name} PROPERTIES DEPENDS editor_decals_reload)
  endforeach()
  add_test(NAME editor_particles_textured_drawn COMMAND falcon_imgdiff "${vdir}/particles_textured.png" "${vdir}/particles_textured_off.png"
    --tolerance 20 --min-pixels 500 --max-pixels 100000000 --out "${vdir}/particles_textured_diff.png")
  set_tests_properties(editor_particles_textured_drawn PROPERTIES DEPENDS editor_particles_textured_reload)

  # The scripting API (scripting roadmap S1..), on a project of its own (each test writes its own
  # test game into Scripts/Game.cs; runtime/tests/data/scripts has their sources):
  #   editor_script_entities - API v18: names, the hierarchy, renaming, Destroy (a mesh no longer
  #                       drawn), Enabled taking a child with it, tint, shadows, material and
  #                       visibility as drawn, transforms, a cached Ref, a spawned mesh; Stop puts
  #                       every one back
  #   editor_script_physics - API v19: bodies (state, velocity, teleport), rays with layers and an
  #                       ignored entity, shape casts, overlaps, a hinge's motor, triggers, the
  #                       character controller (a step climbed, a wall), a collider set while playing
  #   editor_script_components - API v20: a C# script component added in the Inspector, its
  #                       properties set (an entity, an enum), undo, a prefab of it, Play (its
  #                       lifecycle, the spawned prefab's), a hot reload keeping its properties;
  #                       _reload: all of it from the saved scene
  set(sdir "${CMAKE_BINARY_DIR}/test/scripts")
  file(MAKE_DIRECTORY "${sdir}")
  add_test(NAME script_project_clean COMMAND "${CMAKE_COMMAND}" "-DDIR=${sdir}/projects" -P "${scripts}/clean_dir.cmake")
  set_tests_properties(script_project_clean PROPERTIES FIXTURES_SETUP script_project)  # (so -R editor_script_ runs both)
  add_test(NAME script_project_create COMMAND falcon --hidden --no-prefs --no-relaunch
    --launcher-script "${scripts}/launcher_scripts.json" --frames 1000000)
  set_tests_properties(script_project_create PROPERTIES WORKING_DIRECTORY "${sdir}" DEPENDS script_project_clean
    FIXTURES_SETUP script_project)
  set(fx_script_steps entities physics components components_reload)
  set(fx_library_steps library library_api library_ui game_ui script_input script_flow script_render script_scene script_material shaders post_effect surface_shader script_mesh script_systems script_data script_gizmos script_stream script_ticks transparent refraction vertex_shader level_live level_mesh prefab_nesting selection delete_many level_bar_start level_options level_mode_bar level_window level_menus level_chords level_build_preview level_commands level_ops level_uvs level_trims level_layers level_paint level_paint_panel level_cuts level_cut_art level_cut_bake level_async_cuts level_clip level_prototype level_walls level_openings level_kit level_roofs level_wear level_live_wear level_damage level_shadows level_closed_room level_round_wear level_inspect level_modifiers level_splines level_script_modifier level_arrange level_scatter level_snap level_navigation level_nav_terrain level_volumes level_agents level_views level_runtime_cuts level_sample asset_library game_build)
  set(fx_previous script_project_create)
  foreach(step ${fx_script_steps})
    add_test(NAME editor_script_${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${sdir}/projects/Script Test" --editor-script "${scripts}/editor_script_${step}.json" --frames 1000000)
    set_tests_properties(editor_script_${step} PROPERTIES WORKING_DIRECTORY "${sdir}" FIXTURES_REQUIRED script_project
      DEPENDS ${fx_previous})
    set(fx_previous editor_script_${step})
  endforeach()
  # Libraries (E1..E3), on the same project, after the script tests:
  #   editor_library - a library found (untrusted: not built), trusted, its component used, exported,
  #                    removed, added back from the zip and played, turned off; a new one made
  #   editor_library_api - a library's editor code: Tools menu items (undo grouped), an EditorTool
  #   editor_library_ui - a library's editor window (its HTML page and C# side talking both ways)
  #   editor_game_ui - the project's game UI (UI/Game) framed while playing, talking to the game
  #   editor_script_input - API v22: a virtual gamepad, actions, the full key enum, the view size
  #   editor_script_flow - API v23: time scale, saves, the camera's rays, Game.Quit stopping Play
  #   editor_script_render - API v24: render settings by name, debug lines (lasting, per frame)
  #   editor_script_scene - S8 (API v26): Scene.Load stops Play in the editor
  #   editor_script_material - R1 (API v29): material parameters and per-entity copies, restored at Stop
  #   editor_shaders - R2: the project's Shaders/ compiled in the background, errors with their line
  #   editor_post_effect - R3 (API v30): post effects before and after the tonemap
  #   editor_surface_shader - R4: a surface shader compiled into the project's resolve
  #   editor_script_mesh - R5 (API v31): runtime meshes, opaque and see-through, updated, gone at Stop
  #   editor_script_systems - X1 (API v32): systems' phases in order; collision and trigger events
  #   editor_script_data - X2 (API v33): a C# data component placed, set, saved, queried, undone
  #   editor_script_gizmos - X3 (API v34): gizmos editing and playing, [Button] methods, an Inspector page
  #   editor_script_stream - X4 (API v35): an audio stream heard, hull and compound shapes resting
  #   editor_script_ticks - API v36: the simulation's ticks: input edges per tick (a one-frame tap
  #                         too), Time in ticks, collisions inside their tick, DMath's bits
  #   editor_transparent - T1: alpha-blend materials blended in order over the scene, hidden ones drawing
  #                        nothing, picked
  #   editor_refraction - T2: a refracting glass ball (bent, blurred) and a surface shader on it
  #   editor_vertex_shader - V1: a surface shader's vertex function lifting and waving a sphere
  #   editor_level_mesh - level meshes: made in the editor and by editor code (undo steps), picked to the
  #                       face, saved and opened again, changed by the game while playing, back at Stop
  #   editor_level_commands - editor code's commands by key in a face mode, a modal operation (typed, Enter,
  #                           Esc), Shift+R, the keymap, the level toolbar, the command palette
  #   editor_level_ops - the level tools' own commands: extrude (typed), inset, delete, undo; an edge ring and a
  #                      loop cut; Alt+N; a cylinder drawn from the palette (corners on the ground, then its height)
  #   editor_level_uvs - per-face materials (dropped, set, the eyedropper, Alt+V, the palette's Alt+1), U unwrapping,
  #                      the UV editor (Alt+U): a drag moving the UVs, its Fit button
  #   editor_level_trims - trim sheets: the starter set, the trim panel's strip clicked (material and rows), T again,
  #                        a sheet saved and used
  #   editor_level_layers - layered surfaces: a surface material (+ in the Inspector), its mask filled (drawn), undone, vertex colours
  #   editor_level_paint - the paint brush: a layer stroke (one undo step), vertex colours painted, a stroke of
  #                        decals (drawn, undone, redone), Esc
  #   editor_level_paint_panel - the paint panel's materials: + adds one, a stain (live wear), the brush's row
  #                        picked, × removes; a click off the mesh selects another, on nothing keeps it; × closes
  #   editor_level_cuts - live cuts: a cut box through a level wall, moved away and back, deleted and back, saved
  #   editor_level_async_cuts - cuts on worker threads: a 10000-box slab drawn as it was while its moved hole is cut
  #   editor_level_clip - an open plane clipped by a cut sphere (no caps, badged), a closed box capped, saved and opened (from the bake)
  #   editor_level_prototype - blockouts' default: the prototype textures put into the project, level meshes drawn with Dark Grid
  #   editor_level_cut_bake - baked cuts: a cut wall and a cut cube saved, opened again drawn from the bake, a moved door cut again
  #   editor_level_cut_art - live cuts on imported art: a cube's cut copy, picked as the cube, moved, deleted
  #   editor_level_walls - walls: the wall tool, an L with a door (cut), taller, converted, a room, saved
  #   editor_level_openings - an opening slid along its wall (typed, undone), a door prefab with a leaf dropped
  #                           on the wall (into it there), the wall turned (its openings with it)
  #   editor_level_kit - Help/Make Starter Kit, its window dropped on a wall (into it, framed, cut)
  #   editor_level_roofs - Create/Roof over a room (gable), made hip and flat, undone, converted to a mesh, saved
  #   editor_level_wear - generated wear: edge wear and grime into a box's layers (drawn), each undone
  #   editor_level_damage - the damage brush: holes along a stroke (drawn, one undo step), a chipped edge, a
  #                         broken corner, saved
  #   editor_level_shadows - a level slab made after the scene opened casts its shadow on a level floor (drawn)
  #   editor_level_closed_room - inside a closed level room (walls and ceiling far thinner than the GI probes' spacing) the GI's sky
  #                              and bounce are black (drawn), outside its sky is not; a lamp inside lights it
  #   editor_level_round_wear - a worn box given a round Bevel modifier (wear all across the rounds, their faces'
  #                             own mask UVs), applied, chips along the rounds' middles
  #   editor_level_sample - Help/Make Sample Level in an empty scene: the room, door and window, worn concrete, the
  #                         road and fence, volumes and navigation (drawn), saved
  #   editor_level_runtime_cuts - a game's script cutting a wall while playing (Cuts, NetworkedCuts without a session:
  #                               the wall cut, its geometry hash changed and the same for the same cuts); Stop: whole again
  #   editor_level_views - orthographic on and off (Numpad 5), the top view, the measure tool across a 4 m box
  #   editor_level_agents - two NavAgents walking round a wall to their destinations while playing (Stop: back)
  #   editor_level_volumes - gameplay volumes: a trigger (not drawn, a box trigger over it, C# Volumes), a nav
  #                          blocker and a nav area in the navigation bake, a volume made a mesh again, saved
  #   editor_level_navigation - a navigation mesh baked over a floor with a wall (a path round it), baked again
  #                             after the wall moves (auto bake), a C# component's Navigation queries, saved
  #   editor_level_nav_terrain - the navigation mesh baked over a terrain round a level wall on it
  #   editor_level_snap - kit pieces with snap points in their prefab: one snapped onto another (moved and
  #                       turned), by a gizmo drag too; a door prefab's cut volume carried into its instances
  #   editor_level_scatter - a prefab scattered over a field in one stroke (a new scatter set; undone, redone),
  #                          saved, erased with Shift
  #   editor_level_arrange - align to the last selected (undone), distribute, drop to ground, group and ungroup,
  #                          replace with a prefab (undone), a prefab placed at the mouse
  #   editor_level_script_modifier - a C# modifier (MeshModifier) built and put on a level box, a setting of it
  #                                  changed and undone, its class removed (the box as it is again)
  #   editor_level_splines - a road along a spline (drawn), the Spline tool (drag, add, insert, take out, close),
  #                          a pipe loft, a post copied along it (the curve modifier), saved
  #   editor_level_modifiers - the modifier stack: a mirror and an array (drawn), reordered, switched off, removed in
  #                            the Inspector, a bevel, the mirror applied and undone, saved
  #   editor_level_live_wear - live wear: edge wear made again as a window is cut into a wall, grime where a
  #                            floor meets it (gone with the floor moved away), streaks below the sill, a rule
  #                            changed, baked and undone, saved
  #   editor_level_inspect - the level components in the Inspector: an opening's width, a wall's height, a damage
  #                          cut's seed typed into their sections, undone
  #   editor_selection - several selected (add, toggle, Delete, undo), a box, a two-entity gizmo drag, a
  #                      level mesh's faces picked and raised by the gizmo, a box of faces
  #   editor_delete_many - several selected in the Hierarchy: a right click keeps them, its menu's Delete and
  #                        the Delete key delete all of them (each undone)
  #   editor_level_bar_start - the level toolbar when the scripts are already built (the editor code loaded
  #                            before the page is ready)
  #   editor_level_options - the level commands' settings: all found once with a section and an icon; Bevel's
  #                          and the cylinder's read back, set from the page, used (and Bevel's wheel heard back)
  #   editor_level_mode_bar - the mode bar: shown with a level mesh selected, its buttons and keys picking the
  #                           element mode, its chip (the mesh's name, how many are chosen)
  #   editor_level_window   - the Level window: opens on the first level mesh, its sections per mode, a shape's
  #                           settings, greyed rows and why, search, icon mode, dragged and kept, Ctrl+Shift+L
  #   editor_level_menus    - Shift+A's create menu at the mouse (Cuts > Box makes a cut), GameObject > Level,
  #                           Help > Level Starter Content
  #   editor_level_chords   - the chord menus: Shift+3 opens the edge ring (the mode switched), greyed slots and why,
  #                           Backspace and Esc; Shift+4, 1, 1 extrudes at full speed with Shift still held
  #   editor_level_build_preview - the building tools' ghosts before a click (the wall's post, the door's outline
  #                           over a wall, the box's block) and right-click finishing (a right drag doesn't)
  #   editor_prefab_nesting - nested prefabs: a wall nested in a room, Apply adding, moving a nested entity
  #                           (an override of the nesting) and deleting, Unpack one level (undone), saved
  #   editor_level_live - level API v1: live meshes in the visibility buffer: a slab a ball lands on and a
  #                       ray hits, a panel whose triangle a click picks, bad data refused, gone at Stop
  #   editor_asset_library - the Project panel's Library: Falcon's materials listed (read-only, pictured by
  #                       their colour texture), one used (copied into Assets/Falcon) on a box face at its
  #                       real-world size; My Library's folder, material, import, rename, Add to Library
  #   editor_game_build - S8: File > Build Game (NativeAOT, the engine and the project in Build/; no content/library)
  foreach(step ${fx_library_steps})
    add_test(NAME editor_${step} COMMAND falcon --hidden --editor --no-prefs --no-relaunch
      --project "${sdir}/projects/Script Test" --editor-script "${scripts}/editor_${step}.json" --frames 1000000)
    set_tests_properties(editor_${step} PROPERTIES WORKING_DIRECTORY "${sdir}" FIXTURES_REQUIRED script_project
      DEPENDS ${fx_previous})
    set(fx_previous editor_${step})
  endforeach()
  # R1: the materials changed while playing (the shared one red, a copy blue) and put back at Stop.
  add_test(NAME editor_script_material_changed COMMAND falcon_imgdiff "${sdir}/material_before.png" "${sdir}/material_playing.png"
    --tolerance 20 --min-pixels 2000 --max-pixels 100000000 --out "${sdir}/material_changed_diff.png")
  set_tests_properties(editor_script_material_changed PROPERTIES DEPENDS editor_script_material)
  add_test(NAME editor_script_material_restored COMMAND falcon_imgdiff "${sdir}/material_before.png" "${sdir}/material_after.png"
    --tolerance 6 --max-pixels 200 --out "${sdir}/material_restored_diff.png")
  set_tests_properties(editor_script_material_restored PROPERTIES DEPENDS editor_script_material)
  # Scene.Load in a game on its own (the project's JIT build): Main, Second, Main, Second, Main in
  # one process, the game going on; it writes what it saw and quits.
  add_test(NAME script_scene_run COMMAND falcon --project "${sdir}/projects/Script Test" --hidden --frames 3000)
  set_tests_properties(script_scene_run PROPERTIES WORKING_DIRECTORY "${sdir}" FIXTURES_REQUIRED script_project
    DEPENDS editor_script_scene)
  add_test(NAME script_scene_loaded COMMAND "${CMAKE_COMMAND}" -DFILE=${sdir}/scene_loaded.txt -DSECONDS=90
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/wait_for_file.cmake")
  set_tests_properties(script_scene_loaded PROPERTIES PASS_REGULAR_EXPRESSION
    "loads 4/4, spinner 3/2, found True/False, marks 1-1, scene Scenes/Main.fpak, refused True, async 2, loading frames some, stages (starting\\+)?(reading\\+)?ready"
    DEPENDS script_scene_run)
  # (the scene tests' standalone runs play the Script Test game before the next test rewrites it)
  set_tests_properties(editor_script_material PROPERTIES DEPENDS "editor_script_scene;script_scene_loaded")
  set_tests_properties(editor_game_build PROPERTIES DEPENDS "editor_level_sample;editor_asset_library;script_scene_loaded")
  # Live meshes: drawn while playing (the slab and the panel), gone after Stop.
  add_test(NAME editor_level_live_drawn COMMAND falcon_imgdiff "${sdir}/level_live_before.png" "${sdir}/level_live_playing.png"
    --tolerance 12 --min-pixels 5000 --max-pixels 100000000 --out "${sdir}/level_live_drawn_diff.png")
  add_test(NAME editor_level_live_gone COMMAND falcon_imgdiff "${sdir}/level_live_before.png" "${sdir}/level_live_after.png"
    --tolerance 6 --max-pixels 300 --out "${sdir}/level_live_gone_diff.png")
  set_tests_properties(editor_level_live_drawn editor_level_live_gone PROPERTIES DEPENDS editor_level_live)
  # Layered surfaces: the red layer drawn where the mask says (all of it), the blue vertex colours.
  add_test(NAME editor_level_layers_drawn COMMAND falcon_imgdiff "${sdir}/level_layers_before.png" "${sdir}/level_layers_after.png"
    --tolerance 12 --min-pixels 3000 --max-pixels 100000000 --out "${sdir}/level_layers_drawn_diff.png")
  add_test(NAME editor_level_layers_tinted COMMAND falcon_imgdiff "${sdir}/level_layers_before.png" "${sdir}/level_layers_tinted.png"
    --tolerance 12 --min-pixels 3000 --max-pixels 100000000 --out "${sdir}/level_layers_tinted_diff.png")
  set_tests_properties(editor_level_layers_drawn editor_level_layers_tinted PROPERTIES DEPENDS editor_level_layers)
  add_test(NAME editor_level_wear_drawn COMMAND falcon_imgdiff "${sdir}/level_wear_before.png" "${sdir}/level_wear_after.png"
    --tolerance 12 --min-pixels 500 --max-pixels 100000000 --out "${sdir}/level_wear_drawn_diff.png")
  set_tests_properties(editor_level_wear_drawn PROPERTIES DEPENDS editor_level_wear)
  # A roof drawn over the room.
  add_test(NAME editor_level_roofs_drawn COMMAND falcon_imgdiff "${sdir}/level_roofs_before.png" "${sdir}/level_roofs_after.png"
    --tolerance 12 --min-pixels 3000 --max-pixels 100000000 --out "${sdir}/level_roofs_drawn_diff.png")
  set_tests_properties(editor_level_roofs_drawn PROPERTIES DEPENDS editor_level_roofs)
  # The sample level drawn.
  add_test(NAME editor_level_sample_drawn COMMAND falcon_imgdiff "${sdir}/level_sample_before.png" "${sdir}/level_sample_after.png"
    --tolerance 12 --min-pixels 5000 --max-pixels 100000000 --out "${sdir}/level_sample_drawn_diff.png")
  set_tests_properties(editor_level_sample_drawn PROPERTIES DEPENDS editor_level_sample)
  # Splines: the road drawn.
  add_test(NAME editor_level_splines_drawn COMMAND falcon_imgdiff "${sdir}/level_splines_before.png" "${sdir}/level_splines_after.png"
    --tolerance 12 --min-pixels 2000 --max-pixels 100000000 --out "${sdir}/level_splines_drawn_diff.png")
  set_tests_properties(editor_level_splines_drawn PROPERTIES DEPENDS editor_level_splines)
  # Modifiers: the mirrored, arrayed box drawn.
  add_test(NAME editor_level_modifiers_drawn COMMAND falcon_imgdiff "${sdir}/level_modifiers_before.png" "${sdir}/level_modifiers_after.png"
    --tolerance 12 --min-pixels 2000 --max-pixels 100000000 --out "${sdir}/level_modifiers_drawn_diff.png")
  set_tests_properties(editor_level_modifiers_drawn PROPERTIES DEPENDS editor_level_modifiers)
  # Live wear: the worn, grimy, streaked wall drawn.
  add_test(NAME editor_level_live_wear_drawn COMMAND falcon_imgdiff "${sdir}/level_live_wear_before.png" "${sdir}/level_live_wear_after.png"
    --tolerance 12 --min-pixels 500 --max-pixels 100000000 --out "${sdir}/level_live_wear_drawn_diff.png")
  set_tests_properties(editor_level_live_wear_drawn PROPERTIES DEPENDS editor_level_live_wear)
  # The damage brush: its holes drawn.
  add_test(NAME editor_level_damage_drawn COMMAND falcon_imgdiff "${sdir}/level_damage_before.png" "${sdir}/level_damage_after.png"
    --tolerance 12 --min-pixels 300 --max-pixels 100000000 --out "${sdir}/level_damage_drawn_diff.png")
  set_tests_properties(editor_level_damage_drawn PROPERTIES DEPENDS editor_level_damage)
  # Level meshes' shadows: the slab's (out of the view) darkens the floor.
  add_test(NAME editor_level_shadows_drawn COMMAND falcon_imgdiff "${sdir}/level_shadows_before.png" "${sdir}/level_shadows_after.png"
    --tolerance 12 --min-pixels 2000 --max-pixels 100000000 --out "${sdir}/level_shadows_drawn_diff.png")
  set_tests_properties(editor_level_shadows_drawn PROPERTIES DEPENDS editor_level_shadows)
  # No light leaks into a closed level room: its indirect light's sky part and bounce part each match it with
  # both off (black), while outside the sky part is drawn.
  foreach(part sky bounce)
    add_test(NAME editor_level_closed_room_${part} COMMAND falcon_imgdiff "${sdir}/level_closed_${part}.png" "${sdir}/level_closed_black.png"
      --tolerance 10 --max-pixels 2000 --out "${sdir}/level_closed_${part}_diff.png")
    set_tests_properties(editor_level_closed_room_${part} PROPERTIES DEPENDS editor_level_closed_room)
  endforeach()
  add_test(NAME editor_level_closed_room_open COMMAND falcon_imgdiff "${sdir}/level_closed_outside.png" "${sdir}/level_closed_black.png"
    --tolerance 10 --min-pixels 50000 --max-pixels 100000000 --out "${sdir}/level_closed_open_diff.png")
  set_tests_properties(editor_level_closed_room_open PROPERTIES DEPENDS editor_level_closed_room)
  # A point light inside: the probes' rays see it, and its light bounces round the room.
  add_test(NAME editor_level_closed_room_lamp COMMAND falcon_imgdiff "${sdir}/level_closed_lamp.png" "${sdir}/level_closed_black.png"
    --tolerance 10 --min-pixels 50000 --max-pixels 100000000 --out "${sdir}/level_closed_lamp_diff.png")
  set_tests_properties(editor_level_closed_room_lamp PROPERTIES DEPENDS editor_level_closed_room)
  # The decal stamp brush: its stroke's decals drawn.
  add_test(NAME editor_level_decals_drawn COMMAND falcon_imgdiff "${sdir}/level_decals_before.png" "${sdir}/level_decals_after.png"
    --tolerance 12 --min-pixels 300 --max-pixels 100000000 --out "${sdir}/level_decals_drawn_diff.png")
  set_tests_properties(editor_level_decals_drawn PROPERTIES DEPENDS editor_level_paint)
  # V1: the sphere lifted, waving (two moments), and still past its vertexDistance.
  function(fx_vertex_compare name a b)
    add_test(NAME editor_vertex_${name} COMMAND falcon_imgdiff "${sdir}/vertex_${a}.png" "${sdir}/vertex_${b}.png" ${ARGN}
      --out "${sdir}/vertex_${name}_diff.png")
    set_tests_properties(editor_vertex_${name} PROPERTIES DEPENDS editor_vertex_shader)
  endfunction()
  fx_vertex_compare(lifted still lifted --tolerance 12 --min-pixels 5000 --max-pixels 100000000)
  fx_vertex_compare(waving wave_a wave_b --tolerance 8 --min-pixels 1000 --max-pixels 100000000)
  fx_vertex_compare(far still far --tolerance 6 --max-pixels 200)
  # T2: what the glass ball shows bent by refraction, blurred when rough, and its surface shader's.
  function(fx_refract_compare name a b)
    add_test(NAME editor_refraction_${name} COMMAND falcon_imgdiff "${sdir}/refract_${a}.png" "${sdir}/refract_${b}.png" ${ARGN}
      --out "${sdir}/refract_${name}_diff.png")
    set_tests_properties(editor_refraction_${name} PROPERTIES DEPENDS editor_refraction)
  endfunction()
  fx_refract_compare(bent plain bent --tolerance 12 --min-pixels 2000 --max-pixels 100000000)
  fx_refract_compare(rough bent rough --tolerance 8 --min-pixels 1000 --max-pixels 100000000)
  fx_refract_compare(surface bent surface --tolerance 12 --min-pixels 2000 --max-pixels 100000000)
  # T1: the panes drawn over the wall; the one behind the wall drawing nothing; the nearer pane over
  # the farther one from the front (green) and from the back (blue).
  add_test(NAME editor_transparent_drawn COMMAND falcon_imgdiff "${sdir}/transparent_panes.png" "${sdir}/transparent_wall.png"
    --tolerance 12 --min-pixels 5000 --max-pixels 100000000 --out "${sdir}/transparent_drawn_diff.png")
  add_test(NAME editor_transparent_hidden COMMAND falcon_imgdiff "${sdir}/transparent_wall.png" "${sdir}/transparent_bare.png"
    --tolerance 3 --max-pixels 50 --out "${sdir}/transparent_hidden_diff.png")
  add_test(NAME editor_transparent_order_front COMMAND falcon_imgdiff "${sdir}/transparent_order_front.png" --dominant 500 280 g)
  add_test(NAME editor_transparent_order_back COMMAND falcon_imgdiff "${sdir}/transparent_order_back.png" --dominant 500 280 b)
  set_tests_properties(editor_transparent_drawn editor_transparent_hidden editor_transparent_order_front
    editor_transparent_order_back PROPERTIES DEPENDS editor_transparent)
  # R5: the meshes there while playing, changed by Update, gone after Stop.
  function(fx_mesh_compare name a b)
    add_test(NAME editor_mesh_${name} COMMAND falcon_imgdiff "${sdir}/mesh_${a}.png" "${sdir}/mesh_${b}.png" ${ARGN}
      --out "${sdir}/mesh_${name}_diff.png")
    set_tests_properties(editor_mesh_${name} PROPERTIES DEPENDS editor_script_mesh)
  endfunction()
  fx_mesh_compare(drawn before playing --tolerance 12 --min-pixels 5000 --max-pixels 100000000)
  fx_mesh_compare(updated playing updated --tolerance 12 --min-pixels 2000 --max-pixels 100000000)
  fx_mesh_compare(gone before after --tolerance 6 --max-pixels 300)
  # R4: the toon sphere differs from the plain one, its params changed while playing, back after Stop.
  function(fx_surface_compare name a b)
    add_test(NAME editor_surface_${name} COMMAND falcon_imgdiff "${sdir}/surface_${a}.png" "${sdir}/surface_${b}.png" ${ARGN}
      --out "${sdir}/surface_${name}_diff.png")
    set_tests_properties(editor_surface_${name} PROPERTIES DEPENDS editor_surface_shader)
  endfunction()
  fx_surface_compare(toon plain toon --tolerance 12 --min-pixels 5000 --max-pixels 100000000)
  fx_surface_compare(params toon playing --tolerance 12 --min-pixels 5000 --max-pixels 100000000)
  fx_surface_compare(restored toon after --tolerance 6 --max-pixels 300)
  # R3: the frame grey while playing, inverted on top with I, and as it was after Stop.
  function(fx_post_compare name a b)
    add_test(NAME editor_post_${name} COMMAND falcon_imgdiff "${sdir}/post_${a}.png" "${sdir}/post_${b}.png" ${ARGN}
      --out "${sdir}/post_${name}_diff.png")
    set_tests_properties(editor_post_${name} PROPERTIES DEPENDS editor_post_effect)
  endfunction()
  fx_post_compare(grey before grey --tolerance 12 --min-pixels 5000 --max-pixels 100000000)
  fx_post_compare(inverted grey inverted --tolerance 40 --min-pixels 100000 --max-pixels 100000000)
  fx_post_compare(restored before after --tolerance 6 --max-pixels 200)
  # The game Build Game made, started alone (its executable, no arguments but these): it plays the
  # project's published game, which writes that it ran (natively) and quits.
  add_test(NAME game_build_run COMMAND "${sdir}/projects/Script Test/Build/Script Test/Script Test.exe" --hidden --frames 3000)
  set_tests_properties(game_build_run PROPERTIES WORKING_DIRECTORY "${sdir}" FIXTURES_REQUIRED script_project
    DEPENDS editor_game_build)
  add_test(NAME game_build_ran COMMAND "${CMAKE_COMMAND}" -E cat "${sdir}/game_started.txt")
  set_tests_properties(game_build_ran PROPERTIES PASS_REGULAR_EXPRESSION "started, aot True, scene False" DEPENDS game_build_run)
endif()

# Audio tests (no scene, no GPU work to speak of): --hidden mixes on the null device from the fixed
# 60 Hz frame clock, so a run's capture is the same every time. falcon_wavdiff checks each WAV:
#   audio_tone       - a 2D 440 Hz test tone in both ears at its level (amplitude 0.25: RMS 0.177)
#   audio_tone_repeat - a second run gives the same samples
#   audio_wav        - a 44.1 kHz mono WAV, played back at 48 kHz: still 1 kHz, at its level
#   audio_right_pan / audio_left_pan - a tone 3 m to one side, panned: only in that ear
#   audio_right_hrtf / audio_left_hrtf - the same through Steam Audio's HRTF: the far ear hears it too,
#                      through the head, but quieter (at 2 kHz, where the head shadows it; little at 440 Hz)
set(adir "${CMAKE_BINARY_DIR}/test/audio")
file(MAKE_DIRECTORY "${adir}")
function(fx_audio name)
  add_test(NAME audio_${name} COMMAND falcon --hidden --fixed-time --frames 120 --audio-capture "${adir}/${name}.wav" ${ARGN})
  set_tests_properties(audio_${name} PROPERTIES FIXTURES_SETUP audio_${name})
endfunction()
function(fx_audio_check name capture)
  add_test(NAME audio_${name} COMMAND falcon_wavdiff "${adir}/${capture}.wav" ${ARGN})
  set_tests_properties(audio_${name} PROPERTIES FIXTURES_REQUIRED audio_${capture})
endfunction()

fx_audio(tone --audio-tone 440)
fx_audio_check(tone_check tone --skip 0.05 --rms 0.175,0.179 --tone 440)
fx_audio(tone_b --audio-tone 440)
add_test(NAME audio_tone_repeat COMMAND falcon_wavdiff "${adir}/tone.wav" "${adir}/tone_b.wav" --exact)
set_tests_properties(audio_tone_repeat PROPERTIES FIXTURES_REQUIRED "audio_tone;audio_tone_b")

add_test(NAME audio_wav_source COMMAND falcon_wavdiff --write-tone "${adir}/source_44k.wav" 1000 44100 1 3)
set_tests_properties(audio_wav_source PROPERTIES FIXTURES_SETUP audio_wav_source)
fx_audio(wav --audio-play "${adir}/source_44k.wav")
set_tests_properties(audio_wav PROPERTIES FIXTURES_REQUIRED audio_wav_source)
fx_audio_check(wav_check wav --skip 0.05 --rms 0.350,0.357 --tone 1000)

fx_audio(right_pan --audio-tone 440:3,0,0 --audio-spatializer pan)
fx_audio_check(right_pan_check right_pan --skip 0.05 --louder right 100)
fx_audio(left_pan --audio-tone 440:-3,0,0 --audio-spatializer pan)
fx_audio_check(left_pan_check left_pan --skip 0.05 --louder left 100)
if(FALCON_STEAM_AUDIO)
  fx_audio(right_hrtf --audio-tone 2000:3,0,0 --audio-spatializer steam)
  fx_audio_check(right_hrtf_check right_hrtf --skip 0.05 --louder right 2)
  fx_audio(left_hrtf --audio-tone 2000:-3,0,0 --audio-spatializer steam)
  fx_audio_check(left_hrtf_check left_hrtf --skip 0.05 --louder left 2)
  # Steam Audio's distance and air: 10 m ahead is a tenth of 1 m (inverse distance); 100 m away a
  # 10 kHz tone keeps about a quarter of its level (0.26), a 500 Hz one most of it (0.94).
  fx_audio(near_1m --audio-tone 1000:0,0,-1 --audio-no-air --audio-spatializer steam)
  fx_audio(far_10m --audio-tone 1000:0,0,-10 --audio-no-air --audio-spatializer steam)
  fx_audio_check(distance_check far_10m --skip 0.1 --ratio-to "${adir}/near_1m.wav" 0.095,0.105)
  set_tests_properties(audio_distance_check PROPERTIES FIXTURES_REQUIRED "audio_far_10m;audio_near_1m")
  fx_audio(high_air --audio-tone 10000:0,0,-100 --audio-spatializer steam)
  fx_audio(high_dry --audio-tone 10000:0,0,-100 --audio-no-air --audio-spatializer steam)
  fx_audio_check(air_high_check high_air --skip 0.1 --ratio-to "${adir}/high_dry.wav" 0.15,0.4)
  set_tests_properties(audio_air_high_check PROPERTIES FIXTURES_REQUIRED "audio_high_air;audio_high_dry")
  fx_audio(low_air --audio-tone 500:0,0,-100 --audio-spatializer steam)
  fx_audio(low_dry --audio-tone 500:0,0,-100 --audio-no-air --audio-spatializer steam)
  fx_audio_check(air_low_check low_air --skip 0.1 --ratio-to "${adir}/low_dry.wav" 0.9,1.0)
  set_tests_properties(audio_air_low_check PROPERTIES FIXTURES_REQUIRED "audio_low_air;audio_low_dry")
  # Reflections and reverb in Sponza (the render tests' cook): a short 1 kHz burst at 0.5 s, then
  # 0.2 - 1.0 s after it: silence without reflections, its reverb with them (in step with the
  # frame clock). Rays are sampled at random, so two runs agree only within a tolerance.
  if(EXISTS "${fx_sponza}")
    add_test(NAME audio_burst_source COMMAND falcon_wavdiff --write-tone "${adir}/burst.wav" 1000 48000 1 0.05 0.5)
    set_tests_properties(audio_burst_source PROPERTIES FIXTURES_SETUP audio_burst_source)
    foreach(case "dry;0" "reverb;2" "reverb_b;2")
      list(GET case 0 name)
      list(GET case 1 level)
      add_test(NAME audio_sponza_${name} COMMAND falcon --hidden --scene "${CMAKE_BINARY_DIR}/test/render/sponza.fpak" --fixed-time
        --frames 120 --audio-play "${adir}/burst.wav:0,0,-3" --audio-reflections ${level} --audio-capture "${adir}/sponza_${name}.wav")
      set_tests_properties(audio_sponza_${name} PROPERTIES FIXTURES_REQUIRED "sponza_pak;audio_burst_source" FIXTURES_SETUP audio_sponza_${name})
    endforeach()
    fx_audio_check(sponza_dry_check sponza_dry --skip 0.75 --until 1.5 --rms 0,0.00001)
    fx_audio_check(sponza_reverb_check sponza_reverb --skip 0.75 --until 1.5 --rms 0.0005,0.05)
    add_test(NAME audio_sponza_reverb_repeat COMMAND falcon_wavdiff "${adir}/sponza_reverb.wav" "${adir}/sponza_reverb_b.wav" --tolerance 0.01)
    set_tests_properties(audio_sponza_reverb_repeat PROPERTIES FIXTURES_REQUIRED "audio_sponza_reverb;audio_sponza_reverb_b")
    # Physics sounds: 64 bodies dropped (synthesized impacts, scrapes and rolls; no reflections, so
    # the same samples every run), twice: identical, and loud enough to be there.
    foreach(run a b)
      add_test(NAME audio_physics_drop_${run} COMMAND falcon --hidden --scene "${CMAKE_BINARY_DIR}/test/render/sponza.fpak" --fixed-time
        --frames 400 --physics-drop 64 --audio-reflections 0 --audio-capture "${adir}/physics_drop_${run}.wav")
      set_tests_properties(audio_physics_drop_${run} PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP audio_physics_drop_${run})
    endforeach()
    fx_audio_check(physics_drop_check physics_drop_a --rms 0.003,0.3)
    add_test(NAME audio_physics_drop_repeat COMMAND falcon_wavdiff "${adir}/physics_drop_a.wav" "${adir}/physics_drop_b.wav" --exact)
    set_tests_properties(audio_physics_drop_repeat PROPERTIES FIXTURES_REQUIRED "audio_physics_drop_a;audio_physics_drop_b")
    # Water sounds: 64 bodies dropped into a pool (synthesized splashes and slosh), twice: splashes
    # played (the stats line), and identical.
    foreach(run a b)
      add_test(NAME audio_water_splash_${run} COMMAND falcon --hidden --scene "${CMAKE_BINARY_DIR}/test/render/sponza.fpak" --fixed-time
        --frames 400 --water-demo tropical:0,1.4,0,26,10 --physics-drop 64 --audio-reflections 0
        --audio-capture "${adir}/water_splash_${run}.wav")
      set_tests_properties(audio_water_splash_${run} PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP audio_water_splash_${run}
        PASS_REGULAR_EXPRESSION "audio: water: [1-9][0-9]* splashes")
    endforeach()
    fx_audio_check(water_splash_check water_splash_a --rms 0.003,0.3)
    add_test(NAME audio_water_splash_repeat COMMAND falcon_wavdiff "${adir}/water_splash_a.wav" "${adir}/water_splash_b.wav" --exact)
    set_tests_properties(audio_water_splash_repeat PROPERTIES FIXTURES_REQUIRED "audio_water_splash_a;audio_water_splash_b")
    # Under water: a 4 kHz tone heard from 0.7 m under the pool's surface keeps under 2% of its
    # level from 1.6 m above it (the world's buses low-passed).
    foreach(case "under;0,0.7,0,5,0.7,0" "above;0,3,0,5,3,0")
      list(GET case 0 name)
      list(GET case 1 pose)
      add_test(NAME audio_water_${name} COMMAND falcon --hidden --scene "${CMAKE_BINARY_DIR}/test/render/sponza.fpak" --fixed-time
        --frames 120 --water-demo tropical:0,1.4,0,26,10 --camera ${pose} --audio-tone 4000 --audio-reflections 0
        --audio-capture "${adir}/water_${name}.wav")
      set_tests_properties(audio_water_${name} PROPERTIES FIXTURES_REQUIRED sponza_pak FIXTURES_SETUP audio_water_${name})
    endforeach()
    fx_audio_check(water_muffle_check water_under --skip 0.5 --band 4000 --ratio-to "${adir}/water_above.wav" 0,0.02)
    set_tests_properties(audio_water_muffle_check PROPERTIES FIXTURES_REQUIRED "audio_water_under;audio_water_above")
  endif()
endif()
