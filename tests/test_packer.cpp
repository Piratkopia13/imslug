// imslug M1 tests — shared pool packing.
//
// Covers the acceptance checks of the M1 brief: pool growth by doubling,
// 2-texel curve pairing, band header/list row rules (the shader reads lists
// contiguously in x with no wrap), CalcBandLoc round trips, dirty-row and
// UsedRows bookkeeping, capacity guards and the fp16 texel round trip.

#include "imslug_internal.h"

#include "test_util.h"

#include <cstdio>

using namespace ImSlug;

static bool NearlyEqual(float a, float b, float relativeTolerance)
{
    const float scale = a < 0.0f ? -a : a;
    return std::fabs(a - b) <= relativeTolerance * (scale > 1.0f ? scale : 1.0f);
}

// ---------------------------------------------------------------------------
static void TestHalfConversion()
{
    TEST_CHECK(ImSlugFloatToHalf(0.0f) == 0x0000);
    TEST_CHECK(ImSlugFloatToHalf(1.0f) == 0x3C00);
    TEST_CHECK(ImSlugFloatToHalf(-2.5f) == 0xC100);
    TEST_CHECK(ImSlugFloatToHalf(0.1f) == 0x2E66);  // nearest half to 0.1
    TEST_CHECK(ImSlugFloatToHalf(65504.0f) == 0x7BFF);
    TEST_CHECK(ImSlugFloatToHalf(100000.0f) == 0x7C00);  // overflow -> +inf
    TEST_CHECK(ImSlugFloatToHalf(-100000.0f) == 0xFC00);

    TEST_CHECK(ImSlugHalfToFloat(0x3C00) == 1.0f);
    TEST_CHECK(ImSlugHalfToFloat(0xC100) == -2.5f);
    TEST_CHECK(ImSlugHalfToFloat(0x7C00) > 3.0e38f);  // +inf

    // Round trip for representable and unrepresentable values.
    const float values[] = { 0.0f,   1.0f,    -1.0f,  0.3f, -0.3f,  2.0f,   -2.0f,   0.0001f,
                             1e-08f, 65504.f, 1e-05f, 0.5f, -0.25f, 12.75f, -4096.f, 3.14159f };
    for (float value : values)
    {
        const float roundTrip = ImSlugHalfToFloat(ImSlugFloatToHalf(value));
        TEST_CHECKF(NearlyEqual(value, roundTrip, 1.0f / 1024.0f), "half round trip %.8f -> %.8f", (double)value,
                    (double)roundTrip);
    }

    // Subnormals survive as subnormals (2^-24 is the smallest).
    TEST_CHECK(ImSlugFloatToHalf(5.9604645e-08f) == 0x0001);
    TEST_CHECK(ImSlugHalfToFloat(0x0001) == 5.9604645e-08f);

    // NaN stays NaN.
    const uint16_t nanHalf = ImSlugFloatToHalf(std::nanf(""));
    TEST_CHECK((nanHalf & 0x7C00) == 0x7C00 && (nanHalf & 0x03FF) != 0);
    TEST_CHECK(std::isnan(ImSlugHalfToFloat(nanHalf)));
}

// ---------------------------------------------------------------------------
static void TestPoolGrowth()
{
    ImSlugPools pools;
    ImSlugInitPools(&pools);
    TEST_CHECK(pools.Curves.Texels == nullptr && pools.Curves.Height == 0);
    TEST_CHECK(pools.Bands.Texels == nullptr && pools.Bands.Height == 0);
    TEST_CHECK(pools.Curves.DirtyRowMin > pools.Curves.DirtyRowMax);  // "clean" sentinel

    TEST_CHECK(ImSlugPoolEnsureCurveRows(&pools.Curves, 10));
    TEST_CHECK(pools.Curves.Height == 32);  // doubling base
    TEST_CHECK(ImSlugPoolEnsureCurveRows(&pools.Curves, 100));
    TEST_CHECK(pools.Curves.Height == 128);
    TEST_CHECK(ImSlugPoolEnsureCurveRows(&pools.Curves, 33));
    TEST_CHECK(pools.Curves.Height == 128);  // no shrink, no regrowth
    TEST_CHECK(ImSlugPoolEnsureBandRows(&pools.Bands, 64));
    TEST_CHECK(pools.Bands.Height == 64);

    // Zeroed on growth.
    TEST_CHECK(ImSlugCurveTexelAt(&pools.Curves, 0, 0)[0] == 0);
    TEST_CHECK(ImSlugBandTexelAt(&pools.Bands, 1000, 63)[1] == 0);

    ImSlugDestroyPools(&pools);
    TEST_CHECK(pools.Curves.Texels == nullptr && pools.Bands.Texels == nullptr);
}

