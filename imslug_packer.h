// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packer.h — the shared curve/band pools: allocation, texel writes,
// row-wrap addressing, growth by doubling and dirty-row tracking for uploads.
// Types (ImSlugPools, ImSlugGlyphRecord, ...) are in imslug_internal.h.
//
// Format contract: the imslug-glyph-format skill and the reference pixel
// shader's CalcBandLoc.
// Note on addresses:
//   * curve blocks are contiguous 2-texel units starting on an even x, so the
//     pair (curveLoc, curveLoc + 1) never crosses a row (the reference shader
//     reads the second texel as x + 1 without wrapping);
//   * band blocks are arbitrary-length linear runs addressed with CalcBandLoc,
//     which wraps rows — blocks may start and end mid-row.

#pragma once

#include "imslug_internal.h"

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // IEEE binary16 (half) conversion, round-to-nearest-even.
    // The pools store 16F texels so the CPU reference rasterizer reads exactly
    // the bytes the GPU will read.
    // ---------------------------------------------------------------------
    uint16_t ImSlugFloatToHalf(float value);
    float ImSlugHalfToFloat(uint16_t value);

    // ---------------------------------------------------------------------
    // Lifetime. Zero-initialize; memory is allocated lazily on the first
    // allocation call (initialCurveRows/initialBandRows are the doubling base).
    // ---------------------------------------------------------------------
    void ImSlugInitPools(ImSlugPools* pools);
    void ImSlugDestroyPools(ImSlugPools* pools);

    // Growth by doubling until at least `rows` rows exist. False on OOM.
    bool ImSlugPoolEnsureCurveRows(ImSlugCurvePool* pool, int rows);
    bool ImSlugPoolEnsureBandRows(ImSlugBandPool* pool, int rows);

    // Dirty rows: marked by the write helpers below, consumed by backend
    // uploads. The per-pool variants clear one pool (a partial upload that
    // consumed only that pool must not drop the other pool's pending rows);
    // ImSlugPoolClearDirtyRows resets both at once.
    void ImSlugCurvePoolClearDirtyRows(ImSlugCurvePool* pool);
    void ImSlugBandPoolClearDirtyRows(ImSlugBandPool* pool);
    void ImSlugPaintPoolClearDirtyRecords(ImSlugPaintPool* pool);
    void ImSlugPoolClearDirtyRows(ImSlugPools* pools);

    // ---------------------------------------------------------------------
    // Curve pool
    // ---------------------------------------------------------------------
    struct ImSlugCurveBlock
    {
        int X, Y;  // location of the first curve's first texel
        int CurveCount;
    };

    bool ImSlugAllocCurveBlock(ImSlugCurvePool* pool, int curveCount, ImSlugCurveBlock* outBlock);

    // Location of curve i (0-based) inside a block; no pool access. The caller
    // guarantees 0 <= curveIndex < block.CurveCount.
    void ImSlugCurveLocAt(const ImSlugCurveBlock& block, int curveIndex, int* outX, int* outY);

    // Write p1/p2 (first texel) and p3 (second texel) of one curve. Coordinates
    // are rounded to half precision here — the pool is the source of truth.
    // Marks the touched rows dirty and updates UsedRows.
    void ImSlugWriteCurve(ImSlugCurvePool* pool, int texelX, int texelY, float x1, float y1, float x2, float y2,
                          float x3, float y3);

    // Copies one curve's two texels (8 halves) verbatim into the pool at
    // (texelX, texelY); marks the touched rows dirty and updates UsedRows.
    // Used by the packed loader so the copied pool bytes are exactly the
    // asset's (no float round trip).
    void ImSlugCurveWriteTexelRaw(ImSlugCurvePool* pool, int texelX, int texelY, const uint16_t source[8]);

    // Read texel (x, y); returns a pointer to 4 halves. Out-of-bounds is a bug
    // (IMSLUG_ASSERT); never call it for coordinates outside the pool.
    const uint16_t* ImSlugCurveTexelAt(const ImSlugCurvePool* pool, int x, int y);

    // ---------------------------------------------------------------------
    // Band pool
    // ---------------------------------------------------------------------
    // Layout rules (derived from the reference pixel shader, which reads
    // headers at (glyphLoc.x + i, glyphLoc.y) and each list entry at
    // (listX + i, listY) — contiguous in x, never wrapping):
    //   * the whole header run must fit in one row;
    //   * each band's reference list must fit in one row; the allocator pads
    //     to the next row when a list would straddle;
    //   * offsets (u16, relative to glyphLoc) are resolved with CalcBandLoc,
    //     which reproduces the linear position because the runs do not wrap.
    struct ImSlugBandBlock
    {
        int X, Y;         // glyphLoc: first header texel
        int HeaderCount;  // bandMaxY + 1 + bandMaxX + 1
        int RefCount;     // total curve references in all bands
        int TotalTexels;  // reserved texels including row padding
    };

    // Lay out and reserve a glyph's band block. refCounts has HeaderCount
    // entries (horizontal bands first, then vertical) and outListOffsets gets
    // one u16 offset per band (relative to glyphLoc; equal to the cursor for
    // empty lists). Returns:
    //   Ok            - the block is reserved; outBlock and outListOffsets are
    //                   filled and the pool cursor advanced;
    //   GlyphTooLarge - the block cannot be represented in the band format: a
    //                   reference list longer than kPoolWidth (the shader reads
    //                   lists contiguously in x, so a list never straddles a
    //                   row) or a padded total > kGlyphMaxBlockTexels (u16
    //                   offsets). Definitive: the glyph falls back to stock
    //                   ImGui and must not be retried;
    //   BandPoolLimit - the pool would exceed the 16-bit height (or its row
    //                   growth failed); retryable.
    // On failure the pool cursor is unchanged (nothing is reserved).
    ImSlugBuildStatus ImSlugAllocBandBlock(ImSlugBandPool* pool, int bandMaxX, int bandMaxY, const int* refCounts,
                                           ImSlugBandBlock* outBlock, uint16_t* outListOffsets);

    // CalcBandLoc(glyphLoc, offset) — reference pixel shader:
    //   x = (glyphLocX + offset) & (kPoolWidth - 1)
    //   y = glyphLocY + ((glyphLocX + offset) >> kPoolWidthLog2)
    void ImSlugCalcBandLoc(int glyphLocX, int glyphLocY, uint32_t offset, int* outX, int* outY);

    // headerIndex 0..bandMaxY = horizontal band headers; bandMaxY+1.. = vertical
    // ones. listOffset is the offset of the band's reference list from glyphLoc
    // (as returned by ImSlugAllocBandBlock). Marks rows dirty.
    void ImSlugBandWriteHeader(ImSlugBandPool* pool, const ImSlugBandBlock& block, int headerIndex, uint16_t curveCount,
                               uint16_t listOffset);

    // Write entry `indexInList` of the list at `listOffset`. Marks the touched
    // row dirty.
    void ImSlugBandWriteRef(ImSlugBandPool* pool, const ImSlugBandBlock& block, uint16_t listOffset, int indexInList,
                            uint16_t curveTexelX, uint16_t curveTexelY);

    // Read texel (x, y); returns a pointer to 2 u16 components ((curveCount,
    // offset) for headers, (curveTexelX, curveTexelY) for references).
    const uint16_t* ImSlugBandTexelAt(const ImSlugBandPool* pool, int x, int y);

    // Packed tex.w as consumed by the vertex/pixel shaders:
    //   bits 0..15  bandMaxX
    //   bits 16..23 bandMaxY
    //   bit  28     E (even-odd fill rule; the pixel shader tests bit 12 of the
    //               high half, which is bit 28 of the whole word — reference VS
    //               diagram, confirmed against the reference PS)
    // Bits 24..27 and 29..31 are zero. Input flags are ImSlugGlyphFlags.
    uint32_t ImSlugPackBandMaxWord(int bandMaxX, int bandMaxY, uint32_t glyphFlags);

    // ---------------------------------------------------------------------
    // Band construction (pure quad helpers, so the packed loader and the
    // synthetic builder work without an outline parser)
    // ---------------------------------------------------------------------
    // Band count per axis: clamp(ceil(2*sqrt(curveCount)), 4, 32). The single
    // tuning point; every user of band counts calls it (the measurements
    // behind the constants are in imslug_packer.cpp).
    int ImSlugCalcBandCount(int curveCount);

    ImSlugQuadBounds ImSlugQuadControlBounds(const ImSlugQuad& quad);

    // Aggregate control-point bbox of a quad run; all-zero for an empty run.
    void ImSlugControlBoundsOfQuads(const ImSlugQuad* quads, int quadCount, float* outMinX, float* outMinY,
                                    float* outMaxX, float* outMaxY);

    // Assign curves to bands (pure, deterministic; used by the record builders
    // and by unit tests).
    //   counts: bandMaxY+1 horizontal counts then bandMaxX+1 vertical counts
    //           (zeroed by this call);
    //   refs:   flattened curve indices in band order; per band sorted
    //           descending by max x (horizontal) / max y (vertical), stable;
    //   returns false and sets *requiredRefs when refCapacity is too small.
    // Membership rule: a curve belongs to a band when its control-point bbox,
    // inflated by kBandOverlapEpsilon on both ends, overlaps the band's em
    // range. Exactly axis-parallel curves are excluded from the matching-axis
    // bands.
    bool ImSlugAssignBands(const ImSlugQuad* quads, int quadCount, int bandMaxX, int bandMaxY, float minX, float minY,
                           float maxX, float maxY, int* counts, int* refs, int refCapacity, int* requiredRefs);

    // ---------------------------------------------------------------------
    // Paint pool (color glyph layers)
    // ---------------------------------------------------------------------
    // Appends `count` records to the pool and returns the index of the first
    // one, or -1 on allocation failure (the pool is unchanged). The records
    // are *not* initialized: the caller fills them through
    // ImSlugPaintRecordAt. Growth by doubling (64-record base); the append
    // marks the new records dirty.
    int ImSlugAllocPaintRecords(ImSlugPaintPool* pool, int count);

    // Pointer to record `index` (asserts when out of range). Writes through it
    // are the caller's responsibility; call ImSlugPaintPoolMarkDirty after
    // filling records allocated by ImSlugAllocPaintRecords when the records are
    // (re)written later than the allocation.
    ImSlugPaintRecord* ImSlugPaintRecordAt(ImSlugPaintPool* pool, int index);
    const ImSlugPaintRecord* ImSlugPaintRecordAt(const ImSlugPaintPool* pool, int index);

    void ImSlugPaintPoolMarkDirty(ImSlugPaintPool* pool, int firstIndex, int count);
}  // namespace ImSlug
