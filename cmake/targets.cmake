# Developer targets (see the top of CMakeLists.txt): the simulator's window,
# the Rust tools, tests and checks, and the device firmware through ESP-IDF.

# Homebrew's rustup is keg-only; find cargo without touching the user's PATH.
# rustup's cargo runs rustc through the PATH, so its folder goes on the PATH
# for the commands that run it.
find_program(CARGO_EXE cargo HINTS $ENV{HOME}/.cargo/bin $ENV{USERPROFILE}/.cargo/bin /opt/homebrew/opt/rustup/bin)
if(CARGO_EXE)
    cmake_path(GET CARGO_EXE PARENT_PATH CARGO_DIR)
    set(CARGO ${CMAKE_COMMAND} -E env --modify PATH=path_list_append:${CARGO_DIR} ${CARGO_EXE})
else()
    set(CARGO_EXE cargo)
    set(CARGO cargo)
endif()
set(PC_TOOL ${ROOT}/pc-tool)
set(CLI ${PC_TOOL}/target/release/luaboard${CMAKE_EXECUTABLE_SUFFIX})
set(SIM $<TARGET_FILE:esp32-sim>)
# Where `run` keeps the simulator's NVS, apps and app slots.
set(STATE ${SIM_DIR})

# The options of run, studio, apps and flash are environment variables
# read when the target runs, so they hold for one command, as make's did:
# MODEL, BLE_DEVICE, WATCH (run), STUDIO_ARGS (studio) and LUABOARD_PORT
# (studio, apps, flash; the CLI's own). From the repository root, so
# paths in them resolve as typed.
#
# run builds its simulator in a build folder of its own (build/run): its
# rebuilds on every save then never replace the simulator that check, e2e
# and the pre-push hook test, and their builds never replace the one in the
# window. Its apps and settings stay where they were (STATE).
set(RUN_BUILD ${CMAKE_BINARY_DIR}/run)
set(RUN_SIM ${RUN_BUILD}/${BOARD}/esp32-sim${CMAKE_EXECUTABLE_SUFFIX})
set(RUN_GEN -G ${CMAKE_GENERATOR})
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND RUN_GEN -A ${CMAKE_GENERATOR_PLATFORM})
endif()
add_custom_target(run
    COMMAND ${CMAKE_COMMAND} -S ${ROOT} -B ${RUN_BUILD} ${RUN_GEN} -DCMAKE_BUILD_TYPE=Release -DBOARD=${BOARD}
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DLUABOARD_FETCH_DEPS=${LUABOARD_FETCH_DEPS}
    COMMAND ${CMAKE_COMMAND} --build ${RUN_BUILD} --config $<CONFIG> --target esp32-sim
    COMMAND ${CMAKE_COMMAND} -DSRC=${ROOT} -DBUILD_DIR=${RUN_BUILD} -DCONFIG=$<CONFIG> -DSIM=${RUN_SIM}
            -DSTATE=${STATE} -P ${ROOT}/tools/run.cmake
    WORKING_DIRECTORY ${ROOT} USES_TERMINAL VERBATIM)

add_custom_target(tool
    COMMAND ${CARGO} build --release
    WORKING_DIRECTORY ${PC_TOOL} USES_TERMINAL VERBATIM)

add_custom_target(studio
    COMMAND ${CMAKE_COMMAND} -DCARGO=${CARGO_EXE} -DMANIFEST=${PC_TOOL}/Cargo.toml -P ${ROOT}/tools/studio.cmake
    WORKING_DIRECTORY ${ROOT} USES_TERMINAL VERBATIM)

# Installing over an app replaces its files and keeps its data (logins, settings).
add_custom_target(apps
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -P ${ROOT}/tools/need_marketplace.cmake
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -DCLI=${CLI} -P ${ROOT}/tools/install_apps.cmake
    DEPENDS tool
    USES_TERMINAL VERBATIM)

# The release script's round trip: a manifest made from a CHANGELOG fixture
# must parse in the firmware's parser (fw-unit --manifest), and its notes.md
# must have its relative links made absolute. The marketplace's limits and
# index are held to the firmware's too (fw-unit --marketplace).
set(REL_OUT ${CMAKE_BINARY_DIR}/fw-unit-release)
add_custom_target(unit
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -P ${ROOT}/tools/need_marketplace.cmake
    COMMAND ${CARGO} test --release --quiet --lib --bins
    COMMAND $<TARGET_FILE:fw-unit>
    COMMAND $<TARGET_FILE:fw-unit> --marketplace ${ROOT}/marketplace/tools/board.json ${ROOT}/marketplace/index.json
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${REL_OUT}
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -DTAG=v1.0.0 -DBOARD=m5stack-cores3 -DOUT=${REL_OUT}
            -DCHANGELOG=${ROOT}/app/test/CHANGELOG.fixture.md -DIMAGE=${ROOT}/app/test/CHANGELOG.fixture.md
            -P ${ROOT}/tools/release.cmake
    COMMAND $<TARGET_FILE:fw-unit> --manifest ${REL_OUT}/manifest-m5stack-cores3.json ${ROOT}/app/test/CHANGELOG.fixture.md
            ${REL_OUT}/notes.md
    DEPENDS fw-unit
    WORKING_DIRECTORY ${PC_TOOL} USES_TERMINAL VERBATIM)

