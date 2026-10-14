---
name: imslug vulkan backend
description: Vulkan backend contract for imslug - the frozen standalone rendering core (pipeline/descriptor/upload/instance-ring API, instance record layout) and the ImGui adapter rules (dynamic scissor + reset-render-state callback protocol, validation expectations including the usual VUIDs). Load when working in backends/, on the golden harness, or on draw-data callbacks.
---

# imslug Vulkan backend contract

Two layers:

* **Standalone core** - `backends/imslug_impl_vulkan.*`: the standalone,
  ImGui-free rendering core (pool textures, uploads, instance ring, pipelines,
  draw).
* **ImGui adapter** - the `ImSlug_ImplVulkan_*` adapter in the same file
  (bottom section): bridges `ImGui_ImplVulkan_InitInfo`/`ImDrawData` onto the
  core + the core's `ImSlugRenderIO` callback interface. The core section stays
  ImGui-agnostic; only the adapter block includes imgui/imgui_impl_vulkan
  headers.

Vulkan code lives in `backends/` only; the core stays backend-independent.

## Frozen core API (`backends/imslug_impl_vulkan.h`)

```cpp
struct ImSlugVulkanInitInfo
{
    VkPhysicalDevice PhysicalDevice; VkDevice Device; VkQueue Queue; uint32_t QueueFamily;
    const VkAllocationCallbacks* Allocator;      // may be null
    uint32_t ImageCount;                         // frames in flight = min(ImageCount, 3)
    VkSampleCountFlagBits MSAASamples;           // from ImGui_ImplVulkan_InitInfo.PipelineInfoMain
    VkPipelineCache PipelineCache;               // optional
    VkRenderPass RenderPass; uint32_t Subpass;   // render-pass path (UseDynamicRendering == false)
    bool UseDynamicRendering;                    // dynamic-rendering path
    VkPipelineRenderingCreateInfo PipelineRenderingCreateInfo;  // copied (up to 4 color formats)
    uint32_t MaxInstancesPerFrame;               // 0 => 16384
};

bool ImSlugVulkanInit(const ImSlugVulkanInitInfo*);   // false => not usable (formats missing)
void ImSlugVulkanShutdown();                          // waits for the device, frees everything

bool ImSlugVulkanUploadPools(VkCommandBuffer cmd, ImSlugPools* pools, uint32_t frameIndex);  // records, does not submit

uint32_t ImSlugVulkanGetFramesInFlight();
uint32_t ImSlugVulkanGetInstanceCapacity();
ImSlugGlyphInstance* ImSlugVulkanGetInstanceData(uint32_t frameIndex);  // ring slot base

struct ImSlugVulkanDrawInfo
{
    uint32_t FrameIndex;                       // < frames in flight
    VkRenderPass RenderPass; uint32_t Subpass; // VK_NULL_HANDLE => dynamic rendering
    uint32_t FirstInstance;                    // slot-relative (indexes GetInstanceData(FrameIndex))
    uint32_t InstanceCount;                    // clamped to the remaining slot capacity
    const float* MVP;                          // 16 floats, screen px -> clip, row-major
    float ViewportWidth, ViewportHeight;       // framebuffer px (SlugDilate's dim)
    VkRect2D Scissor;                          // set by Draw (dynamic state)
    uint32_t DebugFlags;                       // debug overlay (ImSlugVulkanDebugOverlay), 0 = off
};
void ImSlugVulkanDraw(VkCommandBuffer, const ImSlugVulkanDrawInfo*);

// Debug overlay (see docs/DESIGN.md section 6; shader side in the shaders
// skill): flags combine, RefHeat wins over BandGrid, geometry/placement and
// fallbacks are untouched. ImSlugVulkanDrawInfo::DebugFlags carries them into
// the push constants.
enum ImSlugVulkanDebugOverlay
{
    ImSlugVulkanDebugOverlay_None = 0,
    ImSlugVulkanDebugOverlay_GlyphBounds = 1 << 0,  // 1px outline of the drawn quad
    ImSlugVulkanDebugOverlay_BandGrid = 1 << 1,     // H/V band checkerboard
    ImSlugVulkanDebugOverlay_RefHeat = 1 << 2,      // band-walk cost heatmap
};

// Shared helper: screen-pixels (Y down) -> clip ortho, row-major. No Y negation
// (Vulkan NDC y points down); see the imslug-shaders skill.
inline void ImSlugVulkanBuildOrthoMVP(float width, float height, float mvp[16]);
```

