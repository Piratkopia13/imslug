// imslug — M2 unit test: the frozen glyph-instance record.
//
// Pins the layout (`sizeof`, field offsets — complementing the static_asserts
// in imslug_internal.h), the packed bandMax word (band maxima + E at bit 28,
// PLAN §11 D8(g)) and the placement contract of ImSlugBuildGlyphInstance:
//   screen (px, Y down) = (penX + emX * P, penY - emY * P)
// with the corner correspondence the vertex shader builds from gl_VertexIndex:
// em (MinX, MinY) maps to screen (ScreenMinX, ScreenMaxY) — i.e. the Y flip
// lives in the screen rect. The golden tests verify the same mapping end to end
// against the CPU rasterizer; this suite keeps the contract cheap to check.

#include "test_util.h"

#include "imslug_internal.h"

#include <stddef.h>

using namespace ImSlug;

namespace
{
    ImSlugGlyphRecord MakeRecord()
    {
        ImSlugGlyphRecord record = {};
        record.BandLocX = 1234;
        record.BandLocY = 7;
        record.BandMaxX = 4;
        record.BandMaxY = 9;
        record.BandScaleX = 5.0f;
        record.BandScaleY = 6.0f;
        record.BandOffsetX = -0.25f;
        record.BandOffsetY = -0.5f;
        record.CurveLocX = 100;
        record.CurveLocY = 3;
        record.MinX = 0.1f;
        record.MinY = -0.2f;
        record.MaxX = 0.7f;
        record.MaxY = 0.8f;
        record.Advance = 0.6f;
        record.CurveCount = 12;
        record.Flags = ImSlugGlyphFlags_EvenOdd;
        return record;
    }

    // em coordinate of a screen point, from the instance: em = anchor + J * d,
    // anchored at the rect corner that matches em (MinX, MaxY) = (ScreenMinX,
    // ScreenMinY) — the same affine map the shader's dilated texcoords define.
    void ScreenToEm(const ImSlugGlyphInstance& instance, float screenX, float screenY, float* outEmX, float* outEmY)
    {
        const float dx = screenX - instance.ScreenMinX;
        const float dy = screenY - instance.ScreenMinY;
        *outEmX = instance.EmMinX + dx * instance.Jac00 + dy * instance.Jac10;
        *outEmY = instance.EmMaxY + dx * instance.Jac01 + dy * instance.Jac11;
    }