// ---------------------------------------------------------------------------
static void TestCurveBlocks()
{
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    ImSlugCurveBlock block;
    TEST_CHECK(ImSlugAllocCurveBlock(&pools.Curves, 5, &block));
    TEST_CHECK(block.X % 2 == 0);  // pairs never straddle a row
    TEST_CHECK(pools.Curves.CursorTexels == 10);

    for (int i = 0; i < 5; ++i)
    {
        int x = 0, y = 0;
        ImSlugCurveLocAt(block, i, &x, &y);
        TEST_CHECK(x % 2 == 0);
        ImSlugWriteCurve(&pools.Curves, x, y, (float)i, 0.5f + (float)i, 1.5f, 2.5f, 3.5f, -4.5f);
    }

    for (int i = 0; i < 5; ++i)
    {
        int x = 0, y = 0;
        ImSlugCurveLocAt(block, i, &x, &y);
        const uint16_t* texel = ImSlugCurveTexelAt(&pools.Curves, x, y);
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[0]), (float)i, 1.0f / 1024.0f));
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[1]), 0.5f + (float)i, 1.0f / 1024.0f));
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[2]), 1.5f, 1.0f / 1024.0f));
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[3]), 2.5f, 1.0f / 1024.0f));
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[4]), 3.5f, 1.0f / 1024.0f));
        TEST_CHECK(NearlyEqual(ImSlugHalfToFloat(texel[5]), -4.5f, 1.0f / 1024.0f));
        TEST_CHECK(texel[6] == 0 && texel[7] == 0);
    }

    // Dirty rows: writes above touched row 0 only.
    TEST_CHECK(pools.Curves.DirtyRowMin == 0 && pools.Curves.DirtyRowMax == 0);
    TEST_CHECK(pools.Curves.UsedRows == 1);

    // A block straddling the end of a row: both curves still keep their pair on
    // one row, and the wrapped row is marked dirty.
    pools.Curves.CursorTexels = kPoolWidth - 2;
    TEST_CHECK(ImSlugAllocCurveBlock(&pools.Curves, 2, &block));
    TEST_CHECK(block.X == kPoolWidth - 2 && block.Y == 0);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ImSlugCurveLocAt(block, 0, &x0, &y0);
    ImSlugCurveLocAt(block, 1, &x1, &y1);
    TEST_CHECK(x0 == kPoolWidth - 2 && y0 == 0);
    TEST_CHECK(x1 == 0 && y1 == 1);
    ImSlugWriteCurve(&pools.Curves, x1, y1, 0, 0, 0, 0, 0, 0);
    TEST_CHECK(pools.Curves.DirtyRowMax == 1);
    TEST_CHECK(pools.Curves.UsedRows == 2);

    ImSlugPoolClearDirtyRows(&pools);
    TEST_CHECK(pools.Curves.DirtyRowMin > pools.Curves.DirtyRowMax);
    TEST_CHECK(pools.Bands.DirtyRowMin > pools.Bands.DirtyRowMax);
    TEST_CHECK(pools.Curves.UsedRows == 2);  // the high-water mark survives

    // Per-pool clearing (M2 uploads consume one pool at a time): clearing only
    // the curve pool must leave the band pool's pending rows untouched.
    pools.Curves.DirtyRowMin = 1;
    pools.Curves.DirtyRowMax = 3;
    pools.Bands.DirtyRowMin = 2;
    pools.Bands.DirtyRowMax = 5;
    ImSlugCurvePoolClearDirtyRows(&pools.Curves);
    TEST_CHECK(pools.Curves.DirtyRowMin > pools.Curves.DirtyRowMax);
    TEST_CHECK(pools.Bands.DirtyRowMin == 2 && pools.Bands.DirtyRowMax == 5);
    ImSlugBandPoolClearDirtyRows(&pools.Bands);
    TEST_CHECK(pools.Bands.DirtyRowMin > pools.Bands.DirtyRowMax);

    // Capacity guard: beyond the 16-bit texel y limit the allocator refuses
    // (constructing the fake pool state avoids a 2 GB allocation).
    ImSlugCurvePool fake;
    fake.Texels = nullptr;
    fake.Width = kPoolWidth;
    fake.Height = kPoolMaxHeight;
    fake.CursorTexels = (kPoolMaxHeight - 1) * kPoolWidth + kPoolWidth - 2;
    fake.UsedRows = 0;
    fake.DirtyRowMin = 1;
    fake.DirtyRowMax = 0;
    TEST_CHECK(!ImSlugAllocCurveBlock(&fake, 2, &block));
    TEST_CHECK(ImSlugAllocCurveBlock(&fake, 0, &block));  // empty blocks never fail

    ImSlugDestroyPools(&pools);
}

