---
description: Configure + build imslug via scripts/build (Windows: .bat, Linux/macOS: .sh)
---

The canonical build entry point just ran:

!`scripts/build.bat`

Report concisely:

- configure/build result (pass/fail) and the resolved toolchain line
  (cmake path, generator, build dir);
- every compiler warning and error with `file:line` - do not summarize them away;
- if the build failed, fix the cause and re-run `scripts/build.bat`.

Never invent cmake invocations: documented pass-through arguments live in
`AGENTS.md` ("Commands"). If you invoked `/build` with arguments, they are
appended at the end of this prompt - apply them by re-running the script
yourself, e.g. `scripts/build.bat --target imslug_tests`.

On Linux/macOS the entry point is `scripts/build.sh` (same arguments).
