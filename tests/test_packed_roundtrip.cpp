// imslug — round-trip test: runtime TTF generation vs packed asset loading.
//
// The invariant is that the offline generator can never diverge from the
// runtime generator (same core code, byte-exact output). This suite proves it:
//
//   1. the writer (imslug_gen_core) bakes Roboto's ASCII range from the TTF
//      font into an in-memory asset and byte-compares it with the committed
//      test asset (freshness + determinism: two writer runs must be identical);
//   2. the runtime TTF path and the packed loader build the *same* glyphs into
//      two pool sets in the same (ascending glyph index) order — the records
//      must be bitwise equal, the pool rows byte-identical and the CPU raster
//      coverage bitwise equal;
//   3. metrics (the ImGui-parity pixel values) must be bitwise equal at every
//      tested size, and the packed font's source metrics must read back what
//      the writer stored;
//   4. a second font (Inter) and degenerate charsets (missing codepoints,
//      several codepoints sharing one glyph) behave as specified.
//
// Requires the TTF parser (IMSLUG_TTF_PARSER=ON); the parser-free loader checks
// live in test_packed_loader.cpp.

#include "imslug_gen_writer.h"
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
    struct FontFile
    {
        unsigned char* Data = nullptr;
        int Size = 0;

        ~FontFile() { std::free(Data); }
    };

    struct AssetBuffer
    {
        unsigned char* Data = nullptr;
        int Size = 0;

        ~AssetBuffer() { std::free(Data); }
    };

    // Unique glyph indices of `codepoints`, ascending (the writer's build
    // order; the runtime reference must use the same order for the pool
    // layouts to be comparable).
    void CollectGlyphIndices(const ImSlug::ImSlugFont* font, const uint32_t* codepoints, int codepointCount,
                             int* outIndices, int* outCount)
    {
        int count = 0;
        for (int i = 0; i < codepointCount; ++i)
        {
            const int glyphIndex = ImSlug::ImSlugGetGlyphIndex(font, codepoints[i]);
            if (glyphIndex == 0)
                continue;
            bool seen = false;
            for (int j = 0; j < count; ++j)
                seen = seen || outIndices[j] == glyphIndex;
            if (!seen)
                outIndices[count++] = glyphIndex;
        }
        std::sort(outIndices, outIndices + count);
        *outCount = count;
    }

    bool WriteAsset(const ImSlug::ImSlugFont* font, const uint32_t* codepoints, int codepointCount, const char* name,
                    const unsigned char* sourceData, int sourceDataSize, AssetBuffer* out, char* error,
                    int errorCapacity)
    {
        ImSlug::ImSlugAssetOptions options;
        options.SizeMinPx = 8.0f;
        options.SizeMaxPx = 72.0f;
        return ImSlug::ImSlugWritePackedAsset(const_cast<ImSlug::ImSlugFont*>(font), codepoints, codepointCount,
                                              options, name, sourceData, sourceDataSize, &out->Data, &out->Size,
                                              nullptr, error, errorCapacity);
    }

    // The core round-trip for one font: runtime records in `poolsRuntime`, the
    // packed loader's records in `poolsPacked`, compared bitwise.
    void RoundTripFont(const char* fontFileName, const uint32_t* codepoints, int codepointCount, bool compareFreshness,
                       const char* committedAssetPath)
    {
        FontFile fontFile;
        fontFile.Data = imslug_test::LoadFontFile(fontFileName, &fontFile.Size);
        TEST_CHECK(fontFile.Data != nullptr);
        if (fontFile.Data == nullptr)
            return;

        ImSlug::ImSlugFont* runtimeFont =
            ImSlug::ImSlugCreateFontFromMemory(fontFile.Data, fontFile.Size, 0, /*copyData=*/true);
        TEST_CHECK(runtimeFont != nullptr);
        if (runtimeFont == nullptr)
            return;

        char error[512];
        AssetBuffer asset;
        TEST_CHECKF(WriteAsset(runtimeFont, codepoints, codepointCount, fontFileName, fontFile.Data, fontFile.Size,
                               &asset, error, (int)sizeof(error)),
                    "writer failed: %s", error);
        if (asset.Data == nullptr)
        {
            ImSlug::ImSlugDestroyFont(runtimeFont);
            return;
        }

        // Determinism: a second writer run must be byte-identical.
        {
            AssetBuffer again;
            TEST_CHECKF(WriteAsset(runtimeFont, codepoints, codepointCount, fontFileName, fontFile.Data, fontFile.Size,
                                   &again, error, (int)sizeof(error)),
                        "writer failed (second run): %s", error);
            TEST_CHECK(again.Size == asset.Size);
            TEST_CHECK(again.Size == 0 || std::memcmp(again.Data, asset.Data, (size_t)asset.Size) == 0);
        }

        // Freshness: the committed asset must match the current writer output.
        if (compareFreshness)
        {
            AssetBuffer committed;
            committed.Data = imslug_test::LoadFile(committedAssetPath, &committed.Size);
            TEST_CHECKF(committed.Data != nullptr, "committed asset missing: %s", committedAssetPath);
            if (committed.Data != nullptr)
            {
                TEST_CHECKF(committed.Size == asset.Size &&
                                std::memcmp(committed.Data, asset.Data, (size_t)asset.Size) == 0,
                            "committed asset is stale; run scripts/gen_asset.*");
            }
        }

        // Build the runtime reference records in ascending glyph index order.
        int glyphIndices[1024];
        int glyphCount = 0;
        CollectGlyphIndices(runtimeFont, codepoints, codepointCount, glyphIndices, &glyphCount);
        TEST_CHECK(glyphCount > 0);

        ImSlug::ImSlugPools poolsRuntime;
        ImSlug::ImSlugInitPools(&poolsRuntime);
        const ImSlug::ImSlugGlyphRecord* recordsRuntime[1024];
        int builtRuntime = 0;
        for (int i = 0; i < glyphCount; ++i)
        {
            const ImSlug::ImSlugGlyphRecord* record = nullptr;
            const ImSlug::ImSlugBuildStatus status =
                ImSlug::ImSlugGetGlyphRecordByIndex(runtimeFont, &poolsRuntime, glyphIndices[i], &record);
            TEST_CHECKF(status == ImSlug::ImSlugBuildStatus_Ok, "%s glyph %d: status %d", fontFileName, glyphIndices[i],
                        (int)status);
            if (status == ImSlug::ImSlugBuildStatus_Ok)
                recordsRuntime[builtRuntime++] = record;
        }

        // Load the packed font and build the same glyphs into a fresh pool set.
        ImSlug::ImSlugFont* packedFont = ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, asset.Size);
        TEST_CHECK(packedFont != nullptr);
        if (packedFont == nullptr)
        {
            ImSlug::ImSlugDestroyPools(&poolsRuntime);
            ImSlug::ImSlugDestroyFont(runtimeFont);
            return;
        }
        ImSlug::ImSlugPools poolsPacked;
        ImSlug::ImSlugInitPools(&poolsPacked);
        const ImSlug::ImSlugGlyphRecord* recordsPacked[1024];
        int builtPacked = 0;
        for (int i = 0; i < glyphCount; ++i)
        {
            const ImSlug::ImSlugGlyphRecord* record = nullptr;
            const ImSlug::ImSlugBuildStatus status =
                ImSlug::ImSlugGetGlyphRecordByIndex(packedFont, &poolsPacked, glyphIndices[i], &record);
            TEST_CHECKF(status == ImSlug::ImSlugBuildStatus_Ok, "packed %s glyph %d: status %d", fontFileName,
                        glyphIndices[i], (int)status);
            if (status == ImSlug::ImSlugBuildStatus_Ok)
                recordsPacked[builtPacked++] = record;
        }

        TEST_CHECK(builtRuntime == glyphCount && builtPacked == glyphCount);

        // Records bitwise equal.
        for (int i = 0; i < builtRuntime && i < builtPacked; ++i)
        {
            TEST_CHECKF(std::memcmp(recordsRuntime[i], recordsPacked[i], sizeof(ImSlug::ImSlugGlyphRecord)) == 0,
                        "%s glyph %d: record mismatch", fontFileName, glyphIndices[i]);
        }

        // Pool rows byte-identical (same build order => identical layout).
        TEST_CHECK(poolsRuntime.Curves.UsedRows == poolsPacked.Curves.UsedRows);
        TEST_CHECK(poolsRuntime.Bands.UsedRows == poolsPacked.Bands.UsedRows);
        if (poolsRuntime.Curves.UsedRows == poolsPacked.Curves.UsedRows && poolsRuntime.Curves.UsedRows > 0)
        {
            const size_t bytes = (size_t)poolsRuntime.Curves.UsedRows * ImSlug::kPoolWidth * 4 * sizeof(uint16_t);
            TEST_CHECK(std::memcmp(poolsRuntime.Curves.Texels, poolsPacked.Curves.Texels, bytes) == 0);
        }
        if (poolsRuntime.Bands.UsedRows == poolsPacked.Bands.UsedRows && poolsRuntime.Bands.UsedRows > 0)
        {
            const size_t bytes = (size_t)poolsRuntime.Bands.UsedRows * ImSlug::kPoolWidth * 2 * sizeof(uint16_t);
            TEST_CHECK(std::memcmp(poolsRuntime.Bands.Texels, poolsPacked.Bands.Texels, bytes) == 0);
        }

        // Metrics bitwise equal at every tested size (ImGui parity contract).
        static const float kSizes[] = { 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 72.0f };
        for (float size : kSizes)
        {
            ImSlug::ImSlugFontMetrics runtimeMetrics, packedMetrics;
            ImSlug::ImSlugGetFontMetrics(runtimeFont, size, &runtimeMetrics);
            ImSlug::ImSlugGetFontMetrics(packedFont, size, &packedMetrics);
            TEST_CHECK(std::memcmp(&runtimeMetrics, &packedMetrics, sizeof(runtimeMetrics)) == 0);
            for (int i = 0; i < builtRuntime && i < builtPacked; ++i)
            {
                ImSlug::ImSlugGlyphMetrics a, b;
                TEST_CHECK(ImSlug::ImSlugGetGlyphMetricsByIndex(runtimeFont, glyphIndices[i], size, &a));
                TEST_CHECK(ImSlug::ImSlugGetGlyphMetricsByIndex(packedFont, glyphIndices[i], size, &b));
                TEST_CHECKF(std::memcmp(&a, &b, sizeof(a)) == 0, "%s glyph %d @ %.0f px: metrics mismatch",
                            fontFileName, glyphIndices[i], size);
            }
        }

        // Source metrics read back exactly what the writer stored (the runtime
        // query is the definition the writer serialized).
        for (int i = 0; i < builtRuntime && i < builtPacked; ++i)
        {
            ImSlug::ImSlugGlyphSourceMetrics runtimeSource, packedSource;
            TEST_CHECK(ImSlug::ImSlugGetGlyphSourceMetrics(runtimeFont, glyphIndices[i], &runtimeSource));
            TEST_CHECK(ImSlug::ImSlugGetGlyphSourceMetrics(packedFont, glyphIndices[i], &packedSource));
            TEST_CHECKF(std::memcmp(&runtimeSource, &packedSource, sizeof(runtimeSource)) == 0,
                        "%s glyph %d: source metrics mismatch", fontFileName, glyphIndices[i]);
        }

        // CPU raster coverage bitwise equal for a sample of glyphs (identical
        // pools + records must produce identical float math).
        const int sampleGlyphs[] = { 'A', 'g', 'Q', '%', '@', '|', '.' };
        for (int sample : sampleGlyphs)
        {
            const int glyphIndex = ImSlug::ImSlugGetGlyphIndex(packedFont, (unsigned int)sample);
            if (glyphIndex == 0)
                continue;  // not part of this charset/asset
            const ImSlug::ImSlugGlyphRecord* a = nullptr;
            const ImSlug::ImSlugGlyphRecord* b = nullptr;
            if (ImSlug::ImSlugGetGlyphRecordByIndex(runtimeFont, &poolsRuntime, glyphIndex, &a) !=
                    ImSlug::ImSlugBuildStatus_Ok ||
                ImSlug::ImSlugGetGlyphRecordByIndex(packedFont, &poolsPacked, glyphIndex, &b) !=
                    ImSlug::ImSlugBuildStatus_Ok)
            {
                TEST_CHECK(false);
                continue;
            }
            const int width = (int)ceilf((a->MaxX - a->MinX) * 32.0f) + 4;
            const int height = (int)ceilf((a->MaxY - a->MinY) * 32.0f) + 4;
            if (width <= 0 || height <= 0 || width > 256 || height > 256)
            {
                TEST_CHECK(false);
                continue;
            }
            float* coverageA = (float*)std::malloc((size_t)width * height * sizeof(float));
            float* coverageB = (float*)std::malloc((size_t)width * height * sizeof(float));
            TEST_CHECK(coverageA != nullptr && coverageB != nullptr);
            if (coverageA != nullptr && coverageB != nullptr)
            {
                const float centerX0 = a->MinX + 0.5f / 32.0f;
                const float centerY0 = a->MaxY - 0.5f / 32.0f;
                ImSlug::ImSlugRasterGlyphRect(&poolsRuntime, a, centerX0, centerY0, 32.0f, coverageA, width, height);
                ImSlug::ImSlugRasterGlyphRect(&poolsPacked, b, centerX0, centerY0, 32.0f, coverageB, width, height);
                TEST_CHECKF(std::memcmp(coverageA, coverageB, (size_t)width * height * sizeof(float)) == 0,
                            "%s glyph '%c': coverage mismatch", fontFileName, sample);
            }
            std::free(coverageA);
            std::free(coverageB);
        }

        // Codepoint-keyed lookup returns the same record as the index-keyed
        // one (the adapter's path).
        {
            const unsigned int codepoint = 'A';
            const ImSlug::ImSlugGlyphRecord* byCodepoint = nullptr;
            const ImSlug::ImSlugGlyphRecord* byIndex = nullptr;
            TEST_CHECK(ImSlug::ImSlugGetGlyphRecord(packedFont, &poolsPacked, codepoint, &byCodepoint) ==
                       ImSlug::ImSlugBuildStatus_Ok);
            TEST_CHECK(ImSlug::ImSlugGetGlyphRecordByIndex(packedFont, &poolsPacked,
                                                           ImSlug::ImSlugGetGlyphIndex(packedFont, codepoint),
                                                           &byIndex) == ImSlug::ImSlugBuildStatus_Ok);
            TEST_CHECK(byCodepoint == byIndex);
        }

        ImSlug::ImSlugDestroyPools(&poolsPacked);
        ImSlug::ImSlugDestroyPools(&poolsRuntime);
        ImSlug::ImSlugDestroyFont(packedFont);
        ImSlug::ImSlugDestroyFont(runtimeFont);
    }

    // Degenerate charsets: missing codepoints are skipped; several codepoints
    // mapping to one glyph produce one glyph entry and several map entries.
    void TestCharsetEdgeCases()
    {
        FontFile fontFile;
        fontFile.Data = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontFile.Size);
        TEST_CHECK(fontFile.Data != nullptr);
        if (fontFile.Data == nullptr)
            return;
        ImSlug::ImSlugFont* font =
            ImSlug::ImSlugCreateFontFromMemory(fontFile.Data, fontFile.Size, 0, /*copyData=*/true);
        TEST_CHECK(font != nullptr);
        if (font == nullptr)
            return;

        char error[512];
        // 'A' + a CJK codepoint that Roboto lacks.
        {
            const uint32_t codepoints[] = { 'A', 0x4E00 };
            AssetBuffer asset;
            ImSlug::ImSlugAssetOptions options;
            ImSlug::ImSlugAssetStats stats;
            TEST_CHECK(ImSlug::ImSlugWritePackedAsset(font, codepoints, 2, options, "edge", fontFile.Data,
                                                      fontFile.Size, &asset.Data, &asset.Size, &stats, error,
                                                      (int)sizeof(error)));
            TEST_CHECK(stats.GlyphCount == 1);
            TEST_CHECK(stats.CodepointCount == 1);
            TEST_CHECK(stats.SkippedCodepoints == 1);
            ImSlug::ImSlugFont* packed = ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, asset.Size);
            TEST_CHECK(packed != nullptr);
            if (packed != nullptr)
            {
                TEST_CHECK(ImSlug::ImSlugGetGlyphIndex(packed, 'A') > 0);
                TEST_CHECK(ImSlug::ImSlugGetGlyphIndex(packed, 0x4E00) == 0);
                ImSlug::ImSlugDestroyFont(packed);
            }
        }

        // Two codepoints sharing one glyph (Roboto maps U+00A0 to the space
        // glyph): one glyph entry, two map entries, one stable record.
        {
            const int spaceGlyph = ImSlug::ImSlugGetGlyphIndex(font, ' ');
            const int nbspGlyph = ImSlug::ImSlugGetGlyphIndex(font, 0xA0);
            if (spaceGlyph > 0 && spaceGlyph == nbspGlyph)
            {
                const uint32_t codepoints[] = { ' ', 0xA0 };
                AssetBuffer asset;
                ImSlug::ImSlugAssetOptions options;
                ImSlug::ImSlugAssetStats stats;
                TEST_CHECK(ImSlug::ImSlugWritePackedAsset(font, codepoints, 2, options, "edge", fontFile.Data,
                                                          fontFile.Size, &asset.Data, &asset.Size, &stats, error,
                                                          (int)sizeof(error)));
                TEST_CHECK(stats.GlyphCount == 1);
                TEST_CHECK(stats.CodepointCount == 2);
                ImSlug::ImSlugFont* packed = ImSlug::ImSlugCreateFontFromPackedMemory(asset.Data, asset.Size);
                TEST_CHECK(packed != nullptr);
                if (packed != nullptr)
                {
                    TEST_CHECK(ImSlug::ImSlugGetGlyphIndex(packed, ' ') == spaceGlyph);
                    TEST_CHECK(ImSlug::ImSlugGetGlyphIndex(packed, 0xA0) == spaceGlyph);
                    ImSlug::ImSlugPools pools;
                    ImSlug::ImSlugInitPools(&pools);
                    const ImSlug::ImSlugGlyphRecord* a = nullptr;
                    const ImSlug::ImSlugGlyphRecord* b = nullptr;
                    TEST_CHECK(ImSlug::ImSlugGetGlyphRecord(packed, &pools, ' ', &a) == ImSlug::ImSlugBuildStatus_Ok);
                    TEST_CHECK(ImSlug::ImSlugGetGlyphRecord(packed, &pools, 0xA0, &b) == ImSlug::ImSlugBuildStatus_Ok);
                    TEST_CHECK(a == b);
                    ImSlug::ImSlugDestroyPools(&pools);
                    ImSlug::ImSlugDestroyFont(packed);
                }
            }
            else
            {
                std::printf("  note: Roboto does not share a glyph for ' ' and U+00A0; case skipped\n");
            }
        }

        ImSlug::ImSlugDestroyFont(font);
    }
}  // namespace

int main()
{
    // The committed asset's exact generation parameters: Roboto, ASCII
    // printable, size range 8:72, source name "Roboto-Regular.ttf".
    uint32_t ascii[0x7E - 0x20 + 1];
    int asciiCount = 0;
    for (uint32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
        ascii[asciiCount++] = codepoint;
    RoundTripFont("Roboto-Regular.ttf", ascii, asciiCount, /*compareFreshness=*/true, IMSLUG_PACKED_TEST_ASSET);

    // Second font, mixed charset.
    uint32_t inter[26 + 26 + 10];
    int interCount = 0;
    for (uint32_t c = 'A'; c <= 'Z'; ++c)
        inter[interCount++] = c;
    for (uint32_t c = 'a'; c <= 'z'; ++c)
        inter[interCount++] = c;
    for (uint32_t c = '0'; c <= '9'; ++c)
        inter[interCount++] = c;
    RoundTripFont("Inter-Regular.ttf", inter, interCount, /*compareFreshness=*/false, nullptr);

    TestCharsetEdgeCases();
    return imslug_test::Finish("imslug_packed_roundtrip");
}
