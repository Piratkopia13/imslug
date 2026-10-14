// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_gen_writer.h — the `.imslug` container v1 writer. This is the
// offline half of the packed-asset pipeline: it consumes the *runtime* core
// code (the font handle, band construction, the packer) and serializes the
// exact pool texels + glyph records the runtime would have produced, so the
// loader can reproduce them byte for byte (the round-trip test proves it).
//
// The writer is deliberately tool-side (built into imslug_gen_core, linked by
// the CLI and the round-trip test): the runtime library never needs it. The
// container layout is defined in imslug_packed.h / docs/PACKED_FORMAT.md.
//
// Determinism contract: for the same font bytes, the same codepoint set (order
// independent — it is sorted and deduplicated), the same options and the same
// source name, the output is byte-identical: glyphs are built in ascending
// glyph-index order, the tables are sorted, alignment padding is zero, and no
// timestamps or environment data are embedded.

#pragma once

#include "imslug_internal.h"
#include "imslug_packed.h"

namespace ImSlug
{
    struct ImSlugAssetOptions
    {
        // Recorded in the header for diagnostics (glyph geometry is
        // size-independent; the range documents the sizes the asset was baked
        // for). Not used by the loader.
        float SizeMinPx = 8.0f;
        float SizeMaxPx = 72.0f;
    };

    struct ImSlugAssetStats
    {
        int GlyphCount;           // glyph table entries
        int CodepointCount;       // codepoint map entries
        int SkippedCodepoints;    // requested codepoints without a glyph in the font
        int FailedGlyphs;         // glyphs the runtime builder refused (capacity limits)
        int CurveRows, BandRows;  // pool rows serialized
        int AssetSize;            // total bytes written
    };

    // FNV-1a 32-bit over `size` bytes (the source-font checksum).
    uint32_t ImSlugFnv1a32(const void* data, int size);

    // Builds a v1 container for `codepoints` (any order, duplicates allowed)
    // from `font` (a TTF font handle). `sourceName` is stored for diagnostics
    // (the CLI passes the source file's base name); `sourceData`/`sourceDataSize`
    // provide the checksum. On success *outData (malloc'd, caller frees) and
    // *outSize receive the asset, and `outStats` (optional) is filled. On
    // failure returns false and, when `errorMessage` is non-null, writes a
    // reason into it. The output is self-validated before returning.
    //
    // Note: the writer uses the font's transient build pool set, so it clears
    // the font's cached glyph records on entry and exit (records handed out
    // before the call must not be used afterwards).
    bool ImSlugWritePackedAsset(ImSlugFont* font, const unsigned int* codepoints, int codepointCount,
                                const ImSlugAssetOptions& options, const char* sourceName, const void* sourceData,
                                int sourceDataSize, unsigned char** outData, int* outSize, ImSlugAssetStats* outStats,
                                char* errorMessage, int errorMessageCapacity);
}  // namespace ImSlug
