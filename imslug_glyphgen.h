// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_glyphgen.h — the font-side core: stbtt outline extraction to em-space
// quadratics, adaptive band construction, ImGui-parity metrics, and the font
// handle with the lazy glyph-record cache M2 consumes.
//
// Everything here works in em space (1.0 = em, Y up, baseline origin) unless
// the name says otherwise ("...Em", "bakeSizePixels"). Metrics are the one
// deliberate exception: ImSlugGlyphMetrics pixels mirror Dear ImGui's stb
// loader so layout stays pixel-identical (PLAN §4.1) — see the comments in
// imslug_glyphgen.cpp for the exact source lines the formulas come from.

#pragma once

#include "imslug_internal.h"

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Outline extraction (stbtt -> em-space quadratics)
    // ---------------------------------------------------------------------
#if !defined(IMSLUG_NO_STBTT)
    // Contour order is preserved; lines become {p1,p3,p3}; both control-point
    // runs are converted; degenerate segments (all three points equal) are
    // dropped. Returns false and sets *quadCount to the required capacity when
    // the buffer is too small (never writes past quadCapacity).
    bool ImSlugExtractGlyphOutline(const stbtt_fontinfo* font, int glyphIndex, ImSlugQuad* quads, int quadCapacity,
                                   int* quadCount);
#endif

    // Cubic -> two quadratics: controls at 3/4 respectively 1/4, which is the
    // standard approximation (exact endpoints, C1 junction for a uniform
    // parameterization; max error ~ 1/64 of the segment scale). Returns 2, or 0
    // when the cubic is degenerate. Unit-tested; not wired to a CFF parser.
    int ImSlugConvertCubicToQuadratics(const ImSlugCubic* cubic, ImSlugQuad outQuads[2]);

    // ---------------------------------------------------------------------
    // Band construction: moved to imslug_packer.h in M7 (the pure quad/band
    // helpers must work without an outline parser). Declarations:
    //   ImSlugCalcBandCount, ImSlugQuadControlBounds, ImSlugControlBoundsOfQuads,
    //   ImSlugAssignBands.
    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Font
    // ---------------------------------------------------------------------
    struct ImSlugFont;       // opaque
    struct ImSlugColorFont;  // opaque (M5; imslug_colr.h)

    // Mirrors ImGui's stb loader with OversampleH/V=1, RasterizerDensity=1,
    // ExtraSizeScale=1, GlyphOffset=0 — the configuration Slug fonts use (no
    // atlas rasterization). Pixels are Y-down from the text base position.
    // MinX..MaxY are the em-space control-point bbox of the extracted outline
    // (the same values as the glyph record's, size-independent); the query
    // returns false for missing glyphs and for scratch allocation failure.
    struct ImSlugGlyphMetrics
    {
        float AdvanceX;                // pixels
        float X0, Y0, X1, Y1;          // pixels (ImFontGlyph layout fields)
        float AdvanceEm;               // advance_fu / unitsPerEm
        float MinX, MinY, MaxX, MaxY;  // control-point bbox, em, Y up
    };

    struct ImSlugFontMetrics
    {
        int UnitsPerEm;
        int HheaAscent, HheaDescent;  // font units
        float PixelsPerEm;            // pixels per em at the bake size
        float ScaleForPixelHeight;    // font units -> pixels at the bake size
        float Ascent, Descent;        // pixels (ImGui parity: ceil / floor)
    };

    // M7: raw per-glyph source metrics. The packed-asset writer stores these so
    // the loader can reproduce the TTF metrics (and the glyph boxes) at any
    // bake size without a parser: `Box*` is the glyf box exactly as
    // stbtt_GetGlyphBox reports it in font units (all zero when the glyph has
    // no glyf data, which the metrics path treats as invisible), `AdvanceFu`
    // the hmtx advance. Both font kinds implement the query (a packed font
    // reads its stored values back).
    struct ImSlugGlyphSourceMetrics
    {
        int AdvanceFu;
        int BoxX0, BoxY0, BoxX1, BoxY1;
    };

    // Copies the font bytes unless copyData is false (then the caller keeps the
    // buffer alive; mirrors ImGui's AddFontFromMemoryTTF contract).
    ImSlugFont* ImSlugCreateFontFromMemory(const void* fontData, int fontDataSize, int fontIndex = 0,
                                           bool copyData = true);
    void ImSlugDestroyFont(ImSlugFont* font);

    void ImSlugGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics);
    bool ImSlugGetGlyphMetrics(const ImSlugFont* font, unsigned int codepoint, float bakeSizePixels,
                               ImSlugGlyphMetrics* outMetrics);
    bool ImSlugGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                      ImSlugGlyphMetrics* outMetrics);

    // 0 = not found (stbtt_FindGlyphIndex semantics).
    int ImSlugGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint);

    // M5: the font's COLR/CPAL data (parsed lazily on first use; null when the
    // font has no usable color tables - the monochrome path). Owned by the
    // font, destroyed with it.
    ImSlugColorFont* ImSlugFontGetColor(ImSlugFont* font);

    // Number of glyphs in the font (stbtt_fontinfo::numGlyphs). M3 uses it to
    // size per-bake slot tables.
    int ImSlugGetGlyphCount(const ImSlugFont* font);

    // Copies the em-space outline quads of one glyph into a caller buffer
    // (double-call pattern: returns false and sets *outCount to the required
    // capacity when the buffer is too small). Same extraction as the record
    // path, so the bbox matches. Fails for glyphIndex <= 0.
    bool ImSlugGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads, int quadCapacity,
                                    int* outCount);

    // Raw source metrics of one glyph (M7; see ImSlugGlyphSourceMetrics).
    // Fails for glyphIndex <= 0.
    bool ImSlugGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex, ImSlugGlyphSourceMetrics* outMetrics);

    // M3: build a glyph record from em-space quads that do not come from the
    // font (the composite ellipsis is three translated dot outlines). The pools
    // receive the curves/bands exactly like a font glyph; a failed build is
    // transactional (the pools are rolled back) and the caller's record is left
    // untouched. `advanceEm` is the em-space advance (1.0 = em); `font` is any
    // font of the same source and only supplies the build scratch.
    ImSlugBuildStatus ImSlugBuildSyntheticGlyphRecord(ImSlugFont* font, ImSlugPools* pools, const ImSlugQuad* quads,
                                                      int quadCount, float advanceEm, ImSlugGlyphRecord* outRecord);

    // Lazily build (once per glyph index) and cache the glyph record, filling
    // the pools. The returned record points into the font's cache and stays
    // valid until the font is destroyed. Records embed pool locations: if the
    // same font is used with a different ImSlugPools, records are rebuilt.
    ImSlugBuildStatus ImSlugGetGlyphRecord(ImSlugFont* font, ImSlugPools* pools, unsigned int codepoint,
                                           const ImSlugGlyphRecord** outRecord);
    ImSlugBuildStatus ImSlugGetGlyphRecordByIndex(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord);
}  // namespace ImSlug
