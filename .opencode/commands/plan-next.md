---
description: Summarize the next unfinished work item from the briefs + PLAN §7 (no implementation)
---

Report the next work item. Do not start implementing it, do not edit any file.

1. Read `.agent-docs/PLAN.md` §7 only (the work queue: contents, acceptance
   criteria, estimate) - never the whole plan.
2. Read the briefs under `.agent-docs/milestones/` and pick the first item that
   is not `done` (its brief's status line tells you). If the requested item is
   appended at the end of this prompt, summarize that one instead.
3. Read that brief in full, plus `.agent-docs/PLAN.md` §11
   (decisions) and any other section the brief names.

Summarize (≤ ~40 lines):

- item title, status, estimate;
- mission in one sentence;
- deliverables (bullets, with paths);
- acceptance criteria and the verification commands;
- dependencies on earlier work and the files it must have produced;
- open decisions or owner questions the brief flags;
- anything in the completion notes of the previous brief that affects it.

End with: "Start it with: read AGENTS.md, then this brief." Do not write files.
