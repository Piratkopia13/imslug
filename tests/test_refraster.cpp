// imslug M1 tests — CPU reference rasterizer.
//
// 1. Pins the pure shader math (root eligibility table, solvers, coverage
//    combine + fill rules) against closed-form expectations.
// 2. The M1 acceptance gate: rasterized 'g', 'A', 'W', 'é' at 16/32/64 px are
//    compared against stbtt's anti-aliased bitmaps with concrete thresholds
//    (mean |delta| and 99th percentile; see MeanThresholdForSize/P99ThresholdForSize
//    and kHardPixelTolerance).
// 3. Banded vs unbanded agreement (the bands are an acceleration structure and
//    must not change the result).

#include "imslug_internal.h"

#include "test_util.h"

#include <cstdio>

using namespace ImSlug;

// Acceptance thresholds: the reference Slug coverage estimator is not exact
// area coverage, so away from edges we require near-exactness
// (kHardPixelTolerance) and use size-dependent aggregate tripwires — see
// CompareGlyphAgainstStbtt for the rationale.
struct TestFont
{
    ImSlugFont* Font = nullptr;
    stbtt_fontinfo Info = {};
    unsigned char* Data = nullptr;
    int Size = 0;
    int UnitsPerEm = 0;
};

static bool OpenTestFont(const char* fileName, TestFont* out)
{
    out->Data = imslug_test::LoadFontFile(fileName, &out->Size);
    if (out->Data == nullptr)
        return false;
    out->Font = ImSlugCreateFontFromMemory(out->Data, out->Size, 0, true);
    if (out->Font == nullptr)
        return false;
    if (!stbtt_InitFont(&out->Info, out->Data, 0))
        return false;
    out->UnitsPerEm = (out->Info.data[out->Info.head + 18] << 8) | out->Info.data[out->Info.head + 19];
    return true;
}

static void CloseTestFont(TestFont* font)
{
    ImSlugDestroyFont(font->Font);
    std::free(font->Data);
}

// ---------------------------------------------------------------------------
// Pure shader math
// ---------------------------------------------------------------------------
static void TestRootCode()
{
    // The reference table (0x2E74 >> shift) & 0x0101 with
    // shift = 4*sign(y3) + 2*sign(y2) + sign(y1):
    //   (+,+,+) -> 0     (-,+,+) -> 0x100 (+,-,+) -> 0x101 (-,-,+) -> 0x100
    //   (+,+,-) -> 0x001 (-,+,-) -> 0x101 (+,-,-) -> 0x001 (-,-,-) -> 0
    struct Case
    {
        float y1, y2, y3;
        uint32_t expected;
    };
    const Case cases[] = {
        { 1.0f, 1.0f, 1.0f, 0x000 },
        { -1.0f, 1.0f, 1.0f, 0x100 },
        { 1.0f, -1.0f, 1.0f, 0x101 },
        { -1.0f, -1.0f, 1.0f, 0x100 },
        { 1.0f, 1.0f, -1.0f, 0x001 },
        { -1.0f, 1.0f, -1.0f, 0x101 },
        { 1.0f, -1.0f, -1.0f, 0x001 },
        { -1.0f, -1.0f, -1.0f, 0x000 },
        // Only the signs matter — magnitudes and signed zeros behave.
        { 0.25f, 3.5f, 0.125f, 0x000 },
        { -0.25f, 3.5f, 0.125f, 0x100 },
    };
    for (const Case& testCase : cases)
    {
        const uint32_t code = ImSlugCalcRootCode(testCase.y1, testCase.y2, testCase.y3);
        TEST_CHECKF(code == testCase.expected, "CalcRootCode(%g, %g, %g) = 0x%X, expected 0x%X", (double)testCase.y1,
                    (double)testCase.y2, (double)testCase.y3, code, testCase.expected);
    }
    // -0.0 has its sign bit set: sign is read from the bit pattern.
    TEST_CHECK(ImSlugCalcRootCode(-0.0f, -0.0f, -0.0f) == 0x000);  // (-,-,-) -> 0
    TEST_CHECK(ImSlugCalcRootCode(-0.0f, -0.0f, 0.5f) == 0x100);   // (-,-,+) -> 0x100
}

