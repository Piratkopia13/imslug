// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_glyphgen.cpp — the font-side core:
//   1. stbtt outline extraction -> em-space quadratics (Y up, contour order);
//   2. adaptive band construction with descending max-x/max-y sort;
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

#include "imslug_font.h"

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
                // Line rule: a line is stored as {p1, p3, p3} (control point
                // and end point coincide).
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
    // Font handle (TTF kind; the shared base lives in imslug_font.h)
    // ---------------------------------------------------------------------
    struct ImSlugTtfFont
    {
        ImSlugFont Base;  // must be the first member (kind cast)
        stbtt_fontinfo FontInfo;
        unsigned char* OwnedData;  // null when created with copyData = false
        int DataSize;              // bytes of the font buffer (FontInfo.data)

        // Lazily parsed COLR/CPAL data (null when absent/unusable).
        ImSlugColorFont* ColorFont;
        bool ColorFontAttempted;
    };

    static ImSlugTtfFont* TtfFont(ImSlugFont* font)
    {
        return (ImSlugTtfFont*)font;  // Base is the first member
    }

    static const ImSlugTtfFont* TtfFont(const ImSlugFont* font)
    {
        return (const ImSlugTtfFont*)font;
    }

    void ImSlugTtfDestroy(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        ImSlugTtfFont* ttf = TtfFont(font);
        if (ttf->ColorFont != nullptr)
            ImSlugColorFontDestroy(ttf->ColorFont);
        ImSlugFreeFontCommon(font);
        free(ttf->OwnedData);
        free(ttf);
    }

    ImSlugColorFont* ImSlugTtfGetColorFont(ImSlugFont* font)
    {
        if (font == nullptr)
            return nullptr;
        ImSlugTtfFont* ttf = TtfFont(font);
        if (!ttf->ColorFontAttempted)
        {
            ttf->ColorFontAttempted = true;
            // stbtt stores the resolved font offset; reuse it so ttcf
            // collections resolve the same face.
            ttf->ColorFont =
                ImSlugColorFontCreate(ttf->FontInfo.data, ttf->DataSize, ttf->FontInfo.fontstart, font->UnitsPerEm);
        }
        return ttf->ColorFont;
    }

    ImSlugFont* ImSlugCreateFontFromMemory(const void* fontData, int fontDataSize, int fontIndex, bool copyData)
    {
        if (fontData == nullptr || fontDataSize <= 0 || fontIndex < 0)
            return nullptr;

        ImSlugTtfFont* ttf = (ImSlugTtfFont*)malloc(sizeof(ImSlugTtfFont));
        if (ttf == nullptr)
            return nullptr;
        memset(ttf, 0, sizeof(*ttf));
        ttf->Base.Kind = ImSlugFontKind_Ttf;

        unsigned char* bytes = nullptr;
        if (copyData)
        {
            bytes = (unsigned char*)malloc((size_t)fontDataSize);
            if (bytes == nullptr)
            {
                free(ttf);
                return nullptr;
            }
            memcpy(bytes, fontData, (size_t)fontDataSize);
        }
        else
        {
            bytes = (unsigned char*)fontData;
        }

        const int fontOffset = stbtt_GetFontOffsetForIndex(bytes, fontIndex);
        if (fontOffset < 0 || !stbtt_InitFont(&ttf->FontInfo, bytes, fontOffset))
        {
            if (copyData)
                free(bytes);
            free(ttf);
            return nullptr;
        }

        ttf->OwnedData = copyData ? bytes : nullptr;
        ttf->DataSize = fontDataSize;
        ttf->Base.UnitsPerEm = ReadUnitsPerEm(&ttf->FontInfo);
        if (ttf->Base.UnitsPerEm <= 0)
        {
            ImSlugTtfDestroy(&ttf->Base);  // handles the cache/scratch being empty
            return nullptr;
        }
        return &ttf->Base;
    }

    // Extract a glyph's outline into the font's grow-only scratch buffer.
    // const (the scratch members are mutable): the metrics query reuses the
    // exact same extraction the glyph record is built from.
    static bool ExtractWithScratch(const ImSlugFont* font, int glyphIndex, int* outQuadCount)
    {
        const ImSlugTtfFont* ttf = TtfFont(font);
        int required = 0;
        if (ImSlugExtractGlyphOutline(&ttf->FontInfo, glyphIndex, font->ScratchQuads, font->ScratchQuadCapacity,
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
        if (!ImSlugExtractGlyphOutline(&ttf->FontInfo, glyphIndex, font->ScratchQuads, font->ScratchQuadCapacity,
                                       &quadCount))
        {
            return false;
        }
        *outQuadCount = quadCount;
        return true;
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
    // ImSlugImCeil/ImSlugImFloor/ImSlugImRound live in imslug_font.h (shared
    // with the packed metrics path).
    static float ScaleForPixelHeight1(const ImSlugFont* font)
    {
        // stbtt_ScaleForPixelHeight(font, 1.0f) == 1 / (hhea.ascent - hhea.descent).
        return stbtt_ScaleForPixelHeight(&TtfFont(font)->FontInfo, 1.0f);
    }

    void ImSlugTtfGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);

        const float scale = ScaleForPixelHeight1(font) * bakeSizePixels;
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&TtfFont(font)->FontInfo, &ascent, &descent, &lineGap);

        outMetrics->UnitsPerEm = font->UnitsPerEm;
        outMetrics->HheaAscent = ascent;
        outMetrics->HheaDescent = descent;
        outMetrics->ScaleForPixelHeight = scale;
        outMetrics->PixelsPerEm = scale * (float)font->UnitsPerEm;
        outMetrics->Ascent = ImSlugImCeil((float)ascent * scale);
        outMetrics->Descent = ImSlugImFloor((float)descent * scale);
    }

    bool ImSlugTtfGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                         ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        if (glyphIndex <= 0)
            return false;  // no .notdef metrics (ImGui's loader returns false there)

        // Em-space control-point bbox, from the same extraction the glyph
        // record is built from (size-independent, so it does not depend on the
        // bake size). Scratch allocation failure fails the whole query, like a
        // record build failure.
        int quadCount = 0;
        if (!ExtractWithScratch(font, glyphIndex, &quadCount))
            return false;
        ImSlugControlBoundsOfQuads(font->ScratchQuads, quadCount, &outMetrics->MinX, &outMetrics->MinY,
                                   &outMetrics->MaxX, &outMetrics->MaxY);

        const float scale = ScaleForPixelHeight1(font) * bakeSizePixels;

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&TtfFont(font)->FontInfo, glyphIndex, scale, scale, 0.0f, 0.0f, &x0, &y0, &x1,
                                        &y1);

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&TtfFont(font)->FontInfo, glyphIndex, &advance, &lsb);

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
            stbtt_GetFontVMetrics(&TtfFont(font)->FontInfo, &ascent, &descent, &lineGap);
            const float ascentOffset = ImSlugImRound(ImSlugImCeil((float)ascent * scale));
            outMetrics->X0 = (float)x0;
            outMetrics->Y0 = (float)y0 + ascentOffset;
            outMetrics->X1 = (float)x1;
            outMetrics->Y1 = (float)y1 + ascentOffset;
        }
        return true;
    }

    int ImSlugTtfGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint)
    {
        IMSLUG_ASSERT(font != nullptr);
        return stbtt_FindGlyphIndex(&TtfFont(font)->FontInfo, (int)codepoint);
    }

    int ImSlugTtfGetGlyphCount(const ImSlugFont* font)
    {
        IMSLUG_ASSERT(font != nullptr);
        return TtfFont(font)->FontInfo.numGlyphs;
    }

    bool ImSlugTtfGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads, int quadCapacity,
                                       int* outCount)
    {
        IMSLUG_ASSERT(font != nullptr && outCount != nullptr);
        *outCount = 0;
        if (glyphIndex <= 0)
            return false;

        int required = 0;
        if (!ImSlugExtractGlyphOutline(&TtfFont(font)->FontInfo, glyphIndex, outQuads, quadCapacity, &required))
        {
            *outCount = required;
            return false;
        }
        *outCount = required;
        return true;
    }

    // The raw per-glyph source metrics the packed-asset writer stores so
    // the loader can reproduce this font's metrics at any bake size without a
    // parser: the glyf box exactly as stbtt_GetGlyphBox reports it (zeros when
    // the glyph has no glyf data — the runtime metrics path treats that as
    // invisible) and the hmtx advance.
    bool ImSlugTtfGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex, ImSlugGlyphSourceMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        outMetrics->AdvanceFu = 0;
        outMetrics->BoxX0 = 0;
        outMetrics->BoxY0 = 0;
        outMetrics->BoxX1 = 0;
        outMetrics->BoxY1 = 0;
        if (glyphIndex <= 0)
            return false;

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (stbtt_GetGlyphBox(&TtfFont(font)->FontInfo, glyphIndex, &x0, &y0, &x1, &y1))
        {
            outMetrics->BoxX0 = x0;
            outMetrics->BoxY0 = y0;
            outMetrics->BoxX1 = x1;
            outMetrics->BoxY1 = y1;
        }

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&TtfFont(font)->FontInfo, glyphIndex, &advance, &lsb);
        outMetrics->AdvanceFu = advance;
        return true;
    }

    // ---------------------------------------------------------------------
    // Glyph records (TTF kind)
    // ---------------------------------------------------------------------
    // The shared band/curve builder lives in imslug_font.cpp
    // (ImSlugWriteGlyphRecordToPools / ...Transactional); this is the
    // TTF-specific wrapper that extracts the outline and the hmtx advance
    // first. The transactional contract: a failed build leaves the pools
    // exactly as it found them, so transient failures can be retried.
    ImSlugBuildStatus ImSlugTtfBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                ImSlugGlyphRecord* record)
    {
        int quadCount = 0;
        if (!ExtractWithScratch(font, glyphIndex, &quadCount))
            return ImSlugBuildStatus_OutOfMemory;  // scratch growth failed; pools
                                                   // untouched

        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&TtfFont(font)->FontInfo, glyphIndex, &advance, &lsb);
        const float advanceEm = (float)advance / (float)font->UnitsPerEm;
        return ImSlugBuildGlyphRecordToPoolsTransactional(font, pools, font->ScratchQuads, quadCount, advanceEm,
                                                          record);
    }
}  // namespace ImSlug
