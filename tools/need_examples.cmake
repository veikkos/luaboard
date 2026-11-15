# Stop with one clear line when examples/ (the luaboard-marketplace
# submodule) isn't checked out, instead of failing deep inside a test.
#
#   cmake -DROOT=<repo> -P need_examples.cmake
if(NOT EXISTS ${ROOT}/examples/hello/app.json)
    message(FATAL_ERROR "examples/ is empty: run git submodule update --init")
endif()
