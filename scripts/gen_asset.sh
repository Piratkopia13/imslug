#!/bin/sh
# ============================================================================
# imslug - scripts/gen_asset.sh
#
# Regenerate the committed packed test asset (tests/assets/Roboto-ASCII.imslug)
# with the offline generator CLI, plus the two generated headers the GLFW
# example embeds as its default fonts:
#   example/glfw_vulkan/imslug_example_asset.h  (the packed asset itself)
#   example/glfw_vulkan/imslug_example_font.h   (the raw TTF for the stock
#                                                ImGui atlas font)
# All three are generated artifacts: never edit them by hand, and commit them
# together with any generator/format/version change that made them stale.
# imslug_packed_roundtrip and imslug_asset_fresh (which also byte-compares
# both example headers) compare against them and fail when stale.
#
# Overrides: IMSLUG_BUILD_DIR / IMSLUG_BUILD_TYPE / IMSLUG_CMAKE (see
# scripts/build.sh).
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

BUILD_DIR="${IMSLUG_BUILD_DIR:-build}"

echo "[gen_asset] build step (imslug_gen)"
"$(dirname "$0")/build.sh" --target imslug_gen || exit 1

GEN="$BUILD_DIR/imslug_gen"
if [ ! -x "$GEN" ]; then
    echo "[gen_asset] ERROR: imslug_gen not found at $GEN" >&2
    exit 1
fi

echo "[gen_asset] $GEN font tests/fonts/Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests/assets/Roboto-ASCII.imslug --emit-header example/glfw_vulkan/imslug_example_asset.h --header-symbol imslug_example_asset"
"$GEN" font tests/fonts/Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests/assets/Roboto-ASCII.imslug --emit-header example/glfw_vulkan/imslug_example_asset.h --header-symbol imslug_example_asset || exit 1

echo "[gen_asset] $GEN embed tests/fonts/Roboto-Regular.ttf --emit-header example/glfw_vulkan/imslug_example_font.h --header-symbol imslug_example_font"
"$GEN" embed tests/fonts/Roboto-Regular.ttf --emit-header example/glfw_vulkan/imslug_example_font.h --header-symbol imslug_example_font
exit $?
