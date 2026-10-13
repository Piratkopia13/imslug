// imslug M3 tests — the ImGui-facing layer: custom ImFontLoader, glyph handle
// encoding/decoding, fine-clip reconstruction and the draw-data pre-pass.
//
// This is a test-only build: Dear ImGui is linked from the shared target and
// imgui_internal.h is used freely (same vocabulary as imslug_imgui_adapter.cpp).
// No Vulkan device is needed — the pre-pass writes into a fake RenderIO, so
// this suite runs everywhere.
//
// Ground truth for placement checks: an *independent* ImSlugFont built from the
// same file in the test (its own records/pools), the ImFontGlyph metrics ImGui
// baked through our loader (which the parity test pins against ImGui's own stb
// loader) and ImFontBaked::Ascent (ImGui's line-top -> baseline offset).
// Expected instance rect = pen + record em bbox * pixelsPerEm with
// pen = (truncated) AddText position (+ Ascent for Y).

#include "imslug.h"

#include "imslug_imgui_adapter.h"
#include "imslug_internal.h"

#include "test_util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ImSlug;

namespace
{
    // ---------------------------------------------------------------------
    // Fake backend (RenderIO) + fake stock-backend hooks
    // ---------------------------------------------------------------------
    const uint32_t kFakeCapacity = 8192;
    const uint32_t kMaxReferenceInstances = 512;
    ImSlugGlyphInstance g_FakeInstances[kFakeCapacity];
    uint32_t g_FakeUsed = 0;
    uint32_t g_FakeFrameIndex = 0;
    int g_FakeDrawCalls = 0;
    ImSlugBatch g_FakeLastBatch;

    uint32_t FakeGetFrameIndex()
    {
        return g_FakeFrameIndex;
    }

    ImSlugGlyphInstance* FakeReserve(uint32_t count, uint32_t* outFirst)
    {
        if (g_FakeUsed + count > kFakeCapacity)
            return nullptr;
        *outFirst = g_FakeUsed;
        g_FakeUsed += count;
        return g_FakeInstances + (g_FakeUsed - count);
    }

    void FakeDraw(const ImSlugBatch* batch)
    {
        g_FakeDrawCalls++;
        g_FakeLastBatch = *batch;
    }

    const ImSlugRenderIO g_FakeIO = { "test", FakeGetFrameIndex, FakeReserve, FakeDraw };

    // ImGui_ImplVulkan_Init publishes these in the real app; the pre-pass
    // refuses to run without them (R5). Each callback writes its own counter:
    // empty identical functions would be folded into one address by the linker
    // (/OPT:ICF), which would make the identity checks meaningless.
    int g_FakeCallbackSink[4];
    void FakeResetRenderState(const ImDrawList*, const ImDrawCmd*)
    {
        g_FakeCallbackSink[0]++;
    }
    void FakeSetSamplerLinear(const ImDrawList*, const ImDrawCmd*)
    {
        g_FakeCallbackSink[1]++;
    }
    void FakeSetSamplerNearest(const ImDrawList*, const ImDrawCmd*)
    {
        g_FakeCallbackSink[2]++;
    }
    void FakeThirdPartyCallback(const ImDrawList*, const ImDrawCmd*)
    {
        g_FakeCallbackSink[3]++;
    }

    void InstallFakeBackendHooks()
    {
        ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
        platformIO.DrawCallback_ResetRenderState = FakeResetRenderState;
        platformIO.DrawCallback_SetSamplerLinear = FakeSetSamplerLinear;
        platformIO.DrawCallback_SetSamplerNearest = FakeSetSamplerNearest;
    }

    void ResetFake()
    {
        g_FakeUsed = 0;
        g_FakeDrawCalls = 0;
        memset(&g_FakeLastBatch, 0, sizeof(g_FakeLastBatch));
    }

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------
    bool Nearly(float a, float b, float tolerance)
    {
        return std::fabs(a - b) <= tolerance;
    }

    // Dispatches every Slug callback the way a backend would (geometry commands
    // are irrelevant here); returns the number of batch draws.
    int DispatchSlugCallbacks(const ImDrawData* drawData)
    {
        int draws = 0;
        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        {
            const ImDrawList* list = drawData->CmdLists[listIndex];
            for (int cmdIndex = 0; cmdIndex < list->CmdBuffer.Size; ++cmdIndex)
            {
                const ImDrawCmd& cmd = list->CmdBuffer[cmdIndex];
                if (cmd.UserCallback == ImSlug::DrawCallback)
                {
                    cmd.UserCallback(list, &cmd);
                    draws++;
                }
            }
        }
        return draws;
    }

    // Counts indices whose vertex UVs decode as Slug handles inside commands
    // that are drawn as geometry (they would sample the atlas at garbage UVs).
    int CountSlugVerticesInGeometry(const ImDrawList* list)
    {
        int count = 0;
        for (int cmdIndex = 0; cmdIndex < list->CmdBuffer.Size; ++cmdIndex)
        {
            const ImDrawCmd& cmd = list->CmdBuffer[cmdIndex];
            if (cmd.UserCallback != nullptr)
                continue;
            for (unsigned int i = 0; i < cmd.ElemCount; ++i)
            {
                const unsigned int idx = list->IdxBuffer[cmd.IdxOffset + i];
                const ImDrawVert& v = list->VtxBuffer[idx + cmd.VtxOffset];
                ImSlugDecodedVertex decoded;
                if (ImSlugAdapterDecodeVertex(v.uv.x, v.uv.y, &decoded))
                    count++;
            }
        }
        return count;
    }

    bool HasCallbackResetPairs(const ImDrawList* list, ImDrawCallback resetCallback)
    {
        for (int i = 0; i + 1 < list->CmdBuffer.Size; ++i)
            if (list->CmdBuffer[i].UserCallback == ImSlug::DrawCallback &&
                list->CmdBuffer[i + 1].UserCallback == resetCallback)
                return true;
        return false;
    }

    // Test-side core font (independent records/pools, same outlines).
    ImSlugFont* g_TestFont = nullptr;
    const float g_TestFontSize = 16.0f;

    struct ExpectedGlyph
    {
        const ImSlugGlyphRecord* Record;
        float PixelsPerEm;
    };

    bool GetExpectedGlyph(unsigned int codepoint, float sizePx, ExpectedGlyph* out)
    {
        ImSlugFontMetrics fontMetrics;
        ImSlugGetFontMetrics(g_TestFont, sizePx, &fontMetrics);
        out->PixelsPerEm = fontMetrics.PixelsPerEm;
        ImSlugGlyphMetrics metrics;
        if (!ImSlugGetGlyphMetrics(g_TestFont, codepoint, sizePx, &metrics))
            return false;
        ImSlugPools* pools = ImSlugGetPools();
        if (pools == nullptr)
            return false;
        return ImSlugGetGlyphRecord(g_TestFont, pools, codepoint, &out->Record) == ImSlugBuildStatus_Ok;
    }

