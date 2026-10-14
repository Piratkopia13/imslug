// imslug — M7 packed-asset loader test (no parser required).
//
// Covers the `.imslug` container v1 reader (imslug_packed.*) without needing
// the TTF parser: the committed test asset is loaded through the packed font
// kind, records are built into the shared pools, metrics are checked against
// the stored source data, the CPU rasterizer produces sane ink, and corrupt /
// truncated / version-mismatched inputs fail cleanly (never crash, never
// partially load).
//
// The semantic round-trip against the runtime TTF generator (byte-exact pool
// comparison) lives in test_packed_roundtrip.cpp, which needs the parser.

#include "imslug_c.h"
#include "imslug_internal.h"
#include "imslug_packed.h"

#include "test_util.h"

#include <algorithm>
#include <math.h>

#ifndef IMSLUG_PACKED_TEST_ASSET
#define IMSLUG_PACKED_TEST_ASSET "tests/assets/Roboto-ASCII.imslug"
#endif

namespace
{
    // Deterministic 32-bit LCG (corruption fuzz; no std::random dependency).
    struct Lcg
    {
        uint32_t State = 0x12345678u;
        uint32_t Next()
        {
            State = State * 1664525u + 1013904223u;
            return State;
        }
    };

    uint32_t Fnv1a32(const unsigned char* data, int size)
    {
        uint32_t hash = 2166136261u;
        for (int i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    bool IsFinite2(float a, float b)
    {
        return std::isfinite(a) && std::isfinite(b);
    }

    struct AssetBytes
    {
        unsigned char* Data = nullptr;
        int Size = 0;

        ~AssetBytes() { std::free(Data); }
    };

    // ---------------------------------------------------------------------
    // Container sniffing + validation
    // ---------------------------------------------------------------------
    void TestSniffAndValidate()
    {
        AssetBytes asset;
        asset.Data = imslug_test::LoadFile(IMSLUG_PACKED_TEST_ASSET, &asset.Size);
        TEST_CHECK(asset.Data != nullptr);
        if (asset.Data == nullptr)
            return;

        TEST_CHECK(ImSlug::ImSlugPackedIsContainer(asset.Data, asset.Size));
        TEST_CHECK(!ImSlug::ImSlugPackedIsContainer(nullptr, 0));
        TEST_CHECK(!ImSlug::ImSlugPackedIsContainer(asset.Data, 4));
        {
            const char notPacked[] = "NotAnAsset!";
            TEST_CHECK(!ImSlug::ImSlugPackedIsContainer(notPacked, (int)sizeof(notPacked)));
        }
        {
            int fontSize = 0;
            unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontSize);
            TEST_CHECK(fontData != nullptr);
            if (fontData != nullptr)
            {
                TEST_CHECK(!ImSlug::ImSlugPackedIsContainer(fontData, fontSize));
                std::free(fontData);
            }
        }

        ImSlug::ImSlugPackedInfo info;
        char message[256];
        TEST_CHECK(ImSlug::ImSlugValidatePackedMemory(asset.Data, asset.Size, &info, message, (int)sizeof(message)));
        TEST_CHECK(info.Version == 1);
        TEST_CHECK(info.UnitsPerEm == 2048);
        TEST_CHECK(info.GlyphCount == 95);
        TEST_CHECK(info.CodepointCount == 95);
        TEST_CHECK(info.CurveRows >= 1 && info.BandRows >= 1);
        TEST_CHECK(info.AssetSize == (uint32_t)asset.Size);
        TEST_CHECK(info.SizeMinPx == 8.0f && info.SizeMaxPx == 72.0f);
        TEST_CHECKF(std::strcmp(info.SourceName, "Roboto-Regular.ttf") == 0, "source name: %s", info.SourceName);
        {
            int fontSize = 0;
            unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontSize);
            TEST_CHECK(fontData != nullptr);
            if (fontData != nullptr)
            {
                TEST_CHECK(info.SourceSize == (uint32_t)fontSize);
                TEST_CHECK(info.SourceChecksum == Fnv1a32(fontData, fontSize));
                std::free(fontData);
            }
        }

        // Truncation at many lengths: never valid, never a crash.
        const int lengths[] = { 0, 1, 8, 111, 112, 113, 200, 1000, asset.Size / 2, asset.Size - 1 };
        for (int length : lengths)
        {
            char detail[256];
            TEST_CHECK(!ImSlug::ImSlugValidatePackedMemory(asset.Data, length, nullptr, detail, (int)sizeof(detail)));
            TEST_CHECK(detail[0] != '\0');  // a reason is always written
            TEST_CHECK(ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, length) == nullptr);
        }

