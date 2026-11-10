# Third-party libraries (pinned git submodules under external/).

set(FX_EXT "${CMAKE_SOURCE_DIR}/external")

# Tracy: on-demand so an unconnected client costs nothing at boot. Its lifetime is manual
# (runEngine starts and stops it): falcon.exe is also the cooker and Chromium's helper
# processes, which must not each start a profiler (threads, a listening socket, a crash handler).
set(TRACY_ENABLE ${FALCON_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
set(TRACY_MANUAL_LIFETIME ON CACHE BOOL "" FORCE)  # (and so TRACY_DELAYED_INIT)
add_subdirectory("${FX_EXT}/tracy" "${CMAKE_BINARY_DIR}/_deps/tracy" EXCLUDE_FROM_ALL)

set(ENKITS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ENKITS_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${FX_EXT}/enkiTS" "${CMAKE_BINARY_DIR}/_deps/enkiTS" EXCLUDE_FROM_ALL)

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
add_subdirectory("${FX_EXT}/mimalloc" "${CMAKE_BINARY_DIR}/_deps/mimalloc" EXCLUDE_FROM_ALL)

set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_PIC OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FX_EXT}/flecs" "${CMAKE_BINARY_DIR}/_deps/flecs" EXCLUDE_FROM_ALL)

add_subdirectory("${FX_EXT}/meshoptimizer" "${CMAKE_BINARY_DIR}/_deps/meshoptimizer" EXCLUDE_FROM_ALL)

# SPIRV-Cross: falcon_shaderc turns each shader's SPIR-V into Metal Shading Language, every
# resource at the index SDL_GPU's Metal backend binds it to. Only its C++ core, GLSL and MSL.
# (It asks for CMake 3.10: CMP0091 NEW so it takes the static CRT like everything else.)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${FX_EXT}/SPIRV-Cross" "${CMAKE_BINARY_DIR}/_deps/spirv-cross" EXCLUDE_FROM_ALL)

add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE "${FX_EXT}/cgltf")

# Box3D (alpha, pinned): physics. Built as a static C17 library; asserts follow the config.
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps/fetch" CACHE PATH "" FORCE)
set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)
add_subdirectory("${FX_EXT}/box3d" "${CMAKE_BINARY_DIR}/_deps/box3d" EXCLUDE_FROM_ALL)

# Level tools: Manifold (Apache-2.0) for booleans, MikkTSpace (zlib) for tangents, xatlas (MIT)
# for mask UVs, Recast/Detour (zlib) for navigation. Manifold is built static, serial, without
# its 2D CrossSection (Clipper2) and without downloads. Its Tracy hook is off: it would fetch
# its own Tracy, and ours is already a target (the normal variable shadows the cache entry here).
set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(TRACY_ENABLE OFF)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${FX_EXT}/manifold" "${CMAKE_BINARY_DIR}/_deps/manifold" EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
unset(TRACY_ENABLE)

add_library(mikktspace STATIC "${FX_EXT}/mikktspace/mikktspace.c")
target_include_directories(mikktspace PUBLIC "${FX_EXT}/mikktspace")

add_library(xatlas STATIC "${FX_EXT}/xatlas/source/xatlas/xatlas.cpp")
target_include_directories(xatlas PUBLIC "${FX_EXT}/xatlas/source/xatlas")

# Recast/Detour 1.6.0's own CMake predates CMake 3.5 (rejected now): built from its sources.
foreach(fx_nav Recast Detour DetourCrowd DetourTileCache DebugUtils)
  file(GLOB fx_nav_src "${FX_EXT}/recastnavigation/${fx_nav}/Source/*.cpp")
  add_library(${fx_nav} STATIC ${fx_nav_src})
  target_include_directories(${fx_nav} PUBLIC "${FX_EXT}/recastnavigation/${fx_nav}/Include")
endforeach()
target_link_libraries(DetourCrowd PUBLIC Detour)
target_link_libraries(DetourTileCache PUBLIC Detour)
target_link_libraries(DebugUtils PUBLIC Recast Detour DetourTileCache)
