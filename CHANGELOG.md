# Changelog

All notable changes to imslug are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); version numbers use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