        // Field-level corruption: each mutation must be rejected with a reason.
        struct Mutation
        {
            int Offset;
            uint8_t Value;
            const char* What;
        };
        const Mutation mutations[] = {
            { 0, 'X', "magic" },
            { 8, 2, "version" },
            { 12, 0, "header size" },
            { 20, 1, "flags" },
        };
        for (const Mutation& mutation : mutations)
        {
            unsigned char* copy = (unsigned char*)std::malloc((size_t)asset.Size);
            TEST_CHECK(copy != nullptr);
            if (copy == nullptr)
                continue;
            std::memcpy(copy, asset.Data, (size_t)asset.Size);
            copy[mutation.Offset] = mutation.Value;
            char detail[256];
            TEST_CHECKF(!ImSlug::ImSlugValidatePackedMemory(copy, asset.Size, nullptr, detail, (int)sizeof(detail)),
                        "mutation of %s was accepted", mutation.What);
            TEST_CHECK(ImSlug::ImSlugCreateFontFromPackedMemory(copy, asset.Size) == nullptr);
            std::free(copy);
        }

        // The unmodified asset must remain valid after all that.
        TEST_CHECK(ImSlug::ImSlugValidatePackedMemory(asset.Data, asset.Size, nullptr, message, (int)sizeof(message)));

