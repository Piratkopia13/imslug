// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_gen_writer.cpp — `.imslug` container v1 serialization (M7). See
// imslug_gen_writer.h for the contract and imslug_packed.h for the layout.
//
// Pipeline (all runtime code; the writer only serializes):
//   1. sort + dedupe the requested codepoints, resolve glyph indices;
//   2. build each unique glyph's record into a private pool set in ascending
//      glyph-index order (deterministic layout; identical to what a runtime
//      font would build for the same glyph order);
//   3. read the per-glyph source metrics (glyf box + hmtx advance) so the
//      loader can reproduce metrics without a parser;
//   4. serialize header + glyph table + codepoint map + source name + the two
//      pool blobs in the canonical layout;
//   5. validate the result with the reader (a writer bug is caught here, not
//      at load time).

#include "imslug_gen_writer.h"

#include "imslug_c.h"  // IMSLUG_VERSION_NUM (no imgui.h dependency)

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        void Fail(char* message, int messageCapacity, const char* format, ...)
        {
            if (message == nullptr || messageCapacity <= 0)
                return;
            va_list args;
            va_start(args, format);
            vsnprintf(message, (size_t)messageCapacity, format, args);
            va_end(args);
        }

        int CompareU32(const void* a, const void* b)
        {
            const uint32_t left = *(const uint32_t*)a;
            const uint32_t right = *(const uint32_t*)b;
            return left < right ? -1 : (left > right ? 1 : 0);
        }

        int CompareInt(const void* a, const void* b)
        {
            const int left = *(const int*)a;
            const int right = *(const int*)b;
            return left < right ? -1 : (left > right ? 1 : 0);
        }

        int CompareCodepointEntry(const void* a, const void* b)
        {
            const uint32_t left = ((const ImSlugPackedCodepointEntry*)a)->Codepoint;
            const uint32_t right = ((const ImSlugPackedCodepointEntry*)b)->Codepoint;
            return left < right ? -1 : (left > right ? 1 : 0);
        }

        int CompareGlyphIndex(const void* a, const void* b)
        {
            const int left = ((const ImSlugPackedGlyphEntry*)a)->GlyphIndex;
            const int right = ((const ImSlugPackedGlyphEntry*)b)->GlyphIndex;
            return left < right ? -1 : (left > right ? 1 : 0);
        }

        // A codepoint that resolved to a glyph index, kept alongside the entry
        // list so the map can be emitted sorted by codepoint.
        struct CodepointRef
        {
            uint32_t Codepoint;
            int GlyphIndex;
        };

        int CompareCodepointRef(const void* a, const void* b)
        {
            const uint32_t left = ((const CodepointRef*)a)->Codepoint;
            const uint32_t right = ((const CodepointRef*)b)->Codepoint;
            return left < right ? -1 : (left > right ? 1 : 0);
        }
    }  // namespace

    uint32_t ImSlugFnv1a32(const void* data, int size)
    {
        const unsigned char* bytes = (const unsigned char*)data;
        uint32_t hash = 2166136261u;
        for (int i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        return hash;
    }

    bool ImSlugWritePackedAsset(ImSlugFont* font, const unsigned int* codepoints, int codepointCount,
                                const ImSlugAssetOptions& options, const char* sourceName, const void* sourceData,
                                int sourceDataSize, unsigned char** outData, int* outSize, ImSlugAssetStats* outStats,
                                char* errorMessage, int errorMessageCapacity)
    {
        if (outData != nullptr)
            *outData = nullptr;
        if (outSize != nullptr)
            *outSize = 0;
        if (outStats != nullptr)
            memset(outStats, 0, sizeof(*outStats));
        if (errorMessage != nullptr && errorMessageCapacity > 0)
            errorMessage[0] = '\0';

        if (font == nullptr || outData == nullptr || outSize == nullptr)
        {
            Fail(errorMessage, errorMessageCapacity, "writer: invalid arguments");
            return false;
        }
        if (codepointCount < 0 || (codepointCount > 0 && codepoints == nullptr))
        {
            Fail(errorMessage, errorMessageCapacity, "writer: invalid codepoint list");
            return false;
        }
        if (!(options.SizeMinPx > 0.0f) || !(options.SizeMaxPx >= options.SizeMinPx))
        {
            Fail(errorMessage, errorMessageCapacity, "writer: invalid size range %.2f:%.2f", options.SizeMinPx,
                 options.SizeMaxPx);
            return false;
        }
        if (sourceName == nullptr)
            sourceName = "";
        if (sourceData == nullptr)
            sourceDataSize = 0;

        // The writer builds into a transient pool set; the font's record cache
        // keys on the pool set address (M1 contract: pool sets are long-lived),
        // which a later call may reuse. Clear the cache on entry and on every
        // exit so no stale record survives the call.
        struct CacheGuard
        {
            ImSlugFont* Font;
            ~CacheGuard() { ImSlugFontClearCache(Font); }
        } cacheGuard = { font };

        // 1. Sort + dedupe codepoints; resolve glyph indices.
        uint32_t* sortedCodepoints = nullptr;
        if (codepointCount > 0)
        {
            sortedCodepoints = (uint32_t*)malloc((size_t)codepointCount * sizeof(uint32_t));
            if (sortedCodepoints == nullptr)
            {
                Fail(errorMessage, errorMessageCapacity, "writer: out of memory");
                return false;
            }
            for (int i = 0; i < codepointCount; ++i)
                sortedCodepoints[i] = codepoints[i];
            qsort(sortedCodepoints, (size_t)codepointCount, sizeof(uint32_t), CompareU32);
        }

        CodepointRef* resolved =
            (CodepointRef*)malloc((size_t)(codepointCount > 0 ? codepointCount : 1) * sizeof(CodepointRef));
        if (resolved == nullptr)
        {
            free(sortedCodepoints);
            Fail(errorMessage, errorMessageCapacity, "writer: out of memory");
            return false;
        }
        int resolvedCount = 0;
        int skipped = 0;
        for (int i = 0; i < codepointCount; ++i)
        {
            if (i > 0 && sortedCodepoints[i] == sortedCodepoints[i - 1])
                continue;  // duplicate codepoint
            const int glyphIndex = ImSlugGetGlyphIndex(font, sortedCodepoints[i]);
            if (glyphIndex == 0)
            {
                ++skipped;
                continue;
            }
            resolved[resolvedCount].Codepoint = sortedCodepoints[i];
            resolved[resolvedCount].GlyphIndex = glyphIndex;
            ++resolvedCount;
        }
        free(sortedCodepoints);

        // 2. Build each unique glyph once, ascending by glyph index.
        ImSlugPools pools;
        ImSlugInitPools(&pools);

        ImSlugPackedGlyphEntry* glyphs = (ImSlugPackedGlyphEntry*)malloc(
            (size_t)(resolvedCount > 0 ? resolvedCount : 1) * sizeof(ImSlugPackedGlyphEntry));
        if (glyphs == nullptr)
        {
            free(resolved);
            ImSlugDestroyPools(&pools);
            Fail(errorMessage, errorMessageCapacity, "writer: out of memory");
            return false;
        }
        int glyphCount = 0;
        int failed = 0;
        // Collect unique glyph indices, ascending (deterministic pool layout).
        int* glyphIndices = (int*)malloc((size_t)(resolvedCount > 0 ? resolvedCount : 1) * sizeof(int));
        if (glyphIndices == nullptr)
        {
            free(resolved);
            free(glyphs);
            ImSlugDestroyPools(&pools);
            Fail(errorMessage, errorMessageCapacity, "writer: out of memory");
            return false;
        }
        for (int i = 0; i < resolvedCount; ++i)
            glyphIndices[i] = resolved[i].GlyphIndex;
        qsort(glyphIndices, (size_t)resolvedCount, sizeof(int), CompareInt);
        int uniqueCount = 0;
        for (int i = 0; i < resolvedCount; ++i)
        {
            if (i == 0 || glyphIndices[i] != glyphIndices[i - 1])
                glyphIndices[uniqueCount++] = glyphIndices[i];
        }

        for (int i = 0; i < uniqueCount; ++i)
        {
            const ImSlugGlyphRecord* record = nullptr;
            const ImSlugBuildStatus status = ImSlugGetGlyphRecordByIndex(font, &pools, glyphIndices[i], &record);
            if (status != ImSlugBuildStatus_Ok || record == nullptr)
            {
                ++failed;  // runtime fallback would apply; the asset just omits it
                continue;
            }
            ImSlugGlyphSourceMetrics sourceMetrics;
            if (!ImSlugGetGlyphSourceMetrics(font, glyphIndices[i], &sourceMetrics))
            {
                ++failed;
                continue;
            }
            ImSlugPackedGlyphEntry& entry = glyphs[glyphCount++];
            memset(&entry, 0, sizeof(entry));
            entry.GlyphIndex = glyphIndices[i];
            entry.AdvanceFu = sourceMetrics.AdvanceFu;
            entry.BoxX0 = sourceMetrics.BoxX0;
            entry.BoxY0 = sourceMetrics.BoxY0;
            entry.BoxX1 = sourceMetrics.BoxX1;
            entry.BoxY1 = sourceMetrics.BoxY1;
            entry.Record = *record;
        }
        free(glyphIndices);
        qsort(glyphs, (size_t)glyphCount, sizeof(ImSlugPackedGlyphEntry), CompareGlyphIndex);

        // 3. Codepoint map (sorted; glyphs that failed the build are dropped).
        ImSlugPackedCodepointEntry* map = (ImSlugPackedCodepointEntry*)malloc(
            (size_t)(resolvedCount > 0 ? resolvedCount : 1) * sizeof(ImSlugPackedCodepointEntry));
        if (map == nullptr)
        {
            free(resolved);
            free(glyphs);
            ImSlugDestroyPools(&pools);
            Fail(errorMessage, errorMessageCapacity, "writer: out of memory");
            return false;
        }
        int mapCount = 0;
        for (int i = 0; i < resolvedCount; ++i)
        {
            // Find the entry index of this glyph (binary search over the
            // sorted table).
            int low = 0;
            int high = glyphCount;
            while (low < high)
            {
                const int mid = low + (high - low) / 2;
                if (glyphs[mid].GlyphIndex == resolved[i].GlyphIndex)
                {
                    map[mapCount].Codepoint = resolved[i].Codepoint;
                    map[mapCount].GlyphEntry = (uint32_t)mid;
                    ++mapCount;
                    break;
                }
                if (glyphs[mid].GlyphIndex < resolved[i].GlyphIndex)
                    low = mid + 1;
                else
                    high = mid;
            }
        }
        free(resolved);
        qsort(map, (size_t)mapCount, sizeof(ImSlugPackedCodepointEntry), CompareCodepointEntry);

        // 4. Serialize (canonical layout, zeroed alignment padding).
        const uint32_t curveRows = (uint32_t)pools.Curves.UsedRows;
        const uint32_t bandRows = (uint32_t)pools.Bands.UsedRows;
        const uint64_t glyphBytes = (uint64_t)glyphCount * sizeof(ImSlugPackedGlyphEntry);
        const uint64_t mapBytes = (uint64_t)mapCount * sizeof(ImSlugPackedCodepointEntry);
        const uint32_t nameLength = (uint32_t)strlen(sourceName);
        const uint64_t glyphTableOffset = kPackedAssetHeaderSize;
        const uint64_t codepointMapOffset = glyphTableOffset + glyphBytes;
        const uint64_t sourceNameOffset = codepointMapOffset + mapBytes;
        const uint64_t curveBlobOffset = ImSlugPackedAlign16((uint32_t)sourceNameOffset + nameLength + 1);
        const uint64_t curveBytes = (uint64_t)curveRows * kPoolWidth * 8;
        const uint64_t bandBlobOffset = ImSlugPackedAlign16((uint32_t)(curveBlobOffset + curveBytes));
        const uint64_t bandBytes = (uint64_t)bandRows * kPoolWidth * 4;
        const uint64_t totalSize64 = bandBlobOffset + bandBytes;
        if (totalSize64 > 0x7FFFFFFFull)
        {
            free(glyphs);
            free(map);
            ImSlugDestroyPools(&pools);
            Fail(errorMessage, errorMessageCapacity, "writer: asset too large (%llu bytes)",
                 (unsigned long long)totalSize64);
            return false;
        }
        const uint32_t totalSize = (uint32_t)totalSize64;

        unsigned char* buffer = (unsigned char*)malloc(totalSize);
        if (buffer == nullptr)
        {
            free(glyphs);
            free(map);
            ImSlugDestroyPools(&pools);
            Fail(errorMessage, errorMessageCapacity, "writer: out of memory (%u bytes)", totalSize);
            return false;
        }
        memset(buffer, 0, totalSize);

        ImSlugPackedHeader* header = (ImSlugPackedHeader*)buffer;
        memcpy(header->Magic, kPackedAssetMagic, sizeof(kPackedAssetMagic));
        header->Version = kPackedAssetVersion;
        header->HeaderSize = kPackedAssetHeaderSize;
        header->GeneratorVersion = IMSLUG_VERSION_NUM;
        header->Flags = 0;
        header->UnitsPerEm = (uint32_t)font->UnitsPerEm;
        {
            ImSlugFontMetrics fontMetrics;
            ImSlugGetFontMetrics(font, 1.0f, &fontMetrics);
            header->HheaAscent = fontMetrics.HheaAscent;
            header->HheaDescent = fontMetrics.HheaDescent;
        }
        header->SourceSize = (uint32_t)sourceDataSize;
        header->SourceChecksum = sourceDataSize > 0 ? ImSlugFnv1a32(sourceData, sourceDataSize) : 0;
        header->GlyphCount = (uint32_t)glyphCount;
        header->CodepointCount = (uint32_t)mapCount;
        header->CurveRows = curveRows;
        header->BandRows = bandRows;
        header->GlyphTableOffset = (uint32_t)glyphTableOffset;
        header->CodepointMapOffset = (uint32_t)codepointMapOffset;
        header->CurveBlobOffset = (uint32_t)curveBlobOffset;
        header->BandBlobOffset = (uint32_t)bandBlobOffset;
        header->SourceNameOffset = (uint32_t)sourceNameOffset;
        header->SourceNameLength = nameLength;
        memcpy(&header->SizeMinPxBits, &options.SizeMinPx, sizeof(float));
        memcpy(&header->SizeMaxPxBits, &options.SizeMaxPx, sizeof(float));

        if (glyphBytes > 0)
            memcpy(buffer + glyphTableOffset, glyphs, (size_t)glyphBytes);
        if (mapBytes > 0)
            memcpy(buffer + codepointMapOffset, map, (size_t)mapBytes);
        memcpy(buffer + sourceNameOffset, sourceName, nameLength + 1);
        if (curveRows > 0)
        {
            memcpy(buffer + curveBlobOffset, pools.Curves.Texels,
                   (size_t)curveRows * kPoolWidth * 4 * sizeof(uint16_t));
        }
        if (bandRows > 0)
        {
            memcpy(buffer + bandBlobOffset, pools.Bands.Texels, (size_t)bandRows * kPoolWidth * 2 * sizeof(uint16_t));
        }
        free(glyphs);
        free(map);
        ImSlugDestroyPools(&pools);

        // 5. Self-check with the reader.
        ImSlugPackedInfo info;
        char validationMessage[256];
        if (!ImSlugValidatePackedMemory(buffer, (int)totalSize, &info, validationMessage,
                                        (int)sizeof(validationMessage)))
        {
            free(buffer);
            Fail(errorMessage, errorMessageCapacity, "writer: generated asset failed validation: %s",
                 validationMessage);
            return false;
        }
        if (info.GlyphCount != (uint32_t)glyphCount || info.CodepointCount != (uint32_t)mapCount)
        {
            free(buffer);
            Fail(errorMessage, errorMessageCapacity, "writer: generated asset failed validation (count mismatch)");
            return false;
        }

        if (outStats != nullptr)
        {
            outStats->GlyphCount = glyphCount;
            outStats->CodepointCount = mapCount;
            outStats->SkippedCodepoints = skipped;
            outStats->FailedGlyphs = failed;
            outStats->CurveRows = (int)curveRows;
            outStats->BandRows = (int)bandRows;
            outStats->AssetSize = (int)totalSize;
        }
        *outData = buffer;
        *outSize = (int)totalSize;
        return true;
    }
}  // namespace ImSlug
