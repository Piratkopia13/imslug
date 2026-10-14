// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packed.cpp — the `.imslug` container v1 reader: full structural
// validation, the packed font kind (records + metrics without an outline
// parser) and the per-glyph copy of the baked pool data into the shared pools.
//
// Format definition + invariants: imslug_packed.h (machine-checked) and
// docs/PACKED_FORMAT.md (durable spec). Highlights:
//   * every offset/range is bounds-checked before use; a malformed asset is
//     rejected whole (never partially accepted, never a crash);
//   * glyph records are copied into the target pools lazily, exactly once per
//     (glyph index, target pools), through the shared cache in imslug_font.cpp;
//   * band references are patched by the curve block's linear delta, so the
//     copy preserves the runtime pool layout byte for byte when the loader is
//     fed the same glyph order as the generator (the round-trip test);
//   * metrics are reproduced from the stored font-level hhea/upem and the
//     per-glyph glyf box with the exact TTF formulas (see imslug_font.h's
//     ImSlugImCeil/Floor/Round), so layout is identical to a runtime font.

#include "imslug_packed.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ImSlug
{
    namespace
    {
        // ---------------------------------------------------------------------
        // Diagnostics
        // ---------------------------------------------------------------------
        void Fail(char* message, int messageCapacity, const char* text)
        {
            if (message == nullptr || messageCapacity <= 0)
                return;
            int n = 0;
            while (text[n] != '\0' && n < messageCapacity - 1)
            {
                message[n] = text[n];
                ++n;
            }
            message[n] = '\0';
        }

        void FailF(char* message, int messageCapacity, const char* format, ...)
        {
            if (message == nullptr || messageCapacity <= 0)
                return;
            va_list args;
            va_start(args, format);
            vsnprintf(message, (size_t)messageCapacity, format, args);
            va_end(args);
        }

        bool HostIsLittleEndian()
        {
            const uint16_t one = 1;
            return *(const unsigned char*)&one == 1;
        }

        // ---------------------------------------------------------------------
        // Validated view
        // ---------------------------------------------------------------------
        // Everything below only runs on a validated asset, so these pointers
        // are always in range. The canonical layout check (see ValidateAsset)
        // guarantees the regions are ordered and sized exactly as the writer
        // emitted them.
        struct Asset
        {
            const uint8_t* Data;
            uint32_t Size;
            const ImSlugPackedHeader* Header;
            const ImSlugPackedGlyphEntry* Glyphs;
            const ImSlugPackedCodepointEntry* Codepoints;
            const uint16_t* CurveTexels;  // CurveRows * kPoolWidth texels * 4 halves
            const uint16_t* BandTexels;   // BandRows * kPoolWidth texels * 2 halves
        };

        uint64_t CurveTexelCount(const ImSlugPackedHeader* header)
        {
            return (uint64_t)header->CurveRows * kPoolWidth;
        }

        uint64_t BandTexelCount(const ImSlugPackedHeader* header)
        {
            return (uint64_t)header->BandRows * kPoolWidth;
        }

        const uint16_t* CurveTexelAt(const Asset& asset, int x, int y)
        {
            return asset.CurveTexels + ((size_t)y * kPoolWidth + (size_t)x) * 4;
        }

        const uint16_t* BandTexelAt(const Asset& asset, int x, int y)
        {
            return asset.BandTexels + ((size_t)y * kPoolWidth + (size_t)x) * 2;
        }

        // ---------------------------------------------------------------------
        // Structural validation
        // ---------------------------------------------------------------------
        // One glyph record: pool ranges, band header/list row rules (the pixel
        // shader reads header runs and reference lists contiguously in x and
        // never row-wraps them) and the curve references (which must address
        // the glyph's own curve block, first texel of a curve).
        bool ValidateRecord(const Asset& asset, const ImSlugGlyphRecord& record, char* message, int messageCapacity)
        {
            const uint64_t curveTexels = CurveTexelCount(asset.Header);
            const uint64_t bandTexels = BandTexelCount(asset.Header);

            // Floats the shader/layout consume: finite and inside the builder's
            // documented envelope (kMaxCoord = 64 em; band scales are derived
            // from the bbox and can be large for thin glyphs).
            const float bounded[8] = { record.MinX,    record.MinY,       record.MaxX,       record.MaxY,
                                       record.Advance, record.BandScaleX, record.BandScaleY, 0.0f };
            for (int i = 0; i < 7; ++i)
            {
                if (!isfinite(bounded[i]) || fabsf(bounded[i]) > (i < 5 ? 64.0f : 1.0e9f))
                {
                    Fail(message, messageCapacity, "glyph record has non-finite/out-of-range floats");
                    return false;
                }
            }
            if (!isfinite(record.BandOffsetX) || fabsf(record.BandOffsetX) > 1.0e9f || !isfinite(record.BandOffsetY) ||
                fabsf(record.BandOffsetY) > 1.0e9f)
            {
                Fail(message, messageCapacity, "glyph record has non-finite band offsets");
                return false;
            }
            if (record.BandMaxX < 0 || record.BandMaxX >= kMaxBandCountPerAxis || record.BandMaxY < 0 ||
                record.BandMaxY >= kMaxBandCountPerAxis)
            {
                Fail(message, messageCapacity, "glyph record band counts out of range");
                return false;
            }
            if (record.CurveLocX < 0 || record.CurveLocX >= kPoolWidth || (record.CurveLocX & 1) != 0)
            {
                Fail(message, messageCapacity, "glyph record curve location invalid");
                return false;
            }

            const uint64_t curveLinear = (uint64_t)(uint32_t)record.CurveLocY * kPoolWidth + record.CurveLocX;
            const uint64_t curveCount = record.CurveCount;
            if (curveCount > 0)
            {
                if (record.CurveLocY < 0 || (uint64_t)record.CurveLocY >= (uint64_t)asset.Header->CurveRows ||
                    curveLinear + 2 * curveCount > curveTexels)
                {
                    Fail(message, messageCapacity, "glyph record curve block out of range");
                    return false;
                }
                // Every stored curve coordinate must be finite and inside the
                // builder's envelope (kMaxCoord = 64 em): a corrupt asset must
                // never feed NaN/inf geometry to the shader or the CPU arbiter.
                for (uint64_t c = 0; c < curveCount; ++c)
                {
                    const uint64_t linear = curveLinear + 2 * c;
                    const uint16_t* first =
                        CurveTexelAt(asset, (int)(linear & (kPoolWidth - 1)), (int)(linear >> kPoolWidthLog2));
                    const uint16_t* second = CurveTexelAt(asset, (int)((linear + 1) & (kPoolWidth - 1)),
                                                          (int)((linear + 1) >> kPoolWidthLog2));
                    const float values[6] = { ImSlugHalfToFloat(first[0]),  ImSlugHalfToFloat(first[1]),
                                              ImSlugHalfToFloat(first[2]),  ImSlugHalfToFloat(first[3]),
                                              ImSlugHalfToFloat(second[0]), ImSlugHalfToFloat(second[1]) };
                    for (int v = 0; v < 6; ++v)
                    {
                        if (!isfinite(values[v]) || fabsf(values[v]) > 64.0f)
                        {
                            Fail(message, messageCapacity, "glyph curve coordinate out of range");
                            return false;
                        }
                    }
                }
            }

            if (record.BandLocX < 0 || record.BandLocX >= kPoolWidth || record.BandLocY < 0 ||
                (uint64_t)record.BandLocY >= (uint64_t)asset.Header->BandRows)
            {
                Fail(message, messageCapacity, "glyph record band location invalid");
                return false;
            }

            const int headerCount = record.BandMaxY + 1 + record.BandMaxX + 1;
            if (record.BandLocX + headerCount > kPoolWidth)
            {
                Fail(message, messageCapacity, "glyph record band headers straddle a pool row");
                return false;
            }
            const uint64_t bandLinear = (uint64_t)(uint32_t)record.BandLocY * kPoolWidth + record.BandLocX;
            if (bandLinear + headerCount > bandTexels)
            {
                Fail(message, messageCapacity, "glyph record band block out of range");
                return false;
            }

            for (int band = 0; band < headerCount; ++band)
            {
                const uint16_t* header = BandTexelAt(asset, record.BandLocX + band, record.BandLocY);
                const uint32_t count = header[0];
                const uint32_t listOffset = header[1];
                if (listOffset < (uint32_t)headerCount)
                {
                    Fail(message, messageCapacity, "band reference list overlaps the header run");
                    return false;
                }
                const uint32_t listX = (uint32_t)(record.BandLocX + (int)listOffset) & (kPoolWidth - 1);
                if (listX + count > (uint32_t)kPoolWidth)
                {
                    Fail(message, messageCapacity, "band reference list straddles a pool row");
                    return false;
                }
                const uint64_t listLinear = bandLinear + listOffset;
                if (listLinear + count > bandTexels)
                {
                    Fail(message, messageCapacity, "band reference list out of range");
                    return false;
                }
                for (uint32_t r = 0; r < count; ++r)
                {
                    int refX = 0, refY = 0;
                    ImSlugCalcBandLoc(record.BandLocX, record.BandLocY, listOffset + r, &refX, &refY);
                    if (refY < 0 || (uint64_t)refY >= (uint64_t)asset.Header->BandRows)
                    {
                        Fail(message, messageCapacity, "band reference list entry out of range");
                        return false;
                    }
                    const uint16_t* ref = BandTexelAt(asset, refX, refY);
                    const uint64_t refLinear = (uint64_t)(uint32_t)ref[1] * kPoolWidth + ref[0];
                    if ((ref[0] & 1) != 0 || refLinear < curveLinear || refLinear >= curveLinear + 2 * curveCount ||
                        (refLinear - curveLinear) % 2 != 0 || refLinear + 1 >= curveTexels)
                    {
                        Fail(message, messageCapacity, "band reference does not address the glyph's curve block");
                        return false;
                    }
                }
            }
            return true;
        }

        bool ValidateAsset(const void* data, int size, ImSlugPackedInfo* outInfo, char* message, int messageCapacity)
        {
            if (message != nullptr && messageCapacity > 0)
                message[0] = '\0';
            if (!HostIsLittleEndian())
            {
                Fail(message, messageCapacity, "big-endian hosts are not supported (the format is little-endian)");
                return false;
            }
            if (data == nullptr || size < (int)kPackedAssetHeaderSize)
            {
                Fail(message, messageCapacity, "file too small for a .imslug v1 header");
                return false;
            }

            const uint8_t* bytes = (const uint8_t*)data;
            const ImSlugPackedHeader* header = (const ImSlugPackedHeader*)bytes;
            if (memcmp(header->Magic, kPackedAssetMagic, sizeof(kPackedAssetMagic)) != 0)
            {
                Fail(message, messageCapacity, "not an .imslug container (bad magic)");
                return false;
            }
            if (header->Version != kPackedAssetVersion)
            {
                FailF(message, messageCapacity, "unsupported .imslug version %u (this build reads version %u)",
                      header->Version, kPackedAssetVersion);
                return false;
            }
            if (header->HeaderSize != kPackedAssetHeaderSize)
            {
                FailF(message, messageCapacity, "unexpected header size %u (expected %u)", header->HeaderSize,
                      kPackedAssetHeaderSize);
                return false;
            }
            if (header->Flags != 0)
            {
                Fail(message, messageCapacity, "reserved header flags are nonzero");
                return false;
            }
            if (header->UnitsPerEm < 1 || header->UnitsPerEm > 16384)
            {
                Fail(message, messageCapacity, "unitsPerEm out of range");
                return false;
            }
            if (header->HheaAscent <= header->HheaDescent)
            {
                Fail(message, messageCapacity, "hhea ascent/descent unusable (ascent must exceed descent)");
                return false;
            }
            if (header->SourceSize == 0)
            {
                Fail(message, messageCapacity, "source font size is zero");
                return false;
            }
            if (header->CurveRows > (uint32_t)kPoolMaxHeight || header->BandRows > (uint32_t)kPoolMaxHeight)
            {
                Fail(message, messageCapacity, "pool row counts exceed the runtime pool height");
                return false;
            }

            // Canonical layout: the writer emits header, glyph table, codepoint
            // map, source name, curve blob, band blob, each region starting
            // where the previous one ends (blobs 16-byte aligned). Matching the
            // exact total size also rejects trailing garbage.
            const uint64_t glyphBytes = (uint64_t)header->GlyphCount * sizeof(ImSlugPackedGlyphEntry);
            const uint64_t mapBytes = (uint64_t)header->CodepointCount * sizeof(ImSlugPackedCodepointEntry);
            const uint64_t nameBytes = (uint64_t)header->SourceNameLength + 1;
            const uint64_t curveBytes = (uint64_t)header->CurveRows * kPoolWidth * 4 * sizeof(uint16_t);
            const uint64_t bandBytes = (uint64_t)header->BandRows * kPoolWidth * 2 * sizeof(uint16_t);

            if (header->GlyphTableOffset != kPackedAssetHeaderSize ||
                header->CodepointMapOffset != header->GlyphTableOffset + glyphBytes ||
                header->SourceNameOffset != header->CodepointMapOffset + mapBytes ||
                header->CurveBlobOffset != ImSlugPackedAlign16(header->SourceNameOffset + (uint32_t)nameBytes) ||
                header->BandBlobOffset != ImSlugPackedAlign16(header->CurveBlobOffset + (uint32_t)curveBytes) ||
                (uint64_t)size != (uint64_t)header->BandBlobOffset + bandBytes)
            {
                Fail(message, messageCapacity, "region layout does not match the canonical .imslug v1 layout");
                return false;
            }

            Asset asset;
            asset.Data = bytes;
            asset.Size = (uint32_t)size;
            asset.Header = header;
            asset.Glyphs = (const ImSlugPackedGlyphEntry*)(bytes + header->GlyphTableOffset);
            asset.Codepoints = (const ImSlugPackedCodepointEntry*)(bytes + header->CodepointMapOffset);
            asset.CurveTexels = (const uint16_t*)(bytes + header->CurveBlobOffset);
            asset.BandTexels = (const uint16_t*)(bytes + header->BandBlobOffset);

            // Source name: UTF-8, NUL-terminated, no embedded NUL before the end.
            const char* name = (const char*)bytes + header->SourceNameOffset;
            if (name[header->SourceNameLength] != '\0')
            {
                Fail(message, messageCapacity, "source name is not NUL-terminated");
                return false;
            }
            if (memchr(name, '\0', header->SourceNameLength) != nullptr)
            {
                Fail(message, messageCapacity, "source name contains an embedded NUL");
                return false;
            }

            // Glyph table: strictly ascending by glyph index, all > 0.
            int previousGlyph = 0;
            for (uint32_t i = 0; i < header->GlyphCount; ++i)
            {
                const ImSlugPackedGlyphEntry& entry = asset.Glyphs[i];
                if (entry.GlyphIndex <= 0 || entry.GlyphIndex <= previousGlyph)
                {
                    Fail(message, messageCapacity, "glyph table is not strictly ascending (or has index <= 0)");
                    return false;
                }
                previousGlyph = entry.GlyphIndex;

                // Box sanity: within the builder's 64 em envelope (and thus
                // immune to int overflow in the metrics formulas).
                const int64_t boxLimit = (int64_t)64 * (int64_t)header->UnitsPerEm;
                if (llabs((int64_t)entry.BoxX0) > boxLimit || llabs((int64_t)entry.BoxY0) > boxLimit ||
                    llabs((int64_t)entry.BoxX1) > boxLimit || llabs((int64_t)entry.BoxY1) > boxLimit)
                {
                    Fail(message, messageCapacity, "glyph box out of range");
                    return false;
                }
                char detail[192];
                if (!ValidateRecord(asset, entry.Record, detail, (int)sizeof(detail)))
                {
                    FailF(message, messageCapacity, "glyph %d: %s", entry.GlyphIndex, detail);
                    return false;
                }
            }

            // Codepoint map: strictly ascending, entries address glyph rows.
            uint32_t previousCodepoint = 0;
            for (uint32_t i = 0; i < header->CodepointCount; ++i)
            {
                const ImSlugPackedCodepointEntry& entry = asset.Codepoints[i];
                if (i > 0 && entry.Codepoint <= previousCodepoint)
                {
                    Fail(message, messageCapacity, "codepoint map is not strictly ascending");
                    return false;
                }
                previousCodepoint = entry.Codepoint;
                if (entry.GlyphEntry >= header->GlyphCount)
                {
                    Fail(message, messageCapacity, "codepoint map entry out of range");
                    return false;
                }
            }

            if (outInfo != nullptr)
            {
                memset(outInfo, 0, sizeof(*outInfo));
                outInfo->Version = header->Version;
                outInfo->GeneratorVersion = header->GeneratorVersion;
                outInfo->Flags = header->Flags;
                outInfo->UnitsPerEm = header->UnitsPerEm;
                outInfo->HheaAscent = header->HheaAscent;
                outInfo->HheaDescent = header->HheaDescent;
                outInfo->SourceSize = header->SourceSize;
                outInfo->SourceChecksum = header->SourceChecksum;
                outInfo->GlyphCount = header->GlyphCount;
                outInfo->CodepointCount = header->CodepointCount;
                outInfo->CurveRows = header->CurveRows;
                outInfo->BandRows = header->BandRows;
                memcpy(&outInfo->SizeMinPx, &header->SizeMinPxBits, sizeof(float));
                memcpy(&outInfo->SizeMaxPx, &header->SizeMaxPxBits, sizeof(float));
                outInfo->SourceName = name;
                outInfo->AssetSize = (uint32_t)size;
            }
            return true;
        }

        // ---------------------------------------------------------------------
        // Packed font
        // ---------------------------------------------------------------------
        struct ImSlugPackedFont
        {
            ImSlugFont Base;           // must be the first member (kind cast)
            unsigned char* OwnedData;  // copy of the validated container
            int DataSize;
            Asset AssetView;
        };

        static ImSlugPackedFont* PackedFont(ImSlugFont* font)
        {
            return (ImSlugPackedFont*)font;
        }

        static const ImSlugPackedFont* PackedFont(const ImSlugFont* font)
        {
            return (const ImSlugPackedFont*)font;
        }

        // Binary search over the sorted glyph table; null when absent.
        const ImSlugPackedGlyphEntry* FindGlyphEntry(const ImSlugPackedFont* font, int glyphIndex)
        {
            const ImSlugPackedHeader* header = font->AssetView.Header;
            uint32_t low = 0;
            uint32_t high = header->GlyphCount;
            while (low < high)
            {
                const uint32_t mid = low + (high - low) / 2;
                const int value = font->AssetView.Glyphs[mid].GlyphIndex;
                if (value == glyphIndex)
                    return &font->AssetView.Glyphs[mid];
                if (value < glyphIndex)
                    low = mid + 1;
                else
                    high = mid;
            }
            return nullptr;
        }

        // Binary search over the sorted codepoint map; null when absent.
        const ImSlugPackedGlyphEntry* FindCodepoint(const ImSlugPackedFont* font, uint32_t codepoint)
        {
            const ImSlugPackedHeader* header = font->AssetView.Header;
            uint32_t low = 0;
            uint32_t high = header->CodepointCount;
            while (low < high)
            {
                const uint32_t mid = low + (high - low) / 2;
                const uint32_t value = font->AssetView.Codepoints[mid].Codepoint;
                if (value == codepoint)
                    return &font->AssetView.Glyphs[font->AssetView.Codepoints[mid].GlyphEntry];
                if (value < codepoint)
                    low = mid + 1;
                else
                    high = mid;
            }
            return nullptr;
        }

        // Reconstructs the glyph's outline quads from its curve texels (fp16;
        // the values are the pool's, exactly what the GPU/CPU rasterizer read).
        bool GetEntryQuads(const ImSlugPackedFont* font, const ImSlugPackedGlyphEntry& entry, ImSlugQuad* outQuads,
                           int quadCapacity, int* outCount)
        {
            *outCount = 0;
            const int curveCount = (int)entry.Record.CurveCount;
            if (curveCount > quadCapacity)
            {
                *outCount = curveCount;
                return false;
            }
            for (int i = 0; i < curveCount; ++i)
            {
                const int x = (int)(((uint32_t)entry.Record.CurveLocX + (uint32_t)(2 * i)) & (kPoolWidth - 1));
                const int y = entry.Record.CurveLocY +
                              (int)(((uint32_t)entry.Record.CurveLocX + (uint32_t)(2 * i)) >> kPoolWidthLog2);
                const uint16_t* first = CurveTexelAt(font->AssetView, x, y);
                const uint16_t* second = CurveTexelAt(font->AssetView, x + 1, y);
                ImSlugQuad& quad = outQuads[i];
                quad.X1 = ImSlugHalfToFloat(first[0]);
                quad.Y1 = ImSlugHalfToFloat(first[1]);
                quad.X2 = ImSlugHalfToFloat(first[2]);
                quad.Y2 = ImSlugHalfToFloat(first[3]);
                quad.X3 = ImSlugHalfToFloat(second[0]);
                quad.Y3 = ImSlugHalfToFloat(second[1]);
            }
            *outCount = curveCount;
            return true;
        }

        // Copies one glyph's curve/band data from the asset blobs into the
        // target pools and rebases the record. Transactional: on any failure
        // the pool cursors/bookkeeping are restored.
        ImSlugBuildStatus CopyGlyphToPools(ImSlugFont* fontBase, ImSlugPools* pools, int glyphIndex,
                                           const ImSlugPackedGlyphEntry& entry, ImSlugGlyphRecord* record)
        {
            ImSlugPackedFont* font = PackedFont(fontBase);
            const ImSlugGlyphRecord& source = entry.Record;
            const int curveCount = (int)source.CurveCount;

            const int curveCursor = pools->Curves.CursorTexels;
            const int curveUsedRows = pools->Curves.UsedRows;
            const int curveDirtyMin = pools->Curves.DirtyRowMin;
            const int curveDirtyMax = pools->Curves.DirtyRowMax;
            const int bandCursor = pools->Bands.CursorTexels;
            const int bandUsedRows = pools->Bands.UsedRows;
            const int bandDirtyMin = pools->Bands.DirtyRowMin;
            const int bandDirtyMax = pools->Bands.DirtyRowMax;

            ImSlugBuildStatus status = ImSlugBuildStatus_Ok;

            // Curve block: one contiguous 2-texel block, copied verbatim.
            ImSlugCurveBlock curveBlock;
            ImSlugBandBlock bandBlock;
            memset(&bandBlock, 0, sizeof(bandBlock));
            if (!ImSlugAllocCurveBlock(&pools->Curves, curveCount, &curveBlock))
            {
                status = ImSlugBuildStatus_CurvePoolLimit;
            }
            else
            {
                for (int i = 0; status == ImSlugBuildStatus_Ok && i < curveCount; ++i)
                {
                    const uint32_t sourceLinear =
                        (uint32_t)source.CurveLocY * kPoolWidth + (uint32_t)source.CurveLocX + (uint32_t)(2 * i);
                    const uint16_t* sourceTexel = CurveTexelAt(font->AssetView, (int)(sourceLinear & (kPoolWidth - 1)),
                                                               (int)(sourceLinear >> kPoolWidthLog2));
                    int targetX = 0, targetY = 0;
                    ImSlugCurveLocAt(curveBlock, i, &targetX, &targetY);
                    ImSlugCurveWriteTexelRaw(&pools->Curves, targetX, targetY, sourceTexel);
                }

                // Band block: read the per-band reference counts from the
                // asset, let the target allocator lay the block out, then write
                // the headers and the (delta-patched) references.
                const int headerCount = source.BandMaxY + 1 + source.BandMaxX + 1;
                int refCounts[2 * kMaxBandCountPerAxis];
                for (int band = 0; band < headerCount; ++band)
                {
                    const uint16_t* header = BandTexelAt(font->AssetView, source.BandLocX + band, source.BandLocY);
                    refCounts[band] = header[0];
                }

                uint16_t listOffsets[2 * kMaxBandCountPerAxis];
                status = ImSlugAllocBandBlock(&pools->Bands, source.BandMaxX, source.BandMaxY, refCounts, &bandBlock,
                                              listOffsets);
                if (status == ImSlugBuildStatus_Ok)
                {
                    // Linear delta from the asset's curve block to the target's;
                    // band references store absolute texel coordinates, so one
                    // delta patches every reference.
                    const int64_t curveDelta = (int64_t)curveBlock.Y * kPoolWidth + curveBlock.X -
                                               ((int64_t)source.CurveLocY * kPoolWidth + source.CurveLocX);
                    for (int band = 0; band < headerCount && status == ImSlugBuildStatus_Ok; ++band)
                    {
                        const uint16_t* header = BandTexelAt(font->AssetView, source.BandLocX + band, source.BandLocY);
                        const uint32_t sourceListOffset = header[1];
                        ImSlugBandWriteHeader(&pools->Bands, bandBlock, band, (uint16_t)refCounts[band],
                                              listOffsets[band]);
                        for (int r = 0; r < refCounts[band]; ++r)
                        {
                            int refX = 0, refY = 0;
                            ImSlugCalcBandLoc(source.BandLocX, source.BandLocY, sourceListOffset + (uint32_t)r, &refX,
                                              &refY);
                            const uint16_t* ref = BandTexelAt(font->AssetView, refX, refY);
                            const int64_t targetLinear = (int64_t)ref[1] * kPoolWidth + ref[0] + curveDelta;
                            // The delta maps the asset block onto the target
                            // block, so the patched reference stays a first
                            // curve texel (even x, pair shares a row).
                            IMSLUG_ASSERT(targetLinear >= 0 &&
                                          targetLinear + 1 < (int64_t)pools->Curves.Height * kPoolWidth &&
                                          (targetLinear & 1) == 0);
                            ImSlugBandWriteRef(&pools->Bands, bandBlock, listOffsets[band], r,
                                               (uint16_t)(targetLinear & (kPoolWidth - 1)),
                                               (uint16_t)(targetLinear >> kPoolWidthLog2));
                        }
                    }
                }
            }

            if (status != ImSlugBuildStatus_Ok)
            {
                // Roll back to the pre-copy pool state.
                pools->Curves.CursorTexels = curveCursor;
                pools->Curves.UsedRows = curveUsedRows;
                pools->Curves.DirtyRowMin = curveDirtyMin;
                pools->Curves.DirtyRowMax = curveDirtyMax;
                pools->Bands.CursorTexels = bandCursor;
                pools->Bands.UsedRows = bandUsedRows;
                pools->Bands.DirtyRowMin = bandDirtyMin;
                pools->Bands.DirtyRowMax = bandDirtyMax;
                return status;
            }

            *record = source;
            record->CurveLocX = curveBlock.X;
            record->CurveLocY = curveBlock.Y;
            record->BandLocX = bandBlock.X;
            record->BandLocY = bandBlock.Y;
            (void)glyphIndex;
            return ImSlugBuildStatus_Ok;
        }
    }  // namespace

    // ---------------------------------------------------------------------
    // Validation / inspection API
    // ---------------------------------------------------------------------
    bool ImSlugPackedIsContainer(const void* data, int size)
    {
        return data != nullptr && size >= (int)sizeof(kPackedAssetMagic) &&
               memcmp(data, kPackedAssetMagic, sizeof(kPackedAssetMagic)) == 0;
    }

    bool ImSlugValidatePackedMemory(const void* data, int size, ImSlugPackedInfo* outInfo, char* message,
                                    int messageCapacity)
    {
        return ValidateAsset(data, size, outInfo, message, messageCapacity);
    }

    // ---------------------------------------------------------------------
    // Packed font kind
    // ---------------------------------------------------------------------
    ImSlugFont* ImSlugCreateFontFromPackedMemory(const void* data, int size)
    {
        if (!ValidateAsset(data, size, nullptr, nullptr, 0))
            return nullptr;

        ImSlugPackedFont* font = (ImSlugPackedFont*)malloc(sizeof(ImSlugPackedFont));
        if (font == nullptr)
            return nullptr;
        memset(font, 0, sizeof(*font));
        font->Base.Kind = ImSlugFontKind_Packed;

        unsigned char* owned = (unsigned char*)malloc((size_t)size);
        if (owned == nullptr)
        {
            free(font);
            return nullptr;
        }
        memcpy(owned, data, (size_t)size);
        font->OwnedData = owned;
        font->DataSize = size;

        // Rebuild the view over the owned copy (the caller's bytes may die).
        font->AssetView.Data = owned;
        font->AssetView.Size = (uint32_t)size;
        font->AssetView.Header = (const ImSlugPackedHeader*)owned;
        font->AssetView.Glyphs = (const ImSlugPackedGlyphEntry*)(owned + font->AssetView.Header->GlyphTableOffset);
        font->AssetView.Codepoints =
            (const ImSlugPackedCodepointEntry*)(owned + font->AssetView.Header->CodepointMapOffset);
        font->AssetView.CurveTexels = (const uint16_t*)(owned + font->AssetView.Header->CurveBlobOffset);
        font->AssetView.BandTexels = (const uint16_t*)(owned + font->AssetView.Header->BandBlobOffset);
        font->Base.UnitsPerEm = (int)font->AssetView.Header->UnitsPerEm;
        return &font->Base;
    }

    void ImSlugPackedDestroy(ImSlugFont* font)
    {
        if (font == nullptr)
            return;
        ImSlugPackedFont* packed = PackedFont(font);
        ImSlugFreeFontCommon(font);
        free(packed->OwnedData);
        free(packed);
    }

    // ---------------------------------------------------------------------
    // Metrics (exact TTF formulas over stored values; see imslug_font.h)
    // ---------------------------------------------------------------------
    void ImSlugPackedGetFontMetrics(const ImSlugFont* font, float bakeSizePixels, ImSlugFontMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        const ImSlugPackedFont* packed = PackedFont(font);
        const ImSlugPackedHeader* header = packed->AssetView.Header;

        // stbtt_ScaleForPixelHeight(font, 1.0f) * bakeSizePixels, same
        // expression order as the TTF path.
        const float scale = (1.0f / (float)(header->HheaAscent - header->HheaDescent)) * bakeSizePixels;
        outMetrics->UnitsPerEm = (int)header->UnitsPerEm;
        outMetrics->HheaAscent = header->HheaAscent;
        outMetrics->HheaDescent = header->HheaDescent;
        outMetrics->ScaleForPixelHeight = scale;
        outMetrics->PixelsPerEm = scale * (float)header->UnitsPerEm;
        outMetrics->Ascent = ImSlugImCeil((float)header->HheaAscent * scale);
        outMetrics->Descent = ImSlugImFloor((float)header->HheaDescent * scale);
    }

    bool ImSlugPackedGetGlyphMetricsByIndex(const ImSlugFont* font, int glyphIndex, float bakeSizePixels,
                                            ImSlugGlyphMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        const ImSlugPackedFont* packed = PackedFont(font);
        const ImSlugPackedGlyphEntry* entry = FindGlyphEntry(packed, glyphIndex);
        if (entry == nullptr)
            return false;

        const ImSlugPackedHeader* header = packed->AssetView.Header;
        const float scale = (1.0f / (float)(header->HheaAscent - header->HheaDescent)) * bakeSizePixels;

        outMetrics->AdvanceX = (float)entry->AdvanceFu * scale;
        outMetrics->AdvanceEm = (float)entry->AdvanceFu / (float)header->UnitsPerEm;
        outMetrics->MinX = entry->Record.MinX;
        outMetrics->MinY = entry->Record.MinY;
        outMetrics->MaxX = entry->Record.MaxX;
        outMetrics->MaxY = entry->Record.MaxY;

        // stbtt_GetGlyphBitmapBoxSubpixel with shift 0 (STBTT_ifloor/iceil).
        const int x0 = (int)floorf((float)entry->BoxX0 * scale);
        const int y0 = (int)floorf((float)(-entry->BoxY1) * scale);
        const int x1 = (int)ceilf((float)entry->BoxX1 * scale);
        const int y1 = (int)ceilf((float)(-entry->BoxY0) * scale);
        if (x0 == x1 || y0 == y1)
        {
            // Invisible glyph: ImFontGlyph's zero-initialized corners.
            outMetrics->X0 = 0.0f;
            outMetrics->Y0 = 0.0f;
            outMetrics->X1 = 0.0f;
            outMetrics->Y1 = 0.0f;
        }
        else
        {
            const float ascentOffset = ImSlugImRound(ImSlugImCeil((float)header->HheaAscent * scale));
            outMetrics->X0 = (float)x0;
            outMetrics->Y0 = (float)y0 + ascentOffset;
            outMetrics->X1 = (float)x1;
            outMetrics->Y1 = (float)y1 + ascentOffset;
        }
        return true;
    }

    int ImSlugPackedGetGlyphIndex(const ImSlugFont* font, unsigned int codepoint)
    {
        IMSLUG_ASSERT(font != nullptr);
        const ImSlugPackedGlyphEntry* entry = FindCodepoint(PackedFont(font), codepoint);
        return entry != nullptr ? entry->GlyphIndex : 0;
    }

    int ImSlugPackedGetGlyphCount(const ImSlugFont* font)
    {
        IMSLUG_ASSERT(font != nullptr);
        return (int)PackedFont(font)->AssetView.Header->GlyphCount;
    }

    bool ImSlugPackedGetGlyphOutlineQuads(const ImSlugFont* font, int glyphIndex, ImSlugQuad* outQuads,
                                          int quadCapacity, int* outCount)
    {
        IMSLUG_ASSERT(font != nullptr && outCount != nullptr);
        *outCount = 0;
        if (glyphIndex <= 0)
            return false;
        const ImSlugPackedGlyphEntry* entry = FindGlyphEntry(PackedFont(font), glyphIndex);
        if (entry == nullptr)
            return false;
        return GetEntryQuads(PackedFont(font), *entry, outQuads, quadCapacity, outCount);
    }

    bool ImSlugPackedGetGlyphSourceMetrics(const ImSlugFont* font, int glyphIndex, ImSlugGlyphSourceMetrics* outMetrics)
    {
        IMSLUG_ASSERT(font != nullptr && outMetrics != nullptr);
        outMetrics->AdvanceFu = 0;
        outMetrics->BoxX0 = 0;
        outMetrics->BoxY0 = 0;
        outMetrics->BoxX1 = 0;
        outMetrics->BoxY1 = 0;
        const ImSlugPackedGlyphEntry* entry = FindGlyphEntry(PackedFont(font), glyphIndex);
        if (entry == nullptr)
            return false;
        outMetrics->AdvanceFu = entry->AdvanceFu;
        outMetrics->BoxX0 = entry->BoxX0;
        outMetrics->BoxY0 = entry->BoxY0;
        outMetrics->BoxX1 = entry->BoxX1;
        outMetrics->BoxY1 = entry->BoxY1;
        return true;
    }

    // ---------------------------------------------------------------------
    // Record loading (copy + rebase into the target pools)
    // ---------------------------------------------------------------------
    ImSlugBuildStatus ImSlugPackedBuildGlyphRecord(ImSlugFont* font, ImSlugPools* pools, int glyphIndex,
                                                   ImSlugGlyphRecord* record)
    {
        const ImSlugPackedGlyphEntry* entry = FindGlyphEntry(PackedFont(font), glyphIndex);
        if (entry == nullptr)
            return ImSlugBuildStatus_GlyphNotFound;
        return CopyGlyphToPools(font, pools, glyphIndex, *entry, record);
    }
}  // namespace ImSlug