        // Deterministic random mutations: validate + create must never crash,
        // and creation must succeed exactly when validation does.
        Lcg rng;
        int accepted = 0;
        int rejected = 0;
        for (int iteration = 0; iteration < 400; ++iteration)
        {
            unsigned char* copy = (unsigned char*)std::malloc((size_t)asset.Size);
            if (copy == nullptr)
                break;
            std::memcpy(copy, asset.Data, (size_t)asset.Size);
            const uint32_t mutationsCount = 1 + rng.Next() % 3;
            for (uint32_t m = 0; m < mutationsCount; ++m)
                copy[rng.Next() % (uint32_t)asset.Size] ^= (uint8_t)(1u << (rng.Next() % 8));
            char detail[256];
            const bool valid =
                ImSlug::ImSlugValidatePackedMemory(copy, asset.Size, nullptr, detail, (int)sizeof(detail));
            if (valid)
            {
                ++accepted;
                ImSlug::ImSlugFont* font = ImSlug::ImSlugCreateFontFromPackedMemory(copy, asset.Size);
                TEST_CHECK(font != nullptr);
                if (font != nullptr)
                    ImSlug::ImSlugDestroyFont(font);
            }
            else
            {
                ++rejected;
                TEST_CHECK(detail[0] != '\0');
                TEST_CHECK(ImSlug::ImSlugCreateFontFromPackedMemory(copy, asset.Size) == nullptr);
            }
            std::free(copy);
        }
        TEST_CHECKF(rejected > accepted, "mutations: %d rejected, %d accepted", rejected, accepted);
    }

    // ---------------------------------------------------------------------
    // Packed font: lookup, metrics, records, raster
    // ---------------------------------------------------------------------
    void TestPackedFont()
    {
        AssetBytes asset;
        asset.Data = imslug_test::LoadFile(IMSLUG_PACKED_TEST_ASSET, &asset.Size);
        TEST_CHECK(asset.Data != nullptr);
        if (asset.Data == nullptr)
            return;

        ImSlug::ImSlugFont* font = ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, asset.Size);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;

        TEST_CHECK(ImSlug::ImSlugGetGlyphCount(font) == 95);
        const int glyphA = ImSlug::ImSlugGetGlyphIndex(font, 'A');
        const int glyphSpace = ImSlug::ImSlugGetGlyphIndex(font, ' ');
        const int glyphDot = ImSlug::ImSlugGetGlyphIndex(font, '.');
        TEST_CHECK(glyphA > 0);
        TEST_CHECK(glyphSpace > 0);
        TEST_CHECK(glyphDot > 0);
        TEST_CHECK(ImSlug::ImSlugGetGlyphIndex(font, 0x4E00) == 0);  // CJK: not in the asset

        // Font metrics must follow the stored hhea values with the TTF
        // formulas (stbtt_ScaleForPixelHeight(font, 1.0f) * size).
        for (float size : { 8.0f, 12.0f, 16.0f, 32.0f, 72.0f })
        {
            ImSlug::ImSlugFontMetrics metrics;
            ImSlug::ImSlugGetFontMetrics(font, size, &metrics);
            const float scale = (1.0f / (float)(metrics.HheaAscent - metrics.HheaDescent)) * size;
            TEST_CHECK(metrics.UnitsPerEm == 2048);
            TEST_CHECK(metrics.HheaAscent == 2146);
            TEST_CHECK(metrics.HheaDescent == -555);
            TEST_CHECK(metrics.ScaleForPixelHeight == scale);
            TEST_CHECK(metrics.PixelsPerEm == scale * 2048.0f);
            TEST_CHECK(metrics.Ascent == ceilf((float)metrics.HheaAscent * scale));
            TEST_CHECK(metrics.Descent == floorf((float)metrics.HheaDescent * scale));
            TEST_CHECK(metrics.Ascent > 0.0f && metrics.Descent < 0.0f);
        }

        // Glyph metrics: 'A' is visible and advance-positive; ' ' is invisible
        // (ImGui's zeroed corners) but still advances.
        {
            ImSlug::ImSlugGlyphMetrics metricsA;
            TEST_CHECK(ImSlug::ImSlugGetGlyphMetricsByIndex(font, glyphA, 32.0f, &metricsA));
            TEST_CHECK(metricsA.AdvanceX > 0.0f);
            TEST_CHECK(metricsA.AdvanceEm > 0.0f);
            TEST_CHECK(metricsA.X1 > metricsA.X0);
            TEST_CHECK(metricsA.Y1 > metricsA.Y0);
            TEST_CHECK(metricsA.MinX < metricsA.MaxX && metricsA.MinY < metricsA.MaxY);
            TEST_CHECK(IsFinite2(metricsA.MinX, metricsA.MaxY));
        }
        {
            ImSlug::ImSlugGlyphMetrics metricsSpace;
            TEST_CHECK(ImSlug::ImSlugGetGlyphMetricsByIndex(font, glyphSpace, 32.0f, &metricsSpace));
            TEST_CHECK(metricsSpace.AdvanceX > 0.0f);
            TEST_CHECK(metricsSpace.X0 == 0.0f && metricsSpace.Y0 == 0.0f && metricsSpace.X1 == 0.0f &&
                       metricsSpace.Y1 == 0.0f);
        }

        // Raw source metrics are stored per glyph.
        {
            ImSlug::ImSlugGlyphSourceMetrics source;
            TEST_CHECK(ImSlug::ImSlugGetGlyphSourceMetrics(font, glyphA, &source));
            TEST_CHECK(source.AdvanceFu > 0);
            TEST_CHECK(source.BoxX1 > source.BoxX0);
            TEST_CHECK(ImSlug::ImSlugGetGlyphSourceMetrics(font, 0, &source) == false);
        }

        // Records: build the whole ASCII range into the shared pools.
        ImSlug::ImSlugPools pools;
        ImSlug::ImSlugInitPools(&pools);
        int built = 0;
        const ImSlug::ImSlugGlyphRecord* recordA = nullptr;
        for (unsigned int codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
        {
            const ImSlug::ImSlugGlyphRecord* record = nullptr;
            const ImSlug::ImSlugBuildStatus status = ImSlug::ImSlugGetGlyphRecord(font, &pools, codepoint, &record);
            TEST_CHECKF(status == ImSlug::ImSlugBuildStatus_Ok, "codepoint 0x%02X: status %d", codepoint, (int)status);
            if (status != ImSlug::ImSlugBuildStatus_Ok)
                continue;
            ++built;
            if (codepoint == 'A')
                recordA = record;
        }
        TEST_CHECK(built == 95);
        TEST_CHECK(pools.Curves.UsedRows >= 1 && pools.Bands.UsedRows >= 1);
        if (recordA == nullptr)
        {
            ImSlug::ImSlugDestroyPools(&pools);
            ImSlug::ImSlugDestroyFont(font);
            TEST_CHECK(false);
            return;
        }

        // Cached: the same query returns the same stable pointer.
        {
            const ImSlug::ImSlugGlyphRecord* again = nullptr;
            TEST_CHECK(ImSlug::ImSlugGetGlyphRecord(font, &pools, 'A', &again) == ImSlug::ImSlugBuildStatus_Ok);
            TEST_CHECK(again == recordA);
        }

        // Outline quads are recoverable (fp16 values) and match the record's
        // curve count; the bbox is within the fp16 rounding of the record's.
        {
            ImSlug::ImSlugQuad quads[512];
            int quadCount = 0;
            TEST_CHECK(ImSlug::ImSlugGetGlyphOutlineQuads(font, glyphA, quads, 512, &quadCount));
            TEST_CHECK(quadCount == (int)recordA->CurveCount);
            TEST_CHECK(quadCount > 0);
            float minX = quads[0].X1, maxX = quads[0].X1, minY = quads[0].Y1, maxY = quads[0].Y1;
            for (int i = 0; i < quadCount; ++i)
            {
                minX = std::min(minX, std::min(quads[i].X1, std::min(quads[i].X2, quads[i].X3)));
                maxX = std::max(maxX, std::max(quads[i].X1, std::max(quads[i].X2, quads[i].X3)));
                minY = std::min(minY, std::min(quads[i].Y1, std::min(quads[i].Y2, quads[i].Y3)));
                maxY = std::max(maxY, std::max(quads[i].Y1, std::max(quads[i].Y2, quads[i].Y3)));
            }
            const float epsilon = 0.01f;  // fp16 quantization of ~1 em values
            TEST_CHECK(fabsf(minX - recordA->MinX) <= epsilon);
            TEST_CHECK(fabsf(maxX - recordA->MaxX) <= epsilon);
            TEST_CHECK(fabsf(minY - recordA->MinY) <= epsilon);
            TEST_CHECK(fabsf(maxY - recordA->MaxY) <= epsilon);
        }

        // CPU raster: 'A' has finite coverage with real ink inside its box.
        {
            const int width = (int)ceilf((recordA->MaxX - recordA->MinX) * 32.0f) + 4;
            const int height = (int)ceilf((recordA->MaxY - recordA->MinY) * 32.0f) + 4;
            TEST_CHECK(width > 4 && height > 4 && width < 256 && height < 256);
            float* coverage = (float*)std::malloc((size_t)width * height * sizeof(float));
            TEST_CHECK(coverage != nullptr);
            if (coverage != nullptr)
            {
                const float centerX0 = recordA->MinX + 0.5f / 32.0f;
                const float centerY0 = recordA->MaxY - 0.5f / 32.0f;
                ImSlug::ImSlugRasterGlyphRect(&pools, recordA, centerX0, centerY0, 32.0f, coverage, width, height);
                int ink = 0;
                int deep = 0;
                bool finite = true;
                for (int i = 0; i < width * height; ++i)
                {
                    if (!std::isfinite(coverage[i]))
                        finite = false;
                    if (coverage[i] > 0.5f)
                        ++ink;
                    if (coverage[i] > 0.9f)
                        ++deep;
                }
                TEST_CHECK(finite);
                TEST_CHECK(ink > 20);  // 'A' at 32 px is clearly inked
                TEST_CHECK(deep > 10);
                TEST_CHECK(ink < width * height / 2);
                std::free(coverage);
            }
        }

        // The synthetic (ellipsis) builder works on packed fonts too: it
        // recovers the dot outline and builds a record into the shared pools.
        {
            ImSlug::ImSlugQuad dotQuads[64];
            int dotCount = 0;
            TEST_CHECK(ImSlug::ImSlugGetGlyphOutlineQuads(font, glyphDot, dotQuads, 64, &dotCount));
            TEST_CHECK(dotCount > 0);
            ImSlug::ImSlugGlyphRecord record;
            const ImSlug::ImSlugBuildStatus status =
                ImSlug::ImSlugBuildSyntheticGlyphRecord(font, &pools, dotQuads, dotCount, 0.5f, &record);
            TEST_CHECK(status == ImSlug::ImSlugBuildStatus_Ok);
            TEST_CHECK(record.CurveCount == (uint32_t)dotCount);
            TEST_CHECK(record.Advance == 0.5f);
        }

        // Two packed fonts of the same asset share the pools without
        // interfering (each owns its records).
        {
            ImSlug::ImSlugFont* second = ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, asset.Size);
            TEST_CHECK(second != nullptr);
            if (second != nullptr)
            {
                const ImSlug::ImSlugGlyphRecord* record = nullptr;
                TEST_CHECK(ImSlug::ImSlugGetGlyphRecord(second, &pools, 'A', &record) == ImSlug::ImSlugBuildStatus_Ok);
                TEST_CHECK(record != recordA);
                TEST_CHECK(record->CurveCount == recordA->CurveCount);
                TEST_CHECK(record->MinX == recordA->MinX && record->MaxX == recordA->MaxX);
                ImSlug::ImSlugDestroyFont(second);
            }
        }

        ImSlug::ImSlugDestroyPools(&pools);
        ImSlug::ImSlugDestroyFont(font);
    }

    // ---------------------------------------------------------------------
    // C API additions
    // ---------------------------------------------------------------------
    void TestCApi()
    {
        AssetBytes asset;
        asset.Data = imslug_test::LoadFile(IMSLUG_PACKED_TEST_ASSET, &asset.Size);
        TEST_CHECK(asset.Data != nullptr);
        if (asset.Data == nullptr)
            return;

        TEST_CHECK(imslug_is_packed_font_data(asset.Data, asset.Size) == 1);
        TEST_CHECK(imslug_is_packed_font_data(nullptr, 0) == 0);

        char message[256];
        TEST_CHECK(imslug_validate_packed_font_data(asset.Data, asset.Size, message, (int)sizeof(message)) == 1);

        unsigned char* copy = (unsigned char*)std::malloc((size_t)asset.Size);
        TEST_CHECK(copy != nullptr);
        if (copy != nullptr)
        {
            std::memcpy(copy, asset.Data, (size_t)asset.Size);
            copy[8] = 9;  // unsupported version
            TEST_CHECK(imslug_validate_packed_font_data(copy, asset.Size, message, (int)sizeof(message)) == 0);
            TEST_CHECK(std::strstr(message, "version") != nullptr);
            // A null message buffer is allowed.
            TEST_CHECK(imslug_validate_packed_font_data(copy, asset.Size, nullptr, 0) == 0);
            std::free(copy);
        }
    }
}  // namespace

int main()
{
    TestSniffAndValidate();
    TestPackedFont();
    TestCApi();
    return imslug_test::Finish("imslug_packed_loader");
}
