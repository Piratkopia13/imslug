// imslug M1 tests — outline extraction, cubic conversion, band construction.
//
// Covers the acceptance checks of the M1 brief: extraction into em-space
// quadratics (orientation, line encoding, contour continuity), the
// cubic->quadratic utility, adaptive band counts, and the band membership
// superset/sort invariants checked against an independent re-derivation of the
// outline's exact parametric ranges.

#include "imslug_internal.h"

#include "test_util.h"

#include <cstdio>

using namespace ImSlug;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static float EvalQuad(float p0, float p1, float p2, float t)
{
    const float u = 1.0f - t;
    return u * u * p0 + 2.0f * u * t * p1 + t * t * p2;
}

// Exact parametric range of one quadratic component (not the control box).
static void ExactRange(float c0, float c1, float c2, float* outMin, float* outMax)
{
    float minimum = c0 < c2 ? c0 : c2;
    float maximum = c0 > c2 ? c0 : c2;
    const float denominator = c0 - 2.0f * c1 + c2;
    if (denominator != 0.0f)
    {
        const float t = (c0 - c1) / denominator;
        if (t > 0.0f && t < 1.0f)
        {
            const float value = EvalQuad(c0, c1, c2, t);
            if (value < minimum)
                minimum = value;
            if (value > maximum)
                maximum = value;
        }
    }
    *outMin = minimum;
    *outMax = maximum;
}

static void QuadExactRange(const ImSlugQuad& quad, float* minX, float* minY, float* maxX, float* maxY)
{
    ExactRange(quad.X1, quad.X2, quad.X3, minX, maxX);
    ExactRange(quad.Y1, quad.Y2, quad.Y3, minY, maxY);
}

static bool IsAxisParallelHorizontal(const ImSlugQuad& quad)
{
    return quad.Y1 == quad.Y2 && quad.Y2 == quad.Y3;
}
static bool IsAxisParallelVertical(const ImSlugQuad& quad)
{
    return quad.X1 == quad.X2 && quad.X2 == quad.X3;
}

struct TestQuadSet
{
    ImSlugQuad* Quads;
    int QuadCount;
    int Required;
};

static bool ExtractTestQuads(const stbtt_fontinfo& info, int glyphIndex, TestQuadSet* out)
{
    int required = 0;
    ImSlugExtractGlyphOutline(&info, glyphIndex, nullptr, 0, &required);
    out->Required = required;
    out->QuadCount = 0;
    out->Quads = nullptr;
    if (required <= 0)
        return true;
    out->Quads = (ImSlugQuad*)std::malloc((size_t)required * sizeof(ImSlugQuad));
    if (out->Quads == nullptr)
        return false;
    if (!ImSlugExtractGlyphOutline(&info, glyphIndex, out->Quads, required, &out->QuadCount))
        return false;
    // A successful extraction into a buffer that fits the upper bound must not
    // need more space later.
    if (out->QuadCount > required)
        return false;
    return true;
}

