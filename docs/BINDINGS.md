# Binding imslug — C API and Rust bindings

This document is for authors of language bindings and for C applications that
consume imslug without the C++ API. The C++ API (`imslug.h`,
`backends/imslug_impl_vulkan.h`) is canonical; the C API is an `extern "C"`
mirror with additions only, and this file records its conventions, the ABI
policy and the binding status.

Status: the C API and the C example are complete and CI-gated; the
Rust wrapper crate (`imslug-rs`, a sibling local repository) covers the smoke
scope (link, context, font registration, clean shutdown). Full rendering through
`imgui-rs-vulkan-renderer` is explicitly deferred — see
[the integration answer](#imgui-rs--imgui-rs-vulkan-renderer-status). The
`.imslug` packed-asset loader is auto-detected by content in the same
font-add entry points and comes with two additive C entries —
`imslug_is_packed_font_data` and `imslug_validate_packed_font_data`; the format
is documented in `docs/PACKED_FORMAT.md`.

## Files

| File | What it is |
|---|---|
| `imslug_c.h` | The C API: core mirror + Vulkan entry points (C11-clean, no `imgui.h`). |
| `imslug_c.cpp` | Core mirror implementation (int↔bool config translation, statistics copy). |
| `backends/imslug_c_vulkan.cpp` | Vulkan C entry points (lives in `backends/`: the core stays backend-independent). |
| `example/c/` | Pure-C11 example: CPU smoke always, headless Vulkan frame with `--vulkan`. |
| `tests/test_c_abi.c` | C11 + strict-warnings compilation and struct-layout gate. |
| `tests/test_c_api.cpp` | C++ mirror checks (C API vs canonical C++ API, value for value). |
| `tests/test_packed_loader.cpp` | Packed `.imslug` validation/loader checks incl. the new C entries (runs without the TTF parser). |
| `imslug-rs` (sibling repo) | Rust wrapper crate: raw `sys` declarations + safe wrapper + smoke test. |

The C API is compiled into the `imslug` library; the Vulkan C entry points into
`imslug_vulkan` (link both, like the C++ integration does).

## Conventions (read this before binding)

* **Symbols are `imslug_*`** (spelled out: `imslug_init`,
  `imslug_add_font_from_file_ttf`, `imslug_impl_vulkan_init`, …). The C++ API
  keeps its `ImSlug::` / `ImSlug_ImplVulkan_*` names; the C mirror is 1:1 by
  meaning.
* **No C++ construct leaks**: fixed-size POD structs, opaque pointers for
  ImGui handles, `int` (0/1) for booleans — never C++ `bool` in the ABI.
  `imslug_config`, `imslug_statistics` and `imslug_vulkan_init_info` are
  passed/returned **by value** and contain no pointers with allocation
  semantics.
* **Opaque handles** (`ImFont*`, `ImFontConfig*`, `ImDrawData*`, `ImDrawList*`,
  `ImDrawCmd*`) are forward-declared in `imslug_c.h`; their definitions must
  come from the app's Dear ImGui binding (cimgui for C, imgui-sys/bindgen for
  Rust). `NULL` font configs/glyph ranges select Dear ImGui's defaults.
* **`imslug_wchar` must match the app's `ImWchar`**: 16-bit by default,
  32-bit when the ImGui build defines `IMGUI_USE_WCHAR32` (also define
  `IMSLUG_WCHAR32` for the C header when your binding does not surface
  `IMGUI_USE_WCHAR32`). Non-BMP codepoints (e.g. color emoji) require the
  32-bit configuration in the whole build — a 16-bit `ImWchar` cannot address
  them.
* **Vulkan declarations are conditional**: include `<vulkan/vulkan.h>` before
  `imslug_c.h`, or define `IMSLUG_C_VULKAN` to have the header include it. The
  core declarations are always visible, so the header stays dependency-free
  for CPU-only consumers.
