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
option(IMSLUG_BACKEND_VULKAN "Build the Vulkan backend module (placeholder until M3)" ON)
option(IMSLUG_BUILD_TESTS    "Build unit tests and enable ctest" ON)
option(IMSLUG_BUILD_EXAMPLES "Build the example applications (M2+)" OFF)
option(IMSLUG_BUILD_TOOLS    "Build the offline tools (imslug_gen, M7)" OFF)

set(IMSLUG_IMGUI_DIR "" CACHE PATH
    "Use an existing Dear ImGui checkout instead of fetching it (-DIMSLUG_IMGUI_DIR=...)")
set(IMSLUG_IMGUI_TAG "v1.92.9b" CACHE STRING
    "Dear ImGui release tag fetched with FetchContent")

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
# the source tree; M1+ compiles the subset we need (imgui.cpp, imgui_draw.cpp,
# imgui_tables.cpp, imgui_widgets.cpp) from IMSLUG_IMGUI_SOURCE_DIR.
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

add_library(imslug_imgui INTERFACE)
if(IMSLUG_IMGUI_LINK_TARGET)
    target_link_libraries(imslug_imgui INTERFACE ${IMSLUG_IMGUI_LINK_TARGET})
else()
    target_include_directories(imslug_imgui SYSTEM INTERFACE
        "${IMSLUG_IMGUI_SOURCE_DIR}"
        "${IMSLUG_IMGUI_SOURCE_DIR}/backends"
        "${IMSLUG_IMGUI_SOURCE_DIR}/misc/cpp")
endif()

# ---------------------------------------------------------------------------
# Core library (no backend, no ImGui linkage required yet — see PLAN §6)
# ---------------------------------------------------------------------------
add_library(imslug STATIC
    imslug.cpp
    imslug.h)
set_target_properties(imslug PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
imslug_target_defaults(imslug)
target_include_directories(imslug PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:include>")

# ---------------------------------------------------------------------------
# Optional targets — placeholders until their milestones land. They are
# created only when the corresponding option is ON; see the milestone briefs.
# ---------------------------------------------------------------------------
if(IMSLUG_BACKEND_VULKAN)
    add_library(imslug_vulkan INTERFACE)
    target_link_libraries(imslug_vulkan INTERFACE imslug)
    message(STATUS "imslug: imslug_vulkan placeholder target (Vulkan backend lands in M3)")
endif()

if(IMSLUG_BUILD_EXAMPLES)
    add_library(imslug_example_glfw_vulkan INTERFACE)
    target_link_libraries(imslug_example_glfw_vulkan INTERFACE imslug)
    message(STATUS "imslug: imslug_example_glfw_vulkan placeholder target (example lands in M2)")
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