# The end-to-end suite is a Rust test of the PC tools (pc-tool/tests/e2e),
# its fake servers included: nothing else to install. E2E_ONLY="oauth menu"
# runs only the sections whose name contains a word.
add_custom_target(e2e
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -P ${ROOT}/tools/need_marketplace.cmake
    COMMAND ${CMAKE_COMMAND} -E env SIM=${SIM} ${CARGO} test --release --test e2e
    DEPENDS esp32-sim
    WORKING_DIRECTORY ${PC_TOOL} USES_TERMINAL VERBATIM)

add_custom_target(lint
    COMMAND ${CARGO} fmt --check
    COMMAND ${CARGO} clippy --release --all-targets -- -D warnings
    WORKING_DIRECTORY ${PC_TOOL} USES_TERMINAL VERBATIM)

# The simulator with warnings as errors, in a build folder of its own.
add_custom_target(werror
    # Release, as CI builds it: some warnings only show up with optimisation.
    COMMAND ${CMAKE_COMMAND} -S ${ROOT} -B ${CMAKE_BINARY_DIR}/werror
            -DCMAKE_BUILD_TYPE=Release -DLUABOARD_WERROR=ON
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/werror --config $<CONFIG> --target esp32-sim
    USES_TERMINAL VERBATIM)

# What CI checks; `git push` runs it too after `cmake --build build --target hooks`.
# Without marketplace/ (a clone without --recurse-submodules) it stops at once
# with that one line, instead of after building everything else. The glob is
# checked again at every build, so checking the submodule out is noticed.
file(GLOB MARKETPLACE_HERE CONFIGURE_DEPENDS ${ROOT}/marketplace/hello/app.json)
if(MARKETPLACE_HERE)
    add_custom_target(check DEPENDS lint werror unit e2e)
else()
    add_custom_target(check COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -P ${ROOT}/tools/need_marketplace.cmake VERBATIM)
endif()

add_custom_target(hooks
    COMMAND git config core.hooksPath .githooks
    COMMAND ${CMAKE_COMMAND} -E echo "pre-push hook on: git push runs the check target (and fw when the firmware changed)"
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT} -P ${ROOT}/tools/marketplace_hook.cmake
    WORKING_DIRECTORY ${ROOT} VERBATIM)

# ---- the device: ESP-IDF through Espressif's installer `eim` -------------
# `eim install -i v6.0.3 -t esp32s3` once (or the toolchain target), then fw.
# Besides the normal build output, fw writes one merged image (bootloader +
# partition table + app, flashed at 0) for `luaboard flash` and the studio.
set(IDF_VERSION v6.0.3 CACHE STRING "ESP-IDF version for fw / flash")
find_program(EIM eim)
set(TOOLCHAIN_DIR ${ROOT}/.toolchain)
set(FW_IMAGE ${CMAKE_BINARY_DIR}/luaboard-${BOARD}.bin)
set(IDF_BUILD ${CMAKE_BINARY_DIR}/idf-${BOARD})
# ESP-IDF supports Python 3.10-3.13; newer ones fail eim's sanity check. The
# newest Homebrew python@3.1x in that range goes first on the PATH for eim.
file(GLOB IDF_PY_BINS /opt/homebrew/opt/python@3.1[0-3]/libexec/bin)
set(IDF_ENV ${CMAKE_COMMAND} -E env)
if(IDF_PY_BINS)
    list(SORT IDF_PY_BINS COMPARE NATURAL ORDER DESCENDING)
    list(GET IDF_PY_BINS 0 IDF_PY_BIN)
    list(APPEND IDF_ENV --modify PATH=path_list_prepend:${IDF_PY_BIN})
endif()
if(EIM)
    add_custom_target(toolchain
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TOOLCHAIN_DIR}
        # eim saves an eim_config.toml in the current folder: keep it in .toolchain
        COMMAND ${CMAKE_COMMAND} -E chdir ${TOOLCHAIN_DIR}
                ${IDF_ENV} ${EIM} install -i ${IDF_VERSION} -t esp32s3 -p ${TOOLCHAIN_DIR}
        USES_TERMINAL VERBATIM)
    add_custom_target(fw
        COMMAND ${IDF_ENV} BOARD=${BOARD}
                ${EIM} run "idf.py -B ${IDF_BUILD} build merge-bin -o ${FW_IMAGE}" ${IDF_VERSION}
        COMMAND ${CMAKE_COMMAND} -E echo "firmware image: ${FW_IMAGE}"
        WORKING_DIRECTORY ${ROOT}/app/idf USES_TERMINAL VERBATIM)
    add_custom_target(flash
        COMMAND ${IDF_ENV} ${CMAKE_COMMAND} -DEIM=${EIM} -DIDF_BUILD=${IDF_BUILD} -DIDF_VERSION=${IDF_VERSION}
                -DBOARD=${BOARD} -DFW_IMAGE=${FW_IMAGE} -P ${ROOT}/tools/flash.cmake
        WORKING_DIRECTORY ${ROOT}/app/idf USES_TERMINAL VERBATIM)
else()
    foreach(t toolchain fw flash)
        add_custom_target(${t}
            COMMAND ${CMAKE_COMMAND} -E echo "${t}: needs Espressif's installer eim on the PATH (https://docs.espressif.com/projects/idf-im-ui/)"
            COMMAND ${CMAKE_COMMAND} -E false VERBATIM)
    endforeach()
endif()
