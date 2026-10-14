// imslug M4 fuzz harness — fine-clip reconstruction robustness.
//
// Deterministic, seeded, GPU-free: builds randomized ImGui frames (random
// glyphs/strings/sizes/positions/clip rectangles, fractional scales, inverted
// and zero-size clips) through the real pre-pass and asserts:
//
//   1. no crash, no NaN/non-finite value in instances, batches or scissors;
//   2. the command structure stays valid: every Slug callback is followed by a
//      reset, no Slug-looking vertex is left in a geometry command, no run is
//      dropped (the fake ring is sized for the whole case);
//   3. decoded placement error against an independent ground truth (the M3
//      test font, the exact ImGui fine-clip math mirrored from imgui_draw.cpp
//      5809-6017, including the effective clip = draw-list clip ∩ fine clip)
//      stays inside documented bounds per clip category, and every instance
//      covers the quad ImGui drew;
//   4. with Config::EnableFineClipReconstruction off (the R3 fallback) every
//      instance stays *inside* the quad ImGui drew (so no ink can escape the
//      fine-clip rectangle), nothing is dropped in non-degenerate cases and no
//      placement is NaN.
//
// The two passes (reconstruction on/off) run the same deterministic case
// sequence, so the fallback is fuzzed exactly as hard as the primary path.
// `--iterations N` (default 3000), `--seed S`, `--only N --verbose` (single
// case diagnostics), `IMSLUG_FUZZ_DUMP=<case>` (dump one case of a full run,
// for order-dependent failures). The CI profile is 100000 cases
// (IMSLUG_FUZZ_ITERATIONS); every profile then runs the fast "high-slot"
// phase: ~4300 live slabs warm the encoded-UV quantization regime and re-run
// the cases that once failed there (see below).
//
// Bugs this harness found and now pins (seeded cases in parentheses):
//   * `kPlacementSlack` was 1.05 px but ImGui's integer glyph box can exceed
//     the control-point bbox by round(ascent) + ceil (= ~1.5 px), so real
//     glyphs were dropped by the placement sanity check (cases 135, 213, 337);
//   * a triangle with only some corners decoding as handles was left in the
//     geometry stream (atlas-garbage UVs) by degenerate fine clips; it is now
//     dropped with its quad tail (structure violations before the fix);
//   * high slab ids (thousands of bakes, ~2 ulp of the encoded UV per fraction
//     step) made sub-pixel slivers regress: a collapsed corner-fraction span
//     (all four samples bitwise equal) dropped the glyph, and sliver-scale
//     spans made the fitted affine map up to 3.2 px off. Fixed by routing
//     sliver-scale spans to the single-corner sample + baked ppem (kSpanReliable
//     0.02 -> 0.1; measured sliver max 2.71 px -> 0.31 px at 100k cases) and by
//     accepting a collapsed span on an axis whose quad still has extent
//     (cases 20033, 45549, 47496, 66672, 86675, 94792 - kept as the high-slot
//     regression phase, no warm-up dependency on the long profile).
//
// Deliberate exclusion: window clipping (the draw list is the background list,
// whose clip covers every fuzz position; M3's suite owns window-edge clipping).
// Text with '\n' is walked exactly like RenderText (x resets, y advances).

#include "imslug.h"

#include "imslug_imgui_adapter.h"
#include "imslug_internal.h"

#include "test_util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ImSlug;

namespace
{
    // ---------------------------------------------------------------------
    // Deterministic PRNG (xorshift64*) — same seed = same cases.
    // ---------------------------------------------------------------------
    struct Rng
    {
        uint64_t State;

