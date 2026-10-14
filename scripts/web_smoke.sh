#!/bin/sh
# ============================================================================
# imslug - scripts/web_smoke.sh
#
# Browser smoke for the assembled web demo: serves <build dir>/site/ and drives
# headless Chrome through scripts/web_smoke/smoke.mjs (see that file for the
# checks and the IMSLUG_SMOKE_* overrides). CI: the Pages workflow's smoke
# step; runs there against the software WebGPU adapter.
#
#   scripts/web_smoke.sh
#
# Requirements: Node >= 22 with npm (installs puppeteer-core into
# scripts/web_smoke/node_modules on first use) and Chrome/Chromium - CI images
# ship Google Chrome; set IMSLUG_SMOKE_CHROME otherwise.
#
# Overrides: see scripts/web_smoke/smoke.mjs; IMSLUG_BUILD_DIR selects the
# build directory (default: build-web).
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

if ! command -v node >/dev/null 2>&1; then
    echo "[web_smoke] ERROR: node not found (SMOKE needs Node >= 22)." >&2
    exit 1
fi

if [ ! -d scripts/web_smoke/node_modules ]; then
    echo "[web_smoke] installing smoke dependencies (puppeteer-core)"
    (cd scripts/web_smoke && npm ci --no-audit --no-fund) || exit 1
fi

: "${IMSLUG_SMOKE_SITE:=${IMSLUG_BUILD_DIR:-build-web}/site}"
export IMSLUG_SMOKE_SITE

exec node scripts/web_smoke/smoke.mjs
