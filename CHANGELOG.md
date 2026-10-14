# Changelog

All notable changes to imslug are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); version numbers use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **WebGPU backend and WebAssembly browser demo.** `backends/imslug_impl_webgpu.*`
  mirrors the Vulkan module (standalone core + `ImSlug_ImplWGPU_*` ImGui
  adapter, render-IO name `webgpu`) with its API-forced differences documented
  in the header: per-batch 80-byte uniform records with dynamic offsets replace
  push constants, per-slot instance buffers upload with one `queue.writeBuffer`
  per frame, growth never waits on the device, and device error/lost callbacks
  are installed by the app on its `WGPUDeviceDescriptor`.
- **Hand-maintained WGSL port** of the Slug shaders (`shaders/wgsl/`), embedded
  by `scripts/gen_shaders.*` into the committed `shaders/imslug_shaders_webgpu.h`;
  the auditable GLSL→WGSL mapping lives in `shaders/reference/wgsl-port.txt`.
  The SPIR-V header and the Vulkan path are unchanged.
- **Shared example + browser shell.** The example's application (options, state,
  scenes, switcher) lives in `example/common/example_app.*`;
  `example/glfw_vulkan/main.cpp` is the desktop shell and
  `example/web/main.cpp` (+ `example/web/shell.html`) the Emscripten shell. The
  web build embeds the fetched CJK subset and a COLR v1 subset of the fetched
  Noto Color Emoji when present (`scripts/build_web.*`,
  `scripts/subset_emoji_for_web.py` + `imslug_gen embed`); both stay
  desktop-only when the assets are absent.
- **Web tooling and deployment.** `IMSLUG_BACKEND_WEBGPU` (default ON under
  Emscripten), `scripts/build_web.bat|sh` (assembles `build-web/site/`),
  `scripts/web_smoke.sh` + `scripts/web_smoke/` (headless-Chrome smoke with a
  software WebGPU adapter) and `.github/workflows/pages.yml` (build + smoke +
  GitHub Pages deploy on `main`).

### Changed

- The example's shared application moved to `example/common/`; the two
  generated default-font headers moved with it (`example/common/imslug_example_*.h`).
  `--screenshot` output and CLI behavior are unchanged (verified with
  interleaved A/B runs against the pre-refactor binary).
- Second-backend policy: **WebGPU is the second backend** (browser); the
  earlier "GL3 first" note is superseded (`docs/DESIGN.md` §8, `docs/FAQ.md`).
  A desktop GL backend would be a new module if a consumer ever needs one.

- **Standalone example.** The GLFW example's default fonts are now embedded in
  the executable — the committed packed Roboto ASCII asset for Slug plus the
  raw Roboto TTF for ImGui's stock atlas loader (the `M` toggle / mixed-font
  scene) — so `imslug_example_glfw_vulkan.exe` runs with no data files next to
  it. `--font` or the new `IMSLUG_EXAMPLE_FONT` environment variable overrides
  the default (`--font` wins); scene 9 falls back to the embedded asset when
  the committed file is absent.
- **`imslug_gen embed <file> --emit-header <out.h>`** wraps any raw file in the
  same deterministic byte-array header the asset path uses (used for the
  embedded TTF); `imslug_asset_fresh` now also byte-compares the two committed
  example headers against fresh CLI output.
- **`IMSLUG_STATIC_CRT`** (configure option, default OFF, MSVC only) links the C
  runtime statically (`/MT`, mirrored for fetched GLFW) so the example becomes
  a single-file executable whose only non-OS import is `vulkan-1.dll`.
- **Example comparison mode.** The GLFW+Vulkan example gained a heavy demo /
  perf scene (key `P`): an `M` toggle (or `--renderer atlas`) swaps the body
  text between Slug outlines and ImGui's dynamic font system for the same
  registered font, and a performance panel shows wall-clock frame time (plot),
  whole-frame GPU time from timestamp queries, draw / Slug / atlas counters and
  a vsync toggle (FIFO <-> MAILBOX/IMMEDIATE, `--novsync` on the CLI).

