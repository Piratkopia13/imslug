// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packer.cpp — shared curve/band pool storage: allocation with growth by
// doubling, row-wrap addressing (CalcBandLoc), dirty-row tracking and the
// capacity guards behind the graceful-fallback contract.
//
// All writes go through the helpers here so dirty rows and UsedRows can never
// drift from the actual texel writes.

#include "imslug_packer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // IEEE binary16 conversion (round to nearest even)
    // ---------------------------------------------------------------------
    uint16_t ImSlugFloatToHalf(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));

        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
        const uint32_t mantissa = bits & 0x7FFFFFu;

        if (exponent <= 0)
        {
            // Zero, subnormal or underflow.
            if (exponent < -10)
                return (uint16_t)sign;
            const uint32_t shifted = mantissa | 0x800000u;
            const uint32_t shift = (uint32_t)(14 - exponent);
            uint32_t half = shifted >> shift;
            const uint32_t roundBit = (shifted >> (shift - 1)) & 1u;
            const uint32_t sticky = (shifted & ((1u << (shift - 1)) - 1u)) != 0;
            half += (roundBit & (sticky | (half & 1u)));
            return (uint16_t)(sign | half);
        }
        if (exponent >= 31)
        {
            // Infinity or NaN (keep NaN payloads quiet).
            const bool isNaN = ((bits >> 23) & 0xFFu) == 0xFFu && mantissa != 0;
            return (uint16_t)(sign | (isNaN ? 0x7E00u : 0x7C00u));
        }

        uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
        const uint32_t roundBit = (mantissa >> 12) & 1u;
        const uint32_t sticky = (mantissa & 0xFFFu) != 0;
        half += (roundBit & (sticky | (half & 1u)));
        return (uint16_t)half;
    }

    float ImSlugHalfToFloat(uint16_t value)
    {
        const uint32_t sign = (uint32_t)(value & 0x8000u) << 16;
        uint32_t exponent = (value >> 10) & 0x1Fu;
        uint32_t mantissa = value & 0x3FFu;
        uint32_t bits;

        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign;
            }
            else
            {
                // Subnormal: value = mantissa * 2^-24; normalize so the float's
                // implicit bit becomes explicit.
                exponent = 127 - 14;
                mantissa <<= 13;
                while ((mantissa & 0x800000u) == 0)
                {
                    mantissa <<= 1;
                    exponent--;
                }
                mantissa &= 0x7FFFFFu;
                bits = sign | (exponent << 23) | mantissa;
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }

        float result;
        memcpy(&result, &bits, sizeof(result));
        return result;
    }

    // ---------------------------------------------------------------------
    // Pool growth and dirty tracking
    // ---------------------------------------------------------------------
    // First allocation: 32 rows, then doubling.
    static const int kInitialPoolRows = 32;

    static bool EnsureRows(uint16_t** texels, int* height, int rows, size_t bytesPerTexel)
    {
        if (rows <= *height)
            return true;
        if (rows > kPoolMaxHeight)
            return false;

        int newHeight = (*height > 0) ? *height : kInitialPoolRows;
        while (newHeight < rows)
            newHeight *= 2;
        if (newHeight > kPoolMaxHeight)
            newHeight = kPoolMaxHeight;

        const size_t oldBytes = (size_t)kPoolWidth * (size_t)(*height) * bytesPerTexel;
        const size_t newBytes = (size_t)kPoolWidth * (size_t)newHeight * bytesPerTexel;
        void* newData = realloc(*texels, newBytes);
        if (newData == NULL)
            return false;

        memset((unsigned char*)newData + oldBytes, 0, newBytes - oldBytes);
        *texels = (uint16_t*)newData;
        *height = newHeight;
        return true;
    }

    bool ImSlugPoolEnsureCurveRows(ImSlugCurvePool* pool, int rows)
    {
        IMSLUG_ASSERT(pool != NULL);
        return EnsureRows(&pool->Texels, &pool->Height, rows, 4 * sizeof(uint16_t));
    }

    bool ImSlugPoolEnsureBandRows(ImSlugBandPool* pool, int rows)
    {
        IMSLUG_ASSERT(pool != NULL);
        return EnsureRows(&pool->Texels, &pool->Height, rows, 2 * sizeof(uint16_t));
    }

    static void MarkDirty(int* dirtyMin, int* dirtyMax, int* usedRows, int row)
    {
        if (*dirtyMin > *dirtyMax)
        {
            *dirtyMin = row;
            *dirtyMax = row;
        }
        else
        {
            if (row < *dirtyMin)
                *dirtyMin = row;
            if (row > *dirtyMax)
                *dirtyMax = row;
        }
        if (row + 1 > *usedRows)
            *usedRows = row + 1;
    }

    // ---------------------------------------------------------------------
    // Lifetime
    // ---------------------------------------------------------------------
    static void InitCurvePool(ImSlugCurvePool* pool)
    {
        pool->Texels = NULL;
        pool->Width = kPoolWidth;
        pool->Height = 0;
        pool->CursorTexels = 0;
        pool->UsedRows = 0;
        pool->DirtyRowMin = 1;
        pool->DirtyRowMax = 0;
    }

    static void InitBandPool(ImSlugBandPool* pool)
    {
        pool->Texels = NULL;
        pool->Width = kPoolWidth;
        pool->Height = 0;
        pool->CursorTexels = 0;
        pool->UsedRows = 0;
        pool->DirtyRowMin = 1;
        pool->DirtyRowMax = 0;
    }

    static void InitPaintPool(ImSlugPaintPool* pool)
    {
        pool->Bytes = NULL;
        pool->Capacity = 0;
        pool->Count = 0;
        pool->DirtyRecordMin = 1;
        pool->DirtyRecordMax = 0;
    }

    void ImSlugInitPools(ImSlugPools* pools)
    {
        IMSLUG_ASSERT(pools != NULL);
        InitCurvePool(&pools->Curves);
        InitBandPool(&pools->Bands);
        InitPaintPool(&pools->Paints);
    }

    void ImSlugDestroyPools(ImSlugPools* pools)
    {
        if (pools == NULL)
            return;
        free(pools->Curves.Texels);
        free(pools->Bands.Texels);
        free(pools->Paints.Bytes);
        ImSlugInitPools(pools);
    }

    void ImSlugCurvePoolClearDirtyRows(ImSlugCurvePool* pool)
    {
        IMSLUG_ASSERT(pool != NULL);
        if (pool == NULL)
            return;
        pool->DirtyRowMin = 1;
        pool->DirtyRowMax = 0;
    }

    void ImSlugBandPoolClearDirtyRows(ImSlugBandPool* pool)
    {
        IMSLUG_ASSERT(pool != NULL);
        if (pool == NULL)
            return;
        pool->DirtyRowMin = 1;
        pool->DirtyRowMax = 0;
    }

    void ImSlugPaintPoolClearDirtyRecords(ImSlugPaintPool* pool)
    {
        IMSLUG_ASSERT(pool != NULL);
        if (pool == NULL)
            return;
        pool->DirtyRecordMin = 1;
        pool->DirtyRecordMax = 0;
    }

    void ImSlugPoolClearDirtyRows(ImSlugPools* pools)
    {
        IMSLUG_ASSERT(pools != NULL);
        ImSlugCurvePoolClearDirtyRows(&pools->Curves);
        ImSlugBandPoolClearDirtyRows(&pools->Bands);
        ImSlugPaintPoolClearDirtyRecords(&pools->Paints);
    }

    // ---------------------------------------------------------------------
    // Paint pool
    // ---------------------------------------------------------------------
    static const int kInitialPaintRecords = 64;

    void ImSlugPaintPoolMarkDirty(ImSlugPaintPool* pool, int firstIndex, int count)
    {
        IMSLUG_ASSERT(pool != NULL && firstIndex >= 0 && count >= 0 && firstIndex + count <= pool->Count);
        if (pool == NULL || count <= 0)
            return;
        const int lastIndex = firstIndex + count - 1;
        if (pool->DirtyRecordMin > pool->DirtyRecordMax)
        {
            pool->DirtyRecordMin = firstIndex;
            pool->DirtyRecordMax = lastIndex;
        }
        else
        {
            if (firstIndex < pool->DirtyRecordMin)
                pool->DirtyRecordMin = firstIndex;
            if (lastIndex > pool->DirtyRecordMax)
                pool->DirtyRecordMax = lastIndex;
        }
    }

    int ImSlugAllocPaintRecords(ImSlugPaintPool* pool, int count)
    {
        IMSLUG_ASSERT(pool != NULL && count > 0);
        if (pool == NULL || count <= 0)
            return -1;
        if (pool->Count + count > pool->Capacity)
        {
            int newCapacity = pool->Capacity > 0 ? pool->Capacity : kInitialPaintRecords;
            while (newCapacity < pool->Count + count)
                newCapacity *= 2;
            void* grown = realloc(pool->Bytes, (size_t)newCapacity * sizeof(ImSlugPaintRecord));
            if (grown == NULL)
                return -1;
            memset((unsigned char*)grown + (size_t)pool->Capacity * sizeof(ImSlugPaintRecord), 0,
                   (size_t)(newCapacity - pool->Capacity) * sizeof(ImSlugPaintRecord));
            pool->Bytes = (uint8_t*)grown;
            pool->Capacity = newCapacity;
        }
        const int first = pool->Count;
        pool->Count += count;
        ImSlugPaintPoolMarkDirty(pool, first, count);
        return first;
    }

    ImSlugPaintRecord* ImSlugPaintRecordAt(ImSlugPaintPool* pool, int index)
    {
        IMSLUG_ASSERT(pool != NULL && index >= 0 && index < pool->Count);
        if (pool == NULL || index < 0 || index >= pool->Count)
            return NULL;
        return (ImSlugPaintRecord*)(pool->Bytes + (size_t)index * sizeof(ImSlugPaintRecord));
    }

    const ImSlugPaintRecord* ImSlugPaintRecordAt(const ImSlugPaintPool* pool, int index)
    {
        IMSLUG_ASSERT(pool != NULL && index >= 0 && index < pool->Count);
        if (pool == NULL || index < 0 || index >= pool->Count)
            return NULL;
        return (const ImSlugPaintRecord*)(pool->Bytes + (size_t)index * sizeof(ImSlugPaintRecord));
    }

    // ---------------------------------------------------------------------
    // Curve pool
    // ---------------------------------------------------------------------
    bool ImSlugAllocCurveBlock(ImSlugCurvePool* pool, int curveCount, ImSlugCurveBlock* outBlock)
    {
        IMSLUG_ASSERT(pool != NULL && outBlock != NULL);
        IMSLUG_ASSERT((pool->CursorTexels & 1) == 0);  // 2-texel alignment is an invariant
        if (curveCount < 0)
            return false;

        const int totalTexels = curveCount * 2;
        const int startTexels = pool->CursorTexels;
        const int endTexels = startTexels + totalTexels;
        if (totalTexels > 0)
        {
            const int lastRow = (endTexels - 1) / kPoolWidth;
            if (lastRow >= kPoolMaxHeight)
                return false;
            if (!ImSlugPoolEnsureCurveRows(pool, lastRow + 1))
                return false;
        }

        outBlock->X = startTexels & (kPoolWidth - 1);
        outBlock->Y = startTexels >> kPoolWidthLog2;
        outBlock->CurveCount = curveCount;
        pool->CursorTexels = endTexels;
        return true;
    }

    void ImSlugCurveLocAt(const ImSlugCurveBlock& block, int curveIndex, int* outX, int* outY)
    {
        IMSLUG_ASSERT(curveIndex >= 0 && curveIndex < block.CurveCount);
        const int linear = block.Y * kPoolWidth + block.X + curveIndex * 2;
        *outX = linear & (kPoolWidth - 1);
        *outY = linear >> kPoolWidthLog2;
    }

    void ImSlugWriteCurve(ImSlugCurvePool* pool, int texelX, int texelY, float x1, float y1, float x2, float y2,
                          float x3, float y3)
    {
        IMSLUG_ASSERT(pool != NULL && pool->Texels != NULL);
        IMSLUG_ASSERT(texelX >= 0 && texelX + 1 < pool->Width && texelY >= 0 && texelY < pool->Height);
        IMSLUG_ASSERT((texelX & 1) == 0);  // the two texels of one curve share a row

        uint16_t* texel = pool->Texels + ((size_t)texelY * (size_t)pool->Width + (size_t)texelX) * 4;
        texel[0] = ImSlugFloatToHalf(x1);
        texel[1] = ImSlugFloatToHalf(y1);
        texel[2] = ImSlugFloatToHalf(x2);
        texel[3] = ImSlugFloatToHalf(y2);
        texel[4] = ImSlugFloatToHalf(x3);
        texel[5] = ImSlugFloatToHalf(y3);
        texel[6] = 0;
        texel[7] = 0;

        MarkDirty(&pool->DirtyRowMin, &pool->DirtyRowMax, &pool->UsedRows, texelY);
    }

    void ImSlugCurveWriteTexelRaw(ImSlugCurvePool* pool, int texelX, int texelY, const uint16_t source[8])
    {
        IMSLUG_ASSERT(pool != NULL && pool->Texels != NULL && source != NULL);
        IMSLUG_ASSERT(texelX >= 0 && texelX + 1 < pool->Width && texelY >= 0 && texelY < pool->Height);
        IMSLUG_ASSERT((texelX & 1) == 0);  // the two texels of one curve share a row

        uint16_t* texel = pool->Texels + ((size_t)texelY * (size_t)pool->Width + (size_t)texelX) * 4;
        memcpy(texel, source, 8 * sizeof(uint16_t));
        MarkDirty(&pool->DirtyRowMin, &pool->DirtyRowMax, &pool->UsedRows, texelY);
    }

    const uint16_t* ImSlugCurveTexelAt(const ImSlugCurvePool* pool, int x, int y)
    {
        IMSLUG_ASSERT(pool != NULL && pool->Texels != NULL);
        IMSLUG_ASSERT(x >= 0 && x < pool->Width && y >= 0 && y < pool->Height);
        return pool->Texels + ((size_t)y * (size_t)pool->Width + (size_t)x) * 4;
    }

    // ---------------------------------------------------------------------
    // Band pool
    // ---------------------------------------------------------------------
    ImSlugBuildStatus ImSlugAllocBandBlock(ImSlugBandPool* pool, int bandMaxX, int bandMaxY, const int* refCounts,
                                           ImSlugBandBlock* outBlock, uint16_t* outListOffsets)
    {
        IMSLUG_ASSERT(pool != NULL && outBlock != NULL && refCounts != NULL && outListOffsets != NULL);
        IMSLUG_ASSERT(bandMaxX >= 0 && bandMaxX < kMaxBandCountPerAxis);
        IMSLUG_ASSERT(bandMaxY >= 0 && bandMaxY < kMaxBandCountPerAxis);
        if (bandMaxX < 0 || bandMaxX >= kMaxBandCountPerAxis || bandMaxY < 0 || bandMaxY >= kMaxBandCountPerAxis)
            return ImSlugBuildStatus_GlyphTooLarge;  // caller bug (asserted); unrepresentable in Release

        const int headerCount = bandMaxY + 1 + bandMaxX + 1;
        int refCount = 0;
        for (int i = 0; i < headerCount; ++i)
        {
            IMSLUG_ASSERT(refCounts[i] >= 0);
            if (refCounts[i] < 0)
                return ImSlugBuildStatus_GlyphTooLarge;
            refCount += refCounts[i];
        }

        // Lay out within the pool; pad to a fresh row wherever the shader's
        // contiguous-x reads would straddle one.
        int cursor = pool->CursorTexels;
        if ((cursor & (kPoolWidth - 1)) + headerCount > kPoolWidth)
            cursor = ((cursor >> kPoolWidthLog2) + 1) << kPoolWidthLog2;
        const int glyphLocX = cursor & (kPoolWidth - 1);
        const int glyphLocY = cursor >> kPoolWidthLog2;
        cursor += headerCount;
        for (int i = 0; i < headerCount; ++i)
        {
            const int listLength = refCounts[i];
            // A list longer than a whole row can never satisfy the contiguous-x
            // rule (padding only moves it to a row start); the block is
            // unrepresentable, so refuse it instead of wrapping silently.
            if (listLength > kPoolWidth)
                return ImSlugBuildStatus_GlyphTooLarge;
            if (listLength > 0 && ((cursor & (kPoolWidth - 1)) + listLength > kPoolWidth))
                cursor = ((cursor >> kPoolWidthLog2) + 1) << kPoolWidthLog2;
            outListOffsets[i] = (uint16_t)(cursor - (glyphLocY * kPoolWidth + glyphLocX));
            cursor += listLength;
        }

        const int totalTexels = cursor - (glyphLocY * kPoolWidth + glyphLocX);
        if (totalTexels > kGlyphMaxBlockTexels)
            return ImSlugBuildStatus_GlyphTooLarge;  // band offsets are stored as u16 relative to glyphLoc
        const int lastRow = (cursor - 1) / kPoolWidth;
        if (lastRow >= kPoolMaxHeight)
            return ImSlugBuildStatus_BandPoolLimit;
        if (!ImSlugPoolEnsureBandRows(pool, lastRow + 1))
            return ImSlugBuildStatus_BandPoolLimit;

        outBlock->X = glyphLocX;
        outBlock->Y = glyphLocY;
        outBlock->HeaderCount = headerCount;
        outBlock->RefCount = refCount;
        outBlock->TotalTexels = totalTexels;
        pool->CursorTexels = cursor;
        return ImSlugBuildStatus_Ok;
    }

    void ImSlugCalcBandLoc(int glyphLocX, int glyphLocY, uint32_t offset, int* outX, int* outY)
    {
        // Reference pixel shader CalcBandLoc():
        //   x = (glyphLocX + offset) & 4095; y = glyphLocY + ((glyphLocX + offset) >> 12).
        const int x = glyphLocX + (int)offset;
        *outX = x & (kPoolWidth - 1);
        *outY = glyphLocY + (x >> kPoolWidthLog2);
    }

    static uint16_t* BandTexelPtr(ImSlugBandPool* pool, const ImSlugBandBlock& block, uint32_t offset)
    {
        int x, y;
        ImSlugCalcBandLoc(block.X, block.Y, offset, &x, &y);
        IMSLUG_ASSERT(x >= 0 && x < pool->Width && y >= 0 && y < pool->Height);
        MarkDirty(&pool->DirtyRowMin, &pool->DirtyRowMax, &pool->UsedRows, y);
        return pool->Texels + ((size_t)y * (size_t)pool->Width + (size_t)x) * 2;
    }

    void ImSlugBandWriteHeader(ImSlugBandPool* pool, const ImSlugBandBlock& block, int headerIndex, uint16_t curveCount,
                               uint16_t listOffset)
    {
        IMSLUG_ASSERT(pool != NULL && block.HeaderCount > 0);
        IMSLUG_ASSERT(headerIndex >= 0 && headerIndex < block.HeaderCount);
        uint16_t* texel = BandTexelPtr(pool, block, (uint32_t)headerIndex);
        texel[0] = curveCount;
        texel[1] = listOffset;
    }

    void ImSlugBandWriteRef(ImSlugBandPool* pool, const ImSlugBandBlock& block, uint16_t listOffset, int indexInList,
                            uint16_t curveTexelX, uint16_t curveTexelY)
    {
        IMSLUG_ASSERT(pool != NULL);
        IMSLUG_ASSERT(indexInList >= 0);
        uint16_t* texel = BandTexelPtr(pool, block, (uint32_t)listOffset + (uint32_t)indexInList);
        texel[0] = curveTexelX;
        texel[1] = curveTexelY;
    }

    uint32_t ImSlugPackBandMaxWord(int bandMaxX, int bandMaxY, uint32_t glyphFlags)
    {
        IMSLUG_ASSERT(bandMaxX >= 0 && bandMaxX <= 0xFF);
        IMSLUG_ASSERT(bandMaxY >= 0 && bandMaxY <= 0xFF);
        uint32_t word = ((uint32_t)bandMaxX & 0xFFu) | (((uint32_t)bandMaxY & 0xFFu) << 16);
        if ((glyphFlags & ImSlugGlyphFlags_EvenOdd) != 0)
            word |= 0x10000000u;  // bit 28 = bit 12 of the high half (E flag)
        return word;
    }

    const uint16_t* ImSlugBandTexelAt(const ImSlugBandPool* pool, int x, int y)
    {
        IMSLUG_ASSERT(pool != NULL && pool->Texels != NULL);
        IMSLUG_ASSERT(x >= 0 && x < pool->Width && y >= 0 && y < pool->Height);
        return pool->Texels + ((size_t)y * (size_t)pool->Width + (size_t)x) * 2;
    }

    // ---------------------------------------------------------------------
    // Band construction
    // ---------------------------------------------------------------------
    // The packed loader (imslug_packed.cpp) and the synthetic record builder
    // (imslug_font.cpp) need band assignment without an outline parser, so
    // the pure quad/band helpers live in this always-compiled translation
    // unit. The declarations stay in imslug_packer.h.
    //
    // Band tuning (measured with the band-sweep tool,
    // tests/bench/band_sweep.cpp, over Roboto/Inter Latin and the CJK subset,
    // sizes 8..72 px, with a model of the pixel shader's reference walk
    // (band index + sorted early-out)):
    //
    //   sqrt(2n) 4..32 : 6.86 / 7.20 / 9.25 refs per pixel, 114/129/193 KB
    //   2*sqrt(n) 4..32: 6.12 / 6.42 / 8.25 refs per pixel, 143/159/240 KB
    //   2.5*sqrt(n)    : 5.76 / 6.03 / 7.71, 168/188/281 KB
    //   3*sqrt(n)      : 5.51 / 5.74 / 7.38, 193/214/320 KB
    //
    // 2*sqrt(n) buys ~11% less fragment work (and ~15% better p99/max tails,
    // which matters for low-end GPUs) for ~25% more band-pool memory - still
    // well inside the ~0.5 MB curve+band budget of a full Latin UI font.
    // Further steps cost ~2x the memory for ~5% more, so they were rejected.
    // The cap stays 32 (kMaxBandCountPerAxis): the header run must fit in one
    // pool row.
    int ImSlugCalcBandCount(int curveCount)
    {
        if (curveCount <= 0)
            return 4;  // clamp(ceil(2*sqrt(0)), 4, 32)
        const float n = ceilf(2.0f * sqrtf((float)curveCount));
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

    // Aggregate control-point bbox of a quad run; all-zero for an empty run.
    // The single source for both the glyph record bbox and the metrics API's
    // em-space MinX..MaxY, so the two can never drift apart.
    void ImSlugControlBoundsOfQuads(const ImSlugQuad* quads, int quadCount, float* outMinX, float* outMinY,
                                    float* outMaxX, float* outMaxY)
    {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (quadCount > 0)
        {
            ImSlugQuadBounds bounds = ImSlugQuadControlBounds(quads[0]);
            minX = bounds.MinX;
            minY = bounds.MinY;
            maxX = bounds.MaxX;
            maxY = bounds.MaxY;
            for (int i = 1; i < quadCount; ++i)
            {
                bounds = ImSlugQuadControlBounds(quads[i]);
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
        *outMinX = minX;
        *outMinY = minY;
        *outMaxX = maxX;
        *outMaxY = maxY;
    }

    // Exactly axis-parallel curves can never cross a ray of the matching axis;
    // they are excluded from those bands.
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

}  // namespace ImSlug
