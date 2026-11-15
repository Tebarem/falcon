# Managed scripting: Falcon.Engine (C# API) for the editor's project builds, and the tests' games:
# the Sandbox sample game, the API's speed test (Bench) and Arena (the networked sample), each in
# both script modes:
#   bin/scripts/jit/Falcon.Engine.dll  the engine's API, its source generator and build targets
#   sample_scripts/jit/Sandbox.dll     managed assembly, hot-reloadable (CoreCLR)
#   sample_scripts/aot/Sandbox.dll     NativeAOT: game + Falcon.Engine compiled to native code
#                                      (Sandbox.so on Linux, Sandbox.dylib on macOS)
#   sample_scripts/bench/{jit,aot}/Bench.dll, sample_scripts/arena/{jit,aot}/Arena.dll
# The games are outside bin/ (what ships): only the tests run them.
find_program(FALCON_DOTNET dotnet REQUIRED)

# NativeAOT for this OS (FALCON_DOTNET_RID: engine/CMakeLists.txt). On Windows it links with the
# MSVC tools of the build environment (build.ps1 runs inside vcvars); elsewhere with clang, or gcc
# when there is no clang.
if(WIN32)
  set(fx_aot_ext .dll)
  set(fx_aot_tools -p:IlcUseEnvironmentalTools=true)
else()
  if(APPLE)
    set(fx_aot_ext .dylib)
  else()
    set(fx_aot_ext .so)
  endif()
  find_program(fx_clang clang)
  if(fx_clang)
    set(fx_aot_tools)
  else()
    set(fx_aot_tools -p:CppCompilerAndLinker=gcc)
  endif()
endif()
set(FALCON_AOT_EXT "${fx_aot_ext}" CACHE INTERNAL "NativeAOT script libraries' extension")
set(fx_aot_publish -c Release -r ${FALCON_DOTNET_RID} -p:PublishAot=true ${fx_aot_tools})

