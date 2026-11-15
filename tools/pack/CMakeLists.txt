add_executable(falcon_pack main.cpp)
target_link_libraries(falcon_pack PRIVATE falcon_platform)
falcon_compile_options(falcon_pack STRICT)
# A build tool: kept out of bin/, which is what ships.
set_target_properties(falcon_pack PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