// Maps a band's curve reference (a curve-texel location) back to a curve index
// using the record's contiguous curve block; returns -1 when not a curve of
// this glyph (a packing bug).
static int CurveIndexFromTexelLoc(const ImSlugGlyphRecord& record, int texelX, int texelY)
{
    for (int i = 0; i < (int)record.CurveCount; ++i)
    {
        ImSlugCurveBlock block;
        block.X = record.CurveLocX;
        block.Y = record.CurveLocY;
        block.CurveCount = (int)record.CurveCount;
        int x = 0, y = 0;
        ImSlugCurveLocAt(block, i, &x, &y);
        if (x == texelX && y == texelY)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Cubic -> quadratics
// ---------------------------------------------------------------------------
static void TestCubicToQuadratics()
{
    // Quarter circle-ish cubic; the 2-quad approximation error stays below
    // ~0.5% of the segment scale.
    ImSlugCubic cubic;
    cubic.X1 = 0.0f;
    cubic.Y1 = 0.0f;
    cubic.X2 = 0.0f;
    cubic.Y2 = 1.0f;
    cubic.X3 = 1.0f;
    cubic.Y3 = 1.0f;
    cubic.X4 = 1.0f;
    cubic.Y4 = 0.0f;

    ImSlugQuad quads[2];
    TEST_CHECK(ImSlugConvertCubicToQuadratics(&cubic, quads) == 2);
    TEST_CHECK(quads[0].X1 == 0.0f && quads[0].Y1 == 0.0f);
    TEST_CHECK(quads[1].X3 == 1.0f && quads[1].Y3 == 0.0f);
    // Junction is the cubic midpoint at t = 0.5: (0.5, 0.75).
    TEST_CHECK(quads[0].X3 == 0.5f && quads[0].Y3 == 0.75f);
    TEST_CHECK(quads[1].X1 == 0.5f && quads[1].Y1 == 0.75f);

    float worst = 0.0f;
    for (int step = 0; step <= 40; ++step)
    {
        const float t = (float)step / 40.0f;
        const float u = 1.0f - t;
        const float cx =
            u * u * u * cubic.X1 + 3.0f * u * u * t * cubic.X2 + 3.0f * u * t * t * cubic.X3 + t * t * t * cubic.X4;
        const float cy =
            u * u * u * cubic.Y1 + 3.0f * u * u * t * cubic.Y2 + 3.0f * u * t * t * cubic.Y3 + t * t * t * cubic.Y4;
        const float local = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
        const ImSlugQuad& quad = t < 0.5f ? quads[0] : quads[1];
        const float qx = EvalQuad(quad.X1, quad.X2, quad.X3, local);
        const float qy = EvalQuad(quad.Y1, quad.Y2, quad.Y3, local);
        const float dx = qx - cx;
        const float dy = qy - cy;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > worst)
            worst = distance;
    }
    // Theoretical max error of the 3/4-split two-quadratic approximation:
    // |P3 - 3 P2 + 3 P1 - P0| / 54 (Farin; here (2, 0)/54).
    const float thirdDx = cubic.X4 - 3.0f * cubic.X3 + 3.0f * cubic.X2 - cubic.X1;
    const float thirdDy = cubic.Y4 - 3.0f * cubic.Y3 + 3.0f * cubic.Y2 - cubic.Y1;
    const float errorBound = std::sqrt(thirdDx * thirdDx + thirdDy * thirdDy) / 54.0f;
    std::printf("  cubic->2 quads: max error %.6f, theoretical bound %.6f (|P3-3P2+3P1-P0|/54)\n", (double)worst,
                (double)errorBound);
    TEST_CHECK(worst <= errorBound * 1.01f);
    TEST_CHECK(worst > 0.0f);  // it is an approximation, not an exact match

    // Degenerate cubic (all four points equal) -> nothing.
    ImSlugCubic degenerate;
    degenerate.X1 = degenerate.Y1 = 0.25f;
    degenerate.X2 = degenerate.Y2 = 0.25f;
    degenerate.X3 = degenerate.Y3 = 0.25f;
    degenerate.X4 = degenerate.Y4 = 0.25f;
    TEST_CHECK(ImSlugConvertCubicToQuadratics(&degenerate, quads) == 0);
}

// ---------------------------------------------------------------------------
// Outline extraction
// ---------------------------------------------------------------------------
static void TestExtractionForFont(const char* fontName)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    stbtt_fontinfo info;
    TEST_CHECK(stbtt_InitFont(&info, fontData, 0) != 0);
    const float unitScale = 1.0f / (float)((info.data[info.head + 18] << 8) | info.data[info.head + 19]);

    const unsigned int codepoints[] = { 'I', 'g', 'A', 'W', 0xE9 /* é */ };
    for (unsigned int codepoint : codepoints)
    {
        const int glyphIndex = stbtt_FindGlyphIndex(&info, (int)codepoint);
        TEST_CHECKF(glyphIndex > 0, "U+%04X missing in %s", codepoint, fontName);
        if (glyphIndex <= 0)
            continue;

        TestQuadSet set = {};
        TEST_CHECK(ExtractTestQuads(info, glyphIndex, &set));
        TEST_CHECK(set.QuadCount > 0);

        // stbtt vertex census: contours and line segments.
        stbtt_vertex* vertices = nullptr;
        const int vertexCount = stbtt_GetGlyphShape(&info, glyphIndex, &vertices);
        int moveCount = 0, lineCount = 0;
        for (int i = 0; i < vertexCount; ++i)
        {
            if (vertices[i].type == STBTT_vmove)
                moveCount++;
            else if (vertices[i].type == STBTT_vline)
                lineCount++;
        }
        stbtt_FreeShape(&info, vertices);

        // Contours are preserved: a new contour starts wherever a quad does not
        // continue from the previous end point.
        int contourCount = 1;
        int lineEncodedCount = 0;
        for (int i = 0; i < set.QuadCount; ++i)
        {
            if (i > 0 && (set.Quads[i].X1 != set.Quads[i - 1].X3 || set.Quads[i].Y1 != set.Quads[i - 1].Y3))
                contourCount++;
            TEST_CHECK(!(set.Quads[i].X1 == set.Quads[i].X2 && set.Quads[i].X2 == set.Quads[i].X3 &&
                         set.Quads[i].Y1 == set.Quads[i].Y2 && set.Quads[i].Y2 == set.Quads[i].Y3));
            if (set.Quads[i].X2 == set.Quads[i].X3 && set.Quads[i].Y2 == set.Quads[i].Y3)
                lineEncodedCount++;
        }
        TEST_CHECKF(contourCount == moveCount, "U+%04X: %d contours extracted vs %d stbtt moves", codepoint,
                    contourCount, moveCount);
        // Lines must be encoded {p1,p3,p3}; counts line up (a curve control
        // point coinciding with its endpoint is not expected in these fonts).
        TEST_CHECKF(lineEncodedCount == lineCount, "U+%04X: %d line-encoded quads vs %d stbtt lines", codepoint,
                    lineEncodedCount, lineCount);

        // Orientation and containment vs the glyf header box (Y up, em space).
        int boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0;
        TEST_CHECK(stbtt_GetGlyphBox(&info, glyphIndex, &boxX0, &boxY0, &boxX1, &boxY1) != 0);
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int i = 0; i < set.QuadCount; ++i)
        {
            const ImSlugQuadBounds bounds = ImSlugQuadControlBounds(set.Quads[i]);
            if (bounds.MinX < minX)
                minX = bounds.MinX;
            if (bounds.MinY < minY)
                minY = bounds.MinY;
            if (bounds.MaxX > maxX)
                maxX = bounds.MaxX;
            if (bounds.MaxY > maxY)
                maxY = bounds.MaxY;
            TEST_CHECK(std::isfinite(bounds.MinX) && std::isfinite(bounds.MinY) && std::isfinite(bounds.MaxX) &&
                       std::isfinite(bounds.MaxY));
        }
        const float slack = 1.0f / 512.0f;  // glyf header boxes may be slightly loose
        TEST_CHECKF(minX >= (float)boxX0 * unitScale - slack && maxX <= (float)boxX1 * unitScale + slack,
                    "U+%04X x extremes [%.4f, %.4f] outside header box [%.4f, %.4f]", codepoint, (double)minX,
                    (double)maxX, (double)((float)boxX0 * unitScale), (double)((float)boxX1 * unitScale));
        TEST_CHECKF(minY >= (float)boxY0 * unitScale - slack && maxY <= (float)boxY1 * unitScale + slack,
                    "U+%04X y extremes [%.4f, %.4f] outside header box [%.4f, %.4f]", codepoint, (double)minY,
                    (double)maxY, (double)((float)boxY0 * unitScale), (double)((float)boxY1 * unitScale));

        // The 'g' descender test: em space is Y up, the tail hangs below 0.
        if (codepoint == 'g')
            TEST_CHECKF(minY < -0.01f, "%s 'g' descender: minY = %.4f (expected < 0)", fontName, (double)minY);
        if (codepoint == 'A' || codepoint == 'W' || codepoint == 'I')
            TEST_CHECKF(minY > -0.05f, "%s U+%04X: unexpected descender minY = %.4f", fontName, codepoint,
                        (double)minY);

        // 'I' is a plain rectangle in both test fonts: 1 contour, all segments
        // straight, so every quad must be a line encoding.
        if (codepoint == 'I')
        {
            TEST_CHECKF(set.QuadCount == lineCount && lineEncodedCount == lineCount,
                        "%s 'I' should be a pure line contour (quads %d, lines %d)", fontName, set.QuadCount,
                        lineCount);
        }

        // Capacity failure path: one short, report the requirement.
        int probe = 0;
        TEST_CHECK(!ImSlugExtractGlyphOutline(&info, glyphIndex, set.Quads, set.QuadCount - 1, &probe) ==
                   (set.QuadCount > 0));
        TEST_CHECK(probe == set.Required);

        std::printf("  %s U+%04X: %d quads, %d contours, bbox [%.3f %.3f %.3f %.3f]\n", fontName, codepoint,
                    set.QuadCount, contourCount, (double)minX, (double)minY, (double)maxX, (double)maxY);
        std::free(set.Quads);
    }

    std::free(fontData);
}

