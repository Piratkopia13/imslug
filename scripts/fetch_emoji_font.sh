#!/bin/sh
# ============================================================================
# imslug - scripts/fetch_emoji_font.sh
#
# Fetches the pinned Noto Color Emoji TTF (COLR v1) and verifies its SHA-256
# into tests/fonts/emoji/NotoColorEmoji-Regular.ttf (a gitignored test asset;
# see tests/fonts/emoji/PROVENANCE.txt for the exact provenance). Suites that
# need the font skip when it is absent, so this script is optional - CI runs
# it, local runs may too.
#
#   scripts/fetch_emoji_font.sh           fetch when needed
#
# Overrides (all optional):
#   IMSLUG_EMOJI_URL      source URL (default: pinned google/fonts raw URL)
#   IMSLUG_EMOJI_SHA256   expected source SHA-256 (default: pinned)
#   IMSLUG_EMOJI_SOURCE   use a local source TTF instead of downloading
#
# Exit codes: 0 = ready (or cannot fetch here: tests skip color fonts);
#             1 = failed.
# ============================================================================

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$root"

OUT_DIR=tests/fonts/emoji
OUT="$OUT_DIR/NotoColorEmoji-Regular.ttf"
: "${IMSLUG_EMOJI_URL:=https://raw.githubusercontent.com/google/fonts/main/ofl/notocoloremoji/NotoColorEmoji-Regular.ttf}"
: "${IMSLUG_EMOJI_SHA256:=4d82a18d8d95f60ba883ce242bbadbf84a576e987745dd7ba38d71c67cff2d73}"
: "${IMSLUG_EMOJI_LICENSE_URL:=https://raw.githubusercontent.com/google/fonts/main/ofl/notocoloremoji/OFL.txt}"

if [ -f "$OUT" ]; then
    echo "[emoji-font] $OUT already present - nothing to do."
    exit 0
fi

if [ -n "${IMSLUG_EMOJI_SOURCE:-}" ]; then
    SRC="$IMSLUG_EMOJI_SOURCE"
else
    mkdir -p "$OUT_DIR/.cache"
    SRC="$OUT_DIR/.cache/NotoColorEmoji-Regular.ttf"
fi

if [ ! -f "$SRC" ]; then
    echo "[emoji-font] downloading $IMSLUG_EMOJI_URL"
    if command -v curl >/dev/null 2>&1; then
        curl -sSL --fail -o "$SRC" "$IMSLUG_EMOJI_URL" || { rm -f "$SRC"; echo "[emoji-font] download failed; skipping emoji font fetch."; exit 0; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$SRC" "$IMSLUG_EMOJI_URL" || { rm -f "$SRC"; echo "[emoji-font] download failed; skipping emoji font fetch."; exit 0; }
    else
        echo "[emoji-font] neither curl nor wget is available; skipping emoji font fetch."
        exit 0
    fi
fi

PYTHON=${IMSLUG_PYTHON:-python3}
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "[emoji-font] python 3 is unavailable; skipping the SHA-256 check and copy."
    exit 0
fi
src_hash=$("$PYTHON" - "$SRC" <<'PY'
import hashlib
import sys
with open(sys.argv[1], "rb") as handle:
    print(hashlib.sha256(handle.read()).hexdigest())
PY
)
if [ "$src_hash" != "$IMSLUG_EMOJI_SHA256" ]; then
    echo "[emoji-font] ERROR: source SHA-256 mismatch: got $src_hash" >&2
    echo "[emoji-font]   expected $IMSLUG_EMOJI_SHA256 - refusing to copy." >&2
    exit 1
fi

cp "$SRC" "$OUT"
if command -v curl >/dev/null 2>&1; then
    curl -sSL --fail -o "$OUT_DIR/OFL.txt" "$IMSLUG_EMOJI_LICENSE_URL" 2>/dev/null || true
fi
echo "[emoji-font] wrote $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)."
