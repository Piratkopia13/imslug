# imslug tests (ctest).
#
# M0 suites: smoke + the C++11 public-header check.
# M1 suites: glyph generation, packing, metrics, CPU reference rasterizer
# (imslug_tests_*), plus an optional test-only build of Dear ImGui that pins
# metric parity against its stb loader.

# ---------------------------------------------------------------------------
# M0 smoke tests
# ---------------------------------------------------------------------------
add_executable(imslug_tests test_imslug.cpp)
target_link_libraries(imslug_tests PRIVATE imslug)
set_target_properties(imslug_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug_tests)
add_test(NAME imslug_smoke COMMAND imslug_tests)

# The public header must keep compiling as C++11 (AGENTS.md invariant 4), so it
# gets its own translation unit built with the older standard on purpose.
add_executable(imslug_header_cxx11 test_header_cxx11.cpp)
target_link_libraries(imslug_header_cxx11 PRIVATE imslug)
set_target_properties(imslug_header_cxx11 PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug_header_cxx11)
add_test(NAME imslug_header_cxx11 COMMAND imslug_header_cxx11)

# ---------------------------------------------------------------------------
# M6: C API. The ABI gate is a C11 translation unit compiled with strict
# warnings (the header must be C11-clean and the struct layouts must match
# tests/test_c_api.cpp); the behavioral mirror test compiles both headers in
# one C++17 TU.
# ---------------------------------------------------------------------------
add_executable(imslug_tests_c_abi test_c_abi.c)
target_link_libraries(imslug_tests_c_abi PRIVATE imslug)
set_target_properties(imslug_tests_c_abi PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(imslug_tests_c_abi PRIVATE /W4 /WX)
else()
    target_compile_options(imslug_tests_c_abi PRIVATE -Wall -Wextra -Werror)
endif()
if(IMSLUG_HAVE_VULKAN AND IMSLUG_BACKEND_VULKAN)
    # Also compile the Vulkan declarations + the default init info check.
    target_compile_definitions(imslug_tests_c_abi PRIVATE IMSLUG_C_VULKAN)
    target_link_libraries(imslug_tests_c_abi PRIVATE imslug_vulkan)
endif()
add_test(NAME imslug_c_abi COMMAND imslug_tests_c_abi)

if(IMSLUG_TTF_PARSER)
    add_executable(imslug_tests_c_api test_c_api.cpp test_util.h)
    target_link_libraries(imslug_tests_c_api PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_c_api PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_c_api)
    target_compile_definitions(imslug_tests_c_api PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_c_api COMMAND imslug_tests_c_api)
endif()

# ---------------------------------------------------------------------------
# M7: packed-asset (`.imslug` container v1) tests.
#   * imslug_packed_loader: container validation, corrupt/truncated inputs,
#     packed metrics/records/raster - no TTF parser needed, so it also runs in
#     the no-parser configuration (IMSLUG_TTF_PARSER=OFF);
#   * imslug_packed_roundtrip: writer vs runtime TTF generation, bitwise
#     (parser + tools required);
#   * imslug_asset_fresh: the committed test asset matches the generator CLI
#     (parser + tools required).
# ---------------------------------------------------------------------------
set(IMSLUG_PACKED_TEST_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/Roboto-ASCII.imslug")

add_executable(imslug_tests_packed_loader test_packed_loader.cpp test_util.h)
target_link_libraries(imslug_tests_packed_loader PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
set_target_properties(imslug_tests_packed_loader PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug_tests_packed_loader)
target_compile_definitions(imslug_tests_packed_loader PRIVATE
    IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    IMSLUG_PACKED_TEST_ASSET="${IMSLUG_PACKED_TEST_ASSET}"
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
add_test(NAME imslug_packed_loader COMMAND imslug_tests_packed_loader)

if(IMSLUG_TTF_PARSER)
    add_executable(imslug_tests_packed_roundtrip test_packed_roundtrip.cpp test_util.h)
    target_link_libraries(imslug_tests_packed_roundtrip PRIVATE imslug imslug_gen_core ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_packed_roundtrip PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_packed_roundtrip)
    target_include_directories(imslug_tests_packed_roundtrip PRIVATE "${CMAKE_SOURCE_DIR}/tools/imslug_gen")
    target_compile_definitions(imslug_tests_packed_roundtrip PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_PACKED_TEST_ASSET="${IMSLUG_PACKED_TEST_ASSET}"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_packed_roundtrip COMMAND imslug_tests_packed_roundtrip)

    if(IMSLUG_BUILD_TOOLS)
        # CLI-level freshness check: run imslug_gen (font + info + --emit-header)
        # and byte-compare the regenerated asset with the committed one.
        add_test(NAME imslug_asset_fresh
            COMMAND ${CMAKE_COMMAND}
                -DIMSLUG_GEN=$<TARGET_FILE:imslug_gen>
                -DIMSLUG_FONT=${CMAKE_CURRENT_SOURCE_DIR}/fonts/Roboto-Regular.ttf
                -DIMSLUG_ASSET=${IMSLUG_PACKED_TEST_ASSET}
                -DIMSLUG_OUT_DIR=${CMAKE_BINARY_DIR}/asset_check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_asset_fresh.cmake)
    endif()
endif()

# ---------------------------------------------------------------------------
# CPU-only suites. Each target gets the test-font directory as a compile
# definition so the tests do not depend on the ctest working directory.
# ---------------------------------------------------------------------------
function(imslug_add_cpu_test target source test_name)
    add_executable(${target} ${source} test_util.h)
    target_link_libraries(${target} PRIVATE imslug)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(${target})
    target_compile_definitions(${target} PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

imslug_add_cpu_test(imslug_tests_packer test_packer.cpp imslug_packer)
# M2: the frozen glyph-instance record + placement mapping (no GPU needed).
imslug_add_cpu_test(imslug_tests_instance test_instance.cpp imslug_instance)
if(IMSLUG_TTF_PARSER)
    imslug_add_cpu_test(imslug_tests_glyphgen test_glyphgen.cpp imslug_glyphgen)
    imslug_add_cpu_test(imslug_tests_metrics test_metrics.cpp imslug_metrics)
    imslug_add_cpu_test(imslug_tests_refraster test_refraster.cpp imslug_refraster)
    # M5: COLR/CPAL reader, color-glyph cache and the CPU paint sampler. The emoji
    # font is a fetched, gitignored asset; the real-font checks skip when absent.
    add_executable(imslug_tests_colr test_colr.cpp test_util.h)
    target_link_libraries(imslug_tests_colr PRIVATE imslug)
    set_target_properties(imslug_tests_colr PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_colr)
    target_compile_definitions(imslug_tests_colr PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_EMOJI_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/emoji/NotoColorEmoji-Regular.ttf"
        # Local manual check only: a system COLR v1 font (never fetched/committed);
        # the check skips when the file is absent.
        IMSLUG_SEGOE_FONT="C:/Windows/Fonts/seguiemj.ttf"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_colr COMMAND imslug_tests_colr)
endif()
# ---------------------------------------------------------------------------
# M2 golden-image tests. Headless offscreen Vulkan (no surface/swapchain), so
# they run on lavapipe in CI; the comparison arbiter is the M1 CPU rasterizer,
# not a committed image. SKIP_RETURN_CODE 77 = no usable Vulkan device.
# ---------------------------------------------------------------------------
if(IMSLUG_HAVE_VULKAN)
    set(IMSLUG_GOLDEN_OUT_DIR "${CMAKE_BINARY_DIR}/golden")
    file(MAKE_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")

    add_executable(imslug_tests_golden golden/test_golden_vulkan.cpp test_util.h)
    target_link_libraries(imslug_tests_golden PRIVATE imslug imslug_vulkan)
    set_target_properties(imslug_tests_golden PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_golden)
    target_compile_definitions(imslug_tests_golden PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_PACKED_TEST_ASSET="${IMSLUG_PACKED_TEST_ASSET}"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

    add_test(NAME imslug_golden
        COMMAND imslug_tests_golden
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    add_test(NAME imslug_golden_dynamic
        COMMAND imslug_tests_golden --dynamic-rendering
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    # M7: the same matrix from the committed packed asset (no outline parsing).
    # This is the render proof for the no-parser configuration; in normal builds
    # it additionally pins packed-loader vs TTF-generator rendering.
    add_test(NAME imslug_golden_packed
        COMMAND imslug_tests_golden --packed "${IMSLUG_PACKED_TEST_ASSET}"
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    add_test(NAME imslug_golden_packed_dynamic
        COMMAND imslug_tests_golden --packed "${IMSLUG_PACKED_TEST_ASSET}" --dynamic-rendering
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    set_tests_properties(imslug_golden_packed imslug_golden_packed_dynamic PROPERTIES SKIP_RETURN_CODE 77)

    if(IMSLUG_TTF_PARSER)
        # M3: the same ImGui scene rendered with the Slug font and with the atlas
        # font, compared end to end (layout exact within 1 px, AA perceptual).
        add_executable(imslug_tests_imgui_golden golden/test_imgui_golden_vulkan.cpp test_util.h)
        target_link_libraries(imslug_tests_imgui_golden PRIVATE imslug imslug_vulkan)
        set_target_properties(imslug_tests_imgui_golden PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_tests_imgui_golden)
        target_compile_definitions(imslug_tests_imgui_golden PRIVATE
            IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
            IMSLUG_CJK_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/cjk/NotoSansSC-subset.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
        add_test(NAME imslug_imgui_golden
            COMMAND imslug_tests_imgui_golden
            WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
        add_test(NAME imslug_imgui_golden_dynamic
            COMMAND imslug_tests_imgui_golden --dynamic-rendering
            WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")

        # M5: color-font (COLR v1) goldens: emoji strings rendered as one instance
        # per layer and compared against the CPU arbiter (coverage + paint
        # sampling). The emoji font is a fetched, gitignored asset; the suite skips
        # (77) when it is absent.
        add_executable(imslug_tests_emoji_golden golden/test_emoji_golden_vulkan.cpp test_util.h)
        target_link_libraries(imslug_tests_emoji_golden PRIVATE imslug imslug_vulkan)
        set_target_properties(imslug_tests_emoji_golden PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_tests_emoji_golden)
        target_compile_definitions(imslug_tests_emoji_golden PRIVATE
            IMSLUG_EMOJI_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/emoji/NotoColorEmoji-Regular.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
        add_test(NAME imslug_emoji_golden
            COMMAND imslug_tests_emoji_golden
            WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
        add_test(NAME imslug_emoji_golden_dynamic
            COMMAND imslug_tests_emoji_golden --dynamic-rendering
            WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
        set_tests_properties(imslug_imgui_golden imslug_imgui_golden_dynamic imslug_emoji_golden
            imslug_emoji_golden_dynamic PROPERTIES SKIP_RETURN_CODE 77)

        # M4: the perf harness (scripts/bench.*). Not a ctest suite: it prints and
        # records machine-specific numbers.
        add_executable(imslug_tests_bench bench/bench_main.cpp test_util.h)
        target_link_libraries(imslug_tests_bench PRIVATE imslug imslug_vulkan)
        set_target_properties(imslug_tests_bench PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_tests_bench)
        target_include_directories(imslug_tests_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_compile_definitions(imslug_tests_bench PRIVATE
            IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
            IMSLUG_CJK_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/cjk/NotoSansSC-subset.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    endif()

    set_tests_properties(imslug_golden imslug_golden_dynamic PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "imslug tests: golden-image tests skipped (Vulkan not found)")
endif()

    # ---------------------------------------------------------------------------
    # M3/M4: TTF-parser-dependent suites (ImGui integration, fuzz, stress, bench
    # tools). The no-parser configuration (IMSLUG_TTF_PARSER=OFF) builds only the
    # packed-asset tests above.
    # ---------------------------------------------------------------------------
    if(IMSLUG_TTF_PARSER)
        # ---------------------------------------------------------------------------
        # M3: ImGui-facing tests (loader, handle encoding, pre-pass). Dear ImGui is
        # linked from the shared target; it is compiled with third-party settings.
        # ---------------------------------------------------------------------------
        add_executable(imslug_tests_imgui test_imslug_imgui.cpp test_util.h)
    target_link_libraries(imslug_tests_imgui PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_imgui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_imgui)
    target_compile_definitions(imslug_tests_imgui PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_EMOJI_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/emoji/NotoColorEmoji-Regular.ttf"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_imgui COMMAND imslug_tests_imgui)

    add_executable(imslug_tests_imgui_parity test_imguiparity.cpp test_util.h)
    target_link_libraries(imslug_tests_imgui_parity PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_imgui_parity PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_imgui_parity)
    target_compile_definitions(imslug_tests_imgui_parity PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_metrics_imgui_parity COMMAND imslug_tests_imgui_parity)

    # ---------------------------------------------------------------------------
    # M4: fine-clip fuzz harness (deterministic; no GPU). Runs the same case
    # sequence through the reconstruction path and the R3 fallback.
    # ---------------------------------------------------------------------------
    add_executable(imslug_tests_fuzz test_fuzz.cpp test_util.h)
    target_link_libraries(imslug_tests_fuzz PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_fuzz PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_fuzz)
    target_compile_definitions(imslug_tests_fuzz PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_fuzz COMMAND imslug_tests_fuzz)

    # The CI fuzz profile is deliberately larger than a quick developer run;
    # IMSLUG_FUZZ_ITERATIONS overrides both ("continuous fuzz/stress" in CI). The
    # cases are tiny frames, so 100k cases per pass is ~1 s on a desktop CPU.
    set(IMSLUG_FUZZ_ITERATIONS "100000" CACHE STRING "Fuzz cases per pass for ctest")
    set_tests_properties(imslug_fuzz PROPERTIES
        ENVIRONMENT "IMSLUG_FUZZ_ITERATIONS=${IMSLUG_FUZZ_ITERATIONS}")

    # ---------------------------------------------------------------------------
    # M4: stress suite (5k glyphs, wrapped text, CJK, degenerate outlines, pool
    # limits, atlas repack). The CJK font is a fetched, gitignored asset
    # (scripts/fetch_cjk_font.*); the suite skips it when absent.
    # ---------------------------------------------------------------------------
    add_executable(imslug_tests_stress test_stress.cpp test_util.h)
    target_link_libraries(imslug_tests_stress PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_stress PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_stress)
    target_compile_definitions(imslug_tests_stress PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_CJK_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/cjk/NotoSansSC-subset.ttf"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_stress COMMAND imslug_tests_stress)

    # ---------------------------------------------------------------------------
    # M4: benchmark tools (not ctest suites). Band tuning sweep (CPU-only) and the
    # perf harness (GPU, headless Vulkan). scripts/bench.* build and run them.
    # ---------------------------------------------------------------------------
    add_executable(imslug_tests_band_sweep bench/band_sweep.cpp test_util.h)
    target_link_libraries(imslug_tests_band_sweep PRIVATE imslug ${IMSLUG_IMGUI_TARGET})
    set_target_properties(imslug_tests_band_sweep PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_band_sweep)
    target_include_directories(imslug_tests_band_sweep PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(imslug_tests_band_sweep PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        IMSLUG_CJK_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fonts/cjk/NotoSansSC-subset.ttf"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

endif()