static float EvalQuadComponent(float p0, float p1, float p2, float t)
{
    const float u = 1.0f - t;
    return u * u * p0 + 2.0f * u * t * p1 + t * t * p2;
}

static void TestSolvers()
{
    // Linear (a == 0): y goes 1 -> 0 across the curve; the crossing sits at the
    // end point.
    {
        const float p12[4] = { 0.0f, 1.0f, 0.5f, 0.0f };  // p1=(0,1) p2=(0.5,0)
        const float p3[2] = { 1.0f, 0.0f };               // p3=(1,0)
        float r[2];
        ImSlugSolveHorizPoly(p12, p3, r);
        TEST_CHECK(std::fabs(r[0] - 1.0f) < 1e-6f && std::fabs(r[1] - 1.0f) < 1e-6f);
    }
    // Tangent quadratic: y(t) = 1 - 4t + 4t^2 = (1-2t)^2, double root at t=0.5;
    // x(t) = t.
    {
        const float p12[4] = { 0.0f, 1.0f, 0.5f, -1.0f };
        const float p3[2] = { 1.0f, 1.0f };
        float r[2];
        ImSlugSolveHorizPoly(p12, p3, r);
        TEST_CHECK(std::fabs(r[0] - 0.5f) < 1e-5f && std::fabs(r[1] - 0.5f) < 1e-5f);
    }
    // Two crossings: y(t) = 1 - 6t + 6t^2 (a=6, b=3, c=1); x(t) = t.
    {
        const float p12[4] = { 0.0f, 1.0f, 0.5f, -2.0f };
        const float p3[2] = { 1.0f, 1.0f };
        float r[2];
        ImSlugSolveHorizPoly(p12, p3, r);
        const float expectedT1 = 0.5f - std::sqrt(3.0f) / 6.0f;
        const float expectedT2 = 0.5f + std::sqrt(3.0f) / 6.0f;
        TEST_CHECK(std::fabs(r[0] - expectedT1) < 1e-5f);
        TEST_CHECK(std::fabs(r[1] - expectedT2) < 1e-5f);
    }
    // Vertical solver mirrors with x/y swapped.
    {
        const float p12[4] = { 1.0f, 0.0f, -2.0f, 0.5f };  // p1=(1,0) p2=(-2,0.5)
        const float p3[2] = { 1.0f, 1.0f };                // p3=(1,1)
        float r[2];
        ImSlugSolveVertPoly(p12, p3, r);
        const float expectedT1 = 0.5f - std::sqrt(3.0f) / 6.0f;
        const float expectedT2 = 0.5f + std::sqrt(3.0f) / 6.0f;
        TEST_CHECK(std::fabs(r[0] - expectedT1) < 1e-5f);
        TEST_CHECK(std::fabs(r[1] - expectedT2) < 1e-5f);
    }
    // The solver output really is C(t) at the crossing: spot-check by
    // evaluating the quadratic at t = (r - p1.x) / (a.x t^2 ... ) — instead
    // verify against the parametric evaluation used by the generator: find t
    // from the closed form and confirm the y component is ~0 at that t.
    {
        const float p1x = 0.2f, p1y = 0.9f;
        const float p2x = 0.6f, p2y = -1.4f;
        const float p3x = 1.1f, p3y = 0.3f;
        const float p12[4] = { p1x, p1y, p2x, p2y };
        const float p3[2] = { p3x, p3y };
        float r[2];
        ImSlugSolveHorizPoly(p12, p3, r);
        for (int i = 0; i < 2; ++i)
        {
            // Invert x(t) numerically (monotonic here) and check y(t) ~ 0 at
            // the crossing x reported by the solver.
            float lo = 0.0f, hi = 1.0f;
            for (int iteration = 0; iteration < 40; ++iteration)
            {
                const float mid = 0.5f * (lo + hi);
                if (EvalQuadComponent(p1x, p2x, p3x, mid) < r[i])
                    lo = mid;
                else
                    hi = mid;
            }
            const float t = 0.5f * (lo + hi);
            const float y = EvalQuadComponent(p1y, p2y, p3y, t);
            TEST_CHECKF(std::fabs(y) < 1e-3f, "solver root %d: y(t) = %g", i, (double)y);
        }
    }
}

