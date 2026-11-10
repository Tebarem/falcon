add_executable(falcon_imgdiff main.cpp)
target_link_libraries(falcon_imgdiff PRIVATE SDL3::SDL3)
# A test tool: kept out of bin/, which is what ships (falcon.exe and its runtime folders).
set_target_properties(falcon_imgdiff PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
