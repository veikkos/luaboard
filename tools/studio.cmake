# The `studio` target: build and start the studio, with the options read now
# from the environment (cmake/targets.cmake): STUDIO_ARGS (a project folder,
# some of its .lua files, or a firmware .bin) and LUABOARD_PORT. Without a
# port the studio picks the simulator's link if one is running.
#
#   cmake -DCARGO=<cargo> -DMANIFEST=<pc-tool/Cargo.toml> -P studio.cmake
# rustup's cargo runs rustc through the PATH: add cargo's folder.
cmake_path(GET CARGO PARENT_PATH cargo_dir)
if(cargo_dir)
    cmake_path(CONVERT "$ENV{PATH}" TO_CMAKE_PATH_LIST path NORMALIZE)
    list(APPEND path ${cargo_dir})
    cmake_path(CONVERT "${path}" TO_NATIVE_PATH_LIST path)
    set(ENV{PATH} "${path}")
endif()
set(args)
if(NOT "$ENV{LUABOARD_PORT}" STREQUAL "")
    list(APPEND args --port "$ENV{LUABOARD_PORT}")
endif()
separate_arguments(files NATIVE_COMMAND "$ENV{STUDIO_ARGS}")
execute_process(
    COMMAND ${CARGO} run --release --quiet --manifest-path ${MANIFEST} --bin luaboard-studio -- ${args} ${files}
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "studio: ${rc}")
endif()
