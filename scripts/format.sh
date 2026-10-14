#!/bin/sh
# ============================================================================
# imslug - scripts/format.sh
#
# Run clang-format over our C/C++ sources. Vendored trees (third_party/,
# vendor/) and generated artifacts (shaders/imslug_shaders_vulkan.h,
# example/common/imslug_example_*.h) are never touched - upstream/emitted
# files keep their own byte layout.
#
#   scripts/format.sh            format in place
#   scripts/format.sh --check    report files that would change, exit non-zero
#
# Limitation: repository paths must not contain whitespace (word splitting).
# ============================================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/.." || exit 1

CHECK=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        -h|--help)
            echo "Usage: scripts/format.sh [--check]"
            exit 0
            ;;
        *)
            echo "[format] unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

CLANG_FORMAT=$(command -v clang-format 2>/dev/null || true)
if [ -z "$CLANG_FORMAT" ]; then
    echo "[format] ERROR: clang-format not found - install LLVM or add it to PATH." >&2
    exit 1
fi

files=$(git ls-files -co --exclude-standard -- '*.h' '*.hpp' '*.c' '*.cpp' '*.cc' '*.cxx' \
    | grep -Ev '^(third_party|vendor)/' \
    | grep -Ev '^shaders/imslug_shaders_vulkan\.h$' \
    | grep -Ev '^example/common/imslug_example_(asset|font)\.h$')

count=0
rc=0
for file in $files; do
    if [ "$CHECK" = 1 ]; then
        "$CLANG_FORMAT" --dry-run --Werror "$file" || rc=1
    else
        "$CLANG_FORMAT" -i "$file" || rc=1
    fi
    count=$((count + 1))
done

if [ "$rc" = 0 ]; then
    echo "[format] OK ($count file(s))."
else
    echo "[format] FAIL: formatting differences found - run scripts/format.sh to fix." >&2
fi
exit $rc