static void TestCoverage()
{
    // Reference formula with xcov = 2c and xwgt = ywgt = 1:
    //   coverage = max(|2c| / 2, min(|2c|, 0)) = c
    // Nonzero fill: saturate.
    TEST_CHECK(std::fabs(ImSlugCalcCoverage(1.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_None) - 0.5f) < 1e-6f);
    TEST_CHECK(ImSlugCalcCoverage(2.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_None) == 1.0f);
    TEST_CHECK(ImSlugCalcCoverage(3.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_None) == 1.0f);
    TEST_CHECK(ImSlugCalcCoverage(-4.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_None) == 1.0f);  // |abs|
    // The weaker ray wins: |0.2*1 + -0.9*1| / 2 = 0.35 vs min(0.2, 0.9) = 0.2.
    TEST_CHECK(std::fabs(ImSlugCalcCoverage(0.2f, -0.9f, 1.0f, 1.0f, ImSlugGlyphFlags_None) - 0.35f) < 1e-6f);
    // Zero weights: the weighted term vanishes, the min(|xcov|, |ycov|) term is
    // the epsilon-guarded fallback (reference formula).
    TEST_CHECK(ImSlugCalcCoverage(1.0f, 1.0f, 0.0f, 0.0f, ImSlugGlyphFlags_None) == 1.0f);
    TEST_CHECK(ImSlugCalcCoverage(0.0f, 0.0f, 0.0f, 0.0f, ImSlugGlyphFlags_None) == 0.0f);
    // Even-odd wrap of the base coverage c: 1 - |1 - frac(c/2)*2|.
    TEST_CHECK(std::fabs(ImSlugCalcCoverage(1.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_EvenOdd) - 0.5f) < 1e-6f);
    TEST_CHECK(ImSlugCalcCoverage(2.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_EvenOdd) == 1.0f);  // c = 1 -> 1
    TEST_CHECK(ImSlugCalcCoverage(4.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_EvenOdd) == 0.0f);  // c = 2 -> 0
    TEST_CHECK(ImSlugCalcCoverage(6.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_EvenOdd) == 1.0f);  // c = 3 -> 1
    TEST_CHECK(ImSlugCalcCoverage(5.0f, 0.0f, 1.0f, 1.0f, ImSlugGlyphFlags_EvenOdd) == 0.5f);  // c = 2.5 -> 0.5
}

// ---------------------------------------------------------------------------
// stbtt comparison (acceptance gate)
// ---------------------------------------------------------------------------
// Threshold model, justified by the measured data (printed by the test):
//   * The CPU rasterizer mirrors the reference Slug coverage estimator, which
//     is NOT exact area coverage: its anti-aliasing ramp is centered on the
//     sample point, so even a pixel that stbtt rates exactly 0.0 or 1.0 can
//     differ by up to ~0.15 when the outline passes near the sample (measured
//     envelope). On genuinely anti-aliased edge pixels the two can disagree by
//     up to ~0.37.
//   * Deep pixels — where the whole 3x3 stbtt neighbourhood is saturated 0 or
//     1, i.e. the outline is more than a pixel away — must be saturated too
//     (kDeepPixelTolerance). This catches geometry, scaling and half-pixel
//     alignment bugs.
//   * The aggregate mean/p99 gates are regression tripwires with ~10-15%
//     headroom over the measured values. 16 px "size" means the
//     ascender..descender span is 16 px (an em is only ~12-14 px), so those
//     bitmaps are almost all edge pixels and the tolerated aggregate is
//     naturally larger.
struct DeltaStats
{
    double Mean;
    double P99;
    double Max;
    int PixelCount;
    int DeepPixelFailures;
    double DeepPixelWorst;
};

