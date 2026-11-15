# Rebuilds the simulator whenever a source changes, while it runs: the `run`
# target starts this next to the simulator, which boots each new build in its
# window (--watch), so saving a file is enough. A build that fails leaves the
# last good one running. Polls twice a second, so no file watcher is needed.
# Ends when the simulator does: its "<nvs>.run" mark is gone, or wasn't
# renewed for 30 s (the simulator rewrites it every 5 s with --watch).
#
#   cmake -DSRC=<repo> -DBUILD_DIR=<build> -DCONFIG=<Release> -DMARK=<nvs>.run -P watch_build.cmake
foreach(v SRC BUILD_DIR MARK)
    if(NOT ${v})
        message(FATAL_ERROR "watch_build.cmake: -D${v}= missing")
    endif()
endforeach()

function(sources_stamp out)
    # Single files are listed, not globbed: GLOB_RECURSE would look for their
    # names in every folder of the repository (.toolchain, build, target).
    file(GLOB_RECURSE files
        ${SRC}/app/src/* ${SRC}/app/lua/* ${SRC}/platform/* ${SRC}/libs/* ${SRC}/cmake/*)
    list(APPEND files ${SRC}/CMakeLists.txt ${SRC}/app/idf/sdkconfig.defaults)
    set(stamp "")
    foreach(f IN LISTS files)
        file(TIMESTAMP "${f}" t "%Y%m%d%H%M%S%f" UTC)
        string(APPEND stamp "${f}=${t};")
    endforeach()
    string(MD5 stamp "${stamp}")
    set(${out} "${stamp}" PARENT_SCOPE)
endfunction()

function(sim_alive out)
    set(${out} FALSE PARENT_SCOPE)
    if(EXISTS "${MARK}")
        file(TIMESTAMP "${MARK}" t "%s" UTC)
        string(TIMESTAMP now "%s" UTC)
        math(EXPR age "${now} - ${t}")
        if(age LESS 30)
            set(${out} TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Wait for the simulator to come up (its run mark), then watch while it runs.
foreach(i RANGE 100)
    sim_alive(alive)
    if(alive)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.1)
endforeach()

sources_stamp(last)
while(TRUE)
    sim_alive(alive)
    if(NOT alive)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.5)
    sources_stamp(now)
    if(now STREQUAL last)
        continue()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.2)   # an editor may save in steps
    sources_stamp(last)                                         # (before building: a save meanwhile is caught next round)
    set(cfg)
    if(CONFIG)
        set(cfg --config ${CONFIG})
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} ${cfg} --target esp32-sim
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    # To stderr: stdout goes nowhere (see run.cmake).
    if(rc EQUAL 0)
        message(NOTICE "watch: rebuilt, the simulator boots it")
    else()
        message(NOTICE "${out}\nwatch: build failed, the simulator keeps the last good one")
    endif()
endwhile()
