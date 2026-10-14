#!/bin/sh
# ============================================================================
# imslug - scripts/gen_asset.sh
#
# Regenerate the committed packed test asset (tests/assets/Roboto-ASCII.imslug)
# with the M7 offline generator CLI. The asset is a generated artifact: never
# edit it by hand, and commit it together with any generator/format/version
# change that made it stale. imslug_packed_roundtrip and imslug_asset_fresh
# compare against it and fail when it is stale.
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

echo "[gen_asset] $GEN font tests/fonts/Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests/assets/Roboto-ASCII.imslug"
"$GEN" font tests/fonts/Roboto-Regular.ttf --size-range 8:72 --chars 0x20-0x7E -o tests/assets/Roboto-ASCII.imslug
exit $?