static const double kDeepPixelTolerance = 0.05;

static double MeanThresholdForSize(float pixelSize)
{
    if (pixelSize <= 16.0f)
        return 0.09;
    if (pixelSize <= 32.0f)
        return 0.05;
    return 0.03;
}

static double P99ThresholdForSize(float pixelSize)
{
    if (pixelSize <= 16.0f)
        return 0.27;
    if (pixelSize <= 32.0f)
        return 0.32;
    return 0.27;
}

static DeltaStats CompareGlyphAgainstStbtt(TestFont& font, ImSlugPools* pools, unsigned int codepoint, float pixelSize,
                                           const char* label)
{
    DeltaStats stats = { 0.0, 0.0, 0.0, 0, 0, 0.0 };

    // The same scale ImGui bakes with at this size.
    const float scale = stbtt_ScaleForPixelHeight(&font.Info, 1.0f) * pixelSize;
    int width = 0, height = 0, xOffset = 0, yOffset = 0;
    unsigned char* bitmap =
        stbtt_GetCodepointBitmap(&font.Info, scale, scale, (int)codepoint, &width, &height, &xOffset, &yOffset);
    TEST_CHECKF(bitmap != nullptr && width > 0 && height > 0, "%s: stbtt bitmap missing", label);
    if (bitmap == nullptr || width <= 0 || height <= 0)
        return stats;

    const ImSlugGlyphRecord* record = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font.Font, pools, codepoint, &record) == ImSlugBuildStatus_Ok);

    const float pixelsPerEm = scale * (float)font.UnitsPerEm;
    const float inverse = 1.0f / pixelsPerEm;
    const int kBinCount = 1001;
    int histogram[kBinCount] = {};
    double sum = 0.0;

    for (int j = 0; j < height; ++j)
    {
        for (int i = 0; i < width; ++i)
        {
            const float emX = ((float)xOffset + (float)i + 0.5f) * inverse;
            const float emY = -((float)yOffset + (float)j + 0.5f) * inverse;
            const float coverage = ImSlugRasterSample(pools, record, emX, emY, pixelsPerEm, pixelsPerEm);
            const float reference = (float)bitmap[j * width + i] / 255.0f;
            const double delta = std::fabs((double)coverage - (double)reference);
            sum += delta;
            if (delta > stats.Max)
                stats.Max = delta;

            // Deep pixel: the full 3x3 stbtt neighbourhood is saturated.
            bool deepZero = (i > 0 && i < width - 1 && j > 0 && j < height - 1);
            bool deepOne = deepZero;
            for (int dj = -1; dj <= 1 && (deepZero || deepOne); ++dj)
            {
                for (int di = -1; di <= 1; ++di)
                {
                    const unsigned char value = bitmap[(j + dj) * width + (i + di)];
                    if (value != 0)
                        deepZero = false;
                    if (value != 255)
                        deepOne = false;
                }
            }
            if (deepZero || deepOne)
            {
                const double deepDelta = deepZero ? (double)coverage : 1.0 - (double)coverage;
                if (deepDelta > stats.DeepPixelWorst)
                    stats.DeepPixelWorst = deepDelta;
                if (deepDelta > kDeepPixelTolerance)
                {
                    stats.DeepPixelFailures++;
                    if (stats.DeepPixelFailures <= 3)
                        std::printf("  deep-pixel mismatch %s at (%d,%d): ours %.4f vs stbtt %.0f\n", label, i, j,
                                    (double)coverage, deepOne ? 1.0 : 0.0);
                }
            }

            int bin = (int)(delta * (double)(kBinCount - 1) + 0.5);
            if (bin > kBinCount - 1)
                bin = kBinCount - 1;
            histogram[bin]++;
        }
    }
    stats.PixelCount = width * height;
    stats.Mean = sum / (double)stats.PixelCount;

    const int p99Target = (int)((double)stats.PixelCount * 0.99);
    int cumulative = 0;
    for (int bin = 0; bin < kBinCount; ++bin)
    {
        cumulative += histogram[bin];
        if (cumulative >= p99Target)
        {
            stats.P99 = (double)bin / (double)(kBinCount - 1);
            break;
        }
    }
    if (cumulative < p99Target)
        stats.P99 = 1.0;

    stbtt_FreeBitmap(bitmap, nullptr);

    std::printf("  %-22s %5.0f px: mean %.4f  p99 %.4f  max %.4f  deep-worst %.4f  (%dx%d)\n", label, (double)pixelSize,
                stats.Mean, stats.P99, stats.Max, stats.DeepPixelWorst, width, height);
    TEST_CHECKF(stats.DeepPixelFailures == 0, "%s: %d deep pixels differ by more than %.2f (worst %.4f)", label,
                stats.DeepPixelFailures, kDeepPixelTolerance, stats.DeepPixelWorst);
    const double meanThreshold = MeanThresholdForSize(pixelSize);
    const double p99Threshold = P99ThresholdForSize(pixelSize);
    TEST_CHECKF(stats.Mean <= meanThreshold, "%s: mean |delta| %.4f > %.3f", label, stats.Mean, meanThreshold);
    TEST_CHECKF(stats.P99 <= p99Threshold, "%s: p99 |delta| %.4f > %.3f", label, stats.P99, p99Threshold);
    return stats;
}

