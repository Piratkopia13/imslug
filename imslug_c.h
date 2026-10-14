// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_c.h — C API (M6): an extern "C" mirror of the public C++ API
// (imslug.h) plus the ImGui-facing Vulkan entry points
// (backends/imslug_impl_vulkan.h), following cimgui-style conventions.
//
// The C++ API is canonical: this header is a mirror and receives additions
// only. It is C11-clean (no namespaces, no default arguments, no bools in the
// ABI — see "ABI conventions" below) and compiles standalone; the Vulkan entry
// points are declared when Vulkan headers are visible (include <vulkan/
// vulkan.h> first, or define IMSLUG_C_VULKAN before this header to have it
// included here).
//
// Binding authors: docs/BINDINGS.md (repo root) has the conventions, the
// ownership/threading rules and the binding status (cimgui/Rust).
//
// ---------------------------------------------------------------------------
// Integration shape (identical to the C++ API; see imslug.h for the rationale)
// ---------------------------------------------------------------------------
//   igCreateContext(NULL);                       // live context first (cimgui)
//   ImGui_ImplVulkan_Init(&init_info);           // stock backend, unchanged
//   imslug_init(imslug_config_default());        // core: pools + registry
//   imslug_impl_vulkan_init(vulkan_info);        // imslug backend module
//   ImFont* font = imslug_add_font_from_file_ttf("Inter.ttf", 18.0f, NULL, NULL);
//   // ... per frame ...
//   igRender();
//   imslug_impl_vulkan_upload_pools(cmd);        // before the render pass
//   imslug_impl_vulkan_render_draw_data(igGetDrawData(), cmd, pipeline);
//
// `ImFont`, `ImFontConfig`, `ImDrawData`, `ImDrawList` and `ImDrawCmd` are
// forward-declared here and must come from the app's Dear ImGui C binding
// (cimgui for C apps, imgui-sys/bindgen for Rust). `NULL` font configs and
// glyph ranges select Dear ImGui's defaults.
//
// ---------------------------------------------------------------------------
// ABI conventions (binding authors: read this)
// ---------------------------------------------------------------------------
//   * Every function has C linkage and takes/returns only C types: fixed-size
//     POD structs, pointers to opaque handles, `int` (0/1) for booleans,
//     `uint32_t`/`float`/`double` nowhere. No C++ construct leaks.
//   * Handles (`ImFont*`, `ImDrawData*`, ...) are owned by Dear ImGui, not by
//     imslug; imslug never allocates or frees them.
//   * Structs are passed and returned by value; they contain no pointers with
//     allocation semantics. Fields are only appended in future versions, and
//     a struct layout change is an ABI break that bumps IMSLUG_VERSION (0.x
//     policy: additions land as new entry points first).
//   * `imslug_wchar` must match the width of the app's ImWchar (16-bit by
//     default, 32-bit with IMGUI_USE_WCHAR32 — required for non-BMP
//     codepoints, e.g. color emoji). Define IMGUI_USE_WCHAR32 (or
//     IMSLUG_WCHAR32) before including this header in that configuration.
//   * Threading: like Dear ImGui itself, imslug is single-threaded. Every
//     entry point must be called from the thread that owns the ImGui context;
//     there is no internal synchronization. The per-frame sequence
//     (init -> add fonts -> frame -> upload -> render -> shutdown) is the only
//     supported order.
//
// ---------------------------------------------------------------------------
// Ownership notes (mirror imslug.h)
// ---------------------------------------------------------------------------
//   * Fonts returned by imslug_add_font_from_* are ordinary ImFont objects
//     owned by the ImGui atlas (ImGui::GetIO().Fonts). Removing/destroying the
//     atlas (or ImGui::DestroyContext) releases them; imslug_shutdown() first
//     invalidates their Slug data.
//   * imslug_add_font_from_memory_ttf: ownership of `data` transfers to the
//     ImGui atlas by default (Dear ImGui frees it; set
//     ImFontConfig::FontDataOwnedByAtlas = false to keep ownership). imslug
//     reads the bytes before returning and keeps its own copy of the outline
//     data; the caller must keep the data alive until the atlas is built (the
//     same contract as stock Dear ImGui).
//   * imslug_add_font_from_file_ttf reads and copies the file (Dear ImGui and
//     imslug each keep their own copy for the atlas' lifetime).
//   * imslug_shutdown() is idempotent and invalidates Slug fonts without
//     touching the atlas.
//
// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------
// The numeric version mirrors imslug.h (IMSLUG_VERSION_*); the values are
// checked against the library at runtime by imslug_version_string()/
// imslug_version_num().

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// ---------------------------------------------------------------------------
// Version (kept in sync with imslug.h; verified by tests/test_c_api.cpp)
// ---------------------------------------------------------------------------
#ifndef IMSLUG_VERSION_MAJOR
#define IMSLUG_VERSION_MAJOR 0
#endif
#ifndef IMSLUG_VERSION_MINOR
#define IMSLUG_VERSION_MINOR 1
#endif
#ifndef IMSLUG_VERSION_PATCH
#define IMSLUG_VERSION_PATCH 0
#endif

