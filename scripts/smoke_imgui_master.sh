#!/usr/bin/env sh
# ============================================================================
# imslug - scripts/smoke_imgui_master.sh
#
# ImGui master smoke job: detects internal-API drift against ImGui master.
# Configures a separate build directory against a *fetched Dear ImGui master*
# instead of the pinned release tag and runs the full test suite. The suite is
# exactly the code that touches imgui_internal.h (the custom ImFontLoader, the
# baked glyph contract, the pre-pass), so a red run means the internal API
# moved.
#
#   scripts/smoke_imgui_master.sh                # master, build dir build-master
#   IMSLUG_SMOKE_IMGUI_TAG=v1.93.0 scripts/smoke_imgui_master.sh
#
# Overrides: IMSLUG_SMOKE_IMGUI_TAG (default: master)
#            IMSLUG_SMOKE_BUILD_DIR (default: build-master)
#            IMSLUG_BUILD_TYPE      (default: Release)
#
# Runs on demand (and daily in CI). Not part of the default scripts/test.* path:
# it needs network (FetchContent) and a fresh configure.
# ============================================================================
set -eu

cd "$(dirname "$0")/.." || exit 1

TAG="${IMSLUG_SMOKE_IMGUI_TAG:-master}"
BUILD_DIR="${IMSLUG_SMOKE_BUILD_DIR:-build-master}"
BUILD_TYPE="${IMSLUG_BUILD_TYPE:-Release}"

echo "[smoke] Dear ImGui '$TAG' -> $BUILD_DIR (this is a drift check, not a release gate)"
IMSLUG_RECONFIGURE=1 \
IMSLUG_BUILD_DIR="$BUILD_DIR" \
IMSLUG_BUILD_TYPE="$BUILD_TYPE" \
IMSLUG_CMAKE_DEFINES="-DIMSLUG_IMGUI_TAG=$TAG" \
    "$(dirname "$0")/test.sh" "$@"
