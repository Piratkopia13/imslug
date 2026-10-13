// imslug — band-count strategy sweep (M4).
//
// The band count per glyph is the one tuning knob of the Slug data format:
// records are size-independent (one band layout serves 8..72 px), so the
// choice must balance the fragment shader's per-pixel reference-list work
// against the band pool footprint. This tool measures both, for a set of
// candidate functions, with the *real* generator (ImSlugGetGlyphOutlineQuads +
// ImSlugAssignBands) and a faithful model of the shader's inner loop:
//
//   * for every glyph and every candidate, bands are assigned with the
//     production rules (control-point bbox, epsilon, axis-parallel exclusion);
//   * for every size 8..72 px, the glyph's pixel-center grid (expanded by the
//     half-pixel dilation) is sampled: band index = clamp(int(em * scale +
//     offset), 0, bandMax) per axis, then the band's reference list is walked
//     exactly like the pixel shader: refs are sorted descending by max
//     x (horizontal) / max y (vertical) and the walk stops when the largest
//     control point is more than half a pixel behind the sample
//     ((refMax - sample) * ppem < -0.5). The number of refs visited before the
//     break is the per-pixel cost proxy;
//   * the band pool footprint is what ImSlugAllocBandBlock would reserve for
//     the whole font (headers + lists + the row-padding rule; R16G16_UINT =
//     4 bytes/texel).
//
// Usage (via scripts/bench.* or directly):
//   imslug_tests_band_sweep [--fonts roboto,inter,cjk] [--csv <file>]
//
// The chosen default and the decision are recorded in PLAN §11 and the
// imslug-glyph-format skill; this tool exists so the numbers can be
// re-measured after generator changes.

#include "imslug_internal.h"

#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef IMSLUG_CJK_TEST_FONT
#define IMSLUG_CJK_TEST_FONT "tests/fonts/cjk/NotoSansSC-subset.ttf"
#endif

using namespace ImSlug;

namespace
{
    // ---------------------------------------------------------------------
    // Candidate band-count strategies: (curveCount) -> band count per axis.
    // ---------------------------------------------------------------------
    struct Candidate
    {
        const char* Name;
        int (*Count)(int curveCount);
    };

    int ClampInt(int value, int lo, int hi)
    {
        return value < lo ? lo : (value > hi ? hi : value);
    }

    int Fixed8(int)
    {
        return 8;
    }
    int Sqrt2n4_16(int n)
    {
        return ClampInt((int)ceilf(sqrtf(2.0f * (float)n)), 4, 16);
    }
    int Sqrt2n4_32(int n)
    {
        return ClampInt((int)ceilf(sqrtf(2.0f * (float)n)), 4, 32);
    }
    int Sqrt2n4_64(int n)
    {
        return ClampInt((int)ceilf(sqrtf(2.0f * (float)n)), 4, 64);
    }
    int Sqrtn4_32(int n)
    {
        return ClampInt((int)ceilf(sqrtf((float)n)), 4, 32);
    }
    int TwoSqrtn4_32(int n)
    {
        return ClampInt((int)ceilf(2.0f * sqrtf((float)n)), 4, 32);
    }
    int TwoHalfSqrtn4_32(int n)
    {
        return ClampInt((int)ceilf(2.5f * sqrtf((float)n)), 4, 32);
    }
    int ThreeSqrtn4_32(int n)
    {
        return ClampInt((int)ceilf(3.0f * sqrtf((float)n)), 4, 32);
    }

    const Candidate kCandidates[] = {
        { "sqrt(2n) 4..16", Sqrt2n4_16 },      { "sqrt(2n) 4..32 (M1 default)", Sqrt2n4_32 },
        { "sqrt(2n) 4..64", Sqrt2n4_64 },      { "sqrt(n)  4..32", Sqrtn4_32 },
        { "2*sqrt(n) 4..32", TwoSqrtn4_32 },   { "2.5*sqrt(n) 4..32", TwoHalfSqrtn4_32 },
        { "3*sqrt(n) 4..32", ThreeSqrtn4_32 }, { "fixed 8", Fixed8 },
    };
    const int kCandidateCount = (int)(sizeof(kCandidates) / sizeof(kCandidates[0]));

    const int kSizeMin = 8;
    const int kSizeMax = 72;
    const int kSizeStep = 2;
    const int kSizeCount = (kSizeMax - kSizeMin) / kSizeStep + 1;