    void CheckPlacementAgainstGroundTruth(const ImSlugGlyphInstance* instances, uint32_t count, const char* text,
                                          ImFont* font, float sizePx, const ImVec2& pos, bool noPixelSnap,
                                          float tolerance)
    {
        ImFontBaked* baked = font->GetFontBaked(sizePx);
        const float scale = sizePx / baked->Size;
        // The frozen instance builder's Y is the baseline; ImGui's AddText
        // position is the line top, `baked->Ascent` pixels above the baseline.
        float penX = noPixelSnap ? pos.x : (float)(int)pos.x;
        const float penY = (noPixelSnap ? pos.y : (float)(int)pos.y) + baked->Ascent * scale;
        uint32_t instanceIndex = 0;
        for (const char* p = text; *p != '\0'; ++p)
        {
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)(unsigned char)*p);
            if (glyph == nullptr)
                continue;
            if (glyph->Visible)
            {
                ExpectedGlyph expected;
                TEST_CHECK(GetExpectedGlyph((unsigned int)(unsigned char)*p, baked->Size, &expected));
                if (instanceIndex >= count)
                {
                    TEST_CHECKF(false, "missing instance %u for '%c'", instanceIndex, *p);
                    break;
                }
                const ImSlugGlyphInstance& instance = instances[instanceIndex];
                const float ppem = expected.PixelsPerEm * scale;
                const float expMinX = penX + expected.Record->MinX * ppem;
                const float expMaxX = penX + expected.Record->MaxX * ppem;
                const float expMinY = penY - expected.Record->MaxY * ppem;
                const float expMaxY = penY - expected.Record->MinY * ppem;
                TEST_CHECKF(Nearly(instance.ScreenMinX, expMinX, tolerance),
                            "instance %u ('%c') ScreenMinX %.4f vs %.4f", instanceIndex, *p, instance.ScreenMinX,
                            expMinX);
                TEST_CHECKF(Nearly(instance.ScreenMaxX, expMaxX, tolerance),
                            "instance %u ('%c') ScreenMaxX %.4f vs %.4f", instanceIndex, *p, instance.ScreenMaxX,
                            expMaxX);
                TEST_CHECKF(Nearly(instance.ScreenMinY, expMinY, tolerance),
                            "instance %u ('%c') ScreenMinY %.4f vs %.4f", instanceIndex, *p, instance.ScreenMinY,
                            expMinY);
                TEST_CHECKF(Nearly(instance.ScreenMaxY, expMaxY, tolerance),
                            "instance %u ('%c') ScreenMaxY %.4f vs %.4f", instanceIndex, *p, instance.ScreenMaxY,
                            expMaxY);
                TEST_CHECKF(instance.EmMinX == expected.Record->MinX && instance.EmMaxX == expected.Record->MaxX &&
                                instance.EmMinY == expected.Record->MinY && instance.EmMaxY == expected.Record->MaxY,
                            "instance %u ('%c') em bbox mismatch", instanceIndex, *p);
                instanceIndex++;
            }
            penX += glyph->AdvanceX * scale;
        }
        TEST_CHECKF(instanceIndex == count, "instance count %u vs expected %u", count, instanceIndex);
    }

    // Same check for clipped runs: ImGui's CPU fine clip *drops* glyphs whose
    // span misses the clip rect, so instances must be paired with the text
    // glyphs by identity (record em bbox) and proximity instead of by index.
    void CheckClippedPlacement(const ImSlugGlyphInstance* instances, uint32_t count, const char* text, ImFont* font,
                               float sizePx, const ImVec2& pos, float tolerance)
    {
        ImFontBaked* baked = font->GetFontBaked(sizePx);
        const float scale = sizePx / baked->Size;
        const float penY = (float)(int)pos.y + baked->Ascent * scale;
        int glyphCursor = 0;
        float penX = (float)(int)pos.x;
        for (uint32_t i = 0; i < count; ++i)
        {
            const ImSlugGlyphInstance& instance = instances[i];
            bool matched = false;
            while (text[glyphCursor] != '\0' && !matched)
            {
                const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)(unsigned char)text[glyphCursor]);
                if (glyph == nullptr)
                {
                    glyphCursor++;
                    continue;
                }
                if (glyph->Visible)
                {
                    ExpectedGlyph expected;
                    if (GetExpectedGlyph((unsigned int)(unsigned char)text[glyphCursor], baked->Size, &expected))
                    {
                        const float ppem = expected.PixelsPerEm * scale;
                        const float expMinX = penX + expected.Record->MinX * ppem;
                        const float expMinY = penY - expected.Record->MaxY * ppem;
                        const float expWidth = (expected.Record->MaxX - expected.Record->MinX) * ppem;
                        const float expHeight = (expected.Record->MaxY - expected.Record->MinY) * ppem;
                        if (instance.EmMinX == expected.Record->MinX && instance.EmMaxX == expected.Record->MaxX &&
                            instance.EmMinY == expected.Record->MinY && instance.EmMaxY == expected.Record->MaxY &&
                            Nearly(instance.ScreenMinX, expMinX, tolerance) &&
                            Nearly(instance.ScreenMinY, expMinY, tolerance))
                        {
                            TEST_CHECKF(Nearly(instance.ScreenMaxX, expMinX + expWidth, tolerance),
                                        "clipped instance %u ('%c') ScreenMaxX %.4f vs %.4f", i, text[glyphCursor],
                                        instance.ScreenMaxX, expMinX + expWidth);
                            TEST_CHECKF(Nearly(instance.ScreenMaxY, expMinY + expHeight, tolerance),
                                        "clipped instance %u ('%c') ScreenMaxY %.4f vs %.4f", i, text[glyphCursor],
                                        instance.ScreenMaxY, expMinY + expHeight);
                            matched = true;
                        }
                    }
                }
                penX += glyph->AdvanceX * scale;
                glyphCursor++;
            }
            TEST_CHECKF(matched, "clipped instance %u (%.2f,%.2f)-(%.2f,%.2f) matches no text glyph", i,
                        instance.ScreenMinX, instance.ScreenMinY, instance.ScreenMaxX, instance.ScreenMaxY);
        }
    }

    void OpenTestWindow(const char* name, const ImVec2& pos = ImVec2(0.0f, 0.0f),
                        const ImVec2& size = ImVec2(600.0f, 400.0f))
    {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
    }

    // The text used by the placement and DPI tests.
    // Text through a widget-sized layout: this uses g.FontSize, which includes
    // style.FontScaleMain/FontScaleDpi (an explicit AddText size would not).
    void drawTextHelper(ImFont* font, const ImVec2& pos)
    {
        ImGui::SetCursorScreenPos(pos);
        ImGui::TextUnformatted("Hello Wg");
        (void)font;
    }

    // One full frame: build, render, pre-pass, dispatch callbacks. The caller
    // does the NewFrame + UI building; RunFrame finishes the frame.
    ImDrawData* RunFrame(uint32_t frameIndex)
    {
        ResetFake();
        g_FakeFrameIndex = frameIndex;
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        ImSlug::PreprocessDrawData(drawData);
        DispatchSlugCallbacks(drawData);
        return drawData;
    }
}  // namespace

