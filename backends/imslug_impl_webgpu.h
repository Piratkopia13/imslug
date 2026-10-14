// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_impl_webgpu.h — standalone WebGPU rendering core for the Slug
// pipeline. ImGui-free by design: it takes explicit WebGPU objects and records
// draws into a caller-provided render pass encoder. The ImSlug_Impl* adapter at
// the bottom of this header bridges to the stock ImGui WebGPU backend; the init
// config maps 1:1 from ImGui_ImplWGPU_InitInfo.
//
// What the module owns:
//   * the two pool textures (curve RGBA16F + band RG16_UINT) and their views;
//   * the paint-record storage buffer (bind group entry 2);
//   * one CPU instance ring (frames_in_flight x MaxInstancesPerFrame records,
//     the memory ImSlugWGPUGetInstanceData returns) plus one GPU
//     VERTEX|COPY_DST buffer per frame in flight, written with a single
//     queue.writeBuffer per frame;
//   * the per-draw uniform ring — the WebGPU replacement for the Vulkan push
//     constants — one 80 B parameter record per Slug batch, bound as a dynamic
//     offset uniform binding (bind group entry 3);
//   * the Slug pipeline, the bind group layout and the one bind group.
//
// Shader/pipeline contract (see shaders/wgsl/slug.{vert,frag}.wgsl, the WGSL
// port of shaders/vulkan/slug.{vert,frag}):
//   * 4-vertex triangle strip per glyph, instance data at shader locations 0..7
//     (ImSlugGlyphInstance), vertex step mode Instance, arrayStride 96;
//   * blend mirrors stock imgui_impl_wgpu exactly (src alpha /
//     one-minus-src-alpha color, one / one-minus-src-alpha alpha);
//   * no depth or stencil state unless the init info's DepthStencilFormat is
//     set (then it mirrors imgui_impl_wgpu's disabled depth state);
//   * dynamic scissor only: the pipeline carries no dynamic viewport, and the
//     adapter never sets one (ImGui's render state owns the viewport);
//   * bind group 0 layout (frozen): 0 = texture_2d<f32> curve pool (RGBA16F),
//     1 = texture_2d<u32> band pool (RG16_UINT), 2 = var<storage, read> paint
//     records, 3 = var<uniform> per-draw parameters with a dynamic offset.
//     No samplers anywhere (everything is textureLoad).
//
// Differences from the Vulkan module (imslug_impl_vulkan.{h,cpp}), all forced
// by the API rather than chosen:
//   * WebGPU has no push constants: per-draw parameters ride a uniform ring
//     (80 B records at a minUniformBufferOffsetAlignment-aligned stride), one
//     record per Slug batch, ring size NumFramesInFlight x MaxBatchesPerFrame.
//   * WebGPU has no device wait: pool/ring growth destroys the old resource and
//     lets the implementation defer the in-flight reads itself.
//   * WebGPU instance data lives in a CPU ring that the caller uploads per
//     frame (ImSlugWGPUUploadInstances), because the GPU buffer cannot be
//     persistently mapped.
//   * Device error/lost callbacks can only be installed on the device
//     descriptor at device-creation time, and the device belongs to the caller:
//     the APP must install them (WGPUDeviceDescriptor::uncapturedErrorCallbackInfo
//     + deviceLostCallbackInfo) for device-lost visibility - the module cannot
//     attach callbacks to a device it does not create. For its own creation
//     work the module pushes a validation error scope and prints the first
//     failure once through stderr, repo style ("[imslug] ..."): Init brackets
//     everything it creates, and ImSlugWGPUUploadPools brackets the frame's
//     growth/recreation work (instance/uniform ring growth, pool textures,
//     paint buffer growth, bind group). Creation paths outside those scopes
//     (the in-place instance-ring growth in ImSlugWGPUReserveInstances) rely
//     on the device's own error handling. There are no exceptions and no
//     aborts.
//
// WebGPU API usage follows the pinned Dear ImGui WebGPU backend
// (backends/imgui_impl_wgpu.{h,cpp}): raw <webgpu/webgpu.h> (the Dawn flavour
// provided by --use-port=emdawnwebgpu under Emscripten), no C++ wrappers.

#pragma once

#include "imslug_internal.h"

#include <webgpu/webgpu.h>