    struct GlyphData
    {
        std::vector<ImSlugQuad> Quads;
        float MinX, MinY, MaxX, MaxY;  // control-point bbox
    };

    struct FontData
    {
        const char* Name;
        ImSlugFont* Font;
        std::vector<GlyphData> Glyphs;
        int CurveTotal;
        float UnitsPerEm;
    };

    // ---------------------------------------------------------------------
    // One candidate's measurement over one font.
    // ---------------------------------------------------------------------
    struct Result
    {
        // refs/pixel statistics, per size (mean over all sampled pixels and p95
        // over the per-glyph means; the aggregate rows are over the used range)
        double MeanRefs[kSizeCount];
        double P95Refs[kSizeCount];
        double P99PixelRefs[kSizeCount];
        int MaxPixelRefs[kSizeCount];
        double MeanRefsSmall;    // sizes 8..16 (typical UI body text)
        double MeanRefsMid;      // sizes 18..32
        double MeanRefsLarge;    // sizes 34..72
        double P99PixelRefsAll;  // tail across all sizes (low-end GPU worst pixel)
        int MaxPixelRefsAll;
        double BandPoolKB;
        int WorstBandRefs;
        int WorstBandCurves;
    };

    // Emulates ImSlugAllocBandBlock's reservation for one glyph (no pool
    // writes): returns the padded texel count at the current cursor.
    int EmulateBandBlock(int& cursor, int bandMaxX, int bandMaxY, const int* counts)
    {
        const int headerCount = bandMaxY + 1 + bandMaxX + 1;
        if ((cursor & (kPoolWidth - 1)) + headerCount > kPoolWidth)
            cursor = ((cursor >> kPoolWidthLog2) + 1) << kPoolWidthLog2;
        const int glyphLocX = cursor & (kPoolWidth - 1);
        const int glyphLocY = cursor >> kPoolWidthLog2;
        cursor += headerCount;
        for (int i = 0; i < headerCount; ++i)
        {
            const int listLength = counts[i];
            if (listLength > 0 && ((cursor & (kPoolWidth - 1)) + listLength > kPoolWidth))
                cursor = ((cursor >> kPoolWidthLog2) + 1) << kPoolWidthLog2;
            cursor += listLength;
        }
        return cursor - (glyphLocY * kPoolWidth + glyphLocX);
    }