// ---------------------------------------------------------------------------
// Adaptive band counts
// ---------------------------------------------------------------------------
static void TestBandCounts()
{
    TEST_CHECK(ImSlugCalcBandCount(0) == 4);
    TEST_CHECK(ImSlugCalcBandCount(1) == 4);
    TEST_CHECK(ImSlugCalcBandCount(8) == 4);     // ceil(sqrt(16)) = 4
    TEST_CHECK(ImSlugCalcBandCount(9) == 5);     // ceil(sqrt(18)) = 5
    TEST_CHECK(ImSlugCalcBandCount(32) == 8);    // ceil(sqrt(64)) = 8
    TEST_CHECK(ImSlugCalcBandCount(512) == 32);  // clamp at 32
    TEST_CHECK(ImSlugCalcBandCount(100000) == 32);
    for (int n = 0; n < 2000; ++n)
    {
        const int count = ImSlugCalcBandCount(n);
        TEST_CHECK(count >= 4 && count <= kMaxBandCountPerAxis);
        if (n > 0)
            TEST_CHECK(count >= ImSlugCalcBandCount(n - 1));
    }
}

// ---------------------------------------------------------------------------
// Band construction against the packed pools
// ---------------------------------------------------------------------------
static void TestBandsForFont(const char* fontName)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    TEST_CHECK(font != nullptr);
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    stbtt_fontinfo info;
    stbtt_InitFont(&info, fontData, 0);

    const unsigned int codepoints[] = { 'g', 'A', 'W', 0xE9 };
    for (unsigned int codepoint : codepoints)
    {
        const ImSlugGlyphRecord* record = nullptr;
        const ImSlugBuildStatus status = ImSlugGetGlyphRecord(font, &pools, codepoint, &record);
        TEST_CHECKF(status == ImSlugBuildStatus_Ok, "build U+%04X failed (%d)", codepoint, (int)status);
        TEST_CHECK(record != nullptr);  // the status contract: Ok implies a valid record
        if (status != ImSlugBuildStatus_Ok)
            continue;

        TestQuadSet set = {};
        TEST_CHECK(ExtractTestQuads(info, stbtt_FindGlyphIndex(&info, (int)codepoint), &set));
        TEST_CHECK(set.QuadCount == (int)record->CurveCount);

        const int hBandCount = (int)record->BandMaxY + 1;
        const int vBandCount = (int)record->BandMaxX + 1;
        const float extentX = record->MaxX - record->MinX;
        const float extentY = record->MaxY - record->MinY;
        const float epsilon = kBandOverlapEpsilon;

        int totalRefs = 0;
        for (int band = 0; band < hBandCount + vBandCount; ++band)
        {
            const bool horizontal = band < hBandCount;
            const int bandIndex = horizontal ? band : band - hBandCount;

            const uint16_t* header = ImSlugBandTexelAt(&pools.Bands, record->BandLocX + band, record->BandLocY);
            const int refCount = (int)header[0];
            int listX = 0, listY = 0;
            ImSlugCalcBandLoc(record->BandLocX, record->BandLocY, header[1], &listX, &listY);

            // The header run and every list must live inside one row (the
            // shader reads them contiguously in x without wrapping).
            if (horizontal)
                TEST_CHECK(record->BandLocX + band < kPoolWidth);
            else
                TEST_CHECK(record->BandLocX + band < kPoolWidth);
            TEST_CHECK(listX + (refCount > 0 ? refCount - 1 : 0) < kPoolWidth);

            float previousKey = 1e9f;
            for (int r = 0; r < refCount; ++r)
            {
                const uint16_t* ref = ImSlugBandTexelAt(&pools.Bands, listX + r, listY);
                const int curveIndex = CurveIndexFromTexelLoc(*record, ref[0], ref[1]);
                TEST_CHECKF(curveIndex >= 0, "U+%04X band %d ref %d points at a non-curve texel", codepoint, band, r);
                if (curveIndex < 0)
                    continue;
                const ImSlugQuad& quad = set.Quads[curveIndex];
                const ImSlugQuadBounds control = ImSlugQuadControlBounds(quad);

                // Sort: descending by max x (horizontal) / max y (vertical).
                const float sortKey = horizontal ? control.MaxX : control.MaxY;
                TEST_CHECKF(sortKey <= previousKey + 1e-6f, "U+%04X band %d not sorted at ref %d", codepoint, band, r);
                previousKey = sortKey;

                // No false positives: the curve's control box overlaps the band
                // (inflated by the documented epsilon).
                float bandLo = 0.0f, bandHi = 0.0f;
                if (horizontal)
                {
                    bandLo = record->MinY + extentY * (float)bandIndex / (float)hBandCount;
                    bandHi = record->MinY + extentY * (float)(bandIndex + 1) / (float)hBandCount;
                    TEST_CHECK(control.MinY - epsilon <= bandHi && control.MaxY + epsilon >= bandLo);
                    TEST_CHECK(!IsAxisParallelHorizontal(quad));
                }
                else
                {
                    bandLo = record->MinX + extentX * (float)bandIndex / (float)vBandCount;
                    bandHi = record->MinX + extentX * (float)(bandIndex + 1) / (float)vBandCount;
                    TEST_CHECK(control.MinX - epsilon <= bandHi && control.MaxX + epsilon >= bandLo);
                    TEST_CHECK(!IsAxisParallelVertical(quad));
                }
                totalRefs++;
            }

            // Superset: every curve whose exact parametric range overlaps the
            // band must be in the list (independent of the control-box rule).
            if ((horizontal ? extentY : extentX) > 0.0f)
            {
                const float rangeLo = horizontal ? record->MinY + extentY * (float)bandIndex / (float)hBandCount
                                                 : record->MinX + extentX * (float)bandIndex / (float)vBandCount;
                const float rangeHi = horizontal ? record->MinY + extentY * (float)(bandIndex + 1) / (float)hBandCount
                                                 : record->MinX + extentX * (float)(bandIndex + 1) / (float)vBandCount;
                for (int i = 0; i < set.QuadCount; ++i)
                {
                    const ImSlugQuad& quad = set.Quads[i];
                    if (horizontal && IsAxisParallelHorizontal(quad))
                        continue;
                    if (!horizontal && IsAxisParallelVertical(quad))
                        continue;
                    float exactMinX = 0, exactMinY = 0, exactMaxX = 0, exactMaxY = 0;
                    QuadExactRange(quad, &exactMinX, &exactMinY, &exactMaxX, &exactMaxY);
                    const float exactLo = horizontal ? exactMinY : exactMinX;
                    const float exactHi = horizontal ? exactMaxY : exactMaxX;
                    if (exactLo > rangeHi || exactHi < rangeLo)
                        continue;  // cannot contribute to this band

                    bool present = false;
                    for (int r = 0; r < refCount && !present; ++r)
                    {
                        const uint16_t* ref = ImSlugBandTexelAt(&pools.Bands, listX + r, listY);
                        present = (CurveIndexFromTexelLoc(*record, ref[0], ref[1]) == i);
                    }
                    TEST_CHECKF(present, "U+%04X %s band %d misses curve %d (range [%.4f, %.4f] band [%.4f, %.4f])",
                                codepoint, horizontal ? "h" : "v", bandIndex, i, (double)exactLo, (double)exactHi,
                                (double)rangeLo, (double)rangeHi);
                }
            }
        }
        TEST_CHECK(totalRefs > 0);
        std::printf("  %s U+%04X: bands %dx%d, %d refs, %d curves\n", fontName, codepoint, hBandCount, vBandCount,
                    totalRefs, set.QuadCount);
        std::free(set.Quads);
    }

    ImSlugDestroyPools(&pools);
    ImSlugDestroyFont(font);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
