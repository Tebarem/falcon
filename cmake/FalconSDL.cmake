# Builds SDL 3.4 with cef-lite's external-texture extension and our GPU timestamp queries
# (cmake/patches/sdl3-falcon-timestamps.patch; a local patch, not for upstream SDL), as a static
# library linked into falcon.exe (sdl3-falcon-static.patch: the two extensions' API macros drop
# dllimport / dllexport under SDL_FALCON_STATIC).
#
# The pristine submodule is never modified: it is copied into the build tree once,
# patched there, and re-copied whenever the SDL revision or a patch changes.

set(FX_SDL_SOURCE "${CMAKE_SOURCE_DIR}/external/sdl-src/SDL")
set(FX_SDL_PATCH "${CMAKE_SOURCE_DIR}/external/cef-lite/patches/sdl3-falcon-external-texture.patch")
set(FX_SDL_ROOT "${CMAKE_BINARY_DIR}/_deps/sdl-falcon")
set(FX_SDL_STAMP "${FX_SDL_ROOT}/.falcon-stamp")

execute_process(
  COMMAND git -C "${FX_SDL_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE fx_sdl_rev OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
set(FX_SDL_TIMESTAMPS_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/sdl3-falcon-timestamps.patch")
set(FX_SDL_STATIC_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/sdl3-falcon-static.patch")
set(FX_SDL_NATIVE_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/sdl3-falcon-native.patch")
file(SHA256 "${FX_SDL_PATCH}" fx_sdl_patch_hash)
file(SHA256 "${FX_SDL_TIMESTAMPS_PATCH}" fx_sdl_timestamps_hash)
file(SHA256 "${FX_SDL_STATIC_PATCH}" fx_sdl_static_hash)
file(SHA256 "${FX_SDL_NATIVE_PATCH}" fx_sdl_native_hash)
set(fx_sdl_stamp_expected
  "${fx_sdl_rev}-${fx_sdl_patch_hash}-${fx_sdl_timestamps_hash}-${fx_sdl_static_hash}-${fx_sdl_native_hash}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FX_SDL_PATCH}" "${FX_SDL_TIMESTAMPS_PATCH}"
  "${FX_SDL_STATIC_PATCH}" "${FX_SDL_NATIVE_PATCH}")

set(fx_sdl_stamp_current "")
if(EXISTS "${FX_SDL_STAMP}")
  file(READ "${FX_SDL_STAMP}" fx_sdl_stamp_current)
endif()

if(NOT fx_sdl_stamp_current STREQUAL fx_sdl_stamp_expected)
  message(STATUS "Falcon: preparing patched SDL (${fx_sdl_rev})")
  file(REMOVE_RECURSE "${FX_SDL_ROOT}")
  file(COPY "${FX_SDL_SOURCE}/" DESTINATION "${FX_SDL_ROOT}/SDL" PATTERN ".git" EXCLUDE)

  # The upstream patch has CRLF endings and blank separator lines between hunks,
  # which git apply rejects. Normalize before applying.
  file(READ "${FX_SDL_PATCH}" fx_patch)
  string(REPLACE "\r\n" "\n" fx_patch "${fx_patch}")
  string(REPLACE "\n\n@@" "\n@@" fx_patch "${fx_patch}")
  string(REPLACE "\n\ndiff --git" "\ndiff --git" fx_patch "${fx_patch}")
  file(WRITE "${FX_SDL_ROOT}/falcon.patch" "${fx_patch}")

  # git apply resolves paths against the enclosing repository, so give the copy its own.
  execute_process(COMMAND git init -q WORKING_DIRECTORY "${FX_SDL_ROOT}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND git apply -p1 --whitespace=nowarn falcon.patch
    WORKING_DIRECTORY "${FX_SDL_ROOT}"
    RESULT_VARIABLE fx_apply_result)
  if(NOT fx_apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply the cef-lite SDL patch to ${fx_sdl_rev}")
  endif()
  # Ours go on top (their hunks were made against the cef-lite-patched tree).
  foreach(fx_sdl_local_patch "${FX_SDL_TIMESTAMPS_PATCH}" "${FX_SDL_STATIC_PATCH}" "${FX_SDL_NATIVE_PATCH}")
    execute_process(
      COMMAND git apply -p1 --whitespace=nowarn "${fx_sdl_local_patch}"
      WORKING_DIRECTORY "${FX_SDL_ROOT}"
      RESULT_VARIABLE fx_apply_result)
    if(NOT fx_apply_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${fx_sdl_local_patch} to ${fx_sdl_rev}")
    endif()
  endforeach()
  file(WRITE "${FX_SDL_STAMP}" "${fx_sdl_stamp_expected}")
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_GPU ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
add_subdirectory("${FX_SDL_ROOT}/SDL" "${CMAKE_BINARY_DIR}/_deps/sdl-build" EXCLUDE_FROM_ALL)
# SDL and everything that includes its headers: no DLL boundary for the extensions' API.
target_compile_definitions(SDL3-static PUBLIC SDL_FALCON_STATIC)