static void TestRasterGateForFont(const char* fontName)
{
    TestFont font;
    TEST_CHECK(OpenTestFont(fontName, &font));
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    const unsigned int codepoints[4] = { 'g', 'A', 'W', 0xE9 };
    const float sizes[3] = { 16.0f, 32.0f, 64.0f };
    for (unsigned int codepoint : codepoints)
    {
        DeltaStats statsAt16 = { 0.0, 0.0, 0.0, 0, 0, 0.0 };
        for (float size : sizes)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "%s U+%04X", fontName, codepoint);
            const DeltaStats stats = CompareGlyphAgainstStbtt(font, &pools, codepoint, size, label);
            if (size == 16.0f)
                statsAt16 = stats;
        }

        // Convergence: as the pixel footprint shrinks, the estimator must
        // approach exact area coverage. Measured mean at 256 px is 0.04-0.6%
        // against 1.5-8.3% at 16 px; a geometry or alignment regression would
        // keep the mean flat instead of collapsing.
        char largeLabel[64];
        std::snprintf(largeLabel, sizeof(largeLabel), "%s U+%04X", fontName, codepoint);
        const DeltaStats largeStats = CompareGlyphAgainstStbtt(font, &pools, codepoint, 256.0f, largeLabel);
        TEST_CHECKF(largeStats.Mean <= 0.012, "%s U+%04X: mean at 256 px %.4f > 0.012", fontName, codepoint,
                    largeStats.Mean);
        TEST_CHECKF(largeStats.Mean <= statsAt16.Mean * 0.25, "%s U+%04X: no convergence (16 px %.4f -> 256 px %.4f)",
                    fontName, codepoint, statsAt16.Mean, largeStats.Mean);
    }

    // Space: no curves, no coverage anywhere.
    {
        const ImSlugGlyphRecord* record = nullptr;
        TEST_CHECK(ImSlugGetGlyphRecord(font.Font, &pools, ' ', &record) == ImSlugBuildStatus_Ok);
        TEST_CHECK(record->CurveCount == 0);
        TEST_CHECK(ImSlugRasterSample(&pools, record, 0.1f, 0.1f, 32.0f, 32.0f) == 0.0f);
    }

    ImSlugDestroyPools(&pools);
    CloseTestFont(&font);
}

