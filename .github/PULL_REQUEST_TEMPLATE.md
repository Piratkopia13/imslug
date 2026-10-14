## Summary

<!-- What does this change, and why? Link the milestone/issue if there is one. -->

## Verification

<!-- Commands you ran and their result (not "it builds"): -->

- [ ] `scripts/build.*` and `scripts/test.*` pass from a clean tree.
- [ ] New behavior is covered by tests; no invariant in `AGENTS.md` is violated.
- [ ] Shader sources + regenerated SPIR-V header are committed together and
      `scripts/gen_shaders.* --check` is clean (if shaders changed).
- [ ] The packed test asset is regenerated with `scripts/gen_asset.*` (if the
      generator/format/version changed).
- [ ] `scripts/check_docs.*` passes; no transient documents are tracked.
- [ ] `NOTICE`/attribution updated if third-party code was added.
- [ ] Durable docs (`docs/**`, `README.md`, `CHANGELOG.md`) updated if behavior,
      public API, defaults or limitations changed.

## Notes for the reviewer

<!-- Risks, follow-ups, deliberate deviations, decisions recorded outside the
     repo (the plan lives in the gitignored .agent-docs/). -->