// Forward declarations only: the WebGPU core stays ImGui-free, so this header
// does not include imgui.h / imgui_impl_wgpu.h. Include those before this
// header to use the ImSlug_ImplWGPU_* adapter.
struct ImDrawData;
struct ImGui_ImplWGPU_InitInfo;

#ifndef IMSLUG_API
#define IMSLUG_API  // imslug.h defines the real one; this header stays standalone
#endif

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Init
    // ---------------------------------------------------------------------
    struct ImSlugWGPUInitInfo
    {
        // Device objects, owned by the caller (never created/destroyed here).
        WGPUDevice Device = nullptr;
        WGPUQueue Queue = nullptr;

        // Render target + depth formats and the multisample state the Slug
        // pipeline is created with. They must match the stock backend's
        // pipeline exactly: Slug draws share ImGui's render pass, and a WebGPU
        // pipeline is only compatible with a pass whose attachment formats,
        // sample count and depth-stencil format match (a depth-stencil format
        // of Undefined means "no depth attachment").
        WGPUTextureFormat RenderTargetFormat = WGPUTextureFormat_Undefined;
        WGPUTextureFormat DepthStencilFormat = WGPUTextureFormat_Undefined;
        WGPUMultisampleState MultisampleState = {};  // count 0 => {1, ~0u, false}

        // Instance ring sizing; 0 selects the default (16384) and lets the
        // ring grow when a frame overflows (doubling, at least to the observed
        // demand, capped at 8x the default). A nonzero value is a hard cap:
        // the ring keeps that capacity and an overflowing frame drops its
        // overflowing runs with a one-time stderr warning.
        uint32_t MaxInstancesPerFrame = 0;

        // Frames in flight, clamped to 3 (the Vulkan module's min(ImageCount,
        // 3)). One instance-ring slot, one uniform-ring region and one in-flight
        // reuse contract per frame. 0 is invalid (Init fails), exactly like
        // ImSlugVulkanInitInfo::ImageCount == 0.
        uint32_t NumFramesInFlight = 0;
    };

    // Creates the bind group layout, pipeline layout, uniform ring, instance
    // ring and paint buffer and checks the formats and device limits. Returns
    // false (leaving nothing behind) when the device objects or the formats are
    // unusable (the render target must be a color format; a depth format, when
    // given, must be a depth-stencil format), when a required object cannot be
    // created, or when maxTextureDimension2D is below the pool width.
    bool ImSlugWGPUInit(const ImSlugWGPUInitInfo* initInfo);

    // Destroys everything owned by the module (no device wait exists in
    // WebGPU; in-flight work keeps its resources alive in the implementation).
    void ImSlugWGPUShutdown();

    // ---------------------------------------------------------------------
    // Pool uploads
    // ---------------------------------------------------------------------
    // Creates/recreates both pool textures to match the pools' allocated size
    // (growth => full re-upload) and uploads the dirty rows with
    // queue.writeTexture (one call per contiguous range), then clears the dirty
    // rows of every pool it actually consumed. Must be called once per frame,
    // before the first draw and before ImSlugWGPUUploadInstances.
    //
    // `frameIndex` selects the frame's instance-ring slot and uniform-ring
    // region and must match the index later passed to
    // ImSlugWGPUGetInstanceData / ImSlugWGPUUploadInstances / ImSlugWGPUDraw.
    // Recording frame N again is only allowed after the submission that
    // previously used frame N has completed (the standard frames-in-flight
    // contract), which is also what makes the per-slot buffers safe without a
    // device wait.
    bool ImSlugWGPUUploadPools(ImSlugPools* pools, uint32_t frameIndex);

    // ---------------------------------------------------------------------
    // Instance ring
    // ---------------------------------------------------------------------
    uint32_t ImSlugWGPUGetFramesInFlight();
    uint32_t ImSlugWGPUGetInstanceCapacity();

    // Base pointer of the CPU ring slot for `frameIndex` (< frames in flight).
    // Valid until the next ring growth (growth frees the old ring; re-fetch
    // after ImSlugWGPUUploadPools/ImSlugWGPUReserveInstances, never cache the
    // pointer across frames); otherwise valid until Shutdown. Write up to
    // GetInstanceCapacity() records.
    ImSlugGlyphInstance* ImSlugWGPUGetInstanceData(uint32_t frameIndex);

    // Reserves `count` contiguous instance records in the current frame's ring
    // slot (the slot advances once per ImSlugWGPUUploadPools call). Returns the
    // base of the slot-relative range - write the records there and pass the
    // returned FirstInstance to ImSlugWGPUDrawInfo - or null when the frame's
    // capacity is exhausted; the caller must then skip that batch. The ImGui
    // adapter's ReserveInstances callback forwards here; harnesses drive it
    // directly.
    //
    // Overflow is never silent and self-heals when the capacity came from the
    // default (InitInfo::MaxInstancesPerFrame == 0): the first failure at a
    // given capacity prints a one-line stderr warning (demand vs capacity plus
    // the actionable knob) and the ring grows - in place when the frame has
    // reserved nothing yet, otherwise at the next ImSlugWGPUUploadPools - by
    // doubling (at least to the observed demand) up to 8x the default. An
    // explicit MaxInstancesPerFrame is a hard cap: the ring never grows past it
    // and every overflowing run drops.
    ImSlugGlyphInstance* ImSlugWGPUReserveInstances(uint32_t count, uint32_t* outFirstInstance);

    // Copies the frame's staged instance records into that slot's GPU buffer
    // with one queue.writeBuffer. Call it once per frame after the records were
    // written (the adapter calls it right after ImSlug::PreprocessDrawData) and
    // before ImSlugWGPUDraw. `instanceCount` is clamped to the ring capacity;
    // draws beyond the uploaded range are no-ops (a frame that never uploaded
    // draws nothing rather than uninitialized records).
    bool ImSlugWGPUUploadInstances(uint32_t frameIndex, uint32_t instanceCount);

    // ---------------------------------------------------------------------
    // Draw
    // ---------------------------------------------------------------------
    struct ImSlugWGPUDrawInfo
    {
        uint32_t FrameIndex = 0;     // < ImSlugWGPUGetFramesInFlight()
        uint32_t FirstInstance = 0;  // slot-relative: indexes into GetInstanceData(FrameIndex)
        uint32_t InstanceCount = 0;

        // 16 floats: the four rows of the object-space (screen pixels) -> clip
        // MVP, same layout as the reference `slug_matrix` and ImGui's
        // projection matrix (see ImSlugWGPUBuildOrthoMVP for WebGPU's
        // Y-up clip space).
        const float* MVP = nullptr;
        float ViewportWidth = 0.0f;   // framebuffer pixels; must match the bound viewport
        float ViewportHeight = 0.0f;  // (SlugDilate's `dim`, in pixels)
        uint32_t ScissorX = 0;
        uint32_t ScissorY = 0;
        uint32_t ScissorW = 0;
        uint32_t ScissorH = 0;

        // Debug overlay flags (ImSlugWGPUDebugOverlay; 0 = off). The value rides
        // the uniform record into the shaders; it never changes geometry,
        // placement or the fallback behavior.
        uint32_t DebugFlags = 0;
    };

    // Records one Slug draw call into `pass`: binds the Slug pipeline, the bind
    // group (with the batch's dynamic uniform offset), the slot's instance
    // buffer and the scissor, then wgpuRenderPassEncoderDraw(pass, 4,
    // InstanceCount). The batch's uniform record is written with one
    // queue.writeBuffer before the draw. FirstInstance/InstanceCount are
    // slot-relative (they index the records written at GetInstanceData(
    // FrameIndex)); out-of-range FrameIndex, a FirstInstance beyond the
    // uploaded range, and draws before any pool upload are no-ops
    // (release-safe early-outs); InstanceCount is clamped to the remaining slot
    // capacity. The caller owns the viewport state: bind a viewport matching
    // ViewportWidth/Height before drawing (the ImGui backend sets it once per
    // frame from ImDrawData).
    void ImSlugWGPUDraw(WGPURenderPassEncoder pass, const ImSlugWGPUDrawInfo* drawInfo);

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
    enum ImSlugWGPUDebugOverlay
    {
        ImSlugWGPUDebugOverlay_None = 0,
        ImSlugWGPUDebugOverlay_GlyphBounds = 1 << 0,
        ImSlugWGPUDebugOverlay_BandGrid = 1 << 1,
        ImSlugWGPUDebugOverlay_RefHeat = 1 << 2,
    };

    // ---------------------------------------------------------------------
    // ImGui adapter (implements the bridge to the stock ImGui WebGPU
    // backend). Usage, in this order:
    //   ImGui_ImplWGPU_Init(&init_info);        // stock backend, unchanged
    //   ImSlug::Init();                         // core
    //   ImSlug_ImplWGPU_Init(&init_info);       // this module
    //   ImSlug::AddFontFromFileTTF(...);        // Slug fonts
    // Per frame:
    //   ImSlug_ImplWGPU_UploadPools();          // before the render pass
    //   ImSlug_ImplWGPU_RenderDrawData(draw_data, pass);
    // ---------------------------------------------------------------------
    // Mirrors ImGui_ImplWGPU_InitInfo (device, render target/depth formats,
    // multisample state, frames in flight) onto ImSlugWGPUInitInfo, acquires
    // the device's default queue for the module's lifetime, initializes the
    // WebGPU core and registers the callback render interface. Requires
    // ImSlug::Init() and a renderer with ImGuiBackendFlags_RendererHasTextures.
    IMSLUG_API bool ImSlug_ImplWGPU_Init(const ImGui_ImplWGPU_InitInfo* initInfo);

    // Unregisters the render interface and shuts the WebGPU core down
    // (releasing the queue reference acquired by Init). ImSlug::Shutdown() is
    // separate and owned by the app.
    IMSLUG_API void ImSlug_ImplWGPU_Shutdown();

    // Uploads this frame's glyph-pool rows (dirty rows only) and advances the
    // internal frames-in-flight slot. Call once per frame before the render
    // pass and before ImSlug_ImplWGPU_RenderDrawData: the pool textures are
    // static afterwards, so draws need no queue writes of their own.
    IMSLUG_API void ImSlug_ImplWGPU_UploadPools();

    // Rewrites Slug text runs into callback draws (ImSlug::PreprocessDrawData),
    // uploads the frame's staged instances with one queue.writeBuffer, then
    // renders the draw data through the stock backend. `pass` must be the
    // render pass encoder the InitInfo described. The batch callback reads
    // ImGui_ImplWGPU_RenderState from PlatformIO().Renderer_RenderState and the
    // core injects a DrawCallback_ResetRenderState command right after every
    // Slug run, which re-establishes ImGui's pipeline
    // (ImGui_ImplWGPU_SetupRenderState).
    IMSLUG_API void ImSlug_ImplWGPU_RenderDrawData(ImDrawData* drawData, WGPURenderPassEncoder pass);

    // Runtime toggle for the debug overlay (ImSlugWGPUDebugOverlay flags;
    // 0 = off). The initial value comes from Config::DebugDrawBounds (glyph
    // bounds), so an app that never calls this still gets the overlay the
    // config asked for. Takes effect on the next RenderDrawData; the overlay
    // is per-module (not per viewport) and does not survive Shutdown.
    IMSLUG_API void ImSlug_ImplWGPU_SetDebugOverlay(uint32_t overlayFlags);
    IMSLUG_API uint32_t ImSlug_ImplWGPU_GetDebugOverlay();

    // ---------------------------------------------------------------------
    // Helpers shared by the example and a future golden harness
    // ---------------------------------------------------------------------
    // Standard screen-pixels -> clip ortho. Screen pixels are Y down (row 0 =
    // top of the framebuffer); WebGPU's clip space is Y up (ndc.y = +1 is the
    // top row, the same convention as OpenGL/D3D and imgui_impl_wgpu), so -
    // unlike the Vulkan helper - row 1 negates Y. The reference Slug demo's
    // `slug_matrix` plays the same role; its HLSL follows D3D's
    // Y-flip-then-NDC-flip convention, hence the different row signs there.
    inline void ImSlugWGPUBuildOrthoMVP(float framebufferWidth, float framebufferHeight, float mvp[16])
    {
        const float sx = 2.0f / framebufferWidth;
        const float sy = -2.0f / framebufferHeight;
        mvp[0] = sx;
        mvp[1] = 0.0f;
        mvp[2] = 0.0f;
        mvp[3] = -1.0f;  // row 0
        mvp[4] = 0.0f;
        mvp[5] = sy;
        mvp[6] = 0.0f;
        mvp[7] = 1.0f;  // row 1 (WebGPU clip space is Y up)
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
