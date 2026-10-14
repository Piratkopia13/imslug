// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_vulkan.h — standalone Vulkan rendering core for the Slug
// pipeline. ImGui-free by design: it takes explicit Vulkan objects and
// records draws into a caller-provided command buffer. The ImSlug_Impl*
// adapter at the bottom of this header bridges to the stock ImGui Vulkan
// backend; the init config maps 1:1 from ImGui_ImplVulkan_InitInfo.PipelineInfoMain.
//
// What the module owns:
//   * the two pool textures (curve RGBA16F + band R16G16_UINT), their views,
//     samplers and descriptor sets (one set per frame in flight, static after
//     upload);
//   * one staging buffer per frame in flight, used for dirty-row uploads;
//   * the host-visible persistently mapped instance ring
//     (frames_in_flight x MaxInstancesPerFrame; grows within its ceiling when
//     a frame overflows and no explicit capacity was set);
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

// Forward declarations only: the Vulkan core stays ImGui-free, so this header
// does not include imgui.h / imgui_impl_vulkan.h. Include those before this
// header to use the ImSlug_ImplVulkan_* adapter.
struct ImDrawData;
struct ImGui_ImplVulkan_InitInfo;

#ifndef IMSLUG_API
#define IMSLUG_API  // imslug.h defines the real one; this header stays standalone
#endif

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

        // Instance ring sizing; 0 selects the default (16384) and lets the
        // ring grow when a frame overflows (doubling, at least to the observed
        // demand, capped at 8x the default). A nonzero value is a hard cap:
        // the ring keeps that capacity and an overflowing frame drops its
        // overflowing runs with a one-time stderr warning.
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
    // Valid until the next ring growth (growth retires the old buffer once the
    // frames that recorded against it completed, so re-fetch after
    // ImSlugVulkanUploadPools/ImSlugVulkanReserveInstances; never cache the
    // pointer across frames); otherwise valid until Shutdown. Write up to
    // GetInstanceCapacity() records.
    ImSlugGlyphInstance* ImSlugVulkanGetInstanceData(uint32_t frameIndex);

    // Reserves `count` contiguous instance records in the current frame's ring
    // slot (the slot advances once per ImSlugVulkanUploadPools call). Returns
    // the base of the slot-relative range - write the records there and pass
    // the returned FirstInstance to ImSlugVulkanDrawInfo - or null when the
    // frame's capacity is exhausted; the caller must then skip that batch. The
    // ImGui adapter's ReserveInstances callback forwards here; harnesses drive
    // it directly.
    //
    // Overflow is never silent and self-heals when the capacity came from the
    // default (InitInfo::MaxInstancesPerFrame == 0): the first failure at a
    // given capacity prints a one-line stderr warning (demand vs capacity plus
    // the actionable knob) and the ring grows - in place when the frame has
    // reserved nothing yet, otherwise at the next ImSlugVulkanUploadPools - by
    // doubling (at least to the observed demand) up to 8x the default. An
    // explicit MaxInstancesPerFrame is a hard cap: the ring never grows past
    // it and every overflowing run drops.
    ImSlugGlyphInstance* ImSlugVulkanReserveInstances(uint32_t count, uint32_t* outFirstInstance);

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

        // Debug overlay flags (ImSlugVulkanDebugOverlay; 0 = off). The
        // value rides the push constants into the shaders; it never changes
        // geometry, placement or the fallback behavior.
        uint32_t DebugFlags = 0;
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
    // Debug overlay
    // ---------------------------------------------------------------------
    // Renders every Slug glyph with a diagnostic visualization on top of the
    // normal text:
    //   * GlyphBounds: a 1px outline of each glyph's *drawn* quad (the rect
    //     ImGui's text logic produced, after fine clipping) - the same input
    //     Config::DebugDrawBounds validates the reconstructed instance
    //     against, so a generator/placement bug shows up as an instance whose
    //     ink does not fill its outline;
    //   * BandGrid: a checkerboard of the glyph's H/V band grid (adjacent
    //     bands alternate tint), so band assignment and band boundaries are
    //     visible per glyph;
    //   * RefHeat: a heatmap of the band-reference walk cost of every pixel
    //     (blue = lowest, red = >= 16 references fetched), i.e. the fragment
    //     work the band tuning balances against memory.
    // Flags combine; RefHeat takes precedence over BandGrid when both are set.
    // The overlay is a rendering aid only: it never changes geometry,
    // placement, draw order or the atlas fallback.
    enum ImSlugVulkanDebugOverlay
    {
        ImSlugVulkanDebugOverlay_None = 0,
        ImSlugVulkanDebugOverlay_GlyphBounds = 1 << 0,
        ImSlugVulkanDebugOverlay_BandGrid = 1 << 1,
        ImSlugVulkanDebugOverlay_RefHeat = 1 << 2,
    };

    // ---------------------------------------------------------------------
    // ImGui adapter (implements the bridge to the stock ImGui Vulkan
    // backend). Usage, in this order:
    //   ImGui_ImplVulkan_Init(&init_info);      // stock backend, unchanged
    //   ImSlug::Init();                         // core
    //   ImSlug_ImplVulkan_Init(&init_info);     // this module
    //   ImSlug::AddFontFromFileTTF(...);        // Slug fonts
    // Per frame:
    //   ImSlug_ImplVulkan_UploadPools(cmd);     // before the render pass
    //   ImGui_ImplVulkan_RenderDrawData / ImSlug_ImplVulkan_RenderDrawData
    // ---------------------------------------------------------------------
    // Mirrors ImGui_ImplVulkan_InitInfo.PipelineInfoMain (render pass, subpass,
    // MSAA, dynamic rendering) onto ImSlugVulkanInitInfo, initializes the
    // Vulkan core and registers the callback render interface. Requires ImSlug::Init()
    // and a renderer with ImGuiBackendFlags_RendererHasTextures.
    IMSLUG_API bool ImSlug_ImplVulkan_Init(const ImGui_ImplVulkan_InitInfo* initInfo);

    // Same as ImSlug_ImplVulkan_Init, but takes the Vulkan objects directly
    // (ImSlugVulkanInitInfo) instead of an ImGui_ImplVulkan_InitInfo. This is
    // the C API's init path (imslug_c.h: imslug_impl_vulkan_init) and the hook
    // for integrations that never hold an ImGui_ImplVulkan_InitInfo (custom /
    // non-C++ renderers). Precedence for the instance ring: a nonzero
    // ImSlugVulkanInitInfo::MaxInstancesPerFrame wins over
    // Config::MaxInstancesPerFrame; 0 falls back to the config value (and 0
    // there selects the backend default, 16384). A zero MSAASamples selects
    // VK_SAMPLE_COUNT_1_BIT. Everything else behaves exactly like
    // ImSlug_ImplVulkan_Init (render IO registration, debug-overlay seeding
    // from Config::DebugDrawBounds, shutdown of a previous module).
    IMSLUG_API bool ImSlug_ImplVulkan_InitFromVulkanInfo(const ImSlugVulkanInitInfo* initInfo);

    // Unregisters the render interface and shuts the Vulkan core down (waits
    // for the device). ImSlug::Shutdown() is separate and owned by the app.
    IMSLUG_API void ImSlug_ImplVulkan_Shutdown();

    // Records this frame's glyph-pool uploads (dirty rows only) into `cmd`.
    // Call once per frame, outside a render pass and before the first
    // ImSlug_ImplVulkan_RenderDrawData call: the pool textures are static
    // afterwards, so draws need no transfer commands inside the render pass.
    // Advances the internal frames-in-flight slot.
    IMSLUG_API void ImSlug_ImplVulkan_UploadPools(VkCommandBuffer cmd);

    // Rewrites Slug text runs into callback draws (ImSlug::PreprocessDrawData)
    // and then renders the draw data through the stock backend. `cmd` must be
    // inside the render pass the InitInfo described (or inside a compatible
    // one); `mainPipeline` mirrors ImGui_ImplVulkan_RenderDrawData's argument.
    IMSLUG_API void ImSlug_ImplVulkan_RenderDrawData(ImDrawData* drawData, VkCommandBuffer cmd,
                                                     VkPipeline mainPipeline = VK_NULL_HANDLE);

    // Runtime toggle for the debug overlay (ImSlugVulkanDebugOverlay flags;
    // 0 = off). The initial value comes from Config::DebugDrawBounds (glyph
    // bounds), so an app that never calls this still gets the overlay the
    // config asked for. Takes effect on the next RenderDrawData; the overlay
    // is per-module (not per viewport) and does not survive Shutdown.
    IMSLUG_API void ImSlug_ImplVulkan_SetDebugOverlay(uint32_t overlayFlags);
    IMSLUG_API uint32_t ImSlug_ImplVulkan_GetDebugOverlay();

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
