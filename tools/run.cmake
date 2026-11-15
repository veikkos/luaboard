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
#   WIFI_NET=<name>|none                the network the board joins while it has
#                                       none set up (default Simulator, so it is
#                                       online without `luaboard wifi set`; none:
#                                       it waits for one, as a new board does)
#   APPS_URL=<base>                     the app marketplace for Get apps and
#                                       app updates (none: the simulator
#                                       doesn't reach it), e.g.
#                                       https://raw.githubusercontent.com/veikkos/luaboard-marketplace
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
if("$ENV{WIFI_NET}" STREQUAL "")
    list(APPEND args --set wifi_net=Simulator)
elseif(NOT "$ENV{WIFI_NET}" STREQUAL "none")
    list(APPEND args --set "wifi_net=$ENV{WIFI_NET}")
endif()
if(NOT "$ENV{APPS_URL}" STREQUAL "")
    list(APPEND args --set "apps_url=$ENV{APPS_URL}")
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
