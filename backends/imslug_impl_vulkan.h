// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_vulkan.h — standalone Vulkan rendering core for the Slug
// pipeline (M2). ImGui-free by design: it takes explicit Vulkan objects and
// records draws into a caller-provided command buffer. The M3 ImGui backend
// (backends/imslug_imgui_*.*) is an adapter on top of this API; the init
// config maps 1:1 from ImGui_ImplVulkan_InitInfo.PipelineInfoMain.
//
// What the module owns:
//   * the two pool textures (curve RGBA16F + band R16G16_UINT), their views,
//     samplers and descriptor sets (one set per frame in flight, static after
//     upload);
//   * one staging buffer per frame in flight, used for dirty-row uploads;
//   * the host-visible persistently mapped instance ring
//     (frames_in_flight x MaxInstancesPerFrame);
//   * the Slug pipeline, created lazily per render pass/subpass (or once for
//     dynamic rendering) and cached.
//
// Shader/pipeline contract (see shaders/vulkan/slug.vert/.frag):
//   * 4-vertex triangle strip per glyph, instance data at attribute locations
//     0..6 (ImSlugGlyphInstance), VK_VERTEX_INPUT_RATE_INSTANCE;
//   * dynamic viewport + scissor; blend mirrors stock imgui_impl_vulkan
//     exactly (src alpha / one-minus-src-alpha color, one /
//     one-minus-src-alpha alpha);
//   * push constants: 4 MVP rows (screen pixels -> clip) + viewport size in
//     pixels (SlugDilate's `dim`).

#pragma once

#include "imslug_internal.h"

