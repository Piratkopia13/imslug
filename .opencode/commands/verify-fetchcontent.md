---
description: Clean-project FetchContent verification (consume imslug as a dependency, build the example, run it)
---

The clean-project verification just ran:

!`scripts/verify_fetchcontent.bat`

Report concisely:

- the cloned ref and the build result (pass/fail);
- the `fc_smoke` version line and the example's `--help` output;
- every error with the failing step - do not summarize failures away;
- if it failed, fix the cause and re-run.

This consumes the repository through `FetchContent` as a dependency (a git
clone of the committed HEAD), so commit before re-running when you changed
sources - uncommitted changes are not part of the clone. Set
`IMSLUG_FC_SCREENSHOT=1` to additionally render one offscreen frame and check
the PPM. On Linux/macOS the entry point is `scripts/verify_fetchcontent.sh`.
