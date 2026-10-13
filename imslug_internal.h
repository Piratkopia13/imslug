// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_internal.h — implementation-only shared data contract for the glyph
// pipeline (M1): TTF bytes -> em-space quadratics -> curve/band pools -> CPU
// reference rasterizer. This header holds the shared *types and constants*;
// the APIs live in the module headers included at the bottom of this file:
//
//   imslug_packer.h     shared curve/band pools, allocation, dirty rows
//   imslug_glyphgen.h   outline extraction, band construction, font records
//   imslug_refraster.h  CPU coverage rasterizer (arbiter for M2+)
//
// M3+ code includes this one header. See the M1 completion notes in
// .agent-docs/milestones/M1-core-glyph-pipeline.md and the skill
// .opencode/skills/imslug-glyph-format.
//
// Rules for this header (AGENTS.md invariant 4, M1 brief):
//   * never include imgui.h;
//   * no STL containers/strings — plain buffers and explicit alloc/free;
//   * no exceptions: fallible entry points return bool / ImSlugBuildStatus.
// The implementation is C++17.
//
// Data format (contract: PLAN §2.1-2.2; mirrored by the reference shaders in M2):
//   * Em space everywhere: 1.0 = em, Y up, origin at the pen position on the
//     baseline. Never mix font units or pixels into stored geometry.
//   * Curve pool: RGBA16F, fixed width 4096. One quadratic = 2 texels:
//     (x1,y1,x2,y2) then (x3,y3,0,0). Lines are {p1,p3,p3} (never degenerate).
//   * Band pool: R16G16_UINT, fixed width 4096. Per glyph, at glyphLoc:
//       row glyphLoc.y, x = glyphLoc.x + 0 .. bandMaxY  — horizontal band
//       headers (curveCount, offset);
//       row glyphLoc.y, x = glyphLoc.x + bandMaxY+1 .. — vertical band headers;
//       then curve reference lists (curveTexelX, curveTexelY), addressed with
//       CalcBandLoc(glyphLoc, offset).
//     Both header runs live in the glyph's first row, exactly as the reference
//     pixel shader reads them (SlugPixelShader.hlsl: glyphLoc.x + bandIndex.y
//     and glyphLoc.x + bandMax.y + 1 + bandIndex.x, row glyphLoc.y).
//   * The E (even-odd) flag is bit 28 of the packed bandMax word (tex.w), i.e.
//     bit 12 of its high 16-bit half — see ImSlugPackBandMaxWord from the
//     reference vertex/pixel shader comments (PLAN §2.3).
//
// References (attribution: NOTICE section 2/4):
//   * Slug reference shaders, Copyright 2017 Eric Lengyel (MIT OR Apache-2.0):
//     https://github.com/EricLengyel/Slug — SlugPixelShader.hlsl blob
//     170a13f0fe3ad7562877eb89c4afcd36b89bdbce is the math and addressing
//     contract mirrored by imslug_refraster.cpp.
//   * Slughorn (MIT): https://github.com/AlphaPixel/slughorn — render.hpp blob
//     1b87727ce5f68b8b3c2c383ce0991fec9652b3ad is the CPU cross-check for the
//     same math (and the degenerate-division guard in the root solver).
//   * Dear ImGui v1.92.9b (imgui_draw.cpp) — the metrics formulas mirrored by
//     imslug_glyphgen.cpp (comments cite the exact lines).

#pragma once

// stb_truetype.h is vendored unmodified under third_party/stb/ (see its
// PROVENANCE.txt). The implementation is compiled in imslug_stb.cpp, in a
// separate target with third-party warning settings.
#include "stb_truetype.h"

#include <stddef.h>
#include <stdint.h>

namespace ImSlug
{
// ---------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------
// ImGui-style assert: active unless NDEBUG. Runtime capacity problems must
// still be reported through return values — never rely on asserts for
// fallback behavior (PLAN R11).
#if !defined(IMSLUG_ASSERT)
#include <assert.h>
#if defined(NDEBUG)
#define IMSLUG_ASSERT(_EXPR) ((void)0)
#else
#define IMSLUG_ASSERT(_EXPR) assert(_EXPR)
#endif
#endif