#include <vulkan/vulkan.h>

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Init
    // ---------------------------------------------------------------------
    struct ImSlugVulkanInitInfo
    {
        // Device objects, owned by the caller (never created/destroyed here).
        VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
        VkDevice Device = VK_NULL_HANDLE;
        VkQueue Queue = VK_NULL_HANDLE;
        uint32_t QueueFamily = 0;
        const VkAllocationCallbacks* Allocator = nullptr;

        // Frames in flight = min(ImageCount, 3); one descriptor set and one
        // instance ring slot per frame.
        uint32_t ImageCount = 0;

        VkSampleCountFlagBits MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineCache PipelineCache = VK_NULL_HANDLE;  // optional

        // Render-pass path (UseDynamicRendering == false): the pipeline mirrors
        // this render pass/subpass. Additional render passes (multi-viewport)
        // are supported lazily, keyed per render pass.
        VkRenderPass RenderPass = VK_NULL_HANDLE;
        uint32_t Subpass = 0;

        // Dynamic-rendering path: PipelineRenderingCreateInfo must be filled
        // (sType + color/depth formats); it is copied by Init.
        bool UseDynamicRendering = false;
        VkPipelineRenderingCreateInfo PipelineRenderingCreateInfo = {};

        // Instance ring sizing; 0 selects the default (16384 instances/frame).
        uint32_t MaxInstancesPerFrame = 0;
    };

    // Creates the descriptor set layout/pool, samplers, instance ring and
    // checks the pool texture formats. Returns false (leaving nothing behind)
    // when the device cannot sample R16G16B16A16_SFLOAT / R16G16_UINT.
    bool ImSlugVulkanInit(const ImSlugVulkanInitInfo* initInfo);

    // Waits for the device, then destroys everything owned by the module.
    void ImSlugVulkanShutdown();

    // ---------------------------------------------------------------------
    // Pool uploads
    // ---------------------------------------------------------------------
    // Creates/recreates both pool images to match the pools' allocated size
    // (growth => full re-upload; rare, and a device wait is used), records
    // staging copies + layout barriers for the dirty rows into `cmd`, then
    // clears the dirty rows of every pool it actually consumed. `cmd` must be
    // in the recording state; the caller submits it. Must be called before the
    // first draw.
    //
    // `frameIndex` selects the frame's staging buffer (and its pool-image
    // hazard ordering) and must match the index later passed to
    // ImSlugVulkanDraw / ImSlugVulkanGetInstanceData. Recording frame N again
    // is only allowed after the submission that previously used frame N has
    // completed (the standard frames-in-flight contract), which is also what
    // makes per-frame staging safe without a device wait.
    bool ImSlugVulkanUploadPools(VkCommandBuffer cmd, ImSlugPools* pools, uint32_t frameIndex);

    // ---------------------------------------------------------------------
    // Instance ring
    // ---------------------------------------------------------------------
    uint32_t ImSlugVulkanGetFramesInFlight();
    uint32_t ImSlugVulkanGetInstanceCapacity();

    // Base pointer of the ring slot for `frameIndex` (< frames in flight).
    // Valid until Shutdown; write up to GetInstanceCapacity() records.
    ImSlugGlyphInstance* ImSlugVulkanGetInstanceData(uint32_t frameIndex);

    // ---------------------------------------------------------------------
    // Draw
    // ---------------------------------------------------------------------
    struct ImSlugVulkanDrawInfo
    {
        uint32_t FrameIndex = 0;                   // < ImSlugVulkanGetFramesInFlight()
        VkRenderPass RenderPass = VK_NULL_HANDLE;  // VK_NULL_HANDLE => dynamic rendering
        uint32_t Subpass = 0;
        uint32_t FirstInstance = 0;  // slot-relative: indexes into GetInstanceData(FrameIndex)
        uint32_t InstanceCount = 0;

        // 16 floats: the four rows of the object-space (screen pixels) -> clip
        // MVP, same layout as the reference `slug_matrix` and ImGui's
        // projection matrix.
        const float* MVP = nullptr;
        float ViewportWidth = 0.0f;   // framebuffer pixels; must match the bound viewport
        float ViewportHeight = 0.0f;  // (SlugDilate's `dim`, in pixels)
        VkRect2D Scissor = {};
    };

    // Records one Slug draw call: binds the pipeline (created lazily for
    // RenderPass/Subpass), the frame's descriptor set and the instance ring,
    // pushes the MVP + viewport, sets the scissor, then
    // vkCmdDraw(4, InstanceCount, 0, FrameIndex * GetInstanceCapacity() +
    // FirstInstance). FirstInstance/InstanceCount are slot-relative (they
    // index the records written at GetInstanceData(FrameIndex)); the draw adds
    // the ring slot base itself. Out-of-range FrameIndex or slot-relative
    // FirstInstance, and draws before any pool upload, are no-ops (release-safe
    // early-outs); InstanceCount is clamped to the remaining slot capacity.
    // The caller owns the viewport dynamic state: bind a viewport matching
    // ViewportWidth/Height before drawing (the example sets it once per frame).
    void ImSlugVulkanDraw(VkCommandBuffer cmd, const ImSlugVulkanDrawInfo* drawInfo);

    // ---------------------------------------------------------------------
    // Helpers shared by the example and the golden harness
    // ---------------------------------------------------------------------
    // Standard screen-pixels -> clip ortho. Screen pixels are Y down (row 0 =
    // top of the framebuffer) and so is Vulkan's NDC (ndc.y = -1 is the top
    // row), so there is no Y negation here - unlike OpenGL. The reference Slug
    // demo's `slug_matrix` plays the same role; its HLSL follows D3D's
    // Y-flip-then-NDC-flip convention, hence the different row signs there.
    inline void ImSlugVulkanBuildOrthoMVP(float framebufferWidth, float framebufferHeight, float mvp[16])
    {
        const float sx = 2.0f / framebufferWidth;
        const float sy = 2.0f / framebufferHeight;
        mvp[0] = sx;
        mvp[1] = 0.0f;
        mvp[2] = 0.0f;
        mvp[3] = -1.0f;  // row 0
        mvp[4] = 0.0f;
        mvp[5] = sy;
        mvp[6] = 0.0f;
        mvp[7] = -1.0f;  // row 1
        mvp[8] = 0.0f;
        mvp[9] = 0.0f;
        mvp[10] = 0.0f;
        mvp[11] = 0.0f;  // row 2
        mvp[12] = 0.0f;
        mvp[13] = 0.0f;
        mvp[14] = 0.0f;
        mvp[15] = 1.0f;  // row 3
    }
}  // namespace ImSlug
