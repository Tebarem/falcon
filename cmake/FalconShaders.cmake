# Slang -> SPIR-V (+ DXIL on Windows), and SPIR-V -> MSL, at build time, embedded into the binary
# as C++ arrays (no shader files to open at boot). Each entry's MSL also lands in <build>/msl/ (CI
# compiles it with Apple's Metal compiler).
#
# DXIL needs DXC (dxcompiler + dxil, from the Windows SDK): Windows builds require it; Linux and
# macOS builds have no D3D12 to run it and leave it out.

if(WIN32)
  file(GLOB fx_sdk_bins LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
  list(SORT fx_sdk_bins COMPARE NATURAL ORDER DESCENDING)
  set(fx_dxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
  foreach(dir IN LISTS fx_sdk_bins)
    list(APPEND fx_dxc_hints "${dir}/x64")
  endforeach()
  find_path(FALCON_DXC_DIR dxcompiler.dll HINTS ${fx_dxc_hints} NO_DEFAULT_PATH)
  if(NOT FALCON_DXC_DIR OR NOT EXISTS "${FALCON_DXC_DIR}/dxil.dll")
    message(FATAL_ERROR "dxcompiler.dll + dxil.dll not found (install the Windows SDK or set FALCON_DXC_DIR)")
  endif()
  message(STATUS "Falcon: DXC from ${FALCON_DXC_DIR}")
  set(fx_shaderc_dxc --dxc "${FALCON_DXC_DIR}")
else()
  set(FALCON_DXC_DIR "")
  set(fx_shaderc_dxc --no-dxil 1)
endif()

# falcon_add_shaders(<target> ROOT <dir> SOURCES <files relative to ROOT>...)
# Creates a static library <target> exposing "<stem>.shader.h" for each source.
function(falcon_add_shaders target)
  cmake_parse_arguments(arg "" "ROOT" "SOURCES" ${ARGN})
  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/gen/shaders")
  set(generated)
  foreach(src IN LISTS arg_SOURCES)
    get_filename_component(stem "${src}" NAME_WE)
    set(cpp "${out_dir}/${stem}.shader.cpp")
    set(hdr "${out_dir}/${stem}.shader.h")
    add_custom_command(
      OUTPUT "${cpp}" "${hdr}"
      COMMAND falcon_shaderc
        --root "${arg_ROOT}"
        --source "${src}"
        --out-dir "${out_dir}"
        ${fx_shaderc_dxc}
        --depfile "${out_dir}/${stem}.d"
        --msl-dir "${CMAKE_BINARY_DIR}/msl"
      DEPENDS falcon_shaderc "${arg_ROOT}/${src}"
      DEPFILE "${out_dir}/${stem}.d"
      COMMENT "slang ${src}"
      VERBATIM)
    list(APPEND generated "${cpp}")
  endforeach()
  add_library(${target} STATIC ${generated})
  target_include_directories(${target} PUBLIC "${out_dir}" "${CMAKE_SOURCE_DIR}/engine")
endfunction()
