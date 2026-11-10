add_executable(falcon_wavdiff main.cpp)
falcon_compile_options(falcon_wavdiff STRICT)
# A test tool: kept out of bin/, which is what ships (falcon.exe and its runtime folders).
set_target_properties(falcon_wavdiff PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
