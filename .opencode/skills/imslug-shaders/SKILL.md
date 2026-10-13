---
name: imslug shaders
description: Shader sources of truth, the glslangValidator workflow, the committed SPIR-V header and its freshness check, attribution headers, and golden tests. Load when editing shaders/**, running scripts/gen_shaders.*, or porting reference shader code.
---

# imslug shaders

## Sources of truth

| Path | Role |
|---|---|
| `shaders/vulkan/*.vert`, `*.frag` (later `*.comp`) | Authoritative GLSL (Vulkan dialect). Compiled by `scripts/gen_shaders.*`. |
| `shaders/reference/slug.hlsl` | Attributed port of Eric Lengyel's reference HLSL: logic notes and the port checklist, never compiled. |
| `shaders/imslug_shaders_vulkan.h` | **Generated + committed** SPIR-V word arrays. Never edited by hand. |

Generated `.spv` intermediates are gitignored; only the header is committed
together with its sources (PLAN §5.8).

## Regenerate / verify

* Regenerate: `scripts\gen_shaders.bat` (Windows) / `scripts/gen_shaders.sh`.
* Verify the committed bytes are fresh: add `--check` - it compiles into a temp
  file and diffs against the committed header; it fails when that header is
  missing or stale. Fix by regenerating and committing the header **with** the
  sources.
* Tool resolution: `GLSLANG_VALIDATOR` env var, then `$VULKAN_SDK/Bin` (Windows)
  / `$VULKAN_SDK/bin` (Linux), then `PATH`. A missing tool fails with an
  actionable message.
* Invocation used by both scripts (keep in sync):
  `glslangValidator -V --target-env vulkan1.2 -o <tmp>/<name>.<stage>.spv <src>`
* The emitter (`scripts/emit_shader_header.py`) writes, per module,
  `imslug_<stem>_<stage>_spv[]` plus `..._spv_len` (word count, not bytes).
  Output is deterministic: sorted inputs, no timestamps, LF, 8 words per line.
  A diff that churns without a source change is a generator bug - report it,
  never hand-patch the header.

## Attribution rules (hard requirement: PLAN §1.2, R10)

* Every shader source file starts with:
  `// Slug shader code Copyright 2017 by Eric Lengyel.`
  plus a short note on what was ported/adapted and under which license
  (the reference is dual MIT / Apache-2.0; both texts live in `NOTICE`).
* Port checklist (PLAN Appendix B): `SlugUnpack`/`SlugDilate` (perspective-correct
  1 px dilation via MVP rows), `CalcBandLoc` (log2 width 12 / 4096), band header
  fetch (H then V offsets), `CalcRootCode` equivalence classes + linear fallback
  when `|a| < 1/65536`, `SolveHorizPoly`/`SolveVertPoly` winding accumulation with
  coverage weights, `CalcCoverage` combine + even-odd flag (bit 12 of the packed
  bandMax word), instance-mode VS (corner from `gl_VertexIndex`, normal = corner
  direction), ImGui-exact blend state, discard below `fill < 0.001`.
* Do not copy code from projects without a license (`osgSlug`). DearWidgets is
  CC0 and Slughorn is MIT - adapting anything requires a `NOTICE` entry and a
  credit line in the file header.

## Verification after shader changes

1. `scripts/gen_shaders.*` - must succeed.
2. `scripts/gen_shaders.* --check` - must pass on a clean tree.
3. Golden-image tests (M2+, `scripts/golden.*`): fixed strings at multiple sizes
   and DPI densities, compared against the CPU reference; run with
   `VK_LAYER_KHRONOS_validation` and report the first validation error verbatim.
4. Fallback check: unregistered fonts still render through ImGui's texture path
   (the non-Slug path must never depend on shader changes).