        uint32_t Next()
        {
            State ^= State >> 12;
            State ^= State << 25;
            State ^= State >> 27;
            return (uint32_t)((State * 2685821657736338717ull) >> 32);
        }
        uint32_t RangeInt(uint32_t lo, uint32_t hi)  // inclusive
        {
            return lo + Next() % (hi - lo + 1);
        }
        float Unit() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); }
        float Range(float lo, float hi) { return lo + Unit() * (hi - lo); }
        bool Chance(float probability) { return Unit() < probability; }
    };

    // ---------------------------------------------------------------------
    // Fake backend (RenderIO) + the stock-backend hooks the pre-pass needs.
    // ---------------------------------------------------------------------
    const uint32_t kRingCapacity = 16384;
    ImSlugGlyphInstance g_Ring[kRingCapacity];
    uint32_t g_RingUsed = 0;
    uint32_t g_RingCapacity = kRingCapacity;
    uint32_t g_FrameIndex = 0;
    int g_DrawCalls = 0;
    long long g_BatchViolations = 0;
    long long g_CurrentCase = -1;  // for diagnostics
    bool g_Verbose = false;        // --verbose: dump the walk and draw data

    // Diagnostics: the first few resyncs and bad batches, so a single
    // occurrence in a 20k-case run is actionable instead of opaque.
    struct ResyncRecord
    {
        long long Case;
        unsigned int Codepoint;
        float Size;
        long long Skipped;
    };
    ResyncRecord g_ResyncLog[8];
    int g_ResyncLogCount = 0;

    struct BadBatchRecord
    {
        long long Case;
        float L, T, R, B;
        unsigned int Count;
        unsigned int Magic;
    };
    BadBatchRecord g_BadBatchLog[8];
    int g_BadBatchLogCount = 0;

    bool IsFiniteBatch(const ImSlugBatch* batch)
    {
        // An empty intersection (run entirely outside the command's clip) is
        // legal and expected for degenerate cases; the adapter guards the
        // scissor before recording. Only non-finite values are a bug here.
        return std::isfinite(batch->ClipMinX) && std::isfinite(batch->ClipMinY) && std::isfinite(batch->ClipMaxX) &&
               std::isfinite(batch->ClipMaxY);
    }

    uint32_t FakeGetFrameIndex()
    {
        return g_FrameIndex;
    }
    ImSlugGlyphInstance* FakeReserve(uint32_t count, uint32_t* outFirst)
    {
        if (count > g_RingCapacity - g_RingUsed)
            return nullptr;
        *outFirst = g_RingUsed;
        g_RingUsed += count;
        return g_Ring + (g_RingUsed - count);
    }
    void FakeDraw(const ImSlugBatch* batch)
    {
        g_DrawCalls++;
        if (batch->Magic != kBatchMagic || batch->InstanceCount == 0 || !IsFiniteBatch(batch))
        {
            g_BatchViolations++;
            if (g_BadBatchLogCount < 8)
            {
                BadBatchRecord& record = g_BadBatchLog[g_BadBatchLogCount++];
                record.Case = g_CurrentCase;
                record.L = batch->ClipMinX;
                record.T = batch->ClipMinY;
                record.R = batch->ClipMaxX;
                record.B = batch->ClipMaxY;
                record.Count = batch->InstanceCount;
                record.Magic = batch->Magic;
            }
        }
    }
    const ImSlugRenderIO g_FakeIO = { "fuzz", FakeGetFrameIndex, FakeReserve, FakeDraw };

    void FakeResetRenderState(const ImDrawList*, const ImDrawCmd*) {}

    void ResetFake()
    {
        g_RingUsed = 0;
        g_DrawCalls = 0;
    }

    // ---------------------------------------------------------------------
    // Case generation
    // ---------------------------------------------------------------------
    const unsigned int kCodepoints[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',  'L',  'M',   'N',
                                         'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y',  'Z',  'a',   'b',
                                         'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',  'n',  'o',   'p',
                                         'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0',  '1',  '2',   '3',
                                         '4', '5', '6', '7', '8', '9', ' ', ' ', '.', ',', ':',  ';',  '!',   '?',
                                         '(', ')', '-', '+', '=', '/', '%', '#', '@', '&', 0xE9, 0xF1, 0x2026 };

    struct LineCase
    {
        char Text[64];
        float Size;
        ImVec2 Pos;
        bool NoPixelSnap;
        bool HasClip;
        ImVec4 Clip;
        bool HasNewline;
        ImVec4 ListClip;  // draw-list clip captured at draw time (filled by RunCase)
    };

    struct Case
    {
        LineCase Lines[3];
        int LineCount;
        bool ShapeBetweenLines;
    };

    void GenerateCase(Rng& rng, Case* outCase)
    {
        outCase->LineCount = (int)rng.RangeInt(1, 3);
        outCase->ShapeBetweenLines = rng.Chance(0.25f);

        for (int line = 0; line < outCase->LineCount; ++line)
        {
            LineCase& out = outCase->Lines[line];
            const int length = (int)rng.RangeInt(1, 20);
            int at = 0;
            for (int i = 0; i < length && at < (int)sizeof(out.Text) - 4; ++i)
            {
                const unsigned int codepoint =
                    kCodepoints[rng.RangeInt(0, (unsigned int)(sizeof(kCodepoints) / sizeof(kCodepoints[0])) - 1)];
                at += ImTextCharToUtf8(out.Text + at, codepoint);
            }
            if (at == 0)
                out.Text[at++] = 'A';
            out.Text[at] = '\0';
            out.HasNewline = rng.Chance(0.08f);
            if (out.HasNewline && at < (int)sizeof(out.Text) - 2)
            {
                out.Text[at] = '\n';
                out.Text[at + 1] = '\0';
            }

            // Sizes: mostly normal UI sizes, some tiny/huge/fractional.
            const float sizeKind = rng.Unit();
            if (sizeKind < 0.1f)
                out.Size = rng.Range(4.0f, 8.0f);
            else if (sizeKind < 0.9f)
                out.Size = rng.Range(9.0f, 40.0f);
            else
                out.Size = rng.Range(40.0f, 96.0f);

            const float baseX = rng.Range(-10.0f, 300.0f);
            const float baseY = rng.Range(-10.0f, 200.0f);
            out.Pos = ImVec2(rng.Chance(0.5f) ? floorf(baseX) : baseX, rng.Chance(0.5f) ? floorf(baseY) : baseY);
            out.NoPixelSnap = rng.Chance(0.2f);

            // Fine-clip variety: none / a few sides / all sides / tiny /
            // zero-size / inverted / huge / fractional coordinates.
            out.HasClip = rng.Chance(0.8f);
            const float roughWidth = (float)length * out.Size * 0.5f;
            const float roughHeight = out.Size * 1.4f;
            switch (out.HasClip ? (int)rng.RangeInt(0, 9) : 9)
            {
            case 0:
            case 1:  // one side
            {
                const float inset = rng.Range(0.5f, out.Size * 1.2f);
                switch (rng.RangeInt(0, 3))
                {
                case 0:
                    out.Clip = ImVec4(out.Pos.x + inset, out.Pos.y - out.Size, out.Pos.x + roughWidth,
                                      out.Pos.y + roughHeight);
                    break;
                case 1:
                    out.Clip = ImVec4(out.Pos.x - out.Size, out.Pos.y + inset, out.Pos.x + roughWidth,
                                      out.Pos.y + roughHeight);
                    break;
                case 2:
                    out.Clip = ImVec4(out.Pos.x - out.Size, out.Pos.y - out.Size, out.Pos.x + roughWidth - inset,
                                      out.Pos.y + roughHeight);
                    break;
                default:
                    out.Clip = ImVec4(out.Pos.x - out.Size, out.Pos.y - out.Size, out.Pos.x + roughWidth,
                                      out.Pos.y + roughHeight - inset);
                    break;
                }
                break;
            }
            case 2:  // two/three sides
            {
                const float insetA = rng.Range(0.5f, out.Size);
                const float insetB = rng.Range(0.5f, out.Size);
                out.Clip = ImVec4(out.Pos.x + insetA, out.Pos.y + insetB, out.Pos.x + roughWidth,
                                  out.Pos.y + roughHeight - rng.Range(0.0f, insetB));
                break;
            }
            case 3:  // all four sides, tight
            {
                const float left = rng.Range(0.5f, 3.0f);
                const float top = rng.Range(0.5f, 3.0f);
                out.Clip = ImVec4(out.Pos.x + left, out.Pos.y + top, out.Pos.x + left + rng.Range(1.0f, 6.0f),
                                  out.Pos.y + top + rng.Range(1.0f, 6.0f));
                break;
            }
            case 4:  // squeezed around a fractional point
            {
                const float cx = out.Pos.x + rng.Range(0.0f, out.Size);
                const float cy = out.Pos.y + rng.Range(0.0f, out.Size);
                const float w = rng.Range(0.25f, 3.0f);
                const float h = rng.Range(0.25f, 3.0f);
                out.Clip = ImVec4(cx, cy, cx + w, cy + h);
                break;
            }
            case 5:  // zero-size on one axis
            {
                const float cx = out.Pos.x + rng.Range(0.0f, roughWidth);
                out.Clip = ImVec4(cx, out.Pos.y - out.Size, cx, out.Pos.y + roughHeight);
                break;
            }
            case 6:  // zero-size on both axes
            {
                const float cx = out.Pos.x + rng.Range(0.0f, roughWidth);
                const float cy = out.Pos.y + rng.Range(0.0f, roughHeight);
                out.Clip = ImVec4(cx, cy, cx, cy);
                break;
            }
            case 7:  // inverted
            {
                const float x = out.Pos.x + rng.Range(-2.0f, roughWidth);
                const float y = out.Pos.y + rng.Range(-2.0f, roughHeight);
                out.Clip = ImVec4(x + rng.Range(1.0f, 12.0f), y + rng.Range(1.0f, 12.0f), x - rng.Range(1.0f, 12.0f),
                                  y - rng.Range(1.0f, 12.0f));
                break;
            }
            case 8:  // huge, fractional edges
            {
                out.Clip = ImVec4(out.Pos.x - 1000.0f - rng.Range(0.0f, 1.0f), out.Pos.y - 1000.0f, out.Pos.x + 2000.0f,
                                  out.Pos.y + 2000.0f + rng.Range(0.0f, 1.0f));
                break;
            }
            default:  // no clip
                out.HasClip = false;
                out.Clip = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
                break;
            }
        }
    }

    bool ClipIsDegenerate(const ImVec4& clip)
    {
        return clip.x >= clip.z || clip.y >= clip.w;
    }

    // The effective clip AddText passes to RenderText: the draw-list clip
    // intersected with the fine clip (imgui.cpp AddText).
    ImVec4 EffectiveClip(const LineCase& line)
    {
        ImVec4 clip = line.ListClip;
        if (line.HasClip)
        {
            clip.x = ImMax(clip.x, line.Clip.x);
            clip.y = ImMax(clip.y, line.Clip.y);
            clip.z = ImMin(clip.z, line.Clip.z);
            clip.w = ImMin(clip.w, line.Clip.w);
        }
        return clip;
    }

    // ---------------------------------------------------------------------
    // Ground truth
    // ---------------------------------------------------------------------
    ImSlugFont* g_TestFont = nullptr;

    struct ExpectedGlyph
    {
        const ImSlugGlyphRecord* Record;
        const ImFontGlyph* Glyph;
        float PixelsPerEm;                 // at the baked size
        float Scale;                       // size / baked->Size
        float PenX, PenY;                  // baseline pen (display coordinates)
        float QuadL, QuadT, QuadR, QuadB;  // the (possibly cropped) quad ImGui drew
        int Cropped[4];                    // L, T, R, B
        float SpanX, SpanY;                // visible fraction of the box per axis
        bool CropBoundary;                 // a crop fraction within 1e-3 of the 0/1
                                           // endpoints of the engine's threshold:
                                           // its decision can go either way, so
                                           // placement/containment are not asserted
    };

    enum ClipCategory
    {
        Category_Unclipped = 0,
        Category_OneSide,
        Category_Multi,
        Category_AllFour,
        Category_Count
    };

    // The accuracy contract is driven by the *visible fraction* (the fitted
    // slope's conditioning), not by how many sides were cropped.
    enum SpanClass
    {
        Span_Unclipped = 0,  // full box on both axes
        Span_Mild,           // min span >= 0.5
        Span_Moderate,       // 0.1 <= min span < 0.5
        Span_Sliver,         // min span < 0.1 (a few pixels of a glyph)
        SpanClass_Count
    };

    SpanClass SpanClassOf(const ExpectedGlyph& expected)
    {
        const float minSpan = expected.SpanX < expected.SpanY ? expected.SpanX : expected.SpanY;
        if (minSpan >= 0.999f)
            return Span_Unclipped;
        if (minSpan >= 0.5f)
            return Span_Mild;
        if (minSpan >= 0.1f)
            return Span_Moderate;
        return Span_Sliver;
    }

    bool GetGroundTruthGlyph(unsigned int codepoint, float bakedSize, ExpectedGlyph* out)
    {
        ImSlugFontMetrics fontMetrics;
        ImSlugGetFontMetrics(g_TestFont, bakedSize, &fontMetrics);
        out->PixelsPerEm = fontMetrics.PixelsPerEm;
        ImSlugPools* pools = ImSlugGetPools();
        if (pools == nullptr)
            return false;
        return ImSlugGetGlyphRecord(g_TestFont, pools, codepoint, &out->Record) == ImSlugBuildStatus_Ok;
    }

    // Mirrors ImFont::RenderText for one AddText call (imgui_draw.cpp
    // 5809-6017): the Y fast-forward, pixel-snap truncation, per-glyph scale,
    // the x-overlap draw test, the four crop branches, the y1 >= y2 skip and
    // the newline walk. Fills one ExpectedGlyph per glyph ImGui actually draws
    // (cropped quad included).
    //
    // ImGui always clips against the draw-list clip (intersected with the fine
    // clip when one is passed): the same crop branches apply to text at the
    // display edge, so the ground truth always uses the effective clip.
    int ComputeLineGroundTruth(const LineCase& line, ImFont* font, ExpectedGlyph* out, int capacity)
    {
        ImFontBaked* baked = font->GetFontBaked(line.Size);
        const float scale = line.Size / baked->Size;

        const ImVec4 clip = EffectiveClip(line);
        const float lineHeight = line.Size;

        float x = line.Pos.x;
        float y = line.Pos.y;
        if (!line.NoPixelSnap)
        {
            x = IM_TRUNC(x);
            y = IM_TRUNC(y);
        }
        const float originX = x;

        const char* s = line.Text;
        if (y > clip.w)
            return 0;  // RenderText: nothing is drawn
        while (y + lineHeight < clip.y && *s != '\0')
        {
            const char* lineEnd = (const char*)ImMemchr(s, '\n', strlen(s));
            s = lineEnd ? lineEnd + 1 : s + strlen(s);
            y += lineHeight;
        }
        if (*s == '\0')
            return 0;
        if (y > clip.w)
            return 0;

        const float penY = y + baked->Ascent * scale;
        int count = 0;
        for (; *s != '\0';)
        {
            unsigned int c = (unsigned int)(unsigned char)*s;
            if (c < 0x80)
                s += 1;
            else
                s += ImTextCharFromUtf8(&c, s, line.Text + strlen(line.Text));
            if (c == '\n')
            {
                x = originX;
                y += lineHeight;
                if (y > clip.w)
                    break;
                continue;
            }
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)c);
            const float charWidth = glyph->AdvanceX * scale;
            if (g_Verbose && glyph->Visible)
                std::printf("    [walk] U+%04X pen (%.4f %.4f) box x %.4f..%.4f y %.4f..%.4f adv %.4f\n",
                            (unsigned int)c, x, y, x + glyph->X0 * scale, x + glyph->X1 * scale, y + glyph->Y0 * scale,
                            y + glyph->Y1 * scale, charWidth);
            if (glyph->Visible)
            {
                float x1 = x + glyph->X0 * scale;
                float x2 = x + glyph->X1 * scale;
                float y1 = y + glyph->Y0 * scale;
                float y2 = y + glyph->Y1 * scale;
                const float originalWidth = x2 - x1;
                const float originalHeight = y2 - y1;
                const bool overlaps = (x1 <= clip.z && x2 >= clip.x);
                if (overlaps)
                {
                    int cropped[4] = { 0, 0, 0, 0 };
                    bool skipped = false;
                    bool expectedBoundary = false;
                    // The crop branches only run with the CpuFineClip flag: a
                    // fine clip was passed, so ImGui interpolates the UVs. The
                    // draw-list clip alone culls (overlap test) but never crops.
                    if (line.HasClip)
                    {
                        // The corner fraction is affine in the screen coordinate
                        // (frac(x) = (x - origX1) / originalWidth), so ImGui's
                        // interpolated UVs decode to exactly these values. The
                        // engine's "is this side cropped" test
                        // (kFracOpenEpsilon) is replicated so a sub-0.1% crop is
                        // not counted as a crop; reconstruction then places the
                        // full box, which still covers the quad within the slack.
                        const float epsilon = 1e-3f;
                        const float boundaryEpsilon = 2e-3f;
                        const float origX1 = x1;
                        const float origY1 = y1;
                        bool boundary = false;
                        if (x1 < clip.x)
                        {
                            const float fraction = (clip.x - origX1) / originalWidth;
                            cropped[0] = (fraction > epsilon && fraction < 1.0f - epsilon) ? 1 : 0;
                            if (fraction > epsilon && fraction < boundaryEpsilon)
                                boundary = true;
                            if (fraction > 1.0f - boundaryEpsilon && fraction < 1.0f - epsilon)
                                boundary = true;
                            x1 = clip.x;
                        }
                        if (y1 < clip.y)
                        {
                            const float fraction = (clip.y - origY1) / originalHeight;
                            cropped[1] = (fraction > epsilon && fraction < 1.0f - epsilon) ? 1 : 0;
                            if (fraction > epsilon && fraction < boundaryEpsilon)
                                boundary = true;
                            if (fraction > 1.0f - boundaryEpsilon && fraction < 1.0f - epsilon)
                                boundary = true;
                            y1 = clip.y;
                        }
                        if (x2 > clip.z)
                        {
                            const float fraction = (clip.z - origX1) / originalWidth;
                            cropped[2] = (fraction > epsilon && fraction < 1.0f - epsilon) ? 1 : 0;
                            if (fraction > epsilon && fraction < boundaryEpsilon)
                                boundary = true;
                            if (fraction > 1.0f - boundaryEpsilon && fraction < 1.0f - epsilon)
                                boundary = true;
                            x2 = clip.z;
                        }
                        if (y2 > clip.w)
                        {
                            const float fraction = (clip.w - origY1) / originalHeight;
                            cropped[3] = (fraction > epsilon && fraction < 1.0f - epsilon) ? 1 : 0;
                            if (fraction > epsilon && fraction < boundaryEpsilon)
                                boundary = true;
                            if (fraction > 1.0f - boundaryEpsilon && fraction < 1.0f - epsilon)
                                boundary = true;
                            y2 = clip.w;
                        }
                        expectedBoundary = boundary;
                        if (y1 >= y2)
                            skipped = true;
                        // A zero/inverted-width crop (only reachable with a
                        // degenerate or fully-collapsed effective clip) draws
                        // no visible ink; the engine may drop it
                        // (ReconstructPlacement rejects the collapsed span) and
                        // the ground truth does not expect an instance for it.
                        // Near-collapsed but nonzero spans are *not* skipped:
                        // the engine places them with the bounded "sliver"
                        // error, and the expectation must match that behavior.
                        if (x2 <= x1)
                            skipped = true;
                    }
                    if (!skipped)
                    {
                        if (count >= capacity)
                        {
                            TEST_CHECKF(false, "ground-truth capacity exceeded");
                            break;
                        }
                        ExpectedGlyph& expected = out[count];
                        memset(&expected, 0, sizeof(expected));
                        expected.Glyph = glyph;
                        expected.Scale = scale;
                        if (!GetGroundTruthGlyph(c, baked->Size, &expected))
                        {
                            TEST_CHECKF(false, "no ground-truth record for U+%04X", c);
                        }
                        else
                        {
                            expected.PenX = x;
                            expected.PenY = penY;
                            expected.QuadL = x1;
                            expected.QuadT = y1;
                            expected.QuadR = x2;
                            expected.QuadB = y2;
                            expected.SpanX = originalWidth > 0.0f ? (x2 - x1) / originalWidth : 1.0f;
                            expected.SpanY = originalHeight > 0.0f ? (y2 - y1) / originalHeight : 1.0f;
                            expected.CropBoundary = expectedBoundary;
                            for (int side = 0; side < 4; ++side)
                                expected.Cropped[side] = cropped[side];
                            count++;
                        }
                    }
                }
            }
            x += charWidth;
        }
        return count;
    }

    int CategoryOf(const ExpectedGlyph& expected)
    {
        const int cropped = expected.Cropped[0] + expected.Cropped[1] + expected.Cropped[2] + expected.Cropped[3];
        if (cropped == 0)
            return Category_Unclipped;
        if (cropped >= 4)
            return Category_AllFour;
        if (cropped == 1)
            return Category_OneSide;
        return Category_Multi;
    }

    // ---------------------------------------------------------------------
    // Statistics + bounds
    // ---------------------------------------------------------------------
    // Placement bounds by span class (px): measured maxima in the M4 report
    // (.agent-docs/reports/M4-<date>.md) with documented headroom. The
    // reconstruction is exact for unclipped quads; a cropped axis carries the
    // float rounding of ImGui's interpolation and the conditioning of the
    // remaining fraction span, so the bound loosens as the visible fraction
    // shrinks. A "sliver" is a few pixels of a glyph at a clip edge.
    //
    // The sliver bound is 1.0 px against a measured 0.31 px max (100k cases,
    // 2026-10-14) after the M4 fix that switches sliver-scale fraction spans to
    // the single-corner sample + baked ppem: before it the fitted map amplified
    // the encoded-UV quantization (2.71 px measured, and reconstruction could
    // drop the glyph outright). Keep the headroom: the class boundary sits at
    // the reliable-span threshold, where the fitted path still contributes.
    const double kBoundSpanUnclipped = 0.02;
    const double kBoundSpanMild = 0.1;
    const double kBoundSpanModerate = 0.5;
    const double kBoundSpanSliver = 1.0;

    struct Stats
    {
        long long Cases;
        long long GroundTruth;
        long long Instances;
        long long Missing;  // ImGui drew, we did not emit
        long long Unexpected;
        long long Drops;  // dropped/mismatched glyphs in paired lines
        long long ScissorViolations;
        long long StructureViolations;
        long long DegenerateCaseCount;
        long long StructureOnlyLines;
        long long StructureOnlyInstances;
        long long Resyncs;
        long long ResyncedInstances;
        long long UndecidedSlivers;
        long long BoundarySkips;
        double MaxPenError[SpanClass_Count];
        double SumPenError[SpanClass_Count];
        double MaxPpemError[SpanClass_Count];
        double MaxCoverShortfall;
        long long Count[SpanClass_Count];
        long long CropCategoryCount[Category_Count];
    };

    Stats g_Stats = {};

    const char* SpanClassName(int spanClass)
    {
        static const char* kNames[SpanClass_Count] = { "unclipped", "mild", "moderate", "sliver" };
        return kNames[spanClass];
    }

    const char* CategoryName(int category)
    {
        static const char* kNames[Category_Count] = { "unclipped", "one-side", "multi", "all-four" };
        return kNames[category];
    }

    double BoundOf(SpanClass spanClass)
    {
        switch (spanClass)
        {
        case Span_Unclipped:
            return kBoundSpanUnclipped;
        case Span_Mild:
            return kBoundSpanMild;
        case Span_Moderate:
            return kBoundSpanModerate;
        default:
            return kBoundSpanSliver;
        }
    }

    // A quad whose visible extent on an axis is below 1/50 px has no ink a user
    // could see, and ImGui's float UV interpolation can collapse its fraction
    // span so that the engine legitimately drops it (ReconstructPlacement
    // rejects a zero span). Such an expectation is "undecided": a placed
    // instance is checked normally; a missing one is not a failure.
    bool IsSubPixelSliver(const ExpectedGlyph& expected)
    {
        return (expected.QuadR - expected.QuadL) < 0.02f || (expected.QuadB - expected.QuadT) < 0.02f;
    }

    // ---------------------------------------------------------------------
    // Pass setup
    // ---------------------------------------------------------------------
    ImFont* g_SlugFont = nullptr;

    bool SetupPass(bool reconstruction, const char* fontPath)
    {
        ImSlugSetRenderIO(nullptr);
        ImSlug::Shutdown();
        if (ImGui::GetCurrentContext() != nullptr)
            ImGui::DestroyContext();

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(4096.0f, 3072.0f);  // background-list clip covers every fuzz position
        io.IniFilename = nullptr;

        Config config;
        config.EnableFineClipReconstruction = reconstruction;
        config.DebugDrawBounds = true;  // validate every placement too
        if (!ImSlug::Init(config))
        {
            TEST_CHECK(false);
            return false;
        }
        ImSlugSetRenderIO(&g_FakeIO);
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState = FakeResetRenderState;

        g_SlugFont = ImSlug::AddFontFromFileTTF(fontPath, 16.0f);
        TEST_CHECK(g_SlugFont != nullptr);
        return g_SlugFont != nullptr;
    }

    // ---------------------------------------------------------------------
    // One case: build the frame, run the pre-pass, dispatch, check.
    // ---------------------------------------------------------------------
    void RunCase(Case& testCase, bool reconstructionOn, uint32_t frameIndex, int iteration)
    {
        g_Stats.Cases++;
        g_CurrentCase = iteration;
        ImGui::NewFrame();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        for (int line = 0; line < testCase.LineCount; ++line)
        {
            LineCase& l = testCase.Lines[line];
            l.ListClip = ImVec4(drawList->GetClipRectMin().x, drawList->GetClipRectMin().y,
                                drawList->GetClipRectMax().x, drawList->GetClipRectMax().y);
            if (l.NoPixelSnap)
                drawList->Flags |= ImDrawListFlags_TextNoPixelSnap;
            const ImVec4* clip = l.HasClip ? &l.Clip : nullptr;
            drawList->AddText(g_SlugFont, l.Size, l.Pos, IM_COL32(255, 255, 255, 255), l.Text, nullptr, 0.0f, clip);
            drawList->Flags &= ~ImDrawListFlags_TextNoPixelSnap;
            if (testCase.ShapeBetweenLines)
                drawList->AddLine(ImVec2(l.Pos.x, l.Pos.y + l.Size + 1.0f),
                                  ImVec2(l.Pos.x + 80.0f, l.Pos.y + l.Size + 1.0f), IM_COL32(120, 120, 120, 255));
        }

        ResetFake();
        g_FrameIndex = frameIndex;
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        ImSlug::PreprocessDrawData(drawData);

        // Dispatch the callbacks the way a backend does (exercises the trampoline
        // and validates every batch's scissor through FakeDraw).
        if (drawData != nullptr)
            for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
            {
                const ImDrawList* list = drawData->CmdLists[listIndex];
                for (int i = 0; i < list->CmdBuffer.Size; ++i)
                {
                    const ImDrawCmd& cmd = list->CmdBuffer[i];
                    if (cmd.UserCallback == ImSlug::DrawCallback)
                        cmd.UserCallback(list, &cmd);
                }
            }
        if (g_BatchViolations != 0)
            TEST_CHECKF(false, "case %d: %lld invalid batch scissor/magic", iteration, g_BatchViolations);

        // ---- structure ------------------------------------------------------
        if (drawData != nullptr)
            for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
            {
                const ImDrawList* list = drawData->CmdLists[listIndex];
                for (int i = 0; i < list->CmdBuffer.Size; ++i)
                {
                    const ImDrawCmd& cmd = list->CmdBuffer[i];
                    if (cmd.UserCallback == ImSlug::DrawCallback)
                    {
                        if (i + 1 >= list->CmdBuffer.Size ||
                            list->CmdBuffer[i + 1].UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState)
                        {
                            g_Stats.StructureViolations++;
                            TEST_CHECKF(false, "case %d: callback %d not followed by a reset", iteration, i);
                        }
                    }
                    else if (cmd.UserCallback == nullptr)
                    {
                        for (unsigned int e = 0; e < cmd.ElemCount; ++e)
                        {
                            const unsigned int idx = list->IdxBuffer[cmd.IdxOffset + e] + cmd.VtxOffset;
                            const ImDrawVert& v = list->VtxBuffer[idx];
                            ImSlugDecodedVertex decoded;
                            if (ImSlugAdapterDecodeVertex(v.uv.x, v.uv.y, &decoded))
                            {
                                g_Stats.StructureViolations++;
                                TEST_CHECKF(false, "case %d: Slug vertex in geometry", iteration);
                                break;
                            }
                        }
                    }
                }
            }
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        if (stats.DroppedRuns != 0 || stats.BoundsMismatches != 0)
        {
            g_Stats.StructureViolations++;
            TEST_CHECKF(false, "case %d: %u dropped runs, %u bounds mismatches", iteration, stats.DroppedRuns,
                        stats.BoundsMismatches);
        }

        // ---- instance sanity (finite, non-degenerate) -----------------------
        for (uint32_t i = 0; i < g_RingUsed; ++i)
        {
            const ImSlugGlyphInstance& instance = g_Ring[i];
            const float values[] = { instance.EmMinX,      instance.EmMinY,     instance.EmMaxX,
                                     instance.EmMaxY,      instance.ScreenMinX, instance.ScreenMinY,
                                     instance.ScreenMaxX,  instance.ScreenMaxY, instance.Jac00,
                                     instance.Jac01,       instance.Jac10,      instance.Jac11,
                                     instance.BandScaleX,  instance.BandScaleY, instance.BandOffsetX,
                                     instance.BandOffsetY, instance.ColorR,     instance.ColorG,
                                     instance.ColorB,      instance.ColorA };
            for (float value : values)
            {
                if (!std::isfinite(value))
                {
                    TEST_CHECKF(false, "case %d: non-finite instance field", iteration);
                    return;
                }
            }
            if (!(instance.ScreenMaxX > instance.ScreenMinX) || !(instance.ScreenMaxY > instance.ScreenMinY))
            {
                TEST_CHECKF(false, "case %d: degenerate instance rect (%.2f %.2f %.2f %.2f)", iteration,
                            instance.ScreenMinX, instance.ScreenMinY, instance.ScreenMaxX, instance.ScreenMaxY);
                return;
            }
        }

        // ---- ground truth pairing, per line, in drawing order ----------------
        ExpectedGlyph expected[64];
        long long emitted = 0;
        bool anyDegenerateLine = false;
        for (int line = 0; line < testCase.LineCount; ++line)
        {
            const LineCase& l = testCase.Lines[line];
            const int expectedCount = ComputeLineGroundTruth(l, g_SlugFont, expected, 64);
            const bool clipDegenerate = ClipIsDegenerate(EffectiveClip(l));
            if (clipDegenerate)
            {
                g_Stats.DegenerateCaseCount++;
                anyDegenerateLine = true;
            }
            g_Stats.GroundTruth += expectedCount;
            if (clipDegenerate)
            {
                // Degenerate (zero-size/inverted/outside) effective clips make
                // ImGui itself interpolate NaN UVs and emit inverted quads; the
                // engine's contract there is only "never crash, never emit a
                // NaN, keep the structure valid, never leak Slug UVs into
                // geometry" (checked above). Placement pairing would be
                // meaningless, so the line is counted and skipped.
                g_Stats.StructureOnlyLines++;
                continue;
            }
            for (int g = 0; g < expectedCount; ++g)
            {
                const ExpectedGlyph& e = expected[g];
                // Strict order pairing (well-formed clips must place every
                // glyph); if a rare drop shifted the stream, resync once by
                // scanning a bounded window for the same em bbox so one drop
                // cannot cascade failures over the rest of the case.
                if (emitted < (long long)g_RingUsed)
                {
                    const ImSlugGlyphInstance& candidate = g_Ring[emitted];
                    const bool matches = candidate.EmMinX == e.Record->MinX && candidate.EmMinY == e.Record->MinY &&
                                         candidate.EmMaxX == e.Record->MaxX && candidate.EmMaxY == e.Record->MaxY;
                    if (!matches)
                    {
                        long long resync = -1;
                        for (long long probe = emitted + 1; probe < (long long)g_RingUsed && probe < emitted + 12;
                             ++probe)
                        {
                            const ImSlugGlyphInstance& candidate2 = g_Ring[probe];
                            if (candidate2.EmMinX == e.Record->MinX && candidate2.EmMinY == e.Record->MinY &&
                                candidate2.EmMaxX == e.Record->MaxX && candidate2.EmMaxY == e.Record->MaxY)
                            {
                                resync = probe;
                                break;
                            }
                        }
                        if (resync >= 0)
                        {
                            g_Stats.ResyncedInstances += resync - emitted;
                            g_Stats.Resyncs++;
                            if (g_ResyncLogCount < 8)
                            {
                                ResyncRecord& record = g_ResyncLog[g_ResyncLogCount++];
                                record.Case = iteration;
                                record.Codepoint = (unsigned int)e.Glyph->Codepoint;
                                record.Size = l.Size;
                                record.Skipped = resync - emitted;
                            }
                            emitted = resync;
                        }
                    }
                }
                if (emitted >= (long long)g_RingUsed)
                {
                    if (IsSubPixelSliver(e))
                    {
                        g_Stats.UndecidedSlivers++;
                        continue;
                    }
                    g_Stats.Missing++;
                    g_Stats.Drops++;
                    TEST_CHECKF(false, "case %d: missing instance for U+%04X (size %.2f)", iteration,
                                (unsigned int)e.Glyph->Codepoint, l.Size);
                    continue;
                }
                const ImSlugGlyphInstance& instance = g_Ring[emitted];
                const bool matches = instance.EmMinX == e.Record->MinX && instance.EmMinY == e.Record->MinY &&
                                     instance.EmMaxX == e.Record->MaxX && instance.EmMaxY == e.Record->MaxY;
                if (!matches && IsSubPixelSliver(e))
                {
                    // The engine dropped this invisible sliver: not a failure,
                    // and the next expectation pairs against the same instance.
                    g_Stats.UndecidedSlivers++;
                    continue;
                }
                emitted++;
                g_Stats.Instances++;
                const SpanClass spanClass = SpanClassOf(e);
                g_Stats.Count[spanClass]++;
                g_Stats.CropCategoryCount[CategoryOf(e)]++;

                if (!matches)
                {
                    g_Stats.Drops++;
                    TEST_CHECKF(false, "case %d: instance is not the expected glyph (U+%04X size %.2f, em bbox)",
                                iteration, (unsigned int)e.Glyph->Codepoint, l.Size);
                    continue;
                }

                const double ppem =
                    (double)(instance.ScreenMaxX - instance.ScreenMinX) / (double)(e.Record->MaxX - e.Record->MinX);
                const double expectedPpem = (double)e.PixelsPerEm * (double)e.Scale;
                const double impliedPenX = (double)instance.ScreenMinX - (double)e.Record->MinX * ppem;
                const double impliedPenY = (double)instance.ScreenMaxY + (double)e.Record->MinY * ppem;
                const double penErrorX = fabs(impliedPenX - (double)e.PenX);
                const double penErrorY = fabs(impliedPenY - (double)e.PenY);
                const double penError = penErrorX > penErrorY ? penErrorX : penErrorY;
                const double ppemError = fabs(ppem - expectedPpem) / expectedPpem;

                // Placement error is only meaningful where the placement is
                // supposed to be exact: always in the reconstruction pass, and
                // for unclipped glyphs in the fallback pass (cropped ones are
                // intentionally compressed into the quad). Boundary crops are
                // skipped: the engine's decoded fraction can fall on either
                // side of its own threshold.
                const bool placementMustBeExact = !e.CropBoundary && (reconstructionOn || spanClass == Span_Unclipped);
                if (placementMustBeExact)
                {
                    g_Stats.SumPenError[spanClass] += penError;
                    if (penError > g_Stats.MaxPenError[spanClass])
                        g_Stats.MaxPenError[spanClass] = penError;
                    if (ppemError > g_Stats.MaxPpemError[spanClass])
                        g_Stats.MaxPpemError[spanClass] = ppemError;

                    const double bound = BoundOf(spanClass);
                    if (penError > bound)
                        TEST_CHECKF(false, "case %d: %s pen error %.4f px > bound %.4f (U+%04X size %.2f)", iteration,
                                    SpanClassName(spanClass), penError, bound, (unsigned int)e.Glyph->Codepoint,
                                    l.Size);
                }

                // Coverage: the instance must cover the quad ImGui drew
                // (reconstruction) or fit inside it (fallback).
                if (e.CropBoundary)
                {
                    g_Stats.BoundarySkips++;
                }
                else if (reconstructionOn)
                {
                    // Covering means the instance's edges reach past the quad's
                    // (cropped) edges: positive = the instance falls short.
                    const double shortfallL = (double)instance.ScreenMinX - (double)e.QuadL;
                    const double shortfallR = (double)e.QuadR - (double)instance.ScreenMaxX;
                    const double shortfallT = (double)instance.ScreenMinY - (double)e.QuadT;
                    const double shortfallB = (double)e.QuadB - (double)instance.ScreenMaxY;
                    double maxShortfall = shortfallL;
                    if (shortfallR > maxShortfall)
                        maxShortfall = shortfallR;
                    if (shortfallT > maxShortfall)
                        maxShortfall = shortfallT;
                    if (shortfallB > maxShortfall)
                        maxShortfall = shortfallB;
                    if (maxShortfall > g_Stats.MaxCoverShortfall)
                        g_Stats.MaxCoverShortfall = maxShortfall;
                    // Bound: ImGui's integer glyph box (floor/ceil of the em
                    // bbox plus round(ascent) on Y) can exceed the control-point
                    // bbox by up to ~1.5 px per side; measured max in this run
                    // is reported. 1.75 leaves headroom without letting a real
                    // placement error hide.
                    if (maxShortfall > 1.75)
                        TEST_CHECKF(false,
                                    "case %d: instance (%.3f %.3f %.3f %.3f) does not cover quad (%.3f %.3f %.3f %.3f) "
                                    "by %.3f px (U+%04X size %.2f)",
                                    iteration, instance.ScreenMinX, instance.ScreenMinY, instance.ScreenMaxX,
                                    instance.ScreenMaxY, e.QuadL, e.QuadT, e.QuadR, e.QuadB, maxShortfall,
                                    (unsigned int)e.Glyph->Codepoint, l.Size);
                }
                else
                {
                    // Containment only applies when the engine actually takes
                    // the fallback (at least one side flagged as cropped): a
                    // sub-epsilon crop is treated as uncropped and the instance
                    // is the full box, confined by the run scissor instead.
                    const bool engineTakesFallback =
                        e.Cropped[0] != 0 || e.Cropped[1] != 0 || e.Cropped[2] != 0 || e.Cropped[3] != 0;
                    const double slack = 0.6;
                    if (engineTakesFallback &&
                        (instance.ScreenMinX < e.QuadL - slack || instance.ScreenMaxX > e.QuadR + slack ||
                         instance.ScreenMinY < e.QuadT - slack || instance.ScreenMaxY > e.QuadB + slack))
                    {
                        g_Stats.ScissorViolations++;
                        TEST_CHECKF(false,
                                    "case %d (fallback): instance (%.2f %.2f %.2f %.2f) escapes quad "
                                    "(%.2f %.2f %.2f %.2f)",
                                    iteration, instance.ScreenMinX, instance.ScreenMinY, instance.ScreenMaxX,
                                    instance.ScreenMaxY, e.QuadL, e.QuadT, e.QuadR, e.QuadB);
                    }
                }
            }
        }
        if (anyDegenerateLine)
        {
            // Structure-only case: instances are not paired (NaN-producing
            // ImGui clips); count them for visibility.
            g_Stats.StructureOnlyInstances += (long long)g_RingUsed;
        }
        else if (emitted < (long long)g_RingUsed)
        {
            g_Stats.Unexpected += (long long)g_RingUsed - emitted;
            TEST_CHECKF(false, "case %d: %lld instances have no ground-truth glyph", iteration,
                        (long long)g_RingUsed - emitted);
        }
        int callbacks = 0;
        if (drawData != nullptr)
            for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
            {
                const ImDrawList* list = drawData->CmdLists[listIndex];
                for (int i = 0; i < list->CmdBuffer.Size; ++i)
                    if (list->CmdBuffer[i].UserCallback == ImSlug::DrawCallback)
                        callbacks++;
            }
        if (callbacks != g_DrawCalls)
            TEST_CHECKF(false, "case %d: %d callbacks vs %d draws", iteration, callbacks, g_DrawCalls);
    }

    void PrintStats(const char* pass)
    {
        std::printf("\n[%s] %lld cases (%lld degenerate lines), %lld ground-truth glyphs, %lld paired instances, "
                    "%lld structure-only instances\n",
                    pass, g_Stats.Cases, g_Stats.StructureOnlyLines, g_Stats.GroundTruth, g_Stats.Instances,
                    g_Stats.StructureOnlyInstances);
        std::printf("  missing %lld, unexpected %lld, resyncs %lld (%lld instances), undecided slivers %lld, boundary "
                    "skips %lld, drops %lld, structure violations %lld, fallback escapes %lld\n",
                    g_Stats.Missing, g_Stats.Unexpected, g_Stats.Resyncs, g_Stats.ResyncedInstances,
                    g_Stats.UndecidedSlivers, g_Stats.BoundarySkips, g_Stats.Drops, g_Stats.StructureViolations,
                    g_Stats.ScissorViolations);
        std::printf("  coverage gap max %.4f px (bound 1.75), batch violations %lld\n", g_Stats.MaxCoverShortfall,
                    g_BatchViolations);
        for (int c = 0; c < SpanClass_Count; ++c)
        {
            if (g_Stats.Count[c] == 0)
                continue;
            std::printf("  %-10s n=%7lld  pen err mean %.5f max %.5f (bound %.2f)  ppem rel err max %.6f\n",
                        SpanClassName(c), g_Stats.Count[c],
                        g_Stats.Count[c] > 0 ? g_Stats.SumPenError[c] / (double)g_Stats.Count[c] : 0.0,
                        g_Stats.MaxPenError[c], BoundOf((SpanClass)c), g_Stats.MaxPpemError[c]);
        }
        std::printf("  crops: unclipped %lld, one-side %lld, multi %lld, all-four %lld\n",
                    g_Stats.CropCategoryCount[Category_Unclipped], g_Stats.CropCategoryCount[Category_OneSide],
                    g_Stats.CropCategoryCount[Category_Multi], g_Stats.CropCategoryCount[Category_AllFour]);
        for (int i = 0; i < g_ResyncLogCount; ++i)
            std::printf("  resync[%d]: case %lld U+%04X size %.2f skipped %lld instance(s)\n", i, g_ResyncLog[i].Case,
                        g_ResyncLog[i].Codepoint, g_ResyncLog[i].Size, g_ResyncLog[i].Skipped);
        for (int i = 0; i < g_BadBatchLogCount; ++i)
            std::printf("  bad batch[%d]: case %lld magic 0x%08X count %u clip (%.3f %.3f %.3f %.3f)\n", i,
                        g_BadBatchLog[i].Case, g_BadBatchLog[i].Magic, g_BadBatchLog[i].Count, g_BadBatchLog[i].L,
                        g_BadBatchLog[i].T, g_BadBatchLog[i].R, g_BadBatchLog[i].B);
    }

    void ResetStats()
    {
        memset(&g_Stats, 0, sizeof(g_Stats));
        g_BatchViolations = 0;
        g_ResyncLogCount = 0;
        g_BadBatchLogCount = 0;
    }

    // ---------------------------------------------------------------------
    // Verbose single-case diagnostics (--only N --verbose)
    // ---------------------------------------------------------------------
    void DumpCase(const Case& testCase)
    {
        for (int line = 0; line < testCase.LineCount; ++line)
        {
            const LineCase& l = testCase.Lines[line];
            std::printf("  line %d: \"%s\" size %.3f pos (%.3f %.3f) snap=%d clip=%d", line, l.Text, l.Size, l.Pos.x,
                        l.Pos.y, !l.NoPixelSnap, l.HasClip);
            if (l.HasClip)
                std::printf(" (%.3f %.3f %.3f %.3f)", l.Clip.x, l.Clip.y, l.Clip.z, l.Clip.w);
            std::printf(" listClip (%.1f %.1f %.1f %.1f)\n", l.ListClip.x, l.ListClip.y, l.ListClip.z, l.ListClip.w);
        }
    }

    void DumpInstances()
    {
        std::printf("  instances (%u):\n", g_RingUsed);
        for (uint32_t i = 0; i < g_RingUsed && i < 40; ++i)
            std::printf("    [%2u] rect (%.4f %.4f %.4f %.4f) em (%.5f %.5f %.5f %.5f)\n", i, g_Ring[i].ScreenMinX,
                        g_Ring[i].ScreenMinY, g_Ring[i].ScreenMaxX, g_Ring[i].ScreenMaxY, g_Ring[i].EmMinX,
                        g_Ring[i].EmMinY, g_Ring[i].EmMaxX, g_Ring[i].EmMaxY);
    }

    void DumpDrawData()
    {
        const ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr)
            return;
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* list = drawData->CmdLists[listIndex];
            std::printf("  list %d: %d verts, %d idx, %d cmds\n", listIndex, list->VtxBuffer.Size, list->IdxBuffer.Size,
                        list->CmdBuffer.Size);
            for (int i = 0; i < list->VtxBuffer.Size && i < 24; ++i)
            {
                const ImDrawVert& v = list->VtxBuffer[i];
                ImSlugDecodedVertex decoded;
                const bool slug = ImSlugAdapterDecodeVertex(v.uv.x, v.uv.y, &decoded);
                std::printf("    v%2d pos (%9.3f %9.3f) uv (%10.4f %10.4f) slug=%d frac (%.3f %.3f)\n", i, v.pos.x,
                            v.pos.y, v.uv.x, v.uv.y, slug ? 1 : 0, decoded.FracX, decoded.FracY);
            }
            for (int i = 0; i < list->CmdBuffer.Size && i < 12; ++i)
            {
                const ImDrawCmd& cmd = list->CmdBuffer[i];
                std::printf("    cmd %d: idx %u+%u vtx %u elem %u clip (%.1f %.1f %.1f %.1f) cb %s\n", i, cmd.IdxOffset,
                            cmd.ElemCount, cmd.VtxOffset, cmd.ElemCount, cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z,
                            cmd.ClipRect.w,
                            cmd.UserCallback == nullptr
                                ? "none"
                                : (cmd.UserCallback == ImSlug::DrawCallback ? "slug" : "other"));
            }
        }
    }

    void DumpExpected(const Case& testCase)
    {
        ExpectedGlyph expected[64];
        for (int line = 0; line < testCase.LineCount; ++line)
        {
            const int count = ComputeLineGroundTruth(testCase.Lines[line], g_SlugFont, expected, 64);
            std::printf("  expected (line %d, %d glyphs):\n", line, count);
            for (int g = 0; g < count && g < 40; ++g)
                std::printf("    [%2d] U+%04X pen (%.4f %.4f) quad (%.4f %.4f %.4f %.4f) crop %d%d%d%d em (%.5f %.5f "
                            "%.5f %.5f) ppem %.4f\n",
                            g, (unsigned int)expected[g].Glyph->Codepoint, expected[g].PenX, expected[g].PenY,
                            expected[g].QuadL, expected[g].QuadT, expected[g].QuadR, expected[g].QuadB,
                            expected[g].Cropped[0], expected[g].Cropped[1], expected[g].Cropped[2],
                            expected[g].Cropped[3], expected[g].Record->MinX, expected[g].Record->MinY,
                            expected[g].Record->MaxX, expected[g].Record->MaxY,
                            expected[g].PixelsPerEm * expected[g].Scale);
        }
    }
    // ---------------------------------------------------------------------
    // Targeted high-slot regression (M4)
    // ---------------------------------------------------------------------
    // The random sequence reaches high slab ids (thousands of bakes) only after
    // tens of thousands of cases; there the encoded-UV ulp exceeds the fine-clip
    // offset of sub-pixel slivers. M4 found two failure classes in that regime:
    // a fully collapsed corner-fraction span dropped the glyph (the fit returned
    // "all samples equal"), and a sliver-scale span made the fitted placement up
    // to 3.2 px off (case numbers below). This phase warms the slab registry to
    // that regime (~4300 live slabs) and re-runs exactly those cases through the
    // normal checks - a fast regression that does not depend on the long
    // profile's iteration count.
    void RunHighSlotRegression(const char* fontPath, const unsigned char* fontData, int fontDataSize, uint64_t seed)
    {
        std::printf("[high-slot] targeted regression (slab ids > ~4000)\n");
        if (!SetupPass(/*reconstruction=*/true, fontPath))
            return;
        ImSlugDestroyFont(g_TestFont);
        g_TestFont = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
        if (g_TestFont == nullptr)
            return;
        ResetStats();

        // 45 integer sizes x ~95 visible Latin glyphs: ~4300 live slabs. The
        // frames are throwaway (no checks); only the slab registry state
        // matters. AddText bakes the glyphs (allocating the slabs), so the
        // pre-pass is not needed here.
        char glyphs[128];
        int at = 0;
        for (unsigned int codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
            at += ImTextCharToUtf8(glyphs + at, codepoint);
        glyphs[at] = '\0';
        for (int size = 6; size <= 50; ++size)
        {
            ImGui::NewFrame();
            ImGui::GetBackgroundDrawList()->AddText(g_SlugFont, (float)size, ImVec2(4.0f, 4.0f),
                                                    IM_COL32(255, 255, 255, 255), glyphs);
            ImGui::Render();
        }
        Statistics warmStats;
        ImSlug::GetStatistics(&warmStats);
        std::printf("  warm-up: %u live slabs\n", warmStats.GlyphSlabs);

        const long long cases[] = { 20033, 45549, 47496, 66672, 86675, 94792 };
        for (long long caseIndex : cases)
        {
            Rng caseRng;
            caseRng.State = seed + (uint64_t)caseIndex * 0xD1342543DE82EF95ull;
            Case testCase;
            GenerateCase(caseRng, &testCase);
            ResetFake();
            RunCase(testCase, /*reconstructionOn=*/true, (uint32_t)caseIndex + 2000000u, (int)caseIndex);
        }
        PrintStats("high-slot regression");
    }
}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    long long iterations = 3000;
    if (const char* env = getenv("IMSLUG_FUZZ_ITERATIONS"))
        iterations = strtoll(env, nullptr, 10);
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    long long onlyCase = -1;
    int passStart = 0;
    const char* dumpEnv = getenv("IMSLUG_FUZZ_DUMP");
    const long long dumpCase = dumpEnv != nullptr ? strtoll(dumpEnv, nullptr, 10) : -1;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            iterations = strtoll(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = strtoull(argv[++i], nullptr, 0);
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            onlyCase = strtoll(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--verbose") == 0)
            g_Verbose = true;
        else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc)
            passStart = atoi(argv[++i]);
    }
    std::printf("imslug_fuzz: fine-clip fuzz harness (%lld cases x 2 passes, seed 0x%llX)\n", iterations,
                (unsigned long long)seed);

    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
    TEST_CHECK(fontData != nullptr);

    // Two passes over the same deterministic case sequence: the primary
    // reconstruction path and the R3 fallback (Config flag off).
    const int passCount = (onlyCase >= 0) ? passStart + 1 : 2;
    for (int pass = passStart; pass < passCount; ++pass)
    {
        const bool reconstruction = (pass == 0);
        if (!SetupPass(reconstruction, IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf")))
            break;
        ImSlugDestroyFont(g_TestFont);
        g_TestFont = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
        TEST_CHECK(g_TestFont != nullptr);
        ResetStats();

        Case testCase;
        for (long long iteration = 0; iteration < iterations; ++iteration)
        {
            Rng caseRng;
            caseRng.State = seed + (uint64_t)iteration * 0xD1342543DE82EF95ull;
            GenerateCase(caseRng, &testCase);
            if (onlyCase >= 0 && iteration != onlyCase)
                continue;
            ResetFake();
            // Dump before the case runs: RunCase captures the draw-list clip.
            RunCase(testCase, reconstruction, (uint32_t)(iteration + pass * 1000000), (int)iteration);
            // IMSLUG_FUZZ_DUMP=<case>: dump one case of a full run (state after
            // the whole sequence, for reproducing order-dependent failures).
            if (g_Verbose || iteration == dumpCase)
            {
                std::printf("=== case %lld (reconstruction=%d) ===\n", iteration, reconstruction ? 1 : 0);
                DumpCase(testCase);
                DumpDrawData();
                DumpInstances();
                DumpExpected(testCase);
                Statistics liveStats;
                ImSlug::GetStatistics(&liveStats);
                std::printf("  statistics: slabs %u, batches %u, instances %u, dropped %u, bounds mismatches %u\n",
                            liveStats.GlyphSlabs, liveStats.LiveBatches, liveStats.Instances, liveStats.DroppedRuns,
                            liveStats.BoundsMismatches);
            }
        }
        PrintStats(reconstruction ? "reconstruction" : "fallback (R3)");
    }

    // Targeted high-slot regression (fast; skipped in the --only debug mode).
    if (onlyCase < 0)
        RunHighSlotRegression(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), fontData, fontDataSize, seed);

    ImSlugDestroyFont(g_TestFont);
    g_TestFont = nullptr;
    ImSlugSetRenderIO(nullptr);
    ImSlug::Shutdown();
    ImGui::DestroyContext();
    std::free(fontData);
    return imslug_test::Finish("imslug_fuzz");
}
