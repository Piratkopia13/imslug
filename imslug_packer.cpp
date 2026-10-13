// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packer.cpp — shared curve/band pool storage: allocation with growth by
// doubling, row-wrap addressing (CalcBandLoc), dirty-row tracking and the
// capacity guards behind the graceful-fallback contract (PLAN §4.3, R11).
//
// All writes go through the helpers here so dirty rows and UsedRows can never
// drift from the actual texel writes.

#include "imslug_packer.h"

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
    // First allocation: 32 rows, then doubling (PLAN §4.3: "growth by doubling").
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

    void ImSlugInitPools(ImSlugPools* pools)
    {
        IMSLUG_ASSERT(pools != NULL);
        InitCurvePool(&pools->Curves);
        InitBandPool(&pools->Bands);
    }

    void ImSlugDestroyPools(ImSlugPools* pools)
    {
        if (pools == NULL)
            return;
        free(pools->Curves.Texels);
        free(pools->Bands.Texels);
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

    void ImSlugPoolClearDirtyRows(ImSlugPools* pools)
    {
        IMSLUG_ASSERT(pools != NULL);
        ImSlugCurvePoolClearDirtyRows(&pools->Curves);
        ImSlugBandPoolClearDirtyRows(&pools->Bands);
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
}  // namespace ImSlug
