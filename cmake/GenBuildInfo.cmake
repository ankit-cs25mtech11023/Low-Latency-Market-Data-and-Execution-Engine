# Runs at BUILD time (not configure time) via the lle_build_info target, so the git SHA baked
# into every binary is the commit it was actually built from. Configure-time generation would
# keep reporting the commit that was checked out when `cmake --preset` last ran.
# Inputs (-D): SRC_DIR, TEMPLATE, OUT, CMAKE_BUILD_TYPE, CMAKE_CXX_COMPILER_ID,
#              CMAKE_CXX_COMPILER_VERSION, LLE_BUILD_FLAGS, LLE_SANITIZER
execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE LLE_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# Uncommitted changes to tracked files make the SHA an incomplete description of the source.
execute_process(COMMAND git diff --quiet HEAD -- WORKING_DIRECTORY ${SRC_DIR} RESULT_VARIABLE _dirty ERROR_QUIET)
if(_dirty EQUAL 0)
    set(LLE_GIT_DIRTY "false")
else()
    set(LLE_GIT_DIRTY "true")
endif()
configure_file(${TEMPLATE} ${OUT}.tmp @ONLY)
# Only touch the real header when something changed, so unchanged builds do not recompile.
file(COPY_FILE ${OUT}.tmp ${OUT} ONLY_IF_DIFFERENT)
