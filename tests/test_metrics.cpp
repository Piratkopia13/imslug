// imslug M1 tests — glyph metrics.
//
// The M1 contract: layout stays pixel-identical to Dear ImGui's stb loader, so
// every metric is re-derived here from stbtt with the formulas of ImGui
// v1.92.9b (imgui_draw.cpp lines 4725/4761-4765/4790-4849) and compared
// exactly against ImSlugGetGlyphMetrics. The test-only ImGui parity suite
// (test_imguiparity.cpp) additionally compares against the real loader.

#include "imslug_internal.h"

#include "test_util.h"

#include <cstdio>

using namespace ImSlug;

struct ExpectedMetrics
{
    float AdvanceX;
    float X0, Y0, X1, Y1;
};

// Independent re-derivation of ImGui's default stb-loader metrics for a glyph
// at a bake size (OversampleH/V = 1, RasterizerDensity = 1, ExtraSizeScale = 1,
// GlyphOffset = 0).
static ExpectedMetrics ComputeExpectedMetrics(const stbtt_fontinfo& info, int glyphIndex, float bakeSizePixels)
{
    ExpectedMetrics metrics = {};
    const float scale = stbtt_ScaleForPixelHeight(&info, 1.0f) * bakeSizePixels;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&info, glyphIndex, scale, scale, 0.0f, 0.0f, &x0, &y0, &x1, &y1);
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyphIndex, &advance, &lsb);

    metrics.AdvanceX = (float)advance * scale;
    if (x0 != x1 && y0 != y1)
    {
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        const float ascentPx = std::ceil((float)ascent * scale);   // ImCeil
        const float ascentOffset = (float)(int)(ascentPx + 0.5f);  // IM_ROUND
        metrics.X0 = (float)x0;
        metrics.X1 = (float)x1;
        metrics.Y0 = (float)y0 + ascentOffset;
        metrics.Y1 = (float)y1 + ascentOffset;
    }
    return metrics;
}

static bool ExactOrClose(float a, float b)
{
    if (a == b)
        return true;
    return std::fabs(a - b) <= 1e-5f * (std::fabs(a) > 1.0f ? std::fabs(a) : 1.0f);
}

static void TestMetricsForFont(const char* fontName, float unitsPerEm)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    TEST_CHECK(font != nullptr);
    stbtt_fontinfo info;
    stbtt_InitFont(&info, fontData, 0);

    const float sizes[] = { 9.0f, 16.0f, 32.0f, 48.0f, 64.0f };
    const unsigned int codepoints[] = { ' ', 'I', 'g', 'A', 'W', '|', 0xE9 /* é */ };

    for (float size : sizes)
    {
        // Font metrics: pixels per em and ceil/floor ascent/descent.
        ImSlugFontMetrics fontMetrics;
        ImSlugGetFontMetrics(font, size, &fontMetrics);
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        const float scale = stbtt_ScaleForPixelHeight(&info, 1.0f) * size;
        TEST_CHECK(fontMetrics.UnitsPerEm == (int)unitsPerEm);
        TEST_CHECK(fontMetrics.HheaAscent == ascent && fontMetrics.HheaDescent == descent);
        TEST_CHECK(ExactOrClose(fontMetrics.ScaleForPixelHeight, scale));
        TEST_CHECK(ExactOrClose(fontMetrics.PixelsPerEm, scale * unitsPerEm));
        TEST_CHECK(fontMetrics.Ascent == std::ceil((float)ascent * scale));
        TEST_CHECK(fontMetrics.Descent == std::floor((float)descent * scale));

        float worst = 0.0f;
        for (unsigned int codepoint : codepoints)
        {
            const int glyphIndex = stbtt_FindGlyphIndex(&info, (int)codepoint);
            TEST_CHECKF(glyphIndex > 0, "%s U+%04X not in font", fontName, codepoint);
            if (glyphIndex <= 0)
                continue;

            ImSlugGlyphMetrics ours;
            TEST_CHECK(ImSlugGetGlyphMetrics(font, codepoint, size, &ours));
            const ExpectedMetrics expected = ComputeExpectedMetrics(info, glyphIndex, size);

            TEST_CHECKF(ExactOrClose(ours.AdvanceX, expected.AdvanceX), "%s U+%04X @ %.1f: AdvanceX %.5f vs %.5f",
                        fontName, codepoint, (double)size, (double)ours.AdvanceX, (double)expected.AdvanceX);
            TEST_CHECKF(ExactOrClose(ours.X0, expected.X0) && ExactOrClose(ours.Y0, expected.Y0) &&
                            ExactOrClose(ours.X1, expected.X1) && ExactOrClose(ours.Y1, expected.Y1),
                        "%s U+%04X @ %.1f: box (%.3f %.3f %.3f %.3f) vs (%.3f %.3f %.3f %.3f)", fontName, codepoint,
                        (double)size, (double)ours.X0, (double)ours.Y0, (double)ours.X1, (double)ours.Y1,
                        (double)expected.X0, (double)expected.Y0, (double)expected.X1, (double)expected.Y1);

            // The em-space advance must agree with the pixels-per-em relation:
            // AdvanceX = AdvanceEm * PixelsPerEm.
            TEST_CHECKF(ExactOrClose(ours.AdvanceX, ours.AdvanceEm * fontMetrics.PixelsPerEm),
                        "%s U+%04X @ %.1f: AdvanceEm %.6f inconsistent with AdvanceX", fontName, codepoint,
                        (double)size, (double)ours.AdvanceEm);

            worst = std::fmax(worst, std::fabs(ours.AdvanceX - expected.AdvanceX));
        }
        std::printf("  %s @ %5.1f px: worst |AdvanceX delta| = %.6f\n", fontName, (double)size, (double)worst);
    }

    // Missing codepoints report "not found" and write nothing.
    ImSlugGlyphMetrics missing;
    missing.AdvanceX = 1234.0f;
    TEST_CHECK(!ImSlugGetGlyphMetrics(font, 0x10FFFD, 16.0f, &missing));

    ImSlugDestroyFont(font);
    std::free(fontData);
}

