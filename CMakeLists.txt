# imslug — Slug-based GPU text rendering for Dear ImGui.
#
# Entry points for humans, CI and agents are scripts/build.* and scripts/test.*
# (see AGENTS.md); do not bypass them with ad-hoc cmake invocations.
#
# Style contract (PLAN §6): public headers compile as C++11 and contain no STL;
# our implementation is C++17 without exceptions; third-party targets keep
# their own settings.

cmake_minimum_required(VERSION 3.21)

project(imslug
    VERSION 0.1.0
    DESCRIPTION "Slug-based GPU text rendering for Dear ImGui"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(IMSLUG_BACKEND_VULKAN "Build the Vulkan backend module (M2 renderer core; reused by the M3 ImGui adapter)" ON)
option(IMSLUG_BUILD_TESTS    "Build unit tests and enable ctest" ON)
option(IMSLUG_BUILD_EXAMPLES "Build the example applications (M2+)" OFF)
option(IMSLUG_BUILD_TOOLS    "Build the offline tools (imslug_gen, M7)" OFF)

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
# sources. The imslug core needs Dear ImGui from M3 on (imslug.h includes
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
# apply to third-party code (AGENTS.md invariant 2; M1 brief).
# ---------------------------------------------------------------------------
add_library(imslug_stb STATIC imslug_stb.cpp)
target_include_directories(imslug_stb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
if(MSVC)
    target_compile_options(imslug_stb PRIVATE /w)
else()
    target_compile_options(imslug_stb PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Core library (M3: includes the ImGui-facing core + adapter; the backend
# modules stay in backends/ — AGENTS.md invariants 4 and 7)
# ---------------------------------------------------------------------------
add_library(imslug STATIC
    imslug.cpp
    imslug.h
    imslug_internal.h
    imslug_glyphgen.h
    imslug_glyphgen.cpp
    imslug_packer.h
    imslug_packer.cpp
    imslug_refraster.h
    imslug_refraster.cpp
    imslug_imgui_adapter.h
    imslug_imgui_adapter.cpp)
set_target_properties(imslug PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug)
target_include_directories(imslug PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb>"
    "$<INSTALL_INTERFACE:include>")
target_link_libraries(imslug PRIVATE imslug_stb)
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
# Optional targets — placeholders until their milestones land. They are
# created only when the corresponding option is ON; see the milestone briefs.
# ---------------------------------------------------------------------------
if(IMSLUG_BACKEND_VULKAN)
    if(IMSLUG_HAVE_VULKAN)
        # Standalone Vulkan rendering core (M2): ImGui-free, reused by M3.
        add_library(imslug_vulkan STATIC
            backends/imslug_impl_vulkan.cpp
            backends/imslug_impl_vulkan.h)
        set_target_properties(imslug_vulkan PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_vulkan)
        target_include_directories(imslug_vulkan PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends>"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
        target_link_libraries(imslug_vulkan PUBLIC imslug)
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(imslug_vulkan PUBLIC Vulkan::Vulkan)
        else()
            target_include_directories(imslug_vulkan PUBLIC ${Vulkan_INCLUDE_DIRS})
        endif()
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

        # Dear ImGui platform/renderer backends (third-party; examples only).
        add_library(imslug_imgui_backends STATIC
            "${IMSLUG_IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
            "${IMSLUG_IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
        set_target_properties(imslug_imgui_backends PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_link_libraries(imslug_imgui_backends PUBLIC ${IMSLUG_IMGUI_TARGET} glfw)
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(imslug_imgui_backends PUBLIC Vulkan::Vulkan)
        else()
            target_include_directories(imslug_imgui_backends PUBLIC ${Vulkan_INCLUDE_DIRS})
        endif()
        if(MSVC)
            target_compile_options(imslug_imgui_backends PRIVATE /w)
        else()
            target_compile_options(imslug_imgui_backends PRIVATE -w)
        endif()

        add_executable(imslug_example_glfw_vulkan example/glfw_vulkan/main.cpp)
        set_target_properties(imslug_example_glfw_vulkan PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        imslug_target_defaults(imslug_example_glfw_vulkan)
        target_link_libraries(imslug_example_glfw_vulkan PRIVATE imslug imslug_vulkan glfw imslug_imgui_backends)
        target_compile_definitions(imslug_example_glfw_vulkan PRIVATE
            IMSLUG_EXAMPLE_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/Roboto-Regular.ttf"
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    endif()
endif()

if(IMSLUG_BUILD_TOOLS)
    add_library(imslug_gen INTERFACE)
    target_link_libraries(imslug_gen INTERFACE imslug)
    message(STATUS "imslug: imslug_gen placeholder target (offline tool lands in M7)")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IMSLUG_BUILD_TESTS)
    add_subdirectory(tests)
endif()
