# AGENTS.md — imslug

Slug-based GPU text rendering for Dear ImGui. Vulkan-first, drop-in (no ImGui
fork), targets ImGui ≥ 1.92.8. **Read this file first and keep it accurate.**

## Project map

| Path | What it is |
|---|---|
| `README.md` / `CHANGELOG.md` | Landing page + release history (Keep a Changelog; the `imslug-release` skill holds the release/tag checklist). |
| `imslug.h` / `imslug.cpp` | Public API + core. Header must stay C++11-compatible and STL-free. |
| `imslug_c.h` / `imslug_c.cpp` | C API: `extern "C"` mirror of the public API, C11-clean; the Vulkan C mirror is `backends/imslug_c_vulkan.cpp`. Binding guide: `docs/BINDINGS.md`. |
| `imslug_internal.h` | Impl-only frozen core API: pools, glyph records, font/metrics/raster declarations (umbrella over the module headers). |
| `imslug_font.{h,cpp}` | Font-handle dispatch: kind tag, shared glyph-record cache, shared band/curve builder; always compiled (no stbtt). |
| `imslug_glyphgen.{h,cpp}` | stbtt outline extraction → em-space quadratics, band construction, TTF font handle + ImGui-parity metrics (compiled only with `IMSLUG_TTF_PARSER=ON`). |
| `imslug_packed.{h,cpp}` | `.imslug` container v1 reader/validator + packed font kind; always compiled. Format spec: `docs/PACKED_FORMAT.md`. |
| `imslug_packer.{h,cpp}` | Shared curve/band pools: allocation, row rules, growth, dirty rows, capacity guards; pure band-construction helpers shared by all font kinds. |
| `imslug_refraster.{h,cpp}` | CPU coverage rasterizer — the arbiter for the golden tests. |
| `imslug_stb.cpp` | Single TU instantiating vendored stb_truetype; own third-party warning target. |
| `third_party/stb/` | Vendored upstream stb (read-only; `PROVENANCE.txt` pins commit + SHA-256). |
| `backends/` | Backend modules: `imslug_impl_vulkan.*` (standalone Vulkan core, ImGui-free; the ImGui adapter lives here too) and `imslug_impl_webgpu.*` (the WebGPU mirror, browser/Emscripten). |
| `shaders/` | Authoritative GLSL sources + committed generated SPIR-V header `imslug_shaders_vulkan.h`; the hand-maintained WGSL port under `wgsl/` + its committed generated header `imslug_shaders_webgpu.h`; mapping table in `reference/wgsl-port.txt`. |
| `tools/imslug_gen/` | Offline generator CLI + `.imslug` writer. |
| `example/` | Example applications: the shared app `common/example_app.*` (all scenes incl. the packed-asset demo), the desktop shell `glfw_vulkan/` and the browser shell `web/` (Emscripten + WebGPU; page: `example/web/shell.html`). |
| `tests/` | Unit tests, CPU reference rasterizer, golden images, test fonts under `tests/fonts/`, committed generated assets under `tests/assets/`. |
| `scripts/` | Deterministic build/test/generate entry points (see Commands). |
| `.opencode/` | Commands, repo skills and subagents for agent sessions. |
| `.agent-docs/` | Transient agent working material for tasks in flight. **Gitignored — never committed; delete finished material.** |
| `docs/` | Durable user documentation (allowlist; promoted deliberately, never a side effect). |

## Status

* v0.1.0 release prepared: version macros final, `CHANGELOG.md` entry marked
  "tag pending owner"; the owner holds the `v0.1.0` tag (see the
  `imslug-release` skill for the exact preconditions and the tag command).
* The WebGPU backend + browser demo are landed in `Unreleased`
  (`backends/imslug_impl_webgpu.*`, `shaders/wgsl/`, `scripts/build_web.*`,
  `scripts/web_smoke.*`); `.github/workflows/pages.yml` deploys the demo to
  GitHub Pages on every push to `main` (the README live-demo link).
* Work is **features and bug-fixes**, not milestone-numbered plans: name the
  change after what it does (commit subject, tests, docs) and record the
  rationale in the commit. There is no milestone queue or task brief, and new
  work is never appended as an "M12"-style item.
