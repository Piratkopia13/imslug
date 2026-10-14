# Integrating imslug into an application

This is the drop-in guide: what imslug needs, how to add it to a CMake project,
the exact initialization and per-frame order, how fonts and the fallback path
behave, and how to diagnose the usual problems.

imslug is a **Dear ImGui extension, not a fork**: it hooks ImGui's font system
(`ImFontLoader`) and rewrites the draw data after `ImGui::Render()` so that
fonts registered through `ImSlug::AddFont*` render with the
[Slug algorithm](https://jcgt.org/published/0006/02/02/) instead of atlas
bitmaps. Every other font, shape, texture and draw command keeps going through
ImGui's stock path.

If you just want to see it run, build the example app instead
(`-DIMSLUG_BUILD_EXAMPLES=ON`, scene switcher keys 1–9, `N` for the node
editor, `P` for the heavy demo/perf scene, `M` — or the top-center renderer bar
— to swap Slug <-> ImGui's
dynamic font system; in atlas mode the Slug-only samples explain themselves
instead of drawing); this document is for putting imslug into your own project.
For the C API and Rust, see [`BINDINGS.md`](BINDINGS.md); for the design and the
measured numbers, see [`DESIGN.md`](DESIGN.md); common questions are answered in
[`FAQ.md`](FAQ.md).

## 1. Requirements

| Requirement | Notes |
|---|---|
| CMake ≥ 3.21 | imslug has no `install()` rules in v0.1; consume it with `FetchContent` or `add_subdirectory`. |
| C++17 compiler | MSVC 2022, GCC or Clang. The public headers are C++11-compatible and STL-free. |
| Dear ImGui ≥ 1.92.8 | `imslug.h` `#error`s on older versions. v1.92.9b is the pinned and tested release (`IMSLUG_IMGUI_TAG`). |
| Vulkan | Vulkan 1.2+ headers and loader for the backend module (the committed SPIR-V is 1.5). Without Vulkan the core still builds; the Vulkan module and examples are skipped. |
| GLFW | Only for the example app (fetched at a pinned tag, `IMSLUG_GLFW_TAG`). |
| stb_truetype | Vendored (`third_party/stb/`), compiled only when `IMSLUG_TTF_PARSER=ON` (default). |

## 2. Adding imslug to your build

imslug resolves Dear ImGui itself: an existing checkout via
`-DIMSLUG_IMGUI_DIR=<checkout>`, a `find_package(imgui)` target, or
`FetchContent` at `IMSLUG_IMGUI_TAG` (default `v1.92.9b`). If your application
also builds Dear ImGui, point both at the **same checkout and compile it once**
— two ImGui builds in one binary means duplicate symbols and incompatible
pointers.

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(imslug
    GIT_REPOSITORY <imslug-repo-url>
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(imslug)

target_link_libraries(my_app PRIVATE imslug imslug_vulkan)
```

`imslug` is the core library (it links `imslug_imgui`, the ImGui core TU set,
as a public dependency, so the include directories come with it).
`imslug_vulkan` is the backend module and the ImGui adapter
(`ImSlug_ImplVulkan_*`); when Vulkan is not found it is an interface
placeholder. The stock `imgui_impl_vulkan.cpp` is compiled by imslug as
`imslug_imgui_vulkan_backend` when Vulkan is found; link it or compile the
backend yourself from the same ImGui checkout. The GLFW platform backend and
the demo window are built only with `IMSLUG_BUILD_EXAMPLES=ON`
(`imslug_imgui_backends`, `imslug_imgui_demo`).

### Build options

| Option | Default | Effect |
|---|---|---|
| `IMSLUG_BACKEND_VULKAN` | `ON` | Build the Vulkan backend module (needs Vulkan). |
| `IMSLUG_BUILD_TESTS` | `ON` | Build the test suite and register ctest targets. |
| `IMSLUG_BUILD_EXAMPLES` | `OFF` | Build the GLFW+Vulkan example (needs Vulkan and GLFW). |
| `IMSLUG_BUILD_C_EXAMPLE` | `ON` | Build the pure-C11 example (needs the TTF parser). |
| `IMSLUG_BUILD_TOOLS` | `ON` | Build the `imslug_gen` offline generator (needs the TTF parser). |
| `IMSLUG_TTF_PARSER` | `ON` | Runtime stb_truetype outline parser; `OFF` = packed `.imslug` assets only (§7). |
| `IMSLUG_IMGUI_DIR` / `IMSLUG_IMGUI_TAG` | empty / `v1.92.9b` | Use a local ImGui checkout / pick the fetched tag. |
| `IMSLUG_GLFW_DIR` / `IMSLUG_GLFW_TAG` | empty / `3.4` | Same for GLFW (examples only). |

A consumer that does not want imslug's tests or tools in its build can turn
them off before `FetchContent_MakeAvailable`:

```cmake
set(IMSLUG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IMSLUG_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
```

### add_subdirectory

If imslug is vendored into your tree (or a sibling checkout):

```cmake
add_subdirectory(third_party/imslug)
target_link_libraries(my_app PRIVATE imslug imslug_vulkan)
```

`scripts/verify_fetchcontent.*` is the repository's own clean-project check: a
throwaway project consumes this repo via `FetchContent` (a git clone of the
current commit), builds the example plus a tiny consumer executable and runs
both. Use it as the reference for a working setup.

## 3. Initialization

The order is fixed and checked with asserts; the stock backend must come first
because imslug needs the renderer flags it publishes
(`ImGuiBackendFlags_RendererHasTextures`) and the reset-render-state callback
it installs.

```cpp
// once, in this order:
ImGui::CreateContext();                   // live context first
ImGui_ImplVulkan_Init(&init_info);        // stock backend, unchanged
ImSlug::Init();                           // imslug core (pools + registry)
ImSlug_ImplVulkan_Init(&init_info);       // imslug Vulkan module + ImGui adapter

ImFont* font = ImSlug::AddFontFromFileTTF("fonts/Inter.ttf", 18.0f);
ImGui::GetIO().FontDefault = font;        // or push it per window/font stack
```

* `ImSlug::Init(config)` takes an optional `ImSlug::Config` (see `imslug.h`);
  the defaults are what the example and the golden tests run with.
* `ImSlug_ImplVulkan_Init` mirrors `ImGui_ImplVulkan_InitInfo.PipelineInfoMain`
  (render pass/subpass or dynamic rendering, MSAA, pipeline cache). If you
  never hold an `ImGui_ImplVulkan_InitInfo` (custom renderer, C API), use the
  additive `ImSlug_ImplVulkan_InitFromVulkanInfo` with plain Vulkan objects.
* `ImSlug::Init` returns `false` when the context cannot support the pipeline;
  treat that as "render through the stock path" (imslug is designed to be
  optional, never mandatory).
* `ImSlug_ImplVulkan_Init` returns `false` when the device cannot sample the
  pool formats (`R16G16B16A16_SFLOAT`, `R16G16_UINT`) or the pool height
  exceeds `maxImageDimension2D`; in that case no Slug draws happen and all
  text stays on the atlas path.

### Shutdown

```cpp
ImSlug_ImplVulkan_Shutdown();   // unregisters + waits for the device
ImSlug::Shutdown();             // invalidates Slug font data (idempotent)
ImGui_ImplVulkan_Shutdown();
ImGui::DestroyContext();
```

## 4. Per frame

```cpp
ImGui::Render();
ImSlug_ImplVulkan_UploadPools(cmd);      // BEFORE the render pass; once per frame
// ... begin the render pass ...
ImSlug_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd, pipeline);
// ... end the render pass / submit ...
```

* `UploadPools` copies only the pool rows new glyphs touched (0–10 KB/frame in
  steady state) into a per-frame staging buffer and advances the internal
  frames-in-flight slot. It **must** be called outside a render pass (transfer
  commands) and exactly once per frame; calling it twice in the same frame
  asserts in debug.
* `RenderDrawData` replaces `ImGui_ImplVulkan_RenderDrawData` for the main
  viewport: it runs the pre-pass (rewriting Slug text runs into callback
  commands), then hands the draw data to the stock backend. Non-text geometry
  is untouched. The optional `pipeline` argument is the same main-pipeline
  override the stock backend takes.
* One instance per glyph (plus one per color layer) is written into a
  frames-in-flight ring sized by `Config::MaxInstancesPerFrame` (default
  16384). A frame that overflows never fails silently: the first drop at a
  given capacity prints a one-time stderr warning (demand vs capacity plus the
  knob that changes it), only the runs that do not fit are dropped (counted in
  `Statistics::DroppedRuns`), and with the default capacity the ring grows for
  the next frame (doubling, at least to the observed demand, capped at 8x the
  default) so the text comes back. A nonzero `Config::MaxInstancesPerFrame` is a
  fixed hard cap: nothing grows and the warning asks you to raise it.

## 5. Registering fonts

`ImSlug::AddFontFromFileTTF` / `AddFontFromMemoryTTF` mirror
`ImFontAtlas::AddFontFrom*`. The returned `ImFont` is a normal ImGui font with
identical layout metrics, but its glyphs are **rect-less**: nothing is
rasterized or packed into the atlas, and their UVs are imslug's stable glyph
handles. Do not sample the atlas texture for them.

* `sizePixels` is the bake size. Glyph geometry is size-independent, so one
  registered font serves every size and DPI; adding the same font at several
  sizes is a layout choice, not a data cost.
* `glyphRanges` works like stock (missing codepoints return `false` and ImGui's
  fallback glyph applies).
* `ImFontConfig` overrides that would change the baked metrics
  (`OversampleH/V`, `PixelSnapH`, `RasterizerDensity`, `GlyphOffset`,
  `ExtraSizeScale`, `GlyphMinAdvanceX`) are ignored with an assert: Slug glyphs
  are rendered analytically, so the knobs have no meaning. `MergeMode` is not
  supported yet (assert; the source is added as a regular source).
* Fonts not registered with imslug — including the built-in default font and
  fonts added through `io.Fonts->AddFont*` directly — keep rendering through
  ImGui's texture pipeline, in the same frame, in the same draw lists. Mixed
  Slug/atlas fonts are covered by the test suite.

## 6. DPI, sizes and runtime scale

Nothing special is required. Because the outline/band data lives in em space,
`style.FontScaleDpi` changes, window DPI changes, runtime font-size changes and
arbitrary zoom all work without re-baking anything; the fragment shader
resolves coverage per pixel. The example's scene 5 exercises a runtime
`FontScaleDpi` slider. Practical note: like ImGui's own stb loader, Slug is
unhinted, so prefer ≥ 13–16 px body sizes (see `DESIGN.md` §7 and `FAQ.md`).

## 7. Color fonts

COLR v0/v1 color glyphs render automatically: one Slug instance per layer with
per-fragment paint evaluation (solid, linear/radial/sweep gradients, ClipBoxes,
the exact composite subset). `Config::EnableColorFonts = false` disables the
path (color glyphs then draw their monochrome `glyf` outline, which is empty
for most emoji fonts). Non-BMP codepoints (most emoji, e.g. the snail U+1F40C)
require `IMGUI_USE_WCHAR32` in the whole build — a 16-bit `ImWchar` cannot
address them. imslug's CMake does this by default (`IMSLUG_WCHAR32=ON`) and
propagates the define to everything that links the Dear ImGui target, imslug or
its backends; `-DIMSLUG_WCHAR32=OFF` builds a 16-bit configuration instead (for
an app that must match the 16-bit default). The documented paint fallbacks are
in `DESIGN.md` §9.

## 8. Offline assets (no runtime parser)

The same `AddFont*` entry points accept pre-baked `.imslug` assets; the kind is
detected by content, not by extension:

```sh
imslug_gen font fonts/Inter.ttf --size-range 8:72 --chars 0x20-0x7E -o inter.imslug
```

```cpp
ImFont* font = ImSlug::AddFontFromFileTTF("inter.imslug", 18.0f);  // same call
```

Build with `-DIMSLUG_TTF_PARSER=OFF` to ship **without any TrueType parser**:
only packed assets load, TTF sources fail cleanly with a warning, and the
example defaults to the committed test asset. `imslug_gen info` validates and
inspects an asset; `--emit-header` embeds one in a C build. The container v1 is
deterministic and fully validated on load ([`PACKED_FORMAT.md`](PACKED_FORMAT.md));
v1 assets are monochrome only. The runtime path and the packed path produce
bitwise-identical glyph data (round-trip tested), so the choice is purely a
deployment trade-off.

## 9. Multi-viewport

v0.1 is tested with a single viewport. In a multi-viewport setup the Slug
pipeline mirrors the **main** render pass from `PipelineInfoMain`, while a
secondary viewport created by the `ImGui_ImplVulkanH_*` helpers owns its own
render pass, and the callback-time render state does not expose the active
viewport's pass. Secondary viewports whose pass is incompatible with the main
pipeline therefore cannot work; passes that happen to be compatible (same
formats/samples) follow Vulkan's pipeline-compatibility rules but are outside
the tested matrix. A correct path needs an explicit per-viewport render-pass
API (the Vulkan core already creates pipelines lazily per render pass) — planned
after v0.1, see `DESIGN.md` §8.

## 10. Diagnostics

* `ImSlug::GetStatistics(&stats)` — live glyph slabs, batches/instances of the
  last pre-pass, dropped runs, bounds mismatches, color layers.
* `Config::DebugDrawBounds = true` — validates every reconstructed instance
  against the drawn quad (debug assert + `Statistics::BoundsMismatches`); also
  seeds the glyph-bounds debug overlay.
* `ImSlug_ImplVulkan_SetDebugOverlay(flags)` — glyph bounds / band grid /
  reference-walk heat views on top of normal text (see `DESIGN.md` §6).
* Run with the Vulkan validation layers; the golden tests and the example run
  with them on by default (`IMSLUG_VALIDATION=0` disables).
* `ImSlug::GetVersion()` / `IMSLUG_VERSION` — version string.

## 11. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Text renders as atlas bitmaps for a registered font | The pre-pass refused to rewrite: the backend must set `ImGuiBackendFlags_RendererHasTextures` and publish `PlatformIO::DrawCallback_ResetRenderState` (both come from the stock `imgui_impl_vulkan` init). One-time stderr warning + debug assert. |
| Nothing renders for a Slug font | `ImSlug::Init` / `ImSlug_ImplVulkan_Init` returned false (check the return values), or `UploadPools` never ran (release: stale/empty pools; debug assert). |
| Text disappears in frames with a huge number of glyphs | Instance-ring exhaustion — never silent: a one-time stderr warning reports the demand vs the capacity. With the default capacity the ring grows for the next frame (doubling, capped at 8x the default), so the text returns; a fixed `Config::MaxInstancesPerFrame` is a hard cap, so raise it (or leave 0). Ongoing drops show in `Statistics::DroppedRuns`. |
| Validation errors about pipelines / render passes | `ImSlug_ImplVulkan_Init` must describe the render pass (or dynamic-rendering formats) the draws actually run in. |
| `imslug.h` fails to compile with an `#error` about Dear ImGui | ImGui < 1.92.8 or `imgui.h` not included first; see §1. |
| Glyphs are squeezed or jump at clip edges | Fine-clip reconstruction disabled? The default (`EnableFineClipReconstruction = true`) reconstructs true placement; the fallback compresses into the drawn quad by design. |
| OTF/CFF font stays on the atlas path | Not parsed in v0.1 (TTF `glyf` only); see `FAQ.md`. |
| Non-BMP emoji missing | Build the whole app with `IMGUI_USE_WCHAR32`; 16-bit `ImWchar` cannot address them. imslug's CMake defines it by default (`IMSLUG_WCHAR32=ON`); check that a caller-provided `imgui` target matches (configure-time AUTHOR_WARNING). |
| Color glyphs render as empty boxes | The font's emoji glyphs have no `glyf` outline and the color path was disabled or the paint graph was unsupported; check `Config::EnableColorFonts` and `DESIGN.md` §9. |
| Duplicate ImGui symbols at link time | imslug and your app compiled Dear ImGui twice; share one checkout/one TU set. |
| Crash in a packed-asset build with a TTF path | `IMSLUG_TTF_PARSER=OFF` accepts only `.imslug` assets; bake with `imslug_gen` or enable the parser. |

## 12. Known limitations

See `DESIGN.md` §8 for the full list (GPU cost profile, multi-viewport,
OTF/CFF, packed color fonts, non-BMP, hinting policy) and `FAQ.md` for the
common questions.

## 13. WebGPU backend (browser)

`backends/imslug_impl_webgpu.*` mirrors the Vulkan module for Emscripten +
`--use-port=emdawnwebgpu` builds (CMake option `IMSLUG_BACKEND_WEBGPU`, default
ON under Emscripten). The flow is the same shape:

```cpp
// once, in this order:
ImGui::CreateContext();
ImGui_ImplWGPU_Init(&init_info);     // stock imgui_impl_wgpu backend
ImSlug::Init();
ImSlug_ImplWGPU_Init(&init_info);    // imslug WebGPU module + ImGui adapter

ImFont* font = ImSlug::AddFontFromFileTTF("fonts/Inter.ttf", 18.0f);

// per frame:
ImGui::Render();
ImSlug_ImplWGPU_UploadPools();       // before the render pass; once per frame
// ... begin the render pass ...
ImSlug_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
```

* The differences from the Vulkan module are API-forced and documented in
  `backends/imslug_impl_webgpu.h`: WebGPU has no push constants, so each Slug
  batch carries an 80-byte uniform record in a dynamic-offset ring (stride
  aligned to `minUniformBufferOffsetAlignment`); growth never waits on the
  device; the CPU instance ring uploads with one `queue.writeBuffer` per frame
  (`ImSlugWGPUUploadInstances`, called by the adapter after the pre-pass).
* WebGPU has no runtime error-callback setters: install
  `uncapturedErrorCallbackInfo` / `deviceLostCallbackInfo` on your
  `WGPUDeviceDescriptor` (the example shell does). The module brackets its own
  creation work in validation error scopes and reports the first failure once.
* The Slug pipeline must match the stock backend (render target format, depth
  format, sample count) because Slug draws share ImGui's render pass.
* Browser builds: `scripts/build_web.*` (site in `build-web/site/`, optional
  embedded CJK/color-emoji subsets), `scripts/web_smoke.*` (headless-Chrome
  smoke) and the `imslug-webgpu-backend` skill hold the Emscripten flags, the
  page hooks and the known pitfalls.
