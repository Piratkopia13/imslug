---
description: Run the benchmark harness (available from M4)
---

Benchmark entry point just ran:

!`scripts/bench.bat`

Until M4 this is a non-fatal stub. Report what it printed. Once benchmarks
exist: always report the build type and machine details (CPU/GPU/driver) with
the numbers, keep runs comparable (same scene, same build type), and prefer a
few repeated runs with median/min over single samples. Benchmark failures must
not fail the build; regressions are reported to the owner.

On Linux/macOS the entry point is `scripts/bench.sh`.
