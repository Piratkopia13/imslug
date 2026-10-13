#!/bin/sh
# ============================================================================
# imslug - scripts/fetch_cjk_font.sh
#
# Fetches the pinned Noto Sans SC variable TTF, verifies its SHA-256, and
# subsets it with fontTools into tests/fonts/cjk/NotoSansSC-subset.ttf (a
# gitignored test asset; see tests/fonts/cjk/PROVENANCE.txt for the exact
# provenance and subset command). Suites that need the font skip when it is
# absent, so this script is optional - CI runs it, local runs may too.
#
#   scripts/fetch_cjk_font.sh             fetch + subset when needed
#
# Overrides (all optional):
#   IMSLUG_CJK_URL      source URL (default: pinned google/fonts raw URL)
#   IMSLUG_CJK_SHA256   expected source SHA-256 (default: pinned)
#   IMSLUG_CJK_SOURCE   use a local source TTF instead of downloading
#   IMSLUG_PYTHON       python 3 executable (needs fontTools)
#
# Exit codes: 0 = ready (or cannot fetch here: tests skip CJK); 1 = failed.
# ============================================================================

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$root"

OUT_DIR=tests/fonts/cjk
OUT="$OUT_DIR/NotoSansSC-subset.ttf"
CHARS="$OUT_DIR/subset_chars.txt"
: "${IMSLUG_CJK_URL:=https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf}"
: "${IMSLUG_CJK_SHA256:=a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da}"
: "${IMSLUG_CJK_LICENSE_URL:=https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/OFL.txt}"

if [ ! -f "$CHARS" ]; then
    echo "[cjk-font] ERROR: missing $CHARS (the committed subset character set)." >&2
    exit 1
fi
if [ -f "$OUT" ]; then
    echo "[cjk-font] $OUT already present - nothing to do."
    exit 0
fi

PYTHON=${IMSLUG_PYTHON:-python3}
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python
if ! command -v "$PYTHON" >/dev/null 2>&1 || ! "$PYTHON" -c "import fontTools" >/dev/null 2>&1; then
    echo "[cjk-font] python 3 with fontTools is unavailable (pip install fonttools);"
    echo "[cjk-font] skipping CJK font fetch - the stress/golden suites will skip CJK."
    exit 0
fi

if [ -n "${IMSLUG_CJK_SOURCE:-}" ]; then
    SRC="$IMSLUG_CJK_SOURCE"
else
    mkdir -p "$OUT_DIR/.cache"
    SRC="$OUT_DIR/.cache/NotoSansSC-vf.ttf"
fi

if [ ! -f "$SRC" ]; then
    echo "[cjk-font] downloading $IMSLUG_CJK_URL"
    if command -v curl >/dev/null 2>&1; then
        curl -sSL --fail -o "$SRC" "$IMSLUG_CJK_URL" || { rm -f "$SRC"; echo "[cjk-font] download failed; skipping CJK font fetch."; exit 0; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$SRC" "$IMSLUG_CJK_URL" || { rm -f "$SRC"; echo "[cjk-font] download failed; skipping CJK font fetch."; exit 0; }
    else
        echo "[cjk-font] neither curl nor wget is available; skipping CJK font fetch."
        exit 0
    fi
fi

src_hash=$("$PYTHON" - "$SRC" <<'PY'
import hashlib
import sys
with open(sys.argv[1], "rb") as handle:
    print(hashlib.sha256(handle.read()).hexdigest())
PY
)
if [ "$src_hash" != "$IMSLUG_CJK_SHA256" ]; then
    echo "[cjk-font] ERROR: source SHA-256 mismatch: got $src_hash" >&2
    echo "[cjk-font]   expected $IMSLUG_CJK_SHA256 - refusing to subset." >&2
    exit 1
fi

echo "[cjk-font] subsetting into $OUT"
"$PYTHON" -m fontTools.subset "$SRC" --output-file="$OUT" --text-file="$CHARS" \
    --layout-features-=kern,liga,clig,calt --no-hinting --desubroutinize
[ -f "$OUT" ] || exit 1
if command -v curl >/dev/null 2>&1; then
    curl -sSL --fail -o "$OUT_DIR/OFL.txt" "$IMSLUG_CJK_LICENSE_URL" 2>/dev/null || true
fi
echo "[cjk-font] wrote $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)."
