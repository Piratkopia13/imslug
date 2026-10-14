# AGENTS.md — imslug

Slug-based GPU text rendering for Dear ImGui. Vulkan-first, drop-in (no ImGui
fork), targets ImGui ≥ 1.92.8. **Read this file first and keep it accurate.**

## Project map

| Path | What it is |
|---|---|
| `README.md` / `CHANGELOG.md` | Landing page + release history (Keep a Changelog; the `imslug-release` skill holds the release/tag checklist). |
| `imslug.h` / `imslug.cpp` | Public API + core. Header must stay C++11-compatible and STL-free. |
| `imslug_c.h` / `imslug_c.cpp` | C API (M6): `extern "C"` mirror of the public API, C11-clean; the Vulkan C mirror is `backends/imslug_c_vulkan.cpp`. Binding guide: `docs/BINDINGS.md`. |
| `imslug_internal.h` | Impl-only frozen core API: pools, glyph records, font/metrics/raster declarations (M1; umbrella over the module headers). |
| `imslug_font.{h,cpp}` | Font-handle dispatch (M7): kind tag, shared glyph-record cache, shared band/curve builder; always compiled (no stbtt). |
| `imslug_glyphgen.{h,cpp}` | stbtt outline extraction → em-space quadratics, band construction, TTF font handle + ImGui-parity metrics (M1; compiled only with `IMSLUG_TTF_PARSER=ON`). |
| `imslug_packed.{h,cpp}` | `.imslug` container v1 reader/validator + packed font kind (M7); always compiled. Format spec: `docs/PACKED_FORMAT.md`. |
| `imslug_packer.{h,cpp}` | Shared curve/band pools: allocation, row rules, growth, dirty rows, capacity guards; pure band-construction helpers (M1; moved in M7). |
| `imslug_refraster.{h,cpp}` | CPU coverage rasterizer — the arbiter for M2+ golden tests (M1). |
| `imslug_stb.cpp` | Single TU instantiating vendored stb_truetype; own third-party warning target. |
| `third_party/stb/` | Vendored upstream stb (read-only; `PROVENANCE.txt` pins commit + SHA-256). |
| `backends/` | Backend modules: `imslug_impl_vulkan.*` (M2 standalone Vulkan core, ImGui-free); the M3 ImGui adapter lives here too. |
| `shaders/` | Authoritative GLSL sources (M2) + committed generated SPIR-V header `imslug_shaders_vulkan.h`. |
| `tools/imslug_gen/` | Offline generator CLI + `.imslug` writer (M7). |
| `example/` | Example applications (M2+; scene 9 is the packed-asset demo). |
| `tests/` | Unit tests (M1+), CPU reference rasterizer, golden images (M2+), test fonts under `tests/fonts/`, committed generated assets under `tests/assets/`. |
| `scripts/` | Deterministic build/test/generate entry points (see Commands). |
| `.opencode/` | Commands, repo skills and subagents for agent sessions. |
| `.agent-docs/` | Transient agent working material (plan, briefs, notes). **Gitignored — never committed.** |
| `docs/` | Durable user documentation (allowlist; promoted deliberately, never a side effect). |

## Current milestone

* Work queue/status: `.agent-docs/PLAN.md` §7. Decisions log: §11.
* Your brief: the **lowest-numbered** file in `.agent-docs/milestones/` whose
  `Status:` line is not `done` (the initial commit shipped M0 = repo bootstrap;
  M1 (glyph pipeline), M2 (standalone Vulkan renderer), M3 (ImGui integration),
  M4 (robustness/perf/polish), M5 (color fonts), M6 (C API + Rust bindings) and
  M7 (offline generator + `.imslug` interop) are done, so M8 = release polish is
  next), unless the owner assigned you a specific milestone.
  Read **only** that brief — never the whole plan.
* `.agent-docs/` is gitignored transient material: never `git add -f` it, never
  write scratch markdown anywhere else (see Docs policy).

## Commands (exact — never guess flags)

Windows (cmd or PowerShell) / Linux + macOS (POSIX sh):