// ---------------------------------------------------------------------------
// 1. The loader bakes ImGui-parity metrics and rect-less glyphs (no bitmaps).
// ---------------------------------------------------------------------------
static void TestLoaderMetricsAndRects()
{
    std::printf("[test] loader metrics + no glyph bitmaps\n");

    ImFont* slugFont = ImSlug::AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), g_TestFontSize);
    TEST_CHECK(slugFont != nullptr);
    // The stock comparison uses the same metric-neutral configuration the Slug
    // loader enforces (Oversample* = 1: ImGui's auto-oversampling shifts small
    // size boxes by up to half a pixel).
    ImFontConfig stockConfig;
    stockConfig.OversampleH = 1;
    stockConfig.OversampleV = 1;
    ImFont* stockFont =
        ImGui::GetIO().Fonts->AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Roboto-Regular.ttf"), 16.0f, &stockConfig);
    TEST_CHECK(stockFont != nullptr);
    TEST_CHECK(ImSlug::IsRegistered(slugFont));
    TEST_CHECK(!ImSlug::IsRegistered(stockFont));
    TEST_CHECK(!ImSlug::IsRegistered(nullptr));

    const unsigned int codepoints[] = { 'A', 'g', 'W', 'i', '0', '.', ' ', 0xE9 };
    const float sizes[] = { 9.0f, 16.0f, 32.0f, 48.0f };
    int compared = 0;
    for (float size : sizes)
    {
        ImFontBaked* baked = slugFont->GetFontBaked(size);
        ImFontBaked* stockBaked = stockFont->GetFontBaked(size);
        TEST_CHECK(baked != nullptr && stockBaked != nullptr);
        if (baked == nullptr || stockBaked == nullptr)
            continue;
        for (unsigned int codepoint : codepoints)
        {
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)codepoint);
            const ImFontGlyph* stock = stockBaked->FindGlyph((ImWchar)codepoint);
            TEST_CHECK(glyph != nullptr && stock != nullptr);
            if (glyph == nullptr || stock == nullptr)
                continue;

            TEST_CHECKF(glyph->AdvanceX == stock->AdvanceX, "AdvanceX %.6f vs %.6f (U+%04X @ %.0f)", glyph->AdvanceX,
                        stock->AdvanceX, codepoint, size);
            TEST_CHECKF(glyph->X0 == stock->X0 && glyph->Y0 == stock->Y0 && glyph->X1 == stock->X1 &&
                            glyph->Y1 == stock->Y1,
                        "box (%.3f %.3f %.3f %.3f) vs (%.3f %.3f %.3f %.3f) (U+%04X @ %.0f)", glyph->X0, glyph->Y0,
                        glyph->X1, glyph->Y1, stock->X0, stock->Y0, stock->X1, stock->Y1, codepoint, size);
            TEST_CHECK(glyph->Visible == stock->Visible);
            TEST_CHECK(!glyph->Colored);
            if (glyph->Visible)
                TEST_CHECKF(glyph->PackId == ImFontAtlasRectId_Invalid, "PackId %d (U+%04X @ %.0f)", glyph->PackId,
                            codepoint, size);
            compared++;
        }
    }
    std::printf("  %d glyph/size comparisons\n", compared);

    // From here on only Slug glyphs are baked: no rect may ever be allocated.
    const ImVec2i texSizeBefore = ImVec2i(ImGui::GetIO().Fonts->TexData->Width, ImGui::GetIO().Fonts->TexData->Height);
    const int packedRectsBefore = ImGui::GetIO().Fonts->Builder->RectsPackedCount;

    ImFontBaked* baked16 = slugFont->GetFontBaked(16.0f);
    int visible = 0;
    for (unsigned int codepoint = 0x20; codepoint < 0x100; ++codepoint)
    {
        const ImFontGlyph* glyph = baked16->FindGlyph((ImWchar)codepoint);
        if (glyph != nullptr && glyph->Visible)
        {
            visible++;
            TEST_CHECKF(glyph->PackId == ImFontAtlasRectId_Invalid, "PackId %d (U+%04X)", glyph->PackId, codepoint);
        }
    }
    std::printf("  baked %d visible Latin glyphs (0 bitmaps)\n", visible);
    const ImVec2i texSizeAfter = ImVec2i(ImGui::GetIO().Fonts->TexData->Width, ImGui::GetIO().Fonts->TexData->Height);
    const int packedRectsAfter = ImGui::GetIO().Fonts->Builder->RectsPackedCount;
    TEST_CHECKF(texSizeAfter.x == texSizeBefore.x && texSizeAfter.y == texSizeBefore.y,
                "atlas texture grew for Slug glyphs: %dx%d -> %dx%d", texSizeBefore.x, texSizeBefore.y, texSizeAfter.x,
                texSizeAfter.y);
    TEST_CHECKF(packedRectsAfter == packedRectsBefore, "packed rects %d -> %d", packedRectsBefore, packedRectsAfter);

    // The synthetic ellipsis is a Slug glyph too: ImGui's autocombining path
    // (which needs a packed '.' rect) must not run for Slug fonts.
    ImFontBaked* baked32 = slugFont->GetFontBaked(32.0f);
    TEST_CHECKF(slugFont->EllipsisAutoBake == false, "EllipsisAutoBake must be off for Slug fonts");
    const ImFontGlyph* ellipsis = baked32->FindGlyph(slugFont->EllipsisChar);
    TEST_CHECK(ellipsis != nullptr);
    if (ellipsis != nullptr)
    {
        TEST_CHECKF(ellipsis->PackId == ImFontAtlasRectId_Invalid, "ellipsis PackId %d", ellipsis->PackId);
        TEST_CHECK(ellipsis->Visible);
        TEST_CHECKF(ellipsis->X1 - ellipsis->X0 > 2.0f, "ellipsis box width %.2f", ellipsis->X1 - ellipsis->X0);
        ImSlugDecodedVertex decoded;
        TEST_CHECK(ImSlugAdapterDecodeVertex(ellipsis->U0, ellipsis->V0, &decoded));
        TEST_CHECK(decoded.Slab != nullptr);
        std::printf("  synthetic ellipsis: U+%04X, box %.2fx%.2f px, advance %.2f\n",
                    (unsigned int)slugFont->EllipsisChar, ellipsis->X1 - ellipsis->X0, ellipsis->Y1 - ellipsis->Y0,
                    ellipsis->AdvanceX);
    }
    TEST_CHECKF(ImGui::GetIO().Fonts->TexData->Width == texSizeBefore.x, "texture width after ellipsis");
    TEST_CHECKF(ImGui::GetIO().Fonts->Builder->RectsPackedCount == packedRectsBefore,
                "packed rects after ellipsis: %d vs %d", ImGui::GetIO().Fonts->Builder->RectsPackedCount,
                packedRectsBefore);

    Statistics stats;
    ImSlug::GetStatistics(&stats);
    std::printf("  live slabs: %u\n", stats.GlyphSlabs);
    TEST_CHECK(stats.GlyphSlabs > 0);
}

