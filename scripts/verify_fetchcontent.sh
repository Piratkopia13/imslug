#!/usr/bin/env sh
# ============================================================================
# imslug - scripts/verify_fetchcontent.sh
#
# Clean-project verification: a throwaway CMake project consumes this
# repository via FetchContent (a git clone of the committed state), builds the
# GLFW+Vulkan example and a minimal consumer executable, and runs both.
#
#   scripts/verify_fetchcontent.sh
#
# The FetchContent clone is made from the repository's committed history, so
# commit your work first if you want the current tree verified.
#
# Overrides:
#   IMSLUG_FC_BUILD_DIR=<dir>  scratch project dir (default: build/fetchcontent-smoke)
#   IMSLUG_FC_TAG=<git ref>    ref cloned by FetchContent (default: current HEAD commit)
#   IMSLUG_FC_SCREENSHOT=1     also render one offscreen frame and check the PPM
#   IMSLUG_BUILD_TYPE=<cfg>    configuration for the throwaway project (default: Release)
#   IMSLUG_CMAKE=<path>        cmake to use (default: cmake on PATH)
#   IMSLUG_GENERATOR=<name>    force a CMake generator (default: Ninja when available)
# ============================================================================
set -eu

cd "$(dirname "$0")/.." || exit 1

BUILD_DIR="${IMSLUG_FC_BUILD_DIR:-build/fetchcontent-smoke}"
BUILD_TYPE="${IMSLUG_BUILD_TYPE:-Release}"

CMAKE="${IMSLUG_CMAKE:-}"
if [ -z "$CMAKE" ]; then
    CMAKE=$(command -v cmake || true)
fi
if [ -z "$CMAKE" ]; then
    echo "[fetchcontent] ERROR: cmake not found (PATH or IMSLUG_CMAKE)." >&2
    exit 1
fi

TAG="${IMSLUG_FC_TAG:-}"
if [ -z "$TAG" ]; then
    TAG=$(git rev-parse HEAD) || {
        echo "[fetchcontent] ERROR: cannot resolve HEAD; set IMSLUG_FC_TAG." >&2
        exit 1
    }
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cp scripts/fetchcontent_smoke/CMakeLists.txt scripts/fetchcontent_smoke/main.cpp "$BUILD_DIR/"

GEN_ARGS=""
if [ -z "${IMSLUG_GENERATOR:-}" ]; then
    if command -v ninja >/dev/null 2>&1; then
        GEN_ARGS="-G Ninja"
    fi
else
    GEN_ARGS="-G ${IMSLUG_GENERATOR}"
fi

echo "[fetchcontent] source:    $(pwd)"
echo "[fetchcontent] ref:       $TAG"
echo "[fetchcontent] project:   $BUILD_DIR"
echo "[fetchcontent] cmake:     $CMAKE"
echo "[fetchcontent] generator: ${GEN_ARGS:-<default>}"

# shellcheck disable=SC2086  # GEN_ARGS is intentionally word-split
"$CMAKE" -S "$BUILD_DIR" -B "$BUILD_DIR/build" $GEN_ARGS \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DIMSLUG_FC_SOURCE="$(pwd)" \
    -DIMSLUG_FC_TAG="$TAG"

"$CMAKE" --build "$BUILD_DIR/build" --parallel --target fc_smoke imslug_example_glfw_vulkan

EXE_DIR="$BUILD_DIR/build"
if [ ! -x "$EXE_DIR/fc_smoke" ] && [ -x "$EXE_DIR/Release/fc_smoke" ]; then
    EXE_DIR="$EXE_DIR/Release"
fi
if [ ! -x "$EXE_DIR/fc_smoke" ]; then
    echo "[fetchcontent] ERROR: fc_smoke not found under $BUILD_DIR/build." >&2
    exit 1
fi

# The example is a target of the fetched imslug project, so FetchContent
# places its binary in the dependency's build tree.
EXAMPLE_EXE="$EXE_DIR/imslug_example_glfw_vulkan"
if [ ! -x "$EXAMPLE_EXE" ] && [ -x "$BUILD_DIR/build/_deps/imslug-build/imslug_example_glfw_vulkan" ]; then
    EXAMPLE_EXE="$BUILD_DIR/build/_deps/imslug-build/imslug_example_glfw_vulkan"
fi
if [ ! -x "$EXAMPLE_EXE" ]; then
    echo "[fetchcontent] ERROR: imslug_example_glfw_vulkan not found under $BUILD_DIR/build." >&2
    exit 1
fi

echo "[fetchcontent] running fc_smoke ..."
"$EXE_DIR/fc_smoke"

echo "[fetchcontent] running imslug_example_glfw_vulkan --help ..."
"$EXAMPLE_EXE" --help

if [ "${IMSLUG_FC_SCREENSHOT:-0}" = "1" ]; then
    echo "[fetchcontent] rendering one offscreen frame (scene 1) ..."
    "$EXAMPLE_EXE" --screenshot "$BUILD_DIR/example.ppm" 1
    if [ ! -f "$BUILD_DIR/example.ppm" ]; then
        echo "[fetchcontent] ERROR: screenshot missing." >&2
        exit 1
    fi
    echo "[fetchcontent] screenshot: $BUILD_DIR/example.ppm"
fi

echo "[fetchcontent] OK: clean-project FetchContent build + example run passed."
