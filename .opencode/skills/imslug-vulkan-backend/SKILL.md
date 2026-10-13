---
name: imslug vulkan backend
description: Vulkan backend contract for imslug - pipeline/descriptor/callback rules, the dynamic-scissor + reset-render-state protocol, upload rules, and validation expectations including the usual VUIDs. Load when working in backends/ or on draw-data callbacks (M3+).
---

# imslug Vulkan backend contract

Distilled from `.agent-docs/PLAN.md` §3.4 and §5 (authoritative). Vulkan code
lives in `backends/imslug_impl_vulkan.*` only; the core stays backend-independent.

## Entry points

```cpp
// backends/imslug_impl_vulkan.h
bool ImSlug_ImplVulkan_Init(const ImGui_ImplVulkan_InitInfo* init_info);   // after ImGui_ImplVulkan_Init
void ImSlug_ImplVulkan_Shutdown();
void ImSlug_ImplVulkan_RenderDrawData(ImDrawData*, VkCommandBuffer,
                                      VkPipeline main_pipeline = VK_NULL_HANDLE);
```

## Pipeline rules

* Mirror `init_info->PipelineInfoMain` exactly (RenderPass/Subpass/MSAA/
  `PipelineRenderingCreateInfo`) so our pipeline is compatible with the app's
  render pass. Support `UseDynamicRendering` and multi-viewport by creating
  pipelines lazily, keyed on the render pass / rendering-create-info.
* Blend state must match the stock ImGui Vulkan pipeline exactly.
* Data textures: curve pool `VK_FORMAT_R16G16B16A16_SFLOAT` sampled with a
  sampler (nearest), band pool `VK_FORMAT_R16G16_UINT` sampled as `usampler2D`.
  No `shaderInt64` or exotic features.
* Push constants: projection rows, display pos/scale, texture width log2 (12).

## Descriptor rules

* 1.92.8+ convention: separate `VkImageView` + `VkSampler`; our own descriptor
  pool with `FREE_DESCRIPTOR_SET_BIT`; one set per frame-in-flight; the data
  textures are static after upload, so no per-frame descriptor writes.

## Callback protocol (wrong state here corrupts the whole frame)

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
4. One draw = `vkCmdBindPipeline` -> bind our descriptor set -> bind instance
   vertex buffer -> `vkCmdSetScissor` -> `vkCmdDraw(4, instanceCount, 0,
   firstInstance)` (the quad comes from `gl_VertexIndex` in the VS).
5. The pre-pass rewrites only `CmdBuffer` (scratch vector): geometry triangles
   keep exact order/offsets, each maximal text run becomes a callback command
   with `ElemCount = 0` and `ClipRect`/`TexRef` copied from the original; the
   vertex/index buffers are never mutated.

## Upload rules

* Staging buffer + `vkCmdCopyBufferToImage` + barrier to
  `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`; upload only dirty rows per frame;
  full re-upload on growth (rare, needs a barrier per image).
* Instance buffer: host-visible persistently mapped ring
  (`frames_in_flight x max_instances_per_frame`), written once per frame in the
  pre-pass, bound with `VK_WHOLE_SIZE` and drawn with per-batch `firstInstance`
  offsets (no vertex binding offsets).

## Validation expectations

* Validation-clean at runtime and in CI (lavapipe for golden images). Quote the
  exact VUID from the layer output; never silence a validation error.
* Frequent failures and their real cause:
  * "pipeline ... is not compatible with the current render pass/subpass" -
    `PipelineInfoMain` not mirrored (render pass, subpass, MSAA, dynamic
    rendering).
  * "descriptor set ... is not compatible with the pipeline layout" - set
    created from a different layout than the bound one.
  * "image layout ... is not VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" - the
    post-upload barrier is missing or mis-scoped.
  * "vkCmdDraw ... vertex buffer ... is not bound / too small" - the instance
    ring size or `firstInstance` offset is wrong.
  * "scissor ... is outside the render area" - `ClipRect` conversion wrong;
    use the draw data's framebuffer size, not the OS window size.
  * "Reset/mismatched state" after a Slug run - the injected
    `DrawCallback_ResetRenderState` command is missing or misplaced.
* Keep the same-state contract documented in `docs/` when the backend lands
  (M3) and re-check it whenever ImGui's backend changes.

## Fallbacks

* Texture-pool overflow, unsupported formats, loader failure, or a renderer
  without `RendererHasTextures`: render through the stock ImGui path (or assert
  with a clear message for truly unsupported setups). Never leave a frame
  half-drawn, and never let the Slug path become mandatory for normal text.