// fp16-quantized band admission (M2 review R1)
// ---------------------------------------------------------------------------
// Band generation evaluates the axis-parallel exclusion on the fp32 outline,
// but the pools store binary16. A curve can therefore become all-equal on one
// axis only after quantization while still being admitted to the matching-axis
// bands, where the nearly-linear solver branch runs. The shader now carries the
// CPU arbiter's |b| guard so both sides stay identical there; this scan asserts
// the invariant ("no admitted matching-axis curve has an all-equal quantized
// triple") and censuses the collisions from raw quads, so a font that starts
// triggering the branch is reported with numbers.
static void TestQuantizedBandCurves(const char* fontName)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, fontData, 0))
    {
        TEST_CHECKF(false, "%s: stbtt_InitFont failed", fontName);
        std::free(fontData);
        return;
    }
    ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    if (font == nullptr)
    {
        TEST_CHECKF(false, "%s: ImSlugCreateFontFromMemory failed", fontName);
        std::free(fontData);
        return;
    }
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    long long admittedRefs = 0;
    long long admittedViolations = 0;
    int firstViolationGlyph = -1;
    bool firstViolationHorizontal = false;

    long long rawQuads = 0;
    long long quantizedCollisionsX = 0;
    long long quantizedCollisionsY = 0;

    int builtGlyphs = 0;
    for (int glyphIndex = 1; glyphIndex < info.numGlyphs; ++glyphIndex)
    {
        const ImSlugGlyphRecord* record = nullptr;
        if (ImSlugGetGlyphRecordByIndex(font, &pools, glyphIndex, &record) != ImSlugBuildStatus_Ok)
            continue;  // GlyphTooLarge etc. falls back to the stock path anyway
        ++builtGlyphs;

        // Census: quantize the raw control points; count curves that are only
        // axis-parallel after rounding to binary16 (a collision).
        TestQuadSet set = {};
        if (!ExtractTestQuads(info, glyphIndex, &set))
            break;  // out of memory - stop the scan, the build itself succeeded
        for (int i = 0; i < set.QuadCount; ++i)
        {
            const ImSlugQuad& quad = set.Quads[i];
            ++rawQuads;
            const bool rawHorizontal = quad.Y1 == quad.Y2 && quad.Y2 == quad.Y3;
            const bool rawVertical = quad.X1 == quad.X2 && quad.X2 == quad.X3;
            const float qy1 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.Y1));
            const float qy2 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.Y2));
            const float qy3 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.Y3));
            const float qx1 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.X1));
            const float qx2 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.X2));
            const float qx3 = ImSlugHalfToFloat(ImSlugFloatToHalf(quad.X3));
            if (!rawHorizontal && qy1 == qy2 && qy2 == qy3)
                ++quantizedCollisionsY;
            if (!rawVertical && qx1 == qx2 && qx2 == qx3)
                ++quantizedCollisionsX;
        }
        std::free(set.Quads);

        if (record->CurveCount == 0)
            continue;

        // Horizontal bands: every referenced curve must not be all-equal in its
        // stored (fp16) y triple.
        for (int band = 0; band <= (int)record->BandMaxY; ++band)
        {
            const uint16_t* header = ImSlugBandTexelAt(&pools.Bands, record->BandLocX + band, record->BandLocY);
            const int refCount = (int)header[0];
            int listX = 0, listY = 0;
            ImSlugCalcBandLoc(record->BandLocX, record->BandLocY, header[1], &listX, &listY);
            for (int r = 0; r < refCount; ++r)
            {
                const uint16_t* ref = ImSlugBandTexelAt(&pools.Bands, listX + r, listY);
                const uint16_t* texel = ImSlugCurveTexelAt(&pools.Curves, ref[0], ref[1]);
                ++admittedRefs;
                if (ImSlugHalfToFloat(texel[1]) == ImSlugHalfToFloat(texel[3]) &&
                    ImSlugHalfToFloat(texel[3]) == ImSlugHalfToFloat(texel[5]))
                {
                    if (admittedViolations == 0)
                    {
                        firstViolationGlyph = glyphIndex;
                        firstViolationHorizontal = true;
                    }
                    ++admittedViolations;
                }
            }
        }

        // Vertical bands: the stored x triple.
        for (int band = 0; band <= (int)record->BandMaxX; ++band)
        {
            const uint16_t* header =
                ImSlugBandTexelAt(&pools.Bands, record->BandLocX + record->BandMaxY + 1 + band, record->BandLocY);
            const int refCount = (int)header[0];
            int listX = 0, listY = 0;
            ImSlugCalcBandLoc(record->BandLocX, record->BandLocY, header[1], &listX, &listY);
            for (int r = 0; r < refCount; ++r)
            {
                const uint16_t* ref = ImSlugBandTexelAt(&pools.Bands, listX + r, listY);
                const uint16_t* texel = ImSlugCurveTexelAt(&pools.Curves, ref[0], ref[1]);
                ++admittedRefs;
                if (ImSlugHalfToFloat(texel[0]) == ImSlugHalfToFloat(texel[2]) &&
                    ImSlugHalfToFloat(texel[2]) == ImSlugHalfToFloat(texel[4]))
                {
                    if (admittedViolations == 0)
                    {
                        firstViolationGlyph = glyphIndex;
                        firstViolationHorizontal = false;
                    }
                    ++admittedViolations;
                }
            }
        }
    }

    std::printf("  %s: %d glyphs, %lld admitted refs, %lld raw quads, quantized collisions x=%lld y=%lld\n", fontName,
                builtGlyphs, admittedRefs, rawQuads, quantizedCollisionsX, quantizedCollisionsY);
    TEST_CHECKF(admittedViolations == 0,
                "%s: %lld admitted matching-axis refs with an all-equal fp16 triple (first: glyph %d, %s band)",
                fontName, admittedViolations, firstViolationGlyph,
                firstViolationHorizontal ? "horizontal" : "vertical");
    TEST_CHECK(admittedRefs > 0);

    ImSlugDestroyPools(&pools);
    ImSlugDestroyFont(font);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
