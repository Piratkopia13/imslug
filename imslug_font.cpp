// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_font.cpp — the font-handle dispatch layer shared by the TTF kind
// (imslug_glyphgen.cpp, stb_truetype) and the packed kind (imslug_packed.cpp,
// `.imslug` container v1). It owns:
//
//   * the shared glyph-record cache (one entry per glyph index and target
//     pool set) and the "Ok implies a valid stable record pointer" contract;
//   * the shared band/curve builder used by the TTF record path, the packed
//     loader and the synthetic (ellipsis) path, including the transactional
//     rollback contract;
//   * the public font API entry points, dispatched by font kind.
//
// This translation unit never includes stb_truetype.h (or imgui.h): it is part
// of every build, including the no-parser build (IMSLUG_NO_STBTT).

#include "imslug_font.h"

#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        bool EnsureArray(void** data, int* capacity, int required, size_t elementSize)
        {
            if (required <= *capacity)
                return true;
            int newCapacity = (*capacity > 0) ? *capacity : 64;
            while (newCapacity < required)
                newCapacity *= 2;
            void* newData = realloc(*data, (size_t)newCapacity * elementSize);
            if (newData == nullptr)
                return false;
            *data = newData;
            *capacity = newCapacity;
            return true;
        }
    }  // namespace

    // ---------------------------------------------------------------------
    // Common lifetime
    // ---------------------------------------------------------------------
    void ImSlugFreeFontCommon(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        ImSlugFontClearCache(font);
        free(font->ScratchQuads);
        free(font->ScratchRefs);
        free(font->ScratchCurveLocs);
        font->ScratchQuads = nullptr;
        font->ScratchQuadCapacity = 0;
        font->ScratchRefs = nullptr;
        font->ScratchRefCapacity = 0;
        font->ScratchCurveLocs = nullptr;
        font->ScratchCurveLocCapacity = 0;
    }

    void ImSlugFontClearCache(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        for (int i = 0; i < font->CacheCount; ++i)
            free(font->Cache[i]);
        free(font->Cache);
        font->Cache = nullptr;
        font->CacheCount = 0;
        font->CacheCapacity = 0;
    }

    // ---------------------------------------------------------------------
    // Shared record builder
    // ---------------------------------------------------------------------
    // Shared body of the font and synthetic record builders: assigns bands and
    // writes the curves/bands into the pools using the font's scratch buffers.
    // `quads` come from the font's extraction, from the packed loader's curve
    // blobs or from a caller-provided synthetic outline (the ellipsis glyph).
    // The
    // pools are left untouched only when this returns Ok; callers that need
    // the transactional contract use the wrapper below.
    ImSlugBuildStatus ImSlugWriteGlyphRecordToPools(ImSlugFont* font, ImSlugPools* pools, const ImSlugQuad* quads,
                                                    int quadCount, float advanceEm, ImSlugGlyphRecord* record)
    {
        memset(record, 0, sizeof(*record));
        record->CurveCount = (uint32_t)quadCount;
        record->Flags = ImSlugGlyphFlags_None;  // nonzero fill rule
        record->Advance = advanceEm;

        // Robustness: reject outlines that are not finite, or whose
        // coordinates dwarf any sane em box, before they reach the bbox, the
        // band math or the fp16 pool texels (a NaN bbox would poison the band
        // scales and put NaN positions into the vertex stream). TTF extraction
        // cannot produce these; the synthetic caller path can, and a font with
        // a corrupt bbox could. Definitive failure -> stock ImGui fallback.
        const float kMaxCoord = 64.0f;  // em; real outlines live within a few em
        for (int i = 0; i < quadCount; ++i)
        {
            const float coordinates[6] = {
                quads[i].X1, quads[i].Y1, quads[i].X2, quads[i].Y2, quads[i].X3, quads[i].Y3
            };
            for (int c = 0; c < 6; ++c)
            {
                if (!isfinite(coordinates[c]) || fabsf(coordinates[c]) > kMaxCoord)
                    return ImSlugBuildStatus_InvalidOutline;
            }
        }

        // Control-point bbox (conservative and equal to what the bands assume).
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        ImSlugControlBoundsOfQuads(quads, quadCount, &minX, &minY, &maxX, &maxY);
        record->MinX = minX;
        record->MinY = minY;
        record->MaxX = maxX;
        record->MaxY = maxY;

        // Adaptive band counts (always >= 1 band so the headers exist).
        const int bandCount = quadCount > 0 ? ImSlugCalcBandCount(quadCount) : 1;
        const int bandMaxX = bandCount - 1;
        const int bandMaxY = bandCount - 1;

        // Band assignment: a capacity probe (which fills the counts), then the
        // real fill. With refCapacity = 0 the probe only fails when
        // requiredRefs > 0; the returned counts are valid either way.
        int requiredRefs = 0;
        ImSlugAssignBands(quads, quadCount, bandMaxX, bandMaxY, minX, minY, maxX, maxY, font->BandCounts, nullptr, 0,
                          &requiredRefs);
        if (!EnsureArray((void**)&font->ScratchRefs, &font->ScratchRefCapacity, requiredRefs, sizeof(int)))
            return ImSlugBuildStatus_OutOfMemory;
        if (!ImSlugAssignBands(quads, quadCount, bandMaxX, bandMaxY, minX, minY, maxX, maxY, font->BandCounts,
                               font->ScratchRefs, font->ScratchRefCapacity, &requiredRefs))
        {
            return ImSlugBuildStatus_OutOfMemory;  // capacity was grown above
        }

        // Curve pool: one contiguous 2-texel block for the whole glyph.
        ImSlugCurveBlock curveBlock;
        if (!ImSlugAllocCurveBlock(&pools->Curves, quadCount, &curveBlock))
            return ImSlugBuildStatus_CurvePoolLimit;
        if (!EnsureArray((void**)&font->ScratchCurveLocs, &font->ScratchCurveLocCapacity, quadCount,
                         2 * sizeof(uint16_t)))
        {
            return ImSlugBuildStatus_OutOfMemory;
        }
        for (int i = 0; i < quadCount; ++i)
        {
            int texelX = 0, texelY = 0;
            ImSlugCurveLocAt(curveBlock, i, &texelX, &texelY);
            const ImSlugQuad& quad = quads[i];
            ImSlugWriteCurve(&pools->Curves, texelX, texelY, quad.X1, quad.Y1, quad.X2, quad.Y2, quad.X3, quad.Y3);
            font->ScratchCurveLocs[i * 2 + 0] = (uint16_t)texelX;
            font->ScratchCurveLocs[i * 2 + 1] = (uint16_t)texelY;
        }

        // Band pool: headers + reference lists (the allocator inserts row
        // padding wherever a list would straddle a row). Its status is already
        // classified against the real padded total: GlyphTooLarge is
        // definitive (fall back to stock ImGui), BandPoolLimit is retryable.
        uint16_t listOffsets[2 * kMaxBandCountPerAxis];
        ImSlugBandBlock bandBlock;
        const ImSlugBuildStatus bandStatus =
            ImSlugAllocBandBlock(&pools->Bands, bandMaxX, bandMaxY, font->BandCounts, &bandBlock, listOffsets);
        if (bandStatus != ImSlugBuildStatus_Ok)
            return bandStatus;

        int refCursor = 0;
        for (int band = 0; band <= bandMaxY + bandMaxX + 1; ++band)
        {
            const int bandRefCount = font->BandCounts[band];
            ImSlugBandWriteHeader(&pools->Bands, bandBlock, band, (uint16_t)bandRefCount, listOffsets[band]);
            for (int r = 0; r < bandRefCount; ++r)
            {
                const int curveIndex = font->ScratchRefs[refCursor];
                ImSlugBandWriteRef(&pools->Bands, bandBlock, listOffsets[band], r,
                                   font->ScratchCurveLocs[curveIndex * 2 + 0],
                                   font->ScratchCurveLocs[curveIndex * 2 + 1]);
                refCursor++;
            }
        }

        record->BandLocX = bandBlock.X;
        record->BandLocY = bandBlock.Y;
        record->BandMaxX = bandMaxX;
        record->BandMaxY = bandMaxY;
        const float extentX = maxX - minX;
        const float extentY = maxY - minY;
        record->BandScaleX = extentX > 0.0f ? (float)(bandMaxX + 1) / extentX : 0.0f;
        record->BandScaleY = extentY > 0.0f ? (float)(bandMaxY + 1) / extentY : 0.0f;
        record->BandOffsetX = -minX * record->BandScaleX;
        record->BandOffsetY = -minY * record->BandScaleY;
        record->CurveLocX = curveBlock.X;
        record->CurveLocY = curveBlock.Y;
        return ImSlugBuildStatus_Ok;
    }

    // Builds are transactional: a failed build is retried on the next call
    // (non-cached failures), so it must leave the pools exactly as it found
    // them. Both cursors and the dirty/used bookkeeping are rolled back;
    // Texels/Height are kept (pool growth is not undone, and the garbage texels
    // written by the failed attempt are overwritten by the next successful
    // build at the same cursor).
    static ImSlugBuildStatus RollbackOnFailure(ImSlugPools* pools, ImSlugBuildStatus status, int curveCursor,
                                               int curveUsedRows, int curveDirtyMin, int curveDirtyMax, int bandCursor,
                                               int bandUsedRows, int bandDirtyMin, int bandDirtyMax)
    {
        if (status != ImSlugBuildStatus_Ok)
        {
            pools->Curves.CursorTexels = curveCursor;
            pools->Curves.UsedRows = curveUsedRows;
            pools->Curves.DirtyRowMin = curveDirtyMin;
            pools->Curves.DirtyRowMax = curveDirtyMax;
            pools->Bands.CursorTexels = bandCursor;
            pools->Bands.UsedRows = bandUsedRows;
            pools->Bands.DirtyRowMin = bandDirtyMin;
            pools->Bands.DirtyRowMax = bandDirtyMax;
        }
        return status;
    }

    ImSlugBuildStatus ImSlugBuildGlyphRecordToPoolsTransactional(ImSlugFont* font, ImSlugPools* pools,
                                                                 const ImSlugQuad* quads, int quadCount,
                                                                 float advanceEm, ImSlugGlyphRecord* record)
    {
        const int curveCursor = pools->Curves.CursorTexels;
        const int curveUsedRows = pools->Curves.UsedRows;
        const int curveDirtyMin = pools->Curves.DirtyRowMin;
        const int curveDirtyMax = pools->Curves.DirtyRowMax;
        const int bandCursor = pools->Bands.CursorTexels;
        const int bandUsedRows = pools->Bands.UsedRows;
        const int bandDirtyMin = pools->Bands.DirtyRowMin;
        const int bandDirtyMax = pools->Bands.DirtyRowMax;

        const ImSlugBuildStatus status =
            ImSlugWriteGlyphRecordToPools(font, pools, quads, quadCount, advanceEm, record);
        return RollbackOnFailure(pools, status, curveCursor, curveUsedRows, curveDirtyMin, curveDirtyMax, bandCursor,
                                 bandUsedRows, bandDirtyMin, bandDirtyMax);
    }

    // ---------------------------------------------------------------------
    // Glyph cache + kind dispatch
    // ---------------------------------------------------------------------
    static ImSlugBuildStatus BuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                              ImSlugGlyphRecord* record)
    {
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedBuildGlyphRecord(font, pools, glyphIndex, record);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfBuildGlyphRecord(font, pools, glyphIndex, record);
#else
        // The TTF kind cannot exist in a no-parser build; this is unreachable
        // (font creation is compile-gated the same way) and returns the
        // fallback status defensively.
        return ImSlugBuildStatus_InvalidFont;
#endif
    }

    ImSlugBuildStatus ImSlugGetOrBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord)
    {
        IMSLUG_ASSERT(font != nullptr && pools != nullptr && outRecord != nullptr);
        *outRecord = nullptr;
        if (glyphIndex <= 0)
            return ImSlugBuildStatus_GlyphNotFound;

        for (int i = 0; i < font->CacheCount; ++i)
        {
            const ImSlugGlyphCacheEntry* entry = font->Cache[i];
            if (entry->GlyphIndex == glyphIndex && entry->OwnerPools == pools)
            {
                if (entry->Status == ImSlugBuildStatus_Ok)
                    *outRecord = &entry->Record;
                return entry->Status;
            }
        }

        // Reserve the cache entry *before* building: "Ok implies a valid stable
        // record pointer" must hold unconditionally, so a successful build must
        // never fail to publish. Allocation failures here return OutOfMemory
        // with the pools untouched, and are retried on the next call.
        if (!EnsureArray((void**)&font->Cache, &font->CacheCapacity, font->CacheCount + 1,
                         sizeof(ImSlugGlyphCacheEntry*)))
            return ImSlugBuildStatus_OutOfMemory;
        ImSlugGlyphCacheEntry* entry = (ImSlugGlyphCacheEntry*)malloc(sizeof(ImSlugGlyphCacheEntry));
        if (entry == nullptr)
            return ImSlugBuildStatus_OutOfMemory;

        ImSlugGlyphRecord record;
        const ImSlugBuildStatus status = BuildGlyphRecord(font, pools, glyphIndex, &record);

        // Cache successes and definitive failures ("not found" / too large /
        // invalid outline); transient capacity or allocation failures are
        // retried on the next call (the builder rolled the pools back).
        const bool cacheable =
            (status == ImSlugBuildStatus_Ok || status == ImSlugBuildStatus_GlyphNotFound ||
             status == ImSlugBuildStatus_GlyphTooLarge || status == ImSlugBuildStatus_InvalidOutline);
        if (!cacheable)
        {
            free(entry);
            return status;
        }

        entry->GlyphIndex = glyphIndex;
        entry->Status = status;
        entry->OwnerPools = pools;
        entry->Record = record;
        font->Cache[font->CacheCount++] = entry;
        if (status == ImSlugBuildStatus_Ok)
            *outRecord = &entry->Record;  // guaranteed: the entry was reserved above
        return status;
    }

    // ---------------------------------------------------------------------
    // Public font API (dispatched by kind)
    // ---------------------------------------------------------------------
    void ImSlugDestroyFont(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        if (font->Kind == ImSlugFontKind_Packed)
        {
            ImSlugPackedDestroy(font);
            return;
        }
#if !defined(IMSLUG_NO_STBTT)
        ImSlugTtfDestroy(font);
#else
        // Unreachable: the TTF kind cannot be created in a no-parser build.
        ImSlugFreeFontCommon(font);
        free(font);
#endif
    }

    void ImSlugGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
        {
            ImSlugPackedGetFontMetrics(font, bakeSizePixels, outMetrics);
            return;
        }
