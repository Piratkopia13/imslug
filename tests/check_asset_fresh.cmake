# imslug — CLI-level freshness check for the committed packed test asset.
#
# Invoked by ctest as `imslug_asset_fresh` with:
#   -DIMSLUG_GEN=<imslug_gen executable>
#   -DIMSLUG_FONT=<tests/fonts/Roboto-Regular.ttf>
#   -DIMSLUG_ASSET=<tests/assets/Roboto-ASCII.imslug>
#   -DIMSLUG_OUT_DIR=<build>/asset_check
#
# It regenerates the asset through the CLI with the exact documented parameters,
# byte-compares it with the committed file (a stale asset fails here - run
# scripts/gen_asset.* and commit the result), and smoke-runs `info`, `--help`
# and `--emit-header` (including an emitter-determinism re-run).

foreach(required IMSLUG_GEN IMSLUG_FONT IMSLUG_ASSET IMSLUG_OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check_asset_fresh.cmake: ${required} is not set")
    endif()
endforeach()

file(MAKE_DIRECTORY "${IMSLUG_OUT_DIR}")
set(OUT "${IMSLUG_OUT_DIR}/Roboto-ASCII.imslug")
set(HEADER "${IMSLUG_OUT_DIR}/Roboto-ASCII.h")

# --help is a smoke check of the CLI surface.
execute_process(
    COMMAND "${IMSLUG_GEN}" --help
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "usage:")
    message(FATAL_ERROR "imslug_gen --help failed (${result}): ${error}")
endif()

# Regenerate the committed asset.
execute_process(
    COMMAND "${IMSLUG_GEN}" font "${IMSLUG_FONT}" --size-range 8:72 --chars 0x20-0x7E -o "${OUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "imslug_gen font failed (${result}): ${error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}" "${IMSLUG_ASSET}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "the committed packed test asset is stale (${IMSLUG_ASSET}); regenerate it with scripts/gen_asset.* and commit")
endif()

# `info` validates and prints the summary.
execute_process(
    COMMAND "${IMSLUG_GEN}" info "${IMSLUG_ASSET}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "imslug_gen info failed (${result}): ${error}")
endif()
if(NOT output MATCHES "container v1" OR NOT output MATCHES "Roboto-Regular.ttf")
    message(FATAL_ERROR "imslug_gen info output unexpected: ${output}")
endif()

# --emit-header must produce a compilable header with the requested symbol, and
# be deterministic across runs.
foreach(run 1 2)
    execute_process(
        COMMAND "${IMSLUG_GEN}" font "${IMSLUG_FONT}" --size-range 8:72 --chars 0x20-0x7E -o "${OUT}.${run}"
                --emit-header "${HEADER}.${run}" --header-symbol imslug_asset_test
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "imslug_gen --emit-header run ${run} failed (${result}): ${error}")
    endif()
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${HEADER}.1" "${HEADER}.2"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "imslug_gen --emit-header output is not deterministic")
endif()
file(READ "${HEADER}.1" header_content)
if(NOT header_content MATCHES "imslug_asset_test_data" OR NOT header_content MATCHES "imslug_asset_test_size")
    message(FATAL_ERROR "emitted header misses the requested symbol")
endif()

message(STATUS "imslug_asset_fresh: OK")
