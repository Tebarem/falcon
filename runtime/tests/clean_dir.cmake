# cmake -DDIR=<folder> -P clean_dir.cmake: removes the folder, retrying for a few seconds (a
# preview process the last editor test stopped, or a virus scan of files just written, can hold
# it briefly).
foreach(attempt RANGE 20)
  file(REMOVE_RECURSE "${DIR}")
  if(NOT EXISTS "${DIR}")
    return()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.25)
endforeach()
message(FATAL_ERROR "Could not remove ${DIR}")