    void MeasureCandidate(const FontData& font, const Candidate& candidate, Result* out)
    {
        std::vector<int> counts;
        std::vector<int> refs;
        int cursor = 0;
        long long poolTexels = 0;
        out->WorstBandRefs = 0;
        out->WorstBandCurves = 0;

        // Per-size accumulators.
        std::vector<double> sumRefs(kSizeCount, 0.0);
        std::vector<long long> sumPixels(kSizeCount, 0);
        std::vector<std::vector<double>> perGlyph(kSizeCount);
        std::vector<std::vector<long long>> pixelHist(kSizeCount, std::vector<long long>(512, 0));

        float pixelsPerEmScale = font.UnitsPerEm;  // multiplied by size/(ascent-descent) below
        // Use the same scale the pipeline uses: PixelsPerEm at size S.
        ImSlugFontMetrics metrics;
        ImSlugGetFontMetrics(font.Font, 1.0f, &metrics);
        pixelsPerEmScale = metrics.PixelsPerEm;  // pixels per em at size 1 => * size

        for (const GlyphData& glyph : font.Glyphs)
        {
            const int curveCount = (int)glyph.Quads.size();
            const int bandCount = curveCount > 0 ? candidate.Count(curveCount) : 1;
            const int bandMaxX = bandCount - 1;
            const int bandMaxY = bandCount - 1;
            const int headerCount = (bandMaxY + 1) + (bandMaxX + 1);
            counts.assign((size_t)headerCount, 0);
            const int required = curveCount * headerCount + 16;
            refs.assign((size_t)required, 0);
            int requiredRefs = 0;
            if (!ImSlugAssignBands(glyph.Quads.data(), curveCount, bandMaxX, bandMaxY, glyph.MinX, glyph.MinY,
                                   glyph.MaxX, glyph.MaxY, counts.data(), refs.data(), (int)refs.size(), &requiredRefs))
                continue;

            for (int band = 0; band < headerCount; ++band)
                if (counts[band] > out->WorstBandRefs)
                {
                    out->WorstBandRefs = counts[band];
                    out->WorstBandCurves = curveCount;
                }
            poolTexels += EmulateBandBlock(cursor, bandMaxX, bandMaxY, counts.data());

            const float extentX = glyph.MaxX - glyph.MinX;
            const float extentY = glyph.MaxY - glyph.MinY;
            const float bandScaleX = extentX > 0.0f ? (float)(bandMaxX + 1) / extentX : 0.0f;
            const float bandScaleY = extentY > 0.0f ? (float)(bandMaxY + 1) / extentY : 0.0f;
            const float bandOffsetX = -glyph.MinX * bandScaleX;
            const float bandOffsetY = -glyph.MinY * bandScaleY;

            // Per-band ref ranges in the flattened list (sorted per band).
            const int hBands = bandMaxY + 1;
            std::vector<int> bandStart((size_t)headerCount + 1, 0);
            for (int band = 0; band < headerCount; ++band)
                bandStart[band + 1] = bandStart[band] + counts[band];

            // Per-band max coordinate (the sort key) for the early-out model.
            std::vector<float> refMaxX((size_t)requiredRefs, 0.0f);
            std::vector<float> refMaxY((size_t)requiredRefs, 0.0f);
            for (int band = 0; band < headerCount; ++band)
            {
                const bool horizontal = band < hBands;
                for (int r = bandStart[band]; r < bandStart[band + 1]; ++r)
                {
                    const ImSlugQuad& quad = glyph.Quads[(size_t)refs[r]];
                    const float maxX = quad.X1 > quad.X3 ? (quad.X1 > quad.X2 ? quad.X1 : quad.X2)
                                                         : (quad.X3 > quad.X2 ? quad.X3 : quad.X2);
                    const float maxY = quad.Y1 > quad.Y3 ? (quad.Y1 > quad.Y2 ? quad.Y1 : quad.Y2)
                                                         : (quad.Y3 > quad.Y2 ? quad.Y3 : quad.Y2);
                    refMaxX[r] = maxX;
                    refMaxY[r] = maxY;
                    (void)horizontal;
                }
            }

            for (int sizeIndex = 0; sizeIndex < kSizeCount; ++sizeIndex)
            {
                const float size = (float)(kSizeMin + sizeIndex * kSizeStep);
                const float pixelsPerEm = pixelsPerEmScale * size;

                const int x0 = (int)floorf(glyph.MinX * pixelsPerEm) - 1;
                const int x1 = (int)ceilf(glyph.MaxX * pixelsPerEm) + 1;
                const int y0 = (int)floorf(glyph.MinY * pixelsPerEm) - 1;
                const int y1 = (int)ceilf(glyph.MaxY * pixelsPerEm) + 1;
                if (x1 <= x0 || y1 <= y0)
                    continue;

                long long refsVisited = 0;
                long long pixels = 0;
                for (int py = y0; py <= y1; ++py)
                {
                    const float sampleY = ((float)py + 0.5f) / pixelsPerEm;
                    for (int px = x0; px <= x1; ++px)
                    {
                        const float sampleX = ((float)px + 0.5f) / pixelsPerEm;
                        long long pixelCost = 0;
                        // Horizontal band: indexed by y, walks by max x.
                        if (bandScaleY > 0.0f)
                        {
                            const int band = ClampInt((int)(sampleY * bandScaleY + bandOffsetY), 0, bandMaxY);
                            for (int r = bandStart[band]; r < bandStart[band + 1]; ++r)
                            {
                                pixelCost++;
                                if ((refMaxX[r] - sampleX) * pixelsPerEm < -0.5f)
                                    break;
                            }
                        }
                        // Vertical band: indexed by x, walks by max y.
                        if (bandScaleX > 0.0f)
                        {
                            const int band = ClampInt((int)(sampleX * bandScaleX + bandOffsetX), 0, bandMaxX);
                            const int listIndex = hBands + band;
                            for (int r = bandStart[listIndex]; r < bandStart[listIndex + 1]; ++r)
                            {
                                pixelCost++;
                                if ((refMaxY[r] - sampleY) * pixelsPerEm < -0.5f)
                                    break;
                            }
                        }
                        refsVisited += pixelCost;
                        pixelHist[sizeIndex][pixelCost < 511 ? (size_t)pixelCost : 511]++;
                        pixels++;
                    }
                }
                if (pixels > 0)
                {
                    sumRefs[sizeIndex] += (double)refsVisited;
                    sumPixels[sizeIndex] += pixels;
                    perGlyph[sizeIndex].push_back((double)refsVisited / (double)pixels);
                }
            }
        }

        out->MeanRefsSmall = 0.0;
        out->MeanRefsMid = 0.0;
        out->MeanRefsLarge = 0.0;
        out->P99PixelRefsAll = 0.0;
        out->MaxPixelRefsAll = 0;
        int smallCount = 0, midCount = 0, largeCount = 0;
        for (int sizeIndex = 0; sizeIndex < kSizeCount; ++sizeIndex)
        {
            const int size = kSizeMin + sizeIndex * kSizeStep;
            out->MeanRefs[sizeIndex] =
                sumPixels[sizeIndex] > 0 ? sumRefs[sizeIndex] / (double)sumPixels[sizeIndex] : 0.0;
            std::vector<double>& values = perGlyph[sizeIndex];
            out->P95Refs[sizeIndex] = 0.0;
            if (!values.empty())
            {
                std::sort(values.begin(), values.end());
                out->P95Refs[sizeIndex] = values[(size_t)((values.size() - 1) * 0.95)];
            }
            // Per-pixel tail from the histogram (the low-end GPU worst case).
            double p99 = 0.0;
            int maxRefs = 0;
            if (sumPixels[sizeIndex] > 0)
            {
                const long long threshold = (long long)((double)sumPixels[sizeIndex] * 0.99);
                long long seen = 0;
                for (int bucket = 0; bucket < 512; ++bucket)
                {
                    seen += pixelHist[sizeIndex][(size_t)bucket];
                    if (pixelHist[sizeIndex][(size_t)bucket] > 0)
                        maxRefs = bucket;
                    if (seen >= threshold && p99 == 0.0)
                        p99 = (double)bucket;
                }
            }
            out->P99PixelRefs[sizeIndex] = p99;
            out->MaxPixelRefs[sizeIndex] = maxRefs;
            if (p99 > out->P99PixelRefsAll)
                out->P99PixelRefsAll = p99;
            if (maxRefs > out->MaxPixelRefsAll)
                out->MaxPixelRefsAll = maxRefs;
            if (size <= 16)
            {
                out->MeanRefsSmall += out->MeanRefs[sizeIndex];
                smallCount++;
            }
            else if (size <= 32)
            {
                out->MeanRefsMid += out->MeanRefs[sizeIndex];
                midCount++;
            }
            else
            {
                out->MeanRefsLarge += out->MeanRefs[sizeIndex];
                largeCount++;
            }
        }
        out->MeanRefsSmall /= (double)(smallCount > 0 ? smallCount : 1);
        out->MeanRefsMid /= (double)(midCount > 0 ? midCount : 1);
        out->MeanRefsLarge /= (double)(largeCount > 0 ? largeCount : 1);
        out->BandPoolKB = (double)poolTexels * 4.0 / 1024.0;
    }

