// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_glyphgen.cpp — the font-side core:
//   1. stbtt outline extraction -> em-space quadratics (Y up, contour order);
//   2. adaptive band construction (PLAN §4.2) with descending max-x/max-y sort;
//   3. the font handle with the lazy glyph-record cache and ImGui-parity metrics.
//
// Units: everything stored here is em space (1.0 = em, Y up, baseline origin).
// stbtt_GetGlyphShape returns font-unit coordinates in the font's own Y-up
// convention (the glyf table; composites are transformed by stbtt), so the
// conversion is a single division by unitsPerEm — no Y flip (the orientation
// test for 'g' in tests/test_glyphgen.cpp pins this).
//
// Metrics mirror Dear ImGui v1.92.9b's stb loader (imgui_draw.cpp) exactly so
// layout stays pixel-identical. Source lines cited in comments below are from
// the pinned ImGui tag used by this repo (IMSLUG_IMGUI_TAG, CMakeLists.txt).

#include "imslug_glyphgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------
    static bool EnsureArray(void** data, int* capacity, int required, size_t elementSize)
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

    // ---------------------------------------------------------------------
    // Outline extraction
    // ---------------------------------------------------------------------
    // FreeType/OpenType unitsPerEm lives in head.unitsPerEm; stbtt_fontinfo
    // exposes the table offsets but not the value, so read it here.
    static int ReadUnitsPerEm(const stbtt_fontinfo* font)
    {
        const unsigned char* head = font->data + font->head;
        return (head[18] << 8) | head[19];
    }

    int ImSlugConvertCubicToQuadratics(const ImSlugCubic* cubic, ImSlugQuad outQuads[2])
    {
        IMSLUG_ASSERT(cubic != nullptr && outQuads != nullptr);

        const float p0x = cubic->X1, p0y = cubic->Y1;
        if (cubic->X1 == cubic->X2 && cubic->Y1 == cubic->Y2 && cubic->X1 == cubic->X3 && cubic->Y1 == cubic->Y3 &&
            cubic->X1 == cubic->X4 && cubic->Y1 == cubic->Y4)
        {
            return 0;  // fully degenerate cubic
        }

        // Midpoint at t = 0.5: M = (P0 + 3 P1 + 3 P2 + P3) / 8.
        const float mx = (cubic->X1 + 3.0f * cubic->X2 + 3.0f * cubic->X3 + cubic->X4) * 0.125f;
        const float my = (cubic->Y1 + 3.0f * cubic->Y2 + 3.0f * cubic->Y3 + cubic->Y4) * 0.125f;

        // Q1 = P0 + 3/4 (P1 - P0); Q2 = P3 + 3/4 (P2 - P3).
        outQuads[0].X1 = p0x;
        outQuads[0].Y1 = p0y;
        outQuads[0].X2 = p0x + 0.75f * (cubic->X2 - p0x);
        outQuads[0].Y2 = p0y + 0.75f * (cubic->Y2 - p0y);
        outQuads[0].X3 = mx;
        outQuads[0].Y3 = my;

        outQuads[1].X1 = mx;
        outQuads[1].Y1 = my;
        outQuads[1].X2 = cubic->X4 + 0.75f * (cubic->X3 - cubic->X4);
        outQuads[1].Y2 = cubic->Y4 + 0.75f * (cubic->Y3 - cubic->Y4);
        outQuads[1].X3 = cubic->X4;
        outQuads[1].Y3 = cubic->Y4;
        return 2;
    }

    bool ImSlugExtractGlyphOutline(const stbtt_fontinfo* font, int glyphIndex, ImSlugQuad* quads, int quadCapacity,
                                   int* quadCount)
    {
        IMSLUG_ASSERT(font != nullptr && quadCount != nullptr);
        IMSLUG_ASSERT(quads != nullptr || quadCapacity <= 0);
        *quadCount = 0;

        stbtt_vertex* vertices = nullptr;
        const int vertexCount = stbtt_GetGlyphShape(font, glyphIndex, &vertices);
        if (vertexCount <= 0 || vertices == nullptr)
        {
            if (vertices != nullptr)
                stbtt_FreeShape(font, vertices);
            return true;  // empty glyph (space, or no outline data)
        }

        const float unitScale = 1.0f / (float)ReadUnitsPerEm(font);

        // Pass 1: required capacity (every non-move vertex is at least one quad,
        // and a cubic becomes two).
        int required = 0;
        for (int i = 0; i < vertexCount; ++i)
        {
            const stbtt_vertex& v = vertices[i];
            if (v.type == STBTT_vcubic)
                required += 2;
            else if (v.type != STBTT_vmove)
                required += 1;
        }
        if (required > quadCapacity)
        {
            stbtt_FreeShape(font, vertices);
            *quadCount = required;
            return false;
        }

        // Pass 2: convert. stbtt closes each contour (stbtt__close_shape), so
        // every segment between consecutive vertices belongs to the outline;
        // an implicit closing segment must NOT be added again.
        int written = 0;
        float currentX = 0.0f, currentY = 0.0f;
        for (int i = 0; i < vertexCount; ++i)
        {
            const stbtt_vertex& v = vertices[i];
            switch (v.type)
            {
            case STBTT_vmove:
                currentX = (float)v.x * unitScale;
                currentY = (float)v.y * unitScale;
                break;

            case STBTT_vline:
            {
                // Line rule (PLAN §2.2 rule 1): {p1, p3, p3}.
                const float x = (float)v.x * unitScale;
                const float y = (float)v.y * unitScale;
                if (x != currentX || y != currentY)
                {
                    ImSlugQuad& quad = quads[written++];
                    quad.X1 = currentX;
                    quad.Y1 = currentY;
                    quad.X2 = x;
                    quad.Y2 = y;
                    quad.X3 = x;
                    quad.Y3 = y;
                }
                currentX = x;
                currentY = y;
                break;
            }

            case STBTT_vcurve:
            {
                const float cx = (float)v.cx * unitScale;
                const float cy = (float)v.cy * unitScale;
                const float x = (float)v.x * unitScale;
                const float y = (float)v.y * unitScale;
                if (x != currentX || y != currentY || cx != currentX || cy != currentY)
                {
                    ImSlugQuad& quad = quads[written++];
                    quad.X1 = currentX;
                    quad.Y1 = currentY;
                    quad.X2 = cx;
                    quad.Y2 = cy;
                    quad.X3 = x;
                    quad.Y3 = y;
                }
                currentX = x;
                currentY = y;
                break;
            }

            case STBTT_vcubic:
            {
                const float x = (float)v.x * unitScale;
                const float y = (float)v.y * unitScale;
                ImSlugCubic cubic;
                cubic.X1 = currentX;
                cubic.Y1 = currentY;
                cubic.X2 = (float)v.cx * unitScale;
                cubic.Y2 = (float)v.cy * unitScale;
                cubic.X3 = (float)v.cx1 * unitScale;
                cubic.Y3 = (float)v.cy1 * unitScale;
                cubic.X4 = x;
                cubic.Y4 = y;
                ImSlugQuad converted[2];
                const int quadCountCubic = ImSlugConvertCubicToQuadratics(&cubic, converted);
                for (int q = 0; q < quadCountCubic; ++q)
                    quads[written++] = converted[q];
                currentX = x;
                currentY = y;
                break;
            }

            default:
                break;
            }
        }

        stbtt_FreeShape(font, vertices);
        *quadCount = written;
        return true;
    }

    // ---------------------------------------------------------------------
    // Band construction
    // ---------------------------------------------------------------------
    int ImSlugCalcBandCount(int curveCount)
    {
        if (curveCount <= 0)
            return 4;  // clamp(ceil(sqrt(0)), 4, 32)
        const float n = ceilf(sqrtf(2.0f * (float)curveCount));
        int count = (int)n;
        if (count < 4)
            count = 4;
        if (count > kMaxBandCountPerAxis)
            count = kMaxBandCountPerAxis;
        return count;
    }

    ImSlugQuadBounds ImSlugQuadControlBounds(const ImSlugQuad& quad)
    {
        ImSlugQuadBounds bounds;
        bounds.MinX =
            quad.X1 < quad.X2 ? (quad.X1 < quad.X3 ? quad.X1 : quad.X3) : (quad.X2 < quad.X3 ? quad.X2 : quad.X3);
        bounds.MinY =
            quad.Y1 < quad.Y2 ? (quad.Y1 < quad.Y3 ? quad.Y1 : quad.Y3) : (quad.Y2 < quad.Y3 ? quad.Y2 : quad.Y3);
        bounds.MaxX =
            quad.X1 > quad.X2 ? (quad.X1 > quad.X3 ? quad.X1 : quad.X3) : (quad.X2 > quad.X3 ? quad.X2 : quad.X3);
        bounds.MaxY =
            quad.Y1 > quad.Y2 ? (quad.Y1 > quad.Y3 ? quad.Y1 : quad.Y3) : (quad.Y2 > quad.Y3 ? quad.Y2 : quad.Y3);
        return bounds;
    }

    // Exactly axis-parallel curves can never cross a ray of the matching axis
    // (PLAN §2.2 rule 2); they are excluded from those bands.
    static bool IsHorizontalCurve(const ImSlugQuad& quad)
    {
        return quad.Y1 == quad.Y2 && quad.Y2 == quad.Y3;
    }

    static bool IsVerticalCurve(const ImSlugQuad& quad)
    {
        return quad.X1 == quad.X2 && quad.X2 == quad.X3;
    }

    // Stable insertion sort, descending by key (deterministic — equal keys
    // keep their curve-index order).
    static void SortBandRefsDescending(int* refs, int count, const ImSlugQuad* quads, bool horizontal)
    {
        for (int i = 1; i < count; ++i)
        {
            const int item = refs[i];
            const ImSlugQuadBounds bounds = ImSlugQuadControlBounds(quads[item]);
            const float key = horizontal ? bounds.MaxX : bounds.MaxY;
            int j = i - 1;
            while (j >= 0)
            {
                const ImSlugQuadBounds previous = ImSlugQuadControlBounds(quads[refs[j]]);
                const float previousKey = horizontal ? previous.MaxX : previous.MaxY;
                if (previousKey >= key)
                    break;
                refs[j + 1] = refs[j];
                j--;
            }
            refs[j + 1] = item;
        }
    }

    bool ImSlugAssignBands(const ImSlugQuad* quads, int quadCount, int bandMaxX, int bandMaxY, float minX, float minY,
                           float maxX, float maxY, int* counts, int* refs, int refCapacity, int* requiredRefs)
    {
        IMSLUG_ASSERT(quads != nullptr || quadCount == 0);
        IMSLUG_ASSERT(counts != nullptr && requiredRefs != nullptr);
        IMSLUG_ASSERT(refs != nullptr || refCapacity <= 0);
        IMSLUG_ASSERT(bandMaxX >= 0 && bandMaxX < kMaxBandCountPerAxis);
        IMSLUG_ASSERT(bandMaxY >= 0 && bandMaxY < kMaxBandCountPerAxis);

        const int hBandCount = bandMaxY + 1;  // horizontal slices, indexed by y
        const int vBandCount = bandMaxX + 1;  // vertical slices, indexed by x
        const int totalBands = hBandCount + vBandCount;
        *requiredRefs = 0;

        memset(counts, 0, (size_t)totalBands * sizeof(int));
        if (quadCount <= 0)
            return true;

        const float extentX = maxX - minX;
        const float extentY = maxY - minY;
        const float epsilon = kBandOverlapEpsilon;

        // Pass 1: counts. A curve joins a band when its control-point bbox
        // (inflated by epsilon) overlaps the band's em range.
        for (int i = 0; i < quadCount; ++i)
        {
            const ImSlugQuad& quad = quads[i];
            const ImSlugQuadBounds bounds = ImSlugQuadControlBounds(quad);

            if (!IsHorizontalCurve(quad) && extentY > 0.0f)
            {
                for (int band = 0; band < hBandCount; ++band)
                {
                    const float bandY0 = minY + extentY * (float)band / (float)hBandCount;
                    const float bandY1 = minY + extentY * (float)(band + 1) / (float)hBandCount;
                    if (bounds.MinY - epsilon <= bandY1 && bounds.MaxY + epsilon >= bandY0)
                        counts[band]++;
                }
            }

            if (!IsVerticalCurve(quad) && extentX > 0.0f)
            {
                for (int band = 0; band < vBandCount; ++band)
                {
                    const float bandX0 = minX + extentX * (float)band / (float)vBandCount;
                    const float bandX1 = minX + extentX * (float)(band + 1) / (float)vBandCount;
                    if (bounds.MinX - epsilon <= bandX1 && bounds.MaxX + epsilon >= bandX0)
                        counts[hBandCount + band]++;
                }
            }
        }

        int total = 0;
        for (int band = 0; band < totalBands; ++band)
            total += counts[band];
        *requiredRefs = total;
        if (total > refCapacity)
            return false;

        // Pass 2: fill each band in curve-index order (stable input order for
        // the sort below), then sort.
        int cursors[2 * kMaxBandCountPerAxis];
        int running = 0;
        for (int band = 0; band < totalBands; ++band)
        {
            cursors[band] = running;
            running += counts[band];
        }

        for (int i = 0; i < quadCount; ++i)
        {
            const ImSlugQuad& quad = quads[i];
            const ImSlugQuadBounds bounds = ImSlugQuadControlBounds(quad);

            if (!IsHorizontalCurve(quad) && extentY > 0.0f)
            {
                for (int band = 0; band < hBandCount; ++band)
                {
                    const float bandY0 = minY + extentY * (float)band / (float)hBandCount;
                    const float bandY1 = minY + extentY * (float)(band + 1) / (float)hBandCount;
                    if (bounds.MinY - epsilon <= bandY1 && bounds.MaxY + epsilon >= bandY0)
                        refs[cursors[band]++] = i;
                }
            }

            if (!IsVerticalCurve(quad) && extentX > 0.0f)
            {
                for (int band = 0; band < vBandCount; ++band)
                {
                    const float bandX0 = minX + extentX * (float)band / (float)vBandCount;
                    const float bandX1 = minX + extentX * (float)(band + 1) / (float)vBandCount;
                    if (bounds.MinX - epsilon <= bandX1 && bounds.MaxX + epsilon >= bandX0)
                        refs[cursors[hBandCount + band]++] = i;
                }
            }
        }

        for (int band = 0; band < totalBands; ++band)
        {
            const int start = (band == 0) ? 0 : cursors[band - 1];
            SortBandRefsDescending(refs + start, counts[band], quads, band < hBandCount);
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // Font handle
    // ---------------------------------------------------------------------
    struct ImSlugGlyphCacheEntry
    {
        int GlyphIndex;
        ImSlugBuildStatus Status;
        ImSlugPools* OwnerPools;
        ImSlugGlyphRecord Record;
    };

    struct ImSlugFont
    {
        stbtt_fontinfo FontInfo;
        unsigned char* OwnedData;  // null when created with copyData = false
        int UnitsPerEm;

        // Build scratch (grow-only; the font is single-threaded like ImGui).
        ImSlugQuad* ScratchQuads;
        int ScratchQuadCapacity;
        int BandCounts[2 * kMaxBandCountPerAxis];
        int* ScratchRefs;
        int ScratchRefCapacity;
        uint16_t* ScratchCurveLocs;  // (x, y) pairs, one per curve
        int ScratchCurveLocCapacity;

        // Lazy glyph cache: entries are individually allocated so a record
        // pointer stays stable while more glyphs are built.
        ImSlugGlyphCacheEntry** Cache;
        int CacheCount;
        int CacheCapacity;
    };

    static void DestroyFont(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        for (int i = 0; i < font->CacheCount; ++i)
            free(font->Cache[i]);
        free(font->Cache);
        free(font->ScratchQuads);
        free(font->ScratchRefs);
        free(font->ScratchCurveLocs);
        free(font->OwnedData);
        free(font);
    }

    ImSlugFont* ImSlugCreateFontFromMemory(const void* fontData, int fontDataSize, int fontIndex, bool copyData)
    {
        if (fontData == nullptr || fontDataSize <= 0 || fontIndex < 0)
            return nullptr;

        ImSlugFont* font = (ImSlugFont*)malloc(sizeof(ImSlugFont));
        if (font == nullptr)
            return nullptr;
        memset(font, 0, sizeof(*font));

        unsigned char* bytes = nullptr;
        if (copyData)
        {
            bytes = (unsigned char*)malloc((size_t)fontDataSize);
            if (bytes == nullptr)
            {
                free(font);
                return nullptr;
            }
            memcpy(bytes, fontData, (size_t)fontDataSize);
        }
        else
        {
            bytes = (unsigned char*)fontData;
        }

        const int fontOffset = stbtt_GetFontOffsetForIndex(bytes, fontIndex);
        if (fontOffset < 0 || !stbtt_InitFont(&font->FontInfo, bytes, fontOffset))
        {
            if (copyData)
                free(bytes);
            free(font);
            return nullptr;
        }

        font->OwnedData = copyData ? bytes : nullptr;
        font->UnitsPerEm = ReadUnitsPerEm(&font->FontInfo);
        if (font->UnitsPerEm <= 0)
        {
            DestroyFont(font);  // DestroyFont handles the cache/scratch being empty
            return nullptr;
        }
        return font;
    }

    void ImSlugDestroyFont(ImSlugFont* font)
    {
        DestroyFont(font);
    }

    // ---------------------------------------------------------------------
    // Metrics (Dear ImGui v1.92.9b, imgui_draw.cpp, stb loader)
    // ---------------------------------------------------------------------
    // ImGui line 4725: ScaleFactor = stbtt_ScaleForPixelHeight(FontInfo, 1.0f)
    // ImGui line 4761: scale_for_layout = ScaleFactor * baked->Size (ExtraSizeScale = 1)
    // ImGui line 4764: Ascent = ImCeil(unscaled_ascent * scale_for_layout)
    // ImGui line 4765: Descent = ImFloor(unscaled_descent * scale_for_layout)
    // ImGui lines 4790-4797: bitmap box and advance; line 4822 re-fetches the box.
    // ImGui lines 4846-4849: X0 = x0 * recip_h + font_off_x (recip = 1, offset 0 with
    //   OversampleH/V = 1, RasterizerDensity = 1, GlyphOffset = 0);
    //   Y0 = y0 + IM_ROUND(Ascent).
    // ImGui line 4807: an empty bitmap box leaves X0..Y1 at the ImFontGlyph
    //   constructor's zeros (invisible glyph, e.g. space).
    static float ImSlugImCeil(float x)  // ImGui: #define ImCeil(X) ceilf(X)
    {
        return ceilf(x);
    }

    static float ImSlugImFloor(float x)  // ImGui: ImFloor, exact floor for negatives too
    {
        return (x >= 0.0f || (float)(int)x == x) ? (float)(int)x : (float)((int)x - 1);
    }

    static float ImSlugImRound(float x)  // ImGui: #define IM_ROUND(_VAL) ((float)(int)((_VAL) + 0.5f))
    {
        return (float)(int)(x + 0.5f);
    }

    static float ScaleForPixelHeight1(const ImSlugFont* font)
    {
        // stbtt_ScaleForPixelHeight(font, 1.0f) == 1 / (hhea.ascent - hhea.descent).
        return stbtt_ScaleForPixelHeight(&font->FontInfo, 1.0f);
    }

    void ImSlugGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);

        const float scale = ScaleForPixelHeight1(font) * bakeSizePixels;
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font->FontInfo, &ascent, &descent, &lineGap);

        outMetrics->UnitsPerEm = font->UnitsPerEm;
        outMetrics->HheaAscent = ascent;
        outMetrics->HheaDescent = descent;
        outMetrics->ScaleForPixelHeight = scale;
        outMetrics->PixelsPerEm = scale * (float)font->UnitsPerEm;
        outMetrics->Ascent = ImSlugImCeil((float)ascent * scale);
        outMetrics->Descent = ImSlugImFloor((float)descent * scale);
    }

    bool ImSlugGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                      ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        if (glyphIndex <= 0)
            return false;  // no .notdef metrics (ImGui's loader returns false there)

        const float scale = ScaleForPixelHeight1(font) * bakeSizePixels;

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&font->FontInfo, glyphIndex, scale, scale, 0.0f, 0.0f, &x0, &y0, &x1, &y1);

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font->FontInfo, glyphIndex, &advance, &lsb);

        outMetrics->AdvanceX = (float)advance * scale;
        outMetrics->AdvanceEm = (float)advance / (float)font->UnitsPerEm;
        if (x0 == x1 || y0 == y1)
        {
            // Invisible glyph: ImFontGlyph's zero-initialized corners.
            outMetrics->X0 = 0.0f;
            outMetrics->Y0 = 0.0f;
            outMetrics->X1 = 0.0f;
            outMetrics->Y1 = 0.0f;
        }
        else
        {
            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&font->FontInfo, &ascent, &descent, &lineGap);
            const float ascentOffset = ImSlugImRound(ImSlugImCeil((float)ascent * scale));
            outMetrics->X0 = (float)x0;
            outMetrics->Y0 = (float)y0 + ascentOffset;
            outMetrics->X1 = (float)x1;
            outMetrics->Y1 = (float)y1 + ascentOffset;
        }
        return true;
    }

    bool ImSlugGetGlyphMetrics(const ImSlugFont* font, unsigned int codepoint, float bakeSizePixels,
                               ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr);
        const int glyphIndex = stbtt_FindGlyphIndex(&font->FontInfo, (int)codepoint);
        return ImSlugGetGlyphMetricsByIndex(font, glyphIndex, bakeSizePixels, outMetrics);
    }

    int ImSlugGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint)
    {
        IMSLUG_ASSERT(font != nullptr);
        return stbtt_FindGlyphIndex(&font->FontInfo, (int)codepoint);
    }

    // ---------------------------------------------------------------------
    // Glyph records
    // ---------------------------------------------------------------------
    static bool ExtractWithScratch(ImSlugFont* font, int glyphIndex, int* outQuadCount)
    {
        int required = 0;
        if (ImSlugExtractGlyphOutline(&font->FontInfo, glyphIndex, font->ScratchQuads, font->ScratchQuadCapacity,
                                      &required))
        {
            *outQuadCount = required;
            return true;
        }
        if (required <= 0 ||
            !EnsureArray((void**)&font->ScratchQuads, &font->ScratchQuadCapacity, required, sizeof(ImSlugQuad)))
        {
            return false;
        }
        int quadCount = 0;
        if (!ImSlugExtractGlyphOutline(&font->FontInfo, glyphIndex, font->ScratchQuads, font->ScratchQuadCapacity,
                                       &quadCount))
        {
            return false;
        }
        *outQuadCount = quadCount;
        return true;
    }

    static ImSlugBuildStatus BuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                              ImSlugGlyphRecord* record)
    {
        int quadCount = 0;
        if (!ExtractWithScratch(font, glyphIndex, &quadCount))
            return ImSlugBuildStatus_OutOfMemory;

        memset(record, 0, sizeof(*record));
        record->CurveCount = (uint32_t)quadCount;
        record->Flags = ImSlugGlyphFlags_None;  // nonzero fill rule

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font->FontInfo, glyphIndex, &advance, &lsb);
        record->Advance = (float)advance / (float)font->UnitsPerEm;

        // Control-point bbox (conservative and equal to what the bands assume).
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (quadCount > 0)
        {
            ImSlugQuadBounds bounds = ImSlugQuadControlBounds(font->ScratchQuads[0]);
            minX = bounds.MinX;
            minY = bounds.MinY;
            maxX = bounds.MaxX;
            maxY = bounds.MaxY;
            for (int i = 1; i < quadCount; ++i)
            {
                bounds = ImSlugQuadControlBounds(font->ScratchQuads[i]);
                if (bounds.MinX < minX)
                    minX = bounds.MinX;
                if (bounds.MinY < minY)
                    minY = bounds.MinY;
                if (bounds.MaxX > maxX)
                    maxX = bounds.MaxX;
                if (bounds.MaxY > maxY)
                    maxY = bounds.MaxY;
            }
        }
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
        ImSlugAssignBands(font->ScratchQuads, quadCount, bandMaxX, bandMaxY, minX, minY, maxX, maxY, font->BandCounts,
                          nullptr, 0, &requiredRefs);
        if (!EnsureArray((void**)&font->ScratchRefs, &font->ScratchRefCapacity, requiredRefs, sizeof(int)))
            return ImSlugBuildStatus_OutOfMemory;
        if (!ImSlugAssignBands(font->ScratchQuads, quadCount, bandMaxX, bandMaxY, minX, minY, maxX, maxY,
                               font->BandCounts, font->ScratchRefs, font->ScratchRefCapacity, &requiredRefs))
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
            const ImSlugQuad& quad = font->ScratchQuads[i];
            ImSlugWriteCurve(&pools->Curves, texelX, texelY, quad.X1, quad.Y1, quad.X2, quad.Y2, quad.X3, quad.Y3);
            font->ScratchCurveLocs[i * 2 + 0] = (uint16_t)texelX;
            font->ScratchCurveLocs[i * 2 + 1] = (uint16_t)texelY;
        }

        // Band pool: headers + reference lists (the allocator inserts row
        // padding wherever a list would straddle a row).
        const int headerCount = bandMaxY + 1 + bandMaxX + 1;
        uint16_t listOffsets[2 * kMaxBandCountPerAxis];
        ImSlugBandBlock bandBlock;
        if (!ImSlugAllocBandBlock(&pools->Bands, bandMaxX, bandMaxY, font->BandCounts, &bandBlock, listOffsets))
        {
            return (headerCount + requiredRefs > kGlyphMaxBlockTexels) ? ImSlugBuildStatus_GlyphTooLarge
                                                                       : ImSlugBuildStatus_BandPoolLimit;
        }

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

    static ImSlugBuildStatus GetOrBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
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

        ImSlugGlyphRecord record;
        const ImSlugBuildStatus status = BuildGlyphRecord(font, pools, glyphIndex, &record);

        // Cache successes and definitive "not found"; transient capacity or
        // allocation failures are retried on the next call.
        const bool cacheable = (status == ImSlugBuildStatus_Ok || status == ImSlugBuildStatus_GlyphNotFound ||
                                status == ImSlugBuildStatus_GlyphTooLarge);
        if (cacheable)
        {
            if (!EnsureArray((void**)&font->Cache, &font->CacheCapacity, font->CacheCount + 1,
                             sizeof(ImSlugGlyphCacheEntry*)))
            {
                return status == ImSlugBuildStatus_Ok ? status : ImSlugBuildStatus_OutOfMemory;
            }
            ImSlugGlyphCacheEntry* entry = (ImSlugGlyphCacheEntry*)malloc(sizeof(ImSlugGlyphCacheEntry));
            if (entry == nullptr)
                return status == ImSlugBuildStatus_Ok ? status : ImSlugBuildStatus_OutOfMemory;
            entry->GlyphIndex = glyphIndex;
            entry->Status = status;
            entry->OwnerPools = pools;
            entry->Record = record;
            font->Cache[font->CacheCount++] = entry;
            if (status == ImSlugBuildStatus_Ok)
                *outRecord = &entry->Record;
        }
        return status;
    }

    ImSlugBuildStatus ImSlugGetGlyphRecordByIndex(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                  const ImSlugGlyphRecord** outRecord)
    {
        return GetOrBuildGlyphRecord(font, pools, glyphIndex, outRecord);
    }

    ImSlugBuildStatus ImSlugGetGlyphRecord(ImSlugFont* font, ImSlugPools* pools, unsigned int codepoint,
                                           const ImSlugGlyphRecord** outRecord)
    {
        IMSLUG_ASSERT(font != nullptr);
        const int glyphIndex = stbtt_FindGlyphIndex(&font->FontInfo, (int)codepoint);
        return GetOrBuildGlyphRecord(font, pools, glyphIndex, outRecord);
    }
}  // namespace ImSlug
