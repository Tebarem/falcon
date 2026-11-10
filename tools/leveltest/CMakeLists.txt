# The level tools' kernel and geometry libraries (Manifold, MikkTSpace, xatlas, Recast/Detour),
# and the level kernel (engine/level), tested without the engine: their sources are compiled in
# rather than linking falcon_engine.
add_executable(falcon_level_test main.cpp "${CMAKE_SOURCE_DIR}/engine/core/mesh_tangents.cpp"
  "${CMAKE_SOURCE_DIR}/engine/core/json.cpp" "${CMAKE_SOURCE_DIR}/engine/level/poly_mesh.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/mesh_ops.cpp" "${CMAKE_SOURCE_DIR}/engine/level/mesh_shapes.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/trim_sheet.cpp" "${CMAKE_SOURCE_DIR}/engine/level/mesh_cut.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/walls.cpp" "${CMAKE_SOURCE_DIR}/engine/level/roofs.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/cut_bake.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/wear.cpp" "${CMAKE_SOURCE_DIR}/engine/level/mask_brush.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/rubble.cpp" "${CMAKE_SOURCE_DIR}/engine/level/modifiers.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/splines.cpp" "${CMAKE_SOURCE_DIR}/engine/level/navmesh.cpp"
  "${CMAKE_SOURCE_DIR}/engine/level/smart_material.cpp")
target_include_directories(falcon_level_test PRIVATE "${CMAKE_SOURCE_DIR}/engine")
target_link_libraries(falcon_level_test PRIVATE manifold mikktspace xatlas Recast Detour DetourCrowd)
falcon_compile_options(falcon_level_test STRICT)
# A test tool: kept out of bin/, which is what ships (falcon.exe and its runtime folders).
set_target_properties(falcon_level_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
