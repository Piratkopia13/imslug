---
description: Run the golden-image tests (available from M2)
---

Golden-image test entry point just ran:

!`scripts/golden.bat`

Until the Vulkan renderer lands (M2) this is a non-fatal stub. Report what it
printed. Once goldens exist: run them with validation layers enabled, compare
against the CPU reference, and for a failure report the image id, the tolerance
used, and the max channel delta. Never approve a golden update by eye - a golden
change requires explaining the rendering difference.

On Linux/macOS the entry point is `scripts/golden.sh`.
