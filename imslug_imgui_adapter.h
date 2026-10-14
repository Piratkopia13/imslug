// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_imgui_adapter.h — internal interface between the core (imslug.cpp)
// and the ImGui-facing adapter (imslug_imgui_adapter.cpp). Every use of
// imgui_internal.h lives in that one translation unit; the core sees
// only these declarations, which use public ImGui types or plain data.
//
// The adapter owns:
//   * the custom `const ImFontLoader` installed on Slug font sources,
//     including the stb_truetype metrics bake and the UV handle encoding;
//   * the synthetic ellipsis glyph (three translated dots) for fonts whose
//     cmap lacks U+2026/0x0085, so no bitmap is ever allocated for Slug fonts;
//   * UV decoding for the pre-pass (identifier + corner fractions).

#pragma once

#include "imslug.h"
#include "imslug_internal.h"

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Font registration
    // ---------------------------------------------------------------------
    // Prepares `cfg` for ImFontAtlas::AddFontFrom*: installs the Slug loader,
    // forces the metric-neutral settings the analytic bake depends on and
    // asserts on overrides that would break metric parity (see imslug.h).
    // `cfg` must be a live ImFontConfig the caller passes to the atlas; its
    // FontLoader/FontLoaderData are overwritten.
    void ImSlugAdapterPrepareFontConfig(ImFontConfig* cfg);

    // True when the ImFont was created through the adapter (any source carries
    // the Slug loader).
    bool ImSlugAdapterIsFontRegistered(const ImFont* font);

    // ---------------------------------------------------------------------
    // UV handle decode (pre-pass)
    // ---------------------------------------------------------------------
    // One decoded vertex: the glyph slab plus the corner position inside the
    // baked pixel box (0..1 per axis; Y grows downward like ImGui's V0/V1).
    struct ImSlugDecodedVertex
    {
        const ImSlugGlyphSlab* Slab = nullptr;
        float FracX = 0.0f;
        float FracY = 0.0f;
    };

    // Decodes a vertex UV pair. Returns false — treat the vertex as ordinary
    // geometry — when the values are not Slug handles: non-finite, outside the
    // magic ranges, an unknown/dead slot, an inconsistent slot between the two
    // axes, or a fraction outside the cell tolerance.
    bool ImSlugAdapterDecodeVertex(float u, float v, ImSlugDecodedVertex* out);

    // ---------------------------------------------------------------------
    // Lifetime
    // ---------------------------------------------------------------------
    // Frees adapter-owned state (synthetic records, per-source data). Called
    // from ImSlug::Shutdown; the ImGui atlas still owns the ImFonts.
    void ImSlugAdapterShutdown();
}  // namespace ImSlug