file(GLOB_RECURSE fx_managed_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cs" "${CMAKE_CURRENT_SOURCE_DIR}/*.csproj"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.props" "${CMAKE_CURRENT_SOURCE_DIR}/*.targets")
list(FILTER fx_managed_sources EXCLUDE REGEX "/(bin|obj)/")

set(fx_scripts "${CMAKE_BINARY_DIR}/bin/scripts")
set(fx_samples "${CMAKE_BINARY_DIR}/sample_scripts")
set(fx_game "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/Sandbox.csproj")
# The script components' source generator, next to Falcon.Engine.dll for the editor's project builds
# (Falcon.Game.targets imports it); the Sandbox and Bench reference its project.
set(fx_generator "${CMAKE_CURRENT_SOURCE_DIR}/Falcon.Engine.Generators/Falcon.Engine.Generators.csproj")
add_custom_command(
  OUTPUT "${fx_scripts}/jit/Falcon.Engine.Generators.dll" "${fx_scripts}/jit/Falcon.Game.targets" "${fx_scripts}/jit/AotEntry.cs"
  COMMAND "${FALCON_DOTNET}" build "${fx_generator}" -c Release -o "${fx_scripts}/generator" --nologo -v:q
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${fx_scripts}/jit"
  COMMAND "${CMAKE_COMMAND}" -E copy "${fx_scripts}/generator/Falcon.Engine.Generators.dll" "${fx_scripts}/jit/"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Falcon.Game.targets" "${fx_scripts}/jit/"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Falcon.Engine/GameEntry/AotEntry.cs" "${fx_scripts}/jit/"
  DEPENDS ${fx_managed_sources}
  COMMENT "dotnet build Falcon.Engine.Generators"
  VERBATIM)
# Falcon.Engine.dll for the editor's project builds (game_scripts.h). The sample games used to be
# built into bin/scripts/: their files there are removed.
set(fx_engine "${CMAKE_CURRENT_SOURCE_DIR}/Falcon.Engine/Falcon.Engine.csproj")
add_custom_command(
  OUTPUT "${fx_scripts}/jit/Falcon.Engine.dll"
  COMMAND "${FALCON_DOTNET}" build "${fx_engine}" -c Release -o "${fx_scripts}/jit" --nologo -v:q
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${fx_scripts}/aot" "${fx_scripts}/bench" "${fx_scripts}/arena"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${fx_scripts}/jit/Sandbox.dll" "${fx_scripts}/jit/Sandbox.pdb"
    "${fx_scripts}/jit/Sandbox.deps.json" "${fx_scripts}/jit/Sandbox.runtimeconfig.json"
  DEPENDS ${fx_managed_sources} falcon_schema "${fx_scripts}/jit/Falcon.Engine.Generators.dll"
  COMMENT "dotnet build Falcon.Engine"
  VERBATIM)
add_custom_command(
  OUTPUT "${fx_samples}/jit/Sandbox.dll"
  COMMAND "${FALCON_DOTNET}" build "${fx_game}" -c Release -o "${fx_samples}/jit" --nologo -v:q
  DEPENDS ${fx_managed_sources} falcon_schema "${fx_scripts}/jit/Falcon.Engine.dll"
  COMMENT "dotnet build Sandbox (JIT)"
  VERBATIM)
# Depends on the JIT build so the two never write the shared obj/ directories at once.
add_custom_command(
  OUTPUT "${fx_samples}/aot/Sandbox${fx_aot_ext}"
  COMMAND "${FALCON_DOTNET}" publish "${fx_game}" ${fx_aot_publish} -o "${fx_samples}/aot" --nologo -v:q
  DEPENDS ${fx_managed_sources} "${fx_samples}/jit/Sandbox.dll"
  COMMENT "dotnet publish Sandbox (NativeAOT)"
  VERBATIM)
# The bench builds after the Sandbox: they share Falcon.Engine's obj/ directory.
set(fx_bench "${CMAKE_CURRENT_SOURCE_DIR}/Bench/Bench.csproj")
add_custom_command(
  OUTPUT "${fx_samples}/bench/jit/Bench.dll"
  COMMAND "${FALCON_DOTNET}" build "${fx_bench}" -c Release -o "${fx_samples}/bench/jit" --nologo -v:q
  DEPENDS ${fx_managed_sources} "${fx_samples}/aot/Sandbox${fx_aot_ext}"
  COMMENT "dotnet build Bench (JIT)"
  VERBATIM)
add_custom_command(
  OUTPUT "${fx_samples}/bench/aot/Bench${fx_aot_ext}"
  COMMAND "${FALCON_DOTNET}" publish "${fx_bench}" ${fx_aot_publish} -o "${fx_samples}/bench/aot" --nologo -v:q
  DEPENDS ${fx_managed_sources} "${fx_samples}/bench/jit/Bench.dll"
  COMMENT "dotnet publish Bench (NativeAOT)"
  VERBATIM)
# Arena, the networked sample game (deterministic simulation and rollback tests), after the bench.
set(fx_arena "${CMAKE_CURRENT_SOURCE_DIR}/Arena/Arena.csproj")
add_custom_command(
  OUTPUT "${fx_samples}/arena/jit/Arena.dll"
  COMMAND "${FALCON_DOTNET}" build "${fx_arena}" -c Release -o "${fx_samples}/arena/jit" --nologo -v:q
  DEPENDS ${fx_managed_sources} "${fx_samples}/bench/aot/Bench${fx_aot_ext}"
  COMMENT "dotnet build Arena (JIT)"
  VERBATIM)
add_custom_command(
  OUTPUT "${fx_samples}/arena/aot/Arena${fx_aot_ext}"
  COMMAND "${FALCON_DOTNET}" publish "${fx_arena}" ${fx_aot_publish} -o "${fx_samples}/arena/aot" --nologo -v:q
  DEPENDS ${fx_managed_sources} "${fx_samples}/arena/jit/Arena.dll"
  COMMENT "dotnet publish Arena (NativeAOT)"
  VERBATIM)
add_custom_target(falcon_scripts ALL DEPENDS "${fx_scripts}/jit/Falcon.Engine.dll" "${fx_samples}/jit/Sandbox.dll" "${fx_samples}/aot/Sandbox${fx_aot_ext}"
  "${fx_samples}/bench/jit/Bench.dll" "${fx_samples}/bench/aot/Bench${fx_aot_ext}" "${fx_samples}/arena/jit/Arena.dll"
  "${fx_samples}/arena/aot/Arena${fx_aot_ext}")