// ---------------------------------------------------------------------------
// 2. Handle encoding: decode round-trip + fine-clip interpolation.
// ---------------------------------------------------------------------------
static void TestHandleRoundTrip()
{
    std::printf("[test] handle encode/decode + fine-clip interpolation\n");
    ImFont* slugFont = ImGui::GetIO().Fonts->Fonts[0];
    ImFontBaked* baked = slugFont->GetFontBaked(32.0f);
    TEST_CHECK(baked != nullptr);

    const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)'A');
    TEST_CHECK(glyph != nullptr && glyph->Visible);
    if (glyph == nullptr || !glyph->Visible)
        return;

    ImSlugDecodedVertex tl, tr, br, bl;
    TEST_CHECK(ImSlugAdapterDecodeVertex(glyph->U0, glyph->V0, &tl));
    TEST_CHECK(ImSlugAdapterDecodeVertex(glyph->U1, glyph->V0, &tr));
    TEST_CHECK(ImSlugAdapterDecodeVertex(glyph->U1, glyph->V1, &br));
    TEST_CHECK(ImSlugAdapterDecodeVertex(glyph->U0, glyph->V1, &bl));
    TEST_CHECK(tl.Slab != nullptr && tl.Slab == tr.Slab && tr.Slab == br.Slab && br.Slab == bl.Slab);
    TEST_CHECK(Nearly(tl.FracX, 0.0f, 1e-6f) && Nearly(tl.FracY, 0.0f, 1e-6f));
    TEST_CHECK(Nearly(tr.FracX, 1.0f, 1e-6f) && Nearly(tr.FracY, 0.0f, 1e-6f));
    TEST_CHECK(Nearly(br.FracX, 1.0f, 1e-6f) && Nearly(br.FracY, 1.0f, 1e-6f));
    TEST_CHECK(Nearly(bl.FracX, 0.0f, 1e-6f) && Nearly(bl.FracY, 1.0f, 1e-6f));
    TEST_CHECK(tl.Slab->Record != nullptr);
    TEST_CHECK(tl.Slab->X0 == glyph->X0 && tl.Slab->X1 == glyph->X1);
    TEST_CHECK(tl.Slab->Ascent == baked->Ascent);

    // ImGui's exact CPU fine-clip interpolation (imgui_draw.cpp 5947-5968).
    const float x1 = 10.0f, x2 = 30.0f, y1 = 20.0f, y2 = 50.0f;
    struct FineClipExpectation
    {
        float Clip[4];
        float FracTL[2];  // -1 = unchanged (checked against the unclipped corner)
        float FracBR[2];
    };
    const FineClipExpectation expectations[4] = {
        { { x1 + 4.0f, y1, x2, y2 }, { 4.0f / (x2 - x1), 0.0f }, { -1.0f, -1.0f } },         // left
        { { x1, y1 + 3.0f, x2, y2 }, { 0.0f, 3.0f / (y2 - y1) }, { -1.0f, -1.0f } },         // top
        { { x1, y1, x2 - 5.0f, y2 }, { -1.0f, -1.0f }, { 1.0f - 5.0f / (x2 - x1), 1.0f } },  // right
        { { x1, y1, x2, y2 - 7.0f }, { -1.0f, -1.0f }, { 1.0f, 1.0f - 7.0f / (y2 - y1) } },  // bottom
    };
    for (int side = 0; side < 4; ++side)
    {
        float u1 = glyph->U0, v1 = glyph->V0, u2 = glyph->U1, v2 = glyph->V1;
        const float cx = expectations[side].Clip[0], cy = expectations[side].Clip[1];
        const float cz = expectations[side].Clip[2], cw = expectations[side].Clip[3];
        if (x1 < cx)
            u1 = u1 + (1.0f - (x2 - cx) / (x2 - x1)) * (u2 - u1);
        if (y1 < cy)
            v1 = v1 + (1.0f - (y2 - cy) / (y2 - y1)) * (v2 - v1);
        if (x2 > cz)
            u2 = u1 + ((cz - x1) / (x2 - x1)) * (u2 - u1);
        if (y2 > cw)
            v2 = v1 + ((cw - y1) / (y2 - y1)) * (v2 - v1);

        ImSlugDecodedVertex clippedTL, clippedBR;
        TEST_CHECK(ImSlugAdapterDecodeVertex(u1, v1, &clippedTL));
        TEST_CHECK(ImSlugAdapterDecodeVertex(u2, v2, &clippedBR));
        TEST_CHECK(clippedTL.Slab == tl.Slab && clippedBR.Slab == tl.Slab);
        const float expectedTLX = expectations[side].FracTL[0] < 0.0f ? 0.0f : expectations[side].FracTL[0];
        const float expectedTLY = expectations[side].FracTL[1] < 0.0f ? 0.0f : expectations[side].FracTL[1];
        const float expectedBRX = expectations[side].FracBR[0] < 0.0f ? 1.0f : expectations[side].FracBR[0];
        const float expectedBRY = expectations[side].FracBR[1] < 0.0f ? 1.0f : expectations[side].FracBR[1];
        TEST_CHECKF(Nearly(clippedTL.FracX, expectedTLX, 3e-3f), "side %d TL fracX %.5f vs %.5f", side, clippedTL.FracX,
                    expectedTLX);
        TEST_CHECKF(Nearly(clippedTL.FracY, expectedTLY, 3e-3f), "side %d TL fracY %.5f vs %.5f", side, clippedTL.FracY,
                    expectedTLY);
        TEST_CHECKF(Nearly(clippedBR.FracX, expectedBRX, 3e-3f), "side %d BR fracX %.5f vs %.5f", side, clippedBR.FracX,
                    expectedBRX);
        TEST_CHECKF(Nearly(clippedBR.FracY, expectedBRY, 3e-3f), "side %d BR fracY %.5f vs %.5f", side, clippedBR.FracY,
                    expectedBRY);
    }

    // Rejection: atlas UVs, non-finite values, mixed slots, garbage.
    ImSlugDecodedVertex decoded;
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(0.0f, 0.0f, &decoded), "atlas UV (0,0) must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(0.5f, 0.5f, &decoded), "atlas UV (0.5,0.5) must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(1.0f, 1.0f, &decoded), "atlas UV (1,1) must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(NAN, glyph->V0, &decoded), "NaN must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(glyph->U0, glyph->V1 * 3.0f, &decoded), "mixed slot must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(1e30f, 1e30f, &decoded), "huge values must not decode");
    TEST_CHECKF(!ImSlugAdapterDecodeVertex(-1.0f, -1.0f, &decoded), "negative UVs must not decode");
}