* **Version**: `IMSLUG_VERSION_MAJOR/MINOR/PATCH/NUM/STRING` mirror
  `imslug.h`; `imslug_version_string()` / `imslug_version_num()` return the
  values compiled into the library. Additions are source- and ABI-compatible;
  a struct layout change is an ABI break and bumps the version (0.x policy:
  new capabilities land as new entry points first).
* **Threading**: like Dear ImGui, imslug is single-threaded. Every entry point
  must be called from the thread that owns the ImGui context; there is no
  internal synchronization.
* **Context requirement**: `imslug_init` and the font-add functions require a
  live ImGui context (create it first); the Vulkan module additionally
  requires the stock renderer backend to be initialized (it sets
  `ImGuiBackendFlags_RendererHasTextures`, which the pre-pass requires).

## Minimal C integration

The order is identical to the C++ API (`imslug.h` documents each step). With
cimgui names for the ImGui calls:

```c
igCreateContext(NULL);                          /* live context first */
ImGui_ImplVulkan_Init(&init_info);              /* stock backend, unchanged */
imslug_init(imslug_config_default());           /* core: pools + registry */

imslug_vulkan_init_info vk = imslug_vulkan_init_info_default();
vk.PhysicalDevice = ...; vk.Device = ...; vk.Queue = ...; vk.QueueFamily = ...;
vk.ImageCount = image_count; vk.RenderPass = render_pass; vk.Subpass = 0;
imslug_impl_vulkan_init(vk);                    /* imslug Vulkan module */

ImFont* font = imslug_add_font_from_file_ttf("fonts/Inter.ttf", 18.0f, NULL, NULL);
igGetIO()->FontDefault = font;                  /* or push it per window */

/* per frame, before the render pass: */
imslug_impl_vulkan_upload_pools(cmd);           /* dirty glyph-pool rows only */
/* inside the render pass, instead of ImGui_ImplVulkan_RenderDrawData: */
imslug_impl_vulkan_render_draw_data(igGetDrawData(), cmd, pipeline);

/* shutdown: Vulkan module first, then the core, then the context */
imslug_impl_vulkan_shutdown();
imslug_shutdown();
igDestroyContext(NULL);
```

A C app that also wants Dear ImGui itself links cimgui (generated bindings for
the same Dear ImGui version). The example (`example/c/`) does not vendor
cimgui: it ships a tiny example-local shim (`example/c/imgui_c_shim.*`) for the
few ImGui entry points it needs, and consumes imslug exclusively through
`imslug_c.h`.

Build and run the example:

```sh
scripts/build.sh                                  # builds imslug_example_c (default)
build/imslug_example_c                            # CPU smoke (no GPU needed)
build/imslug_example_c --vulkan --screenshot out.ppm   # headless offscreen frame
```

`--vulkan` exits 77 (ctest `SKIP_RETURN_CODE`) when no usable Vulkan device
exists; the environment variables `IMSLUG_VALIDATION=0` and
`IMSLUG_VK_DEVICE=<n>` mirror the C++ example.

## Packed `.imslug` assets

`imslug_add_font_from_file_ttf` and `imslug_add_font_from_memory_ttf` accept
**both** TTF/OTF bytes and `.imslug` packed assets: the kind is auto-detected by
content (the container magic), not by file extension, so no binding code has to
choose an entry point. A packed asset skips outline parsing entirely and works
in builds configured without the TTF parser (`-DIMSLUG_TTF_PARSER=OFF`); the
format and the generator CLI are documented in `docs/PACKED_FORMAT.md`.

Two additive entries support tooling and defensive loaders:

```c
/* 1 when data starts with the .imslug magic (cheap sniff, no parsing). */
int imslug_is_packed_font_data(const void* data, int data_size);

/* Full structural validation of a v1 asset; 1 valid / 0 invalid, with an
   optional NUL-terminated reason in error_message. Never crashes. */
int imslug_validate_packed_font_data(const void* data, int data_size,
                                     char* error_message, int error_message_capacity);
```

Both are additions (the existing ABI is unchanged); bind them freely.
`imslug-rs`'s `sys.rs` does not declare them yet — the wrapper only exposes the
file variant of the font-add path, so packed assets already work through it
(`add_font_from_file_ttf("font.imslug", ...)`); the explicit validation entries
can be bound later without a build change.

