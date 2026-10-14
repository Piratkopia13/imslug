// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// example/common/example_app.cpp — shared example application (platform-free).
//
// The scene set, the switcher, the font handling and the per-frame ImGui
// build, exactly as the standalone example had them; the platform shell
// (example/glfw_vulkan/main.cpp, example/web/main.cpp) owns the window/GPU
// context and implements the ExamplePlatform hooks. See example_app.h for the
// seams.
//
// The renderer comparison bar (DrawRendererBar) is the example's headline
// control: the Slug/atlas toggle sits at the top center of the frame so the
// two renderers are always one click apart. The atlas mode is honest by
// construction: content that only Slug can draw (color emoji, packed assets)
// is not shown through Slug while the atlas renderer is active - those scenes
// explain themselves instead - and everything Slug still draws is labeled as
// such. The CJK scene registers the fetched subset through both renderers, so
// the comparison stays about the renderer, not about missing registrations.

#include "example_app.h"

#include "imgui.h"
// ImTextCharToUtf8 (CJK scene); FindWindowByName / SetWindowPos /
// BringWindowToDisplayFront (renderer bar and the demo-window nudge).
#include "imgui_internal.h"
#include "imslug.h"

// The default fonts, embedded at build time (generated artifacts; regenerate
// with scripts/gen_asset.*): the packed Roboto-ASCII asset for Slug and the
// raw Roboto TTF for ImGui's own atlas loader (mixed-font scene / M toggle).
#include "imslug_example_asset.h"
#include "imslug_example_font.h"

// The browser build has no filesystem: when the web shell was built with the
// fetched test fonts embedded (scripts/build_web.* runs imslug_gen embed and
// defines IMSLUG_EXAMPLE_CJK_EMBED / IMSLUG_EXAMPLE_EMOJI_EMBED, adding the
// headers' directory to the include path), the CJK and color-emoji scenes
// register those subsets instead of probing the fetched files. The emoji
// subset is COLR v1 only (built by scripts/subset_emoji_for_web.py).
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_CJK_EMBED)
#include "imslug_example_cjk.h"
#endif
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_EMOJI_EMBED)
#include "imslug_example_emoji.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ImSlug;

namespace
{
    ExampleAppState g_App = {};
    ExamplePlatform g_Platform = {};

    // Scene windows start below the renderer bar (DrawRendererBar pins the
    // bar to the top center, 8 px from the edge); FirstUseEver, so the user
    // can move them freely afterwards.
    constexpr float kSceneWindowY = 96.0f;

    // ---------------------------------------------------------------------
    // Scenes
    // ---------------------------------------------------------------------
    void SceneDemo()
    {
        if (g_App.ShowDemoStats)
            ImGui::ShowMetricsWindow(&g_App.ShowDemoStats);
        ImGui::ShowDemoWindow(nullptr);
        // The demo window is the one scene window this app does not position
        // (it places itself at FirstUseEver); nudge it below the renderer bar
        // once, when it first appears. It stays where the user drags it.
        static bool demoSettled = false;
        if (!demoSettled)
        {
            ImGuiWindow* demo = ImGui::FindWindowByName("Dear ImGui Demo");
            if (demo != nullptr)
            {
                ImGui::SetWindowPos(demo, ImVec2(demo->Pos.x, kSceneWindowY));
                demoSettled = true;
            }
        }
    }

