---
name: imslug webgpu backend
description: WebGPU backend contract for imslug - the frozen standalone core (bind group 0, the 80 B uniform ring, the instance ring API, upload/growth policy) and the ImGui adapter, plus the Emscripten browser stack, page hooks, build/smoke tooling and its pitfalls. Load when working in backends/imslug_impl_webgpu.*, example/web/**, scripts/build_web.* or scripts/web_smoke/**.
---

# imslug WebGPU backend contract

Two layers:

* **Standalone core** - `backends/imslug_impl_webgpu.*`: the ImGui-free WebGPU
  core (pool textures, uploads, uniform ring, instance ring, pipeline, draws
  recorded into a caller-provided render pass encoder). Uses raw
  `<webgpu/webgpu.h>` exactly like the pinned `imgui_impl_wgpu.cpp` (Dawn
  flavour, provided by `--use-port=emdawnwebgpu`); no C++ wrappers.
* **ImGui adapter** - the `ImSlug_ImplWGPU_*` block at the bottom of the same
  file: maps `ImGui_ImplWGPU_InitInfo`/`ImDrawData` onto the core and registers
  the core's `ImSlugRenderIO` callback interface under the name `"webgpu"`.
  Only the adapter block includes imgui / imgui_impl_wgpu headers.

WebGPU code lives in `backends/` only; the core stays backend-independent
(AGENTS.md invariant 7). The shader pair is `shaders/wgsl/slug.{vert,frag}.wgsl`
+ `shaders/imslug_shaders_webgpu.h` - see the `imslug-shaders` skill.

## Frozen core API (`backends/imslug_impl_webgpu.h`)

```cpp
struct ImSlugWGPUInitInfo
{
    WGPUDevice Device; WGPUQueue Queue;     // owned by the caller, never created/destroyed here
    WGPUTextureFormat RenderTargetFormat;   // must match the stock pipeline exactly
    WGPUTextureFormat DepthStencilFormat;   // Undefined = no depth attachment
    WGPUMultisampleState MultisampleState;  // count 0 => {1, ~0u, false}
    uint32_t MaxInstancesPerFrame;          // 0 => 16384 (adaptive growth); nonzero = hard cap
    uint32_t NumFramesInFlight;             // 0 invalid (Init fails); clamped to 3
};

bool ImSlugWGPUInit(const ImSlugWGPUInitInfo*);  // false => not usable (formats/limits/creation)
void ImSlugWGPUShutdown();                       // no device wait in WebGPU; in-flight work keeps its resources alive

// Creates/recreates both pool textures to the pools' allocated size and uploads
// the dirty rows; must run once per frame, before the first draw and before
// ImSlugWGPUUploadInstances. `frameIndex` selects the instance/uniform rings and
// must match the later GetInstanceData / UploadInstances / Draw calls.
bool ImSlugWGPUUploadPools(ImSlugPools* pools, uint32_t frameIndex);

uint32_t ImSlugWGPUGetFramesInFlight();
uint32_t ImSlugWGPUGetInstanceCapacity();
ImSlugGlyphInstance* ImSlugWGPUGetInstanceData(uint32_t frameIndex);  // CPU ring slot base; re-fetch after growth
// Reserves `count` contiguous records in the current frame's slot (null when the
// frame is full: drop that batch). Overflow warns once per capacity and grows the
// ring when the capacity came from the default; an explicit MaxInstancesPerFrame
// is a hard cap. The adapter's ReserveInstances forwards here.
ImSlugGlyphInstance* ImSlugWGPUReserveInstances(uint32_t count, uint32_t* outFirstInstance);

// Copies the frame's staged records into that slot's GPU buffer with one
// queue.writeBuffer; call after the records were written (the adapter calls it
// right after ImSlug::PreprocessDrawData) and before ImSlugWGPUDraw.
// instanceCount is clamped; draws beyond the uploaded range are no-ops.
bool ImSlugWGPUUploadInstances(uint32_t frameIndex, uint32_t instanceCount);

struct ImSlugWGPUDrawInfo
{
    uint32_t FrameIndex; uint32_t FirstInstance; uint32_t InstanceCount;  // FirstInstance slot-relative
    const float* MVP;         // 16 floats, display px -> clip (DisplaySize, never the framebuffer size)
    float ViewportWidth, ViewportHeight;                                  // framebuffer px (SlugDilate's dim)
    uint32_t ScissorX, ScissorY, ScissorW, ScissorH;                      // caller owns viewport + scissor state
    uint32_t DebugFlags;                                                  // ImSlugWGPUDebugOverlay, 0 = off
};
void ImSlugWGPUDraw(WGPURenderPassEncoder pass, const ImSlugWGPUDrawInfo*);

enum ImSlugWGPUDebugOverlay
{
    ImSlugWGPUDebugOverlay_None = 0,
    ImSlugWGPUDebugOverlay_GlyphBounds = 1 << 0,  // 1px outline of the drawn quad
    ImSlugWGPUDebugOverlay_BandGrid = 1 << 1,     // H/V band checkerboard
    ImSlugWGPUDebugOverlay_RefHeat = 1 << 2,      // band-walk cost heatmap
};

// Display px (Y down) -> clip ortho, row-major. WebGPU clip space is Y UP, so -
// unlike the Vulkan helper - row 1 negates Y (mvp[5] = -2/H, mvp[7] = 1); the
// same convention as imgui_impl_wgpu's projection matrix. Callers pass the ImGui
// *display* (logical) size: instances are in display space, and a
// framebuffer-sized projection (DisplayFramebufferScale > 1) draws the text
// 1/scale too small and off its own scissor - invisible at DPI != 1.
inline void ImSlugWGPUBuildOrthoMVP(float displayWidth, float displayHeight, float mvp[16]);
```

Module-level singleton (like the ImGui backends): one device per process, the
module owns everything except `Device`/`Queue` and the render-pass encoder.
`Init` returns false (leaving nothing behind) when the device objects or formats
are unusable (render target must be a color format; a depth format, when given,
must be depth-stencil), when a required object cannot be created, or when
`maxTextureDimension2D` is below the 4096-texel pool width. `Shutdown` is
idempotent. The adapter maps `ImGui_ImplWGPU_InitInfo` onto this struct
(`PipelineMultisampleState`, `RenderTargetFormat`, `DepthStencilFormat`,
`NumFramesInFlight`, `Config::MaxInstancesPerFrame`).

## Frozen layouts

**Bind group 0** (one group; entry 3 is bound with a dynamic offset):

| entry | WGSL type | resource | visibility |
|---|---|---|---|
| 0 | `texture_2d<f32>` | curve pool (RGBA16F) | Fragment |
| 1 | `texture_2d<u32>` | band pool (RG16_UINT) | Fragment |
| 2 | `var<storage, read>` | paint records (`ImSlugPaintRecord`) | Fragment |
| 3 | `var<uniform>` | per-draw params (dynamic offset) | Vertex + Fragment (the fragment shader reads `debug_flags`) |

**No samplers anywhere**: every access is `textureLoad` (nearest, no filtering;
manual row wrap only through `CalcBandLoc`).

**Uniform params record (80 B, the push-constant replacement)** -
`static_assert`ed against the WGSL `Pc`:

| offset | field |
|---|---|
| 0/16/32/48 | `mvp0..mvp3` (`vec4<f32>`, the four MVP rows) |
| 64 | `viewport` (`vec2<f32>`) |
| 72 | `debug_flags` (`u32`) |
| 76 | 4 B trailing padding |

Dynamic-offset stride = `alignUp(max(80, minUniformBufferOffsetAlignment), alignment)`.
The WebGPU default alignment is 256 -> stride 256; the round-up also keeps the
offsets valid for an alignment that does not divide 80 (e.g. 64 -> 128).

**Instance record** (frozen, `imslug_internal.h`): `ImSlugGlyphInstance`, 96 B,
one vertex buffer with `stepMode = Instance`, `arrayStride 96`, locations 0..7
(Float32x4 x4, Uint32x2, Uint32, Float32x4, Uint32) - identical to the Vulkan
module's attribute table; location 7 is the `R32_UINT` `PaintIndex`.

**Paint record**: `ImSlugPaintRecord`, 352 B, fields at 0/16/32/48/64/80/96,
`Stops: array<vec2<u32>, 32>`. A minimal one-record buffer exists from `Init`
on, so the bind group is valid for plain text too (`kNoPaintIndex`).

## Behavior rules (implemented)

* **Per-slot instance buffers**: one CPU ring (`frames_in_flight x capacity`,
  returned by `ImSlugWGPUGetInstanceData`) plus one GPU
  `VERTEX | COPY_DST` buffer **per slot** (a single buffer + `writeBuffer`
  would overwrite in-flight frames). The whole staged range is written once per
  frame (`ImSlugWGPUUploadInstances`).
* **Draws use a `setVertexBuffer` offset, never `firstInstance`** (base-instance
  is feature-gated; offsets are always core-valid):
  `draw(4, InstanceCount)` with the binding offset `FirstInstance * 96`.
* **Uploads**: pool rows go through `queue.writeTexture` (one call per
  contiguous range); the first upload after creation/growth sends
  `[0, UsedRows)`, later frames `[DirtyRowMin, DirtyRowMax]`. `UploadPools`
  clears the dirty rows **of exactly the pools it consumed** and returns false
  without uploading anything when a pool cannot be uploaded - never a
  half-uploaded pair.
* **Growth** (mirrors the Vulkan ring; WebGPU has no device wait, so growth
  destroys the old resource and the implementation defers in-flight reads):
  * instances: default capacity 16384, doubling (at least to observed demand),
    capped at 8x the default; one-time stderr warning per capacity naming demand
    vs capacity. An explicit `MaxInstancesPerFrame` is a hard cap - every
    overflowing run drops. Growth invalidates `GetInstanceData` pointers.
  * uniform ring: one 80 B record per Slug batch,
    `NumFramesInFlight x MaxBatchesPerFrame` regions; default 2048 batches, the
    same growth/warning policy capped at 8x (no explicit knob in the init info).
  * pools: `maxTextureDimension2D` (`wgpuDeviceGetLimits`; the spec-default
    fallback is 8192, the reachable limit since the pool design allows 65536
    rows) caps the pool height; an over-height frame warns once (heights +
    actionable knob) and disables Slug for that frame. Paint growth respects
    `maxStorageBufferBindingSize` (fallback 128 MiB) with the same one-time
    warning shape.
* **Error scopes**: the module cannot attach callbacks to a device it does not
  create, so `ImSlugWGPUInit` brackets everything it creates and
  `ImSlugWGPUUploadPools` brackets the frame's growth/recreation work
  (validation error scope; the pop callback prints the first failure once via
  stderr, repo style). **The app must install
  `WGPUDeviceDescriptor::deviceLostCallbackInfo` +
  `uncapturedErrorCallbackInfo` at device creation** for device-lost /
  uncaptured-error visibility (the example does). The in-place instance-ring
  growth in `ReserveInstances` is outside both scopes and relies on the
  device's own handling. No exceptions, no aborts.
* **Release-safe early-outs** in `ImSlugWGPUDraw`: out-of-range `FrameIndex`,
  `FirstInstance` beyond the uploaded range, null MVP, empty scissor, and draws
  before any pool upload are no-ops; `InstanceCount` is clamped to the remaining
  slot capacity. Debug builds assert. The caller owns the viewport state (bind
  a viewport matching `ViewportWidth/Height`; the ImGui backend sets one per
  frame from `ImDrawData`).
* **Debug overlay**: flags 1/2/4 ride the uniform record; RefHeat wins over
  BandGrid; geometry/placement/fallbacks untouched. The adapter seeds the
  initial value from `Config::DebugDrawBounds` (glyph bounds); the runtime
  toggle mirrors the Vulkan pair.

## ImGui adapter protocol

```c
bool ImSlug_ImplWGPU_Init(const ImGui_ImplWGPU_InitInfo*);  // maps formats/MSAA/frames; acquires the default queue for the module lifetime
void ImSlug_ImplWGPU_Shutdown();                            // unregisters the render IO, releases the queue; ImSlug::Shutdown() is separate
void ImSlug_ImplWGPU_UploadPools();                         // once per frame, before the render pass
void ImSlug_ImplWGPU_RenderDrawData(ImDrawData*, WGPURenderPassEncoder);
void ImSlug_ImplWGPU_SetDebugOverlay(uint32_t);             // ImSlugWGPUDebugOverlay flags
uint32_t ImSlug_ImplWGPU_GetDebugOverlay();
```

Init order (all required): `ImGui_ImplWGPU_Init` (stock, unchanged) ->
`ImSlug::Init()` -> `ImSlug_ImplWGPU_Init` -> `AddFont*`. The renderer needs
`ImGuiBackendFlags_RendererHasTextures`.

Per-frame order contract: `ImSlug_ImplWGPU_UploadPools()` **before the render
pass** -> begin pass -> `ImSlug_ImplWGPU_RenderDrawData(draw_data, pass)`.

* `UploadPools` advances the frames-in-flight slot, resets the reserve cursors,
  applies pending ring growth and uploads the dirty pool rows. Exactly once per
  ImGui frame (a second call asserts in debug); `RenderDrawData` asserts when it
  never ran.
* `RenderDrawData` stores DisplayPos/DisplaySize/FramebufferScale + the
  framebuffer size from the draw data, runs `ImSlug::PreprocessDrawData` (text
  runs -> callback commands), writes the frame's staged instances with one
  `queue.writeBuffer`, then hands the data to
  `ImGui_ImplWGPU_RenderDrawData`. Non-text geometry stays on the stock path.
* The batch callback reads `ImGui_ImplWGPU_RenderState` from
  `GetPlatformIO().Renderer_RenderState` (Device + RenderPassEncoder only; the
  adapter tracks DisplayPos/DisplaySize/FramebufferScale itself), sets its own
  pipeline, bind group (dynamic offset), vertex buffer and scissor, and draws.
  The batch MVP maps **display space** (DisplaySize, the instances' space) to
  clip - exactly like `imgui_impl_wgpu`'s projection - while
  ViewportWidth/Height (pc.viewport, SlugDilate) stay the *framebuffer* size so
  the dilation remains 1 physical pixel at any DPI. The core injects
  `DrawCallback_ResetRenderState` right after every Slug run; the stock backend
  re-establishes ImGui's state via `ImGui_ImplWGPU_SetupRenderState` - never
  skip it.
* Scissor: batch clip rects arrive in ImGui display coordinates; convert with
  DisplayPos/FramebufferScale and clamp to the framebuffer exactly like the
  stock backend (the pass encoder rejects off-bounds rects).
* Fallback: `ImSlug_ImplWGPU_RenderDrawData` renders through the stock path when
  the module is not initialized (or the draw data is null); a failed
  `ImSlugWGPUInit` returns false and the app keeps stock ImGui text (invariant 6).

## Browser stack (Emscripten + web shell)

`example/web/main.cpp` is the platform shell around the shared app
(`example/common/example_app.*`), built as `imslug_example_web` by
`scripts/build_web.*`; the page shell is `example/web/shell.html`.

* Emscripten >= 4.0.10 (CI pins 5.0.1); ports `--use-port=emdawnwebgpu`
  (supplies `<webgpu/webgpu.h>`, Dawn flavour) and `--use-port=contrib.glfw3`.
  Link flags: `-sWASM=1 -sASYNCIFY=1 -sALLOW_MEMORY_GROWTH=1
  -sNO_EXIT_RUNTIME=1 -sDISABLE_EXCEPTION_CATCHING=1 -sNO_FILESYSTEM=1
  -sASSERTIONS=0 -sSTACK_SIZE=1048576 --shell-file=example/web/shell.html`;
  compile flag `-Wno-dollar-in-identifier-extension` (EM_ASM `$0`).
* Device/surface: raw C API bootstrap (instance with `TimedWaitAny`, blocking
  adapter/device requests, canvas selector `#canvas`); resize re-configures the
  surface; Emscripten presents implicitly at the end of the rAF callback - there
  is no `wgpuSurfacePresent` call. The device descriptor installs the
  device-lost + uncaptured-error callbacks (the app owns the device).
* Page hooks (JS bodies live in `shell.html`; the C++ side calls them through
  tiny `EM_ASM` blocks): `window.__imslugFrames` (int),
  `window.__imslugReady` (bool), `window.__imslugError` (str), plus the shell
  functions `__imslugSetError(message)`, `__imslugFrameHook()` and
  `__imslugDeviceLostHook()`. The smoke test reads the three globals;
  `shell.html` shows the WebGPU capability notice when `navigator.gpu` / an
  adapter is missing.
* Per-frame order (shared with the desktop shell): poll -> resize+configure ->
  `ImGui_ImplWGPU_NewFrame` -> `ImGui_ImplGlfw_NewFrame` -> shared app builds
  the frame -> `ImGui::Render` -> capture stats -> `ImSlug_ImplWGPU_UploadPools`
  -> begin pass -> `ImSlug_ImplWGPU_RenderDrawData` -> end/submit -> JS hooks.
  Queue writes (`writeBuffer`/`writeTexture`) may be issued while the pass is
  being encoded, but keep the documented order.
* No filesystem and no desktop-only features on web: embedded fonts only (no
  `--font`/env override), no screenshot CLI, no GPU timestamps, no vsync
  toggle; the CJK and color-emoji subsets embed at build time when the fetched
  fonts are present (the scenes show their "not embedded" message otherwise).

## Build / smoke tooling

* `scripts/build_web.*`: checks emsdk, optionally embeds the fetched CJK subset
  and a COLR v1 color-emoji subset with the **native** `imslug_gen` (a
  cross-compiled binary cannot run during the web build) into
  `build-web/include/imslug_example_{cjk,emoji}.h` and passes
  `-DIMSLUG_CJK_EMBED_HEADER=` / `-DIMSLUG_EMOJI_EMBED_HEADER=` (the emoji
  subset is produced by `scripts/subset_emoji_for_web.py`, which drops the SVG
  tables and needs fonttools); configures with `-DIMSLUG_BUILD_EXAMPLES=ON
  -DIMSLUG_BACKEND_VULKAN=OFF -DIMSLUG_BUILD_TESTS=OFF
  -DIMSLUG_BUILD_C_EXAMPLE=OFF -DIMSLUG_BUILD_TOOLS=OFF`; builds
  `imslug_example_web`; assembles the Pages site in `build-web/site/`
  (`index.html`, `index.js`, `index.wasm`, `.nojekyll`). Overrides mirror the
  other scripts (`IMSLUG_BUILD_DIR`, `IMSLUG_BUILD_TYPE`, `IMSLUG_CMAKE`,
  `IMSLUG_RECONFIGURE`, `IMSLUG_CMAKE_DEFINES`, plus `IMSLUG_NATIVE_BUILD_DIR`).
* `scripts/web_smoke.*`: Node >= 22, puppeteer-core pinned (25.13.0), Chrome;
  serves the site and runs two passes by default: the primary pass
  (`IMSLUG_SMOKE_DPR`, default 1) waits for `window.__imslugReady` + >= 60
  frames, checks `__imslugError` empty, the console/page errors and an ink
  fraction >= 0.002 in a screenshot (`build-web/smoke.png`), and the hi-DPI
  pass (800x500 @ DPR 3, `IMSLUG_SMOKE_HIDPI=0` disables it) additionally
  presses the shared app's `M` text-renderer toggle and requires the Slug and
  atlas frames to show the same *bright* ink (masks dilated by 1 px): Slug ink
  >= 50% of atlas ink, <= 25% of the combined ink without a nearby counterpart
  and <= 70% differing in luma. The `M`-key swap in Slug mode is what catches
  text drawn in the wrong coordinate space (the framebuffer-scale bug) at phone
  DPR even though the canvas is far from blank. Artifacts:
  `smoke.png`, `smoke-hidpi.png`, `smoke-hidpi-atlas.png`. Retries the whole
  run (`IMSLUG_SMOKE_RETRIES`) and prints collected console/page errors (plus
  the analyzer stats) per failed attempt. Adapters: `swiftshader` (default; `--use-webgpu-adapter=swiftshader`
  - note `force_swiftshader` is *not* a valid value and silently falls back to
  the default), `native` (default adapter), `lavapipe` (software Dawn over Mesa
  lavapipe: `--enable-features=Vulkan --disable-vulkan-surface`, headed Chrome,
  needs a display + `VK_DRIVER_FILES=<lvp_icd.x86_64.json>`; run under
  `xvfb-run -a`). `IMSLUG_SMOKE_NO_HOOKS=1` smoke-tests a foreign WebGPU page
  (dwell + screenshot + pixel check only).
* `.github/workflows/pages.yml`: build -> smoke -> deploy; Pages source =
  "GitHub Actions". The runner uses **lavapipe + xvfb** (SwiftShader devices die
  at startup on GitHub runners - verified: three attempts, all "device lost"),
  and the smoke step starts `continue-on-error: true` until proven stable.

## Pitfalls (learned, keep)

* **cmd.exe expands `%VAR%` at parse time inside parenthesized blocks.**
  `scripts/build_web.bat` therefore uses flat lines + `goto :label` blocks for
  the emsdk environment (reading `%EMSDK%` after it is set).
* **Escape a literal `>` in an `echo` message** (as `^>`): a bare `>` inside
  the message - e.g. `>= 4.0.10` before the `>&2` - redirects the rest of the
  line instead of being printed (`scripts/build_web.bat` writes
  `^(^>= 4.0.10^)`).
* **clang-format-on-write reflows C++**: EM_ASM JavaScript bodies must live in
  `example/web/shell.html`; every EM_ASM call site in C++ carries
  `// clang-format off` / `// clang-format on`.
* **Generated headers are excluded from `scripts/format.*`**
  (`shaders/imslug_shaders_vulkan.h`, `shaders/imslug_shaders_webgpu.h`,
  `example/common/imslug_example_{asset,font}.h`) - never format or hand-edit
  them; regenerate via the owning script.
* **`-sNO_FILESYSTEM=1` + the shared app compile out the desktop probes**: file
  I/O, `--font`/env font overrides and the scene-9 path probing are
  `#ifndef __EMSCRIPTEN__`; the web demo uses the embedded fonts (packed asset
  for Slug, TTF for ImGui's loader, plus the generated CJK/emoji subset headers
  when the build embedded them).
* **Slug instances are in ImGui display space - build the batch MVP from
  `DisplaySize`, never from the framebuffer size.** `DisplayFramebufferScale`
  is > 1 on phones/retina; a framebuffer-sized projection draws the text 1/scale
  too small, and the batch's own scissor rect (converted to physical pixels)
  clips it away, so the text simply vanishes. Only
  `ViewportWidth/Height`/`pc.viewport` (SlugDilate) are framebuffer pixels. The
  bug shipped once (web demo invisible text on a phone) - the hi-DPI smoke pass
  and the ImGui golden's 2x/3x framebuffer scales are the regression guards.
* **`navigator.gpu` is undefined on `about:blank`** in headless Chrome - probe
  capabilities on a real http(s) origin (the smoke server), not an empty page.
* The generated WGSL header embeds **NUL-terminated** string arrays
  (`imslug_slug_vert_wgsl` / `imslug_slug_frag_wgsl`); the module passes
  `{ arr, WGPU_STRLEN }`. Keep the emitter's terminator contract
  (`*_len` excludes the NUL).
* WGSL parse/pipeline errors surface when `CreatePipeline` runs (inside Init's
  error scope); a failed Init leaves nothing behind and the adapter returns
  false.

## Verification

* Web: `scripts/build_web.*` then `scripts/web_smoke.*` (default swiftshader;
  rerun with `IMSLUG_SMOKE_ADAPTER=native` on a GPU machine).
* Shaders: `scripts/gen_shaders.* --check` verifies **both** committed headers
  (SPIR-V + WGSL) - never regenerate only half (see the `imslug-shaders` skill).
* Native suite stays green: `scripts/build.*`, `scripts/test.*`,
  `scripts/golden.*` (the Vulkan golden is the arbiter for the shared pipeline;
  the browser demo validates the WGSL in-browser).
* `scripts/check_docs.*` before finishing (AGENTS.md docs policy).