#if !defined(IMSLUG_NO_STBTT)
        ImSlugTtfGetFontMetrics(font, bakeSizePixels, outMetrics);
#else
        memset(outMetrics, 0, sizeof(*outMetrics));  // unreachable: no TTF font can exist
#endif
    }

    bool ImSlugGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                      ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedGetGlyphMetricsByIndex(font, glyphIndex, bakeSizePixels, outMetrics);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetGlyphMetricsByIndex(font, glyphIndex, bakeSizePixels, outMetrics);
#else
        return false;  // unreachable: no TTF font can exist
#endif
    }

    bool ImSlugGetGlyphMetrics(const ImSlugFont* font, unsigned int codepoint, float bakeSizePixels,
                               ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr);
        const int glyphIndex = ImSlugGetGlyphIndex(font, codepoint);
        return ImSlugGetGlyphMetricsByIndex(font, glyphIndex, bakeSizePixels, outMetrics);
    }

    int ImSlugGetGlyphCount(const ImSlugFont* font)
    {
        IMSLUG_ASSERT(font != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedGetGlyphCount(font);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetGlyphCount(font);
#else
        return 0;  // unreachable: no TTF font can exist
#endif
    }

    int ImSlugGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint)
    {
        IMSLUG_ASSERT(font != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedGetGlyphIndex(font, codepoint);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetGlyphIndex(font, codepoint);
#else
        return 0;  // unreachable: no TTF font can exist
#endif
    }

    bool ImSlugGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads, int quadCapacity,
                                    int* outCount)
    {
        IMSLUG_ASSERT(font != nullptr && outCount != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedGetGlyphOutlineQuads(font, glyphIndex, outQuads, quadCapacity, outCount);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetGlyphOutlineQuads(font, glyphIndex, outQuads, quadCapacity, outCount);
#else
        *outCount = 0;
        return false;  // unreachable: no TTF font can exist
#endif
    }

    bool ImSlugGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex, ImSlugGlyphSourceMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        if (font->Kind == ImSlugFontKind_Packed)
            return ImSlugPackedGetGlyphSourceMetrics(font, glyphIndex, outMetrics);
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetGlyphSourceMetrics(font, glyphIndex, outMetrics);
#else
        return false;  // unreachable: no TTF font can exist
#endif
    }

    ImSlugBuildStatus ImSlugGetGlyphRecordByIndex(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord)
    {
        return ImSlugGetOrBuildGlyphRecord(font, pools, glyphIndex, outRecord);
    }

    ImSlugBuildStatus ImSlugGetGlyphRecord(ImSlugFont* font, ImSlugPools* pools, unsigned int codepoint,
                                           const ImSlugGlyphRecord** outRecord)
    {
        IMSLUG_ASSERT(font != nullptr);
        const int glyphIndex = ImSlugGetGlyphIndex(font, codepoint);
        return ImSlugGetOrBuildGlyphRecord(font, pools, glyphIndex, outRecord);
    }

    // Same contract as the font record builders, from quads that did not
    // come from the font (the synthetic ellipsis). `pools` is owned by the
    // caller; the record embeds pool locations, so it must be freed/never
    // reused if the pools are destroyed. `font` supplies the build scratch only
    // (both kinds have one).
    ImSlugBuildStatus ImSlugBuildSyntheticGlyphRecord(ImSlugFont* font, ImSlugPools* pools, const ImSlugQuad* quads,
                                                      int quadCount, float advanceEm, ImSlugGlyphRecord* outRecord)
    {
        IMSLUG_ASSERT(font != nullptr && pools != nullptr && outRecord != nullptr);
        IMSLUG_ASSERT(quadCount >= 0 && (quadCount == 0 || quads != nullptr));
        return ImSlugBuildGlyphRecordToPoolsTransactional(font, pools, quads, quadCount, advanceEm, outRecord);
    }

    ImSlugColorFont* ImSlugFontGetColor(ImSlugFont* font)
    {
        if (font == nullptr)
            return nullptr;
        // Packed assets are monochrome in v1 (no COLR/CPAL payload): the color
        // path falls back to the glyf outline, exactly like a font without
        // color tables.
        if (font->Kind == ImSlugFontKind_Packed)
            return nullptr;
#if !defined(IMSLUG_NO_STBTT)
        return ImSlugTtfGetColorFont(font);
#else
        return nullptr;  // unreachable: no TTF font can exist
#endif
    }
}  // namespace ImSlug