// The record's em-space advance and bbox must agree with the metrics API and
// with stbtt's glyf box.
static void TestRecordConsistency(const char* fontName)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    ImSlugPools pools;
    ImSlugInitPools(&pools);
    stbtt_fontinfo info;
    stbtt_InitFont(&info, fontData, 0);
    const int unitsPerEm = (info.data[info.head + 18] << 8) | info.data[info.head + 19];

    const unsigned int codepoints[4] = { 'g', 'A', 'W', 0xE9 };
    for (unsigned int codepoint : codepoints)
    {
        const ImSlugGlyphRecord* record = nullptr;
        TEST_CHECK(ImSlugGetGlyphRecord(font, &pools, codepoint, &record) == ImSlugBuildStatus_Ok);
        ImSlugGlyphMetrics metrics;
        TEST_CHECK(ImSlugGetGlyphMetrics(font, codepoint, 32.0f, &metrics));
        TEST_CHECKF(ExactOrClose(record->Advance, metrics.AdvanceEm), "%s U+%04X: record advance %.8f vs %.8f",
                    fontName, codepoint, (double)record->Advance, (double)metrics.AdvanceEm);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBox(&info, stbtt_FindGlyphIndex(&info, (int)codepoint), &x0, &y0, &x1, &y1);
        const float scale = 1.0f / (float)unitsPerEm;
        TEST_CHECKF(record->MinX >= (float)x0 * scale - 1e-4f && record->MaxX <= (float)x1 * scale + 1e-4f &&
                        record->MinY >= (float)y0 * scale - 1e-4f && record->MaxY <= (float)y1 * scale + 1e-4f,
                    "%s U+%04X: em bbox [%.4f %.4f %.4f %.4f] outside glyf box", fontName, codepoint,
                    (double)record->MinX, (double)record->MinY, (double)record->MaxX, (double)record->MaxY);
    }

    ImSlugDestroyPools(&pools);
    ImSlugDestroyFont(font);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("imslug_metrics: ImGui-parity advance/box metrics\n");
    TestMetricsForFont("Roboto-Regular.ttf", 2048.0f);
    TestMetricsForFont("Inter-Regular.ttf", 2048.0f);
    TestRecordConsistency("Roboto-Regular.ttf");
    TestRecordConsistency("Inter-Regular.ttf");
    return imslug_test::Finish("imslug_metrics");
}