// ---------------------------------------------------------------------------
// 3. Pre-pass: command structure, geometry preservation, batch dispatch.
// ---------------------------------------------------------------------------
static void TestPrepassStructure()
{
    std::printf("[test] pre-pass structure + callback dispatch\n");
    ImFont* slugFont = ImGui::GetIO().Fonts->Fonts[0];
    ImFont* stockFont = ImGui::GetIO().Fonts->Fonts[1];
    const ImDrawCallback resetCallback = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
    TEST_CHECK(resetCallback != nullptr);

    ImGui::NewFrame();
    OpenTestWindow("structure");
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(30.0f, 200.0f), ImVec2(120.0f, 230.0f), IM_COL32(200, 60, 60, 255));
    drawList->AddText(slugFont, g_TestFontSize, ImVec2(20.0f, 20.0f), IM_COL32(255, 255, 255, 255), "Hamburgefonstiv");
    drawList->AddCircleFilled(ImVec2(300.0f, 100.0f), 20.0f, IM_COL32(60, 200, 60, 255));
    drawList->AddText(slugFont, g_TestFontSize, ImVec2(20.0f, 120.0f), IM_COL32(255, 255, 255, 255), "after a shape");
    // A pre-existing third-party callback: passes through untouched and splits
    // the text runs (an app-side AddCallback, e.g. from a plot library).
    drawList->AddCallback(FakeThirdPartyCallback, nullptr);
    drawList->AddText(slugFont, g_TestFontSize, ImVec2(20.0f, 160.0f), IM_COL32(255, 255, 255, 255), "after callback");
    // Mixed fonts: stock text must keep its atlas path (no Slug callback).
    drawList->AddText(stockFont, 16.0f, ImVec2(20.0f, 240.0f), IM_COL32(255, 255, 255, 255), "stock atlas text");
    ImGui::End();

    ImDrawData* drawData = RunFrame(0);
    TEST_CHECK(drawData != nullptr && drawData->CmdListsCount > 0);
    if (drawData == nullptr || drawData->CmdListsCount == 0)
        return;

    int callbackCommands = 0;
    int resetCommands = 0;
    int slugVerticesInGeometry = 0;
    int thirdPartyCallbacks = 0;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
    {
        const ImDrawList* list = drawData->CmdLists[listIndex];
        TEST_CHECK(HasCallbackResetPairs(list, resetCallback));
        for (int cmdIndex = 0; cmdIndex < list->CmdBuffer.Size; ++cmdIndex)
        {
            const ImDrawCmd& cmd = list->CmdBuffer[cmdIndex];
            if (cmd.UserCallback == ImSlug::DrawCallback)
                callbackCommands++;
            else if (cmd.UserCallback == resetCallback)
                resetCommands++;
            else if (cmd.UserCallback != nullptr)
                thirdPartyCallbacks++;
        }
        slugVerticesInGeometry += CountSlugVerticesInGeometry(list);
    }
    TEST_CHECKF(callbackCommands == 3, "%d Slug callbacks (expected 3 runs: two texts split by the shape callback)",
                callbackCommands);
    TEST_CHECKF(callbackCommands == resetCommands, "callback %d vs reset %d commands", callbackCommands, resetCommands);
    TEST_CHECKF(slugVerticesInGeometry == 0, "%d Slug vertices left in geometry commands", slugVerticesInGeometry);
    TEST_CHECKF(thirdPartyCallbacks == 1, "%d third-party callbacks (expected the app's AddCallback)",
                thirdPartyCallbacks);

    Statistics stats;
    ImSlug::GetStatistics(&stats);
    std::printf("  command lists %d, batches %u, instances %u, dropped %u, bounds mismatches %u\n",
                drawData->CmdListsCount, stats.LiveBatches, stats.Instances, stats.DroppedRuns, stats.BoundsMismatches);
    TEST_CHECK(stats.Instances > 30);
    TEST_CHECK(stats.DroppedRuns == 0);
    TEST_CHECK(stats.BoundsMismatches == 0);

    // The backend dispatch (inside RunFrame) already called every batch draw.
    TEST_CHECKF(g_FakeDrawCalls == callbackCommands, "%d draw calls vs %d callbacks", g_FakeDrawCalls,
                callbackCommands);
    TEST_CHECK(g_FakeLastBatch.Magic == kBatchMagic);
    TEST_CHECK(g_FakeLastBatch.InstanceCount > 0);
    TEST_CHECK(g_FakeLastBatch.InstanceCount <= g_FakeUsed);
}