| Task | Windows | Linux / macOS |
|---|---|---|
| Configure + build | `scripts\build.bat` | `scripts/build.sh` |
| Build + run ctest | `scripts\test.bat` | `scripts/test.sh` |
| Regenerate shader header | `scripts\gen_shaders.bat` | `scripts/gen_shaders.sh` |
| Verify shaders are fresh | `scripts\gen_shaders.bat --check` | `scripts/gen_shaders.sh --check` |
| Regenerate the packed test asset (M7) | `scripts\gen_asset.bat` | `scripts/gen_asset.sh` |
| Golden-image tests (M2+) | `scripts\golden.bat` | `scripts/golden.sh` |
| Clean-project FetchContent check (M8) | `scripts\verify_fetchcontent.bat` | `scripts/verify_fetchcontent.sh` |
| Benchmarks (M4+) | `scripts\bench.bat` | `scripts/bench.sh` |
| Docs-hygiene check | `scripts\check_docs.bat` | `scripts/check_docs.sh` |
| clang-format our sources | `scripts\format.bat` (`--check`) | `scripts/format.sh` (`--check`) |

* Extra arguments pass through: `scripts\build.bat --target imslug_tests` /
  `scripts/build.sh --target imslug_tests`, `scripts\test.bat -R smoke` /
  `scripts/test.sh -R smoke`.
* `scripts/emit_shader_header.py` is internal to `gen_shaders` — call the script,
  never the emitter directly.
* Overrides: `IMSLUG_BUILD_DIR` (default `build`), `IMSLUG_BUILD_TYPE`
  (default `Release`), `IMSLUG_RECONFIGURE=1` (force configure),
  `IMSLUG_CMAKE=<path to cmake>`, `IMSLUG_CMAKE_DEFINES="-D<var>=<value> ..."`
  (configure-time CMake options, forces a reconfigure; needed on Windows where
  cmd.exe splits `-Dvar=value` arguments).
* `IMSLUG_BUILD_EXAMPLES=ON` (configure option) builds the GLFW+Vulkan example
  (`build/imslug_example_glfw_vulkan.exe`; GLFW is fetched at a pinned tag or
  taken from `IMSLUG_GLFW_DIR`). It is an ImGui app with a scene switcher
  (keys 1–9, 1-based: demo, widgets/plots/tables, clipping torture, mixed fonts,
  runtime `FontScaleDpi`, 5k-glyph stress, CJK subset, color emoji, packed
  `.imslug` asset), F1/F2/F3 debug-overlay toggles (glyph bounds / band grid /
  ref heat, `--overlay` on the CLI) and `--screenshot <file.ppm> [scene]
  [dpi_scale]` for deterministic offscreen captures (scene numbers are 1-based
  like the keys). `--font <path>` accepts a TTF or an `.imslug` asset (the
  loader auto-detects); in a no-parser build the default font is the committed
  asset.
* `IMSLUG_TTF_PARSER` (configure option, default ON) builds the runtime TTF
  outline parser (stb_truetype). `-DIMSLUG_TTF_PARSER=OFF` builds the core
  **without stbtt** (packed `.imslug` assets only; TTF font sources fail
  cleanly): the packed tests, the packed goldens and the example still build and
  run (the example defaults to `tests/assets/Roboto-ASCII.imslug`).
* `IMSLUG_BUILD_TOOLS` (configure option, default ON) builds the `imslug_gen`
  CLI + its writer library (`build/imslug_gen.exe`); tests use the writer for
  the round-trip suite. The committed generated asset is
  `tests/assets/Roboto-ASCII.imslug`, regenerated with `scripts/gen_asset.*`
  (format spec: `docs/PACKED_FORMAT.md`; freshness enforced by
  `imslug_packed_roundtrip` + `imslug_asset_fresh`).
* `IMSLUG_BUILD_C_EXAMPLE` (configure option, default ON) builds the pure-C11
  example (`build/imslug_example_c.exe`): the default run is a CPU smoke
  (context, init, add font, one headless frame, shutdown; ctest
  `imslug_c_example`), `--vulkan` renders one offscreen frame through the C API
  (ctest `imslug_c_example_vulkan`, skips with exit 77 without a device).
  `IMSLUG_BUILD_C_EXAMPLE=OFF` skips it (e.g. in embedded/CI CPU-only builds).
* Fetched, gitignored test assets (optional; suites skip when absent):
  `scripts/fetch_cjk_font.*` (Noto Sans SC subset, M4) and
  `scripts/fetch_emoji_font.*` (Noto Color Emoji, COLR v1, M5). Provenance and
  pinned SHA-256 live next to them in `tests/fonts/*/PROVENANCE.txt`.
* `.opencode/commands/` mirrors these commands: `/build`, `/test`,
  `/gen-shaders`, `/gen-asset`, `/golden`, `/verify-fetchcontent`, `/bench`,
  `/check-docs`, `/plan-next`, `/update-plan`.
