// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// Public C++ header. Constraints (AGENTS.md invariant 4, PLAN §6):
//   * must stay C++11-compatible — no C++14/17 constructs here;
//   * must not use the C++ standard library (no <string>, no <vector>, ...);
//   * implementation is C++17 and uses no exceptions.
//
// Integration shape (PLAN §3, M3): initialize the stock backend first, then
// ImSlug, then register fonts with ImSlug::AddFont*. Ordinary ImGui code —
// widgets, the demo window, third-party libraries — keeps working unchanged:
// registered fonts render through Slug, everything else through the atlas.
//
//   ImGui::CreateContext();                   // live context first
//   ImGui_ImplVulkan_Init(&init_info);        // stock backend, unchanged
//   ImSlug::Init();                           // core: pools + glyph registry
//   ImSlug_ImplVulkan_Init(&init_info);       // imslug backend module
//   ImFont* font = ImSlug::AddFontFromFileTTF("Inter.ttf", 18.0f);
//   // ... per frame ...
//   ImGui::Render();
//   ImSlug_ImplVulkan_UploadPools(cmd);       // before the render pass
//   ImSlug_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd, pipeline);
//
// The C API mirror arrives in M6; glyph generation landed in M1, the standalone
// Vulkan core in M2 (see .agent-docs/milestones/).

#pragma once

#include "imgui.h"

#include <stdint.h>  // uint32_t (imslug's public structs spell integer sizes out)

// The loader hook (ImFontConfig::FontLoader), the rect-less glyph contract and
// the platform_io reset callback this module relies on all landed in 1.92.8.
#if !defined(IMGUI_VERSION_NUM)
#error "imslug.h requires Dear ImGui (include imgui.h before, or through, imslug.h)"
#elif IMGUI_VERSION_NUM < 19208
#error                                                                                                                 \
    "imslug requires Dear ImGui 1.92.8 or newer (ImFontLoader + PlatformIO::DrawCallback_ResetRenderState). v1.92.9b is the pinned/tested version."
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define IMSLUG_VERSION_MAJOR 0
#define IMSLUG_VERSION_MINOR 1
#define IMSLUG_VERSION_PATCH 0

// Numeric version for compile-time comparisons, e.g. 0.1.0 -> 100.
#define IMSLUG_VERSION_NUM ((IMSLUG_VERSION_MAJOR * 10000) + (IMSLUG_VERSION_MINOR * 100) + (IMSLUG_VERSION_PATCH))

// Version string of the library ("major.minor.patch", plus a suffix while pre-0.1).
#define IMSLUG_VERSION "0.1.0-dev"

