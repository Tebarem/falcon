# Steam (Valve's Steamworks SDK 1.65): on when scripts/fetch-steamworks.ps1 (Windows) or
# `cmake -DDEP=steamworks -DZIP=... -P scripts/fetch.cmake` (Linux, macOS) has unpacked the SDK's
# headers and this OS's steam_api64.dll / libsteam_api.so / libsteam_api.dylib into
# external/steamworks. The engine loads it at run time from beside falcon (engine/steam/steam_client.cpp)
# and needs nothing at build time, so a build or a machine without the SDK still runs, without Steam.
# The C# bindings (managed/Falcon.Engine/Steam/Generated) are committed: tools/steamgen makes them.

set(FX_STEAMWORKS_ROOT "${CMAKE_SOURCE_DIR}/external/steamworks")
set(fx_steamworks_found OFF)
if(WIN32)
  set(fx_steam_api "${FX_STEAMWORKS_ROOT}/lib/steam_api64.dll")
elseif(APPLE)
  set(fx_steam_api "${FX_STEAMWORKS_ROOT}/lib/libsteam_api.dylib")
else()
  set(fx_steam_api "${FX_STEAMWORKS_ROOT}/lib/libsteam_api.so")
endif()
if(EXISTS "${FX_STEAMWORKS_ROOT}/include/steam/steam_api_flat.h" AND EXISTS "${fx_steam_api}")
  set(fx_steamworks_found ON)
endif()
option(FALCON_STEAMWORKS "Steam through the Steamworks SDK (needs scripts/fetch-steamworks.ps1)" ${fx_steamworks_found})
if(FALCON_STEAMWORKS AND NOT fx_steamworks_found)
  message(WARNING "FALCON_STEAMWORKS: external/steamworks is incomplete (run scripts/fetch-steamworks.ps1); Steam is off")
  set(FALCON_STEAMWORKS OFF CACHE BOOL "" FORCE)
endif()
message(STATUS "Falcon: Steamworks ${FALCON_STEAMWORKS}")

if(FALCON_STEAMWORKS)
  # steamcmd too, for the editor's Upload to Steam (in bin/steamcmd: Build Game leaves it out).
  set(fx_steamcmd_copy "")
  if(EXISTS "${FX_STEAMWORKS_ROOT}/steamcmd/steamcmd.exe")
    set(fx_steamcmd_copy
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/steamcmd"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FX_STEAMWORKS_ROOT}/steamcmd/steamcmd.exe" "${CMAKE_BINARY_DIR}/bin/steamcmd")
  endif()
  add_custom_target(falcon_steamworks_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fx_steam_api}" "${CMAKE_BINARY_DIR}/bin"
    ${fx_steamcmd_copy}
    VERBATIM)
endif()
