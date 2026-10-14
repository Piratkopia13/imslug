// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_refraster.h — CPU coverage rasterizer: the ground truth/arbiter for
// the golden tests. It mirrors the reference pixel shader
// (EricLengyel/Slug, SlugPixelShader.hlsl, Copyright 2017 Eric Lengyel,
// MIT OR Apache-2.0) function by function — CalcRootCode, SolveHorizPoly,
// SolveVertPoly, CalcBandLoc, CalcCoverage, the banded curve walk and its
// sorted early-outs — with pixelsPerEm supplied explicitly because a CPU
// sample has no derivatives.
//
// The pure math functions are exported so unit tests can pin the shader
// contract (root eligibility table, solver branch, coverage combine) without
// pools. Band indexes/offsets follow imslug_internal.h exactly.
//
// Deviations from the HLSL, both deliberate and documented in the .cpp:
//   * the degenerate linear solve avoids the division the shader would turn
//     into inf/NaN (that guard expression is adapted from Slughorn, MIT; the
//     rest of this file is a port of the reference shader — NOTICE section 4);
//   * frac() is the IEEE frac (x - floor(x)).

#pragma once

#include "imslug_internal.h"

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Pure shader math (SlugPixelShader.hlsl)
    // ---------------------------------------------------------------------
    // Root eligibility code for the sample-relative control points y1, y2, y3:
    // signs of the three y values select one of 8 cases of the reference table
    // (0x2E74 >> shift) & 0x0101. Bit 0 = root 1 eligible, bit 8 = root 2.
    uint32_t ImSlugCalcRootCode(float y1, float y2, float y3);

    // t-values (well, sample-relative x/y values where C(t) crosses 0), in the
    // shader's convention: p12 = (x1, y1, x2, y2), p3 = (x3, y3), relative to
    // the sample. Returns the two coordinates; the "nearly linear" branch
    // (|a| < 1/65536) returns a double root.
    void ImSlugSolveHorizPoly(const float p12[4], const float p3[2], float outR[2]);
    void ImSlugSolveVertPoly(const float p12[4], const float p3[2], float outR[2]);

    // Final combine: max of the weighted mean and the weaker of the two
    // coverages; then the fill rule (nonzero saturate, even-odd for bit 12 of
    // `flags`) — exactly CalcCoverage() in the reference shader.
    float ImSlugCalcCoverage(float xcov, float ycov, float xwgt, float ywgt, uint32_t flags);

    // ---------------------------------------------------------------------
    // Pool-backed samples
    // ---------------------------------------------------------------------
    // Mirrors SlugRender(): clamps band indexes, fetches the band headers and
    // reference lists out of the packed pools, walks the curves with the sorted
    // early-outs, combines with ImSlugCalcCoverage. `flags` for the coverage
    // step comes from glyph->Flags (EvenOdd bit).
    float ImSlugRasterSample(const ImSlugPools* pools, const ImSlugGlyphRecord* glyph, float emX, float emY,
                             float pixelsPerEmX, float pixelsPerEmY);

    // Unbanded variant: walks the glyph's contiguous curve block (CurveLocX/Y +
    // CurveCount) with no bands at all. Same math per curve; for tests that
    // prove band assignment never changes the result.
    float ImSlugRasterSampleUnbanded(const ImSlugCurvePool* curves, const ImSlugGlyphRecord* glyph, float emX,
                                     float emY, float pixelsPerEmX, float pixelsPerEmY);

    // Fill width*height coverage floats: row 0 is the top row, samples at pixel
    // centers, (centerX0, centerY0) is the em coordinate of pixel (0,0)'s
    // center and em Y decreases by 1 / pixelsPerEm per row.
    void ImSlugRasterGlyphRect(const ImSlugPools* pools, const ImSlugGlyphRecord* glyph, float centerX0, float centerY0,
                               float pixelsPerEm, float* outCoverage, int width, int height);
}  // namespace ImSlug
