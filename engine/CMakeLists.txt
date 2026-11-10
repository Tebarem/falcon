falcon_add_shaders(falcon_shaders
  ROOT "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  SOURCES
    bringup_triangle.slang
    naive_scene.slang
    cluster_cull.slang
    cluster_expand.slang
    visbuffer.slang
    msaa_edges.slang
    msaa_edges_masked.slang
    hiz.slang
    visbuffer_resolve.slang
    reflections.slang
    reflection_source.slang
    reflection_far.slang
    hiz_debug.slang
    vt_feedback.slang
    light_binning.slang
    vsm_pages.slang
    vsm_mark.slang
    vsm_raster.slang
    vsm_copy.slang
    ao.slang
    ao_blur.slang
    taa.slang
    exposure.slang
    bloom.slang
    tonemap.slang
    exposure_texture.slang
    fxaa.slang
    skin_vertices.slang
    editor_pick.slang
    editor_scene_view.slang
    ray_traced_view.slang
    probe_trace.slang
    probe_blend.slang
    probe_relocate.slang
    probe_view.slang
    terrain_surface.slang
    terrain_scatter.slang
    terrain_generate.slang
    water_surface.slang
    water_composite.slang
    water_fft.slang
    water_mips.slang
    water_ripples.slang
    water_sspr.slang
    animator_preview.slang
    particles.slang
    runtime_meshes.slang
    transparent.slang
    transparent_cull.slang
    transparent_pick.slang
    vertex_deform.slang)

# Shared ECS components: C++ header (build tree) + C# structs (managed source tree, committed so
# the managed projects also build on their own).
set(fx_schema "${CMAKE_CURRENT_SOURCE_DIR}/scene/components.fschema")
set(fx_schema_h "${CMAKE_CURRENT_BINARY_DIR}/gen/components.gen.h")
set(fx_schema_cs "${CMAKE_SOURCE_DIR}/managed/Falcon.Engine/Generated/Components.g.cs")
add_custom_command(
  OUTPUT "${fx_schema_h}" "${fx_schema_cs}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/gen" "${CMAKE_SOURCE_DIR}/managed/Falcon.Engine/Generated"
  COMMAND falcon_schemagen "${fx_schema}" "${fx_schema_h}" "${fx_schema_cs}"
  DEPENDS falcon_schemagen "${fx_schema}"
  COMMENT "schemagen components.fschema"
  VERBATIM)
add_custom_target(falcon_schema DEPENDS "${fx_schema_h}" "${fx_schema_cs}")

# What the engine asks of the operating system (core/platform.h): a library of its own, which the
# transports use too.
if(WIN32)
  add_library(falcon_platform STATIC core/platform_win32.cpp)
  target_link_libraries(falcon_platform PRIVATE psapi shell32 ole32)