// Failed builds must not leak pool space and must keep the status contract
// ---------------------------------------------------------------------------
// Non-cached build failures (capacity limits) are retried on the next call, so
// a failed attempt has to leave the pools exactly as it found them; otherwise
// repeated calls would consume the pools forever. The band pool is forced to
// fail by pointing its cursor past the 16-bit height limit: the allocator
// refuses before touching Texels (the fake cursor is never dereferenced), and
// the curve block allocated before it must be rolled back.
//
// The OOM branches (realloc/malloc failures) have no fault injection in the M1
// core, so they are not exercised directly; instead GetOrBuildGlyphRecord
// reserves the cache entry *before* building, so a successful build always has
// a slot to publish into and "Ok implies a non-null record" holds
// unconditionally (asserted below for the normal path and after retries).
static void TestFailedBuildDoesNotLeak()
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    TEST_CHECK(font != nullptr);

    // Reference build in a clean pool set: the build after the failures must
    // land exactly where this one did.
    ImSlugPools reference;
    ImSlugInitPools(&reference);
    const ImSlugGlyphRecord* referenceRecord = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font, &reference, 'A', &referenceRecord) == ImSlugBuildStatus_Ok);
    TEST_CHECK(referenceRecord != nullptr);

    ImSlugPools pools;
    ImSlugInitPools(&pools);
    const int failedBandCursor = (kPoolMaxHeight + 1) * kPoolWidth;
    pools.Bands.CursorTexels = failedBandCursor;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const ImSlugGlyphRecord* record = nullptr;
        const ImSlugBuildStatus status = ImSlugGetGlyphRecord(font, &pools, 'A', &record);
        TEST_CHECKF(status == ImSlugBuildStatus_BandPoolLimit, "attempt %d: status %d (expected BandPoolLimit)",
                    attempt, (int)status);
        TEST_CHECK(record == nullptr);               // Ok must never be reported with a null record
        TEST_CHECK(pools.Curves.CursorTexels == 0);  // curve block rolled back
        TEST_CHECK(pools.Bands.CursorTexels == failedBandCursor);
    }

    // Repairing the pool state lets the same font build the glyph, and the
    // result is byte-identical to the reference build: no half-consumed state.
    pools.Bands.CursorTexels = 0;
    const ImSlugGlyphRecord* record = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font, &pools, 'A', &record) == ImSlugBuildStatus_Ok);
    TEST_CHECK(record != nullptr);
    TEST_CHECK(pools.Curves.CursorTexels == reference.Curves.CursorTexels);
    TEST_CHECK(pools.Bands.CursorTexels == reference.Bands.CursorTexels);
    TEST_CHECK(std::memcmp(record, referenceRecord, sizeof(*record)) == 0);

    // The cached re-request returns the same stable pointer and touches nothing.
    const ImSlugGlyphRecord* cached = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font, &pools, 'A', &cached) == ImSlugBuildStatus_Ok);
    TEST_CHECK(cached == record);
    TEST_CHECK(pools.Curves.CursorTexels == reference.Curves.CursorTexels);

    ImSlugDestroyPools(&pools);
    ImSlugDestroyPools(&reference);
    ImSlugDestroyFont(font);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------
