# Turn on the marketplace's pre-push hook in examples/ (the submodule):
# pushing from there runs the check target. Only when examples/ is checked
# out: in an empty examples/ folder `git -C examples config` would change the
# main repository's own hooksPath instead.
#
#   cmake -DROOT=<repo> -P marketplace_hook.cmake
execute_process(COMMAND git -C ${ROOT}/examples rev-parse --show-superproject-working-tree
    OUTPUT_VARIABLE super OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(super STREQUAL "")
    message("examples/ isn't checked out (git submodule update --init), so its pre-push hook is off")
else()
    execute_process(COMMAND git -C ${ROOT}/examples config core.hooksPath ../.githooks/marketplace
        COMMAND_ERROR_IS_FATAL ANY)
    message("and in examples/ (the marketplace) too: git push there runs the check target")
endif()
