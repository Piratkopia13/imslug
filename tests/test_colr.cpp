// imslug — unit test: COLR/CPAL reader, color-glyph cache, CPU paint sampler.
//
// Three layers of coverage:
//   1. synthetic COLR/CPAL tables grafted onto the committed Roboto bytes
//      (real glyf outlines, hand-built color tables): v0 layer lists, v1 paint
//      graphs (solid, gradients, transforms, composites, ClipBox, unsupported
//      formats, cycles), palette resolution and the alpha folding;
//   2. the CPU paint sampler (ImSlugSamplePaintRecord), the mirror of the
//      fragment shader's paint code: solid/linear/radial/sweep values, extend
//      modes, clip boxes and degenerate (ill-formed) gradients;
//   3. the fetched Noto Color Emoji font (tests/fonts/emoji/, gitignored):
//      structural checks over a real COLR v1 font; the suite prints a skip and
//      continues when the asset is absent.
//
// Layout/advance parity with the monochrome path lives in test_imslug_imgui.cpp
// (it needs the ImGui atlas); the emoji goldens (imslug_emoji_golden) compare
// the GPU against this file's CPU sampling end to end.
//
// No STL containers here on purpose: the project builds our targets with
// MSVC's /EHs-c- (AGENTS.md invariant 4), and MSVC's STL emits C4530 warnings
// when vector reallocation is instantiated without unwind semantics.

#include "test_util.h"