static void TestDeterminism()
{
    const char* fontName = "Roboto-Regular.ttf";
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    // Two independent font objects, two independent pool sets, same bytes.
    ImSlugFont* fontA = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    ImSlugFont* fontB = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    ImSlugPools poolsA, poolsB;
    ImSlugInitPools(&poolsA);
    ImSlugInitPools(&poolsB);

    const unsigned int codepoints[] = { 'g', 'A', 'W', 'o', '|', 0xE9 };
    ImSlugGlyphRecord recordA[6], recordB[6];
    for (int i = 0; i < 6; ++i)
    {
        const ImSlugGlyphRecord* record = nullptr;
        TEST_CHECK(ImSlugGetGlyphRecord(fontA, &poolsA, codepoints[i], &record) == ImSlugBuildStatus_Ok);
        recordA[i] = *record;
        TEST_CHECK(ImSlugGetGlyphRecord(fontB, &poolsB, codepoints[i], &record) == ImSlugBuildStatus_Ok);
        recordB[i] = *record;
    }

    TEST_CHECK(poolsA.Curves.CursorTexels == poolsB.Curves.CursorTexels);
    TEST_CHECK(poolsA.Bands.CursorTexels == poolsB.Bands.CursorTexels);
    TEST_CHECK(std::memcmp(poolsA.Curves.Texels, poolsB.Curves.Texels,
                           (size_t)poolsA.Curves.CursorTexels * 4 * sizeof(uint16_t)) == 0);
    TEST_CHECK(std::memcmp(poolsA.Bands.Texels, poolsB.Bands.Texels,
                           (size_t)poolsA.Bands.CursorTexels * 2 * sizeof(uint16_t)) == 0);
    TEST_CHECK(std::memcmp(recordA, recordB, sizeof(recordA)) == 0);

    // Re-requesting a built glyph returns the cached record, not a rebuild.
    const int curveCursorBefore = poolsA.Curves.CursorTexels;
    const ImSlugGlyphRecord* again = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(fontA, &poolsA, 'g', &again) == ImSlugBuildStatus_Ok);
    TEST_CHECK(again == &recordA[0] ||
               (again->BandLocX == recordA[0].BandLocX && again->CurveLocX == recordA[0].CurveLocX));
    TEST_CHECK(poolsA.Curves.CursorTexels == curveCursorBefore);

    // Missing codepoint -> GlyphNotFound, no pool write.
    const ImSlugGlyphRecord* missing = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(fontA, &poolsA, 0x10FFFD, &missing) == ImSlugBuildStatus_GlyphNotFound);
    TEST_CHECK(missing == nullptr);
    TEST_CHECK(poolsA.Curves.CursorTexels == curveCursorBefore);

    ImSlugDestroyPools(&poolsA);
    ImSlugDestroyPools(&poolsB);
    ImSlugDestroyFont(fontA);
    ImSlugDestroyFont(fontB);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("imslug_glyphgen: outline extraction, cubic conversion, band construction\n");
    TestCubicToQuadratics();
    TestBandCounts();
    TestExtractionForFont("Roboto-Regular.ttf");
    TestExtractionForFont("Inter-Regular.ttf");
    TestBandsForFont("Roboto-Regular.ttf");
    TestBandsForFont("Inter-Regular.ttf");
    TestQuantizedBandCurves("Roboto-Regular.ttf");
    TestQuantizedBandCurves("Inter-Regular.ttf");
    TestDeterminism();
    TestFailedBuildDoesNotLeak();
    return imslug_test::Finish("imslug_glyphgen");
}
