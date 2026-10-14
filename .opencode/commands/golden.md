---
description: Run the golden-image tests
---

Golden-image test entry point just ran:

!`scripts/golden.bat`

The harness renders fixed strings at sizes 12/16/32/64 and scales
1.0/1.25/2.0 (plus a 3.5x zoom case) offscreen through the real Vulkan
pipeline and compares every pixel against the CPU reference rasterizer; both
the classic render-pass and the dynamic-rendering (core Vulkan 1.3) paths run.
It skips with exit 77 when no usable Vulkan device exists.

Report the per-case line (max/mean/outliers) and the validation result. On
failure the PPMs land in `<build>/golden/` (`_gpu`, `_cpu`, `_diff`); report
the case, the worst pixel coordinates, and whether the difference is a
discontinuity tie (a sample landing on a control-point coordinate, where the
reference's sign-bit root eligibility legitimately resolves either way) or a
real rendering difference. Never approve a golden change by eye - a threshold
change requires measurements and a recorded decision (AGENTS.md invariant 9).

On Linux/macOS the entry point is `scripts/golden.sh`.
