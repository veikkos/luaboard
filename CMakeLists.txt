# luaboard: the desktop build (simulator and tools), on Windows, macOS and Linux.
#
#   cmake --preset default                configure (build/; see CMakePresets.json)
#   cmake --build --preset default        the simulator (build/<board>/esp32-sim[.exe])
#
# Targets, `cmake --build --preset default --target <name>`:
#   esp32-sim   the native simulator (firmware + ESP32 mock + SDL window); default
#   run         build it (in build/run) and open its window, serial link at /tmp/esp32-sim
#               (Windows: \\.\pipe\esp32-sim), control channel on 127.0.0.1:3334
#               (`luaboard sim-ctl`). It rebuilds when a source changes
#               and boots the new firmware in the same window, apps kept
#               (tools/watch_build.cmake and the simulator's --watch)
#   tool        the PC tools (Rust: luaboard-studio GUI + luaboard CLI)
#   studio      build and start the studio
#   apps        (re)install your apps from marketplace/ on the device or simulator
#   unit        Rust unit tests and the C unit tests (fw-unit, app/test)
#   e2e         end-to-end tests against headless simulators (pc-tool/tests/e2e)
#   lint        rustfmt and clippy
#   werror      the simulator with warnings as errors (in build/werror)
#   check       what CI checks: lint, werror, unit, e2e
#   hooks       make `git push` run check first (and fw when the firmware changed)
#   toolchain   install ESP-IDF into .toolchain with Espressif's `eim`
#   fw, flash   the device firmware via ESP-IDF
#
# Options for one command are environment variables, from the repository root
# (cmake/targets.cmake), e.g. `MODEL=cores3 cmake --build --preset default --target run`:
#   MODEL          run: board variant, cores3se (default), cores3, cores3-lite
#   BLE_DEVICE     run: simulated Bluetooth devices, "a.json b.json"
#   WATCH=0        run: no rebuilding when a source changes
#   WIFI_NET       run: the network the board joins while it has none set up (default Simulator, none: no)
#   APPS_URL       run: the app marketplace, https://raw.githubusercontent.com/veikkos/luaboard-marketplace
#   STUDIO_ARGS    studio: a project folder, some of its .lua files, or a firmware .bin
#   LUABOARD_PORT  studio, apps, flash: the device's serial port (else the simulator)
#   APPS           apps: only these, "gcal paint" (default tools/own_apps.txt, "all": every app)
#   E2E_ONLY       e2e: only sections whose name contains a word, "oauth menu"
#   E2E_KEEP=1     e2e: keep failed sections' work folders (logs, screenshots)
#   E2E_LIMIT      e2e: time limit per section in seconds (default 120)
#
# Dependencies: SDL2 and mbedTLS 4 come from the system when CMake finds them
# (Homebrew, apt, vcpkg), otherwise they are downloaded and built here
# (LUABOARD_FETCH_DEPS).
cmake_minimum_required(VERSION 3.25)
project(luaboard C)
if(APPLE)
    enable_language(OBJC)
endif()

set(BOARD m5stack-cores3 CACHE STRING "Board: platform/boards/<BOARD> + platform/sim/boards/<BOARD>")
option(LUABOARD_WERROR "Treat warnings in our own sources as errors" OFF)
option(LUABOARD_FETCH_DEPS "Download SDL2 / mbedTLS when they aren't installed" ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
# The simulator lands in build/<board>/, where the docs, the studio's
# Firmware window and the tests look for it.
set(SIM_DIR ${CMAKE_CURRENT_BINARY_DIR}/${BOARD})

# Static runtime on Windows: a simulator image is one self-contained .exe,
# so the firmware-update slots can hold a copy of it anywhere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# ------------------------------------------------------------- dependencies

include(FetchContent)

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2-static)
    set(SDL_LIB SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(SDL_LIB SDL2::SDL2)
elseif(LUABOARD_FETCH_DEPS)
    message(STATUS "SDL2 not found: building it")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
        URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL2)
    set(SDL_LIB SDL2::SDL2-static)