    bool LoadFont(const char* name, const char* path, FontData* out)
    {
        int size = 0;
        unsigned char* data = imslug_test::LoadFile(path, &size);
        if (data == nullptr)
            return false;
        ImSlugFont* font = ImSlugCreateFontFromMemory(data, size, 0, true);
        std::free(data);
        if (font == nullptr)
            return false;
        out->Name = name;
        out->Font = font;
        out->CurveTotal = 0;

        // Latin sample: the printable ASCII range plus Latin-1 (the ranges a UI
        // font actually uses); the CJK subset contributes its whole range.
        unsigned int first = 0x20, last = 0x17F;
        if (strcmp(name, "cjk") == 0)
        {
            first = 0x4E00;
            last = 0x4EFF;
        }
        for (unsigned int codepoint = first; codepoint <= last; ++codepoint)
        {
            const int glyphIndex = ImSlugGetGlyphIndex(font, codepoint);
            if (glyphIndex == 0)
                continue;
            ImSlugQuad scratch[512];
            int quadCount = 0;
            if (!ImSlugGetGlyphOutlineQuads(font, glyphIndex, scratch, 512, &quadCount))
                continue;
            if (quadCount <= 0)
                continue;
            GlyphData glyph;
            glyph.Quads.assign(scratch, scratch + quadCount);
            glyph.MinX = glyph.Quads[0].X1;
            glyph.MaxX = glyph.Quads[0].X1;
            glyph.MinY = glyph.Quads[0].Y1;
            glyph.MaxY = glyph.Quads[0].Y1;
            for (const ImSlugQuad& quad : glyph.Quads)
            {
                const float xs[3] = { quad.X1, quad.X2, quad.X3 };
                const float ys[3] = { quad.Y1, quad.Y2, quad.Y3 };
                for (int i = 0; i < 3; ++i)
                {
                    glyph.MinX = glyph.MinX < xs[i] ? glyph.MinX : xs[i];
                    glyph.MaxX = glyph.MaxX > xs[i] ? glyph.MaxX : xs[i];
                    glyph.MinY = glyph.MinY < ys[i] ? glyph.MinY : ys[i];
                    glyph.MaxY = glyph.MaxY > ys[i] ? glyph.MaxY : ys[i];
                }
            }
            out->CurveTotal += quadCount;
            out->Glyphs.push_back(std::move(glyph));
        }
        ImSlugFontMetrics metrics;
        ImSlugGetFontMetrics(font, 1.0f, &metrics);
        out->UnitsPerEm = (float)metrics.UnitsPerEm;
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* csvPath = nullptr;
    bool wantRoboto = true, wantInter = true, wantCjk = true;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
            csvPath = argv[++i];
        else if (strcmp(argv[i], "--fonts") == 0 && i + 1 < argc)
        {
            wantRoboto = wantInter = wantCjk = false;
            const char* list = argv[++i];
            if (strstr(list, "roboto") != nullptr)
                wantRoboto = true;
            if (strstr(list, "inter") != nullptr)
                wantInter = true;
            if (strstr(list, "cjk") != nullptr)
                wantCjk = true;
        }
    }

