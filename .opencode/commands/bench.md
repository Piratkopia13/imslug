---
description: Run the benchmark harness (M4+)
---

Benchmark entry point just ran:

!`scripts/bench.bat`

The harness prints and records machine-specific numbers (CPU pre-pass, pool
uploads, lazy glyph build, R8 small-size table; GPU timestamp queries with
`--gpu`); the markdown report lands in `<build dir>\bench\bench_report.md`.
Budgets are informational — a red number is a finding, not a build failure.

Always report the build type and machine details (CPU/GPU/driver) with the
numbers, keep runs comparable (same scene, same build type, same flags), prefer
repeated runs with medians over single samples, and never quote a GPU row whose
`instances` column is 0 (that row did not render Slug text; the harness warns).
The manual procedure is in `docs/DESIGN.md` ("Measuring performance").

On Linux/macOS the entry point is `scripts/bench.sh`.
