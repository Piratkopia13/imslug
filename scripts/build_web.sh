#!/bin/sh
# ============================================================================
# imslug - scripts/build_web.sh
#
# Build the browser demo (WebGPU backend + Emscripten example) and assemble the
# static site GitHub Pages serves: <build dir>/site/ with index.html +
# index.js + index.wasm + .nojekyll. Canonical entry point for humans, CI and
# .opencode/commands - see AGENTS.md ("Commands").
#
#   scripts/build_web.sh
#
# Requirements:
#   * emsdk >= 4.0.10 (CI pins 5.0.1): have emcmake/em++ on PATH (CI: the
#     emscripten-core/setup-emsdk action), or set EMSDK to the emsdk root /
#     install it at $HOME/emsdk; emsdk_env.sh is sourced when found.
#   * cmake >= 3.21 and ninja (or make).
#   * A native build (scripts/build.sh) to have run once: the demo embeds the
#     fetched CJK subset with the NATIVE imslug_gen (a cross-compiled binary
#     cannot run during the web build). Without it the demo still builds
#     (embedded Roboto fonts only) and the CJK scene shows its "font not
#     embedded" message.
#
# Overrides:
#   IMSLUG_BUILD_DIR=<dir>         web build directory   (default: build-web)
#   IMSLUG_BUILD_TYPE=<cfg>        single-config config  (default: Release)
#   IMSLUG_NATIVE_BUILD_DIR=<dir>  native build with imslug_gen (default: build)
#   IMSLUG_CMAKE=<path>            cmake to use
#   IMSLUG_RECONFIGURE=1           force a configure pass
#   IMSLUG_CMAKE_DEFINES="-D..."   extra configure-time options
#   IMSLUG_SKIP_EMSDK_ENV=1        do not source emsdk_env.sh
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

BUILD_DIR="${IMSLUG_BUILD_DIR:-build-web}"
BUILD_TYPE="${IMSLUG_BUILD_TYPE:-Release}"
NATIVE_DIR="${IMSLUG_NATIVE_BUILD_DIR:-build}"

# --- emsdk environment -------------------------------------------------------
if [ -z "${IMSLUG_SKIP_EMSDK_ENV:-}" ] && ! command -v emcmake >/dev/null 2>&1; then
    for candidate in "${EMSDK:-}" "$HOME/emsdk" /usr/local/emsdk /opt/emsdk; do
        if [ -n "$candidate" ] && [ -f "$candidate/emsdk_env.sh" ]; then
            # shellcheck disable=SC1090
            . "$candidate/emsdk_env.sh" >/dev/null 2>&1 || true
            if command -v emcmake >/dev/null 2>&1; then
                echo "[build_web] sourced $candidate/emsdk_env.sh"
                break
            fi
        fi
    done
fi
if ! command -v emcmake >/dev/null 2>&1; then
    echo "[build_web] ERROR: emcmake not found. Options:" >&2
    echo "[build_web]   - install emsdk (>= 4.0.10) and source emsdk_env.sh, or" >&2
    echo "[build_web]   - set EMSDK=/path/to/emsdk, or" >&2
    echo "[build_web]   - put emcmake on PATH (CI: emscripten-core/setup-emsdk)." >&2
    exit 1
fi

# --- cmake / generator -------------------------------------------------------
CMAKE_EXE="${IMSLUG_CMAKE:-}"
if [ -z "$CMAKE_EXE" ]; then
    CMAKE_EXE=$(command -v cmake 2>/dev/null || true)
fi
if [ -z "$CMAKE_EXE" ]; then
    echo "[build_web] ERROR: cmake not found. Options:" >&2
    echo "[build_web]   - put cmake on PATH, or" >&2
    echo "[build_web]   - set IMSLUG_CMAKE=/path/to/cmake" >&2
    exit 1
fi

GEN="${IMSLUG_GENERATOR:-}"
if [ -z "$GEN" ]; then
    if command -v ninja >/dev/null 2>&1; then
        GEN="Ninja"
    else
        GEN="Unix Makefiles"
    fi
fi

# --- optional CJK subset embed (native imslug_gen) ---------------------------
CJK_HEADER=""
GEN_EXE="$NATIVE_DIR/imslug_gen"
if [ ! -x "$GEN_EXE" ] && [ -x "$NATIVE_DIR/$BUILD_TYPE/imslug_gen" ]; then
    GEN_EXE="$NATIVE_DIR/$BUILD_TYPE/imslug_gen"
fi
CJK_FONT="tests/fonts/cjk/NotoSansSC-subset.ttf"
if [ -x "$GEN_EXE" ] && [ -f "$CJK_FONT" ]; then
    echo "[build_web] embedding the CJK subset with $GEN_EXE"
    mkdir -p "$BUILD_DIR/include"
    if ! "$GEN_EXE" embed "$CJK_FONT" --emit-header "$BUILD_DIR/include/imslug_example_cjk.h" \
            --header-symbol imslug_example_cjk; then
        echo "[build_web] ERROR: imslug_gen embed failed." >&2
        exit 1
    fi
    CJK_HEADER="$BUILD_DIR/include/imslug_example_cjk.h"
else
    echo "[build_web] CJK subset or native imslug_gen missing - building without embedded CJK"
    echo "[build_web]   (fetch once: scripts/fetch_cjk_font.sh; native tool: scripts/build.sh)"
fi

# --- configure only when needed ---------------------------------------------
DEFINES="-DIMSLUG_BUILD_EXAMPLES=ON -DIMSLUG_BACKEND_VULKAN=OFF -DIMSLUG_BUILD_TESTS=OFF -DIMSLUG_BUILD_C_EXAMPLE=OFF -DIMSLUG_BUILD_TOOLS=OFF"
if [ -n "$CJK_HEADER" ]; then
    DEFINES="$DEFINES -DIMSLUG_CJK_EMBED_HEADER=$CJK_HEADER"
fi
if [ -n "${IMSLUG_CMAKE_DEFINES:-}" ]; then
    DEFINES="$DEFINES $IMSLUG_CMAKE_DEFINES"
fi

RECONFIGURE="${IMSLUG_RECONFIGURE:-0}"
if [ -n "$IMSLUG_CMAKE_DEFINES" ]; then RECONFIGURE=1; fi
if [ -n "$CJK_HEADER" ]; then RECONFIGURE=1; fi

if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ "$RECONFIGURE" != "1" ]; then
    :
else
    echo "[build_web] emcmake:   $(command -v emcmake)"
    echo "[build_web] cmake:     $CMAKE_EXE"
    echo "[build_web] build dir: $BUILD_DIR"
    echo "[build_web] defines:   $DEFINES"
    # shellcheck disable=SC2086
    emcmake "$CMAKE_EXE" -S . -B "$BUILD_DIR" -G "$GEN" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $DEFINES || exit 1
fi

echo "[build_web] building imslug_example_web in $BUILD_DIR"
"$CMAKE_EXE" --build "$BUILD_DIR" --target imslug_example_web --parallel || exit 1

# --- assemble the site -------------------------------------------------------
SITE="$BUILD_DIR/site"
for f in index.html index.js index.wasm; do
    if [ ! -f "$SITE/$f" ]; then
        echo "[build_web] ERROR: $SITE/$f missing after the build." >&2
        exit 1
    fi
done
touch "$SITE/.nojekyll"

echo "[build_web] site ready: $SITE"
echo "[build_web] serve locally with: python3 -m http.server 8000 --directory $SITE"
exit 0