    std::printf("imslug band sweep: sizes %d..%d px, %d candidates\n", kSizeMin, kSizeMax, kCandidateCount);
    std::vector<FontData> fonts;
    FontData font;
    if (wantRoboto && LoadFont("roboto", IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), &font))
        fonts.push_back(std::move(font));
    if (wantInter && LoadFont("inter", IMSLUG_TEST_FONT_PATH("Inter-Regular.ttf"), &font))
        fonts.push_back(std::move(font));
    if (wantCjk && LoadFont("cjk", IMSLUG_CJK_TEST_FONT, &font))
        fonts.push_back(std::move(font));
    if (fonts.empty())
    {
        std::printf("no fonts loaded (missing test fonts?)\n");
        return 1;
    }

    FILE* csv = csvPath != nullptr ? fopen(csvPath, "w") : nullptr;
    if (csv != nullptr)
        fprintf(csv, "font,candidate,size,mean_refs_per_pixel,p95_refs_per_glyph,p99_refs_per_pixel,max_refs_per_pixel,"
                     "band_pool_kb,worst_band_refs\n");

    for (const FontData& f : fonts)
    {
        std::printf("\nfont %s: %d glyphs, %d curves total\n", f.Name, (int)f.Glyphs.size(), f.CurveTotal);
        std::printf("  %-28s %7s %7s %7s %7s %7s %7s %7s %7s\n", "candidate", "8-16px", "18-32", "34-72", "p99px",
                    "maxpx", "poolKB", "worstRef", "worstN");
        for (const Candidate& candidate : kCandidates)
        {
            Result result;
            MeasureCandidate(f, candidate, &result);
            std::printf("  %-28s %7.2f %7.2f %7.2f %7.0f %7d %7.1f %8d %8d\n", candidate.Name, result.MeanRefsSmall,
                        result.MeanRefsMid, result.MeanRefsLarge, result.P99PixelRefsAll, result.MaxPixelRefsAll,
                        result.BandPoolKB, result.WorstBandRefs, result.WorstBandCurves);
            if (csv != nullptr)
                for (int sizeIndex = 0; sizeIndex < kSizeCount; ++sizeIndex)
                    fprintf(csv, "%s,%s,%d,%.4f,%.4f,%.4f,%d,%.1f,%d\n", f.Name, candidate.Name,
                            kSizeMin + sizeIndex * kSizeStep, result.MeanRefs[sizeIndex], result.P95Refs[sizeIndex],
                            result.P99PixelRefs[sizeIndex], result.MaxPixelRefs[sizeIndex], result.BandPoolKB,
                            result.WorstBandRefs);
        }
    }
    if (csv != nullptr)
        fclose(csv);

    for (FontData& f : fonts)
        ImSlugDestroyFont(f.Font);
    return 0;
}