Module-level singleton (like the ImGui backends): one device per process, all
Vulkan objects except the ones passed in `InitInfo` are owned by the module.
Maps 1:1 from `ImGui_ImplVulkan_InitInfo.PipelineInfoMain`
(`RenderPass`/`Subpass`/`MSAASamples`/`PipelineRenderingCreateInfo`), plus
`PhysicalDevice`/`Device`/`Queue`/`QueueFamily`/`Allocator`/`ImageCount`/
`PipelineCache`.

## Pipeline rules (implemented)

* Mirrors `PipelineInfoMain`: render pass + subpass, or dynamic rendering with
  the copied `VkPipelineRenderingCreateInfo`; MSAA from `MSAASamples`.
* Pipelines are created **lazily and cached per (render pass, subpass)** (16
  entries; dynamic rendering is one entry) - multi-viewport/render-pass changes
  just work. The pipeline cache (`VkPipelineCache`) is honoured.
* State: triangle-strip, cull NONE, fill, CCW; dynamic VIEWPORT + SCISSOR;
  depth/stencil disabled; MSAA per init.
* Blend state exactly matches stock `imgui_impl_vulkan` (color
  `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, alpha `ONE`/`ONE_MINUS_SRC_ALPHA`, ADD),
  so the shader must output **unpremultiplied** color (`alpha *= coverage`).
* Vertex input: **one instance-rate binding** (stride 96,
  `ImSlugGlyphInstance`; instance format v2 has the trailing `PaintIndex`) with attributes
  at locations 0..7; no vertex-rate
  attributes - the quad comes from `gl_VertexIndex`. Draws are
  `vkCmdDraw(4, instanceCount, 0, FrameIndex * InstanceCapacity + FirstInstance)`:
  `FirstInstance` is **slot-relative** (it indexes the records the caller wrote
  at `ImSlugVulkanGetInstanceData(FrameIndex)`) and the draw itself adds the
  ring base. Forgetting the base silently reads slot 0 for every frame; the
  golden harness pins it with a `FrameIndex = 1` +
  `FirstInstance != 0` case.
* Push constants: 4 MVP rows + viewport size + `DebugFlags` (76 B; the
  fragment shader reads the flags, so the layout range and the push itself use
  `VERTEX | FRAGMENT` stage flags — see the shaders skill).
* Required features: none beyond core - the pool formats
  (`R16G16B16A16_SFLOAT`, `R16G16_UINT`) must support `SAMPLED_IMAGE`
  (checked at init, else `Init` returns false). The app needs apiVersion >= 1.2
  because the committed SPIR-V is 1.5 (glslang `--target-env vulkan1.2`).
* No `imgui.h` in this module, ever.

## Descriptor rules (implemented)

* Separate `VkImageView` + `VkSampler` (nearest, clamp) combined-image-sampler
  descriptors; the module owns a pool with `FREE_DESCRIPTOR_SET_BIT`; one set
  per frame in flight, written only when the pool images are (re)created - the
  textures are static after upload.
* Binding 0: curve pool `sampler2D` (RGBA16F); binding 1: band pool
  `usampler2D` (R16G16_UINT); binding 2: the paint record array
  (`STORAGE_BUFFER`, `std430`, `readonly buffer` in the fragment shader;
  `ImSlugPaintRecord`, 352 bytes). Fragment stage only. A minimal 1-record
  buffer exists from `ImSlugVulkanInit` on, so the descriptor set is valid for
  plain text too (the shader never reads it when every instance carries
  `kNoPaintIndex`); growth recreates the buffer (device wait) and rewrites the
  sets.

## Upload rules (implemented)

* `ImSlugVulkanUploadPools(cmd, pools, frameIndex)` creates/recreates both
  images to the pools' allocated size, records staging copies + barriers for
  the dirty rows into `cmd`, and clears the dirty rows **of exactly the pools
  it consumed** (per-pool clearing: a missing pool view must not drop the other
  pool's pending rows). The caller submits. The **first** upload after creation
  sends `[0, UsedRows)`; later frames send `[DirtyRowMin, DirtyRowMax]`.
* **One staging buffer per frame in flight**: the CPU memcpys for frame N
  use `Staging[N]` only, so recording a later frame can never overwrite bytes an
  earlier frame's recorded copy still has to read. Recording frame N again
  requires its previous submission to have completed (the standard
  frames-in-flight contract) - that is also why staging growth needs no device
  wait. Passing the wrong `frameIndex` (or one >= frames in flight) fails the
  call (debug assert + `false`).
* Growth (rare) recreates the image and re-uploads everything, with a
  `vkDeviceWaitIdle` because in-flight frames may still read the old image;
  the descriptor sets are rewritten then. One queue is assumed.
* Barrier protocol per image: `SHADER_READ_ONLY_OPTIMAL` (or `UNDEFINED` on
  first use) -> `TRANSFER_DST_OPTIMAL` -> copy -> `SHADER_READ_ONLY_OPTIMAL`
  (src stage FRAGMENT_SHADER / TRANSFER). Verification-clean with the layer.
* Instance ring: host-visible, host-coherent, persistently mapped,
  `frames_in_flight x capacity` records; the caller writes one frame's records
  into `ImSlugVulkanGetInstanceData(frameIndex)` and batches them with
  slot-relative `FirstInstance` (the buffer is bound whole, never per-batch;
  the draw adds the ring base, see the pipeline rules).
* Paint pool: `ImSlugPools::Paints` (bytes, `ImSlugPaintRecord` array) is
  uploaded into the binding-2 storage buffer in the same call and through the
  same per-frame staging buffer. The first upload covers `[0, Count)`, later
  frames the dirty record range; growth recreates the buffer with a
  `vkDeviceWaitIdle` and forces a full re-upload. Barrier protocol: the buffer
  is read by the fragment shader, so each upload brackets the copy with
  `FRAGMENT_SHADER/READ -> TRANSFER/WRITE` and
  `TRANSFER/WRITE -> FRAGMENT_SHADER/READ` `VkBufferMemoryBarrier`s
  (`VK_WHOLE_SIZE`, one queue assumed).
* Release-safe early-outs: `ImSlugVulkanDraw` is a no-op for an
  out-of-range `FrameIndex`, a slot-relative `FirstInstance` at/beyond the
  capacity, a null MVP, and when no pool upload happened yet (descriptor sets
  not written); `InstanceCount` is clamped to the remaining slot capacity.
  Debug builds still assert on all of those.

## Instance record (frozen, `imslug_internal.h`)

`ImSlugGlyphInstance`, **96 bytes (format v2)**, `static_assert`ed against the
vertex input: em bbox, screen rect (px, Y down), inverse Jacobian (screen -> em),
band scale/offset, glyphLoc (band pool), packed bandMax word with **E at bit 28**
(not "bit 12 of the raw word"), straight-alpha color, and the
`uint32_t PaintIndex` at offset 92 (`kNoPaintIndex` = 0xFFFFFFFF for plain
glyphs; a valid index selects an `ImSlugPools::Paints` record). The placement must
be axis-aligned (scale+translate); the shader dilates along the screen-space
corner diagonals. `ImSlugBuildGlyphInstance()`/`...Scaled()` fill one record
(the scaled variant exists for color layers with a per-axis positive scale) and
always set `PaintIndex = kNoPaintIndex`; `ImSlugPackBandMaxWord()` produces the
packed word. The base layout's first 92 bytes are unchanged - the attribute
locations 0..6 keep their meaning, loc 7 is the `R32_UINT` addition.

## Validation expectations

* Validation-clean at runtime (example + goldens run with
  `VK_LAYER_KHRONOS_validation`; any *error* fails the golden run). Quote the
  exact VUID from the layer output; never silence a validation error.
* Frequent failures and their real cause:
  * `vkCreateGraphicsPipelines(): ... Push constant are used, but VkPipelineLayout ... doesn't set VK_SHADER_STAGE_FRAGMENT_BIT`
    (VUID-VkGraphicsPipelineCreateInfo-layout-07987) / `vkCmdPushConstants(): ...` (VUID-vkCmdPushConstants-offset-01796) -
    the fragment shader declares the push constant block (`DebugFlags`), so the layout range and every
    `vkCmdPushConstants` call must name `VERTEX | FRAGMENT`, not vertex only.
  * `vkCreateGraphicsPipelines(): pNext<VkPipelineRenderingCreateInfo>...` /
    "not compatible with the current render pass/subpass" -
    `PipelineInfoMain` not mirrored (render pass, subpass, MSAA, dynamic
    rendering), or a dangling `pColorAttachmentFormats` pointer passed to Init
    (the struct is copied; keep the array alive until the call returns).
  * `vkCmdBeginRendering(): dynamicRendering is not enabled` - the
    `VkPhysicalDeviceVulkan13Features::dynamicRendering` bit was not enabled at
    device creation.
  * `pCode ... not adhere to the validation rules` / "Invalid SPIR-V binary
    version 1.5 for target environment SPIR-V 1.3" - instance/device apiVersion
    below 1.2.
  * "image layout ... is not VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" - the
    post-upload barrier is missing or mis-scoped.
  * "vertex buffer ... is not bound / too small" - the instance ring size or
    `firstInstance` offset is wrong.
  * "scissor ... is outside the render area" - the scissor is in framebuffer
    pixels; use the draw data's framebuffer size, not the OS window size.
  * Image in layout `UNDEFINED` while expecting `PRESENT_SRC_KHR` - a barrier
    claimed the wrong old layout (e.g. a screenshot copy on an image index that
    was never rendered to).
* Golden harness: `tests/golden/test_golden_vulkan.cpp` (no surface/swapchain,
  lavapipe-friendly; `scripts/golden.*`). Comparison contract: per-glyph CPU
  reference at the same em coordinates/`pixelsPerEm`, the same
  `fill < 0.001` discard and the same straight-alpha blend, 8-bit destination;
  thresholds = strict mean (<= 0.25 LSB) + every pixel within 3 LSB except a
  small budget of *discontinuity outliers* (a sample landing on a control-point
  coordinate, where `CalcRootCode`'s sign-bit eligibility table legitimately
  resolves either way - up to ~0.5 coverage at that one pixel). PPMs land in
  `<build>/golden/`.
* The same binary runs the backend contract regressions before the matrix
  (full-frame readbacks, no golden files): the *ring-slot* case (distinct
  content in slot 0 vs slot 1, draw `FrameIndex = 1`, slot-relative
  `FirstInstance = 2`; fails without the ring base), the *upload-staging* cases
  (consecutive frames recorded/back-to-back before any submit, each adding
  different dirty rows; fails on a shared staging buffer), and the *draw-clamp*
  case (out-of-range `FrameIndex`/`FirstInstance` must leave the target clear).
  They run in both the render-pass and dynamic-rendering invocations.

### ImGui golden test (`imslug_imgui_golden`, both render paths)

`tests/golden/test_imgui_golden_vulkan.cpp` renders one fixed ImGui scene twice
(headless offscreen, 1024x640): with a Slug font and with the same font through
ImGui's own stb loader (atlas path). Everything except the font is identical in
both variants. Measured on the RTX 5080 (render pass and dynamic rendering give
the same numbers):

* mean channel difference 1.241 LSB (budget 8), 0.36% of pixels above 64 LSB
  (budget 2%) - the AA differences live on glyph edges only;
* **zero** ink pixels without a matching ink pixel within 1 px in either
  direction (layout exactness: catches shifted, missing or wrongly scaled
  glyphs);
* validation clean (0 messages) in both paths.

Test-side pitfalls that produced VUIDs before they were fixed (the *adapter* was
clean throughout): the render pass must own its attachment layout
(`initialLayout` UNDEFINED, `finalLayout`, no extra barriers) while the
dynamic-rendering path needs its own UNDEFINED -> COLOR_ATTACHMENT ->
TRANSFER_SRC barriers, and `PipelineRenderingCreateInfo.pColorAttachmentFormats`
must point at storage that outlives the whole module lifetime (a block-scoped
format array is a use-after-scope crash).

### Emoji golden test (`imslug_emoji_golden`, both render paths)

`tests/golden/test_emoji_golden_vulkan.cpp` renders emoji strings (Noto Color
Emoji, the fetched gitignored asset) as one instance per COLR layer and compares
the GPU against a CPU composite: `ImSlugRasterSample` for the coverage plus
`ImSlugSamplePaintRecord` (the mirror of the shader's paint code) for the layer
paint, blended straight-alpha into an 8-bit destination. 30 cases at 16/32/64 px
and 1x/2x/3.5x zoom, skipping (exit 77) when the font or a Vulkan device is
absent. Measured: worst mean 0.031 LSB, worst channel 21 LSB (isolated
discontinuity ties), validation clean; the same pass rule as the monochrome
goldens
(mean <= 0.25, outlier budget 0.05 % + 4).

## Example app

`example/glfw_vulkan/main.cpp` (option `IMSLUG_BUILD_EXAMPLES=ON`): a GLFW window
+ swapchain rendering an ordinary ImGui app (stock GLFW+Vulkan backends, imslug
for the registered font), validation layers on by default
(`IMSLUG_VALIDATION=0` disables, `IMSLUG_VALIDATION_VERBOSE=1` adds info
messages). Scene switcher (keys 1-9 / the radio buttons): demo window,
widgets/plots/tables, clipping torture, mixed Slug+atlas fonts, runtime
`style.FontScaleDpi`, the 5k-glyph stress scene, the fetched CJK subset, the
color-emoji scene (BMP emoji at a size slider plus a single-glyph zoom for the
gradients) and the packed-asset scene. `--screenshot <file.ppm> [scene] [dpi_scale]` captures an
offscreen frame and prints the validation message/error counts on exit. The
frame loop cycles over `ImSlugVulkanGetFramesInFlight()` (not a hardcoded 3) and
null-checks the ring slot pointer - a 2-image swapchain must not touch ring
slot 2.

## ImGui adapter - callback protocol

Public API (declared in `backends/imslug_impl_vulkan.h`, implemented at the
bottom of `backends/imslug_impl_vulkan.cpp`; requires `ImSlug::Init()` and a
renderer with `ImGuiBackendFlags_RendererHasTextures`):

```c
bool ImSlug_ImplVulkan_Init(const ImGui_ImplVulkan_InitInfo*);  // maps PipelineInfoMain
// Additive: same as Init, but takes the Vulkan objects directly
// (ImSlugVulkanInitInfo) - the C API's init path and the hook for integrations
// that never hold an ImGui_ImplVulkan_InitInfo (custom/non-C++ renderers).
// Precedence: a nonzero ImSlugVulkanInitInfo::MaxInstancesPerFrame wins over
// Config::MaxInstancesPerFrame; 0 falls back to the config value.
bool ImSlug_ImplVulkan_InitFromVulkanInfo(const ImSlugVulkanInitInfo*);
void ImSlug_ImplVulkan_Shutdown();
void ImSlug_ImplVulkan_UploadPools(VkCommandBuffer cmd);        // per frame, outside the render pass
void ImSlug_ImplVulkan_RenderDrawData(ImDrawData*, VkCommandBuffer, VkPipeline mainPipeline = VK_NULL_HANDLE);

// Debug overlay (seeded from Config::DebugDrawBounds at Init; takes effect
// on the next RenderDrawData; ignored/asserted before Init).
void     ImSlug_ImplVulkan_SetDebugOverlay(uint32_t overlayFlags);  // ImSlugVulkanDebugOverlay
uint32_t ImSlug_ImplVulkan_GetDebugOverlay();
```

**C API**: `imslug_c.h` mirrors the whole ImGui-facing surface with C
symbols (`imslug_impl_vulkan_init(imslug_vulkan_init_info)` etc.; the C Vulkan
mirror lives in `backends/imslug_c_vulkan.cpp`, invariant 7). `imslug_config` /
`imslug_vulkan_init_info` are fixed-size PODs with `int` booleans; the Vulkan
declarations only appear when `vulkan.h` is visible (or `IMSLUG_C_VULKAN` is
defined). ABI pinned by `tests/test_c_abi.c` (C11, strict warnings) +
`tests/test_c_api.cpp`. Binding guide: `docs/BINDINGS.md`.

Order contract: `UploadPools` -> `RenderDrawData`. A manual
`ImSlug::PreprocessDrawData` call is *not* needed (RenderDrawData runs it) and
before the fix it was harmful: the pre-pass is idempotent per frame now
(`Core::CommittedBatches` - a second call must not re-commit batches, because
`batch->FirstInstance` is a ring index after the first commit; the old
double-commit read the arena out of bounds and drew garbage instances;
regression: `TestPrepassIdempotence`). A manual
pre-pass before `UploadPools` also targets the previous ring slot.

1. `UploadPools` advances the frames-in-flight slot, resets the instance
   allocation cursor and records the dirty-row copies with
   `ImSlugVulkanUploadPools` - it **must** happen before the render pass
   (transfer commands are illegal inside one) and exactly once per frame: a
   second call in the same ImGui frame asserts in debug (it would skip an
   instance-ring slot and overwrite the staging buffer of a frame still in
   flight). `RenderDrawData` asserts in debug when `UploadPools` never ran
   (release: stale pools, glyphs may be empty).
2. `RenderDrawData` stores the draw-data display pos/scale + framebuffer size,
   calls the core pre-pass (rewriting Slug text runs into callbacks) and then
   hands the data to `ImGui_ImplVulkan_RenderDrawData`; all non-text geometry
   goes through the stock path, including the stock scissor/texture setup.
3. `ImDrawCmd::UserCallback` is dispatched **before** the backend sets scissor
   and texture state, so each Slug callback computes its **own dynamic scissor**
   from the batch's clip rect (already display coordinates, intersected with the
   command's own clip rect), converted with `DisplayPos`/`FramebufferScale` and
   clamped exactly like the stock backend.
4. Immediately after every Slug callback command the pre-pass injects a
   `GetPlatformIO().DrawCallback_ResetRenderState` command, so the stock backend
   re-establishes pipeline/descriptors/vertex buffers (never skip it, never
   move it). Without that hook (or `RendererHasTextures`) the pre-pass refuses to
   rewrite anything: a debug assert + a one-time stderr warning.
5. Draws are recorded into the active command buffer from
   `GetPlatformIO().Renderer_RenderState` (`ImGui_ImplVulkan_RenderState`:
   CommandBuffer/Pipeline/PipelineLayout), published for the duration of
   `ImGui_ImplVulkan_RenderDrawData`. One batch = one `ImSlugVulkanDraw` with the
   frame index, the main render pass (`VK_NULL_HANDLE` under dynamic rendering),
   the batch's slot-relative `FirstInstance`/`InstanceCount`, a Y-down ortho MVP
   for the framebuffer, the framebuffer size and the batch scissor.
6. Instances live in the core's instance ring: the adapter tracks the per-frame allocation
   cursor (`ReserveInstances`); exhaustion drops the overflowing run with a
   debug assert and `Statistics::DroppedRuns`.
7. Render pass: the pipeline mirrors `PipelineInfoMain.RenderPass/Subpass`, so
   multi-viewport render passes must be *compatible* with it (same
   formats/samples; the helper-created viewport passes are). Dynamic rendering
   needs `PipelineInfoMain.PipelineRenderingCreateInfo` with the color format
   array alive for the whole module lifetime (the adapter copies the struct but
   not the array - a block-scoped format array is a real crash). **Multi-viewport beyond compatible passes stays a documented
   deferral** (verified against pinned v1.92.9b): a secondary viewport created
   by the `ImGui_ImplVulkanH_*` helpers owns a *distinct* render pass
   (`ImGui_ImplVulkanH_Window::RenderPass`, created in
   `ImGui_ImplVulkanH_CreateOrResizeWindow`), while callback-time
   `ImGui_ImplVulkan_RenderState` exposes only `CommandBuffer/Pipeline/
   PipelineLayout` - no render pass/subpass. The adapter therefore cannot
   discover the active viewport's pass from the published state, and drawing
   Slug text into a foreign pass would use an incompatible pipeline. A correct
   path needs app cooperation (an explicit per-viewport pass API); deferred.
8. The pre-pass rewrites only `CmdBuffer` (scratch vector): geometry triangles
   keep exact order/offsets, each maximal text run becomes a callback command
   with `ElemCount = 0` and `ClipRect`/`TexRef` copied from the original; the
   vertex/index buffers are never mutated.

## Fallbacks

* Texture-pool overflow, unsupported formats, loader failure, or a renderer
  without `RendererHasTextures`: render through the stock ImGui path (or assert
  with a clear message for truly unsupported setups). Never leave a frame
  half-drawn, and never let the Slug path become mandatory for normal text.
* `ImSlugVulkanInit` returning false (missing format support, height beyond
  `maxImageDimension2D`) means "no Slug": the adapter must fall back cleanly.
