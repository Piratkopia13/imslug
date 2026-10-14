// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_colr.cpp — COLR/CPAL reader + color-glyph cache. See
// imslug_colr.h for the contract; this file implements:
//
//   1. a bounds-checked SFNT table directory reader (ttcf-aware, using the same
//      font offset stb_truetype resolved);
//   2. CPAL v0/v1 palette access (palette 0; BGRA color records);
//   3. COLR v0 base-glyph -> layer-record lookup;
//   4. COLR v1 paint-graph flattening into a draw-order layer list: transforms
//      are accumulated per layer (baked into outline records when they are not
//      a positive axis-aligned scale+translate), gradients are resolved into
//      CPU-side paint records (stops resolved through CPAL with the COLR alpha
//      factors), ClipBoxes are carried per paint record, and composite modes
//      that reduce to an alpha multiplication are folded into the layer paint;
//   5. the CPU mirror of the fragment shader's paint evaluation
//      (ImSlugSamplePaintRecord), used by tests and the offscreen goldens'
//      expected images.
//
// Attribution: the parsing approach was cross-checked against DearWidgets' CC0
// docs/concepts/slug.md and src/api/dear_widgets_slug.cpp (allowed reference;
// no code copied — the reader, the flattening and the paint math here are
// original) and the OpenType COLR/CPAL specifications. Gradient evaluation
// follows the spec text: the linear gradient's rotation point p2 (the color at
// p is the color of the position on line p0p1 that the line through p and
// parallel to p0p2 intersects), the WHATWG two-point conical algorithm the
// spec references (the largest omega whose circle with r(omega) > 0 passes
// through the point), and the counter-clockwise sweep angle convention.

