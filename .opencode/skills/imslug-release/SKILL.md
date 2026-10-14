---
name: imslug release
description: Release checklist for imslug - version sources of truth (imslug.h/imslug_c.h/CMakeLists.txt), CHANGELOG/README state, regenerated-artifact freshness (packed asset, shader header), the full verification matrix, NOTICE/attribution audit, and the owner-approved tag procedure. Load when preparing, tagging or documenting a release.
---

# imslug release checklist

Everything a release touches, in order. Version/changelog/tag mechanics live
here because they are cross-cutting; the data-format and shader contracts have
their own skills (`imslug-glyph-format`, `imslug-shaders`,
`imslug-vulkan-backend`).

## Version sources of truth (keep in sync)

| Where | What | Notes |
|---|---|---|
| `imslug.h` | `IMSLUG_VERSION_MAJOR/MINOR/PATCH`, `IMSLUG_VERSION_NUM`, `IMSLUG_VERSION` | canonical; `NUM = MAJOR*10000 + MINOR*100 + PATCH` |
| `imslug_c.h` | the same macros, `#ifndef`-guarded mirror | must match `imslug.h` |
| `CMakeLists.txt` | `project(imslug VERSION x.y.z)` | keep in sync |
| `CHANGELOG.md` | the Keep a Changelog entry for the release | root file; the docs allowlist (`scripts/check_docs.*`) includes it |
| `README.md` | status paragraph | update at release time |

A release version has **no `-dev` suffix**; the next development cycle either
keeps the released value until the next release prep or bumps to the next
version with a `-dev` suffix. `v0.1.0` ships `IMSLUG_VERSION "0.1.0"`.

## Artifacts that embed the version

* `tests/assets/Roboto-ASCII.imslug` stores `IMSLUG_VERSION_NUM` as
  `GeneratorVersion`. **Bumping `NUM` makes `imslug_asset_fresh` and
  `imslug_packed_roundtrip` fail until `scripts/gen_asset.*` regenerates the
  asset and it is committed** — intended, same discipline as the shader
  header.
* `shaders/imslug_shaders_vulkan.h` carries no version, but sources and the
  regenerated header must always be committed together
  (`scripts/gen_shaders.* --check` enforces it).

## Release procedure

1. **Scope/state.** `main` is feature-complete for the release, the tree is
   clean, and the plan's status/decision notes are current.
2. **Version.** Update the five rows above. Bump PATCH for fixes, MINOR for
   additive (ABI-compatible) features, MAJOR only with owner approval; in 0.x,
   a breaking change bumps MINOR.
3. **Artifacts.** If `NUM` changed: `scripts/gen_asset.*`. If shaders changed:
   `scripts/gen_shaders.*`. Commit generated files together with their inputs.
4. **Verification (all green; exact commands):**
   * `scripts/build.*`
   * `scripts/test.*` — all suites (Release and a Debug build with asserts,
     `IMSLUG_BUILD_DIR=build-debug IMSLUG_BUILD_TYPE=Debug`)
   * `scripts/golden.*` — GPU-vs-CPU golden images (skips without a device)
   * `scripts/gen_shaders.* --check` — committed SPIR-V is fresh
   * `scripts/gen_asset.*` — the committed packed asset is byte-fresh
   * `scripts/check_docs.*` — no transient docs, allowlist intact
   * `scripts/format.* --check`
   * `scripts/verify_fetchcontent.*` — clean-project FetchContent build
   * `scripts/bench.*` — informational; move the numbers in `docs/DESIGN.md`
     §4 when they materially change
   * the example with validation on (`--screenshot` for a deterministic frame)
   * `scripts/smoke_imgui_master.*` — ImGui-master drift check (informational,
     needs network; never a release gate)
5. **NOTICE/attribution audit.** Lengyel credit + MIT/Apache texts, stb, the
   prior-art projects, the test-font licenses, the trademark/affiliation
   disclaimer. **Never remove `NOTICE`.** Update it if third-party code or
   fonts were added.
6. **Owner approval, then tag.** Never tag or publish unilaterally (the
   project config asks on `git tag` / `git push` / `gh release`). The tag is
   annotated and points at the release commit:
   ```
   git tag -a vX.Y.Z -m "imslug vX.Y.Z"
   ```
   There is no remote; never push without an explicit owner request. Release
   notes are drafted from `CHANGELOG.md` (transient drafts live in
   `.agent-docs/reports/`, never committed).
7. **Post-tag.** The tree stays clean and the tag must point at the commit
   whose headers say `X.Y.Z`. Moving a tag is an owner decision
   (`git tag -d vX.Y.Z` + re-create) — never do it silently.

## Changelog rules

* Root `CHANGELOG.md`, newest entry first, sections per Keep a Changelog:
  Added / Changed / Fixed / Known limitations.
* Describe user-visible behavior and the decision behind it (not the
  plan's internal wording); never paste `.agent-docs/` wholesale.
* While a release is prepared but untagged, mark the entry
  "(release prepared; tag pending owner)" and note the exact tag command
  alongside the plan entry.

## What CI enforces (mirror these locally)

| Job | Covers |
|---|---|
| Windows (MSVC) | build, full ctest, shader freshness, docs hygiene |
| Linux (GCC/Clang) | same + fetched CJK/emoji fonts (fuzz at the CI profile) |
| Linux golden (lavapipe) | golden images both render paths + C example Vulkan frame |
| No-parser | `IMSLUG_TTF_PARSER=OFF` build, packed suites/goldens, example |
| Clean-project FetchContent | throwaway project consumes the repo, builds + runs the example |
| Benchmark report (informational) | `scripts/bench.* --cpu` artifact |
| ImGui-master smoke (daily, informational) | internal-API drift against ImGui `master` |
