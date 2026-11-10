# The cooker, linked into falcon.exe as `falcon cook` (cook_main.h). It uses the engine's
# anim_core and json (falcon_engine has them) and owns the importers' implementations (cgltf,
# stb_image, stb_image_resize, rgbcx, clusterlod, bc7enc).
# clusterlod.h (meshoptimizer's demo) with a local patch, a simplifier override hook that
# build_mesh.cpp uses to prune foliage. Like SDL, it is copied into the build tree and patched
# there; the submodule stays pristine.
set(FX_CLOD_SOURCE "${CMAKE_SOURCE_DIR}/external/meshoptimizer/demo/clusterlod.h")
set(FX_CLOD_PATCH "${CMAKE_SOURCE_DIR}/cmake/patches/meshoptimizer-clusterlod-override.patch")
set(FX_CLOD_ROOT "${CMAKE_BINARY_DIR}/_deps/clusterlod")
file(SHA256 "${FX_CLOD_SOURCE}" fx_clod_source_hash)
file(SHA256 "${FX_CLOD_PATCH}" fx_clod_patch_hash)
set(fx_clod_stamp_expected "${fx_clod_source_hash}-${fx_clod_patch_hash}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FX_CLOD_SOURCE}" "${FX_CLOD_PATCH}")
set(fx_clod_stamp_current "")
if(EXISTS "${FX_CLOD_ROOT}/.falcon-stamp")
  file(READ "${FX_CLOD_ROOT}/.falcon-stamp" fx_clod_stamp_current)
endif()
if(NOT fx_clod_stamp_current STREQUAL fx_clod_stamp_expected)
  file(REMOVE_RECURSE "${FX_CLOD_ROOT}")
  file(COPY "${FX_CLOD_SOURCE}" DESTINATION "${FX_CLOD_ROOT}/demo")
  execute_process(COMMAND git init -q WORKING_DIRECTORY "${FX_CLOD_ROOT}" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND git apply -p1 --whitespace=nowarn "${FX_CLOD_PATCH}"
    WORKING_DIRECTORY "${FX_CLOD_ROOT}"
    RESULT_VARIABLE fx_clod_apply)
  if(NOT fx_clod_apply EQUAL 0)
    message(FATAL_ERROR "Failed to apply ${FX_CLOD_PATCH}")
  endif()
  file(WRITE "${FX_CLOD_ROOT}/.falcon-stamp" "${fx_clod_stamp_expected}")
endif()

add_library(falcon_cook_lib STATIC main.cpp import_gltf.cpp import_anim.cpp build_anim.cpp build_graph.cpp build_mesh.cpp build_textures.cpp build_collision.cpp build_rocks.cpp build_displace.cpp
  "${CMAKE_SOURCE_DIR}/external/bc7enc/bc7enc.c")
target_include_directories(falcon_cook_lib PRIVATE
  "${CMAKE_SOURCE_DIR}/engine"
  "${CMAKE_SOURCE_DIR}/external/meshoptimizer/src"
  "${FX_CLOD_ROOT}/demo"  # the patched clusterlod.h
  "${CMAKE_SOURCE_DIR}/external/stb"
  "${CMAKE_SOURCE_DIR}/external/stb_image_resize"
  "${CMAKE_SOURCE_DIR}/external/bc7enc")
target_include_directories(falcon_cook_lib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(falcon_cook_lib PRIVATE meshoptimizer cgltf box3d)
falcon_compile_options(falcon_cook_lib)
target_compile_definitions(falcon_cook_lib PRIVATE _CRT_SECURE_NO_WARNINGS)

# Cooker tests on the small Khronos sample (if downloaded): structural verification
# and byte-identical output across two cooks.
set(fx_helmet "${CMAKE_SOURCE_DIR}/samples/_src/khronos/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb")
if(EXISTS "${fx_helmet}")
  add_test(NAME cook_verify COMMAND falcon cook "${fx_helmet}" "${CMAKE_BINARY_DIR}/test/helmet_a.fpak" --verify)
  add_test(NAME cook_determinism_second COMMAND falcon cook "${fx_helmet}" "${CMAKE_BINARY_DIR}/test/helmet_b.fpak" --force --threads 3)
  add_test(NAME cook_determinism_compare COMMAND ${CMAKE_COMMAND} -E compare_files
    "${CMAKE_BINARY_DIR}/test/helmet_a.fpak" "${CMAKE_BINARY_DIR}/test/helmet_b.fpak")
  add_test(NAME cook_determinism_compare_vt COMMAND ${CMAKE_COMMAND} -E compare_files
    "${CMAKE_BINARY_DIR}/test/helmet_a.fvt" "${CMAKE_BINARY_DIR}/test/helmet_b.fvt")
  set_tests_properties(cook_determinism_compare_vt PROPERTIES DEPENDS cook_determinism_second)
  set_tests_properties(cook_determinism_second PROPERTIES DEPENDS cook_verify)
  set_tests_properties(cook_determinism_compare PROPERTIES DEPENDS cook_determinism_second)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test")
endif()

# Height displacement (a material's height map pushed into copies of the sphere, cube and plane):
# verified, and byte-identical across two cooks.
set(fx_displace_data "${CMAKE_SOURCE_DIR}/runtime/tests/data")
set(fx_displace_args --rocks --source-root "${fx_displace_data}" --material "${fx_displace_data}/studs.fmat"
  --displace "sphere=${fx_displace_data}/studs.fmat" --displace "cube=${fx_displace_data}/studs.fmat"
  --displace "plane=${fx_displace_data}/studs.fmat")
add_test(NAME cook_displace COMMAND falcon cook --empty "${CMAKE_BINARY_DIR}/test/displace_a.fpak" ${fx_displace_args} --verify --force)
add_test(NAME cook_displace_second COMMAND falcon cook --empty "${CMAKE_BINARY_DIR}/test/displace_b.fpak" ${fx_displace_args} --force --threads 3)
add_test(NAME cook_displace_compare COMMAND ${CMAKE_COMMAND} -E compare_files
  "${CMAKE_BINARY_DIR}/test/displace_a.fpak" "${CMAKE_BINARY_DIR}/test/displace_b.fpak")
set_tests_properties(cook_displace PROPERTIES PASS_REGULAR_EXPRESSION "cube displaced by studs.fmat: 12 -> [0-9]+ triangles.*verify: 0 violations")
set_tests_properties(cook_displace_second PROPERTIES DEPENDS cook_displace)
set_tests_properties(cook_displace_compare PROPERTIES DEPENDS cook_displace_second)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test")
