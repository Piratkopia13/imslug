#!/bin/sh
# ============================================================================
# imslug - scripts/golden.sh
#
# Build and run the golden-image tests (M2 scenes, M3 Slug-vs-atlas, M5 emoji):
# headless offscreen Vulkan rendering
# of fixed strings at sizes 12/16/32/64 x DPI scales 1.0/1.25/2.0, compared per
# pixel against the M1 CPU reference rasterizer. Both render paths are
# exercised: a classic render pass and core Vulkan 1.3 dynamic rendering.
#
# Output: PPMs land in <build dir>/golden/ (<case>_gpu.ppm, <case>_cpu.ppm,
# <case>_diff.ppm on failure). Tests skip (exit 77) when no usable Vulkan
# device is available.
#
#   scripts/golden.sh                  build + run both render-path tests
#   scripts/golden.sh -R golden_dyn    extra args pass to ctest
#
# Overrides: IMSLUG_BUILD_DIR / IMSLUG_BUILD_TYPE (see scripts/build.sh).
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

BUILD_DIR="${IMSLUG_BUILD_DIR:-build}"
BUILD_TYPE="${IMSLUG_BUILD_TYPE:-Release}"

echo "[golden] build step"
"$(dirname "$0")/build.sh" --target imslug_tests_golden imslug_tests_imgui_golden imslug_tests_emoji_golden || exit 1

CTEST_EXE="${IMSLUG_CTEST:-ctest}"

if [ "$#" -eq 0 ]; then
    echo "[golden] ctest --test-dir $BUILD_DIR -R golden --output-on-failure"
    "$CTEST_EXE" --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" -R golden --output-on-failure
else
    echo "[golden] ctest --test-dir $BUILD_DIR -R golden --output-on-failure $*"
    "$CTEST_EXE" --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" -R golden --output-on-failure "$@"
fi
exit $?
