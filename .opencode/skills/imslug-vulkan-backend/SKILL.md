---
name: imslug vulkan backend
description: Vulkan backend contract for imslug - the frozen M2 standalone rendering core (pipeline/descriptor/upload/instance-ring API, instance record layout) and the M3 ImGui adapter rules (dynamic scissor + reset-render-state callback protocol, validation expectations including the usual VUIDs). Load when working in backends/, on the golden harness, or on draw-data callbacks.
---

# imslug Vulkan backend contract

Two layers:

* **M2 (implemented)** - `backends/imslug_impl_vulkan.*`: the standalone,
  ImGui-free rendering core (pool textures, uploads, instance ring, pipelines,
  draw). This is the contract below; it is frozen for M3 to build on.
* **M3 (planned, adapter)** - `ImSlug_ImplVulkan_*` on top of the core, mapping
  `ImGui_ImplVulkan_InitInfo`/`ImDrawData` onto it. The callback protocol at the
  end of this file still applies when that adapter lands.

Vulkan code lives in `backends/` only; the core stays backend-independent.

## M2 frozen API (`backends/imslug_impl_vulkan.h`)

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
};
void ImSlugVulkanDraw(VkCommandBuffer, const ImSlugVulkanDrawInfo*);

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
  just work (R4). The pipeline cache (`VkPipelineCache`) is honoured.
* State: triangle-strip, cull NONE, fill, CCW; dynamic VIEWPORT + SCISSOR;
  depth/stencil disabled; MSAA per init.
* Blend state exactly matches stock `imgui_impl_vulkan` (color
  `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, alpha `ONE`/`ONE_MINUS_SRC_ALPHA`, ADD),
  so the shader must output **unpremultiplied** color (`alpha *= coverage`).
* Vertex input: **one instance-rate binding** (stride 92,
  `ImSlugGlyphInstance`) with attributes at locations 0..6; no vertex-rate
  attributes - the quad comes from `gl_VertexIndex`. Draws are
  `vkCmdDraw(4, instanceCount, 0, FrameIndex * InstanceCapacity + FirstInstance)`:
  `FirstInstance` is **slot-relative** (it indexes the records the caller wrote
  at `ImSlugVulkanGetInstanceData(FrameIndex)`) and the draw itself adds the
  ring base. Forgetting the base silently reads slot 0 for every frame (the
  review's B1); the golden harness pins it with a `FrameIndex = 1` +
  `FirstInstance != 0` case.
* Push constants: 4 MVP rows + viewport size (72 B, vertex stage).
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
  `usampler2D` (R16G16_UINT). Fragment stage only.

## Upload rules (implemented)

* `ImSlugVulkanUploadPools(cmd, pools, frameIndex)` creates/recreates both
  images to the pools' allocated size, records staging copies + barriers for
  the dirty rows into `cmd`, and clears the dirty rows **of exactly the pools
  it consumed** (per-pool clearing: a missing pool view must not drop the other
  pool's pending rows). The caller submits. The **first** upload after creation
  sends `[0, UsedRows)`; later frames send `[DirtyRowMin, DirtyRowMax]`.
* **One staging buffer per frame in flight** (B2): the CPU memcpys for frame N
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
* Release-safe early-outs (R3): `ImSlugVulkanDraw` is a no-op for an
  out-of-range `FrameIndex`, a slot-relative `FirstInstance` at/beyond the
  capacity, a null MVP, and when no pool upload happened yet (descriptor sets
  not written); `InstanceCount` is clamped to the remaining slot capacity.
  Debug builds still assert on all of those.

## Instance record (frozen, `imslug_internal.h`)

`ImSlugGlyphInstance`, 92 bytes, `static_assert`ed against the vertex input:
em bbox, screen rect (px, Y down), inverse Jacobian (screen -> em), band
scale/offset, glyphLoc (band pool), packed bandMax word with **E at bit 28**
(PLAN §11 D8(g); not "bit 12 of the raw word"), straight-alpha color. The
placement must be axis-aligned (scale+translate); the shader dilates along the
screen-space corner diagonals. `ImSlugBuildGlyphInstance()` fills one record;
`ImSlugPackBandMaxWord()` produces the packed word.

## Validation expectations

* Validation-clean at runtime (example + goldens run with
  `VK_LAYER_KHRONOS_validation`; any *error* fails the golden run). Quote the
  exact VUID from the layer output; never silence a validation error.
* Frequent failures and their real cause:
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

## Example app

`example/glfw_vulkan/main.cpp` (option `IMSLUG_BUILD_EXAMPLES=ON`): GLFW window
+ swapchain, validation layers on by default (`IMSLUG_VALIDATION=0` disables,
`IMSLUG_VALIDATION_VERBOSE=1` adds info messages), keyboard size/zoom control,
`--screenshot <file.ppm> <px_size> <scale>` readback mode. It prints the
validation message/error counts on exit. The frame loop cycles over
`ImSlugVulkanGetFramesInFlight()` (not a hardcoded 3) and null-checks the ring
slot pointer - a 2-image swapchain must not touch ring slot 2 (review B3).

## M3 callback protocol (planned; unchanged)

1. `ImDrawCmd::UserCallback` is dispatched **before** the backend sets scissor
   and texture state. The callback must set its **own dynamic scissor** from
   `pcmd->ClipRect`, converted with `DisplayPos`/`FramebufferScale`.
2. Immediately after every Slug callback command, inject a
   `GetPlatformIO().DrawCallback_ResetRenderState` command, so the stock backend
   re-establishes pipeline/descriptors/vertex buffers (R5 - never skip it, never
   move it).
3. Record draws directly into the active command buffer from
   `GetPlatformIO().Renderer_RenderState` (`ImGui_ImplVulkan_RenderState`:
   CommandBuffer / Pipeline / PipelineLayout), published for the duration of
   `ImGui_ImplVulkan_RenderDrawData`.
4. One draw = `ImSlugVulkanDraw` with the frame index, current render pass
   (`VK_NULL_HANDLE` under dynamic rendering), the batch's slot-relative
   `FirstInstance`/`InstanceCount`, the projection (screen px -> clip), the
   framebuffer size and the clip-rect scissor. The M3 pre-pass also calls
   `ImSlugVulkanUploadPools(cmd, pools, frameIndex)` once per frame before the
   draws (the frame index must be the current ring slot).
5. The pre-pass rewrites only `CmdBuffer` (scratch vector): geometry triangles
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
