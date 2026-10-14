// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packed.h — the `.imslug` container v1: the offline asset format
// produced by tools/imslug_gen and loaded without any outline parser. The
// durable format specification lives in docs/PACKED_FORMAT.md; this header is
// the machine-checked definition (layout + validation + the packed font kind).
//
// Design contract:
//   * the asset is generated from the *runtime* generator code — the same
//     font handle, band construction and packer — so a packed glyph record,
//     its curve texels and its band texels are byte-identical to what the
//     runtime path would have built (the round-trip test proves it);
//   * the loader copies each glyph's pool data into the shared pools on first
//     use and patches the band references by the curve-block delta; it never
//     parses an outline;
//   * the container is little-endian and deterministic: same font bytes +
//     same generator + same options => byte-identical output (no timestamps,
//     stable ordering);
//   * v1 is monochrome: a packed font has no COLR/CPAL payload, so color
//     glyphs fall back to their glyf outline exactly like a font without
//     color tables. Color support is future work behind a version bump.
//
// Layout (all offsets in bytes from the file start, all integers
// little-endian; `align16(x) = (x + 15) & ~15`):
//
//   [0, 112)                        ImSlugPackedHeader (HeaderSize = 112)
//   [glyphTableOffset, +N*92)       ImSlugPackedGlyphEntry[N], sorted by
//                                   GlyphIndex ascending (GlyphIndex > 0)
//   [codepointMapOffset, +M*8)      ImSlugPackedCodepointEntry[M], sorted by
//                                   Codepoint ascending
//   [sourceNameOffset, +len+1)      source file name, UTF-8 + NUL
//   [curveBlobOffset, +rows*32768)  curve pool rows (4096 texels * 8 bytes)
//   [bandBlobOffset, +rows*16384)   band pool rows (4096 texels * 4 bytes)
//
// The writer emits the canonical layout validated here (regions in the order
// above, blobs 16-byte aligned, no padding bytes other than the alignment
// zeros).

#pragma once

#include "imslug_font.h"

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Format constants
    // ---------------------------------------------------------------------
    static const uint32_t kPackedAssetVersion = 1;
    static const uint32_t kPackedAssetHeaderSize = 112;
    static const uint32_t kPackedAssetAlign = 16;
    static const char kPackedAssetMagic[8] = { 'I', 'M', 'S', 'L', 'U', 'G', '\0', '\0' };

    static inline uint32_t ImSlugPackedAlign16(uint32_t value)
    {
        return (value + (kPackedAssetAlign - 1)) & ~(kPackedAssetAlign - 1);
    }

    // Fixed header (112 bytes; static_asserted). `SourceChecksum` is FNV-1a
    // 32-bit over the source font bytes; `SizeMinPxBits`/`SizeMaxPxBits` are
    // the f32 bit patterns of the CLI's `--size-range` (diagnostics only —
    // glyph geometry is size-independent).
    struct ImSlugPackedHeader
    {
        char Magic[8];              // kPackedAssetMagic
        uint32_t Version;           // kPackedAssetVersion
        uint32_t HeaderSize;        // sizeof(ImSlugPackedHeader)
        uint32_t GeneratorVersion;  // IMSLUG_VERSION_NUM at generation time
        uint32_t Flags;             // reserved; 0 in v1
        uint32_t UnitsPerEm;
        int32_t HheaAscent;
        int32_t HheaDescent;
        uint32_t SourceSize;      // source font bytes
        uint32_t SourceChecksum;  // FNV-1a 32
        uint32_t GlyphCount;      // glyph table entries
        uint32_t CodepointCount;  // codepoint map entries
        uint32_t CurveRows;       // curve blob rows (4096 * 8 bytes each)
        uint32_t BandRows;        // band blob rows (4096 * 4 bytes each)
        uint32_t GlyphTableOffset;
        uint32_t CodepointMapOffset;
        uint32_t CurveBlobOffset;
        uint32_t BandBlobOffset;
        uint32_t SourceNameOffset;
        uint32_t SourceNameLength;  // excluding the NUL
        uint32_t SizeMinPxBits;     // f32
        uint32_t SizeMaxPxBits;     // f32
        uint32_t Reserved[5];       // 0
    };
    static_assert(sizeof(ImSlugPackedHeader) == kPackedAssetHeaderSize, "packed container v1 header layout");
    static_assert(offsetof(ImSlugPackedHeader, GlyphTableOffset) == 60, "packed container v1 header layout");

    // One baked glyph (92 bytes). `Record` is the runtime glyph record whose
    // pool locations address the asset's blobs; the loader rebases them onto
    // the target pools.
    struct ImSlugPackedGlyphEntry
    {
        int32_t GlyphIndex;                  // source font glyph id
        int32_t AdvanceFu;                   // hmtx advance, font units
        int32_t BoxX0, BoxY0, BoxX1, BoxY1;  // glyf box, font units
        ImSlugGlyphRecord Record;
    };
    static_assert(sizeof(ImSlugPackedGlyphEntry) == 92, "packed container v1 glyph entry layout");
    static_assert(offsetof(ImSlugPackedGlyphEntry, Record) == 24, "packed container v1 glyph entry layout");

    struct ImSlugPackedCodepointEntry
    {
        uint32_t Codepoint;
        uint32_t GlyphEntry;  // index into the glyph table
    };
    static_assert(sizeof(ImSlugPackedCodepointEntry) == 8, "packed container v1 codepoint map layout");

    // ---------------------------------------------------------------------
    // Validation / inspection
    // ---------------------------------------------------------------------
    // Filled by ImSlugValidatePackedMemory; `SourceName` points into the
    // validated bytes (NUL-terminated) and is only valid while they are.
    struct ImSlugPackedInfo
    {
        uint32_t Version;
        uint32_t GeneratorVersion;
        uint32_t Flags;
        uint32_t UnitsPerEm;
        int32_t HheaAscent, HheaDescent;
        uint32_t SourceSize;
        uint32_t SourceChecksum;
        uint32_t GlyphCount;
        uint32_t CodepointCount;
        uint32_t CurveRows, BandRows;
        float SizeMinPx, SizeMaxPx;
        const char* SourceName;
        uint32_t AssetSize;
    };

    // Cheap content sniff: true when `data` starts with the container magic.
    // Never dereferences more than 8 bytes.
    bool ImSlugPackedIsContainer(const void* data, int size);

    // Full structural validation: header fields, canonical region layout,
    // sorted tables, every glyph record (pool ranges, band header/list row
    // rules, curve references) and the source name. Returns true and fills
    // *outInfo (when non-null) for a valid v1 asset; otherwise returns false
    // and, when `message` is non-null, writes a human-readable reason into
    // `message` (NUL-terminated, truncated to messageCapacity). Corrupt input
    // never crashes and is never partially accepted.
    bool ImSlugValidatePackedMemory(const void* data, int size, ImSlugPackedInfo* outInfo, char* message,
                                    int messageCapacity);

    // Creates a packed font from validated container bytes. The bytes are
    // copied (the font owns its copy for its lifetime). Returns null for any
    // invalid input (use ImSlugValidatePackedMemory for a reason). The font is
    // destroyed with ImSlugDestroyFont like any other font.
    ImSlugFont* ImSlugCreateFontFromPackedMemory(const void* data, int size);
}  // namespace ImSlug
