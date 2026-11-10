add_executable(falcon_net_test main.cpp)
target_link_libraries(falcon_net_test PRIVATE falcon_net)
falcon_compile_options(falcon_net_test STRICT)
# A test tool: kept out of bin/, which is what ships (falcon.exe and its runtime folders).
set_target_properties(falcon_net_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