* `.agent-docs/` is gitignored transient material for in-flight work only:
  never `git add -f` it, never write scratch markdown anywhere else, and delete
  a task's files when the task lands (see Docs policy).

## Commands (exact — never guess flags)

Windows (cmd or PowerShell) / Linux + macOS (POSIX sh):

| Task | Windows | Linux / macOS |
|---|---|---|
| Configure + build | `scripts\build.bat` | `scripts/build.sh` |
| Build + run ctest | `scripts\test.bat` | `scripts/test.sh` |
| Build the web demo (Emscripten + Pages site) | `scripts\build_web.bat` | `scripts/build_web.sh` |
| Browser smoke (web demo) | `node scripts\web_smoke\smoke.mjs` | `scripts/web_smoke.sh` |
| Regenerate shader header | `scripts\gen_shaders.bat` | `scripts/gen_shaders.sh` |
| Verify shaders are fresh | `scripts\gen_shaders.bat --check` | `scripts/gen_shaders.sh --check` |
| Regenerate the packed test asset | `scripts\gen_asset.bat` | `scripts/gen_asset.sh` |
| Golden-image tests | `scripts\golden.bat` | `scripts/golden.sh` |
| Clean-project FetchContent check | `scripts\verify_fetchcontent.bat` | `scripts/verify_fetchcontent.sh` |
| Benchmarks | `scripts\bench.bat` | `scripts/bench.sh` |
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
  `.imslug` asset; key `P` = heavy demo / perf scene with a performance panel:
  frame-ms plot, whole-frame GPU timestamps, draw/Slug/atlas counters and a
  vsync toggle), an `M` toggle (or `--renderer atlas`) that swaps the body text
  between Slug and ImGui's dynamic font system for the same registered font,
  F1/F2/F3 debug-overlay toggles (glyph bounds / band grid / ref heat,
  `--overlay` on the CLI) and `--screenshot <file.ppm> [scene] [dpi_scale]` for
  deterministic offscreen captures (scene numbers are 1-based like the keys).
  `--font <path>` accepts a TTF or an `.imslug` asset (the loader auto-detects).
  The default fonts are embedded in the binary (the committed packed asset for
  Slug + the raw TTF for ImGui's stock atlas font, so `M` works standalone);
  `--font` or `IMSLUG_EXAMPLE_FONT` (env var; `--font` wins) overrides them.
  No data files are needed next to the executable. The CJK/emoji scenes still
  use the fetched test fonts when present.
* `IMSLUG_STATIC_CRT` (configure option, default OFF, MSVC only) links the C
  runtime statically (`/MT`, mirrored for GLFW): the example then needs no
  VC++ redistributable — a genuinely single-file executable whose only
  non-OS import is `vulkan-1.dll` (GPU driver). Provide it through
  `IMSLUG_CMAKE_DEFINES="-DIMSLUG_STATIC_CRT=ON"`; toggling it on an existing
  build directory forces a reconfigure of GLFW's runtime choice. It covers the
  targets this project builds: a caller-provided `glfw`/imgui target must use
  `/MT` itself (a configure-time AUTHOR_WARNING flags that case).
* `IMSLUG_BACKEND_WEBGPU` (configure option; default ON under Emscripten, OFF
  elsewhere) builds the WebGPU backend module + the third-party
  `imgui_impl_wgpu` target. `scripts\build_web.bat` / `scripts/build_web.sh` is
  the one web entry point: it sources `emsdk_env` when the emsdk root is known
  (emsdk ≥ 4.0.10; CI pins 5.0.1), embeds the fetched CJK and color-emoji
  subsets with the native `imslug_gen` when present
  (`-DIMSLUG_CJK_EMBED_HEADER=` / `-DIMSLUG_EMOJI_EMBED_HEADER=<build>/include/*.h`;
  the emoji subset is produced by `scripts/subset_emoji_for_web.py`, COLR v1
  only), configures `build-web/` through `emcmake` (`-G Ninja`; examples on,
  Vulkan, tests, C example and tools off) and assembles `build-web/site/` for
  GitHub Pages (`index.html`, `index.js`, `index.wasm`, `.nojekyll`).
  `scripts/web_smoke.sh` / `node scripts\web_smoke\smoke.mjs` serves that site
  and drives headless Chrome (software WebGPU adapter by default; waits for
  `window.__imslugReady` + a frame count, checks the screenshot for ink and the
  console for errors; a second hi-DPI pass at DPR 3 toggles the text renderer
  with `M` and requires the Slug and atlas frames to show the same ink, the
  phone/retina regression guard; pinned deps install into
  `scripts/web_smoke/node_modules` on first use). The shell is
  `example/web/main.cpp` over the shared app, the page is
  `example/web/shell.html`, the deploy workflow is
  `.github/workflows/pages.yml`.
* `IMSLUG_TTF_PARSER` (configure option, default ON) builds the runtime TTF
  outline parser (stb_truetype). `-DIMSLUG_TTF_PARSER=OFF` builds the core
  **without stbtt** (packed `.imslug` assets only; TTF font sources fail
  cleanly): the packed tests, the packed goldens and the example still build and
  run (the example's embedded defaults are the packed asset for Slug and the
  TTF for ImGui's own loader, which this option does not remove).
* `IMSLUG_BUILD_TOOLS` (configure option, default ON) builds the `imslug_gen`
  CLI + its writer library (`build/imslug_gen.exe`); tests use the writer for
  the round-trip suite. The committed generated artifacts are
  `tests/assets/Roboto-ASCII.imslug` and the two example headers
  `example/common/imslug_example_{asset,font}.h`, all regenerated with
  `scripts/gen_asset.*` (format spec: `docs/PACKED_FORMAT.md`; freshness
  enforced by `imslug_packed_roundtrip` + `imslug_asset_fresh`).
* `IMSLUG_BUILD_C_EXAMPLE` (configure option, default ON) builds the pure-C11
  example (`build/imslug_example_c.exe`): the default run is a CPU smoke
  (context, init, add font, one headless frame, shutdown; ctest
  `imslug_c_example`), `--vulkan` renders one offscreen frame through the C API
  (ctest `imslug_c_example_vulkan`, skips with exit 77 without a device).
  `IMSLUG_BUILD_C_EXAMPLE=OFF` skips it (e.g. in embedded/CI CPU-only builds).
* Fetched, gitignored test assets (optional; suites skip when absent):
  `scripts/fetch_cjk_font.*` (Noto Sans SC subset) and
  `scripts/fetch_emoji_font.*` (Noto Color Emoji, COLR v1). Provenance and
  pinned SHA-256 live next to them in `tests/fonts/*/PROVENANCE.txt`.
* `.opencode/commands/` mirrors these commands: `/build`, `/test`,
  `/gen-shaders`, `/gen-asset`, `/golden`, `/verify-fetchcontent`, `/bench`,
  `/build-web`, `/check-docs`.
* Golden images run in a headless offscreen pass (`tests/golden/`, PPMs in
  `<build>/golden/`) and skip with exit 77 when no Vulkan device is available:
  the monochrome tests compare GPU output against the CPU reference
  rasterizer; the ImGui test (`imslug_imgui_golden`, both render paths) renders
  one ImGui scene with a Slug font and with the atlas font and compares them
  (layout within 1 px, AA perceptual) - the default scene also at framebuffer
  scales 2x and 3x (`DisplayFramebufferScale > 1`, the phone/retina case where
  the renderer expands a logical-pixel layout to the framebuffer).
* If a workflow is missing a script, add one under `scripts/` (and a
  `.opencode/commands/` entry) instead of inventing ad-hoc commands.

## Invariants (do not violate)

1. **Never hand-edit generated artifacts.** `shaders/imslug_shaders_vulkan.h`
   (SPIR-V) and `shaders/imslug_shaders_webgpu.h` (WGSL) are generated by
   `scripts/gen_shaders.*` and committed; regenerate them (both are
   `--check`ed). `tests/assets/Roboto-ASCII.imslug` and the two example headers
   `example/common/imslug_example_{asset,font}.h` are generated by
   `scripts/gen_asset.*` (`imslug_gen`) and committed; regenerate them
   (freshness is test-enforced).
2. **Never modify third-party sources** (Dear ImGui, stb, GLFW): vendored or
   fetched trees are read-only. Adapt through our own files.
3. **Never remove or weaken `NOTICE` attribution** (Lengyel credit + MIT and
   Apache-2.0 texts, stb, prior art). Attribution is a hard requirement and
   every ported shader carries its attribution header.
4. **No STL in public headers**; `imslug.h` compiles as C++11. Implementation is
   C++17, no exceptions in our code, warnings are not suppressed.
5. **Match ImGui style** (4-space indent, Allman braces, 120 columns; see
   `.clang-format`). Do not reformat third-party code.
6. **The non-Slug fallback path must keep working**: fonts not registered with
   imslug, and overflow/degenerate cases, render through stock ImGui.
7. **Core stays backend-independent** — Vulkan/WebGPU/ImGui-specific code lives
   in `backends/` and adapter files only.
8. **Shader sources and their regenerated headers (SPIR-V + WGSL) are committed
   together** and must be deterministic.
9. **Record decisions durably**: the commit message states the change and its
   rationale; `docs/**`, `README.md` and `CHANGELOG.md` are updated when
   behavior, contracts, defaults or limitations change. Never park decisions in
   `.agent-docs/` (transient; cleaned up when a task lands).

## Docs policy

* Committed markdown allowlist: `README.md`, `AGENTS.md`, `CHANGELOG.md`,
  `docs/**`, `.opencode/**`, `.github/**`; plus `LICENSE`/`NOTICE`
  (non-markdown).
* In-flight notes, drafts, evidence and scratch go in `.agent-docs/` **only** —
  never at the repo root or anywhere else.
* `.agent-docs/` holds in-flight work only: when a task lands, delete its
  transient files so finished material does not accumulate. The durable record
  is the commit message plus `docs/**`/`CHANGELOG.md` where behavior changed.
* Never commit `.agent-docs/**`; never `git add -f` ignored files.
* Promoting a scratch doc into `docs/` is a deliberate, reviewed step: rewrite
  it in `docs/` and delete the scratch copy (never leave both).
* `scripts/check_docs.*` enforces this; run it before finishing.

## Session procedure

1. **Start:** read this file first; check `.agent-docs/` for any in-flight notes
   a previous session left; list available skills (`skill` tool) and load the
   ones the task needs. Load the `opencode` skill **before** touching
   `.opencode/` or `opencode.jsonc`.
2. **Baseline:** run `scripts/test.*` before edits. After shader edits run
   `scripts/gen_shaders.* --check`; after backend edits run the
   validation-layer smoke test.
3. **During:** keep changes task-scoped; use subagents (`general`,
   `explore`, `plan`, and the repo agents in `.opencode/agents/`) for isolated
   research/test work; extend the repo skills/scripts instead of one-off hacks.
4. **Finish:** run `scripts/build.*`, `scripts/test.*`, `scripts/check_docs.*`
   (plus `scripts/gen_shaders.* --check` after shader work); update durable docs
   where behavior changed; delete the transient `.agent-docs/` files this task
   created; leave the repo green.
5. **Blocked?** Ask the repo owner. If the harness itself misbehaves, use the
   `report` skill instead of silently working around it. Never push, tag or
   publish without an explicit request.

## Definition of done

- [ ] `scripts/build.*` and `scripts/test.*` pass from a clean tree.
- [ ] New behavior is covered by tests; no invariant above is violated.
- [ ] `scripts/check_docs.*` passes (no transient docs added).
- [ ] `NOTICE`/attribution updated if third-party code was added.
- [ ] Shaders: sources + regenerated header committed together, `--check` clean.
- [ ] Transient `.agent-docs/` files for this task deleted; decisions live in
      the commit message (and durable docs where behavior changed).

## Repo skills

* `imslug-glyph-format` — curve/band data invariants, packing, reference tests.
* `imslug-shaders` — shader sources of truth, glslangValidator workflow, SPIR-V
  freshness check, attribution rules.
* `imslug-vulkan-backend` — Vulkan pipeline/descriptor/callback contract and
  validation expectations.
* `imslug-webgpu-backend` — WebGPU core/adapter contract, the WGSL port, and
  the Emscripten build + browser smoke harness.
* `imslug-release` — version/changelog/NOTICE/tag checklist.
* `opencode` (built-in) — required before editing `.opencode/` or
  `opencode.jsonc`.