    void SceneWidgets()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("widgets, plots and tables");
        ImGui::Text("A normal ImGui window: labels, buttons, sliders, plots.");
        static float value = 0.5f;
        static bool flag = true;
        ImGui::SliderFloat("slider", &value, 0.0f, 1.0f);
        ImGui::Checkbox("checkbox", &flag);
        if (ImGui::Button("a button"))
            value = 0.25f;
        ImGui::SameLine();
        ImGui::Text("inline text after a button");
        static float history[120] = {};
        history[g_App.FramesRendered % 120] = value;
        ImGui::PlotLines("plot", history, 120, g_App.FramesRendered % 120, "value", 0.0f, 1.0f, ImVec2(0.0f, 80.0f));
        if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            for (int row = 0; row < 12; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("row %d", row);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Hamburgefonstiv %d", row * 7);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("U+%04X", 0x40 + row);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Long text in differently sized children, tooltips and scrolling: every
    // CPU fine-clip path (four sides), window-edge clipping and ellipsized
    // labels in one scene.
    void SceneClipping()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::Begin("clipping torture");

        ImGui::Text("1) labels clipped by narrow cells (CPU fine clip on the right):");
        if (ImGui::BeginTable("cliptable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            for (int row = 0; row < 6; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("cell %d", row);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("a very long label that must be elided ...");
            }
            ImGui::EndTable();
        }

        ImGui::Text("2) selectables with long labels (clip on all sides):");
        for (int row = 0; row < 4; ++row)
            ImGui::Selectable("a selectable with a ridiculously long caption that overflows", false, 0,
                              ImVec2(180.0f, 0.0f));

        ImGui::Text("3) child windows of shrinking size (window-edge + fine clip):");
        const float widths[4] = { 220.0f, 96.0f, 40.0f, 14.0f };
        for (int i = 0; i < 4; ++i)
        {
            ImGui::BeginChild(i == 0 ? "child-wide" : "child", ImVec2(widths[i], 34.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Hamburgefonstiv in a small child window");
            ImGui::TextUnformatted("second line, scrolled out of view");
            ImGui::EndChild();
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Text("4) tooltips (hover the box):");
        ImGui::Button("hover me", ImVec2(120.0f, 0.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("tooltip text rendered by %s as well\nsecond tooltip line",
                              g_App.AtlasText ? "the atlas" : "Slug");

        ImGui::Text("5) text at the window edge (scrolled horizontally):");
        ImGui::BeginChild("h-scroll", ImVec2(0.0f, 60.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted("left edge | a long horizontally scrolling line with lots of words | right edge");
        ImGui::TextUnformatted("Hamburgefonstiv 0123456789 repeated: abcdefghijklmnopqrstuvwxyz");
        ImGui::EndChild();

        ImGui::Text("6) a tiny child (a glyph squeezed on every side):");
        ImGui::BeginChild("tiny", ImVec2(11.0f, 7.0f), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Wg");
        ImGui::EndChild();

        ImGui::End();
    }

    void SceneMixedFonts()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560.0f, 360.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("mixed fonts (Slug + atlas)");
        ImGui::TextWrapped("The line below alternates between the Slug font and a stock atlas font in one frame; both "
                           "paths must stay correct.");
        ImGui::Separator();
        // Both fonts are pushed explicitly, so the labels stay true in either
        // mode: the "Slug:" rows are Slug output even while the atlas renders
        // the default text (this is the labeled coexistence comparison).
        for (int row = 0; row < 6; ++row)
        {
            if ((row % 2) == 0 && g_App.SlugFont != nullptr)
            {
                ImGui::PushFont(g_App.SlugFont, 0.0f);
                ImGui::Text("Slug: Hamburgefonstiv 0123456789");
                ImGui::PopFont();
            }
            else if (g_App.StockFont != nullptr)
            {
                ImGui::PushFont(g_App.StockFont, 0.0f);
                ImGui::Text("atlas: Hamburgefonstiv 0123456789");
                ImGui::PopFont();
            }
        }
        ImGui::Separator();
        ImGui::Text("Shapes, lines and images keep using the atlas:");
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p, ImVec2(p.x + 180.0f, p.y + 24.0f), IM_COL32(70, 120, 200, 255), 4.0f);
        drawList->AddCircleFilled(ImVec2(p.x + 220.0f, p.y + 12.0f), 12.0f, IM_COL32(200, 140, 60, 255));
        drawList->AddText(ImVec2(p.x + 8.0f, p.y + 4.0f), IM_COL32(255, 255, 255, 255), "text over a shape");
        ImGui::Dummy(ImVec2(240.0f, 30.0f));
        ImGui::End();
    }

    void SceneDpi()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("runtime FontScaleDpi");
        if (g_App.AtlasText)
            ImGui::TextWrapped("Changing style.FontScaleDpi re-rasterizes the visible glyphs into the atlas at every "
                               "new size: watch the texture size grow. Switch to Slug (M) to reuse the same outlines "
                               "at any size without atlas bakes.");
        else
            ImGui::TextWrapped("Changing style.FontScaleDpi re-bakes the metrics at a new size while the Slug outlines "
                               "and bands are reused: the atlas must not grow (watch the texture size).");
        ImGui::SliderFloat("style.FontScaleDpi", &g_App.DpiScale, 0.5f, 3.0f, "%.2f");
        ImGui::GetStyle().FontScaleDpi = g_App.DpiScale;
        ImGui::SeparatorText("sample text");
        ImGui::Text("Hamburgefonstiv 0123456789");
        ImGui::TextWrapped("The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.");
        ImGui::SeparatorText("atlas / backend state");
        const ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("atlas texture: %dx%d", tex != nullptr ? tex->Width : 0, tex != nullptr ? tex->Height : 0);
        ImGui::Text("pixels per glyph rect (live): %d", stats.GlyphSlabs);
        ImGui::Text("Slug instances last frame: %u", stats.Instances);
        ImGui::End();
    }

    // Stress scene: ~5k glyphs of long multi-line text plus a wrapped
    // paragraph, at the current base size (watch the stats: instances/runs).
    // The text is regenerated only when the layout inputs change.
    void SceneStress()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1100.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("5k glyph stress");
        if (g_App.AtlasText)
            ImGui::TextWrapped("One window, thousands of glyphs rendered by ImGui's atlas: %d lines of long mixed "
                               "text plus a wrapped paragraph. Switch to Slug (M) to run the same wall through the "
                               "Slug path (the instance counters stay at 0 here).",
                               38);
        else
            ImGui::TextWrapped("One window, thousands of glyphs: %d lines of long mixed text plus a wrapped "
                               "paragraph. The stats below come from ImSlug::GetStatistics(); dropped runs must "
                               "stay 0.",
                               38);
        static char* text = nullptr;
        if (text == nullptr)
        {
            const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
            const int patternLength = (int)strlen(pattern);
            const int lines = 38;
            const int columns = (int)(1300.0f / (float)g_App.BaseSize * 10.0f);  // ~fill the window width
            text = (char*)malloc((size_t)lines * (size_t)(columns + 2) + 2);
            if (text != nullptr)
            {
                int at = 0;
                for (int line = 0; line < lines; ++line)
                {
                    for (int column = 0; column < columns; ++column)
                        text[at++] = pattern[(line * columns + column) % patternLength];
                    if (line + 1 < lines)
                        text[at++] = '\n';
                }
                text[at] = '\0';
            }
        }
        if (text != nullptr)
            ImGui::TextUnformatted(text);
        ImGui::End();
    }

    // Registers the fetched Noto Sans SC subset (scripts/fetch_cjk_font.*),
    // through Slug (`slug` true) or through ImGui's own dynamic atlas loader.
    // The desktop reads the fetched file; the web shell has no filesystem and
    // uses the subset embedded at build time (IMSLUG_EXAMPLE_CJK_EMBED,
    // scripts/build_web.*). Returns null when the source is missing; the scene
    // reports that per mode.
    ImFont* AddCjkFont(bool slug)
    {
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_CJK_EMBED)
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        if (slug)
            return ImSlug::AddFontFromMemoryTTF(imslug_example_cjk_data, (int)imslug_example_cjk_size, 22.0f, &config);
        // Static embedded data: FontDataOwnedByAtlas=false above, so the
        // const-cast is only ImGui's non-const pointer style.
        return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)imslug_example_cjk_data, (int)imslug_example_cjk_size,
                                                          22.0f, &config);
#elif !defined(__EMSCRIPTEN__)
        FILE* cjkFile = fopen(IMSLUG_CJK_TEST_FONT, "rb");
        if (cjkFile == nullptr)
            return nullptr;
        fclose(cjkFile);
        if (slug)
            return ImSlug::AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 22.0f);
        // Oversample 1 (the Slug metrics' assumption) keeps the stock
        // registration at the same layout as the Slug one.
        ImFontConfig stockConfig;
        stockConfig.OversampleH = 1;
        stockConfig.OversampleV = 1;
        return ImGui::GetIO().Fonts->AddFontFromFileTTF(IMSLUG_CJK_TEST_FONT, 22.0f, &stockConfig);
#else
        (void)slug;  // web build without the embedded subset
        return nullptr;
#endif
    }

