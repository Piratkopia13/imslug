#!/bin/sh
# ============================================================================
# imslug - scripts/gen_shaders.sh
#
# Compile shaders/vulkan/*.{vert,frag,comp} to SPIR-V and update the committed
# header shaders/imslug_shaders_vulkan.h; embed shaders/wgsl/*.wgsl as
# NUL-terminated C string arrays in the committed header
# shaders/imslug_shaders_webgpu.h. Each half is skipped with a message when its
# source directory has no shader sources; only when both are missing is there
# nothing to generate.
#
#   scripts/gen_shaders.sh           regenerate both headers
#   scripts/gen_shaders.sh --check   build into temp files and diff against the
#                                    committed headers (non-zero if stale)
#
# Requirements:
#   * glslangValidator - GLSLANG_VALIDATOR, or $VULKAN_SDK/bin, or PATH
#                        (SPIR-V half only);
#   * python 3         - $PYTHON, or python3/python on PATH (both halves);
# Invocation per GLSL source:
#   glslangValidator -V --target-env vulkan1.2 -o <tmp>/<name>.<stage>.spv <src>
# WGSL embedding (pure python, no external tool):
#   python3 scripts/emit_shader_header.py --mode wgsl --input-dir shaders/wgsl --output <header>
# Never hand-edit the generated headers (AGENTS.md invariant 1).
# ============================================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/.." || exit 1

SHADER_DIR="shaders/vulkan"
HEADER="shaders/imslug_shaders_vulkan.h"
WGSL_DIR="shaders/wgsl"
WGSL_HEADER="shaders/imslug_shaders_webgpu.h"
CHECK=0

for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        -h|--help)
            echo "Usage: scripts/gen_shaders.sh [--check]"
            echo "  (no args)  regenerate $HEADER and $WGSL_HEADER"
            echo "  --check    build into temp files and diff against the committed headers"
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

have_spirv=0
for src in "$SHADER_DIR"/*.vert "$SHADER_DIR"/*.frag "$SHADER_DIR"/*.comp; do
    if [ -f "$src" ]; then
        have_spirv=1
        break
    fi
done

wgsl_count=0
for src in "$WGSL_DIR"/*.wgsl; do
    if [ -f "$src" ]; then
        wgsl_count=$((wgsl_count + 1))
    fi
done

if [ "$have_spirv" = 0 ] && [ "$wgsl_count" = 0 ]; then
    if [ "$CHECK" = 1 ]; then
        echo "[gen_shaders] no shader sources found - --check passes trivially."
    else
        echo "[gen_shaders] no shader sources found - nothing to generate."
    fi
    exit 0
fi

PYTHON_EXE="${PYTHON:-}"
if [ -z "$PYTHON_EXE" ]; then
    PYTHON_EXE=$(command -v python3 2>/dev/null || true)
fi
if [ -z "$PYTHON_EXE" ]; then
    PYTHON_EXE=$(command -v python 2>/dev/null || true)
fi
[ -n "$PYTHON_EXE" ] || fail "python 3 not found (needed to emit the shader headers); put it on PATH or set PYTHON."

tmp=$(mktemp -d "${TMPDIR:-/tmp}/imslug-gen-shaders.XXXXXX") || fail "mktemp failed"
trap 'rm -rf "$tmp"' EXIT INT TERM

if [ "$have_spirv" = 1 ]; then
    GLSLANG="${GLSLANG_VALIDATOR:-}"
    if [ -z "$GLSLANG" ]; then
        if [ -n "${VULKAN_SDK:-}" ] && [ -x "${VULKAN_SDK}/bin/glslangValidator" ]; then
            GLSLANG="${VULKAN_SDK}/bin/glslangValidator"
        else
            # glslang 16.x ships the tool as `glslang`; older SDKs as
            # `glslangValidator`. Same CLI for our invocation.
            GLSLANG=$(command -v glslangValidator 2>/dev/null || true)
            if [ -z "$GLSLANG" ]; then
                GLSLANG=$(command -v glslang 2>/dev/null || true)
            fi
        fi
    fi
    [ -n "$GLSLANG" ] || fail "glslangValidator not found: install the Vulkan SDK and set VULKAN_SDK, or put glslangValidator/glslang on PATH, or set GLSLANG_VALIDATOR."

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

    "$PYTHON_EXE" "$SCRIPT_DIR/emit_shader_header.py" --mode spirv --input-dir "$tmp" --output "$out" || fail "header emission failed (scripts/emit_shader_header.py)"

    if [ "$CHECK" = 1 ]; then
        [ -f "$HEADER" ] || fail "$HEADER is missing but shader sources exist: run scripts/gen_shaders.sh and commit the generated header."
        cmp -s "$HEADER" "$tmp/imslug_shaders_vulkan.h" || fail "committed SPIR-V header is stale: $HEADER - run scripts/gen_shaders.sh, review the diff and commit the header together with its sources."
        echo "[gen_shaders] --check OK: $HEADER is up to date ($count shader(s))."
    else
        echo "[gen_shaders] wrote $HEADER ($count shader(s))."
    fi
else
    echo "[gen_shaders] no SPIR-V shader sources in $SHADER_DIR - skipping the SPIR-V header."
fi

if [ "$wgsl_count" -gt 0 ]; then
    if [ "$CHECK" = 1 ]; then
        out="$tmp/imslug_shaders_webgpu.h"
    else
        out="$WGSL_HEADER"
    fi

    "$PYTHON_EXE" "$SCRIPT_DIR/emit_shader_header.py" --mode wgsl --input-dir "$WGSL_DIR" --output "$out" || fail "header emission failed (scripts/emit_shader_header.py)"

    if [ "$CHECK" = 1 ]; then
        [ -f "$WGSL_HEADER" ] || fail "$WGSL_HEADER is missing but WGSL sources exist: run scripts/gen_shaders.sh and commit the generated header."
        cmp -s "$WGSL_HEADER" "$tmp/imslug_shaders_webgpu.h" || fail "committed WGSL header is stale: $WGSL_HEADER - run scripts/gen_shaders.sh, review the diff and commit the header together with its sources."
        echo "[gen_shaders] --check OK: $WGSL_HEADER is up to date ($wgsl_count file(s))."
    else
        echo "[gen_shaders] wrote $WGSL_HEADER ($wgsl_count file(s))."
    fi
else
    echo "[gen_shaders] no WGSL sources in $WGSL_DIR - skipping the WGSL header."
fi

exit 0
