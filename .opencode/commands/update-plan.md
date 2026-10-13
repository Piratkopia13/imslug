---
description: Record a decision or status change in the transient plan files
---

Record the change in the transient plan. `.agent-docs/` is gitignored working
material: never commit it, never `git add -f` it, never move it to the repo root.

1. `.agent-docs/PLAN.md` §11 (decisions log): add an entry
   `D<n> (resolved/open, <today's date>):` with the decision, the options
   considered, and the rationale. Keep it factual and dated; do not rewrite
   history - supersede with a new entry.
2. The current milestone brief in `.agent-docs/milestones/`: update its status
   line; when the milestone ends, fill its §8 completion notes (status + date,
   deviations, what the next milestone must know, verification evidence with
   exact commands and results).
3. If durable docs changed, update them in the same change (README/AGENTS/
   `docs/**`); promoting a scratch doc means rewriting it under `docs/` and
   deleting the scratch copy.
4. Run `scripts/check_docs.*` afterwards when any markdown moved.

What to record: $ARGUMENTS
