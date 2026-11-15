# The `run` target: the simulator in its window, its serial link at
# /tmp/esp32-sim (Windows: \\.\pipe\esp32-sim), its control channel on
# 127.0.0.1:3334 (`luaboard sim-ctl`), and next to it the watcher that
# rebuilds it when a source changes (watch_build.cmake).
#
#   cmake -DSRC= -DBUILD_DIR= -DCONFIG= -DSIM= -DSTATE= -P run.cmake
#
# Options come from the environment, for this run only (as make's were):
#   MODEL=cores3se|cores3|cores3-lite   the board variant
#   BLE_DEVICE="a.json b.json"          simulated Bluetooth devices
#   WATCH=0                             no rebuilding, no booting new builds
# The target runs from the repository root, so paths are taken from there.
#
# Both run as one pipeline, which is how a CMake script starts two programs
# at once: the watcher's stdout goes to the simulator's stdin, which it
# doesn't read; the watcher talks on stderr.
if(WIN32)
    set(link esp32-sim)
else()
    set(link /tmp/esp32-sim)
endif()
set(args --link ${link} --nvs ${STATE}/esp32-sim-nvs.bin --flash ${STATE}/sim-flash --control 3334)
if(NOT "$ENV{MODEL}" STREQUAL "")
    list(APPEND args --model "$ENV{MODEL}")
endif()
separate_arguments(devices NATIVE_COMMAND "$ENV{BLE_DEVICE}")
foreach(d IN LISTS devices)
    list(APPEND args --ble-device ${d})
endforeach()

if("$ENV{WATCH}" STREQUAL "0")
    execute_process(COMMAND ${SIM} ${args})
else()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DSRC=${SRC} -DBUILD_DIR=${BUILD_DIR} -DCONFIG=${CONFIG}
                -DMARK=${STATE}/esp32-sim-nvs.bin.run -P ${SRC}/tools/watch_build.cmake
        COMMAND ${SIM} ${args} --watch)
endif()
