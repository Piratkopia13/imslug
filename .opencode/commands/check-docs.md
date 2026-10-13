---
description: Enforce the docs policy (no transient markdown, .agent-docs/ ignored)
---

Docs-hygiene check just ran:

!`scripts/check_docs.bat`

It fails when (a) anything under `.agent-docs/` is tracked by git, (b)
`.gitignore` lost its `.agent-docs/` entry, or (c) markdown exists outside the
durable allowlist (`README.md`, `AGENTS.md`, `docs/**`, `.opencode/**`,
`.github/**`; vendored trees excluded).

If it failed: move transient notes into `.agent-docs/` (or delete them), and
never `git add -f` ignored files. If a scratch document has matured into
durable documentation, rewrite it under `docs/` and delete the scratch copy -
never leave both. Re-run until it passes, then report the final line.

On Linux/macOS the entry point is `scripts/check_docs.sh`.
