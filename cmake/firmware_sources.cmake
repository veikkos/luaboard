# The firmware's sources and include folders, for both of its builds: the
# simulator (CMakeLists.txt) and the device (app/idf/main/CMakeLists.txt).
# Each build adds its own: the mock platform and miniz on the computer,
# ESP-IDF's drivers and the ROM's miniz on the device. Needs ROOT and BOARD.

# A new file is picked up at the next build. (ESP-IDF first reads its
# components in script mode, which has no CONFIGURE_DEPENDS.)
if(CMAKE_SCRIPT_MODE_FILE)
    set(GLOB_DEPENDS)
else()
    set(GLOB_DEPENDS CONFIGURE_DEPENDS)
endif()

set(LUA_DIR ${ROOT}/third_party/lua/src)
# Only the Lua core and the libraries the firmware opens (no io/os/debug/package).
file(GLOB FW_LUA_SRCS ${GLOB_DEPENDS} ${LUA_DIR}/*.c)
list(FILTER FW_LUA_SRCS EXCLUDE REGEX ".*/(lua|luac|linit|liolib|loslib|loadlib|ldblib)\\.c$")

file(GLOB FW_APP_SRCS ${GLOB_DEPENDS} ${ROOT}/app/src/*.c)
set(FW_LIB_SRCS ${ROOT}/libs/font5x7/font5x7.c ${ROOT}/libs/sha256/sha256.c ${ROOT}/libs/sha1/sha1.c
    ${ROOT}/third_party/tjpgd/tjpgd.c
    ${ROOT}/third_party/qrcodegen/qrcodegen.c)
set(FW_SRC_INCLUDES
    ${ROOT}/platform/boards/${BOARD} ${ROOT}/app/src ${ROOT}/libs/font5x7 ${ROOT}/libs/sha256 ${ROOT}/libs/sha1
    ${ROOT}/third_party/tjpgd ${ROOT}/third_party/bmi270 ${ROOT}/third_party/qrcodegen ${LUA_DIR})

# The built-in Lua libraries, embedded as byte arrays (builtin_lua.c).
file(GLOB LUA_BUILTINS ${GLOB_DEPENDS} ${ROOT}/app/lua/*.lua)