// Numeric version for compile-time comparisons, e.g. 0.1.0 -> 100.
#ifndef IMSLUG_VERSION_NUM
#define IMSLUG_VERSION_NUM ((IMSLUG_VERSION_MAJOR * 10000) + (IMSLUG_VERSION_MINOR * 100) + (IMSLUG_VERSION_PATCH))
#endif

// Version string of the library ("major.minor.patch").
#ifndef IMSLUG_VERSION
#define IMSLUG_VERSION "0.1.0"
#endif

// Export/visibility decoration hook. Empty for the static library; a future
// shared-library or C API build may redefine it (same as imslug.h).
#ifndef IMSLUG_C_API
#define IMSLUG_C_API
#endif

    // ---------------------------------------------------------------------------
    // Opaque Dear ImGui handles (definitions come from the app's ImGui binding)
    // ---------------------------------------------------------------------------
    typedef struct ImFont ImFont;
    typedef struct ImFontConfig ImFontConfig;
    typedef struct ImDrawData ImDrawData;
    typedef struct ImDrawList ImDrawList;
    typedef struct ImDrawCmd ImDrawCmd;

// Matches ImWchar of the ImGui build: 16-bit by default, 32-bit with
// IMGUI_USE_WCHAR32 (or IMSLUG_WCHAR32) — see "ABI conventions" above.
#if defined(IMGUI_USE_WCHAR32) || defined(IMSLUG_WCHAR32)
    typedef uint32_t imslug_wchar;