    // CJK scene: the subset as a second Slug font and, while the atlas
    // renderer is active, through a stock registration of the same TTF - so
    // the sample stays visible in both modes and the comparison stays about
    // the renderer (ImGui's dynamic fonts handle CJK; Slug handles it without
    // baking glyphs into the atlas). Skipped with a message when the subset
    // is not fetched/embedded.
    void SceneCjk()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("CJK (fetched subset)");
        const bool slug = !g_App.AtlasText;
        if (slug)
        {
            if (g_App.CjkFont == nullptr && !g_App.CjkAttempted)
            {
                g_App.CjkAttempted = true;
                g_App.CjkFont = AddCjkFont(true);
            }
        }
        else if (g_App.CjkStockFont == nullptr && !g_App.CjkStockAttempted)
        {
            g_App.CjkStockAttempted = true;
            g_App.CjkStockFont = AddCjkFont(false);
        }
        ImFont* font = slug ? g_App.CjkFont : g_App.CjkStockFont;
        if (font == nullptr)
        {
#if defined(__EMSCRIPTEN__)
            ImGui::TextWrapped("This web build does not embed the CJK subset. Fetch it (scripts/fetch_cjk_font.*) "
                               "and rebuild (scripts/build_web.* embeds it; see tests/fonts/cjk/PROVENANCE.txt).");
#else
            ImGui::TextWrapped("The CJK test font is not fetched. Run scripts/fetch_cjk_font.* (see "
                               "tests/fonts/cjk/PROVENANCE.txt), then restart or reselect this scene.");
#endif
        }
        else
        {
            if (slug)
                ImGui::TextWrapped("2040 Hanzi (U+4E00..U+4EFF, 60 lines x 34) rendered from the subset's outlines "
                                   "through Slug; the worst glyph in the range has 146 quadratic curves.");
            else
                ImGui::TextWrapped("The same 2040 Hanzi through ImGui's dynamic atlas (the subset is also registered "
                                   "as a stock font): glyphs are rasterized and baked into the atlas on first use - "
                                   "watch the texture size. Switch to Slug (M) for the outline path.");
            const ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
            ImGui::Text("atlas texture: %dx%d", tex != nullptr ? tex->Width : 0, tex != nullptr ? tex->Height : 0);
            static char* cjkText = nullptr;
            if (cjkText == nullptr)
            {
                const int lines = 60;
                const int columns = 34;
                cjkText = (char*)malloc((size_t)lines * (size_t)columns * 4 + 8);
                if (cjkText != nullptr)
                {
                    int at = 0;
                    int index = 0;
                    for (int line = 0; line < lines; ++line)
                    {
                        for (int column = 0; column < columns; ++column)
                        {
                            const unsigned int codepoint = 0x4E00u + (unsigned int)(index++ % 0x100);
                            at += ImTextCharToUtf8(cjkText + at, codepoint);
                        }
                        if (line + 1 < lines)
                            cjkText[at++] = '\n';
                    }
                    cjkText[at] = '\0';
                }
            }
            ImGui::PushFont(font, 22.0f);
            if (cjkText != nullptr)
                ImGui::TextUnformatted(cjkText);
            ImGui::PopFont();
        }
        ImGui::End();
    }

    // Emoji scene: the fetched Noto Color Emoji (COLR v1; scripts/fetch_emoji_font.*).
    // The example build defines IMGUI_USE_WCHAR32 (CMake: IMSLUG_WCHAR32,
    // default ON), so the scene uses the whole set - the non-BMP snail/shell
    // mascot (U+1F40C / U+1F41A) included. The scene shows the layers at
    // several sizes plus one glyph zoomed up to 220 px so the v1 gradients
    // (radial/linear paints) are visible at arbitrary zoom.
    //
    // Color glyphs are a Slug-only sample: Dear ImGui's atlas has no color-font
    // support, so in atlas mode the scene explains itself instead of drawing
    // Slug output that could be mistaken for atlas output. The font registers
    // on demand, when the scene is first viewed in Slug mode.
    void SceneEmoji()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("color emoji (COLR v1)");
        if (g_App.AtlasText)
        {
            ImGui::TextWrapped("Color emoji render through Slug only: the COLR v0/v1 layer stacks and their "
                               "per-fragment paint evaluation (flat solids, radial/linear gradients) have no "
                               "equivalent in Dear ImGui's font atlas, which has no color-glyph support. No emoji "
                               "are drawn while the atlas renderer is active; switch back to Slug (M) to view the "
                               "scene.");
            ImGui::TextDisabled("(the emoji font is not registered through the atlas in this demo)");
            ImGui::End();
            return;
        }
        if (g_App.EmojiFont == nullptr && !g_App.EmojiAttempted)
        {
            g_App.EmojiAttempted = true;
#if defined(__EMSCRIPTEN__) && defined(IMSLUG_EXAMPLE_EMOJI_EMBED)
            // The browser build embeds a build-time COLR v1 subset of the
            // fetched font (scripts/build_web.*: subset_emoji_for_web.py +
            // imslug_gen embed; SVG/bitmap tables dropped).
            ImFontConfig emojiConfig;
            emojiConfig.FontDataOwnedByAtlas = false;
            g_App.EmojiFont = ImSlug::AddFontFromMemoryTTF(imslug_example_emoji_data, (int)imslug_example_emoji_size,
                                                           24.0f, &emojiConfig);
#elif !defined(__EMSCRIPTEN__)
            FILE* emojiFile = fopen(IMSLUG_EMOJI_TEST_FONT, "rb");
            if (emojiFile != nullptr)
            {
                fclose(emojiFile);
                g_App.EmojiFont = ImSlug::AddFontFromFileTTF(IMSLUG_EMOJI_TEST_FONT, 24.0f);
            }
#endif
        }
        if (g_App.EmojiFont == nullptr)
        {
#if defined(__EMSCRIPTEN__)
            ImGui::TextWrapped("This web build does not embed the color-emoji subset. Fetch the font "
                               "(scripts/fetch_emoji_font.*) and rebuild (scripts/build_web.* embeds a COLR v1 "
                               "subset; see tests/fonts/emoji/PROVENANCE.txt).");
#else
            ImGui::TextWrapped("The color-emoji test font is not fetched. Run scripts/fetch_emoji_font.* (see "
                               "tests/fonts/emoji/PROVENANCE.txt), then restart or reselect this scene.");
#endif
        }
        else
        {
            ImGui::TextWrapped("Noto Color Emoji: one Slug instance per COLR layer, flat v0/v1 solids and v1 gradients "
                               "(radial/linear) evaluated per fragment. Tint only fades the layer colors.");
            ImGui::SliderFloat("emoji size", &g_App.EmojiSize, 12.0f, 96.0f, "%.0f px");
            // The mascot entries are non-BMP: they are only addressable with a
            // 32-bit ImWchar, so a 16-bit build (IMSLUG_WCHAR32=OFF) shows the
            // BMP lineup and the mascot row explains instead.
            static const char* kEmoji = "\xE2\x98\x80 \xE2\x9B\x85 \xE2\x9D\xA4 \xE2\xAD\x90 \xE2\x9A\xA1 \xE2\x9C\xA8 "
                                        "\xE2\x9B\x84 \xE2\x98\x94 \xE2\x9A\xBD \xE2\x9C\x82 \xE2\x99\xBB \xE2\x9A\x93"
#if defined(IMGUI_USE_WCHAR32)
                                        " \xF0\x9F\x90\x8C \xF0\x9F\x90\x9A"
#endif
                ;
            ImGui::PushFont(g_App.EmojiFont, g_App.EmojiSize);
            ImGui::TextUnformatted(kEmoji);
            ImGui::PopFont();

            ImGui::Separator();
            ImGui::Text("zoom (gradients stay exact):");
            const float zoom = g_App.EmojiZoom;
            ImGui::SliderFloat("zoom size", &g_App.EmojiZoom, 32.0f, 220.0f, "%.0f px");
            ImGui::PushFont(g_App.EmojiFont, zoom);
            ImGui::TextUnformatted("\xE2\x9D\xA4");
            ImGui::PopFont();

            ImGui::SeparatorText("the imslug mascot");
#if defined(IMGUI_USE_WCHAR32)
            ImGui::PushFont(g_App.EmojiFont, 56.0f);
            ImGui::TextUnformatted("\xF0\x9F\x90\x8C \xF0\x9F\x90\x9A \xF0\x9F\x90\x8C");
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("Unicode has no slug emoji: the snail (U+1F40C) and the spiral shell (U+1F41A) play "
                               "the part - both non-BMP, addressed because this build defines IMGUI_USE_WCHAR32 "
                               "(CMake: IMSLUG_WCHAR32).");
            ImGui::PopStyleColor();
#else
            ImGui::TextWrapped("Unicode has no slug emoji: the snail (U+1F40C) and the spiral shell (U+1F41A) would "
                               "play the part, but they are non-BMP and this build has a 16-bit ImWchar "
                               "(IMSLUG_WCHAR32=OFF). Build with IMGUI_USE_WCHAR32 in the whole app to see them.");
#endif

            ImGui::Separator();
            Statistics stats;
            ImSlug::GetStatistics(&stats);
            ImGui::Text("this frame: %u instances (%u color layers), %u runs", stats.Instances, stats.ColorLayers,
                        stats.LiveBatches);
        }
        ImGui::End();
    }

    // Packed-asset scene: text rendered from a pre-baked `.imslug` container
    // (glyph records + curve/band texels; no outline parsing). The asset is the
    // committed test asset (tests/assets/Roboto-ASCII.imslug, also embedded in
    // this binary); the scene loads it as a second Slug font, so the shared
    // pools and the auto-detecting loader are both exercised. With no checkout
    // next to the executable the embedded copy stands in. In a no-parser build
    // this is also the default font source.
    //
    // A packed asset is a Slug-only source (Dear ImGui cannot parse the
    // container and a packed font cannot be registered as an atlas font), so
    // in atlas mode the scene explains itself instead of drawing. The font
    // registers on demand, when the scene is first viewed in Slug mode.
    void ScenePacked()
    {
        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("packed asset (.imslug)");
        if (g_App.AtlasText)
        {
            ImGui::TextWrapped("Packed .imslug assets load only through Slug: Dear ImGui's font loader cannot parse "
                               "the container (imslug auto-detects the format by content, so TTF and packed fonts "
                               "share one call), and a packed font has no atlas registration. No text is drawn "
                               "while the atlas renderer is active; switch back to Slug (M) to view the scene.");
            ImGui::End();
            return;
        }
        if (g_App.PackedFont == nullptr && !g_App.PackedAttempted)
        {
            g_App.PackedAttempted = true;
#ifndef __EMSCRIPTEN__
            FILE* assetFile = fopen(IMSLUG_PACKED_TEST_ASSET, "rb");
            if (assetFile != nullptr)
            {
                fclose(assetFile);
                // The same entry point as a TTF: the loader auto-detects the
                // container by content.
                g_App.PackedFont = ImSlug::AddFontFromFileTTF(IMSLUG_PACKED_TEST_ASSET, 20.0f);
                g_App.PackedFromFile = g_App.PackedFont != nullptr;
            }
            else
#endif
            {
                // Standalone run (no repo checkout next to the executable):
                // fall back to the embedded copy of the same committed asset.
                ImFontConfig packedConfig;
                packedConfig.FontDataOwnedByAtlas = false;
                g_App.PackedFont = ImSlug::AddFontFromMemoryTTF(imslug_example_asset_data,
                                                                (int)imslug_example_asset_size, 20.0f, &packedConfig);
            }
        }
        if (g_App.PackedFont == nullptr)
        {
#ifndef __EMSCRIPTEN__
            ImGui::TextWrapped("The packed test asset is missing (%s). Generate it with scripts/gen_asset.* "
                               "(see tests/assets/PROVENANCE.txt), then restart or reselect this scene.",
                               IMSLUG_PACKED_TEST_ASSET);
#else
            ImGui::TextWrapped("The embedded packed test asset could not be loaded.");
#endif
        }
        else
        {
            ImGui::TextWrapped("This text is rendered from %s - the committed Roboto-ASCII.imslug packed asset "
                               "(.imslug container v1): glyph records, curve texels and band texels copied into "
                               "the shared pools without any outline parsing. The loader auto-detects the format "
                               "by content, so no app code changes between TTF and packed fonts.",
                               g_App.PackedFromFile ? "tests/assets/Roboto-ASCII.imslug"
                                                    : "the asset copy embedded in this binary");
            ImGui::Separator();
            ImGui::PushFont(g_App.PackedFont, 20.0f);
            ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            ImGui::TextUnformatted("abcdefghijklmnopqrstuvwxyz");
            ImGui::TextUnformatted("0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
            ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog.");
            ImGui::PopFont();
            ImGui::Separator();
            Statistics stats;
            ImSlug::GetStatistics(&stats);
            ImGui::Text("this frame: %u instances, %u runs, %u live slabs", stats.Instances, stats.LiveBatches,
                        stats.GlyphSlabs);
        }
        ImGui::End();
    }

    // Node-editor scene (key N): a dummy node-graph canvas for the pan/zoom
    // story - the workload node editors actually have (dozens of small labels
    // that must stay readable while the view is zoomed around). The whole
    // graph pans (middle-drag, or drag on empty canvas) and zooms about the
    // cursor (mouse wheel); node titles and row labels are drawn through the
    // *active* renderer at size * zoom, so the scene is an honest A/B:
    //   * Slug re-renders the outlines at every integer size, allocates no
    //     texture bytes and frees the per-size bakes again when ImGui's cache
    //     GC / CompactCache discards them (watch the atlas row stay put);
    //   * the stock path rasterizes every size it sees into the atlas, so the
    //     texture grows (and the counters move) while zooming.
    // Text straddling the canvas edge also exercises the fine-clip
    // reconstruction. LOD mirrors real node editors: row labels disappear
    // below 8 px, titles below 6 px.
    struct DemoNode
    {
        const char* Title;
        const char* Row0;
        const char* Row1;
        float X, Y;  // world units
        ImU32 Accent;
        bool HasIn;
        bool HasOut;
    };

    void SceneNodeEditor()
    {
        constexpr float kNodeW = 200.0f;
        constexpr float kHeaderH = 30.0f;
        constexpr float kRowH = 22.0f;
        constexpr float kTitlePx = 17.0f;
        constexpr float kRowPx = 14.0f;

        constexpr float kInitialPanX = 30.0f;
        constexpr float kInitialPanY = 110.0f;  // clears the topmost node (-90 world)
        static ImVec2 pan(kInitialPanX, kInitialPanY);
        static float zoom = 1.0f;
        static int draggedNode = -1;
        static DemoNode nodes[] = {
            { "font source", "TTF | .imslug", "glyph outlines", 0.0f, 0.0f, IM_COL32(96, 160, 255, 255), false, true },
            { "band build", "8 bands per row", "curve texels", 280.0f, -90.0f, IM_COL32(120, 200, 140, 255), true,
              true },
            { "coverage", "reference walk", "analytic AA", 280.0f, 120.0f, IM_COL32(230, 170, 90, 255), true, true },
            { "color layers", "COLR v0/v1", "paint graphs", 0.0f, 260.0f, IM_COL32(200, 110, 200, 255), false, true },
            { "composite", "premultiplied", "tint x alpha", 560.0f, 30.0f, IM_COL32(120, 180, 230, 255), true, true },
            { "framebuffer", "sRGB", "present", 840.0f, 30.0f, IM_COL32(160, 160, 180, 255), true, false },
        };
        static const int kWires[][2] = { { 0, 1 }, { 0, 2 }, { 1, 4 }, { 2, 4 }, { 3, 4 }, { 4, 5 } };
        const int kNodeCount = (int)(sizeof(nodes) / sizeof(nodes[0]));
        const int kWireCount = (int)(sizeof(kWires) / sizeof(kWires[0]));

        ImGui::SetNextWindowPos(ImVec2(60.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(1100.0f, 700.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("node editor (pan + zoom)");

        // Text sizes follow the view and the app's DPI scaling; the whole
        // frame is pushed through the active renderer (Slug or atlas).
        const ImGuiStyle& style = ImGui::GetStyle();
        const float dpiScale = style.FontScaleDpi * style.FontScaleMain;
        const float titlePx = kTitlePx * zoom * dpiScale;
        const float rowPx = kRowPx * zoom * dpiScale;
        const bool drawRows = rowPx >= 8.0f;

        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::SmallButton("reset view"))
        {
            pan = ImVec2(kInitialPanX, kInitialPanY);
            zoom = 1.0f;
        }
        ImGui::SameLine();
        const char* lodNote = "";
        if (!drawRows)
            lodNote = titlePx >= 6.0f ? " (rows hidden at this zoom)" : " (text hidden at this zoom)";
        ImGui::Text("zoom %.2fx - node text %.0f px title / %.0f px rows%s", zoom, titlePx, rowPx, lodNote);
        const ImTextureData* tex = io.Fonts->TexData;
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("atlas %dx%d (%.2f MB) - Slug: %u instances, %u runs, %u slabs", tex != nullptr ? tex->Width : 0,
                    tex != nullptr ? tex->Height : 0,
                    tex != nullptr ? (double)tex->GetSizeInBytes() / (1024.0 * 1024.0) : 0.0, stats.Instances,
                    stats.LiveBatches, stats.GlyphSlabs);
        ImGui::TextDisabled("wheel: zoom about the cursor - middle-drag (or drag empty canvas): pan - drag a node to "
                            "move it");
        if (g_App.AtlasText)
            ImGui::TextDisabled("the atlas rasterizes every size you zoom through (the texture grows as it fills)");
        else
            ImGui::TextDisabled("Slug re-renders the outlines at every size - no atlas bakes while you zoom");
        ImGui::Separator();

        // Canvas: one invisible button for input, everything drawn manually.
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 64.0f)
            canvasSize.x = 64.0f;
        if (canvasSize.y < 64.0f)
            canvasSize.y = 64.0f;
        ImGui::InvisibleButton("##canvas", canvasSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool canvasHovered = ImGui::IsItemHovered();
        const bool canvasActive = ImGui::IsItemActive();
        const ImVec2 mouse = io.MousePos;
        auto worldFromScreen = [&](const ImVec2& screen)
        { return ImVec2((screen.x - canvasPos.x - pan.x) / zoom, (screen.y - canvasPos.y - pan.y) / zoom); };
        auto screenFromWorld = [&](float wx, float wy)
        { return ImVec2(canvasPos.x + pan.x + wx * zoom, canvasPos.y + pan.y + wy * zoom); };

        // Zoom about the cursor.
        if (canvasHovered && io.MouseWheel != 0.0f)
        {
            const ImVec2 before = worldFromScreen(mouse);
            zoom *= powf(1.15f, io.MouseWheel);
            zoom = zoom < 0.2f ? 0.2f : (zoom > 4.0f ? 4.0f : zoom);
            pan.x = mouse.x - canvasPos.x - before.x * zoom;
            pan.y = mouse.y - canvasPos.y - before.y * zoom;
        }
        // Press: grab the node under the cursor, or pan on empty canvas.
        if (ImGui::IsItemActivated())
        {
            draggedNode = -1;
            const ImVec2 world = worldFromScreen(mouse);
            for (int i = 0; i < kNodeCount; ++i)
            {
                const float worldH = kHeaderH + 2.0f * kRowH;
                if (world.x >= nodes[i].X && world.x < nodes[i].X + kNodeW && world.y >= nodes[i].Y &&
                    world.y < nodes[i].Y + worldH)
                {
                    draggedNode = i;
                    break;
                }
            }
        }
        if (canvasActive)
        {
            if (io.MouseDown[ImGuiMouseButton_Left] && draggedNode >= 0)
            {
                nodes[draggedNode].X += io.MouseDelta.x / zoom;
                nodes[draggedNode].Y += io.MouseDelta.y / zoom;
            }
            else if (io.MouseDown[ImGuiMouseButton_Left] || io.MouseDown[ImGuiMouseButton_Middle])
            {
                pan.x += io.MouseDelta.x;
                pan.y += io.MouseDelta.y;
            }
        }
        else
        {
            draggedNode = -1;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(20, 22, 28, 255));

        // Grid (world-aligned; the step doubles so lines never get denser than
        // ~14 px).
        float gridStep = 32.0f;
        while (gridStep * zoom < 14.0f)
            gridStep *= 2.0f;
        const ImVec2 worldMin = worldFromScreen(canvasPos);
        const ImVec2 worldMax = worldFromScreen(ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y));
        const ImU32 gridCol = IM_COL32(255, 255, 255, 14);
        for (float x = floorf(worldMin.x / gridStep) * gridStep; x <= worldMax.x; x += gridStep)
        {
            const float sx = canvasPos.x + pan.x + x * zoom;
            drawList->AddLine(ImVec2(sx, canvasPos.y), ImVec2(sx, canvasPos.y + canvasSize.y), gridCol, 1.0f);
        }
        for (float y = floorf(worldMin.y / gridStep) * gridStep; y <= worldMax.y; y += gridStep)
        {
            const float sy = canvasPos.y + pan.y + y * zoom;
            drawList->AddLine(ImVec2(canvasPos.x, sy), ImVec2(canvasPos.x + canvasSize.x, sy), gridCol, 1.0f);
        }

        // Wires behind the nodes (bezier from an output pin to an input pin).
        for (int w = 0; w < kWireCount; ++w)
        {
            const DemoNode& from = nodes[kWires[w][0]];
            const DemoNode& to = nodes[kWires[w][1]];
            const ImVec2 p1 = screenFromWorld(from.X + kNodeW, from.Y + kHeaderH * 0.5f);
            const ImVec2 p2 = screenFromWorld(to.X, to.Y + kHeaderH * 0.5f);
            const float bend = ImMax(40.0f * zoom, (p2.x - p1.x) * 0.5f);
            drawList->AddBezierCubic(p1, ImVec2(p1.x + bend, p1.y), ImVec2(p2.x - bend, p2.y), p2,
                                     IM_COL32(150, 160, 180, 190), ImMax(1.0f, 2.0f * zoom));
        }

        // Nodes.
        ImFont* font = ImGui::GetFont();
        for (int i = 0; i < kNodeCount; ++i)
        {
            const DemoNode& node = nodes[i];
            const ImVec2 p0 = screenFromWorld(node.X, node.Y);
            const ImVec2 p1 = screenFromWorld(node.X + kNodeW, node.Y + kHeaderH + 2.0f * kRowH);
            const float rounding = 6.0f * zoom;
            const ImU32 headerCol = (node.Accent & ~IM_COL32_A_MASK) | (0x80u << IM_COL32_A_SHIFT);
            drawList->AddRectFilled(p0, p1, IM_COL32(34, 38, 48, 255), rounding);
            drawList->AddRectFilled(p0, ImVec2(p1.x, p0.y + kHeaderH * zoom), headerCol, rounding,
                                    ImDrawFlags_RoundCornersTop);
            drawList->AddRect(p0, p1, IM_COL32(12, 13, 17, 255), rounding, 0, ImMax(1.0f, 1.5f * zoom));
            if (node.HasOut)
                drawList->AddCircleFilled(screenFromWorld(node.X + kNodeW, node.Y + kHeaderH * 0.5f),
                                          ImMax(2.0f, 5.0f * zoom), IM_COL32(205, 210, 220, 255));
            if (node.HasIn)
                drawList->AddCircleFilled(screenFromWorld(node.X, node.Y + kHeaderH * 0.5f), ImMax(2.0f, 5.0f * zoom),
                                          IM_COL32(205, 210, 220, 255));
            if (titlePx >= 6.0f)
            {
                drawList->AddText(font, titlePx, ImVec2(p0.x + 8.0f * zoom, p0.y + 6.0f * zoom),
                                  IM_COL32(245, 245, 250, 255), node.Title);
            }
            if (drawRows)
            {
                drawList->AddText(font, rowPx, ImVec2(p0.x + 10.0f * zoom, p0.y + (kHeaderH + 3.0f) * zoom),
                                  IM_COL32(190, 195, 205, 255), node.Row0);
                drawList->AddText(font, rowPx, ImVec2(p0.x + 10.0f * zoom, p0.y + (kHeaderH + kRowH + 3.0f) * zoom),
                                  IM_COL32(190, 195, 205, 255), node.Row1);
            }
        }
        drawList->PopClipRect();
        ImGui::End();
    }

    // Heavy scene (key P): a text-heavy workload plus the performance panel.
    // M swaps the body text between Slug outlines and ImGui's dynamic font
    // system; the panel shows wall-clock frame time (plot), whole-frame GPU
    // time from timestamp queries, draw/Slug/atlas counters and the vsync
    // toggle. Isolated GPU text timings: scripts/bench.* (imslug_tests_bench
    // --gpu); see docs/DESIGN.md section 4.
    void ScenePerf()
    {
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(268.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("performance");
        ImGui::Text("text renderer: %s (M toggles)", g_App.AtlasText ? "ImGui dynamic fonts" : "Slug outlines");
        const float frameMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
        float frameAvg = 0.0f;
        for (int i = 0; i < g_App.FrameMsCount; ++i)
            frameAvg += g_App.FrameMsHistory[i];
        if (g_App.FrameMsCount > 0)
            frameAvg /= (float)g_App.FrameMsCount;
        ImGui::Text("frame: %.1f FPS, %.2f ms (window avg %.2f ms)", io.Framerate, frameMs, frameAvg);
        ImGui::TextUnformatted("frame ms (wall clock):");
        ImGui::PlotLines("##frame_ms", g_App.FrameMsHistory, g_App.FrameMsCount,
                         g_App.FrameMsCount < kExamplePerfHistory ? 0 : g_App.FrameMsCursor, nullptr, 0.0f,
                         frameAvg * 2.0f > 1.0f ? frameAvg * 2.0f : 1.0f, ImVec2(410.0f, 64.0f));
        // The GPU-time and vsync rows are desktop-only: a shell without the
        // hooks (web) hides them instead of showing a stale/placeholder state.
        if (g_Platform.GetGpuFrameMs != nullptr)
        {
            float gpuMs = 0.0f;
            int gpuSlots = 0;
            if (g_Platform.GetGpuFrameMs(g_Platform.user, &gpuMs, &gpuSlots))
            {
                if (gpuSlots > 0)
                    ImGui::Text("GPU frame (timestamps): %.2f ms over %d slots", gpuMs, gpuSlots);
                else
                    ImGui::TextUnformatted("GPU frame (timestamps): warming up");
            }
            else
                ImGui::TextUnformatted("GPU frame: n/a (queue without timestamps)");
        }
        if (g_Platform.GetVsync != nullptr)
        {
            bool vsync = g_Platform.GetVsync(g_Platform.user);
            if (ImGui::Checkbox("vsync", &vsync))
                g_Platform.ToggleVsync(g_Platform.user);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_Platform.GetPresentModeName(g_Platform.user));
        }

        ImGui::SeparatorText("draw stats (last frame)");
        ImGui::Text("windows %d, cmd lists %d, draw cmds %d", io.MetricsRenderWindows, g_App.LastCmdLists,
                    g_App.LastDrawCommands);
        ImGui::Text("verts %d, indices %d", g_App.LastVtxCount, g_App.LastIdxCount);
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("Slug: %u instances (%u color layers), %u runs, %u slabs", stats.Instances, stats.ColorLayers,
                    stats.LiveBatches, stats.GlyphSlabs);
        if (stats.DroppedRuns != 0)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "dropped runs: %u", stats.DroppedRuns);
        const ImTextureData* tex = io.Fonts->TexData;
        ImGui::Text("atlas: %dx%d, %.2f MB, %d fonts", tex != nullptr ? tex->Width : 0,
                    tex != nullptr ? tex->Height : 0,
                    tex != nullptr ? (double)tex->GetSizeInBytes() / (1024.0 * 1024.0) : 0.0, io.Fonts->Fonts.Size);
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(706.0f, kSceneWindowY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(550.0f, 688.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("heavy demo (Slug vs atlas)");
        ImGui::TextWrapped("A text-heavy frame to compare the two render paths. Press M (or use the renderer bar) "
                           "to swap the body text between Slug outlines and ImGui's dynamic font system, and watch "
                           "the performance panel; untick vsync for uncapped numbers.");
        ImGui::SliderInt("lines", &g_App.PerfLines, 8, 80);
        ImGui::SliderInt("chars/line", &g_App.PerfChars, 64, 512);
        ImGui::SliderFloat("large text", &g_App.PerfScale, 1.0f, 6.0f, "%.1fx");
        ImGui::Text("%d lines x %d chars = %.0f glyphs laid out per frame", g_App.PerfLines, g_App.PerfChars,
                    (double)g_App.PerfLines * (double)g_App.PerfChars);

        if (g_App.SlugFont != nullptr && g_App.StockFont != nullptr)
        {
            ImGui::SeparatorText("same text, both paths (2x size)");
            ImGui::PushFont(g_App.SlugFont, (float)g_App.BaseSize * 2.0f);
            ImGui::TextUnformatted("Slug: Hamburgefonstiv 0123456789");
            ImGui::PopFont();
            ImGui::PushFont(g_App.StockFont, (float)g_App.BaseSize * 2.0f);
            ImGui::TextUnformatted("atlas: Hamburgefonstiv 0123456789");
            ImGui::PopFont();
        }

        ImGui::SeparatorText("text load");
        ImFont* largeFont = g_App.AtlasText ? g_App.StockFont : g_App.SlugFont;
        if (largeFont != nullptr)
        {
            ImGui::PushFont(largeFont, (float)g_App.BaseSize * g_App.PerfScale);
            ImGui::TextWrapped("The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor "
                               "jugs. Waltz, bad nymph, for quick jigs vex. Sphinx of black quartz, judge my vow.");
            ImGui::PopFont();
        }

        // Text wall, regenerated only when the inputs (or the base size) change.
        static char* wall = nullptr;
        static int wallLines = -1;
        static int wallChars = -1;
        static int wallSize = -1;
        if (wallLines != g_App.PerfLines || wallChars != g_App.PerfChars || wallSize != g_App.BaseSize)
        {
            free(wall);
            wall = nullptr;
            const int lines = g_App.PerfLines;
            const int chars = g_App.PerfChars;
            char* next = (char*)malloc((size_t)lines * ((size_t)chars + 1) + 1);
            if (next != nullptr)
            {
                const char* pattern = "Hamburgefonstiv 0123456789 Grumpy wizards make toxic brew. ";
                const int patternLength = (int)strlen(pattern);
                size_t at = 0;
                for (int line = 0; line < lines; ++line)
                {
                    for (int column = 0; column < chars; ++column)
                        next[at++] = pattern[(line * chars + column) % patternLength];
                    next[at++] = '\n';
                }
                next[at] = '\0';
                wall = next;
                wallLines = lines;
                wallChars = chars;
                wallSize = g_App.BaseSize;
            }
        }
        if (wall != nullptr)
            ImGui::TextUnformatted(wall);
        ImGui::End();
    }

    // Text renderer toggle (M / the renderer bar / --renderer). Both font
    // registrations coexist: SlugFont renders from the outlines through imslug,
    // StockFont through ImGui's dynamic font system. Only the default font
    // swaps; scenes that push a dedicated font (CJK/emoji/packed) pick their
    // mode themselves (see the scene comments).
    void SetTextRenderer(bool atlas)
    {
        if (g_App.AtlasText == atlas)
            return;
        if (atlas && g_App.StockFont == nullptr)
        {
            fprintf(stderr, "[imslug] text renderer: atlas unavailable (the stock font registration failed)\n");
            return;
        }
        g_App.AtlasText = atlas;
        ImFont* font = atlas ? g_App.StockFont : g_App.SlugFont;
        if (font != nullptr && ImGui::GetCurrentContext() != nullptr)
            ImGui::GetIO().FontDefault = font;
        printf("[imslug] text renderer: %s\n", atlas ? "ImGui dynamic fonts (atlas)" : "Slug outlines");
    }

    // Debug overlay toggle (runtime; mapped onto the shell's backend module
    // through the ExamplePlatform hooks). Returns the new state so a caller
    // can react (keys and checkboxes share it).
    bool ToggleDebugOverlay(uint32_t flag)
    {
        uint32_t overlay = g_Platform.GetDebugOverlay(g_Platform.user);
        overlay ^= flag;
        g_Platform.SetDebugOverlay(g_Platform.user, overlay);
        printf("[imslug] debug overlay: 0x%X%s%s%s\n", overlay,
               (overlay & ExampleDebugOverlay_GlyphBounds) ? " bounds" : "",
               (overlay & ExampleDebugOverlay_BandGrid) ? " band-grid" : "",
               (overlay & ExampleDebugOverlay_RefHeat) ? " ref-heat" : "");
        return (overlay & flag) != 0;
    }

    // The renderer comparison bar: the example's headline control. The two
    // font registrations coexist (SlugFont / StockFont), so M - or these
    // buttons - swaps which one draws the default-font text; a direct
    // comparison of the two renderers is always one click away. The bar is
    // pinned to the top center and brought to the display front every frame
    // (it is a toolbar, not a scene window), the accent border and the status
    // line name the active mode, and atlas mode lists what it hides instead
    // of drawing it (see the emoji/packed scenes).
    void DrawRendererBar()
    {
        const bool slug = !g_App.AtlasText;
        ImGuiIO& io = ImGui::GetIO();
        ImGuiStyle& style = ImGui::GetStyle();
        const ImVec4 slugColor(0.20f, 0.52f, 0.32f, 1.0f);
        const ImVec4 atlasColor(0.58f, 0.40f, 0.16f, 1.0f);

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 8.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, slug ? slugColor : atlasColor);
        ImGui::Begin("##text_renderer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
        ImGuiWindow* barWindow = ImGui::GetCurrentWindow();
        // 1.3x the current base size, capped so a large --size does not turn
        // the toolbar into a wall (the window positions assume a compact bar).
        const float fontSizeBase = style.FontSizeBase > 0.0f ? style.FontSizeBase : (float)g_App.BaseSize;
        const float barFontSize = fontSizeBase * 1.3f < 24.0f ? fontSizeBase * 1.3f : 24.0f;
        ImGui::PushFont(nullptr, barFontSize);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("text renderer (M)");
        // Narrow canvases (the browser demo resizes): put the buttons on
        // their own row instead of overflowing the viewport.
        if (io.DisplaySize.x >= 620.0f)
            ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, slug ? slugColor : style.Colors[ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, slug ? slugColor : style.Colors[ImGuiCol_ButtonHovered]);
        if (ImGui::Button("Slug (imslug)") && !slug)
            SetTextRenderer(false);
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_App.StockFont == nullptr);
        ImGui::PushStyleColor(ImGuiCol_Button, !slug ? atlasColor : style.Colors[ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, !slug ? atlasColor : style.Colors[ImGuiCol_ButtonHovered]);
        if (ImGui::Button("ImGui atlas") && slug)
            SetTextRenderer(true);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::PopFont();
        // The status line stays at the default size even with a large
        // --size/: the bar is a toolbar, not part of the font-size sample.
        ImGui::PushFont(nullptr, fontSizeBase < (float)kExampleDefaultSize ? fontSizeBase : (float)kExampleDefaultSize);
        ImGui::TextDisabled(slug ? "Slug outlines at any size - no atlas bakes"
                                 : "stock ImGui atlas - Slug-only samples (color emoji, packed asset) are hidden");
        ImGui::PopFont();
        ImGui::End();
        ImGui::BringWindowToDisplayFront(barWindow);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void DrawSceneSwitcher()
    {
        const char* sceneNames[kExampleSceneCount] = { "demo window",
                                                       "widgets / plots / tables",
                                                       "clipping torture",
                                                       "mixed fonts",
                                                       "runtime FontScaleDpi",
                                                       "5k glyph stress",
                                                       "CJK (fetched subset)",
                                                       "color emoji (COLR v1)",
                                                       "packed asset (.imslug)",
                                                       "node editor (pan + zoom)",
                                                       "heavy demo / perf (P)" };
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(240.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("imslug example " IMSLUG_VERSION, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted("Scene (keys 1-9, N, P):");
        for (int i = 0; i < kExampleSceneCount; ++i)
        {
            const ImGuiKey shortcutKey = i < 9 ? (ImGuiKey)(ImGuiKey_1 + i) : (i == 9 ? ImGuiKey_N : ImGuiKey_P);
            if (ImGui::RadioButton(sceneNames[i], g_App.Scene == i) || ImGui::IsKeyPressed(shortcutKey))
                g_App.Scene = i;
        }
        ImGui::Separator();
        // The Slug/atlas toggle lives in the renderer bar above; this is the
        // state line next to the scene list.
        ImGui::Text("font: %s, base size %d px", g_App.AtlasText ? "atlas" : "Slug", g_App.BaseSize);
        if (ImGui::Button("+") || ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
            g_App.FontsDirty = true,
            g_App.BaseSize = g_App.BaseSize < kExampleMaxSize ? g_App.BaseSize + 1 : kExampleMaxSize;
        ImGui::SameLine();
        if (ImGui::Button("-") || ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
            g_App.FontsDirty = true,
            g_App.BaseSize = g_App.BaseSize > kExampleMinSize ? g_App.BaseSize - 1 : kExampleMinSize;
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        ImGui::Separator();
        Statistics stats;
        ImSlug::GetStatistics(&stats);
        ImGui::Text("Slug: %u instances, %u runs\n(%u slabs, %u dropped)", stats.Instances, stats.LiveBatches,
                    stats.GlyphSlabs, stats.DroppedRuns);
        if (g_Platform.GetValidationState != nullptr)
            ImGui::Text("validation: %s", g_Platform.GetValidationState(g_Platform.user));
        ImGui::Separator();
        ImGui::Text("debug overlay (F1/F2/F3):");
        {
            const uint32_t overlay = g_Platform.GetDebugOverlay(g_Platform.user);
            bool bounds = (overlay & ExampleDebugOverlay_GlyphBounds) != 0;
            bool bands = (overlay & ExampleDebugOverlay_BandGrid) != 0;
            bool refs = (overlay & ExampleDebugOverlay_RefHeat) != 0;
            if (ImGui::Checkbox("glyph bounds", &bounds))
                ToggleDebugOverlay(ExampleDebugOverlay_GlyphBounds);
            if (ImGui::Checkbox("band grid", &bands))
                ToggleDebugOverlay(ExampleDebugOverlay_BandGrid);
            if (ImGui::Checkbox("ref heat", &refs))
                ToggleDebugOverlay(ExampleDebugOverlay_RefHeat);
        }
        ImGui::End();
    }

    void BuildImGuiFrame()
    {
        ImGui::NewFrame();
        // Performance panel history: wall-clock frame ms (io.DeltaTime is the
        // time NewFrame measured for the previous frame).
        g_App.FrameMsHistory[g_App.FrameMsCursor] = ImGui::GetIO().DeltaTime * 1000.0f;
        g_App.FrameMsCursor = (g_App.FrameMsCursor + 1) % kExamplePerfHistory;
        if (g_App.FrameMsCount < kExamplePerfHistory)
            ++g_App.FrameMsCount;
        DrawSceneSwitcher();
        switch (g_App.Scene)
        {
        case 0:
            SceneDemo();
            break;
        case 1:
            SceneWidgets();
            break;
        case 2:
            SceneClipping();
            break;
        case 3:
            SceneMixedFonts();
            break;
        case 4:
            SceneDpi();
            break;
        case 5:
            SceneStress();
            break;
        case 6:
            SceneCjk();
            break;
        case 7:
            SceneEmoji();
            break;
        case 8:
            ScenePacked();
            break;
        case 9:
            SceneNodeEditor();
            break;
        default:  // 10: heavy demo / perf
            ScenePerf();
            break;
        }
        // Built after the scenes so the bar starts at the top of the window
        // stack (it also re-fronts itself every frame, see DrawRendererBar).
        DrawRendererBar();
    }

    void CaptureDrawStats()
    {
        const ImDrawData* drawData = ImGui::GetDrawData();
        g_App.LastCmdLists = drawData->CmdListsCount;
        g_App.LastVtxCount = drawData->TotalVtxCount;
        g_App.LastIdxCount = drawData->TotalIdxCount;
        g_App.LastDrawCommands = 0;
        for (int i = 0; i < drawData->CmdListsCount; ++i)
            g_App.LastDrawCommands += drawData->CmdLists[i]->CmdBuffer.Size;
    }

    // Re-adds the Slug fonts at the current base size. The previous fonts are
    // removed first: without that every size step would append another ImFont +
    // bake set to the atlas (glyph slabs pile up until the cap) and the atlas
    // grows. Removal discards the bakes, which releases the slabs through
    // FontBakedDestroy; the outlines and bands in the pools stay valid and are
    // reused across sizes (no atlas growth for Slug glyphs).
    void RebuildFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        if (g_App.SlugFont != nullptr)
        {
            io.Fonts->RemoveFont(g_App.SlugFont);
            g_App.SlugFont = nullptr;
        }
        if (g_App.StockFont != nullptr)
        {
            io.Fonts->RemoveFont(g_App.StockFont);
            g_App.StockFont = nullptr;
        }
        const char* fontPath = g_App.FontPath;  // --font / IMSLUG_EXAMPLE_FONT; null = embedded defaults
        if (fontPath != nullptr)
        {
            g_App.SlugFont = ImSlug::AddFontFromFileTTF(fontPath, (float)g_App.BaseSize);
        }
        else
        {
            // The embedded default: the committed packed Roboto-ASCII asset,
            // loaded straight from the binary (no outline parsing - the same
            // path a no-parser build ships). Static data: never transfer
            // ownership to the atlas.
            ImFontConfig slugConfig;
            slugConfig.FontDataOwnedByAtlas = false;
            g_App.SlugFont = ImSlug::AddFontFromMemoryTTF(imslug_example_asset_data, (int)imslug_example_asset_size,
                                                          (float)g_App.BaseSize, &slugConfig);
        }
        if (g_App.SlugFont == nullptr)
        {
            fprintf(stderr, "[imslug] cannot add Slug font: %s\n",
                    fontPath != nullptr ? fontPath : "(embedded packed asset)");
            g_App.ShouldClose = true;
            return;
        }
        // A stock font for the mixed-font scene and the M toggle: through
        // ImGui's own stb loader (Oversample 1 keeps the metrics identical).
        // ImGui cannot parse a packed `.imslug` asset, so the stock font uses
        // the file only when --font named a TTF (FontPathIsPacked is the
        // shell's one-time file sniff); otherwise it comes from the embedded
        // Roboto TTF copy (in a no-parser build too - ImGui's own parser is
        // independent of imslug's).
        ImFontConfig stockConfig;
        stockConfig.OversampleH = 1;
        stockConfig.OversampleV = 1;
        if (fontPath != nullptr && !g_App.FontPathIsPacked)
        {
            // File source: ImGui loads (and, with the default ownership, frees)
            // the buffer - do not override FontDataOwnedByAtlas here or every
            // RebuildFonts() would leak the font file.
            g_App.StockFont = io.Fonts->AddFontFromFileTTF(fontPath, (float)g_App.BaseSize, &stockConfig);
        }
        else
        {
            // Embedded static data: never transfer ownership to the atlas.
            stockConfig.FontDataOwnedByAtlas = false;
            g_App.StockFont = io.Fonts->AddFontFromMemoryTTF(
                (void*)imslug_example_font_data, (int)imslug_example_font_size, (float)g_App.BaseSize, &stockConfig);
        }
        io.FontDefault = g_App.AtlasText ? g_App.StockFont : g_App.SlugFont;
        // ImGui 1.92 dynamic fonts: the AddFont* size is only the font's
        // LegacySize suggestion, resolved into style.FontSizeBase once on the
        // first frame. Drive style.FontSizeBase from BaseSize here so the
        // runtime +/- keys and --size are a real base-size control in both
        // renderers; the shells rebuild before NewFrame, so this takes effect
        // on the next frame. FontScaleDpi (--dpi / scene 4) still multiplies.
        ImGui::GetStyle().FontSizeBase = (float)g_App.BaseSize;
        g_App.FontsDirty = false;
        printf("[imslug] fonts: Slug + atlas at %d px (font: %s)\n", g_App.BaseSize,
               fontPath != nullptr ? fontPath : "embedded Roboto (packed asset, TTF for the atlas)");
    }

    // Installs the shared state from the shell's options.
    void LoadOptions(const ExampleAppOptions* options)
    {
        g_App.FontPath = options->FontPath;
        g_App.FontPathIsPacked = options->FontPathIsPacked;
        g_App.BaseSize = options->BaseSize;
        if (g_App.BaseSize < kExampleMinSize)
            g_App.BaseSize = kExampleMinSize;
        if (g_App.BaseSize > kExampleMaxSize)
            g_App.BaseSize = kExampleMaxSize;
        g_App.Scene = options->SceneNumber != 0 ? options->SceneNumber - 1 : 0;  // scenes are 1-based on the CLI
        if (g_App.Scene < 0 || g_App.Scene >= kExampleSceneCount)
            g_App.Scene = 0;
        g_App.DpiScale = options->DpiScale;
        if (g_App.DpiScale < 0.5f)
            g_App.DpiScale = 0.5f;
        if (g_App.DpiScale > 3.0f)
            g_App.DpiScale = 3.0f;
        g_App.AtlasText = options->AtlasText;
        g_App.EmojiSize = 24.0f;
        g_App.EmojiZoom = 120.0f;
        g_App.PerfLines = 40;
        g_App.PerfChars = 200;
        g_App.PerfScale = 3.0f;
    }
}  // namespace

ExampleAppState* ExampleAppGetState()
{
    return &g_App;
}

void ExampleAppOptionsDefaults(ExampleAppOptions* options)
{
    options->FontPath = nullptr;
    options->FontPathIsPacked = false;
    options->BaseSize = kExampleDefaultSize;
    options->SceneNumber = 0;
    options->DpiScale = 1.0f;
    options->AtlasText = false;
    options->DebugOverlay = 0;
    options->DebugOverlayGiven = false;
}

int ExampleAppParseArg(int argc, char** argv, int argIndex, ExampleAppOptions* options)
{
    const char* arg = argv[argIndex];
    if (strcmp(arg, "--font") == 0 && argIndex + 1 < argc)
    {
        // A filesystem path: only the desktop shell passes one (the web shell
        // always uses the embedded fonts).
        options->FontPath = argv[argIndex + 1];
        return 2;
    }
    if (strcmp(arg, "--size") == 0 && argIndex + 1 < argc)
    {
        options->BaseSize = atoi(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--renderer") == 0 && argIndex + 1 < argc)
    {
        const char* value = argv[argIndex + 1];
        if (strcmp(value, "slug") == 0)
            options->AtlasText = false;
        else if (strcmp(value, "atlas") == 0)
            options->AtlasText = true;
        else
        {
            fprintf(stderr, "[imslug] --renderer expects 'slug' or 'atlas' (got '%s')\n", value);
            exit(1);
        }
        return 2;
    }
    if (strcmp(arg, "--scene") == 0 && argIndex + 1 < argc)
    {
        options->SceneNumber = atoi(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--dpi") == 0 && argIndex + 1 < argc)
    {
        options->DpiScale = (float)atof(argv[argIndex + 1]);
        return 2;
    }
    if (strcmp(arg, "--overlay") == 0 && argIndex + 1 < argc)
    {
        const char* spec = argv[argIndex + 1];
        uint32_t flags = 0;
        if (strstr(spec, "bounds") != nullptr)
            flags |= ExampleDebugOverlay_GlyphBounds;
        if (strstr(spec, "grid") != nullptr)
            flags |= ExampleDebugOverlay_BandGrid;
        if (strstr(spec, "refs") != nullptr)
            flags |= ExampleDebugOverlay_RefHeat;
        if (flags == 0 && spec[0] >= '0' && spec[0] <= '9')
            flags = (uint32_t)atoi(spec);
        if (strcmp(spec, "none") == 0)
            flags = 0;
        options->DebugOverlay = flags;
        options->DebugOverlayGiven = true;
        return 2;
    }
    return 0;
}

void ExampleAppInit(const ExampleAppOptions* options, const ExamplePlatform* platform)
{
    g_App = ExampleAppState {};
    g_Platform = *platform;
    LoadOptions(options);
}

void ExampleAppRebuildFonts()
{
    RebuildFonts();
}

void ExampleAppBuildFrame()
{
    BuildImGuiFrame();
}

void ExampleAppCaptureDrawStats()
{
    CaptureDrawStats();
}

bool ExampleAppToggleDebugOverlay(uint32_t flag)
{
    return ToggleDebugOverlay(flag);
}

void ExampleAppSetTextRenderer(bool atlas)
{
    SetTextRenderer(atlas);
}

void ExampleAppResetFonts()
{
    g_App.BaseSize = kExampleDefaultSize;
    g_App.DpiScale = 1.0f;
    // The FontScaleDpi scene re-applies the slider every frame it is shown;
    // write the reset through immediately so key 0 restores the DPI scale
    // without revisiting scene 4 (the base size follows via RebuildFonts).
    if (ImGui::GetCurrentContext() != nullptr)
        ImGui::GetStyle().FontScaleDpi = g_App.DpiScale;
    g_App.FontsDirty = true;
    printf("[imslug] fonts reset to %d px, DPI 1.00\n", g_App.BaseSize);
}