// ---------------------------------------------------------------------------
static void TestBandBlocks()
{
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    // 4 x 4 bands (headerCount = 10), two lists that do not fit the current row.
    const int bandMaxX = 3;
    const int bandMaxY = 3;
    const int headerCount = bandMaxY + 1 + bandMaxX + 1;
    int refCounts[10] = { 2, 3, 0, 4, 1, 0, 2, 2, 3, 1 };
    int totalRefs = 0;
    for (int i = 0; i < headerCount; ++i)
        totalRefs += refCounts[i];

    ImSlugBandBlock block;
    uint16_t listOffsets[2 * kMaxBandCountPerAxis];
    TEST_CHECK(ImSlugAllocBandBlock(&pools.Bands, bandMaxX, bandMaxY, refCounts, &block, listOffsets) ==
               ImSlugBuildStatus_Ok);
    TEST_CHECK(block.HeaderCount == headerCount);
    TEST_CHECK(block.RefCount == totalRefs);
    TEST_CHECK(block.X + headerCount <= kPoolWidth);  // header run inside one row

    // Offsets are non-decreasing, resolve inside the pool via CalcBandLoc, and
    // every non-empty list fits inside its row.
    uint32_t previous = 0;
    int seenNonEmpty = 0;
    for (int band = 0; band < headerCount; ++band)
    {
        TEST_CHECK(listOffsets[band] >= previous);  // non-decreasing in offset space
        previous = listOffsets[band];
        int listX = 0, listY = 0;
        ImSlugCalcBandLoc(block.X, block.Y, listOffsets[band], &listX, &listY);
        TEST_CHECK(listY * kPoolWidth + listX == (int)listOffsets[band] + block.Y * kPoolWidth + block.X);
        if (refCounts[band] > 0)
        {
            seenNonEmpty++;
            TEST_CHECK(listX + refCounts[band] <= kPoolWidth);
            TEST_CHECK(listY < pools.Bands.Height);
        }
    }
    TEST_CHECK(seenNonEmpty > 0);

    // Write and read back headers and refs.
    for (int band = 0; band < headerCount; ++band)
        ImSlugBandWriteHeader(&pools.Bands, block, band, (uint16_t)refCounts[band], listOffsets[band]);
    int refIndex = 0;
    for (int band = 0; band < headerCount; ++band)
    {
        for (int r = 0; r < refCounts[band]; ++r)
        {
            ImSlugBandWriteRef(&pools.Bands, block, listOffsets[band], r, (uint16_t)(100 + refIndex),
                               (uint16_t)(200 + refIndex));
            refIndex++;
        }
    }
    for (int band = 0; band < headerCount; ++band)
    {
        const uint16_t* header = ImSlugBandTexelAt(&pools.Bands, block.X + band, block.Y);
        TEST_CHECK(header[0] == refCounts[band]);
        TEST_CHECK(header[1] == listOffsets[band]);
    }
    refIndex = 0;
    for (int band = 0; band < headerCount; ++band)
    {
        int listX = 0, listY = 0;
        ImSlugCalcBandLoc(block.X, block.Y, listOffsets[band], &listX, &listY);
        for (int r = 0; r < refCounts[band]; ++r)
        {
            const uint16_t* ref = ImSlugBandTexelAt(&pools.Bands, listX + r, listY);
            TEST_CHECK(ref[0] == 100 + refIndex && ref[1] == 200 + refIndex);
            refIndex++;
        }
    }

    // The header run is allowed to sit anywhere; the next glyph starts after
    // this block (cursor advanced by at least HeaderCount + RefCount).
    TEST_CHECK(pools.Bands.CursorTexels >= block.Y * kPoolWidth + block.X + headerCount + totalRefs - 1);

    // A block whose lists must be row-padded: force the cursor near the row end.
    ImSlugPools wrapped;
    ImSlugInitPools(&wrapped);
    TEST_CHECK(ImSlugPoolEnsureBandRows(&wrapped.Bands, 2));
    wrapped.Bands.CursorTexels = kPoolWidth - 5;  // headers (4) fit, lists then need a new row
    int smallCounts[4] = { 3, 2, 0, 4 };
    ImSlugBandBlock wrappedBlock;
    uint16_t wrappedOffsets[4];
    TEST_CHECK(ImSlugAllocBandBlock(&wrapped.Bands, 1, 1, smallCounts, &wrappedBlock, wrappedOffsets) ==
               ImSlugBuildStatus_Ok);
    TEST_CHECK(wrappedBlock.X == kPoolWidth - 5 && wrappedBlock.Y == 0);
    // List 0 (3 entries) does not fit the 1 texel left in row 0 (x = 4095), so
    // it is padded to row 1: offset 5 = (kPoolWidth + 0) - (kPoolWidth - 5).
    TEST_CHECK(wrappedOffsets[0] == 5);
    int list0X = 0, list0Y = 0;
    ImSlugCalcBandLoc(wrappedBlock.X, wrappedBlock.Y, wrappedOffsets[0], &list0X, &list0Y);
    TEST_CHECK(list0X == 0 && list0Y == 1);
    // List 1 follows at x = 3 of row 1; list 2 is empty; list 3 (4 entries)
    // follows at x = 5.
    int list1X = 0, list1Y = 0;
    ImSlugCalcBandLoc(wrappedBlock.X, wrappedBlock.Y, wrappedOffsets[1], &list1X, &list1Y);
    TEST_CHECK(list1Y == 1 && list1X == 3);
    TEST_CHECK(list1X + smallCounts[1] <= kPoolWidth);
    // List 3 (the second vertical band) follows at x = 5.
    int list3X = 0, list3Y = 0;
    ImSlugCalcBandLoc(wrappedBlock.X, wrappedBlock.Y, wrappedOffsets[3], &list3X, &list3Y);
    TEST_CHECK(list3Y == 1 && list3X == 5 && list3X + smallCounts[3] <= kPoolWidth);
    ImSlugDestroyPools(&wrapped);

    // Header run near the row end moves the whole block to the next row.
    ImSlugPools headerWrap;
    ImSlugInitPools(&headerWrap);
    TEST_CHECK(ImSlugPoolEnsureBandRows(&headerWrap.Bands, 2));
    headerWrap.Bands.CursorTexels = kPoolWidth - 3;  // 10 headers do not fit
    int headerWrapCounts[10] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    ImSlugBandBlock headerWrapBlock;
    uint16_t headerWrapOffsets[10];
    TEST_CHECK(ImSlugAllocBandBlock(&headerWrap.Bands, 3, 3, headerWrapCounts, &headerWrapBlock, headerWrapOffsets) ==
               ImSlugBuildStatus_Ok);
    TEST_CHECK(headerWrapBlock.X == 0 && headerWrapBlock.Y == 1);
    ImSlugDestroyPools(&headerWrap);

    // Capacity guard: a block that cannot be addressed with u16 offsets.
    // (Raw HeaderCount + RefCount is 70010 > 65535 here; the padded total is
    // larger still, so this is a definitive GlyphTooLarge, not a pool limit.)
    ImSlugPools huge;
    ImSlugInitPools(&huge);
    int hugeCounts[10];
    for (int i = 0; i < 10; ++i)
        hugeCounts[i] = 7000;  // 70000 refs > 65535 - 10
    ImSlugBandBlock hugeBlock;
    uint16_t hugeOffsets[10];
    TEST_CHECK(ImSlugAllocBandBlock(&huge.Bands, 3, 3, hugeCounts, &hugeBlock, hugeOffsets) ==
               ImSlugBuildStatus_GlyphTooLarge);
    TEST_CHECK(huge.Bands.Texels == nullptr);  // nothing was allocated
    ImSlugDestroyPools(&huge);

    // Pool height limit (fake state, no allocation).
    ImSlugBandPool fakeBand;
    fakeBand.Texels = nullptr;
    fakeBand.Width = kPoolWidth;
    fakeBand.Height = kPoolMaxHeight;
    fakeBand.CursorTexels = (kPoolMaxHeight - 1) * kPoolWidth + kPoolWidth - 3;
    fakeBand.UsedRows = 0;
    fakeBand.DirtyRowMin = 1;
    fakeBand.DirtyRowMax = 0;
    int fakeCounts[10] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    TEST_CHECK(ImSlugAllocBandBlock(&fakeBand, 3, 3, fakeCounts, &hugeBlock, hugeOffsets) ==
               ImSlugBuildStatus_BandPoolLimit);

    ImSlugDestroyPools(&pools);
}

