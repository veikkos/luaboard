# The `examples` target: (re)install the apps in tools/own_apps.txt from
# marketplace/ on the device or simulator ($LUABOARD_PORT, which the CLI reads,
# else the simulator). EXAMPLES="gcal paint" (environment) installs only
# those, EXAMPLES=all every app in marketplace/.
#
#   cmake -DROOT=<repo> -DCLI=<luaboard> -P install_examples.cmake
if(DEFINED ENV{EXAMPLES} AND NOT "$ENV{EXAMPLES}" STREQUAL "" AND NOT "$ENV{EXAMPLES}" STREQUAL "all")
    string(REPLACE " " ";" names "$ENV{EXAMPLES}")
elseif(NOT "$ENV{EXAMPLES}" STREQUAL "all")
    file(STRINGS ${ROOT}/tools/own_apps.txt names REGEX "^[^#]")
else()
    file(GLOB dirs LIST_DIRECTORIES true ${ROOT}/marketplace/*)
    set(names)
    foreach(d IN LISTS dirs)
        if(IS_DIRECTORY ${d} AND EXISTS ${d}/main.lua)   # apps, not .github/ or tools/
            get_filename_component(n ${d} NAME)
            list(APPEND names ${n})
        endif()
    endforeach()
endif()
set(failed)
foreach(e IN LISTS names)
    message(STATUS "== ${e}")
    execute_process(COMMAND ${CLI} install ${ROOT}/marketplace/${e} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        list(APPEND failed ${e})
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "not installed: ${failed}")
endif()
