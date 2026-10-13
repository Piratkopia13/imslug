#!/bin/sh
# ============================================================================
# imslug - scripts/gen_shaders.sh
#
# Compile shaders/vulkan/*.{vert,frag,comp} to SPIR-V and update the committed
# header shaders/imslug_shaders_vulkan.h. Authoritative shader sources arrive
# in M2; with no sources present this succeeds trivially.
#
#   scripts/gen_shaders.sh           regenerate the header
#   scripts/gen_shaders.sh --check   compile into a temp file and diff against
#                                    the committed header (non-zero if stale)
#
# Requirements (once shader sources exist):
#   * glslangValidator - GLSLANG_VALIDATOR, or $VULKAN_SDK/bin, or PATH;
#   * python 3         - $PYTHON, or python3/python on PATH;
# Invocation per source:
#   glslangValidator -V --target-env vulkan1.2 -o <tmp>/<name>.<stage>.spv <src>
# Never hand-edit the generated header (AGENTS.md invariant 1).
# ============================================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/.." || exit 1

SHADER_DIR="shaders/vulkan"
HEADER="shaders/imslug_shaders_vulkan.h"
CHECK=0

for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        -h|--help)
            echo "Usage: scripts/gen_shaders.sh [--check]"
            echo "  (no args)  regenerate $HEADER"
            echo "  --check    compile into a temp file and diff against the committed header"
            exit 0
            ;;
        *)
            echo "[gen_shaders] unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

fail() {
    echo "[gen_shaders] ERROR: $1" >&2
    exit 1
}

have_sources=0
for src in "$SHADER_DIR"/*.vert "$SHADER_DIR"/*.frag "$SHADER_DIR"/*.comp; do
    if [ -f "$src" ]; then
        have_sources=1
        break
    fi
done

if [ "$have_sources" = 0 ]; then
    if [ "$CHECK" = 1 ]; then
        echo "[gen_shaders] no shader sources yet (M2) - --check passes trivially."
    else
        echo "[gen_shaders] no shader sources yet (M2) - nothing to generate."
    fi
    exit 0
fi

GLSLANG="${GLSLANG_VALIDATOR:-}"
if [ -z "$GLSLANG" ]; then
    if [ -n "${VULKAN_SDK:-}" ] && [ -x "${VULKAN_SDK}/bin/glslangValidator" ]; then
        GLSLANG="${VULKAN_SDK}/bin/glslangValidator"
    else
        GLSLANG=$(command -v glslangValidator 2>/dev/null || true)
    fi
fi
[ -n "$GLSLANG" ] || fail "glslangValidator not found: install the Vulkan SDK and set VULKAN_SDK, or put glslangValidator on PATH, or set GLSLANG_VALIDATOR."

PYTHON_EXE="${PYTHON:-}"
if [ -z "$PYTHON_EXE" ]; then
    PYTHON_EXE=$(command -v python3 2>/dev/null || true)
fi
if [ -z "$PYTHON_EXE" ]; then
    PYTHON_EXE=$(command -v python 2>/dev/null || true)
fi
[ -n "$PYTHON_EXE" ] || fail "python 3 not found (needed to emit the SPIR-V header); put it on PATH or set PYTHON."

tmp=$(mktemp -d "${TMPDIR:-/tmp}/imslug-gen-shaders.XXXXXX") || fail "mktemp failed"
trap 'rm -rf "$tmp"' EXIT INT TERM

count=0
for src in "$SHADER_DIR"/*.vert "$SHADER_DIR"/*.frag "$SHADER_DIR"/*.comp; do
    [ -f "$src" ] || continue
    base=$(basename "$src")
    name=${base%.*}
    stage=${base##*.}
    "$GLSLANG" -V --target-env vulkan1.2 -o "$tmp/$name.$stage.spv" "$src" || fail "glslangValidator failed for $src"
    count=$((count + 1))
done

if [ "$CHECK" = 1 ]; then
    out="$tmp/imslug_shaders_vulkan.h"
else
    out="$HEADER"
fi

"$PYTHON_EXE" "$SCRIPT_DIR/emit_shader_header.py" --input-dir "$tmp" --output "$out" || fail "header emission failed (scripts/emit_shader_header.py)"

if [ "$CHECK" = 1 ]; then
    [ -f "$HEADER" ] || fail "$HEADER is missing but shader sources exist: run scripts/gen_shaders.sh and commit the generated header."
    cmp -s "$HEADER" "$tmp/imslug_shaders_vulkan.h" || fail "committed SPIR-V header is stale: $HEADER - run scripts/gen_shaders.sh, review the diff and commit the header together with its sources."
    echo "[gen_shaders] --check OK: $HEADER is up to date ($count shader(s))."
else
    echo "[gen_shaders] wrote $HEADER ($count shader(s))."
fi

exit 0