// ---------------------------------------------------------------------------
// A reference list must fit in one pool row (the shader reads it contiguously
// in x); a list longer than kPoolWidth is unrepresentable and must be refused
// as GlyphTooLarge instead of silently straddling rows.
static void TestBandBlockListRowRule()
{
    ImSlugPools pools;
    ImSlugInitPools(&pools);
    ImSlugBandBlock block;
    uint16_t offsets[2 * kMaxBandCountPerAxis];

    int tooLongCounts[2] = { kPoolWidth + 1, 0 };
    TEST_CHECK(ImSlugAllocBandBlock(&pools.Bands, 0, 0, tooLongCounts, &block, offsets) ==
               ImSlugBuildStatus_GlyphTooLarge);
    TEST_CHECK(pools.Bands.CursorTexels == 0);  // nothing was reserved

    // A list of exactly kPoolWidth is fine: the allocator pads it to a fresh
    // row so it fits exactly.
    int fullRowCounts[2] = { kPoolWidth, 0 };
    TEST_CHECK(ImSlugAllocBandBlock(&pools.Bands, 0, 0, fullRowCounts, &block, offsets) == ImSlugBuildStatus_Ok);
    TEST_CHECK(block.HeaderCount == 2 && block.RefCount == kPoolWidth);
    int listX = 0, listY = 0;
    ImSlugCalcBandLoc(block.X, block.Y, offsets[0], &listX, &listY);
    TEST_CHECK(listX == 0 && listY == 1);  // headers row 0, the full list row 1
    TEST_CHECK(pools.Bands.CursorTexels == 2 * kPoolWidth);

    ImSlugDestroyPools(&pools);
}

