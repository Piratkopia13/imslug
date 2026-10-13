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
# M1 suites. Each target gets the test-font directory as a compile definition
# so the tests do not depend on the ctest working directory.
# ---------------------------------------------------------------------------
function(imslug_add_m1_test target source test_name)
    add_executable(${target} ${source} test_util.h)
    target_link_libraries(${target} PRIVATE imslug)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(${target})
    target_compile_definitions(${target} PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

imslug_add_m1_test(imslug_tests_glyphgen test_glyphgen.cpp imslug_glyphgen)
imslug_add_m1_test(imslug_tests_packer test_packer.cpp imslug_packer)
imslug_add_m1_test(imslug_tests_metrics test_metrics.cpp imslug_metrics)
imslug_add_m1_test(imslug_tests_refraster test_refraster.cpp imslug_refraster)
# M2: the frozen glyph-instance record + placement mapping (no GPU needed).
imslug_add_m1_test(imslug_tests_instance test_instance.cpp imslug_instance)

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
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

    add_test(NAME imslug_golden
        COMMAND imslug_tests_golden
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    add_test(NAME imslug_golden_dynamic
        COMMAND imslug_tests_golden --dynamic-rendering
        WORKING_DIRECTORY "${IMSLUG_GOLDEN_OUT_DIR}")
    set_tests_properties(imslug_golden imslug_golden_dynamic PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "imslug tests: golden-image tests skipped (Vulkan not found)")
endif()

# ---------------------------------------------------------------------------
# ImGui metric parity (M1 brief: "vs ImGui's loader in a test-only build").
# Dear ImGui is compiled here only — never into the library or the backend.
# Skipped when ImGui came from find_package (no source tree to compile).
# ---------------------------------------------------------------------------
if(IMSLUG_IMGUI_SOURCE_DIR)
    add_library(imslug_imgui_testlib STATIC
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_widgets.cpp")
    set_target_properties(imslug_imgui_testlib PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(imslug_imgui_testlib PUBLIC "${IMSLUG_IMGUI_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(imslug_imgui_testlib PRIVATE /w) # third-party: no warnings
    else()
        target_compile_options(imslug_imgui_testlib PRIVATE -w)
    endif()

    add_executable(imslug_tests_imgui_parity test_imguiparity.cpp test_util.h)
    target_link_libraries(imslug_tests_imgui_parity PRIVATE imslug imslug_imgui_testlib)
    set_target_properties(imslug_tests_imgui_parity PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_tests_imgui_parity)
    target_compile_definitions(imslug_tests_imgui_parity PRIVATE
        IMSLUG_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    add_test(NAME imslug_metrics_imgui_parity COMMAND imslug_tests_imgui_parity)
else()
    message(STATUS "imslug tests: ImGui metric-parity test skipped (no ImGui source tree)")
endif()