#else
typedef uint16_t imslug_wchar;
#endif

    // ---------------------------------------------------------------------------
    // Configuration (mirror of ImSlug::Config; `int` booleans, 0 = false)
    // ---------------------------------------------------------------------------
    typedef struct imslug_config
    {
        // Master switch: when 0, fonts can still be registered but the pre-pass
        // leaves the draw data untouched (pure stock rendering).
        int EnableSlugForRegisteredFonts;

        // Debug aid: validate the reconstructed placement of every text glyph and
        // report (debug assert + Statistics.BoundsMismatches) when an instance
        // does not cover its source quad. On the Vulkan backend it also seeds the
        // glyph-bounds debug overlay.
        int DebugDrawBounds;

        // Fine-clip reconstruction (default on): rebuild a clipped glyph's
        // unclipped placement from the corner fractions ImGui's CPU fine clip left
        // in the UVs. When 0 — the R3 fallback — clipped glyphs are compressed
        // into the quad ImGui drew instead. Unclipped glyphs are placed
        // identically either way.
        int EnableFineClipReconstruction;

        // Instance ring sizing per frame; 0 selects the backend default (16384).
        // A frame that overflows drops the overflowing runs (Statistics.DroppedRuns).
        uint32_t MaxInstancesPerFrame;

        // Upper bound of live glyph slabs (one per baked visible glyph); 0 selects
        // the default (65536). Values above 65536 are clamped to the decoder's
        // slot cap; glyphs beyond the cap fall back to ImGui's fallback glyph.
        uint32_t MaxGlyphSlabs;

        // Render COLR color glyphs (one Slug instance per layer, gradients
        // evaluated in the fragment shader); 0 draws color glyphs through the
        // monochrome path. Read when a glyph is first baked, so it must be set at
        // imslug_init / before the atlas bakes the glyph.
        int EnableColorFonts;
    } imslug_config;

    // The default configuration (same values as ImSlug::Config's defaults).
    IMSLUG_C_API imslug_config imslug_config_default(void);

    // Creates the pools, the glyph registry and the per-frame arena. Requires a
    // live ImGui context (create it before this call) and, for rendering, the
    // stock renderer backend initialized first (it sets
    // ImGuiBackendFlags_RendererHasTextures, which imslug requires). Returns 1 on
    // success, 0 when it cannot work on this context.
    IMSLUG_C_API int imslug_init(imslug_config config);

    // Idempotent. Invalidates imslug fonts: their ImFont objects stay owned by the
    // ImGui atlas, but they stop being Slug-rendered.
    IMSLUG_C_API void imslug_shutdown(void);

    // Optional: resets the per-frame arena. The pre-pass calls it automatically
    // when it observes a new ImGui::GetFrameCount().
    IMSLUG_C_API void imslug_new_frame(void);

    // ---------------------------------------------------------------------------
    // Font registration (mirrors ImFontAtlas::AddFontFrom*)
    // ---------------------------------------------------------------------------
    // Adds a font whose glyphs are rendered by Slug. The returned ImFont is a
    // normal ImFont (layout metrics identical to the stock stb loader); its glyphs
    // are rect-less — the atlas never rasterizes or packs them, and their UVs are
    // imslug's glyph handles.
    //
    // M7: both functions accept TTF/OTF bytes and `.imslug` packed assets; the
    // kind is auto-detected by content (see imslug_is_packed_font_data and
    // docs/PACKED_FORMAT.md). The function names are kept for source
    // compatibility (additions-only policy); packed assets skip outline parsing
    // and work in builds without the TTF parser.
    //
    // `font_config` may be NULL (defaults); overrides that would change the baked
    // metrics (Oversample*, PixelSnapH, RasterizerDensity, GlyphOffset,
    // ExtraSizeScale, MergeMode, GlyphMinAdvanceX) are ignored with an assert.
    // `glyph_ranges` is an ImWchar array terminated by 0 (NULL = ImGui default);
    // the array must stay alive until the atlas is built. Returns NULL on failure.
    IMSLUG_C_API ImFont* imslug_add_font_from_file_ttf(const char* path, float size_pixels,
                                                       const ImFontConfig* font_config,
                                                       const imslug_wchar* glyph_ranges);
    IMSLUG_C_API ImFont* imslug_add_font_from_memory_ttf(const void* data, int data_size, float size_pixels,
                                                         const ImFontConfig* font_config,
                                                         const imslug_wchar* glyph_ranges);

    // 1 when `font` has at least one Slug source (i.e. was created by
    // imslug_add_font_from_*); 0 otherwise (including NULL).
    IMSLUG_C_API int imslug_is_registered(const ImFont* font);

    // ---------------------------------------------------------------------------
    // Packed assets (M7; additions only)
    // ---------------------------------------------------------------------------
    // imslug_add_font_from_{file,memory}_ttf accept both TTF/OTF bytes and
    // `.imslug` packed assets: the kind is auto-detected by content (the
    // container magic), not by file extension. A packed asset skips outline
    // parsing entirely and works in builds without the TTF parser
    // (IMSLUG_TTF_PARSER=OFF). Format: docs/PACKED_FORMAT.md; generate assets
    // with the imslug_gen CLI.
    //
    // 1 when `data` starts with the `.imslug` container magic; cheap content
    // sniff, no parsing (0 for NULL/too-short input).
    IMSLUG_C_API int imslug_is_packed_font_data(const void* data, int data_size);

    // Full structural validation of a `.imslug` v1 asset. Returns 1 when the
    // asset is valid; 0 otherwise, writing a human-readable reason (when
    // `error_message` is non-NULL) into `error_message` truncated to
    // `error_message_capacity` bytes. Never crashes on corrupt input.
    IMSLUG_C_API int imslug_validate_packed_font_data(const void* data, int data_size, char* error_message,
                                                      int error_message_capacity);

    // ---------------------------------------------------------------------------
    // Draw pipeline (backend-facing)
    // ---------------------------------------------------------------------------
    // Rewrites the draw data in place so text runs of registered fonts become
    // backend callback commands and everything else stays exactly as ImGui
    // produced it. Call it after ImGui::Render(), before the renderer backend
    // consumes the draw data. Idempotent per frame; a no-op when no backend is
    // registered.
    IMSLUG_C_API void imslug_preprocess_draw_data(ImDrawData* draw_data);

    // The stable ImDrawCallback registered on Slug runs. Exposed for completeness
    // (the pre-pass installs it itself); dispatches to the backend module.
    IMSLUG_C_API void imslug_draw_callback(const ImDrawList* parent_list, const ImDrawCmd* cmd);

    // ---------------------------------------------------------------------------
    // Diagnostics
    // ---------------------------------------------------------------------------
    typedef struct imslug_statistics
    {
        uint32_t GlyphSlabs;        // live slabs (baked visible glyphs)
        uint32_t LiveBatches;       // batches produced in the last pre-pass
        uint32_t Instances;         // glyph instances produced in the last pre-pass
        uint32_t DroppedRuns;       // text runs dropped (no instance capacity)
        uint32_t BoundsMismatches;  // debug placement checks that failed (DebugDrawBounds)
        uint32_t ColorLayers;       // of Instances, how many are color layers
    } imslug_statistics;

    // Copies the current statistics into `out` (a no-op when out is NULL).
    IMSLUG_C_API void imslug_get_statistics(imslug_statistics* out);

    // Returns the library version as a static string, identical to IMSLUG_VERSION
    // (and to ImSlug::GetVersion()).
    IMSLUG_C_API const char* imslug_version_string(void);

    // Returns IMSLUG_VERSION_NUM as compiled into the library.
    IMSLUG_C_API uint32_t imslug_version_num(void);

