find_package(slang CONFIG REQUIRED PATHS "${FALCON_SLANG_ROOT}/cmake" "${FALCON_SLANG_ROOT}/lib/cmake/slang" NO_DEFAULT_PATH)

add_executable(falcon_shaderc main.cpp)
target_link_libraries(falcon_shaderc PRIVATE slang::slang spirv-cross-msl)
set_target_properties(falcon_shaderc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
falcon_compile_options(falcon_shaderc)
if(NOT WIN32)
  # Slang's libraries sit beside the executable, as on Windows.
  set_target_properties(falcon_shaderc PROPERTIES BUILD_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>"
    INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>")
endif()

# Slang loads its compiler/glslang modules from next to the executable (bin/*.dll on Windows,
# lib/*.so or lib/*.dylib elsewhere).
if(WIN32)
  file(GLOB fx_slang_dlls "${FALCON_SLANG_ROOT}/bin/*.dll")
elseif(APPLE)
  file(GLOB fx_slang_dlls "${FALCON_SLANG_ROOT}/lib/*.dylib")
else()
  file(GLOB fx_slang_dlls "${FALCON_SLANG_ROOT}/lib/*.so*")
endif()
foreach(dll IN LISTS fx_slang_dlls)
  add_custom_command(TARGET falcon_shaderc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:falcon_shaderc>"
    VERBATIM)
endforeach()

# Shipped with the editor in bin/shaderc/ (R2: a project's Shaders/ compiled while it runs), with
# DXC for DXIL; not Slang's LLVM back end (CPU targets only, 100 MB). Build Game leaves it out.
set(fx_shaderc_bin "${CMAKE_BINARY_DIR}/bin/shaderc")
# What projects' shaders import (--include bin/shaderc/include): falcon_post (R3).
configure_file("${CMAKE_SOURCE_DIR}/engine/shaders/include/falcon_post.slang" "${fx_shaderc_bin}/include/falcon_post.slang" COPYONLY)
# The engine's shader sources (bin/shaderc/engine), for a project's own build of the resolve with
# its surface shaders (R4). Its project_surfaces (none) goes to include/, after the project's.
file(GLOB fx_engine_slang CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/engine/shaders/*.slang")
foreach(src IN LISTS fx_engine_slang)
  get_filename_component(name "${src}" NAME)
  if(name STREQUAL "project_surfaces.slang" OR name STREQUAL "falcon_surface.slang")
    configure_file("${src}" "${fx_shaderc_bin}/include/${name}" COPYONLY)
  else()
    configure_file("${src}" "${fx_shaderc_bin}/engine/${name}" COPYONLY)
  endif()
endforeach()
add_custom_command(TARGET falcon_shaderc POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${fx_shaderc_bin}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:falcon_shaderc>" "${fx_shaderc_bin}"
  VERBATIM)
if(WIN32)
  add_custom_command(TARGET falcon_shaderc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FALCON_DXC_DIR}/dxcompiler.dll" "${FALCON_DXC_DIR}/dxil.dll" "${fx_shaderc_bin}"
    VERBATIM)
endif()
foreach(dll IN LISTS fx_slang_dlls)
  if(NOT dll MATCHES "slang-llvm")
    add_custom_command(TARGET falcon_shaderc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${fx_shaderc_bin}"
      VERBATIM)
  endif()
endforeach()