// Export/visibility decoration hook. Empty for the static library; a future
// shared-library or C API build may redefine it (M6/M8).
#ifndef IMSLUG_API
#define IMSLUG_API
#endif

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Module lifetime
    // ---------------------------------------------------------------------
    struct Config
    {
        // Master switch: when false, fonts can still be registered but the
        // pre-pass leaves the draw data untouched (pure stock rendering).
        bool EnableSlugForRegisteredFonts = true;

        // Debug aid: validate the reconstructed placement of every text glyph
        // and report (IMSLUG_ASSERT + Statistics::BoundsMismatches) when an
        // instance does not cover its source quad. Cheap; off by default. On
        // the Vulkan backend it also seeds the glyph-bounds debug overlay
        // (M4; toggle at runtime with ImSlug_ImplVulkan_SetDebugOverlay).
        bool DebugDrawBounds = false;

        // Fine-clip reconstruction (default on): rebuild a glyph's unclipped
        // placement from the corner fractions that ImGui's CPU fine clip left
        // in the UVs, so a glyph half outside a clip edge renders at its true
        // position (the scissor cuts it). When disabled — the R3 fallback —
        // clipped glyphs are placed by compressing the glyph's em box into the
        // quad ImGui actually drew, so all ink stays inside the fine-clip
        // rectangle at the cost of a visibly squeezed glyph near the edge;
        // unclipped glyphs are placed identically either way. Useful as a
        // safety valve and as the fuzz target of `imslug_fuzz`.
        bool EnableFineClipReconstruction = true;

        // Instance ring sizing per frame; 0 selects the backend default
        // (16384). The pre-pass needs one instance per glyph; a frame that
        // overflows drops the overflowing runs (Statistics::DroppedRuns).
        uint32_t MaxInstancesPerFrame = 0;

        // Upper bound of live glyph slabs (one per baked visible glyph; a
        // bake of the same glyph at another size owns its own slab); 0 selects
        // the default (65536). Values above 65536 are clamped to the decoder's
        // slot cap (slot ids >= 65536 would decode as non-Slug geometry).
        // Glyphs beyond the cap fall back to ImGui's fallback glyph (M3 scope:
        // the cap is far above typical UI usage).
        uint32_t MaxGlyphSlabs = 0;

        // M5: render COLR color glyphs (one Slug instance per layer, gradients
        // evaluated in the fragment shader). When false, color glyphs draw
        // through the monochrome path (their glyf outline, often empty for
        // emoji fonts). Read when a glyph is first baked, so it must be set at
        // ImSlug::Init / before the atlas bakes the glyph.
        bool EnableColorFonts = true;
    };

    // Creates the pools, the glyph registry and the per-frame arena. Requires a
    // live ImGui context (create it before this call); fonts are registered
    // afterwards with AddFontFrom* above, and the stock renderer backend must
    // already be initialized (it sets ImGuiBackendFlags_RendererHasTextures,
    // which imslug requires). Returns false when it cannot work on this context.
    IMSLUG_API bool Init(const Config& config = Config());

    // Idempotent. Invalidates ImSlug fonts: their ImFont objects stay owned by
    // the ImGui atlas, but they stop being Slug-rendered.
    IMSLUG_API void Shutdown();

    // Optional: resets the per-frame arena. The pre-pass calls it automatically
    // when it observes a new ImGui::GetFrameCount().
    IMSLUG_API void NewFrame();

    // ---------------------------------------------------------------------
    // Font registration (mirrors ImFontAtlas::AddFontFrom*)
    // ---------------------------------------------------------------------
    // Adds a font whose glyphs are rendered by Slug. The returned ImFont is a
    // normal ImFont (layout metrics identical to the stock stb loader); its
    // glyphs are rect-less — the atlas never rasterizes or packs them, and
    // their UVs are imslug's glyph handles.
    //
    // M7: the font kind is auto-detected by content. Bytes starting with the
    // `.imslug` magic (see docs/PACKED_FORMAT.md) are a pre-baked packed asset
    // and load without any outline parsing — the path for deployments built
    // without the runtime TTF parser (IMSLUG_TTF_PARSER=OFF) and for faster
    // startup. Everything else is parsed as TTF/OTF by stb_truetype (when the
    // parser is built in). The `sizePixels` argument is the bake size either
    // way: packed metrics are computed from the stored font metrics, so any
    // size works and layout matches the source font.
    //
    // Ownership: the font bytes are read/copied by Dear ImGui (atlas owned) and
    // independently copied by imslug for outline extraction; both copies live
    // for the atlas' lifetime and are released with the atlas / Shutdown.
    //
    // ImFontConfig overrides that would change the baked metrics (Oversample*,
    // PixelSnapH, RasterizerDensity, GlyphOffset, ExtraSizeScale, MergeMode,
    // GlyphMinAdvanceX) are ignored with an assert: Slug glyphs are rasterized
    // analytically, so those knobs have no meaning. Everything else (SizePixels,
    // GlyphExtraAdvanceX, GlyphRanges, DstFont, ...) is honored like stock.
    IMSLUG_API ImFont* AddFontFromFileTTF(const char* path, float sizePixels, const ImFontConfig* fontConfig = nullptr,
                                          const ImWchar* glyphRanges = nullptr);
    IMSLUG_API ImFont* AddFontFromMemoryTTF(const void* data, int dataSize, float sizePixels,
                                            const ImFontConfig* fontConfig = nullptr,
                                            const ImWchar* glyphRanges = nullptr);

    // True when `data` starts with the `.imslug` container magic (a packed
    // asset accepted by AddFontFromFileTTF/AddFontFromMemoryTTF). Cheap
    // content sniff, no parsing; useful for choosing a stock fallback font.
    IMSLUG_API bool IsPackedFontData(const void* data, int dataSize);

    // True when `font` has at least one Slug source (i.e. was created by
    // AddFontFrom* above).
    IMSLUG_API bool IsRegistered(const ImFont* font);

    // ---------------------------------------------------------------------
    // Draw pipeline (backend-facing)
    // ---------------------------------------------------------------------
    // Rewrites the draw data in place so text runs of registered fonts become
    // backend callback commands and everything else stays exactly as ImGui
    // produced it. Call it after ImGui::Render(), before the renderer backend
    // consumes the draw data. Idempotent per frame (the arena resets on the
    // next frame count). Safe to call when no backend is registered: nothing
    // is rewritten.
    IMSLUG_API void PreprocessDrawData(ImDrawData* drawData);

    // The stable ImDrawCallback registered on Slug runs. Dispatches to the
    // backend module registered with ImSlugSetRenderIO (internal).
    IMSLUG_API void DrawCallback(const ImDrawList* parentList, const ImDrawCmd* cmd);

    // ---------------------------------------------------------------------
    // Diagnostics (tests, M4 fuzzing/perf)
    // ---------------------------------------------------------------------
    struct Statistics
    {
        uint32_t GlyphSlabs;        // live slabs (baked visible glyphs)
        uint32_t LiveBatches;       // batches produced in the last pre-pass
        uint32_t Instances;         // glyph instances produced in the last pre-pass
        uint32_t DroppedRuns;       // text runs dropped (no instance capacity)
        uint32_t BoundsMismatches;  // debug placement checks that failed (DebugDrawBounds)
        uint32_t ColorLayers;       // M5: of Instances, how many are color layers
    };

    IMSLUG_API void GetStatistics(Statistics* out);

    // Returns the library version as a static string, identical to IMSLUG_VERSION.
    IMSLUG_API const char* GetVersion();
}  // namespace ImSlug
