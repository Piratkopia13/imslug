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
//     1b87727ce5f68b8b3c2c383ce0991fec9652b3ad was the CPU cross-check for the
//     same math. The *only* code adapted from Slughorn is the degenerate-linear
//     guard expression in the root solvers (render.hpp _solveHorizPoly /
//     _solveVertPoly: divide by b only when |b| >= 1/65536, else t = 0), used
//     where the reference HLSL would divide by zero; its MIT license text is
//     quoted in NOTICE section 4. No other Slughorn code is copied.
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
    // Glyph instance (frozen M2 layout; mirrored by shaders/vulkan/slug.vert)
    // ---------------------------------------------------------------------
    // One instance per glyph quad. The vertex shader builds the quad corners
    // from gl_VertexIndex (4 vertices, triangle strip) and reads this record
    // through VK_VERTEX_INPUT_RATE_INSTANCE attributes at locations 0..6:
    //
    //   loc 0  vec4   EmBBox      (MinX, MinY, MaxX, MaxY) em space, Y up
    //   loc 1  vec4   ScreenRect  (MinX, MinY, MaxX, MaxY) screen pixels, Y down
    //   loc 2  vec4   Jac         inverse Jacobian rows: em += (dot(d, .xy), dot(d, .zw))
    //   loc 3  vec4   Banding     (BandScaleX, BandScaleY, BandOffsetX, BandOffsetY)
    //   loc 4  uvec2  GlyphLoc    band pool glyph location (x, y)
    //   loc 5  uint   BandMaxWord bandMaxX | bandMaxY << 16 | E << 28 (D8(g))
    //   loc 6  vec4   Color       straight alpha (r, g, b, a)
    //
    // ScreenRect corners correspond to EmBBox corners with the Y flip applied
    // (em Y up, screen Y down): em (MinX, MinY) maps to (ScreenMinX,
    // ScreenMaxY). The placement must be an axis-aligned scale+translate (no
    // rotation): the shader dilates along the screen-space corner diagonals.
    struct ImSlugGlyphInstance
    {
        float EmMinX, EmMinY, EmMaxX, EmMaxY;
        float ScreenMinX, ScreenMinY, ScreenMaxX, ScreenMaxY;
        float Jac00, Jac01, Jac10, Jac11;
        float BandScaleX, BandScaleY, BandOffsetX, BandOffsetY;
        uint32_t GlyphLocX, GlyphLocY;
        uint32_t BandMaxWord;
        float ColorR, ColorG, ColorB, ColorA;
    };
    static_assert(sizeof(ImSlugGlyphInstance) == 92, "frozen M2 instance layout (shaders + vertex input)");
    static_assert(offsetof(ImSlugGlyphInstance, ScreenMinX) == 16, "frozen M2 instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, Jac00) == 32, "frozen M2 instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, BandScaleX) == 48, "frozen M2 instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, GlyphLocX) == 64, "frozen M2 instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, BandMaxWord) == 72, "frozen M2 instance layout");
    static_assert(offsetof(ImSlugGlyphInstance, ColorR) == 76, "frozen M2 instance layout");

    // Fills one instance for `record` placed by an axis-aligned transform:
    //   screen (pixels, Y down) = (penX + emX * pixelsPerEm, penY - emY * pixelsPerEm)
    // where (penX, penY) is the pen position on the baseline. Color is straight
    // alpha. Empty glyphs (CurveCount == 0) are representable (coverage 0), but
    // callers normally skip them.
    void ImSlugBuildGlyphInstance(const ImSlugGlyphRecord* record, float penX, float penY, float pixelsPerEm, float r,
                                  float g, float b, float a, ImSlugGlyphInstance* outInstance);

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
        ImSlugBuildStatus_BandPoolLimit,   // pool height reached or row growth failed
        ImSlugBuildStatus_GlyphTooLarge,   // band block not representable: a ref
                                           // list longer than one row, or a
                                           // padded block > 65535 texels (u16
                                           // offsets) -- definitive fallback
    };
}  // namespace ImSlug

// Module APIs (the shared types above are visible to all of them).
#include "imslug_packer.h"
#include "imslug_glyphgen.h"
#include "imslug_refraster.h"