// ---------------------------------------------------------------------------
// Bands must never change the result (same math, last-touch order aside)
// ---------------------------------------------------------------------------
static void TestBandedVsUnbanded(const char* fontName)
{
    TestFont font;
    TEST_CHECK(OpenTestFont(fontName, &font));
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    const unsigned int codepoints[4] = { 'g', 'A', 'W', 0xE9 };
    for (unsigned int codepoint : codepoints)
    {
        const ImSlugGlyphRecord* record = nullptr;
        TEST_CHECK(ImSlugGetGlyphRecord(font.Font, &pools, codepoint, &record) == ImSlugBuildStatus_Ok);

        // Sample a grid covering the glyph plus a margin, at a size that puts
        // pixels near curves.
        const float pixelsPerEm = 48.0f;
        const float margin = 0.05f;
        float worst = 0.0f;
        for (int j = 0; j <= 48; ++j)
        {
            for (int i = 0; i <= 48; ++i)
            {
                const float emX =
                    record->MinX - margin + (record->MaxX - record->MinX + 2.0f * margin) * (float)i / 48.0f;
                const float emY =
                    record->MinY - margin + (record->MaxY - record->MinY + 2.0f * margin) * (float)j / 48.0f;
                const float banded = ImSlugRasterSample(&pools, record, emX, emY, pixelsPerEm, pixelsPerEm);
                const float unbanded =
                    ImSlugRasterSampleUnbanded(&pools.Curves, record, emX, emY, pixelsPerEm, pixelsPerEm);
                const float delta = std::fabs(banded - unbanded);
                if (delta > worst)
                    worst = delta;
            }
        }
        std::printf("  %s U+%04X: max |banded - unbanded| = %.6f\n", fontName, codepoint, (double)worst);
        // Banding changes only the accumulation order of contributions that are
        // exact zeros for excluded curves; allow fp reassociation noise.
        TEST_CHECKF(worst <= 1e-4f, "%s U+%04X: banded/unbanded disagree by %.6f", fontName, codepoint, (double)worst);
    }

    ImSlugDestroyPools(&pools);
    CloseTestFont(&font);
}

// Sanity: deep inside and far outside must be saturated/empty.
static void TestInteriorExterior()
{
    TestFont font;
    TEST_CHECK(OpenTestFont("Roboto-Regular.ttf", &font));
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    const ImSlugGlyphRecord* record = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font.Font, &pools, 'A', &record) == ImSlugBuildStatus_Ok);

    // A point inside the left stem of 'A' (about 15% into the glyph, a bit
    // above the baseline): strongly covered.
    const float stemX = record->MinX + 0.18f * (record->MaxX - record->MinX);
    const float stemY = record->MinY + 0.35f * (record->MaxY - record->MinY);
    const float inside = ImSlugRasterSample(&pools, record, stemX, stemY, 64.0f, 64.0f);
    TEST_CHECKF(inside > 0.9f, "'A' stem sample coverage %.3f (expected > 0.9)", (double)inside);

    // A point well outside the glyph.
    const float outside = ImSlugRasterSample(&pools, record, record->MinX - 0.5f, record->MinY - 0.5f, 64.0f, 64.0f);
    TEST_CHECK(outside == 0.0f);

    ImSlugDestroyPools(&pools);
    CloseTestFont(&font);
}

