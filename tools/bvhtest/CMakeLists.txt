# The ray tracing BVH builder (engine/render/ray_tracing/bvh_builder.*), tested without the engine:
# its traversal on the CPU (the GPU's logic) against brute force.
add_executable(falcon_bvh_test main.cpp "${CMAKE_SOURCE_DIR}/engine/render/ray_tracing/bvh_builder.cpp")
target_include_directories(falcon_bvh_test PRIVATE "${CMAKE_SOURCE_DIR}/engine")
falcon_compile_options(falcon_bvh_test STRICT)
# A test tool: kept out of bin/, which is what ships (falcon.exe and its runtime folders).
set_target_properties(falcon_bvh_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
