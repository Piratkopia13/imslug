#!/bin/sh
# ============================================================================
# imslug - scripts/golden.sh
#
# Golden-image tests (offscreen Vulkan rendering, compared against the CPU
# reference). Not available until M2 (standalone Vulkan renderer).
#
# This is a non-fatal stub in M0: it exits 0 with a notice so CI and the
# /golden command keep working. Reproduce/update goldens with the same script
# once M2 lands; never eyeball-approve a golden diff.
# ============================================================================
echo "[golden] golden-image tests are available from M2 - nothing to do."
exit 0
