---
description: Summarize the next unfinished milestone from the briefs + PLAN §7 (no implementation)
---

Report the next milestone. Do not start implementing it, do not edit any file.

1. Read `.agent-docs/PLAN.md` §7 only (the milestone table: contents, acceptance
   criteria, estimate) - never the whole plan.
2. Read `.agent-docs/milestones/` and pick the first milestone that is not
   `done` (its brief's status line tells you). If the requested milestone is
   appended at the end of this prompt, summarize that one instead.
3. Read that milestone's brief in full, plus `.agent-docs/PLAN.md` §11
   (decisions) and any other section the brief names.

Summarize (≤ ~40 lines):

- milestone id + title, status, estimate;
- mission in one sentence;
- deliverables (bullets, with paths);
- acceptance criteria and the verification commands;
- dependencies on earlier milestones and files they must have produced;
- open decisions or owner questions the brief flags;
- anything in §8 completion notes of the previous milestone that affects it.

End with: "Start it with: read AGENTS.md, then this brief." Do not write files.
