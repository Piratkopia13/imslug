// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_font.h — the font handle shared by the two font kinds:
//   * TTF fonts (imslug_glyphgen.cpp): stb_truetype outline extraction;
//   * packed fonts (imslug_packed.cpp): glyph data read from an `.imslug`
//     container — no outline parser is involved.
// The dispatch layer (imslug_font.cpp) routes every public font API to the
// kind-specific implementation and owns the shared glyph-record cache, the
// build scratch buffers and the synthetic-record builder.
//
// Implementation-only header: no imgui.h, no stb_truetype.h (the no-parser
// build, IMSLUG_NO_STBTT, never sees either), no STL.
//
// Ownership: `ImSlugFont` is created by ImSlugCreateFontFromMemory
// (imslug_glyphgen.h, TTF) or ImSlugCreateFontFromPackedMemory
// (imslug_packed.h, .imslug) and destroyed with ImSlugDestroyFont. Both kinds
// begin with the same base, so a cast to the base is valid in both directions.

#pragma once

#include "imslug_internal.h"

#include <math.h>

namespace ImSlug
{
    enum ImSlugFontKind
    {
        ImSlugFontKind_Ttf = 0,     // stb_truetype outline extraction
        ImSlugFontKind_Packed = 1,  // .imslug container
    };

    // One cached glyph-record build per (glyph index, target pools). Entries are
    // individually allocated so a record pointer stays stable while more glyphs
    // are built; "Ok implies a valid stable record pointer" is unconditional
    // (the entry is reserved before the build).
    struct ImSlugGlyphCacheEntry
    {
        int GlyphIndex;
        ImSlugBuildStatus Status;
        ImSlugPools* OwnerPools;
        ImSlugGlyphRecord Record;
    };

    // Common base of every font object (both kind-specific structs start with
    // this). Fields are documented where they are used.
    struct ImSlugFont
    {
        ImSlugFontKind Kind;
        int UnitsPerEm;

        // Build scratch (grow-only; fonts are single-threaded like ImGui).
        // Mutable so const queries (metrics) can reuse the extraction path and
        // therefore report exactly the record's geometry.
        mutable ImSlugQuad* ScratchQuads;
        mutable int ScratchQuadCapacity;
        int BandCounts[2 * kMaxBandCountPerAxis];
        int* ScratchRefs;
        int ScratchRefCapacity;
        uint16_t* ScratchCurveLocs;  // (x, y) pairs, one per curve
        int ScratchCurveLocCapacity;

        // Lazy glyph cache.
        ImSlugGlyphCacheEntry** Cache;
        int CacheCount;
        int CacheCapacity;
    };

    // ---------------------------------------------------------------------
    // ImGui metric parity helpers (v1.92.9b)
    // ---------------------------------------------------------------------
    // The exact ImGui macros the stb loader's metric formulas use; shared by
    // the TTF metrics path (imslug_glyphgen.cpp) and the packed metrics path
    // (imslug_packed.cpp) so the two can never drift:
    //   ImCeil(X)  = ceilf(X)                       (imgui.h)
    //   ImFloor(X) = exact floor for negatives too  (imgui_internal.h)
    //   IM_ROUND(_VAL) = (float)(int)((_VAL) + 0.5f) (imgui.h)
    static inline float ImSlugImCeil(float x)
    {
        return ceilf(x);
    }

    static inline float ImSlugImFloor(float x)
    {
        return (x >= 0.0f || (float)(int)x == x) ? (float)(int)x : (float)((int)x - 1);
    }

    static inline float ImSlugImRound(float x)
    {
        return (float)(int)(x + 0.5f);
    }

    // ---------------------------------------------------------------------
    // Shared layer (imslug_font.cpp)
    // ---------------------------------------------------------------------
    // Frees the common fields (cache entries + scratch buffers). Kind-specific
    // destroy functions call this after releasing their own data.
    void ImSlugFreeFontCommon(ImSlugFont* font);

    // Drops every cached glyph record (all target pool sets), e.g. before a
    // transient pool set is destroyed or when its address may be reused.
    // Records previously handed out for this font become dangling - call it
    // only when no caller holds a record (the contract assumes pool sets
    // are long-lived; the offline writer is the one transient-pools user).
    void ImSlugFontClearCache(ImSlugFont* font);

    // Shared record builder: assigns bands and writes the curves/bands into
    // `pools` using the font's scratch buffers. The pools are left untouched
    // only when this returns Ok; on failure the caller must roll back (use the
    // transactional variant below). `advanceEm` is the em-space advance.
    ImSlugBuildStatus ImSlugWriteGlyphRecordToPools(ImSlugFont* font, ImSlugPools* pools, const ImSlugQuad* quads,
                                                    int quadCount, float advanceEm, ImSlugGlyphRecord* record);

    // ImSlugWriteGlyphRecordToPools with the transactional contract: on any
    // failure the curve/band cursors and the dirty/used bookkeeping are rolled
    // back so a retry cannot consume pool space.
    ImSlugBuildStatus ImSlugBuildGlyphRecordToPoolsTransactional(ImSlugFont* font, ImSlugPools* pools,
                                                                 const ImSlugQuad* quads, int quadCount,
                                                                 float advanceEm, ImSlugGlyphRecord* record);

    // Cache lookup + build via the font's kind-specific record builder. On a
    // cache miss the entry is reserved before the build; successes and
    // definitive failures are cached, transient ones (pool limits, OOM) are
    // retried on the next call.
    ImSlugBuildStatus ImSlugGetOrBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord);

    // ---------------------------------------------------------------------
    // Kind-specific implementations
    // ---------------------------------------------------------------------
    // TTF (imslug_glyphgen.cpp; compiled only with the runtime parser, i.e.
    // !IMSLUG_NO_STBTT).
    void ImSlugTtfDestroy(ImSlugFont* font);
    void ImSlugTtfGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics);
    bool ImSlugTtfGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                         ImSlugGlyphMetrics* outMetrics);
    int ImSlugTtfGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint);
    bool ImSlugTtfGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads, int quadCapacity,
                                       int* outCount);
    bool ImSlugTtfGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex, ImSlugGlyphSourceMetrics* outMetrics);
    ImSlugBuildStatus ImSlugTtfBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                ImSlugGlyphRecord* record);
    ImSlugColorFont* ImSlugTtfGetColorFont(ImSlugFont* font);
    int ImSlugTtfGetGlyphCount(const ImSlugFont* font);

    // Packed (imslug_packed.cpp; always compiled, parser or not).
    void ImSlugPackedDestroy(ImSlugFont* font);
    void ImSlugPackedGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics);
    bool ImSlugPackedGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                            ImSlugGlyphMetrics* outMetrics);
    int ImSlugPackedGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint);
    bool ImSlugPackedGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads,
                                          int quadCapacity, int* outCount);
    bool ImSlugPackedGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex,
                                           ImSlugGlyphSourceMetrics* outMetrics);
    ImSlugBuildStatus ImSlugPackedBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                   ImSlugGlyphRecord* record);

    // Packed-font glyph count (stored entries; no parser needed).
    int ImSlugPackedGetGlyphCount(const ImSlugFont* font);
}  // namespace ImSlug
