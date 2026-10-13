// imslug M4 stress suite — robustness at scale and on degenerate input.
//
// GPU-free: the real pre-pass runs against a fake RenderIO with a ring sized
// for the whole scene, so the suite runs everywhere (lavapipe CI included).
//
//   1. ~5k visible glyphs in one frame: one run/batch, every glyph placed,
//      pre-pass timing reported (the bench harness owns the perf gate);
//   2. long wrapped text in a child window (ImGui's wrapping + per-line
//      commands), plus empty/missing glyphs and spaces;
//   3. CJK: the fetched Noto Sans SC subset (scripts/fetch_cjk_font.*) renders
//      thousands of Hanzi with extreme curve counts; skipped when the font is
//      absent (PROVENANCE.txt documents the fetch);
//   4. degenerate input: NaN / out-of-range synthetic outlines are rejected
//      transactionally (InvalidOutline), fully degenerate quads are harmless
//      (coverage 0), empty glyphs work;
//   5. pool-limit fallbacks (R11): CurvePoolLimit and BandPoolLimit are
//      retryable and fall back to ImGui's replacement glyph; GlyphTooLarge
//      (a band reference list longer than a row) is definitive; every failed
//      build leaves the pools untouched;
//   6. atlas growth/repacking with 5k Slug glyphs in flight: handles and
//      placements stay bit-identical (proof, not just spot check).

#include "imslug.h"

#include "imslug_imgui_adapter.h"
#include "imslug_internal.h"
#include "imslug_refraster.h"

#include "test_util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef IMSLUG_CJK_TEST_FONT
#define IMSLUG_CJK_TEST_FONT "tests/fonts/cjk/NotoSansSC-subset.ttf"
#endif

using namespace ImSlug;

namespace
{
    const uint32_t kStressRingCapacity = 65536;
    ImSlugGlyphInstance g_Ring[kStressRingCapacity];
    uint32_t g_RingUsed = 0;
    uint32_t g_FrameIndex = 0;
    int g_DrawCalls = 0;
    ImSlugBatch g_LastBatch;
    long long g_BatchViolations = 0;

    uint32_t FakeGetFrameIndex()
    {
        return g_FrameIndex;
    }
    ImSlugGlyphInstance* FakeReserve(uint32_t count, uint32_t* outFirst)
    {
        if (count > kStressRingCapacity - g_RingUsed)
            return nullptr;
        *outFirst = g_RingUsed;
        g_RingUsed += count;
        return g_Ring + (g_RingUsed - count);
    }
    void FakeDraw(const ImSlugBatch* batch)
    {
        g_DrawCalls++;
        g_LastBatch = *batch;
        if (batch->Magic != kBatchMagic || batch->InstanceCount == 0 || !std::isfinite(batch->ClipMinX) ||
            !std::isfinite(batch->ClipMinY) || !std::isfinite(batch->ClipMaxX) || !std::isfinite(batch->ClipMaxY))
            g_BatchViolations++;
    }
    const ImSlugRenderIO g_FakeIO = { "stress", FakeGetFrameIndex, FakeReserve, FakeDraw };

    void FakeResetRenderState(const ImDrawList*, const ImDrawCmd*) {}

    void ResetFake()
    {
        g_RingUsed = 0;
        g_DrawCalls = 0;
    }

    // Runs the frame: render, pre-pass, dispatch callbacks (as a backend would).
    ImDrawData* RunFrame(uint32_t frameIndex)
    {
        ResetFake();
        g_FrameIndex = frameIndex;
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        ImSlug::PreprocessDrawData(drawData);
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
        return drawData;
    }

