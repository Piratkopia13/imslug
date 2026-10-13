#!/bin/sh
# ============================================================================
# imslug - scripts/test.sh
#
# Build (via scripts/build.sh) and run the ctest suite.
# Extra arguments pass through to ctest:
#
#   scripts/test.sh                   run everything
#   scripts/test.sh -R smoke          run tests matching a regex
#
# Overrides: IMSLUG_BUILD_DIR / IMSLUG_BUILD_TYPE (see scripts/build.sh).
# ============================================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/.." || exit 1

BUILD_DIR="${IMSLUG_BUILD_DIR:-build}"

echo "[test] build step"
"$SCRIPT_DIR/build.sh" || { echo "[test] ERROR: build failed; not running ctest." >&2; exit 1; }

CTEST_EXE=$(command -v ctest 2>/dev/null || true)
if [ -z "$CTEST_EXE" ]; then
    CMAKE_EXE=$(command -v cmake 2>/dev/null || true)
    if [ -n "$CMAKE_EXE" ] && [ -x "$(dirname "$CMAKE_EXE")/ctest" ]; then
        CTEST_EXE="$(dirname "$CMAKE_EXE")/ctest"
    fi
fi
if [ -z "$CTEST_EXE" ]; then
    echo "[test] ERROR: ctest not found (it ships with cmake; put it on PATH)." >&2
    exit 1
fi

echo "[test] ctest --test-dir $BUILD_DIR --output-on-failure $*"
"$CTEST_EXE" --test-dir "$BUILD_DIR" --output-on-failure "$@" || exit 1
exit 0