#include "imslug_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        const float kDegToRad = 0.01745329251994329577f;
        const float kRadToDeg = 57.29577951308232087680f;

        // ---------------------------------------------------------------------
        // Big-endian, bounds-checked primitives
        // ---------------------------------------------------------------------
        bool ReadU16(const uint8_t* data, uint32_t size, uint32_t offset, uint16_t* out)
        {
            if (offset > size || size - offset < 2)
                return false;
            *out = (uint16_t)((data[offset] << 8) | data[offset + 1]);
            return true;
        }

        bool ReadI16(const uint8_t* data, uint32_t size, uint32_t offset, int16_t* out)
        {
            uint16_t raw = 0;
            if (!ReadU16(data, size, offset, &raw))
                return false;
            *out = (int16_t)raw;
            return true;
        }

        bool ReadU32(const uint8_t* data, uint32_t size, uint32_t offset, uint32_t* out)
        {
            if (offset > size || size - offset < 4)
                return false;
            *out = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                   ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];
            return true;
        }

        float F2Dot14ToFloat(uint16_t raw)
        {
            return (float)(int16_t)raw / 16384.0f;
        }

        float Fixed16ToFloat(uint32_t raw)
        {
            return (float)(int32_t)raw / 65536.0f;
        }

        // ---------------------------------------------------------------------
        // SFNT table directory
        // ---------------------------------------------------------------------
        struct SfntTable
        {
            uint32_t Offset;
            uint32_t Length;
            bool Valid;
        };

        bool FindSfntTable(const uint8_t* data, uint32_t size, uint32_t fontOffset, const char tag[4], SfntTable* out)
        {
            out->Offset = 0;
            out->Length = 0;
            out->Valid = false;
            uint16_t numTables = 0;
            if (fontOffset > size || size - fontOffset < 12)
                return false;
            if (!ReadU16(data, size, fontOffset + 4, &numTables) || numTables == 0)
                return false;
            const uint32_t recordBase = fontOffset + 12;
            for (uint16_t i = 0; i < numTables; ++i)
            {
                const uint32_t record = recordBase + (uint32_t)i * 16u;
                if (record > size || size - record < 16)
                    return false;
                if (memcmp(data + record, tag, 4) != 0)
                    continue;
                uint32_t offset = 0;
                uint32_t length = 0;
                if (!ReadU32(data, size, record + 8, &offset) || !ReadU32(data, size, record + 12, &length))
                    return false;
                if (offset > size || length > size - offset)
                    return false;
                out->Offset = offset;
                out->Length = length;
                out->Valid = true;
                return true;
            }
            return false;
        }

        // ---------------------------------------------------------------------
        // Affine helpers (font-unit space; Y up)
        // ---------------------------------------------------------------------
        // (a, b, tx, c, d, ty): x' = a x + b y + tx, y' = c x + d y + ty. The
        // COLR field order is (xx, yx, xy, yy, dx, dy) with
        // x' = xx x + xy y + dx, so a = xx, b = xy, c = yx, d = yy.
        void ComposeAffine(const float outer[6], const float inner[6], float out[6])
        {
            const float oa = outer[0], ob = outer[1], oc = outer[3], od = outer[4];
            const float ia = inner[0], ib = inner[1], itx = inner[2];
            const float ic = inner[3], id = inner[4], ity = inner[5];
            out[0] = oa * ia + ob * ic;
            out[1] = oa * ib + ob * id;
            out[2] = oa * itx + ob * ity + outer[2];
            out[3] = oc * ia + od * ic;
            out[4] = oc * ib + od * id;
            out[5] = oc * itx + od * ity + outer[5];
        }

        bool InvertAffine(const float m[6], float out[6])
        {
            const float det = m[0] * m[4] - m[1] * m[3];
            if (fabsf(det) < 1.0e-20f)
                return false;
            const float invDet = 1.0f / det;
            const float ia = m[4] * invDet;
            const float ib = -m[1] * invDet;
            const float ic = -m[3] * invDet;
            const float id = m[0] * invDet;
            out[0] = ia;
            out[1] = ib;
            out[3] = ic;
            out[4] = id;
            out[2] = -(ia * m[2] + ib * m[5]);
            out[5] = -(ic * m[2] + id * m[5]);
            return true;
        }

        void ApplyAffine(const float m[6], float x, float y, float* outX, float* outY)
        {
            *outX = m[0] * x + m[1] * y + m[2];
            *outY = m[3] * x + m[4] * y + m[5];
        }

        // Positive axis-aligned (scale + translate) within a relative epsilon:
        // reusable with the plain glyph record and a per-axis scale mapping.
        bool IsAxisAlignedPositive(const float m[6], float epsilon)
        {
            const float ax = fabsf(m[0]);
            const float ay = fabsf(m[4]);
            const float scale = (ax > ay ? ax : ay) + 1.0f;
            return fabsf(m[1]) <= epsilon * scale && fabsf(m[3]) <= epsilon * scale && m[0] >= 0.0f && m[4] >= 0.0f;
        }

        // ---------------------------------------------------------------------
        // Paint table access
        // ---------------------------------------------------------------------
        struct PaintTable
        {
            const uint8_t* Base;  // COLR table start
            uint32_t Size;        // COLR table length
            uint32_t Offset;      // paint table offset from the COLR start
        };

        bool ReadOffset24(const PaintTable& paint, uint32_t fieldOffset, uint32_t* out)
        {
            const uint8_t* data = paint.Base;
            const uint32_t at = paint.Offset + fieldOffset;
            if (at > paint.Size || paint.Size - at < 3)
                return false;
            *out = ((uint32_t)data[at] << 16) | ((uint32_t)data[at + 1] << 8) | (uint32_t)data[at + 2];
            return true;
        }

        bool ReadPaintFormat(const PaintTable& paint, uint8_t* out)
        {
            if (paint.Offset >= paint.Size)
                return false;
            *out = paint.Base[paint.Offset];
            return true;
        }

        bool ReadPaintU16(const PaintTable& paint, uint32_t fieldOffset, uint16_t* out)
        {
            return ReadU16(paint.Base, paint.Size, paint.Offset + fieldOffset, out);
        }

        bool ReadPaintI16(const PaintTable& paint, uint32_t fieldOffset, int16_t* out)
        {
            return ReadI16(paint.Base, paint.Size, paint.Offset + fieldOffset, out);
        }

        bool ReadPaintU32(const PaintTable& paint, uint32_t fieldOffset, uint32_t* out)
        {
            return ReadU32(paint.Base, paint.Size, paint.Offset + fieldOffset, out);
        }

        bool ReadPaintF2Dot14(const PaintTable& paint, uint32_t fieldOffset, float* out)
        {
            uint16_t raw = 0;
            if (!ReadPaintU16(paint, fieldOffset, &raw))
                return false;
            *out = F2Dot14ToFloat(raw);
            return true;
        }

        PaintTable ChildPaint(const PaintTable& parent, uint32_t offset)
        {
            PaintTable child = parent;
            child.Offset = parent.Offset + offset;
            return child;
        }

        // ---------------------------------------------------------------------
        // CPAL
        // ---------------------------------------------------------------------
        struct CpalColor
        {
            uint8_t R, G, B, A;
        };

        // ---------------------------------------------------------------------
        // Resolved layer (build scratch)
        // ---------------------------------------------------------------------
        struct ResolvedLayer
        {
            uint16_t GlyphID;
            float M[6];      // paint-local -> base font units (outline transform)
            float FillM[6];  // fill-local -> base font units (paint geometry)
            float AlphaMul;  // constant mask folded in at paint building
            // Fill in *fill-local* font units:
            uint32_t Type;    // ImSlugColorPaintType
            uint32_t Extend;  // ImSlugColorExtend
            uint32_t SolidColor;
            float Geo0[4];
            float Geo1[4];
            uint32_t StopCount;
            uint32_t Stops[kMaxColorStops];  // (offsetF2Dot14 << 16) | rgba8
        };

        struct FlattenState
        {
            ImSlugColorFont* Font;
            ResolvedLayer* Layers;
            int LayerCapacity;
            int LayerCount;
            int NodeCount;
            bool Truncated;
            // PaintColrGlyph re-entry guard: the base glyphs currently being
            // flattened (a cyclic reference is skipped, not re-expanded).
            uint16_t ColrGlyphStack[8];
            int ColrGlyphStackDepth;
        };
    }  // namespace

    // -------------------------------------------------------------------------
    // The opaque per-font color state
    // -------------------------------------------------------------------------
    struct ImSlugColorFont
    {
        const uint8_t* Data;
        uint32_t Size;
        int UnitsPerEm;

        SfntTable Colr;
        SfntTable Cpal;
        bool ColrV0;
        bool ColrV1;

        uint16_t V0BaseRecordCount;
        uint32_t V0BaseRecordsOffset;
        uint32_t V0LayerRecordsOffset;
        uint16_t V0LayerRecordCount;
        uint32_t V1BaseGlyphListOffset;
        uint32_t V1LayerListOffset;
        uint32_t V1ClipListOffset;  // 0 = none

        uint16_t CpalNumPaletteEntries;
        uint16_t CpalNumPalettes;
        uint16_t CpalNumColorRecords;
        uint32_t CpalColorRecordsOffset;
        uint16_t CpalPalette0First;
        bool CpalParsed;  // palette data usable

        // Lazily built color-glyph cache. Entries are individually allocated so
        // layer/record pointers stay stable while more glyphs are built. A
        // cached entry with Status != Ok means "definitively no color glyph".
        struct CachedGlyph
        {
            int GlyphIndex;
            ImSlugBuildStatus Status;
            ImSlugColorGlyph Glyph;
            ImSlugColorLayer* Layers;    // owned (Glyph.Layers points here)
            ImSlugGlyphRecord* Records;  // owned (synthetic records)
        };
        CachedGlyph** Cache;
        int CacheCount;
        int CacheCapacity;
    };

    namespace
    {
        bool GetPaletteColor(const ImSlugColorFont* font, uint16_t paletteIndex, CpalColor* out)
        {
            if (paletteIndex == 0xFFFFu)
            {
                // Text foreground. ImGui forces RGB=255 for Colored glyphs (the
                // pinned v1.92.9b RenderText/RenderChar paths), so the
                // foreground here is white; the COLR alpha factor is applied by
                // the caller.
                out->R = 255;
                out->G = 255;
                out->B = 255;
                out->A = 255;
                return true;
            }
            if (!font->CpalParsed || paletteIndex >= font->CpalNumPaletteEntries)
                return false;
            const uint32_t record = (uint32_t)font->CpalPalette0First + (uint32_t)paletteIndex;
            if (record >= font->CpalNumColorRecords)
                return false;
            const uint8_t* data = font->Data + font->Cpal.Offset;
            const uint32_t offset = font->CpalColorRecordsOffset + record * 4u;
            if (offset > font->Cpal.Length || font->Cpal.Length - offset < 4)
                return false;
            out->B = data[offset + 0];
            out->G = data[offset + 1];
            out->R = data[offset + 2];
            out->A = data[offset + 3];
            return true;
        }

        uint32_t PackCpalColor(const CpalColor& color, float alphaFactor)
        {
            const float a = color.A / 255.0f;
            const float factor = alphaFactor < 0.0f ? 0.0f : (alphaFactor > 1.0f ? 1.0f : alphaFactor);
            return ImSlugPackColorRGBA8((float)color.R / 255.0f, (float)color.G / 255.0f, (float)color.B / 255.0f,
                                        a * factor);
        }

        bool ParseCpal(ImSlugColorFont* font)
        {
            const uint8_t* data = font->Data + font->Cpal.Offset;
            const uint32_t size = font->Cpal.Length;
            uint16_t version = 0;
            if (!ReadU16(data, size, 0, &version) || version > 1)
                return false;
            if (!ReadU16(data, size, 2, &font->CpalNumPaletteEntries) ||
                !ReadU16(data, size, 4, &font->CpalNumPalettes) ||
                !ReadU16(data, size, 6, &font->CpalNumColorRecords) ||
                !ReadU32(data, size, 8, &font->CpalColorRecordsOffset))
                return false;
            if (font->CpalNumPalettes == 0 || font->CpalNumColorRecords == 0)
                return false;
            // Palette 0's first color-record index follows the fixed header.
            if (!ReadU16(data, size, 12, &font->CpalPalette0First))
                return false;
            if (font->CpalColorRecordsOffset > size ||
                (uint32_t)font->CpalNumColorRecords * 4u > size - font->CpalColorRecordsOffset)
                return false;
            font->CpalParsed = true;
            return true;
        }

        bool ParseColr(ImSlugColorFont* font)
        {
            const uint8_t* data = font->Data + font->Colr.Offset;
            const uint32_t size = font->Colr.Length;
            uint16_t version = 0;
            if (!ReadU16(data, size, 0, &version))
                return false;

            if (version == 0)
            {
                font->ColrV0 = true;
                if (!ReadU16(data, size, 2, &font->V0BaseRecordCount) ||
                    !ReadU32(data, size, 4, &font->V0BaseRecordsOffset) ||
                    !ReadU32(data, size, 8, &font->V0LayerRecordsOffset) ||
                    !ReadU16(data, size, 12, &font->V0LayerRecordCount))
                    return false;
                if ((uint32_t)font->V0BaseRecordCount * 6u > size || (uint32_t)font->V0LayerRecordCount * 4u > size)
                    return false;
                return true;
            }

            if (version != 1)
                return false;

            font->ColrV1 = true;
            uint32_t baseGlyphList = 0;
            uint32_t layerList = 0;
            uint32_t clipList = 0;
            if (!ReadU32(data, size, 14, &baseGlyphList) || !ReadU32(data, size, 18, &layerList) ||
                !ReadU32(data, size, 22, &clipList))
                return false;
            if (baseGlyphList == 0 || baseGlyphList >= size || layerList == 0 || layerList >= size)
                return false;
            font->V1BaseGlyphListOffset = baseGlyphList;
            font->V1LayerListOffset = layerList;
            font->V1ClipListOffset = (clipList != 0 && clipList < size) ? clipList : 0;

            // A v1 table may still carry a v0 base-glyph array for glyphs that
            // are absent from the v1 list (0xFFFF = "no v0 records").
            uint16_t v0Count = 0;
            uint32_t v0BaseOffset = 0;
            if (ReadU16(data, size, 2, &v0Count) && v0Count != 0xFFFFu && v0Count != 0 &&
                ReadU32(data, size, 4, &v0BaseOffset))
            {
                uint32_t v0LayerOffset = 0;
                uint16_t v0LayerCount = 0;
                if (ReadU32(data, size, 8, &v0LayerOffset) && ReadU16(data, size, 12, &v0LayerCount))
                {
                    font->V0BaseRecordCount = v0Count;
                    font->V0BaseRecordsOffset = v0BaseOffset;
                    font->V0LayerRecordsOffset = v0LayerOffset;
                    font->V0LayerRecordCount = v0LayerCount;
                    font->ColrV0 = true;
                }
            }
            return true;
        }

        void DestroyColorFont(ImSlugColorFont* font)
        {
            if (font == nullptr)
                return;
            for (int i = 0; i < font->CacheCount; ++i)
            {
                free(font->Cache[i]->Layers);
                free(font->Cache[i]->Records);
                free(font->Cache[i]);
            }
            free(font->Cache);
            free(font);
        }

        // ---------------------------------------------------------------------
        // COLR lookups
        // ---------------------------------------------------------------------
        bool FindV0BaseRecord(const ImSlugColorFont* font, uint16_t glyphID, uint16_t* outFirstLayer,
                              uint16_t* outLayerCount)
        {
            if (!font->ColrV0)
                return false;
            const uint8_t* data = font->Data + font->Colr.Offset;
            const uint32_t size = font->Colr.Length;
            uint32_t lo = 0;
            uint32_t hi = font->V0BaseRecordCount;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2u;
                const uint32_t record = font->V0BaseRecordsOffset + mid * 6u;
                uint16_t recordGlyph = 0;
                if (!ReadU16(data, size, record, &recordGlyph))
                    return false;
                if (recordGlyph == glyphID)
                {
                    if (!ReadU16(data, size, record + 2, outFirstLayer) ||
                        !ReadU16(data, size, record + 4, outLayerCount))
                        return false;
                    return true;
                }
                if (recordGlyph < glyphID)
                    lo = mid + 1u;
                else
                    hi = mid;
            }
            return false;
        }

        // Returns the paint offset (from the COLR start) of a glyph's v1 paint
        // graph, or false when the glyph has none.
        bool FindV1BasePaint(const ImSlugColorFont* font, uint16_t glyphID, uint32_t* outPaintOffset)
        {
            if (!font->ColrV1)
                return false;
            const uint8_t* data = font->Data + font->Colr.Offset;
            const uint32_t size = font->Colr.Length;
            const uint32_t list = font->V1BaseGlyphListOffset;
            uint32_t count = 0;
            if (!ReadU32(data, size, list, &count))
                return false;
            // Records: {glyphID u16, paintOffset u32 (from the list start)}.
            if ((uint64_t)count * 6u + 4u > (uint64_t)size)
                return false;
            uint32_t lo = 0;
            uint32_t hi = count;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2u;
                const uint32_t record = list + 4u + mid * 6u;
                uint16_t recordGlyph = 0;
                if (!ReadU16(data, size, record, &recordGlyph))
                    return false;
                if (recordGlyph == glyphID)
                {
                    uint32_t paintOffset = 0;
                    if (!ReadU32(data, size, record + 2, &paintOffset))
                        return false;
                    // The offset is relative to the BaseGlyphList start.
                    if (paintOffset >= size - list)
                        return false;
                    *outPaintOffset = list + paintOffset;
                    return true;
                }
                if (recordGlyph < glyphID)
                    lo = mid + 1u;
                else
                    hi = mid;
            }
            return false;
        }

        bool ResolveClipBox(const ImSlugColorFont* font, uint16_t glyphID, bool* outValid, float* outMinX,
                            float* outMinY, float* outMaxX, float* outMaxY)
        {
            *outValid = false;
            *outMinX = *outMinY = *outMaxX = *outMaxY = 0.0f;
            if (font->V1ClipListOffset == 0)
                return true;
            const uint8_t* data = font->Data + font->Colr.Offset;
            const uint32_t size = font->Colr.Length;
            const uint32_t list = font->V1ClipListOffset;
            // ClipList format 1: uint8 format, uint32 clipCount, then
            // ClipRecord[clipCount] = {startGlyphID u16, endGlyphID u16,
            // clipBoxOffset u24 (from the ClipList start)} - sorted ranges.
            if (list >= size || data[list] != 1)
                return true;  // unknown format: no clip (graceful)
            uint32_t count = 0;
            if (!ReadU32(data, size, list + 1, &count))
                return false;
            if ((uint64_t)count * 7u + 5u > (uint64_t)size)
                return false;
            uint32_t lo = 0;
            uint32_t hi = count;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2u;
                const uint32_t record = list + 5u + mid * 7u;
                uint16_t startGlyph = 0;
                uint16_t endGlyph = 0;
                if (!ReadU16(data, size, record, &startGlyph) || !ReadU16(data, size, record + 2, &endGlyph))
                    return false;
                if (glyphID < startGlyph)
                {
                    hi = mid;
                    continue;
                }
                if (glyphID > endGlyph)
                {
                    lo = mid + 1u;
                    continue;
                }
                const uint32_t boxOffset =
                    ((uint32_t)data[record + 4] << 16) | ((uint32_t)data[record + 5] << 8) | (uint32_t)data[record + 6];
                const uint32_t box = list + boxOffset;
                if (box >= size)
                    return false;
                const uint8_t format = data[box];
                if (format != 1 && format != 2)
                    return true;  // unknown clip format: no clip (graceful)
                int16_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;
                if (!ReadI16(data, size, box + 1, &xMin) || !ReadI16(data, size, box + 3, &yMin) ||
                    !ReadI16(data, size, box + 5, &xMax) || !ReadI16(data, size, box + 7, &yMax))
                    return false;
                *outValid = true;
                *outMinX = (float)xMin;
                *outMinY = (float)yMin;
                *outMaxX = (float)xMax;
                *outMaxY = (float)yMax;
                return true;
            }
            return true;
        }

        // ---------------------------------------------------------------------
        // Transform nodes
        // ---------------------------------------------------------------------
        // Reads the local transform of a transform-format paint (12..31) and
        // the child paint offset. Returns false for other formats / bad tables.
        bool ReadTransformNode(const PaintTable& paint, uint8_t format, float local[6], uint32_t* outChild)
        {
            local[0] = 1.0f;
            local[1] = 0.0f;
            local[2] = 0.0f;
            local[3] = 0.0f;
            local[4] = 1.0f;
            local[5] = 0.0f;
            switch (format)
            {
            case 12:
            case 13:  // PaintTransform / PaintVarTransform
            {
                uint32_t transformOffset = 0;
                if (!ReadOffset24(paint, 1, outChild) || !ReadOffset24(paint, 4, &transformOffset))
                    return false;
                const uint32_t base = paint.Offset + transformOffset;
                float xx = 1, yx = 0, xy = 0, yy = 1, dx = 0, dy = 0;
                uint32_t raw = 0;
                // Affine2x3 field order: xx, yx, xy, yy, dx, dy.
                const uint32_t fields[6] = { 0, 4, 8, 12, 16, 20 };
                float* values[6] = { &xx, &yx, &xy, &yy, &dx, &dy };
                for (int i = 0; i < 6; ++i)
                {
                    if (!ReadU32(paint.Base, paint.Size, base + fields[i], &raw))
                        return false;
                    *values[i] = Fixed16ToFloat(raw);
                }
                local[0] = xx;
                local[1] = xy;
                local[2] = dx;
                local[3] = yx;
                local[4] = yy;
                local[5] = dy;
                return true;
            }
            case 14:
            case 15:  // PaintTranslate: format u8, paintOffset u24, dx i16 (4), dy i16 (6)
            {
                int16_t dx = 0, dy = 0;
                if (!ReadOffset24(paint, 1, outChild) || !ReadPaintI16(paint, 4, &dx) || !ReadPaintI16(paint, 6, &dy))
                    return false;
                local[2] = (float)dx;
                local[5] = (float)dy;
                return true;
            }
            case 16:
            case 17:  // PaintScale
            case 18:
            case 19:  // PaintScaleAroundCenter
            case 20:
            case 21:  // PaintScaleUniform
            case 22:
            case 23:  // PaintScaleUniformAroundCenter
            {
                float sx = 1.0f, sy = 1.0f;
                float cx = 0.0f, cy = 0.0f;
                if (format == 16 || format == 17)
                {
                    if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &sx) ||
                        !ReadPaintF2Dot14(paint, 6, &sy))
                        return false;
                }
                else if (format == 18 || format == 19)
                {
                    int16_t cxRaw = 0, cyRaw = 0;
                    if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &sx) ||
                        !ReadPaintF2Dot14(paint, 6, &sy) || !ReadPaintI16(paint, 8, &cxRaw) ||
                        !ReadPaintI16(paint, 10, &cyRaw))
                        return false;
                    cx = (float)cxRaw;
                    cy = (float)cyRaw;
                }
                else if (format == 20 || format == 21)
                {
                    if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &sx))
                        return false;
                    sy = sx;
                }
                else  // 22/23
                {
                    int16_t cxRaw = 0, cyRaw = 0;
                    if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &sx) ||
                        !ReadPaintI16(paint, 6, &cxRaw) || !ReadPaintI16(paint, 8, &cyRaw))
                        return false;
                    sy = sx;
                    cx = (float)cxRaw;
                    cy = (float)cyRaw;
                }
                local[0] = sx;
                local[4] = sy;
                local[2] = cx - sx * cx;
                local[5] = cy - sy * cy;
                return true;
            }
            case 24:
            case 25:  // PaintRotate
            case 26:
            case 27:  // PaintRotateAroundCenter
            {
                float angle = 0.0f;
                float cx = 0.0f, cy = 0.0f;
                if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &angle))
                    return false;
                if (format == 26 || format == 27)
                {
                    int16_t cxRaw = 0, cyRaw = 0;
                    if (!ReadPaintI16(paint, 6, &cxRaw) || !ReadPaintI16(paint, 8, &cyRaw))
                        return false;
                    cx = (float)cxRaw;
                    cy = (float)cyRaw;
                }
                const float radians = angle * 180.0f * kDegToRad;
                const float cs = cosf(radians);
                const float sn = sinf(radians);
                // Counter-clockwise rotation in the Y-up design grid:
                // x' = cos x - sin y, y' = sin x + cos y.
                local[0] = cs;
                local[1] = -sn;
                local[3] = sn;
                local[4] = cs;
                local[2] = cx - cs * cx + sn * cy;
                local[5] = cy - sn * cx - cs * cy;
                return true;
            }
            default:  // 28..31: PaintSkew / PaintSkewAroundCenter
            {
                float xSkew = 0.0f, ySkew = 0.0f;
                float cx = 0.0f, cy = 0.0f;
                if (!ReadOffset24(paint, 1, outChild) || !ReadPaintF2Dot14(paint, 4, &xSkew) ||
                    !ReadPaintF2Dot14(paint, 6, &ySkew))
                    return false;
                if (format == 30 || format == 31)
                {
                    int16_t cxRaw = 0, cyRaw = 0;
                    if (!ReadPaintI16(paint, 8, &cxRaw) || !ReadPaintI16(paint, 10, &cyRaw))
                        return false;
                    cx = (float)cxRaw;
                    cy = (float)cyRaw;
                }
                const float tanX = tanf(xSkew * 180.0f * kDegToRad);
                const float tanY = tanf(ySkew * 180.0f * kDegToRad);
                local[1] = tanX;
                local[3] = tanY;
                local[2] = -tanX * cy;
                local[5] = -tanY * cx;
                return true;
            }
            }
        }

        // ---------------------------------------------------------------------
        // Paint graph flattening
        // ---------------------------------------------------------------------
        void ResetLayer(ResolvedLayer* layer)
        {
            memset(layer, 0, sizeof(*layer));
            layer->AlphaMul = 1.0f;
            layer->M[0] = 1.0f;
            layer->M[4] = 1.0f;
            layer->FillM[0] = 1.0f;
            layer->FillM[4] = 1.0f;
        }

        // True when the subtree paints the whole plane with a uniform color
        // (a PaintSolid reached only through transform wrappers). Its alpha is
        // what src_in / src_out / dest_in / dest_out multiply with.
        bool TryConstantPaintAlpha(const ImSlugColorFont* font, const PaintTable& paint, int depth, float* outAlpha)
        {
            if (depth > kMaxColorGraphDepth)
                return false;
            uint8_t format = 0;
            if (!ReadPaintFormat(paint, &format))
                return false;

            if (format == 2 || format == 3)  // PaintSolid / PaintVarSolid
            {
                uint16_t paletteIndex = 0;
                if (!ReadPaintU16(paint, 1, &paletteIndex))
                    return false;
                float alpha = 1.0f;
                if (!ReadPaintF2Dot14(paint, 3, &alpha))
                    return false;
                CpalColor color = { 0, 0, 0, 0 };
                if (!GetPaletteColor(font, paletteIndex, &color))
                    return false;
                alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
                *outAlpha = ((float)color.A / 255.0f) * alpha;
                return true;
            }

            // Transform wrappers keep a full-plane constant constant.
            if (format >= 12 && format <= 31)
            {
                float local[6];
                uint32_t child = 0;
                if (!ReadTransformNode(paint, format, local, &child))
                    return false;
                return TryConstantPaintAlpha(font, ChildPaint(paint, child), depth + 1, outAlpha);
            }
            return false;
        }

        // Resolves the fill of a PaintGlyph: the fill subtree's transforms are
        // composed from `baseM` into outFillM, and one of the leaf fill types
        // (solid / gradients) is written into `layer`. Returns false for
        // unsupported subtrees (the layer is skipped).
        bool ResolveFill(const ImSlugColorFont* font, const PaintTable& paint, const float baseM[6], int depth,
                         float outFillM[6], ResolvedLayer* layer)
        {
            if (depth > kMaxColorGraphDepth)
                return false;
            uint8_t format = 0;
            if (!ReadPaintFormat(paint, &format))
                return false;

            if (format == 2 || format == 3)  // solid
            {
                uint16_t paletteIndex = 0;
                if (!ReadPaintU16(paint, 1, &paletteIndex))
                    return false;
                float alpha = 1.0f;
                if (!ReadPaintF2Dot14(paint, 3, &alpha))
                    return false;
                CpalColor color = { 0, 0, 0, 0 };
                if (!GetPaletteColor(font, paletteIndex, &color))
                    return false;
                layer->Type = ImSlugColorPaintType_Solid;
                layer->StopCount = 1;
                layer->SolidColor = PackCpalColor(color, alpha);
                memcpy(outFillM, baseM, sizeof(float) * 6);
                return true;
            }

            if (format >= 4 && format <= 9)  // gradients (+ var variants)
            {
                uint32_t colorLineOffset = 0;
                if (!ReadOffset24(paint, 1, &colorLineOffset))
                    return false;
                const uint32_t colorLine = paint.Offset + colorLineOffset;
                if (colorLine > paint.Size || paint.Size - colorLine < 3)
                    return false;
                uint32_t extendRaw = paint.Base[colorLine];
                if (extendRaw > 2)
                    extendRaw = ImSlugColorExtend_Pad;
                uint16_t stopCount = 0;
                if (!ReadU16(paint.Base, paint.Size, colorLine + 1, &stopCount) || stopCount == 0)
                    return false;
                uint32_t count = stopCount;
                if (count > (uint32_t)kMaxColorStops)
                    count = (uint32_t)kMaxColorStops;  // documented truncation
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t stop = colorLine + 3u + i * 6u;
                    uint16_t rawOffset = 0;
                    uint16_t paletteIndex = 0;
                    uint16_t rawAlpha = 0;
                    if (!ReadU16(paint.Base, paint.Size, stop + 0, &rawOffset) ||
                        !ReadU16(paint.Base, paint.Size, stop + 2, &paletteIndex) ||
                        !ReadU16(paint.Base, paint.Size, stop + 4, &rawAlpha))
                        return false;
                    CpalColor color = { 0, 0, 0, 0 };
                    if (!GetPaletteColor(font, paletteIndex, &color))
                        return false;
                    layer->Stops[2 * i] = (uint32_t)rawOffset;
                    layer->Stops[2 * i + 1] = PackCpalColor(color, F2Dot14ToFloat(rawAlpha));
                }
                layer->StopCount = count;
                layer->Extend = extendRaw;

                int16_t v0 = 0, v1 = 0, v2 = 0, v3 = 0, v4 = 0, v5 = 0;
                if (format == 4 || format == 5)
                {
                    layer->Type = ImSlugColorPaintType_LinearGradient;
                    if (!ReadPaintI16(paint, 4, &v0) || !ReadPaintI16(paint, 6, &v1) || !ReadPaintI16(paint, 8, &v2) ||
                        !ReadPaintI16(paint, 10, &v3) || !ReadPaintI16(paint, 12, &v4) || !ReadPaintI16(paint, 14, &v5))
                        return false;
                    layer->Geo0[0] = (float)v0;
                    layer->Geo0[1] = (float)v1;
                    layer->Geo0[2] = (float)v2;
                    layer->Geo0[3] = (float)v3;
                    layer->Geo1[0] = (float)v4;
                    layer->Geo1[1] = (float)v5;
                }
                else if (format == 6 || format == 7)
                {
                    layer->Type = ImSlugColorPaintType_RadialGradient;
                    uint16_t r0 = 0;
                    uint16_t r1 = 0;
                    if (!ReadPaintI16(paint, 4, &v0) || !ReadPaintI16(paint, 6, &v1) ||
                        !ReadU16(paint.Base, paint.Size, paint.Offset + 8, &r0) || !ReadPaintI16(paint, 10, &v2) ||
                        !ReadPaintI16(paint, 12, &v3) || !ReadU16(paint.Base, paint.Size, paint.Offset + 14, &r1))
                        return false;
                    layer->Geo0[0] = (float)v0;
                    layer->Geo0[1] = (float)v1;
                    layer->Geo0[2] = (float)r0;
                    layer->Geo1[0] = (float)v2;
                    layer->Geo1[1] = (float)v3;
                    layer->Geo1[2] = (float)r1;
                }
                else  // 8/9: sweep
                {
                    layer->Type = ImSlugColorPaintType_SweepGradient;
                    float startAngle = 0.0f;
                    float endAngle = 0.0f;
                    if (!ReadPaintI16(paint, 4, &v0) || !ReadPaintI16(paint, 6, &v1) ||
                        !ReadPaintF2Dot14(paint, 8, &startAngle) || !ReadPaintF2Dot14(paint, 10, &endAngle))
                        return false;
                    // The spec's BiasedAngle: (raw + 1) * 180 degrees.
                    layer->Geo0[0] = (float)v0;
                    layer->Geo0[1] = (float)v1;
                    layer->Geo0[2] = startAngle * 180.0f + 180.0f;
                    layer->Geo0[3] = endAngle * 180.0f + 180.0f;
                }
                memcpy(outFillM, baseM, sizeof(float) * 6);
                return true;
            }

            // Fill subtrees that are transform wrappers: compose and recurse.
            if (format >= 12 && format <= 31)
            {
                float local[6];
                uint32_t child = 0;
                if (!ReadTransformNode(paint, format, local, &child))
                    return false;
                float combined[6];
                ComposeAffine(baseM, local, combined);
                return ResolveFill(font, ChildPaint(paint, child), combined, depth + 1, outFillM, layer);
            }

            // Any other fill (layers, glyphs, composites) is unsupported as a
            // fill: the layer is skipped (documented fallback).
            return false;
        }

        // Emits one layer for a PaintGlyph, or skips it when its fill cannot be
        // represented.
        void EmitGlyphLayer(ImSlugColorFont* font, const PaintTable& paint, const float m[6], float alphaMul,
                            FlattenState* state)
        {
            // PaintGlyph: format u8, paintOffset u24, glyphID u16 (offset 4).
            uint32_t fillOffset = 0;
            uint16_t glyphID = 0;
            if (!ReadOffset24(paint, 1, &fillOffset) || !ReadPaintU16(paint, 4, &glyphID))
                return;
            if (glyphID == 0)
                return;  // .notdef as a layer is a font bug; skip
            if (state->LayerCount >= state->LayerCapacity)
            {
                state->Truncated = true;
                return;
            }

            ResolvedLayer layer;
            ResetLayer(&layer);
            layer.GlyphID = glyphID;
            memcpy(layer.M, m, sizeof(layer.M));
            layer.AlphaMul = alphaMul;

            float fillM[6];
            if (!ResolveFill(font, ChildPaint(paint, fillOffset), m, 0, fillM, &layer))
                return;
            memcpy(layer.FillM, fillM, sizeof(layer.FillM));

            state->Layers[state->LayerCount++] = layer;
        }

        void FlattenPaint(ImSlugColorFont* font, const PaintTable& paint, const float m[6], float alphaMul, int depth,
                          int colorGlyphDepth, FlattenState* state)
        {
            if (depth > kMaxColorGraphDepth || ++state->NodeCount > kMaxColorGraphNodes)
            {
                state->Truncated = true;
                return;
            }

            uint8_t format = 0;
            if (!ReadPaintFormat(paint, &format))
                return;

            if (format == 1)  // PaintColrLayers
            {
                // uint8 format, uint8 numLayers, uint32 firstLayerIndex.
                if (paint.Offset + 6u > paint.Size)
                    return;
                const uint8_t numLayers = paint.Base[paint.Offset + 1];
                uint32_t firstLayer = 0;
                if (!ReadPaintU32(paint, 2, &firstLayer))
                    return;
                const uint8_t* data = paint.Base;
                const uint32_t size = paint.Size;
                const uint32_t layerList = font->V1LayerListOffset;
                uint32_t layerCount = 0;
                if (!ReadU32(data, size, layerList, &layerCount))
                    return;
                for (uint32_t i = 0; i < (uint32_t)numLayers; ++i)
                {
                    const uint32_t index = firstLayer + i;
                    if (index >= layerCount)
                    {
                        state->Truncated = true;
                        break;
                    }
                    uint32_t offset = 0;
                    if (!ReadU32(data, size, layerList + 4u + index * 4u, &offset))
                        break;
                    // LayerList entries are offsets from the LayerList start.
                    if (offset >= size - layerList)
                        break;
                    PaintTable child;
                    child.Base = data;
                    child.Size = size;
                    child.Offset = layerList + offset;
                    // Layer-list entries are in the current coordinate system.
                    FlattenPaint(font, child, m, alphaMul, depth + 1, colorGlyphDepth, state);
                }
                return;
            }

            if (format == 10)  // PaintGlyph
            {
                EmitGlyphLayer(font, paint, m, alphaMul, state);
                return;
            }

            if (format == 11)  // PaintColrGlyph
            {
                uint16_t glyphID = 0;
                if (!ReadPaintU16(paint, 1, &glyphID))
                    return;
                for (int i = 0; i < state->ColrGlyphStackDepth; ++i)
                {
                    if (state->ColrGlyphStack[i] == glyphID)
                    {
                        state->Truncated = true;  // cyclic reference: skip
                        return;
                    }
                }
                if (state->ColrGlyphStackDepth >= 8)
                {
                    state->Truncated = true;
                    return;
                }
                uint32_t paintOffset = 0;
                if (!FindV1BasePaint(font, glyphID, &paintOffset))
                    return;  // dangling reference: skip
                PaintTable child;
                child.Base = paint.Base;
                child.Size = paint.Size;
                child.Offset = paintOffset;
                state->ColrGlyphStack[state->ColrGlyphStackDepth++] = glyphID;
                FlattenPaint(font, child, m, alphaMul, depth + 1, colorGlyphDepth + 1, state);
                --state->ColrGlyphStackDepth;
                return;
            }

            if (format == 32)  // PaintComposite
            {
                uint32_t sourceOffset = 0;
                uint32_t backdropOffset = 0;
                if (!ReadOffset24(paint, 1, &sourceOffset) || paint.Offset + 5u > paint.Size)
                    return;
                const uint8_t mode = paint.Base[paint.Offset + 4];
                if (!ReadOffset24(paint, 5, &backdropOffset))
                    return;
                const PaintTable source = ChildPaint(paint, sourceOffset);
                const PaintTable backdrop = ChildPaint(paint, backdropOffset);

                switch (mode)
                {
                case 1:  // COMPOSITE_SRC: source only
                    FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    return;
                case 2:  // COMPOSITE_DEST: backdrop only
                    FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    return;
                case 3:  // COMPOSITE_SRC_OVER: backdrop then source
                    FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    return;
                case 4:  // COMPOSITE_DEST_OVER: source then backdrop
                    FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    return;
                case 5:  // COMPOSITE_SRC_IN: source scaled by the backdrop alpha
                {
                    float backdropAlpha = 0.0f;
                    if (TryConstantPaintAlpha(font, backdrop, 0, &backdropAlpha))
                        FlattenPaint(font, source, m, alphaMul * backdropAlpha, depth + 1, colorGlyphDepth, state);
                    else
                    {
                        // Documented approximation (shape-mask backdrop): the
                        // backdrop is drawn first, then the source over it.
                        FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                        FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    }
                    return;
                }
                case 6:  // COMPOSITE_DEST_IN
                {
                    float sourceAlpha = 0.0f;
                    if (TryConstantPaintAlpha(font, source, 0, &sourceAlpha))
                        FlattenPaint(font, backdrop, m, alphaMul * sourceAlpha, depth + 1, colorGlyphDepth, state);
                    else
                    {
                        FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                        FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    }
                    return;
                }
                case 7:  // COMPOSITE_SRC_OUT
                {
                    float backdropAlpha = 0.0f;
                    if (TryConstantPaintAlpha(font, backdrop, 0, &backdropAlpha))
                        FlattenPaint(font, source, m, alphaMul * (1.0f - backdropAlpha), depth + 1, colorGlyphDepth,
                                     state);
                    else
                    {
                        FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                        FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    }
                    return;
                }
                case 8:  // COMPOSITE_DEST_OUT
                {
                    float sourceAlpha = 0.0f;
                    if (TryConstantPaintAlpha(font, source, 0, &sourceAlpha))
                        FlattenPaint(font, backdrop, m, alphaMul * (1.0f - sourceAlpha), depth + 1, colorGlyphDepth,
                                     state);
                    else
                    {
                        FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                        FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    }
                    return;
                }
                default:
                    // SRC_ATOP/DEST_ATOP/XOR and every blend mode (PLUS,
                    // SCREEN, MULTIPLY, SOFT_LIGHT, ...): approximated as
                    // source-over (documented in imslug_colr.h; every composite
                    // in the two license-vetted test fonts either reduces to a
                    // constant mask above or renders correctly in this order).
                    FlattenPaint(font, backdrop, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    FlattenPaint(font, source, m, alphaMul, depth + 1, colorGlyphDepth, state);
                    return;
                }
            }

            if (format >= 12 && format <= 31)  // transform wrappers
            {
                float local[6];
                uint32_t child = 0;
                if (!ReadTransformNode(paint, format, local, &child))
                    return;
                float combined[6];
                ComposeAffine(m, local, combined);
                FlattenPaint(font, ChildPaint(paint, child), combined, alphaMul, depth + 1, colorGlyphDepth, state);
                return;
            }

            // Formats 2..9 (fills without shapes) and unknown formats: nothing
            // to draw at this level.
        }
    }  // namespace

    ImSlugColorFont* ImSlugColorFontCreate(const void* fontData, int fontDataSize, int fontOffset, int unitsPerEm)
    {
        if (fontData == nullptr || fontDataSize <= 0 || fontOffset < 0 || unitsPerEm <= 0)
            return nullptr;
        ImSlugColorFont* font = (ImSlugColorFont*)malloc(sizeof(ImSlugColorFont));
        if (font == nullptr)
            return nullptr;
        memset(font, 0, sizeof(*font));
        font->Data = (const uint8_t*)fontData;
        font->Size = (uint32_t)fontDataSize;
        font->UnitsPerEm = unitsPerEm;

        const char kColrTag[4] = { 'C', 'O', 'L', 'R' };
        const char kCpalTag[4] = { 'C', 'P', 'A', 'L' };
        if (!FindSfntTable(font->Data, font->Size, (uint32_t)fontOffset, kColrTag, &font->Colr) ||
            !FindSfntTable(font->Data, font->Size, (uint32_t)fontOffset, kCpalTag, &font->Cpal))
        {
            DestroyColorFont(font);
            return nullptr;
        }
        if (!ParseColr(font) || !ParseCpal(font))
        {
            DestroyColorFont(font);
            return nullptr;
        }
        return font;
    }

    void ImSlugColorFontDestroy(ImSlugColorFont* colorFont)
    {
        DestroyColorFont(colorFont);
    }

    bool ImSlugColorFontHasGlyph(const ImSlugColorFont* colorFont, int glyphIndex)
    {
        if (colorFont == nullptr || glyphIndex <= 0 || glyphIndex > 0xFFFF)
            return false;
        const uint16_t glyphID = (uint16_t)glyphIndex;
        uint32_t paintOffset = 0;
        if (FindV1BasePaint(colorFont, glyphID, &paintOffset))
            return true;
        uint16_t firstLayer = 0, layerCount = 0;
        if (FindV0BaseRecord(colorFont, glyphID, &firstLayer, &layerCount))
            return layerCount != 0;
        return false;
    }

    void ImSlugColorFontGetStats(const ImSlugColorFont* colorFont, ImSlugColorFontStats* outStats)
    {
        if (outStats == nullptr)
            return;
        memset(outStats, 0, sizeof(*outStats));
        if (colorFont == nullptr)
            return;
        outStats->HasColrV0 = colorFont->ColrV0 ? 1 : 0;
        outStats->HasColrV1 = colorFont->ColrV1 ? 1 : 0;
        outStats->HasClipList = colorFont->V1ClipListOffset != 0 ? 1 : 0;
        outStats->NumPalettes = colorFont->CpalNumPalettes;
        outStats->NumPaletteEntries = colorFont->CpalNumPaletteEntries;
    }

    uint32_t ImSlugPackColorRGBA8(float r, float g, float b, float a)
    {
        const float values[4] = { r, g, b, a };
        uint32_t packed = 0;
        for (int i = 0; i < 4; ++i)
        {
            float value = values[i];
            value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            const uint32_t byte = (uint32_t)(value * 255.0f + 0.5f);
            packed |= byte << (i * 8);
        }
        return packed;
    }

    // -------------------------------------------------------------------------
    // CPU paint evaluation (mirror of the fragment shader's paint code)
    // -------------------------------------------------------------------------
    namespace
    {
        float ExtendParameter(float t, uint32_t extend)
        {
            if (extend == ImSlugColorExtend_Repeat)
                return t - floorf(t);
            if (extend == ImSlugColorExtend_Reflect)
            {
                const float half = t * 0.5f;
                return 1.0f - fabsf((half - floorf(half)) * 2.0f - 1.0f);
            }
            return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

        float StopOffset(uint32_t packedOffset)
        {
            return F2Dot14ToFloat((uint16_t)(packedOffset & 0xFFFFu));
        }

        void UnpackColor(uint32_t packed, float* outR, float* outG, float* outB, float* outA)
        {
            *outR = (float)(packed & 0xFFu) * (1.0f / 255.0f);
            *outG = (float)((packed >> 8) & 0xFFu) * (1.0f / 255.0f);
            *outB = (float)((packed >> 16) & 0xFFu) * (1.0f / 255.0f);
            *outA = (float)((packed >> 24) & 0xFFu) * (1.0f / 255.0f);
        }

        // Samples the color line at parameter t (t already extended).
        void SampleColorLine(const ImSlugPaintRecord* paint, float t, float* outRgba)
        {
            const uint32_t count = paint->StopCount;
            if (count == 0)
            {
                outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 0.0f;
                return;
            }
            uint32_t upper = 1;
            while (upper < count && StopOffset(paint->Stops[2 * upper]) < t)
                ++upper;
            if (upper >= count)
            {
                UnpackColor(paint->Stops[2 * (count - 1) + 1], &outRgba[0], &outRgba[1], &outRgba[2], &outRgba[3]);
                return;
            }
            const uint32_t lower = upper - 1;
            const float t0 = StopOffset(paint->Stops[2 * lower]);
            const float t1 = StopOffset(paint->Stops[2 * upper]);
            float f = 0.0f;
            if (t1 > t0)
                f = (t - t0) / (t1 - t0);
            f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
            float r0, g0, b0, a0, r1, g1, b1, a1;
            UnpackColor(paint->Stops[2 * lower + 1], &r0, &g0, &b0, &a0);
            UnpackColor(paint->Stops[2 * upper + 1], &r1, &g1, &b1, &a1);
            outRgba[0] = r0 + (r1 - r0) * f;
            outRgba[1] = g0 + (g1 - g0) * f;
            outRgba[2] = b0 + (b1 - b0) * f;
            outRgba[3] = a0 + (a1 - a0) * f;
        }
    }  // namespace

    void ImSlugSamplePaintRecord(const ImSlugPaintRecord* paint, float emX, float emY, float outRgba[4])
    {
        outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 0.0f;
        if (paint == nullptr)
            return;

        // ClipBox (record/base em space). Disabled when max <= min.
        if (paint->Clip[2] > paint->Clip[0] && paint->Clip[3] > paint->Clip[1])
        {
            if (emX < paint->Clip[0] || emX > paint->Clip[2] || emY < paint->Clip[1] || emY > paint->Clip[3])
                return;
        }

        if (paint->Type == ImSlugColorPaintType_Solid)
        {
            UnpackColor(paint->SolidColor, &outRgba[0], &outRgba[1], &outRgba[2], &outRgba[3]);
            return;
        }

        // Paint-space coordinate.
        const float localX = paint->Xform0[0] * emX + paint->Xform0[1] * emY + paint->Xform0[2];
        const float localY = paint->Xform1[0] * emX + paint->Xform1[1] * emY + paint->Xform1[2];

        float t = 0.0f;
        if (paint->Type == ImSlugColorPaintType_LinearGradient)
        {
            const float p0x = paint->Geo0[0], p0y = paint->Geo0[1];
            const float p1x = paint->Geo0[2], p1y = paint->Geo0[3];
            const float p2x = paint->Geo1[0], p2y = paint->Geo1[1];
            const float e1x = p1x - p0x, e1y = p1y - p0y;
            const float e2x = p2x - p0x, e2y = p2y - p0y;
            const float cross = e1x * e2y - e1y * e2x;
            if ((e1x == 0.0f && e1y == 0.0f) || (e2x == 0.0f && e2y == 0.0f) ||
                fabsf(cross) < 1.0e-12f * (fabsf(e1x) + fabsf(e1y)) * (fabsf(e2x) + fabsf(e2y)))
                return;  // ill-formed per the spec: not rendered
            const float dx = localX - p0x;
            const float dy = localY - p0y;
            t = (dx * e2y - dy * e2x) / cross;
        }
        else if (paint->Type == ImSlugColorPaintType_RadialGradient)
        {
            const float c0x = paint->Geo0[0], c0y = paint->Geo0[1], r0 = paint->Geo0[2];
            const float c1x = paint->Geo1[0], c1y = paint->Geo1[1], r1 = paint->Geo1[2];
            const float dx = c1x - c0x, dy = c1y - c0y;
            const float dr = r1 - r0;
            const float qx = localX - c0x, qy = localY - c0y;
            const float a = dx * dx + dy * dy - dr * dr;
            const float b = -2.0f * (qx * dx + qy * dy + r0 * dr);
            const float c = qx * qx + qy * qy - r0 * r0;

            bool found = false;
            float best = 0.0f;
            if (fabsf(a) <= 1.0e-9f * (dx * dx + dy * dy + dr * dr + 1.0f))
            {
                if (fabsf(b) > 1.0e-20f)
                {
                    best = -c / b;
                    found = r0 + best * dr > 0.0f;
                }
            }
            else
            {
                const float disc = b * b - 4.0f * a * c;
                if (disc >= 0.0f)
                {
                    const float root = sqrtf(disc);
                    const float candidates[2] = { (-b + root) / (2.0f * a), (-b - root) / (2.0f * a) };
                    for (int i = 0; i < 2; ++i)
                    {
                        const float candidate = candidates[i];
                        if (r0 + candidate * dr <= 0.0f)
                            continue;  // circles with r(omega) > 0 only
                        if (!found || candidate > best)
                        {
                            best = candidate;
                            found = true;
                        }
                    }
                }
            }
            if (!found)
                return;  // outside the cone / both radii zero: nothing painted
            t = best;
        }
        else  // sweep
        {
            const float cx = paint->Geo0[0], cy = paint->Geo0[1];
            const float startAngle = paint->Geo0[2], endAngle = paint->Geo0[3];
            const float span = endAngle - startAngle;
            if (fabsf(span) < 1.0e-4f)
            {
                if (paint->Extend != ImSlugColorExtend_Pad)
                    return;  // repeat/reflect with equal angles: nothing drawn
                t = 0.0f;
            }
            else
            {
                float theta = atan2f(localY - cy, localX - cx) * kRadToDeg;
                if (theta < 0.0f)
                    theta += 360.0f;
                t = (theta - startAngle) / span;
            }
        }

        t = ExtendParameter(t, paint->Extend);
        SampleColorLine(paint, t, outRgba);
    }

    // -------------------------------------------------------------------------
    // Color glyph cache
    // -------------------------------------------------------------------------
    namespace
    {
        ImSlugColorFont::CachedGlyph* FindCachedGlyph(const ImSlugColorFont* font, int glyphIndex)
        {
            for (int i = 0; i < font->CacheCount; ++i)
                if (font->Cache[i]->GlyphIndex == glyphIndex)
                    return font->Cache[i];
            return nullptr;
        }

        ImSlugColorFont::CachedGlyph* AppendCachedGlyph(ImSlugColorFont* font, int glyphIndex, ImSlugBuildStatus status)
        {
            if (font->CacheCount == font->CacheCapacity)
            {
                const int newCapacity = font->CacheCapacity == 0 ? 16 : font->CacheCapacity * 2;
                void* grown = realloc(font->Cache, (size_t)newCapacity * sizeof(ImSlugColorFont::CachedGlyph*));
                if (grown == nullptr)
                    return nullptr;
                font->Cache = (ImSlugColorFont::CachedGlyph**)grown;
                font->CacheCapacity = newCapacity;
            }
            ImSlugColorFont::CachedGlyph* entry =
                (ImSlugColorFont::CachedGlyph*)malloc(sizeof(ImSlugColorFont::CachedGlyph));
            if (entry == nullptr)
                return nullptr;
            memset(entry, 0, sizeof(*entry));
            entry->GlyphIndex = glyphIndex;
            entry->Status = status;
            font->Cache[font->CacheCount++] = entry;
            return entry;
        }

        void UnionRect(float x, float y, float* minX, float* minY, float* maxX, float* maxY, bool* haveBounds)
        {
            if (!*haveBounds)
            {
                *minX = *maxX = x;
                *minY = *maxY = y;
                *haveBounds = true;
                return;
            }
            if (x < *minX)
                *minX = x;
            if (x > *maxX)
                *maxX = x;
            if (y < *minY)
                *minY = y;
            if (y > *maxY)
                *maxY = y;
        }
    }  // namespace

    ImSlugBuildStatus ImSlugGetColorGlyph(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                          const ImSlugColorGlyph** outColorGlyph)
    {
        if (outColorGlyph != nullptr)
            *outColorGlyph = nullptr;
        if (font == nullptr || pools == nullptr || outColorGlyph == nullptr || glyphIndex <= 0 || glyphIndex > 0xFFFF)
            return ImSlugBuildStatus_GlyphNotFound;

        ImSlugColorFont* colorFont = ImSlugFontGetColor(font);
        if (colorFont == nullptr)
            return ImSlugBuildStatus_GlyphNotFound;

        if (ImSlugColorFont::CachedGlyph* cached = FindCachedGlyph(colorFont, glyphIndex))
        {
            if (cached->Status == ImSlugBuildStatus_Ok)
            {
                *outColorGlyph = &cached->Glyph;
                return cached->Glyph.LayerCount > 0 ? ImSlugBuildStatus_Ok : ImSlugBuildStatus_GlyphNotFound;
            }
            return cached->Status;
        }

        const uint16_t glyphID = (uint16_t)glyphIndex;
        const float invUpem = 1.0f / (float)colorFont->UnitsPerEm;

        // ClipBox of the base glyph (font units -> base em).
        bool clipValid = false;
        float clipBaseEm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float clipMinX = 0, clipMinY = 0, clipMaxX = 0, clipMaxY = 0;
        if (ResolveClipBox(colorFont, glyphID, &clipValid, &clipMinX, &clipMinY, &clipMaxX, &clipMaxY) && clipValid)
        {
            clipBaseEm[0] = clipMinX * invUpem;
            clipBaseEm[1] = clipMinY * invUpem;
            clipBaseEm[2] = clipMaxX * invUpem;
            clipBaseEm[3] = clipMaxY * invUpem;
            if (!(clipBaseEm[2] > clipBaseEm[0] && clipBaseEm[3] > clipBaseEm[1]))
                clipValid = false;  // degenerate box: no clip
        }
        else
        {
            clipValid = false;
        }

        // Resolve the layer list.
        ResolvedLayer* resolved = (ResolvedLayer*)malloc((size_t)kMaxColorLayers * sizeof(ResolvedLayer));
        if (resolved == nullptr)
            return ImSlugBuildStatus_OutOfMemory;

        FlattenState state;
        state.Font = colorFont;
        state.Layers = resolved;
        state.LayerCapacity = kMaxColorLayers;
        state.LayerCount = 0;
        state.NodeCount = 0;
        state.Truncated = false;
        state.ColrGlyphStackDepth = 0;
        state.ColrGlyphStack[0] = glyphID;  // the glyph being resolved (cycle guard)
        state.ColrGlyphStackDepth = 1;

        const float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        uint32_t paintOffset = 0;
        if (FindV1BasePaint(colorFont, glyphID, &paintOffset))
        {
            PaintTable root;
            root.Base = colorFont->Data + colorFont->Colr.Offset;
            root.Size = colorFont->Colr.Length;
            root.Offset = paintOffset;
            FlattenPaint(colorFont, root, identity, 1.0f, 0, 0, &state);
        }

        // COLR v0 fallback (also used for v0-only fonts).
        if (state.LayerCount == 0)
        {
            uint16_t firstLayer = 0;
            uint16_t layerCount = 0;
            if (FindV0BaseRecord(colorFont, glyphID, &firstLayer, &layerCount))
            {
                const uint8_t* data = colorFont->Data + colorFont->Colr.Offset;
                const uint32_t size = colorFont->Colr.Length;
                for (uint32_t i = 0; i < (uint32_t)layerCount; ++i)
                {
                    const uint32_t index = (uint32_t)firstLayer + i;
                    if (index >= colorFont->V0LayerRecordCount)
                        break;
                    if (state.LayerCount >= state.LayerCapacity)
                    {
                        state.Truncated = true;
                        break;
                    }
                    uint16_t layerGlyph = 0;
                    uint16_t paletteIndex = 0;
                    const uint32_t record = colorFont->V0LayerRecordsOffset + index * 4u;
                    if (!ReadU16(data, size, record, &layerGlyph) || !ReadU16(data, size, record + 2, &paletteIndex))
                        break;
                    if (layerGlyph == 0)
                        continue;
                    CpalColor color = { 0, 0, 0, 0 };
                    if (!GetPaletteColor(colorFont, paletteIndex, &color))
                        continue;
                    ResolvedLayer layer;
                    ResetLayer(&layer);
                    layer.GlyphID = layerGlyph;
                    layer.Type = ImSlugColorPaintType_Solid;
                    layer.StopCount = 1;
                    layer.SolidColor = PackCpalColor(color, 1.0f);
                    state.Layers[state.LayerCount++] = layer;
                }
            }
        }

        if (state.LayerCount == 0)
        {
            free(resolved);
            // Definitive: the glyph has color data but nothing drawable.
            ImSlugColorFont::CachedGlyph* entry =
                AppendCachedGlyph(colorFont, glyphIndex, ImSlugBuildStatus_GlyphNotFound);
            (void)entry;
            return ImSlugBuildStatus_GlyphNotFound;
        }

        ImSlugColorLayer* layers = (ImSlugColorLayer*)malloc((size_t)state.LayerCount * sizeof(ImSlugColorLayer));
        ImSlugGlyphRecord* records = (ImSlugGlyphRecord*)malloc((size_t)state.LayerCount * sizeof(ImSlugGlyphRecord));
        if (layers == nullptr || records == nullptr)
        {
            free(layers);
            free(records);
            free(resolved);
            return ImSlugBuildStatus_OutOfMemory;
        }
        memset(layers, 0, (size_t)state.LayerCount * sizeof(ImSlugColorLayer));
        memset(records, 0, (size_t)state.LayerCount * sizeof(ImSlugGlyphRecord));

        const float kAxisEpsilon = 1.0e-4f;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        bool haveBounds = false;
        int writeCount = 0;
        ImSlugBuildStatus retryable = ImSlugBuildStatus_Ok;

        for (int i = 0; i < state.LayerCount; ++i)
        {
            const ResolvedLayer& layer = resolved[i];
            ImSlugColorLayer built;
            memset(&built, 0, sizeof(built));

            // Outline record in record space + the record->base mapping in em.
            float recordToBaseEm[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
            bool skipLayer = false;
            if (IsAxisAlignedPositive(layer.M, kAxisEpsilon))
            {
                const ImSlugGlyphRecord* record = nullptr;
                const ImSlugBuildStatus status = ImSlugGetGlyphRecordByIndex(font, pools, (int)layer.GlyphID, &record);
                if (status != ImSlugBuildStatus_Ok || record == nullptr)
                {
                    skipLayer = true;
                    if (status != ImSlugBuildStatus_Ok && status != ImSlugBuildStatus_GlyphNotFound &&
                        status != ImSlugBuildStatus_GlyphTooLarge && status != ImSlugBuildStatus_InvalidOutline)
                        retryable = status;
                }
                else if (record->CurveCount == 0)
                {
                    skipLayer = true;  // empty outline: nothing to draw
                }
                else
                {
                    built.Record = record;
                    built.ScaleX = layer.M[0];
                    built.ScaleY = layer.M[4];
                    built.OffsetXEm = layer.M[2] * invUpem;
                    built.OffsetYEm = layer.M[5] * invUpem;
                    recordToBaseEm[0] = layer.M[0];
                    recordToBaseEm[1] = 0.0f;
                    recordToBaseEm[2] = built.OffsetXEm;
                    recordToBaseEm[3] = 0.0f;
                    recordToBaseEm[4] = layer.M[4];
                    recordToBaseEm[5] = built.OffsetYEm;
                }
            }
            else
            {
                // Bake the transform into the outline: extract em-space quads,
                // map them through the transform, and build a synthetic record
                // in the base em space.
                ImSlugQuad localStack[64];
                ImSlugQuad* quads = localStack;
                int quadCount = 0;
                if (!ImSlugGetGlyphOutlineQuads(font, (int)layer.GlyphID, quads, 64, &quadCount))
                {
                    if (quadCount <= 0)
                    {
                        skipLayer = true;
                    }
                    else
                    {
                        ImSlugQuad* heap = (ImSlugQuad*)malloc((size_t)quadCount * sizeof(ImSlugQuad));
                        if (heap == nullptr)
                        {
                            retryable = ImSlugBuildStatus_OutOfMemory;
                            skipLayer = true;
                        }
                        else if (!ImSlugGetGlyphOutlineQuads(font, (int)layer.GlyphID, heap, quadCount, &quadCount))
                        {
                            free(heap);
                            skipLayer = true;
                        }
                        else
                        {
                            quads = heap;
                        }
                    }
                }
                if (!skipLayer)
                {
                    if (quadCount == 0)
                    {
                        skipLayer = true;
                    }
                    else
                    {
                        float mEm[6];
                        mEm[0] = layer.M[0];
                        mEm[1] = layer.M[1];
                        mEm[2] = layer.M[2] * invUpem;
                        mEm[3] = layer.M[3];
                        mEm[4] = layer.M[4];
                        mEm[5] = layer.M[5] * invUpem;
                        for (int q = 0; q < quadCount; ++q)
                        {
                            float x1, y1, x2, y2, x3, y3;
                            ApplyAffine(mEm, quads[q].X1, quads[q].Y1, &x1, &y1);
                            ApplyAffine(mEm, quads[q].X2, quads[q].Y2, &x2, &y2);
                            ApplyAffine(mEm, quads[q].X3, quads[q].Y3, &x3, &y3);
                            quads[q].X1 = x1;
                            quads[q].Y1 = y1;
                            quads[q].X2 = x2;
                            quads[q].Y2 = y2;
                            quads[q].X3 = x3;
                            quads[q].Y3 = y3;
                        }
                        const ImSlugBuildStatus status =
                            ImSlugBuildSyntheticGlyphRecord(font, pools, quads, quadCount, 0.0f, &records[writeCount]);
                        if (status == ImSlugBuildStatus_Ok)
                        {
                            built.Record = &records[writeCount];
                            built.ScaleX = 1.0f;
                            built.ScaleY = 1.0f;
                            built.OffsetXEm = 0.0f;
                            built.OffsetYEm = 0.0f;
                        }
                        else
                        {
                            skipLayer = true;
                            if (status != ImSlugBuildStatus_GlyphTooLarge && status != ImSlugBuildStatus_InvalidOutline)
                                retryable = status;
                        }
                    }
                }
                if (quads != localStack)
                    free(quads);
            }

            if (skipLayer)
                continue;

            // Paint record: allocate lazily so skipped layers cost nothing.
            if (writeCount == 0)
            {
                if (ImSlugAllocPaintRecords(&pools->Paints, state.LayerCount) < 0)
                {
                    retryable = ImSlugBuildStatus_OutOfMemory;
                    break;
                }
            }
            const int paintIndex = pools->Paints.Count - state.LayerCount + writeCount;

            // Paint geometry: fill-local font units -> em; angles stay degrees.
            float fillToBaseEm[6];
            fillToBaseEm[0] = layer.FillM[0];
            fillToBaseEm[1] = layer.FillM[1];
            fillToBaseEm[2] = layer.FillM[2] * invUpem;
            fillToBaseEm[3] = layer.FillM[3];
            fillToBaseEm[4] = layer.FillM[4];
            fillToBaseEm[5] = layer.FillM[5] * invUpem;

            ImSlugPaintRecord* paint = ImSlugPaintRecordAt(&pools->Paints, paintIndex);
            memset(paint, 0, sizeof(*paint));
            paint->Type = layer.Type;
            paint->Extend = layer.Extend;
            paint->StopCount = layer.StopCount;
            paint->SolidColor = layer.SolidColor;

            // Fold the constant composite mask into the colors' alpha.
            const float alphaMul = layer.AlphaMul < 0.0f ? 0.0f : (layer.AlphaMul > 1.0f ? 1.0f : layer.AlphaMul);
            if (paint->Type == ImSlugColorPaintType_Solid)
            {
                const uint32_t alpha = (paint->SolidColor >> 24) & 0xFFu;
                uint32_t scaled = (uint32_t)((float)alpha * alphaMul + 0.5f);
                if (scaled > 255u)
                    scaled = 255u;
                paint->SolidColor = (paint->SolidColor & 0x00FFFFFFu) | (scaled << 24);
            }
            else
            {
                for (uint32_t s = 0; s < paint->StopCount; ++s)
                {
                    const uint32_t color = layer.Stops[2 * s + 1];
                    const uint32_t alpha = (color >> 24) & 0xFFu;
                    uint32_t scaled = (uint32_t)((float)alpha * alphaMul + 0.5f);
                    if (scaled > 255u)
                        scaled = 255u;
                    paint->Stops[2 * s] = layer.Stops[2 * s];
                    paint->Stops[2 * s + 1] = (color & 0x00FFFFFFu) | (scaled << 24);
                }
            }

            if (paint->Type != ImSlugColorPaintType_Solid)
            {
                // record -> paint-local = paintToBase^-1 * recordToBase.
                float paintToBaseInv[6];
                float recordToPaint[6];
                if (InvertAffine(fillToBaseEm, paintToBaseInv))
                    ComposeAffine(paintToBaseInv, recordToBaseEm, recordToPaint);
                else
                    memcpy(recordToPaint, recordToBaseEm, sizeof(float) * 6);
                paint->Xform0[0] = recordToPaint[0];
                paint->Xform0[1] = recordToPaint[1];
                paint->Xform0[2] = recordToPaint[2];
                paint->Xform1[0] = recordToPaint[3];
                paint->Xform1[1] = recordToPaint[4];
                paint->Xform1[2] = recordToPaint[5];

                if (paint->Type == ImSlugColorPaintType_LinearGradient)
                {
                    paint->Geo0[0] = layer.Geo0[0] * invUpem;
                    paint->Geo0[1] = layer.Geo0[1] * invUpem;
                    paint->Geo0[2] = layer.Geo0[2] * invUpem;
                    paint->Geo0[3] = layer.Geo0[3] * invUpem;
                    paint->Geo1[0] = layer.Geo1[0] * invUpem;
                    paint->Geo1[1] = layer.Geo1[1] * invUpem;
                }
                else if (paint->Type == ImSlugColorPaintType_RadialGradient)
                {
                    paint->Geo0[0] = layer.Geo0[0] * invUpem;
                    paint->Geo0[1] = layer.Geo0[1] * invUpem;
                    paint->Geo0[2] = layer.Geo0[2] * invUpem;
                    paint->Geo1[0] = layer.Geo1[0] * invUpem;
                    paint->Geo1[1] = layer.Geo1[1] * invUpem;
                    paint->Geo1[2] = layer.Geo1[2] * invUpem;
                }
                else  // sweep: center scales, angles are degrees
                {
                    paint->Geo0[0] = layer.Geo0[0] * invUpem;
                    paint->Geo0[1] = layer.Geo0[1] * invUpem;
                    paint->Geo0[2] = layer.Geo0[2];
                    paint->Geo0[3] = layer.Geo0[3];
                }
            }

            // ClipBox: base em -> record space.
            if (clipValid)
            {
                float recordToBaseInv[6];
                if (InvertAffine(recordToBaseEm, recordToBaseInv))
                {
                    const float xs[4] = { clipBaseEm[0], clipBaseEm[2], clipBaseEm[0], clipBaseEm[2] };
                    const float ys[4] = { clipBaseEm[1], clipBaseEm[1], clipBaseEm[3], clipBaseEm[3] };
                    float minClipX = 0.0f, minClipY = 0.0f, maxClipX = 0.0f, maxClipY = 0.0f;
                    bool haveClip = false;
                    for (int c = 0; c < 4; ++c)
                    {
                        float rx = 0.0f, ry = 0.0f;
                        ApplyAffine(recordToBaseInv, xs[c], ys[c], &rx, &ry);
                        UnionRect(rx, ry, &minClipX, &minClipY, &maxClipX, &maxClipY, &haveClip);
                    }
                    if (haveClip && maxClipX > minClipX && maxClipY > minClipY)
                    {
                        paint->Clip[0] = minClipX;
                        paint->Clip[1] = minClipY;
                        paint->Clip[2] = maxClipX;
                        paint->Clip[3] = maxClipY;
                    }
                }
            }

            built.PaintIndex = (uint32_t)paintIndex;

            // Union bounds of the drawn records in base em.
            const ImSlugGlyphRecord* record = built.Record;
            UnionRect(built.OffsetXEm + record->MinX * built.ScaleX, built.OffsetYEm + record->MinY * built.ScaleY,
                      &minX, &minY, &maxX, &maxY, &haveBounds);
            UnionRect(built.OffsetXEm + record->MaxX * built.ScaleX, built.OffsetYEm + record->MaxY * built.ScaleY,
                      &minX, &minY, &maxX, &maxY, &haveBounds);

            layers[writeCount] = built;
            ++writeCount;
        }

        free(resolved);

        if (writeCount == 0 || retryable != ImSlugBuildStatus_Ok)
        {
            free(layers);
            free(records);
            if (retryable != ImSlugBuildStatus_Ok)
                return retryable;  // not cached: retried on a later bake
            ImSlugColorFont::CachedGlyph* entry =
                AppendCachedGlyph(colorFont, glyphIndex, ImSlugBuildStatus_GlyphNotFound);
            (void)entry;
            return ImSlugBuildStatus_GlyphNotFound;
        }

        // Widen the union with the base glyph's own em bbox when it has one, so
        // the slab's reconstruction record always covers the quad ImGui drew
        // (the metrics box comes from the base glyph's glyf outline when that is
        // non-empty, from the layer union otherwise - see the adapter).
        {
            ImSlugGlyphMetrics baseMetrics;
            if (ImSlugGetGlyphMetricsByIndex(font, glyphID, 16.0f, &baseMetrics) &&
                baseMetrics.MinX < baseMetrics.MaxX && baseMetrics.MinY < baseMetrics.MaxY)
            {
                UnionRect(baseMetrics.MinX, baseMetrics.MinY, &minX, &minY, &maxX, &maxY, &haveBounds);
                UnionRect(baseMetrics.MaxX, baseMetrics.MaxY, &minX, &minY, &maxX, &maxY, &haveBounds);
            }
        }

        ImSlugColorFont::CachedGlyph* entry = AppendCachedGlyph(colorFont, glyphIndex, ImSlugBuildStatus_Ok);
        if (entry == nullptr)
        {
            free(layers);
            free(records);
            return ImSlugBuildStatus_OutOfMemory;
        }
        entry->Layers = layers;
        entry->Records = records;
        entry->Glyph.GlyphIndex = glyphIndex;
        entry->Glyph.LayerCount = writeCount;
        entry->Glyph.Layers = layers;
        entry->Glyph.MinX = minX;
        entry->Glyph.MinY = minY;
        entry->Glyph.MaxX = maxX;
        entry->Glyph.MaxY = maxY;
        memset(&entry->Glyph.BaseRecord, 0, sizeof(entry->Glyph.BaseRecord));
        entry->Glyph.BaseRecord.MinX = minX;
        entry->Glyph.BaseRecord.MinY = minY;
        entry->Glyph.BaseRecord.MaxX = maxX;
        entry->Glyph.BaseRecord.MaxY = maxY;
        *outColorGlyph = &entry->Glyph;
        return ImSlugBuildStatus_Ok;
    }

    int ImSlugBuildColorGlyphInstances(const ImSlugColorGlyph* colorGlyph, float penX, float penY, float pixelsPerEm,
                                       float tintA, ImSlugGlyphInstance* outInstances, int capacity,
                                       int* outLayerIndices)
    {
        if (colorGlyph == nullptr || outInstances == nullptr || capacity <= 0)
            return 0;
        int count = 0;
        for (int i = 0; i < colorGlyph->LayerCount && count < capacity; ++i)
        {
            const ImSlugColorLayer& layer = colorGlyph->Layers[i];
            if (layer.Record == nullptr)
                continue;
            const float scaledPenX = penX + layer.OffsetXEm * pixelsPerEm;
            const float scaledPenY = penY - layer.OffsetYEm * pixelsPerEm;
            ImSlugBuildGlyphInstanceScaled(layer.Record, scaledPenX, scaledPenY, pixelsPerEm * layer.ScaleX,
                                           pixelsPerEm * layer.ScaleY, 1.0f, 1.0f, 1.0f, tintA, &outInstances[count]);
            outInstances[count].PaintIndex = layer.PaintIndex;
            if (outLayerIndices != nullptr)
                outLayerIndices[count] = i;
            ++count;
        }
        return count;
    }
}  // namespace ImSlug