    // Structural invariants every stress scene must keep.
    void CheckStructure(const char* what, ImDrawData* drawData)
    {
        TEST_CHECKF(g_BatchViolations == 0, "%s: %lld invalid batches", what, g_BatchViolations);
        if (drawData == nullptr)
            return;
        int callbacks = 0;
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* list = drawData->CmdLists[listIndex];
            for (int i = 0; i < list->CmdBuffer.Size; ++i)
            {
                const ImDrawCmd& cmd = list->CmdBuffer[i];
                if (cmd.UserCallback == ImSlug::DrawCallback)
                {
                    callbacks++;
                    TEST_CHECKF(i + 1 < list->CmdBuffer.Size &&
                                    list->CmdBuffer[i + 1].UserCallback ==
                                        ImGui::GetPlatformIO().DrawCallback_ResetRenderState,
                                "%s: Slug callback not followed by a reset", what);
                }
                else if (cmd.UserCallback == nullptr)
                {
                    for (unsigned int e = 0; e < cmd.ElemCount; ++e)
                    {
                        const ImDrawVert& v = list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + e] + cmd.VtxOffset];
                        ImSlugDecodedVertex decoded;
                        if (ImSlugAdapterDecodeVertex(v.uv.x, v.uv.y, &decoded))
                        {
                            TEST_CHECKF(false, "%s: Slug vertex left in geometry", what);
                            break;
                        }
                    }
                }
            }
        }
        TEST_CHECKF(callbacks == g_DrawCalls, "%s: %d callbacks vs %d draws", what, callbacks, g_DrawCalls);
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        TEST_CHECKF(stats.DroppedRuns == 0, "%s: %u dropped runs", what, stats.DroppedRuns);
        TEST_CHECKF(stats.BoundsMismatches == 0, "%s: %u bounds mismatches", what, stats.BoundsMismatches);
    }

    // ---------------------------------------------------------------------
    // 1. ~5k visible glyphs in one frame
    // ---------------------------------------------------------------------
    void TestFiveThousandGlyphs()
    {
        std::printf("[stress] 5k glyphs in one frame\n");
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
        const float size = 16.0f;

        // 125 lines x 40 chars = 5000 chars; spaces are invisible (not drawn).
        // The lines must stay inside the window clip: ImGui culls text that is
        // outside the draw-list clip (its own optimization, not a bug).
        char text[5001 + 126];
        const char* pattern = "Hamburgefonstiv 0123456789 ";
        int cursor = 0;
        for (int i = 0; i < 5000; ++i)
        {
            if (i > 0 && i % 40 == 0)
                text[cursor++] = '\n';
            text[cursor++] = pattern[i % (int)strlen(pattern)];
        }
        text[cursor] = '\0';

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(-20.0f, -20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600.0f, 2300.0f), ImGuiCond_Always);
        ImGui::Begin("stress-5k", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const auto start = std::chrono::steady_clock::now();
        drawList->AddText(font, size, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255), text);
        const auto textDone = std::chrono::steady_clock::now();
        ImGui::End();
        ImDrawData* drawData = RunFrame(1);
        const auto prePassDone = std::chrono::steady_clock::now();

        Statistics stats;
        ImSlug::GetStatistics(&stats);
        std::printf("  instances %u (ring %u), batches %u, text %.2f ms, pre-pass+dispatch %.2f ms\n", stats.Instances,
                    g_RingUsed, stats.LiveBatches, std::chrono::duration<double, std::milli>(textDone - start).count(),
                    std::chrono::duration<double, std::milli>(prePassDone - textDone).count());
        TEST_CHECKF(stats.Instances >= 4500, "expected ~5k instances, got %u", stats.Instances);
        TEST_CHECKF(g_RingUsed == stats.Instances, "ring %u vs instances %u", g_RingUsed, stats.Instances);
        TEST_CHECKF(stats.LiveBatches == 1, "one contiguous run expected, got %u", stats.LiveBatches);
        CheckStructure("5k", drawData);

        // Every instance is finite and has a non-degenerate rect.
        for (uint32_t i = 0; i < g_RingUsed; ++i)
        {
            const ImSlugGlyphInstance& instance = g_Ring[i];
            TEST_CHECK(std::isfinite(instance.ScreenMinX) && std::isfinite(instance.ScreenMaxX) &&
                       std::isfinite(instance.ScreenMinY) && std::isfinite(instance.ScreenMaxY));
            TEST_CHECK(instance.ScreenMaxX > instance.ScreenMinX && instance.ScreenMaxY > instance.ScreenMinY);
        }

        // Sanity: the instance data survives a ring with a frame index != 0 that
        // a backend would add to the first instance (M2 D10a contract).
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 100.0f), ImGuiCond_Always);
        ImGui::Begin("stress-frame2", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 255),
                                            "frame two");
        ImGui::End();
        drawData = RunFrame(2);
        TEST_CHECK(g_RingUsed > 0);
        CheckStructure("5k frame 2", drawData);
    }

    // ---------------------------------------------------------------------
    // 2. Long wrapped text, spaces, missing glyphs
    // ---------------------------------------------------------------------
    void TestWrappedTextAndEdgeGlyphs()
    {
        std::printf("[stress] wrapped text + spaces + missing glyphs\n");
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];

        char paragraph[4097];
        const char* words[] = { "wrapping", "text", "with",  "mixed",  "length", "words", "and",    "spaces",
                                "over",     "many", "lines", "inside", "a",      "child", "window", "!" };
        paragraph[0] = '\0';
        for (int i = 0; i < 700; ++i)
        {
            strncat(paragraph, words[i % 16], sizeof(paragraph) - strlen(paragraph) - 1);
            strncat(paragraph, " ", sizeof(paragraph) - strlen(paragraph) - 1);
        }

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 600.0f), ImGuiCond_Always);
        ImGui::Begin("stress-wrap", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar);
        ImGui::TextWrapped("%s", paragraph);
        ImGui::End();
        ImDrawData* drawData = RunFrame(3);
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        std::printf("  instances %u, batches %u\n", stats.Instances, stats.LiveBatches);
        TEST_CHECKF(stats.Instances > 1000, "wrapped paragraph produced %u instances", stats.Instances);
        TEST_CHECKF(stats.LiveBatches >= 1, "expected at least one run, got %u", stats.LiveBatches);
        CheckStructure("wrapped", drawData);

        // Spaces are invisible (no instance); a missing codepoint comes from
        // ImGui's fallback glyph (never a Slug handle).
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f), ImGuiCond_Always);
        ImGui::Begin("stress-edge", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(font, 16.0f, ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 255), "   ");
        drawList->AddText(font, 16.0f, ImVec2(10.0f, 40.0f), IM_COL32(255, 255, 255, 255), "A B");
        ImGui::End();
        drawData = RunFrame(4);
        Statistics spaceStats;
        ImSlug::GetStatistics(&spaceStats);
        TEST_CHECKF(spaceStats.Instances == 2, "spaces+A B should place exactly 2 glyphs, got %u",
                    spaceStats.Instances);
        CheckStructure("edge glyphs", drawData);

        // Missing codepoint: ImGui substitutes its replacement glyph (U+FFFD
        // here: ImGui picks the first of FallbackChar/'?'/' ' the font has; it
        // is itself a normal Slug glyph) - never a per-codepoint garbage
        // record, and all missing codepoints share it.
        ImFontBaked* baked = font->GetFontBaked(16.0f);
        const ImFontGlyph* missing = baked->FindGlyph((ImWchar)0xE000);  // private use, unassigned
        const ImFontGlyph* missing2 = baked->FindGlyph((ImWchar)0xE001);
        TEST_CHECK(missing != nullptr);
        if (missing != nullptr)
        {
            TEST_CHECKF(missing == missing2, "missing codepoints must share ImGui's replacement glyph");
            TEST_CHECKF(missing == baked->FindGlyph((ImWchar)0xFFFD), "replacement must be U+FFFD for this font");
            ImSlugDecodedVertex decoded;
            if (ImSlugAdapterDecodeVertex(missing->U0, missing->V0, &decoded))
                TEST_CHECKF(decoded.Slab != nullptr && decoded.Slab->Record != nullptr,
                            "replacement glyph carries a half-decoded handle");
        }

        ImGui::NewFrame();
        ImGui::Begin("stress-missing", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(font, 16.0f, ImVec2(10.0f, 10.0f), IM_COL32(255, 255, 255, 255), "A");
        ImGui::End();
        drawData = RunFrame(5);
        CheckStructure("missing codepoint frame", drawData);
    }

    // ---------------------------------------------------------------------
    // 3. CJK (fetched subset; skip when absent)
    // ---------------------------------------------------------------------
    bool CjkFontAvailable()
    {
        std::FILE* file = std::fopen(IMSLUG_CJK_TEST_FONT, "rb");
        if (file == nullptr)
            return false;
        std::fclose(file);
        return true;
    }

    void TestCjk()
    {
        std::printf("[stress] CJK subset\n");
        if (!CjkFontAvailable())
        {
            std::printf("  SKIP: %s not fetched (run scripts/fetch_cjk_font.*; see tests/fonts/cjk/PROVENANCE.txt)\n",
                        IMSLUG_CJK_TEST_FONT);
            return;
        }

        ImFont* cjkFont = ImSlug::AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 24.0f);
        TEST_CHECKF(cjkFont != nullptr, "cannot add the CJK font");
        if (cjkFont == nullptr)
            return;
        ImFontBaked* baked = cjkFont->GetFontBaked(24.0f);
        TEST_CHECK(baked != nullptr);
        if (baked == nullptr)
            return;

        // 120 lines x 30 Hanzi = 3600 glyphs from the subset range
        // (U+4E00..U+4EFF, repeated). Multi-line so ImGui's draw-list clip
        // does not cull the text.
        char text[16384];
        int at = 0;
        int codepointIndex = 0;
        for (int line = 0; line < 120; ++line)
        {
            for (int column = 0; column < 30; ++column)
            {
                unsigned int codepoint = 0x4E00u + (unsigned int)(codepointIndex++ % 0x100);
                at += ImTextCharToUtf8(text + at, codepoint);
            }
            if (line + 1 < 120)
                text[at++] = '\n';
        }
        text[at] = '\0';

        const auto start = std::chrono::steady_clock::now();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1200.0f, 3000.0f), ImGuiCond_Always);
        ImGui::Begin("stress-cjk", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(cjkFont, 24.0f, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255), text);
        ImGui::End();
        ImDrawData* drawData = RunFrame(6);
        const auto done = std::chrono::steady_clock::now();

        Statistics stats;
        ImSlug::GetStatistics(&stats);
        std::printf("  Hanzi %d, instances %u, batches %u, first-build+pre-pass %.1f ms\n", at, stats.Instances,
                    stats.LiveBatches, std::chrono::duration<double, std::milli>(done - start).count());
        TEST_CHECKF(stats.Instances >= 3500, "expected ~3600 CJK instances, got %u", stats.Instances);
        TEST_CHECKF(stats.DroppedRuns == 0, "%u dropped runs", stats.DroppedRuns);
        CheckStructure("cjk", drawData);

        // Extreme curve counts: report the worst glyph in the range and pin
        // the band-count cap (kMaxBandCountPerAxis) + record validity.
        int worstCurves = 0;
        unsigned int worstCodepoint = 0;
        int checked = 0;
        for (unsigned int codepoint = 0x4E00; codepoint <= 0x4EFF; ++codepoint)
        {
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)codepoint);
            if (glyph == nullptr || !glyph->Visible)
                continue;
            ImSlugDecodedVertex decoded;
            TEST_CHECK(ImSlugAdapterDecodeVertex(glyph->U0, glyph->V0, &decoded));
            if (decoded.Slab == nullptr || decoded.Slab->Record == nullptr)
                continue;
            const ImSlugGlyphRecord* record = decoded.Slab->Record;
            TEST_CHECKF(record->BandMaxX < kMaxBandCountPerAxis && record->BandMaxY < kMaxBandCountPerAxis,
                        "band count over the cap for U+%04X", codepoint);
            TEST_CHECKF(record->BandScaleX >= 0.0f && record->BandScaleY >= 0.0f, "negative band scale");
            if ((int)record->CurveCount > worstCurves)
            {
                worstCurves = (int)record->CurveCount;
                worstCodepoint = codepoint;
            }
            checked++;
        }
        std::printf("  %d glyphs checked; worst curve count %d (U+%04X)\n", checked, worstCurves, worstCodepoint);
        TEST_CHECKF(worstCurves > 20, "expected a curve-heavy glyph, got %d", worstCurves);
        TEST_CHECK(checked > 200);
    }

    // ---------------------------------------------------------------------
    // 4. Degenerate input: NaN / out-of-range / all-equal / empty outlines
    // ---------------------------------------------------------------------
    void TestDegenerateOutlines()
    {
        std::printf("[stress] degenerate outlines\n");
        ImSlugPools* pools = ImSlugGetPools();
        int fontDataSize = 0;
        unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
        TEST_CHECK(fontData != nullptr);
        ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
        TEST_CHECK(font != nullptr);
        if (pools == nullptr || font == nullptr)
            return;

        const int curveCursor = pools->Curves.CursorTexels;
        const int bandCursor = pools->Bands.CursorTexels;
        ImSlugGlyphRecord record;

        // NaN and infinity are rejected definitively; the pools stay untouched.
        // (volatile keeps the compiler from constant-folding the divisions.)
        volatile float zero = 0.0f;
        const float nanValue = zero / zero;
        const float infValue = 1.0f / zero;
        ImSlugQuad bad[3] = {
            { 0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f },
            { 0.1f, 0.1f, nanValue, 0.2f, 0.3f, 0.3f },
            { 0.1f, 0.1f, 0.2f, infValue, 0.3f, 0.3f },
        };
        TEST_CHECKF(ImSlugBuildSyntheticGlyphRecord(font, pools, bad, 3, 0.5f, &record) ==
                        ImSlugBuildStatus_InvalidOutline,
                    "NaN/Inf outline must be rejected");
        TEST_CHECK(pools->Curves.CursorTexels == curveCursor && pools->Bands.CursorTexels == bandCursor);

        // Far outside any sane em range: same definitive rejection.
        ImSlugQuad huge[1] = { { 0.0f, 0.0f, 1.0e6f, 0.0f, 1.0e6f, 1.0f } };
        TEST_CHECKF(ImSlugBuildSyntheticGlyphRecord(font, pools, huge, 1, 0.5f, &record) ==
                        ImSlugBuildStatus_InvalidOutline,
                    "out-of-range outline must be rejected");
        TEST_CHECK(pools->Curves.CursorTexels == curveCursor && pools->Bands.CursorTexels == bandCursor);

        // A fully degenerate segment is representable (coverage 0), not a crash:
        // it matches no band on either axis, so the shader never evaluates it.
        ImSlugQuad degenerate[1] = { { 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f } };
        const ImSlugBuildStatus degenerateStatus =
            ImSlugBuildSyntheticGlyphRecord(font, pools, degenerate, 1, 0.5f, &record);
        TEST_CHECKF(degenerateStatus == ImSlugBuildStatus_Ok, "degenerate segment build status %d",
                    (int)degenerateStatus);
        if (degenerateStatus == ImSlugBuildStatus_Ok)
        {
            TEST_CHECK(record.BandScaleX == 0.0f && record.BandScaleY == 0.0f);
            float coverage[16 * 16] = {};
            ImSlugRasterGlyphRect(pools, &record, 0.25f, 0.25f, 16.0f, coverage, 16, 16);
            float ink = 0.0f;
            for (float value : coverage)
                ink += value;
            TEST_CHECKF(ink == 0.0f, "degenerate glyph rasterized ink %.4f", ink);
        }

        // Empty glyph (no quads): representable, rasterizes to nothing.
        const ImSlugBuildStatus emptyStatus = ImSlugBuildSyntheticGlyphRecord(font, pools, nullptr, 0, 0.5f, &record);
        TEST_CHECKF(emptyStatus == ImSlugBuildStatus_Ok, "empty glyph build status %d", (int)emptyStatus);
        if (emptyStatus == ImSlugBuildStatus_Ok)
        {
            TEST_CHECK(record.CurveCount == 0);
            TEST_CHECK(record.BandScaleX == 0.0f && record.BandScaleY == 0.0f);
        }

        ImSlugDestroyFont(font);
        std::free(fontData);
    }

    // ---------------------------------------------------------------------
    // 5. Pool-limit fallbacks (R11)
    // ---------------------------------------------------------------------
    void TestPoolLimits()
    {
        std::printf("[stress] pool limits -> graceful fallback\n");
        ImSlugPools* pools = ImSlugGetPools();
        TEST_CHECK(pools != nullptr);
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
        ImFontBaked* baked = font->GetFontBaked(32.0f);
        TEST_CHECK(baked != nullptr);
        if (pools == nullptr || baked == nullptr)
            return;

        // --- retryable CurvePoolLimit ------------------------------------
        {
            const int saved = pools->Curves.CursorTexels;
            const int savedBands = pools->Bands.CursorTexels;
            pools->Curves.CursorTexels = kPoolMaxHeight * kPoolWidth + 1;  // beyond the 16-bit height
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)'Q');     // not baked yet at this size
            TEST_CHECK(glyph != nullptr);
            ImSlugDecodedVertex decoded;
            TEST_CHECKF(glyph == baked->FindGlyph((ImWchar)0xFFFD),
                        "curve-pool overflow must fall back to ImGui's replacement glyph");
            TEST_CHECKF(pools->Curves.CursorTexels == kPoolMaxHeight * kPoolWidth + 1,
                        "failed build must not advance the curve cursor");
            TEST_CHECKF(pools->Bands.CursorTexels == savedBands, "failed build advanced the band cursor");
            pools->Curves.CursorTexels = saved;
            const ImFontGlyph* retried = baked->FindGlyph((ImWchar)'Q');
            TEST_CHECK(retried == glyph);
            TEST_CHECKF(ImSlugAdapterDecodeVertex(retried->U0, retried->V0, &decoded),
                        "the retry after restoring capacity must build the glyph");
        }

        // --- retryable BandPoolLimit -------------------------------------
        {
            const int saved = pools->Bands.CursorTexels;
            const int savedCurves = pools->Curves.CursorTexels;
            pools->Bands.CursorTexels = kPoolMaxHeight * kPoolWidth + 1;
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)'R');
            TEST_CHECK(glyph != nullptr);
            TEST_CHECKF(glyph == baked->FindGlyph((ImWchar)0xFFFD),
                        "band-pool overflow must fall back to ImGui's replacement glyph");
            ImSlugDecodedVertex decoded;
            TEST_CHECKF(pools->Bands.CursorTexels == kPoolMaxHeight * kPoolWidth + 1,
                        "failed build must not advance the band cursor");
            TEST_CHECKF(pools->Curves.CursorTexels == savedCurves, "failed build advanced the curve cursor");
            pools->Bands.CursorTexels = saved;
            const ImFontGlyph* retried = baked->FindGlyph((ImWchar)'R');
            TEST_CHECKF(ImSlugAdapterDecodeVertex(retried->U0, retried->V0, &decoded),
                        "the retry after restoring capacity must build the glyph");
        }

        // --- definitive GlyphTooLarge (one band's ref list exceeds a row) --
        {
            int fontDataSize = 0;
            unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
            ImSlugFont* scratchFont = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
            std::free(fontData);
            TEST_CHECK(scratchFont != nullptr);
            if (scratchFont != nullptr)
            {
                // 5000 tiny diagonal segments stacked at the same place: every
                // horizontal band that overlaps them collects all 5000 refs,
                // which cannot be represented (lists never wrap a row).
                const int quadCount = 5000;
                ImSlugQuad* quads = (ImSlugQuad*)std::malloc((size_t)quadCount * sizeof(ImSlugQuad));
                TEST_CHECK(quads != nullptr);
                for (int i = 0; i < quadCount; ++i)
                {
                    const float offset = 0.001f * (float)(i % 8);
                    quads[i].X1 = 0.2f + offset;
                    quads[i].Y1 = 0.2f;
                    quads[i].X2 = 0.25f + offset;
                    quads[i].Y2 = 0.25f;
                    quads[i].X3 = 0.3f + offset;
                    quads[i].Y3 = 0.2f;
                }
                const int curveCursor = pools->Curves.CursorTexels;
                const int bandCursor = pools->Bands.CursorTexels;
                ImSlugGlyphRecord record;
                const ImSlugBuildStatus status =
                    ImSlugBuildSyntheticGlyphRecord(scratchFont, pools, quads, quadCount, 0.5f, &record);
                TEST_CHECKF(status == ImSlugBuildStatus_GlyphTooLarge, "expected GlyphTooLarge, got %d", (int)status);
                TEST_CHECKF(pools->Curves.CursorTexels == curveCursor && pools->Bands.CursorTexels == bandCursor,
                            "GlyphTooLarge must be transactional");
                std::free(quads);
                ImSlugDestroyFont(scratchFont);
            }
        }
    }

    // ---------------------------------------------------------------------
    // 6. Atlas growth/repack with 5k Slug glyphs in flight
    // ---------------------------------------------------------------------
    void TestRepackImmunity()
    {
        std::printf("[stress] atlas repack immunity (5k glyphs)\n");
        ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        ImSlugPools* pools = ImSlugGetPools();

        // 3000 glyphs over 60 lines of 50 chars, inside the window clip.
        char text[3001 + 61];
        int cursor = 0;
        for (int i = 0; i < 3000; ++i)
        {
            if (i > 0 && i % 50 == 0)
                text[cursor++] = '\n';
            text[cursor++] = (char)('A' + (i % 26));
        }
        text[cursor] = '\0';

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(-20.0f, -20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600.0f, 1200.0f), ImGuiCond_Always);
        ImGui::Begin("repack", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(font, 16.0f, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255), text);
        ImGui::End();
        RunFrame(10);
        const uint32_t referenceCount = g_RingUsed;
        TEST_CHECKF(referenceCount >= 2900, "reference frame produced %u instances", referenceCount);
        if (referenceCount == 0 || referenceCount > kStressRingCapacity)
            return;
        ImSlugGlyphInstance* reference =
            (ImSlugGlyphInstance*)std::malloc((size_t)referenceCount * sizeof(ImSlugGlyphInstance));
        TEST_CHECK(reference != nullptr);
        if (reference == nullptr)
            return;
        memcpy(reference, g_Ring, (size_t)referenceCount * sizeof(ImSlugGlyphInstance));

        // A 96 px stock font repacks the atlas (the M3 test proves growth);
        // Slug handles never touch the atlas, so every instance must be
        // bit-identical afterwards.
        ImFontConfig bigConfig;
        bigConfig.OversampleH = 1;
        bigConfig.OversampleV = 1;
        ImFont* bigFont = atlas->AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Inter-Regular.ttf"), 96.0f, &bigConfig);
        TEST_CHECK(bigFont != nullptr);
        if (bigFont != nullptr)
        {
            ImFontBaked* bigBaked = bigFont->GetFontBaked(96.0f);
            for (unsigned int codepoint = 0x20; codepoint < 0x100; ++codepoint)
                bigBaked->FindGlyph((ImWchar)codepoint);
        }

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(-20.0f, -20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600.0f, 1200.0f), ImGuiCond_Always);
        ImGui::Begin("repack", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(font, 16.0f, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255), text);
        ImGui::End();
        ImDrawData* drawData = RunFrame(11);
        {
            int verts = 0;
            for (int i = 0; i < drawData->CmdListsCount; ++i)
                verts += drawData->CmdLists[i]->VtxBuffer.Size;
            std::printf("  atlas-update frame: ImGui drew %d glyph quads, %u instances\n", verts / 4, g_RingUsed);
        }
        // The first frame after a texture update may draw fewer quads (ImGui's
        // own texture-change path); every instance it does produce must be
        // bit-identical to a pre-repack instance (placements never move).
        TEST_CHECKF(g_RingUsed <= referenceCount, "more instances after the repack: %u vs %u", g_RingUsed,
                    referenceCount);
        CheckStructure("repack update frame", drawData);
        long long unmatched = 0;
        for (uint32_t i = 0; i < g_RingUsed; ++i)
        {
            bool found = (i < referenceCount) && memcmp(&reference[i], &g_Ring[i], sizeof(ImSlugGlyphInstance)) == 0;
            for (uint32_t r = 0; !found && r < referenceCount; ++r)
                found = memcmp(&reference[r], &g_Ring[i], sizeof(ImSlugGlyphInstance)) == 0;
            if (!found)
                unmatched++;
        }
        TEST_CHECKF(unmatched == 0, "%lld instances are not bit-identical to a pre-repack instance", unmatched);

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(-20.0f, -20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600.0f, 1200.0f), ImGuiCond_Always);
        ImGui::Begin("repack", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetWindowDrawList()->AddText(font, 16.0f, ImVec2(0.0f, 0.0f), IM_COL32(255, 255, 255, 255), text);
        ImGui::End();
        drawData = RunFrame(12);
        TEST_CHECKF(g_RingUsed == referenceCount, "instance count changed after the repack: %u vs %u", g_RingUsed,
                    referenceCount);
        CheckStructure("repack settled", drawData);
        int mismatches = 0;
        const uint32_t compare = g_RingUsed < referenceCount ? g_RingUsed : referenceCount;
        for (uint32_t i = 0; i < compare; ++i)
            if (memcmp(&reference[i], &g_Ring[i], sizeof(ImSlugGlyphInstance)) != 0)
                mismatches++;
        TEST_CHECKF(mismatches == 0, "%d instances changed after the atlas repack", mismatches);

        std::free(reference);
        if (bigFont != nullptr)
            atlas->RemoveFont(bigFont);
        (void)pools;
    }
}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("imslug_stress: M4 stress suite\n");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(4096.0f, 3072.0f);
    io.IniFilename = nullptr;
    Config config;
    config.DebugDrawBounds = true;  // placement validation on every stress glyph
    TEST_CHECK(ImSlug::Init(config));
    ImSlugSetRenderIO(&g_FakeIO);
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = FakeResetRenderState;

    ImFont* font = ImSlug::AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), 16.0f);
    TEST_CHECK(font != nullptr);

    TestFiveThousandGlyphs();
    TestWrappedTextAndEdgeGlyphs();
    TestCjk();
    TestDegenerateOutlines();
    TestPoolLimits();
    TestRepackImmunity();

    ImSlugSetRenderIO(nullptr);
    ImSlug::Shutdown();
    ImGui::DestroyContext();
    return imslug_test::Finish("imslug_stress");
}