else()
    message(FATAL_ERROR "SDL2 not found (brew install sdl2 / apt install libsdl2-dev), or configure with -DLUABOARD_FETCH_DEPS=ON")
endif()

# mbedTLS 4.x: the same library family ESP-IDF 6 uses.
# Static when the system has it: a simulator image in an OTA slot must still
# start after the system's mbedTLS is upgraded (or on another computer).
find_package(MbedTLS 4 CONFIG QUIET)
if(TARGET MbedTLS::mbedtls_static)
    set(TLS_LIBS MbedTLS::mbedtls_static MbedTLS::mbedx509_static)
elseif(TARGET MbedTLS::mbedtls)
    set(TLS_LIBS MbedTLS::mbedtls MbedTLS::mbedx509)
elseif(LUABOARD_FETCH_DEPS)
    message(STATUS "mbedTLS 4 not found: building it")
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(GEN_FILES OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2
        URL_HASH SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(mbedtls)
    set(TLS_LIBS mbedtls mbedx509)
else()
    message(FATAL_ERROR "mbedTLS 4 not found (brew install mbedtls), or configure with -DLUABOARD_FETCH_DEPS=ON")
endif()

# ------------------------------------------------------------------ sources

include(${ROOT}/cmake/firmware_sources.cmake)   # FW_*_SRCS, FW_SRC_INCLUDES, LUA_BUILTINS
file(GLOB MOCK_SRCS CONFIGURE_DEPENDS ${ROOT}/platform/esp32-mock/src/*.c)
file(GLOB SIM_SRCS CONFIGURE_DEPENDS
    ${ROOT}/platform/sim/*.c ${ROOT}/platform/sim/models/*.c ${ROOT}/platform/sim/boards/${BOARD}/*.c)

# The host abstraction: what the simulator needs from the computer it runs on.
if(WIN32)
    file(GLOB HOST_SRCS CONFIGURE_DEPENDS ${ROOT}/platform/host/win32/*.c)
else()
    file(GLOB HOST_SRCS CONFIGURE_DEPENDS ${ROOT}/platform/host/posix/*.c)
endif()
list(APPEND HOST_SRCS ${ROOT}/platform/host/host_text.c)
# The Bluetooth of the computer: CoreBluetooth on macOS, none elsewhere (the
# simulated devices still work).
if(APPLE)
    list(FILTER MOCK_SRCS EXCLUDE REGEX ".*/mock_ble_host_none\\.c$")
    list(APPEND MOCK_SRCS ${ROOT}/platform/esp32-mock/src/mock_ble_host.m)
endif()

# The firmware thread gets the device's main task stack, read from the device
# config, so a script that runs out of stack on the board does so here too.
file(STRINGS ${ROOT}/app/idf/sdkconfig.defaults MAIN_STACK_LINE REGEX "^CONFIG_ESP_MAIN_TASK_STACK_SIZE=")
string(REGEX REPLACE "^CONFIG_ESP_MAIN_TASK_STACK_SIZE=" "" MAIN_STACK "${MAIN_STACK_LINE}")
if(NOT MAIN_STACK)
    message(FATAL_ERROR "CONFIG_ESP_MAIN_TASK_STACK_SIZE missing from app/idf/sdkconfig.defaults")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROOT}/app/idf/sdkconfig.defaults)

# ------------------------------------------------------------ generated

# The built-in Lua libraries as byte arrays (builtin_lua.c).
add_custom_command(
    OUTPUT ${GEN}/builtin_lua_data.h
    COMMAND ${CMAKE_COMMAND} -DOUT=${GEN}/builtin_lua_data.h "-DFILES=${LUA_BUILTINS}" -P ${ROOT}/cmake/embed_files.cmake
    DEPENDS ${LUA_BUILTINS} ${ROOT}/cmake/embed_files.cmake
    VERBATIM)
