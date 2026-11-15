# The firmware's sources as in the simulator build (cmake/firmware_sources.cmake),
# minus the mock platform: here the real ESP-IDF drivers provide the headers
# the app includes.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
if(NOT DEFINED ENV{BOARD})
    set(BOARD m5stack-cores3)
else()
    set(BOARD $ENV{BOARD})
endif()

include(${ROOT}/cmake/firmware_sources.cmake)   # FW_*_SRCS, FW_SRC_INCLUDES, LUA_BUILTINS

idf_component_register(
    SRCS ${FW_APP_SRCS} ${FW_LUA_SRCS} ${FW_LIB_SRCS} ${CMAKE_CURRENT_LIST_DIR}/panic_hook.c
    INCLUDE_DIRS ${FW_SRC_INCLUDES}
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_usb_serial_jtag esp_driver_uart esp_driver_ledc esp_driver_sdspi sdmmc fatfs vfs
             esp_driver_i2s esp_adc esp_timer nvs_flash heap esp_wifi esp_netif esp_event lwip mbedtls pthread bt
             app_update esp_app_format esp_system xtensa esp_rom)
# png.c inflates with the ROM's miniz (tinfl): esp_rom/include/miniz.h.
# mdns comes from the component registry, see idf_component.yml

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
# The crash record (panic_hook.c): ESP-IDF's panic handler goes through ours
# first. -u keeps the wrapper in the link though nothing calls it by name.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler" "-Wl,-u,__wrap_esp_panic_handler")
# The Lua core limits nesting by the stack left, not a count
# (app/src/cstack.h). Keep in step with the top-level CMakeLists.txt.
set_source_files_properties(${FW_LUA_SRCS} PROPERTIES COMPILE_OPTIONS "-include;${ROOT}/app/src/cstack.h")

# The built-in Lua libraries (app/lua/*.lua) as byte arrays for builtin_lua.c,
# generated as in the simulator build (cmake/embed_files.cmake).
set(BUILTIN_LUA_H ${CMAKE_CURRENT_BINARY_DIR}/gen/builtin_lua_data.h)
add_custom_command(
    OUTPUT ${BUILTIN_LUA_H}
    COMMAND ${CMAKE_COMMAND} -DOUT=${BUILTIN_LUA_H} "-DFILES=${LUA_BUILTINS}" -P ${ROOT}/cmake/embed_files.cmake
    DEPENDS ${LUA_BUILTINS} ${ROOT}/cmake/embed_files.cmake
    VERBATIM)
add_custom_target(builtin_lua_data DEPENDS ${BUILTIN_LUA_H})
add_dependencies(${COMPONENT_LIB} builtin_lua_data)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