    void TestLayout()
    {
        TEST_CHECK(sizeof(ImSlugGlyphInstance) == 96);  // M5: + PaintIndex (v2)
        TEST_CHECK(offsetof(ImSlugGlyphInstance, EmMinX) == 0);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, ScreenMinX) == 16);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, Jac00) == 32);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, BandScaleX) == 48);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, GlyphLocX) == 64);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, BandMaxWord) == 72);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, ColorR) == 76);
        TEST_CHECK(offsetof(ImSlugGlyphInstance, PaintIndex) == 92);

        // M5 paint record layout (GPU-visible std430 contract).
        TEST_CHECK(sizeof(ImSlugPaintRecord) == 352);
        TEST_CHECK(offsetof(ImSlugPaintRecord, Xform0) == 16);
        TEST_CHECK(offsetof(ImSlugPaintRecord, Geo0) == 48);
        TEST_CHECK(offsetof(ImSlugPaintRecord, Clip) == 80);
        TEST_CHECK(offsetof(ImSlugPaintRecord, Stops) == 96);
    }

    void TestPackedWord()
    {
        const uint32_t plain = ImSlugPackBandMaxWord(4, 9, ImSlugGlyphFlags_None);
        TEST_CHECK((plain & 0xFFFFu) == 4u);
        TEST_CHECK(((plain >> 16) & 0xFFu) == 9u);
        TEST_CHECK((plain & 0x10000000u) == 0u);

        const uint32_t evenOdd = ImSlugPackBandMaxWord(31, 1, ImSlugGlyphFlags_EvenOdd);
        TEST_CHECK((evenOdd & 0x10000000u) != 0u);        // E at bit 28 ...
        TEST_CHECK((((evenOdd >> 16) & 0x1000u) != 0u));  // ... = bit 12 of the high half
        TEST_CHECK((evenOdd & 0xFFu) == 31u);
    }

    void TestInstanceBuild()
    {
        const ImSlugGlyphRecord record = MakeRecord();
        const float penX = 40.0f;
        const float penY = 100.0f;
        const float pixelsPerEm = 32.0f;
        ImSlugGlyphInstance instance = {};
        ImSlugBuildGlyphInstance(&record, penX, penY, pixelsPerEm, 1.0f, 0.5f, 0.25f, 1.0f, &instance);

        TEST_CHECK(instance.EmMinX == record.MinX && instance.EmMinY == record.MinY);
        TEST_CHECK(instance.EmMaxX == record.MaxX && instance.EmMaxY == record.MaxY);
        TEST_CHECK(instance.ScreenMinX == penX + record.MinX * pixelsPerEm);
        TEST_CHECK(instance.ScreenMaxX == penX + record.MaxX * pixelsPerEm);
        TEST_CHECK(instance.ScreenMinY == penY - record.MaxY * pixelsPerEm);  // top = em max Y
        TEST_CHECK(instance.ScreenMaxY == penY - record.MinY * pixelsPerEm);
        TEST_CHECK(instance.Jac00 == 1.0f / pixelsPerEm && instance.Jac11 == -1.0f / pixelsPerEm);
        TEST_CHECK(instance.Jac01 == 0.0f && instance.Jac10 == 0.0f);
        TEST_CHECK(instance.BandScaleX == record.BandScaleX && instance.BandOffsetY == record.BandOffsetY);
        TEST_CHECK(instance.GlyphLocX == (uint32_t)record.BandLocX && instance.GlyphLocY == (uint32_t)record.BandLocY);
        TEST_CHECK(instance.BandMaxWord == ImSlugPackBandMaxWord(record.BandMaxX, record.BandMaxY, record.Flags));
        TEST_CHECK(instance.ColorR == 1.0f && instance.ColorG == 0.5f && instance.ColorB == 0.25f &&
                   instance.ColorA == 1.0f);

        // Corner mapping: em (MinX, MinY) is the screen rect's bottom-left
        // (ScreenMinX, ScreenMaxY); em (MaxX, MaxY) is its top-right. The
        // affine map re-derives the anchors, so compare with a tiny epsilon
        // (one ulp of the pen coordinates; the shader's interpolation rounds
        // the same way).
        const float epsilon = 1e-5f;
        float emX = 0.0f;
        float emY = 0.0f;
        ScreenToEm(instance, instance.ScreenMinX, instance.ScreenMaxY, &emX, &emY);
        TEST_CHECKF(std::fabs(emX - record.MinX) <= epsilon && std::fabs(emY - record.MinY) <= epsilon,
                    "em min corner -> (%f, %f)", emX, emY);
        ScreenToEm(instance, instance.ScreenMaxX, instance.ScreenMinY, &emX, &emY);
        TEST_CHECKF(std::fabs(emX - record.MaxX) <= epsilon && std::fabs(emY - record.MaxY) <= epsilon,
                    "em max corner -> (%f, %f)", emX, emY);

        // 1 px in screen space is exactly 1 / P em in both axes (the shader's
        // fwidth sees the same value, hence the same coverage ramps).
        ScreenToEm(instance, instance.ScreenMinX + 1.0f, instance.ScreenMaxY - 1.0f, &emX, &emY);
        TEST_CHECKF(std::fabs(emX - (record.MinX + 1.0f / pixelsPerEm)) <= epsilon, "screen +1px -> em %f", emX);
        TEST_CHECKF(std::fabs(emY - (record.MinY + 1.0f / pixelsPerEm)) <= epsilon, "screen -1px -> em %f", emY);

        // M5: a plain instance carries no paint record.
        TEST_CHECK(instance.PaintIndex == kNoPaintIndex);
    }

    // M5: the per-axis scaled builder used by color layers whose transform is a
    // positive axis-aligned scale. Same corner contract, per-axis mapping.
    void TestInstanceBuildScaled()
    {
        const ImSlugGlyphRecord record = MakeRecord();
        const float penX = 10.0f;
        const float penY = 50.0f;
        const float pixelsPerEmX = 20.0f;
        const float pixelsPerEmY = 40.0f;
        ImSlugGlyphInstance instance = {};
        ImSlugBuildGlyphInstanceScaled(&record, penX, penY, pixelsPerEmX, pixelsPerEmY, 1.0f, 1.0f, 1.0f, 0.5f,
                                       &instance);

        TEST_CHECK(instance.ScreenMinX == penX + record.MinX * pixelsPerEmX);
        TEST_CHECK(instance.ScreenMaxX == penX + record.MaxX * pixelsPerEmX);
        TEST_CHECK(instance.ScreenMinY == penY - record.MaxY * pixelsPerEmY);
        TEST_CHECK(instance.ScreenMaxY == penY - record.MinY * pixelsPerEmY);
        TEST_CHECK(instance.Jac00 == 1.0f / pixelsPerEmX && instance.Jac11 == -1.0f / pixelsPerEmY);
        TEST_CHECK(instance.PaintIndex == kNoPaintIndex);
        TEST_CHECK(instance.ColorA == 0.5f);

        // The inverse Jacobian maps the corners back to the record's em bbox.
        float emX = 0.0f;
        float emY = 0.0f;
        const float epsilon = 1e-5f;
        ScreenToEm(instance, instance.ScreenMinX, instance.ScreenMaxY, &emX, &emY);
        TEST_CHECKF(std::fabs(emX - record.MinX) <= epsilon && std::fabs(emY - record.MinY) <= epsilon,
                    "scaled em min corner -> (%f, %f)", emX, emY);
        ScreenToEm(instance, instance.ScreenMaxX, instance.ScreenMinY, &emX, &emY);
        TEST_CHECKF(std::fabs(emX - record.MaxX) <= epsilon && std::fabs(emY - record.MaxY) <= epsilon,
                    "scaled em max corner -> (%f, %f)", emX, emY);
    }
}  // namespace

int main()
{
    TestLayout();
    TestPackedWord();
    TestInstanceBuild();
    TestInstanceBuildScaled();
    return imslug_test::Finish("imslug_instance");
}
