# Changelog

All notable changes to imslug are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); version numbers use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — 2026-11-04

First release. imslug replaces Dear ImGui's bitmap-atlas text rendering with
the [Slug algorithm](https://jcgt.org/published/0006/02/02/): registered fonts
are drawn from their Bézier outlines in the fragment shader, at any size or
DPI, with analytic antialiasing and no per-size atlas builds. It is a drop-in
extension — no ImGui fork, supporting Vulkan, WebGPU (browser), and OpenGL 3.2+ /
OpenGL ES 3.0+ backends — and every non-registered font, shape and texture keeps
rendering through ImGui's own path.

### Added

- **Core glyph pipeline.** stb_truetype outline extraction, lines/quadratic
  Bézier conversion, adaptive band construction with sorted reference lists,
  packing into two shared pools (curve `RGBA16F`, band `R16G16_UINT`),
  ImGui-parity metrics (`OversampleH/V = 1`, `RasterizerDensity = 1`, …) and a
  CPU reference rasterizer that arbitrates every golden test.
- **Three GPU rendering backends:**
  - **Standalone Vulkan renderer.** An ImGui-free `ImSlugVulkan*` core
    (`backends/imslug_impl_vulkan.*`) + Dear ImGui adapter (`ImSlug_ImplVulkan_*`):
    instanced glyph draws with dynamic 1 px dilation, a frames-in-flight
    instance ring with automatic growth, dirty-row pool uploads through a
    staging buffer, and both classic render-pass and Vulkan 1.3
    dynamic-rendering paths.
  - **WebGPU backend and WebAssembly browser demo.** `backends/imslug_impl_webgpu.*`
    mirrors the Vulkan module (standalone core + `ImSlug_ImplWGPU_*` ImGui
    adapter, render-IO name `webgpu`): per-batch 80-byte uniform records with
    dynamic offsets, per-slot instance buffers uploaded with `queue.writeBuffer`,
    and growth without device stalls. Accompanied by a hand-maintained WGSL
    port of the shaders (`shaders/wgsl/`), Emscripten browser shell
    (`example/web/`), and automated GitHub Pages deployment.
  - **OpenGL 3.2+ and OpenGL ES 3.0+ backend.** `backends/imslug_impl_opengl3.*`
    provides the standalone core + `ImSlug_ImplOpenGL3_*` ImGui adapter, with
    a minimal self-contained dynamic GL loader (no external loader dependency),
    2D `GL_RGBA32UI` paint textures for COLR v0/v1 color gradients (avoiding SSBO
    limits on GL 3.2 / GLES 3.0 / macOS), dynamic GLSL shader selection
    (`#version 150`, `#version 330 core`, `#version 300 es`), instanced VBO
    rendering with full OpenGL state save/restore, and GLFW desktop example
    (`example/glfw_opengl3/`).
- **Dear ImGui integration.** A custom `ImFontLoader` producing rect-less
  glyphs with stable UV handles (no glyph bitmaps for Slug fonts), a draw-data
  pre-pass that rewrites Slug text runs into callback commands with their own
  dynamic scissor and an injected reset-render-state command, fine-clip
  reconstruction (clipped glyphs keep their true position), and the mixed-font
  fallback path.
- **Color fonts.** A COLR v0/v1 reader and layered rendering: one Slug
  instance per layer, per-fragment solid/linear/radial/sweep gradients, ClipBox
  clipping, the exact draw-order composites plus alpha-folded constant-mask
  composites, and documented graceful fallbacks for the rest. Verified against
  Noto Color Emoji and locally against Segoe UI Emoji.
- **C API and Rust bindings.** `imslug_c.h` / `imslug_c.cpp` — an
  `extern "C"` mirror with additions only — plus Vulkan and OpenGL C entry
  points (`backends/imslug_c_vulkan.cpp`, `backends/imslug_c_opengl3.cpp`),
  a pure-C11 example (`example/c/`), a C11 ABI gate, and the `imslug-rs`
  wrapper crate documentation (`docs/BINDINGS.md`).
- **Offline assets.** The `imslug_gen` CLI, embedded headers
  (`imslug_gen embed`), and the versioned, validated, deterministic `.imslug`
  container v1 (`docs/PACKED_FORMAT.md`), loaded through the same `AddFont*`
  entry points by content auto-detection — including `-DIMSLUG_TTF_PARSER=OFF`
  builds that ship with no TrueType parser at all.
- **Shared example application and shells.** Shared application
  (`example/common/example_app.*`) powering desktop Vulkan (`example/glfw_vulkan/`),
  desktop OpenGL (`example/glfw_opengl3/`), and browser WebGPU (`example/web/`)
  shells. Features:
  - Top-center renderer bar (`M` toggle or `--renderer atlas`) for live Slug-vs-atlas
    comparison;
  - 11 demo scenes including node-editor zoom/pan stress (key `N`), heavy perf
    scene (key `P`) with wall-clock plot, GPU timestamp queries and vsync toggle,
    and snail/shell emoji mascots (U+1F40C / U+1F41A);
  - Embedded default fonts (committed packed Roboto ASCII asset for Slug + raw
    Roboto TTF for ImGui's atlas) for standalone single-file execution;
  - F1/F2/F3 debug overlays (glyph bounds / band grid / reference-walk heat);
  - Deterministic `--screenshot <file.ppm> [scene] [dpi_scale]` capture.
- **Build configurations:**
  - `IMSLUG_WCHAR32` (default ON) defining `IMGUI_USE_WCHAR32` across the
    build for 32-bit `ImWchar` (required for non-BMP codepoints like color emoji);
  - `IMSLUG_STATIC_CRT` (MSVC) linking the C runtime statically (`/MT`) for
    single-file executables without VC++ redistributable requirements.
- **Robustness, performance and tooling.** Fine-clip fuzz coverage
  (100k-case CI profile), tuned band defaults (`clamp(ceil(2*sqrt(n)), 4, 32)`
  per axis), a benchmark harness (`scripts/bench.*`) with CPU/GPU numbers, and
  headless golden-image test suites across Vulkan and OpenGL 3.2+
  comparing GPU rendering against the CPU reference rasterizer.
- **Durable documentation and CI.** `docs/INTEGRATION.md`, `docs/DESIGN.md`,
  `docs/FAQ.md`, `docs/BINDINGS.md`, `docs/PACKED_FORMAT.md`, repo skills, and a
  CI matrix covering Windows MSVC, Linux GCC/Clang, lavapipe headless goldens,
  headless-Chrome WebGPU browser smoke, no-parser builds, and clean-project
  FetchContent consumption.

### Fixed

- **Slug text disappeared on high-DPI targets (phones, retina).** Both ImGui
  adapters built the batch ortho projection from the *framebuffer* size, but the
  instances live in ImGui *display* (logical) space: with
  `ImDrawData::FramebufferScale != 1` every glyph was drawn `1/scale` too small
  and clipped away by the scissor rect. The projection now maps
  `DisplayPos`..`DisplayPos + DisplaySize` like stock ImGui backends, keeping
  dilation at 1 physical pixel across all DPI scales.
- **Instance-ring overflow is no longer silent or permanent.** Frames exceeding
  the instance ring emit a one-time stderr warning, and the buffer automatically
  grows up to 8x the default capacity with deferred retirement (no GPU stalls).
- **Draw-data pre-pass idempotency.** Fixed a pre-pass double-commit that
  produced invalid early GPU benchmark numbers and wrong instances when a
  manual pre-pass preceded `UploadPools`.
- **Base font size resizing.** Fixed `+`/`-` font scaling to synchronize
  `style.FontSizeBase` with `BaseSize`, separated font scaling from scene
  cycling key bindings, and hardened `--screenshot` against swapchain recreation.
- Global `<assert.h>` include inside `namespace ImSlug` fixed for clang-cl/UCRT builds.
- CI workflow YAML step-name escaping fixed.

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
  `docs/DESIGN.md` §4).