// ---------------------------------------------------------------------------
// Classification must use the allocator's real padded total: row padding can
// push a block past kGlyphMaxBlockTexels even though HeaderCount + RefCount
// fits, which the old caller-side check misreported as a retryable pool limit.
static void TestBandBlockPaddingOverflow()
{
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    const int bandMaxY = 20;  // 21 horizontal bands + 1 vertical -> 22 headers
    int refCounts[22];
    int rawTotal = 0;
    for (int i = 0; i < 21; ++i)
    {
        refCounts[i] = 3000;  // ends before the row end, so the next list pads
        rawTotal += refCounts[i];
    }
    refCounts[21] = 0;
    TEST_CHECK(rawTotal + 22 <= kGlyphMaxBlockTexels);  // the old classification's input says "pool limit"

    ImSlugBandBlock block;
    uint16_t offsets[2 * kMaxBandCountPerAxis];
    TEST_CHECK(ImSlugAllocBandBlock(&pools.Bands, 0, bandMaxY, refCounts, &block, offsets) ==
               ImSlugBuildStatus_GlyphTooLarge);
    TEST_CHECK(pools.Bands.CursorTexels == 0);  // nothing was reserved

    ImSlugDestroyPools(&pools);
}

// ---------------------------------------------------------------------------
static void TestBandMaxWord()
{
    // bits 0..15 bandMaxX, 16..23 bandMaxY, 28 = E.
    const uint32_t none = ImSlugPackBandMaxWord(31, 32, ImSlugGlyphFlags_None);
    TEST_CHECK(none == 0x0020001Fu);
    TEST_CHECK((none & 0xFFu) == 31);
    TEST_CHECK(((none >> 16) & 0xFFu) == 32);  // shader: bandMax.y &= 0x00FF
    TEST_CHECK(((none >> 16) & 0x1000u) == 0);

    const uint32_t evenOdd = ImSlugPackBandMaxWord(5, 7, ImSlugGlyphFlags_EvenOdd);
    TEST_CHECK(evenOdd == (0x00070005u | 0x10000000u));
    TEST_CHECK(((evenOdd >> 16) & 0x1000u) != 0);  // CalcCoverage tests bit 12 of the high half
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("imslug_packer: fp16 texels, pool growth, row wrapping, guards\n");
    TestHalfConversion();
    TestPoolGrowth();
    TestCurveBlocks();
    TestBandBlocks();
    TestBandBlockListRowRule();
    TestBandBlockPaddingOverflow();
    TestBandMaxWord();
    return imslug_test::Finish("imslug_packer");
}
