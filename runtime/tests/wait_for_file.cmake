# cmake -DFILE=<path> -DSECONDS=<n> -P wait_for_file.cmake: prints the file once it exists (a process
# started by the one under test writes it), failing after SECONDS.
foreach(i RANGE ${SECONDS})
  if(EXISTS "${FILE}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)  # (written whole: give it a moment)
    file(READ "${FILE}" text)
    message("${text}")
    return()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
endforeach()
message(FATAL_ERROR "${FILE} did not appear in ${SECONDS} s")
