# ctest dashboard_copy: the dashboard next to the built binary (DST) is the one of
# the sources (SRC), file by file. Run as `cmake -DSRC=... -DDST=... -P <this>`.
file(GLOB_RECURSE files RELATIVE "${SRC}" "${SRC}/*")
set(stale "")
foreach(f IN LISTS files)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SRC}/${f}" "${DST}/${f}"
                    RESULT_VARIABLE differs OUTPUT_QUIET ERROR_QUIET)
    if(differs)
        list(APPEND stale "${f}")
    endif()
endforeach()
if(stale)
    message(FATAL_ERROR "the dashboard next to the binary is stale or incomplete: ${stale}")
endif()
message(STATUS "dashboard copy matches the sources (${files})")
