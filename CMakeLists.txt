# imslug — Slug-based GPU text rendering for Dear ImGui.
#
# Entry points for humans, CI and agents are scripts/build.* and scripts/test.*
# (see AGENTS.md); do not bypass them with ad-hoc cmake invocations.
#
# Style contract: public headers compile as C++11 and contain no STL;
# our implementation is C++17 without exceptions; third-party targets keep
# their own settings.

cmake_minimum_required(VERSION 3.21)

project(imslug
    VERSION 0.1.0
    DESCRIPTION "Slug-based GPU text rendering for Dear ImGui"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(IMSLUG_BACKEND_VULKAN "Build the Vulkan backend module (standalone renderer core; reused by the ImGui adapter)" ON)
option(IMSLUG_BUILD_TESTS    "Build unit tests and enable ctest" ON)
option(IMSLUG_BUILD_EXAMPLES "Build the example applications" OFF)
option(IMSLUG_BUILD_C_EXAMPLE "Build the C example (example/c: CPU smoke always, headless Vulkan frame when Vulkan is found)" ON)
option(IMSLUG_BUILD_TOOLS    "Build the offline tools (imslug_gen)" ON)
option(IMSLUG_TTF_PARSER     "Build the runtime TTF outline parser (stb_truetype; OFF = packed .imslug assets only)" ON)

set(IMSLUG_IMGUI_DIR "" CACHE PATH
    "Use an existing Dear ImGui checkout instead of fetching it (-DIMSLUG_IMGUI_DIR=...)")
set(IMSLUG_IMGUI_TAG "v1.92.9b" CACHE STRING
    "Dear ImGui release tag fetched with FetchContent")

set(IMSLUG_GLFW_DIR "" CACHE PATH
    "Use an existing GLFW checkout instead of fetching it (-DIMSLUG_GLFW_DIR=...)")
set(IMSLUG_GLFW_TAG "3.4" CACHE STRING
    "GLFW release tag fetched with FetchContent")

enable_testing()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
# The MSVC toolchain injects /EHsc into CMAKE_CXX_FLAGS by default. Drop it:
# our targets use /EHs-c- (no exceptions in our code, AGENTS.md invariant 4) and
# emit a D9025 "overriding /EHs" warning for every file otherwise. A future
# third-party target that needs exception handling re-adds /EHsc explicitly.
if(MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Warnings plus the "no exceptions in our code" rule. Applies to imslug targets
# only, never to third-party code.
function(imslug_target_defaults target)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHs-c-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Dear ImGui (pinned dependency)
#
# Resolution order:
#   1. -DIMSLUG_IMGUI_DIR=<dir> for a local checkout (offline builds),
#   2. find_package(imgui) exposing a target named imgui::imgui or imgui,
#   3. FetchContent at the pinned tag IMSLUG_IMGUI_TAG (default v1.92.9b).
#
# Upstream Dear ImGui ships no CMakeLists.txt, so FetchContent only populates
# the source tree; we compile the subset we need (imgui.cpp, imgui_draw.cpp,
# imgui_tables.cpp, imgui_widgets.cpp) into `imslug_imgui_lib` when we own the
# sources. The imslug core needs Dear ImGui (imslug.h includes
# imgui.h and the pre-pass walks ImDrawData), so a checkout is mandatory.
# ---------------------------------------------------------------------------
set(IMSLUG_IMGUI_SOURCE_DIR "")
set(IMSLUG_IMGUI_LINK_TARGET "")

if(IMSLUG_IMGUI_DIR)
    if(NOT EXISTS "${IMSLUG_IMGUI_DIR}/imgui.h")
        message(FATAL_ERROR
            "IMSLUG_IMGUI_DIR='${IMSLUG_IMGUI_DIR}' does not look like a Dear ImGui checkout (imgui.h not found)")
    endif()
    set(IMSLUG_IMGUI_SOURCE_DIR "${IMSLUG_IMGUI_DIR}")
    message(STATUS "imslug: using Dear ImGui from IMSLUG_IMGUI_DIR=${IMSLUG_IMGUI_DIR}")
else()
    find_package(imgui QUIET CONFIG)
    if(TARGET imgui::imgui)
        set(IMSLUG_IMGUI_LINK_TARGET imgui::imgui)
    elseif(TARGET imgui)
        set(IMSLUG_IMGUI_LINK_TARGET imgui)
    endif()

    if(IMSLUG_IMGUI_LINK_TARGET)
        message(STATUS "imslug: using Dear ImGui via find_package(imgui) -> ${IMSLUG_IMGUI_LINK_TARGET}")
    else()
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        ${IMSLUG_IMGUI_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(imgui)
        set(IMSLUG_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")
        message(STATUS "imslug: fetched Dear ImGui ${IMSLUG_IMGUI_TAG} -> ${IMSLUG_IMGUI_SOURCE_DIR}")
    endif()
endif()

# Dear ImGui as a link target: the provided package target when available, or
# our own target compiled from the checkout with third-party warning settings.
# IMSLUG_IMGUI_TARGET is what every imslug target links.
set(IMSLUG_IMGUI_TARGET "")
if(IMSLUG_IMGUI_LINK_TARGET)
    set(IMSLUG_IMGUI_TARGET ${IMSLUG_IMGUI_LINK_TARGET})
elseif(IMSLUG_IMGUI_SOURCE_DIR)
    add_library(imslug_imgui STATIC
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_widgets.cpp")
    set_target_properties(imslug_imgui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(imslug_imgui SYSTEM PUBLIC
        "${IMSLUG_IMGUI_SOURCE_DIR}"
        "${IMSLUG_IMGUI_SOURCE_DIR}/backends"
        "${IMSLUG_IMGUI_SOURCE_DIR}/misc/cpp")
    if(MSVC)
        target_compile_options(imslug_imgui PRIVATE /w)  # third-party: no warnings
    else()
        target_compile_options(imslug_imgui PRIVATE -w)
    endif()
    set(IMSLUG_IMGUI_TARGET imslug_imgui)
else()
    message(FATAL_ERROR
        "imslug: Dear ImGui sources not found. Set IMSLUG_IMGUI_DIR=<checkout>, provide a find_package(imgui) "
        "target, or allow FetchContent (network). See CMakeLists.txt.")
endif()

# Dear ImGui renderer/platform backends are built inside the examples section
# below (they need GLFW and Vulkan headers and are only used there).

# ---------------------------------------------------------------------------
# stb (vendored, third-party) — its own target so our warning settings never
# apply to third-party code (AGENTS.md invariant 2). The no-parser
# build (IMSLUG_TTF_PARSER=OFF) excludes it entirely.
# ---------------------------------------------------------------------------
if(IMSLUG_TTF_PARSER)
    add_library(imslug_stb STATIC imslug_stb.cpp)
    target_include_directories(imslug_stb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
    if(MSVC)
        target_compile_options(imslug_stb PRIVATE /w)
    else()
        target_compile_options(imslug_stb PRIVATE -w)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library (includes the ImGui-facing core + adapter; the backend
# modules stay in backends/ — AGENTS.md invariants 4 and 7)
#
# imslug_font.cpp (font-kind dispatch + shared record builder) and
# imslug_packed.cpp (the `.imslug` container v1 loader) are always compiled;
# imslug_glyphgen.cpp (the stb_truetype outline path) only with the parser.
# ---------------------------------------------------------------------------
set(IMSLUG_CORE_SOURCES
    imslug.cpp
    imslug.h
    imslug_c.cpp
    imslug_c.h
    imslug_internal.h
    imslug_font.h
    imslug_font.cpp
    imslug_packed.h
    imslug_packed.cpp
    imslug_glyphgen.h
    imslug_packer.h
    imslug_packer.cpp
    imslug_refraster.h
    imslug_refraster.cpp
    imslug_colr.h
    imslug_colr.cpp
    imslug_imgui_adapter.h
    imslug_imgui_adapter.cpp)
if(IMSLUG_TTF_PARSER)
    list(APPEND IMSLUG_CORE_SOURCES imslug_glyphgen.cpp)
endif()

add_library(imslug STATIC ${IMSLUG_CORE_SOURCES})
set_target_properties(imslug PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug)
target_include_directories(imslug PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb>"
    "$<INSTALL_INTERFACE:include>")
if(IMSLUG_TTF_PARSER)
    target_link_libraries(imslug PRIVATE imslug_stb)
else()
    target_compile_definitions(imslug PUBLIC IMSLUG_NO_STBTT=1)
    message(STATUS "imslug: TTF parser disabled (IMSLUG_TTF_PARSER=OFF) - packed .imslug assets only")
endif()
target_link_libraries(imslug PUBLIC ${IMSLUG_IMGUI_TARGET})

# ---------------------------------------------------------------------------
# Vulkan (backend module, golden tests, example)
# ---------------------------------------------------------------------------
find_package(Vulkan QUIET)
set(IMSLUG_HAVE_VULKAN OFF)
if(Vulkan_FOUND)
    set(IMSLUG_HAVE_VULKAN ON)
    message(STATUS "imslug: Vulkan found - backend module, golden tests and example enabled")
else()
    message(STATUS "imslug: Vulkan not found - backend module, golden tests and example disabled")
endif()

# ---------------------------------------------------------------------------
# Optional targets — created only when the corresponding option is ON.
# ---------------------------------------------------------------------------
if(IMSLUG_BACKEND_VULKAN)
    if(IMSLUG_HAVE_VULKAN)
        # Dear ImGui's Vulkan renderer backend (third-party): the ImGui adapter
        # in imslug_vulkan calls ImGui_ImplVulkan_RenderDrawData, so it is linked
        # together with the module (and reused by the examples).
        add_library(imslug_imgui_vulkan_backend STATIC
            "${IMSLUG_IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
        set_target_properties(imslug_imgui_vulkan_backend PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_link_libraries(imslug_imgui_vulkan_backend PUBLIC ${IMSLUG_IMGUI_TARGET})
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(imslug_imgui_vulkan_backend PUBLIC Vulkan::Vulkan)
        else()
            target_include_directories(imslug_imgui_vulkan_backend PUBLIC ${Vulkan_INCLUDE_DIRS})
        endif()
        if(MSVC)
            target_compile_options(imslug_imgui_vulkan_backend PRIVATE /w)
        else()
            target_compile_options(imslug_imgui_vulkan_backend PRIVATE -w)
        endif()

        # Standalone Vulkan rendering core + ImGui adapter + the C
        # Vulkan mirror: reused by the examples, the golden harness and
        # the integration tests.
        add_library(imslug_vulkan STATIC
            backends/imslug_impl_vulkan.cpp
            backends/imslug_impl_vulkan.h
            backends/imslug_c_vulkan.cpp)
        set_target_properties(imslug_vulkan PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_vulkan)
        target_include_directories(imslug_vulkan PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends>"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
        target_link_libraries(imslug_vulkan PUBLIC imslug imslug_imgui_vulkan_backend)
    else()
        add_library(imslug_vulkan INTERFACE)
        target_link_libraries(imslug_vulkan INTERFACE imslug)
        message(STATUS "imslug: imslug_vulkan placeholder target (Vulkan not found)")
    endif()
endif()

if(IMSLUG_BUILD_EXAMPLES)
    if(NOT IMSLUG_HAVE_VULKAN)
        message(STATUS "imslug: examples need Vulkan - skipped")
    else()
        # GLFW: local checkout (IMSLUG_GLFW_DIR), an existing glfw target, or
        # FetchContent at the pinned tag. Read-only third-party tree either way.
        if(IMSLUG_GLFW_DIR)
            if(NOT EXISTS "${IMSLUG_GLFW_DIR}/include/GLFW/glfw3.h")
                message(FATAL_ERROR "IMSLUG_GLFW_DIR='${IMSLUG_GLFW_DIR}' does not look like a GLFW checkout")
            endif()
            add_subdirectory("${IMSLUG_GLFW_DIR}" "${CMAKE_BINARY_DIR}/_deps/glfw-local" EXCLUDE_FROM_ALL)
        elseif(NOT TARGET glfw)
            include(FetchContent)
            set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
            set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(glfw
                GIT_REPOSITORY https://github.com/glfw/glfw.git
                GIT_TAG        ${IMSLUG_GLFW_TAG}
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(glfw)
        endif()

        # Dear ImGui platform backend (third-party; examples only). The Vulkan
        # renderer backend is built globally (see below): the ImGui adapter
        # in imslug_vulkan calls into it.
        add_library(imslug_imgui_backends STATIC
            "${IMSLUG_IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
        set_target_properties(imslug_imgui_backends PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_link_libraries(imslug_imgui_backends PUBLIC ${IMSLUG_IMGUI_TARGET} glfw)
        if(MSVC)
            target_compile_options(imslug_imgui_backends PRIVATE /w)
        else()
            target_compile_options(imslug_imgui_backends PRIVATE -w)
        endif()

        # Dear ImGui's demo window lives in its own TU (third-party settings).
        add_library(imslug_imgui_demo STATIC "${IMSLUG_IMGUI_SOURCE_DIR}/imgui_demo.cpp")
        set_target_properties(imslug_imgui_demo PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_link_libraries(imslug_imgui_demo PUBLIC ${IMSLUG_IMGUI_TARGET})
        if(MSVC)
            target_compile_options(imslug_imgui_demo PRIVATE /w)
        else()
            target_compile_options(imslug_imgui_demo PRIVATE -w)
        endif()

        add_executable(imslug_example_glfw_vulkan example/glfw_vulkan/main.cpp)
        set_target_properties(imslug_example_glfw_vulkan PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_example_glfw_vulkan)
        target_link_libraries(imslug_example_glfw_vulkan PRIVATE imslug imslug_vulkan glfw imslug_imgui_backends imslug_imgui_demo)
        # The default Slug font is the TTF when the parser is built in and
        # the committed packed asset otherwise (the no-parser demo runs entirely
        # from baked data; the stock atlas font still uses ImGui's own parser).
        if(IMSLUG_TTF_PARSER)
            set(IMSLUG_EXAMPLE_FONT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/Roboto-Regular.ttf")
        else()
            set(IMSLUG_EXAMPLE_FONT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/Roboto-ASCII.imslug")
        endif()
        target_compile_definitions(imslug_example_glfw_vulkan PRIVATE
            IMSLUG_EXAMPLE_FONT_PATH="${IMSLUG_EXAMPLE_FONT_PATH}"
            IMSLUG_EXAMPLE_STOCK_FONT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/Roboto-Regular.ttf"
            IMSLUG_PACKED_TEST_ASSET="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/Roboto-ASCII.imslug"
            IMSLUG_CJK_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/cjk/NotoSansSC-subset.ttf"
            IMSLUG_EMOJI_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/emoji/NotoColorEmoji-Regular.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    endif()
endif()

# ---------------------------------------------------------------------------
# Offline tools: the `.imslug` asset writer + the imslug_gen CLI.
# The writer consumes the same runtime core code as the renderer (font handle,
# band construction, packer), so generated assets cannot diverge from runtime
# generation. It needs the TTF parser; a no-parser build has no generator.
# ---------------------------------------------------------------------------
if(IMSLUG_TTF_PARSER)
    # The writer is a small static library so tests (the round-trip suite) can
    # generate assets in-process; the CLI is a thin argument/file wrapper.
    add_library(imslug_gen_core STATIC
        tools/imslug_gen/imslug_gen_writer.h
        tools/imslug_gen/imslug_gen_writer.cpp)
    set_target_properties(imslug_gen_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    imslug_target_defaults(imslug_gen_core)
    target_link_libraries(imslug_gen_core PUBLIC imslug)

    if(IMSLUG_BUILD_TOOLS)
        add_executable(imslug_gen tools/imslug_gen/imslug_gen_main.cpp)
        set_target_properties(imslug_gen PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_gen)
        target_link_libraries(imslug_gen PRIVATE imslug_gen_core)
        target_compile_definitions(imslug_gen PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
        message(STATUS "imslug: imslug_gen offline tool enabled")
    endif()
else()
    message(STATUS "imslug: imslug_gen skipped (IMSLUG_TTF_PARSER=OFF)")
endif()

# ---------------------------------------------------------------------------
# C example: a pure-C11 consumer of imslug_c.h. The CPU smoke (context,
# init, add font, one headless frame, shutdown) builds and runs everywhere;
# when Vulkan is available the headless offscreen frame is compiled in too
# (`--vulkan`, exit 77 without a device). CI: the default build on Windows +
# Linux; `imslug_c_example` in ctest; `--vulkan` in the lavapipe golden job.
# ---------------------------------------------------------------------------
if(IMSLUG_BUILD_C_EXAMPLE)
    if(NOT IMSLUG_TTF_PARSER)
        message(STATUS "imslug: C example skipped (IMSLUG_TTF_PARSER=OFF; it registers a TTF font)")
    else()
        add_executable(imslug_example_c
            example/c/main.c
            example/c/imgui_c_shim.h
            example/c/imgui_c_shim.cpp)
        set_target_properties(imslug_example_c PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
        if(MSVC)
            target_compile_options(imslug_example_c PRIVATE /W4)
            target_compile_options(imslug_example_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c- /permissive->)
        else()
            target_compile_options(imslug_example_c PRIVATE -Wall -Wextra -Wpedantic)
            target_compile_options(imslug_example_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
        endif()
        target_link_libraries(imslug_example_c PRIVATE imslug)
        target_compile_definitions(imslug_example_c PRIVATE
            IMSLUG_C_EXAMPLE_FONT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/Roboto-Regular.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
        if(IMSLUG_HAVE_VULKAN AND IMSLUG_BACKEND_VULKAN)
            target_link_libraries(imslug_example_c PRIVATE imslug_vulkan)
            target_compile_definitions(imslug_example_c PRIVATE IMSLUG_C_EXAMPLE_VULKAN=1)
        endif()
        add_test(NAME imslug_c_example COMMAND imslug_example_c)
        if(IMSLUG_HAVE_VULKAN AND IMSLUG_BACKEND_VULKAN)
            # Full offscreen frame; skips (77) without a usable Vulkan device.
            add_test(NAME imslug_c_example_vulkan COMMAND imslug_example_c --vulkan)
            set_tests_properties(imslug_c_example_vulkan PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IMSLUG_BUILD_TESTS)
    add_subdirectory(tests)
endif()