#include "imslug_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    // ---------------------------------------------------------------------
    // Minimal growable byte buffer (no STL, no exceptions)
    // ---------------------------------------------------------------------
    struct Bytes
    {
        unsigned char* Data = nullptr;
        size_t Size = 0;
        size_t Capacity = 0;

        Bytes() = default;
        Bytes(const Bytes&) = delete;
        Bytes& operator=(const Bytes&) = delete;
        Bytes(Bytes&& other) noexcept
            : Data(other.Data)
            , Size(other.Size)
            , Capacity(other.Capacity)
        {
            other.Data = nullptr;
            other.Size = 0;
            other.Capacity = 0;
        }
        Bytes& operator=(Bytes&& other) noexcept
        {
            if (this != &other)
            {
                std::free(Data);
                Data = other.Data;
                Size = other.Size;
                Capacity = other.Capacity;
                other.Data = nullptr;
                other.Size = 0;
                other.Capacity = 0;
            }
            return *this;
        }
        ~Bytes() { std::free(Data); }

        bool Reserve(size_t needed)
        {
            if (needed <= Capacity)
                return true;
            size_t newCapacity = Capacity == 0 ? 256 : Capacity;
            while (newCapacity < needed)
                newCapacity *= 2;
            void* grown = std::realloc(Data, newCapacity);
            if (grown == nullptr)
                return false;
            Data = (unsigned char*)grown;
            Capacity = newCapacity;
            return true;
        }

        bool Resize(size_t size, unsigned char fill = 0)
        {
            if (!Reserve(size))
                return false;
            if (size > Size)
                memset(Data + Size, fill, size - Size);
            Size = size;
            return true;
        }

        bool Push(unsigned char value)
        {
            if (!Reserve(Size + 1))
                return false;
            Data[Size++] = value;
            return true;
        }

        bool Append(const unsigned char* bytes, size_t count)
        {
            if (!Reserve(Size + count))
                return false;
            memcpy(Data + Size, bytes, count);
            Size += count;
            return true;
        }

        unsigned char& operator[](size_t index) { return Data[index]; }
        unsigned char operator[](size_t index) const { return Data[index]; }
    };

    void WriteU16(Bytes& bytes, size_t at, uint32_t value)
    {
        bytes[at] = (unsigned char)(value >> 8);
        bytes[at + 1] = (unsigned char)value;
    }

    void WriteU32(Bytes& bytes, size_t at, uint32_t value)
    {
        bytes[at] = (unsigned char)(value >> 24);
        bytes[at + 1] = (unsigned char)(value >> 16);
        bytes[at + 2] = (unsigned char)(value >> 8);
        bytes[at + 3] = (unsigned char)value;
    }

    uint16_t ReadU16At(const Bytes& bytes, size_t at)
    {
        return (uint16_t)((bytes[at] << 8) | bytes[at + 1]);
    }

    uint32_t ReadU32At(const Bytes& bytes, size_t at)
    {
        return ((uint32_t)bytes[at] << 24) | ((uint32_t)bytes[at + 1] << 16) | ((uint32_t)bytes[at + 2] << 8) |
               (uint32_t)bytes[at + 3];
    }

    // Appends COLR + CPAL tables to a copy of `base` (a real font), rebuilding
    // the SFNT directory. Checksums are zeroed (stb_truetype does not verify).
    Bytes BuildFontWithTables(const Bytes& base, const Bytes& colr, const Bytes& cpal)
    {
        const uint16_t numTables = ReadU16At(base, 4);
        const uint32_t newCount = (uint32_t)numTables + 2;
        Bytes out;
        out.Resize(12 + (size_t)newCount * 16);
        memcpy(out.Data, base.Data, 12);
        WriteU16(out, 4, newCount);

        size_t record = 12;
        for (uint16_t i = 0; i < numTables; ++i)
        {
            memcpy(out.Data + record, base.Data + 12 + (size_t)i * 16, 16);
            record += 16;
        }

        // Table data area: copy the original tables, then append the new ones
        // (4-byte aligned). Table offsets in the directory are absolute (from
        // the file start), so they include the directory size.
        const size_t directorySize = 12 + (size_t)newCount * 16;
        Bytes data;
        for (uint16_t i = 0; i < numTables; ++i)
        {
            const size_t source = 12 + (size_t)i * 16;
            const uint32_t offset = ReadU32At(base, source + 8);
            const uint32_t length = ReadU32At(base, source + 12);
            const size_t recordAt = 12 + (size_t)i * 16;
            WriteU32(out, recordAt + 8, (uint32_t)(directorySize + data.Size));
            data.Append(base.Data + offset, length);
            while (data.Size % 4 != 0)
                data.Push(0);
        }

        const char* tags[2] = { "COLR", "CPAL" };
        const Bytes* tables[2] = { &colr, &cpal };
        for (int t = 0; t < 2; ++t)
        {
            const size_t recordAt = record;
            memcpy(out.Data + recordAt, tags[t], 4);
            WriteU32(out, recordAt + 4, 0);
            WriteU32(out, recordAt + 8, (uint32_t)(directorySize + data.Size));
            WriteU32(out, recordAt + 12, (uint32_t)tables[t]->Size);
            data.Append(tables[t]->Data, tables[t]->Size);
            while (data.Size % 4 != 0)
                data.Push(0);
            record += 16;
        }

        out.Append(data.Data, data.Size);
        return out;
    }

    struct V0Layer
    {
        uint16_t Glyph;
        uint16_t Palette;
    };

    // COLR v0: base glyph -> (layer glyph, palette index) list.
    Bytes BuildColrV0(uint16_t baseGlyph, const V0Layer* layers, int layerCount)
    {
        Bytes out;
        out.Resize(20 + (size_t)layerCount * 4);
        WriteU16(out, 0, 0);                      // version
        WriteU16(out, 2, 1);                      // numBaseGlyphRecords
        WriteU32(out, 4, 14);                     // baseGlyphRecordsOffset
        WriteU32(out, 8, 14 + 6);                 // layerRecordsOffset
        WriteU16(out, 12, (uint32_t)layerCount);  // numLayerRecords
        WriteU16(out, 14, baseGlyph);
        WriteU16(out, 16, 0);  // firstLayerIndex
        WriteU16(out, 18, (uint32_t)layerCount);
        for (int i = 0; i < layerCount; ++i)
        {
            WriteU16(out, 20 + (size_t)i * 4, layers[i].Glyph);
            WriteU16(out, 22 + (size_t)i * 4, layers[i].Palette);
        }
        return out;
    }

    // CPAL v0 with one palette (index 0) holding `colors` (RGBA order; stored
    // BGRA in the table).
    Bytes BuildCpal(const uint32_t* colors, int colorCount)
    {
        Bytes out;
        out.Resize(14 + (size_t)colorCount * 4);
        WriteU16(out, 0, 0);
        WriteU16(out, 2, (uint32_t)colorCount);  // numPaletteEntries
        WriteU16(out, 4, 1);                     // numPalettes
        WriteU16(out, 6, (uint32_t)colorCount);  // numColorRecords
        WriteU32(out, 8, 14);                    // colorRecordsArrayOffset
        WriteU16(out, 12, 0);                    // palette 0 first record
        for (int i = 0; i < colorCount; ++i)
        {
            const uint32_t rgba = colors[i];
            out[14 + (size_t)i * 4 + 0] = (unsigned char)(rgba >> 16);  // B
            out[14 + (size_t)i * 4 + 1] = (unsigned char)(rgba >> 8);   // G
            out[14 + (size_t)i * 4 + 2] = (unsigned char)rgba;          // R
            out[14 + (size_t)i * 4 + 3] = (unsigned char)(rgba >> 24);  // A
        }
        return out;
    }

    // COLR v1 graph builder: paints live in a position-independent blob (all
    // paint-to-paint offsets are parent-relative, so they survive the final
    // layout), the LayerList/BaseGlyphList/ClipList are laid out in front.
    struct ColrV1Builder
    {
        Bytes Paints;
        uint32_t LayerPaints[64] = {};
        int LayerPaintCount = 0;

        uint32_t Pos() const { return (uint32_t)Paints.Size; }
        void U8(uint32_t v) { Paints.Push((unsigned char)v); }
        void U16(uint32_t v)
        {
            Paints.Push((unsigned char)(v >> 8));
            Paints.Push((unsigned char)v);
        }
        void I16(int v) { U16((uint16_t)(int16_t)v); }
        void U24(uint32_t v)
        {
            Paints.Push((unsigned char)(v >> 16));
            Paints.Push((unsigned char)(v >> 8));
            Paints.Push((unsigned char)v);
        }
        void U32(uint32_t v)
        {
            U16(v >> 16);
            U16(v);
        }
        void Fixed(float v) { U32((uint32_t)(int32_t)lround(v * 65536.0)); }
        void F2Dot14(float v) { I16((int)lround(v * 16384.0)); }
        void PatchU24(uint32_t at, uint32_t v)
        {
            Paints[at] = (unsigned char)(v >> 16);
            Paints[at + 1] = (unsigned char)(v >> 8);
            Paints[at + 2] = (unsigned char)v;
        }

        struct StopSpec
        {
            int16_t OffsetRaw;  // F2DOT14 (16384 = 1.0)
            uint16_t PaletteIndex;
            uint8_t Alpha;
        };

        // Every paint-to-paint offset is parent-relative (the spec's Offset24),
        // so a child must live *after* its parent in the blob: the helpers
        // below write the parent first with a placeholder and patch the offset
        // once the child has been appended. The value is relative to the
        // parent paint's start (not to the offset field).
        void AttachOffset24(uint32_t paintAt, uint32_t fieldOffset, uint32_t childAt)
        {
            PatchU24(paintAt + fieldOffset, childAt - paintAt);
        }

        uint32_t ColorLine(uint8_t extend, const StopSpec* stops, int stopCount)
        {
            const uint32_t at = Pos();
            U8(extend);
            U16((uint32_t)stopCount);
            for (int i = 0; i < stopCount; ++i)
            {
                I16(stops[i].OffsetRaw);
                U16(stops[i].PaletteIndex);
                F2Dot14((float)stops[i].Alpha / 255.0f);
            }
            return at;
        }

        uint32_t PaintSolid(uint16_t paletteIndex, float alpha)
        {
            const uint32_t at = Pos();
            U8(2);
            U16(paletteIndex);
            F2Dot14(alpha);
            return at;
        }

        // Parent + child in one call (the color line follows the gradient).
        uint32_t PaintLinear(const StopSpec* stops, int stopCount, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             int16_t x2, int16_t y2)
        {
            const uint32_t at = Pos();
            U8(4);
            U24(0);
            I16(x0);
            I16(y0);
            I16(x1);
            I16(y1);
            I16(x2);
            I16(y2);
            const uint32_t colorLine = ColorLine(0, stops, stopCount);
            AttachOffset24(at, 1, colorLine);
            return at;
        }

        uint32_t PaintSweep(const StopSpec* stops, int stopCount, int16_t cx, int16_t cy, float startDeg, float endDeg)
        {
            const uint32_t at = Pos();
            U8(8);
            U24(0);
            I16(cx);
            I16(cy);
            F2Dot14((startDeg - 180.0f) / 180.0f);  // BiasedAngle
            F2Dot14((endDeg - 180.0f) / 180.0f);
            const uint32_t colorLine = ColorLine(0, stops, stopCount);
            AttachOffset24(at, 1, colorLine);
            return at;
        }

        uint32_t PaintGlyphBegin(uint16_t glyph)
        {
            const uint32_t at = Pos();
            U8(10);
            U24(0);
            U16(glyph);
            return at;
        }

        void AttachPaint(uint32_t parentAt, uint32_t childAt) { AttachOffset24(parentAt, 1, childAt); }

        uint32_t PaintTransformBegin()
        {
            const uint32_t at = Pos();
            U8(12);
            U24(0);
            U24(0);
            return at;
        }

        uint32_t AppendAffine(float xx, float yx, float xy, float yy, float dx, float dy)
        {
            const uint32_t at = Pos();
            Fixed(xx);
            Fixed(yx);
            Fixed(xy);
            Fixed(yy);
            Fixed(dx);
            Fixed(dy);
            return at;
        }

        void AttachTransform(uint32_t at, uint32_t childAt, uint32_t affineAt)
        {
            AttachOffset24(at, 1, childAt);
            AttachOffset24(at, 4, affineAt);
        }

        uint32_t PaintTranslateBegin()
        {
            const uint32_t at = Pos();
            U8(14);
            U24(0);
            U16(0);  // dx, patched by AttachTranslate
            U16(0);  // dy
            return at;
        }

        void AttachTranslate(uint32_t at, int16_t dx, int16_t dy)
        {
            Paints[at + 4] = (unsigned char)((uint16_t)dx >> 8);
            Paints[at + 5] = (unsigned char)(uint16_t)dx;
            Paints[at + 6] = (unsigned char)((uint16_t)dy >> 8);
            Paints[at + 7] = (unsigned char)(uint16_t)dy;
        }

        uint32_t PaintCompositeBegin(uint8_t mode)
        {
            const uint32_t at = Pos();
            U8(32);
            U24(0);
            U8(mode);
            U24(0);
            return at;
        }

        void AttachComposite(uint32_t at, uint32_t sourceAt, uint32_t backdropAt)
        {
            AttachOffset24(at, 1, sourceAt);
            AttachOffset24(at, 5, backdropAt);
        }

        // Reserves `count` layer-list slots (the caller captures LayerPaintCount
        // before the call and fills the slots with SetLayerPaint as the child
        // paints are built).
        uint32_t PaintColrLayersBegin(int count)
        {
            const uint32_t at = Pos();
            U8(1);
            U8((uint32_t)count);
            U32((uint32_t)LayerPaintCount);  // firstLayerIndex (uint32)
            LayerPaintCount += count;
            return at;
        }

        void SetLayerPaint(int slot, uint32_t paintAt)
        {
            if (slot >= 0 && slot < LayerPaintCount)
                LayerPaints[slot] = paintAt;
        }

        uint32_t PaintColrGlyph(uint16_t glyph)
        {
            const uint32_t at = Pos();
            U8(11);
            U16(glyph);
            return at;
        }

        uint32_t PaintUnknown(uint8_t format)
        {
            const uint32_t at = Pos();
            U8(format);
            return at;
        }

        // Lays out header + BaseGlyphList + LayerList (+ ClipList) + paints.
        Bytes Finish(uint16_t baseGlyph, uint32_t rootPaint, bool hasClip, int16_t cx0, int16_t cy0, int16_t cx1,
                     int16_t cy1)
        {
            const uint32_t headerSize = 34;
            const uint32_t baseGlyphListOffset = headerSize;
            const uint32_t baseGlyphListSize = 4 + 6;
            const uint32_t layerListOffset = baseGlyphListOffset + baseGlyphListSize;
            const uint32_t layerListSize = 4 + (uint32_t)LayerPaintCount * 4;
            const uint32_t clipListOffset = layerListOffset + layerListSize;
            const uint32_t clipListSize = hasClip ? (5 + 7 + 9) : 0;
            const uint32_t paintBase = clipListOffset + clipListSize;

            Bytes out;
            out.Resize(headerSize);
            WriteU16(out, 0, 1);   // version 1
            WriteU16(out, 2, 0);   // numBaseGlyphRecords = 0 (no v0 array)
            WriteU32(out, 4, 0);   // baseGlyphRecordsOffset
            WriteU32(out, 8, 0);   // layerRecordsOffset
            WriteU16(out, 12, 0);  // numLayerRecords
            WriteU32(out, 14, baseGlyphListOffset);
            WriteU32(out, 18, layerListOffset);
            WriteU32(out, 22, hasClip ? clipListOffset : 0);
            WriteU32(out, 26, 0);  // varIndexMapOffset
            WriteU32(out, 30, 0);  // itemVariationStoreOffset

            // BaseGlyphList
            out.Resize(baseGlyphListOffset + baseGlyphListSize);
            WriteU32(out, baseGlyphListOffset, 1);
            WriteU16(out, baseGlyphListOffset + 4, baseGlyph);
            WriteU32(out, baseGlyphListOffset + 6, paintBase + rootPaint - baseGlyphListOffset);

            // LayerList
            out.Resize(layerListOffset + layerListSize);
            WriteU32(out, layerListOffset, (uint32_t)LayerPaintCount);
            for (int i = 0; i < LayerPaintCount; ++i)
                WriteU32(out, layerListOffset + 4 + (size_t)i * 4, paintBase + LayerPaints[i] - layerListOffset);

            // ClipList (one record covering the base glyph)
            if (hasClip)
            {
                out.Resize(clipListOffset + clipListSize);
                out[clipListOffset] = 1;  // ClipListFormat1
                WriteU32(out, clipListOffset + 1, 1);
                WriteU16(out, clipListOffset + 5, baseGlyph);
                WriteU16(out, clipListOffset + 7, baseGlyph);
                const uint32_t boxOffset = 5 + 7;
                out[clipListOffset + 9] = (unsigned char)(boxOffset >> 16);
                out[clipListOffset + 10] = (unsigned char)(boxOffset >> 8);
                out[clipListOffset + 11] = (unsigned char)boxOffset;
                const uint32_t box = clipListOffset + boxOffset;
                out[box] = 1;  // ClipBoxFormat1
                WriteU16(out, box + 1, (uint32_t)(uint16_t)cx0);
                WriteU16(out, box + 3, (uint32_t)(uint16_t)cy0);
                WriteU16(out, box + 5, (uint32_t)(uint16_t)cx1);
                WriteU16(out, box + 7, (uint32_t)(uint16_t)cy1);
            }

            out.Append(Paints.Data, Paints.Size);
            return out;
        }
    };

    // ---------------------------------------------------------------------
    // Paint record helpers (sampler tests)
    // ---------------------------------------------------------------------
    ImSlugPaintRecord MakeSolidPaint(uint32_t rgba, float clipMinX, float clipMinY, float clipMaxX, float clipMaxY)
    {
        ImSlugPaintRecord paint = {};
        paint.Type = ImSlugColorPaintType_Solid;
        paint.StopCount = 1;
        paint.SolidColor = rgba;
        paint.Clip[0] = clipMinX;
        paint.Clip[1] = clipMinY;
        paint.Clip[2] = clipMaxX;
        paint.Clip[3] = clipMaxY;
        paint.Xform0[0] = 1.0f;
        paint.Xform1[1] = 1.0f;
        return paint;
    }

    void SetStop(ImSlugPaintRecord& paint, int index, float offset, uint32_t rgba)
    {
        const int raw = (int)lround(offset * 16384.0f);
        paint.Stops[2 * index] = (uint32_t)(uint16_t)(int16_t)raw;
        paint.Stops[2 * index + 1] = rgba;
    }

    void TestPaintSamplerSolidAndClip()
    {
        const uint32_t red = ImSlugPackColorRGBA8(1.0f, 0.0f, 0.0f, 1.0f);
        ImSlugPaintRecord paint = MakeSolidPaint(red, 0.0f, 0.0f, 0.0f, 0.0f);
        float rgba[4];
        ImSlugSamplePaintRecord(&paint, 0.5f, 0.5f, rgba);
        TEST_CHECK(rgba[0] == 1.0f && rgba[1] == 0.0f && rgba[2] == 0.0f && rgba[3] == 1.0f);

        // Clip disabled (max <= min): sampled everywhere.
        ImSlugSamplePaintRecord(&paint, -5.0f, 9.0f, rgba);
        TEST_CHECK(rgba[3] == 1.0f);

        // Clip enabled: outside -> transparent.
        paint = MakeSolidPaint(red, -0.25f, -0.25f, 0.25f, 0.25f);
        ImSlugSamplePaintRecord(&paint, 0.5f, 0.0f, rgba);
        TEST_CHECK(rgba[3] == 0.0f);
        ImSlugSamplePaintRecord(&paint, 0.25f, 0.25f, rgba);
        TEST_CHECK(rgba[3] == 1.0f);
    }

    void TestPaintSamplerLinear()
    {
        // p0 = (0,0), p1 = (1,0), p2 = (0,1): iso-lines parallel to p0p2
        // (vertical), so t = x.
        ImSlugPaintRecord paint = MakeSolidPaint(0, 0, 0, 0, 0);
        paint.Type = ImSlugColorPaintType_LinearGradient;
        paint.Extend = ImSlugColorExtend_Pad;
        paint.StopCount = 2;
        SetStop(paint, 0, 0.0f, ImSlugPackColorRGBA8(0.0f, 0.0f, 0.0f, 1.0f));  // black
        SetStop(paint, 1, 1.0f, ImSlugPackColorRGBA8(1.0f, 1.0f, 1.0f, 1.0f));  // white
        paint.Geo0[0] = 0.0f;
        paint.Geo0[1] = 0.0f;
        paint.Geo0[2] = 1.0f;
        paint.Geo0[3] = 0.0f;
        paint.Geo1[0] = 0.0f;
        paint.Geo1[1] = 1.0f;

        float rgba[4];
        ImSlugSamplePaintRecord(&paint, 0.5f, 0.9f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.01f, "linear t=0.5 -> %f", rgba[0]);
        ImSlugSamplePaintRecord(&paint, 0.0f, 0.5f, rgba);
        TEST_CHECK(rgba[0] == 0.0f);
        ImSlugSamplePaintRecord(&paint, 1.0f, -3.0f, rgba);
        TEST_CHECK(rgba[0] == 1.0f);
        // Pad beyond both ends.
        ImSlugSamplePaintRecord(&paint, -2.0f, 0.0f, rgba);
        TEST_CHECK(rgba[0] == 0.0f);
        ImSlugSamplePaintRecord(&paint, 3.0f, 0.0f, rgba);
        TEST_CHECK(rgba[0] == 1.0f);

        // Repeat: t wraps; -0.25 -> 0.75.
        paint.Extend = ImSlugColorExtend_Repeat;
        ImSlugSamplePaintRecord(&paint, -0.25f, 0.0f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 0.75f) < 0.01f, "repeat t=-0.25 -> %f", rgba[0]);
        // Reflect: t=1.75 -> 0.25.
        paint.Extend = ImSlugColorExtend_Reflect;
        ImSlugSamplePaintRecord(&paint, 1.75f, 0.0f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 0.25f) < 0.01f, "reflect t=1.75 -> %f", rgba[0]);

        // Rotated iso-lines: p2 = (1,1) makes the iso-lines parallel to
        // (1,1), so t = (x - y).
        paint.Extend = ImSlugColorExtend_Pad;
        paint.Geo1[0] = 1.0f;
        paint.Geo1[1] = 1.0f;
        ImSlugSamplePaintRecord(&paint, 0.75f, 0.25f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.01f, "rotated t=x-y -> %f", rgba[0]);

        // Ill-formed (p0 == p2): nothing is painted.
        paint.Geo1[0] = 0.0f;
        paint.Geo1[1] = 0.0f;
        ImSlugSamplePaintRecord(&paint, 0.5f, 0.5f, rgba);
        TEST_CHECK(rgba[3] == 0.0f);

        // The paint transform maps record space into paint space.
        paint.Geo1[0] = 0.0f;
        paint.Geo1[1] = 1.0f;
        paint.Xform0[2] = -0.5f;  // local = record - 0.5 -> t = x - 0.5
        ImSlugSamplePaintRecord(&paint, 0.5f, 0.0f, rgba);
        TEST_CHECK(rgba[0] == 0.0f);
        ImSlugSamplePaintRecord(&paint, 1.0f, 0.0f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.01f, "transformed linear -> %f", rgba[0]);
    }

    void TestPaintSamplerRadial()
    {
        // c0 = (0,0), r0 = 0, c1 = (1,0), r1 = 1: circles centred on the axis
        // with radius = distance from the origin, so a point at x is on the
        // circle omega = x/2.
        ImSlugPaintRecord paint = MakeSolidPaint(0, 0, 0, 0, 0);
        paint.Type = ImSlugColorPaintType_RadialGradient;
        paint.Extend = ImSlugColorExtend_Pad;
        paint.StopCount = 2;
        SetStop(paint, 0, 0.0f, ImSlugPackColorRGBA8(0.0f, 0.0f, 0.0f, 1.0f));
        SetStop(paint, 1, 1.0f, ImSlugPackColorRGBA8(1.0f, 1.0f, 1.0f, 1.0f));
        paint.Geo0[0] = 0.0f;
        paint.Geo0[1] = 0.0f;
        paint.Geo0[2] = 0.0f;
        paint.Geo1[0] = 1.0f;
        paint.Geo1[1] = 0.0f;
        paint.Geo1[2] = 1.0f;

        float rgba[4];
        ImSlugSamplePaintRecord(&paint, 1.0f, 0.0f, rgba);  // omega = 0.5
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.02f, "radial omega=0.5 -> %f", rgba[0]);
        ImSlugSamplePaintRecord(&paint, 2.0f, 0.0f, rgba);  // omega = 1
        TEST_CHECKF(fabsf(rgba[0] - 1.0f) < 0.02f, "radial omega=1 -> %f", rgba[0]);
        // Pad beyond the last circle.
        ImSlugSamplePaintRecord(&paint, 4.0f, 0.0f, rgba);
        TEST_CHECKF(fabsf(rgba[0] - 1.0f) < 0.02f, "radial pad -> %f", rgba[0]);
        // Outside the cone (no circle passes through): nothing painted.
        ImSlugSamplePaintRecord(&paint, 0.0f, 2.0f, rgba);
        TEST_CHECK(rgba[3] == 0.0f);

        // Equal circles: nothing painted.
        paint.Geo1[0] = 0.0f;
        paint.Geo1[2] = 0.0f;
        ImSlugSamplePaintRecord(&paint, 0.0f, 0.0f, rgba);
        TEST_CHECK(rgba[3] == 0.0f);
    }

    void TestPaintSamplerSweep()
    {
        // Centre (0,0), 0..360 degrees counter-clockwise: t = angle/360.
        ImSlugPaintRecord paint = MakeSolidPaint(0, 0, 0, 0, 0);
        paint.Type = ImSlugColorPaintType_SweepGradient;
        paint.Extend = ImSlugColorExtend_Pad;
        paint.StopCount = 2;
        SetStop(paint, 0, 0.0f, ImSlugPackColorRGBA8(0.0f, 0.0f, 0.0f, 1.0f));
        SetStop(paint, 1, 1.0f, ImSlugPackColorRGBA8(1.0f, 1.0f, 1.0f, 1.0f));
        paint.Geo0[2] = 0.0f;
        paint.Geo0[3] = 360.0f;

        float rgba[4];
        ImSlugSamplePaintRecord(&paint, 1.0f, 0.0f, rgba);  // 0 degrees
        TEST_CHECKF(rgba[0] < 0.01f, "sweep 0deg -> %f", rgba[0]);
        ImSlugSamplePaintRecord(&paint, 0.0f, 1.0f, rgba);  // 90 degrees
        TEST_CHECKF(fabsf(rgba[0] - 0.25f) < 0.01f, "sweep 90deg -> %f", rgba[0]);
        ImSlugSamplePaintRecord(&paint, -1.0f, 0.0f, rgba);  // 180 degrees
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.01f, "sweep 180deg -> %f", rgba[0]);

        // 90..270: t = (theta - 90)/180.
        paint.Geo0[2] = 90.0f;
        paint.Geo0[3] = 270.0f;
        ImSlugSamplePaintRecord(&paint, -1.0f, 0.0f, rgba);  // 180 -> 0.5
        TEST_CHECKF(fabsf(rgba[0] - 0.5f) < 0.01f, "sweep span 90..270 -> %f", rgba[0]);

        // Equal angles with repeat: nothing.
        paint.Geo0[2] = 45.0f;
        paint.Geo0[3] = 45.0f;
        paint.Extend = ImSlugColorExtend_Repeat;
        ImSlugSamplePaintRecord(&paint, 0.0f, 1.0f, rgba);
        TEST_CHECK(rgba[3] == 0.0f);
    }

    // ---------------------------------------------------------------------
    // Synthetic-font tests (real Roboto outlines + hand-built color tables)
    // ---------------------------------------------------------------------
    Bytes LoadRoboto()
    {
        int size = 0;
        unsigned char* data = imslug_test::LoadFontFile("Roboto-Regular.ttf", &size);
        Bytes bytes;
        if (data != nullptr)
        {
            bytes.Append(data, (size_t)size);
            std::free(data);
        }
        return bytes;
    }

    void TestSyntheticV0()
    {
        Bytes roboto = LoadRoboto();
        if (roboto.Size == 0)
        {
            std::printf("  [colr] Roboto missing - skipping the synthetic tests\n");
            return;
        }
        ImSlugFont* probe = ImSlugCreateFontFromMemory(roboto.Data, (int)roboto.Size, 0, true);
        TEST_CHECK(probe != nullptr);
        if (probe == nullptr)
            return;
        const int glyphA = ImSlugGetGlyphIndex(probe, 'A');
        const int glyphB = ImSlugGetGlyphIndex(probe, 'B');
        const int glyphC = ImSlugGetGlyphIndex(probe, 'C');
        ImSlugDestroyFont(probe);
        TEST_CHECK(glyphA > 0 && glyphB > 0 && glyphC > 0);

        // Palette (r | g<<8 | b<<16 | a<<24): red opaque, green half-alpha,
        // blue opaque, plus a foreground reference (0xFFFF) in the layer list.
        const uint32_t palette[3] = { 0xFF0000FFu, 0x8000FF00u, 0xFFFF0000u };
        const Bytes cpal = BuildCpal(palette, 3);
        const V0Layer v0Layers[2] = { { (uint16_t)glyphB, 0 }, { (uint16_t)glyphC, 0xFFFF } };
        const Bytes colr = BuildColrV0((uint16_t)glyphA, v0Layers, 2);
        const Bytes fontBytes = BuildFontWithTables(roboto, colr, cpal);

        ImSlugFont* font = ImSlugCreateFontFromMemory(fontBytes.Data, (int)fontBytes.Size, 0, true);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;

        ImSlugColorFont* colorFont = ImSlugFontGetColor(font);
        TEST_CHECK(colorFont != nullptr);
        ImSlugColorFontStats stats = {};
        ImSlugColorFontGetStats(colorFont, &stats);
        TEST_CHECK(stats.HasColrV0 == 1 && stats.HasColrV1 == 0);
        TEST_CHECK(stats.NumPalettes == 1 && stats.NumPaletteEntries == 3);
        TEST_CHECK(ImSlugColorFontHasGlyph(colorFont, glyphA));
        TEST_CHECK(!ImSlugColorFontHasGlyph(colorFont, glyphB));
        TEST_CHECK(!ImSlugColorFontHasGlyph(colorFont, 0));

        ImSlugPools pools;
        ImSlugInitPools(&pools);
        const ImSlugColorGlyph* colorGlyph = nullptr;
        const ImSlugBuildStatus status = ImSlugGetColorGlyph(font, &pools, glyphA, &colorGlyph);
        TEST_CHECK(status == ImSlugBuildStatus_Ok && colorGlyph != nullptr);
        if (colorGlyph != nullptr)
        {
            TEST_CHECK(colorGlyph->LayerCount == 2);
            TEST_CHECK(colorGlyph->MinX < colorGlyph->MaxX && colorGlyph->MinY < colorGlyph->MaxY);
            // v0 layers reuse the plain glyph records with identity transforms.
            for (int i = 0; i < colorGlyph->LayerCount; ++i)
            {
                const ImSlugColorLayer& layer = colorGlyph->Layers[i];
                TEST_CHECK(layer.Record != nullptr && layer.Record->CurveCount > 0);
                TEST_CHECK(layer.ScaleX == 1.0f && layer.ScaleY == 1.0f);
                TEST_CHECK(layer.OffsetXEm == 0.0f && layer.OffsetYEm == 0.0f);
            }
            // Colors: layer 0 = palette 0 (red, opaque), layer 1 = foreground
            // (white; ImGui forces RGB=255 for Colored glyphs), opaque.
            const ImSlugPaintRecord* paint0 = ImSlugPaintRecordAt(&pools.Paints, (int)colorGlyph->Layers[0].PaintIndex);
            const ImSlugPaintRecord* paint1 = ImSlugPaintRecordAt(&pools.Paints, (int)colorGlyph->Layers[1].PaintIndex);
            TEST_CHECK(paint0 != nullptr && paint1 != nullptr);
            if (paint0 != nullptr && paint1 != nullptr)
            {
                TEST_CHECK(paint0->Type == ImSlugColorPaintType_Solid);
                TEST_CHECK((paint0->SolidColor & 0xFFu) == 255u);
                TEST_CHECK(((paint0->SolidColor >> 24) & 0xFFu) == 255u);
                TEST_CHECK((paint1->SolidColor & 0xFFu) == 255u && ((paint1->SolidColor >> 8) & 0xFFu) == 255u &&
                           ((paint1->SolidColor >> 16) & 0xFFu) == 255u);
            }
            // Instances: one per layer, paint index set, tint alpha carried.
            ImSlugGlyphInstance instances[8];
            int layerIndices[8];
            const int count =
                ImSlugBuildColorGlyphInstances(colorGlyph, 10.0f, 20.0f, 32.0f, 0.5f, instances, 8, layerIndices);
            TEST_CHECK(count == 2);
            TEST_CHECK(instances[0].PaintIndex == colorGlyph->Layers[0].PaintIndex);
            TEST_CHECK(instances[1].PaintIndex == colorGlyph->Layers[1].PaintIndex);
            TEST_CHECK(instances[0].ColorA == 0.5f && instances[0].ColorR == 1.0f);
            TEST_CHECK(layerIndices[0] == 0 && layerIndices[1] == 1);
        }
        ImSlugDestroyPools(&pools);
        ImSlugDestroyFont(font);
    }

    void TestSyntheticV1()
    {
        Bytes roboto = LoadRoboto();
        if (roboto.Size == 0)
            return;
        ImSlugFont* probe = ImSlugCreateFontFromMemory(roboto.Data, (int)roboto.Size, 0, true);
        if (probe == nullptr)
            return;
        const int glyphA = ImSlugGetGlyphIndex(probe, 'A');
        const int glyphB = ImSlugGetGlyphIndex(probe, 'B');
        ImSlugDestroyFont(probe);

        ColrV1Builder builder;
        const ColrV1Builder::StopSpec stops[2] = { { 0, 0, 255 }, { 16384, 1, 255 } };
        // The root first (children follow their parents in the blob).
        const int rootFirstLayer = builder.LayerPaintCount;
        const uint32_t root = builder.PaintColrLayersBegin(6);

        // Layer 0: solid glyph.
        const uint32_t layer0 = builder.PaintGlyphBegin((uint16_t)glyphB);
        builder.AttachPaint(layer0, builder.PaintSolid(1, 0.75f));
        builder.SetLayerPaint(rootFirstLayer + 0, layer0);

        // Layer 1: glyph filled by a linear gradient, scaled 2x and translated;
        // the gradient itself is translated *inside* the fill (the Noto
        // PaintGlyph -> PaintTransform -> gradient shape), so the paint
        // transform is the inverse of that inner translate.
        const uint32_t layer1 = builder.PaintTransformBegin();
        const uint32_t layer1Glyph = builder.PaintGlyphBegin((uint16_t)glyphB);
        const uint32_t fillShift = builder.PaintTranslateBegin();
        builder.AttachPaint(fillShift, builder.PaintLinear(stops, 2, 0, 0, 512, 0, 0, 512));
        builder.AttachTranslate(fillShift, 128, 64);
        builder.AttachPaint(layer1Glyph, fillShift);
        const uint32_t affine1 = builder.AppendAffine(2.0f, 0.0f, 0.0f, 2.0f, 64.0f, -32.0f);
        builder.AttachTransform(layer1, layer1Glyph, affine1);
        builder.SetLayerPaint(rootFirstLayer + 1, layer1);

        // Layer 2: src_in composite with a constant 0.5-alpha backdrop folds
        // into the layer alpha.
        const uint32_t composite = builder.PaintCompositeBegin(5 /* src_in */);
        const uint32_t maskSource = builder.PaintGlyphBegin((uint16_t)glyphA);
        builder.AttachPaint(maskSource, builder.PaintSolid(2, 1.0f));
        builder.AttachComposite(composite, maskSource, builder.PaintSolid(0, 0.5f));
        builder.SetLayerPaint(rootFirstLayer + 2, composite);

        // Layer 3: a sweep gradient (exercises the angle path).
        const uint32_t layer3 = builder.PaintGlyphBegin((uint16_t)glyphB);
        builder.AttachPaint(layer3, builder.PaintSweep(stops, 2, 0, 0, 0.0f, 360.0f));
        builder.SetLayerPaint(rootFirstLayer + 3, layer3);

        // Layer 4: unsupported paint format (skipped).
        const uint32_t layer4 = builder.PaintGlyphBegin((uint16_t)glyphB);
        builder.AttachPaint(layer4, builder.PaintUnknown(33));
        builder.SetLayerPaint(rootFirstLayer + 4, layer4);

        // Layer 5: a cyclic PaintColrGlyph reference (the guard must not hang).
        builder.SetLayerPaint(rootFirstLayer + 5, builder.PaintColrGlyph((uint16_t)glyphA));

        const Bytes colr = builder.Finish((uint16_t)glyphA, root, true, 0, -900, 1024, 900);
        const uint32_t palette[3] = { 0xFF302010u, 0xFF0000FFu, 0xFF00FF00u };  // r|g<<8|b<<16|a<<24
        const Bytes fontBytes = BuildFontWithTables(roboto, colr, BuildCpal(palette, 3));
        // Debug aid: dump the synthetic font when requested.
        const char* dumpPath = std::getenv("IMSLUG_COLR_DUMP");
        if (dumpPath != nullptr)
        {
            FILE* dump = std::fopen(dumpPath, "wb");
            if (dump != nullptr)
            {
                std::fwrite(fontBytes.Data, 1, fontBytes.Size, dump);
                std::fclose(dump);
            }
        }

        ImSlugFont* font = ImSlugCreateFontFromMemory(fontBytes.Data, (int)fontBytes.Size, 0, true);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;
        ImSlugColorFont* colorFont = ImSlugFontGetColor(font);
        TEST_CHECK(colorFont != nullptr);
        ImSlugColorFontStats stats = {};
        ImSlugColorFontGetStats(colorFont, &stats);
        TEST_CHECK(stats.HasColrV1 == 1 && stats.HasClipList == 1);
        TEST_CHECK(ImSlugColorFontHasGlyph(colorFont, glyphA));

        ImSlugPools pools;
        ImSlugInitPools(&pools);
        const ImSlugColorGlyph* colorGlyph = nullptr;
        const ImSlugBuildStatus status = ImSlugGetColorGlyph(font, &pools, glyphA, &colorGlyph);
        TEST_CHECK(status == ImSlugBuildStatus_Ok && colorGlyph != nullptr);
        if (colorGlyph != nullptr)
        {
            // 6 graph layers, minus the unsupported one (skipped) and the
            // cyclic reference (skipped): 4.
            TEST_CHECKF(colorGlyph->LayerCount == 4, "layers=%d", colorGlyph->LayerCount);

            // Layer 0: solid, alpha 0.75.
            const ImSlugPaintRecord* solidPaint =
                ImSlugPaintRecordAt(&pools.Paints, (int)colorGlyph->Layers[0].PaintIndex);
            TEST_CHECK(solidPaint != nullptr && solidPaint->Type == ImSlugColorPaintType_Solid);
            if (solidPaint != nullptr)
                TEST_CHECK(((solidPaint->SolidColor >> 24) & 0xFFu) == 191u);  // 0.75 * 255

            // Layer 1: linear gradient; the scale+translate transform is
            // axis-aligned positive, so the glyph record is reused with a 2x
            // scale mapping and the paint transform is the inverse of the
            // gradient's own (inner) translate: record -> paint-local.
            const ImSlugColorLayer& gradientLayer = colorGlyph->Layers[1];
            TEST_CHECK(gradientLayer.ScaleX == 2.0f && gradientLayer.ScaleY == 2.0f);
            const ImSlugPaintRecord* linearPaint = ImSlugPaintRecordAt(&pools.Paints, (int)gradientLayer.PaintIndex);
            TEST_CHECK(linearPaint != nullptr && linearPaint->Type == ImSlugColorPaintType_LinearGradient);
            if (linearPaint != nullptr)
            {
                const float upem = 2048.0f;  // Roboto's unitsPerEm
                TEST_CHECKF(fabsf(linearPaint->Xform0[0] - 1.0f) < 1e-4f, "xform a=%f", linearPaint->Xform0[0]);
                TEST_CHECKF(fabsf(linearPaint->Xform0[1]) < 1e-4f, "xform b=%f", linearPaint->Xform0[1]);
                TEST_CHECKF(fabsf(linearPaint->Xform0[2] + 128.0f / upem) < 1e-4f, "xform tx=%f",
                            linearPaint->Xform0[2]);
                TEST_CHECKF(fabsf(linearPaint->Xform1[2] + 64.0f / upem) < 1e-4f, "xform ty=%f",
                            linearPaint->Xform1[2]);
                // Geometry in em (font units / upem).
                TEST_CHECKF(fabsf(linearPaint->Geo0[2] - 512.0f / upem) < 1e-5f, "geo p1x=%f", linearPaint->Geo0[2]);
                // ClipBox (0,-900)..(1024,900) font units -> base em -> record
                // space (the inverse of the layer's 2x scale + translate).
                const float expectedClipMinX = (0.0f / upem - 64.0f / upem) / 2.0f;
                const float expectedClipMaxX = (1024.0f / upem - 64.0f / upem) / 2.0f;
                TEST_CHECKF(fabsf(linearPaint->Clip[0] - expectedClipMinX) < 1e-4f, "clip minx=%f",
                            linearPaint->Clip[0]);
                TEST_CHECKF(fabsf(linearPaint->Clip[2] - expectedClipMaxX) < 1e-4f, "clip maxx=%f",
                            linearPaint->Clip[2]);
                TEST_CHECK(linearPaint->Clip[3] > linearPaint->Clip[1]);
            }

            // Layer 2: the src_in composite folds the 0.5 backdrop alpha into
            // the source layer (solid palette 2 alpha 1.0 -> round(0.5*255)).
            const ImSlugPaintRecord* foldedPaint =
                ImSlugPaintRecordAt(&pools.Paints, (int)colorGlyph->Layers[2].PaintIndex);
            TEST_CHECK(foldedPaint != nullptr && foldedPaint->Type == ImSlugColorPaintType_Solid);
            if (foldedPaint != nullptr)
                TEST_CHECKF(((foldedPaint->SolidColor >> 24) & 0xFFu) == 128u, "folded alpha=%u",
                            (unsigned)((foldedPaint->SolidColor >> 24) & 0xFFu));

            // Layer 3: sweep gradient with the spec's BiasedAngle conversion.
            const ImSlugPaintRecord* sweepPaint =
                ImSlugPaintRecordAt(&pools.Paints, (int)colorGlyph->Layers[3].PaintIndex);
            TEST_CHECK(sweepPaint != nullptr && sweepPaint->Type == ImSlugColorPaintType_SweepGradient);
            if (sweepPaint != nullptr)
            {
                TEST_CHECKF(fabsf(sweepPaint->Geo0[2] - 0.0f) < 1e-3f, "start=%f", sweepPaint->Geo0[2]);
                TEST_CHECKF(fabsf(sweepPaint->Geo0[3] - 360.0f) < 1e-3f, "end=%f", sweepPaint->Geo0[3]);
            }

            // Every layer's record is drawable and the union bbox covers them.
            for (int i = 0; i < colorGlyph->LayerCount; ++i)
            {
                const ImSlugColorLayer& layer = colorGlyph->Layers[i];
                TEST_CHECK(layer.Record != nullptr && layer.Record->CurveCount > 0);
                const float minX = layer.OffsetXEm + layer.Record->MinX * layer.ScaleX;
                const float maxX = layer.OffsetXEm + layer.Record->MaxX * layer.ScaleX;
                TEST_CHECK(minX >= colorGlyph->MinX - 1e-4f && maxX <= colorGlyph->MaxX + 1e-4f);
            }
        }

        // A second call is served from the cache (same pointer, no rebuild).
        const ImSlugColorGlyph* cached = nullptr;
        TEST_CHECK(ImSlugGetColorGlyph(font, &pools, glyphA, &cached) == ImSlugBuildStatus_Ok);
        TEST_CHECK(cached == colorGlyph);
        const int paintCountBefore = pools.Paints.Count;
        TEST_CHECK(ImSlugGetColorGlyph(font, &pools, glyphA, &cached) == ImSlugBuildStatus_Ok);
        TEST_CHECK(pools.Paints.Count == paintCountBefore);

        ImSlugDestroyPools(&pools);
        ImSlugDestroyFont(font);
    }

    // A truncated/garbage COLR table must not crash and must report "no color
    // glyph" (the monochrome fallback).
    void TestMalformedTables()
    {
        Bytes roboto = LoadRoboto();
        if (roboto.Size == 0)
            return;
        ImSlugFont* probe = ImSlugCreateFontFromMemory(roboto.Data, (int)roboto.Size, 0, true);
        if (probe == nullptr)
            return;
        const int glyphA = ImSlugGetGlyphIndex(probe, 'A');
        ImSlugDestroyFont(probe);

        const uint32_t palette[1] = { 0xFF0000FFu };
        const Bytes cpal = BuildCpal(palette, 1);

        // COLR v1 header pointing at out-of-range offsets.
        {
            Bytes colr;
            colr.Resize(34);
            WriteU16(colr, 0, 1);
            WriteU32(colr, 14, 0xFFFFFF00u);
            WriteU32(colr, 18, 0xFFFFFF00u);
            const Bytes fontBytes = BuildFontWithTables(roboto, colr, cpal);
            ImSlugFont* font = ImSlugCreateFontFromMemory(fontBytes.Data, (int)fontBytes.Size, 0, true);
            TEST_CHECK(font != nullptr);
            if (font != nullptr)
            {
                TEST_CHECK(ImSlugFontGetColor(font) == nullptr);  // unusable table
                ImSlugDestroyFont(font);
            }
        }

        // A v0 table whose record count cannot fit the table: rejected.
        {
            const V0Layer layers[1] = { { (uint16_t)glyphA, 0 } };
            Bytes colr = BuildColrV0((uint16_t)glyphA, layers, 1);
            WriteU16(colr, 2, 5000);  // numBaseGlyphRecords
            const Bytes fontBytes = BuildFontWithTables(roboto, colr, cpal);
            ImSlugFont* font = ImSlugCreateFontFromMemory(fontBytes.Data, (int)fontBytes.Size, 0, true);
            TEST_CHECK(font != nullptr);
            if (font != nullptr)
            {
                TEST_CHECK(ImSlugFontGetColor(font) == nullptr);
                ImSlugDestroyFont(font);
            }
        }

        // A truncated v0 layer record: the table parses, but the glyph has no
        // drawable layers (the monochrome fallback), and nothing crashes.
        {
            const V0Layer layers[1] = { { (uint16_t)glyphA, 0 } };
            Bytes colr = BuildColrV0((uint16_t)glyphA, layers, 1);
            colr.Resize(20);  // the layer record (at 20) is cut off
            const Bytes fontBytes = BuildFontWithTables(roboto, colr, cpal);
            ImSlugFont* font = ImSlugCreateFontFromMemory(fontBytes.Data, (int)fontBytes.Size, 0, true);
            TEST_CHECK(font != nullptr);
            if (font != nullptr)
            {
                ImSlugPools pools;
                ImSlugInitPools(&pools);
                const ImSlugColorGlyph* colorGlyph = nullptr;
                const ImSlugBuildStatus status = ImSlugGetColorGlyph(font, &pools, glyphA, &colorGlyph);
                TEST_CHECK(status != ImSlugBuildStatus_Ok && colorGlyph == nullptr);
                ImSlugDestroyPools(&pools);
                ImSlugDestroyFont(font);
            }
        }
    }

    // ---------------------------------------------------------------------
    // Real-font structural checks (Noto Color Emoji, fetched asset)
    // ---------------------------------------------------------------------
    void TestRealFont()
    {
#ifdef IMSLUG_EMOJI_TEST_FONT
        int size = 0;
        unsigned char* data = imslug_test::LoadFile(IMSLUG_EMOJI_TEST_FONT, &size);
        if (data == nullptr)
        {
            std::printf("  [colr] emoji font not present - skipping (fetch with scripts/fetch_emoji_font.*)\n");
            return;
        }
        ImSlugFont* font = ImSlugCreateFontFromMemory(data, size, 0, true);
        std::free(data);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;

        ImSlugColorFont* colorFont = ImSlugFontGetColor(font);
        TEST_CHECK(colorFont != nullptr);
        if (colorFont == nullptr)
        {
            ImSlugDestroyFont(font);
            return;
        }
        ImSlugColorFontStats stats = {};
        ImSlugColorFontGetStats(colorFont, &stats);
        TEST_CHECKF(stats.HasColrV1 == 1, "Noto Color Emoji is COLR v1");
        TEST_CHECK(stats.HasClipList == 1);
        TEST_CHECK(stats.NumPalettes >= 1 && stats.NumPaletteEntries > 100);

        // U+1F600 (grinning face) and U+2764 (heart) are color glyphs.
        const int grinning = ImSlugGetGlyphIndex(font, 0x1F600u);
        const int heart = ImSlugGetGlyphIndex(font, 0x2764u);
        TEST_CHECK(grinning > 0 && heart > 0);
        TEST_CHECK(ImSlugColorFontHasGlyph(colorFont, grinning));
        TEST_CHECK(ImSlugColorFontHasGlyph(colorFont, heart));

        ImSlugPools pools;
        ImSlugInitPools(&pools);
        // The adapter's color path needs the base glyph record to build (it is
        // the monochrome fallback); Noto's base glyphs have empty glyf outlines,
        // so the record is a valid zero-curve record.
        const ImSlugGlyphRecord* baseRecord = nullptr;
        const ImSlugBuildStatus baseStatus = ImSlugGetGlyphRecord(font, &pools, 0x1F600u, &baseRecord);
        TEST_CHECKF(baseStatus == ImSlugBuildStatus_Ok && baseRecord != nullptr, "base record status %d",
                    (int)baseStatus);
        if (baseRecord != nullptr)
            TEST_CHECK(baseRecord->CurveCount == 0);
        const ImSlugColorGlyph* colorGlyph = nullptr;
        const ImSlugBuildStatus status = ImSlugGetColorGlyph(font, &pools, grinning, &colorGlyph);
        TEST_CHECK(status == ImSlugBuildStatus_Ok && colorGlyph != nullptr);
        if (colorGlyph != nullptr)
        {
            // The measured font: U+1F600 has 10 draw ops (verified with
            // fontTools). Allow a range so a font
            // update does not make this brittle.
            TEST_CHECKF(colorGlyph->LayerCount >= 5 && colorGlyph->LayerCount <= 40, "layers=%d",
                        colorGlyph->LayerCount);
            TEST_CHECK(colorGlyph->MinX < colorGlyph->MaxX && colorGlyph->MinY < colorGlyph->MaxY);
            // The union bbox must be covered by the reconstruction record (the
            // base glyf outline is empty for Noto, so the metrics box is
            // synthesized from the layers).
            TEST_CHECK(colorGlyph->BaseRecord.MinX <= colorGlyph->MinX + 1e-6f);
            TEST_CHECK(colorGlyph->BaseRecord.MaxX >= colorGlyph->MaxX - 1e-6f);
            TEST_CHECK(colorGlyph->BaseRecord.MinY <= colorGlyph->MinY + 1e-6f);
            TEST_CHECK(colorGlyph->BaseRecord.MaxY >= colorGlyph->MaxY - 1e-6f);

            int gradients = 0;
            int solids = 0;
            for (int i = 0; i < colorGlyph->LayerCount; ++i)
            {
                const ImSlugColorLayer& layer = colorGlyph->Layers[i];
                TEST_CHECK(layer.Record != nullptr && layer.Record->CurveCount > 0);
                TEST_CHECK(layer.ScaleX > 0.0f && layer.ScaleY > 0.0f);
                const ImSlugPaintRecord* paint = ImSlugPaintRecordAt(&pools.Paints, (int)layer.PaintIndex);
                TEST_CHECK(paint != nullptr);
                if (paint == nullptr)
                    continue;
                if (paint->Type == ImSlugColorPaintType_Solid)
                    ++solids;
                else
                    ++gradients;
                TEST_CHECK(paint->StopCount >= 1 && paint->StopCount <= (uint32_t)kMaxColorStops);
                TEST_CHECK(paint->Clip[2] >= paint->Clip[0] && paint->Clip[3] >= paint->Clip[1]);

                // Sample the paint at the record bbox centre: a valid color.
                const float cx = 0.5f * (layer.Record->MinX + layer.Record->MaxX);
                const float cy = 0.5f * (layer.Record->MinY + layer.Record->MaxY);
                float rgba[4];
                ImSlugSamplePaintRecord(paint, cx, cy, rgba);
                TEST_CHECK(rgba[0] >= 0.0f && rgba[0] <= 1.0f && rgba[3] >= 0.0f && rgba[3] <= 1.0f);
            }
            TEST_CHECKF(gradients + solids == colorGlyph->LayerCount, "grad=%d solid=%d", gradients, solids);
            TEST_CHECK(gradients > 0);
            std::printf("  [colr] Noto U+1F600: %d layers (%d gradients, %d solid), paint pool %d records\n",
                        colorGlyph->LayerCount, gradients, solids, pools.Paints.Count);
        }

        // A monochrome Latin glyph in the same font has no color data.
        const int latinA = ImSlugGetGlyphIndex(font, 'A');
        if (latinA > 0)
            TEST_CHECK(!ImSlugColorFontHasGlyph(colorFont, latinA));

        ImSlugDestroyPools(&pools);
        ImSlugDestroyFont(font);
#else
        std::printf("  [colr] IMSLUG_EMOJI_TEST_FONT not defined - skipping\n");
#endif
    }
    // ---------------------------------------------------------------------
    // Local-only structural check: Segoe UI Emoji (a system font, never
    // fetched/committed). It is a structurally different COLR v1 font: real
    // base-glyf outlines, rotations/skews in the transform chain, and
    // constant-mask src_in composites. Skipped when the file is absent (CI).
    // ---------------------------------------------------------------------
    void TestSegoeLocal()
    {
#ifdef IMSLUG_SEGOE_FONT
        int size = 0;
        unsigned char* data = imslug_test::LoadFile(IMSLUG_SEGOE_FONT, &size);
        if (data == nullptr)
        {
            std::printf("  [colr] Segoe UI Emoji not present - skipping (local manual check)\n");
            return;
        }
        ImSlugFont* font = ImSlugCreateFontFromMemory(data, size, 0, true);
        std::free(data);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;
        ImSlugColorFont* colorFont = ImSlugFontGetColor(font);
        TEST_CHECK(colorFont != nullptr);
        if (colorFont != nullptr)
        {
            ImSlugColorFontStats stats = {};
            ImSlugColorFontGetStats(colorFont, &stats);
            TEST_CHECK(stats.HasColrV1 == 1);

            ImSlugPools pools;
            ImSlugInitPools(&pools);
            int gradients = 0;
            int solids = 0;
            int totalLayers = 0;
            int checked = 0;
            // A spread of BMP emoji the system font covers.
            const unsigned int codepoints[] = { 0x2764, 0x2600, 0x26BD, 0x2B50, 0x2708 };
            for (unsigned int codepoint : codepoints)
            {
                const int glyphIndex = ImSlugGetGlyphIndex(font, codepoint);
                if (glyphIndex <= 0 || !ImSlugColorFontHasGlyph(colorFont, glyphIndex))
                    continue;
                const ImSlugColorGlyph* colorGlyph = nullptr;
                if (ImSlugGetColorGlyph(font, &pools, glyphIndex, &colorGlyph) != ImSlugBuildStatus_Ok ||
                    colorGlyph == nullptr)
                    continue;
                ++checked;
                totalLayers += colorGlyph->LayerCount;
                TEST_CHECK(colorGlyph->MinX < colorGlyph->MaxX && colorGlyph->MinY < colorGlyph->MaxY);
                for (int i = 0; i < colorGlyph->LayerCount; ++i)
                {
                    const ImSlugColorLayer& layer = colorGlyph->Layers[i];
                    TEST_CHECK(layer.Record != nullptr && layer.Record->CurveCount > 0);
                    const ImSlugPaintRecord* paint = ImSlugPaintRecordAt(&pools.Paints, (int)layer.PaintIndex);
                    TEST_CHECK(paint != nullptr);
                    if (paint == nullptr)
                        continue;
                    if (paint->Type == ImSlugColorPaintType_Solid)
                        ++solids;
                    else
                        ++gradients;
                    TEST_CHECK(paint->StopCount >= 1 && paint->StopCount <= (uint32_t)kMaxColorStops);
                    float rgba[4];
                    ImSlugSamplePaintRecord(paint, 0.5f * (layer.Record->MinX + layer.Record->MaxX),
                                            0.5f * (layer.Record->MinY + layer.Record->MaxY), rgba);
                    TEST_CHECK(rgba[3] >= 0.0f && rgba[3] <= 1.0f);
                }
            }
            TEST_CHECKF(checked >= 3, "only %d Segoe emoji resolved", checked);
            TEST_CHECK(gradients > 0);
            std::printf("  [colr] Segoe UI Emoji (local): %d glyphs, %d layers (%d gradients, %d solids)\n", checked,
                        totalLayers, gradients, solids);
            ImSlugDestroyPools(&pools);
        }
        ImSlugDestroyFont(font);
#else
        std::printf("  [colr] IMSLUG_SEGOE_FONT not defined - skipping the local Segoe check\n");
#endif
    }
}  // namespace

int main()
{
    TestPaintSamplerSolidAndClip();
    TestPaintSamplerLinear();
    TestPaintSamplerRadial();
    TestPaintSamplerSweep();
    TestSyntheticV0();
    TestSyntheticV1();
    TestMalformedTables();
    TestRealFont();
    TestSegoeLocal();
    return imslug_test::Finish("imslug_colr");
}