// ---------------------------------------------------------------------------
// ImSlugRasterGlyphRect convention (the frozen API M2 depends on)
// ---------------------------------------------------------------------------
// Row 0 is the top row; (centerX0, centerY0) is the em coordinate of pixel
// (0,0)'s center; x increases by 1/ppe per column and y decreases by 1/ppe per
// row (em space is Y up). 'I' is a plain rectangle in Roboto, so a single
// column through the stem pins the mapping and the orientation.
static void TestGlyphRectConvention()
{
    TestFont font;
    TEST_CHECK(OpenTestFont("Roboto-Regular.ttf", &font));
    ImSlugPools pools;
    ImSlugInitPools(&pools);

    const ImSlugGlyphRecord* record = nullptr;
    TEST_CHECK(ImSlugGetGlyphRecord(font.Font, &pools, 'I', &record) == ImSlugBuildStatus_Ok);

    const float ppe = 64.0f;
    const float inverse = 1.0f / ppe;
    const float centerX0 = 0.5f * (record->MinX + record->MaxX);  // stem center
    const float centerY0 = record->MaxY + 2.0f * inverse;         // two pixels above the cap
    const int width = 3;
    const int height = (int)((centerY0 - record->MinY) / inverse) + 3;  // through the baseline + margin
    TEST_CHECK(height > 0 && height <= 200);
    float coverage[3 * 200];
    ImSlugRasterGlyphRect(&pools, record, centerX0, centerY0, ppe, coverage, width, height);

    // Every pixel is the sample at (centerX0 + x/ppe, centerY0 - y/ppe).
    float worst = 0.0f;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float expected = ImSlugRasterSample(&pools, record, centerX0 + (float)x * inverse,
                                                      centerY0 - (float)y * inverse, ppe, ppe);
            const float delta = std::fabs(coverage[y * width + x] - expected);
            if (delta > worst)
                worst = delta;
        }
    }
    TEST_CHECKF(worst <= 1e-6f, "rect mapping differs from direct samples by %.8f", (double)worst);

    // Orientation: the top row is above the glyph (empty), the stem center is
    // saturated, and the last row is below the baseline (empty). A flipped em-Y
    // mapping would report zero coverage for the whole column.
    const int stemRow = (int)((centerY0 - 0.5f * (record->MinY + record->MaxY)) / inverse);
    TEST_CHECK(stemRow > 0 && stemRow < height - 1);
    TEST_CHECKF(coverage[stemRow * width + 1] > 0.99f, "stem center coverage %.4f",
                (double)coverage[stemRow * width + 1]);
    TEST_CHECK(coverage[0 * width + 1] == 0.0f);
    TEST_CHECK(coverage[(height - 1) * width + 1] == 0.0f);

    // The first covered row is where the sample drops just below the cap
    // height: emY(row) = centerY0 - row/ppe ~= record->MaxY (within the 1 px
    // sampler ramp).
    int firstCovered = -1;
    for (int y = 0; y < height && firstCovered < 0; ++y)
    {
        if (coverage[y * width + 1] > 0.0f)
            firstCovered = y;
    }
    TEST_CHECK(firstCovered > 0);
    const float firstEmY = centerY0 - (float)firstCovered * inverse;
    TEST_CHECKF(firstEmY <= record->MaxY + 1.5f * inverse && firstEmY >= record->MaxY - 1.5f * inverse,
                "first covered row at emY %.4f (cap height %.4f)", (double)firstEmY, (double)record->MaxY);

    ImSlugDestroyPools(&pools);
    CloseTestFont(&font);
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("imslug_refraster: shader math + stbtt coverage gate (deep pixels <= %.2f, aggregate per size)\n",
                kDeepPixelTolerance);
    TestRootCode();
    TestSolvers();
    TestCoverage();
    TestInteriorExterior();
    TestGlyphRectConvention();
    TestBandedVsUnbanded("Roboto-Regular.ttf");
    TestBandedVsUnbanded("Inter-Regular.ttf");
    TestRasterGateForFont("Roboto-Regular.ttf");
    TestRasterGateForFont("Inter-Regular.ttf");
    return imslug_test::Finish("imslug_refraster");
}