## Vulkan entry points

`imslug_vulkan_init_info` is the C mirror of `ImSlugVulkanInitInfo`; fill it
from your renderer's Vulkan objects. A zero `MSAASamples` selects
`VK_SAMPLE_COUNT_1_BIT`; a zero `MaxInstancesPerFrame` selects
`Config::MaxInstancesPerFrame`, then the backend default (16384) with adaptive
growth on overflow, while a nonzero value is a fixed hard cap. The
instance/device API version must be ≥ 1.2 (the committed SPIR-V is 1.5).

`imslug_impl_vulkan_init` maps to the additive C++
`ImSlug_ImplVulkan_InitFromVulkanInfo` — the entry point for integrations that
never hold an `ImGui_ImplVulkan_InitInfo` (the C API and future non-C++
renderers). It registers the render interface and initializes the standalone
core exactly like `ImSlug_ImplVulkan_Init` does from an
`ImGui_ImplVulkan_InitInfo`; the debug overlay seeds from
`Config::DebugDrawBounds`.

Debug overlay flags: `IMSLUG_VULKAN_DEBUG_OVERLAY_GLYPH_BOUNDS`,
`..._BAND_GRID`, `..._REF_HEAT` (combine; 0 = off) via
`imslug_impl_vulkan_set_debug_overlay` / `..._get_debug_overlay`.

## Ownership and shutdown

* Fonts returned by `imslug_add_font_from_*` are ordinary `ImFont` objects
  owned by the ImGui atlas; `imslug_shutdown()` invalidates their Slug data
  without touching the atlas. Destroy order: Vulkan module → core → context.
* `imslug_add_font_from_memory_ttf` transfers ownership of `data` to the atlas
  by default (Dear ImGui frees it; set
  `ImFontConfig::FontDataOwnedByAtlas = false` to keep ownership). imslug keeps
  its own copy of the outline data, and the caller must keep the bytes alive
  until the atlas is built — the same contract as stock Dear ImGui.
* `imslug_shutdown()` is idempotent; `imslug_get_statistics(NULL)`,
  `imslug_new_frame()`, `imslug_is_registered(NULL)` and
  `imslug_draw_callback(NULL, NULL)` are documented no-ops.

## Known limits (bindings must surface these)

* ImFontConfig overrides that would change the baked metrics (`Oversample*`,
  `PixelSnapH`, `RasterizerDensity`, `GlyphOffset`, `ExtraSizeScale`,
  `MergeMode`, `GlyphMinAdvanceX`) are ignored with an assert: Slug glyphs are
  rasterized analytically. Everything else is honored like stock.
* A Slug font's glyphs are **rect-less**: no atlas bitmaps are allocated and
  the glyph UVs are imslug handles. Do not sample the atlas for them.
* The pre-pass refuses to rewrite draw data without
  `ImGuiBackendFlags_RendererHasTextures` and
  `PlatformIO::DrawCallback_ResetRenderState`; without a registered backend
  module the draw data is left untouched (stock rendering).
* Color glyphs render one instance per COLR layer with per-fragment paint
  evaluation; the documented composite approximations are in
  `docs/DESIGN.md` section 9. `Config::EnableColorFonts = false` disables the
  path.
* Non-BMP emoji need `IMGUI_USE_WCHAR32` in the whole build (see above).
* Packed `.imslug` v1 assets are **monochrome only**: a packed font has no
  COLR/CPAL payload, so color glyphs fall back to their glyf outline (empty for
  most emoji fonts). Bake color fonts with the TTF path until a future
  container version adds color data.

## Rust bindings (`imslug-rs`, sibling repo)

`../imslug-rs` (local git repo, branch `main`, no remote —
same machine only) contains the `imslug` wrapper crate:

* `src/sys.rs` — hand-written `extern "C"` declarations mirroring
  `imslug_c.h` (no bindgen/libclang requirement) for the core API.
