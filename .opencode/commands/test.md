---
description: Build + run the ctest suite via scripts/test
---

The canonical test entry point (builds first, then ctest) just ran:

!`scripts/test.bat`

Report:

- ctest summary: tests run/passed/failed;
- for every failure: test name, the relevant output, and the likely cause;
- whether it is a new regression or a pre-existing failure (check git status /
  recent changes).

Fix regressions and re-run. Never disable a failing test to make the suite
pass - report it instead. Arguments you passed to `/test` (e.g. `-R smoke`) are
appended at the end of this prompt: re-run `scripts/test.bat <args>` yourself
to apply them.

On Linux/macOS the entry point is `scripts/test.sh`.
