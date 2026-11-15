# The `flash` target: flash the firmware with idf.py to the board on
# LUABOARD_PORT, read now from the environment (cmake/targets.cmake).
#
#   cmake -DEIM= -DIDF_BUILD= -DIDF_VERSION= -DBOARD= -DFW_IMAGE= -P flash.cmake
if("$ENV{LUABOARD_PORT}" STREQUAL "")
    message(FATAL_ERROR "flash: set LUABOARD_PORT to the board's serial port, e.g.\n"
        "  LUABOARD_PORT=/dev/cu.usbmodem1101 cmake --build --preset default --target flash\n"
        "or without ESP-IDF: luaboard -p <port> flash ${FW_IMAGE}")
endif()
set(ENV{BOARD} ${BOARD})
execute_process(
    COMMAND ${EIM} run "idf.py -B ${IDF_BUILD} -p $ENV{LUABOARD_PORT} flash" ${IDF_VERSION}
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "flash: idf.py failed (${rc})")
endif()