* `src/lib.rs` — the safe wrapper: `Config`, `init`, `shutdown`,
  `add_font_from_file_ttf`, `is_registered`, `Statistics`, `version`, and a
  minimal `context` module (create/destroy) through the crate's C++ shim.
* `build.rs` — configures + builds the imslug CMake project in
  `target/imslug-cmake` (override the source tree with `IMSLUG_SOURCE_DIR`,
  the build with `IMSLUG_RS_BUILD_DIR`) and compiles `src/imslug_rs_shim.cpp`
  against the fetched Dear ImGui headers. It emits the link flags for
  `imslug`, `imslug_stb`, `imslug_imgui` (and `imslug_vulkan` when the backend
  is built).
* `tests/smoke.rs` — the smoke test: create context, init, add a font from
  `../imslug/tests/fonts/Roboto-Regular.ttf` (override with
  `IMSLUG_RS_FONT`), check registration + statistics, clean shutdown.

```sh
cd ../imslug-rs
cargo test        # builds imslug via CMake, links, runs the smoke test
cargo build
```

Scope: the smoke test intentionally stops before rendering (no Vulkan device
handling in Rust yet). `sys.rs` deliberately binds the **core API only**; the
Vulkan entry points of `imslug_c.h` arrive with the renderer integration
below (the crate's `build.rs` already links the `imslug_vulkan` static library
when the imslug checkout builds it, so adding the declarations later needs no
build changes).

## `imgui-rs` / `imgui-rs-vulkan-renderer` status

**Decision: the full `imgui-rs-vulkan-renderer` integration is deferred.**
What it needs, precisely:

1. **A renderer-agnostic draw hook.** Today the Slug draw path
   (`ImSlugRenderIO::DrawBatch`) is bound to the stock ImGui Vulkan backend's
   published render state (`ImGui_ImplVulkan_RenderState`: command buffer,
   pipeline layout, framebuffer size). A pure-Rust renderer never sets that;
   it needs either an additive backend API to publish its render state /
   record Slug draws itself, or a renderer-side hook. That is a new backend
   contract, not a C-mirror concern.
2. **A single shared Dear ImGui build.** `imgui-rs` compiles its own
   `imgui-sys`; imslug must be compiled against the *same* ImGui instance
   (same headers, one set of symbols), or pointer/interop mixing breaks. A
   real integration has to solve the shared-build problem (e.g. build imslug
   against `imgui-sys`'s sources) — the current crate deliberately keeps its
   own build for the standalone smoke scope.
3. **An upstream/extension point in `imgui-rs-vulkan-renderer`.**
   Its `Renderer::cmd_draw` records *its own* pipelines/descriptors from the
   ImGui draw data and does not expose a way to record a foreign pipeline
   (the Slug instanced draw with the instance-ring buffer) into the same
   command buffer. That requires an upstream change or a fork/wrapper
   renderer.

Because of (1)–(3), the integration is a dedicated follow-up effort; the C API
and `imslug-rs` are its prerequisites (they already expose the
core + a Vulkan init that takes plain Vulkan objects).

## CI coverage

* Every job builds the C example (default `IMSLUG_BUILD_C_EXAMPLE=ON`) and runs
  `imslug_c_example` (CPU smoke) + `imslug_c_abi` + `imslug_c_api` via
  `scripts/test.*`. The packed-asset suites (`imslug_packed_loader`,
  `imslug_packed_roundtrip`, `imslug_asset_fresh`, `imslug_golden_packed*`) run
  in the same jobs.
* The lavapipe job also runs `build/imslug_example_c --vulkan`, the full
  offscreen frame through the C API (validation layers on).
* A dedicated no-parser job configures with `-DIMSLUG_TTF_PARSER=OFF`, runs the
  packed suites + goldens and the example entirely from the committed asset
  (proving the "no font parser in the app" deployment).
* The Windows runner image has no Vulkan SDK, so there the C example compiles
  without the Vulkan path; the Vulkan C declarations are compile-gated on
  Linux (lavapipe job) and locally.