### Changed

- The example's default Slug font is the embedded packed Roboto ASCII asset in
  every configuration (previously the Roboto TTF file in parser builds); all
  example text is ASCII and the parser/no-parser demos now behave identically.

### Fixed

- **Slug text disappeared on high-DPI targets (phones, retina).** Both ImGui
  adapters built the batch ortho projection from the *framebuffer* size, but the
  instances live in ImGui *display* (logical) space: with
  `ImDrawData::FramebufferScale != 1` every glyph was drawn `1/scale` too small
  and — clipped away by the batch's own scissor rect — invisible. That is what
  the deployed web demo did on a phone (the canvas hid no Slug text at all;
  ImGui's atlas text, which scales through its own projection, kept working).
  The MVP now maps `DisplayPos`..`DisplayPos + DisplaySize` like the stock
  backends (`imgui_impl_vulkan`/`imgui_impl_wgpu`); the push-constant/uniform
  `viewport` stays the framebuffer size, so the 1 px dilation remains 1
  *physical* pixel at any DPI. Regression tests: `imslug_tests_imgui_golden`
  renders the default scene at framebuffer scales 2x/3x and compares the
  Slug/atlas ink masks, and the browser smoke gained a hi-DPI pass (DPR 3) that
  presses the `M` text-renderer toggle and requires the Slug and atlas frames to
  show the same ink.
- **Instance-ring overflow is no longer silent or permanent.** A frame that
  exceeds the per-frame instance ring now prints a one-time stderr warning
  (observed demand vs capacity plus the knob that changes it) and, with the
  default capacity (`Config::MaxInstancesPerFrame = 0`), the ring grows for the
  next frame — in place when the frame reserved nothing yet, otherwise at the
  next `ImSlug_ImplVulkan_UploadPools` — doubling, at least to the observed
  demand, capped at 8x the default. The grown-away buffer is deferred-freed
  (no device wait) so in-flight frames keep reading it. An explicit
  `MaxInstancesPerFrame` stays a **hard cap**: its documented drop-and-warn
  fallback is unchanged. New backend regression: drop → both growth paths →
  full re-render, the explicit cap, and the retire timing under validation.
- **Example input and screenshot robustness.** `+`/`-` (and the keypad keys)
  now only change the base font size — they were double-bound to scene cycling,
  so one keypress did two things. `--screenshot` no longer silently skips the
  capture when the swapchain is recreated (the retry services the recreation
  and a write/landing failure is an error) and a minimized window no longer
  renders against a destroyed swapchain; swapchain recreation refreshes the
  backend's image expectation and clamps the frame ring, and a failed
  GPU-timestamp read no longer averages a stale sample.

## [0.1.0] — 2026-10-14 (release prepared; `v0.1.0` tag pending the repo owner)

First release. imslug replaces Dear ImGui's bitmap-atlas text rendering with
the [Slug algorithm](https://jcgt.org/published/0006/02/02/): registered fonts
are drawn from their Bézier outlines in the fragment shader, at any size or
DPI, with analytic antialiasing and no per-size atlas builds. It is a drop-in
extension — no ImGui fork, stock `imgui_impl_vulkan` backend — and every
non-registered font, shape and texture keeps rendering through ImGui's own
path.

### Added

- **Core glyph pipeline.** stb_truetype outline extraction, lines/quadratic
  Bézier conversion, adaptive band construction with sorted reference lists,
  packing into two shared pools (curve `RGBA16F`, band `R16G16_UINT`),
  ImGui-parity metrics (`OversampleH/V = 1`, `RasterizerDensity = 1`, …) and a
  CPU reference rasterizer that arbitrates every golden test.
- **Standalone Vulkan renderer.** An ImGui-free `ImSlugVulkan*` core:
  instanced glyph draws with dynamic 1 px dilation, a frames-in-flight instance
  ring, dirty-row pool uploads through a staging buffer, and both classic
  render-pass and Vulkan 1.3 dynamic-rendering paths.
- **Dear ImGui integration.** A custom `ImFontLoader` producing rect-less
  glyphs with stable UV handles (no glyph bitmaps for Slug fonts), a draw-data
  pre-pass that rewrites Slug text runs into callback commands with their own
  dynamic scissor and an injected reset-render-state command, fine-clip
  reconstruction (clipped glyphs keep their true position), and the mixed-font
  fallback path.
- **Robustness, performance and tooling.** Fine-clip fuzz coverage
  (100k-case CI profile), tuned band defaults
  (`clamp(ceil(2*sqrt(n)), 4, 32)` per axis), a benchmark harness
  (`scripts/bench.*`) with CPU/GPU numbers, the F1/F2/F3 debug overlay
  (glyph bounds / band grid / reference-walk heat), and the script-fetched
  Noto Sans SC subset for CJK stress tests.
- **Color fonts.** A COLR v0/v1 reader and layered rendering: one Slug
  instance per layer, per-fragment solid/linear/radial/sweep gradients, ClipBox
  clipping, the exact draw-order composites plus alpha-folded constant-mask
  composites, and documented graceful fallbacks for the rest. Verified against
  Noto Color Emoji and locally against Segoe UI Emoji.
- **C API and Rust bindings.** `imslug_c.h` / `imslug_c.cpp` — an
  `extern "C"` mirror with additions only — plus the Vulkan C entry points and
  the additive `ImSlug_ImplVulkan_InitFromVulkanInfo`, a pure-C11 example
  (`example/c/`), a C11 ABI gate, and the `imslug-rs` wrapper crate (sibling
  repo, smoke scope) with `docs/BINDINGS.md`.
- **Offline assets.** The `imslug_gen` CLI and the versioned, validated,
  deterministic `.imslug` container v1 (`docs/PACKED_FORMAT.md`), loaded through
  the same `AddFont*` entry points by content auto-detection — including
  `-DIMSLUG_TTF_PARSER=OFF` builds that ship with no TrueType parser at all.
- **Durable documentation and process.** `docs/INTEGRATION.md`, `docs/DESIGN.md`,
  `docs/FAQ.md`, `docs/BINDINGS.md`, `docs/PACKED_FORMAT.md`, repo skills, and a
  CI matrix with Windows + Linux build/test, lavapipe golden images, a no-parser
  job, a benchmark report job, a daily Dear ImGui master drift check, and a
  clean-project FetchContent job.

### Fixed

- Draw-data pre-pass double-commit that produced invalid early GPU benchmark
  numbers and wrong instances when a manual pre-pass preceded `UploadPools`
  (regression-tested).
- The CI workflow YAML was invalid (an unquoted colon in a step name) and had
  never been parsed; fixed during development.
- `<assert.h>` included inside `namespace ImSlug` broke clang-cl/UCRT builds;
  fixed during development (the core is now clang-clean with the parser on and
  off).

### Known limitations

- OTF/CFF outlines are not parsed; such fonts stay on the stock atlas path.
  Planned after v0.1 (see `docs/DESIGN.md` §8 and `docs/FAQ.md`).
- Multi-viewport beyond render passes compatible with the main one is deferred
  (the adapter cannot discover a foreign viewport's render pass at callback
  time).
- Dear ImGui 1.92.8+ is required; v1.92.9b is the pinned and tested version
  (master is tracked by the daily smoke job).
- Packed `.imslug` v1 assets are monochrome only; color fonts must use the TTF
  path until a future container version adds color data.
- Non-BMP codepoints (most emoji) need `IMGUI_USE_WCHAR32` in the whole build.
- Slug text is deliberately heavier per pixel than sampling an atlas
  (frame-time parity with a bounded, text-share-proportional delta — numbers in
  `docs/DESIGN.md` §4); there is no second backend yet (GL3 is the first
  candidate, after v0.1).