else()
  add_library(falcon_platform STATIC core/platform_posix.cpp)
  target_link_libraries(falcon_platform PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()
target_include_directories(falcon_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
falcon_compile_options(falcon_platform STRICT)

# Networking's transports (QUIC, loopback, the conditioner): a library of their own, which the
# engine and the transport tests (tools/nettest) link.
add_library(falcon_net STATIC
  net/loopback_transport.cpp
  net/net_conditioner.cpp
  net/net_transport.cpp
  net/quic_transport.cpp
)
target_include_directories(falcon_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(falcon_net PUBLIC SDL3::SDL3 falcon_platform)
falcon_compile_options(falcon_net STRICT)
if(FALCON_QUIC)
  target_include_directories(falcon_net SYSTEM PRIVATE "${FX_MSQUIC_ROOT}/include")
  target_compile_definitions(falcon_net PRIVATE FALCON_QUIC=1)
  if(WIN32)
    target_link_libraries(falcon_net PRIVATE bcrypt ws2_32 ntdll)  # (ntdll: msquic's QuicAddrFromString)
  endif()
  add_dependencies(falcon_net falcon_msquic_runtime)
endif()

# Rollback's physics snapshots: a fork of Box3D's world snapshot, built into Box3D's library (its
# compile flags decide its structs' layouts) without changing the submodule.
target_sources(box3d PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/physics/rollback/physics_snapshot.c")

add_library(falcon_engine STATIC
  anim/anim_core.cpp
  anim/anim_ik.cpp
  anim/anim_events.cpp
  anim/anim_graph.cpp
  anim/anim_graph_compiler.cpp
  anim/anim_sync.cpp
  anim/anim_tasks.cpp
  anim/animation_system.cpp
  anim/bone_entities.cpp
  anim/graph_library.cpp
  app/engine.cpp
  app/cook_window.cpp
  app/preview.cpp
  assets/scene_pak.cpp
  assets/material_asset.cpp
  assets/water_asset.cpp
  assets/sound_asset.cpp
  assets/acoustic_material.cpp
  assets/surface_sound.cpp
  audio/audio_device.cpp
  audio/audio_mixer.cpp
  audio/audio_bake.cpp
  audio/audio_scene.cpp
  audio/physics_audio.cpp
  audio/water_audio.cpp
  audio/procedural_synth.cpp
  audio/audio_system.cpp
  audio/clip_cache.cpp
  audio/steam_audio.cpp
  core/boot.cpp
  core/dmath.cpp
  core/json.cpp
  core/jobs.cpp
  core/mesh_tangents.cpp
  core/profiler.cpp
  editor/editor_bridge.cpp
  editor/editor_prefs.cpp
  editor/scene_edits.cpp
  editor/profiler_stream.cpp
  editor/scene_view.cpp
  editor/level_arrange.cpp
  editor/level_navigation.cpp
  editor/level_paint.cpp
  editor/level_scatter.cpp
  editor/level_sockets.cpp
  editor/level_splines.cpp
  editor/scene_elements.cpp
  editor/launcher.cpp
  editor/anim_editor.cpp
  editor/anim_preview.cpp
  editor/asset_browser.cpp
  editor/material_library.cpp
  editor/prefab.cpp
  editor/prefab_spawn.cpp
  editor/terrain_editor.cpp
  gpu/capture.cpp
  gpu/device.cpp
  gpu/gpu_memory.cpp
  gpu/gpu_profiler.cpp
  render/bringup_renderer.cpp
  render/gpu_driven_renderer.cpp
  render/light_system.cpp
  render/skinning.cpp
  render/post_process.cpp
  render/mesh_renderer.cpp
  render/fsr_upscaler.cpp
  render/dlss_upscaler.cpp
  render/virtual_shadow_map.cpp
  render/virtual_texture.cpp
  render/shader_file.cpp
  render/probe_gi.cpp
  render/probe_lights.cpp
  render/ray_tracing/bvh_builder.cpp
  render/ray_tracing/scene_bvh.cpp
  render/preview_renderer.cpp
  render/particle_renderer.cpp
  render/transparent_renderer.cpp
  render/decal_renderer.cpp
  render/scene_renderer.cpp
  render/scatter_renderer.cpp
  render/terrain_generator.cpp
  render/live_geometry.cpp
  render/surface_layers.cpp
  level/poly_mesh.cpp
  level/level_mask.cpp
  level/mask_brush.cpp
  level/level_meshes.cpp
  level/mesh_cut.cpp
  level/mesh_ops.cpp
  level/mesh_shapes.cpp
  level/pak_mesh.cpp
  level/starter_trims.cpp
  level/trim_sheet.cpp
  level/smart_material.cpp
  level/splines.cpp
  level/starter_smart.cpp
  level/modifiers.cpp
  level/navmesh.cpp
  level/rubble.cpp
  level/walls.cpp
  level/roofs.cpp
  level/cut_bake.cpp
  level/prototype_materials.cpp
  level/wear.cpp
  render/terrain_renderer.cpp
  render/terrain_surface.cpp
  render/water_ocean.cpp
  render/water_renderer.cpp
  terrain/terrain.cpp
  terrain/terrain_layers.cpp
  terrain/terrain_scatter.cpp
  terrain/terrain_generate.cpp
  water/ocean_spectrum.cpp
  water/water_system.cpp
  scene/fly_camera.cpp
  scene/scene.cpp
  scene/node_animator.cpp
  physics/physics_world.cpp
  project/asset_library.cpp
  project/libraries.cpp
  project/project.cpp
  project/game_scripts.cpp
  project/project_shaders.cpp
  script/script_entry.cpp
  script/script_host.cpp
  script/script_level.cpp
  sim/sim_recorder.cpp
  steam/steam_client.cpp
  steam/steam_transport.cpp
  ui/ui_layer.cpp
  vfx/particle_value.cpp
  vfx/vfx_components.cpp
  vfx/particle_system.cpp
  vfx/decal_system.cpp
  vfx/runtime_meshes.cpp
  vfx/vfx_selftest.cpp)
target_include_directories(falcon_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_dependencies(falcon_engine falcon_schema)
# What belongs to one OS: processes, folder watching, the performance numbers, freeze reports.
if(WIN32)
  target_sources(falcon_engine PRIVATE app/process_win32.cpp core/dir_watch_win32.cpp
    core/system_stats_win32.cpp core/freeze_watch_win32.cpp)
else()
  target_sources(falcon_engine PRIVATE app/process_posix.cpp core/dir_watch_posix.cpp
    core/system_stats_posix.cpp core/freeze_watch_posix.cpp)
endif()

# .NET hosting (nethost + hostfxr headers) from the installed SDK's apphost pack for this OS.
if(WIN32)
  set(fx_dotnet_rid win-x64)
  set(fx_dotnet_roots "$ENV{ProgramFiles}/dotnet")
  set(fx_nethost_lib libnethost.lib)
else()
  if(APPLE)
    set(fx_dotnet_rid osx-arm64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(fx_dotnet_rid linux-arm64)
  else()
    set(fx_dotnet_rid linux-x64)
  endif()
  set(fx_dotnet_roots "$ENV{DOTNET_ROOT}")
  find_program(fx_dotnet_exe dotnet)
  if(fx_dotnet_exe)
    file(REAL_PATH "${fx_dotnet_exe}" fx_dotnet_real)
    get_filename_component(fx_dotnet_dir "${fx_dotnet_real}" DIRECTORY)
    list(APPEND fx_dotnet_roots "${fx_dotnet_dir}")
  endif()
  list(APPEND fx_dotnet_roots "$ENV{HOME}/.dotnet" /usr/share/dotnet /usr/lib/dotnet /usr/local/share/dotnet /usr/local/lib/dotnet)
  set(fx_nethost_lib libnethost.a)
endif()
set(FALCON_DOTNET_RID "${fx_dotnet_rid}" CACHE INTERNAL "")
set(fx_nethost_dirs)
foreach(root IN LISTS fx_dotnet_roots)
  if(root)
    file(GLOB dirs LIST_DIRECTORIES true
      "${root}/packs/Microsoft.NETCore.App.Host.${fx_dotnet_rid}/*/runtimes/${fx_dotnet_rid}/native")
    list(APPEND fx_nethost_dirs ${dirs})
  endif()
endforeach()
list(SORT fx_nethost_dirs COMPARE NATURAL ORDER DESCENDING)
if(NOT fx_nethost_dirs)
  message(FATAL_ERROR "nethost not found for ${fx_dotnet_rid}: install the .NET 10 SDK (or set DOTNET_ROOT)")
endif()
list(GET fx_nethost_dirs 0 FALCON_NETHOST_DIR)
if(NOT EXISTS "${FALCON_NETHOST_DIR}/nethost.h")
  message(FATAL_ERROR "nethost not found: install the .NET 10 SDK")
endif()
message(STATUS "nethost: ${FALCON_NETHOST_DIR}")
target_include_directories(falcon_engine PRIVATE "${FALCON_NETHOST_DIR}")
target_link_libraries(falcon_engine PRIVATE "${FALCON_NETHOST_DIR}/${fx_nethost_lib}")
target_include_directories(falcon_engine PRIVATE "${FX_SDL_ROOT}/SDL/src/video/khronos")
target_link_libraries(falcon_engine PUBLIC SDL3::SDL3 Tracy::TracyClient flecs::flecs_static falcon_shaders box3d enkiTS falcon_net)
# meshoptimizer: the terrain's tile codec.
target_link_libraries(falcon_engine PRIVATE meshoptimizer)
if(WIN32)
  target_link_libraries(falcon_engine PRIVATE dbghelp)  # (freeze reports)
endif()
# MikkTSpace: tangents for cooked glTF primitives without them, and for level meshes.
target_link_libraries(falcon_engine PRIVATE mikktspace)
# Manifold: the level kernel's triangulation (and cuts).
target_link_libraries(falcon_engine PRIVATE manifold xatlas Recast Detour DetourCrowd)
# The scripts' performance numbers (core/system_stats): Windows' GPU counters, the adapters' memory.
if(WIN32)
  target_link_libraries(falcon_engine PRIVATE pdh dxgi)
endif()
target_include_directories(falcon_engine PRIVATE "${CMAKE_SOURCE_DIR}/external/meshoptimizer/src")
# stb_image (a private, static copy): terrain brush images. Terrain layer textures use the
# cooker's stb_image, stb_image_resize2, bc7enc and rgbcx (falcon_cook_lib, in the same exe).
target_include_directories(falcon_engine SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/stb"
  "${CMAKE_SOURCE_DIR}/external/stb_image_resize" "${CMAKE_SOURCE_DIR}/external/bc7enc")
if(WIN32)
  target_compile_definitions(falcon_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
# HTML UI (cef-lite, GPL-3.0): the engine links the static library and depends on the CEF runtime
# (Chromium's binaries in bin/cef/, cef-lite's shaders) being staged next to falcon.exe.
if(FALCON_UI)
  target_link_libraries(falcon_engine PRIVATE cef_lite)
  target_compile_definitions(falcon_engine PRIVATE FALCON_UI=1)
  add_dependencies(falcon_engine cef_lite_runtime falcon_ui_bundles)
  # libcef.dll (~250 MB) loads when the UI is first created, not at process start, from bin/cef/
  # (the delay-load hook in runtime/cef_runtime.cpp).
  if(WIN32)
    target_link_options(falcon_engine INTERFACE "/DELAYLOAD:libcef.dll")
    target_link_libraries(falcon_engine INTERFACE delayimp)
  endif()
endif()
# The engine's own content (content/: the prototype textures blockouts default to, and the
# material library every project's Project panel shows as Falcon) next to falcon.exe as
# bin/content/; the editor puts it into projects (level/prototype_materials.cpp, project/asset_library.cpp).
file(GLOB_RECURSE fx_content_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/content/*")
set(fx_content_stamp "${CMAKE_BINARY_DIR}/_deps/falcon-content.stamp")
add_custom_command(OUTPUT "${fx_content_stamp}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/content" "${CMAKE_BINARY_DIR}/bin/content"
  COMMAND ${CMAKE_COMMAND} -E touch "${fx_content_stamp}"
  DEPENDS ${fx_content_files}
  COMMENT "Copying engine content"
  VERBATIM)
add_custom_target(falcon_content DEPENDS "${fx_content_stamp}")
add_dependencies(falcon_engine falcon_content)
# AMD FSR (cmake/FalconFsr.cmake): the FSR API headers; the DLLs load at runtime from bin/fsr/.
if(FALCON_FSR)
  target_include_directories(falcon_engine SYSTEM PRIVATE "${FX_FSR_ROOT}/include/api/include"
    "${FX_FSR_ROOT}/include/upscalers/include")
  target_compile_definitions(falcon_engine PRIVATE FALCON_FSR=1)
  add_dependencies(falcon_engine falcon_fsr_runtime)
endif()
# NVIDIA DLSS (cmake/FalconDlss.cmake): nothing of NVIDIA's links here; falcon.exe loads the
# plugin bin/dlss/falcon_dlss.dll at runtime (render/dlss_upscaler.cpp). Built along with it.
if(FALCON_DLSS)
  add_dependencies(falcon_engine falcon_dlss)
endif()
# Steam Audio (cmake/FalconSteamAudio.cmake): phonon.dll (~50 MB) is delay-loaded, from bin/audio/
# by full path when audio starts (engine/audio/steam_audio.cpp); libphonon.so / .dylib is linked and
# found in bin/audio/ through the rpath.
if(FALCON_STEAM_AUDIO)
  target_include_directories(falcon_engine SYSTEM PRIVATE "${FX_STEAM_AUDIO_ROOT}/include")
  target_compile_definitions(falcon_engine PRIVATE FALCON_STEAM_AUDIO=1)
  if(WIN32)
    target_link_libraries(falcon_engine PUBLIC "${FX_STEAM_AUDIO_ROOT}/lib/phonon.lib")
    target_link_options(falcon_engine INTERFACE "/DELAYLOAD:phonon.dll")
    target_link_libraries(falcon_engine INTERFACE delayimp)
  else()
    target_link_libraries(falcon_engine PUBLIC "${FX_PHONON_LIB}")
  endif()
  add_dependencies(falcon_engine falcon_steamaudio_runtime)
endif()
if(FALCON_STEAMWORKS)
  target_include_directories(falcon_engine SYSTEM PRIVATE "${FX_STEAMWORKS_ROOT}/include")
  target_compile_definitions(falcon_engine PRIVATE FALCON_STEAMWORKS=1)
  add_dependencies(falcon_engine falcon_steamworks_runtime)
endif()
falcon_compile_options(falcon_engine STRICT)
if(MSVC)
  target_compile_options(falcon_engine PRIVATE /Zc:__cplusplus /Zc:preprocessor /external:anglebrackets /external:W0)
endif()
