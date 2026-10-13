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
    // Contour order is preserved; lines become {p1,p3,p3}; both control-point
    // runs are converted; degenerate segments (all three points equal) are
    // dropped. Returns false and sets *quadCount to the required capacity when
    // the buffer is too small (never writes past quadCapacity).
    bool ImSlugExtractGlyphOutline(const stbtt_fontinfo* font, int glyphIndex, ImSlugQuad* quads, int quadCapacity,
                                   int* quadCount);

    // Cubic -> two quadratics: controls at 3/4 respectively 1/4, which is the
    // standard approximation (exact endpoints, C1 junction for a uniform
    // parameterization; max error ~ 1/64 of the segment scale). Returns 2, or 0
    // when the cubic is degenerate. Unit-tested; not wired to a CFF parser.
    int ImSlugConvertCubicToQuadratics(const ImSlugCubic* cubic, ImSlugQuad outQuads[2]);

    // ---------------------------------------------------------------------
    // Band construction
    // ---------------------------------------------------------------------
    // Band count per axis: clamp(ceil(sqrt(2 * curveCount)), 4, 32) — PLAN §4.2.
    // The single tuning point for M4; every user of band counts calls it.
    int ImSlugCalcBandCount(int curveCount);

    ImSlugQuadBounds ImSlugQuadControlBounds(const ImSlugQuad& quad);

    // Assign curves to bands (pure, deterministic; used by the builder and by
    // unit tests).
    //   counts: bandMaxY+1 horizontal counts then bandMaxX+1 vertical counts
    //           (zeroed by this call);
    //   refs:   flattened curve indices in band order; per band sorted
    //           descending by max x (horizontal) / max y (vertical), stable;
    //   returns false and sets *requiredRefs when refCapacity is too small.
    // Membership rule: a curve belongs to a band when its control-point bbox,
    // inflated by kBandOverlapEpsilon on both ends, overlaps the band's em
    // range. Exactly axis-parallel curves are excluded from the matching-axis
    // bands (PLAN §2.2 rules 1-3).
    bool ImSlugAssignBands(const ImSlugQuad* quads, int quadCount, int bandMaxX, int bandMaxY, float minX, float minY,
                           float maxX, float maxY, int* counts, int* refs, int refCapacity, int* requiredRefs);

    // ---------------------------------------------------------------------
    // Font
    // ---------------------------------------------------------------------
    struct ImSlugFont;  // opaque

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

    // Lazily build (once per glyph index) and cache the glyph record, filling
    // the pools. The returned record points into the font's cache and stays
    // valid until the font is destroyed. Records embed pool locations: if the
    // same font is used with a different ImSlugPools, records are rebuilt.
    ImSlugBuildStatus ImSlugGetGlyphRecord(ImSlugFont* font, ImSlugPools* pools, unsigned int codepoint,
                                           const ImSlugGlyphRecord** outRecord);
    ImSlugBuildStatus ImSlugGetGlyphRecordByIndex(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord);
}  // namespace ImSlug
