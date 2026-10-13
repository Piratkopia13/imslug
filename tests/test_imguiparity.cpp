// imslug M1 test — metric parity against Dear ImGui's own stb loader.
//
// This is a test-only build: Dear ImGui is compiled into this executable
// (imslug_imgui_testlib) and never into the library. For every test font and
// bake size it compares ImSlugGlyphMetrics with the real ImFontGlyph values
// produced by ImGui v1.92.9b's ImGui_ImplStbTrueType_FontBakedLoadGlyph, with
// the config Slug fonts use (or will use in M3): OversampleH/V = 1,
// RasterizerDensity = 1, ExtraSizeScale = 1, GlyphOffset = 0.
//
// AdvanceX is additionally compared against the default configuration, since
// it is independent of oversampling.

#include "imslug_internal.h"

#include "test_util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>

using namespace ImSlug;

static bool Exact(float a, float b)
{
    return a == b;
}

static void TestParityForFont(const char* fontName)
{
    int fontDataSize = 0;
    unsigned char* fontData = imslug_test::LoadFontFile(fontName, &fontDataSize);
    TEST_CHECK(fontData != nullptr);
    if (fontData == nullptr)
        return;

    ImSlugFont* ourFont = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
    TEST_CHECK(ourFont != nullptr);

    ImFontAtlas* atlas = new ImFontAtlas();

    // Slug configuration: no oversampling, no density scaling.
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.RasterizerDensity = 1.0f;
    config.ExtraSizeScale = 1.0f;
    config.GlyphOffset = ImVec2(0.0f, 0.0f);
    ImFont* slugFont = atlas->AddFontFromMemoryTTF(fontData, fontDataSize, 16.0f, &config);
    TEST_CHECK(slugFont != nullptr);

    // Default configuration, for the oversample-independent AdvanceX.
    ImFontConfig defaultConfig;
    defaultConfig.FontDataOwnedByAtlas = false;
    ImFont* defaultFont = atlas->AddFontFromMemoryTTF(fontData, fontDataSize, 16.0f, &defaultConfig);
    TEST_CHECK(defaultFont != nullptr);

    ImFontAtlasBuildMain(atlas);
    TEST_CHECK(atlas->TexData != nullptr);

    const float sizes[] = { 9.0f, 16.0f, 32.0f, 48.0f, 64.0f };
    const unsigned int codepoints[] = { ' ', 'I', 'g', 'A', 'W', 0xE9 };

    float worstBoxDelta = 0.0f;
    float worstAdvanceDelta = 0.0f;
    int compared = 0;
    for (float size : sizes)
    {
        ImFontBaked* baked = slugFont->GetFontBaked(size);
        TEST_CHECK(baked != nullptr);
        ImFontBaked* defaultBaked = defaultFont->GetFontBaked(size);
        TEST_CHECK(defaultBaked != nullptr);
        if (baked == nullptr || defaultBaked == nullptr)
            continue;

        for (unsigned int codepoint : codepoints)
        {
            const int glyphIndex = ImSlugGetGlyphIndex(ourFont, codepoint);
            TEST_CHECKF(glyphIndex > 0, "%s U+%04X not in font", fontName, codepoint);
            if (glyphIndex <= 0)
                continue;

            ImSlugGlyphMetrics ours;
            TEST_CHECK(ImSlugGetGlyphMetrics(ourFont, codepoint, size, &ours));

            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)codepoint);
            const ImFontGlyph* defaultGlyph = defaultBaked->FindGlyph((ImWchar)codepoint);
            TEST_CHECK(glyph != nullptr && defaultGlyph != nullptr);
            if (glyph == nullptr || defaultGlyph == nullptr)
                continue;

            if (!Exact(ours.AdvanceX, glyph->AdvanceX) || !Exact(ours.X0, glyph->X0) || !Exact(ours.Y0, glyph->Y0) ||
                !Exact(ours.X1, glyph->X1) || !Exact(ours.Y1, glyph->Y1))
            {
                TEST_CHECKF(false,
                            "%s U+%04X @ %.0f px: ours (adv %.4f box %.3f %.3f %.3f %.3f) vs ImGui (adv %.4f box "
                            "%.3f %.3f %.3f %.3f)",
                            fontName, codepoint, (double)size, (double)ours.AdvanceX, (double)ours.X0, (double)ours.Y0,
                            (double)ours.X1, (double)ours.Y1, (double)glyph->AdvanceX, (double)glyph->X0,
                            (double)glyph->Y0, (double)glyph->X1, (double)glyph->Y1);
            }

            // AdvanceX is independent of oversampling.
            if (!Exact(ours.AdvanceX, defaultGlyph->AdvanceX))
            {
                TEST_CHECKF(false, "%s U+%04X @ %.0f px: AdvanceX %.6f (ours) vs %.6f (ImGui default config)", fontName,
                            codepoint, (double)size, (double)ours.AdvanceX, (double)defaultGlyph->AdvanceX);
            }

            const float boxDelta = std::fmax(std::fmax(std::fabs(ours.X0 - glyph->X0), std::fabs(ours.Y0 - glyph->Y0)),
                                             std::fmax(std::fabs(ours.X1 - glyph->X1), std::fabs(ours.Y1 - glyph->Y1)));
            const float advanceDelta = std::fabs(ours.AdvanceX - glyph->AdvanceX);
            worstBoxDelta = std::fmax(worstBoxDelta, boxDelta);
            worstAdvanceDelta = std::fmax(worstAdvanceDelta, advanceDelta);
            compared++;
        }
    }
    std::printf("  %s: %d glyph/size comparisons, worst box delta %.6f, worst advance delta %.6f\n", fontName, compared,
                (double)worstBoxDelta, (double)worstAdvanceDelta);

    // ImFontAtlas is standalone-safe (its ctor/dtor do not need a renderer);
    // the context above exists for the atlas build paths.
    delete atlas;
    ImSlugDestroyFont(ourFont);
    std::free(fontData);
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("imslug_metrics_imgui_parity: ImSlug metrics vs Dear ImGui %s stb loader\n", IMGUI_VERSION);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    TestParityForFont("Roboto-Regular.ttf");
    TestParityForFont("Inter-Regular.ttf");

    ImGui::DestroyContext();
    return imslug_test::Finish("imslug_metrics_imgui_parity");
}