* Golden images run in a headless offscreen pass (`tests/golden/`, PPMs in
  `<build>/golden/`) and skip with exit 77 when no Vulkan device is available:
  the M2 tests compare GPU output against the M1 CPU reference; the M3 test
  (`imslug_imgui_golden`, both render paths) renders one ImGui scene with a Slug
  font and with the atlas font and compares them (layout within 1 px, AA
  perceptual).
* If a workflow is missing a script, add one under `scripts/` (and a
  `.opencode/commands/` entry) instead of inventing ad-hoc commands.

## Invariants (do not violate)

1. **Never hand-edit generated artifacts.** `shaders/imslug_shaders_vulkan.h`
   is generated by `scripts/gen_shaders.*` and committed; regenerate it.
   `tests/assets/Roboto-ASCII.imslug` is generated by `scripts/gen_asset.*`
   (`imslug_gen`) and committed; regenerate it (freshness is test-enforced).
2. **Never modify third-party sources** (Dear ImGui, stb, GLFW): vendored or
   fetched trees are read-only. Adapt through our own files.
3. **Never remove or weaken `NOTICE` attribution** (Lengyel credit + MIT and
   Apache-2.0 texts, stb, prior art). Attribution is a hard requirement
   (PLAN §1.2, R10) and every ported shader carries its attribution header.
4. **No STL in public headers**; `imslug.h` compiles as C++11. Implementation is
   C++17, no exceptions in our code, warnings are not suppressed.
5. **Match ImGui style** (4-space indent, Allman braces, 120 columns; see
   `.clang-format`). Do not reformat third-party code.
6. **The non-Slug fallback path must keep working**: fonts not registered with
   imslug, and overflow/degenerate cases, render through stock ImGui.
7. **Core stays backend-independent** — Vulkan/ImGui-specific code lives in
   `backends/` and adapter files only.
8. **Shader sources and their regenerated SPIR-V header are committed together**
   and must be deterministic.
9. **Record decisions** in `.agent-docs/PLAN.md` §11 (date + rationale).

## Docs policy (PLAN §12.5)

* Committed markdown allowlist: `README.md`, `AGENTS.md`, `CHANGELOG.md`,
  `docs/**`, `.opencode/**`, `.github/**`; plus `LICENSE`/`NOTICE`
  (non-markdown).
* Transient plan drafts, session notes and scratch go in `.agent-docs/` **only**
  — never at the repo root or anywhere else.
* Never commit `.agent-docs/**`; never `git add -f` ignored files.
* Promoting a scratch doc into `docs/` is a deliberate, reviewed step: rewrite
  it in `docs/` and delete the scratch copy (never leave both).
* `scripts/check_docs.*` enforces this; run it before finishing.

## Session procedure

1. **Start:** read this file + your milestone brief; check PLAN §7/§11; list
   available skills (`skill` tool) and load the ones the task needs. Load the
   `opencode` skill **before** touching `.opencode/` or `opencode.jsonc`.
2. **Baseline:** run `scripts/test.*` before edits. After shader edits run
   `scripts/gen_shaders.* --check`; after backend edits run the
   validation-layer smoke test.
3. **During:** keep changes milestone-scoped; use subagents (`general`,
   `explore`, `plan`, and the repo agents in `.opencode/agents/`) for isolated
   research/test work; extend the repo skills/scripts instead of one-off hacks.
4. **Finish:** run `scripts/build.*`, `scripts/test.*`, `scripts/check_docs.*`
   (plus `scripts/gen_shaders.* --check` after shader work); update the plan and
   the milestone brief's completion notes; leave the repo green.
5. **Blocked?** Ask the repo owner. If the harness itself misbehaves, use the
   `report` skill instead of silently working around it. Never push, tag or
   publish without an explicit request.

## Definition of done

- [ ] `scripts/build.*` and `scripts/test.*` pass from a clean tree.
- [ ] New behavior is covered by tests; no invariant above is violated.
- [ ] `scripts/check_docs.*` passes (no transient docs added).
- [ ] `NOTICE`/attribution updated if third-party code was added.
- [ ] Shaders: sources + regenerated header committed together, `--check` clean.
- [ ] Plan updated (decisions §11; milestone brief §8 when the milestone ends).

## Repo skills

* `imslug-glyph-format` — curve/band data invariants, packing, reference tests.
* `imslug-shaders` — shader sources of truth, glslangValidator workflow, SPIR-V
  freshness check, attribution rules.
* `imslug-vulkan-backend` — Vulkan pipeline/descriptor/callback contract and
  validation expectations.
* `opencode` (built-in) — required before editing `.opencode/` or
  `opencode.jsonc`.
