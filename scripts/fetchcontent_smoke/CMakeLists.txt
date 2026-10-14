# imslug - scripts/fetchcontent_smoke/CMakeLists.txt
#
# Skeleton of the clean-project verification: a throwaway CMake project
# that consumes the imslug repository via FetchContent (a git clone of the
# current commit), builds the GLFW+Vulkan example plus a minimal consumer
# executable against the public headers.
#
# Do not configure this directory in place: scripts/verify_fetchcontent.* copy
# it into a scratch build directory and configure it there (the only supported
# entry points).
#
# Inputs (passed by the scripts):
#   IMSLUG_FC_SOURCE  path to the imslug repository (cloned by FetchContent)
#   IMSLUG_FC_TAG     git ref to check out (default: main)

cmake_minimum_required(VERSION 3.21)
project(imslug_fetchcontent_smoke LANGUAGES CXX)

if(NOT DEFINED IMSLUG_FC_SOURCE OR IMSLUG_FC_SOURCE STREQUAL "")
    message(FATAL_ERROR "IMSLUG_FC_SOURCE is required (run scripts/verify_fetchcontent.*)")
endif()
file(TO_CMAKE_PATH "${IMSLUG_FC_SOURCE}" IMSLUG_FC_SOURCE)
if(NOT DEFINED IMSLUG_FC_TAG OR IMSLUG_FC_TAG STREQUAL "")
    set(IMSLUG_FC_TAG main)
endif()

# The acceptance target is the example: it needs Vulkan (headers/loader) and
# GLFW (fetched by imslug at the pinned tag).
set(IMSLUG_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(imslug
    GIT_REPOSITORY "${IMSLUG_FC_SOURCE}"
    GIT_TAG        "${IMSLUG_FC_TAG}")
FetchContent_MakeAvailable(imslug)

if(NOT TARGET imslug_example_glfw_vulkan)
    message(FATAL_ERROR
        "imslug_example_glfw_vulkan target is missing - is Vulkan available on this machine?")
endif()

# Minimal consumer: public header only, links the core library.
add_executable(fc_smoke main.cpp)
target_link_libraries(fc_smoke PRIVATE imslug)