    // ---------------------------------------------------------------------
    // Format constants (must stay in sync with the future shaders)
    // ---------------------------------------------------------------------
    static const int kPoolWidth = 4096;  // texels per row, both pools
    static const int kPoolWidthLog2 = 12;
    static const int kPoolMaxHeight = 65536;                  // 16-bit texel y coordinate
    static const int kMaxBandCountPerAxis = 32;               // adaptive cap (M4 re-tunes)
    static const int kGlyphMaxBlockTexels = 65535;            // u16 band offsets
    static const float kBandOverlapEpsilon = 1.0f / 1024.0f;  // em (PLAN §2.2)

    // ---------------------------------------------------------------------
    // Pools (one curve pool + one band pool shared by every Slug font)
    // ---------------------------------------------------------------------
    struct ImSlugCurvePool
    {
        uint16_t* Texels;  // Width * Height texels, 4 halves each (RGBA16F)
        int Width;         // kPoolWidth
        int Height;        // allocated rows (grows by doubling)
        int CursorTexels;  // linear allocation cursor (always 2-texel aligned)
        int UsedRows;      // rows written at least once (for the first upload)
        int DirtyRowMin;   // inclusive dirty range since the last clear;
        int DirtyRowMax;   // empty when DirtyRowMin > DirtyRowMax
    };

    struct ImSlugBandPool
    {
        uint16_t* Texels;  // Width * Height texels, 2 halves each (R16G16_UINT)
        int Width;
        int Height;
        int CursorTexels;
        int UsedRows;
        int DirtyRowMin;
        int DirtyRowMax;
    };

    struct ImSlugPools
    {
        ImSlugCurvePool Curves;
        ImSlugBandPool Bands;
    };

    // ---------------------------------------------------------------------
    // Glyph record (CPU/vertex data, never a texture)
    // ---------------------------------------------------------------------
    enum ImSlugGlyphFlags
    {
        ImSlugGlyphFlags_None = 0,
        ImSlugGlyphFlags_EvenOdd = 1 << 0,  // E flag; packed to bit 28 of tex.w
    };

    struct ImSlugGlyphRecord
    {
        int32_t BandLocX, BandLocY;      // glyphLoc in the band pool
        int32_t BandMaxX, BandMaxY;      // band count - 1 per axis
        float BandScaleX, BandScaleY;    // bandCount / em extent (bands per em)
        float BandOffsetX, BandOffsetY;  // -emMin * bandScale
        int32_t CurveLocX, CurveLocY;    // first curve texel of the contiguous block
        float MinX, MinY, MaxX, MaxY;    // em-space control-point bbox (Y up)
        float Advance;                   // em-space advance (advance_fu / unitsPerEm)
        uint32_t CurveCount;
        uint32_t Flags;  // ImSlugGlyphFlags
    };

    // ---------------------------------------------------------------------
    // Outline data (em space, Y up)
    // ---------------------------------------------------------------------
    struct ImSlugQuad
    {
        float X1, Y1;  // start point
        float X2, Y2;  // control point
        float X3, Y3;  // end point
    };

    struct ImSlugCubic
    {
        float X1, Y1;
        float X2, Y2;
        float X3, Y3;
        float X4, Y4;
    };

    struct ImSlugQuadBounds
    {
        float MinX, MinY, MaxX, MaxY;  // control-point bbox (of the curve)
    };

    // ---------------------------------------------------------------------
    // Build status (capacity guards feed the graceful fallback of PLAN R11)
    // ---------------------------------------------------------------------
    enum ImSlugBuildStatus
    {
        ImSlugBuildStatus_Ok = 0,
        ImSlugBuildStatus_InvalidFont,    // stbtt could not parse the data
        ImSlugBuildStatus_GlyphNotFound,  // codepoint resolves to .notdef (index 0)
        ImSlugBuildStatus_OutOfMemory,
        ImSlugBuildStatus_CurvePoolLimit,  // 16-bit texel y / pool height reached
        ImSlugBuildStatus_BandPoolLimit,
        ImSlugBuildStatus_GlyphTooLarge,  // band block > 65535 texels (u16 offsets)
    };
}  // namespace ImSlug

// Module APIs (the shared types above are visible to all of them).
#include "imslug_packer.h"
#include "imslug_glyphgen.h"
#include "imslug_refraster.h"