// ---------------------------------------------------------------------------
// 4. Placement: ground truth, clipping independence, no-pixel-snap, squeeze.
// ---------------------------------------------------------------------------
static void TestPlacement()
{
    std::printf("[test] placement reconstruction\n");
    ImFont* slugFont = ImGui::GetIO().Fonts->Fonts[0];
    const char* text = "Hello Wg";
    const float size = g_TestFontSize;
    const ImVec2 textPos(17.0f, 33.0f);

    // --- unclipped reference -------------------------------------------------
    ImGui::NewFrame();
    OpenTestWindow("placement");
    ImGui::GetWindowDrawList()->AddText(slugFont, size, textPos, IM_COL32(255, 255, 255, 255), text);
    ImGui::End();
    RunFrame(1);
    const uint32_t referenceCount = g_FakeUsed;
    TEST_CHECK(referenceCount > 0 && referenceCount <= kMaxReferenceInstances);
    if (referenceCount == 0 || referenceCount > kMaxReferenceInstances)
        return;
    TEST_CHECKF(g_FakeDrawCalls == 1, "%d batch draws for one run", g_FakeDrawCalls);
    ImSlugGlyphInstance reference[kMaxReferenceInstances];
    memcpy(reference, g_FakeInstances, (size_t)referenceCount * sizeof(ImSlugGlyphInstance));
    CheckPlacementAgainstGroundTruth(reference, referenceCount, text, slugFont, size, textPos, /*noPixelSnap=*/false,
                                     0.02f);
    // The unclipped scissor is the text extent.
    TEST_CHECK(g_FakeLastBatch.ClipMinX >= textPos.x - 1.0f);
    TEST_CHECK(g_FakeLastBatch.ClipMaxX <= textPos.x + 200.0f);

    // --- window clipping (scissor path): placements must not move ------------
    ImGui::NewFrame();
    OpenTestWindow("clip-window");
    ImGui::SetNextWindowPos(textPos, ImGuiCond_Always);
    ImGui::BeginChild("inner", ImVec2(30.0f, 18.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    // Text positions are display coordinates (like every ImDrawList call).
    ImGui::GetWindowDrawList()->AddText(slugFont, size, textPos, IM_COL32(255, 255, 255, 255), text);
    ImGui::EndChild();
    ImGui::End();
    RunFrame(2);
    TEST_CHECKF(g_FakeUsed > 0 && g_FakeUsed <= referenceCount, "%u instances in the clipped child", g_FakeUsed);
    for (uint32_t i = 0; i < g_FakeUsed; ++i)
        for (int field = 0; field < 4; ++field)
        {
            const float actual = (&g_FakeInstances[i].ScreenMinX)[field];
            const float expected = (&reference[i].ScreenMinX)[field];
            TEST_CHECKF(Nearly(actual, expected, 0.02f), "window-clipped instance %u field %d: %.4f vs %.4f", i, field,
                        actual, expected);
        }
    TEST_CHECK(g_FakeLastBatch.ClipMinX >= textPos.x - 0.5f && g_FakeLastBatch.ClipMaxX <= textPos.x + 30.5f);

    // --- CPU fine clip: all four sides plus a fully squeezed glyph -----------
    struct FineClipCase
    {
        float MinX, MinY, MaxX, MaxY;
        const char* Name;
        bool Squeezed;
    };
    const FineClipCase fineClips[] = {
        { textPos.x + 1.0f, textPos.y, textPos.x + 200.0f, textPos.y + 200.0f, "left", false },
        { textPos.x, textPos.y + 2.0f, textPos.x + 200.0f, textPos.y + 200.0f, "top", false },
        { textPos.x, textPos.y, textPos.x + 30.0f, textPos.y + 200.0f, "right", false },
        { textPos.x, textPos.y, textPos.x + 200.0f, textPos.y + 6.0f, "bottom", false },
        { textPos.x + 3.0f, textPos.y + 1.0f, textPos.x + 27.0f, textPos.y + 9.0f, "all four", false },
        // Narrower and shorter than a single glyph: every side is cropped and
        // both fraction spans collapse to a sliver (the noisy path).
        { textPos.x + 4.0f, textPos.y + 3.0f, textPos.x + 8.0f, textPos.y + 7.0f, "squeezed", true },
    };
    for (const FineClipCase& clipCase : fineClips)
    {
        ImGui::NewFrame();
        OpenTestWindow("clip-fine");
        const ImVec4 fineClip(clipCase.MinX, clipCase.MinY, clipCase.MaxX, clipCase.MaxY);
        ImGui::GetWindowDrawList()->AddText(slugFont, size, textPos, IM_COL32(255, 255, 255, 255), text, nullptr, 0.0f,
                                            &fineClip);
        ImGui::End();
        RunFrame(3 + (uint32_t)(&clipCase - fineClips));

        TEST_CHECKF(g_FakeUsed > 0, "fine clip '%s' produced no instances", clipCase.Name);
        TEST_CHECKF(g_FakeUsed <= referenceCount, "fine clip '%s': %u instances > reference %u", clipCase.Name,
                    g_FakeUsed, referenceCount);
        CheckClippedPlacement(g_FakeInstances, g_FakeUsed, text, slugFont, size, textPos,
                              clipCase.Squeezed ? 1.5f : 0.05f);
        // The run scissor must stay inside the fine clip rectangle.
        TEST_CHECKF(g_FakeLastBatch.ClipMinX >= clipCase.MinX - 0.5f, "'%s' scissor left %.2f < %.2f", clipCase.Name,
                    g_FakeLastBatch.ClipMinX, clipCase.MinX);
        TEST_CHECKF(g_FakeLastBatch.ClipMaxX <= clipCase.MaxX + 0.5f, "'%s' scissor right %.2f > %.2f", clipCase.Name,
                    g_FakeLastBatch.ClipMaxX, clipCase.MaxX);
        TEST_CHECKF(g_FakeLastBatch.ClipMinY >= clipCase.MinY - 0.5f, "'%s' scissor top %.2f < %.2f", clipCase.Name,
                    g_FakeLastBatch.ClipMinY, clipCase.MinY);
        TEST_CHECKF(g_FakeLastBatch.ClipMaxY <= clipCase.MaxY + 0.5f, "'%s' scissor bottom %.2f > %.2f", clipCase.Name,
                    g_FakeLastBatch.ClipMaxY, clipCase.MaxY);
    }

    // --- TextNoPixelSnap with fractional positions ---------------------------
    const ImVec2 fractionalPos(21.375f, 44.625f);
    ImGui::NewFrame();
    OpenTestWindow("no-pixel-snap");
    ImGui::GetWindowDrawList()->Flags |= ImDrawListFlags_TextNoPixelSnap;
    ImGui::GetWindowDrawList()->AddText(slugFont, size, fractionalPos, IM_COL32(255, 255, 255, 255), text);
    ImGui::End();
    RunFrame(20);
    TEST_CHECK(g_FakeUsed == referenceCount);
    CheckPlacementAgainstGroundTruth(g_FakeInstances, g_FakeUsed, text, slugFont, size, fractionalPos,
                                     /*noPixelSnap=*/true, 0.02f);

    // --- RenderChar with the (synthetic) ellipsis ---------------------------
    ImGui::NewFrame();
    OpenTestWindow("ellipsis");
    ImDrawList* ellipsisList = ImGui::GetWindowDrawList();
    ellipsisList->AddText(slugFont, size, ImVec2(20.0f, 20.0f), IM_COL32(255, 255, 255, 255), "abc");
    ellipsisList->AddText(slugFont, size, ImVec2(20.0f, 40.0f), IM_COL32(255, 255, 255, 255), "...");
    const ImVec2 ellipsisPos(20.0f, 60.0f);
    slugFont->RenderChar(ellipsisList, size, ellipsisPos, IM_COL32(255, 255, 255, 255), slugFont->EllipsisChar);
    ImGui::End();
    RunFrame(21);
    TEST_CHECKF(g_FakeUsed > 0, "ellipsis RenderChar produced no instances");
    {
        ImFontBaked* baked = slugFont->GetFontBaked(size);
        const ImFontGlyph* ellipsis = baked->FindGlyph(slugFont->EllipsisChar);
        TEST_CHECK(ellipsis != nullptr && ellipsis->Visible);
        if (ellipsis != nullptr && g_FakeUsed > 0)
        {
            // The RenderChar glyph is the last instance: its box must contain
            // the ink box (the instance rect is the control-point bbox).
            const ImSlugGlyphInstance& instance = g_FakeInstances[g_FakeUsed - 1];
            TEST_CHECKF(instance.ScreenMinX <= ellipsisPos.x + ellipsis->X0 + 1.1f &&
                            instance.ScreenMaxX >= ellipsisPos.x + ellipsis->X1 - 1.1f,
                        "ellipsis X span %.3f..%.3f vs ink %.3f..%.3f", instance.ScreenMinX, instance.ScreenMaxX,
                        ellipsisPos.x + ellipsis->X0, ellipsisPos.x + ellipsis->X1);
            TEST_CHECKF(instance.ScreenMinY <= ellipsisPos.y + ellipsis->Y0 + 1.1f &&
                            instance.ScreenMaxY >= ellipsisPos.y + ellipsis->Y1 - 1.1f,
                        "ellipsis Y span %.3f..%.3f vs ink %.3f..%.3f", instance.ScreenMinY, instance.ScreenMaxY,
                        ellipsisPos.y + ellipsis->Y0, ellipsisPos.y + ellipsis->Y1);
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Edge cases: mid-glyph command split, sampler callbacks, empty frames,
//    unsupported setups.
// ---------------------------------------------------------------------------
static void TestEdgeCases()
{
    std::printf("[test] edge cases\n");
    ImFont* slugFont = ImGui::GetIO().Fonts->Fonts[0];
    const char* text = "EdgeCases";
    const float size = g_TestFontSize;
    const ImVec2 pos(20.0f, 20.0f);

    // Reference instance count for this text.
    ImGui::NewFrame();
    OpenTestWindow("edge-reference");
    ImGui::GetWindowDrawList()->AddText(slugFont, size, pos, IM_COL32(255, 255, 255, 255), text);
    ImGui::End();
    RunFrame(30);
    const uint32_t referenceCount = g_FakeUsed;
    TEST_CHECK(referenceCount > 0);

    // Command split mid-glyph: cut the text command after the first triangle of
    // a glyph. Both halves must be dropped and nothing may leak to geometry.
    ImGui::NewFrame();
    OpenTestWindow("edge-split");
    ImGui::GetWindowDrawList()->AddText(slugFont, size, pos, IM_COL32(255, 255, 255, 255), text);
    ImGui::End();
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ImDrawList* list = drawData->CmdLists[0];
    int splitIndex = -1;
    for (int cmdIndex = 0; cmdIndex < list->CmdBuffer.Size; ++cmdIndex)
    {
        const ImDrawCmd& cmd = list->CmdBuffer[cmdIndex];
        if (cmd.UserCallback == nullptr && cmd.ElemCount >= 6)
        {
            ImDrawCmd first = cmd;
            first.ElemCount = 3;  // one triangle = half a glyph
            ImDrawCmd second = cmd;
            second.IdxOffset = cmd.IdxOffset + 3;
            second.ElemCount = cmd.ElemCount - 3;
            list->CmdBuffer.insert(&list->CmdBuffer[cmdIndex], first);
            list->CmdBuffer[cmdIndex + 1] = second;
            splitIndex = cmdIndex;
            break;
        }
    }
    TEST_CHECKF(splitIndex >= 0, "no text command to split");
    ResetFake();
    g_FakeFrameIndex = 31;
    ImSlug::PreprocessDrawData(drawData);
    DispatchSlugCallbacks(drawData);
    TEST_CHECKF(g_FakeUsed == referenceCount - 1, "mid-glyph split: %u instances (expected %u)", g_FakeUsed,
                referenceCount - 1);
    TEST_CHECKF(CountSlugVerticesInGeometry(list) == 0, "split halves leaked into geometry");

    // A sampler callback command (DrawCallback_SetSampler*) passes through and
    // splits the runs around it.
    ImGui::NewFrame();
    OpenTestWindow("edge-sampler");
    ImDrawList* samplerList = ImGui::GetWindowDrawList();
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    samplerList->AddText(slugFont, size, pos, IM_COL32(255, 255, 255, 255), "before sampler");
    samplerList->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
    samplerList->AddText(slugFont, size, ImVec2(pos.x, pos.y + 20.0f), IM_COL32(255, 255, 255, 255), "after sampler");
    ImGui::End();
    drawData = RunFrame(32);
    {
        int foundSampler = 0;
        int callbacks = 0;
        const ImDrawList* samplerCmdList = drawData->CmdLists[0];
        for (int i = 0; i < samplerCmdList->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd& cmd = samplerCmdList->CmdBuffer[i];
            if (cmd.UserCallback == platformIO.DrawCallback_SetSamplerNearest)
                foundSampler++;
            if (cmd.UserCallback == ImSlug::DrawCallback)
                callbacks++;
        }
        TEST_CHECKF(foundSampler == 1, "sampler callback commands: %d", foundSampler);
        TEST_CHECKF(callbacks == 2, "text runs around the sampler callback: %d (expected 2)", callbacks);
    }

    // Windows that produce nothing to draw (off screen / no background / tiny
    // child) must not crash the pre-pass or drop runs.
    ImGui::NewFrame();
    OpenTestWindow("empty");
    ImGui::SetNextWindowPos(ImVec2(-5000.0f, -5000.0f), ImGuiCond_Always);
    ImGui::Begin("offscreen");
    ImGui::TextUnformatted("off screen");
    ImGui::End();
    ImGui::BeginChild("empty-child", ImVec2(1.0f, 1.0f));
    ImGui::EndChild();
    ImGui::End();
    RunFrame(33);
    Statistics stats;
    ImSlug::GetStatistics(&stats);
    TEST_CHECK(stats.DroppedRuns == 0);

#if defined(NDEBUG)
    // Unsupported/misconfigured setups: the pre-pass must leave the draw data
    // untouched (debug builds assert instead, which is the documented behavior).
    {
        ImGui::NewFrame();
        OpenTestWindow("untouched");
        ImGui::TextUnformatted("still here");
        ImGui::End();
        ImDrawData* untouchedData = RunFrame(34);
        const int commandsBefore = untouchedData->CmdLists[0]->CmdBuffer.Size;
        ResetFake();

        ImGuiIO& io = ImGui::GetIO();
        const ImGuiBackendFlags flags = io.BackendFlags;
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
        ImSlug::PreprocessDrawData(untouchedData);
        TEST_CHECKF(untouchedData->CmdLists[0]->CmdBuffer.Size == commandsBefore,
                    "draw data was rewritten without RendererHasTextures");
        TEST_CHECK(g_FakeUsed == 0);
        io.BackendFlags = flags;

        ImSlugSetRenderIO(nullptr);
        ImSlug::PreprocessDrawData(untouchedData);
        TEST_CHECKF(untouchedData->CmdLists[0]->CmdBuffer.Size == commandsBefore,
                    "draw data was rewritten without a backend");
        TEST_CHECK(g_FakeUsed == 0);
        ImSlugSetRenderIO(&g_FakeIO);

        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        const ImDrawCallback reset = platformIo.DrawCallback_ResetRenderState;
        platformIo.DrawCallback_ResetRenderState = nullptr;
        ImSlug::PreprocessDrawData(untouchedData);
        TEST_CHECKF(untouchedData->CmdLists[0]->CmdBuffer.Size == commandsBefore,
                    "draw data was rewritten without a reset callback");
        TEST_CHECK(g_FakeUsed == 0);
        platformIo.DrawCallback_ResetRenderState = reset;
    }
#else
    std::printf("  (release-only setup checks skipped in this build)\n");
#endif
}

// ---------------------------------------------------------------------------
// 6. Runtime FontScaleDpi changes and atlas growth/repacking (M3 acceptance).
// ---------------------------------------------------------------------------
static void TestRuntimeDpiAndAtlasGrowth()
{
    std::printf("[test] runtime FontScaleDpi + atlas growth immunity\n");
    ImFont* slugFont = ImGui::GetIO().Fonts->Fonts[0];
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImSlugPools* pools = ImSlugGetPools();
    TEST_CHECK(pools != nullptr);

    // Reference frame at DPI 1.
    const ImVec2 pos(20.0f, 30.0f);
    ImGui::NewFrame();
    OpenTestWindow("dpi");
    drawTextHelper(slugFont, pos);
    ImGui::End();
    RunFrame(40);
    const uint32_t referenceCount = g_FakeUsed;
    TEST_CHECK(referenceCount > 0);
    ImSlugGlyphInstance reference[kMaxReferenceInstances];
    TEST_CHECK(referenceCount <= kMaxReferenceInstances);
    memcpy(reference, g_FakeInstances, (size_t)referenceCount * sizeof(ImSlugGlyphInstance));

    const int texturePixelsBefore = atlas->TexData->Width * atlas->TexData->Height;
    const int curveCursorBefore = pools->Curves.CursorTexels;
    const int bandCursorBefore = pools->Bands.CursorTexels;
    const float dpiValues[2] = { 2.0f, 0.75f };
    for (float dpi : dpiValues)
    {
        ImGui::GetStyle().FontScaleDpi = dpi;
        ImGui::NewFrame();
        OpenTestWindow("dpi");
        drawTextHelper(slugFont, pos);
        ImGui::End();
        RunFrame(41 + (uint32_t)(dpi * 100.0f));
        TEST_CHECKF(g_FakeUsed > 0, "no instances at DPI %.2f", dpi);
        // The same glyph outlines and bands are reused: the pools did not grow.
        TEST_CHECKF(pools->Curves.CursorTexels == curveCursorBefore,
                    "curve pool grew on a FontScaleDpi change (%.2f): %d -> %d", dpi, curveCursorBefore,
                    pools->Curves.CursorTexels);
        TEST_CHECKF(pools->Bands.CursorTexels == bandCursorBefore,
                    "band pool grew on a FontScaleDpi change (%.2f): %d -> %d", dpi, bandCursorBefore,
                    pools->Bands.CursorTexels);
        TEST_CHECKF(atlas->TexData->Width * atlas->TexData->Height == texturePixelsBefore,
                    "atlas texture grew on a FontScaleDpi change (%.2f)", dpi);
        // Same placements at the new size, scaled by dpi.
        ImSlugGlyphInstance scaled[kMaxReferenceInstances];
        memcpy(scaled, g_FakeInstances, (size_t)g_FakeUsed * sizeof(ImSlugGlyphInstance));
        for (uint32_t i = 0; i < g_FakeUsed && i < referenceCount; ++i)
        {
            const float penX = (float)(int)pos.x;
            const float penY = (float)(int)pos.y;  // compare pens, not rects
            const float width = scaled[i].ScreenMaxX - scaled[i].ScreenMinX;
            const float refWidth = reference[i].ScreenMaxX - reference[i].ScreenMinX;
            TEST_CHECKF(Nearly(width, refWidth * dpi, 0.05f * refWidth + 0.5f),
                        "DPI %.2f instance %u width %.3f vs %.3f", dpi, i, width, refWidth * dpi);
            (void)penX;
            (void)penY;
        }
    }
    ImGui::GetStyle().FontScaleDpi = 1.0f;

    // Atlas growth/repacking: a big stock font forces the atlas texture to grow
    // (and repack its rects). Slug handles live outside the atlas, so the baked
    // glyph UVs and the reconstructed placements must not change.
    const ImFontGlyph* glyphBefore = slugFont->GetFontBaked(g_TestFontSize)->FindGlyph((ImWchar)'H');
    TEST_CHECK(glyphBefore != nullptr);
    const float u0Before = glyphBefore->U0;
    const float v0Before = glyphBefore->V0;
    ImFontConfig bigConfig;
    bigConfig.OversampleH = 1;
    bigConfig.OversampleV = 1;
    ImFont* bigFont = atlas->AddFontFromFileTTF(IMSLUG_TEST_FONT_PATH("Inter-Regular.ttf"), 96.0f, &bigConfig);
    TEST_CHECK(bigFont != nullptr);
    ImFontBaked* bigBaked = bigFont->GetFontBaked(96.0f);
    for (unsigned int codepoint = 0x20; codepoint < 0x100; ++codepoint)
        bigBaked->FindGlyph((ImWchar)codepoint);
    TEST_CHECKF(atlas->TexData->Width * atlas->TexData->Height > texturePixelsBefore,
                "atlas did not grow for the 96 px stock font (%dx%d)", atlas->TexData->Width, atlas->TexData->Height);

    const ImFontGlyph* glyphAfter = slugFont->GetFontBaked(g_TestFontSize)->FindGlyph((ImWchar)'H');
    TEST_CHECK(glyphAfter != nullptr);
    TEST_CHECKF(glyphAfter->U0 == u0Before && glyphAfter->V0 == v0Before,
                "Slug handle changed after an atlas repack (%.6f -> %.6f)", u0Before, glyphAfter->U0);

    ImGui::NewFrame();
    OpenTestWindow("dpi");
    drawTextHelper(slugFont, pos);
    ImGui::End();
    RunFrame(50);
    TEST_CHECKF(g_FakeUsed == referenceCount, "instance count changed after an atlas repack: %u vs %u", g_FakeUsed,
                referenceCount);
    for (uint32_t i = 0; i < g_FakeUsed && i < referenceCount; ++i)
        TEST_CHECKF(Nearly(g_FakeInstances[i].ScreenMinX, reference[i].ScreenMinX, 0.01f) &&
                        Nearly(g_FakeInstances[i].ScreenMinY, reference[i].ScreenMinY, 0.01f),
                    "instance %u moved after an atlas repack", i);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("imslug_imgui: ImGui %s integration tests\n", IMGUI_VERSION);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    ImSlug::Init();
    ImSlugSetRenderIO(&g_FakeIO);
    InstallFakeBackendHooks();

    // Independent ground-truth font for the placement checks.
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile("Roboto-Regular.ttf", &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData != nullptr)
    {
        g_TestFont = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
        TEST_CHECK(g_TestFont != nullptr);
        std::free(fontData);
    }

    TestLoaderMetricsAndRects();
    TestHandleRoundTrip();
    TestPrepassStructure();
    TestPlacement();
    TestEdgeCases();
    TestRuntimeDpiAndAtlasGrowth();

    if (g_TestFont != nullptr)
        ImSlugDestroyFont(g_TestFont);
    ImSlugSetRenderIO(nullptr);
    ImSlug::Shutdown();
    ImGui::DestroyContext();
    return imslug_test::Finish("imslug_imgui");
}