// ---------------------------------------------------------------------------
// Vulkan backend (mirror of backends/imslug_impl_vulkan.h)
// ---------------------------------------------------------------------------
// Declared when Vulkan headers are visible: include <vulkan/vulkan.h> before
// this header, or define IMSLUG_C_VULKAN to have the header include it.
#if !defined(VK_VERSION_1_0) && defined(IMSLUG_C_VULKAN)
#include <vulkan/vulkan.h>
#endif

#if defined(VK_VERSION_1_0)

    // Mirror of ImSlug::ImSlugVulkanInitInfo. Device objects are owned by the
    // caller (never created/destroyed by imslug). Zero-initialize and fill; a
    // zero MSAASamples selects VK_SAMPLE_COUNT_1_BIT and a zero
    // MaxInstancesPerFrame selects the default (16384; Config::MaxInstancesPerFrame
    // applies when nonzero).
    typedef struct imslug_vulkan_init_info
    {
        // Device objects, owned by the caller (never created/destroyed here).
        VkPhysicalDevice PhysicalDevice;
        VkDevice Device;
        VkQueue Queue;
        uint32_t QueueFamily;
        const VkAllocationCallbacks* Allocator;

        // Frames in flight = min(ImageCount, 3); one descriptor set and one
        // instance ring slot per frame.
        uint32_t ImageCount;

        VkSampleCountFlagBits MSAASamples;  // 0 selects VK_SAMPLE_COUNT_1_BIT
        VkPipelineCache PipelineCache;      // optional

        // Render-pass path (UseDynamicRendering == 0): the pipeline mirrors this
        // render pass/subpass. Additional render passes (multi-viewport) are
        // supported lazily, keyed per render pass.
        VkRenderPass RenderPass;
        uint32_t Subpass;

        // Dynamic-rendering path: PipelineRenderingCreateInfo must be filled
        // (sType + color/depth formats); it is copied by init. `int`, not bool.
        int UseDynamicRendering;
        VkPipelineRenderingCreateInfo PipelineRenderingCreateInfo;

        // Instance ring sizing; 0 selects the default (16384 instances/frame).
        uint32_t MaxInstancesPerFrame;
    } imslug_vulkan_init_info;

    // The default Vulkan init info (zeroed; MSAASamples = VK_SAMPLE_COUNT_1_BIT),
    // matching ImSlugVulkanInitInfo's C++ default construction.
    IMSLUG_C_API imslug_vulkan_init_info imslug_vulkan_init_info_default(void);

    // Initializes the standalone Vulkan core (pools, descriptor sets, instance
    // ring, pipelines created lazily) and registers the render interface the
    // pre-pass uses — the C entry point of ImSlug_ImplVulkan_InitFromVulkanInfo.
    // Requires imslug_init() first (for Config::DebugDrawBounds /
    // MaxInstancesPerFrame) and a live ImGui context. Returns 1 on success, 0 when
    // the device cannot be used (nothing is left behind).
    IMSLUG_C_API int imslug_impl_vulkan_init(imslug_vulkan_init_info init_info);

    // Unregisters the render interface and shuts the Vulkan core down (waits for
    // the device). imslug_shutdown() is separate and owned by the app.
    IMSLUG_C_API void imslug_impl_vulkan_shutdown(void);

    // Records this frame's glyph-pool uploads (dirty rows only) into `cmd`. Call
    // once per frame, outside a render pass and before the first
    // imslug_impl_vulkan_render_draw_data call; advances the frames-in-flight
    // slot.
    IMSLUG_C_API void imslug_impl_vulkan_upload_pools(VkCommandBuffer cmd);

    // Rewrites Slug text runs into callback draws (imslug_preprocess_draw_data)
    // and then renders the draw data through the stock ImGui Vulkan backend. `cmd`
    // must be inside the render pass the init info described (or a compatible
    // one); `main_pipeline` mirrors ImGui_ImplVulkan_RenderDrawData's argument
    // (VK_NULL_HANDLE is valid).
    IMSLUG_C_API void imslug_impl_vulkan_render_draw_data(ImDrawData* draw_data, VkCommandBuffer cmd,
                                                          VkPipeline main_pipeline);

// M4 debug overlay flags (mirror of ImSlugVulkanDebugOverlay).
#define IMSLUG_VULKAN_DEBUG_OVERLAY_NONE 0u
#define IMSLUG_VULKAN_DEBUG_OVERLAY_GLYPH_BOUNDS (1u << 0)
#define IMSLUG_VULKAN_DEBUG_OVERLAY_BAND_GRID (1u << 1)
#define IMSLUG_VULKAN_DEBUG_OVERLAY_REF_HEAT (1u << 2)

    // Runtime toggle for the debug overlay (combine the flags above; 0 = off).
    // The initial value comes from Config::DebugDrawBounds (glyph bounds); takes
    // effect on the next render; a call before init is ignored in release (debug
    // asserts).
    IMSLUG_C_API void imslug_impl_vulkan_set_debug_overlay(uint32_t overlay_flags);
    IMSLUG_C_API uint32_t imslug_impl_vulkan_get_debug_overlay(void);

#endif  // VK_VERSION_1_0

#ifdef __cplusplus
}  // extern "C"
#endif
