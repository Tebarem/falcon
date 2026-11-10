# NVIDIA DLSS (Super Resolution / DLAA, Direct3D 12): on when scripts/fetch-dlss.ps1 has fetched
# NVIDIA's DLSS SDK pieces into external/dlss. NVIDIA's code goes only into a plugin,
# bin/dlss/falcon_dlss.dll (engine/render/dlss_plugin/, linked with the static NGX library), next
# to nvngx_dlss.dll and NVIDIA's LICENSE.txt. falcon.exe has no NVIDIA code: it loads the plugin
# at runtime when it is there (engine/render/dlss_upscaler.cpp), and upscales with TAAU when not.

set(FX_DLSS_ROOT "${CMAKE_SOURCE_DIR}/external/dlss")
set(fx_dlss_found OFF)
if(EXISTS "${FX_DLSS_ROOT}/include/nvsdk_ngx_helpers.h" AND
   EXISTS "${FX_DLSS_ROOT}/lib/nvsdk_ngx_s.lib" AND
   EXISTS "${FX_DLSS_ROOT}/bin/nvngx_dlss.dll")
  set(fx_dlss_found ON)
endif()
option(FALCON_DLSS "NVIDIA DLSS plugin on Direct3D 12 (needs scripts/fetch-dlss.ps1)" ${fx_dlss_found})
if(FALCON_DLSS AND NOT fx_dlss_found)
  message(WARNING "FALCON_DLSS: external/dlss is incomplete (run scripts/fetch-dlss.ps1); DLSS is off")
  set(FALCON_DLSS OFF CACHE BOOL "" FORCE)
endif()
message(STATUS "Falcon: NVIDIA DLSS plugin ${FALCON_DLSS}")

if(FALCON_DLSS)
  set(FX_DLSS_STAGE_DIR "${CMAKE_BINARY_DIR}/bin/dlss")
  add_library(falcon_dlss SHARED "${CMAKE_SOURCE_DIR}/engine/render/dlss_plugin/falcon_dlss.cpp")
  target_include_directories(falcon_dlss PRIVATE "${CMAKE_SOURCE_DIR}/engine/render")
  target_include_directories(falcon_dlss SYSTEM PRIVATE "${FX_DLSS_ROOT}/include")
  # The static NGX library is built against the static CRT, as everything here is.
  target_link_libraries(falcon_dlss PRIVATE
    "$<IF:$<CONFIG:Debug>,${FX_DLSS_ROOT}/lib/nvsdk_ngx_s_dbg.lib,${FX_DLSS_ROOT}/lib/nvsdk_ngx_s.lib>"
    d3d12 dxgi)
  set_target_properties(falcon_dlss PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FX_DLSS_STAGE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FX_DLSS_STAGE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FX_DLSS_STAGE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FX_DLSS_STAGE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${FX_DLSS_STAGE_DIR}")
  add_custom_target(falcon_dlss_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FX_DLSS_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FX_DLSS_ROOT}/bin/nvngx_dlss.dll"
      "${FX_DLSS_ROOT}/LICENSE.txt"
      "${FX_DLSS_STAGE_DIR}"
    VERBATIM)
  add_dependencies(falcon_dlss falcon_dlss_runtime)
endif()