# The build's version for the simulator image tag: checked at every build.
add_custom_target(sim_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${ROOT} -DOUT=${GEN}/sim_version.h -P ${ROOT}/cmake/sim_version.cmake
    BYPRODUCTS ${GEN}/sim_version.h
    VERBATIM)

# ---------------------------------------------------------------- flags

if(MSVC)
    # C17 with C11 atomics; the POSIX the firmware expects comes from the
    # Windows layer (platform/host/win32/posix).
    set(WARN /W3 /wd4100 /wd4244 /wd4267 /wd4305 /wd4996 /wd4146 /wd4018 /wd4389 /wd4142)   # (4142: tjpgd.h's own int types)
    set(LANG_FLAGS /std:c17 /experimental:c11atomics /utf-8 /Zc:preprocessor)
    if(LUABOARD_WERROR)
        list(APPEND WARN /WX)
    endif()
    set(QUIET /W0)
    set(FORCE_INCLUDE /FI)
else()
    set(WARN -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # snprintf into a fixed buffer cuts text that is too long, on purpose
        list(APPEND WARN -Wno-format-truncation)
    endif()
    set(LANG_FLAGS -std=gnu11)
    if(LUABOARD_WERROR)
        list(APPEND WARN -Werror)
    endif()
    set(QUIET -w)
    set(FORCE_INCLUDE -include)
endif()

set(COMMON_DEFS CONFIG_IDF_TARGET="sim" CONFIG_ESP_MAIN_TASK_STACK_SIZE=${MAIN_STACK})
set(FW_INCLUDES ${ROOT}/platform/esp32-mock/include ${FW_SRC_INCLUDES} ${GEN} ${ROOT}/third_party/miniz)
if(WIN32)
    # The POSIX headers ESP-IDF gives the firmware, for MSVC; forced into
    # every source of ours for the few names that are spelled differently.
    list(PREPEND FW_INCLUDES ${ROOT}/platform/host/win32/posix)
    set(PLATFORM_FORCE ${FORCE_INCLUDE}${ROOT}/platform/host/win32/posix/posix_compat.h)
    list(APPEND COMMON_DEFS _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 SDL_MAIN_HANDLED)
else()
    set(PLATFORM_FORCE)
endif()

# Lua: upstream code, built quietly with its own warning level. The core
# limits nesting by the stack left, not a count (app/src/cstack.h).
add_library(lua_core OBJECT ${FW_LUA_SRCS})
target_include_directories(lua_core PRIVATE ${LUA_DIR} ${ROOT}/app/src)
target_compile_options(lua_core PRIVATE ${LANG_FLAGS} ${QUIET} ${PLATFORM_FORCE}
    ${FORCE_INCLUDE}${ROOT}/app/src/cstack.h)
if(WIN32)
    target_include_directories(lua_core PRIVATE ${ROOT}/platform/host/win32/posix)
    target_compile_definitions(lua_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# miniz: only its inflate (tinfl) is used, by png.c. The device uses an older
# tinfl in its ROM (ESP-IDF's esp_rom/include/miniz.h).
add_library(miniz OBJECT ${ROOT}/third_party/miniz/miniz.c)
target_include_directories(miniz PRIVATE ${ROOT}/third_party/miniz)
target_compile_definitions(miniz PRIVATE MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(miniz PRIVATE ${LANG_FLAGS} ${QUIET})

# The firmware, unchanged: its wall clock is the simulated world's
# (sim_wallclock.h).
add_library(firmware OBJECT ${FW_APP_SRCS} ${GEN}/builtin_lua_data.h)
target_include_directories(firmware PRIVATE ${FW_INCLUDES})
target_compile_definitions(firmware PRIVATE ${COMMON_DEFS} MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_compile_options(firmware PRIVATE ${LANG_FLAGS} ${WARN} ${PLATFORM_FORCE}
    ${FORCE_INCLUDE}${ROOT}/platform/esp32-mock/include/sim_wallclock.h)
target_link_libraries(firmware PRIVATE ${TLS_LIBS})

# The ESP32 mock, the board models, the window and the host layer.
add_executable(esp32-sim ${MOCK_SRCS} ${SIM_SRCS} ${HOST_SRCS} ${FW_LIB_SRCS}
    $<TARGET_OBJECTS:firmware> $<TARGET_OBJECTS:lua_core> $<TARGET_OBJECTS:miniz>)
add_dependencies(esp32-sim sim_version)
target_include_directories(esp32-sim PRIVATE ${FW_INCLUDES}
    ${ROOT}/platform/sim ${ROOT}/platform/sim/models ${ROOT}/platform/host/include)
target_compile_definitions(esp32-sim PRIVATE ${COMMON_DEFS})
target_compile_options(esp32-sim PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${LANG_FLAGS} ${WARN} ${PLATFORM_FORCE}>
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc ${WARN}>)
target_link_libraries(esp32-sim PRIVATE ${SDL_LIB} ${TLS_LIBS})
set_target_properties(esp32-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SIM_DIR})
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(esp32-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} ${SIM_DIR})
endforeach()

if(WIN32)
    target_link_libraries(esp32-sim PRIVATE ws2_32 dnsapi bcrypt winmm crypt32)
    # A console program: the simulator prints its serial port and logs.
    if(MSVC)
        # The firmware thread's stack is set where it starts; the main
        # thread runs the window.
        target_link_options(esp32-sim PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
elseif(APPLE)
    # macOS lets a program use Bluetooth only if it says why (NSBluetoothAlwaysUsageDescription).
    set(SIM_PLIST ${ROOT}/platform/sim/Info.plist)
    # Security: the keychain's trusted roots for TLS (host_posix.c).
    target_link_libraries(esp32-sim PRIVATE "-framework CoreBluetooth" "-framework Foundation" "-framework Security")
    target_link_options(esp32-sim PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${SIM_PLIST}")
    set_property(TARGET esp32-sim APPEND PROPERTY LINK_DEPENDS ${SIM_PLIST})
else()
    find_package(Threads REQUIRED)
    target_link_libraries(esp32-sim PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
    # mDNS through Avahi's Bonjour API when it's installed (libavahi-compat-libdnssd-dev).
    find_library(DNSSD_LIB dns_sd)
    find_path(DNSSD_INC dns_sd.h)
    if(DNSSD_LIB AND DNSSD_INC)
        target_compile_definitions(esp32-sim PRIVATE HOST_HAVE_DNS_SD=1)
        target_link_libraries(esp32-sim PRIVATE ${DNSSD_LIB})
    endif()
endif()
if(APPLE)
    target_compile_definitions(esp32-sim PRIVATE HOST_HAVE_DNS_SD=1)
endif()

# C unit tests of the pure logic in app/src (no ESP32 mock, no window).
file(GLOB UNIT_SRCS CONFIGURE_DEPENDS ${ROOT}/app/test/*.c)
add_executable(fw-unit ${UNIT_SRCS} ${ROOT}/app/src/update_manifest.c ${ROOT}/app/src/hex.c
    ${ROOT}/app/src/json.c ${ROOT}/app/src/text.c ${ROOT}/app/src/wake_time.c
    ${ROOT}/app/src/semver.c ${ROOT}/app/src/apps_index.c ${ROOT}/app/src/names.c
    ${ROOT}/app/src/check_sched.c ${ROOT}/app/src/tz.c ${ROOT}/third_party/qrcodegen/qrcodegen.c)
target_include_directories(fw-unit PRIVATE ${ROOT}/app/src ${ROOT}/third_party/qrcodegen)
target_compile_options(fw-unit PRIVATE ${LANG_FLAGS} ${WARN})
if(MSVC)
    target_compile_definitions(fw-unit PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(fw-unit PROPERTIES EXCLUDE_FROM_ALL TRUE)

include(${ROOT}/cmake/targets.cmake)
