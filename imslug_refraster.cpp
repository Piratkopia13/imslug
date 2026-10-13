// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_refraster.cpp — CPU coverage rasterizer mirroring the reference pixel
// shader (EricLengyel/Slug, SlugPixelShader.hlsl, Copyright 2017 Eric Lengyel,
// MIT OR Apache-2.0 — see NOTICE). Function-for-function port:
//   CalcRootCode, SolveHorizPoly, SolveVertPoly, CalcBandLoc, CalcCoverage,
//   SlugRender's banded walk and sorted early-outs.
//
// The GPU gets pixelsPerEm from texcoord derivatives (fwidth); here the caller
// supplies them. Everything else is bit-identical in intent, including reading
// fp16 texels through the pool accessors (so quantization is part of the
// result), the u16 offsets and the row-wrap addressing.
//
// Deviations (both documented, numerically equivalent elsewhere):
//   * SolveHorizPoly/SolveVertPoly use Slughorn's guard for the exactly
//     degenerate linear case (b == 0), where the HLSL would divide by zero and
//     produce NaN. Slughorn (MIT) render.hpp _solveHorizPoly — see NOTICE.
//   * frac() is the IEEE definition x - floor(x).

#include "imslug_refraster.h"

#include <math.h>
#include <string.h>

namespace ImSlug
{
    // ---------------------------------------------------------------------
    // Pure shader math
    // ---------------------------------------------------------------------
    static uint32_t FloatBits(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    static float Saturate(float value)
    {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    static float Max3(float a, float b, float c)
    {
        return a > b ? (a > c ? a : c) : (b > c ? b : c);
    }

    static int ClampInt(int value, int minimum, int maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    uint32_t ImSlugCalcRootCode(float y1, float y2, float y3)
    {
        // Reference: uint i1 = asuint(y1) >> 31; i2 = asuint(y2) >> 30;
        // i3 = asuint(y3) >> 29; then shift = (i2 & 2) | (i1 & ~2); shift =
        // (i3 & 4) | (shift & ~4). The masks keep only the sign bits, so the
        // three shift bits are (sign y3, sign y2, sign y1); the exponent bits
        // aliased by the shifts are masked away.
        const uint32_t i1 = FloatBits(y1) >> 31;
        const uint32_t i2 = FloatBits(y2) >> 30;
        const uint32_t i3 = FloatBits(y3) >> 29;

        uint32_t shift = (i2 & 2u) | (i1 & ~2u);
        shift = (i3 & 4u) | (shift & ~4u);
        return (0x2E74u >> shift) & 0x0101u;
    }

    // Nearly-linear threshold from the reference solver (1 / 65536).
    static const float kSolverEpsilon = 1.0f / 65536.0f;

    void ImSlugSolveHorizPoly(const float p12[4], const float p3[2], float outR[2])
    {
        // Sample-relative quadratic y(t) = a t^2 - 2 b t + c with
        // a = p1.y - 2 p2.y + p3.y, b = p1.y - p2.y, c = p1.y = p12[1].
        const float ax = p12[0] - 2.0f * p12[2] + p3[0];
        const float ay = p12[1] - 2.0f * p12[3] + p3[1];
        const float bx = p12[0] - p12[2];
        const float by = p12[1] - p12[3];

        float t1, t2;
        if (fabsf(ay) < kSolverEpsilon)
        {
            // Nearly linear: solve -2 b t + c = 0 (the shader overwrites t1/t2
            // here; the b == 0 guard avoids the shader's 0/0 NaN).
            const float t = (fabsf(by) >= kSolverEpsilon) ? p12[1] * (0.5f / by) : 0.0f;
            t1 = t;
            t2 = t;
        }
        else
        {
            const float ra = 1.0f / ay;
            const float d = sqrtf(fmaxf(by * by - ay * p12[1], 0.0f));
            t1 = (by - d) * ra;
            t2 = (by + d) * ra;
        }

        // The x coordinates where C(t) crosses y = 0.
        outR[0] = (ax * t1 - bx * 2.0f) * t1 + p12[0];
        outR[1] = (ax * t2 - bx * 2.0f) * t2 + p12[0];
    }

    void ImSlugSolveVertPoly(const float p12[4], const float p3[2], float outR[2])
    {
        const float ax = p12[0] - 2.0f * p12[2] + p3[0];
        const float ay = p12[1] - 2.0f * p12[3] + p3[1];
        const float bx = p12[0] - p12[2];
        const float by = p12[1] - p12[3];

        float t1, t2;
        if (fabsf(ax) < kSolverEpsilon)
        {
            const float t = (fabsf(bx) >= kSolverEpsilon) ? p12[0] * (0.5f / bx) : 0.0f;
            t1 = t;
            t2 = t;
        }
        else
        {
            const float ra = 1.0f / ax;
            const float d = sqrtf(fmaxf(bx * bx - ax * p12[0], 0.0f));
            t1 = (bx - d) * ra;
            t2 = (bx + d) * ra;
        }

        // The y coordinates where C(t) crosses x = 0.
        outR[0] = (ay * t1 - by * 2.0f) * t1 + p12[1];
        outR[1] = (ay * t2 - by * 2.0f) * t2 + p12[1];
    }

    float ImSlugCalcCoverage(float xcov, float ycov, float xwgt, float ywgt, uint32_t glyphFlags)
    {
        // Reference CalcCoverage(): weighted mean vs the weaker ray's coverage.
        float coverage = fmaxf(fabsf(xcov * xwgt + ycov * ywgt) / fmaxf(xwgt + ywgt, kSolverEpsilon),
                               fminf(fabsf(xcov), fabsf(ycov)));

        if ((glyphFlags & ImSlugGlyphFlags_EvenOdd) == 0)
        {
            coverage = Saturate(coverage);  // nonzero fill rule
        }
        else
        {
            const float halved = coverage * 0.5f;
            const float frac = halved - floorf(halved);   // HLSL frac
            coverage = 1.0f - fabsf(1.0f - frac * 2.0f);  // even-odd fill rule
        }
        return coverage;
    }

    // ---------------------------------------------------------------------
    // Pool reads (shared by both walks)
    // ---------------------------------------------------------------------
    // Curve texel pair at (locX, locY): (x1,y1,x2,y2) and (x3,y3,0,0), made
    // relative to the sample position like the shader does.
    static void FetchCurveRelative(const ImSlugCurvePool* curves, int locX, int locY, float sampleX, float sampleY,
                                   float p12[4], float p3[2])
    {
        const uint16_t* texel0 = ImSlugCurveTexelAt(curves, locX, locY);
        p12[0] = ImSlugHalfToFloat(texel0[0]) - sampleX;
        p12[1] = ImSlugHalfToFloat(texel0[1]) - sampleY;
        p12[2] = ImSlugHalfToFloat(texel0[2]) - sampleX;
        p12[3] = ImSlugHalfToFloat(texel0[3]) - sampleY;
        p3[0] = ImSlugHalfToFloat(texel0[4]) - sampleX;
        p3[1] = ImSlugHalfToFloat(texel0[5]) - sampleY;
    }

    static void AccumulateHorizontal(const float p12[4], const float p3[2], float pixelsPerEmX, float* xcov,
                                     float* xwgt)
    {
        const uint32_t code = ImSlugCalcRootCode(p12[1], p12[3], p3[1]);
        if (code == 0u)
            return;

        float r[2];
        ImSlugSolveHorizPoly(p12, p3, r);
        r[0] *= pixelsPerEmX;
        r[1] *= pixelsPerEmX;

        if ((code & 0x01u) != 0u)
        {
            *xcov += Saturate(r[0] + 0.5f);
            *xwgt = fmaxf(*xwgt, Saturate(1.0f - fabsf(r[0]) * 2.0f));
        }
        if ((code & 0x100u) != 0u)
        {
            *xcov -= Saturate(r[1] + 0.5f);
            *xwgt = fmaxf(*xwgt, Saturate(1.0f - fabsf(r[1]) * 2.0f));
        }
    }

    static void AccumulateVertical(const float p12[4], const float p3[2], float pixelsPerEmY, float* ycov, float* ywgt)
    {
        const uint32_t code = ImSlugCalcRootCode(p12[0], p12[2], p3[0]);
        if (code == 0u)
            return;

        float r[2];
        ImSlugSolveVertPoly(p12, p3, r);
        r[0] *= pixelsPerEmY;
        r[1] *= pixelsPerEmY;

        if ((code & 0x01u) != 0u)
        {
            *ycov -= Saturate(r[0] + 0.5f);
            *ywgt = fmaxf(*ywgt, Saturate(1.0f - fabsf(r[0]) * 2.0f));
        }
        if ((code & 0x100u) != 0u)
        {
            *ycov += Saturate(r[1] + 0.5f);
            *ywgt = fmaxf(*ywgt, Saturate(1.0f - fabsf(r[1]) * 2.0f));
        }
    }

    // ---------------------------------------------------------------------
    // Banded sample (SlugRender)
    // ---------------------------------------------------------------------
    float ImSlugRasterSample(const ImSlugPools* pools, const ImSlugGlyphRecord* glyph, float emX, float emY,
                             float pixelsPerEmX, float pixelsPerEmY)
    {
        IMSLUG_ASSERT(pools != nullptr && glyph != nullptr);
        const ImSlugCurvePool* curves = &pools->Curves;
        const ImSlugBandPool* bands = &pools->Bands;

        // int2 bandIndex = clamp(int2(renderCoord * bandTransform.xy +
        // bandTransform.zw), 0, bandMax). int() truncates toward zero like the
        // HLSL cast; the clamp makes that equivalent to floor at the low end.
        const int bandX = ClampInt((int)(emX * glyph->BandScaleX + glyph->BandOffsetX), 0, glyph->BandMaxX);
        const int bandY = ClampInt((int)(emY * glyph->BandScaleY + glyph->BandOffsetY), 0, glyph->BandMaxY);

        float xcov = 0.0f;
        float xwgt = 0.0f;

        // Horizontal band header at (glyphLoc.x + bandIndex.y, glyphLoc.y).
        const uint16_t* hHeader = ImSlugBandTexelAt(bands, glyph->BandLocX + bandY, glyph->BandLocY);
        const int hCount = (int)hHeader[0];
        int hListX = 0, hListY = 0;
        ImSlugCalcBandLoc(glyph->BandLocX, glyph->BandLocY, hHeader[1], &hListX, &hListY);
        for (int curveIndex = 0; curveIndex < hCount; ++curveIndex)
        {
            const uint16_t* ref = ImSlugBandTexelAt(bands, hListX + curveIndex, hListY);
            float p12[4], p3[2];
            FetchCurveRelative(curves, ref[0], ref[1], emX, emY, p12, p3);

            // Curves are sorted descending by max x: once the largest control
            // point falls more than half a pixel left of the sample, no
            // remaining curve can influence the result.
            if (Max3(p12[0], p12[2], p3[0]) * pixelsPerEmX < -0.5f)
                break;
            AccumulateHorizontal(p12, p3, pixelsPerEmX, &xcov, &xwgt);
        }

        float ycov = 0.0f;
        float ywgt = 0.0f;

        // Vertical band header follows all horizontal ones.
        const uint16_t* vHeader =
            ImSlugBandTexelAt(bands, glyph->BandLocX + glyph->BandMaxY + 1 + bandX, glyph->BandLocY);
        const int vCount = (int)vHeader[0];
        int vListX = 0, vListY = 0;
        ImSlugCalcBandLoc(glyph->BandLocX, glyph->BandLocY, vHeader[1], &vListX, &vListY);
        for (int curveIndex = 0; curveIndex < vCount; ++curveIndex)
        {
            const uint16_t* ref = ImSlugBandTexelAt(bands, vListX + curveIndex, vListY);
            float p12[4], p3[2];
            FetchCurveRelative(curves, ref[0], ref[1], emX, emY, p12, p3);

            if (Max3(p12[1], p12[3], p3[1]) * pixelsPerEmY < -0.5f)
                break;
            AccumulateVertical(p12, p3, pixelsPerEmY, &ycov, &ywgt);
        }

        return ImSlugCalcCoverage(xcov, ycov, xwgt, ywgt, glyph->Flags);
    }

    // ---------------------------------------------------------------------
    // Unbanded sample (tests / debugging)
    // ---------------------------------------------------------------------
    static void CurveLocFromRecord(const ImSlugGlyphRecord* glyph, int curveIndex, int* outX, int* outY)
    {
        const int linear = glyph->CurveLocY * kPoolWidth + glyph->CurveLocX + curveIndex * 2;
        *outX = linear & (kPoolWidth - 1);
        *outY = linear >> kPoolWidthLog2;
    }

    float ImSlugRasterSampleUnbanded(const ImSlugCurvePool* curves, const ImSlugGlyphRecord* glyph, float emX,
                                     float emY, float pixelsPerEmX, float pixelsPerEmY)
    {
        IMSLUG_ASSERT(curves != nullptr && glyph != nullptr);

        float xcov = 0.0f, xwgt = 0.0f;
        float ycov = 0.0f, ywgt = 0.0f;
        for (int curveIndex = 0; curveIndex < (int)glyph->CurveCount; ++curveIndex)
        {
            int locX = 0, locY = 0;
            CurveLocFromRecord(glyph, curveIndex, &locX, &locY);
            float p12[4], p3[2];
            FetchCurveRelative(curves, locX, locY, emX, emY, p12, p3);
            AccumulateHorizontal(p12, p3, pixelsPerEmX, &xcov, &xwgt);
            AccumulateVertical(p12, p3, pixelsPerEmY, &ycov, &ywgt);
        }
        return ImSlugCalcCoverage(xcov, ycov, xwgt, ywgt, glyph->Flags);
    }

    // ---------------------------------------------------------------------
    // Rect fill
    // ---------------------------------------------------------------------
    void ImSlugRasterGlyphRect(const ImSlugPools* pools, const ImSlugGlyphRecord* glyph, float centerX0, float centerY0,
                               float pixelsPerEm, float* outCoverage, int width, int height)
    {
        IMSLUG_ASSERT(pools != nullptr && glyph != nullptr && outCoverage != nullptr);
        IMSLUG_ASSERT(width >= 0 && height >= 0);
        const float inversePixelsPerEm = 1.0f / pixelsPerEm;
        for (int y = 0; y < height; ++y)
        {
            const float emY = centerY0 - (float)y * inversePixelsPerEm;
            float* row = outCoverage + (size_t)y * (size_t)width;
            for (int x = 0; x < width; ++x)
            {
                const float emX = centerX0 + (float)x * inversePixelsPerEm;
                row[x] = ImSlugRasterSample(pools, glyph, emX, emY, pixelsPerEm, pixelsPerEm);
            }
        }
    }
}  // namespace ImSlug
