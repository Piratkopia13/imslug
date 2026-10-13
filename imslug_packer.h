// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_packer.h — the shared curve/band pools: allocation, texel writes,
// row-wrap addressing, growth by doubling and dirty-row tracking for uploads.
// Types (ImSlugPools, ImSlugGlyphRecord, ...) are in imslug_internal.h.
//
// Format contract: PLAN §2.1/§4.3 and the reference pixel shader's CalcBandLoc.
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

    // Dirty rows: marked by the write helpers below, used by M2 uploads.
    // ImSlugPoolClearDirtyRows resets both pools (call once per frame after
    // the backend consumed the ranges).
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
    // empty lists). Fails with false when the block cannot be represented
    // (u16 offsets) or the pool would exceed the 16-bit height.
    bool ImSlugAllocBandBlock(ImSlugBandPool* pool, int bandMaxX, int bandMaxY, const int* refCounts,
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
}  // namespace ImSlug
