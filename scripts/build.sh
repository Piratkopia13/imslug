#!/bin/sh
# ============================================================================
# imslug - scripts/build.sh
#
# Configure (only if needed) and build. Canonical entry point for humans, CI
# and .opencode/commands - see AGENTS.md ("Commands").
#
#   scripts/build.sh                     build the default targets
#   scripts/build.sh --target imslug     any extra arg goes to cmake --build
#
# Overrides:
#   IMSLUG_BUILD_DIR=<dir>    build directory        (default: build)
#   IMSLUG_BUILD_TYPE=<cfg>   single-config config   (default: Release)
#   IMSLUG_RECONFIGURE=1      force a configure pass
#   IMSLUG_CMAKE=<path>       cmake to use
#   IMSLUG_GENERATOR=<name>   force a CMake generator (fresh configure only)
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

BUILD_DIR="${IMSLUG_BUILD_DIR:-build}"
BUILD_TYPE="${IMSLUG_BUILD_TYPE:-Release}"

CMAKE_EXE="${IMSLUG_CMAKE:-}"
if [ -z "$CMAKE_EXE" ]; then
    CMAKE_EXE=$(command -v cmake 2>/dev/null || true)
fi
if [ -z "$CMAKE_EXE" ]; then
    echo "[build] ERROR: cmake not found. Options:" >&2
    echo "[build]   - put cmake on PATH, or" >&2
    echo "[build]   - set IMSLUG_CMAKE=/path/to/cmake" >&2
    exit 1
fi

if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "${IMSLUG_RECONFIGURE:-0}" != "1" ]; then
    :
else
    GEN="${IMSLUG_GENERATOR:-}"
    if [ -z "$GEN" ]; then
        if command -v ninja >/dev/null 2>&1; then
            GEN="Ninja"
        else
            GEN="Unix Makefiles"
        fi
    fi
    echo "[build] cmake:      $CMAKE_EXE"
    echo "[build] build dir:  $BUILD_DIR"
    echo "[build] generator:  $GEN"
    echo "[build] configuring ..."
    if [ "$GEN" = "Ninja" ]; then
        "$CMAKE_EXE" -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || exit 1
    else
        "$CMAKE_EXE" -S . -B "$BUILD_DIR" -G "$GEN" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || exit 1
    fi
fi

echo "[build] building $BUILD_DIR $*"
"$CMAKE_EXE" --build "$BUILD_DIR" --parallel "$@" || exit 1
exit 0
